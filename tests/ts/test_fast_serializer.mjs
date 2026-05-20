import assert from 'node:assert/strict';
import test from 'node:test';

import {
  F64,
  FastSerializer,
  Feature,
  I32,
  STR,
  U32,
  defineSchema,
  getFastdbModule,
  initFastdb,
  listOf,
  ref,
} from '../../ts/fastdb4ts/dist/index.js';

await initFastdb();

class Point extends Feature {
  static schema = defineSchema({ x: F64, y: F64 });
}

class Line extends Feature {
  static schema = defineSchema({ points: listOf(Point), id: I32 });
}

class RecursiveNode extends Feature {
  static schema = defineSchema({ val: I32, next: ref(() => RecursiveNode) });
}

class TreeNode extends Feature {
  static schema = defineSchema({ val: I32, children: listOf(() => TreeNode) });
}

class User extends Feature {
  static schema = defineSchema({ name: STR, age: I32, scores: listOf(F64) });
}

class MultiListPayload extends Feature {
  static schema = defineSchema({
    ints: listOf(I32),
    names: listOf(STR),
    points: listOf(Point),
  });
}

class StringListOnly extends Feature {
  static schema = defineSchema({ names: listOf(STR) });
}

class NumericColumnarLists extends Feature {
  static schema = defineSchema({ ids: listOf(U32), values: listOf(F64) });
}

test('serializer supports simple objects', () => {
  const point = new Point({ x: 1.0, y: 2.0 });
  const copy = FastSerializer.loads(FastSerializer.dumps(point), Point);
  assert.equal(copy.x, 1.0);
  assert.equal(copy.y, 2.0);
});

test('serializer loads use owned WASM heap loading instead of legacy copied loading', async () => {
  const module = await getFastdbModule();
  assert.equal(typeof module.WxDatabase.loadFromOwnedHeap, 'function');

  const originalLoadFromHeap = module.WxDatabase.loadFromHeap;
  module.WxDatabase.loadFromHeap = () => {
    throw new Error('legacy copied load path should not be used');
  };
  try {
    const copy = FastSerializer.loads(FastSerializer.dumps(new Point({ x: 1.0, y: 2.0 })), Point);
    assert.equal(copy.x, 1.0);
    assert.equal(copy.y, 2.0);
  } finally {
    module.WxDatabase.loadFromHeap = originalLoadFromHeap;
  }
});

test('serializer supports nested feature lists', () => {
  const line = new Line({
    id: 100,
    points: [new Point({ x: 1.0, y: 2.0 }), new Point({ x: 3.0, y: 4.0 })],
  });

  const copy = FastSerializer.loads(FastSerializer.dumps(line), Line);
  assert.equal(copy.id, 100);
  assert.equal(copy.points.length, 2);
  assert.equal(copy.points[0].x, 1.0);
  assert.equal(copy.points[1].y, 4.0);
});

test('serializer supports scalar lists', () => {
  const user = new User({ name: 'Alice', age: 30, scores: [90.5, 80.0, 95.5] });
  const copy = FastSerializer.loads(FastSerializer.dumps(user), User);
  assert.equal(copy.name, 'Alice');
  assert.equal(copy.age, 30);
  assert.equal(copy.scores.length, 3);
  assert.equal(copy.scores[0], 90.5);
});

test('serializer preserves cyclic references', () => {
  const n1 = new RecursiveNode({ val: 1 });
  const n2 = new RecursiveNode({ val: 2 });
  n1.next = n2;
  n2.next = n1;

  const copy = FastSerializer.loads(FastSerializer.dumps(n1), RecursiveNode);
  assert.equal(copy.val, 1);
  assert.equal(copy.next.val, 2);
  assert.equal(copy.next.next, copy);
});

test('serializer preserves tree structures', () => {
  const root = new TreeNode({ val: 0, children: [] });
  const child1 = new TreeNode({ val: 1, children: [] });
  const child2 = new TreeNode({ val: 2, children: [] });
  const subchild = new TreeNode({ val: 3, children: [] });

  child1.children.push(subchild);
  root.children.push(child1, child2);

  const copy = FastSerializer.loads(FastSerializer.dumps(root), TreeNode);
  assert.equal(copy.val, 0);
  assert.equal(copy.children.length, 2);
  assert.equal(copy.children[0].val, 1);
  assert.equal(copy.children[0].children[0].val, 3);
});

test('serializer supports mixed int/string/feature lists', () => {
  const payload = new MultiListPayload({
    ints: [1, 2, 3, 5, 8],
    names: ['alpha', 'beta', '你好', 'emoji🙂'],
    points: [new Point({ x: 10.0, y: 20.0 }), new Point({ x: 30.0, y: 40.0 })],
  });

  const copy = FastSerializer.loads(FastSerializer.dumps(payload), MultiListPayload);
  assert.deepEqual(copy.ints, [1, 2, 3, 5, 8]);
  assert.deepEqual(copy.names, ['alpha', 'beta', '你好', 'emoji🙂']);
  assert.equal(copy.points.length, 2);
  assert.equal(copy.points[0].x, 10.0);
  assert.equal(copy.points[1].y, 40.0);
});

test('serializer supports string list edge cases', () => {
  const longText = 'x'.repeat(10000);
  const payload = new StringListOnly({
    names: ['', 'ascii', '你好', 'emoji🙂', 'line1\nline2', longText],
  });

  const copy = FastSerializer.loads(FastSerializer.dumps(payload), StringListOnly);
  assert.deepEqual(copy.names, ['', 'ascii', '你好', 'emoji🙂', 'line1\nline2', longText]);

  const emptyCopy = FastSerializer.loads(
    FastSerializer.dumps(new StringListOnly({ names: [] })),
    StringListOnly
  );
  assert.deepEqual(emptyCopy.names, []);
});

test('serializer supports numeric columnar list path', () => {
  const payload = new NumericColumnarLists({
    ids: [0, 1, 2, 1024, 65535, 4294967295],
    values: [0.0, 1.5, -3.25, 1e-6, 1e6],
  });

  const copy = FastSerializer.loads(FastSerializer.dumps(payload), NumericColumnarLists);
  assert.deepEqual(copy.ids, [0, 1, 2, 1024, 65535, 4294967295]);
  assert.equal(copy.values.length, 5);
  assert.equal(copy.values[0], 0.0);
  assert.equal(copy.values[1], 1.5);
  assert.equal(copy.values[2], -3.25);
  assert.equal(copy.values[3], 1e-6);
  assert.equal(copy.values[4], 1e6);
});
