import { mapFeatureFrom, type Feature, type FeatureClass } from './feature.js';
import { getClassSchema, getFieldDefinition } from './schema.js';
import { StridedColumn } from './column.js';
import { FastdbRuntimeError } from './errors.js';
import type {
  FastdbModule,
  WxDatabaseBuildHandle,
  WxDatabaseHandle,
  WxFeatureHandle,
  WxLayerTableBuildHandle,
  WxLayerTableHandle,
} from './wasm-loader.js';

type TableOrigin = WxLayerTableHandle | WxLayerTableBuildHandle;
type DatabaseOrigin = WxDatabaseHandle | WxDatabaseBuildHandle;

export class Table<T extends Feature> {
  readonly featureType: FeatureClass<T>;
  readonly origin: TableOrigin;
  readonly db: DatabaseOrigin;
  readonly module: FastdbModule;
  private columnAccessor: T | null = null;

  constructor(
    featureType: FeatureClass<T>,
    origin: TableOrigin,
    db: DatabaseOrigin,
    module: FastdbModule
  ) {
    this.featureType = featureType;
    this.origin = origin;
    this.db = db;
    this.module = module;
  }

  get length(): number {
    return this.fixed ? (this.origin as WxLayerTableHandle).getFeatureCount() : 0;
  }

  get fixed(): boolean {
    return 'tryGetFeatureAt' in this.origin;
  }

  get name(): string {
    return this.origin.name();
  }

  get(index: number): T {
    if (!this.fixed) {
      throw new FastdbRuntimeError('Row access is only supported for fixed tables.');
    }
    const feature = (this.origin as WxLayerTableHandle).tryGetFeatureAt(this.normalizeIndex(index));
    return mapFeatureFrom(this.featureType, this.db, feature);
  }

  get column(): T {
    if (!this.fixed) {
      throw new FastdbRuntimeError('Column access is only supported for fixed tables.');
    }
    if (this.columnAccessor !== null) {
      return this.columnAccessor;
    }

    const layer = this.origin as WxLayerTableHandle;
    const table = this;
    const schema = getClassSchema(this.featureType);
    const proxy = new Proxy(
      {},
      {
        get(_, prop) {
          if (typeof prop !== 'string') {
            return undefined;
          }
          const def = getFieldDefinition(schema, prop);
          if (!def) {
            return undefined;
          }
          return new StridedColumn(table.module, layer, def.index, def.entry.originType);
        },
      }
    );

    this.columnAccessor = proxy as T;
    return this.columnAccessor;
  }

  *[Symbol.iterator](): Iterator<T> {
    for (let i = 0; i < this.length; i += 1) {
      yield this.get(i);
    }
  }

  *iterReuse(): Iterator<T> {
    if (!this.fixed) {
      throw new FastdbRuntimeError('iterReuse() only supports fixed tables.');
    }

    const wrapper = mapFeatureFrom(this.featureType, this.db, null);
    const target = wrapper as T & { _origin: WxFeatureHandle | null; _cache: Record<string, unknown> | null };
    for (let i = 0; i < this.length; i += 1) {
      target._origin = (this.origin as WxLayerTableHandle).tryGetFeatureAt(i);
      target._cache = null;
      yield wrapper;
    }
  }

  fill(columns: Record<string, ArrayLike<number>>): void {
    for (const [name, values] of Object.entries(columns)) {
      const col = (this.column as unknown as Record<string, StridedColumn>)[name];
      if (!col) {
        throw new FastdbRuntimeError(`Column "${name}" does not exist.`);
      }
      col.fill(values);
    }
  }

  private normalizeIndex(index: number): number {
    const normalized = index < 0 ? this.length + index : index;
    if (normalized < 0 || normalized >= this.length) {
      throw new FastdbRuntimeError(`Row index ${index} is out of range [0, ${this.length}).`);
    }
    return normalized;
  }
}
