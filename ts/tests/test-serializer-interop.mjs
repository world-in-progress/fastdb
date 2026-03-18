import fs from 'node:fs';
import path from 'node:path';
import { spawnSync } from 'node:child_process';
import { fileURLToPath } from 'node:url';

import {
  F64,
  FastSerializer,
  Feature,
  I32,
  U32,
  defineSchema,
  initFastdb,
  listOf,
  ref,
} from '../fastdb4ts/dist/index.js';

const thisFile = fileURLToPath(import.meta.url);
const testsDir = path.dirname(thisFile);
const repoRoot = path.resolve(testsDir, '../..');
const tmpDir = path.join(testsDir, '.tmp');
const pyToTsPath = path.join(tmpDir, 'python-to-ts.bin');
const tsToPyPath = path.join(tmpDir, 'ts-to-python.bin');

class Point extends Feature {
  static schema = defineSchema({ x: F64, y: F64 });
}

class Line extends Feature {
  static schema = defineSchema({ points: listOf(Point), id: I32 });
}

class RecursiveNode extends Feature {
  static schema = defineSchema({ val: I32, next: ref(() => RecursiveNode) });
}

class NumericColumnarLists extends Feature {
  static schema = defineSchema({ ids: listOf(U32), values: listOf(F64) });
}

function runUvPython(command) {
  const result = spawnSync('uv', ['run', '--no-sync', 'python', 'ts/tests/serializer_interop.py', command], {
    cwd: repoRoot,
    stdio: 'inherit',
  });
  if (result.status !== 0) {
    throw new Error(`uv python command failed: ${command}`);
  }
}

await initFastdb();
fs.mkdirSync(tmpDir, { recursive: true });

try {
  runUvPython('write-python-fixture');

  const line = FastSerializer.loads(fs.readFileSync(pyToTsPath), Line);
  if (line.id !== 42 || line.points.length !== 2 || line.points[0].x !== 1.5 || line.points[1].y !== 4.5) {
    throw new Error('Python->TS serializer interop failed.');
  }

  const n1 = new RecursiveNode({ val: 7 });
  const n2 = new RecursiveNode({ val: 8 });
  n1.next = n2;
  n2.next = n1;

  const numeric = new NumericColumnarLists({
    ids: [1, 2, 4294967295],
    values: [0.5, -1.25, 9.75],
  });

  const cycleBytes = FastSerializer.dumps(n1);
  const numericBytes = FastSerializer.dumps(numeric);
  const header = Buffer.alloc(4);
  header.writeUInt32LE(cycleBytes.length, 0);
  fs.writeFileSync(tsToPyPath, Buffer.concat([header, Buffer.from(cycleBytes), Buffer.from(numericBytes)]));

  runUvPython('verify-ts-fixture');
  console.log('P4 serializer interop test passed');
} finally {
  fs.rmSync(tmpDir, { recursive: true, force: true });
}
