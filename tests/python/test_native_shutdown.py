"""Borrowed buffers survive finalization and release through native destruction."""
import gc
import mmap
from multiprocessing import shared_memory
import subprocess
import sys
import textwrap
import weakref

import numpy as np
import pytest

import fastdb4py as fdb
from fastdb4py import core


@fdb.feature
class ShutdownValue:
    value: fdb.F32


def _bytes():
    engine = fdb.RecordEngine.truncate([fdb.Layout(ShutdownValue, 4)])
    engine.table(ShutdownValue).fill(value=np.arange(4, dtype=np.float32))
    data = engine._origin.buffer().to_bytes()
    engine.close()
    return data


@pytest.mark.parametrize('kind', ['bytes', 'bytearray', 'mmap', 'shared_memory'])
def test_native_buffer_pin_and_mapping_release_without_cyclic_gc(kind):
    enabled = gc.isenabled()
    gc.disable()
    data = _bytes()
    mapping = None
    segment = None
    if kind == 'mmap':
        mapping = mmap.mmap(-1, len(data))
        mapping[:] = data
        backing = mapping
    elif kind == 'shared_memory':
        segment = shared_memory.SharedMemory(create=True, size=len(data))
        segment.buf[:len(data)] = data
        mapping = segment._mmap
        backing = segment.buf
    else:
        backing = bytes(data) if kind == 'bytes' else bytearray(data)
    try:
        native = core.WxDatabase.load_xbuffer(backing, segment)
        reference = weakref.ref(native)
        column = native.get_layer(0).get_column(0).as_nparray()
        if kind == 'bytearray':
            with pytest.raises(BufferError):
                backing.extend(b'x')
        elif kind in ('mmap', 'shared_memory'):
            with pytest.raises(BufferError):
                mapping.close()
        del native
        assert reference() is not None
        np.testing.assert_array_equal(column, [0, 1, 2, 3])
        del column
        assert reference() is None
        if kind == 'shared_memory':
            assert mapping.closed
        elif kind == 'mmap':
            mapping.close()
            assert mapping.closed
        elif kind == 'bytearray':
            backing.extend(b'x')
    finally:
        if segment is not None:
            segment.unlink()
            segment.close()
        elif mapping is not None:
            mapping.close()
        if enabled:
            gc.enable()


@pytest.mark.parametrize('data', [b'', b'wrong database', b'FASTVectorDB0.1\0' + b'\0' * 3])
def test_invalid_buffers_have_one_error_and_release_their_export(data):
    backing = bytearray(data)
    with pytest.raises(ValueError, match=r'^Invalid FastDB database buffer\.$'):
        core.WxDatabase.load_xbuffer(backing)
    backing.extend(b'x')


def test_array_base_always_retains_the_actual_chunk():
    native = core.WxDatabase.load_xbuffer(_bytes())
    chunk = native.buffer()
    array = chunk.as_array(np.uint8)
    assert array.base is chunk
    with pytest.raises(TypeError):
        chunk.as_array(np.uint8, None)
    reference = weakref.ref(native)
    del native, chunk
    assert reference() is not None
    assert array.tobytes().startswith(b'FASTVectorDB0.1')
    del array
    assert reference() is None


def test_failed_shared_load_closes_its_mapping():
    from fastdb4py.record_engine import _load_shared_database

    segment = shared_memory.SharedMemory(create=True, size=64)
    mapping = segment._mmap
    try:
        with pytest.raises(ValueError, match=r'^Invalid FastDB database buffer\.$'):
            _load_shared_database(segment)
        assert mapping.closed
    finally:
        segment.unlink()
        segment.close()


def test_invalid_file_uses_the_same_native_factory_error(tmp_path):
    path = tmp_path / 'invalid.fdb'
    path.write_bytes(b'not a database')
    with pytest.raises(ValueError, match=r'^Invalid FastDB database buffer\.$'):
        fdb.RecordEngine.load(str(path), from_file=True)


def test_close_does_not_invalidate_an_owner_shared_by_two_engines():
    owner = fdb.FdbViewOwner(checked=True)
    first = fdb.RecordEngine.truncate([fdb.Layout(ShutdownValue, 4)])
    second = fdb.RecordEngine.truncate([fdb.Layout(ShutdownValue, 4)])
    left = first.table(ShutdownValue, owner=owner)
    right = second.table(ShutdownValue, owner=owner)
    first.close()
    assert owner.alive
    assert left[0].value == right[0].value == 0
    fdb.invalidate(owner)
    with pytest.raises(fdb.FdbViewInvalidatedError):
        _ = right[0].value
    second.close()


_SHUTDOWN_SETUP = '''
import gc, secrets, threading, time
import numpy as np
import fastdb4py as fdb
@fdb.feature
class V:
    value: fdb.F32
name = 'fdb_shutdown_' + secrets.token_hex(8)
publisher = fdb.RecordEngine.truncate([fdb.Layout(V, 2000000)])
publisher.table(V).fill(value=np.ones(2000000, dtype=np.float32))
publisher.share(name)
'''

_SHUTDOWN_CASES = {
    'daemon': '''
reader = fdb.RecordEngine.load(name)
column = reader.table(V).column.value
publisher.unlink()
ready = threading.Event()
def worker():
    ready.set()
    while True:
        float(column.sum())
threading.Thread(target=worker, daemon=True).start()
ready.wait()
time.sleep(0.1)
print('daemon reader exits safely', flush=True)
''',
    'module_del': '''
class Reporter:
    def __init__(self, column):
        self.column = column
    def __del__(self):
        assert float(self.column.sum()) == 2000000
        print('module destructor read passed', flush=True)
reader = fdb.RecordEngine.load(name)
reporter = Reporter(reader.table(V).column.value)
publisher.unlink()
''',
    'gc_cycle': '''
class Service:
    def __init__(self):
        self.engine = fdb.RecordEngine.load(name)
        self.table = self.engine.table(V)
        self.cycle = self
    def __del__(self):
        assert self.table[3].value == 1
        print('cycle destructor read passed', flush=True)
service = Service()
mapping = service.engine._shm._mmap
del service
gc.collect()
assert mapping.closed
publisher.unlink()
print('cycle mapping closed', flush=True)
''',
}


@pytest.mark.parametrize('case', _SHUTDOWN_CASES)
def test_shared_memory_shutdown_subprocess(case, tmp_path):
    result = subprocess.run(
        [sys.executable, '-X', 'faulthandler', '-c',
         textwrap.dedent(_SHUTDOWN_SETUP + _SHUTDOWN_CASES[case])],
        cwd=tmp_path, capture_output=True, text=True, timeout=30,
    )
    assert result.returncode == 0, result.stdout + result.stderr
    assert 'Exception ignored' not in result.stderr, result.stderr
    if case == 'gc_cycle':
        assert 'cycle mapping closed' in result.stdout
    elif case == 'module_del':
        assert 'module destructor read passed' in result.stdout
