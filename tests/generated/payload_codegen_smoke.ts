import {
  BuildPolicy,
  Builder,
  CompiledSpec,
  PayloadError,
  View,
  initPayload,
} from 'fastdb4ts/payload';
import {
  FdbTsType_fdb_ts_id_4974656d_View,
  compileSpec,
  fdb_ts_id_726f6f74_builder_entry_begin,
  fdb_ts_id_726f6f74_from_payload,
} from './generated.js';

const OTHER_SOURCE = new TextEncoder().encode(
  '{"schema":"fastdb.payload.v1","profile":"record.v1",' +
    '"entries":[{"id":"other","cardinality":"one",' +
    '"type":{"kind":"component","id":"Other"}}],' +
    '"components":[{"id":"Other","kind":"record","fields":[' +
    '{"id":"different","type":{"kind":"u8"}}]}]}',
);

function requireMismatch(callback: () => unknown, path: string): void {
  try {
    callback();
  } catch (error) {
    if (
      error instanceof PayloadError &&
      error.code === 3006 &&
      error.symbol === 'DIGEST_MISMATCH' &&
      error.path === path &&
      error.detailsJson.includes('"reason":"spec_digest_mismatch"')
    ) {
      return;
    }
    throw error;
  }
  throw new Error('generated guard accepted a foreign handle: ' + path);
}

await initPayload();
const wrongSpec = CompiledSpec.compile(OTHER_SOURCE);
const wrongBuilder = Builder.create(wrongSpec);
requireMismatch(
  () => fdb_ts_id_726f6f74_builder_entry_begin(wrongBuilder, 1n),
  '/builder/spec_sha256',
);
wrongBuilder.entryBegin(0, 1n).valueComponentBegin().valueU8(9);
const wrongPlan = wrongBuilder.freeze();
wrongBuilder.dispose();
const wrongPayload = wrongPlan.execute(BuildPolicy.AllowStaging).payload;
requireMismatch(
  () => fdb_ts_id_726f6f74_from_payload(wrongPayload),
  '/payload/spec_sha256',
);
const wrongEntry = wrongPayload.entryView(0);
let wrongView: View;
try {
  wrongView = wrongEntry.at(0n);
} finally {
  wrongEntry.dispose();
}
requireMismatch(
  () => FdbTsType_fdb_ts_id_4974656d_View.tryFromView(wrongView),
  '/view/spec_sha256',
);
wrongView.dispose();
wrongPayload.dispose();
wrongPlan.dispose();
wrongSpec.dispose();

const spec = compileSpec();
const builder = Builder.create(spec);
fdb_ts_id_726f6f74_builder_entry_begin(builder, 1n)
  .valueComponentBegin()
  .valueU8(7);
const plan = builder.freeze();
builder.dispose();
const payload = plan.execute(BuildPolicy.AllowStaging).payload;
const entry = fdb_ts_id_726f6f74_from_payload(payload);
const componentView = entry.at(0n);
const component =
  FdbTsType_fdb_ts_id_4974656d_View.tryFromView(componentView);
componentView.dispose();
if (component === undefined) {
  throw new Error('generated TypeScript component projection was absent');
}

const originalDispose = View.prototype.dispose;
const originalGetU8 = View.prototype.getU8;
let disposeCalls = 0;
View.prototype.dispose = function (this: View): void {
  disposeCalls += 1;
  originalDispose.call(this);
};
try {
  if (component.fdb_ts_id_76616c7565_value() !== 7) {
    throw new Error('generated TypeScript projection returned the wrong value');
  }
  if (Number(disposeCalls) !== 1) {
    throw new Error(
      'generated TypeScript scalar helper did not dispose its temporary View',
    );
  }

  View.prototype.getU8 = function (): number {
    throw new Error('injected generated getter failure');
  };
  let observedInjectedFailure = false;
  try {
    component.fdb_ts_id_76616c7565_value();
  } catch (error) {
    observedInjectedFailure =
      error instanceof Error &&
      error.message === 'injected generated getter failure';
  }
  if (!observedInjectedFailure) {
    throw new Error('generated TypeScript scalar helper hid getter failure');
  }
  if (Number(disposeCalls) !== 2) {
    throw new Error(
      'generated TypeScript scalar helper did not dispose after getter failure',
    );
  }
} finally {
  View.prototype.getU8 = originalGetU8;
  View.prototype.dispose = originalDispose;
}
if (component.fdb_ts_id_76616c7565_value() !== 7) {
  throw new Error('generated TypeScript projection returned the wrong value');
}
component.dispose();
entry.dispose();
payload.dispose();
plan.dispose();
spec.dispose();
