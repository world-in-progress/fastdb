import numpy as np
import pytest

import fastdb4py as fdb


@fdb.feature
class LifetimePoint:
    row_id: fdb.U32
    x: fdb.F64
    name: fdb.STR


@fdb.feature
class LifetimeBlob:
    data: fdb.BYTES


def _point_table(*, owner=None, writeable=True):
    engine = fdb.ColumnEngine.create()
    engine.push_many([
        LifetimePoint(row_id=1, x=1.5, name='alpha'),
        LifetimePoint(row_id=2, x=2.5, name='beta'),
    ])
    engine.combine()
    return engine, engine.table(LifetimePoint, owner=owner, writeable=writeable)


def _blob_table(*, owner=None, writeable=False):
    engine = fdb.ColumnEngine.create()
    engine.push_many([
        LifetimeBlob(data=b'left'),
        LifetimeBlob(data=b'right'),
    ])
    engine.combine()
    return engine, engine.table(LifetimeBlob, owner=owner, writeable=writeable)


def _object_table(*, owner=None, writeable=False):
    engine = fdb.ObjectEngine.create()
    engine.push(LifetimePoint(row_id=1, x=1.5, name='alpha'))
    engine.combine()
    return engine, engine.table(LifetimePoint, owner=owner, writeable=writeable)


def test_view_owner_invalidation_is_idempotent_and_releases_once():
    released = []
    owner = fdb.FdbViewOwner(checked=True, writeable=True, release=lambda: released.append('released'))

    owner.assert_alive()
    fdb.invalidate(owner)
    fdb.invalidate(owner)

    assert released == ['released']
    with pytest.raises(fdb.FdbViewInvalidatedError):
        owner.assert_alive()


def test_owned_feature_keeps_plain_dict_semantics():
    point = LifetimePoint(row_id=7, x=3.5, name='owned')

    fdb.invalidate(point)
    point.x = 4.5

    assert point.__dict__ == {'row_id': 7, 'x': 4.5, 'name': 'owned'}
    assert point.x == 4.5


def test_checked_table_row_and_columns_raise_after_owner_invalidation():
    owner = fdb.FdbViewOwner(checked=True, writeable=True)
    _engine, table = _point_table(owner=owner, writeable=True)

    row = table[0]
    x_col = table.column.x
    name_col = table.column.name

    assert isinstance(row, LifetimePoint)
    assert row.x == pytest.approx(1.5)
    assert x_col[1] == pytest.approx(2.5)
    assert name_col[0] == 'alpha'

    fdb.invalidate(table)

    with pytest.raises(fdb.FdbViewInvalidatedError):
        len(table)
    with pytest.raises(fdb.FdbViewInvalidatedError):
        _ = row.x
    with pytest.raises(fdb.FdbViewInvalidatedError):
        _ = x_col[0]
    with pytest.raises(fdb.FdbViewInvalidatedError):
        list(x_col)
    with pytest.raises(fdb.FdbViewInvalidatedError):
        _ = name_col[0]


def test_checked_table_view_is_not_rebound_to_later_owner_for_same_engine():
    owner1 = fdb.FdbViewOwner(checked=True, writeable=False)
    engine, table1 = _point_table(owner=owner1, writeable=False)
    row1 = table1[0]
    x_col1 = table1.column.x
    name_col1 = table1.column.name

    owner2 = fdb.FdbViewOwner(checked=True, writeable=False)
    table2 = engine.table(LifetimePoint, owner=owner2, writeable=False)

    assert table1 is not table2
    assert table2[0].name == 'alpha'

    owner1.invalidate()

    with pytest.raises(fdb.FdbViewInvalidatedError):
        len(table1)
    with pytest.raises(fdb.FdbViewInvalidatedError):
        _ = row1.x
    with pytest.raises(fdb.FdbViewInvalidatedError):
        _ = x_col1[0]
    with pytest.raises(fdb.FdbViewInvalidatedError):
        _ = name_col1[0]

    assert len(table2) == 2
    assert table2[1].name == 'beta'


def test_checked_table_view_does_not_poison_default_cached_table():
    owner = fdb.FdbViewOwner(checked=True, writeable=False)
    engine, checked_table = _point_table(owner=owner, writeable=False)
    assert checked_table[0].name == 'alpha'

    owner.invalidate()

    default_table = engine.table(LifetimePoint)

    assert default_table is not checked_table
    assert len(default_table) == 2
    assert default_table[1].name == 'beta'


def test_checked_bytes_column_raises_after_owner_invalidation():
    owner = fdb.FdbViewOwner(checked=True)
    _engine, table = _blob_table(owner=owner)

    data_col = table.column.data
    assert data_col[1] == b'right'

    owner.invalidate()

    with pytest.raises(fdb.FdbViewInvalidatedError):
        _ = data_col[0]


def test_checked_bytes_column_is_not_rebound_to_later_owner_for_same_engine():
    owner1 = fdb.FdbViewOwner(checked=True)
    engine, table1 = _blob_table(owner=owner1)
    data_col1 = table1.column.data

    owner2 = fdb.FdbViewOwner(checked=True)
    table2 = engine.table(LifetimeBlob, owner=owner2)

    assert table1 is not table2
    assert table2.column.data[0] == b'left'

    owner1.invalidate()

    with pytest.raises(fdb.FdbViewInvalidatedError):
        len(table1)
    with pytest.raises(fdb.FdbViewInvalidatedError):
        _ = data_col1[0]

    assert table2.column.data[1] == b'right'


def test_checked_numeric_column_to_numpy_returns_detached_copy():
    owner = fdb.FdbViewOwner(checked=True, writeable=True)
    _engine, table = _point_table(owner=owner, writeable=True)

    x_col = table.column.x
    copied = x_col.to_numpy()

    assert isinstance(copied, np.ndarray)
    np.testing.assert_allclose(copied, np.array([1.5, 2.5], dtype=np.float64))

    x_col[0] = 99.0
    assert copied[0] == pytest.approx(1.5)

    fdb.invalidate(table)
    np.testing.assert_allclose(copied, np.array([1.5, 2.5], dtype=np.float64))
    with pytest.raises(fdb.FdbViewInvalidatedError):
        x_col.to_numpy()


def test_checked_numeric_column_unsafe_numpy_view_is_explicit_escape_hatch():
    owner = fdb.FdbViewOwner(checked=True, writeable=True)
    _engine, table = _point_table(owner=owner, writeable=True)

    x_col = table.column.x
    raw = x_col.unsafe_numpy_view()

    fdb.invalidate(table)

    assert raw[0] == pytest.approx(1.5)
    with pytest.raises(fdb.FdbViewInvalidatedError):
        _ = x_col[0]


def test_materialized_row_and_columns_survive_owner_invalidation():
    owner = fdb.FdbViewOwner(checked=True, writeable=True)
    _engine, table = _point_table(owner=owner, writeable=True)

    row = table[0]
    owned_row = fdb.materialize(row)
    owned_x = fdb.materialize(table.column.x)
    owned_names = fdb.materialize(table.column.name)

    fdb.invalidate(table)

    assert isinstance(owned_row, LifetimePoint)
    assert owned_row.__dict__ == {'row_id': 1, 'x': 1.5, 'name': 'alpha'}
    owned_row.x = 7.5
    assert owned_row.x == pytest.approx(7.5)
    np.testing.assert_allclose(owned_x, np.array([1.5, 2.5], dtype=np.float64))
    assert owned_names == ['alpha', 'beta']
    with pytest.raises(fdb.FdbViewInvalidatedError):
        _ = row.x


def test_invalidate_recurses_through_containers_of_views():
    owner = fdb.FdbViewOwner(checked=True)
    _engine, table = _point_table(owner=owner, writeable=False)
    row = table[0]
    x_col = table.column.x

    fdb.invalidate({'views': [row, (x_col,)]})

    with pytest.raises(fdb.FdbViewInvalidatedError):
        _ = row.x
    with pytest.raises(fdb.FdbViewInvalidatedError):
        _ = x_col[0]


def test_mapped_feature_write_through_requires_writeable_owner():
    owner = fdb.FdbViewOwner(checked=True, writeable=True)
    _engine, table = _point_table(owner=owner, writeable=True)

    row = table[0]
    row.x = 12.5

    assert table[0].x == pytest.approx(12.5)
    assert table.column.x[0] == pytest.approx(12.5)

    fdb.invalidate(table)
    with pytest.raises(fdb.FdbViewInvalidatedError):
        row.x = 8.0


def test_read_only_mapped_feature_and_column_writes_raise():
    owner = fdb.FdbViewOwner(checked=True, writeable=False)
    _engine, table = _point_table(owner=owner, writeable=False)

    row = table[0]
    with pytest.raises(fdb.FdbViewWriteError):
        row.x = 12.5
    with pytest.raises(fdb.FdbViewWriteError):
        table.column.x[0] = 12.5


def test_explicit_read_only_table_without_checked_owner_rejects_writes():
    _engine, table = _point_table(writeable=False)

    row = table[0]
    with pytest.raises(fdb.FdbViewWriteError):
        row.x = 12.5
    with pytest.raises(fdb.FdbViewWriteError):
        table.column.x[0] = 12.5


def test_object_engine_table_accepts_checked_owner():
    owner = fdb.FdbViewOwner(checked=True)
    _engine, table = _object_table(owner=owner)

    row = table[0]
    assert row.name == 'alpha'

    fdb.invalidate(table)
    with pytest.raises(fdb.FdbViewInvalidatedError):
        _ = row.name


def test_iter_reuse_proxy_checks_owner_after_invalidation():
    owner = fdb.FdbViewOwner(checked=True)
    _engine, table = _point_table(owner=owner, writeable=False)

    proxy = next(table.iter_reuse())
    assert proxy.x == pytest.approx(1.5)

    fdb.invalidate(table)
    with pytest.raises(fdb.FdbViewInvalidatedError):
        _ = proxy.x
