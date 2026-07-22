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
  f32FromBits,
  f64FromBits,
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

export enum ViewKind {
  Sequence = 1,
  Bool = 2,
  U8 = 3,
  U16 = 4,
  U32 = 5,
  I32 = 6,
  U8n = 7,
  U16n = 8,
  F32 = 9,
  F64 = 10,
  Str = 11,
  Wstr = 12,
  Bytes = 13,
  Component = 14,
  List = 15,
  Ref = 16,
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

const SPAN_SIZE_OFFSET = 8;
const SPAN_ERROR_OFFSET = 16;
const SPAN_OUTPUT_SIZE = SPAN_ERROR_OFFSET + POINTER_SIZE;
// `ignoreBOM: true` makes a leading U+FEFF ordinary user content instead of
// silently stripping it; FastDB Core already owns encoding validation.
const utf8Decoder = new TextDecoder('utf-8', {
  fatal: true,
  ignoreBOM: true,
});
const utf16Decoder = new TextDecoder('utf-16le', {
  fatal: true,
  ignoreBOM: true,
});

type AccessSpanCall = (
  access: number,
  outData: number,
  outSize: number,
  outError: number,
) => number;

const accessConstructionToken = Symbol(
  'fastdb.payload.accessConstructionToken',
);
const createAccess = Symbol('fastdb.payload.createAccess');
const accessFinalizer = new FinalizationRegistry<number>((handle) => {
  try {
    payloadModule()._fdb_payload_v1_access_release(handle);
  } catch {
    // Explicit disposal is authoritative; finalization is only a fallback.
  }
});

/** A unique Core access pin. Safe methods always return JavaScript-owned copies. */
export class Access {
  #handle: number;
  readonly #finalizerToken = {};

  private constructor(
    handle: number,
    token: typeof accessConstructionToken,
  ) {
    if (token !== accessConstructionToken) {
      throw new TypeError('Access handles are created only by FastDB');
    }
    if (handle === 0) {
      throw bindingError(
        'FastDB Core returned success without an access pin',
        'missing_access_handle',
      );
    }
    this.#handle = handle;
    accessFinalizer.register(this, handle, this.#finalizerToken);
  }

  static [createAccess](
    handle: number,
    token: typeof accessConstructionToken,
  ): Access {
    if (token !== accessConstructionToken) {
      throw new TypeError('Access handles are created only by FastDB');
    }
    try {
      return new Access(handle, accessConstructionToken);
    } catch (error) {
      if (handle !== 0) {
        payloadModule()._fdb_payload_v1_access_release(handle);
      }
      throw error;
    }
  }

  dispose(): void {
    if (this.#handle !== 0) {
      const handle = this.#handle;
      this.#handle = 0;
      accessFinalizer.unregister(this.#finalizerToken);
      payloadModule()._fdb_payload_v1_access_release(handle);
    }
  }

  payloadBytes(): Uint8Array {
    const module = payloadModule();
    return this.copyByteSpan((access, data, size, error) =>
      module._fdb_payload_v1_access_payload_bytes(
        access,
        data,
        size,
        error,
      ),
    );
  }

  str(): string {
    const module = payloadModule();
    const bytes = this.copyByteSpan((access, data, size, error) =>
      module._fdb_payload_v1_access_str(access, data, size, error),
    );
    try {
      return utf8Decoder.decode(bytes);
    } catch (error) {
      throw bindingError(
        'FastDB Core returned invalid UTF-8 from a validated str view',
        'invalid_core_utf8',
      );
    }
  }

  wstr(): string {
    const module = payloadModule();
    const bytes = withAllocation(module, SPAN_OUTPUT_SIZE, (outputs) => {
      const status = module._fdb_payload_v1_access_wstr(
        this.requireHandle(),
        outputs,
        outputs + SPAN_SIZE_OFFSET,
        outputs + SPAN_ERROR_OFFSET,
      );
      checkStatus(status, outputs + SPAN_ERROR_OFFSET);
      const units = readU64(module, outputs + SPAN_SIZE_OFFSET);
      if (units > BigInt(Number.MAX_SAFE_INTEGER) / 2n) {
        throw bindingError(
          'FastDB Core wstr span exceeds the JavaScript address space',
          'host_size_overflow',
        );
      }
      const data = readU32(module, outputs);
      if (units !== 0n && data % 2 !== 0) {
        throw bindingError(
          'FastDB Core returned unaligned wstr storage',
          'invalid_core_wstr_span',
        );
      }
      return copyBytes(module, data, units * 2n);
    });
    try {
      // wasm32 is little-endian; Core already projected aligned host uint16s.
      return utf16Decoder.decode(bytes);
    } catch (error) {
      throw bindingError(
        'FastDB Core returned invalid UTF-16 from a validated wstr view',
        'invalid_core_utf16',
      );
    }
  }

  bytes(): Uint8Array {
    const module = payloadModule();
    return this.copyByteSpan((access, data, size, error) =>
      module._fdb_payload_v1_access_bytes(access, data, size, error),
    );
  }

  private copyByteSpan(call: AccessSpanCall): Uint8Array {
    const module = payloadModule();
    return withAllocation(module, SPAN_OUTPUT_SIZE, (outputs) => {
      const status = call(
        this.requireHandle(),
        outputs,
        outputs + SPAN_SIZE_OFFSET,
        outputs + SPAN_ERROR_OFFSET,
      );
      checkStatus(status, outputs + SPAN_ERROR_OFFSET);
      return copyBytes(
        module,
        readU32(module, outputs),
        readU64(module, outputs + SPAN_SIZE_OFFSET),
      );
    });
  }

  private requireHandle(): number {
    if (this.#handle === 0) {
      throw bindingError('Access is disposed', 'disposed_handle');
    }
    return this.#handle;
  }
}

type ViewScalarCall = (
  view: number,
  outValue: number,
  outError: number,
) => number;

const viewConstructionToken = Symbol('fastdb.payload.viewConstructionToken');
const createView = Symbol('fastdb.payload.createView');
const viewFinalizer = new FinalizationRegistry<number>((handle) => {
  try {
    payloadModule()._fdb_payload_v1_view_release(handle);
  } catch {
    // Explicit disposal is authoritative; finalization is only a fallback.
  }
});

/** A retainable immutable Core view with generation-checked operations. */
export class View {
  #handle: number;
  readonly #finalizerToken = {};

  private constructor(handle: number, token: typeof viewConstructionToken) {
    if (token !== viewConstructionToken) {
      throw new TypeError('View handles are created only by FastDB');
    }
    if (handle === 0) {
      throw bindingError(
        'FastDB Core returned success without a view',
        'missing_view_handle',
      );
    }
    this.#handle = handle;
    viewFinalizer.register(this, handle, this.#finalizerToken);
  }

  static [createView](
    handle: number,
    token: typeof viewConstructionToken,
  ): View {
    if (token !== viewConstructionToken) {
      throw new TypeError('View handles are created only by FastDB');
    }
    try {
      return new View(handle, viewConstructionToken);
    } catch (error) {
      if (handle !== 0) {
        payloadModule()._fdb_payload_v1_view_release(handle);
      }
      throw error;
    }
  }

  clone(): View {
    const handle = this.requireHandle();
    payloadModule()._fdb_payload_v1_view_retain(handle);
    return View[createView](handle, viewConstructionToken);
  }

  dispose(): void {
    if (this.#handle !== 0) {
      const handle = this.#handle;
      this.#handle = 0;
      viewFinalizer.unregister(this.#finalizerToken);
      payloadModule()._fdb_payload_v1_view_release(handle);
    }
  }

  kind(): ViewKind {
    const value = this.readU32((view, output, error) =>
      payloadModule()._fdb_payload_v1_view_kind(view, output, error),
    );
    if (value < ViewKind.Sequence || value > ViewKind.Ref) {
      throw bindingError(
        'FastDB Core returned an unknown payload view kind',
        'unknown_view_kind',
      );
    }
    return value as ViewKind;
  }

  isNull(): boolean {
    return (
      this.readU8((view, output, error) =>
        payloadModule()._fdb_payload_v1_view_is_null(view, output, error),
      ) !== 0
    );
  }

  length(): bigint {
    return this.readU64((view, output, error) =>
      payloadModule()._fdb_payload_v1_view_length(view, output, error),
    );
  }

  at(index: bigint): View {
    const module = payloadModule();
    return withAllocation(module, POINTER_SIZE * 2, (outputs) => {
      const status = module._fdb_payload_v1_view_at(
        this.requireHandle(),
        checkedU64(index, 'index'),
        outputs,
        outputs + POINTER_SIZE,
      );
      checkStatus(status, outputs + POINTER_SIZE);
      return View[createView](
        readU32(module, outputs),
        viewConstructionToken,
      );
    });
  }

  componentIndex(): number {
    return this.readU32((view, output, error) =>
      payloadModule()._fdb_payload_v1_view_component_index(
        view,
        output,
        error,
      ),
    );
  }

  fieldCount(): number {
    return this.readU32((view, output, error) =>
      payloadModule()._fdb_payload_v1_view_field_count(view, output, error),
    );
  }

  field(index: number): View {
    const module = payloadModule();
    return withAllocation(module, POINTER_SIZE * 2, (outputs) => {
      const status = module._fdb_payload_v1_view_field(
        this.requireHandle(),
        checkedU32(index, 'index'),
        outputs,
        outputs + POINTER_SIZE,
      );
      checkStatus(status, outputs + POINTER_SIZE);
      return View[createView](
        readU32(module, outputs),
        viewConstructionToken,
      );
    });
  }

  getBool(): boolean {
    return (
      this.readU8((view, output, error) =>
        payloadModule()._fdb_payload_v1_view_get_bool(view, output, error),
      ) !== 0
    );
  }

  getU8(): number {
    return this.readU8((view, output, error) =>
      payloadModule()._fdb_payload_v1_view_get_u8(view, output, error),
    );
  }

  getU16(): number {
    return this.readU16((view, output, error) =>
      payloadModule()._fdb_payload_v1_view_get_u16(view, output, error),
    );
  }

  getU32(): number {
    return this.readU32((view, output, error) =>
      payloadModule()._fdb_payload_v1_view_get_u32(view, output, error),
    );
  }

  getI32(): number {
    const module = payloadModule();
    return withAllocation(module, POINTER_SIZE * 2, (outputs) => {
      const status = module._fdb_payload_v1_view_get_i32(
        this.requireHandle(),
        outputs,
        outputs + POINTER_SIZE,
      );
      checkStatus(status, outputs + POINTER_SIZE);
      return new DataView(module.HEAPU8.buffer).getInt32(outputs, true);
    });
  }

  getU8nF64Bits(): bigint {
    return this.readU64((view, output, error) =>
      payloadModule()._fdb_payload_v1_view_get_u8n_f64_bits(
        view,
        output,
        error,
      ),
    );
  }

  getU8n(): number {
    return f64FromBits(this.getU8nF64Bits());
  }

  getU16nF64Bits(): bigint {
    return this.readU64((view, output, error) =>
      payloadModule()._fdb_payload_v1_view_get_u16n_f64_bits(
        view,
        output,
        error,
      ),
    );
  }

  getU16n(): number {
    return f64FromBits(this.getU16nF64Bits());
  }

  getF32Bits(): number {
    return this.readU32((view, output, error) =>
      payloadModule()._fdb_payload_v1_view_get_f32_bits(view, output, error),
    );
  }

  getF32(): number {
    return f32FromBits(this.getF32Bits());
  }

  getF64Bits(): bigint {
    return this.readU64((view, output, error) =>
      payloadModule()._fdb_payload_v1_view_get_f64_bits(view, output, error),
    );
  }

  getF64(): number {
    return f64FromBits(this.getF64Bits());
  }

  acquire(): Access {
    const module = payloadModule();
    return withAllocation(module, POINTER_SIZE * 2, (outputs) => {
      const status = module._fdb_payload_v1_view_acquire(
        this.requireHandle(),
        outputs,
        outputs + POINTER_SIZE,
      );
      checkStatus(status, outputs + POINTER_SIZE);
      return Access[createAccess](
        readU32(module, outputs),
        accessConstructionToken,
      );
    });
  }

  /** Performs exactly one Core materialization call. */
  materialize(): View {
    const module = payloadModule();
    return withAllocation(module, POINTER_SIZE * 2, (outputs) => {
      const status = module._fdb_payload_v1_view_materialize(
        this.requireHandle(),
        outputs,
        outputs + POINTER_SIZE,
      );
      checkStatus(status, outputs + POINTER_SIZE);
      return View[createView](
        readU32(module, outputs),
        viewConstructionToken,
      );
    });
  }

  private readU8(call: ViewScalarCall): number {
    const module = payloadModule();
    return withAllocation(module, POINTER_SIZE * 2, (outputs) => {
      const status = call(
        this.requireHandle(),
        outputs,
        outputs + POINTER_SIZE,
      );
      checkStatus(status, outputs + POINTER_SIZE);
      return module.HEAPU8[outputs];
    });
  }

  private readU16(call: ViewScalarCall): number {
    const module = payloadModule();
    return withAllocation(module, POINTER_SIZE * 2, (outputs) => {
      const status = call(
        this.requireHandle(),
        outputs,
        outputs + POINTER_SIZE,
      );
      checkStatus(status, outputs + POINTER_SIZE);
      return new DataView(module.HEAPU8.buffer).getUint16(outputs, true);
    });
  }

  private readU32(call: ViewScalarCall): number {
    const module = payloadModule();
    return withAllocation(module, POINTER_SIZE * 2, (outputs) => {
      const status = call(
        this.requireHandle(),
        outputs,
        outputs + POINTER_SIZE,
      );
      checkStatus(status, outputs + POINTER_SIZE);
      return readU32(module, outputs);
    });
  }

  private readU64(call: ViewScalarCall): bigint {
    const module = payloadModule();
    return withAllocation(module, 8 + POINTER_SIZE, (outputs) => {
      const status = call(this.requireHandle(), outputs, outputs + 8);
      checkStatus(status, outputs + 8);
      return readU64(module, outputs);
    });
  }

  private requireHandle(): number {
    if (this.#handle === 0) {
      throw bindingError('View is disposed', 'disposed_handle');
    }
    return this.#handle;
  }
}

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

  acquire(): Access {
    const module = payloadModule();
    return withAllocation(module, POINTER_SIZE * 2, (outputs) => {
      const status = module._fdb_payload_v1_payload_acquire(
        this.requireHandle(),
        outputs,
        outputs + POINTER_SIZE,
      );
      checkStatus(status, outputs + POINTER_SIZE);
      return Access[createAccess](
        readU32(module, outputs),
        accessConstructionToken,
      );
    });
  }

  entryView(entryIndex: number): View {
    const module = payloadModule();
    return withAllocation(module, POINTER_SIZE * 2, (outputs) => {
      const status = module._fdb_payload_v1_payload_entry_view(
        this.requireHandle(),
        checkedU32(entryIndex, 'entryIndex'),
        outputs,
        outputs + POINTER_SIZE,
      );
      checkStatus(status, outputs + POINTER_SIZE);
      return View[createView](readU32(module, outputs), viewConstructionToken);
    });
  }

  /**
   * Invalidates synchronously after all access pins have been disposed.
   * The official Wasm host is single-threaded, so retaining an Access here
   * would prevent Core's drain from completing.
   */
  invalidate(): void {
    const module = payloadModule();
    withAllocation(module, POINTER_SIZE, (error) => {
      const status = module._fdb_payload_v1_payload_invalidate(
        this.requireHandle(),
        error,
      );
      checkStatus(status, error);
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
