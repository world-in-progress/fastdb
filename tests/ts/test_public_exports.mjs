import assert from 'node:assert/strict';
import test from 'node:test';

import * as fastdb from '../../ts/fastdb4ts/dist/index.js';

test('fastdb4ts main entry exposes generic call-db runtime names only', () => {
  assert.equal(typeof fastdb.encodeFastdbCallDb, 'function');
  assert.equal(typeof fastdb.decodeFastdbCallDb, 'function');
  assert.equal(typeof fastdb.viewFastdbCallDb, 'function');
  assert.equal(typeof fastdb.encodeFastdbFeature, 'function');
  assert.equal(typeof fastdb.decodeFastdbFeature, 'function');

  const cTwoRuntimeExports = Object.keys(fastdb)
    .filter((name) => /C2|CTwo|C_Two/.test(name))
    .sort();
  assert.deepEqual(cTwoRuntimeExports, []);
});
