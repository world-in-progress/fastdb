import assert from 'node:assert/strict';
import { createHash } from 'node:crypto';
import test from 'node:test';

import {
  ArtifactKind,
  ArtifactSet,
  BuildPolicy,
  Builder,
  CodegenOptions,
  CodegenTarget,
  CompiledSpec,
  PayloadError,
  initPayload,
} from '../../ts/fastdb4ts/dist/payload/index.js';

const encoder = new TextEncoder();
const SPEC_A = encoder.encode(
  '{"schema":"fastdb.payload.v1","profile":"record.v1",' +
    '"entries":[{"id":"value","cardinality":"one",' +
    '"type":{"kind":"u8"}}],"components":[]}',
);
const SPEC_B = encoder.encode(
  '{"schema":"fastdb.payload.v1","profile":"record.v1",' +
    '"entries":[{"id":"other","cardinality":"one",' +
    '"type":{"kind":"u8"}}],"components":[]}',
);

function hex(bytes) {
  return Buffer.from(bytes).toString('hex');
}

function isDigestMismatch(error, path, actual, expected) {
  return (
    error instanceof PayloadError &&
    error.code === 3006 &&
    error.symbol === 'DIGEST_MISMATCH' &&
    error.path === path &&
    error.message === 'Portable payload spec digest does not match' &&
    error.detailsJson ===
      `{"actual":"${hex(actual)}","expected":"${hex(expected)}",` +
        '"reason":"spec_digest_mismatch"}'
  );
}

test('Core codegen projects all targets into independently owned artifacts', async () => {
  await initPayload();
  const suffixes = new Map([
    [CodegenTarget.Cpp, '.hpp'],
    [CodegenTarget.Rust, '.rs'],
    [CodegenTarget.Python, '.py'],
    [CodegenTarget.TypeScript, '.ts'],
  ]);
  const spec = CompiledSpec.compile(SPEC_A);
  try {
    for (const [target, suffix] of suffixes) {
      const generated = spec.generate(target);
      const clone = generated.clone();
      assert.equal(generated.size(), 1n);
      const artifact = clone.artifact(0n);
      clone.dispose();
      generated.dispose();

      assert.equal(artifact.kind, ArtifactKind.Source);
      assert.ok(artifact.relativePath.endsWith(suffix));
      assert.ok(artifact.bytes.byteLength > 0);
      assert.deepEqual(
        artifact.sha256,
        new Uint8Array(createHash('sha256').update(artifact.bytes).digest()),
      );

      const mutated = artifact.bytes;
      mutated.fill(0);
      assert.notDeepEqual(artifact.bytes, mutated);
      const mutatedDigest = artifact.sha256;
      mutatedDigest.fill(0);
      assert.notDeepEqual(artifact.sha256, mutatedDigest);
    }

    assert.throws(
      () =>
        spec.generate(
          CodegenTarget.Cpp,
          new CodegenOptions({ maxArtifacts: 0n }),
        ),
      (error) =>
        error instanceof PayloadError &&
        error.code === 6003 &&
        error.path === '/codegen/limits/max_artifacts',
    );
    assert.equal(ArtifactSet.fromHandle, undefined);
    assert.throws(
      () => spec.generate('Cpp'),
      (error) =>
        error instanceof RangeError &&
        error.message === 'target must be a CodegenTarget',
    );

    const mutatedOptions = new CodegenOptions();
    mutatedOptions.maxArtifacts = -1n;
    assert.throws(
      () => spec.generate(CodegenTarget.Cpp, mutatedOptions),
      (error) =>
        error instanceof RangeError &&
        error.message === 'maxArtifacts must be an unsigned 64-bit bigint',
    );
  } finally {
    spec.dispose();
  }
});

test('Core provenance guards reject same indexes from another spec', async () => {
  await initPayload();
  const specA = CompiledSpec.compile(SPEC_A);
  const specB = CompiledSpec.compile(SPEC_B);
  const digestA = specA.sha256();
  const digestB = specB.sha256();
  const builder = Builder.create(specA);
  let plan;
  let payload;
  let view;
  let detached;
  try {
    builder.requireSpecSha256(digestA);
    assert.throws(
      () => builder.requireSpecSha256(digestB),
      (error) =>
        isDigestMismatch(
          error,
          '/builder/spec_sha256',
          digestA,
          digestB,
        ),
    );

    plan = builder.entryBegin(0, 1n).valueU8(7).freeze();
    builder.dispose();
    payload = plan.execute(BuildPolicy.AllowStaging).payload;
    payload.requireSpecSha256(digestA);
    assert.throws(
      () => payload.requireSpecSha256(digestB),
      (error) =>
        isDigestMismatch(
          error,
          '/payload/spec_sha256',
          digestA,
          digestB,
        ),
    );

    view = payload.entryView(0);
    detached = view.materialize();
    for (const candidate of [view, detached]) {
      candidate.requireSpecSha256(digestA);
      assert.throws(
        () => candidate.requireSpecSha256(digestB),
        (error) =>
          isDigestMismatch(
            error,
            '/view/spec_sha256',
            digestA,
            digestB,
          ),
      );
    }
  } finally {
    detached?.dispose();
    view?.dispose();
    payload?.dispose();
    plan?.dispose();
    builder.dispose();
    specB.dispose();
    specA.dispose();
  }
});
