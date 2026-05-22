from __future__ import annotations

from collections.abc import Mapping
from threading import Lock
from typing import Any, Callable


class FdbViewInvalidatedError(RuntimeError):
    """Raised when a backed FastDB view is used after its owner is invalidated."""


class FdbViewWriteError(RuntimeError):
    """Raised when a backed FastDB view cannot be written safely."""


class FdbViewOwner:
    """Logical lifetime owner for backed FastDB views.

    ``checked=False`` is the trusted standalone fast path: invalidation and
    generation checks are intentionally cheap no-ops for existing workflows.
    The writeable flag still gates explicit read-only views.
    ``checked=True`` is for call-scoped or lease-scoped views where stale use
    must fail deterministically.
    """

    def __init__(
        self,
        *,
        checked: bool = True,
        writeable: bool = False,
        release: Callable[[], None] | None = None,
    ) -> None:
        self.checked = checked
        self.writeable = writeable
        self.generation = 0
        self._alive = True
        self._release = release
        self._lock = Lock()

    @property
    def alive(self) -> bool:
        if not self.checked:
            return self._alive
        with self._lock:
            return self._alive

    def assert_alive(self, generation: int | None = None) -> None:
        if not self.checked:
            return
        with self._lock:
            if not self._alive:
                raise FdbViewInvalidatedError('FastDB backed view owner has been invalidated.')
            if generation is not None and generation != self.generation:
                raise FdbViewInvalidatedError('FastDB backed view is stale after owner remap.')

    def assert_writeable(self, generation: int | None = None) -> None:
        self.assert_alive(generation)
        if not self.writeable:
            raise FdbViewWriteError('FastDB backed view is read-only.')

    def invalidate(self) -> None:
        with self._lock:
            if not self._alive:
                return
            self._alive = False
            self.generation += 1
            release = self._release
            self._release = None
        if release is not None:
            release()

    def bump_generation(self) -> None:
        with self._lock:
            if not self._alive:
                return
            self.generation += 1


def trusted_view_owner(*, writeable: bool = True) -> FdbViewOwner:
    return FdbViewOwner(checked=False, writeable=writeable)


def owner_of(value: Any) -> FdbViewOwner | None:
    if isinstance(value, FdbViewOwner):
        return value

    owner = getattr(value, '_owner', None)
    if isinstance(owner, FdbViewOwner):
        return owner

    cache = getattr(value, '__dict__', None)
    if isinstance(cache, dict):
        backing = cache.get('_fdb_backing')
        owner = getattr(backing, 'owner', None)
        if isinstance(owner, FdbViewOwner):
            return owner

    owner = getattr(value, '_fdb_owner', None)
    if isinstance(owner, FdbViewOwner):
        return owner

    table = getattr(value, '_table', None)
    owner = getattr(table, '_fdb_owner', None)
    if isinstance(owner, FdbViewOwner):
        return owner

    return None


def invalidate(value: Any) -> None:
    """Invalidate a FastDB owner or every FastDB-managed view inside a container."""
    if isinstance(value, FdbViewOwner):
        value.invalidate()
        return

    if isinstance(value, Mapping):
        for item in value.values():
            invalidate(item)
        return

    if isinstance(value, (list, tuple, set, frozenset)):
        for item in value:
            invalidate(item)
        return

    owner = owner_of(value)
    if owner is not None:
        owner.invalidate()
