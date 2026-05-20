import numpy as np
from collections.abc import Iterable, Mapping
from threading import Lock, RLock
from contextlib import contextmanager
from typing import TypeVar, Generic, Type, Generator

from .. import core
from ..registry import get_schema
from ..reader import bind_feature, MappedFeature
from ..string_column import BytesColumn, StringColumn, _normalize_string_values
from ..type import OriginFieldType, coerce_bool_scalar

T = TypeVar('T')
_column_accessor_lock = Lock()
_FILL_NUMERIC_DTYPES = {
    OriginFieldType.u8: np.uint8,
    OriginFieldType.u16: np.uint16,
    OriginFieldType.u32: np.uint32,
    OriginFieldType.i32: np.int32,
    OriginFieldType.u8n: np.uint8,
    OriginFieldType.u16n: np.uint16,
    OriginFieldType.f32: np.float32,
    OriginFieldType.f64: np.float64,
}
_BOOL_FILL_SCALAR_TYPES = (str, bytes, bytearray, memoryview)


def _normalize_bool_fill_values(field_name: str, values, expected: int) -> np.ndarray:
    if isinstance(values, (*_BOOL_FILL_SCALAR_TYPES, Mapping)) or not isinstance(values, Iterable):
        raise TypeError(
            f'fastdb bool fill field {field_name!r} must be an iterable of bool items, not a scalar, bytes-like value, or mapping.',
        )

    if isinstance(values, np.ndarray):
        arr = _normalize_bool_fill_ndarray(field_name, values)
    else:
        arr = np.ascontiguousarray(
            [0 if value is None else (1 if coerce_bool_scalar(value) else 0) for value in values],
            dtype=np.uint8,
        )

    if arr.ndim != 1:
        raise ValueError(
            f'{field_name} expected 1-D column values, got {arr.ndim}-D.',
        )
    if len(arr) != expected:
        raise ValueError(f'{field_name} expected {expected} rows, got {len(arr)}.')
    return arr


def _normalize_bool_fill_ndarray(field_name: str, values: np.ndarray) -> np.ndarray:
    if values.ndim == 0:
        raise TypeError(
            f'fastdb bool fill field {field_name!r} must be an iterable of bool items, not a scalar array.',
        )
    if np.issubdtype(values.dtype, np.bool_):
        return np.ascontiguousarray(values.astype(np.uint8, copy=False))
    if np.issubdtype(values.dtype, np.integer) or np.issubdtype(values.dtype, np.floating):
        valid = np.logical_or(values == 0, values == 1)
        if not bool(np.all(valid)):
            raise ValueError(
                f'cannot coerce {field_name!r} to fastdb bool scalar column; expected bool, 0/1, or true/false string values.',
            )
        return np.ascontiguousarray(values.astype(np.uint8, copy=False))
    return np.ascontiguousarray(
        [0 if value is None else (1 if coerce_bool_scalar(value) else 0) for value in values],
        dtype=np.uint8,
    )


def _create_column_accessor(feature_type: Type[T], table) -> T:
    """
    Create a column accessor that provides numpy array access with proper type hints.

    The ColumnAccessor class is cached inside ClassSchema.column_accessor_class,
    eliminating the separate WeakKeyDictionary (Cache 3).
    """
    with _column_accessor_lock:
        schema = get_schema(feature_type)
        ColumnAccessorClass = schema.column_accessor_class
        if ColumnAccessorClass is not None:
            return ColumnAccessorClass(table, feature_type)

        # Get original annotations from feature_type
        original_annotations = getattr(feature_type, '__annotations__', {}).copy()

        # field_index_map pre-computed in ClassSchema — no get_all_defns() call needed.
        _field_index_map = schema.field_index_map

        # Create the dynamic column accessor class with modified annotations
        class ColumnAccessor:
            """Column accessor that returns numpy arrays for field access"""

            # Set the new annotations
            __annotations__ = original_annotations

            def __init__(self, table, feature_type):
                # Don't call parent __init__ to avoid initializing cache
                # Just set internal references
                object.__setattr__(self, '_table', table)
                object.__setattr__(self, '_field_index_map', _field_index_map)
                object.__setattr__(self, '_name_cache', {})
                object.__setattr__(self, '_cache_lock', Lock())

            def __getattr__(self, name: str) -> np.ndarray:
                """Override to return numpy array instead of single value"""
                # Hot path: name → array directly (1× getattribute + 1× dict.get).
                # Cached arrays are stable: fixed-scale tables never reallocate columns.
                name_cache = object.__getattribute__(self, '_name_cache')
                arr = name_cache.get(name)
                if arr is not None:
                    return arr

                # Cold path: validate field name, call SWIG, populate cache under lock.
                cache_lock = object.__getattribute__(self, '_cache_lock')
                with cache_lock:
                    arr = name_cache.get(name)
                    if arr is not None:
                        return arr

                    fmap = object.__getattribute__(self, '_field_index_map')
                    idx = fmap.get(name)
                    if idx is None:
                        raise AttributeError(f'Field "{name}" not found in the table.')

                    table = object.__getattribute__(self, '_table')
                    table_origin = table._origin
                    fd = schema.fields[idx]
                    if fd.field_type in {OriginFieldType.str, OriginFieldType.wstr}:
                        arr = StringColumn(table, idx, name, wide=fd.field_type == OriginFieldType.wstr)
                    elif fd.field_type == OriginFieldType.bytes:
                        arr = BytesColumn(table, idx, name)
                    else:
                        column = table_origin.get_column(idx)
                        if column is None:
                            raise AttributeError(
                                f'Field "{name}" does not expose a fixed native column.',
                            )
                        arr = column.as_nparray()
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
        return ColumnAccessor(table, feature_type)

class Table(Generic[T]):
    def __init__(self):
        self._fc: np.ndarray = np.zeros(1, dtype=np.int64)
        self._column: T | None = None
        self._feature_type: Type[T] | None = None
        self._db: core.WxDatabase | core.WxDatabaseBuild = None
        self._origin: core.WxLayerTable | core.WxLayerTableBuild | None = None
        self._fixed_fill_handler = None
        self._read_lock = RLock()

    @property
    def feature_count(self) -> int:
        return int(self._fc[0])

    @feature_count.setter
    def feature_count(self, value: int) -> None:
        self._fc[0] = value

    def __len__(self) -> int:
        return self._origin.get_feature_count()

    def __getitem__(self, index: int) -> T:
        with self._read_lock:
            count = self._origin.get_feature_count()
            if index < 0:
                index = count + index
            if index < 0 or index >= count:
                raise IndexError(f'Feature index {index} out of range [0, {count}).')

            return bind_feature(self._feature_type, self._db, self._origin, index)

    def __iter__(self) -> Generator[T, None, None]:
        for i in range(self._origin.get_feature_count()):
            with self._read_lock:
                item = bind_feature(self._feature_type, self._db, self._origin, i)
            yield item

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

        Returns a proxy typed as ``T`` for field-name autocompletion.
        At runtime, accessing any field (e.g. ``table.column.x``) returns
        the entire column as a ``numpy.ndarray``, **not** a scalar.

        Note: Python's type system cannot express "same fields as T but
        all typed as np.ndarray", so the static type of each field shows
        the scalar type (e.g. ``float``) rather than ``np.ndarray``.
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
            table._column = _create_column_accessor(feature_type, table) if feature_type is not None else None

        return table

    def _remap(
        self,
        origin: core.WxLayerTable | core.WxLayerTableBuild,
        db: core.WxDatabase | core.WxDatabaseBuild,
    ) -> None:
        self._db = db
        self._origin = origin
        if self.fixed:
            self.feature_count = origin.get_feature_count()
            self._column = (
                _create_column_accessor(self._feature_type, self)
                if self._feature_type is not None else None
            )
        else:
            self._column = None

    def rewind(self):
        self._origin.rewind()

    def fill(self, **col_arrays) -> None:
        """Bulk-fill fixed tables with validated numeric and UTF-8 string columns.

        After a successful fill(), any numpy array previously obtained from
        ``tbl.column.<field>`` is stale and must not be reused. Always
        re-fetch ``tbl.column.<field>`` after fill().
        """
        if not self.fixed:
            raise RuntimeError('fill() only supports fixed-scale tables.')
        if self._fixed_fill_handler is None:
            raise RuntimeError('fill() is unavailable for read-only fixed tables.')
        if not col_arrays:
            raise ValueError('fill() requires at least one column.')

        expected = len(self)
        writes = {}
        col = self._column
        schema = get_schema(self._feature_type)
        for field_name, values in col_arrays.items():
            field_def = schema.get(field_name)
            if field_def is None:
                raise AttributeError(
                    f'Field "{field_name}" not found in table "{self._feature_type.__name__}".'
                )
            field_type = field_def.field_type
            if field_type == OriginFieldType.list:
                raise TypeError(
                    f'Field "{field_name}" does not support fill() for type "{field_type.name}".'
                )

            column = getattr(col, field_name)
            if isinstance(column, StringColumn):
                try:
                    writes[field_name] = _normalize_string_values(values, expected)
                except ValueError as exc:
                    raise ValueError(f'{field_name} {exc}') from None
                continue

            if field_name in schema.bool_field_names:
                writes[field_name] = _normalize_bool_fill_values(
                    field_name,
                    values,
                    expected,
                )
                continue

            arr = np.ascontiguousarray(values, dtype=_FILL_NUMERIC_DTYPES[field_type])
            if len(arr) != expected:
                raise ValueError(
                    f'{field_name} expected {expected} rows, got {len(arr)}.'
                )
            writes[field_name] = arr

        self._fixed_fill_handler(writes)

    def iter_reuse(self) -> Generator[T, None, None]:
        """High-performance iterator reusing a single MappedFeature proxy.

        WARNING: Do NOT hold references to the yielded object across iterations.
        The same MappedFeature wrapper is returned with its internal pointer mutated.
        Only supported for fixed-scale tables (table.fixed == True).
        """
        if not self.fixed:
            raise RuntimeError('iter_reuse() only supports fixed-scale tables.')

        schema = get_schema(self._feature_type)
        count = self._origin.get_feature_count()
        if count == 0:
            return

        with self._read_lock:
            feat0 = self._origin.tryGetFeature(0)
        proxy = MappedFeature(self._feature_type, feat0, schema)
        yield proxy

        for i in range(1, count):
            with self._read_lock:
                feat = self._origin.tryGetFeature(i)
            object.__setattr__(proxy, '_feat', feat)
            yield proxy
