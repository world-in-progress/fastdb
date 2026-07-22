import assert from 'node:assert/strict';
import { readFile } from 'node:fs/promises';
import test from 'node:test';

import {
  Access,
  BuildPolicy,
  Builder,
  CompiledSpec,
  PayloadError,
  View,
  ViewKind,
  initPayload,
} from '../../ts/fastdb4ts/dist/payload/index.js';

const recordSpec = new URL(
  '../golden/payload/v1/spec/valid/record-all-types.source.json',
  import.meta.url,
);

async function recordPayload() {
  const spec = CompiledSpec.compile(await readFile(recordSpec));
  const builder = Builder.create(spec);
  builder
    .entryBegin(0, 1n)
    .valueComponentBegin()
    .valueBool(true)
    .valueU8(0xab)
    .valueU16(0x1234)
    .valueU32(0x89ab_cdef)
    .valueI32(-42)
    .valueU8n(0)
    .valueU16n(1)
    .valueF32Bits(0x3fc0_0000)
    .valueF64Bits(0x4004_0000_0000_0000n)
    .valueStr('\ufeffA\0B')
    .valueWstrUnits(
      new Uint16Array([0xfeff, 0x0041, 0, 0xd83c, 0xdf0d, 0x03a9]),
    )
    .valueBytes(new Uint8Array([0, 1, 0xff]))
    .valueComponentBegin()
    .valueListBegin(3n)
    .valueListBegin(0n)
    .valueNull()
    .valueListBegin(3n)
    .valueStr('')
    .valueNull()
    .valueStr('tail');
  builder
    .entryBegin(1, 4n)
    .valueNull()
    .valueListBegin(0n)
    .valueListBegin(3n)
    .valueU8(0)
    .valueNull()
    .valueU8(0xff)
    .valueListBegin(1n)
    .valueU8(7);
  const plan = builder.freeze();
  builder.dispose();
  const result = plan.execute(BuildPolicy.AllowStaging);
  plan.dispose();
  return { spec, payload: result.payload };
}

function usingView(view, operation) {
  try {
    return operation(view);
  } finally {
    view.dispose();
  }
}

function usingAccess(access, operation) {
  try {
    return operation(access);
  } finally {
    access.dispose();
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

test('complete record views are Core-owned and materialized', async () => {
  await initPayload();
  const { spec, payload } = await recordPayload();
  const binary = payload.binaryBytes();
  usingAccess(payload.acquire(), (access) => {
    assert.deepEqual(access.payloadBytes(), binary);
    assert.throws(
      () => access.str(),
      (error) =>
        requireError(
          error,
          2004,
          'TYPE_MISMATCH',
          '/access',
          'Portable payload view kind does not match the operation',
          '{"reason":"view_kind_mismatch"}',
        ),
    );
  });

  const sequence = payload.entryView(0);
  assert.equal(sequence.kind(), ViewKind.Sequence);
  assert.equal(sequence.isNull(), false);
  assert.equal(sequence.length(), 1n);
  const root = sequence.at(0n);
  assert.equal(root.kind(), ViewKind.Component);
  assert.equal(root.componentIndex(), 0);
  assert.equal(root.fieldCount(), 14);

  usingView(root.field(0), (value) => assert.equal(value.getBool(), true));
  usingView(root.field(1), (value) => assert.equal(value.getU8(), 0xab));
  usingView(root.field(2), (value) => assert.equal(value.getU16(), 0x1234));
  usingView(root.field(3), (value) =>
    assert.equal(value.getU32(), 0x89ab_cdef),
  );
  usingView(root.field(4), (value) => assert.equal(value.getI32(), -42));
  usingView(root.field(5), (value) => {
    assert.equal(value.getU8nF64Bits(), 0n);
    assert.equal(value.getU8n(), 0);
  });
  usingView(root.field(6), (value) => {
    assert.equal(value.getU16nF64Bits(), 0x3ff0_0000_0000_0000n);
    assert.equal(value.getU16n(), 1);
  });
  usingView(root.field(7), (value) => {
    assert.equal(value.getF32Bits(), 0x3fc0_0000);
    assert.equal(value.getF32(), 1.5);
  });
  usingView(root.field(8), (value) => {
    assert.equal(value.getF64Bits(), 0x4004_0000_0000_0000n);
    assert.equal(value.getF64(), 2.5);
  });

  const text = root.field(9);
  assert.equal(text.kind(), ViewKind.Str);
  const textAccess = text.acquire();
  text.dispose();
  assert.equal(textAccess.str(), '\ufeffA\0B');

  usingView(root.field(10), (wide) => {
    assert.equal(wide.kind(), ViewKind.Wstr);
    usingAccess(wide.acquire(), (access) =>
      assert.equal(access.wstr(), '\ufeffA\0🌍Ω'),
    );
  });
  usingView(root.field(11), (opaque) => {
    assert.equal(opaque.kind(), ViewKind.Bytes);
    usingAccess(opaque.acquire(), (access) =>
      assert.deepEqual(access.bytes(), new Uint8Array([0, 1, 0xff])),
    );
  });
  usingView(root.field(12), (leaf) => {
    assert.equal(leaf.kind(), ViewKind.Component);
    assert.equal(leaf.componentIndex(), 1);
    assert.equal(leaf.fieldCount(), 0);
  });
  usingView(root.field(13), (nested) => {
    assert.equal(nested.kind(), ViewKind.List);
    assert.equal(nested.length(), 3n);
    usingView(nested.at(0n), (empty) => {
      assert.equal(empty.isNull(), false);
      assert.equal(empty.length(), 0n);
    });
    usingView(nested.at(1n), (nullList) =>
      assert.equal(nullList.isNull(), true),
    );
    usingView(nested.at(2n), (present) => {
      assert.equal(present.length(), 3n);
      usingView(present.at(0n), (emptyText) =>
        usingAccess(emptyText.acquire(), (access) =>
          assert.equal(access.str(), ''),
        ),
      );
      usingView(present.at(1n), (nullText) =>
        assert.equal(nullText.isNull(), true),
      );
      usingView(present.at(2n), (tail) =>
        usingAccess(tail.acquire(), (access) =>
          assert.equal(access.str(), 'tail'),
        ),
      );
    });
  });

  usingView(payload.entryView(1), (series) => {
    assert.equal(series.length(), 4n);
    usingView(series.at(0n), (nullList) =>
      assert.equal(nullList.isNull(), true),
    );
    usingView(series.at(1n), (empty) => {
      assert.equal(empty.isNull(), false);
      assert.equal(empty.length(), 0n);
    });
    usingView(series.at(2n), (values) => {
      assert.equal(values.length(), 3n);
      usingView(values.at(0n), (value) => assert.equal(value.getU8(), 0));
      usingView(values.at(1n), (value) => assert.equal(value.isNull(), true));
      usingView(values.at(2n), (value) =>
        assert.equal(value.getU8(), 0xff),
      );
    });
    usingView(series.at(3n), (values) =>
      usingView(values.at(0n), (value) => assert.equal(value.getU8(), 7)),
    );
  });

  assert.throws(
    () => root.getU8(),
    (error) =>
      requireError(
        error,
        2004,
        'TYPE_MISMATCH',
        '/entries/0/0',
        'Portable payload view kind does not match the operation',
        '{"reason":"view_kind_mismatch"}',
      ),
  );

  for (let index = 0; index < 1_000; index += 1) {
    const retained = root.clone();
    assert.equal(retained.fieldCount(), 14);
    retained.dispose();
  }

  const detached = root.materialize();
  textAccess.dispose();
  payload.invalidate();
  payload.dispose();
  spec.dispose();

  assert.throws(
    () => root.kind(),
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
  usingView(detached.field(1), (value) => assert.equal(value.getU8(), 0xab));
  usingView(detached.field(9), (detachedText) =>
    usingAccess(detachedText.acquire(), (access) =>
      assert.equal(access.str(), '\ufeffA\0B'),
    ),
  );

  detached.dispose();
  root.dispose();
  sequence.dispose();
});

test('view and access handles cannot be forged or reused after disposal', async () => {
  await initPayload();
  assert.throws(() => Reflect.construct(View, [1]), /created only/);
  assert.throws(() => Reflect.construct(Access, [1]), /created only/);
  for (const symbol of Object.getOwnPropertySymbols(View)) {
    const candidate = View[symbol];
    if (typeof candidate === 'function') {
      assert.throws(() => candidate.call(View, 1, Symbol()), /created only/);
    }
  }
  for (const symbol of Object.getOwnPropertySymbols(Access)) {
    const candidate = Access[symbol];
    if (typeof candidate === 'function') {
      assert.throws(() => candidate.call(Access, 1, Symbol()), /created only/);
    }
  }

  const forgedView = Object.create(View.prototype);
  const forgedAccess = Object.create(Access.prototype);
  assert.throws(() => forgedView.kind());
  assert.throws(() => forgedAccess.payloadBytes());

  const { spec, payload } = await recordPayload();
  const view = payload.entryView(0);
  const access = payload.acquire();
  view.dispose();
  access.dispose();
  assert.throws(() => view.kind(), /disposed/);
  assert.throws(() => access.payloadBytes(), /disposed/);
  payload.invalidate();
  payload.dispose();
  spec.dispose();
});
