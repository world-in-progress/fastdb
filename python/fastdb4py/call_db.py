from __future__ import annotations

from collections.abc import Iterable, Mapping
from dataclasses import dataclass
from typing import Any

import numpy as np

from . import core
from .column_engine import ColumnEngine, _get_default_table_build
from .decorator import feature
from .materialize import materialize
from .object_engine import LayerState, ObjectEngine
from .orm.table import Table, _FILL_NUMERIC_DTYPES, _normalize_bool_fill_values
from .registry import (
    LayerSchema,
    get_schema,
    is_feature,
    lookup_class,
    non_native_list_storage_diagnostics,
    raw_payload_storage_diagnostics,
)
from .schema import (
    columnar_capability,
    export_schema,
    feature_schema_dependencies,
    object_graph_capability,
    schema_sha256 as fastdb_schema_sha256,
)
from .string_column import StringColumn, _StringSequencePayload
from .type import (
    BOOL,
    BYTES,
    F64,
    I32,
    STR,
    WSTR,
    OriginFieldType,
    coerce_bool_scalar,
    get_origin_type,
)
from .view_owner import FdbViewOwner

CALL_DB_SCHEMA_VERSION = 'fastdb.call-db.schema.v1'
CALL_DB_CODEC_ID = 'org.fastdb.call-db'
CALL_DB_COLUMNAR_PROFILE = 'fastdb.call.columnar.v1'
CALL_DB_OBJECT_GRAPH_PROFILE = 'fastdb.call.object-graph.v1'
CALL_DB_ARRAY_VALUE_FIELD = 'value'

_VALID_CALL_DB_PROFILES = {
    CALL_DB_COLUMNAR_PROFILE,
    CALL_DB_OBJECT_GRAPH_PROFILE,
}
_SCALAR_KIND_BY_FIELD_TYPE = {
    OriginFieldType.u8: 'u8',
    OriginFieldType.u16: 'u16',
    OriginFieldType.u32: 'u32',
    OriginFieldType.i32: 'i32',
    OriginFieldType.u8n: 'u8n',
    OriginFieldType.u16n: 'u16n',
    OriginFieldType.f32: 'f32',
    OriginFieldType.f64: 'f64',
    OriginFieldType.str: 'str',
}
_FIELD_SCALAR_KIND_BY_FIELD_TYPE = {
    **_SCALAR_KIND_BY_FIELD_TYPE,
    OriginFieldType.wstr: 'wstr',
    OriginFieldType.bytes: 'bytes',
}
_VALID_CALL_DB_SCALAR_KINDS = {
    'bool',
    'bytes',
    'wstr',
    *_SCALAR_KIND_BY_FIELD_TYPE.values(),
}
_MISSING = object()


@dataclass(frozen=True)
class FastdbCallDbScalarField:
    name: str
    kind: str
    value_position: int
    parameter: str | None = None
    return_index: int | None = None


@dataclass(frozen=True)
class FastdbCallDbArrayItem:
    name: str
    kind: str


@dataclass(frozen=True)
class FastdbCallDbFeatureDependency:
    feature_schema_sha256: str
    feature: type | None = None


@dataclass(frozen=True)
class FastdbCallDbTable:
    name: str
    kind: str
    cardinality: str
    feature: type | None = None
    feature_schema_sha256: str | None = None
    feature_schema_dependencies: tuple[FastdbCallDbFeatureDependency, ...] = ()
    parameter: str | None = None
    return_index: int | None = None
    value_position: int | None = None
    fields: tuple[FastdbCallDbScalarField, ...] = ()
    item: FastdbCallDbArrayItem | None = None


@dataclass(frozen=True)
class FastdbCallDbBinding:
    codec_id: str
    profile: str
    schema_sha256: str
    method: str
    direction: str
    tables: tuple[FastdbCallDbTable, ...]


def encode_call_db(binding: FastdbCallDbBinding | Mapping[str, Any] | object, value: object) -> bytes:
    """Encode a generic FastDB call-db payload from a binding and logical value."""
    normalized = _normalize_binding(binding)
    if normalized.profile == CALL_DB_OBJECT_GRAPH_PROFILE:
        _ensure_object_graph_call_db(normalized)
        values = _normalize_call_values(normalized, value)
        return _encode_object_graph_call_db(normalized, values)
    _ensure_columnar_call_db(normalized)
    values = _normalize_call_values(normalized, value)
    exported = _try_export_columnar_call_db(normalized, values)
    if exported is not None:
        return exported.tobytes()
    engine = ColumnEngine.create()
    for table in normalized.tables:
        _encode_call_table(engine, table, values)
    engine.combine()
    chunk = engine._origin.buffer()  # noqa: SLF001
    return chunk.to_bytes()


def try_export_call_db(
    binding: FastdbCallDbBinding | Mapping[str, Any] | object,
    value: object,
) -> memoryview | None:
    """Return an existing call-db-compatible buffer when no repack is needed.

    The first supported exact-match shape is a single fixed ``Batch[Feature]``
    value whose backing database contains exactly the named call-db table. Other
    shapes return ``None`` so callers can fall back to ``encode_call_db(...)``.
    """
    normalized = _normalize_binding(binding)
    if normalized.profile != CALL_DB_COLUMNAR_PROFILE:
        _validate_binding(normalized)
        return None
    _ensure_columnar_call_db(normalized)
    values = _normalize_call_values(normalized, value)
    return _try_export_columnar_call_db(normalized, values)


def decode_call_db(
    binding: FastdbCallDbBinding | Mapping[str, Any] | object,
    payload: bytes | bytearray | memoryview,
) -> object:
    """Decode a generic FastDB call-db payload into materialized Python values."""
    normalized = _normalize_binding(binding)
    if normalized.profile == CALL_DB_OBJECT_GRAPH_PROFILE:
        _ensure_object_graph_call_db(normalized)
        engine = _object_engine_from_buffer(payload, normalized.tables)
        return _materialize_values_object_graph(normalized, engine)
    _ensure_columnar_call_db(normalized)
    engine = _column_engine_from_buffer(payload)
    return _materialize_values(normalized, engine)


def view_call_db(
    binding: FastdbCallDbBinding | Mapping[str, Any] | object,
    payload: bytes | bytearray | memoryview,
    *,
    owner: FdbViewOwner | None = None,
) -> 'FastdbCallDbView':
    """Create an owner-bound retained view for a columnar FastDB call-db payload."""
    normalized = _normalize_binding(binding)
    if normalized.profile != CALL_DB_COLUMNAR_PROFILE:
        raise ValueError(f'{normalized.profile} does not support retained buffer views.')
    _ensure_columnar_call_db(normalized)
    if owner is None:
        owner = FdbViewOwner(checked=True, writeable=False)
    buffer = payload if isinstance(payload, memoryview) else memoryview(payload)
    return FastdbCallDbView(
        binding=normalized,
        engine=_column_engine_from_buffer(buffer),
        buffer=buffer,
        owner=owner,
    )


@dataclass(frozen=True)
class FastdbCallDbView:
    binding: FastdbCallDbBinding
    engine: ColumnEngine
    buffer: memoryview
    owner: FdbViewOwner

    @property
    def _fdb_owner(self) -> FdbViewOwner:
        return self.owner

    def logical_values(self) -> tuple[object, ...]:
        self._ensure_alive()
        values: list[Any] = [None] * _call_value_count(self.binding)
        for table in self.binding.tables:
            if table.kind == 'scalars':
                for field in table.fields:
                    values[field.value_position] = self.scalar(field.name)
                continue
            if table.value_position is None:
                raise ValueError(f'fastdb call-db table {table.name!r} is missing value_position.')
            if table.kind == 'feature':
                if table.cardinality == 'many':
                    values[table.value_position] = self.table(table.name)
                else:
                    values[table.value_position] = self.feature(table.name)
                continue
            if table.kind == 'array':
                values[table.value_position] = self.array(table.name)
                continue
            raise ValueError(f'Unsupported fastdb call-db table kind {table.kind!r}.')
        return tuple(values)

    def logical_value(self) -> object:
        values = self.logical_values()
        if self.binding.direction == 'input':
            return values
        if len(values) == 1:
            return values[0]
        return values

    def materialize(self) -> object:
        self._ensure_alive()
        return _materialize_values(self.binding, self.engine)

    def to_owned(self) -> object:
        return self.materialize()

    def table(self, name_or_index: str | int) -> Table:
        table = self._resolve_table('feature', name_or_index)
        return self._table_for(table)

    def feature(self, name_or_index: str | int) -> Any:
        table = self._resolve_table('feature', name_or_index)
        if table.cardinality != 'one':
            raise TypeError(
                f'fastdb call-db feature view {table.name!r} has cardinality '
                f'{table.cardinality!r}; use table(...) for batch outputs.',
            )
        rows = self._table_for(table)
        if len(rows) < 1:
            raise IndexError(f'fastdb call-db feature table {table.name!r} is empty.')
        return rows[0]

    def array(self, name_or_index: str | int) -> 'FastdbCallDbArrayView':
        return FastdbCallDbArrayView(self, self._resolve_table('array', name_or_index))

    def scalar(self, name_or_index: str | int) -> object:
        table, field = self._resolve_scalar_field(name_or_index)
        rows = self._table_for(table)
        if len(rows) < 1:
            raise IndexError(f'fastdb call-db scalar table {table.name!r} is empty.')
        return _materialize_scalar_value(field.kind, getattr(rows[0], field.name))

    @property
    def column(self) -> Any:
        value = self._single_logical_value()
        column = getattr(value, 'column', None)
        if column is None:
            raise TypeError('single fastdb call-db value does not expose columns.')
        return column

    def __len__(self) -> int:
        return len(self._single_logical_value())  # type: ignore[arg-type]

    def __getitem__(self, index: int) -> Any:
        return self._single_logical_value()[index]  # type: ignore[index]

    def __iter__(self):
        yield from self._single_logical_value()  # type: ignore[misc]

    def _single_logical_value(self) -> object:
        values = self.logical_values()
        if len(values) != 1:
            raise TypeError('direct view access is available only for single-value call-db payloads.')
        return values[0]

    def _table_for(self, table: FastdbCallDbTable) -> Table:
        self._ensure_alive()
        if table.feature is None:
            raise ValueError(f'fastdb call-db table {table.name!r} is missing runtime feature.')
        return self.engine.table(
            table.feature,
            name=table.name,
            owner=self.owner,
            writeable=self.owner.writeable,
        )

    def _resolve_table(self, kind: str, name_or_index: str | int) -> FastdbCallDbTable:
        self._ensure_alive()
        tables = [table for table in self.binding.tables if table.kind == kind]
        if isinstance(name_or_index, int):
            return tables[name_or_index]
        for table in tables:
            if table.name == name_or_index:
                return table
        raise KeyError(f'fastdb call-db {kind} table {name_or_index!r} not found')

    def _resolve_scalar_field(self, name_or_index: str | int) -> tuple[FastdbCallDbTable, FastdbCallDbScalarField]:
        self._ensure_alive()
        scalar_fields = [
            (table, field)
            for table in self.binding.tables
            if table.kind == 'scalars'
            for field in table.fields
        ]
        if isinstance(name_or_index, int):
            return scalar_fields[name_or_index]
        for table, field in scalar_fields:
            if field.name == name_or_index:
                return table, field
        raise KeyError(f'fastdb call-db scalar field {name_or_index!r} not found')

    def _ensure_alive(self) -> None:
        self.owner.assert_alive()
        try:
            _ = self.buffer.nbytes
        except ValueError as exc:
            raise RuntimeError('FastDB call-db buffer has been released.') from exc


@dataclass(frozen=True)
class FastdbCallDbArrayView:
    call_view: FastdbCallDbView
    spec: FastdbCallDbTable

    @property
    def _fdb_owner(self) -> FdbViewOwner:
        return self.call_view.owner

    def materialize(self) -> list[Any]:
        return self.to_owned()

    def to_owned(self) -> list[Any]:
        item_kind = _array_item_kind(self.spec)
        return [
            _materialize_scalar_value(item_kind, getattr(row, CALL_DB_ARRAY_VALUE_FIELD))
            for row in self._table()
        ]

    def __len__(self) -> int:
        return len(self._table())

    def __getitem__(self, index: int | slice) -> Any:
        if isinstance(index, slice):
            return [self[item_index] for item_index in range(*index.indices(len(self)))]
        return _materialize_scalar_value(
            _array_item_kind(self.spec),
            getattr(self._table()[index], CALL_DB_ARRAY_VALUE_FIELD),
        )

    def __iter__(self):
        table = self._table()
        item_kind = _array_item_kind(self.spec)
        for index in range(len(table)):
            self.call_view._ensure_alive()
            yield _materialize_scalar_value(
                item_kind,
                getattr(table[index], CALL_DB_ARRAY_VALUE_FIELD),
            )

    def __eq__(self, other: object) -> bool:
        if isinstance(other, FastdbCallDbArrayView):
            return self.to_owned() == other.to_owned()
        if isinstance(other, (list, tuple)):
            return self.to_owned() == list(other)
        return NotImplemented

    def _table(self) -> Table:
        if self.spec.feature is None:
            raise ValueError(f'fastdb call-db array table {self.spec.name!r} is missing runtime feature.')
        self.call_view._ensure_alive()
        return self.call_view.engine.table(
            self.spec.feature,
            name=self.spec.name,
            owner=self.call_view.owner,
            writeable=self.call_view.owner.writeable,
        )


def _normalize_binding(binding: FastdbCallDbBinding | Mapping[str, Any] | object) -> FastdbCallDbBinding:
    if isinstance(binding, FastdbCallDbBinding):
        tables = tuple(_normalize_table(table) for table in binding.tables)
        normalized = FastdbCallDbBinding(
            codec_id=binding.codec_id,
            direction=binding.direction,
            method=binding.method,
            profile=binding.profile,
            schema_sha256=binding.schema_sha256,
            tables=tables,
        )
    else:
        normalized = FastdbCallDbBinding(
            codec_id=_require_attr(binding, 'codec_id', 'codecId'),
            direction=_require_attr(binding, 'direction'),
            method=_require_attr(binding, 'method'),
            profile=_require_attr(binding, 'profile'),
            schema_sha256=_require_attr(binding, 'schema_sha256', 'schemaSha256'),
            tables=tuple(_normalize_table(table) for table in _require_attr(binding, 'tables')),
        )
    _validate_binding(normalized)
    return normalized


def _normalize_table(table: FastdbCallDbTable | Mapping[str, Any] | object) -> FastdbCallDbTable:
    if isinstance(table, FastdbCallDbTable):
        return FastdbCallDbTable(
            cardinality=table.cardinality,
            feature=table.feature,
            feature_schema_dependencies=tuple(
                _normalize_feature_dependency(item)
                for item in table.feature_schema_dependencies
            ),
            feature_schema_sha256=table.feature_schema_sha256,
            fields=tuple(_normalize_scalar_field(field) for field in table.fields),
            item=_normalize_array_item(table.item) if table.item is not None else None,
            kind=table.kind,
            name=table.name,
            parameter=table.parameter,
            return_index=table.return_index,
            value_position=table.value_position,
        )
    return FastdbCallDbTable(
        cardinality=_require_attr(table, 'cardinality'),
        feature=_optional_attr(table, 'feature', 'feature_type'),
        feature_schema_dependencies=tuple(
            _normalize_feature_dependency(item)
            for item in _optional_attr(
                table,
                'feature_schema_dependencies',
                'featureDependencies',
                default=(),
            )
        ),
        feature_schema_sha256=_optional_attr(table, 'feature_schema_sha256', 'featureSchemaSha256'),
        fields=tuple(
            _normalize_scalar_field(field)
            for field in _optional_attr(table, 'fields', 'scalar_fields', default=())
        ),
        item=(
            _normalize_array_item(_optional_attr(table, 'item', 'array_item'))
            if _optional_attr(table, 'item', 'array_item') is not None
            else None
        ),
        kind=_require_attr(table, 'kind'),
        name=_require_attr(table, 'name'),
        parameter=_optional_attr(table, 'parameter'),
        return_index=_optional_attr(table, 'return_index', 'returnIndex'),
        value_position=_optional_attr(table, 'value_position', 'valuePosition'),
    )


def _normalize_scalar_field(field: FastdbCallDbScalarField | Mapping[str, Any] | object) -> FastdbCallDbScalarField:
    if isinstance(field, FastdbCallDbScalarField):
        return field
    return FastdbCallDbScalarField(
        kind=_require_attr(field, 'kind'),
        name=_require_attr(field, 'name'),
        parameter=_optional_attr(field, 'parameter'),
        return_index=_optional_attr(field, 'return_index', 'returnIndex'),
        value_position=_require_attr(field, 'value_position', 'valuePosition'),
    )


def _normalize_array_item(item: FastdbCallDbArrayItem | Mapping[str, Any] | object) -> FastdbCallDbArrayItem:
    if isinstance(item, FastdbCallDbArrayItem):
        return item
    return FastdbCallDbArrayItem(
        kind=_require_attr(item, 'kind'),
        name=_require_attr(item, 'name'),
    )


def _normalize_feature_dependency(
    item: FastdbCallDbFeatureDependency | Mapping[str, Any] | object,
) -> FastdbCallDbFeatureDependency:
    if isinstance(item, FastdbCallDbFeatureDependency):
        return item
    return FastdbCallDbFeatureDependency(
        feature=_optional_attr(item, 'feature'),
        feature_schema_sha256=_require_attr(item, 'feature_schema_sha256', 'featureSchemaSha256'),
    )


def _require_attr(source: object, *names: str) -> Any:
    value = _optional_attr(source, *names, default=_MISSING)
    if value is _MISSING:
        joined = ' or '.join(names)
        raise ValueError(f'fastdb call-db binding is missing {joined}.')
    return value


def _optional_attr(source: object, *names: str, default: Any = None) -> Any:
    if isinstance(source, Mapping):
        for name in names:
            if name in source:
                return source[name]
        return default
    for name in names:
        if hasattr(source, name):
            return getattr(source, name)
    return default


def _validate_binding(binding: FastdbCallDbBinding) -> None:
    if binding.codec_id != CALL_DB_CODEC_ID:
        raise ValueError(
            f'fastdb call-db runtime expected codec id {CALL_DB_CODEC_ID!r}, '
            f'got {binding.codec_id!r}.',
        )
    if binding.profile not in _VALID_CALL_DB_PROFILES:
        raise ValueError(f'Unsupported fastdb call-db profile {binding.profile!r}.')
    if not isinstance(binding.method, str) or not binding.method:
        raise ValueError('fastdb call-db binding must include a non-empty method name.')
    if binding.direction not in {'input', 'output'}:
        raise ValueError('fastdb call-db binding direction must be "input" or "output".')
    if not isinstance(binding.tables, tuple):
        raise ValueError('fastdb call-db binding tables must be a tuple.')
    _validate_table_names(binding.tables)
    _call_value_count(binding)


def _ensure_columnar_call_db(binding: FastdbCallDbBinding) -> None:
    _validate_binding(binding)
    if binding.profile != CALL_DB_COLUMNAR_PROFILE:
        raise ValueError(f'Unsupported fastdb call-db columnar profile {binding.profile!r}.')
    for table in binding.tables:
        if table.feature is None:
            raise ValueError(f'fastdb call-db table {table.name!r} is missing runtime feature.')
        capability = columnar_capability(table.feature)
        if not capability['eligible']:
            raise TypeError(
                f'fastdb call-db table {table.name!r} is not columnar eligible: '
                f'{capability["diagnostics"]}',
            )


def _ensure_object_graph_call_db(binding: FastdbCallDbBinding) -> None:
    _validate_binding(binding)
    if binding.profile != CALL_DB_OBJECT_GRAPH_PROFILE:
        raise ValueError(f'Unsupported fastdb call-db object-graph profile {binding.profile!r}.')
    seen_feature_types: dict[type, str] = {}
    seen_layer_names: dict[str, tuple[str, str]] = {}

    def claim_layer(layer_name: str, owner: str, kind: str) -> None:
        previous = seen_layer_names.get(layer_name)
        if previous is not None:
            previous_owner, previous_kind = previous
            if previous_kind == 'dependency' and kind == 'dependency' and previous_owner == owner:
                return
            raise TypeError(
                f'{CALL_DB_OBJECT_GRAPH_PROFILE} cannot encode both {previous_owner!r} '
                f'and {owner!r} as layer {layer_name!r}; use distinct feature wrapper '
                'types or table names.',
            )
        seen_layer_names[layer_name] = (owner, kind)

    for table in binding.tables:
        if table.feature is None:
            raise ValueError(f'fastdb call-db table {table.name!r} is missing runtime feature.')
        capability = object_graph_capability(table.feature)
        if not capability['eligible']:
            raise TypeError(
                f'fastdb call-db table {table.name!r} is not object-graph eligible: '
                f'{capability["diagnostics"]}',
            )
        previous = seen_feature_types.get(table.feature)
        if previous is not None:
            raise TypeError(
                f'{CALL_DB_OBJECT_GRAPH_PROFILE} cannot encode feature type '
                f'{table.feature.__name__} in both {previous!r} and {table.name!r}; '
                'use distinct wrapper feature types until named object-graph tables are supported.',
            )
        seen_feature_types[table.feature] = table.name
        layer_name = get_schema(table.feature).layer_name
        claim_layer(layer_name, f'table {table.name}', 'table')
        for dependency_layer_name, dependency_name in _feature_dependency_layer_names(table.feature):
            claim_layer(dependency_layer_name, f'dependency {dependency_name}', 'dependency')


def _encode_call_table(engine: ColumnEngine, table: FastdbCallDbTable, values: tuple[Any, ...]) -> None:
    if table.feature is None:
        raise ValueError(f'fastdb call-db table {table.name!r} is missing runtime feature.')
    if table.kind == 'scalars':
        scalar_values = {
            field.name: _coerce_scalar_value(field.kind, values[field.value_position])
            for field in table.fields
        }
        engine.push(table.feature(**scalar_values), table_name=table.name)
        return
    if table.value_position is None:
        raise ValueError(f'fastdb call-db table {table.name!r} is missing value_position.')
    value = values[table.value_position]
    if table.kind == 'array':
        items = _array_table_values(table, value)
        if items:
            engine.push_many([
                table.feature(value=item)
                for item in items
            ], table_name=table.name)
        else:
            _create_empty_feature_table(engine, table)
        return
    if table.kind == 'feature':
        if table.cardinality == 'many':
            if _try_encode_feature_table_bulk(engine, table, value):
                return
            rows = _feature_table_rows(table, value)
            if rows:
                engine.push_many(rows, table_name=table.name)
            else:
                _create_empty_feature_table(engine, table)
            return
        engine.push(_coerce_feature_row(table.feature, value), table_name=table.name)
        return
    raise ValueError(f'Unsupported fastdb call-db table kind {table.kind!r}.')


def _materialize_values(binding: FastdbCallDbBinding, engine: ColumnEngine) -> object:
    values: list[Any] = [None] * _call_value_count(binding)
    for table in binding.tables:
        if table.feature is None:
            raise ValueError(f'fastdb call-db table {table.name!r} is missing runtime feature.')
        if table.kind == 'scalars':
            row = engine.table(table.feature, name=table.name)[0]
            for field in table.fields:
                values[field.value_position] = _materialize_scalar_value(
                    field.kind,
                    getattr(row, field.name),
                )
            continue
        if table.value_position is None:
            raise ValueError(f'fastdb call-db table {table.name!r} is missing value_position.')
        fastdb_table = engine.table(table.feature, name=table.name)
        if table.kind == 'array':
            item_kind = _array_item_kind(table)
            values[table.value_position] = [
                _materialize_scalar_value(
                    item_kind,
                    getattr(row, CALL_DB_ARRAY_VALUE_FIELD),
                )
                for row in fastdb_table
            ]
            continue
        if table.kind == 'feature':
            if table.cardinality == 'many':
                values[table.value_position] = materialize(fastdb_table)
            else:
                values[table.value_position] = materialize(fastdb_table[0])
            continue
        raise ValueError(f'Unsupported fastdb call-db table kind {table.kind!r}.')
    return _logical_return(binding, values)


def _logical_return(binding: FastdbCallDbBinding, values: list[Any]) -> object:
    if binding.direction == 'input':
        return tuple(values)
    if len(values) == 1:
        return values[0]
    return tuple(values)


def _normalize_call_values(binding: FastdbCallDbBinding, values: object) -> tuple[Any, ...]:
    expected = _call_value_count(binding)
    if binding.direction == 'input':
        if not isinstance(values, tuple):
            raise TypeError('input fastdb call-db serialization expects a tuple of parameter values.')
        if len(values) != expected:
            raise ValueError(f'expected {expected} input values, got {len(values)}.')
        return values
    if expected == 1:
        return (values,)
    if not isinstance(values, tuple):
        raise TypeError('multi-value output fastdb call-db serialization expects a tuple.')
    if len(values) != expected:
        raise ValueError(f'expected {expected} output values, got {len(values)}.')
    return values


def _call_value_count(binding: FastdbCallDbBinding) -> int:
    positions: list[int] = []
    for table in binding.tables:
        _validate_table_shape(table)
        if table.kind == 'scalars':
            positions.extend(field.value_position for field in table.fields)
            continue
        if table.value_position is None:
            raise ValueError(f'fastdb call-db {table.kind} table {table.name!r} is missing value_position.')
        positions.append(table.value_position)
    if not positions:
        return 0
    _validate_value_positions(positions)
    return len(positions)


def _validate_table_names(tables: tuple[FastdbCallDbTable, ...]) -> None:
    seen: set[str] = set()
    duplicates: list[str] = []
    for table in tables:
        if not isinstance(table.name, str) or not table.name:
            raise ValueError('fastdb call-db table names must be non-empty strings.')
        if table.name in seen:
            duplicates.append(table.name)
            continue
        seen.add(table.name)
    if duplicates:
        raise ValueError(f'fastdb call-db duplicate table name values: {sorted(set(duplicates))!r}.')


def _validate_table_shape(table: FastdbCallDbTable) -> None:
    if table.kind == 'scalars':
        if table.cardinality != 'one':
            raise ValueError(f'fastdb call-db scalar table {table.name!r} must have cardinality "one".')
        if not table.fields:
            raise ValueError(f'fastdb call-db scalar table {table.name!r} must include fields metadata.')
        seen: set[str] = set()
        duplicates: list[str] = []
        for field in table.fields:
            _validate_scalar_field(table, field)
            if field.name in seen:
                duplicates.append(field.name)
            seen.add(field.name)
        if duplicates:
            raise ValueError(f'fastdb call-db duplicate scalar field name values: {sorted(set(duplicates))!r}.')
        _validate_runtime_feature(table)
        return
    if table.kind == 'array':
        if table.cardinality != 'many':
            raise ValueError(f'fastdb call-db array table {table.name!r} must have cardinality "many".')
        if table.item is None:
            raise ValueError(f'fastdb call-db array table {table.name!r} is missing item metadata.')
        if table.item.name != CALL_DB_ARRAY_VALUE_FIELD:
            raise ValueError(
                f'fastdb call-db array table {table.name!r} item name must be '
                f'{CALL_DB_ARRAY_VALUE_FIELD!r}.',
            )
        _validate_scalar_kind(table.item.kind, f'fastdb call-db array table {table.name!r} item')
        _validate_runtime_feature(table)
        return
    if table.kind == 'feature':
        if table.cardinality not in {'one', 'many'}:
            raise ValueError(
                f'fastdb call-db feature table {table.name!r} must have cardinality "one" or "many".',
            )
        _validate_runtime_feature(table)
        return
    raise ValueError(f'Unsupported fastdb call-db table kind {table.kind!r}.')


def _validate_scalar_field(table: FastdbCallDbTable, field: FastdbCallDbScalarField) -> None:
    if not isinstance(field.name, str) or not field.name:
        raise ValueError(f'fastdb call-db scalar field in table {table.name!r} must include a non-empty name.')
    _validate_scalar_kind(field.kind, f'fastdb call-db scalar field {field.name!r}')
    if type(field.value_position) is not int or field.value_position < 0:
        raise ValueError('fastdb call-db scalar value_position values must be non-negative integers.')


def _validate_runtime_feature(table: FastdbCallDbTable) -> None:
    if table.feature is None:
        raise ValueError(f'fastdb call-db table {table.name!r} is missing runtime feature.')
    if not is_feature(table.feature):
        raise TypeError(
            f'{getattr(table.feature, "__name__", table.feature)!r} is not a fastdb @feature class.',
        )
    if table.kind == 'array':
        annotations = _runtime_feature_annotations(
            table.feature,
            context=f'fastdb call-db array table {table.name!r}',
        )
        if set(annotations) != {CALL_DB_ARRAY_VALUE_FIELD}:
            raise ValueError(
                f'fastdb call-db array table {table.name!r} runtime feature fields must be '
                f'[{CALL_DB_ARRAY_VALUE_FIELD!r}].',
            )
        expected_kind = _scalar_kind(annotations[CALL_DB_ARRAY_VALUE_FIELD])
        if table.item is None:
            raise ValueError(f'fastdb call-db array table {table.name!r} is missing item metadata.')
        if table.item.kind != expected_kind:
            raise ValueError(
                f'fastdb call-db array table {table.name!r} item metadata kind {table.item.kind!r} '
                f'does not match runtime feature kind {expected_kind!r}.',
            )
        return
    if table.kind == 'scalars':
        annotations = _runtime_feature_annotations(
            table.feature,
            context=f'fastdb call-db scalar table {table.name!r}',
        )
        field_names = {field.name for field in table.fields}
        annotation_names = set(annotations)
        if field_names != annotation_names:
            raise ValueError(
                f'fastdb call-db scalar table {table.name!r} field metadata {sorted(field_names)!r} '
                f'does not match runtime feature fields {sorted(annotation_names)!r}.',
            )
        for field in table.fields:
            expected_kind = _scalar_kind(annotations[field.name])
            if field.kind != expected_kind:
                raise ValueError(
                    f'fastdb call-db scalar field {field.name!r} metadata kind {field.kind!r} '
                    f'does not match runtime feature kind {expected_kind!r}.',
                )
    if table.kind == 'feature':
        expected_schema = export_schema(table.feature)
        expected_hash = fastdb_schema_sha256(expected_schema)
        if table.feature_schema_sha256 is not None and table.feature_schema_sha256 != expected_hash:
            raise ValueError(
                f'fastdb call-db feature table {table.name!r} feature schema hash '
                f'does not match runtime feature {table.feature.__name__}.',
            )
        expected_dependencies = feature_schema_dependencies(table.feature)
        actual_hashes = tuple(
            dependency.feature_schema_sha256
            for dependency in table.feature_schema_dependencies
        )
        expected_hashes = tuple(fastdb_schema_sha256(dependency) for dependency in expected_dependencies)
        if actual_hashes and actual_hashes != expected_hashes:
            raise ValueError(
                f'fastdb call-db feature table {table.name!r} feature schema dependencies '
                f'do not match runtime feature {table.feature.__name__}.',
            )


def _validate_scalar_kind(kind: object, context: str) -> None:
    if kind not in _VALID_CALL_DB_SCALAR_KINDS:
        raise ValueError(f'{context} uses unsupported fastdb scalar kind {kind!r}.')


def _validate_value_positions(positions: list[int]) -> None:
    seen: set[int] = set()
    duplicates: list[int] = []
    for position in positions:
        if type(position) is not int or position < 0:
            raise ValueError('fastdb call-db value_position values must be non-negative integers.')
        if position in seen:
            duplicates.append(position)
        seen.add(position)
    if duplicates:
        raise ValueError(f'fastdb call-db duplicate value_position values: {sorted(set(duplicates))!r}.')
    actual = sorted(set(positions))
    expected = list(range(len(actual)))
    if actual != expected:
        raise ValueError(f'fastdb call-db value_position values must be contiguous from 0; got {actual!r}.')


def _runtime_feature_annotations(feature_type: type, *, context: str) -> dict[str, object]:
    try:
        from typing import get_type_hints

        hints = get_type_hints(feature_type)
    except NameError:
        hints = dict(getattr(feature_type, '__annotations__', {}))
    except TypeError as exc:
        raise TypeError(f'{context} has invalid runtime feature annotations.') from exc
    return {
        name: annotation
        for name, annotation in hints.items()
        if isinstance(name, str) and not name.startswith('_')
    }


def _create_empty_feature_table(engine: ColumnEngine, table: FastdbCallDbTable) -> None:
    if table.feature is None:
        raise ValueError(f'fastdb call-db table {table.name!r} is missing runtime feature.')
    schema = get_schema(table.feature)
    diagnostics = [
        *raw_payload_storage_diagnostics(schema),
        *non_native_list_storage_diagnostics(schema),
    ]
    if diagnostics:
        raise TypeError(
            f"fastdb call-db cannot create a native table for "
            f"{table.feature.__name__}: {'; '.join(diagnostics)}"
        )
    origin = engine._origin  # noqa: SLF001
    mapped = Table.map_from(
        table.feature,
        _get_default_table_build(
            origin,
            table.name,
            raw_payload=bool(schema.bytes_plan),
        ),
        origin,
    )
    for field in schema.fields:
        if field.field_type == OriginFieldType.list:
            mapped._origin.add_list_field(field.name, field.cpp_type)  # noqa: SLF001
        else:
            mapped._origin.add_field(field.name, field.field_type.value)  # noqa: SLF001
    engine._table_map[table.name] = mapped  # noqa: SLF001
    engine._table_feature_types[table.name] = table.feature  # noqa: SLF001


def _try_encode_feature_table_bulk(
    engine: ColumnEngine,
    table: FastdbCallDbTable,
    value: Any,
) -> bool:
    if table.feature is None or not isinstance(value, Table):
        return False
    if value._feature_type is not table.feature:  # noqa: SLF001
        return False
    if not value.fixed:
        return False
    schema = get_schema(table.feature)
    if not _supports_feature_table_bulk(schema):
        return False

    row_count = len(value)
    if row_count == 0:
        _create_empty_feature_table(engine, table)
        return True

    target, field_ids = _create_truncated_feature_table(engine, table, row_count)
    writes: dict[str, object] = {}
    source_columns = value.column
    for field in schema.fields:
        source_column = getattr(source_columns, field.name)
        if field.field_type == OriginFieldType.str:
            writes[field.name] = _string_column_payload(source_column)
            continue
        if field.field_type == OriginFieldType.bytes:
            raise TypeError(
                f'fastdb call-db bulk encoding does not support '
                f'{table.feature.__name__}.{field.name} with type {field.field_type.name!r}.',
            )
        if field.name in schema.bool_field_names:
            writes[field.name] = _normalize_bool_fill_values(
                field.name,
                source_column,
                row_count,
            )
            continue
        dtype = _FILL_NUMERIC_DTYPES.get(field.field_type)
        if dtype is None:
            raise TypeError(
                f'fastdb call-db bulk encoding does not support '
                f'{table.feature.__name__}.{field.name} with type {field.field_type.name!r}.',
            )
        writes[field.name] = np.ascontiguousarray(
            _numeric_column_array(source_column),
            dtype=dtype,
        )

    _write_fixed_table_columns(target._origin, field_ids, writes)  # noqa: SLF001
    return True


def _try_export_columnar_call_db(
    binding: FastdbCallDbBinding,
    values: tuple[Any, ...],
) -> memoryview | None:
    if len(binding.tables) != 1:
        return None
    table = binding.tables[0]
    if table.kind != 'feature' or table.cardinality != 'many':
        return None
    if table.value_position is None or table.feature is None:
        return None
    value = values[table.value_position]
    if not isinstance(value, Table):
        return None
    return _try_export_exact_feature_table(table, value)


def _try_export_exact_feature_table(table: FastdbCallDbTable, value: Table) -> memoryview | None:
    value._assert_alive()  # noqa: SLF001
    if value._feature_type is not table.feature:  # noqa: SLF001
        return None
    if not value.fixed:
        return None
    if value.name != table.name:
        return None
    db = value._db  # noqa: SLF001
    if not isinstance(db, core.WxDatabase):
        return None
    if db.get_layer_count() != 1:
        return None
    if db.get_layer(0).name() != table.name:
        return None
    return _existing_database_buffer(db)


def _existing_database_buffer(db: core.WxDatabase) -> memoryview | None:
    buffer = getattr(db, '_buffer', None)
    if buffer is None:
        return None
    try:
        view = buffer if isinstance(buffer, memoryview) else memoryview(buffer)
        _ = view.nbytes
    except ValueError as exc:
        raise RuntimeError('FastDB call-db export buffer has been released.') from exc
    except TypeError:
        return None
    return view


def _supports_feature_table_bulk(schema: LayerSchema) -> bool:
    if schema.has_ref_fields or schema.list_plan or schema.bytes_plan:
        return False
    supported = set(_FILL_NUMERIC_DTYPES) | {OriginFieldType.str}
    return all(field.field_type in supported for field in schema.fields)


def _create_truncated_feature_table(
    engine: ColumnEngine,
    table: FastdbCallDbTable,
    row_count: int,
) -> tuple[Table, dict[str, int]]:
    if table.feature is None:
        raise ValueError(f'fastdb call-db table {table.name!r} is missing runtime feature.')
    schema = get_schema(table.feature)
    diagnostics = [
        *raw_payload_storage_diagnostics(schema),
        *non_native_list_storage_diagnostics(schema),
    ]
    if diagnostics:
        raise TypeError(
            f"fastdb call-db cannot create a native table for "
            f"{table.feature.__name__}: {'; '.join(diagnostics)}"
        )
    origin = engine._origin  # noqa: SLF001
    if not isinstance(origin, core.WxDatabaseBuild):
        raise RuntimeError('fastdb call-db bulk encoding requires a writable ColumnEngine.')
    mapped = Table.map_from(
        table.feature,
        _get_default_table_build(
            origin,
            table.name,
            raw_payload=bool(schema.bytes_plan),
        ),
        origin,
    )
    field_ids = {}
    for field in schema.fields:
        field_ids[field.name] = len(field_ids)
        if field.field_type == OriginFieldType.list:
            mapped._origin.add_list_field(field.name, field.cpp_type)  # noqa: SLF001
        else:
            mapped._origin.add_field(field.name, field.field_type.value)  # noqa: SLF001
    origin.truncate(table.name, row_count)
    mapped.feature_count = row_count
    engine._table_map[table.name] = mapped  # noqa: SLF001
    engine._table_feature_types[table.name] = table.feature  # noqa: SLF001
    return mapped, field_ids


def _write_fixed_table_columns(layer_build: object, field_ids: dict[str, int], writes: dict[str, object]) -> None:
    for field_name, payload in writes.items():
        field_index = field_ids[field_name]
        if isinstance(payload, _StringSequencePayload):
            layer_build.set_string_column_from_sequence(field_index, payload.values)
        elif isinstance(payload, tuple):
            offsets, data = payload
            layer_build.set_string_column_bulk(field_index, offsets, data)
        else:
            layer_build.set_numeric_column_bulk(field_index, payload)


def _numeric_column_array(column: object) -> np.ndarray:
    unsafe_numpy_view = getattr(column, 'unsafe_numpy_view', None)
    if callable(unsafe_numpy_view):
        return unsafe_numpy_view()
    return np.asarray(column)


def _string_column_payload(column: object) -> object:
    if isinstance(column, StringColumn):
        offsets = column._offsets()  # noqa: SLF001
        data = column._data()  # noqa: SLF001
        if offsets is not None and data is not None:
            return (
                np.ascontiguousarray(offsets, dtype=np.uint32),
                np.ascontiguousarray(data, dtype=np.uint8),
            )
    return _StringSequencePayload(list(column))


def _array_table_values(table: FastdbCallDbTable, value: Any) -> list[Any]:
    if isinstance(value, (str, bytes, bytearray, memoryview, Mapping)) or not isinstance(value, Iterable):
        raise TypeError(f'{table.name} expected an iterable Array[{_array_item_kind(table)}], got {type(value).__name__}.')
    item_kind = _array_item_kind(table)
    return [_coerce_scalar_value(item_kind, item) for item in value]


def _feature_table_rows(table: FastdbCallDbTable, value: Any) -> list[Any]:
    if table.feature is None:
        raise ValueError(f'fastdb call-db feature table {table.name!r} is missing runtime feature.')
    records = _table_like_records(value)
    source = records if records is not None else value
    if isinstance(source, (str, bytes, bytearray, memoryview, Mapping)) or not isinstance(source, Iterable):
        raise TypeError(f'{table.name} expected an iterable Batch[{table.feature.__name__}], got {type(value).__name__}.')
    return [_coerce_feature_row(table.feature, row) for row in source]


def _coerce_feature_row(feature_type: type, row: Any) -> Any:
    if isinstance(row, feature_type) and not _is_backed_feature_row(row):
        return row
    values: dict[str, Any] = {}
    for field in get_schema(feature_type).fields:
        if isinstance(row, Mapping):
            if field.name not in row:
                raise KeyError(f'missing field {field.name!r} for fastdb feature {feature_type.__name__}.')
            value = row[field.name]
            kind = _FIELD_SCALAR_KIND_BY_FIELD_TYPE.get(field.field_type)
            values[field.name] = _coerce_scalar_value(kind, value) if kind is not None else value
            continue
        if not hasattr(row, field.name):
            raise KeyError(f'missing field {field.name!r} for fastdb feature {feature_type.__name__}.')
        value = getattr(row, field.name)
        kind = _FIELD_SCALAR_KIND_BY_FIELD_TYPE.get(field.field_type)
        values[field.name] = _coerce_scalar_value(kind, value) if kind is not None else value
    return feature_type(**values)


def _is_backed_feature_row(row: Any) -> bool:
    state = getattr(row, '__dict__', None)
    return isinstance(state, dict) and '_fdb_backing' in state


def _table_like_records(value: Any) -> Iterable[Any] | None:
    if isinstance(value, (str, bytes, bytearray, memoryview, Mapping)):
        return None
    to_pylist = getattr(value, 'to_pylist', None)
    if callable(to_pylist):
        return to_pylist()
    to_dicts = getattr(value, 'to_dicts', None)
    if callable(to_dicts):
        return to_dicts()
    to_dict = getattr(value, 'to_dict', None)
    if not callable(to_dict):
        return None
    try:
        return to_dict('records')
    except TypeError:
        try:
            return to_dict(orient='records')
        except TypeError:
            return None


def _column_engine_from_buffer(data: bytes | bytearray | memoryview) -> ColumnEngine:
    engine = ColumnEngine()
    engine._origin = core.WxDatabase.load_xbuffer(data)  # noqa: SLF001
    engine._origin._buffer = data  # noqa: SLF001
    return engine


def _object_engine_from_buffer(
    data: bytes | bytearray | memoryview,
    tables: tuple[FastdbCallDbTable, ...] = (),
) -> ObjectEngine:
    engine = ObjectEngine()
    engine._db = core.WxDatabase.load_xbuffer(data)  # noqa: SLF001
    engine._db._buffer = data  # noqa: SLF001
    engine._buffer = data  # noqa: SLF001
    engine._built = True  # noqa: SLF001

    for index in range(engine._db.get_layer_count()):  # noqa: SLF001
        layer = engine._db.get_layer(index)  # noqa: SLF001
        registered_cls = lookup_class(layer.name())
        if registered_cls is None:
            continue
        schema = get_schema(registered_cls)
        state = LayerState(
            cls=registered_cls,
            schema=schema,
            layer_idx=index,
            row_count=layer.get_feature_count(),
        )
        engine._layers[registered_cls] = state  # noqa: SLF001
        engine._layer_order.append(registered_cls)  # noqa: SLF001
    _bind_call_plan_layers(engine, tables)
    return engine


def _bind_call_plan_layers(
    engine: ObjectEngine,
    tables: tuple[FastdbCallDbTable, ...],
) -> None:
    if not tables:
        return
    layer_indices = {
        engine._db.get_layer(index).name(): index  # noqa: SLF001
        for index in range(engine._db.get_layer_count())  # noqa: SLF001
    }
    for table in tables:
        if table.feature is None:
            continue
        schema = get_schema(table.feature)
        layer_idx = layer_indices.get(schema.layer_name)
        if layer_idx is None:
            continue
        layer = engine._db.get_layer(layer_idx)  # noqa: SLF001
        engine._layers[table.feature] = LayerState(  # noqa: SLF001
            cls=table.feature,
            schema=schema,
            layer_idx=layer_idx,
            row_count=layer.get_feature_count(),
        )
        if table.feature not in engine._layer_order:  # noqa: SLF001
            engine._layer_order.append(table.feature)  # noqa: SLF001


def _encode_object_graph_call_db(binding: FastdbCallDbBinding, values: tuple[Any, ...]) -> bytes:
    engine = ObjectEngine.create()
    for table in binding.tables:
        if table.feature is None:
            raise ValueError(f'fastdb call-db table {table.name!r} is missing runtime feature.')
        if table.kind == 'scalars':
            scalar_values = {
                field.name: _coerce_scalar_value(field.kind, values[field.value_position])
                for field in table.fields
            }
            engine.push(table.feature(**scalar_values))
            continue
        if table.value_position is None:
            raise ValueError(f'fastdb call-db table {table.name!r} is missing value_position.')
        value = values[table.value_position]
        if table.kind == 'array':
            items = _array_table_values(table, value)
            if not items:
                engine._ensure_layer(table.feature)  # noqa: SLF001
                continue
            for item in items:
                engine.push(table.feature(value=item))
            continue
        if table.kind == 'feature':
            if table.cardinality == 'many':
                rows = _feature_table_rows(table, value)
                if not rows:
                    engine._ensure_layer(table.feature)  # noqa: SLF001
                    continue
                for item in rows:
                    engine.push(item)
                continue
            engine.push(value)
            continue
        raise ValueError(f'Unsupported fastdb call-db table kind {table.kind!r}.')
    engine.combine()
    return bytes(engine._buffer)  # noqa: SLF001


def _materialize_values_object_graph(binding: FastdbCallDbBinding, engine: ObjectEngine) -> object:
    values: list[Any] = [None] * _call_value_count(binding)
    seen: dict[tuple[type, int], object] = {}
    for table in binding.tables:
        if table.feature is None:
            raise ValueError(f'fastdb call-db table {table.name!r} is missing runtime feature.')
        if table.kind == 'scalars':
            row = engine.get(table.feature, 0, mode='copy')
            for field in table.fields:
                values[field.value_position] = _materialize_scalar_value(
                    field.kind,
                    getattr(row, field.name),
                )
            continue
        if table.value_position is None:
            raise ValueError(f'fastdb call-db table {table.name!r} is missing value_position.')
        if table.kind == 'array':
            item_kind = _array_item_kind(table)
            values[table.value_position] = [
                _materialize_scalar_value(
                    item_kind,
                    getattr(row, CALL_DB_ARRAY_VALUE_FIELD),
                )
                for row in engine.iter(table.feature, mode='copy')
            ]
            continue
        if table.kind == 'feature':
            if table.cardinality == 'many':
                values[table.value_position] = [
                    _copy_object_graph_feature(engine, table.feature, index, seen)
                    for index in range(engine.count(table.feature))
                ]
            else:
                values[table.value_position] = _copy_object_graph_feature(
                    engine,
                    table.feature,
                    0,
                    seen,
                )
            continue
        raise ValueError(f'Unsupported fastdb call-db table kind {table.kind!r}.')
    return _logical_return(binding, values)


def _copy_object_graph_feature(
    engine: ObjectEngine,
    feature_type: type,
    row_idx: int,
    seen: dict[tuple[type, int], object],
) -> object:
    key = (feature_type, row_idx)
    existing = seen.get(key)
    if existing is not None:
        return existing

    state = engine._layers[feature_type]  # noqa: SLF001
    layer = engine._db.get_layer(state.layer_idx)  # noqa: SLF001
    feature_value = layer.tryGetFeature(row_idx)
    schema = get_schema(feature_type)
    obj = feature_type.__new__(feature_type)
    seen[key] = obj

    from .reader import _read_field

    for field in schema.fields:
        if field.field_type == OriginFieldType.ref:
            ref = feature_value.get_field_as_ref(field.field_id)
            target_type = _target_type_from_ref(engine, ref, field.ref_target)
            if target_type is None:
                value = None
            else:
                value = _copy_object_graph_feature(
                    engine,
                    target_type,
                    _decode_ref_row(ref),
                    seen,
                )
        elif field.field_type == OriginFieldType.list and field.list_elem_type == OriginFieldType.ref:
            target_type = field.list_ref_target
            value = []
            for index in range(feature_value.get_field_list_size(field.field_id)):
                ref = feature_value.get_field_list_ref_at(field.field_id, index)
                item_type = _target_type_from_ref(engine, ref, target_type)
                if item_type is None:
                    value.append(None)
                else:
                    value.append(
                        _copy_object_graph_feature(
                            engine,
                            item_type,
                            _decode_ref_row(ref),
                            seen,
                        ),
                    )
        else:
            value = _read_field(feature_value, field)
        obj.__dict__[field.name] = value
    return obj


def _target_type_from_ref(
    engine: ObjectEngine,
    ref: object,
    declared_target: type | None,
) -> type | None:
    if ref is None:
        return None
    layer_idx = getattr(ref, 'ilayer', None)
    if layer_idx is None:
        return None
    if declared_target is not None:
        return declared_target
    if 0 <= layer_idx < len(engine._layer_order):  # noqa: SLF001
        return engine._layer_order[layer_idx]  # noqa: SLF001
    return None


def _decode_ref_row(ref: object) -> int:
    low = int(getattr(ref, 'ifeature', 0))
    high = int(getattr(ref, 'ifeatureH', 0))
    return low | (high << 8)


def _feature_dependency_layer_names(feature_type: type) -> tuple[tuple[str, str], ...]:
    dependencies: dict[str, tuple[str, str]] = {}
    visiting: set[type] = set()

    def visit(current: type) -> None:
        if current in visiting:
            return
        visiting.add(current)
        schema = get_schema(current)
        for field in schema.ref_fields:
            _visit_target(field.ref_target)
        for field in schema.list_ref_fields:
            _visit_target(field.list_ref_target)
        visiting.remove(current)

    def _visit_target(target: type | None) -> None:
        if target is None or not is_feature(target):
            return
        schema = get_schema(target)
        if schema.layer_name not in dependencies:
            dependencies[schema.layer_name] = (schema.layer_name, target.__name__)
            visit(target)

    visit(feature_type)
    root_layer_name = get_schema(feature_type).layer_name
    dependencies.pop(root_layer_name, None)
    return tuple(
        dependencies[layer_name]
        for layer_name in sorted(dependencies)
    )


def _scalar_kind(annotation: object) -> str | None:
    if annotation is BOOL or annotation is bool:
        return 'bool'
    field_type = get_origin_type(annotation)
    if field_type == OriginFieldType.unknown:
        return None
    return _FIELD_SCALAR_KIND_BY_FIELD_TYPE.get(field_type)


def _materialize_scalar_value(kind: str, value: object) -> object:
    if kind == 'bool':
        return coerce_bool_scalar(value)
    return value


def _coerce_scalar_value(kind: str | None, value: object) -> object:
    if kind == 'bool':
        return coerce_bool_scalar(value)
    if kind in {'u8', 'u16', 'u32', 'i32', 'u8n', 'u16n'}:
        return int(value)
    if kind in {'f32', 'f64'}:
        return float(value)
    if kind in {'str', 'wstr'}:
        return str(value)
    if kind == 'bytes':
        return bytes(value)
    return value


def _array_item_kind(table: FastdbCallDbTable) -> str:
    if table.item is None:
        raise ValueError(f'fastdb call-db array table {table.name!r} is missing item metadata.')
    return table.item.kind
