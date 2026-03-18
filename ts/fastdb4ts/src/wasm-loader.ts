import FastdbWasm from './wasm/fastdb4ts.js';

export interface WxMemoryStreamHandle {
  reset(): void;
  delete(): void;
}

export interface FastdbModule {
  WxMemoryStream: new () => WxMemoryStreamHandle;
}

export interface FastdbModuleFactory {
  (moduleOverrides?: Record<string, unknown>): Promise<FastdbModule>;
}

let modulePromise: Promise<FastdbModule> | null = null;

export function initFastdb(): Promise<FastdbModule> {
  if (modulePromise === null) {
    modulePromise = (FastdbWasm as FastdbModuleFactory)();
  }
  return modulePromise;
}

export async function getFastdbModule(): Promise<FastdbModule> {
  return initFastdb();
}
