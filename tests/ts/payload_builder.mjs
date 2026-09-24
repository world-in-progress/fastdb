import assert from 'node:assert/strict';
import { readFile } from 'node:fs/promises';
import test from 'node:test';

import {
  BuildPlan,
  Builder,
  CompiledSpec,
  FixedRun,
  ObjectHandle,
  PayloadError,
  initPayload,
} from '../../ts/fastdb4ts/dist/payload/index.js';

const specUrl = new URL('../golden/payload/v1/binary/spec/', import.meta.url);

async function compile(name) {
  return CompiledSpec.compile(await readFile(new URL(name, specUrl)));
}

function fixedBytes() {
  const bytes = new Uint8Array(6);
  const view = new DataView(bytes.buffer);
  view.setUint16(0, 0x1234, true);
  view.setUint16(2, 0, true);
  view.setUint16(4, 0xffff, true);
  return bytes;
}

function finishFixed(builder) {
  builder.entryBegin(1, 1n).valueU8(0xab);
  builder
    .entryBegin(2, 3n)
    .valueFixedRun(
      new FixedRun(fixedBytes(), 3n, 2n, new Uint8Array([0x05]), 0n),
    )
    .entryBegin(3, 1n)
    .valueU32(0x1234_5678)
    .entryBegin(4, 1n)
    .valueI32(-2)
    .entryBegin(5, 4n)
    .valueF32Bits(0x8000_0000)
    .valueF32Bits(0x7f80_0000)
    .valueF32Bits(0xff80_0000)
    .valueF32Bits(0x7fa1_2345)
    .entryBegin(6, 4n)
    .valueF64Bits(0x8000_0000_0000_0000n)
    .valueF64Bits(0x7ff0_0000_0000_0000n)
    .valueF64Bits(0xfff0_0000_0000_0000n)
    .valueF64Bits(0x7ff0_0000_0000_0042n);
}

async function authorFixed() {
  const spec = await compile('fixed-scalars.source.json');
  const builder = Builder.create(spec);
  spec.dispose();
  try {
    builder.entryBegin(0, 1n).valueBool(true);
    finishFixed(builder);
    return builder.freeze();
  } finally {
    builder.dispose();
  }
}

test('fixed run and immutable plan facts are Core-owned', async () => {
  await initPayload();
  const plan = await authorFixed();
  try {
    const info = plan.info();
    assert.equal(info.totalBytes, 928n);
    assert.equal(info.logicalValueCount, 22n);
    assert.equal(info.listElementCount, 0n);
    assert.equal(info.textBytes, 0n);
    assert.equal(info.opaqueBytes, 0n);
    assert.equal(info.maxAlignment, 8);
    assert.equal(info.directBuildStatus, 1);
    assert.equal(info.graphObjectCount, 0n);

    const clone = plan.clone();
    plan.dispose();
    assert.deepEqual(clone.info(), info);
    clone.dispose();
    clone.dispose();
  } finally {
    plan.dispose();
  }
});

test('nested component lists preserve null and empty shapes', async () => {
  await initPayload();
  const spec = await compile('nested-lists.source.json');
  const builder = Builder.create(spec);
  spec.dispose();
  try {
    builder
      .entryBegin(0, 2n)
      .valueComponentBegin()
      .valueListBegin(3n)
      .valueNull()
      .valueListBegin(0n)
      .valueListBegin(3n)
      .valueStr('')
      .valueNull()
      .valueStr('alpha')
      .valueNull()
      .valueListBegin(0n)
      .valueComponentBegin()
      .valueListBegin(0n)
      .valueListBegin(3n)
      .valueNull()
      .valueWstrUnits(new Uint16Array())
      .valueWstrUnits(new Uint16Array([0x0041, 0xd83d, 0xde03]))
      .valueListBegin(2n)
      .valueNull()
      .valueBytes(new Uint8Array([0x00, 0xff, 0x41]))
      .entryBegin(1, 3n)
      .valueNull()
      .valueListBegin(0n)
      .valueListBegin(3n)
      .valueU16(7)
      .valueNull()
      .valueU16(9)
      .entryBegin(2, 1n)
      .valueListBegin(3n)
      .valueNull()
      .valueListBegin(0n)
      .valueListBegin(3n)
      .valueStr('')
      .valueNull()
      .valueStr('tail');
    const plan = builder.freeze();
    try {
      const info = plan.info();
      assert.equal(info.totalBytes, 1960n);
      assert.equal(info.listElementCount, 20n);
      assert.equal(info.graphObjectCount, 0n);
    } finally {
      plan.dispose();
    }
  } finally {
    builder.dispose();
  }
});

test('graph authoring preserves forward self and mutual refs', async () => {
  await initPayload();
  const spec = await compile('graph-all-values.source.json');
  const nodeComponent = spec.componentIndex('Node');
  const assetComponent = spec.componentIndex('Asset');
  const builder = Builder.create(spec);
  spec.dispose();
  try {
    const node = builder.declareObject(nodeComponent);
    const asset = builder.declareObject(assetComponent);
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
      const info = plan.info();
      assert.equal(info.totalBytes, 1304n);
      assert.equal(info.regionCount, 13n);
      assert.equal(info.logicalValueCount, 40n);
      assert.equal(info.listElementCount, 3n);
      assert.equal(info.textBytes, 14n);
      assert.equal(info.opaqueBytes, 3n);
      assert.equal(info.validationWork, 146n);
      assert.equal(info.maxAlignment, 8);
      assert.equal(info.directBuildStatus, 1);
      assert.equal(info.graphObjectCount, 2n);
    } finally {
      plan.dispose();
    }
  } finally {
    builder.dispose();
  }
});

test('disconnected objects are explicit roots', async () => {
  await initPayload();
  const spec = await compile('graph-disconnected-roots.source.json');
  const component = spec.componentIndex('Node');
  const builder = Builder.create(spec);
  spec.dispose();
  try {
    const first = builder.declareObject(component);
    const second = builder.declareObject(component);
    builder
      .objectFillBegin(first)
      .valueU32(11)
      .valueNull()
      .objectFillBegin(second)
      .valueU32(22)
      .valueNull()
      .entryBegin(0, 2n)
      .valueObject(first)
      .valueObject(second);
    const plan = builder.freeze();
    try {
      assert.equal(plan.info().totalBytes, 328n);
      assert.equal(plan.info().graphObjectCount, 2n);
    } finally {
      plan.dispose();
    }
  } finally {
    builder.dispose();
  }
});

test('Core type error is preserved and builder mutation is retryable', async () => {
  await initPayload();
  const spec = await compile('fixed-scalars.source.json');
  const builder = Builder.create(spec);
  spec.dispose();
  try {
    builder.entryBegin(0, 1n);
    assert.throws(
      () => builder.valueU8(1),
      (error) => {
        assert.ok(error instanceof PayloadError);
        assert.equal(error.code, 2004);
        assert.equal(error.symbol, 'TYPE_MISMATCH');
        return true;
      },
    );
    builder.valueBool(true);
    finishFixed(builder);
    const plan = builder.freeze();
    try {
      assert.equal(plan.info().totalBytes, 928n);
    } finally {
      plan.dispose();
    }
  } finally {
    builder.dispose();
  }
});

test('Core-owned builder limits are projected without binding semantics', async () => {
  await initPayload();
  const spec = await compile('graph-disconnected-roots.source.json');
  const component = spec.componentIndex('Node');
  const builder = Builder.create(spec, { maxGraphObjects: 1n });
  spec.dispose();
  try {
    builder.declareObject(component);
    assert.throws(
      () => builder.declareObject(component),
      (error) => {
        assert.ok(error instanceof PayloadError);
        assert.equal(error.code, 2013);
        assert.equal(error.symbol, 'BUILDER_RESOURCE_LIMIT');
        assert.equal(error.path, '/objects/Node/1');
        return true;
      },
    );
  } finally {
    builder.dispose();
  }
});

test('authoring handles cannot be forged from JavaScript', () => {
  assert.throws(() => new ObjectHandle(1n), /created only/);
  assert.throws(() => new Builder(0), /created only/);
  assert.throws(() => new BuildPlan(0), /created only/);

  const forgedObject = Object.create(ObjectHandle.prototype);
  forgedObject.raw = 1n;
  const objectValue = Object.getOwnPropertySymbols(ObjectHandle.prototype)[0];
  assert.throws(() => ObjectHandle.prototype[objectValue].call(forgedObject));

  const forgedBuilder = Object.create(Builder.prototype);
  forgedBuilder.handle = 1;
  assert.throws(() => forgedBuilder.requireHandle());

  const forgedPlan = Object.create(BuildPlan.prototype);
  forgedPlan.handle = 1;
  assert.throws(() => forgedPlan.requireHandle());

  for (const factory of Object.getOwnPropertySymbols(ObjectHandle)) {
    assert.throws(() => ObjectHandle[factory](1n), /created only/);
  }
  for (const factory of Object.getOwnPropertySymbols(BuildPlan)) {
    assert.throws(() => BuildPlan[factory](1), /created only/);
  }
});

test('compiled spec handles cannot be forged from JavaScript', () => {
  assert.throws(() => new CompiledSpec(0), /created only/);

  const forgedSpec = Object.create(CompiledSpec.prototype);
  forgedSpec.handle = 1;
  assert.throws(() => forgedSpec.requireHandle());
});
