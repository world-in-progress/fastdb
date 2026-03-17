from ..type import OriginFieldType
from ._schema import get_class_schema


def parse_defns(cls):
    """Return field name → (OriginFieldType, field_index) mapping for cls."""
    return get_class_schema(cls).origin_hints


def get_all_defns(cls) -> list[tuple[str, OriginFieldType]]:
    """Return [(field_name, OriginFieldType), ...] sorted by field index."""
    return get_class_schema(cls).ordered_defns