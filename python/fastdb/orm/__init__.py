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
from ..feature import Feature, get_all_defns

T = TypeVar('T', bound=Feature)

@dataclass
class TableDefn:
    feature_type: Type[Feature]
    feature_capacity: int
    name: str = ''

class TableBuilder(Generic[T]):
    def __init__(self, feature_type: Type[T], orm: 'ORM'):
        if not issubclass(feature_type, Feature):
            raise TypeError('Feature_type must be a subclass of Feature.')
        self._orm = orm
        self._feature_type = feature_type
    
    def __getitem__(self, table_name: str | Type[T]) -> Table[T]:
        if not isinstance(table_name, str):
            table_name = table_name.__name__
            if table_name != self._feature_type.__name__:
                raise TypeError('table_name must match the feature_type name if you use a feature type as the table name.')
            
        if table_name in self._orm._table_map:
            return self._orm._table_map[table_name]
        
        db = self._orm._origin
        table_count = db.get_layer_count()
        for i in range (table_count):
            o_table: core.WxLayerTable = db.get_layer(i)
            if o_table.name() == table_name:
                table = Table.map_from(self._feature_type, o_table, db)
                
                self._orm._table_map[table_name] = table
                return table
        raise KeyError(f'Table "{table_name}" not found in fastdb.')

class ORM:
    def __init__(self):
        self._shm: shared_memory.SharedMemory | None = None
        self._table_map: dict[str, Table | TableBuilder] = {}
        self._origin: core.WxDatabase | core.WxDatabaseBuild | None = None
        self._named_table: core.WxLayerTable | core.WxLayerTableBuild | None = None

    @property
    def fixed(self) -> bool:
        return isinstance(self._origin, core.WxDatabase)
    
    @staticmethod
    def create() -> 'ORM':
        orm = ORM()
        orm._origin = core.WxDatabaseBuild()
        
        # Create default name table
        nl: core.WxLayerTableBuild = orm._origin.create_layer_begin('_name_')
        nl.add_field('name', OriginFieldType.str.value)
        nl.add_field('ref', OriginFieldType.ref.value)
        orm._named_table = nl
        
        return orm
    
    @staticmethod
    def truncate(scales: List[TableDefn]) -> 'ORM':
        # Create orm with dynamic scales
        orm = ORM()
        orm._origin = core.WxDatabaseBuild()
        
        # Check if all scales are valid
        for scale in scales:
            if not issubclass(scale.feature_type, Feature):
                raise TypeError('feature_type must be a subclass of Feature.')
            if scale.feature_capacity <= 0:
                raise ValueError('feature_capacity must be positive.')
        
        # Populate tables with empty features
        for scale in scales:
            empty_feature = scale.feature_type()
            table_name = scale.name if scale.name else scale.feature_type.__name__
            for _ in range (scale.feature_capacity):
                orm.push(empty_feature, table_name)
                
        # Combine the memory by saving and reloading
        orm._combine()
        return orm
    
    def _combine(self):
        """Combine memory from all tables into a single continuous block."""
        # Check if database need to be combined
        if self._origin is None:
            warnings.warn('Database is empty, cannot combine.', UserWarning)
            return
        if isinstance(self._origin, core.WxDatabase):
            warnings.warn('Database has been combined, no need to combine again.', UserWarning)
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
        """Create an orm instance by loading from file system or shared memory."""
        orm = ORM()
        
        # Try to load database from file system
        if from_file:
            path = Path(name)
            if path.exists():
                orm._origin = core.WxDatabase.load(str(path))
            else:
                raise FileNotFoundError(f"Database '{name}' not found in file system.")
        
        # Try to load database from shared memory
        else:
            name = _normalize_shm_name(name)
            try:
                orm._shm = shared_memory.SharedMemory(name=name)
                orm._origin = core.WxDatabase.load_xbuffer(orm._shm.buf)
            
            except FileNotFoundError:
                raise FileNotFoundError(f"Database '{name}' not found in shared memory.")
        
        # Try to find named table
        # For most of the time, name table should alaways indexed at 0 if exists
        # But we still iterate through all tables to be safe, and the performance impact is negligible
        table_count = orm._origin.get_layer_count()
        for i in range (table_count):
            o_table: core.WxLayerTable = orm._origin.get_layer(i)
            if o_table.name() == '_name_':
                orm._named_table = o_table
                break
        
        return orm

    def push(self, feature: T, table_name: str = '', *, feature_name: str = '', is_ref=False) -> Any:
        """Push the given feature to the database."""
        # Check if is synchronizable
        if self._origin is None:
            warnings.warn('Database has not connected to fastdb, not supporting push operation.', UserWarning)
            return
        if self.fixed:
            warnings.warn('Database has fixed scale, not supporting push operation.', UserWarning)
            return
        if not isinstance(feature, Feature):
            warnings.warn('Provided feature is not an instance of Feature.', UserWarning)
            return
        
        feature_type = feature.__class__
        defns = get_all_defns(feature_type)
        
        # Try to get table
        table_name = table_name if table_name else feature_type.__name__
        table: Table[T] = self._table_map.get(table_name, None)
        if table is not None:
            table = self._table_map[table_name]
        else:
            # Create new table
            new_table = Table.map_from(feature_type, self._origin.create_layer_begin(table_name), self._origin)
            
            # Define table
            for defn in defns:
                field_name, origin_type = defn
                new_table._origin.add_field(field_name, origin_type.value)
            
            # Add to table map
            self._table_map[table_name] = new_table
            table = new_table
        
        # Push feature data to table
        with Table.push2(table) as t:
            for idx, (fn, ft) in enumerate(defns):
                value = getattr(feature, fn)
                if ft == OriginFieldType.u8     \
                or ft == OriginFieldType.u16    \
                or ft == OriginFieldType.u32    \
                or ft == OriginFieldType.i32    \
                or ft == OriginFieldType.f32    \
                or ft == OriginFieldType.f64:
                    t.set_field(idx, value)
                elif ft == OriginFieldType.str:
                    t.set_field_cstring(idx, value)
                elif ft == OriginFieldType.wstr:
                    t.set_field_wstring(idx, value)
                elif ft == OriginFieldType.bytes:
                    t.set_geometry_raw(value)
                elif ft == OriginFieldType.ref:
                    fref: Feature = value
                    ref = self.push(fref, is_ref=True)
                    t.set_field(idx, ref)
                else:
                    warnings.warn(f'Unsupported field type "{ft}" for field "{fn}".', UserWarning)

        # Create a ref to the just added feature for it is a ref or need to be named
        if is_ref or feature_name:
            feature_idx = table.feature_count - 1
            ref = table._origin.create_feature_ref(feature_idx)
            
            if not feature_name:
                return ref
        
            # Add ref feature to named table if feature_name is provided
            if feature_name:
                nl = self._named_table
                nl.add_feature_begin()
                nl.set_field_cstring(0, feature_name)
                nl.set_field(1, ref)
                nl.add_feature_end()
    
    def get(self, feature_type: Type[T], name: str) -> T | None:
        """Get feature by name from the database."""
        if self._origin is None:
            raise RuntimeError('Database is empty, cannot get feature.')
        if not self.fixed:
            raise RuntimeError('Database still in build mode, cannot get feature.')
        if self._named_table is None:
            raise RuntimeError('Database has no named table, cannot get feature by name.')
        
        # Search named table for the given name
        of: core.WxFeature | None = None
        nt = self._named_table
        nt.rewind()
        while nt.next():
            n = nt.get_field_as_string(0)
            if n == name:
                ref = nt.get_field_as_ref(1)
                of = self._origin.tryGetFeature(ref)
                break
        if not of:
            return None
        
        # Create feature and map from origin feature
        return feature_type.map_from(self._origin, of)

    def close(self):
        """Close the database and release resources."""
        if self._shm:
            self._shm.close()
            self._shm = None
            self._origin = None
    
    def unlink(self):
        """Unlink the shared memory database."""
        if self._shm:
            self._shm.unlink()
            self._shm = None
            self._origin = None
    
    def share(self, shm_name: str, close_after: bool = False):
        """Share the database in shared memory."""
        if self._origin is None:
            raise RuntimeError('Database is empty, cannot share.')
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
        """Save the database to a file."""
        if self._origin is None:
            raise RuntimeError('Database is empty, cannot save.')
        
        # Directly save database to file if _db is WxDatabaseBuild
        if isinstance(self._origin, core.WxDatabaseBuild):
            self._origin.save(path)
        else:
            # Get database buffer and write to file
            chunk: core.chunk_data_t = self._origin.buffer()
            with open(path, 'wb') as f:
                f.write(chunk.to_bytes())
        
    def __len__(self):
        """Return the number of tables in the database."""
        if self._origin is None:
            return 0
        return self._origin.get_layer_count()
    
    def __getitem__(self, feature_type: Type[T]) -> TableBuilder[T]:
        """Get table builder by specific feature type."""
        if self._origin is None:
            raise RuntimeError('Database is empty, cannot access tables.')
        if not issubclass(feature_type, Feature):
            raise TypeError('feature_type must be a subclass of Feature.')
        
        return TableBuilder[T](feature_type, self)
    
# Helpers ##################################################

def _normalize_shm_name(shm_name: str) -> str:
    if platform.system() != 'Windows':
        return shm_name
    if shm_name.startswith(('Local\\', 'Global\\')):
        return shm_name
    return f'Local\\{shm_name}'
