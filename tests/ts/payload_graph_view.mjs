import assert from 'node:assert/strict';
import { readFile } from 'node:fs/promises';
import test from 'node:test';

import {
  BuildPolicy,
  Builder,
  CompiledSpec,
  GraphIdentity,
  PayloadError,
  ViewKind,
  initPayload,
} from '../../ts/fastdb4ts/dist/payload/index.js';

const fixtures = new URL('../golden/payload/v1/binary/spec/', import.meta.url);

async function compile(name) {
  return CompiledSpec.compile(await readFile(new URL(name, fixtures)));
}

async function graphPayload() {
  const spec = await compile('graph-all-values.source.json');
  const nodeIndex = spec.componentIndex('Node');
  const inlineIndex = spec.componentIndex('Inline');
  const assetIndex = spec.componentIndex('Asset');
  const builder = Builder.create(spec);
  spec.dispose();
  try {
    const node = builder.declareObject(nodeIndex);
    const asset = builder.declareObject(assetIndex);
    builder
      .objectFillBegin(node)
      .valueBool(true)
      .valueU8(0x12)
      .valueU16(0x3456)
      .valueU32(0x789a_bcde)
      .valueI32(-1_234_567)
      .valueU8nBits(0x3fe0_0000_0000_0000n)
      .valueU16nBits(0n)
      .valueF32Bits(0x7fa1_2345)
      .valueF64Bits(0xfff8_0000_0000_1234n)
      .valueStr('same')
      .valueWstrUnits(new Uint16Array([0x0041, 0xd83d, 0xde00]))
      .valueBytes(new Uint8Array([0x00, 0xff, 0x7e]))
      .valueComponentBegin()
      .valueNull()
      .valueU16(0xbeef)
      .valueListBegin(3n)
      .valueF32Bits(0x8000_0000)
      .valueNull()
      .valueF32Bits(0xff80_0001)
      .valueRef(node)
      .valueRef(asset)
      .objectFillBegin(asset)
      .valueStr('same')
      .valueRef(node)
      .entryBegin(0, 1n)
      .valueObject(node)
      .entryBegin(1, 1n)
      .valueObject(asset)
      .entryBegin(2, 2n)
      .valueRef(node)
      .valueNull()
      .entryBegin(3, 3n)
      .valueU8nBits(0n)
      .valueU8nBits(0x3fe0_0000_0000_0000n)
      .valueU8nBits(0x3ff0_0000_0000_0000n)
      .entryBegin(4, 3n)
      .valueU16nBits(0xbff0_0000_0000_0000n)
      .valueU16nBits(0n)
      .valueU16nBits(0x3ff0_0000_0000_0000n);
    const plan = builder.freeze();
    try {
      return {
        payload: plan.execute(BuildPolicy.AllowStaging).payload,
        nodeIndex,
        inlineIndex,
        assetIndex,
      };
    } finally {
      plan.dispose();
    }
  } finally {
    builder.dispose();
  }
}

function root(payload, entryIndex) {
  const sequence = payload.entryView(entryIndex);
  try {
    return sequence.at(0n);
  } finally {
    sequence.dispose();
  }
}

function usingView(view, operation) {
  try {
    return operation(view);
  } finally {
    view.dispose();
  }
}

function requireError(error, code, symbol, path, message, detailsJson) {
  assert.ok(error instanceof PayloadError);
  assert.equal(error.code, code);
  assert.equal(error.symbol, symbol);
  assert.equal(error.path, path);
  assert.equal(error.message, message);
  assert.equal(error.detailsJson, detailsJson);
  return true;
}

test('graph identity, refs, cycles, and materialized closure are Core-owned', async () => {
  await initPayload();
  const { payload, nodeIndex, inlineIndex, assetIndex } = await graphPayload();
  const sourceRoot = root(payload, 0);
  const rootIdentity = new GraphIdentity(nodeIndex, 0n);
  const assetIdentity = new GraphIdentity(assetIndex, 0n);
  assert.deepEqual(sourceRoot.graphIdentity(), rootIdentity);

  usingView(sourceRoot.field(12), (inline) => {
    assert.equal(inline.componentIndex(), inlineIndex);
    assert.throws(
      () => inline.graphIdentity(),
      (error) =>
        requireError(
          error,
          2004,
          'TYPE_MISMATCH',
          '/entries/0/0/fields/12',
          'Portable payload view kind does not match the operation',
          '{"reason":"view_kind_mismatch"}',
        ),
    );
  });

  const selfRef = sourceRoot.field(14);
  assert.equal(selfRef.kind(), ViewKind.Ref);
  assert.deepEqual(selfRef.graphIdentity(), rootIdentity);
  assert.throws(
    () => selfRef.field(0),
    (error) =>
      requireError(
        error,
        2004,
        'TYPE_MISMATCH',
        '/entries/0/0/fields/14',
        'Portable payload view kind does not match the operation',
        '{"reason":"view_kind_mismatch"}',
      ),
  );
  usingView(selfRef.refTarget(), (target) =>
    assert.deepEqual(target.graphIdentity(), rootIdentity),
  );

  const assetRef = sourceRoot.field(15);
  assert.deepEqual(assetRef.graphIdentity(), assetIdentity);
  usingView(assetRef.refTarget(), (asset) => {
    assert.deepEqual(asset.graphIdentity(), assetIdentity);
    usingView(asset.field(1), (ownerRef) => {
      assert.deepEqual(ownerRef.graphIdentity(), rootIdentity);
      usingView(ownerRef.refTarget(), (owner) =>
        assert.deepEqual(owner.graphIdentity(), rootIdentity),
      );
    });
  });

  usingView(payload.entryView(2), (refs) => {
    usingView(refs.at(0n), (sharedRef) => {
      assert.deepEqual(sharedRef.graphIdentity(), rootIdentity);
      usingView(sharedRef.refTarget(), (shared) =>
        assert.deepEqual(shared.graphIdentity(), rootIdentity),
      );
    });
    usingView(refs.at(1n), (nullRef) => {
      assert.equal(nullRef.isNull(), true);
      assert.throws(
        () => nullRef.refTarget(),
        (error) =>
          requireError(
            error,
            2003,
            'UNEXPECTED_NULL',
            '/entries/2/1',
            'Portable payload view is null',
            '{"reason":"unexpected_null"}',
          ),
      );
      assert.throws(
        () => nullRef.graphIdentity(),
        (error) =>
          requireError(
            error,
            2003,
            'UNEXPECTED_NULL',
            '/entries/2/1',
            'Portable payload view is null',
            '{"reason":"unexpected_null"}',
          ),
      );
    });
  });

  for (let iteration = 0; iteration < 1_000; iteration += 1) {
    const retainedRoot = sourceRoot.clone();
    const retainedRef = assetRef.clone();
    assert.equal(retainedRoot.graphIdentity().objectId, 0n);
    usingView(retainedRef.refTarget(), (target) =>
      assert.equal(target.graphIdentity().objectId, 0n),
    );
    retainedRef.dispose();
    retainedRoot.dispose();
  }

  const detached = sourceRoot.materialize();
  assert.deepEqual(detached.graphIdentity(), rootIdentity);
  payload.invalidate();
  assert.throws(
    () => sourceRoot.graphIdentity(),
    (error) =>
      requireError(
        error,
        4001,
        'VIEW_INVALIDATED',
        '/view',
        'Portable payload access barrier rejected the operation',
        '{"reason":"view_invalidated"}',
      ),
  );
  assert.throws(
    () => assetRef.refTarget(),
    (error) =>
      requireError(
        error,
        4001,
        'VIEW_INVALIDATED',
        '/view',
        'Portable payload access barrier rejected the operation',
        '{"reason":"view_invalidated"}',
      ),
  );
  assetRef.dispose();
  selfRef.dispose();
  sourceRoot.dispose();
  payload.dispose();

  usingView(detached.field(14), (detachedSelf) => {
    assert.deepEqual(detachedSelf.graphIdentity(), rootIdentity);
    usingView(detachedSelf.refTarget(), (target) =>
      assert.deepEqual(target.graphIdentity(), rootIdentity),
    );
  });
  usingView(detached.field(15), (detachedAssetRef) =>
    usingView(detachedAssetRef.refTarget(), (detachedAsset) => {
      assert.deepEqual(detachedAsset.graphIdentity(), assetIdentity);
      usingView(detachedAsset.field(1), (ownerRef) =>
        usingView(ownerRef.refTarget(), (owner) =>
          assert.deepEqual(owner.graphIdentity(), rootIdentity),
        ),
      );
    }),
  );
  detached.dispose();
});

test('disconnected roots have distinct payload-scoped identities', async () => {
  await initPayload();
  const spec = await compile('graph-disconnected-roots.source.json');
  const component = spec.componentIndex('Node');
  const builder = Builder.create(spec);
  spec.dispose();
  try {
    const firstHandle = builder.declareObject(component);
    const secondHandle = builder.declareObject(component);
    builder
      .objectFillBegin(firstHandle)
      .valueU32(11)
      .valueNull()
      .objectFillBegin(secondHandle)
      .valueU32(22)
      .valueNull()
      .entryBegin(0, 2n)
      .valueObject(firstHandle)
      .valueObject(secondHandle);
    const plan = builder.freeze();
    try {
      const payload = plan.execute(BuildPolicy.AllowStaging).payload;
      usingView(payload.entryView(0), (roots) => {
        usingView(roots.at(0n), (first) => {
          assert.deepEqual(first.graphIdentity(), new GraphIdentity(component, 0n));
          usingView(first.field(0), (value) => assert.equal(value.getU32(), 11));
        });
        usingView(roots.at(1n), (second) => {
          assert.deepEqual(second.graphIdentity(), new GraphIdentity(component, 1n));
          usingView(second.field(0), (value) => assert.equal(value.getU32(), 22));
        });
      });

      const independent = await graphPayload();
      usingView(root(independent.payload, 0), (independentRoot) =>
        assert.equal(independentRoot.graphIdentity().objectId, 0n),
      );
      payload.invalidate();
      payload.dispose();
      usingView(root(independent.payload, 0), (independentRoot) =>
        usingView(independentRoot.field(3), (value) =>
          assert.equal(value.getU32(), 0x789a_bcde),
        ),
      );
      independent.payload.dispose();
    } finally {
      plan.dispose();
    }
  } finally {
    builder.dispose();
  }
});
