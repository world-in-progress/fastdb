import { FastdbRuntimeError } from './errors.js';
import {
  type FastdbModule,
  type WxDatabaseHandle,
} from './wasm-loader.js';

export class FastdbOwnedBytes {
  private ptr: number | null;

  constructor(
    private readonly module: FastdbModule,
    ptr: number,
    private readonly size: number
  ) {
    if (!Number.isSafeInteger(ptr) || ptr <= 0) {
      throw new FastdbRuntimeError('fastdb owned buffer pointer must be a positive WASM heap address.');
    }
    if (!Number.isSafeInteger(size) || size <= 0) {
      throw new FastdbRuntimeError('fastdb owned buffer byte length must be a positive integer.');
    }
    this.ptr = ptr;
  }

  get byteLength(): number {
    return this.size;
  }

  get dataPtr(): number {
    return this.requireOwned();
  }

  get view(): Uint8Array {
    const ptr = this.requireOwned();
    return this.module.HEAPU8.subarray(ptr, ptr + this.size);
  }

  release(): void {
    if (this.ptr === null) {
      return;
    }
    const ptr = this.ptr;
    this.ptr = null;
    this.module._free(ptr);
  }

  takeForDatabase(): { module: FastdbModule; ptr: number; byteLength: number } {
    const ptr = this.requireOwned();
    this.ptr = null;
    return {
      module: this.module,
      ptr,
      byteLength: this.size,
    };
  }

  private requireOwned(): number {
    if (this.ptr === null) {
      throw new FastdbRuntimeError('fastdb owned buffer has already been transferred or released.');
    }
    return this.ptr;
  }
}

export type FastdbDatabaseBytes = Uint8Array | ArrayBuffer | FastdbOwnedBytes;

export function allocateFastdbOwnedBytes(module: FastdbModule, byteLength: number): FastdbOwnedBytes {
  if (!Number.isSafeInteger(byteLength) || byteLength <= 0) {
    throw new FastdbRuntimeError('fastdb owned buffer byte length must be a positive integer.');
  }
  const ptr = module._malloc(byteLength);
  if (ptr === 0) {
    throw new FastdbRuntimeError(`Failed to allocate ${byteLength} bytes in the fastdb WASM heap.`);
  }
  return new FastdbOwnedBytes(module, ptr, byteLength);
}

export function loadDatabaseFromBytes(module: FastdbModule, bytes: FastdbDatabaseBytes): WxDatabaseHandle {
  if (bytes instanceof FastdbOwnedBytes) {
    const owned = bytes.takeForDatabase();
    if (owned.module !== module) {
      owned.module._free(owned.ptr);
      throw new FastdbRuntimeError('fastdb owned buffer was allocated by a different WASM module.');
    }
    return loadDatabaseFromOwnedHeap(module, owned.ptr, owned.byteLength);
  }
  if (bytes instanceof ArrayBuffer) {
    return loadDatabaseFromBytes(module, new Uint8Array(bytes));
  }
  if (bytes.byteLength === 0) {
    throw new FastdbRuntimeError('Cannot load an empty fastdb buffer.');
  }
  const owned = allocateFastdbOwnedBytes(module, bytes.byteLength);

  try {
    owned.view.set(bytes);
    const payload = owned.takeForDatabase();
    return loadDatabaseFromOwnedHeap(module, payload.ptr, payload.byteLength);
  } finally {
    owned.release();
  }
}

function loadDatabaseFromOwnedHeap(module: FastdbModule, ptr: number, byteLength: number): WxDatabaseHandle {
  let ownedByDatabase = false;
  try {
    const db = module.WxDatabase.loadFromOwnedHeap(ptr, byteLength);
    if (!db) {
      throw new FastdbRuntimeError('Failed to load fastdb buffer.');
    }
    ownedByDatabase = true;
    return db;
  } finally {
    if (!ownedByDatabase) {
      module._free(ptr);
    }
  }
}
