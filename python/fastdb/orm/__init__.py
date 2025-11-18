import platform
import tempfile
import warnings
from pathlib import Path
from dataclasses import dataclass
from multiprocessing import shared_memory
from typing import List, TypeVar, Type, Any, Generic

from .. import core
from .table import Table
from ..type import OriginFieldType
from ..pipe import FeaturePipe, get_all_defns

T = TypeVar('T', bound=FeaturePipe)

@dataclass
class TableDefn:
    pipe_type: Type[FeaturePipe]
    feature_capacity: int
    name: str = ''

class TableBuilder(Generic[T]):
    def __init__(self, pipe_type: Type[T], block: 'ORM'):
        if not issubclass(pipe_type, FeaturePipe):
            raise TypeError('pipe_type must be a subclass of FeaturePipe.')
        self._block = block
        self._pipe_type = pipe_type
    
    def __getitem__(self, layer_name: str | Type[T]) -> Table[T]:
        if not isinstance(layer_name, str):
            layer_name = layer_name.__name__
            if layer_name != self._pipe_type.__name__:
                raise TypeError('layer_name must match the pipe_type name if you use a pipe type as the layer name.')
            
        if layer_name in self._block._layer_map:
            return self._block._layer_map[layer_name]
        
        db = self._block._origin
        layer_count = db.get_layer_count()
        for i in range (layer_count):
            o_layer: core.WxLayerTable = db.get_layer(i)
            if o_layer.name() == layer_name:
                layer = Table[T]()
                layer.map_from(self._pipe_type, o_layer, db)
                
                self._block._layer_map[layer_name] = layer
                return layer
        raise KeyError(f'Layer "{layer_name}" not found in block.')
        

class ORM:
    def __init__(self):
        self._shm: shared_memory.SharedMemory | None = None
        self._layer_map: dict[str, Table | TableBuilder] = {}
        self._origin: core.WxDatabase | core.WxDatabaseBuild | None = None
        self._named_table: core.WxLayerTable | core.WxLayerTableBuild | None = None

    @property
    def fixed(self) -> bool:
        return isinstance(self._origin, core.WxDatabase)
    
    @staticmethod
    def create() -> 'ORM':
        block = ORM()
        block._origin = core.WxDatabaseBuild()
        
        # Create default name layer
        nl: core.WxLayerTableBuild = block._origin.create_layer_begin('_name_')
        nl.add_field('name', OriginFieldType.str.value)
        nl.add_field('ref', OriginFieldType.ref.value)
        block._named_table = nl
        
        return block
    
    @staticmethod
    def truncate(scales: List[TableDefn]) -> 'ORM':
        # Create block with dynamic scales
        block = ORM()
        block._origin = core.WxDatabaseBuild()
        
        # Check if all scales are valid
        for scale in scales:
            if not issubclass(scale.pipe_type, FeaturePipe):
                raise TypeError('pipe_type must be a subclass of FeaturePipe.')
            if scale.feature_capacity <= 0:
                raise ValueError('feature_capacity must be positive.')
        
        # Populate layers
        db: core.WxDatabaseBuild = block._origin
        for scale in scales:
            empty_pipe = scale.pipe_type()
            layer_name = scale.name if scale.name else scale.pipe_type.__name__
            for _ in range (0, scale.feature_capacity):
                block.push(empty_pipe, layer_name)
                
        # Combine the memory by saving and reloading
        block._combine()
        return block
    
    def _combine(self):
        """Combine memory from all layers into a single continuous block."""
        # Check if block need to be combined
        if self._origin is None:
            warnings.warn('Block is empty, cannot combine.', UserWarning)
            return
        if isinstance(self._origin, core.WxDatabase):
            warnings.warn('Block has been combined, no need to combine again.', UserWarning)
            return
        
        # Save to a temporary file and reload
        with tempfile.NamedTemporaryFile(delete=False) as tmp:
            tmp_path = str(Path(tmp.name))
        # Use try-finally to ensure capability of tempfile using in Windows
        try:
            self._origin.save(tmp_path)
            self._origin = core.WxDatabase.load(tmp_path)
        finally:
            Path(tmp_path).unlink(missing_ok=True)
    
    @staticmethod
    def load(name: str, from_file: bool = False) -> 'ORM':
        """Create a Block instance by loading from file system or shared memory."""
        block = ORM()
        
        # Try to load block from file system
        if from_file:
            path = Path(name)
            if path.exists():
                block._origin = core.WxDatabase.load(str(path))
            else:
                raise FileNotFoundError(f"Block '{name}' not found in file system.")
        
        # Try to load block from shared memory
        else:
            name = _normalize_shm_name(name)
            try:
                block._shm = shared_memory.SharedMemory(name=name)
                block._origin = core.WxDatabase.load_xbuffer(block._shm.buf)
            
            except FileNotFoundError:
                raise FileNotFoundError(f"Block '{name}' not found in shared memory.")
        
        # Try to find name layer
        # For most of the time, name layer should alaways indexed at 0 if exists
        # But we still iterate through all layers to be safe, and the performance impact is negligible
        layer_count = block._origin.get_layer_count()
        for i in range (layer_count):
            o_layer: core.WxLayerTable = block._origin.get_layer(i)
            if o_layer.name() == '_name_':
                block._named_table = o_layer
                break
        
        return block

    def push(self, pipe: T, layer_name: str = '', *, name: str = '', is_ref=False) -> Any:
        """Push the given feature pipe to the block database."""
        # Check if is synchronizable
        if self._origin is None:
            warnings.warn('Block has not connected to fastdb, not supporting push operation.', UserWarning)
            return
        if self.fixed:
            warnings.warn('Block has fixed scale, not supporting push operation.', UserWarning)
            return
        if not isinstance(pipe, FeaturePipe):
            warnings.warn('Provided pipe is not an instance of FeaturePipe.', UserWarning)
            return
        
        pipe_type = pipe.__class__
        defns = get_all_defns(pipe_type)
        
        # Try to get layer
        layer_name = layer_name if layer_name else pipe_type.__name__
        layer: Table[T] = self._layer_map.get(layer_name, None)
        if layer is not None:
            layer = self._layer_map[layer_name]
        else:
            # Create new layer
            new_layer = Table[pipe_type]()
            new_layer.map_from(pipe_type, self._origin.create_layer_begin(layer_name), self._origin)
            
            # Define layer
            for defn in defns:
                field_name, origin_type = defn
                new_layer._origin.add_field(field_name, origin_type.value)
            
            # Add to layer map
            self._layer_map[layer_name] = new_layer
            layer = new_layer
        
        # Push pipe data to layer
        with Table.push2(layer) as l:
            for idx, (fn, ft) in enumerate(defns):
                value = getattr(pipe, fn)
                if ft == OriginFieldType.u8     \
                or ft == OriginFieldType.u16    \
                or ft == OriginFieldType.u32    \
                or ft == OriginFieldType.i32    \
                or ft == OriginFieldType.f32    \
                or ft == OriginFieldType.f64:
                    l.set_field(idx, value)
                elif ft == OriginFieldType.str:
                    l.set_field_cstring(idx, value)
                elif ft == OriginFieldType.wstr:
                    l.set_field_wstring(idx, value)
                elif ft == OriginFieldType.bytes:
                    l.set_geometry_raw(value)
                elif ft == OriginFieldType.ref:
                    fref: FeaturePipe = value
                    ref = self.push(fref, is_ref=True)
                    l.set_field(idx, ref)
                else:
                    warnings.warn(f'Unsupported field type "{ft}" for field "{fn}".', UserWarning)

        # Create a ref to the just added feature for it is a ref or need to be named
        if is_ref or name:
            pipe_idx = layer.feature_count - 1
            ref = layer._origin.create_feature_ref(pipe_idx)
            
            if not name:
                return ref
        
            # Add ref feature to name layer if name is provided
            if name:
                nl = self._named_table
                nl.add_feature_begin()
                nl.set_field_cstring(0, name)
                nl.set_field(1, ref)
                nl.add_feature_end()
    
    def get(self, pipe_type: Type[T], name: str) -> T | None:
        """Get feature pipe by name from the block."""
        if self._origin is None:
            raise RuntimeError('Block is empty, cannot get feature.')
        if not self.fixed:
            raise RuntimeError('Block still in build mode, cannot get feature.')
        if self._named_table is None:
            raise RuntimeError('Block has no name layer, cannot get feature by name.')
        
        # Search name layer for the given name
        of: core.WxFeature | None = None
        nl = self._named_table
        nl.rewind()
        while nl.next():
            n = nl.get_field_as_string(0)
            if n == name:
                ref = nl.get_field_as_ref(1)
                of = self._origin.tryGetFeature(ref)
                break
        if not of:
            return None
        
        # Create pipe
        pipe = pipe_type()
        pipe.map_from(self._origin, of.layer(), of)
        return pipe

    def close(self):
        """Close the block and release resources."""
        if self._shm:
            self._shm.close()
            self._shm = None
            self._origin = None
    
    def unlink(self):
        """Unlink the shared memory block."""
        if self._shm:
            self._shm.unlink()
            self._shm = None
            self._origin = None
    
    def share(self, shm_name: str, close_after: bool = False):
        """Share the block in shared memory."""
        if self._origin is None:
            raise RuntimeError('Block is empty, cannot share.')
        if isinstance(self._origin, core.WxDatabaseBuild):
            self._combine() # combine first if still in build mode
        
        shm_name = _normalize_shm_name(shm_name)
        
        # Copy database buffer to shared memory
        chunk = self._origin.buffer()
        self._shm = shared_memory.SharedMemory(create=True, size=chunk.size, name=shm_name)
        chunk.copy_to_buffer(self._shm.buf)
        
        # Reload database from shared memory
        self._origin = core.WxDatabase.load_xbuffer(self._shm.buf)
        
        if close_after and platform.system() != 'Windows':
            self.close()
    
    def save(self, path: str):
        """Save the block to a file."""
        if self._origin is None:
            raise RuntimeError('Block is empty, cannot save.')
        
        # Directly save database to file if _db is WxDatabaseBuild
        if isinstance(self._origin, core.WxDatabaseBuild):
            self._origin.save(path)
        else:
            # Get database buffer and write to file
            chunk: core.chunk_data_t = self._origin.buffer()
            with open(path, 'wb') as f:
                f.write(chunk.to_bytes())
        
    def __len__(self):
        """Return the number of layers in the block."""
        if self._origin is None:
            return 0
        return self._origin.get_layer_count()
    
    def __getitem__(self, pipe_type: Type[T]) -> TableBuilder[T]:
        """Get layer by index with specified feature schema."""
        is_name = isinstance(pipe_type, str)
        if self._origin is None:
            raise RuntimeError('Block is empty, cannot access layers.')
        if not issubclass(pipe_type, FeaturePipe):
            raise TypeError('pipe_type must be a subclass of FeaturePipe.')
        
        layer_name = pipe_type.__name__
        return TableBuilder[T](pipe_type, self)
    
# Helpers ##################################################

def _normalize_shm_name(shm_name: str) -> str:
    if platform.system() != 'Windows':
        return shm_name
    if shm_name.startswith(('Local\\', 'Global\\')):
        return shm_name
    return f'Local\\{shm_name}'
