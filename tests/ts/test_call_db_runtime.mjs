import assert from 'node:assert/strict';
import test from 'node:test';

import {
  BOOL,
  BYTES,
  F64,
  Feature,
  I32,
  listOf,
  ORM,
  ref,
  STR,
  StridedColumn,
  TableDefn,
  WSTR,
  allocateFastdbOwnedBytes,
  decodeFastdbCallDb,
  decodeFastdbFeature,
  defineSchema,
  encodeFastdbCallDb,
  encodeFastdbFeature,
  getFastdbModule,
  initFastdb,
  loadDatabaseFromBytes,
  viewFastdbCallDb,
} from '../../ts/fastdb4ts/dist/index.js';

await initFastdb();

class Point extends Feature {
  static schema = defineSchema({ x: F64, y: F64, name: STR });
}

class NumericPoint extends Feature {
  static schema = defineSchema({ x: F64, y: F64, active: BOOL });
}

class BlobPayload extends Feature {
  static schema = defineSchema({ payload: BYTES, label: STR });
}

class WideBlobPayload extends Feature {
  static schema = defineSchema({ name: WSTR, payload: BYTES });
}

class DoubleBlobPayload extends Feature {
  static schema = defineSchema({ left: BYTES, right: BYTES });
}

class ScalarListPayload extends Feature {
  static schema = defineSchema({ values: listOf(I32), label: STR });
}

class GraphPoint extends Feature {
  static schema = defineSchema({ x: F64, y: F64, name: STR });
}

class GraphNode extends Feature {
  static schema = defineSchema({ value: F64, child: ref(() => GraphPoint) });
}

class GraphCluster extends Feature {
  static schema = defineSchema({ root: ref(() => GraphNode), leaves: listOf(() => GraphPoint) });
}

const pointFeatureBinding = {
  codecId: 'org.fastdb.columnar',
  feature: Point,
  profile: 'columnar.v1',
  schemaSha256: 'test',
};

const blobFeatureBinding = {
  codecId: 'org.fastdb.columnar',
  feature: BlobPayload,
  profile: 'columnar.v1',
  schemaSha256: 'blob-test',
};

test('FastDB feature codec runtime encodes and decodes columnar feature payloads', () => {
  const payload = encodeFastdbFeature(
    pointFeatureBinding,
    new Point({ x: 1.5, y: 2.5, name: 'center' })
  );

  const decoded = decodeFastdbFeature(pointFeatureBinding, payload);

  assert.equal(decoded.x, 1.5);
  assert.equal(decoded.y, 2.5);
  assert.equal(decoded.name, 'center');
  assert.equal(decoded.fixed, false);
});

test('FastDB feature codec runtime encodes and decodes single bytes field payloads', () => {
  const payloadBytes = new Uint8Array([1, 3, 5, 8]);
  const payload = encodeFastdbFeature(
    blobFeatureBinding,
    new BlobPayload({ payload: payloadBytes, label: 'blob' })
  );

  const decoded = decodeFastdbFeature(blobFeatureBinding, payload);

  assert.equal(decoded.label, 'blob');
  assert.equal(decoded.payload instanceof Uint8Array, true);
  assert.deepEqual(Array.from(decoded.payload), [1, 3, 5, 8]);
  assert.equal(decoded.fixed, false);
});

test('FastDB feature codec runtime rejects multiple bytes fields before raw payload aliasing', () => {
  assert.throws(
    () => encodeFastdbFeature(
      {
        codecId: 'org.fastdb.columnar',
        feature: DoubleBlobPayload,
        profile: 'columnar.v1',
        schemaSha256: 'double-blob-test',
      },
      new DoubleBlobPayload({ left: new Uint8Array([1]), right: new Uint8Array([2]) })
    ),
    /multiple bytes fields/
  );
  assert.throws(
    () => encodeFastdbFeature(
      {
        codecId: 'org.fastdb.object-graph',
        feature: DoubleBlobPayload,
        profile: 'object_graph.v1',
        schemaSha256: 'double-blob-test',
      },
      new DoubleBlobPayload({ left: new Uint8Array([1]), right: new Uint8Array([2]) })
    ),
    /multiple bytes fields/
  );
});

test('FastDB feature codec runtime encodes and decodes numeric list fields', () => {
  const binding = {
    codecId: 'org.fastdb.columnar',
    feature: ScalarListPayload,
    profile: 'columnar.v1',
    schemaSha256: 'list-test',
  };

  const payload = encodeFastdbFeature(
    binding,
    new ScalarListPayload({ values: [1, 2, 5], label: 'supported' })
  );
  const decoded = decodeFastdbFeature(binding, payload);

  assert.deepEqual(decoded.values, [1, 2, 5]);
  assert.equal(decoded.label, 'supported');
  assert.equal(decoded.fixed, false);
});

test('FastDB runtime rejects mismatched codec ids before encoding', () => {
  assert.throws(
    () => encodeFastdbFeature(
      { ...pointFeatureBinding, codecId: 'org.fastdb.call-db' },
      new Point({ x: 1, y: 2, name: 'bad' })
    ),
    /expected codec id "org.fastdb.columnar"/
  );

  assert.throws(
    () => encodeFastdbCallDb(
      {
        codecId: 'org.fastdb.columnar',
        direction: 'input',
        method: 'bad',
        profile: 'fastdb.call.columnar.v1',
        schemaSha256: 'test',
        tables: [],
      },
      []
    ),
    /expected codec id "org.fastdb.call-db"/
  );
});

test('FastDB feature codec runtime rejects mismatched object-graph codec ids', () => {
  const binding = {
    ...pointFeatureBinding,
    codecId: 'org.fastdb.columnar',
    profile: 'object_graph.v1',
  };

  assert.throws(
    () => encodeFastdbFeature(binding, new Point({ x: 1, y: 2, name: 'graph' })),
    /expected codec id "org.fastdb.object-graph"/
  );
  assert.throws(
    () => decodeFastdbFeature(binding, new Uint8Array()),
    /expected codec id "org.fastdb.object-graph"/
  );
});

test('FastDB feature codec runtime roundtrips object-graph feature payloads', () => {
  const binding = {
    codecId: 'org.fastdb.object-graph',
    feature: GraphNode,
    profile: 'object_graph.v1',
    schemaSha256: 'graph-node',
  };

  const payload = encodeFastdbFeature(
    binding,
    new GraphNode({
      value: 3.5,
      child: new GraphPoint({ x: 1.0, y: 2.0, name: 'leaf' }),
    })
  );
  const decoded = decodeFastdbFeature(binding, payload);

  assert.equal(decoded.value, 3.5);
  assert.equal(decoded.child.name, 'leaf');
  assert.equal(decoded.child.x, 1.0);
});

test('FastDB call-db runtime roundtrips scalar, array, and batch input tables', () => {
  const binding = {
    codecId: 'org.fastdb.call-db',
    direction: 'input',
    method: 'query',
    profile: 'fastdb.call.columnar.v1',
    schemaSha256: 'test',
    tables: [
      {
        cardinality: 'one',
        fields: [{ kind: 'i32', name: 'level', parameter: 'level', valuePosition: 0 }],
        kind: 'scalars',
        name: '__c2_args',
      },
      {
        cardinality: 'many',
        item: { kind: 'i32', name: 'value' },
        kind: 'array',
        name: 'ids',
        parameter: 'ids',
        valuePosition: 1,
      },
      {
        cardinality: 'many',
        feature: Point,
        featureSchemaSha256: 'point',
        kind: 'feature',
        name: 'points',
        parameter: 'points',
        valuePosition: 2,
      },
    ],
  };

  const payload = encodeFastdbCallDb(binding, [
    3,
    [10, 20],
    [new Point({ x: 1, y: 2, name: 'a' }), new Point({ x: 3, y: 4, name: 'b' })],
  ]);
  const decoded = decodeFastdbCallDb(binding, payload);

  assert.equal(decoded[0], 3);
  assert.deepEqual(decoded[1], [10, 20]);
  assert.equal(decoded[2].length, 2);
  assert.equal(decoded[2][0].x, 1);
  assert.equal(decoded[2][1].name, 'b');
  assert.equal(decoded[2][0].fixed, false);
});

test('FastDB call-db runtime accepts sequence-like array and batch input tables', () => {
  const binding = {
    codecId: 'org.fastdb.call-db',
    direction: 'input',
    method: 'query_sequences',
    profile: 'fastdb.call.columnar.v1',
    schemaSha256: 'sequence-test',
    tables: [
      {
        cardinality: 'one',
        fields: [{ kind: 'i32', name: 'level', parameter: 'level', valuePosition: 0 }],
        kind: 'scalars',
        name: '__c2_args',
      },
      {
        cardinality: 'many',
        item: { kind: 'i32', name: 'value' },
        kind: 'array',
        name: 'ids',
        parameter: 'ids',
        valuePosition: 1,
      },
      {
        cardinality: 'many',
        feature: Point,
        featureSchemaSha256: 'point',
        kind: 'feature',
        name: 'points',
        parameter: 'points',
        valuePosition: 2,
      },
    ],
  };
  function* pointRows() {
    yield new Point({ x: 1, y: 2, name: 'a' });
    yield new Point({ x: 3, y: 4, name: 'b' });
  }

  const payload = encodeFastdbCallDb(binding, [
    3,
    new Int32Array([10, 20]),
    pointRows(),
  ]);
  const decoded = decodeFastdbCallDb(binding, payload);

  assert.equal(decoded[0], 3);
  assert.deepEqual(decoded[1], [10, 20]);
  assert.equal(decoded[2].length, 2);
  assert.equal(decoded[2][0].x, 1);
  assert.equal(decoded[2][1].name, 'b');

  const arrayLikePayload = encodeFastdbCallDb(binding, [
    4,
    { 0: 30, 1: 40, length: 2 },
    new Set([
      new Point({ x: 5, y: 6, name: 'c' }),
      new Point({ x: 7, y: 8, name: 'd' }),
    ]),
  ]);
  const arrayLikeDecoded = decodeFastdbCallDb(binding, arrayLikePayload);

  assert.equal(arrayLikeDecoded[0], 4);
  assert.deepEqual(arrayLikeDecoded[1], [30, 40]);
  assert.equal(arrayLikeDecoded[2].length, 2);
  assert.equal(arrayLikeDecoded[2][0].name, 'c');
  assert.equal(arrayLikeDecoded[2][1].x, 7);
});

test('FastDB call-db runtime rejects ambiguous sequence-like scalar and mapping values', () => {
  const binding = {
    codecId: 'org.fastdb.call-db',
    direction: 'input',
    method: 'query_sequences_guardrail',
    profile: 'fastdb.call.columnar.v1',
    schemaSha256: 'sequence-guardrail-test',
    tables: [
      {
        cardinality: 'one',
        fields: [{ kind: 'i32', name: 'level', parameter: 'level', valuePosition: 0 }],
        kind: 'scalars',
        name: '__c2_args',
      },
      {
        cardinality: 'many',
        item: { kind: 'i32', name: 'value' },
        kind: 'array',
        name: 'ids',
        parameter: 'ids',
        valuePosition: 1,
      },
      {
        cardinality: 'many',
        feature: Point,
        featureSchemaSha256: 'point',
        kind: 'feature',
        name: 'points',
        parameter: 'points',
        valuePosition: 2,
      },
    ],
  };

  assert.throws(
    () => encodeFastdbCallDb(binding, [3, '10,20', []]),
    /expects an array or sequence value, not a string/
  );
  assert.throws(
    () => encodeFastdbCallDb(binding, [3, new Map([['a', 10]]), []]),
    /expects an array or sequence value/
  );
  assert.throws(
    () => encodeFastdbCallDb(binding, [3, { length: 2 }, []]),
    /expects an indexed array-like sequence value/
  );
  assert.throws(
    () => encodeFastdbCallDb(binding, [3, function twoMissingValues(a, b) {}, []]),
    /expects an array or sequence value/
  );
  function iterableFunction() {}
  iterableFunction[Symbol.iterator] = function* values() {
    yield 10;
    yield 20;
  };
  assert.throws(
    () => encodeFastdbCallDb(binding, [3, iterableFunction, []]),
    /expects an array or sequence value/
  );
});

test('FastDB call-db runtime rejects duplicate valuePosition metadata', () => {
  const binding = {
    codecId: 'org.fastdb.call-db',
    direction: 'input',
    method: 'query',
    profile: 'fastdb.call.columnar.v1',
    schemaSha256: 'test',
    tables: [
      {
        cardinality: 'one',
        fields: [{ kind: 'i32', name: 'level', parameter: 'level', valuePosition: 0 }],
        kind: 'scalars',
        name: '__c2_args',
      },
      {
        cardinality: 'many',
        item: { kind: 'i32', name: 'value' },
        kind: 'array',
        name: 'ids',
        parameter: 'ids',
        valuePosition: 0,
      },
    ],
  };

  assert.throws(
    () => encodeFastdbCallDb(binding, [7, [10, 20]]),
    /duplicate valuePosition/
  );
});

test('FastDB call-db runtime rejects non-contiguous valuePosition metadata', () => {
  const binding = {
    codecId: 'org.fastdb.call-db',
    direction: 'input',
    method: 'query',
    profile: 'fastdb.call.columnar.v1',
    schemaSha256: 'test',
    tables: [
      {
        cardinality: 'one',
        fields: [{ kind: 'i32', name: 'level', parameter: 'level', valuePosition: 0 }],
        kind: 'scalars',
        name: '__c2_args',
      },
      {
        cardinality: 'many',
        item: { kind: 'i32', name: 'value' },
        kind: 'array',
        name: 'ids',
        parameter: 'ids',
        valuePosition: 2,
      },
    ],
  };

  assert.throws(
    () => encodeFastdbCallDb(binding, [7, [10, 20]]),
    /contiguous/
  );
});

test('FastDB call-db runtime rejects negative valuePosition metadata', () => {
  const binding = {
    codecId: 'org.fastdb.call-db',
    direction: 'input',
    method: 'query',
    profile: 'fastdb.call.columnar.v1',
    schemaSha256: 'test',
    tables: [
      {
        cardinality: 'one',
        fields: [{ kind: 'i32', name: 'level', parameter: 'level', valuePosition: 0 }],
        kind: 'scalars',
        name: '__c2_args',
      },
      {
        cardinality: 'many',
        item: { kind: 'i32', name: 'value' },
        kind: 'array',
        name: 'ids',
        parameter: 'ids',
        valuePosition: -1,
      },
    ],
  };

  assert.throws(
    () => encodeFastdbCallDb(binding, [7, [10, 20]]),
    /non-negative integer valuePosition/
  );
});

test('FastDB call-db runtime rejects malformed method and direction metadata', () => {
  const binding = {
    codecId: 'org.fastdb.call-db',
    direction: 'input',
    method: 'query',
    profile: 'fastdb.call.columnar.v1',
    schemaSha256: 'test',
    tables: [
      {
        cardinality: 'many',
        item: { kind: 'i32', name: 'value' },
        kind: 'array',
        name: 'ids',
        parameter: 'ids',
        valuePosition: 0,
      },
    ],
  };

  assert.throws(
    () => encodeFastdbCallDb({ ...binding, method: '' }, [[10, 20]]),
    /non-empty method/
  );
  assert.throws(
    () => encodeFastdbCallDb({ ...binding, direction: 'sideways' }, [[10, 20]]),
    /direction/
  );
});

test('FastDB call-db runtime rejects duplicate table names', () => {
  const binding = {
    codecId: 'org.fastdb.call-db',
    direction: 'input',
    method: 'query',
    profile: 'fastdb.call.columnar.v1',
    schemaSha256: 'test',
    tables: [
      {
        cardinality: 'one',
        fields: [{ kind: 'i32', name: 'level', parameter: 'level', valuePosition: 0 }],
        kind: 'scalars',
        name: '__c2_args',
      },
      {
        cardinality: 'many',
        item: { kind: 'i32', name: 'value' },
        kind: 'array',
        name: '__c2_args',
        parameter: 'ids',
        valuePosition: 1,
      },
    ],
  };

  assert.throws(
    () => encodeFastdbCallDb(binding, [7, [10, 20]]),
    /duplicate table name/
  );
});

test('FastDB call-db runtime roundtrips boolean scalar and array tables', () => {
  const binding = {
    codecId: 'org.fastdb.call-db',
    direction: 'input',
    method: 'flags',
    profile: 'fastdb.call.columnar.v1',
    schemaSha256: 'test',
    tables: [
      {
        cardinality: 'one',
        fields: [{ kind: 'bool', name: 'enabled', parameter: 'enabled', valuePosition: 0 }],
        kind: 'scalars',
        name: '__c2_args',
      },
      {
        cardinality: 'many',
        item: { kind: 'bool', name: 'value' },
        kind: 'array',
        name: 'mask',
        parameter: 'mask',
        valuePosition: 1,
      },
    ],
  };

  const payload = encodeFastdbCallDb(binding, ['false', ['true', '0', 1, 0]]);
  const decoded = decodeFastdbCallDb(binding, payload);

  assert.equal(decoded[0], false);
  assert.deepEqual(decoded[1], [true, false, true, false]);
  assert.throws(
    () => encodeFastdbCallDb(binding, ['sometimes', [true]]),
    /fastdb bool scalar/
  );
});

test('FastDB call-db runtime encodes numeric columnar tables without row push fallback', () => {
  const binding = {
    codecId: 'org.fastdb.call-db',
    direction: 'input',
    method: 'numeric_batch',
    profile: 'fastdb.call.columnar.v1',
    schemaSha256: 'test',
    tables: [
      {
        cardinality: 'one',
        fields: [{ kind: 'i32', name: 'level', parameter: 'level', valuePosition: 0 }],
        kind: 'scalars',
        name: '__c2_args',
      },
      {
        cardinality: 'many',
        item: { kind: 'i32', name: 'value' },
        kind: 'array',
        name: 'ids',
        parameter: 'ids',
        valuePosition: 1,
      },
      {
        cardinality: 'many',
        feature: NumericPoint,
        featureSchemaSha256: 'numeric',
        kind: 'feature',
        name: 'points',
        parameter: 'points',
        valuePosition: 2,
      },
    ],
  };
  const points = [
    new NumericPoint({ x: 1.5, y: 2.5, active: true }),
    new NumericPoint({ x: 3.5, y: 4.5, active: false }),
  ];
  const originalPush = ORM.prototype.push;
  ORM.prototype.push = function pushShouldNotRun() {
    throw new Error('row push fallback used');
  };
  try {
    const payload = encodeFastdbCallDb(binding, [7, [10, 20], points]);
    const decoded = decodeFastdbCallDb(binding, payload);

    assert.equal(decoded[0], 7);
    assert.deepEqual(decoded[1], [10, 20]);
    assert.equal(decoded[2].length, 2);
    assert.equal(decoded[2][0].active, true);
    assert.equal(decoded[2][1].y, 4.5);
  } finally {
    ORM.prototype.push = originalPush;
  }
});

test('StridedColumn.fill writes directly through strided memory without per-cell feature setters', () => {
  const orm = ORM.truncate([new TableDefn(NumericPoint, 3)]);
  const table = orm.table(NumericPoint);
  const originalSet = StridedColumn.prototype.set;
  StridedColumn.prototype.set = function setShouldNotRun() {
    throw new Error('per-cell StridedColumn.set fallback used');
  };
  try {
    table.fill({
      active: [1, 0, 1],
      x: new Float64Array([1.25, 2.5, 3.75]),
      y: [4.5, 5.5, 6.5],
    });

    assert.deepEqual(Array.from(table.column.x.toArray()), [1.25, 2.5, 3.75]);
    assert.deepEqual(Array.from(table.column.y.toArray()), [4.5, 5.5, 6.5]);
    assert.equal(table.get(0).active, true);
    assert.equal(table.get(1).active, false);
    assert.equal(table.get(2).active, true);
  } finally {
    StridedColumn.prototype.set = originalSet;
    orm.close();
  }
});

test('FastDB call-db runtime roundtrips large numeric columnar batches without fixed-table detours', () => {
  const rowCount = 2048;
  const binding = {
    codecId: 'org.fastdb.call-db',
    direction: 'input',
    method: 'large_numeric_batch',
    profile: 'fastdb.call.columnar.v1',
    schemaSha256: 'large-numeric',
    tables: [
      {
        cardinality: 'one',
        fields: [{ kind: 'i32', name: 'level', parameter: 'level', valuePosition: 0 }],
        kind: 'scalars',
        name: '__c2_args',
      },
      {
        cardinality: 'many',
        item: { kind: 'i32', name: 'value' },
        kind: 'array',
        name: 'ids',
        parameter: 'ids',
        valuePosition: 1,
      },
      {
        cardinality: 'many',
        feature: NumericPoint,
        featureSchemaSha256: 'numeric',
        kind: 'feature',
        name: 'points',
        parameter: 'points',
        valuePosition: 2,
      },
    ],
  };
  const ids = Array.from({ length: rowCount }, (_, index) => index + 100);
  const points = Array.from(
    { length: rowCount },
    (_, index) => new NumericPoint({
      active: index % 2 === 0,
      x: index + 0.25,
      y: index + 0.75,
    })
  );
  const originalPush = ORM.prototype.push;
  const originalTruncate = ORM.truncate;
  ORM.prototype.push = function pushShouldNotRun() {
    throw new Error('row push fallback used');
  };
  ORM.truncate = function truncateShouldNotRun() {
    throw new Error('fixed-table encode detour used');
  };
  try {
    const payload = encodeFastdbCallDb(binding, [7, ids, points]);
    const decoded = decodeFastdbCallDb(binding, payload);

    assert.equal(decoded[0], 7);
    assert.equal(decoded[1].length, rowCount);
    assert.equal(decoded[1][rowCount - 1], ids[rowCount - 1]);
    assert.equal(decoded[2].length, rowCount);
    assert.equal(decoded[2][0].active, true);
    assert.equal(decoded[2][1].active, false);
    assert.equal(decoded[2][rowCount - 1].y, rowCount - 1 + 0.75);
  } finally {
    ORM.prototype.push = originalPush;
    ORM.truncate = originalTruncate;
  }
});

test('FastDB call-db runtime exposes single feature outputs through retained views', () => {
  const binding = {
    codecId: 'org.fastdb.call-db',
    direction: 'output',
    method: 'nearest',
    profile: 'fastdb.call.columnar.v1',
    schemaSha256: 'test',
    tables: [
      {
        cardinality: 'one',
        feature: NumericPoint,
        featureSchemaSha256: 'numeric',
        kind: 'feature',
        name: 'return_0',
        returnIndex: 0,
        valuePosition: 0,
      },
    ],
  };

  const payload = encodeFastdbCallDb(binding, new NumericPoint({ x: 1.5, y: 2.5, active: true }));
  const decoded = decodeFastdbCallDb(binding, payload);
  const view = viewFastdbCallDb(binding, payload);

  try {
    assert.equal(decoded.fixed, false);
    assert.equal(view.feature('return_0').fixed, true);
    assert.equal(view.feature(0).active, true);
    assert.equal(view.feature(0).x, 1.5);
  } finally {
    view.close();
  }

  assert.throws(() => view.feature(0), /closed/);
});

test('FastDB columnar call-db runtime uses owned WASM heap loading instead of legacy copied loading', async () => {
  const module = await getFastdbModule();
  assert.equal(typeof module.WxDatabase.loadFromOwnedHeap, 'function');

  const originalLoadFromHeap = module.WxDatabase.loadFromHeap;
  module.WxDatabase.loadFromHeap = () => {
    throw new Error('legacy copied load path should not be used');
  };
  try {
    const binding = {
      codecId: 'org.fastdb.call-db',
      direction: 'output',
      method: 'nearest',
      profile: 'fastdb.call.columnar.v1',
      schemaSha256: 'test',
      tables: [
        {
          cardinality: 'one',
          feature: NumericPoint,
          featureSchemaSha256: 'numeric',
          kind: 'feature',
          name: 'return_0',
          returnIndex: 0,
          valuePosition: 0,
        },
      ],
    };

    const payload = encodeFastdbCallDb(binding, new NumericPoint({ x: 1.5, y: 2.5, active: true }));
    const decoded = decodeFastdbCallDb(binding, payload);
    const view = viewFastdbCallDb(binding, payload);
    try {
      assert.equal(decoded.x, 1.5);
      assert.equal(view.feature(0).y, 2.5);
    } finally {
      view.close();
    }
  } finally {
    module.WxDatabase.loadFromHeap = originalLoadFromHeap;
  }
});

test('FastDB columnar call-db runtime accepts transport-owned WASM payloads', async () => {
  const module = await getFastdbModule();
  const binding = {
    codecId: 'org.fastdb.call-db',
    direction: 'output',
    method: 'nearest',
    profile: 'fastdb.call.columnar.v1',
    schemaSha256: 'test',
    tables: [
      {
        cardinality: 'one',
        feature: NumericPoint,
        featureSchemaSha256: 'numeric',
        kind: 'feature',
        name: 'return_0',
        returnIndex: 0,
        valuePosition: 0,
      },
    ],
  };
  const payload = encodeFastdbCallDb(binding, new NumericPoint({ x: 3.5, y: 4.5, active: false }));

  const decodePayload = allocateFastdbOwnedBytes(module, payload.byteLength);
  decodePayload.view.set(payload);
  const decoded = decodeFastdbCallDb(binding, decodePayload);
  assert.equal(decoded.x, 3.5);
  assert.equal(decoded.active, false);
  assert.throws(() => decodePayload.view, /already been transferred/);

  const viewPayload = allocateFastdbOwnedBytes(module, payload.byteLength);
  viewPayload.view.set(payload);
  const view = viewFastdbCallDb(binding, viewPayload);
  try {
    assert.equal(view.feature(0).y, 4.5);
  } finally {
    view.close();
  }
  assert.throws(() => viewPayload.view, /already been transferred/);
});

test('owned heap database loading frees wasm memory if native load throws before ownership transfer', () => {
  const frees = [];
  const heap = new Uint8Array(32);
  const fakeModule = {
    HEAPU8: heap,
    WxDatabase: {
      loadFromOwnedHeap(ptr, size) {
        assert.deepEqual(Array.from(heap.slice(ptr, ptr + size)), [1, 2, 3, 5]);
        throw new Error('native load failed before taking ownership');
      },
    },
    _free(ptr) {
      frees.push(ptr);
    },
    _malloc(size) {
      assert.equal(size, 4);
      return 8;
    },
  };

  assert.throws(
    () => loadDatabaseFromBytes(fakeModule, new Uint8Array([1, 2, 3, 5])),
    /native load failed/
  );
  assert.deepEqual(frees, [8]);
});

test('owned heap database loading consumes caller-populated WASM buffer without JS-to-WASM copy', () => {
  const frees = [];
  const heap = new Uint8Array(64);
  let mallocCalls = 0;
  const dbHandle = { delete() {} };
  const loadCalls = [];
  const fakeModule = {
    HEAPU8: heap,
    WxDatabase: {
      loadFromOwnedHeap(ptr, size) {
        loadCalls.push([ptr, size]);
        assert.deepEqual(Array.from(heap.slice(ptr, ptr + size)), [9, 8, 7, 6]);
        return dbHandle;
      },
    },
    _free(ptr) {
      frees.push(ptr);
    },
    _malloc(size) {
      mallocCalls += 1;
      assert.equal(size, 4);
      return 16;
    },
  };

  const owned = allocateFastdbOwnedBytes(fakeModule, 4);
  assert.equal(mallocCalls, 1);
  owned.view.set([9, 8, 7, 6]);

  fakeModule._malloc = () => {
    throw new Error('owned buffer load must not allocate a second WASM buffer');
  };
  fakeModule.HEAPU8.set = () => {
    throw new Error('owned buffer load must not copy through HEAPU8.set');
  };

  const db = loadDatabaseFromBytes(fakeModule, owned);
  assert.equal(db, dbHandle);
  assert.deepEqual(loadCalls, [[16, 4]]);
  assert.deepEqual(frees, []);
  assert.throws(() => owned.view, /already been transferred/);
  owned.release();
  assert.deepEqual(frees, []);
});

test('owned heap database buffer release frees allocation before transfer', () => {
  const frees = [];
  const fakeModule = {
    HEAPU8: new Uint8Array(32),
    WxDatabase: {
      loadFromOwnedHeap() {
        throw new Error('released buffer must not reach native load');
      },
    },
    _free(ptr) {
      frees.push(ptr);
    },
    _malloc(size) {
      assert.equal(size, 4);
      return 12;
    },
  };

  const owned = allocateFastdbOwnedBytes(fakeModule, 4);
  assert.equal(owned.byteLength, 4);
  assert.equal(owned.dataPtr, 12);
  owned.release();
  owned.release();
  assert.deepEqual(frees, [12]);
  assert.throws(() => owned.view, /already been transferred or released/);
});

test('owned heap database loading frees caller buffer if native load returns null', () => {
  const frees = [];
  const fakeModule = {
    HEAPU8: new Uint8Array(32),
    WxDatabase: {
      loadFromOwnedHeap() {
        return null;
      },
    },
    _free(ptr) {
      frees.push(ptr);
    },
    _malloc(size) {
      assert.equal(size, 4);
      return 20;
    },
  };

  const owned = allocateFastdbOwnedBytes(fakeModule, 4);
  assert.throws(() => loadDatabaseFromBytes(fakeModule, owned), /Failed to load fastdb buffer/);
  assert.deepEqual(frees, [20]);
  owned.release();
  assert.deepEqual(frees, [20]);
});

test('FastDB call-db runtime carries single bytes feature fields through retained views', () => {
  const binding = {
    codecId: 'org.fastdb.call-db',
    direction: 'output',
    method: 'blob',
    profile: 'fastdb.call.columnar.v1',
    schemaSha256: 'blob-call',
    tables: [
      {
        cardinality: 'one',
        feature: BlobPayload,
        featureSchemaSha256: 'blob',
        kind: 'feature',
        name: 'return_0',
        returnIndex: 0,
        valuePosition: 0,
      },
    ],
  };

  const payloadBytes = new Uint8Array([2, 4, 6, 8]);
  const payload = encodeFastdbCallDb(
    binding,
    new BlobPayload({ payload: payloadBytes, label: 'call' })
  );
  const decoded = decodeFastdbCallDb(binding, payload);
  const view = viewFastdbCallDb(binding, payload);

  try {
    assert.equal(decoded.label, 'call');
    assert.deepEqual(Array.from(decoded.payload), [2, 4, 6, 8]);
    assert.deepEqual(Array.from(view.feature(0).payload), [2, 4, 6, 8]);
  } finally {
    view.close();
  }
});

test('FastDB call-db runtime exposes WSTR and bytes batch outputs through retained table columns', () => {
  const binding = {
    codecId: 'org.fastdb.call-db',
    direction: 'output',
    method: 'blobs',
    profile: 'fastdb.call.columnar.v1',
    schemaSha256: 'wide-blob-call',
    tables: [
      {
        cardinality: 'many',
        feature: WideBlobPayload,
        featureSchemaSha256: 'wide-blob',
        kind: 'feature',
        name: 'return_0',
        returnIndex: 0,
        valuePosition: 0,
      },
    ],
  };

  const firstName = '\u7f51\u683c-1';
  const secondName = '\u5c3e-2';
  const payload = encodeFastdbCallDb(binding, [
    new WideBlobPayload({ name: firstName, payload: new Uint8Array([1, 2, 3]) }),
    new WideBlobPayload({ name: secondName, payload: new Uint8Array([9, 8, 7]) }),
  ]);
  const decoded = decodeFastdbCallDb(binding, payload);
  const view = viewFastdbCallDb(binding, payload);

  try {
    assert.equal(decoded.length, 2);
    assert.equal(decoded[0].name, firstName);
    assert.deepEqual(Array.from(decoded[1].payload), [9, 8, 7]);

    const table = view.table('return_0');
    assert.equal(table.length, 2);
    assert.equal(table.get(0).name, firstName);
    assert.deepEqual(Array.from(table.get(1).payload), [9, 8, 7]);
    assert.equal(table.column.name.get(0), firstName);
    assert.deepEqual(table.column.name.toArray(), [firstName, secondName]);
    assert.deepEqual(Array.from(table.column.payload.get(1)), [9, 8, 7]);
    assert.deepEqual(
      table.column.payload.toArray().map((item) => Array.from(item)),
      [[1, 2, 3], [9, 8, 7]]
    );
  } finally {
    view.close();
  }
});

test('FastDB object-graph call-db runtime exposes WSTR and bytes batch outputs through retained rows', () => {
  const binding = {
    codecId: 'org.fastdb.call-db',
    direction: 'output',
    method: 'graph_blobs',
    profile: 'fastdb.call.object-graph.v1',
    schemaSha256: 'wide-blob-graph-call',
    tables: [
      {
        cardinality: 'many',
        feature: WideBlobPayload,
        featureSchemaSha256: 'wide-blob',
        kind: 'feature',
        name: 'return_0',
        returnIndex: 0,
        valuePosition: 0,
      },
    ],
  };

  const firstName = '\u56fe-1';
  const secondName = '\u56fe-2';
  const payload = encodeFastdbCallDb(binding, [
    new WideBlobPayload({ name: firstName, payload: new Uint8Array([4, 5, 6]) }),
    new WideBlobPayload({ name: secondName, payload: new Uint8Array([7, 8, 9]) }),
  ]);
  const decoded = decodeFastdbCallDb(binding, payload);
  const view = viewFastdbCallDb(binding, payload);

  try {
    assert.equal(decoded.length, 2);
    assert.equal(decoded[0].name, firstName);
    assert.deepEqual(Array.from(decoded[1].payload), [7, 8, 9]);

    const table = view.table('return_0');
    assert.equal(table.length, 2);
    assert.equal(table.get(0).name, firstName);
    assert.deepEqual(Array.from(table.get(1).payload), [7, 8, 9]);
    assert.throws(
      () => table.column.name,
      /object-graph call-db views do not expose column access/
    );
  } finally {
    view.close();
  }
});

test('FastDB call-db runtime carries numeric list feature fields through decode and retained rows', () => {
  const binding = {
    codecId: 'org.fastdb.call-db',
    direction: 'output',
    method: 'list_feature',
    profile: 'fastdb.call.columnar.v1',
    schemaSha256: 'list-call',
    tables: [
      {
        cardinality: 'one',
        feature: ScalarListPayload,
        featureSchemaSha256: 'list',
        kind: 'feature',
        name: 'return_0',
        returnIndex: 0,
        valuePosition: 0,
      },
    ],
  };

  const payload = encodeFastdbCallDb(
    binding,
    new ScalarListPayload({ values: [1, 2, 5], label: 'supported' })
  );
  const decoded = decodeFastdbCallDb(binding, payload);
  const view = viewFastdbCallDb(binding, payload);

  try {
    assert.deepEqual(decoded.values, [1, 2, 5]);
    assert.equal(decoded.label, 'supported');
    assert.deepEqual(view.feature(0).values, [1, 2, 5]);
    assert.throws(() => view.table(0).column.values, /list field.*rows/);
  } finally {
    view.close();
  }
});

test('FastDB call-db runtime exposes scalar fields through retained views', () => {
  const binding = {
    codecId: 'org.fastdb.call-db',
    direction: 'output',
    method: 'stats',
    profile: 'fastdb.call.columnar.v1',
    schemaSha256: 'test',
    tables: [
      {
        cardinality: 'one',
        fields: [
          { kind: 'i32', name: 'return_0', valuePosition: 0 },
          { kind: 'bool', name: 'return_1', valuePosition: 1 },
          { kind: 'str', name: 'return_2', valuePosition: 2 },
        ],
        kind: 'scalars',
        name: '__c2_return',
      },
    ],
  };

  const payload = encodeFastdbCallDb(binding, [7, true, 'done']);
  const decoded = decodeFastdbCallDb(binding, payload);
  const view = viewFastdbCallDb(binding, payload);

  try {
    assert.deepEqual(decoded, [7, true, 'done']);
    assert.equal(view.scalar('return_0'), 7);
    assert.equal(view.scalar(1), true);
    assert.equal(view.scalar('return_2'), 'done');
    assert.deepEqual(view.materialize(), [7, true, 'done']);
  } finally {
    view.close();
  }

  assert.throws(() => view.scalar(0), /closed/);
});

test('FastDB call-db runtime rejects hand-built list array table metadata early', () => {
  const binding = {
    codecId: 'org.fastdb.call-db',
    direction: 'input',
    method: 'bad_list_array',
    profile: 'fastdb.call.columnar.v1',
    schemaSha256: 'list-array',
    tables: [
      {
        cardinality: 'many',
        item: { kind: 'list', name: 'value' },
        kind: 'array',
        name: 'values',
        parameter: 'values',
        valuePosition: 0,
      },
    ],
  };

  assert.throws(
    () => encodeFastdbCallDb(binding, [[[1, 2]]]),
    /array table "values" item.*list.*not implemented/
  );
  assert.throws(
    () => decodeFastdbCallDb(binding, new Uint8Array()),
    /array table "values" item.*list.*not implemented/
  );
  assert.throws(
    () => viewFastdbCallDb(binding, new Uint8Array()),
    /array table "values" item.*list.*not implemented/
  );
});

test('FastDB call-db runtime revalidates mutable hand-built binding metadata', () => {
  const binding = {
    codecId: 'org.fastdb.call-db',
    direction: 'input',
    method: 'mutable_array',
    profile: 'fastdb.call.columnar.v1',
    schemaSha256: 'mutable-array',
    tables: [
      {
        cardinality: 'many',
        item: { kind: 'i32', name: 'value' },
        kind: 'array',
        name: 'values',
        parameter: 'values',
        valuePosition: 0,
      },
    ],
  };

  const payload = encodeFastdbCallDb(binding, [[1, 2]]);
  assert.deepEqual(decodeFastdbCallDb(binding, payload)[0], [1, 2]);

  binding.tables[0].item.kind = 'list';
  assert.throws(
    () => encodeFastdbCallDb(binding, [[[1, 2]]]),
    /array table "values" item.*list.*not implemented/
  );
});

test('FastDB call-db runtime rejects malformed table identity metadata', () => {
  const unnamed = {
    codecId: 'org.fastdb.call-db',
    direction: 'input',
    method: 'bad_name',
    profile: 'fastdb.call.columnar.v1',
    schemaSha256: 'bad-name',
    tables: [
      {
        cardinality: 'many',
        item: { kind: 'i32', name: 'value' },
        kind: 'array',
        name: '',
        parameter: 'values',
        valuePosition: 0,
      },
    ],
  };
  assert.throws(
    () => encodeFastdbCallDb(unnamed, [[1, 2]]),
    /non-empty name/
  );

  const wrongCardinality = {
    ...unnamed,
    method: 'bad_cardinality',
    schemaSha256: 'bad-cardinality',
    tables: [
      {
        ...unnamed.tables[0],
        cardinality: 'one',
        name: 'values',
      },
    ],
  };
  assert.throws(
    () => decodeFastdbCallDb(wrongCardinality, new Uint8Array()),
    /array table "values".*cardinality "many"/
  );
});

test('FastDB call-db runtime rejects malformed scalar and array field metadata', () => {
  const duplicateScalar = {
    codecId: 'org.fastdb.call-db',
    direction: 'output',
    method: 'bad_scalar_fields',
    profile: 'fastdb.call.columnar.v1',
    schemaSha256: 'bad-scalar-fields',
    tables: [
      {
        cardinality: 'one',
        fields: [
          { kind: 'i32', name: 'return_0', valuePosition: 0 },
          { kind: 'bool', name: 'return_0', valuePosition: 1 },
        ],
        kind: 'scalars',
        name: '__c2_return',
      },
    ],
  };
  assert.throws(
    () => encodeFastdbCallDb(duplicateScalar, [7, true]),
    /duplicate scalar field name/
  );

  const badScalarKind = {
    codecId: 'org.fastdb.call-db',
    direction: 'output',
    method: 'bad_scalar_kind',
    profile: 'fastdb.call.columnar.v1',
    schemaSha256: 'bad-scalar-kind',
    tables: [
      {
        cardinality: 'one',
        fields: [{ kind: 'mystery', name: 'return_0', valuePosition: 0 }],
        kind: 'scalars',
        name: '__c2_return',
      },
    ],
  };
  assert.throws(
    () => encodeFastdbCallDb(badScalarKind, [7]),
    /Unsupported fastdb call-db scalar kind/
  );
  assert.throws(
    () => decodeFastdbCallDb(badScalarKind, new Uint8Array()),
    /Unsupported fastdb call-db scalar kind/
  );
  assert.throws(
    () => viewFastdbCallDb(badScalarKind, new Uint8Array()),
    /Unsupported fastdb call-db scalar kind/
  );

  const badArrayItemName = {
    codecId: 'org.fastdb.call-db',
    direction: 'input',
    method: 'bad_array_item',
    profile: 'fastdb.call.columnar.v1',
    schemaSha256: 'bad-array-item',
    tables: [
      {
        cardinality: 'many',
        item: { kind: 'i32', name: 'item' },
        kind: 'array',
        name: 'ids',
        parameter: 'ids',
        valuePosition: 0,
      },
    ],
  };
  assert.throws(
    () => encodeFastdbCallDb(badArrayItemName, [[1, 2]]),
    /array table "ids" item name/
  );

  const badArrayItemKind = {
    ...badArrayItemName,
    method: 'bad_array_item_kind',
    schemaSha256: 'bad-array-item-kind',
    tables: [
      {
        ...badArrayItemName.tables[0],
        item: { kind: 'mystery', name: 'value' },
      },
    ],
  };
  assert.throws(
    () => encodeFastdbCallDb(badArrayItemKind, [[1, 2]]),
    /Unsupported fastdb call-db scalar kind/
  );
  assert.throws(
    () => decodeFastdbCallDb(badArrayItemKind, new Uint8Array()),
    /Unsupported fastdb call-db scalar kind/
  );
  assert.throws(
    () => viewFastdbCallDb(badArrayItemKind, new Uint8Array()),
    /Unsupported fastdb call-db scalar kind/
  );
});

test('FastDB call-db runtime rejects malformed feature schema metadata', () => {
  const missingFeatureHash = {
    codecId: 'org.fastdb.call-db',
    direction: 'output',
    method: 'bad_feature_schema',
    profile: 'fastdb.call.columnar.v1',
    schemaSha256: 'bad-feature-schema',
    tables: [
      {
        cardinality: 'many',
        feature: NumericPoint,
        featureSchemaSha256: '',
        kind: 'feature',
        name: 'return_0',
        returnIndex: 0,
        valuePosition: 0,
      },
    ],
  };
  assert.throws(
    () => encodeFastdbCallDb(missingFeatureHash, [new NumericPoint({ x: 1, y: 2, active: true })]),
    /feature schema hash/
  );
  assert.throws(
    () => decodeFastdbCallDb(missingFeatureHash, new Uint8Array()),
    /feature schema hash/
  );
  assert.throws(
    () => viewFastdbCallDb(missingFeatureHash, new Uint8Array()),
    /feature schema hash/
  );

  const unexpectedColumnarDependency = {
    ...missingFeatureHash,
    schemaSha256: 'bad-columnar-dependency',
    tables: [
      {
        ...missingFeatureHash.tables[0],
        featureDependencies: [
          { feature: Point, featureSchemaSha256: 'point' },
        ],
        featureSchemaSha256: 'numeric',
      },
    ],
  };
  assert.throws(
    () => encodeFastdbCallDb(unexpectedColumnarDependency, [new NumericPoint({ x: 1, y: 2, active: true })]),
    /dependencies are only valid for object-graph/
  );
  assert.throws(
    () => decodeFastdbCallDb(unexpectedColumnarDependency, new Uint8Array()),
    /dependencies are only valid for object-graph/
  );
  assert.throws(
    () => viewFastdbCallDb(unexpectedColumnarDependency, new Uint8Array()),
    /dependencies are only valid for object-graph/
  );

  const missingDependencyHash = {
    codecId: 'org.fastdb.call-db',
    direction: 'output',
    method: 'bad_dependency_schema',
    profile: 'fastdb.call.object-graph.v1',
    schemaSha256: 'bad-dependency-schema',
    tables: [
      {
        cardinality: 'one',
        feature: GraphCluster,
        featureDependencies: [
          { feature: GraphNode, featureSchemaSha256: '' },
          { feature: GraphPoint, featureSchemaSha256: 'point' },
        ],
        featureSchemaSha256: 'cluster',
        kind: 'feature',
        name: 'return_0',
        returnIndex: 0,
        valuePosition: 0,
      },
    ],
  };
  assert.throws(
    () => encodeFastdbCallDb(missingDependencyHash, [
      new GraphCluster({
        root: new GraphNode({ value: 1, child: new GraphPoint({ x: 1, y: 2, name: 'leaf' }) }),
        leaves: [],
      }),
    ]),
    /dependency schema hash/
  );
  assert.throws(
    () => decodeFastdbCallDb(missingDependencyHash, new Uint8Array()),
    /dependency schema hash/
  );
  assert.throws(
    () => viewFastdbCallDb(missingDependencyHash, new Uint8Array()),
    /dependency schema hash/
  );
});

test('FastDB call-db runtime rejects unsupported hand-built profile metadata', () => {
  const binding = {
    codecId: 'org.fastdb.call-db',
    direction: 'input',
    method: 'bad_profile',
    profile: 'fastdb.call.unknown.v1',
    schemaSha256: 'bad-profile',
    tables: [
      {
        cardinality: 'many',
        item: { kind: 'i32', name: 'value' },
        kind: 'array',
        name: 'values',
        parameter: 'values',
        valuePosition: 0,
      },
    ],
  };

  assert.throws(
    () => encodeFastdbCallDb(binding, [[1, 2]]),
    /Unsupported fastdb call-db call-db profile/
  );
  assert.throws(
    () => decodeFastdbCallDb(binding, new Uint8Array()),
    /Unsupported fastdb call-db call-db profile/
  );
  assert.throws(
    () => viewFastdbCallDb(binding, new Uint8Array()),
    /Unsupported fastdb call-db call-db profile/
  );
});

test('FastDB call-db runtime exposes retained columnar views with explicit close', () => {
  const binding = {
    codecId: 'org.fastdb.call-db',
    direction: 'output',
    method: 'view_batch',
    profile: 'fastdb.call.columnar.v1',
    schemaSha256: 'test',
    tables: [
      {
        cardinality: 'many',
        item: { kind: 'i32', name: 'value' },
        kind: 'array',
        name: 'return_0',
        returnIndex: 0,
        valuePosition: 0,
      },
      {
        cardinality: 'many',
        feature: NumericPoint,
        featureSchemaSha256: 'numeric',
        kind: 'feature',
        name: 'return_1',
        returnIndex: 1,
        valuePosition: 1,
      },
    ],
  };
  const points = [
    new NumericPoint({ x: 1.5, y: 2.5, active: true }),
    new NumericPoint({ x: 3.5, y: 4.5, active: false }),
  ];
  const payload = encodeFastdbCallDb(binding, [[10, 20], points]);
  const view = viewFastdbCallDb(binding, payload);
  const ids = view.array('return_0');
  const table = view.table('return_1');
  const xColumn = table.column.x;

  try {
    assert.equal(ids.length, 2);
    assert.deepEqual(ids.toArray(), [10, 20]);
    assert.equal(ids.get(1), 20);
    assert.equal(table.length, 2);
    assert.equal(table.get(0).active, true);
    assert.equal(table.get(0).fixed, true);
    assert.equal(xColumn.get(1), 3.5);
    assert.equal(xColumn.length, 2);
    assert.equal(view.materialize()[1][1].y, 4.5);
    assert.throws(() => view.feature('return_1'), /use table/);
  } finally {
    view.close();
  }

  assert.throws(() => ids.get(0), /closed/);
  assert.throws(() => table.length, /closed/);
  assert.throws(() => xColumn.get(0), /closed/);
});

test('FastDB call-db runtime preserves empty array and batch tables', () => {
  const binding = {
    codecId: 'org.fastdb.call-db',
    direction: 'output',
    method: 'empty',
    profile: 'fastdb.call.columnar.v1',
    schemaSha256: 'test',
    tables: [
      {
        cardinality: 'many',
        item: { kind: 'i32', name: 'value' },
        kind: 'array',
        name: 'return_0',
        returnIndex: 0,
        valuePosition: 0,
      },
    ],
  };

  const payload = encodeFastdbCallDb(binding, []);
  const decoded = decodeFastdbCallDb(binding, payload);

  assert.deepEqual(decoded, []);
});

test('FastDB call-db runtime roundtrips object-graph call-db feature outputs', () => {
  const binding = {
    codecId: 'org.fastdb.call-db',
    direction: 'output',
    method: 'graph',
    profile: 'fastdb.call.object-graph.v1',
    schemaSha256: 'graph-call',
    tables: [
      {
        cardinality: 'one',
        feature: GraphCluster,
        featureDependencies: [
          { feature: GraphNode, featureSchemaSha256: 'node' },
          { feature: GraphPoint, featureSchemaSha256: 'point' },
        ],
        featureSchemaSha256: 'cluster',
        kind: 'feature',
        name: 'return_0',
        returnIndex: 0,
        valuePosition: 0,
      },
    ],
  };

  const leaf = new GraphPoint({ x: 4.0, y: 5.0, name: 'list-leaf' });
  const payload = encodeFastdbCallDb(
    binding,
    new GraphCluster({
      root: new GraphNode({ value: 6.0, child: leaf }),
      leaves: [leaf],
    })
  );
  const decoded = decodeFastdbCallDb(binding, payload);

  assert.equal(decoded.root.value, 6.0);
  assert.equal(decoded.root.child.name, 'list-leaf');
  assert.equal(decoded.leaves.length, 1);
  assert.equal(decoded.leaves[0].x, 4.0);
  assert.equal(decoded.root.child, decoded.leaves[0]);
  const view = viewFastdbCallDb(binding, payload);
  try {
    const held = view.feature('return_0');
    assert.equal(held.root.value, 6.0);
    assert.equal(held.root.child.name, 'list-leaf');
    assert.equal(held.root.child, held.leaves[0]);
    assert.equal(view.materialize().root.child.name, 'list-leaf');
  } finally {
    view.close();
  }
  assert.throws(() => view.feature('return_0'), /closed/);
});

test('FastDB object-graph call-db runtime uses owned WASM heap loading instead of legacy copied loading', async () => {
  const module = await getFastdbModule();
  assert.equal(typeof module.WxDatabase.loadFromOwnedHeap, 'function');

  const originalLoadFromHeap = module.WxDatabase.loadFromHeap;
  module.WxDatabase.loadFromHeap = () => {
    throw new Error('legacy copied load path should not be used');
  };
  try {
    const binding = {
      codecId: 'org.fastdb.call-db',
      direction: 'output',
      method: 'graph',
      profile: 'fastdb.call.object-graph.v1',
      schemaSha256: 'graph-call',
      tables: [
        {
          cardinality: 'one',
          feature: GraphCluster,
          featureDependencies: [
            { feature: GraphNode, featureSchemaSha256: 'node' },
            { feature: GraphPoint, featureSchemaSha256: 'point' },
          ],
          featureSchemaSha256: 'cluster',
          kind: 'feature',
          name: 'return_0',
          returnIndex: 0,
          valuePosition: 0,
        },
      ],
    };
    const leaf = new GraphPoint({ x: 4.0, y: 5.0, name: 'owned-load-leaf' });
    const payload = encodeFastdbCallDb(
      binding,
      new GraphCluster({
        root: new GraphNode({ value: 6.0, child: leaf }),
        leaves: [leaf],
      })
    );
    const decoded = decodeFastdbCallDb(binding, payload);
    const view = viewFastdbCallDb(binding, payload);
    try {
      assert.equal(decoded.root.child.name, 'owned-load-leaf');
      assert.equal(view.feature('return_0').root.child.name, 'owned-load-leaf');
    } finally {
      view.close();
    }
  } finally {
    module.WxDatabase.loadFromHeap = originalLoadFromHeap;
  }
});

test('FastDB call-db runtime roundtrips object-graph call-db scalar and array tables', () => {
  const binding = {
    codecId: 'org.fastdb.call-db',
    direction: 'output',
    method: 'graph_summary',
    profile: 'fastdb.call.object-graph.v1',
    schemaSha256: 'graph-summary-call',
    tables: [
      {
        cardinality: 'one',
        fields: [
          { kind: 'i32', name: 'return_0', valuePosition: 0 },
          { kind: 'bool', name: 'return_1', valuePosition: 1 },
        ],
        kind: 'scalars',
        name: '__c2_return',
      },
      {
        cardinality: 'many',
        item: { kind: 'i32', name: 'value' },
        kind: 'array',
        name: 'return_2',
        returnIndex: 2,
        valuePosition: 2,
      },
      {
        cardinality: 'one',
        feature: GraphCluster,
        featureDependencies: [
          { feature: GraphNode, featureSchemaSha256: 'node' },
          { feature: GraphPoint, featureSchemaSha256: 'point' },
        ],
        featureSchemaSha256: 'cluster',
        kind: 'feature',
        name: 'return_3',
        returnIndex: 3,
        valuePosition: 3,
      },
    ],
  };

  const leaf = new GraphPoint({ x: 3.0, y: 4.0, name: 'array-leaf' });
  const payload = encodeFastdbCallDb(
    binding,
    [
      7,
      'false',
      [10, 11, 12],
      new GraphCluster({
        root: new GraphNode({ value: 8.0, child: leaf }),
        leaves: [leaf],
      }),
    ]
  );
  const decoded = decodeFastdbCallDb(binding, payload);

  assert.equal(decoded[0], 7);
  assert.equal(decoded[1], false);
  assert.deepEqual(decoded[2], [10, 11, 12]);
  assert.equal(decoded[3].root.child.name, 'array-leaf');
  assert.equal(decoded[3].root.child, decoded[3].leaves[0]);
  const view = viewFastdbCallDb(binding, payload);
  try {
    assert.equal(view.scalar('return_0'), 7);
    assert.equal(view.scalar('return_1'), false);
    assert.deepEqual(view.array('return_2').toArray(), [10, 11, 12]);
    assert.equal(view.array(0).get(1), 11);
    assert.equal(view.feature('return_3').root.child.name, 'array-leaf');
    assert.equal(view.table('return_3').get(0).root.child.name, 'array-leaf');
    assert.throws(() => view.table('return_3').column.value, /object-graph call-db views do not expose column access/);
    assert.equal(view.materialize()[3].root.child.name, 'array-leaf');
  } finally {
    view.close();
  }
  assert.throws(() => view.array('return_2').get(0), /closed/);
  assert.throws(
    () => encodeFastdbCallDb(binding, [
      7,
      'sometimes',
      [10],
      new GraphCluster({
        root: new GraphNode({ value: 8.0, child: leaf }),
        leaves: [leaf],
      }),
    ]),
    /fastdb bool scalar/
  );
});

test('FastDB call-db runtime accepts object-graph array input iterables', () => {
  const binding = {
    codecId: 'org.fastdb.call-db',
    direction: 'input',
    method: 'graph_query_sequences',
    profile: 'fastdb.call.object-graph.v1',
    schemaSha256: 'graph-query-sequences-call',
    tables: [
      {
        cardinality: 'many',
        item: { kind: 'i32', name: 'value' },
        kind: 'array',
        name: 'ids',
        parameter: 'ids',
        valuePosition: 0,
      },
    ],
  };
  function* ids() {
    yield 10;
    yield 11;
    yield 12;
  }

  const payload = encodeFastdbCallDb(binding, [ids()]);
  const decoded = decodeFastdbCallDb(binding, payload);

  assert.deepEqual(decoded, [[10, 11, 12]]);
});

test('FastDB call-db runtime preserves empty object-graph array and feature tables', () => {
  const binding = {
    codecId: 'org.fastdb.call-db',
    direction: 'input',
    method: 'empty_graph_query',
    profile: 'fastdb.call.object-graph.v1',
    schemaSha256: 'empty-graph-query-call',
    tables: [
      {
        cardinality: 'many',
        item: { kind: 'i32', name: 'value' },
        kind: 'array',
        name: 'ids',
        parameter: 'ids',
        valuePosition: 0,
      },
      {
        cardinality: 'many',
        feature: GraphNode,
        featureDependencies: [
          { feature: GraphPoint, featureSchemaSha256: 'point' },
        ],
        featureSchemaSha256: 'node',
        kind: 'feature',
        name: 'nodes',
        parameter: 'nodes',
        valuePosition: 1,
      },
    ],
  };

  const payload = encodeFastdbCallDb(binding, [[], []]);
  const decoded = decodeFastdbCallDb(binding, payload);

  assert.deepEqual(decoded, [[], []]);
});

test('FastDB call-db runtime rejects object-graph layer name collisions', () => {
  const binding = {
    codecId: 'org.fastdb.call-db',
    direction: 'output',
    method: 'graph_collision',
    profile: 'fastdb.call.object-graph.v1',
    schemaSha256: 'graph-collision',
    tables: [
      {
        cardinality: 'one',
        fields: [
          { kind: 'i32', name: 'return_0', valuePosition: 0 },
        ],
        kind: 'scalars',
        name: 'GraphCluster',
      },
      {
        cardinality: 'one',
        feature: GraphCluster,
        featureDependencies: [
          { feature: GraphNode, featureSchemaSha256: 'node' },
          { feature: GraphPoint, featureSchemaSha256: 'point' },
        ],
        featureSchemaSha256: 'cluster',
        kind: 'feature',
        name: 'return_1',
        returnIndex: 1,
        valuePosition: 1,
      },
    ],
  };

  assert.throws(
    () => encodeFastdbCallDb(binding, [
      1,
      new GraphCluster({
        root: new GraphNode({ value: 2.0, child: new GraphPoint({ x: 1.0, y: 1.0, name: 'leaf' }) }),
        leaves: [],
      }),
    ]),
    /cannot encode both.*GraphCluster/
  );
});

test('FastDB call-db runtime rejects object-graph dependency layer name collisions', () => {
  const binding = {
    codecId: 'org.fastdb.call-db',
    direction: 'output',
    method: 'graph_collision_dependency',
    profile: 'fastdb.call.object-graph.v1',
    schemaSha256: 'graph-collision-dependency',
    tables: [
      {
        cardinality: 'many',
        item: { kind: 'i32', name: 'value' },
        kind: 'array',
        name: 'GraphPoint',
        returnIndex: 0,
        valuePosition: 0,
      },
      {
        cardinality: 'one',
        feature: GraphCluster,
        featureDependencies: [
          { feature: GraphNode, featureSchemaSha256: 'node' },
          { feature: GraphPoint, featureSchemaSha256: 'point' },
        ],
        featureSchemaSha256: 'cluster',
        kind: 'feature',
        name: 'return_1',
        returnIndex: 1,
        valuePosition: 1,
      },
    ],
  };
  const leaf = new GraphPoint({ x: 1, y: 2, name: 'dependency' });

  assert.throws(
    () => encodeFastdbCallDb(binding, [
      [1, 2],
      new GraphCluster({
        root: new GraphNode({ value: 1, child: leaf }),
        leaves: [leaf],
      }),
    ]),
    /cannot encode both.*GraphPoint/
  );
});

test('FastDB call-db runtime rejects duplicate object-graph feature-table layers', () => {
  const binding = {
    codecId: 'org.fastdb.call-db',
    direction: 'output',
    method: 'graph_duplicate_feature',
    profile: 'fastdb.call.object-graph.v1',
    schemaSha256: 'graph-duplicate-feature',
    tables: [
      {
        cardinality: 'one',
        feature: GraphCluster,
        featureDependencies: [
          { feature: GraphNode, featureSchemaSha256: 'node' },
          { feature: GraphPoint, featureSchemaSha256: 'point' },
        ],
        featureSchemaSha256: 'cluster',
        kind: 'feature',
        name: 'return_0',
        returnIndex: 0,
        valuePosition: 0,
      },
      {
        cardinality: 'one',
        feature: GraphCluster,
        featureDependencies: [
          { feature: GraphNode, featureSchemaSha256: 'node' },
          { feature: GraphPoint, featureSchemaSha256: 'point' },
        ],
        featureSchemaSha256: 'cluster',
        kind: 'feature',
        name: 'return_1',
        returnIndex: 1,
        valuePosition: 1,
      },
    ],
  };

  assert.throws(
    () => encodeFastdbCallDb(binding, [
      new GraphCluster({ root: null, leaves: [] }),
      new GraphCluster({ root: null, leaves: [] }),
    ]),
    /cannot encode both.*GraphCluster/
  );
});
