import { Feature, type FeatureClass, type FeatureDatabaseHandle } from './feature.js';
import { getClassSchema, resolveListItem, type SchemaFieldDefinition } from './schema.js';
import { FastdbRuntimeError, FastdbUsageError } from './errors.js';
import { Table } from './table.js';
import { loadDatabaseFromBytes, type FastdbDatabaseBytes } from './database-buffer.js';
import { getInitializedFastdbModule } from './wasm-loader.js';
import { coerceBoolScalar, isListField, type FieldTypeDef } from './types.js';
import type {
  FastdbModule,
  WxDatabaseBuildHandle,
  WxDatabaseHandle,
  WxLayerTableBuildHandle,
  WxLayerTableHandle,
  WxMemoryStreamHandle,
} from './wasm-loader.js';

type DatabaseOrigin = WxDatabaseHandle | WxDatabaseBuildHandle;
type TableOrigin = WxLayerTableHandle | WxLayerTableBuildHandle;

export class TableDefn<T extends Feature> {
  readonly featureType: FeatureClass<T>;
  readonly capacity: number;
  readonly name?: string;

  constructor(featureType: FeatureClass<T>, capacity: number, name?: string) {
    this.featureType = featureType;
    this.capacity = capacity;
    this.name = name;
  }
}

export class ORM {
  private readonly module: FastdbModule;
  private origin: DatabaseOrigin;
  private readonly tableMap = new Map<string, Table<Feature>>();
  private _closed = false;

  private constructor(origin: DatabaseOrigin, module: FastdbModule) {
    this.origin = origin;
    this.module = module;
  }

  get fixed(): boolean {
    return 'bufferView' in this.origin;
  }

  static create(): ORM {
    const module = ORM.getModule();
    const origin = new module.WxDatabaseBuild();
    origin.begin('');
    return new ORM(origin, module);
  }

  static truncate(defns: TableDefn<Feature>[]): ORM {
    const orm = ORM.create();
    const build = orm.getBuildOrigin();
    for (const defn of defns) {
      if (defn.capacity <= 0) {
        throw new FastdbUsageError('Table capacity must be positive.');
      }
      orm.ensureTruncateCompatible(defn.featureType);
      const layerName = defn.name ?? defn.featureType.name;
      const table = build.createLayerBegin(layerName);
      orm.configureDefaultGeometry(table, defn.featureType);
      orm.defineTableFields(table, defn.featureType);
      build.createLayerEnd();
      build.truncate(layerName, defn.capacity);
    }
    orm.combine();
    return orm;
  }

  static fromBuffer(data: FastdbDatabaseBytes): ORM {
    const module = ORM.getModule();
    return new ORM(loadDatabaseFromBytes(module, data), module);
  }

  push<T extends Feature>(feature: T, tableName?: string): void {
    const featureType = feature.constructor as FeatureClass<T>;
    const name = tableName ?? featureType.name;
    const table = this.ensureTable(featureType, name);
    const origin = table.origin as WxLayerTableBuildHandle;
    origin.addFeatureBegin();
    try {
      const schema = getClassSchema(featureType);
      for (const field of schema.fieldList) {
        const value = (feature as Record<string, unknown>)[field.name];
        const kind = field.entry.kind;
        if (kind === 'bool') {
          origin.setFieldInt(field.index, coerceBoolScalar(value) ? 1 : 0);
        } else if (
          kind === 'u8' ||
          kind === 'u16' ||
          kind === 'u32' ||
          kind === 'i32'
        ) {
          origin.setFieldInt(field.index, Math.trunc(Number(value)));
        } else if (
          kind === 'u8n' ||
          kind === 'u16n' ||
          kind === 'f32' ||
          kind === 'f64'
        ) {
          origin.setFieldDouble(field.index, Number(value));
        } else if (kind === 'str') {
          origin.setFieldString(field.index, String(value ?? ''));
        } else if (kind === 'wstr') {
          origin.setFieldWString(field.index, String(value ?? ''));
        } else if (kind === 'bytes') {
          const bytes = normalizeBytes(value);
          withHeapBytes(this.module, bytes, (ptr, size) => {
            origin.setGeometryRaw(ptr, size);
          });
        } else if (isListField(field.entry)) {
          const bytes = normalizeNumericList(
            value,
            listElementFieldType(field),
            `field "${field.name}"`
          );
          withHeapBytes(this.module, bytes, (ptr, size) => {
            origin.setFieldListNumeric(field.index, ptr, size);
          });
        } else {
          throw new FastdbUsageError(`push() does not support field kind "${kind}" yet.`);
        }
      }
    } finally {
      origin.addFeatureEnd();
    }
  }

  ensureTable<T extends Feature>(featureType: FeatureClass<T>, name?: string): Table<T> {
    const build = this.getBuildOrigin();
    const tableName = name ?? featureType.name;
    const cached = this.tableMap.get(tableName);
    if (cached) {
      return cached as Table<T>;
    }
    const origin = build.createLayerBegin(tableName);
    this.configureDefaultGeometry(origin, featureType);
    this.defineTableFields(origin, featureType);
    const table = new Table(featureType, origin, build, this.module);
    this.tableMap.set(tableName, table as Table<Feature>);
    return table;
  }

  combine(): void {
    const build = this.getBuildOrigin();
    const stream = new this.module.WxMemoryStream();
    build.post(stream);
    const bytes = this.copyChunkToBytes(stream);
    stream.delete();

    this.disposeBuildTables();
    build.delete();
    this.origin = ORM.fromBuffer(bytes).origin;
    this.tableMap.clear();
  }

  toBuffer(): Uint8Array {
    if (this.fixed) {
      const db = this.origin as WxDatabaseHandle;
      const chunk = db.bufferView();
      return this.module.HEAPU8.slice(chunk.data, chunk.data + chunk.size);
    }

    const build = this.getBuildOrigin();
    const stream = new this.module.WxMemoryStream();
    build.post(stream);
    const bytes = this.copyChunkToBytes(stream);
    stream.delete();
    return bytes;
  }

  table<T extends Feature>(featureType: FeatureClass<T>, name?: string): Table<T> {
    const tableName = name ?? featureType.name;
    const cached = this.tableMap.get(tableName);
    if (cached) {
      return cached as Table<T>;
    }

    if (!this.fixed) {
      throw new FastdbRuntimeError(`Table "${tableName}" is not available until combine() finishes.`);
    }

    const db = this.origin as WxDatabaseHandle;
    for (let i = 0; i < db.getLayerCount(); i += 1) {
      const layer = db.getLayer(i);
      if (layer.name() === tableName) {
        const table = new Table(featureType, layer, db, this.module);
        this.tableMap.set(tableName, table as Table<Feature>);
        return table;
      }
    }

    throw new FastdbRuntimeError(`Table "${tableName}" not found.`);
  }

  /** Release WASM resources held by this ORM. Idempotent — safe to call multiple times. */
  close(): void {
    if (this._closed) return;
    this._closed = true;
    this.tableMap.clear();
    this.origin.delete();
  }

  private static getModule(): FastdbModule {
    try {
      return getInitializedFastdbModule();
    } catch {
      throw new FastdbRuntimeError(
        'fastdb4ts has not been initialized. Call `await initFastdb()` before creating an ORM.'
      );
    }
  }

  private getBuildOrigin(): WxDatabaseBuildHandle {
    if (this.fixed) {
      throw new FastdbUsageError('Operation is only available for mutable ORM instances.');
    }
    return this.origin as WxDatabaseBuildHandle;
  }

  private defineTableFields(table: WxLayerTableBuildHandle, featureType: FeatureClass): void {
    const schema = getClassSchema(featureType);
    const bytesFields = schema.fieldList.filter((field) => field.entry.kind === 'bytes');
    if (bytesFields.length > 1) {
      throw new FastdbUsageError(
        `ORM.push() does not support multiple bytes fields in "${featureType.name}"; fastdb columnar bytes use the feature raw payload.`
      );
    }
    for (const field of schema.fieldList) {
      if (isListField(field.entry)) {
        table.addListField(field.name, listElementFieldType(field).originType);
      } else {
        table.addField(field.name, field.entry.originType, 0, 1);
      }
    }
  }

  private configureDefaultGeometry(table: WxLayerTableBuildHandle, featureType: FeatureClass): void {
    const schema = getClassSchema(featureType);
    if (schema.fieldList.some((field) => field.entry.kind === 'bytes')) {
      table.setGeometryType(this.module.gtAny, this.module.cfDefault, false);
      return;
    }
    table.setGeometryType(this.module.gtPoint, this.module.cfTx32, true);
    table.setExtent(-180, -90, 180, 90);
  }

  private ensureTruncateCompatible(featureType: FeatureClass): void {
    const schema = getClassSchema(featureType);
    for (const field of schema.fieldList) {
      if (field.entry.kind === 'str' || field.entry.kind === 'wstr' || field.entry.kind === 'bytes') {
        throw new FastdbUsageError(
          `truncate() does not support variable-length field "${field.name}" of type "${field.entry.kind}".`
        );
      }
    }
  }

  private copyChunkToBytes(stream: WxMemoryStreamHandle): Uint8Array {
    const chunk = stream.dataView();
    return this.module.HEAPU8.slice(chunk.data, chunk.data + chunk.size);
  }

  private disposeBuildTables(): void {
    for (const table of this.tableMap.values()) {
      if (!table.fixed) {
        (table.origin as WxLayerTableBuildHandle).delete();
      }
    }
  }
}

function normalizeBytes(value: unknown): Uint8Array {
  if (value === null || value === undefined) {
    return new Uint8Array(0);
  }
  if (value instanceof Uint8Array) {
    return value;
  }
  if (value instanceof ArrayBuffer) {
    return new Uint8Array(value);
  }
  if (ArrayBuffer.isView(value)) {
    return new Uint8Array(value.buffer, value.byteOffset, value.byteLength);
  }
  throw new FastdbUsageError(`push() bytes fields expect Uint8Array or ArrayBuffer, got "${typeof value}".`);
}

function listElementFieldType(field: SchemaFieldDefinition): FieldTypeDef {
  if (!isListField(field.entry)) {
    throw new FastdbUsageError(`Field "${field.name}" is not a list field.`);
  }
  const item = resolveListItem(field.entry);
  if (!isFieldType(item) || !item.arrayCtor || !isSupportedListElementKind(item.kind)) {
    throw new FastdbUsageError(
      `push() list field "${field.name}" supports only numeric scalar list elements in the columnar runtime.`
    );
  }
  return item;
}

function normalizeNumericList(
  value: unknown,
  item: FieldTypeDef,
  context: string
): Uint8Array {
  if (value === null || value === undefined) {
    return new Uint8Array(0);
  }
  const arrayCtor = item.arrayCtor;
  if (!arrayCtor) {
    throw new FastdbUsageError(`${context} is not a numeric fastdb list field.`);
  }
  if (ArrayBuffer.isView(value) && !(value instanceof DataView)) {
    const view = value as ArrayBufferView;
    if (value.constructor === arrayCtor) {
      return new Uint8Array(view.buffer, view.byteOffset, view.byteLength);
    }
  }
  if (!Array.isArray(value) && !(ArrayBuffer.isView(value) && !(value instanceof DataView))) {
    throw new FastdbUsageError(`${context} expects an array or typed array value.`);
  }
  const values = Array.from(value as ArrayLike<unknown>);
  const typed = new arrayCtor(values.length);
  for (let index = 0; index < values.length; index += 1) {
    const numeric = item.kind === 'bool'
      ? (coerceBoolScalar(values[index]) ? 1 : 0)
      : Number(values[index]);
    typed[index] = numeric;
  }
  return new Uint8Array(typed.buffer, typed.byteOffset, typed.byteLength);
}

function isSupportedListElementKind(kind: string): boolean {
  return kind === 'bool' || kind === 'u8' || kind === 'u16' || kind === 'u32' || kind === 'i32' || kind === 'u8n' || kind === 'u16n' || kind === 'f32' || kind === 'f64';
}

function isFieldType(value: unknown): value is FieldTypeDef {
  return typeof value === 'object' && value !== null && 'kind' in value && 'originType' in value;
}

function withHeapBytes<T>(
  module: FastdbModule,
  bytes: Uint8Array,
  fn: (ptr: number, size: number) => T
): T {
  if (bytes.length === 0) {
    return fn(0, 0);
  }
  const ptr = module._malloc(bytes.length);
  try {
    module.HEAPU8.set(bytes, ptr);
    return fn(ptr, bytes.length);
  } finally {
    module._free(ptr);
  }
}
