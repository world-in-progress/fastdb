import warnings
from contextlib import contextmanager
from typing import TypeVar, Generic, Type, Generator

from .. import core
from ..pipe import FeaturePipe, get_all_defns

T = TypeVar('T', bound=FeaturePipe)

class Layer(Generic[T]):
    def __init__(self):
        self.feature_count: int = 0
        self._pipe_type: Type[T] | None = None
        self._db: core.WxDatabase | core.WxDatabaseBuild = None
        self._origin: core.WxLayerTable | core.WxLayerTableBuild | None = None
    
    @staticmethod
    @contextmanager
    def editing(layer: 'Layer[T]') -> Generator[core.WxLayerTableBuild, None, None]:
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