from __future__ import annotations

from dataclasses import dataclass, field
from typing import Any, TypeVar, overload

from .type import Array, ArrayRequirement, Batch, BatchRequirement

T = TypeVar('T')
U = TypeVar('U')
V = TypeVar('V')
W = TypeVar('W')
X = TypeVar('X')

RequirementSpec = BatchRequirement[Any] | ArrayRequirement[Any]


@dataclass
class RequireEnvelope:
    specs: tuple[RequirementSpec, ...]
    state: str = 'open'
    generation: int = 0
    values: tuple[object, ...] = field(default_factory=tuple)

    def bind_values(self, values: tuple[object, ...]) -> None:
        if len(values) != len(self.specs):
            raise ValueError('RequireEnvelope value count must match spec count.')
        self.values = values


@overload
def require(__a: BatchRequirement[T]) -> Batch[T]: ...


@overload
def require(__a: ArrayRequirement[T]) -> Array[T]: ...


@overload
def require(__a: BatchRequirement[T], __b: ArrayRequirement[U]) -> tuple[Batch[T], Array[U]]: ...


@overload
def require(__a: ArrayRequirement[T], __b: BatchRequirement[U]) -> tuple[Array[T], Batch[U]]: ...


@overload
def require(
    __a: RequirementSpec,
    __b: RequirementSpec,
    __c: RequirementSpec,
) -> tuple[object, object, object]: ...


@overload
def require(
    __a: RequirementSpec,
    __b: RequirementSpec,
    __c: RequirementSpec,
    __d: RequirementSpec,
) -> tuple[object, object, object, object]: ...


@overload
def require(
    __a: RequirementSpec,
    __b: RequirementSpec,
    __c: RequirementSpec,
    __d: RequirementSpec,
    __e: RequirementSpec,
) -> tuple[object, object, object, object, object]: ...


def require(*specs: RequirementSpec) -> object:
    if not specs:
        raise ValueError('fdb.require expects at least one requirement spec.')
    for spec in specs:
        if not isinstance(spec, (BatchRequirement, ArrayRequirement)):
            raise TypeError(
                'fdb.require expects BatchRequirement or ArrayRequirement specs.',
            )

    envelope = RequireEnvelope(specs=tuple(specs))
    values = tuple(
        _value_for_spec(spec, envelope=envelope, index=index)
        for index, spec in enumerate(specs)
    )
    envelope.bind_values(values)
    return values[0] if len(values) == 1 else values


def _value_for_spec(
    spec: RequirementSpec,
    *,
    envelope: RequireEnvelope,
    index: int,
) -> object:
    if isinstance(spec, BatchRequirement):
        value = Batch.allocate(spec.feature_type, spec.rows, profile=spec.profile)
    elif isinstance(spec, ArrayRequirement):
        value = Array.allocate(spec.item_type, spec.rows)
    else:
        raise TypeError('unsupported FastDB requirement spec.')
    _attach_require_metadata(value, envelope=envelope, index=index)
    return value


def _attach_require_metadata(value: object, *, envelope: RequireEnvelope, index: int) -> None:
    setattr(value, '_fastdb_require_envelope', envelope)
    setattr(value, '_fastdb_require_index', index)


def _require_envelope_for(value: object) -> RequireEnvelope | None:
    envelope = getattr(value, '_fastdb_require_envelope', None)
    return envelope if isinstance(envelope, RequireEnvelope) else None


def _require_index_for(value: object) -> int | None:
    index = getattr(value, '_fastdb_require_index', None)
    return index if type(index) is int else None
