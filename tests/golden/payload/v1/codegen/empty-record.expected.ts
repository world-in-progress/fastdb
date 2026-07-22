// generated-by: fastdb.payload.codegen.v1
// payload-sha256: 92fbbc65fc79ad9ca6e9637063c8f15a806b40e788532225cafe25ef17cdfc71
// core-abi-version: 1
// generator-version: fastdb.payload.codegen.v1
// target: typescript
import {
  CompiledSpec,
} from 'fastdb4ts/payload';

export const CANONICAL_SOURCE_TEXT = `{"components":[],"entries":[],"profile":"record.v1","schema":"fastdb.payload.v1"}`;
export function canonicalSource(): Uint8Array {
  return new TextEncoder().encode(CANONICAL_SOURCE_TEXT);
}
export const PAYLOAD_SHA256 = '92fbbc65fc79ad9ca6e9637063c8f15a806b40e788532225cafe25ef17cdfc71' as const;

export function compileSpec(): CompiledSpec {
  return CompiledSpec.compile(canonicalSource());
}

export interface IdMetadata {
  readonly symbol: string;
  readonly originalId: string;
  readonly stableIndex: number;
}

export const ENTRIES: readonly IdMetadata[] = Object.freeze([
]);

export const COMPONENTS: readonly IdMetadata[] = Object.freeze([
]);

export interface FieldMetadata {
  readonly symbol: string;
  readonly originalId: string;
  readonly componentIndex: number;
  readonly fieldIndex: number;
}

export const FIELDS: readonly FieldMetadata[] = Object.freeze([
]);
