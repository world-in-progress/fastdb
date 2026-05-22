from __future__ import annotations

from collections.abc import Mapping
from dataclasses import is_dataclass
from typing import Any

import numpy as np

from .registry import get_schema, is_feature


def materialize(value: Any) -> Any:
    """Return an owned Python value detached from any fastdb backing buffer."""
    from .orm.table import Table
    from .string_column import BytesColumn, StringColumn

    if isinstance(value, Table):
        return materialize_table(value)
    if isinstance(value, (StringColumn, BytesColumn)):
        return value.to_pylist()
    if isinstance(value, np.ndarray):
        return value.copy()
    if isinstance(value, memoryview):
        return bytes(value)
    if isinstance(value, bytearray):
        return bytes(value)
    if isinstance(value, tuple):
        return tuple(materialize(item) for item in value)
    if isinstance(value, list):
        return [materialize(item) for item in value]
    if isinstance(value, Mapping):
        return type(value)(
            (key, materialize(item))
            for key, item in value.items()
        )

    to_owned = getattr(value, 'to_owned', None)
    if callable(to_owned):
        owned = to_owned()
        if owned is not value:
            return materialize(owned)

    if is_feature(type(value)):
        return materialize_feature(value)
    return value


def materialize_table(table: Any) -> list[Any]:
    return [materialize(row) for row in table]


def materialize_feature(value: Any) -> Any:
    value_type = type(value)
    cls = getattr(value_type, '__fastdb_feature_base__', value_type)
    schema = get_schema(cls)
    owned = cls.__new__(cls)
    for field in schema.fields:
        owned.__dict__[field.name] = materialize(getattr(value, field.name))
    if is_dataclass(value):
        return owned
    return owned
