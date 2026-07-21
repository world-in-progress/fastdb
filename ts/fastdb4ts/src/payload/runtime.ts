import type {
  PayloadBackingStatsView,
  WxPayloadBackingHandle,
  WxPayloadOwnedBytesHandle,
} from '../wasm-loader.js';
import {
  POINTER_SIZE,
  SHA256_SIZE,
  checkedU32,
  checkedU64,
  copyBytes,
  payloadModule,
  readU32,
  readU64,
  withAllocation,
  withInputBytes,
  writeU32,
  writeU64,
} from './abi.js';
import { BuildPlan, buildPlanHandle } from './builder.js';
import { bindingError, checkStatus } from './error.js';
import { CompiledSpec, Profile, compiledSpecHandle } from './spec.js';

const OPEN_OPTIONS_SIZE = 112;
const EXECUTION_REPORT_SIZE = 72;
const EXECUTION_REPORT_OFFSET = 8;
const EXECUTION_ERROR_OFFSET = EXECUTION_REPORT_OFFSET + EXECUTION_REPORT_SIZE;
const EXECUTION_OUTPUT_SIZE = EXECUTION_ERROR_OFFSET + POINTER_SIZE;

export enum BuildPolicy {
  AllowStaging = 1,
  RequireDirect = 2,
}

export enum ExecutionMode {
  Direct = 1,
  Staged = 2,
}

export enum FallbackReason {
  None = 0,
  PlanRequiresStaging = 1,
  BackingDeclinedDirect = 2,
}

export interface PayloadBackingStats {
  readonly reservations: bigint;
  readonly writes: bigint;
  readonly commits: bigint;
  readonly rollbacks: bigint;
  readonly retains: bigint;
  readonly releases: bigint;
}

export class ExecutionReport {
  constructor(
    readonly mode: ExecutionMode,
    readonly fallbackReason: FallbackReason,
    readonly requestedBytes: bigint,
    readonly usedBytes: bigint,
    readonly stagingBytes: bigint,
    readonly regionCount: bigint,
    readonly backingCapacity: bigint,
  ) {}
}

export interface OpenOptionValues {
  flags?: number;
  maxTotalBytes?: bigint;
  maxRegions?: bigint;
  maxEntries?: bigint;
  maxComponents?: bigint;
  maxNestingDepth?: bigint;
  maxListElements?: bigint;
  maxGraphObjects?: bigint;
  maxStringBytes?: bigint;
  maxValidationWork?: bigint;
}

export class OpenOptions {
  readonly values: Readonly<OpenOptionValues>;

  constructor(values: OpenOptionValues = {}) {
    this.values = Object.freeze({ ...values });
  }
}

export interface WasmMemoryBackingOptions {
  allowDirect?: boolean;
}

interface TestingBackingOptions extends WasmMemoryBackingOptions {
  failWrite?: boolean;
  failCommit?: boolean;
}

const testingBackingToken = Symbol('fastdb.payload.testingBackingToken');
const createTestingBacking = Symbol('fastdb.payload.createTestingBacking');
const backingHandleValue = Symbol('fastdb.payload.backingHandleValue');

const backingFinalizer = new FinalizationRegistry<WxPayloadBackingHandle>(
  (handle) => {
    try {
      handle.delete();
    } catch {
      // Explicit disposal is authoritative; finalization is only a fallback.
    }
  },
);

export class WasmMemoryBacking {
  #native: WxPayloadBackingHandle | null;
  readonly #finalizerToken = {};

  constructor(options: WasmMemoryBackingOptions = {}) {
    if (
      options.allowDirect !== undefined &&
      typeof options.allowDirect !== 'boolean'
    ) {
      throw new TypeError('allowDirect must be boolean');
    }
    this.#native = new (payloadModule().WxPayloadBacking)(
      options.allowDirect ?? true,
      false,
      false,
    );
    backingFinalizer.register(this, this.#native, this.#finalizerToken);
  }

  static [createTestingBacking](
    options: TestingBackingOptions,
    token: typeof testingBackingToken,
  ): WasmMemoryBacking {
    if (token !== testingBackingToken) {
      throw new TypeError('testing backings are created only by FastDB tests');
    }
    const backing = new WasmMemoryBacking({
      allowDirect: options.allowDirect,
    });
    backing.replaceNative(
      options.allowDirect ?? true,
      options.failWrite === true,
      options.failCommit === true,
    );
    return backing;
  }

  stats(): PayloadBackingStats {
    const raw: PayloadBackingStatsView = this.requireNative().stats();
    return {
      reservations: raw.reservations,
      writes: raw.writes,
      commits: raw.commits,
      rollbacks: raw.rollbacks,
      retains: raw.retains,
      releases: raw.releases,
    };
  }

  dispose(): void {
    if (this.#native !== null) {
      const native = this.#native;
      this.#native = null;
      backingFinalizer.unregister(this.#finalizerToken);
      native.delete();
    }
  }

  /** @internal */
  [backingHandleValue](): WxPayloadBackingHandle {
    return this.requireNative();
  }

  private requireNative(): WxPayloadBackingHandle {
    if (this.#native === null) {
      throw bindingError('WasmMemoryBacking is disposed', 'disposed_handle');
    }
    return this.#native;
  }

  private replaceNative(
    allowDirect: boolean,
    failWrite: boolean,
    failCommit: boolean,
  ): void {
    const previous = this.requireNative();
    const replacement = new (payloadModule().WxPayloadBacking)(
      allowDirect,
      failWrite,
      failCommit,
    );
    backingFinalizer.unregister(this.#finalizerToken);
    previous.delete();
    this.#native = replacement;
    backingFinalizer.register(this, this.#native, this.#finalizerToken);
  }
}

export function __testingWasmMemoryBacking(
  options: TestingBackingOptions,
): WasmMemoryBacking {
  return WasmMemoryBacking[createTestingBacking](options, testingBackingToken);
}

const ownedBytesHandleValue = Symbol('fastdb.payload.ownedBytesHandleValue');
const ownedBytesConstructionToken = Symbol(
  'fastdb.payload.ownedBytesConstructionToken',
);
const ownedBytesFinalizer = new FinalizationRegistry<WxPayloadOwnedBytesHandle>(
  (handle) => {
    try {
      handle.delete();
    } catch {
      // Explicit disposal is authoritative; finalization is only a fallback.
    }
  },
);

export class WasmOwnedBytes {
  #native: WxPayloadOwnedBytesHandle | null;
  readonly #finalizerToken = {};

  private constructor(
    native: WxPayloadOwnedBytesHandle,
    token: typeof ownedBytesConstructionToken,
  ) {
    if (token !== ownedBytesConstructionToken) {
      throw new TypeError('WasmOwnedBytes values are created only by FastDB');
    }
    this.#native = native;
    ownedBytesFinalizer.register(this, native, this.#finalizerToken);
  }

  static copyFrom(source: Uint8Array): WasmOwnedBytes {
    if (!(source instanceof Uint8Array)) {
      throw new TypeError('source must be Uint8Array');
    }
    const module = payloadModule();
    return withInputBytes(module, source, (pointer) =>
      new WasmOwnedBytes(
        new module.WxPayloadOwnedBytes(pointer, source.byteLength),
        ownedBytesConstructionToken,
      ),
    );
  }

  dispose(): void {
    if (this.#native !== null) {
      const native = this.#native;
      this.#native = null;
      ownedBytesFinalizer.unregister(this.#finalizerToken);
      native.delete();
    }
  }

  /** @internal */
  [ownedBytesHandleValue](): WxPayloadOwnedBytesHandle {
    if (this.#native === null) {
      throw bindingError('WasmOwnedBytes is disposed', 'disposed_handle');
    }
    return this.#native;
  }
}

const payloadConstructionToken = Symbol('fastdb.payload.payloadConstructionToken');
const createPayload = Symbol('fastdb.payload.createPayload');

const payloadFinalizer = new FinalizationRegistry<number>((handle) => {
  try {
    payloadModule()._fdb_payload_v1_payload_release(handle);
  } catch {
    // Explicit disposal is authoritative; finalization is only a fallback.
  }
});

export class Payload {
  #handle: number;
  #keeper: object | undefined;
  readonly #finalizerToken = {};

  private constructor(
    handle: number,
    token: typeof payloadConstructionToken,
    keeper?: object,
  ) {
    if (token !== payloadConstructionToken) {
      throw new TypeError('Payload handles are created only by FastDB');
    }
    if (handle === 0) {
      throw bindingError(
        'FastDB Core returned success without a payload',
        'missing_payload_handle',
      );
    }
    this.#handle = handle;
    this.#keeper = keeper;
    payloadFinalizer.register(this, handle, this.#finalizerToken);
  }

  static [createPayload](
    handle: number,
    token: typeof payloadConstructionToken,
    keeper?: object,
  ): Payload {
    if (token !== payloadConstructionToken) {
      throw new TypeError('Payload handles are created only by FastDB');
    }
    return new Payload(handle, payloadConstructionToken, keeper);
  }

  static openCopy(
    spec: CompiledSpec,
    source: Uint8Array,
    options = new OpenOptions(),
  ): Payload {
    if (!(source instanceof Uint8Array)) {
      throw new TypeError('source must be Uint8Array');
    }
    const module = payloadModule();
    return withOpenOptions(module, options, (optionsPointer) =>
      withInputBytes(module, source, (bytes, byteCount) =>
        withAllocation(module, POINTER_SIZE * 2, (outputs) => {
          const status = module._fdb_payload_v1_payload_open_copy(
            compiledSpecHandle(spec),
            bytes,
            byteCount,
            optionsPointer,
            outputs,
            outputs + POINTER_SIZE,
          );
          checkStatus(status, outputs + POINTER_SIZE);
          return new Payload(
            readU32(module, outputs),
            payloadConstructionToken,
          );
        }),
      ),
    );
  }

  static openWasmOwned(
    spec: CompiledSpec,
    source: WasmOwnedBytes,
    options = new OpenOptions(),
  ): Payload {
    const module = payloadModule();
    const native = ownedBytesNative(source);
    return withOpenOptions(module, options, (optionsPointer) =>
      withAllocation(module, POINTER_SIZE * 2, (outputs) => {
        const status = module._fdb_payload_v1_payload_open_external(
          compiledSpecHandle(spec),
          native.dataAddress(),
          BigInt(native.size()),
          native.backingAddress(),
          native.ownerToken(),
          optionsPointer,
          outputs,
          outputs + POINTER_SIZE,
        );
        checkStatus(status, outputs + POINTER_SIZE);
        return new Payload(
          readU32(module, outputs),
          payloadConstructionToken,
          source,
        );
      }),
    );
  }

  clone(): Payload {
    const handle = this.requireHandle();
    payloadModule()._fdb_payload_v1_payload_retain(handle);
    return new Payload(handle, payloadConstructionToken, this.#keeper);
  }

  dispose(): void {
    if (this.#handle !== 0) {
      const handle = this.#handle;
      this.#handle = 0;
      payloadFinalizer.unregister(this.#finalizerToken);
      payloadModule()._fdb_payload_v1_payload_release(handle);
      this.#keeper = undefined;
    }
  }

  sha256(): Uint8Array {
    const module = payloadModule();
    return withAllocation(module, SHA256_SIZE + POINTER_SIZE, (outputs) => {
      const status = module._fdb_payload_v1_payload_sha256(
        this.requireHandle(),
        outputs,
        outputs + SHA256_SIZE,
      );
      checkStatus(status, outputs + SHA256_SIZE);
      return module.HEAPU8.slice(outputs, outputs + SHA256_SIZE);
    });
  }

  profile(): Profile {
    const module = payloadModule();
    return withAllocation(module, POINTER_SIZE * 2, (outputs) => {
      const status = module._fdb_payload_v1_payload_profile(
        this.requireHandle(),
        outputs,
        outputs + POINTER_SIZE,
      );
      checkStatus(status, outputs + POINTER_SIZE);
      const value = readU32(module, outputs);
      if (value !== Profile.RecordV1 && value !== Profile.ObjectGraphV1) {
        throw bindingError(
          'FastDB Core returned an unknown payload profile',
          'unknown_profile',
        );
      }
      return value;
    });
  }

  executionReport(): ExecutionReport {
    const module = payloadModule();
    return withAllocation(
      module,
      EXECUTION_REPORT_SIZE + POINTER_SIZE,
      (outputs) => {
        module._fdb_payload_v1_execution_report_init(outputs);
        const status = module._fdb_payload_v1_payload_execution_report(
          this.requireHandle(),
          outputs,
          outputs + EXECUTION_REPORT_SIZE,
        );
        checkStatus(status, outputs + EXECUTION_REPORT_SIZE);
        return readExecutionReport(module, outputs);
      },
    );
  }

  binaryBytes(): Uint8Array {
    const module = payloadModule();
    return withAllocation(module, POINTER_SIZE * 2, (outputs) => {
      const status = module._fdb_payload_v1_payload_binary_blob(
        this.requireHandle(),
        outputs,
        outputs + POINTER_SIZE,
      );
      checkStatus(status, outputs + POINTER_SIZE);
      const blob = readU32(module, outputs);
      if (blob === 0) {
        throw bindingError(
          'FastDB Core returned success without a blob',
          'missing_blob_handle',
        );
      }
      try {
        return copyBytes(
          module,
          module._fdb_payload_v1_blob_data(blob),
          module._fdb_payload_v1_blob_size(blob),
        );
      } finally {
        module._fdb_payload_v1_blob_release(blob);
      }
    });
  }

  private requireHandle(): number {
    if (this.#handle === 0) {
      throw bindingError('Payload is disposed', 'disposed_handle');
    }
    return this.#handle;
  }
}

export class BuildResult {
  constructor(
    readonly payload: Payload,
    readonly report: ExecutionReport,
  ) {}
}

export function executePlan(
  plan: BuildPlan,
  policy: BuildPolicy,
  backing?: WasmMemoryBacking,
): BuildResult {
  if (
    policy !== BuildPolicy.AllowStaging &&
    policy !== BuildPolicy.RequireDirect
  ) {
    throw new TypeError('policy must be BuildPolicy');
  }
  const module = payloadModule();
  const nativeBacking = backing === undefined ? undefined : backingNative(backing);
  return withAllocation(module, EXECUTION_OUTPUT_SIZE, (outputs) => {
    module._fdb_payload_v1_execution_report_init(
      outputs + EXECUTION_REPORT_OFFSET,
    );
    const status = module._fdb_payload_v1_plan_execute(
      buildPlanHandle(plan),
      policy,
      nativeBacking?.backingAddress() ?? 0,
      outputs,
      outputs + EXECUTION_REPORT_OFFSET,
      outputs + EXECUTION_ERROR_OFFSET,
    );
    checkStatus(status, outputs + EXECUTION_ERROR_OFFSET);
    const payload = Payload[createPayload](
      readU32(module, outputs),
      payloadConstructionToken,
      backing,
    );
    try {
      return new BuildResult(
        payload,
        readExecutionReport(module, outputs + EXECUTION_REPORT_OFFSET),
      );
    } catch (error) {
      payload.dispose();
      throw error;
    }
  });
}

function backingNative(backing: WasmMemoryBacking): WxPayloadBackingHandle {
  if (!(backing instanceof WasmMemoryBacking)) {
    throw new TypeError('backing must be WasmMemoryBacking');
  }
  return WasmMemoryBacking.prototype[backingHandleValue].call(backing);
}

function ownedBytesNative(source: WasmOwnedBytes): WxPayloadOwnedBytesHandle {
  if (!(source instanceof WasmOwnedBytes)) {
    throw new TypeError('source must be WasmOwnedBytes');
  }
  return WasmOwnedBytes.prototype[ownedBytesHandleValue].call(source);
}

function withOpenOptions<T>(
  module: ReturnType<typeof payloadModule>,
  options: OpenOptions,
  operation: (pointer: number) => T,
): T {
  if (!(options instanceof OpenOptions)) {
    throw new TypeError('options must be OpenOptions');
  }
  return withAllocation(module, OPEN_OPTIONS_SIZE, (pointer) => {
    module._fdb_payload_v1_open_options_init(pointer);
    const values = options.values;
    if (values.flags !== undefined) {
      writeU32(module, pointer + 4, checkedU32(values.flags, 'flags'));
    }
    const fields: Array<[bigint | undefined, number, string]> = [
      [values.maxTotalBytes, 8, 'maxTotalBytes'],
      [values.maxRegions, 16, 'maxRegions'],
      [values.maxEntries, 24, 'maxEntries'],
      [values.maxComponents, 32, 'maxComponents'],
      [values.maxNestingDepth, 40, 'maxNestingDepth'],
      [values.maxListElements, 48, 'maxListElements'],
      [values.maxGraphObjects, 56, 'maxGraphObjects'],
      [values.maxStringBytes, 64, 'maxStringBytes'],
      [values.maxValidationWork, 72, 'maxValidationWork'],
    ];
    for (const [value, offset, name] of fields) {
      if (value !== undefined) {
        writeU64(module, pointer + offset, checkedU64(value, name));
      }
    }
    return operation(pointer);
  });
}

function readExecutionReport(
  module: ReturnType<typeof payloadModule>,
  pointer: number,
): ExecutionReport {
  const mode = readU32(module, pointer + 4);
  const fallback = readU32(module, pointer + 8);
  if (mode !== ExecutionMode.Direct && mode !== ExecutionMode.Staged) {
    throw bindingError(
      'FastDB Core returned an unknown execution mode',
      'unknown_execution_mode',
    );
  }
  if (
    fallback !== FallbackReason.None &&
    fallback !== FallbackReason.PlanRequiresStaging &&
    fallback !== FallbackReason.BackingDeclinedDirect
  ) {
    throw bindingError(
      'FastDB Core returned an unknown fallback reason',
      'unknown_fallback_reason',
    );
  }
  return new ExecutionReport(
    mode,
    fallback,
    readU64(module, pointer + 16),
    readU64(module, pointer + 24),
    readU64(module, pointer + 32),
    readU64(module, pointer + 40),
    readU64(module, pointer + 48),
  );
}
