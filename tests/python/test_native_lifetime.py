"""Native ownership, borrowed view safety, and bounded long-running retention."""
import gc
import json
from pathlib import Path
import subprocess
import sys
import weakref

import numpy as np
import pytest

import fastdb4py as fdb
from fastdb4py import core


@fdb.feature
class NativeValue:
    value: fdb.F32


def _engine(kind):
    engine_type = getattr(fdb, kind)
    engine = engine_type.truncate([fdb.Layout(NativeValue, 3)])
    engine.table(NativeValue).column.value[:] = [1.25, 2.5, 3.75]
    return engine


@pytest.fixture(autouse=True)
def _without_gc():
    enabled = gc.isenabled()
    gc.disable()
    try:
        yield
    finally:
        if enabled:
            gc.enable()


@pytest.mark.parametrize('kind', ['RecordEngine', 'ObjectEngine'])
@pytest.mark.parametrize('view_kind', ['table', 'row', 'column', 'slice', 'asarray', 'reuse'])
def test_trusted_views_keep_native_database_after_engine_close(kind, view_kind):
    engine = _engine(kind)
    table = engine.table(NativeValue)
    native = table._db
    assert native.thisown
    reference = weakref.ref(native)
    if view_kind == 'table':
        view = table
    elif view_kind == 'row':
        view = table[1]
    elif view_kind == 'reuse':
        view = next(table.iter_reuse())
    elif view_kind == 'slice':
        view = table.column.value[1:]
    elif view_kind == 'asarray':
        view = np.asarray(table.column.value)[1:]
    else:
        view = table.column.value
    if kind == 'RecordEngine':
        engine.close()
        engine.close()
    del native, table, engine
    assert reference() is not None
    if view_kind == 'table':
        assert view[1].value == 2.5
    elif view_kind in ('row', 'reuse'):
        assert view.value == (1.25 if view_kind == 'reuse' else 2.5)
    else:
        expected = [2.5, 3.75] if view_kind in ('slice', 'asarray') else [1.25, 2.5, 3.75]
        np.testing.assert_array_equal(view, expected)
        view[-1] = 9
        assert view[-1] == 9
    del view
    assert reference() is None


@pytest.mark.parametrize('kind', ['RecordEngine', 'ObjectEngine'])
def test_checked_views_survive_engine_close_until_caller_invalidates_owner(kind):
    engine = _engine(kind)
    owner = fdb.FdbViewOwner(checked=True, writeable=True)
    table = engine.table(NativeValue, owner=owner)
    row = table[0]
    column = table.column.value
    detached = column.to_numpy()
    if kind == 'RecordEngine':
        engine.close()
    del engine
    assert row.value == 1.25
    assert column[0] == 1.25
    fdb.invalidate(owner)
    for read in (lambda: len(table), lambda: row.value, lambda: column[0]):
        with pytest.raises(fdb.FdbViewInvalidatedError):
            read()
    np.testing.assert_array_equal(detached, [1.25, 2.5, 3.75])


def test_file_loaded_database_and_its_raw_column_release_after_last_reference(tmp_path):
    engine = _engine('RecordEngine')
    path = tmp_path / 'value.fdb'
    engine.save(str(path))
    engine.close()
    loaded = fdb.RecordEngine.load(str(path), from_file=True)
    native = loaded._origin
    reference = weakref.ref(native)
    assert native.thisown
    column = loaded.table(NativeValue).column.value
    loaded.close()
    assert loaded._origin is None
    del native, loaded
    np.testing.assert_array_equal(column, [1.25, 2.5, 3.75])
    assert reference() is not None
    del column
    assert reference() is None


def test_borrowed_buffer_remains_exported_and_alive_until_native_database_release():
    engine = _engine('RecordEngine')
    backing = bytearray(engine._origin.buffer().to_bytes())
    engine.close()
    native = core.WxDatabase.load_xbuffer(backing)
    assert native.thisown
    reference = weakref.ref(native)
    layer = native.get_layer(0)
    column = layer.get_column(0).as_nparray()
    with pytest.raises(BufferError):
        backing.extend(b'x')
    del native, layer
    np.testing.assert_array_equal(column, [1.25, 2.5, 3.75])
    assert reference() is not None
    del column
    assert reference() is None
    backing.extend(b'x')


def test_raw_chunk_array_keeps_database_alive():
    engine = _engine('RecordEngine')
    expected = engine._origin.buffer().to_bytes()
    reference = weakref.ref(engine._origin)
    array = engine._origin.buffer().as_array(np.uint8)
    engine.close()
    del engine
    assert reference() is not None
    assert array.tobytes() == expected
    del array
    assert reference() is None


def test_stream_copy_input_and_borrowed_array_lifetime():
    stream = core.WxMemoryStream()
    stream.write(b'copied input')
    reference = weakref.ref(stream)
    array = stream.data().as_array(np.uint8)
    del stream
    assert reference() is not None
    assert array.tobytes() == b'copied input'
    del array
    assert reference() is None


def test_shared_mapping_closes_after_last_raw_view():
    import secrets

    engine = _engine('RecordEngine')
    old_table = engine.table(NativeValue)
    engine.share('fdb_lifetime_' + secrets.token_hex(8))
    with pytest.raises(RuntimeError, match='read-only fixed tables'):
        old_table.fill(value=[4, 5, 6])
    mapping = engine._shm._mmap
    column = engine.table(NativeValue).column.value
    engine.unlink()
    del engine
    assert not mapping.closed
    np.testing.assert_array_equal(column, [1.25, 2.5, 3.75])
    del column
    assert mapping.closed


@pytest.mark.skipif(sys.platform == 'win32', reason='close_after releases the publisher on POSIX only')
def test_shared_close_after_publish_releases_mapping(monkeypatch):
    import secrets
    from multiprocessing import shared_memory
    import fastdb4py.record_engine as record_module

    mappings = []
    load_shared = record_module._load_shared_database

    def capture_mapping(shm):
        mappings.append(shm._mmap)
        return load_shared(shm)

    monkeypatch.setattr(record_module, '_load_shared_database', capture_mapping)

    name = 'fdb_close_' + secrets.token_hex(8)
    engine = _engine('RecordEngine')
    engine.share(name, close_after=True)
    assert mappings[0].closed
    engine.close()
    loaded = fdb.RecordEngine.load(name)
    np.testing.assert_array_equal(loaded.table(NativeValue).column.value, [1.25, 2.5, 3.75])
    loaded.close()
    assert mappings[1].closed
    segment = shared_memory.SharedMemory(name=name)
    segment.unlink()
    segment.close()


def test_object_get_row_survives_engine_release():
    engine = _engine('ObjectEngine')
    row = engine.get(NativeValue, 1)
    reference = weakref.ref(engine._db)
    del engine
    assert row.value == 2.5
    assert reference() is not None
    del row
    assert reference() is None


def test_factory_reference_ownership_matches_native_parent():
    standalone = core.WxFeatureRef.make_ref(0, 1)
    assert standalone.thisown
    build = core.WxDatabaseBuild()
    build.begin('')
    layer = build.create_layer_begin('values')
    layer.add_field('value', core.ftF32)
    layer.add_feature_begin()
    layer.set_field(0, 2.5)
    layer.add_feature_end()
    borrowed = layer.create_feature_ref()
    assert not borrowed.thisown
    reference = weakref.ref(build)
    del build, layer
    assert reference() is not None
    assert borrowed.ilayer == 0
    del borrowed
    assert reference() is None


@pytest.mark.parametrize('kind', ['RecordEngine', 'ObjectEngine'])
def test_retained_column_accessor_keeps_native_backing_without_gc(kind):
    engine = _engine(kind)
    native = engine._origin if kind == 'RecordEngine' else engine._db
    reference = weakref.ref(native)
    accessor = engine.table(NativeValue).column
    if kind == 'RecordEngine':
        engine.close()
    del engine, native
    np.testing.assert_array_equal(accessor.value, [1.25, 2.5, 3.75])
    assert reference() is not None
    del accessor
    assert reference() is None


def test_table_retains_engine_without_engine_retaining_table():
    engine = _engine('RecordEngine')
    table = engine.table(NativeValue)
    engine_ref = weakref.ref(engine)
    native_ref = weakref.ref(engine._origin)
    del engine
    assert engine_ref() is not None
    assert table[1].value == 2.5
    del table
    assert engine_ref() is None
    assert native_ref() is None


def test_record_combine_releases_builder_and_push_dispatch_without_gc():
    engine = fdb.RecordEngine.create()
    engine.push(NativeValue(value=1.25))
    builder_ref = weakref.ref(engine._origin)
    engine.combine()
    assert builder_ref() is None
    assert not engine._push_buf
    assert not engine._push_batch_fn
    assert not engine._push_dispatch
    engine.close()


def test_object_combine_releases_layer_and_database_builders_without_gc():
    engine = fdb.ObjectEngine.create()
    state = engine._ensure_layer(NativeValue)
    builder_ref = weakref.ref(engine._db_build)
    layer_ref = weakref.ref(state.build)
    engine.push(NativeValue(value=1.25))
    engine.combine()
    assert state.build is None
    assert builder_ref() is None
    assert layer_ref() is None
    del engine


def test_snapshot_remap_updates_checked_alias_and_invalidates_its_old_views():
    engine = _engine('RecordEngine')
    table = engine.table(NativeValue)
    owner = fdb.FdbViewOwner(checked=True, writeable=True)
    alias = engine.table(NativeValue, owner=owner)
    old_row = alias[0]
    old_column = alias.column.value
    raw = table.column.value
    table.fill(value=[4, 5, 6])
    np.testing.assert_array_equal(alias.column.value.to_numpy(), [4, 5, 6])
    np.testing.assert_array_equal(raw, [1.25, 2.5, 3.75])
    with pytest.raises(fdb.FdbViewInvalidatedError):
        _ = old_row.value
    with pytest.raises(fdb.FdbViewInvalidatedError):
        _ = old_column[0]
    engine.close()


def test_empty_numeric_column():
    engine = fdb.RecordEngine.truncate([fdb.Layout(NativeValue, 0)])
    assert engine.table(NativeValue).column.value.shape == (0,)
    engine.close()


def test_bulk_columns_do_not_request_per_row_native_features(monkeypatch):
    def reject_row_lookup(*args):
        raise AssertionError('bulk columns must not populate the feature cache')

    monkeypatch.setattr(core.WxLayerTable, 'tryGetFeature', reject_row_lookup)
    engine = fdb.RecordEngine.truncate([fdb.Layout(NativeValue, 1_000_000)])
    table = engine.table(NativeValue)
    table.fill(value=np.full(1_000_000, 7, dtype=np.float32))
    assert table.column.value[0] == 7
    assert table.column.value[-1] == 7
    engine.close()


@pytest.mark.parametrize('mode', ['write', 'read'])
def test_million_row_cycles_have_bounded_retained_memory(tmp_path, mode):
    probe = Path(__file__).resolve().parents[2] / 'tools' / 'probe_standalone_memory.py'
    result = subprocess.run(
        [sys.executable, str(probe), '--mode', mode, '--rows', '1000000',
         '--cycles', '10', '--path', str(tmp_path / 'frame.fdb')],
        capture_output=True, text=True, check=True,
    )
    report = json.loads(result.stdout.splitlines()[-1])
    # Allow two allocator warm-up cycles, then at most 16 MiB growth above
    # that baseline across eight cycles (2 MiB/cycle). The old leak retains 16-20 MiB per
    # cycle at this row count, so this tolerates allocator noise while rejecting
    # linear retention by at least an order of magnitude.
    samples = report['retained_bytes'][2:]
    assert max(samples) - report['retained_bytes'][1] < 16 * 1024**2, result.stdout
