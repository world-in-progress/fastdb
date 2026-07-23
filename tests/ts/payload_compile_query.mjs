import assert from 'node:assert/strict';
import { readFile } from 'node:fs/promises';
import test from 'node:test';

import {
  Capabilities,
  CompiledSpec,
  PayloadError,
  Profile,
  initPayload,
} from '../../ts/fastdb4ts/dist/payload/index.js';

const specUrl = new URL('../golden/payload/v1/spec/', import.meta.url);

async function golden(path) {
  return readFile(new URL(path, specUrl));
}

function bytesFromHex(source) {
  return Uint8Array.from(Buffer.from(source.toString('ascii').trim(), 'hex'));
}

test('compile and query record-all-types through Core', async () => {
  await initPayload();
  const source = await golden('valid/record-all-types.source.json');
  const expectedDigest = (await golden('valid/record-all-types.sha256'))
    .toString('ascii')
    .trim();
  const spec = CompiledSpec.compile(source);
  const clone = spec.clone();
  try {
    assert.deepEqual(
      spec.canonicalJson(),
      bytesFromHex(await golden('valid/record-all-types.canonical.hex')),
    );
    assert.deepEqual(
      spec.manifestJson(),
      bytesFromHex(await golden('valid/record-all-types.manifest.hex')),
    );
    assert.equal(Buffer.from(spec.sha256()).toString('hex'), expectedDigest);
    assert.equal(spec.profile(), Profile.RecordV1);
    assert.deepEqual(
      spec.capabilities(),
      new Capabilities(Profile.RecordV1, 0x1bn, 0xffn, 0x0fn, 1),
    );
    assert.equal(spec.entryCount(), 2);
    assert.equal(spec.entryId(0), 'single');
    assert.equal(spec.entryId(1), 'series');
    assert.equal(spec.entryIndex('series'), 1);
    assert.equal(spec.componentCount(), 2);
    assert.equal(spec.componentId(0), 'AllTypes');
    assert.equal(spec.componentId(1), 'Leaf');
    assert.equal(spec.componentIndex('Leaf'), 1);
    assert.equal(spec.componentFieldCount(0), 14);
    assert.equal(spec.componentFieldId(0, 9), 'str_value');
    assert.equal(spec.componentFieldIndex(0, 'list_value'), 13);
    assert.deepEqual(clone.sha256(), spec.sha256());
  } finally {
    clone.dispose();
    spec.dispose();
  }
});
test('compile error preserves every Core field', async () => {
  await initPayload();
  const source = await golden('invalid/bad-kind.source.json');
  assert.throws(
    () => CompiledSpec.compile(source),
    (error) => {
      assert.ok(error instanceof PayloadError);
      assert.equal(error.code, 1005);
      assert.equal(error.symbol, 'INVALID_TYPE');
      assert.equal(error.path, '/entries/0/type/items/kind');
      assert.equal(error.message, 'Payload type kind is invalid');
      assert.equal(
        error.detailsJson,
        '{"actual":"text","reason":"invalid_type_kind"}',
      );
      return true;
    },
  );
});
