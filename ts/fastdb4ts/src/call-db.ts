import { Feature, createFeature, type FeatureClass } from './feature.js';
import { FastdbUsageError } from './errors.js';
import { ORM } from './orm.js';
import { loadDatabaseFromBytes, type FastdbDatabaseBytes } from './database-buffer.js';
import type { StridedColumn } from './column.js';
import type { Table } from './table.js';
import { defineSchema, getClassSchema, resolveListItem, type SchemaFieldDefinition } from './schema.js';
import { getInitializedFastdbModule, type FastdbModule, type WxDatabaseHandle, type WxFeatureHandle, type WxLayerTableBuildHandle } from './wasm-loader.js';
import {
  BOOL,
  BYTES,
  F32,
  F64,
  I32,
  STR,
  U8,
  U8N,
  U16,
  U16N,
  U32,
  WSTR,
  type FieldTypeDef,
  coerceBoolScalar,
  isListField,
  isRefField,
} from './types.js';

export interface FastdbFeatureCodecBinding<T extends Feature = Feature> {
  readonly codecId: string;
  readonly profile: string;
  readonly schemaSha256: string;
  readonly feature: FeatureClass<T>;
}

export interface FastdbCallDbScalarField {
  readonly name: string;
  readonly kind: string;
  readonly parameter?: string;
  readonly valuePosition: number;
}

export interface FastdbCallDbArrayItem {
  readonly name: string;
  readonly kind: string;
}

export interface FastdbCallDbFeatureDependency {
  readonly feature: FeatureClass;
  readonly featureSchemaSha256: string;
}

export interface FastdbCallDbTable {
  readonly name: string;
  readonly kind: 'scalars' | 'array' | 'feature';
  readonly cardinality: string;
  readonly feature?: FeatureClass;
  readonly featureSchemaSha256?: string;
  readonly featureDependencies?: readonly FastdbCallDbFeatureDependency[];
  readonly parameter?: string;
  readonly returnIndex?: number;
  readonly valuePosition?: number;
  readonly fields?: readonly FastdbCallDbScalarField[];
  readonly item?: FastdbCallDbArrayItem;
}

export interface FastdbCallDbBinding {
  readonly codecId: string;
  readonly profile: string;
  readonly schemaSha256: string;
  readonly method: string;
  readonly direction: 'input' | 'output';
  readonly tables: readonly FastdbCallDbTable[];
}

const CALL_DB_COLUMNAR_PROFILE = 'fastdb.call.columnar.v1';
const CALL_DB_OBJECT_GRAPH_PROFILE = 'fastdb.call.object-graph.v1';
const CALL_DB_CODEC_ID = 'org.fastdb.call-db';
const CALL_DB_ARRAY_VALUE_FIELD = 'value';
const FEATURE_COLUMNAR_PROFILE = 'columnar.v1';
const FEATURE_OBJECT_GRAPH_PROFILE = 'object_graph.v1';
const FEATURE_COLUMNAR_CODEC_ID = 'org.fastdb.columnar';
const FEATURE_OBJECT_GRAPH_CODEC_ID = 'org.fastdb.object-graph';
const BULK_APPEND_KINDS = new Set(['bool', 'u8', 'u16', 'u32', 'i32', 'u8n', 'u16n', 'f32', 'f64']);
const VALIDATED_COLUMNAR_FEATURES = new WeakSet<FeatureClass>();

const FIELD_TYPES: Record<string, FieldTypeDef> = {
  bool: BOOL,
  bytes: BYTES,
  f32: F32,
  f64: F64,
  i32: I32,
  str: STR,
  u8: U8,
  u8n: U8N,
  u16: U16,
  u16n: U16N,
  u32: U32,
  wstr: WSTR,
};

const DYNAMIC_FEATURE_CACHE = new WeakMap<object, FeatureClass>();

interface ColumnarFastPathPlan {
  readonly featureType: FeatureClass;
  readonly tableName: string;
  readonly rows: readonly unknown[];
  readonly fields: readonly ColumnarFastPathField[];
}

interface ColumnarFastPathField {
  readonly index: number;
  readonly kind: string;
  readonly value: (row: unknown) => unknown;
}

export function encodeFastdbFeature<T extends Feature>(
  binding: FastdbFeatureCodecBinding<T>,
  value: T
): Uint8Array {
  if (binding.profile === FEATURE_OBJECT_GRAPH_PROFILE) {
    ensureCodecId(binding.codecId, FEATURE_OBJECT_GRAPH_CODEC_ID);
    ensureObjectGraphFeatureRuntime(binding.feature, `fastdb feature codec "${binding.feature.name}"`);
    return encodeObjectGraphFeature(ensureFeatureValue(value, binding.feature, binding.feature.name));
  }
  if (binding.profile !== FEATURE_COLUMNAR_PROFILE) {
    throw new FastdbUsageError(`Unsupported fastdb feature codec profile "${binding.profile}".`);
  }
  ensureCodecId(binding.codecId, FEATURE_COLUMNAR_CODEC_ID);
  ensureColumnarFeatureRuntime(binding.feature, `fastdb feature codec "${binding.feature.name}"`);
  const orm = ORM.create();
  try {
    orm.push(value);
    orm.combine();
    return orm.toBuffer();
  } finally {
    orm.close();
  }
}

export function decodeFastdbFeature<T extends Feature>(
  binding: FastdbFeatureCodecBinding<T>,
  payload: FastdbDatabaseBytes
): T {
  if (binding.profile === FEATURE_OBJECT_GRAPH_PROFILE) {
    ensureCodecId(binding.codecId, FEATURE_OBJECT_GRAPH_CODEC_ID);
    ensureObjectGraphFeatureRuntime(binding.feature, `fastdb feature codec "${binding.feature.name}"`);
    return decodeObjectGraphFeature(binding.feature, payload) as T;
  }
  if (binding.profile !== FEATURE_COLUMNAR_PROFILE) {
    throw new FastdbUsageError(`Unsupported fastdb feature codec profile "${binding.profile}".`);
  }
  ensureCodecId(binding.codecId, FEATURE_COLUMNAR_CODEC_ID);
  ensureColumnarFeatureRuntime(binding.feature, `fastdb feature codec "${binding.feature.name}"`);
  const orm = ORM.fromBuffer(payload);
  try {
    const table = orm.table(binding.feature);
    if (table.length < 1) {
      throw new FastdbUsageError(`fastdb call-db feature payload table "${binding.feature.name}" is empty.`);
    }
    return copyFeature(binding.feature, table.get(0)) as T;
  } finally {
    orm.close();
  }
}

export function encodeFastdbCallDb(
  binding: FastdbCallDbBinding,
  value: unknown
): Uint8Array {
  if (binding.profile === CALL_DB_OBJECT_GRAPH_PROFILE) {
    ensureObjectGraphCallDb(binding);
    const values = normalizeCallValues(binding, value);
    return encodeObjectGraphCallDb(binding, values);
  }
  ensureColumnarCallDb(binding);
  const values = normalizeCallValues(binding, value);
  const fastPathPayload = tryEncodeColumnarCallDbFastPath(binding, values);
  if (fastPathPayload !== null) {
    return fastPathPayload;
  }
  const orm = ORM.create();
  try {
    for (const table of binding.tables) {
      encodeCallTable(orm, table, values);
    }
    orm.combine();
    return orm.toBuffer();
  } finally {
    orm.close();
  }
}

export function decodeFastdbCallDb(
  binding: FastdbCallDbBinding,
  payload: FastdbDatabaseBytes
): unknown {
  if (binding.profile === CALL_DB_OBJECT_GRAPH_PROFILE) {
    ensureObjectGraphCallDb(binding);
    return decodeObjectGraphCallDb(binding, payload);
  }
  ensureColumnarCallDb(binding);
  const orm = ORM.fromBuffer(payload);
  try {
    const values: unknown[] = new Array(callValueCount(binding)).fill(undefined);
    for (const table of binding.tables) {
      decodeCallTable(orm, table, values);
    }
    if (binding.direction === 'input') {
      return values;
    }
    return values.length === 1 ? values[0] : values;
  } finally {
    orm.close();
  }
}

export function viewFastdbCallDb(
  binding: FastdbCallDbBinding,
  payload: FastdbDatabaseBytes
): FastdbCallDbView {
  ensureCodecId(binding.codecId, CALL_DB_CODEC_ID);
  if (binding.profile === CALL_DB_OBJECT_GRAPH_PROFILE) {
    ensureObjectGraphCallDb(binding);
    const ctx = loadObjectGraphContext(payload, objectGraphBindingFeatureTypes(binding));
    try {
      return new FastdbCallDbView(binding, ctx);
    } catch (error) {
      ctx.db.delete();
      throw error;
    }
  }
  ensureColumnarCallDb(binding);
  return new FastdbCallDbView(binding, ORM.fromBuffer(payload));
}

export class FastdbCallDbView {
  private closed = false;
  private readonly mode: 'columnar' | 'object-graph';

  constructor(
    readonly binding: FastdbCallDbBinding,
    private readonly storage: ORM | ObjectGraphLoadContext
  ) {
    if (binding.profile === CALL_DB_OBJECT_GRAPH_PROFILE) {
      ensureObjectGraphCallDb(binding);
      if (!isObjectGraphLoadContext(storage)) {
        throw new FastdbUsageError('fastdb call-db object-graph call-db view requires an object-graph database context.');
      }
      this.mode = 'object-graph';
    } else {
      ensureColumnarCallDb(binding);
      if (!(storage instanceof ORM)) {
        throw new FastdbUsageError('fastdb call-db columnar call-db view requires an ORM instance.');
      }
      this.mode = 'columnar';
    }
  }

  materialize(): unknown {
    this.ensureOpen();
    if (this.mode === 'object-graph') {
      return decodeObjectGraphCallDbFromContext(this.binding, this.objectGraphContext());
    }
    const values: unknown[] = new Array(callValueCount(this.binding)).fill(undefined);
    for (const table of this.binding.tables) {
      decodeCallTable(this.orm(), table, values);
    }
    if (this.binding.direction === 'input') {
      return values;
    }
    return values.length === 1 ? values[0] : values;
  }

  table(nameOrIndex: string | number): FastdbCallDbTableView {
    const spec = this.resolveTable('feature', nameOrIndex);
    return new FastdbCallDbTableView(this, spec);
  }

  feature(nameOrIndex: string | number): Feature {
    const spec = this.resolveTable('feature', nameOrIndex);
    if (spec.cardinality !== 'one') {
      throw new FastdbUsageError(`fastdb call-db feature view "${spec.name}" has cardinality "${spec.cardinality}"; use table() for batch outputs.`);
    }
    if (this.tableLength(spec) < 1) {
      throw new FastdbUsageError(`fastdb call-db feature table "${spec.name}" is empty.`);
    }
    return this.tableFeature(spec, 0);
  }

  array(nameOrIndex: string | number): FastdbCallDbArrayView {
    const spec = this.resolveTable('array', nameOrIndex);
    return new FastdbCallDbArrayView(this, spec);
  }

  scalar(nameOrIndex: string | number): unknown {
    const { field, spec } = this.resolveScalarField(nameOrIndex);
    if (this.tableLength(spec) < 1) {
      throw new FastdbUsageError(`fastdb call-db scalar table "${spec.name}" is empty.`);
    }
    const row = this.tableFeature(spec, 0) as unknown as Record<string, unknown>;
    return materializeScalarValue(field.kind, row[field.name]);
  }

  close(): void {
    if (this.closed) {
      return;
    }
    this.closed = true;
    if (this.mode === 'object-graph') {
      this.objectGraphContext().db.delete();
    } else {
      this.orm().close();
    }
  }

  ensureOpen(): void {
    if (this.closed) {
      throw new FastdbUsageError('fastdb call-db call-db view is closed.');
    }
  }

  tableLength(spec: FastdbCallDbTable): number {
    if (this.mode === 'object-graph') {
      const layerIdx = this.objectGraphLayerIndex(spec);
      return layerIdx === null ? 0 : this.objectGraphContext().db.getLayer(layerIdx).getFeatureCount();
    }
    return this.tableFor(spec).length;
  }

  tableFeature(spec: FastdbCallDbTable, index: number): Feature {
    if (this.mode === 'object-graph') {
      const layerIdx = this.objectGraphLayerIndex(spec);
      if (layerIdx === null) {
        throw new FastdbUsageError(`fastdb call-db object-graph table "${spec.name}" was not found.`);
      }
      const featureType = spec.kind === 'feature' ? requireFeature(spec) : dynamicFeatureForTable(spec);
      return copyObjectGraphFeature(this.objectGraphContext(), featureType, layerIdx, index);
    }
    return this.tableFor(spec).get(index);
  }

  arrayValue(spec: FastdbCallDbTable, index: number): unknown {
    return (this.tableFeature(spec, index) as unknown as Record<string, unknown>).value;
  }

  tableColumn(spec: FastdbCallDbTable, fieldName: string): FastdbCallDbColumnSource | undefined {
    if (this.mode === 'object-graph') {
      throw new FastdbUsageError('fastdb call-db object-graph call-db views do not expose column access; use table().get(...), array(), scalar(), feature(), or materialize().');
    }
    const field = columnFieldForTable(spec, fieldName);
    if (!field) {
      return undefined;
    }
    if (field.kind === 'list') {
      throw new FastdbUsageError(
        `Column accessor "${fieldName}" is a list field; read it from table rows instead of as a strided column.`
      );
    }
    if (field.kind === 'ref') {
      throw new FastdbUsageError(
        `Column accessor "${fieldName}" is a ref field; read it from table rows instead of as a strided column.`
      );
    }
    if (canBulkAppendKind(field.kind)) {
      const column = (this.tableFor(spec).column as unknown as Record<string, StridedColumn>)[fieldName];
      if (column) {
        return new FastdbStridedColumnSource(column);
      }
    }
    return new FastdbRowFieldColumnSource(this, spec, field.name, field.kind);
  }

  private tableFor(spec: FastdbCallDbTable): Table<Feature> {
    this.ensureOpen();
    if (spec.kind === 'feature') {
      return this.orm().table(requireFeature(spec), spec.name) as Table<Feature>;
    }
    if (spec.kind === 'array' || spec.kind === 'scalars') {
      return this.orm().table(dynamicFeatureForTable(spec), spec.name) as Table<Feature>;
    }
    throw new FastdbUsageError(`fastdb call-db table "${spec.name}" is not a supported view table.`);
  }

  private objectGraphLayerIndex(spec: FastdbCallDbTable): number | null {
    this.ensureOpen();
    const ctx = this.objectGraphContext();
    if (spec.kind === 'feature') {
      return layerIndexForType(ctx.db, requireFeature(spec));
    }
    if (spec.kind === 'array' || spec.kind === 'scalars') {
      return layerIndexForName(ctx.db, spec.name);
    }
    throw new FastdbUsageError(`fastdb call-db object-graph table "${spec.name}" is not a supported view table.`);
  }

  private orm(): ORM {
    if (!(this.storage instanceof ORM)) {
      throw new FastdbUsageError('fastdb call-db call-db view is not columnar.');
    }
    return this.storage;
  }

  private objectGraphContext(): ObjectGraphLoadContext {
    if (!isObjectGraphLoadContext(this.storage)) {
      throw new FastdbUsageError('fastdb call-db call-db view is not object-graph.');
    }
    return this.storage;
  }

  private resolveTable(kind: 'array' | 'feature', nameOrIndex: string | number): FastdbCallDbTable {
    this.ensureOpen();
    const tables = this.binding.tables.filter((table) => table.kind === kind);
    if (typeof nameOrIndex === 'number') {
      const table = tables[nameOrIndex];
      if (!table) {
        throw new FastdbUsageError(`fastdb call-db ${kind} table index ${nameOrIndex} is out of range.`);
      }
      return table;
    }
    const table = tables.find((item) => item.name === nameOrIndex);
    if (!table) {
      throw new FastdbUsageError(`fastdb call-db ${kind} table "${nameOrIndex}" was not found.`);
    }
    return table;
  }

  private resolveScalarField(nameOrIndex: string | number): {
    readonly field: FastdbCallDbScalarField;
    readonly spec: FastdbCallDbTable;
  } {
    this.ensureOpen();
    const fields = this.binding.tables.flatMap((table) =>
      table.kind === 'scalars'
        ? (table.fields ?? []).map((field) => ({ field, spec: table }))
        : []
    );
    if (typeof nameOrIndex === 'number') {
      const entry = fields[nameOrIndex];
      if (!entry) {
        throw new FastdbUsageError(`fastdb call-db scalar field index ${nameOrIndex} is out of range.`);
      }
      return entry;
    }
    const entry = fields.find((item) => item.field.name === nameOrIndex);
    if (!entry) {
      throw new FastdbUsageError(`fastdb call-db scalar field "${nameOrIndex}" was not found.`);
    }
    return entry;
  }
}

type FastdbCallDbColumnArray = ReturnType<StridedColumn['toArray']> | unknown[];

interface FastdbCallDbColumnSource {
  readonly length: number;
  get(index: number): unknown;
  toArray(): FastdbCallDbColumnArray;
  forEach(fn: (value: unknown, index: number) => void): void;
}

class FastdbStridedColumnSource implements FastdbCallDbColumnSource {
  constructor(private readonly column: StridedColumn) {}

  get length(): number {
    return this.column.length;
  }

  get(index: number): number {
    return this.column.get(index);
  }

  toArray(): ReturnType<StridedColumn['toArray']> {
    return this.column.toArray();
  }

  forEach(fn: (value: unknown, index: number) => void): void {
    this.column.forEach((value, index) => fn(value, index));
  }
}

class FastdbRowFieldColumnSource implements FastdbCallDbColumnSource {
  constructor(
    private readonly view: FastdbCallDbView,
    private readonly spec: FastdbCallDbTable,
    private readonly fieldName: string,
    private readonly kind: string
  ) {}

  get length(): number {
    return this.view.tableLength(this.spec);
  }

  get(index: number): unknown {
    const row = this.view.tableFeature(this.spec, index) as unknown as Record<string, unknown>;
    return materializeScalarValue(this.kind, row[this.fieldName]);
  }

  toArray(): unknown[] {
    const values: unknown[] = [];
    for (let index = 0; index < this.length; index += 1) {
      values.push(this.get(index));
    }
    return values;
  }

  forEach(fn: (value: unknown, index: number) => void): void {
    for (let index = 0; index < this.length; index += 1) {
      fn(this.get(index), index);
    }
  }
}

export class FastdbCallDbTableView {
  private columnAccessor: Record<string, FastdbCallDbColumnView> | null = null;

  constructor(
    private readonly view: FastdbCallDbView,
    private readonly spec: FastdbCallDbTable
  ) {}

  get length(): number {
    this.view.ensureOpen();
    return this.view.tableLength(this.spec);
  }

  get column(): Record<string, FastdbCallDbColumnView> {
    this.view.ensureOpen();
    if (this.columnAccessor !== null) {
      return this.columnAccessor;
    }
    const tableView = this;
    const accessor = new Proxy(
      {},
      {
        get(_, prop) {
          if (typeof prop !== 'string') {
            return undefined;
          }
          tableView.view.ensureOpen();
          const column = tableView.view.tableColumn(tableView.spec, prop);
          if (!column) {
            return undefined;
          }
          return new FastdbCallDbColumnView(tableView.view, column);
        },
      }
    );
    this.columnAccessor = accessor as Record<string, FastdbCallDbColumnView>;
    return this.columnAccessor;
  }

  get(index: number): Feature {
    this.view.ensureOpen();
    return this.view.tableFeature(this.spec, index);
  }

  *[Symbol.iterator](): Iterator<Feature> {
    for (let i = 0; i < this.length; i += 1) {
      yield this.get(i);
    }
  }

}

export class FastdbCallDbArrayView {
  constructor(
    private readonly view: FastdbCallDbView,
    private readonly spec: FastdbCallDbTable
  ) {}

  get length(): number {
    this.view.ensureOpen();
    return this.view.tableLength(this.spec);
  }

  get(index: number): unknown {
    this.view.ensureOpen();
    return this.view.arrayValue(this.spec, index);
  }

  toArray(): unknown[] {
    const values: unknown[] = [];
    for (let i = 0; i < this.length; i += 1) {
      values.push(this.get(i));
    }
    return values;
  }

  *[Symbol.iterator](): Iterator<unknown> {
    for (let i = 0; i < this.length; i += 1) {
      yield this.get(i);
    }
  }

}

export class FastdbCallDbColumnView {
  constructor(
    private readonly view: FastdbCallDbView,
    private readonly column: FastdbCallDbColumnSource
  ) {}

  get length(): number {
    this.view.ensureOpen();
    return this.column.length;
  }

  get(index: number): unknown {
    this.view.ensureOpen();
    return this.column.get(index);
  }

  toArray(): FastdbCallDbColumnArray {
    this.view.ensureOpen();
    return this.column.toArray();
  }

  forEach(fn: (value: unknown, index: number) => void): void {
    this.view.ensureOpen();
    this.column.forEach(fn);
  }
}

interface ObjectGraphLocation {
  readonly layerIdx: number;
  readonly featureIdx: number;
}

interface ObjectGraphLoadContext {
  readonly db: WxDatabaseHandle;
  readonly module: FastdbModule;
  readonly typeByLayerName: ReadonlyMap<string, FeatureClass>;
  readonly seen: Map<string, Feature>;
}

interface ObjectGraphTableGroup {
  readonly layerName: string;
  readonly featureType: FeatureClass;
  readonly rows: readonly Feature[];
}

function encodeObjectGraphFeature(value: Feature): Uint8Array {
  if (!(value instanceof Feature)) {
    throw new FastdbUsageError('fastdb call-db object-graph feature encode expects a Feature value.');
  }
  const ctx = collectObjectGraph([value]);
  return buildObjectGraphPayload(ctx);
}

function decodeObjectGraphFeature<T extends Feature>(
  featureType: FeatureClass<T>,
  payload: FastdbDatabaseBytes
): T {
  return withObjectGraphLoadContext(payload, [featureType], (ctx) =>
    copyObjectGraphFeature(ctx, featureType, layerIndexForType(ctx.db, featureType), 0) as T
  );
}

function encodeObjectGraphCallDb(
  binding: FastdbCallDbBinding,
  values: readonly unknown[]
): Uint8Array {
  const dynamicGroups: ObjectGraphTableGroup[] = [];
  const featureRoots: Feature[] = [];
  const explicitFeatureTables: FeatureClass[] = [];
  for (const table of binding.tables) {
    if (table.kind === 'scalars') {
      const featureType = dynamicFeatureForTable(table);
      const rowValues: Record<string, unknown> = {};
      for (const field of table.fields ?? []) {
        rowValues[field.name] = valueAt(values, field.valuePosition, table.name);
      }
      dynamicGroups.push({
        featureType,
        layerName: table.name,
        rows: [createDynamicFeature(featureType, rowValues)],
      });
      continue;
    }
    if (table.kind === 'array') {
      const featureType = dynamicFeatureForTable(table);
      const rows = asArray(valueAt(values, requiredPosition(table), table.name), table.name)
        .map((item) => createDynamicFeature(featureType, { value: item }));
      dynamicGroups.push({ featureType, layerName: table.name, rows });
      continue;
    }
    if (table.kind === 'feature') {
      const featureType = requireFeature(table);
      explicitFeatureTables.push(featureType);
      const value = valueAt(values, requiredPosition(table), table.name);
      if (table.cardinality === 'many') {
        for (const item of asArray(value, table.name)) {
          if (!(item instanceof featureType)) {
            throw new FastdbUsageError(`fastdb call-db object-graph table "${table.name}" expects ${featureType.name} rows.`);
          }
          featureRoots.push(item);
        }
        continue;
      }
      if (!(value instanceof featureType)) {
        throw new FastdbUsageError(`fastdb call-db object-graph table "${table.name}" expects a ${featureType.name} value.`);
      }
      featureRoots.push(value);
      continue;
    }
    throw new FastdbUsageError(`Unsupported fastdb call-db object-graph table kind "${table.kind}".`);
  }
  const ctx = collectObjectGraph(featureRoots);
  const featureGroups: ObjectGraphTableGroup[] = ctx.order.map((featureType) => ({
    featureType,
    layerName: featureType.name,
    rows: ctx.groups.get(featureType) ?? [],
  }));
  for (const featureType of explicitFeatureTables) {
    if (!featureGroups.some((group) => group.featureType === featureType)) {
      featureGroups.push({ featureType, layerName: featureType.name, rows: [] });
    }
  }
  return buildObjectGraphPayloadFromGroups([...dynamicGroups, ...featureGroups]);
}

function decodeObjectGraphCallDb(
  binding: FastdbCallDbBinding,
  payload: FastdbDatabaseBytes
): unknown {
  const featureTypes = objectGraphBindingFeatureTypes(binding);
  return withObjectGraphLoadContext(payload, featureTypes, (ctx) => {
    return decodeObjectGraphCallDbFromContext(binding, ctx);
  });
}

function decodeObjectGraphCallDbFromContext(
  binding: FastdbCallDbBinding,
  ctx: ObjectGraphLoadContext
): unknown {
  const values: unknown[] = new Array(callValueCount(binding)).fill(undefined);
  for (const table of binding.tables) {
    if (table.kind === 'scalars') {
      const featureType = dynamicFeatureForTable(table);
      const layerIdx = layerIndexForName(ctx.db, table.name);
      if (layerIdx === null) {
        throw new FastdbUsageError(`fastdb call-db object-graph scalar table "${table.name}" was not found.`);
      }
      const layer = ctx.db.getLayer(layerIdx);
      if (layer.getFeatureCount() < 1) {
        throw new FastdbUsageError(`fastdb call-db object-graph scalar table "${table.name}" is empty.`);
      }
      const row = copyObjectGraphFeature(ctx, featureType, layerIdx, 0) as unknown as Record<string, unknown>;
      for (const field of table.fields ?? []) {
        values[field.valuePosition] = materializeScalarValue(field.kind, row[field.name]);
      }
      continue;
    }
    if (table.kind === 'array') {
      const featureType = dynamicFeatureForTable(table);
      const layerIdx = layerIndexForName(ctx.db, table.name);
      if (layerIdx === null) {
        values[requiredPosition(table)] = [];
        continue;
      }
      const layer = ctx.db.getLayer(layerIdx);
      values[requiredPosition(table)] = Array.from(
        { length: layer.getFeatureCount() },
        (_, rowIdx) => {
          const row = copyObjectGraphFeature(ctx, featureType, layerIdx, rowIdx) as unknown as Record<string, unknown>;
          return materializeScalarValue(table.item?.kind ?? '', row.value);
        }
      );
      continue;
    }
    if (table.kind !== 'feature') {
      throw new FastdbUsageError(`Unsupported fastdb call-db object-graph table kind "${table.kind}".`);
    }
    const featureType = requireFeature(table);
    const layerIdx = layerIndexForType(ctx.db, featureType);
    if (table.cardinality === 'many') {
      const layer = ctx.db.getLayer(layerIdx);
      values[requiredPosition(table)] = Array.from(
        { length: layer.getFeatureCount() },
        (_, rowIdx) => copyObjectGraphFeature(ctx, featureType, layerIdx, rowIdx)
      );
    } else {
      values[requiredPosition(table)] = copyObjectGraphFeature(ctx, featureType, layerIdx, 0);
    }
  }
  if (binding.direction === 'input') {
    return values;
  }
  return values.length === 1 ? values[0] : values;
}

function objectGraphBindingFeatureTypes(binding: FastdbCallDbBinding): FeatureClass[] {
  const types: FeatureClass[] = [];
  const seen = new Set<FeatureClass>();
  for (const table of binding.tables) {
    if (table.kind !== 'feature') {
      continue;
    }
    const featureType = requireFeature(table);
    if (!seen.has(featureType)) {
      types.push(featureType);
      seen.add(featureType);
    }
    for (const dependency of table.featureDependencies ?? []) {
      if (!seen.has(dependency.feature)) {
        types.push(dependency.feature);
        seen.add(dependency.feature);
      }
    }
  }
  return types;
}

function collectObjectGraph(roots: readonly Feature[]): {
  readonly groups: ReadonlyMap<FeatureClass, Feature[]>;
  readonly locations: WeakMap<Feature, ObjectGraphLocation>;
  readonly order: readonly FeatureClass[];
} {
  const groups = new Map<FeatureClass, Feature[]>();
  const seen = new WeakSet<Feature>();

  const visit = (value: Feature): void => {
    if (seen.has(value)) {
      return;
    }
    seen.add(value);
    const featureType = value.constructor as FeatureClass;
    const rows = groups.get(featureType);
    if (rows) {
      rows.push(value);
    } else {
      groups.set(featureType, [value]);
    }
    for (const field of objectGraphTraversalFields(featureType)) {
      const fieldValue = getFeatureFieldValue(value, field.name);
      if (isRefField(field.entry)) {
        if (fieldValue instanceof Feature) {
          visit(fieldValue);
        }
        continue;
      }
      if (Array.isArray(fieldValue)) {
        for (const item of fieldValue) {
          if (item instanceof Feature) {
            visit(item);
          }
        }
      }
    }
  };

  for (const root of roots) {
    visit(root);
  }

  const order = topoSortObjectGraphTypes(groups);
  const locations = new WeakMap<Feature, ObjectGraphLocation>();
  for (const [layerIdx, featureType] of order.entries()) {
    const rows = groups.get(featureType) ?? [];
    rows.forEach((row, featureIdx) => locations.set(row, { layerIdx, featureIdx }));
  }
  return { groups, locations, order };
}

function buildObjectGraphPayload(ctx: {
  readonly groups: ReadonlyMap<FeatureClass, Feature[]>;
  readonly locations: WeakMap<Feature, ObjectGraphLocation>;
  readonly order: readonly FeatureClass[];
}): Uint8Array {
  return buildObjectGraphPayloadFromGroups(ctx.order.map((featureType) => ({
    featureType,
    layerName: featureType.name,
    rows: ctx.groups.get(featureType) ?? [],
  })));
}

function buildObjectGraphPayloadFromGroups(groups: readonly ObjectGraphTableGroup[]): Uint8Array {
  const module = getInitializedFastdbModule();
  const db = new module.WxDatabaseBuild();
  db.begin('');
  const layerBuilders = new Map<number, WxLayerTableBuildHandle>();
  const locations = new WeakMap<Feature, ObjectGraphLocation>();
  groups.forEach((group, layerIdx) => {
    group.rows.forEach((row, featureIdx) => locations.set(row, { layerIdx, featureIdx }));
  });
  try {
    for (const [layerIdx, group] of groups.entries()) {
      const layer = db.createLayerBegin(group.layerName);
      configureObjectGraphGeometry(module, layer, group.featureType);
      layer.setDbIndex(layerIdx);
      defineObjectGraphFields(layer, group.featureType);
      layerBuilders.set(layerIdx, layer);
    }
    for (const [layerIdx, group] of groups.entries()) {
      const layer = layerBuilders.get(layerIdx);
      if (!layer) {
        throw new FastdbUsageError(`Missing object-graph layer builder for ${group.layerName}.`);
      }
      for (const row of group.rows) {
        writeObjectGraphRow(module, layer, row, locations, layerBuilders);
      }
    }
    const stream = new module.WxMemoryStream();
    try {
      db.post(stream);
      return module.HEAPU8.slice(stream.dataView().data, stream.dataView().data + stream.dataView().size);
    } finally {
      stream.delete();
    }
  } finally {
    db.delete();
  }
}

function configureObjectGraphGeometry(
  module: FastdbModule,
  layer: WxLayerTableBuildHandle,
  featureType: FeatureClass
): void {
  if (featureBytesFieldNames(featureType).length > 0) {
    layer.setGeometryType(module.gtAny, module.cfDefault, false);
    return;
  }
  layer.setGeometryType(module.gtPoint, module.cfTx32, true);
  layer.setExtent(-180, -90, 180, 90);
}

function defineObjectGraphFields(layer: WxLayerTableBuildHandle, featureType: FeatureClass): void {
  for (const field of getClassSchema(featureType).fieldList) {
    if (isListField(field.entry)) {
      layer.addListField(field.name, objectGraphListElementType(field));
    } else {
      layer.addField(field.name, field.entry.originType, 0, 1);
    }
  }
}

function writeObjectGraphRow(
  module: FastdbModule,
  layer: WxLayerTableBuildHandle,
  row: Feature,
  locations: WeakMap<Feature, ObjectGraphLocation>,
  layerBuilders: ReadonlyMap<number, WxLayerTableBuildHandle>
): void {
  layer.addFeatureBegin();
  try {
    for (const field of getClassSchema(row.constructor as FeatureClass).fieldList) {
      const value = getFeatureFieldValue(row, field.name);
      if (isRefField(field.entry)) {
        layer.setFieldRef(field.index, objectGraphRefPtr(value, locations, layerBuilders));
        continue;
      }
      if (isListField(field.entry)) {
        if (isFieldType(resolveListItem(field.entry))) {
          const bytes = encodeNumericListBytes(value, listElementFieldType(field));
          withHeapBytes(module, bytes, (ptr, size) => layer.setFieldListNumeric(field.index, ptr, size));
        } else {
          const bytes = encodeObjectGraphRefList(value, locations);
          withHeapBytes(module, bytes, (ptr, size) => layer.setFieldListNumeric(field.index, ptr, size));
        }
        continue;
      }
      writeObjectGraphScalarField(module, layer, field, value);
    }
  } finally {
    layer.addFeatureEnd();
  }
}

function writeObjectGraphScalarField(
  module: FastdbModule,
  layer: WxLayerTableBuildHandle,
  field: SchemaFieldDefinition,
  value: unknown
): void {
  switch (field.entry.kind) {
    case 'bool':
      layer.setFieldInt(field.index, coerceBoolScalar(value) ? 1 : 0);
      return;
    case 'u8':
    case 'u16':
    case 'u32':
    case 'i32':
    case 'u8n':
    case 'u16n':
      layer.setFieldInt(field.index, Math.trunc(Number(value)));
      return;
    case 'f32':
    case 'f64':
      layer.setFieldDouble(field.index, Number(value));
      return;
    case 'str':
      layer.setFieldString(field.index, String(value ?? ''));
      return;
    case 'wstr':
      layer.setFieldWString(field.index, String(value ?? ''));
      return;
    case 'bytes': {
      const bytes = normalizeBytes(value);
      withHeapBytes(module, bytes, (ptr, size) => layer.setGeometryRaw(ptr, size));
      return;
    }
    default:
      throw new FastdbUsageError(`Unsupported object-graph field kind "${field.entry.kind}" on "${field.name}".`);
  }
}

function objectGraphRefPtr(
  value: unknown,
  locations: WeakMap<Feature, ObjectGraphLocation>,
  layerBuilders: ReadonlyMap<number, WxLayerTableBuildHandle>
): number {
  if (!(value instanceof Feature)) {
    return 0;
  }
  const location = locations.get(value);
  if (!location) {
    return 0;
  }
  const layer = layerBuilders.get(location.layerIdx);
  if (!layer) {
    return 0;
  }
  return layer.createFeatureRef(location.featureIdx);
}

function encodeObjectGraphRefList(
  value: unknown,
  locations: WeakMap<Feature, ObjectGraphLocation>
): Uint8Array {
  const items = Array.isArray(value) ? value : [];
  const bytes = new Uint8Array(items.length * 5);
  const view = new DataView(bytes.buffer);
  items.forEach((item, index) => {
    if (!(item instanceof Feature)) {
      return;
    }
    const location = locations.get(item);
    if (!location) {
      return;
    }
    const offset = index * 5;
    view.setUint16(offset, location.layerIdx, true);
    view.setUint8(offset + 2, location.featureIdx & 0xff);
    view.setUint16(offset + 3, location.featureIdx >> 8, true);
  });
  return bytes;
}

function withObjectGraphLoadContext<T>(
  payload: FastdbDatabaseBytes,
  featureTypes: readonly FeatureClass[],
  fn: (ctx: ObjectGraphLoadContext) => T
): T {
  const ctx = loadObjectGraphContext(payload, featureTypes);
  try {
    return fn(ctx);
  } finally {
    ctx.db.delete();
  }
}

function loadObjectGraphContext(
  payload: FastdbDatabaseBytes,
  featureTypes: readonly FeatureClass[]
): ObjectGraphLoadContext {
  const module = getInitializedFastdbModule();
  const db = loadDatabaseFromBytes(module, payload);
  try {
    const typeByLayerName = new Map<string, FeatureClass>();
    for (const featureType of featureTypes) {
      typeByLayerName.set(featureType.name, featureType);
      for (const dependency of discoverObjectGraphTypes(featureType)) {
        typeByLayerName.set(dependency.name, dependency);
      }
    }
    return { db, module, seen: new Map(), typeByLayerName };
  } catch (error) {
    db.delete();
    throw error;
  }
}

function isObjectGraphLoadContext(value: unknown): value is ObjectGraphLoadContext {
  return typeof value === 'object' && value !== null && 'db' in value && 'module' in value && 'seen' in value;
}

function copyObjectGraphFeature(
  ctx: ObjectGraphLoadContext,
  expectedType: FeatureClass,
  layerIdx: number,
  rowIdx: number
): Feature {
  const key = `${layerIdx}:${rowIdx}`;
  const cached = ctx.seen.get(key);
  if (cached) {
    return cached;
  }
  const layer = ctx.db.getLayer(layerIdx);
  const featureType = ctx.typeByLayerName.get(layer.name()) ?? expectedType;
  const row = layer.tryGetFeatureAt(rowIdx);
  const copy = createFeature(featureType);
  ctx.seen.set(key, copy);
  const cache = copy._getCache();
  for (const field of getClassSchema(featureType).fieldList) {
    cache[field.name] = readObjectGraphField(ctx, row, field);
  }
  return copy;
}

function readObjectGraphField(
  ctx: ObjectGraphLoadContext,
  row: WxFeatureHandle,
  field: SchemaFieldDefinition
): unknown {
  if (isRefField(field.entry)) {
    const refPtr = row.getFieldAsRef(field.index);
    return copyObjectGraphRef(ctx, refPtr, field.target as FeatureClass | undefined);
  }
  if (isListField(field.entry)) {
    const item = resolveListItem(field.entry);
    if (isFieldType(item)) {
      const listView = row.getFieldAsListView(field.index);
      return decodeNumericListBytes(
        ctx.module.HEAPU8.slice(listView.data, listView.data + listView.size),
        item
      );
    }
    const values: unknown[] = [];
    const count = row.getFieldListSize(field.index);
    for (let index = 0; index < count; index += 1) {
      values.push(copyObjectGraphRef(ctx, row.getFieldListRefAt(field.index, index), item as FeatureClass));
    }
    return values;
  }
  return readObjectGraphScalarField(ctx.module, row, field);
}

function copyObjectGraphRef(
  ctx: ObjectGraphLoadContext,
  refPtr: number,
  declaredTarget?: FeatureClass
): Feature | null {
  const ref = decodeFeatureRef(ctx.module, refPtr);
  if (ref === null) {
    return null;
  }
  const { layerIdx, rowIdx } = ref;
  const layer = ctx.db.getLayer(layerIdx);
  const target = declaredTarget ?? ctx.typeByLayerName.get(layer.name());
  if (!target) {
    return null;
  }
  return copyObjectGraphFeature(ctx, target, layerIdx, rowIdx);
}

function decodeFeatureRef(
  module: FastdbModule,
  refPtr: number
): { readonly layerIdx: number; readonly rowIdx: number } | null {
  if (!refPtr) {
    return null;
  }
  const heap = module.HEAPU8;
  const layerIdx = heap[refPtr] | (heap[refPtr + 1] << 8);
  const rowIdx = heap[refPtr + 2] | (heap[refPtr + 3] << 8) | (heap[refPtr + 4] << 16);
  return { layerIdx, rowIdx };
}

function readObjectGraphScalarField(
  module: FastdbModule,
  row: WxFeatureHandle,
  field: SchemaFieldDefinition
): unknown {
  switch (field.entry.kind) {
    case 'bool':
      return row.getFieldAsInt(field.index) !== 0;
    case 'u8':
    case 'u16':
    case 'u32':
    case 'i32':
    case 'u8n':
    case 'u16n':
      return row.getFieldAsInt(field.index);
    case 'f32':
    case 'f64':
      return row.getFieldAsFloat(field.index);
    case 'str':
      return row.getFieldAsString(field.index);
    case 'wstr':
      return row.getFieldAsWString(field.index);
    case 'bytes':
      return module.HEAPU8.slice(row.geometryView().data, row.geometryView().data + row.geometryView().size);
    default:
      throw new FastdbUsageError(`Unsupported object-graph field kind "${field.entry.kind}" on "${field.name}".`);
  }
}

function objectGraphTraversalFields(featureType: FeatureClass): SchemaFieldDefinition[] {
  const fields: SchemaFieldDefinition[] = [];
  for (const field of getClassSchema(featureType).fieldList) {
    if (isRefField(field.entry)) {
      fields.push(field);
      continue;
    }
    if (!isListField(field.entry)) {
      continue;
    }
    if (!isFieldType(resolveListItem(field.entry))) {
      fields.push(field);
    }
  }
  return fields;
}

function discoverObjectGraphTypes(root: FeatureClass): FeatureClass[] {
  const out: FeatureClass[] = [];
  const seen = new Set<FeatureClass>();
  const visit = (featureType: FeatureClass): void => {
    if (seen.has(featureType)) {
      return;
    }
    seen.add(featureType);
    out.push(featureType);
    for (const field of objectGraphTraversalFields(featureType)) {
      if (isRefField(field.entry) && field.target) {
        visit(field.target as FeatureClass);
      } else if (isListField(field.entry)) {
        const item = resolveListItem(field.entry);
        if (!isFieldType(item)) {
          visit(item as FeatureClass);
        }
      }
    }
  };
  visit(root);
  return out;
}

function topoSortObjectGraphTypes(groups: ReadonlyMap<FeatureClass, readonly Feature[]>): FeatureClass[] {
  const inDegree = new Map<FeatureClass, number>();
  const outgoing = new Map<FeatureClass, FeatureClass[]>();
  for (const featureType of groups.keys()) {
    inDegree.set(featureType, 0);
    outgoing.set(featureType, []);
  }
  for (const featureType of groups.keys()) {
    const deps = new Set<FeatureClass>();
    for (const field of objectGraphTraversalFields(featureType)) {
      let dependency: FeatureClass | undefined;
      if (isRefField(field.entry)) {
        dependency = field.target as FeatureClass | undefined;
      } else if (isListField(field.entry)) {
        const item = resolveListItem(field.entry);
        dependency = isFieldType(item) ? undefined : item as FeatureClass;
      }
      if (dependency && dependency !== featureType && groups.has(dependency)) {
        deps.add(dependency);
      }
    }
    for (const dependency of deps) {
      outgoing.get(dependency)?.push(featureType);
      inDegree.set(featureType, (inDegree.get(featureType) ?? 0) + 1);
    }
  }
  const queue = Array.from(groups.keys()).filter((featureType) => (inDegree.get(featureType) ?? 0) === 0);
  const out: FeatureClass[] = [];
  while (queue.length > 0) {
    const current = queue.shift() as FeatureClass;
    out.push(current);
    for (const dependent of outgoing.get(current) ?? []) {
      const nextDegree = (inDegree.get(dependent) ?? 0) - 1;
      inDegree.set(dependent, nextDegree);
      if (nextDegree === 0) {
        queue.push(dependent);
      }
    }
  }
  if (out.length !== groups.size) {
    throw new FastdbUsageError('fastdb call-db object-graph runtime does not support circular class-level references.');
  }
  return out;
}

function layerIndexForType(db: WxDatabaseHandle, featureType: FeatureClass): number {
  for (let index = 0; index < db.getLayerCount(); index += 1) {
    if (db.getLayer(index).name() === featureType.name) {
      return index;
    }
  }
  throw new FastdbUsageError(`fastdb call-db object-graph layer "${featureType.name}" was not found.`);
}

function layerIndexForName(db: WxDatabaseHandle, name: string): number | null {
  for (let index = 0; index < db.getLayerCount(); index += 1) {
    if (db.getLayer(index).name() === name) {
      return index;
    }
  }
  return null;
}

function objectGraphListElementType(field: SchemaFieldDefinition): number {
  if (!isListField(field.entry)) {
    throw new FastdbUsageError(`Field "${field.name}" is not a list field.`);
  }
  const item = resolveListItem(field.entry);
  if (isFieldType(item)) {
    if (!isSupportedNativeListKind(item.kind)) {
      throw new FastdbUsageError(`List field "${field.name}" uses unsupported item kind "${item.kind}".`);
    }
    return item.originType;
  }
  return 11;
}

function listElementFieldType(field: SchemaFieldDefinition): FieldTypeDef {
  if (!isListField(field.entry)) {
    throw new FastdbUsageError(`Field "${field.name}" is not a list field.`);
  }
  const item = resolveListItem(field.entry);
  if (!isFieldType(item) || !item.arrayCtor || !isSupportedNativeListKind(item.kind)) {
    throw new FastdbUsageError(`List field "${field.name}" supports only native numeric scalar list elements.`);
  }
  return item;
}

function encodeNumericListBytes(value: unknown, item: FieldTypeDef): Uint8Array {
  if (value === null || value === undefined) {
    return new Uint8Array(0);
  }
  if (!item.arrayCtor) {
    throw new FastdbUsageError(`List item kind "${item.kind}" does not have a typed-array representation.`);
  }
  if (ArrayBuffer.isView(value) && !(value instanceof DataView) && value.constructor === item.arrayCtor) {
    const view = value as ArrayBufferView;
    return new Uint8Array(view.buffer, view.byteOffset, view.byteLength);
  }
  if (!Array.isArray(value) && !(ArrayBuffer.isView(value) && !(value instanceof DataView))) {
    throw new FastdbUsageError('fastdb call-db object-graph numeric list fields expect array or typed-array values.');
  }
  const values = Array.from(value as ArrayLike<unknown>);
  const typed = new item.arrayCtor(values.length);
  for (let index = 0; index < values.length; index += 1) {
    typed[index] = item.kind === 'bool' ? (coerceBoolScalar(values[index]) ? 1 : 0) : Number(values[index]);
  }
  return new Uint8Array(typed.buffer, typed.byteOffset, typed.byteLength);
}

function decodeNumericListBytes(bytes: Uint8Array, item: FieldTypeDef): unknown[] {
  if (bytes.length === 0) {
    return [];
  }
  if (!item.arrayCtor) {
    throw new FastdbUsageError(`List item kind "${item.kind}" does not have a typed-array representation.`);
  }
  const bytesPerElement = item.arrayCtor.BYTES_PER_ELEMENT;
  if (bytes.length % bytesPerElement !== 0) {
    throw new FastdbUsageError(`fastdb call-db list payload has ${bytes.length} byte(s), not a multiple of ${bytesPerElement}.`);
  }
  const source = typedArraySource(bytes);
  const typed = new item.arrayCtor(source.buffer, source.byteOffset, bytes.length / bytesPerElement);
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

function isSupportedNativeListKind(kind: string): boolean {
  return kind === 'bool' || kind === 'u8' || kind === 'u16' || kind === 'u32' || kind === 'i32' || kind === 'u8n' || kind === 'u16n' || kind === 'f32' || kind === 'f64';
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
  throw new FastdbUsageError(`fastdb call-db bytes fields expect Uint8Array or ArrayBuffer, got "${typeof value}".`);
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

function isFieldType(value: unknown): value is FieldTypeDef {
  return typeof value === 'object' && value !== null && 'kind' in value && 'originType' in value;
}

function getFeatureFieldValue(feature: Feature, fieldName: string): unknown {
  const cache = feature._cache;
  if (cache && fieldName in cache) {
    return cache[fieldName];
  }
  return (feature as unknown as Record<string, unknown>)[fieldName];
}

function ensureColumnarCallDb(binding: FastdbCallDbBinding): void {
  ensureCodecId(binding.codecId, CALL_DB_CODEC_ID);
  if (binding.profile !== CALL_DB_COLUMNAR_PROFILE) {
    throw new FastdbUsageError(`Unsupported fastdb call-db call-db profile "${binding.profile}".`);
  }
  ensureCallDbTableShape(binding);
  ensureColumnarCallDbRuntime(binding);
}

function ensureCodecId(actual: string, expected: string): void {
  if (actual !== expected) {
    throw new FastdbUsageError(`fastdb call-db runtime expected codec id "${expected}", got "${actual}".`);
  }
}

function ensureFeatureValue<T extends Feature>(
  value: unknown,
  featureType: FeatureClass<T>,
  context: string
): T {
  if (!(value instanceof featureType)) {
    throw new FastdbUsageError(`fastdb call-db ${context} expects a ${featureType.name} value.`);
  }
  return value;
}

function objectGraphFeatureForTable(table: FastdbCallDbTable): FeatureClass {
  if (table.kind !== 'feature') {
    throw new FastdbUsageError(`fastdb call-db object-graph table "${table.name}" is not a feature table.`);
  }
  return requireFeature(table);
}

function ensureObjectGraphFeatureRuntime(featureType: FeatureClass, context: string): void {
  const schema = getClassSchema(featureType);
  const bytesFields = featureBytesFieldNames(featureType);
  if (bytesFields.length > 1) {
    throw new FastdbUsageError(
      `${context} cannot represent multiple bytes fields ${JSON.stringify(bytesFields)}; fastdb object-graph bytes use the feature raw payload.`
    );
  }
  for (const field of schema.fieldList) {
    if (isListField(field.entry)) {
      const item = resolveListItem(field.entry);
      if (isFieldType(item) && (!item.arrayCtor || !isSupportedNativeListKind(item.kind))) {
        throw new FastdbUsageError(
          `${context} cannot represent list field "${field.name}" with item kind "${item.kind}" in the object-graph runtime.`
        );
      }
    }
  }
}

function featureBytesFieldNames(featureType: FeatureClass): string[] {
  return getClassSchema(featureType).fieldList
    .filter((field) => field.entry.kind === 'bytes')
    .map((field) => field.name);
}

function ensureObjectGraphCallDb(binding: FastdbCallDbBinding): void {
  ensureCodecId(binding.codecId, CALL_DB_CODEC_ID);
  if (binding.profile !== CALL_DB_OBJECT_GRAPH_PROFILE) {
    throw new FastdbUsageError(`Unsupported fastdb call-db call-db profile "${binding.profile}".`);
  }
  ensureCallDbTableShape(binding);
  const layerNames = new Map<string, { readonly kind: 'dependency' | 'feature-table' | 'table'; readonly owner: string }>();
  const claimLayerName = (
    layerName: string,
    owner: string,
    kind: 'dependency' | 'feature-table' | 'table'
  ): void => {
    const previous = layerNames.get(layerName);
    if (previous !== undefined) {
      if (previous.kind === 'dependency' && kind === 'dependency' && previous.owner === owner) {
        return;
      }
      throw new FastdbUsageError(
        `fastdb object-graph call-db cannot encode both "${previous.owner}" and "${owner}" as layer "${layerName}". Use distinct feature wrapper types or table names.`
      );
    }
    layerNames.set(layerName, { kind, owner });
  };
  for (const table of binding.tables) {
    if (table.kind === 'feature') {
      const featureType = objectGraphFeatureForTable(table);
      claimLayerName(featureType.name, `feature table ${table.name}`, 'feature-table');
      ensureObjectGraphFeatureRuntime(
        featureType,
        `fastdb call-db object-graph call-db table "${table.name}"`
      );
      for (const dependency of table.featureDependencies ?? []) {
        claimLayerName(dependency.feature.name, `dependency ${dependency.feature.name}`, 'dependency');
        ensureObjectGraphFeatureRuntime(
          dependency.feature,
          `fastdb call-db object-graph call-db dependency "${dependency.feature.name}"`
        );
      }
      continue;
    }
    if (table.kind === 'array') {
      claimLayerName(table.name, `table ${table.name}`, 'table');
      if (!table.item) {
        throw new FastdbUsageError(`fastdb call-db object-graph array table "${table.name}" is missing item metadata.`);
      }
      ensureObjectGraphScalarKind(
        table.item.kind,
        `fastdb call-db object-graph array table "${table.name}" item`
      );
      continue;
    }
    if (table.kind === 'scalars') {
      claimLayerName(table.name, `table ${table.name}`, 'table');
      const bytesFields = (table.fields ?? [])
        .filter((field) => field.kind === 'bytes')
        .map((field) => field.name);
      if (bytesFields.length > 1) {
        throw new FastdbUsageError(
          `fastdb call-db object-graph scalar table "${table.name}" cannot represent multiple bytes fields ${JSON.stringify(bytesFields)}; fastdb object-graph bytes use the feature raw payload.`
        );
      }
      for (const field of table.fields ?? []) {
        ensureObjectGraphScalarKind(
          field.kind,
          `fastdb call-db object-graph scalar field "${field.name}"`
        );
      }
      continue;
    }
    throw new FastdbUsageError(`Unsupported fastdb call-db object-graph table kind "${table.kind}".`);
  }
}

function ensureCallDbTableShape(binding: FastdbCallDbBinding): void {
  if (typeof binding.method !== 'string' || binding.method.length === 0) {
    throw new FastdbUsageError('fastdb call-db call-db binding must include a non-empty method name.');
  }
  if (binding.direction !== 'input' && binding.direction !== 'output') {
    throw new FastdbUsageError('fastdb call-db call-db binding direction must be "input" or "output".');
  }
  if (!Array.isArray(binding.tables)) {
    throw new FastdbUsageError('fastdb call-db call-db binding tables must be an array.');
  }
  const tableNames = new Set<string>();
  const positions: Array<{ readonly position: number; readonly owner: string }> = [];
  for (const entry of binding.tables as readonly unknown[]) {
    if (typeof entry !== 'object' || entry === null) {
      throw new FastdbUsageError('fastdb call-db call-db table entries must be objects.');
    }
    const table = entry as FastdbCallDbTable;
    if (typeof table.name !== 'string' || table.name.length === 0) {
      throw new FastdbUsageError('fastdb call-db call-db table entries must include a non-empty name.');
    }
    if (tableNames.has(table.name)) {
      throw new FastdbUsageError(`fastdb call-db call-db duplicate table name "${table.name}".`);
    }
    tableNames.add(table.name);
    if (table.kind === 'scalars') {
      if (table.cardinality !== 'one') {
        throw new FastdbUsageError(`fastdb call-db scalar table "${table.name}" must have cardinality "one".`);
      }
      if (!Array.isArray(table.fields)) {
        throw new FastdbUsageError(`fastdb call-db scalar table "${table.name}" must include fields metadata.`);
      }
      const scalarFieldNames = new Set<string>();
      for (const entry of table.fields as readonly unknown[]) {
        if (typeof entry !== 'object' || entry === null) {
          throw new FastdbUsageError(`fastdb call-db scalar table "${table.name}" field entries must be objects.`);
        }
        const field = entry as FastdbCallDbScalarField;
        if (typeof field.name !== 'string' || field.name.length === 0) {
          throw new FastdbUsageError(`fastdb call-db scalar table "${table.name}" fields must include non-empty names.`);
        }
        if (scalarFieldNames.has(field.name)) {
          throw new FastdbUsageError(`fastdb call-db duplicate scalar field name "${field.name}".`);
        }
        scalarFieldNames.add(field.name);
        positions.push({
          owner: `scalar field "${field.name}"`,
          position: requireCallValuePosition(
            field.valuePosition,
            `fastdb call-db scalar field "${field.name}"`
          ),
        });
      }
      continue;
    }
    if (table.kind === 'array') {
      if (table.cardinality !== 'many') {
        throw new FastdbUsageError(`fastdb call-db array table "${table.name}" must have cardinality "many".`);
      }
      if (typeof table.item !== 'object' || table.item === null) {
        throw new FastdbUsageError(`fastdb call-db array table "${table.name}" must include item metadata.`);
      }
      if (table.item.name !== CALL_DB_ARRAY_VALUE_FIELD) {
        throw new FastdbUsageError(`fastdb call-db array table "${table.name}" item name must be "${CALL_DB_ARRAY_VALUE_FIELD}".`);
      }
      positions.push({
        owner: `array table "${table.name}"`,
        position: requireCallValuePosition(
          table.valuePosition,
          `fastdb call-db array table "${table.name}"`
        ),
      });
      continue;
    }
    if (table.kind === 'feature') {
      if (table.cardinality !== 'one' && table.cardinality !== 'many') {
        throw new FastdbUsageError(`fastdb call-db feature table "${table.name}" must have cardinality "one" or "many".`);
      }
      if (typeof table.featureSchemaSha256 !== 'string' || table.featureSchemaSha256.length === 0) {
        throw new FastdbUsageError(`fastdb call-db feature table "${table.name}" must include a non-empty feature schema hash.`);
      }
      if (table.featureDependencies !== undefined) {
        if (!Array.isArray(table.featureDependencies)) {
          throw new FastdbUsageError(`fastdb call-db feature table "${table.name}" dependencies must be an array.`);
        }
        for (const entry of table.featureDependencies as readonly unknown[]) {
          if (typeof entry !== 'object' || entry === null) {
            throw new FastdbUsageError(`fastdb call-db feature table "${table.name}" dependency entries must be objects.`);
          }
          const dependency = entry as FastdbCallDbFeatureDependency;
          if (typeof dependency.featureSchemaSha256 !== 'string' || dependency.featureSchemaSha256.length === 0) {
            throw new FastdbUsageError(`fastdb call-db feature table "${table.name}" dependency schema hash must be non-empty.`);
          }
        }
      }
      positions.push({
        owner: `feature table "${table.name}"`,
        position: requireCallValuePosition(
          table.valuePosition,
          `fastdb call-db feature table "${table.name}"`
        ),
      });
      continue;
    }
    throw new FastdbUsageError(`Unsupported fastdb call-db table kind "${table.kind}".`);
  }
  ensureCallValuePositions(positions);
}

function requireCallValuePosition(position: unknown, context: string): number {
  if (!Number.isInteger(position) || (position as number) < 0) {
    throw new FastdbUsageError(`${context} must include a non-negative integer valuePosition.`);
  }
  return position as number;
}

function ensureCallValuePositions(
  positions: readonly { readonly position: number; readonly owner: string }[]
): void {
  const seen = new Map<number, string>();
  const duplicates: string[] = [];
  for (const { position, owner } of positions) {
    const previous = seen.get(position);
    if (previous !== undefined) {
      duplicates.push(`${position} used by ${previous} and ${owner}`);
      continue;
    }
    seen.set(position, owner);
  }
  if (duplicates.length > 0) {
    throw new FastdbUsageError(`fastdb call-db call-db duplicate valuePosition metadata: ${duplicates.join(', ')}.`);
  }
  const actual = [...seen.keys()].sort((left, right) => left - right);
  const expected = Array.from({ length: actual.length }, (_unused, index) => index);
  if (actual.length !== expected.length || actual.some((position, index) => position !== expected[index])) {
    throw new FastdbUsageError(
      `fastdb call-db call-db valuePosition metadata must be contiguous from 0; got ${JSON.stringify(actual)}.`
    );
  }
}

function ensureObjectGraphScalarKind(kind: string, context: string): void {
  if (kind === 'list') {
    throw new FastdbUsageError(
      `${context} cannot represent list scalar metadata in the TypeScript object-graph call-db runtime yet; array-table list item support is not implemented.`
    );
  }
  if (kind === 'ref') {
    throw new FastdbUsageError(`${context} cannot represent ref scalar metadata in object-graph call-db.`);
  }
  if (!FIELD_TYPES[kind]) {
    throw new FastdbUsageError(`Unsupported fastdb call-db object-graph scalar kind "${kind}".`);
  }
}

function ensureColumnarCallDbRuntime(binding: FastdbCallDbBinding): void {
  for (const table of binding.tables) {
    if (table.kind === 'feature') {
      ensureColumnarFeatureRuntime(
        requireFeature(table),
        `fastdb call-db call-db feature table "${table.name}"`
      );
      if ((table.featureDependencies ?? []).length > 0) {
        throw new FastdbUsageError(
          `fastdb call-db call-db feature table "${table.name}" dependencies are only valid for object-graph call-db.`
        );
      }
      continue;
    }
    if (table.kind === 'array') {
      if (!table.item) {
        throw new FastdbUsageError(`fastdb call-db array table "${table.name}" is missing item metadata.`);
      }
      ensureColumnarScalarKind(
        table.item.kind,
        `fastdb call-db call-db array table "${table.name}" item`
      );
      continue;
    }
    if (table.kind === 'scalars') {
      const bytesFields = (table.fields ?? [])
        .filter((field) => field.kind === 'bytes')
        .map((field) => field.name);
      if (bytesFields.length > 1) {
        throw new FastdbUsageError(
          `fastdb call-db call-db scalar table "${table.name}" cannot represent multiple bytes fields ${JSON.stringify(bytesFields)}; fastdb columnar bytes use the feature raw payload.`
        );
      }
      for (const field of table.fields ?? []) {
        ensureColumnarScalarKind(
          field.kind,
          `fastdb call-db call-db scalar field "${field.name}"`
        );
      }
      continue;
    }
    throw new FastdbUsageError(`Unsupported fastdb call-db table kind "${table.kind}".`);
  }
}

function ensureColumnarFeatureRuntime(featureType: FeatureClass, context: string): void {
  if (VALIDATED_COLUMNAR_FEATURES.has(featureType)) {
    return;
  }
  const schema = getClassSchema(featureType);
  const bytesFields = schema.fieldList
    .filter((field) => field.entry.kind === 'bytes')
    .map((field) => field.name);
  if (bytesFields.length > 1) {
    throw new FastdbUsageError(
      `${context} cannot represent multiple bytes fields ${JSON.stringify(bytesFields)}; fastdb columnar bytes use the feature raw payload.`
    );
  }
  for (const field of schema.fieldList) {
    const kind = field.entry.kind;
    if (kind === 'list') {
      ensureSupportedColumnarListField(field, context);
      continue;
    }
    if (kind === 'ref') {
      throw new FastdbUsageError(
        `${context} cannot represent ref field "${field.name}"; use org.fastdb.object-graph for object references.`
      );
    }
  }
  VALIDATED_COLUMNAR_FEATURES.add(featureType);
}

function ensureSupportedColumnarListField(field: SchemaFieldDefinition, context: string): void {
  if (!isListField(field.entry)) {
    return;
  }
  const item = resolveListItem(field.entry);
  if (!isFieldType(item)) {
    throw new FastdbUsageError(
      `${context} cannot represent list[ref] field "${field.name}"; use org.fastdb.object-graph for object references.`
    );
  }
  if (!item.arrayCtor || !isSupportedNativeListKind(item.kind)) {
    throw new FastdbUsageError(
      `${context} cannot represent list field "${field.name}" with item kind "${item.kind}" in the TypeScript columnar runtime.`
    );
  }
}

function ensureColumnarScalarKind(kind: string, context: string): void {
  if (kind === 'list') {
    throw new FastdbUsageError(
      `${context} cannot represent list scalar metadata in the TypeScript FastDB runtime yet; array-table list item support is not implemented.`
    );
  }
  if (kind === 'ref') {
    throw new FastdbUsageError(`${context} cannot represent ref scalar metadata in columnar call-db.`);
  }
  if (!FIELD_TYPES[kind]) {
    throw new FastdbUsageError(`Unsupported fastdb call-db scalar kind "${kind}".`);
  }
}

function encodeCallTable(orm: ORM, table: FastdbCallDbTable, values: readonly unknown[]): void {
  if (table.kind === 'scalars') {
    const featureType = dynamicFeatureForTable(table);
    const rowValues: Record<string, unknown> = {};
    for (const field of table.fields ?? []) {
      rowValues[field.name] = valueAt(values, field.valuePosition, table.name);
    }
    orm.push(createDynamicFeature(featureType, rowValues), table.name);
    return;
  }
  if (table.kind === 'array') {
    const featureType = dynamicFeatureForTable(table);
    const items = asArray(valueAt(values, requiredPosition(table), table.name), table.name);
    if (items.length === 0) {
      orm.ensureTable(featureType, table.name);
      return;
    }
    for (const item of items) {
      orm.push(createDynamicFeature(featureType, { value: item }), table.name);
    }
    return;
  }
  if (table.kind === 'feature') {
    const featureType = requireFeature(table);
    const value = valueAt(values, requiredPosition(table), table.name);
    if (table.cardinality === 'many') {
      const items = asArray(value, table.name);
      if (items.length === 0) {
        orm.ensureTable(featureType, table.name);
        return;
      }
      for (const item of items) {
        if (!(item instanceof Feature)) {
          throw new FastdbUsageError(`fastdb call-db table "${table.name}" expects Feature rows.`);
        }
        orm.push(item, table.name);
      }
      return;
    }
    if (!(value instanceof Feature)) {
      throw new FastdbUsageError(`fastdb call-db table "${table.name}" expects a Feature value.`);
    }
    orm.push(value, table.name);
    return;
  }
  throw new FastdbUsageError(`Unsupported fastdb call-db table kind "${table.kind}".`);
}

function tryEncodeColumnarCallDbFastPath(
  binding: FastdbCallDbBinding,
  values: readonly unknown[]
): Uint8Array | null {
  if (binding.tables.length === 0) {
    return null;
  }
  for (const table of binding.tables) {
    if (!canUseColumnarFastPath(table)) {
      return null;
    }
  }
  const plans: ColumnarFastPathPlan[] = [];
  for (const table of binding.tables) {
    const plan = columnarFastPathPlan(table, values);
    if (plan === null) {
      return null;
    }
    plans.push(plan);
  }
  const orm = ORM.create();
  try {
    for (const plan of plans) {
      appendColumnarFastPathPlan(orm, plan);
    }
    orm.combine();
    return orm.toBuffer();
  } finally {
    orm.close();
  }
}

function canUseColumnarFastPath(table: FastdbCallDbTable): boolean {
  if (table.kind === 'scalars') {
    return (table.fields ?? []).every((field) => canBulkAppendKind(field.kind));
  }
  if (table.kind === 'array') {
    return Boolean(table.item && canBulkAppendKind(table.item.kind));
  }
  if (table.kind !== 'feature') {
    return false;
  }
  const schema = getClassSchema(requireFeature(table));
  return schema.fieldList.every((field) => canBulkAppendKind(field.entry.kind));
}

function columnarFastPathPlan(
  table: FastdbCallDbTable,
  values: readonly unknown[]
): ColumnarFastPathPlan | null {
  if (table.kind === 'scalars') {
    return scalarFastPathPlan(table, values);
  }
  if (table.kind === 'array') {
    return arrayFastPathPlan(table, values);
  }
  if (table.kind === 'feature') {
    return featureFastPathPlan(table, values);
  }
  return null;
}

function scalarFastPathPlan(
  table: FastdbCallDbTable,
  values: readonly unknown[]
): ColumnarFastPathPlan | null {
  for (const field of table.fields ?? []) {
    if (!canBulkAppendKind(field.kind)) {
      return null;
    }
  }
  const fields = (table.fields ?? []).map((field, index) => ({
    index,
    kind: field.kind,
    value: () => valueAt(values, field.valuePosition, table.name),
  }));
  return {
    fields,
    featureType: dynamicFeatureForTable(table),
    rows: [null],
    tableName: table.name,
  };
}

function arrayFastPathPlan(
  table: FastdbCallDbTable,
  values: readonly unknown[]
): ColumnarFastPathPlan | null {
  if (!table.item || !canBulkAppendKind(table.item.kind)) {
    return null;
  }
  const items = asArray(valueAt(values, requiredPosition(table), table.name), table.name);
  return {
    fields: [{
      index: 0,
      kind: table.item.kind,
      value: (row) => row,
    }],
    featureType: dynamicFeatureForTable(table),
    rows: items,
    tableName: table.name,
  };
}

function featureFastPathPlan(
  table: FastdbCallDbTable,
  values: readonly unknown[]
): ColumnarFastPathPlan | null {
  const featureType = requireFeature(table);
  const rows =
    table.cardinality === 'many'
      ? asArray(valueAt(values, requiredPosition(table), table.name), table.name)
      : [valueAt(values, requiredPosition(table), table.name)];
  if (rows.length === 0) {
    return {
      fields: [],
      featureType,
      rows,
      tableName: table.name,
    };
  }
  for (const row of rows) {
    if (!(row instanceof Feature)) {
      const expected = table.cardinality === 'many' ? 'Feature rows' : 'a Feature value';
      throw new FastdbUsageError(`fastdb call-db table "${table.name}" expects ${expected}.`);
    }
    if (!(row instanceof featureType)) {
      return null;
    }
  }
  const schema = getClassSchema(featureType);
  for (const field of schema.fieldList) {
    if (!canBulkAppendKind(field.entry.kind)) {
      return null;
    }
  }
  return {
    fields: schema.fieldList.map((field) => ({
      index: field.index,
      kind: field.entry.kind,
      value: (row) => (row as Record<string, unknown>)[field.name],
    })),
    featureType,
    rows,
    tableName: table.name,
  };
}

function canBulkAppendKind(kind: string): boolean {
  return BULK_APPEND_KINDS.has(kind);
}

function appendColumnarFastPathPlan(orm: ORM, plan: ColumnarFastPathPlan): void {
  const table = orm.ensureTable(plan.featureType, plan.tableName);
  const origin = table.origin as WxLayerTableBuildHandle;
  for (const row of plan.rows) {
    origin.addFeatureBegin();
    try {
      for (const field of plan.fields) {
        writeColumnarFastPathField(origin, field.index, field.kind, field.value(row));
      }
    } finally {
      origin.addFeatureEnd();
    }
  }
}

function writeColumnarFastPathField(
  origin: WxLayerTableBuildHandle,
  index: number,
  kind: string,
  value: unknown
): void {
  if (kind === 'bool') {
    origin.setFieldInt(index, coerceBoolScalar(value) ? 1 : 0);
    return;
  }
  if (kind === 'u8' || kind === 'u16' || kind === 'u32' || kind === 'i32') {
    origin.setFieldInt(index, Math.trunc(Number(value)));
    return;
  }
  origin.setFieldDouble(index, Number(value));
}

function decodeCallTable(orm: ORM, table: FastdbCallDbTable, values: unknown[]): void {
  if (table.kind === 'scalars') {
    const featureType = dynamicFeatureForTable(table);
    const rows = orm.table(featureType, table.name);
    if (rows.length < 1) {
      throw new FastdbUsageError(`fastdb call-db scalar table "${table.name}" is empty.`);
    }
    const row = rows.get(0) as unknown as Record<string, unknown>;
    for (const field of table.fields ?? []) {
      values[field.valuePosition] = materializeScalarValue(field.kind, row[field.name]);
    }
    return;
  }
  if (table.kind === 'array') {
    const featureType = dynamicFeatureForTable(table);
    const rows = orm.table(featureType, table.name);
    values[requiredPosition(table)] = Array.from(rows, (row) => (row as unknown as Record<string, unknown>).value);
    return;
  }
  if (table.kind === 'feature') {
    const featureType = requireFeature(table);
    const rows = orm.table(featureType, table.name);
    if (table.cardinality === 'many') {
      values[requiredPosition(table)] = Array.from(rows, (row) => copyFeature(featureType, row));
      return;
    }
    if (rows.length < 1) {
      throw new FastdbUsageError(`fastdb call-db feature table "${table.name}" is empty.`);
    }
    values[requiredPosition(table)] = copyFeature(featureType, rows.get(0));
    return;
  }
  throw new FastdbUsageError(`Unsupported fastdb call-db table kind "${table.kind}".`);
}

function normalizeCallValues(binding: FastdbCallDbBinding, value: unknown): readonly unknown[] {
  const expected = callValueCount(binding);
  if (binding.direction === 'input') {
    if (!Array.isArray(value)) {
      throw new FastdbUsageError('fastdb call-db input call-db encode expects an array of method arguments.');
    }
    if (value.length !== expected) {
      throw new FastdbUsageError(`fastdb call-db input call-db expected ${expected} values, got ${value.length}.`);
    }
    return value;
  }
  if (expected <= 1) {
    return [value];
  }
  if (!Array.isArray(value)) {
    throw new FastdbUsageError(`fastdb call-db output call-db expected ${expected} tuple values.`);
  }
  if (value.length !== expected) {
    throw new FastdbUsageError(`fastdb call-db output call-db expected ${expected} values, got ${value.length}.`);
  }
  return value;
}

function callValueCount(binding: FastdbCallDbBinding): number {
  ensureCallDbTableShape(binding);
  let count = 0;
  for (const table of binding.tables) {
    if (table.kind === 'scalars') {
      count += table.fields?.length ?? 0;
      continue;
    }
    count += 1;
  }
  return count;
}

function columnFieldForTable(
  table: FastdbCallDbTable,
  fieldName: string
): { readonly name: string; readonly kind: string } | undefined {
  if (table.kind === 'scalars') {
    const field = (table.fields ?? []).find((item) => item.name === fieldName);
    return field ? { name: field.name, kind: field.kind } : undefined;
  }
  if (table.kind === 'array') {
    if (fieldName !== CALL_DB_ARRAY_VALUE_FIELD || !table.item) {
      return undefined;
    }
    return { name: CALL_DB_ARRAY_VALUE_FIELD, kind: table.item.kind };
  }
  if (table.kind === 'feature') {
    const schema = getClassSchema(requireFeature(table));
    const field = schema.fieldList.find((item) => item.name === fieldName);
    return field ? { name: field.name, kind: field.entry.kind } : undefined;
  }
  return undefined;
}

function dynamicFeatureForTable(table: FastdbCallDbTable): FeatureClass {
  const cached = DYNAMIC_FEATURE_CACHE.get(table);
  if (cached) {
    return cached;
  }
  const schema: Record<string, FieldTypeDef> = {};
  if (table.kind === 'scalars') {
    for (const field of table.fields ?? []) {
      schema[field.name] = fieldTypeForKind(field.kind);
    }
  } else if (table.kind === 'array') {
    if (!table.item) {
      throw new FastdbUsageError(`fastdb call-db array table "${table.name}" is missing item metadata.`);
    }
    schema.value = fieldTypeForKind(table.item.kind);
  } else {
    throw new FastdbUsageError(`fastdb call-db table "${table.name}" does not use a dynamic feature.`);
  }
  class FastdbDynamicCallDbFeature extends Feature {
    static schema = defineSchema(schema);
  }
  DYNAMIC_FEATURE_CACHE.set(table, FastdbDynamicCallDbFeature);
  return FastdbDynamicCallDbFeature;
}

function createDynamicFeature(featureType: FeatureClass, values: Record<string, unknown>): Feature {
  return createFeature(featureType as FeatureClass<any>, values) as Feature;
}

function copyFeature<T extends Feature>(featureType: FeatureClass<T>, source: T): T {
  const values: Record<string, unknown> = {};
  for (const field of getClassSchema(featureType).fieldList) {
    values[field.name] = (source as unknown as Record<string, unknown>)[field.name];
  }
  return createFeature(featureType as FeatureClass<any>, values) as T;
}

function materializeScalarValue(kind: string, value: unknown): unknown {
  if (kind === 'bool') {
    return coerceBoolScalar(value);
  }
  return value;
}

function fieldTypeForKind(kind: string): FieldTypeDef {
  const fieldType = FIELD_TYPES[kind];
  if (!fieldType) {
    throw new FastdbUsageError(`Unsupported fastdb call-db scalar kind "${kind}".`);
  }
  return fieldType;
}

function requireFeature(table: FastdbCallDbTable): FeatureClass {
  if (!table.feature) {
    throw new FastdbUsageError(`fastdb call-db feature table "${table.name}" is missing a feature class.`);
  }
  return table.feature;
}

function requiredPosition(table: FastdbCallDbTable): number {
  if (table.valuePosition === undefined) {
    throw new FastdbUsageError(`fastdb call-db table "${table.name}" is missing valuePosition metadata.`);
  }
  return table.valuePosition;
}

function valueAt(values: readonly unknown[], position: number, tableName: string): unknown {
  if (position < 0 || position >= values.length) {
    throw new FastdbUsageError(`fastdb call-db table "${tableName}" references value position ${position}, but only ${values.length} values were provided.`);
  }
  return values[position];
}

function asArray(value: unknown, tableName: string): readonly unknown[] {
  if (Array.isArray(value)) {
    return value;
  }
  if (typeof value === 'string' || value instanceof String) {
    throw new FastdbUsageError(`fastdb call-db table "${tableName}" expects an array or sequence value, not a string.`);
  }
  if (typeof value === 'function') {
    throw new FastdbUsageError(`fastdb call-db table "${tableName}" expects an array or sequence value.`);
  }
  if (value instanceof ArrayBuffer || value instanceof DataView || value instanceof Map) {
    throw new FastdbUsageError(`fastdb call-db table "${tableName}" expects an array or sequence value.`);
  }
  if (ArrayBuffer.isView(value)) {
    return Array.from(value as unknown as ArrayLike<unknown>);
  }
  if (hasArrayLikeLength(value)) {
    if (!hasIndexedArrayLikeEntries(value)) {
      throw new FastdbUsageError(`fastdb call-db table "${tableName}" expects an indexed array-like sequence value.`);
    }
    return Array.from(value);
  }
  if (isIterableObject(value)) {
    return Array.from(value);
  }
  throw new FastdbUsageError(`fastdb call-db table "${tableName}" expects an array or sequence value.`);
}

function hasArrayLikeLength(value: unknown): value is ArrayLike<unknown> {
  if (value === null || typeof value !== 'object') {
    return false;
  }
  const length = (value as { readonly length?: unknown }).length;
  return typeof length === 'number' && Number.isSafeInteger(length) && length >= 0;
}

function hasIndexedArrayLikeEntries(value: ArrayLike<unknown>): boolean {
  for (let index = 0; index < value.length; index += 1) {
    if (!(index in Object(value))) {
      return false;
    }
  }
  return true;
}

function isIterableObject(value: unknown): value is Iterable<unknown> {
  if (value === null || (typeof value !== 'object' && typeof value !== 'function')) {
    return false;
  }
  return typeof (value as { readonly [Symbol.iterator]?: unknown })[Symbol.iterator] === 'function';
}
