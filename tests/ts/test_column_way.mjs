import assert from 'node:assert/strict';
import test from 'node:test';

import {
  F64,
  Feature,
  ORM,
  TableDefn,
  defineSchema,
  initFastdb,
} from '../../ts/fastdb4ts/dist/index.js';

await initFastdb();

class Point extends Feature {
  static schema = defineSchema({ x: F64, y: F64, z: F64 });
}

test('column way supports row writes and column updates', () => {
  const db = ORM.truncate([
    new TableDefn(Point, 10),
    new TableDefn(Point, 5, 'PointA'),
  ]);

  const table = db.table(Point, 'PointA');

  for (let i = 0; i < 5; i += 1) {
    const point = table.get(i);
    point.x = i * 1.0;
    point.y = i * 2.0;
    point.z = i * 3.0;

    assert.equal(point.x, i * 1.0);
    assert.equal(point.y, i * 2.0);
    assert.equal(point.z, i * 3.0);
  }

  const xs = table.column.x;
  for (let i = 0; i < table.length; i += 1) {
    xs.set(i, xs.get(i) + 1);
  }

  for (let i = 0; i < 5; i += 1) {
    const point = table.get(i);
    assert.equal(point.x, i * 1.0 + 1);
    assert.equal(point.y, i * 2.0);
    assert.equal(point.z, i * 3.0);
  }
});

test('orm buffer roundtrip preserves fixed table data', () => {
  const db = ORM.truncate([new TableDefn(Point, 3)]);
  const table = db.table(Point);

  table.column.x.fill([1.5, 2.5, 3.5]);
  table.column.y.fill([4.5, 5.5, 6.5]);
  table.column.z.fill([7.5, 8.5, 9.5]);

  const copy = ORM.fromBuffer(db.toBuffer());
  const copyTable = copy.table(Point);

  assert.equal(copyTable.get(1).x, 2.5);
  assert.equal(copyTable.get(1).y, 5.5);
  assert.equal(copyTable.get(2).z, 9.5);
});

test('orm buffer roundtrip preserves multi-layer fixed table data after column fill', () => {
  const rowCount = 16;
  const db = ORM.truncate([
    new TableDefn(Point, rowCount),
    new TableDefn(Point, rowCount, 'PointA'),
  ]);
  const table = db.table(Point, 'PointA');

  table.column.x.fill(Array.from({ length: rowCount }, (_, index) => index + 0.5));
  table.column.y.fill(Array.from({ length: rowCount }, (_, index) => index + 1.5));
  table.column.z.fill(Array.from({ length: rowCount }, (_, index) => index + 2.5));

  const copy = ORM.fromBuffer(db.toBuffer());
  try {
    const copyTable = copy.table(Point, 'PointA');
    assert.equal(copyTable.length, rowCount);
    assert.equal(copyTable.get(rowCount - 1).x, rowCount - 1 + 0.5);
    assert.equal(copyTable.get(rowCount - 1).y, rowCount - 1 + 1.5);
    assert.equal(copyTable.get(rowCount - 1).z, rowCount - 1 + 2.5);
  } finally {
    copy.close();
    db.close();
  }
});
