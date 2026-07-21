import {
  POINTER_SIZE,
  checkedI32,
  checkedU8,
  checkedU16,
  checkedU32,
  checkedU64,
  f32Bits,
  f64Bits,
  payloadModule,
  readU32,
  readU64,
  withAllocation,
  withInputBytes,
  withInputU16,
  writeU32,
  writeU64,
} from './abi.js';
import { bindingError, checkStatus } from './error.js';
import { CompiledSpec, compiledSpecHandle } from './spec.js';
import {
  executePlan,
  type BuildPolicy,
  type BuildResult,
  type WasmMemoryBacking,
} from './runtime.js';

const BUILDER_OPTIONS_SIZE = 96;
const FIXED_RUN_SIZE = 96;
const PLAN_INFO_SIZE = 112;

const encoder = new TextEncoder();
const handleConstructionToken = Symbol('fastdb.payload.handleConstructionToken');
const objectHandleValue = Symbol('fastdb.payload.objectHandleValue');
const createObjectHandle = Symbol('fastdb.payload.createObjectHandle');
const createBuildPlan = Symbol('fastdb.payload.createBuildPlan');
const planHandleValue = Symbol('fastdb.payload.planHandleValue');

export interface BuilderOptions {
  flags?: number;
  maxValueNodes?: bigint;
  maxListElements?: bigint;
  maxTextBytes?: bigint;
  maxOpaqueBytes?: bigint;
  maxNestingDepth?: bigint;
  maxTotalBuilderBytes?: bigint;
  maxGraphObjects?: bigint;
}

export class FixedRun {
  readonly data: Uint8Array;
  readonly count: bigint;
  readonly strideBytes: bigint;
  readonly validity?: Uint8Array;
  readonly validityBitOffset: bigint;

  constructor(
    data: Uint8Array,
    count: bigint,
    strideBytes: bigint,
    validity?: Uint8Array,
    validityBitOffset = 0n,
  ) {
    this.data = data.slice();
    this.count = checkedU64(count, 'count');
    this.strideBytes = checkedU64(strideBytes, 'strideBytes');
    this.validity = validity?.slice();
    this.validityBitOffset = checkedU64(
      validityBitOffset,
      'validityBitOffset',
    );
  }
}

export class ObjectHandle {
  readonly #raw: bigint;

  private constructor(
    raw: bigint,
    token: typeof handleConstructionToken,
  ) {
    if (token !== handleConstructionToken) {
      throw new TypeError('ObjectHandle values are created only by FastDB');
    }
    this.#raw = raw;
  }

  static [createObjectHandle](
    raw: bigint,
    token: typeof handleConstructionToken,
  ): ObjectHandle {
    if (token !== handleConstructionToken) {
      throw new TypeError('ObjectHandle values are created only by FastDB');
    }
    if (raw === 0n) {
      throw bindingError(
        'FastDB Core returned success without an object handle',
        'missing_object_handle',
      );
    }
    return new ObjectHandle(raw, handleConstructionToken);
  }

  /** @internal */
  [objectHandleValue](): bigint {
    return this.#raw;
  }
}

export class PlanInfo {
  constructor(
    readonly flags: number,
    readonly totalBytes: bigint,
    readonly regionCount: bigint,
    readonly logicalValueCount: bigint,
    readonly listElementCount: bigint,
    readonly textBytes: bigint,
    readonly opaqueBytes: bigint,
    readonly validationWork: bigint,
    readonly maxAlignment: number,
    readonly directBuildStatus: number,
    readonly graphObjectCount: bigint,
  ) {}
}

const planFinalizer = new FinalizationRegistry<number>((handle) => {
  try {
    payloadModule()._fdb_payload_v1_plan_release(handle);
  } catch {
    // Explicit disposal is authoritative; finalization is only a fallback.
  }
});

export class BuildPlan {
  #handle: number;
  readonly #finalizerToken = {};

  private constructor(handle: number, token: typeof handleConstructionToken) {
    if (token !== handleConstructionToken) {
      throw new TypeError('BuildPlan handles are created only by FastDB');
    }
    if (handle === 0) {
      throw bindingError(
        'FastDB Core returned success without a build plan',
        'missing_plan_handle',
      );
    }
    this.#handle = handle;
    planFinalizer.register(this, handle, this.#finalizerToken);
  }

  static [createBuildPlan](
    handle: number,
    token: typeof handleConstructionToken,
  ): BuildPlan {
    if (token !== handleConstructionToken) {
      throw new TypeError('BuildPlan handles are created only by FastDB');
    }
    return new BuildPlan(handle, handleConstructionToken);
  }

  clone(): BuildPlan {
    const handle = this.requireHandle();
    payloadModule()._fdb_payload_v1_plan_retain(handle);
    return new BuildPlan(handle, handleConstructionToken);
  }

  dispose(): void {
    if (this.#handle !== 0) {
      const handle = this.#handle;
      this.#handle = 0;
      planFinalizer.unregister(this.#finalizerToken);
      payloadModule()._fdb_payload_v1_plan_release(handle);
    }
  }

  info(): PlanInfo {
    const module = payloadModule();
    return withAllocation(module, PLAN_INFO_SIZE + POINTER_SIZE, (outputs) => {
      module._fdb_payload_v1_plan_info_init(outputs);
      const status = module._fdb_payload_v1_plan_info(
        this.requireHandle(),
        outputs,
        outputs + PLAN_INFO_SIZE,
      );
      checkStatus(status, outputs + PLAN_INFO_SIZE);
      return new PlanInfo(
        readU32(module, outputs + 4),
        readU64(module, outputs + 8),
        readU64(module, outputs + 16),
        readU64(module, outputs + 24),
        readU64(module, outputs + 32),
        readU64(module, outputs + 40),
        readU64(module, outputs + 48),
        readU64(module, outputs + 56),
        readU32(module, outputs + 64),
        readU32(module, outputs + 68),
        readU64(module, outputs + 104),
      );
    });
  }

  execute(
    policy: BuildPolicy,
    backing?: WasmMemoryBacking,
  ): BuildResult {
    return executePlan(this, policy, backing);
  }

  /** @internal */
  [planHandleValue](): number {
    return this.requireHandle();
  }

  private requireHandle(): number {
    if (this.#handle === 0) {
      throw bindingError('BuildPlan is disposed', 'disposed_handle');
    }
    return this.#handle;
  }
}

export function buildPlanHandle(plan: BuildPlan): number {
  if (!(plan instanceof BuildPlan)) {
    throw new TypeError('plan must be BuildPlan');
  }
  return BuildPlan.prototype[planHandleValue].call(plan);
}

const builderFinalizer = new FinalizationRegistry<number>((handle) => {
  try {
    payloadModule()._fdb_payload_v1_builder_release(handle);
  } catch {
    // Explicit disposal is authoritative; finalization is only a fallback.
  }
});

export class Builder {
  #handle: number;
  readonly #finalizerToken = {};

  private constructor(handle: number, token: typeof handleConstructionToken) {
    if (token !== handleConstructionToken) {
      throw new TypeError('Builder handles are created only by FastDB');
    }
    if (handle === 0) {
      throw bindingError(
        'FastDB Core returned success without a builder',
        'missing_builder_handle',
      );
    }
    this.#handle = handle;
    builderFinalizer.register(this, handle, this.#finalizerToken);
  }

  static create(spec: CompiledSpec, options?: BuilderOptions): Builder {
    const module = payloadModule();
    return withAllocation(module, POINTER_SIZE * 2, (outputs) => {
      const invoke = (optionsPointer: number): Builder => {
        const status = module._fdb_payload_v1_builder_create(
          compiledSpecHandle(spec),
          optionsPointer,
          outputs,
          outputs + POINTER_SIZE,
        );
        checkStatus(status, outputs + POINTER_SIZE);
        return new Builder(readU32(module, outputs), handleConstructionToken);
      };
      if (options === undefined) {
        return invoke(0);
      }
      return withAllocation(module, BUILDER_OPTIONS_SIZE, (rawOptions) => {
        module._fdb_payload_v1_builder_options_init(rawOptions);
        applyBuilderOptions(module, rawOptions, options);
        return invoke(rawOptions);
      });
    });
  }

  dispose(): void {
    if (this.#handle !== 0) {
      const handle = this.#handle;
      this.#handle = 0;
      builderFinalizer.unregister(this.#finalizerToken);
      payloadModule()._fdb_payload_v1_builder_release(handle);
    }
  }

  entryBegin(entryIndex: number, valueCount: bigint): Builder {
    return this.call((handle, error) =>
      payloadModule()._fdb_payload_v1_builder_entry_begin(
        handle,
        checkedU32(entryIndex, 'entryIndex'),
        checkedU64(valueCount, 'valueCount'),
        error,
      ),
    );
  }

  declareObject(componentIndex: number): ObjectHandle {
    const module = payloadModule();
    return withAllocation(module, 16, (outputs) => {
      const status = module._fdb_payload_v1_builder_object_declare(
        this.requireHandle(),
        checkedU32(componentIndex, 'componentIndex'),
        outputs,
        outputs + 8,
      );
      checkStatus(status, outputs + 8);
      return ObjectHandle[createObjectHandle](
        readU64(module, outputs),
        handleConstructionToken,
      );
    });
  }

  objectFillBegin(object: ObjectHandle): Builder {
    return this.call((handle, error) =>
      payloadModule()._fdb_payload_v1_builder_object_fill_begin(
        handle,
        objectValue(object),
        error,
      ),
    );
  }

  valueNull(): Builder {
    return this.call((handle, error) =>
      payloadModule()._fdb_payload_v1_builder_value_null(handle, error),
    );
  }

  valueBool(value: boolean): Builder {
    if (typeof value !== 'boolean') {
      throw new TypeError('value must be boolean');
    }
    return this.call((handle, error) =>
      payloadModule()._fdb_payload_v1_builder_value_bool(
        handle,
        value ? 1 : 0,
        error,
      ),
    );
  }

  valueU8(value: number): Builder {
    return this.call((handle, error) =>
      payloadModule()._fdb_payload_v1_builder_value_u8(
        handle,
        checkedU8(value, 'value'),
        error,
      ),
    );
  }

  valueU16(value: number): Builder {
    return this.call((handle, error) =>
      payloadModule()._fdb_payload_v1_builder_value_u16(
        handle,
        checkedU16(value, 'value'),
        error,
      ),
    );
  }

  valueU32(value: number): Builder {
    return this.call((handle, error) =>
      payloadModule()._fdb_payload_v1_builder_value_u32(
        handle,
        checkedU32(value, 'value'),
        error,
      ),
    );
  }

  valueI32(value: number): Builder {
    return this.call((handle, error) =>
      payloadModule()._fdb_payload_v1_builder_value_i32(
        handle,
        checkedI32(value, 'value'),
        error,
      ),
    );
  }

  valueU8n(value: number): Builder {
    return this.valueU8nBits(f64Bits(value));
  }

  valueU8nBits(bits: bigint): Builder {
    return this.call((handle, error) =>
      payloadModule()._fdb_payload_v1_builder_value_u8n_f64_bits(
        handle,
        checkedU64(bits, 'bits'),
        error,
      ),
    );
  }

  valueU16n(value: number): Builder {
    return this.valueU16nBits(f64Bits(value));
  }

  valueU16nBits(bits: bigint): Builder {
    return this.call((handle, error) =>
      payloadModule()._fdb_payload_v1_builder_value_u16n_f64_bits(
        handle,
        checkedU64(bits, 'bits'),
        error,
      ),
    );
  }

  valueF32(value: number): Builder {
    return this.valueF32Bits(f32Bits(value));
  }

  valueF32Bits(bits: number): Builder {
    return this.call((handle, error) =>
      payloadModule()._fdb_payload_v1_builder_value_f32_bits(
        handle,
        checkedU32(bits, 'bits'),
        error,
      ),
    );
  }

  valueF64(value: number): Builder {
    return this.valueF64Bits(f64Bits(value));
  }

  valueF64Bits(bits: bigint): Builder {
    return this.call((handle, error) =>
      payloadModule()._fdb_payload_v1_builder_value_f64_bits(
        handle,
        checkedU64(bits, 'bits'),
        error,
      ),
    );
  }

  valueStr(value: string): Builder {
    if (typeof value !== 'string') {
      throw new TypeError('value must be string');
    }
    return this.valueStrBytes(encoder.encode(value));
  }

  valueStrBytes(value: Uint8Array): Builder {
    return this.callBytes(
      value,
      (handle, pointer, size, error) =>
        payloadModule()._fdb_payload_v1_builder_value_str(
          handle,
          pointer,
          size,
          error,
        ),
    );
  }

  valueWstr(value: string): Builder {
    if (typeof value !== 'string') {
      throw new TypeError('value must be string');
    }
    const units = new Uint16Array(value.length);
    for (let index = 0; index < value.length; index += 1) {
      units[index] = value.charCodeAt(index);
    }
    return this.valueWstrUnits(units);
  }

  valueWstrUnits(value: Uint16Array): Builder {
    const module = payloadModule();
    return withInputU16(module, value, (pointer, size) =>
      this.call((handle, error) =>
        module._fdb_payload_v1_builder_value_wstr(
          handle,
          pointer,
          size,
          error,
        ),
      ),
    );
  }

  valueBytes(value: Uint8Array): Builder {
    return this.callBytes(
      value,
      (handle, pointer, size, error) =>
        payloadModule()._fdb_payload_v1_builder_value_bytes(
          handle,
          pointer,
          size,
          error,
        ),
    );
  }

  valueFixedRun(run: FixedRun): Builder {
    if (!(run instanceof FixedRun)) {
      throw new TypeError('run must be FixedRun');
    }
    const module = payloadModule();
    const validity = run.validity ?? new Uint8Array();
    return withInputBytes(module, run.data, (data, dataSize) =>
      withInputBytes(module, validity, (validityData, validitySize) =>
        withAllocation(module, FIXED_RUN_SIZE, (raw) => {
          module._fdb_payload_v1_fixed_run_init(raw);
          writeU32(module, raw + 8, data);
          writeU64(module, raw + 16, dataSize);
          writeU64(module, raw + 24, run.count);
          writeU64(module, raw + 32, run.strideBytes);
          writeU32(module, raw + 40, validityData);
          writeU64(module, raw + 48, validitySize);
          writeU64(module, raw + 56, run.validityBitOffset);
          return this.call((handle, error) =>
            module._fdb_payload_v1_builder_value_fixed_run(handle, raw, error),
          );
        }),
      ),
    );
  }

  valueComponentBegin(): Builder {
    return this.call((handle, error) =>
      payloadModule()._fdb_payload_v1_builder_value_component_begin(
        handle,
        error,
      ),
    );
  }

  valueListBegin(itemCount: bigint): Builder {
    return this.call((handle, error) =>
      payloadModule()._fdb_payload_v1_builder_value_list_begin(
        handle,
        checkedU64(itemCount, 'itemCount'),
        error,
      ),
    );
  }

  valueObject(object: ObjectHandle): Builder {
    return this.call((handle, error) =>
      payloadModule()._fdb_payload_v1_builder_value_object(
        handle,
        objectValue(object),
        error,
      ),
    );
  }

  valueRef(object: ObjectHandle): Builder {
    return this.call((handle, error) =>
      payloadModule()._fdb_payload_v1_builder_value_ref(
        handle,
        objectValue(object),
        error,
      ),
    );
  }

  freeze(): BuildPlan {
    const module = payloadModule();
    return withAllocation(module, POINTER_SIZE * 2, (outputs) => {
      const status = module._fdb_payload_v1_builder_freeze(
        this.requireHandle(),
        outputs,
        outputs + POINTER_SIZE,
      );
      checkStatus(status, outputs + POINTER_SIZE);
      return BuildPlan[createBuildPlan](
        readU32(module, outputs),
        handleConstructionToken,
      );
    });
  }

  private call(operation: (handle: number, outError: number) => number): Builder {
    const module = payloadModule();
    return withAllocation(module, POINTER_SIZE, (outError) => {
      const status = operation(this.requireHandle(), outError);
      checkStatus(status, outError);
      return this;
    });
  }

  private callBytes(
    bytes: Uint8Array,
    operation: (
      handle: number,
      pointer: number,
      size: bigint,
      outError: number,
    ) => number,
  ): Builder {
    const module = payloadModule();
    return withInputBytes(module, bytes, (pointer, size) =>
      this.call((handle, error) =>
        operation(handle, pointer, size, error),
      ),
    );
  }

  private requireHandle(): number {
    if (this.#handle === 0) {
      throw bindingError('Builder is disposed', 'disposed_handle');
    }
    return this.#handle;
  }
}

function objectValue(object: ObjectHandle): bigint {
  if (!(object instanceof ObjectHandle)) {
    throw new TypeError('object must be ObjectHandle');
  }
  return ObjectHandle.prototype[objectHandleValue].call(object);
}

function applyBuilderOptions(
  module: ReturnType<typeof payloadModule>,
  pointer: number,
  options: BuilderOptions,
): void {
  if (options.flags !== undefined) {
    writeU32(module, pointer + 4, checkedU32(options.flags, 'flags'));
  }
  const values: Array<[bigint | undefined, number, string]> = [
    [options.maxValueNodes, 8, 'maxValueNodes'],
    [options.maxListElements, 16, 'maxListElements'],
    [options.maxTextBytes, 24, 'maxTextBytes'],
    [options.maxOpaqueBytes, 32, 'maxOpaqueBytes'],
    [options.maxNestingDepth, 40, 'maxNestingDepth'],
    [options.maxTotalBuilderBytes, 48, 'maxTotalBuilderBytes'],
    [options.maxGraphObjects, 88, 'maxGraphObjects'],
  ];
  for (const [value, offset, name] of values) {
    if (value !== undefined) {
      writeU64(module, pointer + offset, checkedU64(value, name));
    }
  }
}
