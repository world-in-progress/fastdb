import {
  FastdbRuntimeError,
  FastdbSchemaError,
  FastdbUsageError,
} from './errors.js';
import {
  getClassSchema,
  getFieldDefinition,
  type ClassSchema,
  type SchemaFieldDefinition,
} from './schema.js';
import {
  type RefFieldDef,
  type SchemaEntry,
  isNumericField,
  isRefField,
} from './types.js';
import type {
  WxDatabaseBuildHandle,
  WxDatabaseHandle,
  WxFeatureHandle,
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
    case 'wstr':
      return origin.getFieldAsString(def.index);
    case 'bytes':
      return origin.geometryView();
    case 'ref':
      return feature._cache?.[def.name] ?? null;
    default:
      throw new FastdbSchemaError(`Unsupported field kind: ${(entry as SchemaEntry).kind}`);
  }
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
      origin.setFieldInt(def.index, value ? 1 : 0);
      return;
    }

    origin.setFieldInt(def.index, Math.trunc(Number(value)));
    return;
  }

  if (isRefField(entry)) {
    writeRefField(feature, def, entry, value);
    return;
  }

  throw new FastdbUsageError(
    `Direct writes to db-mapped field "${def.name}" of type "${entry.kind}" are not supported yet.`
  );
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
  if (isRefField(entry) && def.target) {
    return createFeature(def.target as FeatureClass);
  }
  return entry.createDefault();
}
