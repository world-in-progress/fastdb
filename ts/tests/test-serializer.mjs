import {
  F64,
  FastSerializer,
  Feature,
  I32,
  STR,
  U32,
  defineSchema,
  initFastdb,
  listOf,
  ref,
} from '../fastdb4ts/dist/index.js';

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

class NumericColumnarLists extends Feature {
  static schema = defineSchema({ ids: listOf(U32), values: listOf(F64) });
}

const p1 = new Point({ x: 1.0, y: 2.0 });
const p2 = new Point({ x: 3.0, y: 4.0 });
const line = new Line({ id: 100, points: [p1, p2] });
const line2 = FastSerializer.loads(FastSerializer.dumps(line), Line);
if (line2.id !== 100 || line2.points.length !== 2 || line2.points[1].y !== 4.0) {
  throw new Error('Nested feature list roundtrip failed.');
}

const n1 = new RecursiveNode({ val: 1 });
const n2 = new RecursiveNode({ val: 2 });
n1.next = n2;
n2.next = n1;
const restoredCycle = FastSerializer.loads(FastSerializer.dumps(n1), RecursiveNode);
if (restoredCycle.next.next !== restoredCycle) {
  throw new Error('Cycle identity was not preserved.');
}

const root = new TreeNode({ val: 0, children: [] });
root.children.push(new TreeNode({ val: 1, children: [new TreeNode({ val: 3, children: [] })] }));
root.children.push(new TreeNode({ val: 2, children: [] }));
const restoredTree = FastSerializer.loads(FastSerializer.dumps(root), TreeNode);
if (restoredTree.children[0].children[0].val !== 3) {
  throw new Error('Tree structure roundtrip failed.');
}

const user = new User({ name: 'Alice', age: 30, scores: [90.5, 80.0, 95.5] });
const user2 = FastSerializer.loads(FastSerializer.dumps(user), User);
if (user2.name !== 'Alice' || user2.scores[2] !== 95.5) {
  throw new Error('Scalar/numeric-list roundtrip failed.');
}

const numeric = new NumericColumnarLists({
  ids: [0, 1, 2, 1024, 65535, 4294967295],
  values: [0.0, 1.5, -3.25, 1e-6, 1e6],
});
const numeric2 = FastSerializer.loads(FastSerializer.dumps(numeric), NumericColumnarLists);
if (numeric2.ids[5] !== 4294967295 || numeric2.values[2] !== -3.25) {
  throw new Error('Columnar numeric-list roundtrip failed.');
}

console.log('P4 serializer smoke test passed');
