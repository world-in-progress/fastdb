import weakref
import numpy as np
from threading import Lock
from contextlib import contextmanager
from typing import TypeVar, Generic, Type, Generator, get_type_hints

from .. import core
from ..pipe import FeaturePipe, get_all_defns

T = TypeVar('T', bound=FeaturePipe)

_column_accessor_cache = weakref.WeakKeyDictionary()
_column_accessor_cache_lock = Lock()

def _create_column_accessor(pipe_type: Type[T], layer_origin, pipe_type_class) -> T:
    """
    Create a column accessor that provides numpy array access with proper type hints.
    
    This dynamically creates a class with the same field names as pipe_type,
    but all fields are typed as np.ndarray instead of their original types (F64, STR, etc).
    """
    with _column_accessor_cache_lock:
        if pipe_type in _column_accessor_cache:
            ColumnAccessorClass = _column_accessor_cache[pipe_type]
            return ColumnAccessorClass(layer_origin, pipe_type_class)
        
        # Get original annotations from pipe_type
        original_annotations = {}
        if hasattr(pipe_type, '__annotations__'):
            original_annotations = pipe_type.__annotations__.copy()
        
        # Create new annotations where all types become np.ndarray
        new_annotations = {}
        for field_name, field_type in original_annotations.items():
            if not field_name.startswith('_'):
                new_annotations[field_name] = np.ndarray
        
        # Create the dynamic column accessor class with modified annotations
        class ColumnAccessor:
            """Column accessor that returns numpy arrays for field access"""
            
            # Set the new annotations
            __annotations__ = new_annotations
            
            def __init__(self, layer_origin, pipe_type_class):
                # Don't call parent __init__ to avoid initializing cache
                # just set internal references
                object.__setattr__(self, '_layer_origin', layer_origin)
                object.__setattr__(self, '_pipe_type_class', pipe_type_class)
            
            def __getattr__(self, name: str) -> np.ndarray:
                """Override to return numpy array instead of single value"""
                # Get field definitions
                defns = get_all_defns(object.__getattribute__(self, '_pipe_type_class'))
                
                for idx, (field_name, _) in enumerate(defns):
                    if field_name == name:
                        layer_origin = object.__getattribute__(self, '_layer_origin')
                        column = layer_origin.get_column(idx)
                        return column.as_nparray()
                
                raise AttributeError(f'Field "{name}" not found in the layer.')
            
            def __setattr__(self, name: str, value):
                """Prevent setting attributes on column accessor"""
                if name.startswith('_'):
                    object.__setattr__(self, name, value)
                else:
                    raise AttributeError(
                        f'Cannot set field "{name}" on column accessor. '
                        'Use layer[index].{name} = value to modify individual features.'
                    )
        
        # Cache the class, not the instance
        _column_accessor_cache[pipe_type] = ColumnAccessor
        return ColumnAccessor(layer_origin, pipe_type_class)
    
    # Create a dynamic class that inherits from the pipe type
    class ColumnAccessor(pipe_type):
        """Column accessor that returns numpy arrays for field access"""
        
        def __init__(self, layer_origin, pipe_type_class):
            # Don't call parent __init__ to avoid initializing cache
            # Just set internal references
            object.__setattr__(self, '_layer_origin', layer_origin)
            object.__setattr__(self, '_pipe_type_class', pipe_type_class)
        
        def __getattr__(self, name: str) -> np.ndarray:
            """Override to return numpy array instead of single value"""
            # Get field definitions
            defns = get_all_defns(object.__getattribute__(self, '_pipe_type_class'))
            
            for idx, (field_name, _) in enumerate(defns):
                if field_name == name:
                    layer_origin = object.__getattribute__(self, '_layer_origin')
                    column = layer_origin.get_column(idx)
                    return column.as_nparray()
            
            raise AttributeError(f'Field "{name}" not found in the layer.')
        
        def __setattr__(self, name: str, value):
            """Prevent setting attributes on column accessor"""
            if name.startswith('_'):
                object.__setattr__(self, name, value)
            else:
                raise AttributeError(
                    f'Cannot set field "{name}" on column accessor. '
                    'Use layer[index].{name} = value to modify individual features.'
                )
    
    return ColumnAccessor(layer_origin, pipe_type_class)

class Layer(Generic[T]):
    def __init__(self):
        self.feature_count: int = 0
        self._column_pipe: T | None = None
        self._pipe_type: Type[T] | None = None
        self._db: core.WxDatabase | core.WxDatabaseBuild = None
        self._origin: core.WxLayerTable | core.WxLayerTableBuild | None = None
    
    @staticmethod
    @contextmanager
    def push2(layer: 'Layer[T]') -> Generator[core.WxLayerTableBuild, None, None]:
        """Context manager to push features to the given layer."""
        if layer._db is None or layer._origin is None:
            raise RuntimeError('Layer has not connected to fastdb, not supporting push operation.')
        if layer.fixed:
            raise RuntimeError('Layer has fixed scale, not supporting push operation.', UserWarning)
        
        layer._origin.add_feature_begin()
        
        yield layer._origin  # type: core.WxLayerTableBuild
        
        layer._origin.add_feature_end()
        layer.feature_count += 1
        
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
        if self._column_pipe is None:
            raise RuntimeError('Layer has not been mapped with a pipe type.')
        return self._column_pipe
    
    @property
    def fixed(self) -> bool:
        return isinstance(self._origin, core.WxLayerTable)
    
    def map_from(
        self,
        pipe_type: Type[T] | None,
        origin: core.WxLayerTable | core.WxLayerTableBuild,
        db: core.WxDatabase | core.WxDatabaseBuild
    ):
        self._db = db
        self._origin = origin
        self._pipe_type = pipe_type
        
        # Get feature count if the fastdb layer has fixed scale
        if self.fixed:
            self.feature_count = origin.get_feature_count()
            # Create column accessor that pretends to be T but returns numpy arrays
            self._column_pipe = _create_column_accessor(pipe_type, origin, pipe_type) if pipe_type is not None else None
    
    def __len__(self) -> int:
        return self._origin.get_feature_count()
    
    def __getitem__(self, index: int) -> T:
        # Check index bounds
        if index < 0 or index >= self._origin.get_feature_count():
            raise IndexError(f'Feature index {index} out of range [0, {self._origin.get_feature_count()}].')
        # Get pipe
        pipe = self._pipe_type()
        pipe.map_from(self._db, self._origin, self._origin.tryGetFeature(index))
        return pipe
    
    def __iter__(self) -> Generator[T, None, None]:
        for i in range(self._origin.get_feature_count()):
            pipe = self._pipe_type()
            pipe.map_from(self._db, self._origin, self._origin.tryGetFeature(i))
            yield pipe
    
    @property
    def next(self) -> bool:
        return self._origin.next()
    
    @property
    def row(self) -> int:
        return self._origin.row()
    
    def rewind(self):
        self._origin.rewind()