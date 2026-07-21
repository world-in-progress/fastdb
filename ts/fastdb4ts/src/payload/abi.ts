import {
  FastdbModule,
  getInitializedFastdbModule,
  initFastdb,
} from '../wasm-loader.js';

export const ABI_VERSION = 1;
export const SHA256_SIZE = 32;
export const CAPABILITIES_SIZE = 72;
export const POINTER_SIZE = 4;

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

export function readU32(module: FastdbModule, pointer: number): number {
  return new DataView(module.HEAPU8.buffer).getUint32(pointer, true);
}

export function readU64(module: FastdbModule, pointer: number): bigint {
  return new DataView(module.HEAPU8.buffer).getBigUint64(pointer, true);
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
