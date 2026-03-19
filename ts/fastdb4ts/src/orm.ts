import { Feature, type FeatureClass, type FeatureDatabaseHandle } from './feature.js';
import { getClassSchema } from './schema.js';
import { FastdbRuntimeError, FastdbUsageError } from './errors.js';
import { Table } from './table.js';
import { getInitializedFastdbModule } from './wasm-loader.js';
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
    const orm = ORM.create();    const build = orm.getBuildOrigin();
    for (const defn of defns) {
      if (defn.capacity <= 0) {
        throw new FastdbUsageError('Table capacity must be positive.');
      }
      orm.ensureTruncateCompatible(defn.featureType);
      const layerName = defn.name ?? defn.featureType.name;
      const table = build.createLayerBegin(layerName);
      table.setGeometryType(orm.module.gtAny, orm.module.cfDefault, false);
      orm.defineTableFields(table, defn.featureType);
      build.createLayerEnd();
      build.truncate(layerName, defn.capacity);
    }
    orm.combine();
    return orm;
  }

  static fromBuffer(data: Uint8Array | ArrayBuffer): ORM {
    const module = ORM.getModule();
    const bytes = data instanceof Uint8Array ? data : new Uint8Array(data);
    const ptr = module._malloc(bytes.byteLength);
    try {
      module.HEAPU8.set(bytes, ptr);
      const db = module.WxDatabase.loadFromHeap(ptr, bytes.byteLength);
      return new ORM(db, module);
    } finally {
      module._free(ptr);
    }
  }

  push<T extends Feature>(feature: T, tableName?: string): void {
    const build = this.getBuildOrigin();
    const featureType = feature.constructor as FeatureClass<T>;
    const name = tableName ?? featureType.name;
    let table = this.tableMap.get(name) as Table<T> | undefined;
    let origin: WxLayerTableBuildHandle;

    if (!table) {
      origin = build.createLayerBegin(name);
      origin.setGeometryType(this.module.gtAny, this.module.cfDefault, false);
      this.defineTableFields(origin, featureType);
      this.tableMap.set(name, new Table(featureType, origin, build, this.module) as Table<Feature>);
      table = this.tableMap.get(name) as Table<T>;
    }

    origin = table.origin as WxLayerTableBuildHandle;
    origin.addFeatureBegin();
    try {
      const schema = getClassSchema(featureType);
      for (const field of schema.fieldList) {
        const value = (feature as Record<string, unknown>)[field.name];
        const kind = field.entry.kind;
        if (kind === 'bool') {
          origin.setFieldInt(field.index, value ? 1 : 0);
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
        } else if (kind === 'str' || kind === 'wstr') {
          origin.setFieldString(field.index, String(value ?? ''));
        } else {
          throw new FastdbUsageError(`push() does not support field kind "${kind}" yet.`);
        }
      }
    } finally {
      origin.addFeatureEnd();
    }
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
    for (const field of schema.fieldList) {
      table.addField(field.name, field.entry.originType, 0, 1);
    }
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
