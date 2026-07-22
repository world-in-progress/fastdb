import assert from 'node:assert/strict';
import { readFile } from 'node:fs/promises';
import test from 'node:test';

import {
  BuildPolicy,
  Builder,
  CompiledSpec,
  ExecutionMode,
  FallbackReason,
  OpenOptions,
  Payload,
  PayloadError,
  Profile,
  WasmMemoryBacking,
  WasmOwnedBytes,
  initPayload,
} from '../../ts/fastdb4ts/dist/payload/index.js';
import { __testingWasmMemoryBacking } from '../../ts/fastdb4ts/dist/payload/runtime.js';

const specUrl = new URL('../golden/payload/v1/binary/spec/', import.meta.url);
const binaryUrl = new URL('../golden/payload/v1/binary/valid/', import.meta.url);
const invalidBinaryUrl = new URL(
  '../golden/payload/v1/binary/invalid/',
  import.meta.url,
);

async function binaryGolden(name) {
  const source = await readFile(new URL(name, binaryUrl), 'ascii');
  return new Uint8Array(Buffer.from(source.trim(), 'hex'));
}

async function invalidBinaryGolden(name) {
  const source = await readFile(new URL(name, invalidBinaryUrl), 'ascii');
  return new Uint8Array(Buffer.from(source.trim(), 'hex'));
}

async function fixedPlan() {
  const spec = CompiledSpec.compile(
    await readFile(new URL('fixed-scalars.source.json', specUrl)),
  );
  const builder = Builder.create(spec);
  builder.entryBegin(0, 1n).valueBool(true);
  builder.entryBegin(1, 1n).valueU8(0xab);
  builder
    .entryBegin(2, 3n)
    .valueU16(0x1234)
    .valueNull()
    .valueU16(0xffff);
  builder.entryBegin(3, 1n).valueU32(0x1234_5678);
  builder.entryBegin(4, 1n).valueI32(-2);
  builder
    .entryBegin(5, 4n)
    .valueF32Bits(0x8000_0000)
    .valueF32Bits(0x7f80_0000)
    .valueF32Bits(0xff80_0000)
    .valueF32Bits(0x7fa1_2345);
  builder
    .entryBegin(6, 4n)
    .valueF64Bits(0x8000_0000_0000_0000n)
    .valueF64Bits(0x7ff0_0000_0000_0000n)
    .valueF64Bits(0xfff0_0000_0000_0000n)
    .valueF64Bits(0x7ff0_0000_0000_0042n);
  const plan = builder.freeze();
  builder.dispose();
  return { spec, plan };
}

test('internal heap execution and payload facts are Core-owned', async () => {
  await initPayload();
  const { spec, plan } = await fixedPlan();
  try {
    const info = plan.info();
    const result = plan.execute(BuildPolicy.AllowStaging);
    try {
      assert.equal(result.report.mode, ExecutionMode.Direct);
      assert.equal(result.report.fallbackReason, FallbackReason.None);
      assert.equal(result.report.requestedBytes, info.totalBytes);
      assert.equal(result.report.usedBytes, info.totalBytes);
      assert.equal(result.report.stagingBytes, 0n);
      assert.equal(result.report.regionCount, info.regionCount);
      assert.ok(result.report.backingCapacity >= info.totalBytes);
      assert.equal(result.payload.profile(), Profile.RecordV1);
      assert.deepEqual(result.payload.executionReport(), result.report);
      const expectedBinary = await binaryGolden('fixed-scalars.bin.hex');
      assert.deepEqual(result.payload.binaryBytes(), expectedBinary);
      assert.equal(BigInt(expectedBinary.byteLength), info.totalBytes);

      const clone = result.payload.clone();
      result.payload.dispose();
      assert.equal(BigInt(clone.binaryBytes().byteLength), info.totalBytes);
      clone.dispose();
    } finally {
      result.payload.dispose();
    }
  } finally {
    plan.dispose();
    spec.dispose();
  }
});

test('Wasm backing reports direct, staged fallback, and exact rejection', async () => {
  await initPayload();
  const { spec, plan } = await fixedPlan();
  const direct = new WasmMemoryBacking();
  const staged = new WasmMemoryBacking({ allowDirect: false });
  try {
    const directResult = plan.execute(BuildPolicy.AllowStaging, direct);
    assert.equal(directResult.report.mode, ExecutionMode.Direct);
    directResult.payload.dispose();
    assert.equal(direct.stats().releases, 1n);

    const stagedResult = plan.execute(BuildPolicy.AllowStaging, staged);
    assert.equal(stagedResult.report.mode, ExecutionMode.Staged);
    assert.equal(
      stagedResult.report.fallbackReason,
      FallbackReason.BackingDeclinedDirect,
    );
    assert.equal(stagedResult.report.stagingBytes, stagedResult.report.usedBytes);
    assert.ok(staged.stats().writes > 0n);
    stagedResult.payload.dispose();
    assert.equal(staged.stats().releases, 1n);

    assert.throws(
      () => plan.execute(BuildPolicy.RequireDirect, staged),
      (error) => {
        assert.ok(error instanceof PayloadError);
        assert.equal(error.code, 2007);
        assert.equal(error.symbol, 'DIRECT_UNAVAILABLE');
        assert.equal(error.path, '/backing');
        assert.equal(
          error.detailsJson,
          '{"reason":"backing_declined_direct"}',
        );
        return true;
      },
    );
  } finally {
    direct.dispose();
    staged.dispose();
    plan.dispose();
    spec.dispose();
  }
});

test('native Wasm callback failures rollback exactly once', async () => {
  await initPayload();
  const { spec, plan } = await fixedPlan();
  for (const [options, expected] of [
    [{ allowDirect: false, failWrite: true }, 5002],
    [{ failCommit: true }, 5003],
  ]) {
    const backing = __testingWasmMemoryBacking(options);
    try {
      assert.throws(
        () => plan.execute(BuildPolicy.AllowStaging, backing),
        (error) => error instanceof PayloadError && error.code === expected,
      );
      assert.equal(backing.stats().rollbacks, 1n);
      assert.equal(backing.stats().releases, 0n);
    } finally {
      backing.dispose();
    }
  }
  plan.dispose();
  spec.dispose();
});

test('copy and Wasm-owned external open keep truthful lifetimes', async () => {
  await initPayload();
  const { spec, plan } = await fixedPlan();
  const built = plan.execute(BuildPolicy.AllowStaging);
  const original = built.payload.binaryBytes();
  const source = original.slice();
  const copied = Payload.openCopy(spec, source, new OpenOptions());
  source.fill(0xff);
  assert.deepEqual(copied.binaryBytes(), original);

  const external = WasmOwnedBytes.copyFrom(original);
  const opened = Payload.openWasmOwned(spec, external, new OpenOptions());
  external.dispose();
  assert.deepEqual(opened.binaryBytes(), original);
  assert.throws(
    () => opened.executionReport(),
    (error) => {
      assert.ok(error instanceof PayloadError);
      assert.equal(error.code, 2008);
      assert.equal(
        error.detailsJson,
        '{"reason":"opened_payload_has_no_execution_report"}',
      );
      return true;
    },
  );
  assert.equal(opened.profile(), Profile.RecordV1);

  opened.dispose();
  copied.dispose();
  built.payload.dispose();
  plan.dispose();
  spec.dispose();
});

test('invalid open preserves every Core field', async () => {
  await initPayload();
  const spec = CompiledSpec.compile(
    await readFile(new URL('empty.source.json', specUrl)),
  );
  try {
    const invalid = await invalidBinaryGolden('header-magic.bin.hex');
    assert.throws(
      () => Payload.openCopy(spec, invalid, new OpenOptions()),
      (error) => {
        assert.ok(error instanceof PayloadError);
        assert.equal(error.code, 3001);
        assert.equal(error.symbol, 'INVALID_MAGIC');
        assert.equal(error.path, '/binary/header/magic');
        assert.equal(error.message, 'Portable payload magic is invalid');
        assert.equal(
          error.detailsJson,
          '{"actual":"4744425041593100","expected":"4644425041593100","reason":"invalid_magic"}',
        );
        return true;
      },
    );
  } finally {
    spec.dispose();
  }
});

test('runtime handles cannot be forged from JavaScript', async () => {
  await initPayload();
  assert.throws(() => Reflect.construct(Payload, [1]), /created only/);
  assert.throws(() => Reflect.construct(WasmOwnedBytes, [{}, Symbol()]));

  const forgedPayload = Object.create(Payload.prototype);
  forgedPayload.handle = 1;
  assert.throws(() => forgedPayload.binaryBytes());

  const forgedBacking = Object.create(WasmMemoryBacking.prototype);
  forgedBacking.native = {};
  assert.throws(() => forgedBacking.stats());
});
