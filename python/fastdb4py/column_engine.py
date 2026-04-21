# python/fastdb4py/column_engine.py
"""ColumnEngine: OLAP/batch columnar workloads (no REF field support)."""
import platform
import warnings
import numpy as np
from pathlib import Path
from typing import List, TypeVar, Type
from multiprocessing import shared_memory

from . import core
from .registry import get_schema, is_feature, LayerSchema
from .layout import Layout
from .orm.table import Table
from .type import OriginFieldType
from .push_compiler import (
    make_inlined_dispatch, make_batch_inlined_dispatch,
)

T = TypeVar('T')


def _normalize_shm_name(shm_name: str) -> str:
    if platform.system() != 'Windows':
        return shm_name
    if shm_name.startswith(('Local\\', 'Global\\')):
        return shm_name
    return f'Local\\{shm_name}'


def _get_default_table_build(db: core.WxDatabaseBuild, t_name: str) -> core.WxLayerTableBuild:
    t = db.create_layer_begin(t_name)
    t.set_geometry_type(core.gtPoint, core.cfTx32, aabboxEnabled=True)
    t.set_extent(-180, -90, 180, 90)
    return t


def _reject_ref(schema: LayerSchema, cls_name: str) -> None:
    """Raise TypeError if schema contains REF fields."""
    if schema.has_ref_fields:
        ref_names = [fd.name for fd in schema.ref_fields + schema.list_ref_fields]
        raise TypeError(
            f"ColumnEngine does not support REF fields. "
            f"{cls_name} has REF fields: {ref_names}. "
            f"Use ObjectEngine for classes with references."
        )


class ColumnEngine:
    def __init__(self):
        self._shm: shared_memory.SharedMemory | None = None
        self._table_map: dict[str, Table] = {}
        self._table_feature_types: dict[str, Type] = {}
        self._origin: core.WxDatabase | core.WxDatabaseBuild | None = None
        self._is_mutable: bool = False
        self._fixed_build: core.WxDatabaseBuild | None = None
        self._fixed_layer_builds: dict[str, core.WxLayerTableBuild] = {}
        self._fixed_table_fields: dict[str, dict[str, int]] = {}
        # Per-class push dispatch caches
        self._push_buf: dict = {}
        self._push_batch_fn: dict = {}
        self._push_dispatch: dict = {}

    @property
    def fixed(self) -> bool:
        return isinstance(self._origin, core.WxDatabase)

    # ------------------------------------------------------------------
    # Factory methods
    # ------------------------------------------------------------------

    @staticmethod
    def create() -> 'ColumnEngine':
        engine = ColumnEngine()
        engine._origin = core.WxDatabaseBuild()
        engine._is_mutable = True
        return engine

    @staticmethod
    def truncate(layouts: List[Layout]) -> 'ColumnEngine':
        """Create a ColumnEngine with fixed-size pre-allocated tables.

        Supports UTF-8 ``STR`` columns. ``WSTR`` and ``BYTES`` remain unsupported.
        Rejects any class with REF fields.
        """
        engine = ColumnEngine()
        engine._fixed_build = core.WxDatabaseBuild()
        engine._origin = engine._fixed_build

        for layout in layouts:
            ft_cls = layout.feature_type
            schema = get_schema(ft_cls)

            _reject_ref(schema, ft_cls.__name__)

            # Reject unsupported variable-length types
            for fd in schema.fields:
                if fd.field_type in (OriginFieldType.bytes, OriginFieldType.wstr):
                    raise ValueError(
                        f'Table defined by feature "{ft_cls.__name__}" contains '
                        f'field "{fd.name}" of type "{fd.field_type.name}". '
                        f'Truncate operation does not support variable-length '
                        f'field types (wstr, bytes).'
                    )

            table_name = ft_cls.__name__
            engine._table_feature_types[table_name] = ft_cls
            existing = engine._table_map.get(table_name)
            if existing is not None:
                warnings.warn(
                    f'Table "{table_name}" already exists, truncate '
                    f'operation will overwrite it.',
                    UserWarning,
                )
            else:
                new_table = Table.map_from(
                    ft_cls,
                    _get_default_table_build(engine._fixed_build, table_name),
                    engine._fixed_build,
                )
                field_ids = {}
                for fd in schema.fields:
                    field_ids[fd.name] = len(field_ids)
                    if fd.field_type == OriginFieldType.list:
                        new_table._origin.add_list_field(fd.name, fd.cpp_type)
                    else:
                        new_table._origin.add_field(fd.name, fd.field_type.value)
                engine._table_map[table_name] = new_table
                engine._fixed_layer_builds[table_name] = new_table._origin
                engine._fixed_table_fields[table_name] = field_ids

            engine._fixed_build.truncate(table_name, layout.capacity)

        engine._publish_fixed_snapshot()
        return engine

    # ------------------------------------------------------------------
    # Push
    # ------------------------------------------------------------------

    def push(self, feature, table_name: str = '') -> None:
        """Push a single @feature instance to the database."""
        cls = feature.__class__
        # Fast path: batchable buffer
        buf = self._push_buf.get(cls)
        if buf is not None:
            buf.append(feature.__dict__)
            return
        # Fast path: complex single dispatch
        single_fn = self._push_dispatch.get(cls)
        if single_fn is not None:
            single_fn(feature.__dict__)
            return

        if not self._is_mutable:
            if self._origin is None:
                warnings.warn('Database has not connected to fastdb.', UserWarning)
            else:
                warnings.warn('Database has fixed scale, push not supported.', UserWarning)
            return

        self._push_full(feature, table_name)

    def _push_full(self, feature, table_name: str = '') -> None:
        """Full push with schema lookup, table creation, and dispatch caching."""
        feature_type = feature.__class__
        if not is_feature(feature_type):
            raise TypeError(
                f'{feature_type.__name__} is not a @feature class. '
                f'Use @feature decorator.'
            )
        schema = get_schema(feature_type)
        _reject_ref(schema, feature_type.__name__)

        feat_table_name = table_name if table_name else feature_type.__name__
        t_obj: Table = self._table_map.get(feat_table_name)
        if t_obj is None:
            new_table = Table.map_from(
                feature_type,
                _get_default_table_build(self._origin, feat_table_name),
                self._origin,
            )
            for fd in schema.fields:
                if fd.field_type == OriginFieldType.list:
                    new_table._origin.add_list_field(fd.name, fd.cpp_type)
                else:
                    new_table._origin.add_field(fd.name, fd.field_type.value)
            self._table_map[feat_table_name] = new_table
            self._table_feature_types[feat_table_name] = feature_type
            t_obj = new_table

        # Ensure compiled push_fn exists
        if schema.push_fn is None:
            from .push_compiler import compile_push_fn
            schema.push_fn = compile_push_fn(
                schema.numeric_plan, schema.str_plan,
                schema.bytes_plan, schema.list_plan,
            )

        schema.push_fn(feature.__dict__, t_obj._origin)
        t_obj.feature_count += 1

        # Cache inlined dispatch for subsequent pushes
        single_fn = make_inlined_dispatch(
            schema.numeric_plan, schema.str_plan,
            schema.bytes_plan, schema.list_plan, t_obj,
            schema.pfd_num_names, schema.pfd_num_ids,
            schema.pfd_str_names, schema.pfd_str_ids,
        )
        batch_fn = make_batch_inlined_dispatch(
            schema.numeric_plan, schema.str_plan,
            schema.bytes_plan, schema.list_plan, t_obj,
            schema.pfd_num_names, schema.pfd_num_ids,
            schema.pfd_str_names, schema.pfd_str_ids,
        )
        if batch_fn is not None:
            self._push_buf[feature_type] = []
            self._push_batch_fn[feature_type] = batch_fn
        else:
            self._push_dispatch[feature_type] = single_fn

    def push_many(self, features: list, table_name: str = '') -> None:
        """Push multiple @feature instances of the same type efficiently."""
        if not self._is_mutable or not features:
            return
        feature_type = features[0].__class__
        if not is_feature(feature_type):
            raise TypeError(
                f'{feature_type.__name__} is not a @feature class. '
                f'Use @feature decorator.'
            )
        schema = get_schema(feature_type)
        _reject_ref(schema, feature_type.__name__)

        if schema.push_fn is None:
            from .push_compiler import compile_push_fn
            schema.push_fn = compile_push_fn(
                schema.numeric_plan, schema.str_plan,
                schema.bytes_plan, schema.list_plan,
            )

        push_fn = schema.push_fn
        feat_table_name = table_name if table_name else feature_type.__name__
        t_obj: Table = self._table_map.get(feat_table_name)
        if t_obj is None:
            new_table = Table.map_from(
                feature_type,
                _get_default_table_build(self._origin, feat_table_name),
                self._origin,
            )
            for fd in schema.fields:
                if fd.field_type == OriginFieldType.list:
                    new_table._origin.add_list_field(fd.name, fd.cpp_type)
                else:
                    new_table._origin.add_field(fd.name, fd.field_type.value)
            self._table_map[feat_table_name] = new_table
            self._table_feature_types[feat_table_name] = feature_type
            t_obj = new_table

        t_origin = t_obj._origin
        fc = t_obj.feature_count
        for feat in features:
            push_fn(feat.__dict__, t_origin)
            fc += 1
        t_obj.feature_count = fc

    def _flush_push_batches(self):
        """Flush all pending batch push buffers to C++."""
        for cls, buf in self._push_buf.items():
            if buf:
                self._push_batch_fn[cls](buf)
                buf.clear()

    # ------------------------------------------------------------------
    # Combine / lifecycle
    # ------------------------------------------------------------------

    def combine(self):
        """Combine memory from all tables into a single continuous block."""
        if self._origin is None:
            warnings.warn('Database is empty, cannot combine.', UserWarning)
            return
        if isinstance(self._origin, core.WxDatabase):
            warnings.warn('Database already combined.', UserWarning)
            return

        self._flush_push_batches()

        memory_stream = core.WxMemoryStream()
        self._origin.post(memory_stream)
        buffer = memory_stream.data().as_array(np.uint8).tobytes()
        self._origin = core.WxDatabase.load_xbuffer(buffer)
        self._origin._buffer = buffer

        if self._shm:
            self._shm.close()
            self._shm = None
        self._table_map = {}

    def table(self, feature_type: Type, name: str = None) -> Table:
        """Get a Table for the given @feature class."""
        if not is_feature(feature_type):
            raise TypeError(
                f'{feature_type.__name__} is not a @feature class. '
                f'Use @feature decorator.'
            )
        table_name = name or feature_type.__name__
        cached = self._table_map.get(table_name)
        if cached is not None:
            self._table_feature_types[table_name] = feature_type
            self._attach_fixed_fill_handler(cached, table_name)
            return cached
        db = self._origin
        fallback_names = list(self._table_feature_types)
        for i in range(db.get_layer_count()):
            layer = db.get_layer(i)
            layer_name = layer.name() or (fallback_names[i] if i < len(fallback_names) else '')
            if layer_name == table_name:
                tbl = Table.map_from(feature_type, layer, db)
                self._table_feature_types[table_name] = feature_type
                self._attach_fixed_fill_handler(tbl, table_name)
                self._table_map[table_name] = tbl
                return tbl
        raise KeyError(f'Table "{table_name}" not found')

    def _attach_fixed_fill_handler(self, table: Table, table_name: str) -> None:
        if table.fixed and self._fixed_build is not None and self._shm is None:
            table._fixed_fill_handler = (
                lambda writes: self._fill_fixed_table(table_name, writes)
            )
        else:
            table._fixed_fill_handler = None

    def _fill_fixed_table(self, table_name: str, writes: dict[str, object]) -> None:
        layer_build = self._fixed_layer_builds[table_name]
        field_ids = self._fixed_table_fields[table_name]
        # If a bulk setter raises, no new snapshot is published and readers
        # continue to observe self._origin. The writable truncate build is not
        # rolled back here, so layer_build may be left partially updated and a
        # later successful fill could publish that partial build-state.
        for field_name, payload in writes.items():
            field_index = field_ids[field_name]
            if isinstance(payload, tuple):
                offsets, data = payload
                layer_build.set_string_column_bulk(field_index, offsets, data)
            else:
                layer_build.set_numeric_column_bulk(field_index, payload)
        self._publish_fixed_snapshot()

    def _find_layer(self, table_name: str):
        fallback_names = list(self._table_feature_types)
        for i in range(self._origin.get_layer_count()):
            layer = self._origin.get_layer(i)
            layer_name = layer.name() or (fallback_names[i] if i < len(fallback_names) else '')
            if layer_name == table_name:
                return layer
        return None

    def _publish_fixed_snapshot(self) -> None:
        if self._fixed_build is None:
            raise RuntimeError('No writable fixed table build is available.')

        memory_stream = core.WxMemoryStream()
        self._fixed_build.post(memory_stream)
        buffer = memory_stream.data().as_array(np.uint8).tobytes()
        self._origin = core.WxDatabase.load_xbuffer(buffer)
        self._origin._buffer = buffer

        for table_name, table in list(self._table_map.items()):
            new_layer = self._find_layer(table_name)
            if new_layer is None:
                continue
            table._remap(new_layer, self._origin)
            self._attach_fixed_fill_handler(table, table_name)

    # ------------------------------------------------------------------
    # Persistence / sharing
    # ------------------------------------------------------------------

    @staticmethod
    def load(name: str, from_file: bool = False) -> 'ColumnEngine':
        """Load a ColumnEngine from file or shared memory."""
        engine = ColumnEngine()
        if from_file:
            path = Path(name)
            if path.exists():
                engine._origin = core.WxDatabase.load(str(path))
            else:
                raise FileNotFoundError(f"Database '{name}' not found in file system.")
        else:
            name = _normalize_shm_name(name)
            try:
                engine._shm = shared_memory.SharedMemory(name=name)
                engine._origin = core.WxDatabase.load_xbuffer(engine._shm.buf)
            except FileNotFoundError:
                raise FileNotFoundError(f"Database '{name}' not found in shared memory.")
        return engine

    def share(self, shm_name: str, close_after: bool = False):
        """Share the database in shared memory."""
        if self._origin is None:
            raise RuntimeError('Database is empty, cannot share.')
        if isinstance(self._origin, core.WxDatabaseBuild):
            self.combine()

        shm_name = _normalize_shm_name(shm_name)
        chunk = self._origin.buffer()
        self._shm = shared_memory.SharedMemory(create=True, size=chunk.size, name=shm_name)
        dest = np.ndarray(chunk.size, dtype=np.uint8, buffer=self._shm.buf)
        dest[:] = chunk.as_array(np.uint8)
        self._origin._buffer = None
        self._origin = core.WxDatabase.load_xbuffer(self._shm.buf)
        self._fixed_build = None
        self._fixed_layer_builds = {}
        self._fixed_table_fields = {}
        for table_name, table in self._table_map.items():
            self._attach_fixed_fill_handler(table, table_name)

        if close_after and platform.system() != 'Windows':
            self.close()

    def save(self, path: str):
        """Save the database to a file."""
        if self._origin is None:
            raise RuntimeError('Database is empty, cannot save.')
        if isinstance(self._origin, core.WxDatabaseBuild):
            self._origin.save(path)
        else:
            chunk: core.chunk_data_t = self._origin.buffer()
            with open(path, 'wb') as f:
                f.write(chunk.to_bytes())

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

    def __len__(self):
        if self._origin is None:
            return 0
        return self._origin.get_layer_count()
