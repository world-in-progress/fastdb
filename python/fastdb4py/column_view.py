from __future__ import annotations

from typing import Any, Iterator, TYPE_CHECKING

import numpy as np

from .view_owner import FdbViewOwner, FdbViewWriteError

if TYPE_CHECKING:
    from .orm.table import Table


class NumericColumnView:
    """Owner-checked view over a numeric native column."""

    def __init__(self, table: 'Table', field_index: int, field_name: str, array: np.ndarray):
        self._table = table
        self._field_index = field_index
        self._field_name = field_name
        self._array = array
        self._owner: FdbViewOwner = table._fdb_owner
        self._writeable = table._fdb_writeable
        self._owner_generation = table._fdb_owner.generation

    def _checked_array(self) -> np.ndarray:
        self._owner.assert_alive(self._owner_generation)
        return self._array

    def _assert_writeable(self) -> None:
        self._owner.assert_alive(self._owner_generation)
        if not self._writeable:
            raise FdbViewWriteError('FastDB column view is read-only.')
        self._owner.assert_writeable(self._owner_generation)

    def __len__(self) -> int:
        return len(self._checked_array())

    def __getitem__(self, index):
        return self._checked_array()[index]

    def __setitem__(self, index, value) -> None:
        self._assert_writeable()
        self._array[index] = value

    def __iter__(self) -> Iterator[Any]:
        for index in range(len(self)):
            yield self[index]

    @property
    def dtype(self):
        return self._checked_array().dtype

    @property
    def shape(self):
        return self._checked_array().shape

    @property
    def ndim(self) -> int:
        return self._checked_array().ndim

    @property
    def size(self) -> int:
        return self._checked_array().size

    def to_numpy(self) -> np.ndarray:
        return self._checked_array().copy()

    def to_owned(self) -> np.ndarray:
        return self.to_numpy()

    def unsafe_numpy_view(self) -> np.ndarray:
        return self._checked_array()

    def __array__(self, dtype=None, copy=None):
        arr = self.to_numpy()
        if dtype is not None:
            arr = arr.astype(dtype, copy=False)
        return arr

    def __repr__(self) -> str:
        try:
            shape = self.shape
            dtype = self.dtype
        except Exception:
            shape = '<invalid>'
            dtype = '<invalid>'
        return f'NumericColumnView(field={self._field_name!r}, shape={shape}, dtype={dtype})'
