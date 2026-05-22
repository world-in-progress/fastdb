import numpy as np

import fastdb4py as fdb


@fdb.feature
class MaterializePoint:
    row_id: fdb.U32
    x: fdb.F64
    name: fdb.STR


@fdb.feature
class MaterializeCustomSetattr:
    x: fdb.F64

    def __setattr__(self, name, value):
        object.__setattr__(self, name, value + 1 if name == 'x' else value)


def _table():
    engine = fdb.ColumnEngine.create()
    engine.push_many([
        MaterializePoint(row_id=1, x=1.5, name='alpha'),
        MaterializePoint(row_id=2, x=2.5, name='beta'),
    ])
    engine.combine()
    return engine, engine.table(MaterializePoint)


def _custom_table():
    engine = fdb.ColumnEngine.create()
    obj = MaterializeCustomSetattr()
    obj.__dict__['x'] = 1.5
    engine.push(obj)
    engine.combine()
    return engine, engine.table(MaterializeCustomSetattr)


def test_materialize_table_returns_detached_feature_rows():
    _engine, table = _table()

    rows = fdb.materialize(table)

    assert [row.row_id for row in rows] == [1, 2]
    assert [row.name for row in rows] == ['alpha', 'beta']
    table.column.x[0] = 99.0
    assert rows[0].x == 1.5


def test_table_to_owned_is_materialized_table_alias():
    _engine, table = _table()

    rows = table.to_owned()

    assert [row.row_id for row in rows] == [1, 2]
    assert [row.name for row in rows] == ['alpha', 'beta']


def test_materialize_column_values_copy_underlying_storage():
    _engine, table = _table()

    xs = fdb.materialize(table.column.x)
    names = fdb.materialize(table.column.name)

    assert isinstance(xs, np.ndarray)
    np.testing.assert_array_equal(xs, np.array([1.5, 2.5], dtype=np.float64))
    assert names == ['alpha', 'beta']
    table.column.x[0] = 99.0
    assert xs[0] == 1.5


def test_materialize_recurses_through_tuple_and_lists():
    _engine, table = _table()

    rows, names = fdb.materialize((table, [table.column.name]))

    assert [row.row_id for row in rows] == [1, 2]
    assert names == [['alpha', 'beta']]


def test_materialize_feature_populates_owned_dict_without_reapplying_setattr():
    _engine, table = _custom_table()

    owned = fdb.materialize(table[0])

    assert owned.__dict__ == {'x': 1.5}
    assert owned.x == 1.5
