"""Cached table projections retain parents without requiring cyclic GC."""
import gc
import weakref

import numpy as np
import pytest

import fastdb4py as fdb


@fdb.feature
class CachedValue:
    value: fdb.F32
    name: fdb.STR


@pytest.fixture(autouse=True)
def without_gc():
    enabled = gc.isenabled()
    gc.disable()
    try:
        yield
    finally:
        if enabled:
            gc.enable()


@pytest.mark.parametrize('checked', [False, True])
def test_accessor_and_columns_are_cached_without_retaining_table(checked):
    engine = fdb.RecordEngine.truncate([fdb.Layout(CachedValue, 3)])
    owner = fdb.FdbViewOwner(checked=checked, writeable=True)
    table = engine.table(CachedValue, owner=owner)
    accessor = table.column
    numeric, strings = accessor.value, accessor.name
    assert table.column is accessor
    assert table.column.value is numeric
    assert table.column.name is strings
    assert weakref.ref(table.column)() is accessor
    table_ref = weakref.ref(table)
    engine_ref = weakref.ref(engine)
    native_ref = weakref.ref(table._db)
    engine.close()
    del engine, table
    assert table_ref() is None
    assert engine_ref() is not None
    assert len(numeric) == len(strings) == 3
    del accessor, numeric, strings
    assert engine_ref() is None
    assert native_ref() is None


def test_table_retains_engine_for_chained_fill_and_releases_immediately():
    table = fdb.RecordEngine.truncate([fdb.Layout(CachedValue, 3)]).table(CachedValue)
    table.fill(value=[1, 2, 3], name=['a', 'b', 'c'])
    np.testing.assert_array_equal(table.column.value, [1, 2, 3])
    assert table.column.name.to_pylist() == ['a', 'b', 'c']
    native_ref = weakref.ref(table._db)
    del table
    assert native_ref() is None


def test_table_strongly_caches_accessor_between_independent_reads():
    engine = fdb.RecordEngine.truncate([fdb.Layout(CachedValue, 3)])
    table = engine.table(CachedValue)
    cached = weakref.ref(table.column)
    assert cached() is not None
    assert table.column is cached()
    column = table.column.value
    assert table.column.value is column
    table_ref = weakref.ref(table)
    del table
    assert table_ref() is None
    assert cached() is None


def test_one_weak_registry_keeps_default_table_identity_and_releases_aliases():
    engine = fdb.RecordEngine.truncate([fdb.Layout(CachedValue, 3)])
    table = engine.table(CachedValue)
    assert engine.table(CachedValue) is table
    owner = fdb.FdbViewOwner(checked=True, writeable=True)
    alias = engine.table(CachedValue, owner=owner)
    assert len(engine._tables) == 2
    table_ref, alias_ref = weakref.ref(table), weakref.ref(alias)
    del table, alias
    assert table_ref() is alias_ref() is None
    assert not engine._tables
    engine_ref = weakref.ref(engine)
    del engine
    assert engine_ref() is None


def test_remap_replaces_cached_columns_and_releases_old_database():
    engine = fdb.RecordEngine.truncate([fdb.Layout(CachedValue, 3)])
    table = engine.table(CachedValue)
    accessor = table.column
    column = accessor.value
    previous = weakref.ref(table._db)
    table.fill(value=[4, 5, 6], name=['x', 'y', 'z'])
    assert table.column is not accessor
    assert table.column.value is table.column.value
    np.testing.assert_array_equal(table.column.value, [4, 5, 6])
    np.testing.assert_array_equal(column, [0, 0, 0])
    assert previous() is not None
    del accessor, column
    assert previous() is None
    current = weakref.ref(table._db)
    del table, engine
    assert current() is None


def test_checked_accessor_cannot_create_columns_on_stale_backing_after_remap():
    engine = fdb.RecordEngine.truncate([fdb.Layout(CachedValue, 3)])
    owner = fdb.FdbViewOwner(checked=True, writeable=True)
    table = engine.table(CachedValue, owner=owner)
    accessor = table.column
    engine.table(CachedValue).fill(value=[1, 2, 3], name=['a', 'b', 'c'])
    for field in ('value', 'name'):
        with pytest.raises(fdb.FdbViewInvalidatedError, match='stale after owner remap'):
            getattr(accessor, field)
    np.testing.assert_array_equal(table.column.value.to_numpy(), [1, 2, 3])
    assert table.column.name.to_pylist() == ['a', 'b', 'c']


def test_retained_string_column_follows_snapshot_without_retaining_table():
    engine = fdb.RecordEngine.truncate([fdb.Layout(CachedValue, 3)])
    table = engine.table(CachedValue)
    column = table.column.name
    table_ref = weakref.ref(table)
    del table
    assert table_ref() is None
    column.fill(['a', 'b', 'c'])
    assert column.to_pylist() == ['a', 'b', 'c']
    engine.table(CachedValue).fill(name=['x', 'y', 'z'])
    assert column.to_pylist() == ['x', 'y', 'z']
    native_ref = weakref.ref(engine._origin)
    del engine
    assert column.to_pylist() == ['x', 'y', 'z']
    del column
    assert native_ref() is None


def test_remap_refreshes_aliases_that_share_a_checked_owner():
    engine = fdb.RecordEngine.truncate([fdb.Layout(CachedValue, 3)])
    owner = fdb.FdbViewOwner(checked=True, writeable=True)
    aliases = [engine.table(CachedValue, owner=owner) for _ in range(2)]
    engine.table(CachedValue).fill(value=[1, 2, 3])
    for table in aliases:
        np.testing.assert_array_equal(table.column.value.to_numpy(), [1, 2, 3])


@pytest.mark.parametrize('method', ['table', 'string', 'utf8'])
def test_engine_writes_after_close_fail_clearly_but_raw_array_writes_work(method):
    engine = fdb.RecordEngine.truncate([fdb.Layout(CachedValue, 3)])
    table = engine.table(CachedValue)
    strings = table.column.name
    array = table.column.value
    engine.close()
    with pytest.raises(RuntimeError, match='RecordEngine is closed'):
        if method == 'table':
            table.fill(value=[1, 2, 3])
        elif method == 'string':
            strings.fill(['a', 'b', 'c'])
        else:
            strings.fill_utf8(*fdb.pack_utf8_column(['a', 'b', 'c']))
    array[:] = [7, 8, 9]
    np.testing.assert_array_equal(table.column.value, [7, 8, 9])
