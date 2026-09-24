import {
  FastdbModule,
  getInitializedFastdbModule,
  initFastdb,
} from '../wasm-loader.js';

export const ABI_VERSION = 1;
export const SHA256_SIZE = 32;
export const CAPABILITIES_SIZE = 72;
export const POINTER_SIZE = 4;
export const U64_MAX = 0xffff_ffff_ffff_ffffn;

export async function initPayloadModule(): Promise<FastdbModule> {
  const module = await initFastdb();
  assertAbiVersion(module);
  return module;
}

export function payloadModule(): FastdbModule {
  const module = getInitializedFastdbModule();
  assertAbiVersion(module);
  return module;
}

function assertAbiVersion(module: FastdbModule): void {
  const actual = module._fdb_payload_v1_abi_version();
  if (actual !== ABI_VERSION) {
    throw new Error(
      `FastDB payload ABI mismatch: expected ${ABI_VERSION}, received ${actual}`,
    );
  }
}

export function allocate(module: FastdbModule, size: number): number {
  if (!Number.isSafeInteger(size) || size <= 0) {
    throw new RangeError(`invalid Wasm allocation size: ${size}`);
  }
  const pointer = module._malloc(size);
  if (pointer === 0) {
    throw new Error(`FastDB Wasm allocation failed for ${size} bytes`);
  }
  module.HEAPU8.fill(0, pointer, pointer + size);
  return pointer;
}

export function withAllocation<T>(
  module: FastdbModule,
  size: number,
  operation: (pointer: number) => T,
): T {
  const pointer = allocate(module, size);
  try {
    return operation(pointer);
  } finally {
    module._free(pointer);
  }
}

export function withInputBytes<T>(
  module: FastdbModule,
  bytes: Uint8Array,
  operation: (pointer: number, size: bigint) => T,
): T {
  if (bytes.byteLength === 0) {
    return operation(0, 0n);
  }
  return withAllocation(module, bytes.byteLength, (pointer) => {
    module.HEAPU8.set(bytes, pointer);
    return operation(pointer, BigInt(bytes.byteLength));
  });
}

export function withSha256<T>(
  module: FastdbModule,
  digest: Uint8Array,
  operation: (pointer: number) => T,
): T {
  if (!(digest instanceof Uint8Array) || digest.byteLength !== SHA256_SIZE) {
    throw new RangeError(`expectedSha256 must contain ${SHA256_SIZE} bytes`);
  }
  return withInputBytes(module, digest, (pointer) => operation(pointer));
}

export function withInputU16<T>(
  module: FastdbModule,
  units: Uint16Array,
  operation: (pointer: number, size: bigint) => T,
): T {
  if (units.length === 0) {
    return operation(0, 0n);
  }
  return withAllocation(module, units.length * 2, (pointer) => {
    const view = new DataView(module.HEAPU8.buffer);
    for (let index = 0; index < units.length; index += 1) {
      view.setUint16(pointer + index * 2, units[index], true);
    }
    return operation(pointer, BigInt(units.length));
  });
}

export function readU32(module: FastdbModule, pointer: number): number {
  return new DataView(module.HEAPU8.buffer).getUint32(pointer, true);
}

export function readU64(module: FastdbModule, pointer: number): bigint {
  return new DataView(module.HEAPU8.buffer).getBigUint64(pointer, true);
}

export function writeU32(
  module: FastdbModule,
  pointer: number,
  value: number,
): void {
  new DataView(module.HEAPU8.buffer).setUint32(pointer, value, true);
}

export function writeU64(
  module: FastdbModule,
  pointer: number,
  value: bigint,
): void {
  new DataView(module.HEAPU8.buffer).setBigUint64(pointer, value, true);
}

export function copyBytes(
  module: FastdbModule,
  pointer: number,
  size: bigint,
): Uint8Array {
  if (size === 0n) {
    return new Uint8Array();
  }
  if (pointer === 0) {
    throw new Error('FastDB Core returned non-empty bytes with null storage');
  }
  if (size > BigInt(Number.MAX_SAFE_INTEGER)) {
    throw new RangeError('FastDB byte span exceeds the JavaScript address space');
  }
  const numericSize = Number(size);
  const end = pointer + numericSize;
  if (!Number.isSafeInteger(end) || end > module.HEAPU8.byteLength) {
    throw new RangeError('FastDB byte span exceeds the current Wasm memory');
  }
  return module.HEAPU8.slice(pointer, end);
}

export function checkedU32(value: number, name: string): number {
  if (!Number.isInteger(value) || value < 0 || value > 0xffff_ffff) {
    throw new RangeError(`${name} must be an unsigned 32-bit integer`);
  }
  return value;
}

export function checkedU16(value: number, name: string): number {
  if (!Number.isInteger(value) || value < 0 || value > 0xffff) {
    throw new RangeError(`${name} must be an unsigned 16-bit integer`);
  }
  return value;
}

export function checkedU8(value: number, name: string): number {
  if (!Number.isInteger(value) || value < 0 || value > 0xff) {
    throw new RangeError(`${name} must be an unsigned 8-bit integer`);
  }
  return value;
}

export function checkedI32(value: number, name: string): number {
  if (
    !Number.isInteger(value) ||
    value < -0x8000_0000 ||
    value > 0x7fff_ffff
  ) {
    throw new RangeError(`${name} must be a signed 32-bit integer`);
  }
  return value;
}

export function checkedU64(value: bigint, name: string): bigint {
  if (typeof value !== 'bigint' || value < 0n || value > U64_MAX) {
    throw new RangeError(`${name} must be an unsigned 64-bit bigint`);
  }
  return value;
}

export function f32Bits(value: number): number {
  if (typeof value !== 'number') {
    throw new TypeError('value must be a number');
  }
  const storage = new ArrayBuffer(4);
  const view = new DataView(storage);
  view.setFloat32(0, value, true);
  return view.getUint32(0, true);
}

export function f64Bits(value: number): bigint {
  if (typeof value !== 'number') {
    throw new TypeError('value must be a number');
  }
  const storage = new ArrayBuffer(8);
  const view = new DataView(storage);
  view.setFloat64(0, value, true);
  return view.getBigUint64(0, true);
}

export function f32FromBits(bits: number): number {
  const storage = new ArrayBuffer(4);
  const view = new DataView(storage);
  view.setUint32(0, bits, true);
  return view.getFloat32(0, true);
}

export function f64FromBits(bits: bigint): number {
  const storage = new ArrayBuffer(8);
  const view = new DataView(storage);
  view.setBigUint64(0, bits, true);
  return view.getFloat64(0, true);
}
