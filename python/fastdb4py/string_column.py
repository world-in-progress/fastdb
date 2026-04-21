from __future__ import annotations

from typing import Iterable, TYPE_CHECKING

import numpy as np

if TYPE_CHECKING:
    from .orm.table import Table


class StringColumn:
    def __init__(self, table: 'Table', field_index: int, field_name: str):
        self._table = table
        self._field_index = field_index
        self._field_name = field_name

    def __len__(self) -> int:
        return len(self._table)

    def _offsets(self) -> np.ndarray | None:
        chunk = self._table._origin.get_string_column_offsets(self._field_index)
        if chunk is None or chunk.size == 0:
            return None
        return chunk.as_array(np.uint32)

    def _data(self) -> np.ndarray | None:
        chunk = self._table._origin.get_string_column_data(self._field_index)
        if chunk is None or chunk.size == 0:
            return np.empty(0, dtype=np.uint8)
        return chunk.as_array(np.uint8)

    def get(self, index: int) -> str:
        length = len(self)
        if index < 0:
            index += length
        if index < 0 or index >= length:
            raise IndexError(f'String index {index} out of range [0, {length}).')

        offsets = self._offsets()
        if offsets is not None:
            data = self._data()
            start = int(offsets[index])
            end = int(offsets[index + 1])
            return bytes(data[start:end]).decode('utf-8')

        feat = self._table._origin.tryGetFeature(index)
        raw = feat.get_field_as_string_view(self._field_index)
        if raw is None:
            return ''
        return raw.to_bytes().decode('utf-8')

    def to_pylist(self) -> list[str]:
        return [self.get(i) for i in range(len(self))]

    def fill(self, strings: Iterable[str]) -> None:
        offsets_arr, data_arr = self._normalize_fill_values(strings, len(self))
        self.fill_utf8(offsets_arr, data_arr)

    def _normalize_fill_values(
        self,
        strings: Iterable[str],
        expected_len: int,
    ) -> tuple[np.ndarray, np.ndarray]:
        raw = bytearray()
        offsets = [0]
        for value in strings:
            encoded = ('' if value is None else str(value)).encode('utf-8')
            raw.extend(encoded)
            offsets.append(len(raw))
        offsets_arr = np.asarray(offsets, dtype=np.uint32)
        data_arr = np.frombuffer(bytes(raw), dtype=np.uint8)
        return self._validate_utf8_payload(offsets_arr, data_arr, expected_len)

    def _validate_utf8_payload(
        self,
        offsets: np.ndarray,
        data: np.ndarray,
        expected_len: int,
    ) -> tuple[np.ndarray, np.ndarray]:
        offsets_arr = np.ascontiguousarray(offsets, dtype=np.uint32)
        data_arr = np.ascontiguousarray(data, dtype=np.uint8)

        if offsets_arr.ndim != 1:
            raise ValueError('offsets must be a 1D uint32 array.')
        if data_arr.ndim != 1:
            raise ValueError('data must be a 1D uint8 array.')
        if len(offsets_arr) != expected_len + 1:
            raise ValueError(
                f'{self._field_name} expected {expected_len} rows, got {len(offsets_arr) - 1}.'
            )
        if len(offsets_arr) == 0 or int(offsets_arr[0]) != 0:
            raise ValueError('offsets must start at 0.')
        if np.any(offsets_arr[1:] < offsets_arr[:-1]):
            raise ValueError('offsets must be monotonically non-decreasing.')
        if int(offsets_arr[-1]) != int(data_arr.size):
            raise ValueError('offsets[-1] must equal the UTF-8 byte length.')
        return offsets_arr, data_arr

    def fill_utf8(self, offsets: np.ndarray, data: np.ndarray) -> None:
        offsets_arr, data_arr = self._validate_utf8_payload(offsets, data, len(self))

        fill_handler = self._table._fixed_fill_handler
        if fill_handler is not None:
            fill_handler({self._field_name: (offsets_arr, data_arr)})
            return

        table_origin = self._table._origin
        if hasattr(table_origin, 'set_string_column_bulk'):
            table_origin.set_string_column_bulk(self._field_index, offsets_arr, data_arr)
            return

        raise RuntimeError(
            'StringColumn.fill_utf8() requires a writable truncate table. '
            'Loaded read-only databases support reads only.'
        )
