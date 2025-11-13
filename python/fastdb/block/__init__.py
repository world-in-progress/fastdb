import tempfile
import warnings
from pathlib import Path
from dataclasses import dataclass
from multiprocessing import shared_memory
from typing import List, TypeVar, Type, Any

from .. import core
from .layer import Layer
from ..type import OriginFieldType
from ..pipe import STR, REF, FeaturePipe, get_all_defns

T = TypeVar('T', bound=FeaturePipe)

@dataclass
class BlockScale:
    pipe_type: Type[FeaturePipe]
    feature_capacity: int

class Block:
    def __init__(self):
        self._layer_map: dict[str, Layer] = {}
        self._shm: shared_memory.SharedMemory | None = None
        self._origin: core.WxDatabase | core.WxDatabaseBuild | None = None
        self._name_layer: core.WxLayerTable | core.WxLayerTableBuild | None = None

    @property
    def fixed(self) -> bool:
        return isinstance(self._origin, core.WxDatabase)
    
    @staticmethod
    def create() -> 'Block':
        block = Block()
        block._origin = core.WxDatabaseBuild()
        
        # Create default name layer
        nl: core.WxLayerTableBuild = block._origin.create_layer_begin('_name_')
        nl.add_field('name', OriginFieldType.str.value)
        nl.add_field('ref', OriginFieldType.ref.value)
        block._name_layer = nl
        
        return block
    
    @staticmethod
    def truncate(scales: List[BlockScale]) -> 'Block':
        # Check if all scales are valid
        for scale in scales:
            if not issubclass(scale.pipe_type, FeaturePipe):
                raise TypeError('pipe_type must be a subclass of FeaturePipe.')
            if scale.feature_capacity <= 0:
                raise ValueError('feature_capacity must be positive.')
        
        # Create db instance
        db = core.WxDatabaseBuild()
        for scale in scales:
            defns = get_all_defns(scale.pipe_type)
            
            # Define layer
            layer: core.WxLayerTableBuild = db.create_layer_begin(scale.pipe_type.__name__)
            for defn in defns:
                field_name, origin_type = defn
                layer.add_field(field_name, origin_type.value)
                
            # Fill layer with specified feature capacity
            for _ in range (0, scale.feature_capacity):
                layer.add_feature_begin()
                layer.add_feature_end()
        
        # Save db to a temporary file and reload
        with tempfile.NamedTemporaryFile() as tmp:
            tmp_path = str(Path(tmp.name))
            db.save(tmp_path)
            db = core.WxDatabase.load(tmp_path)
        
            # Return as a block
            block = Block()
            block._origin = db
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
        with tempfile.NamedTemporaryFile() as tmp:
            tmp_path = str(Path(tmp.name))
            self._origin.save(tmp_path)
            self._origin = core.WxDatabase.load(tmp_path)
    
    @staticmethod
    def load(name: str, from_file: bool = False) -> 'Block':
        """Create a Block instance by loading from file system or shared memory."""
        block = Block()
        
        # Try to load block from file system
        if from_file:
            path = Path(name)
            if path.exists():
                block._origin = core.WxDatabase.load(str(path))
            else:
                raise FileNotFoundError(f"Block '{name}' not found in file system.")
        
        # Try to load block from shared memory
        else:
            try:
                block._shm = shared_memory.SharedMemory(name=name)
                block._origin = core.WxDatabase.load_xbuffer(block._shm.buf)
            
            except FileNotFoundError:
                raise FileNotFoundError(f"Block '{name}' not found in shared memory.")
        
        # Try to find name layer
        layer_count = block._origin.get_layer_count()
        for i in range (layer_count):
            o_layer: core.WxLayerTable = block._origin.get_layer(i)
            if o_layer.name() == '_name_':
                block._name_layer = o_layer
                break
        
        return block

    def push(self, pipe: T, name: str = '') -> Any:
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
        layer_name = pipe_type.__name__
        layer: Layer[T] = self._layer_map.get(layer_name, None)
        if layer is not None:
            layer = self._layer_map[layer_name]
        else:
            # Create new layer
            new_layer = Layer[pipe_type]()
            new_layer.map_from(pipe_type, self._origin.create_layer_begin(layer_name), self._origin)
            
            # Define layer
            for defn in defns:
                field_name, origin_type = defn
                new_layer._origin.add_field(field_name, origin_type.value)
            
            # Add to layer map
            self._layer_map[layer_name] = new_layer
            layer = new_layer
        
        # Push pipe data to layer
        with Layer.editing(layer) as l:
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
                    ref = self.push(fref)
                    l.set_field(idx, ref)
                else:
                    warnings.warn(f'Unsupported field type "{ft}" for field "{fn}".', UserWarning)

        # Create a ref to the just added feature
        pipe_idx = layer.feature_count - 1
        ref = layer._origin.create_feature_ref(pipe_idx)
        
        # Add ref feature to name layer if name is provided
        if name:
            nl = self._name_layer
            nl.add_feature_begin()
            nl.set_field_cstring(0, name)
            nl.set_field(1, ref)
            nl.add_feature_end()
        
        return ref
    
    def get(self, pipe_type: Type[T], name: str) -> T | None:
        """Get feature pipe by name from the block."""
        if self._origin is None:
            raise RuntimeError('Block is empty, cannot get feature.')
        if self._name_layer is None:
            raise RuntimeError('Block has no name layer, cannot get feature by name.')
        
        # Search name layer for the given name
        of: core.WxFeature | None = None
        nl = self._name_layer
        nl.rewind()
        while nl.next():
            n = nl.get_field_as_string(0)
            if n == name:
                ref = nl.get_field_as_ref(1)
                of = self._origin.tryGetFeature(ref)
                break
        nl.rewind()
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
    
    def share(self, name: str, close_after: bool = False):
        """Share the block in shared memory."""
        if self._origin is None:
            raise RuntimeError('Block is empty, cannot share.')
        if isinstance(self._origin, core.WxDatabaseBuild):
            self._combine() # combine first if still in build mode
        
        if self._shm:
            self._shm.close()
            self._shm = None
        
        # Copy database buffer to shared memory
        chunk = self._origin.buffer()
        self._shm = shared_memory.SharedMemory(create=True, size=chunk.size, name=name)
        chunk.copy_to_buffer(self._shm.buf)
        
        # Reload database from shared memory
        self._origin = core.WxDatabase.load_xbuffer(self._shm.buf)
        
        if close_after:
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
    
    def __getitem__(self, pipe_type: Type[T]) -> Layer[T]:
        """Get layer by index with specified feature schema."""
        if self._origin is None:
            raise RuntimeError('Block is empty, cannot access layers.')
        if not issubclass(pipe_type, FeaturePipe):
            raise TypeError('pipe_type must be a subclass of FeaturePipe.')
        
        layer_name = pipe_type.__name__
        if layer_name in self._layer_map:
            return self._layer_map[layer_name]
        else:
            layer_count = self._origin.get_layer_count()
            for i in range (layer_count):
                o_layer: core.WxLayerTable = self._origin.get_layer(i)
                if o_layer.name() == layer_name:
                    layer = Layer[T]()
                    layer.map_from(pipe_type, o_layer, self._origin)
                    return layer
        
        raise KeyError(f'Layer "{layer_name}" not found in block.')
