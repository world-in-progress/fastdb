from __future__ import annotations
from typing import TYPE_CHECKING, Iterator

if TYPE_CHECKING:
    from .feature import Feature


class FeatureRefList:
    """
    Lazy zero-copy wrapper for list<ref> columns in a fastdb layer.

    Elements are resolved on first access and cached per-index.
    Lifetime is bound to the parent ORM / shared memory segment.
    """

    __slots__ = ('_origin', '_fid', '_cls', '_db', '_cache', '_len')

    def __init__(self, origin, fid: int, ref_cls: type, db):
        self._origin = origin  # core.WxFeature (C++ wrapper)
        self._fid    = fid
        self._cls    = ref_cls
        self._db     = db
        self._cache: dict = {}
        self._len: int | None = None

    def __len__(self) -> int:
        if self._len is None:
            self._len = int(self._origin.get_field_list_size(self._fid))
        return self._len

    def __getitem__(self, idx: int) -> 'Feature':
        n = len(self)
        if idx < 0:
            idx = n + idx
        if idx < 0 or idx >= n:
            raise IndexError(f'FeatureRefList index {idx} out of range [0, {n})')
        if idx in self._cache:
            return self._cache[idx]
        ref = self._origin.get_field_list_ref_at(self._fid, idx)
        raw = self._db.tryGetFeature(ref)
        feat = self._cls.map_from(self._db, raw)
        self._cache[idx] = feat
        return feat

    def __iter__(self) -> Iterator['Feature']:
        for i in range(len(self)):
            yield self[i]

    def to_list(self) -> list:
        return [self[i] for i in range(len(self))]

    def __repr__(self) -> str:
        return f'FeatureRefList<{self._cls.__name__}>[{len(self)}]'
