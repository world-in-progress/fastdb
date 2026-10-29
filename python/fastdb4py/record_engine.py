# python/fastdb4py/record_engine.py
"""RecordEngine: AoS record storage with strided field access and no REF support."""
import platform
import warnings
import numpy as np
from pathlib import Path
from typing import List, TypeVar, Type
from multiprocessing import shared_memory
from weakref import ref

from . import core
from .registry import (
    get_schema,
    is_feature,
    LayerSchema,
    non_native_list_storage_diagnostics,
    raw_payload_storage_diagnostics,
)
from .layout import Layout
from .orm.table import Table, _TableState
from .type import OriginFieldType
from .view_owner import FdbViewOwner
from .push_compiler import (
    make_inlined_dispatch, make_batch_inlined_dispatch,
)
from .push import normalize_bool_cache
from .string_column import _StringSequencePayload

T = TypeVar('T')


def _load_shared_database(shm: shared_memory.SharedMemory):
    try:
        return core.WxDatabase.load_xbuffer(shm.buf, shm)
    except BaseException:
        shm.close()
        raise


def _post_build_to_bytearray(build: object) -> bytearray:
    size = build.byte_length()
    buffer = bytearray(size)
    written = build.post_into_buffer(buffer)
    if written != size:
        raise RuntimeError('FastDB native final backing write failed.')
    return buffer


def _normalize_shm_name(shm_name: str) -> str:
    if platform.system() != 'Windows':
        return shm_name
    if shm_name.startswith(('Local\\', 'Global\\')):
        return shm_name
    return f'Local\\{shm_name}'


def _get_default_table_build(
    db: core.WxDatabaseBuild,
    t_name: str,
    *,
    raw_payload: bool = False,
) -> core.WxLayerTableBuild:
    t = db.create_layer_begin(t_name)
    if raw_payload:
        t.set_geometry_type(core.gtAny, core.cfTx32, aabboxEnabled=False)
    else:
        t.set_geometry_type(core.gtPoint, core.cfTx32, aabboxEnabled=True)
    t.set_extent(-180, -90, 180, 90)
    return t


def _reject_ref(schema: LayerSchema, cls_name: str) -> None:
    """Raise TypeError if schema contains REF fields."""
    if schema.has_ref_fields:
        ref_names = [fd.name for fd in schema.ref_fields + schema.list_ref_fields]
        raise TypeError(
            f"RecordEngine does not support REF fields. "
            f"{cls_name} has REF fields: {ref_names}. "
            f"Use ObjectEngine for classes with references."
        )


def _reject_non_native_lists(schema: LayerSchema, cls_name: str) -> None:
    diagnostics = non_native_list_storage_diagnostics(schema)
    if diagnostics:
        raise TypeError(
            f"RecordEngine cannot store non-native list fields for "
            f"{cls_name}: {'; '.join(diagnostics)}"
        )


def _reject_raw_payload_collisions(schema: LayerSchema, cls_name: str) -> None:
    diagnostics = raw_payload_storage_diagnostics(schema)
    if diagnostics:
        raise TypeError(
            f"RecordEngine cannot store raw payload fields for "
            f"{cls_name}: {'; '.join(diagnostics)}"
        )


def _reject_unsupported_schema(schema: LayerSchema, cls_name: str) -> None:
    _reject_ref(schema, cls_name)
    _reject_raw_payload_collisions(schema, cls_name)
    _reject_non_native_lists(schema, cls_name)


class _TableRegistration:
    """Weak projections of one backing state, retained until that state dies."""

    def __init__(self, engine, key, table: Table):
        engine_ref = ref(engine)

        def forget_state(state_ref):
            parent = engine_ref()
            if parent is not None:
                entry = parent._tables.get(key)
                if entry is not None and entry.state is state_ref:
                    del parent._tables[key]

        self.state = ref(table._state, forget_state)
        self.table = ref(table)


class RecordEngine:
    def __init__(self):
        self._shm: shared_memory.SharedMemory | None = None
        self._tables: dict[tuple[str, int], _TableRegistration] = {}
        self._build_states: dict[str, _TableState] = {}
        self._table_feature_types: dict[str, Type] = {}
        self._origin: core.WxDatabase | core.WxDatabaseBuild | None = None
        self._is_mutable: bool = False
        self._fixed_build: core.WxDatabaseBuild | None = None
        self._fixed_layer_builds: dict[str, core.WxLayerTableBuild] = {}
        self._fixed_table_fields: dict[str, dict[str, int]] = {}
        self._fixed_direct_fill: bool = False
        # Per-class push dispatch caches
        self._push_buf: dict = {}
        self._push_batch_fn: dict = {}
        self._push_dispatch: dict = {}
        self._bool_push_schemas: dict = {}
        self._closed = False

    def _assert_open(self) -> None:
        if self._closed:
            raise RuntimeError('RecordEngine is closed.')

    @property
    def fixed(self) -> bool:
        return isinstance(self._origin, core.WxDatabase)

    # ------------------------------------------------------------------
    # Factory methods
    # ------------------------------------------------------------------

    @staticmethod
    def create() -> 'RecordEngine':
        engine = RecordEngine()
        engine._origin = core.WxDatabaseBuild()
        engine._is_mutable = True
        return engine

    @staticmethod
    def _prepare_truncate(
        layouts: List[Layout],
        *,
        materialize_table_buffer: bool = True,
    ) -> 'RecordEngine':
        engine = RecordEngine()
        engine._fixed_build = core.WxDatabaseBuild()
        engine._origin = engine._fixed_build

        for layout in layouts:
            ft_cls = layout.feature_type
            schema = get_schema(ft_cls)

            _reject_unsupported_schema(schema, ft_cls.__name__)

            # Reject unsupported variable-length types
            for fd in schema.fields:
                if fd.field_type in (OriginFieldType.bytes, OriginFieldType.wstr):
                    raise ValueError(
                        f'Table defined by feature "{ft_cls.__name__}" contains '
                        f'field "{fd.name}" of type "{fd.field_type.name}". '
                        f'Truncate operation does not support variable-length '
                        f'field types (wstr, bytes).'
                    )

            table_name = layout.name or ft_cls.__name__
            engine._table_feature_types[table_name] = ft_cls
            existing = engine._fixed_layer_builds.get(table_name)
            if existing is not None:
                warnings.warn(
                    f'Table "{table_name}" already exists, truncate '
                    f'operation will overwrite it.',
                    UserWarning,
                )
            else:
                layer = _get_default_table_build(engine._fixed_build, table_name)
                field_ids = {}
                for fd in schema.fields:
                    field_ids[fd.name] = len(field_ids)
                    if fd.field_type == OriginFieldType.list:
                        layer.add_list_field(fd.name, fd.cpp_type)
                    else:
                        layer.add_field(fd.name, fd.field_type.value)
                if not materialize_table_buffer:
                    layer._set_table_buffer_materialized(False)
                engine._fixed_layer_builds[table_name] = layer
                engine._fixed_table_fields[table_name] = field_ids

            engine._fixed_build.truncate(table_name, layout.capacity)

        return engine

    @staticmethod
    def truncate(layouts: List[Layout]) -> 'RecordEngine':
        """Create a RecordEngine with fixed-size pre-allocated tables.

        Supports UTF-8 ``STR`` columns. ``WSTR`` and ``BYTES`` remain unsupported.
        Rejects any class with REF fields.
        """
        engine = RecordEngine._prepare_truncate(layouts)
        engine._publish_fixed_snapshot()
        return engine

    # ------------------------------------------------------------------
    # Push
    # ------------------------------------------------------------------

    def push(self, feature, table_name: str = '') -> None:
        """Push a single @feature instance to the database."""
        self._assert_open()
        cls = feature.__class__
        # Fast path: batchable buffer
        buf = self._push_buf.get(cls)
        if buf is not None:
            schema = self._bool_push_schemas.get(cls)
            cache = normalize_bool_cache(feature.__dict__, schema) if schema is not None else feature.__dict__
            buf.append(cache)
            return
        # Fast path: complex single dispatch
        single_fn = self._push_dispatch.get(cls)
        if single_fn is not None:
            schema = self._bool_push_schemas.get(cls)
            cache = normalize_bool_cache(feature.__dict__, schema) if schema is not None else feature.__dict__
            single_fn(cache)
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
        _reject_unsupported_schema(schema, feature_type.__name__)
        if schema.bool_field_names or schema.bool_list_field_names:
            self._bool_push_schemas[feature_type] = schema
        cache = normalize_bool_cache(feature.__dict__, schema)

        feat_table_name = table_name if table_name else feature_type.__name__
        t_obj = self._get_build_state(feature_type, feat_table_name, schema)

        # Ensure compiled push_fn exists
        if schema.push_fn is None:
            from .push_compiler import compile_push_fn
            schema.push_fn = compile_push_fn(
                schema.numeric_plan, schema.str_plan,
                schema.bytes_plan, schema.list_plan,
            )

        schema.push_fn(cache, t_obj._origin)
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
        self._assert_open()
        if not self._is_mutable or not features:
            return
        feature_type = features[0].__class__
        if not is_feature(feature_type):
            raise TypeError(
                f'{feature_type.__name__} is not a @feature class. '
                f'Use @feature decorator.'
            )
        schema = get_schema(feature_type)
        _reject_unsupported_schema(schema, feature_type.__name__)

        if schema.push_fn is None:
            from .push_compiler import compile_push_fn
            schema.push_fn = compile_push_fn(
                schema.numeric_plan, schema.str_plan,
                schema.bytes_plan, schema.list_plan,
            )

        push_fn = schema.push_fn
        bool_caches = None
        if schema.bool_field_names or schema.bool_list_field_names:
            bool_caches = [
                normalize_bool_cache(feat.__dict__, schema)
                for feat in features
            ]
        feat_table_name = table_name if table_name else feature_type.__name__
        t_obj = self._get_build_state(feature_type, feat_table_name, schema)

        t_origin = t_obj._origin
        fc = t_obj.feature_count
        if bool_caches is not None:
            for cache in bool_caches:
                push_fn(cache, t_origin)
                fc += 1
        else:
            for feat in features:
                push_fn(feat.__dict__, t_origin)
                fc += 1
        t_obj.feature_count = fc

    def _get_build_state(self, feature_type: Type, name: str, schema: LayerSchema) -> _TableState:
        state = self._build_states.get(name)
        if state is None:
            state = _TableState()
            state._db = self._origin
            state._origin = _get_default_table_build(self._origin, name, raw_payload=bool(schema.bytes_plan))
            state._feature_type = feature_type
            for fd in schema.fields:
                if fd.field_type == OriginFieldType.list:
                    state._origin.add_list_field(fd.name, fd.cpp_type)
                else:
                    state._origin.add_field(fd.name, fd.field_type.value)
            self._build_states[name] = state
            self._table_feature_types[name] = feature_type
        return state

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
        self._assert_open()
        if self._origin is None:
            warnings.warn('Database is empty, cannot combine.', UserWarning)
            return
        if isinstance(self._origin, core.WxDatabase):
            warnings.warn('Database already combined.', UserWarning)
            return

        self._flush_push_batches()

        buffer = _post_build_to_bytearray(self._origin)
        self._origin = core.WxDatabase.load_xbuffer(buffer)

        self._shm = None
        self._tables.clear()
        self._build_states.clear()
        self._is_mutable = False
        self._push_buf.clear()
        self._push_batch_fn.clear()
        self._push_dispatch.clear()
        self._bool_push_schemas.clear()

    def table(
        self,
        feature_type: Type,
        name: str = None,
        *,
        owner: FdbViewOwner | None = None,
        writeable: bool | None = None,
    ) -> Table:
        """Get a Table for the given @feature class."""
        self._assert_open()
        if not is_feature(feature_type):
            raise TypeError(
                f'{feature_type.__name__} is not a @feature class. '
                f'Use @feature decorator.'
            )
        table_name = name or feature_type.__name__
        alias = owner is not None or writeable is not None
        if not alias:
            entry = self._tables.get((table_name, 0))
            if entry is not None:
                state = entry.state()
                if state is not None:
                    cached = entry.table()
                    if cached is None:
                        cached = Table._from_state(state)
                        entry.table = ref(cached)
                    return cached
        db = self._origin
        if isinstance(db, core.WxDatabaseBuild):
            state = self._build_states.get(table_name)
            if state is None:
                raise KeyError(f'Table "{table_name}" not found')
            tbl = Table.map_from(feature_type, state._origin, db, owner=owner, writeable=writeable)
            tbl._state._fc = state._fc
            self._register_table(tbl, table_name, alias)
            return tbl
        fallback_names = list(self._table_feature_types)
        for i in range(db.get_layer_count()):
            layer = db.get_layer(i)
            layer_name = layer.name() or (fallback_names[i] if i < len(fallback_names) else '')
            if layer_name == table_name:
                tbl = Table.map_from(
                    feature_type,
                    layer,
                    db,
                    owner=owner,
                    writeable=writeable,
                )
                self._table_feature_types[table_name] = feature_type
                self._register_table(tbl, table_name, alias)
                return tbl
        raise KeyError(f'Table "{table_name}" not found')

    def _register_table(self, table: Table, name: str, alias: bool) -> None:
        key = (name, id(table._state) if alias else 0)
        self._tables[key] = _TableRegistration(self, key, table)
        table._state._engine = self
        self._attach_fixed_fill_handler(table._state, name, alias)

    def _attach_fixed_fill_handler(self, state: _TableState, table_name: str, alias: bool = False) -> None:
        state._table_name = table_name
        fixed = isinstance(state._origin, core.WxLayerTable)
        if fixed and self._fixed_direct_fill:
            state._fixed_fill_handler = self._fill_mapped_table_in_place
        elif fixed and self._fixed_build is not None and self._shm is None:
            state._fixed_fill_handler = self._fill_mapped_table_and_drop_fixed_build if alias else self._fill_fixed_table
        else:
            state._fixed_fill_handler = None

    def _invalidate_fixed_writes(self) -> None:
        self._fixed_build = None
        self._fixed_layer_builds = {}
        self._fixed_table_fields = {}
        self._fixed_direct_fill = False
        for (table_name, alias_id), entry in list(self._tables.items()):
            state = entry.state()
            if state is not None:
                self._attach_fixed_fill_handler(state, table_name, bool(alias_id))

    def _fill_fixed_table(self, state: _TableState, writes: dict[str, object]) -> None:
        table_name = state._table_name
        layer_build = self._fixed_layer_builds[table_name]
        field_ids = self._fixed_table_fields[table_name]
        try:
            for field_name, payload in writes.items():
                field_index = field_ids[field_name]
                if isinstance(payload, _StringSequencePayload):
                    layer_build.set_string_column_from_sequence(field_index, payload.values)
                elif isinstance(payload, tuple):
                    offsets, data = payload
                    layer_build.set_string_column_bulk(field_index, offsets, data)
                else:
                    layer_build.set_numeric_column_bulk(field_index, payload)
        except Exception:
            # A setter may have already mutated the retained truncate build.
            # Invalidate future writes so a later fill() cannot publish a mixed
            # partial state from this failed batch.
            self._invalidate_fixed_writes()
            raise
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

        buffer = _post_build_to_bytearray(self._fixed_build)
        self._publish_fixed_buffer(buffer, direct_fill=False)

    def _publish_fixed_snapshot_to_buffer(self, buffer: object, *, direct_fill: bool = False) -> None:
        if self._fixed_build is None:
            raise RuntimeError('No writable fixed table build is available.')
        writable = memoryview(buffer).cast('B')
        if writable.readonly:
            raise TypeError('fixed snapshot destination buffer must be writable.')
        size = self._fixed_build.byte_length()
        if writable.nbytes < size:
            raise ValueError(
                f'fixed snapshot destination is too small: expected at least {size} bytes, got {writable.nbytes}.',
            )
        written = self._fixed_build.post_into_buffer(writable)
        if written != size:
            raise RuntimeError('FastDB native final backing write failed.')
        self._publish_fixed_buffer(writable[:size], direct_fill=direct_fill)

    def _publish_fixed_buffer(self, buffer: object, *, direct_fill: bool) -> None:
        previous_db = self._origin
        self._fixed_direct_fill = direct_fill
        self._origin = core.WxDatabase.load_xbuffer(buffer)

        remaps = []
        for (table_name, alias_id), entry in list(self._tables.items()):
            state = entry.state()
            if state is None or state._db is not previous_db:
                continue
            new_layer = self._find_layer(table_name)
            if new_layer is None:
                continue
            remaps.append((state, entry.table(), new_layer, table_name, bool(alias_id)))
        for owner in {state._fdb_owner for state, *_ in remaps}:
            owner.bump_generation()
        for state, table, layer, table_name, alias in remaps:
            if table is None:
                state._remap(layer, self._origin)
            else:
                table._remap(layer, self._origin)
            self._attach_fixed_fill_handler(state, table_name, alias)

    def _fill_mapped_table_in_place(self, state: _TableState, writes: dict[str, object]) -> None:
        schema = get_schema(state._feature_type)
        for field_name, payload in writes.items():
            if isinstance(payload, _StringSequencePayload) or isinstance(payload, tuple):
                raise RuntimeError('direct fixed backing writes do not support variable-size string payloads.')
            column = state._origin.get_column(schema.field_index_map[field_name]).as_nparray()
            column[:] = payload

    def _fill_mapped_table_and_drop_fixed_build(self, state: _TableState, writes: dict[str, object]) -> None:
        self._fill_mapped_table_in_place(state, writes)
        self._invalidate_fixed_writes()

    # ------------------------------------------------------------------
    # Persistence / sharing
    # ------------------------------------------------------------------

    @staticmethod
    def load(name: str, from_file: bool = False) -> 'RecordEngine':
        """Load a RecordEngine from file or shared memory."""
        engine = RecordEngine()
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
                engine._origin = _load_shared_database(engine._shm)
            except FileNotFoundError:
                raise FileNotFoundError(f"Database '{name}' not found in shared memory.")
        return engine

    def share(self, shm_name: str, close_after: bool = False):
        """Share the database in shared memory."""
        self._assert_open()
        if self._origin is None:
            raise RuntimeError('Database is empty, cannot share.')
        if isinstance(self._origin, core.WxDatabaseBuild):
            self.combine()

        shm_name = _normalize_shm_name(shm_name)
        chunk = self._origin.buffer()
        self._shm = shared_memory.SharedMemory(create=True, size=chunk.size, name=shm_name)
        chunk.copy_to_buffer(self._shm.buf)
        self._origin = _load_shared_database(self._shm)
        self._invalidate_fixed_writes()
        self._tables.clear()

        if close_after and platform.system() != 'Windows':
            self.close()

    def save(self, path: str):
        """Save the database to a file."""
        self._assert_open()
        if self._origin is None:
            raise RuntimeError('Database is empty, cannot save.')
        if isinstance(self._origin, core.WxDatabaseBuild):
            self._origin.save(path)
        else:
            chunk: core.chunk_data_t = self._origin.buffer()
            with open(path, 'wb') as f:
                f.write(chunk.to_bytes())

    def close(self):
        """Release engine references; existing views retain their native backing."""
        self._closed = True
        self._tables.clear()
        self._build_states.clear()
        self._table_feature_types.clear()
        self._origin = None
        self._shm = None
        self._fixed_build = None
        self._fixed_layer_builds.clear()
        self._fixed_table_fields.clear()
        self._fixed_direct_fill = False
        self._is_mutable = False
        self._push_buf.clear()
        self._push_batch_fn.clear()
        self._push_dispatch.clear()
        self._bool_push_schemas.clear()

    def unlink(self):
        """Unlink the shared memory database."""
        if self._shm:
            self._shm.unlink()
        self.close()

    def __len__(self):
        if self._origin is None:
            return 0
        return self._origin.get_layer_count()
