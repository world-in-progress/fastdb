import numpy as np
from threading import Lock
from contextlib import contextmanager
from typing import TypeVar, Generic, Type, Generator

from .. import core
from ..feature import Feature, get_all_defns
from ..feature._schema import get_class_schema

T = TypeVar('T', bound=Feature)
_column_accessor_lock = Lock()


def _create_column_accessor(feature_type: Type[T], table_origin) -> T:
    """
    Create a column accessor that provides numpy array access with proper type hints.

    The ColumnAccessor class is cached inside ClassSchema.column_accessor_class,
    eliminating the separate WeakKeyDictionary (Cache 3).
    """
    schema = get_class_schema(feature_type)
    ColumnAccessorClass = schema.column_accessor_class
    if ColumnAccessorClass is not None:
        return ColumnAccessorClass(table_origin, feature_type)

    with _column_accessor_lock:
        ColumnAccessorClass = schema.column_accessor_class
        if ColumnAccessorClass is not None:
            return ColumnAccessorClass(table_origin, feature_type)

        # Get original annotations from feature_type
        original_annotations = getattr(feature_type, '__annotations__', {}).copy()

        # field_index_map pre-computed in ClassSchema — no get_all_defns() call needed.
        _field_index_map = schema.field_index_map

        # Create the dynamic column accessor class with modified annotations
        class ColumnAccessor:
            """Column accessor that returns numpy arrays for field access"""

            # Set the new annotations
            __annotations__ = original_annotations

            def __init__(self, table_origin, feature_type):
                # Don't call parent __init__ to avoid initializing cache
                # Just set internal references
                object.__setattr__(self, '_table_origin', table_origin)
                object.__setattr__(self, '_field_index_map', _field_index_map)
                object.__setattr__(self, '_name_cache', {})

            def __getattr__(self, name: str) -> np.ndarray:
                """Override to return numpy array instead of single value"""
                # Hot path: name → array directly (1× getattribute + 1× dict.get).
                # Cached arrays are stable: fixed-scale tables never reallocate columns.
                name_cache = object.__getattribute__(self, '_name_cache')
                arr = name_cache.get(name)
                if arr is not None:
                    return arr

                # Cold path: validate field name, call SWIG, populate cache.
                fmap = object.__getattribute__(self, '_field_index_map')
                idx = fmap.get(name)
                if idx is None:
                    raise AttributeError(f'Field "{name}" not found in the table.')

                table_origin = object.__getattribute__(self, '_table_origin')
                arr = table_origin.get_column(idx).as_nparray()
                name_cache[name] = arr
                return arr
            
            def __setattr__(self, name: str, value):
                """Prevent setting attributes on column accessor"""
                if name.startswith('_'):
                    object.__setattr__(self, name, value)
                else:
                    raise AttributeError(
                        f'Cannot set field "{name}" on column accessor. '
                        'Use table[index].{name} = value to modify individual features.'
                    )
        
        # Store the class in ClassSchema (replaces _column_accessor_cache WeakKeyDict).
        schema.column_accessor_class = ColumnAccessor
        return ColumnAccessor(table_origin, feature_type)

class Table(Generic[T]):
    def __init__(self):
        self.feature_count: int = 0
        self._column: T | None = None
        self._feature_type: Type[T] | None = None
        self._db: core.WxDatabase | core.WxDatabaseBuild = None
        self._origin: core.WxLayerTable | core.WxLayerTableBuild | None = None
    
    def __len__(self) -> int:
        return self._origin.get_feature_count()
    
    def __getitem__(self, index: int) -> T:
        # Check index bounds
        if index >= self._origin.get_feature_count():
            raise IndexError(f'Feature index {index} out of range [0, {self._origin.get_feature_count()}].')
        
        if index < 0:
            index = self._origin.get_feature_count() + index
        
        # Get feature
        return  self._feature_type.map_from(self._db, self._origin.tryGetFeature(index))
    
    def __iter__(self) -> Generator[T, None, None]:
        for i in range(self._origin.get_feature_count()):
            yield self._feature_type.map_from(self._db, self._origin.tryGetFeature(i))
    
    @staticmethod
    @contextmanager
    def push2(table: 'Table[T]') -> Generator[core.WxLayerTableBuild, None, None]:
        """Context manager to push features to the given table."""
        if table._db is None or table._origin is None:
            raise RuntimeError('Table has not connected to fastdb, not supporting push operation.')
        if table.fixed:
            raise RuntimeError('Table has fixed scale, not supporting push operation.')
        
        table._origin.add_feature_begin()
        
        yield table._origin  # type: core.WxLayerTableBuild
        
        table._origin.add_feature_end()
        table.feature_count += 1
        
    @property
    def name(self) -> str:
        return self._origin.name()
    
    @property
    def column(self) -> T:
        """
        Get column accessor that provides numpy array access to fields.
        
        Returns a "fake" instance of T where accessing any field returns
        the entire column as a numpy array instead of a single value.
        """
        if self._column is None:
            raise RuntimeError('Table has not been mapped with a feature type.')
        return self._column
    
    @property
    def row(self) -> int:
        return self._origin.row()
    
    @property
    def next(self) -> bool:
        return self._origin.next()
    
    @property
    def fixed(self) -> bool:
        return isinstance(self._origin, core.WxLayerTable)
    
    @staticmethod
    def map_from(
        feature_type: Type[T] | None,
        origin: core.WxLayerTable | core.WxLayerTableBuild,
        db: core.WxDatabase | core.WxDatabaseBuild
    ) -> 'Table[T]':
        table = Table[T]()
        table._db = db
        table._origin = origin
        table._feature_type = feature_type
        
        # Get feature count if the fastdb table has fixed scale
        if table.fixed:
            table.feature_count = origin.get_feature_count()
            # Create column accessor that pretends to be T but returns numpy arrays
            table._column = _create_column_accessor(feature_type, origin) if feature_type is not None else None
        
        return table
    
    def rewind(self):
        self._origin.rewind()

    def fill(self, **col_arrays) -> None:
        """
        Batch-write multiple columns from numpy arrays in a single call.

        Each keyword argument maps a field name to a numpy array whose length
        must equal the table's feature count.

        Only supported for fixed-scale tables (table.fixed == True).
        Usage:
            tbl.fill(x=xs, y=ys, z=zs)   # xs, ys, zs are numpy arrays of length N
        """
        if not self.fixed:
            raise RuntimeError('fill() only supports fixed-scale tables.')
        col = self._column
        for field_name, arr in col_arrays.items():
            getattr(col, field_name)[:] = arr

    def iter_reuse(self) -> Generator[T, None, None]:
        """
        High-performance iterator that reuses a single Feature wrapper instance.

        WARNING: Do NOT hold references to the yielded object across iterations.
        The same object is mutated on each step — any reference held outside the
        loop body will see the NEXT item's data.

        Only supported for fixed-scale tables (table.fixed == True).
        """
        if not self.fixed:
            raise RuntimeError('iter_reuse() only supports fixed-scale tables.')

        wrapper = self._feature_type()          # allocate once
        object.__setattr__(wrapper, '_db', self._db)
        count = self._origin.get_feature_count()
        for i in range(count):
            object.__setattr__(wrapper, '_origin', self._origin.tryGetFeature(i))
            object.__setattr__(wrapper, '_cache', None)
            yield wrapper