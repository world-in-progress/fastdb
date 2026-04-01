import tempfile
import platform
import warnings
import struct as _struct
import numpy as np
from pathlib import Path
from dataclasses import dataclass, field as dc_field
from typing import List, TypeVar, Type, Any, Generic
from multiprocessing import shared_memory, resource_tracker

from .. import core
from .table import Table
from ..type import OriginFieldType, LIST_ELEM_CPP_TYPE, LIST_ELEM_DTYPE, get_list_element_type
from ..feature import Feature, get_all_defns
from ..feature._schema import get_class_schema

# Pre-compiled struct.Struct pack-method cache: (typecode, n) → bound .pack method
# struct.Struct.pack is 4× faster than struct.pack for small/medium n (avoids fmt-string lookup overhead)
_struct_pack_method_cache: dict = {}

def _get_struct_pack_method(typecode: str, n: int):
    key = (typecode, n)
    fn = _struct_pack_method_cache.get(key)
    if fn is None:
        _struct_pack_method_cache[key] = fn = _struct.Struct(f'{n}{typecode}').pack
    return fn

# Keep format cache for external consumers
_struct_fmt_cache: dict = {}

def _get_struct_fmt(typecode: str, n: int) -> str:
    key = (typecode, n)
    fmt = _struct_fmt_cache.get(key)
    if fmt is None:
        _struct_fmt_cache[key] = fmt = f'{n}{typecode}'
    return fmt

T = TypeVar('T', bound=Feature)

@dataclass
class TableDefn:
    feature_type: Type[Feature]
    feature_capacity: int
    name: str = ''
    list_capacities: dict = dc_field(default_factory=dict)
    # list_capacities = {'field_name': total_element_count} for truncate mode

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
        self._is_mutable: bool = False  # True only when origin is WxDatabaseBuild (push is allowed)

    @property
    def fixed(self) -> bool:
        return isinstance(self._origin, core.WxDatabase)
    
    @staticmethod
    def create() -> 'ORM':
        orm = ORM()
        orm._origin = core.WxDatabaseBuild()
        orm._is_mutable = True
        
        # Create default name table
        nt = _get_default_table_build(orm._origin, '_name_')
        nt.add_field('name', OriginFieldType.str.value)
        nt.add_field('ref', OriginFieldType.ref.value)
        orm._named_table = nt
        
        return orm
    
    @staticmethod
    def truncate(defns: List[TableDefn]) -> 'ORM':
        """
        Create an orm instance with fixed scale by truncating tables as defined.
        
        Note: 
            (1) Truncate operation does not support variable-length field types (str, wstr, bytes).
            (2) FeatueRef fields are supported, but the referred tables must be explicitly defined. This function does not automatically handle them for you may want to use the referred tables to store other data.
        """
        # Create orm with dynamic scales
        orm = ORM()
        orm._origin = core.WxDatabaseBuild()
        
        # Check if all defns are valid
        for defn in defns:
            if not issubclass(defn.feature_type, Feature):
                raise TypeError('feature_type must be a subclass of Feature.')
            if defn.feature_capacity <= 0:
                raise ValueError('feature_capacity must be positive.')
        
        # Populate tables with empty features
        for defn in defns:
            feature_type = defn.feature_type
            f_defns = get_all_defns(feature_type)
            for field_name, ft in f_defns:
                if ft == OriginFieldType.bytes or ft == OriginFieldType.str or ft == OriginFieldType.wstr:
                    raise ValueError(f'Table defined by feature "{defn.feature_type.__name__}" contains field "{field_name}" of type "{ft.name}". Truncate operation does not support variable-length field types (str, wstr, bytes).')
            
            # Try to get table
            table_name = defn.name if defn.name else defn.feature_type.__name__
            table: Table[T] = orm._table_map.get(table_name, None)
            if table is not None:
                table = orm._table_map[table_name]
                warnings.warn(f'Table "{table_name}" already exists, truncate operation will overwrite it.', UserWarning)
            else:
                # Create new table
                new_table = Table.map_from(feature_type, _get_default_table_build(orm._origin, table_name), orm._origin)
                
                # Define table
                for f_defn in f_defns:
                    field_name, origin_type = f_defn
                    if origin_type == OriginFieldType.list:
                        hints = get_class_schema(feature_type).hints
                        hint = hints.get(field_name)
                        elem_ot = get_list_element_type(hint) if hint is not None else None
                        cpp_elem = LIST_ELEM_CPP_TYPE.get(elem_ot, 8)  # default f64=8
                        new_table._origin.add_list_field(field_name, cpp_elem)
                    else:
                        new_table._origin.add_field(field_name, origin_type.value)
                
                # Add to table map
                orm._table_map[table_name] = new_table
                table = new_table
            orm._origin.truncate(table_name, defn.feature_capacity)
                
            # Dsssyc: Removed pushing empty features for performance consideration
            # The table trucating way modified C++ side to directly allocate features without initializing them
            # More test needed to ensure no side effects
            # Change to the old way if table truncating has issues
            # Old way:
            # empty_feature = defn.feature_type()
            # for _ in range (defn.feature_capacity):
            #     orm.push(empty_feature, table_name)
                
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
        
        # Use memory stream to combine directly
        memory_stream = core.WxMemoryStream()
        self._origin.post(memory_stream)
        buffer = memory_stream.data().as_array(np.uint8).tobytes()
        self._origin = core.WxDatabase.load_xbuffer(buffer)
        self._origin._buffer = buffer  # keep a reference to the buffer to prevent GC
        
        # TODO(Dsssyc): Deprecated: Use memory stream to combine directly
        # Removed these codes about temporary file way after full testing
        
        # # Save to a temporary file and reload
        # with tempfile.NamedTemporaryFile(delete=False) as tmp:
        #     tmp_path = str(Path(tmp.name))
        # # Use try-finally to ensure capability of tempfile using in Windows
        # try:
        #     self._origin.save(tmp_path)
        #     self._origin = core.WxDatabase.load(tmp_path)
        # finally:
        #     Path(tmp_path).unlink(missing_ok=True)
        
        # Empty build cache
        if self._shm:
            self._shm.close()
            self._shm = None
        self._table_map = {}
        self._named_table = None
        
        # Try to find named table
        # For most of the time, name table should alaways indexed at 0 if exists
        # But we still iterate through all tables to be safe, and the performance impact is negligible
        table_count = self._origin.get_layer_count()
        for i in range (table_count):
            o_table: core.WxLayerTable = self._origin.get_layer(i)
            if o_table.name() == '_name_':
                self._named_table = o_table
                break
    
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
        if not self._is_mutable:
            if self._origin is None:
                warnings.warn('Database has not connected to fastdb, not supporting push operation.', UserWarning)
            else:
                warnings.warn('Database has fixed scale, not supporting push operation.', UserWarning)
            return
        
        feature_type = feature.__class__
        
        # Route to graph-based push if feature has list fields (or skip graph for simple case)
        schema = get_class_schema(feature_type)
        if schema.list_element_types:
            if schema.has_ref_fields:
                return self._push_graph(feature, table_name=table_name,
                                         feature_name=feature_name, is_ref=is_ref)
            # Fast path: inline simple list push (no DFS, no method call overhead)
            feat_table_name = table_name if table_name else feature_type.__name__
            t_obj: Table = self._table_map.get(feat_table_name)
            if t_obj is None:
                new_table = Table.map_from(feature_type, _get_default_table_build(self._origin, feat_table_name), self._origin)
                list_elem_types = schema.list_element_types
                for fn, ft in schema.ordered_defns:
                    if ft == OriginFieldType.list:
                        cpp_elem = LIST_ELEM_CPP_TYPE.get(list_elem_types.get(fn), 8)
                        new_table._origin.add_list_field(fn, cpp_elem)
                    else:
                        new_table._origin.add_field(fn, ft.value)
                self._table_map[feat_table_name] = new_table
                t_obj = new_table
            schema.push_fn(feature._cache, t_obj._origin, _get_struct_pack_method)
            feat_idx = t_obj.feature_count
            t_obj.feature_count += 1
            if is_ref or feature_name:
                ref = t_obj._origin.create_feature_ref(feat_idx)
                if feature_name:
                    nl = self._named_table
                    nl.add_feature_begin()
                    nl.set_field_cstring(0, feature_name)
                    nl.set_field(1, ref)
                    nl.add_feature_end()
                if is_ref:
                    return ref
            return

        defns = get_all_defns(feature_type)

        # Try to get table
        table_name = table_name if table_name else feature_type.__name__
        table: Table[T] = self._table_map.get(table_name, None)
        if table is not None:
            table = self._table_map[table_name]
        else:
            # Create new table
            new_table = Table.map_from(feature_type, _get_default_table_build(self._origin, table_name), self._origin)
            
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

    def _push_simple_list(self, feature, schema, *, table_name='', feature_name='', is_ref=False):
        """Fast path for Features with only scalar + numeric-list fields (no ref fields, no DFS needed)."""
        feat_type = type(feature)
        feat_table_name = table_name if table_name else feat_type.__name__

        t_obj: Table = self._table_map.get(feat_table_name)
        if t_obj is None:
            new_table = Table.map_from(feat_type, _get_default_table_build(self._origin, feat_table_name), self._origin)
            list_elem_types = schema.list_element_types
            for fn, ft in schema.ordered_defns:
                if ft == OriginFieldType.list:
                    cpp_elem = LIST_ELEM_CPP_TYPE.get(list_elem_types.get(fn), 8)
                    new_table._origin.add_list_field(fn, cpp_elem)
                else:
                    new_table._origin.add_field(fn, ft.value)
            self._table_map[feat_table_name] = new_table
            t_obj = new_table

        schema.push_fn(feature._cache, t_obj._origin, _get_struct_pack_method)
        feat_idx = t_obj.feature_count  # before incrementing = 0-based index of just-added feature
        t_obj.feature_count += 1

        if is_ref or feature_name:
            ref = t_obj._origin.create_feature_ref(feat_idx)
            if feature_name:
                nl = self._named_table
                nl.add_feature_begin()
                nl.set_field_cstring(0, feature_name)
                nl.set_field(1, ref)
                nl.add_feature_end()
            if is_ref:
                return ref

    def _push_graph(self, root_feature, *, table_name='', feature_name='', is_ref=False):
        """Push a feature graph (with list fields / cycles) using _GraphCollector."""
        import struct
        from ._graph import _GraphCollector

        gc = _GraphCollector()
        gc.collect(root_feature)

        # id(feat) → WxFeatureRef created after writing
        built_refs: dict = {}

        for feat in gc.order:
            feat_type = type(feat)
            defns = get_all_defns(feat_type)
            feat_table_name = feat_type.__name__
            feat_schema = get_class_schema(feat_type)
            back_edge_fields = {f for (fid2, f) in gc.back_edges if fid2 == id(feat)}

            t_obj: Table = self._table_map.get(feat_table_name)
            if t_obj is None:
                new_table = Table.map_from(feat_type, _get_default_table_build(self._origin, feat_table_name), self._origin)
                for fn, ft in defns:
                    if ft == OriginFieldType.list:
                        hint = feat_schema.hints.get(fn)
                        elem_ot = get_list_element_type(hint) if hint is not None else None
                        cpp_elem = LIST_ELEM_CPP_TYPE.get(elem_ot, 8)
                        new_table._origin.add_list_field(fn, cpp_elem)
                    else:
                        new_table._origin.add_field(fn, ft.value)
                self._table_map[feat_table_name] = new_table
                t_obj = new_table

            with Table.push2(t_obj) as t:
                for idx, (fn, ft) in enumerate(defns):
                    value = feat._cache.get(fn)
                    if ft in (OriginFieldType.u8, OriginFieldType.u16, OriginFieldType.u32,
                              OriginFieldType.i32, OriginFieldType.f32, OriginFieldType.f64):
                        t.set_field(idx, value if value is not None else 0)
                    elif ft == OriginFieldType.str:
                        t.set_field_cstring(idx, value or '')
                    elif ft == OriginFieldType.wstr:
                        t.set_field_wstring(idx, value or '')
                    elif ft == OriginFieldType.bytes:
                        t.set_geometry_raw(value or b'')
                    elif ft == OriginFieldType.ref:
                        if fn not in back_edge_fields and value is not None:
                            ref = built_refs.get(id(value))
                            if ref is not None:
                                t.set_field(idx, ref)
                    elif ft == OriginFieldType.list:
                        items = value or []
                        elem_ot = feat_schema.list_element_types.get(fn)
                        if elem_ot == OriginFieldType.ref:
                            # Write refs (or zero placeholder for back-edges) so list length is correct
                            parts = []
                            _zero_ref = b'\x00\x00\x00\x00\x00'  # 5-byte null ref placeholder
                            for child in items:
                                ref = built_refs.get(id(child))
                                if ref is not None:
                                    parts.append(struct.pack('<HBH', ref.ilayer, ref.ifeature, ref.ifeatureH))
                                else:
                                    parts.append(_zero_ref)  # back-edge or unresolved; patched later
                            if parts:
                                buf = b''.join(parts)
                                t.set_field_list_numeric(idx, buf)
                        else:
                            dtype = LIST_ELEM_DTYPE.get(elem_ot, 'float64')
                            arr = np.asarray(items, dtype=dtype)
                            t.set_field_list_numeric(idx, arr.tobytes())

            feat_idx = t_obj.feature_count - 1
            ref = t_obj._origin.create_feature_ref(feat_idx)
            built_refs[id(feat)] = ref

        # Patch back-edges
        for feat_id, field_name in gc.back_edges:
            feat = next((f for f in gc.order if id(f) == feat_id), None)
            if feat is None:
                continue
            feat_type = type(feat)
            defns = get_all_defns(feat_type)
            field_idx = next((i for i, (fn, _) in enumerate(defns) if fn == field_name), None)
            if field_idx is None:
                continue
            feat_table_name = feat_type.__name__
            t_obj = self._table_map.get(feat_table_name)
            if t_obj is None:
                continue
            _, feat_local_idx = gc.id_map[feat_id]
            ft = dict(defns).get(field_name)
            val = feat._cache.get(field_name)
            if ft == OriginFieldType.ref and val is not None:
                target_ref = built_refs.get(id(val))
                if target_ref:
                    t_obj._origin.update_feature_ref(feat_local_idx, field_idx, target_ref)
            elif ft == OriginFieldType.list and val:
                for li, child in enumerate(val):
                    child_ref = built_refs.get(id(child))
                    if child_ref:
                        t_obj._origin.update_list_ref_at(feat_local_idx, field_idx, li, child_ref)

        # Register named feature (root only)
        root_table_name = type(root_feature).__name__
        t_obj = self._table_map.get(root_table_name)
        if t_obj is not None and (is_ref or feature_name):
            feat_idx = t_obj.feature_count - 1
            ref = t_obj._origin.create_feature_ref(feat_idx)
            if feature_name:
                nl = self._named_table
                nl.add_feature_begin()
                nl.set_field_cstring(0, feature_name)
                nl.set_field(1, ref)
                nl.add_feature_end()
            if is_ref:
                return ref
    
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
        """
        Close the database and release resources.
        
        Warning:
            After calling this method, the shared memory database will no longer be accessible.
            Make sure to unlink the shared memory if you want to completely remove it through the unlink() method by other processes.
        """
        if self._shm:
            # Not manually unregistering shared memory
            # However, this may cause some warnings in multiprocessing resource tracker
            # when the process that shares the memory transmits the ownership to other processes and exits without unlinking the shared memory. 
            # But it is generally safe to ignore these warnings as long as you ensure proper unlinking of shared memory when it is no longer needed.
            # May be optimized in the future if necessary.
            # resource_tracker.unregister(self._shm._name, 'shared_memory')
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
        """
        Share the database in shared memory.
        
        Note:
            macOS enforces PSHMNAMLEN = 31 chars for POSIX shm names (bsd/sys/posix_shm.h), much shorter than Linux (255).
            Keep names under 31 chars to stay cross-platform.
        """
        if self._origin is None:
            raise RuntimeError('Database is empty, cannot share.')
        if isinstance(self._origin, core.WxDatabaseBuild):
            self._combine() # combine first if still in build mode
        
        shm_name = _normalize_shm_name(shm_name)
        
        # Copy database buffer to shared memory
        chunk = self._origin.buffer()
        self._shm = shared_memory.SharedMemory(create=True, size=chunk.size, name=shm_name)
        dest = np.ndarray(chunk.size, dtype=np.uint8, buffer=self._shm.buf)
        dest[:] = chunk.as_array(np.uint8)
        
        # Release buffer reference
        self._origin._buffer = None
        
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

def _get_default_table_build(db: core.WxDatabaseBuild, t_name: str) -> core.WxLayerTableBuild:
    t = db.create_layer_begin(t_name)
    t.set_geometry_type(core.gtPoint,core.cfTx32,aabboxEnabled=True)
    t.set_extent(-180, -90, 180, 90)
    return t
