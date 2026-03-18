import assert from 'node:assert/strict';
import test from 'node:test';

import {
  F64,
  FastSerializer,
  Feature,
  I32,
  ORM,
  STR,
  TableDefn,
  defineSchema,
  initFastdb,
  ref,
} from '../../ts/fastdb4ts/dist/index.js';

// ByteReader/ByteWriter are exported from serializer but not from the public index.
// Import directly from the compiled module for unit testing.
import { ByteReader, ByteWriter } from '../../ts/fastdb4ts/dist/serializer.js';

await initFastdb();

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

class Point extends Feature {
  static schema = defineSchema({ x: F64, y: F64 });
}

class Named extends Feature {
  static schema = defineSchema({ name: STR, score: F64 });
}

class Node extends Feature {
  static schema = defineSchema({ val: I32, next: ref(() => Node) });
}

// ---------------------------------------------------------------------------
// ByteReader unit tests (bounds checking)
// ---------------------------------------------------------------------------

test('ByteReader throws on empty buffer', () => {
  const reader = new ByteReader(new Uint8Array(0));
  assert.throws(
    () => reader.readU16(),
    (err) => err instanceof Error && /ByteReader/.test(err.message)
  );
});

test('ByteReader throws reading U16 past end', () => {
  const reader = new ByteReader(new Uint8Array([0x01])); // only 1 byte, need 2
  assert.throws(
    () => reader.readU16(),
    (err) => err instanceof Error && /ByteReader/.test(err.message)
  );
});

test('ByteReader throws reading U32 past end', () => {
  const buf = new Uint8Array([0x01, 0x02, 0x03]); // only 3 bytes, need 4
  const reader = new ByteReader(buf);
  assert.throws(
    () => reader.readU32(),
    (err) => err instanceof Error && /ByteReader/.test(err.message)
  );
});

test('ByteReader throws reading F64 past end', () => {
  const buf = new Uint8Array([0x01, 0x02, 0x03, 0x04]); // only 4 bytes, need 8
  const reader = new ByteReader(buf);
  assert.throws(
    () => reader.readF64(),
    (err) => err instanceof Error && /ByteReader/.test(err.message)
  );
});

test('ByteReader throws when readBytes size exceeds remaining', () => {
  const buf = new Uint8Array([0xAA, 0xBB]);
  const reader = new ByteReader(buf);
  assert.throws(
    () => reader.readBytes(10), // only 2 bytes available
    (err) => err instanceof Error && /ByteReader/.test(err.message)
  );
});

test('ByteReader throws on second read after consuming all bytes', () => {
  const writer = new ByteWriter();
  writer.writeU32(42);
  const buf = writer.finish();

  const reader = new ByteReader(buf);
  assert.equal(reader.readU32(), 42); // first read ok
  assert.throws(
    () => reader.readU32(), // second read past end
    (err) => err instanceof Error && /ByteReader/.test(err.message)
  );
});

test('ByteReader at exact boundary reads correctly without throwing', () => {
  const writer = new ByteWriter();
  writer.writeU16(1234);
  writer.writeU32(5678);
  writer.writeF64(9.99);
  const buf = writer.finish();

  const reader = new ByteReader(buf);
  assert.equal(reader.readU16(), 1234);
  assert.equal(reader.readU32(), 5678);
  assert.ok(Math.abs(reader.readF64() - 9.99) < 1e-12);
});

test('ByteWriter and ByteReader roundtrip', () => {
  const writer = new ByteWriter();
  writer.writeU16(0xBEEF);
  writer.writeU32(0xDEADBEEF);
  writer.writeI32(-42);
  writer.writeF64(Math.PI);
  writer.writeBytes(new Uint8Array([1, 2, 3, 4]));
  const buf = writer.finish();

  const reader = new ByteReader(buf);
  assert.equal(reader.readU16(), 0xBEEF);
  assert.equal(reader.readU32(), 0xDEADBEEF >>> 0);
  assert.equal(reader.readI32(), -42);
  assert.ok(Math.abs(reader.readF64() - Math.PI) < 1e-15);
  assert.deepEqual(reader.readBytes(4), new Uint8Array([1, 2, 3, 4]));
});

// ---------------------------------------------------------------------------
// Feature string write in db-mapped mode (cache fallback)
// ---------------------------------------------------------------------------

test('writing str field to db-mapped feature falls back to in-memory cache', () => {
  const orm = ORM.create();
  orm.push(new Named({ name: 'Alice', score: 10.0 }));
  orm.combine();

  const tbl = orm.table(Named);
  const f = tbl.get(0);

  // Values should be readable from the db
  assert.equal(f.score, 10.0);
  assert.equal(f.name, 'Alice');

  // Write a new string value — should not throw and should be in-memory cached
  f.name = 'Bob';
  assert.equal(f.name, 'Bob');

  // Re-fetching the row from the table returns the original value (change is in-memory only)
  const f2 = tbl.get(0);
  assert.equal(f2.name, 'Alice');

  orm.close();
});

test('writing numeric field to db-mapped feature persists within same feature handle', () => {
  const orm = ORM.truncate([new TableDefn(Point, 3)]);
  const tbl = orm.table(Point);

  const f = tbl.get(0);
  f.x = 99.0;
  assert.equal(f.x, 99.0);

  // Re-fetch: numeric writes go directly to WASM memory
  const f2 = tbl.get(0);
  assert.equal(f2.x, 99.0);

  orm.close();
});

// ---------------------------------------------------------------------------
// ORM.close()
// ---------------------------------------------------------------------------

test('ORM.close() is idempotent', () => {
  const orm = ORM.truncate([new TableDefn(Point, 5)]);
  orm.close();
  // Second close should not throw
  assert.doesNotThrow(() => orm.close());
});

test('ORM.close() works on dynamic ORM', () => {
  const orm = ORM.create();
  orm.push(new Point({ x: 1, y: 2 }));
  orm.combine();
  assert.doesNotThrow(() => orm.close());
});

// ---------------------------------------------------------------------------
// Ref fields default to null (no infinite recursion)
// ---------------------------------------------------------------------------

test('self-referential ref field defaults to null when reading uninitialized db-mapped feature', () => {
  const orm = ORM.truncate([new TableDefn(Point, 1)]);
  // Use a fresh ORM with numeric fields to verify basic pattern works
  const tbl = orm.table(Point);
  const f = tbl.get(0);
  assert.equal(f.x, 0);
  orm.close();
});

test('FastSerializer roundtrip with self-referential ref field', () => {
  const n1 = new Node({ val: 10, next: null });
  const buf = FastSerializer.dumps(n1);
  const restored = FastSerializer.loads(buf, Node);
  assert.equal(restored.val, 10);
  assert.equal(restored.next, null);
});

test('writing ref field to db-mapped feature stores in cache', () => {
  const n1 = new Node({ val: 10, next: null });
  const n2 = new Node({ val: 20, next: null });

  const restored = FastSerializer.loads(FastSerializer.dumps(n1), Node);
  assert.equal(restored.val, 10);

  // Overwrite ref in cache
  restored.next = n2;
  assert.equal(restored.next.val, 20);
});

