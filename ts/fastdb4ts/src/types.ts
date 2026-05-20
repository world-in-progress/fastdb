import { FastdbUsageError } from './errors.js';

export type TypedArrayInstance =
  | Uint8Array
  | Uint16Array
  | Uint32Array
  | Int32Array
  | Float32Array
  | Float64Array;

export type TypedArrayConstructor =
  | Uint8ArrayConstructor
  | Uint16ArrayConstructor
  | Uint32ArrayConstructor
  | Int32ArrayConstructor
  | Float32ArrayConstructor
  | Float64ArrayConstructor;

export type ScalarFieldKind =
  | 'bool'
  | 'u8'
  | 'u16'
  | 'u32'
  | 'i32'
  | 'u8n'
  | 'u16n'
  | 'f32'
  | 'f64'
  | 'str'
  | 'wstr';

export type FieldKind = ScalarFieldKind | 'ref' | 'bytes' | 'list';

export interface FieldTypeDef<TKind extends FieldKind = FieldKind> {
  kind: TKind;
  originType: number;
  numeric: boolean;
  scalar: boolean;
  normalized: boolean;
  createDefault(): unknown;
  arrayCtor?: TypedArrayConstructor;
}

export interface FeatureClassLike {
  new (): unknown;
  name: string;
  schema?: unknown;
}

export interface RefFieldDef extends FieldTypeDef<'ref'> {
  target?: FeatureClassLike | (() => FeatureClassLike);
}

export type ListItemDef =
  | FieldTypeDef<ScalarFieldKind>
  | FeatureClassLike
  | (() => FeatureClassLike);

export interface ListFieldDef extends FieldTypeDef<'list'> {
  item: ListItemDef;
}

const TRUE_BOOL_STRINGS = new Set(['1', 'true', 't', 'yes', 'y', 'on']);
const FALSE_BOOL_STRINGS = new Set(['0', 'false', 'f', 'no', 'n', 'off']);

export function coerceBoolScalar(value: unknown): boolean {
  if (typeof value === 'boolean') {
    return value;
  }
  if (typeof value === 'string') {
    const normalized = value.trim().toLowerCase();
    if (TRUE_BOOL_STRINGS.has(normalized)) {
      return true;
    }
    if (FALSE_BOOL_STRINGS.has(normalized)) {
      return false;
    }
    throw new FastdbUsageError(`cannot coerce ${JSON.stringify(value)} to fastdb bool scalar; expected bool, 0/1, or true/false string.`);
  }
  if (typeof value === 'number') {
    if (value === 0 || value === 1) {
      return value === 1;
    }
    throw new FastdbUsageError(`cannot coerce ${String(value)} to fastdb bool scalar; expected bool, 0/1, or true/false string.`);
  }
  if (typeof value === 'bigint') {
    if (value === 0n || value === 1n) {
      return value === 1n;
    }
    throw new FastdbUsageError(`cannot coerce ${String(value)} to fastdb bool scalar; expected bool, 0/1, or true/false string.`);
  }
  throw new FastdbUsageError(`cannot coerce ${String(value)} to fastdb bool scalar; expected bool, 0/1, or true/false string.`);
}

function makeFieldType<TKind extends FieldKind>(
  kind: TKind,
  originType: number,
  options: {
    numeric?: boolean;
    scalar?: boolean;
    normalized?: boolean;
    arrayCtor?: TypedArrayConstructor;
    createDefault: () => unknown;
  }
): FieldTypeDef<TKind> {
  return Object.freeze({
    kind,
    originType,
    numeric: options.numeric ?? false,
    scalar: options.scalar ?? true,
    normalized: options.normalized ?? false,
    arrayCtor: options.arrayCtor,
    createDefault: options.createDefault,
  });
}

export const BOOL = makeFieldType('bool', 1, {
  numeric: true,
  scalar: true,
  arrayCtor: Uint8Array,
  createDefault: () => false,
});

export const U8 = makeFieldType('u8', 1, {
  numeric: true,
  scalar: true,
  arrayCtor: Uint8Array,
  createDefault: () => 0,
});

export const U16 = makeFieldType('u16', 2, {
  numeric: true,
  scalar: true,
  arrayCtor: Uint16Array,
  createDefault: () => 0,
});

export const U32 = makeFieldType('u32', 3, {
  numeric: true,
  scalar: true,
  arrayCtor: Uint32Array,
  createDefault: () => 0,
});

export const I32 = makeFieldType('i32', 4, {
  numeric: true,
  scalar: true,
  arrayCtor: Int32Array,
  createDefault: () => 0,
});

export const U8N = makeFieldType('u8n', 5, {
  numeric: true,
  scalar: true,
  normalized: true,
  arrayCtor: Uint8Array,
  createDefault: () => 0,
});

export const U16N = makeFieldType('u16n', 6, {
  numeric: true,
  scalar: true,
  normalized: true,
  arrayCtor: Uint16Array,
  createDefault: () => 0,
});

export const F32 = makeFieldType('f32', 7, {
  numeric: true,
  scalar: true,
  arrayCtor: Float32Array,
  createDefault: () => 0,
});

export const F64 = makeFieldType('f64', 8, {
  numeric: true,
  scalar: true,
  arrayCtor: Float64Array,
  createDefault: () => 0,
});

export const STR = makeFieldType('str', 9, {
  scalar: true,
  createDefault: () => '',
});

export const WSTR = makeFieldType('wstr', 10, {
  scalar: true,
  createDefault: () => '',
});

export const BYTES = makeFieldType('bytes', 12, {
  scalar: false,
  createDefault: () => new Uint8Array(0),
});

export const REF: RefFieldDef = Object.freeze({
  kind: 'ref',
  originType: 11,
  numeric: false,
  scalar: false,
  normalized: false,
  createDefault: () => null,
});

const LIST = makeFieldType('list', 13, {
  scalar: false,
  createDefault: () => [],
});

export type SchemaEntry = FieldTypeDef | RefFieldDef | ListFieldDef;

export function ref(target?: FeatureClassLike | (() => FeatureClassLike)): RefFieldDef {
  return Object.freeze({
    ...REF,
    target,
  });
}

export function listOf(item: ListItemDef): ListFieldDef {
  return Object.freeze({
    ...LIST,
    item,
  });
}

export function isRefField(entry: SchemaEntry): entry is RefFieldDef {
  return entry.kind === 'ref';
}

export function isListField(entry: SchemaEntry): entry is ListFieldDef {
  return entry.kind === 'list';
}

export function isNumericField(entry: SchemaEntry): boolean {
  return entry.numeric;
}

export function isScalarField(entry: SchemaEntry): boolean {
  return entry.scalar;
}
