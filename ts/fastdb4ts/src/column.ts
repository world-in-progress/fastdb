import { FastdbRuntimeError } from './errors.js';
import type { FastdbModule, WxLayerTableHandle } from './wasm-loader.js';

export class StridedColumn {
  readonly length: number;
  readonly fieldIndex: number;
  readonly byteOffset: number;
  readonly stride: number;
  readonly kind: number;

  private readonly module: FastdbModule;
  private readonly layer: WxLayerTableHandle;
  private readonly basePtr: number;

  constructor(
    module: FastdbModule,
    layer: WxLayerTableHandle,
    fieldIndex: number,
    kind: number
  ) {
    this.module = module;
    this.layer = layer;
    this.fieldIndex = fieldIndex;
    this.kind = kind;
    this.length = layer.getFeatureCount();
    this.byteOffset = layer.getFieldOffset(fieldIndex);
    this.stride = layer.getFeatureByteSize();

    if (this.length > 0) {
      const feature = layer.tryGetFeatureAt(0);
      this.basePtr = feature.getAddress() + this.byteOffset;
    } else {
      this.basePtr = 0;
    }
  }

  get(index: number): number {
    const offset = this.getElementOffset(index);
    const view = this.getDataView();
    if (this.kind === this.module.ftU8 || this.kind === this.module.ftU8n) {
      return view.getUint8(offset);
    }
    if (this.kind === this.module.ftU16 || this.kind === this.module.ftU16n) {
      return view.getUint16(offset, true);
    }
    if (this.kind === this.module.ftU32) {
      return view.getUint32(offset, true);
    }
    if (this.kind === this.module.ftI32) {
      return view.getInt32(offset, true);
    }
    if (this.kind === this.module.ftF32) {
      return view.getFloat32(offset, true);
    }
    if (this.kind === this.module.ftF64) {
      return view.getFloat64(offset, true);
    }
    throw new FastdbRuntimeError(`Field index ${this.fieldIndex} is not a numeric column.`);
  }

  set(index: number, value: number): void {
    const feature = this.layer.tryGetFeatureAt(this.normalizeIndex(index));
    if (
      this.kind === this.module.ftF32 ||
      this.kind === this.module.ftF64 ||
      this.kind === this.module.ftU8n ||
      this.kind === this.module.ftU16n
    ) {
      feature.setFieldDouble(this.fieldIndex, value);
    } else {
      feature.setFieldInt(this.fieldIndex, Math.trunc(value));
    }
  }

  fill(values: ArrayLike<number>): void {
    if (values.length !== this.length) {
      throw new FastdbRuntimeError(
        `Column fill length mismatch: expected ${this.length}, got ${values.length}.`
      );
    }
    for (let i = 0; i < this.length; i += 1) {
      this.set(i, values[i] ?? 0);
    }
  }

  toArray(): TypedArray {
    const ctor = this.getArrayConstructor();
    const out = new ctor(this.length);
    for (let i = 0; i < this.length; i += 1) {
      out[i] = this.get(i);
    }
    return out;
  }

  forEach(fn: (value: number, index: number) => void): void {
    for (let i = 0; i < this.length; i += 1) {
      fn(this.get(i), i);
    }
  }

  private getElementOffset(index: number): number {
    const normalized = this.normalizeIndex(index);
    return this.basePtr + normalized * this.stride;
  }

  private normalizeIndex(index: number): number {
    const normalized = index < 0 ? this.length + index : index;
    if (normalized < 0 || normalized >= this.length) {
      throw new FastdbRuntimeError(
        `Column index ${index} is out of range [0, ${this.length}).`
      );
    }
    return normalized;
  }

  private getArrayConstructor(): TypedArrayConstructor {
    if (this.kind === this.module.ftU8 || this.kind === this.module.ftU8n) {
      return Uint8Array;
    }
    if (this.kind === this.module.ftU16 || this.kind === this.module.ftU16n) {
      return Uint16Array;
    }
    if (this.kind === this.module.ftU32) {
      return Uint32Array;
    }
    if (this.kind === this.module.ftI32) {
      return Int32Array;
    }
    if (this.kind === this.module.ftF32) {
      return Float32Array;
    }
    if (this.kind === this.module.ftF64) {
      return Float64Array;
    }
    throw new FastdbRuntimeError(`Field index ${this.fieldIndex} is not a numeric column.`);
  }

  private getDataView(): DataView {
    // Always access HEAPU8.buffer fresh: if WASM memory grows, Emscripten replaces
    // module.HEAPU8 with a new typed array. The linear-memory addresses stored in
    // basePtr remain valid (they are indices into the linear address space), but
    // any previously captured ArrayBuffer reference becomes detached.
    return new DataView(this.module.HEAPU8.buffer);
  }
}

type TypedArray =
  | Uint8Array
  | Uint16Array
  | Uint32Array
  | Int32Array
  | Float32Array
  | Float64Array;

type TypedArrayConstructor =
  | Uint8ArrayConstructor
  | Uint16ArrayConstructor
  | Uint32ArrayConstructor
  | Int32ArrayConstructor
  | Float32ArrayConstructor
  | Float64ArrayConstructor;
