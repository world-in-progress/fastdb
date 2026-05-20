import {
  FastdbRuntimeError,
  FastdbSchemaError,
  FastdbUsageError,
} from './errors.js';
import {
  getClassSchema,
  getFieldDefinition,
  resolveListItem,
  type ClassSchema,
  type SchemaFieldDefinition,
} from './schema.js';
import {
  type FieldTypeDef,
  type RefFieldDef,
  type SchemaEntry,
  coerceBoolScalar,
  isListField,
  isNumericField,
  isRefField,
} from './types.js';
import {
  getInitializedFastdbModule,
  type ChunkView,
  type WxDatabaseBuildHandle,
  type WxDatabaseHandle,
  type WxFeatureHandle,
} from './wasm-loader.js';

const PROXY_SYMBOL = Symbol('fastdb4ts.proxy');

export type FeatureDatabaseHandle = WxDatabaseHandle | WxDatabaseBuildHandle;

export interface FeatureClass<T extends Feature = Feature> {
  new (): T;
  name: string;
  schema: unknown;
}

type FeatureInitialValues<T extends Feature> = Partial<Record<keyof T & string, unknown>>;

export class Feature {
  _origin: WxFeatureHandle | null = null;
  _db: FeatureDatabaseHandle | null = null;
  _cache: Record<string, unknown> | null = null;
  _schema: ClassSchema;
  [PROXY_SYMBOL]?: this;

  constructor(initialValues?: Record<string, unknown>) {
    this._schema = getClassSchema(this.constructor as FeatureClass);
    if (initialValues && Object.keys(initialValues).length > 0) {
      this._cache = { ...initialValues };
    }
    return wrapFeature(this);
  }

  get fixed(): boolean {
    return this._origin !== null;
  }

  _getCache(): Record<string, unknown> {
    if (this._cache === null) {
      this._cache = {};
    }
    return this._cache;
  }

  static mapFrom<T extends Feature>(
    this: FeatureClass<T>,
    db: FeatureDatabaseHandle,
    origin: WxFeatureHandle | null
  ): T {
    return mapFeatureFrom(this, db, origin);
  }
}

export function createFeature<T extends Feature>(
  ctor: FeatureClass<T>,
  initialValues?: FeatureInitialValues<T>
): T {
  const feature = new ctor();
  if (initialValues && Object.keys(initialValues).length > 0) {
    feature._cache = { ...(initialValues as Record<string, unknown>) };
  }
  return wrapFeature(feature);
}

export function mapFeatureFrom<T extends Feature>(
  ctor: FeatureClass<T>,
  db: FeatureDatabaseHandle,
  origin: WxFeatureHandle | null
): T {
  const feature = new ctor();
  feature._db = db;
  feature._origin = origin;
  return wrapFeature(feature);
}

export function wrapFeature<T extends Feature>(feature: T): T {
  const existingProxy = feature[PROXY_SYMBOL];
  if (existingProxy) {
    return existingProxy as T;
  }

  const proxy = new Proxy(feature, {
    get(target, prop, receiver) {
      if (typeof prop !== 'string' || prop.startsWith('_')) {
        return Reflect.get(target, prop, receiver);
      }

      const def = getFieldDefinition(target._schema, prop);
      if (!def) {
        return Reflect.get(target, prop, receiver);
      }

      return readField(target, def);
    },
    set(target, prop, value, receiver) {
      if (typeof prop !== 'string' || prop.startsWith('_')) {
        return Reflect.set(target, prop, value, receiver);
      }

      const def = getFieldDefinition(target._schema, prop);
      if (!def) {
        return Reflect.set(target, prop, value, receiver);
      }

      writeField(target, def, value);
      return true;
    },
  });

  feature[PROXY_SYMBOL] = proxy;
  return proxy;
}

function readField(feature: Feature, def: SchemaFieldDefinition): unknown {
  const cache = feature._cache;
  if (cache && def.name in cache) {
    return cache[def.name];
  }

  if (!feature.fixed || feature._origin === null) {
    const value = createDefaultValue(def);
    feature._getCache()[def.name] = value;
    return value;
  }

  return readMappedField(feature, def);
}

function writeField(feature: Feature, def: SchemaFieldDefinition, value: unknown): void {
  if (!feature.fixed || feature._origin === null) {
    feature._getCache()[def.name] = value;
    return;
  }

  writeMappedField(feature, def, value);
}

function readMappedField(feature: Feature, def: SchemaFieldDefinition): unknown {
  const origin = feature._origin;
  if (!origin) {
    throw new FastdbRuntimeError('Feature is not mapped to a WASM origin.');
  }

  const entry = def.entry;
  switch (entry.kind) {
    case 'bool':
      return origin.getFieldAsInt(def.index) !== 0;
    case 'u8':
    case 'u16':
    case 'u32':
    case 'i32':
      return origin.getFieldAsInt(def.index);
    case 'u8n':
    case 'u16n':
    case 'f32':
    case 'f64':
      return origin.getFieldAsFloat(def.index);
    case 'str':
      return origin.getFieldAsString(def.index);
    case 'wstr':
      return origin.getFieldAsWString(def.index);
    case 'bytes':
      return copyChunkToBytes(origin.geometryView());
    case 'list':
      return readMappedListField(origin, def);
    case 'ref':
      return feature._cache?.[def.name] ?? null;
    default:
      throw new FastdbSchemaError(`Unsupported field kind: ${(entry as SchemaEntry).kind}`);
  }
}

function copyChunkToBytes(chunk: ChunkView): Uint8Array {
  const module = getInitializedFastdbModule();
  return module.HEAPU8.slice(chunk.data, chunk.data + chunk.size);
}

function readMappedListField(origin: WxFeatureHandle, def: SchemaFieldDefinition): unknown[] {
  if (!isListField(def.entry)) {
    throw new FastdbSchemaError(`Field "${def.name}" is not a list field.`);
  }
  const item = resolveListItem(def.entry);
  if (!isFieldType(item) || !item.arrayCtor) {
    throw new FastdbSchemaError(
      `Mapped list field "${def.name}" can only materialize numeric scalar lists through the columnar runtime.`
    );
  }
  const bytes = copyChunkToBytes(origin.getFieldAsListView(def.index));
  if (bytes.length === 0) {
    return [];
  }
  const bytesPerElement = item.arrayCtor.BYTES_PER_ELEMENT;
  if (bytes.length % bytesPerElement !== 0) {
    throw new FastdbRuntimeError(
      `Mapped list field "${def.name}" has ${bytes.length} byte(s), not a multiple of ${bytesPerElement}.`
    );
  }
  const source = typedArraySource(bytes);
  const typed = new item.arrayCtor(
    source.buffer,
    source.byteOffset,
    bytes.length / bytesPerElement
  );
  if (item.kind === 'bool') {
    return Array.from(typed, (value) => coerceBoolScalar(value));
  }
  return Array.from(typed);
}

function typedArraySource(bytes: Uint8Array): { buffer: ArrayBuffer; byteOffset: number } {
  if (bytes.buffer instanceof ArrayBuffer) {
    return { buffer: bytes.buffer, byteOffset: bytes.byteOffset };
  }
  const copy = bytes.slice();
  return { buffer: copy.buffer, byteOffset: copy.byteOffset };
}

function isFieldType(value: unknown): value is FieldTypeDef {
  return typeof value === 'object' && value !== null && 'kind' in value && 'originType' in value;
}

function writeMappedField(feature: Feature, def: SchemaFieldDefinition, value: unknown): void {
  const origin = feature._origin;
  if (!origin) {
    throw new FastdbRuntimeError('Feature is not mapped to a WASM origin.');
  }

  const entry = def.entry;
  if (isNumericField(entry)) {
    if (entry.kind === 'f32' || entry.kind === 'f64' || entry.kind === 'u8n' || entry.kind === 'u16n') {
      origin.setFieldDouble(def.index, Number(value));
      return;
    }

    if (entry.kind === 'bool') {
      origin.setFieldInt(def.index, coerceBoolScalar(value) ? 1 : 0);
      return;
    }

    origin.setFieldInt(def.index, Math.trunc(Number(value)));
    return;
  }

  if (isRefField(entry)) {
    writeRefField(feature, def, entry, value);
    return;
  }

  // For str/wstr/bytes/list: WxFeatureHandle does not support writing these types back
  // to the underlying WASM storage on an immutable database. Fall back to in-memory cache
  // only — changes are not persisted across toBuffer() / fromBuffer() round-trips.
  feature._getCache()[def.name] = value;
}

function writeRefField(
  feature: Feature,
  def: SchemaFieldDefinition,
  entry: RefFieldDef,
  value: unknown
): void {
  if (value === null || value === undefined) {
    feature._getCache()[def.name] = null;
    return;
  }

  if (!(value instanceof Feature)) {
    throw new FastdbUsageError(
      `Field "${def.name}" expects a Feature reference, got "${typeof value}".`
    );
  }

  if (entry.target && !(value instanceof (entry.target as FeatureClass))) {
    throw new FastdbUsageError(
      `Field "${def.name}" expects a reference to "${entry.target.name}".`
    );
  }

  feature._getCache()[def.name] = value;
}

function createDefaultValue(def: SchemaFieldDefinition): unknown {
  const entry = def.entry;
  if (isRefField(entry)) {
    // Ref fields default to null to avoid infinite recursion for self-referential schemas.
    return null;
  }
  return entry.createDefault();
}
