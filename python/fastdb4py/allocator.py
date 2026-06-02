from __future__ import annotations

from typing import Protocol

from . import core


ScratchAllocation = core.WxScratchAllocation
ScratchAllocator = core.WxScratchAllocator
HeapScratchAllocator = core.WxHeapScratchAllocator
FinalBackingAllocation = core.WxFinalBackingAllocation
FinalBackingResource = core.WxFinalBackingResource
HeapFinalBackingResource = core.WxHeapFinalBackingResource


class WritableAllocation(Protocol):
    @property
    def buffer(self) -> memoryview: ...

    def commit(self, used_size: int) -> object: ...

    def rollback(self) -> None: ...


class WritableAllocator(Protocol):
    def allocate(self, nbytes: int) -> WritableAllocation: ...


class BytearrayAllocation:
    def __init__(self, nbytes: int, allocator: 'BytearrayAllocator'):
        if type(nbytes) is not int or nbytes < 0:
            raise ValueError('allocation size must be a non-negative integer.')
        self._data = bytearray(nbytes)
        self._allocator = allocator
        self._state = 'open'

    @property
    def buffer(self) -> memoryview:
        self._ensure_open()
        return memoryview(self._data)

    def commit(self, used_size: int) -> bytes:
        self._ensure_open()
        if type(used_size) is not int or used_size < 0 or used_size > len(self._data):
            raise ValueError('used_size must fit within the allocation.')
        self._state = 'committed'
        self._allocator.commit_count += 1
        return bytes(self._data[:used_size])

    def rollback(self) -> None:
        if self._state != 'open':
            return
        self._state = 'rolled_back'
        self._allocator.rollback_count += 1

    def _ensure_open(self) -> None:
        if self._state != 'open':
            raise RuntimeError(f'allocation is {self._state}.')


class BytearrayAllocator:
    def __init__(self):
        self.allocate_count = 0
        self.commit_count = 0
        self.rollback_count = 0
        self.allocations: list[BytearrayAllocation] = []

    def allocate(self, nbytes: int) -> BytearrayAllocation:
        self.allocate_count += 1
        allocation = BytearrayAllocation(nbytes, self)
        self.allocations.append(allocation)
        return allocation


def build_call_db(
    binding: object,
    values: object,
    allocator: WritableAllocator,
    *,
    direct_required: bool = False,
) -> object:
    from .call_db import build_call_db_with_allocator

    return build_call_db_with_allocator(
        binding,
        values,
        allocator,
        direct_required=direct_required,
    )
