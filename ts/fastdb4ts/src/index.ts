export { getFastdbModule, initFastdb } from './wasm-loader.js';
export {
  Feature,
  createFeature,
  mapFeatureFrom,
  wrapFeature,
} from './feature.js';
export {
  ORM,
  TableDefn,
} from './orm.js';
export { Table } from './table.js';
export { StridedColumn } from './column.js';
export {
  defineSchema,
  getClassSchema,
} from './schema.js';
export {
  BOOL,
  BYTES,
  F32,
  F64,
  I32,
  REF,
  STR,
  U8,
  U8N,
  U16,
  U16N,
  U32,
  WSTR,
  ref,
} from './types.js';
export {
  FastdbError,
  FastdbRuntimeError,
  FastdbSchemaError,
  FastdbUsageError,
} from './errors.js';

export type {
  ChunkView,
  FieldDefView,
  FastdbModule,
  FastdbModuleFactory,
  WxDatabaseBuildHandle,
  WxDatabaseHandle,
  WxFeatureHandle,
  WxLayerTableBuildHandle,
  WxLayerTableHandle,
  WxMemoryStreamHandle,
} from './wasm-loader.js';
export type {
  ClassSchema,
  SchemaDefinition,
  SchemaFieldDefinition,
} from './schema.js';
export type {
  FeatureClass,
  FeatureDatabaseHandle,
} from './feature.js';
export type {
  FieldKind,
  FieldTypeDef,
  RefFieldDef,
  SchemaEntry,
  TypedArrayConstructor,
  TypedArrayInstance,
} from './types.js';
