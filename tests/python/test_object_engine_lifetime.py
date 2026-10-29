"""Reference-counted lifetime of standalone ObjectEngine views."""

import weakref

import numpy as np

import fastdb4py as fdb


@fdb.feature
class ObjectLifetimeValue:
    value: fdb.F32


def test_raw_column_retains_native_database_after_engine_is_released():
    engine = fdb.ObjectEngine.create()
    engine.push(ObjectLifetimeValue(value=1.25))
    engine.push(ObjectLifetimeValue(value=2.5))
    engine.combine()

    assert engine._db_build is None
    assert all(state.build is None for state in engine._layers.values())

    table = engine.table(ObjectLifetimeValue)
    column = table.column.value
    native_ref = weakref.ref(table._db)
    engine_ref = weakref.ref(engine)
    del table, engine

    assert engine_ref() is None
    assert native_ref() is not None
    np.testing.assert_array_equal(column, [1.25, 2.5])
    column[1] = 3.75
    assert column[1] == 3.75

    del column
    assert native_ref() is None
