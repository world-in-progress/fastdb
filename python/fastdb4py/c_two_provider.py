from __future__ import annotations

from dataclasses import dataclass
from typing import Any

from . import core
from .column_engine import ColumnEngine
from .object_engine import LayerState, ObjectEngine
from .registry import get_schema, is_feature, lookup_class
from .schema import (
    columnar_capability,
    codec_ref_for_feature,
    export_schema,
    object_graph_capability,
)
from .type import OriginFieldType


@dataclass(frozen=True)
class FastdbCodecAdapter:
    feature_type: type
    profile: str
    codec_ref: dict[str, Any]

    def serialize(self, value: object) -> bytes:
        if self.profile == 'columnar.v1':
            engine = ColumnEngine.create()
            engine.push(value)
            engine.combine()
            chunk = engine._origin.buffer()  # noqa: SLF001
            return chunk.to_bytes()
        if self.profile == 'object_graph.v1':
            engine = ObjectEngine.create()
            engine.push(value)
            engine.combine()
            return bytes(engine._buffer)  # noqa: SLF001
        raise ValueError(f'unsupported fastdb codec profile {self.profile!r}')

    def deserialize(self, data: bytes | bytearray | memoryview) -> object:
        payload = bytes(data)
        if self.profile == 'columnar.v1':
            engine = ColumnEngine()
            engine._origin = core.WxDatabase.load_xbuffer(payload)  # noqa: SLF001
            engine._origin._buffer = payload  # noqa: SLF001
            table = engine.table(self.feature_type)
            return table[0]
        if self.profile == 'object_graph.v1':
            engine = _object_engine_from_bytes(payload)
            return _copy_object_graph_feature(engine, self.feature_type, 0, {})
        raise ValueError(f'unsupported fastdb codec profile {self.profile!r}')

    def from_buffer(self, data: memoryview) -> object:
        return self.deserialize(data)


class FastdbCodecProvider:
    """Dependency-neutral fastdb codec candidate provider.

    This class intentionally does not import C-Two. It returns plain candidate
    dictionaries containing opaque codec refs and fastdb schema descriptors; a
    C-Two adapter package can wrap these candidates with concrete transfer
    adapters.
    """

    def candidates_for_type(
        self,
        typ: type,
        context: object | None = None,
    ) -> list[dict[str, Any]]:
        if not is_feature(typ):
            return []
        columnar = columnar_capability(typ)
        if columnar['eligible']:
            return [{
                'codec_ref': codec_ref_for_feature(typ, profile='columnar'),
                'profile': 'columnar.v1',
                'schema': export_schema(typ),
            }]
        graph = object_graph_capability(typ)
        if graph['eligible']:
            return [{
                'codec_ref': codec_ref_for_feature(typ, profile='object_graph'),
                'profile': 'object_graph.v1',
                'schema': export_schema(typ),
            }]
        return []

    def adapter_for_type(self, typ: type) -> FastdbCodecAdapter:
        candidates = self.candidates_for_type(typ)
        if not candidates:
            raise TypeError(f'{typ!r} has no fastdb C-Two codec candidate.')
        candidate = candidates[0]
        return FastdbCodecAdapter(
            feature_type=typ,
            profile=candidate['profile'],
            codec_ref=candidate['codec_ref'],
        )


def _object_engine_from_bytes(payload: bytes) -> ObjectEngine:
    engine = ObjectEngine()
    engine._db = core.WxDatabase.load_xbuffer(payload)  # noqa: SLF001
    engine._db._buffer = payload  # noqa: SLF001
    engine._buffer = payload  # noqa: SLF001
    engine._built = True  # noqa: SLF001

    for index in range(engine._db.get_layer_count()):  # noqa: SLF001
        layer = engine._db.get_layer(index)  # noqa: SLF001
        layer_name = layer.name()
        registered_cls = lookup_class(layer_name)
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
    return engine


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
    feature = layer.tryGetFeature(row_idx)
    schema = get_schema(feature_type)
    obj = feature_type.__new__(feature_type)
    seen[key] = obj

    from .reader import _read_field

    for field in schema.fields:
        if field.field_type == OriginFieldType.ref:
            ref = feature.get_field_as_ref(field.field_id)
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
            for index in range(feature.get_field_list_size(field.field_id)):
                ref = feature.get_field_list_ref_at(field.field_id, index)
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
            value = _read_field(feature, field)
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


provider = FastdbCodecProvider()
