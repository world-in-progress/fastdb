import assert from 'node:assert/strict';
import test from 'node:test';

import * as fastdb from '../../ts/fastdb4ts/dist/index.js';

test('fastdb4ts root contains no removed RPC authority', () => {
  const removedBindingStem = 'Fastdb' + 'Call' + 'Db';
  for (const name of [
    'encode' + removedBindingStem,
    'decode' + removedBindingStem,
    'view' + removedBindingStem,
    'encodeFastdbFeature',
    'decodeFastdbFeature',
  ]) {
    assert.equal(name in fastdb, false);
  }
  assert.equal(typeof fastdb.Feature, 'function');
  assert.equal(typeof fastdb.ORM, 'function');
  assert.equal(typeof fastdb.FastSerializer, 'function');
});

test('fastdb4ts root contains no downstream runtime surface', () => {
  const cTwoRuntimeExports = Object.keys(fastdb)
    .filter((name) => /C2|CTwo|C_Two/.test(name))
    .sort();
  assert.deepEqual(cTwoRuntimeExports, []);
});
