import { FastdbSchemaError } from './errors.js';
import {
  type FeatureClassLike,
  type ListFieldDef,
  type ListItemDef,
  type RefFieldDef,
  type SchemaEntry,
  isListField,
  isNumericField,
  isRefField,
  isScalarField,
} from './types.js';

export interface SchemaDefinition {
  readonly fields: Record<string, SchemaEntry>;
}

export interface SchemaFieldDefinition {
  readonly name: string;
  readonly index: number;
  readonly entry: SchemaEntry;
  readonly target?: FeatureClassLike;
}

export interface ClassSchema {
  readonly fieldList: readonly SchemaFieldDefinition[];
  readonly fieldMap: ReadonlyMap<string, SchemaFieldDefinition>;
  readonly scalarFieldIds: readonly number[];
  readonly numericFieldIds: readonly number[];
  readonly refFieldIds: readonly number[];
  readonly listFieldIds: readonly number[];
}

const SCHEMA_CACHE = new WeakMap<FeatureClassLike, ClassSchema>();

export function defineSchema(fields: Record<string, SchemaEntry>): SchemaDefinition {
  return Object.freeze({ fields: { ...fields } });
}

export function getClassSchema(ctor: FeatureClassLike): ClassSchema {
  const cached = SCHEMA_CACHE.get(ctor);
  if (cached) {
    return cached;
  }

  const rawSchema = ctor.schema as SchemaDefinition | undefined;
  if (!rawSchema || typeof rawSchema !== 'object' || rawSchema.fields === undefined) {
    throw new FastdbSchemaError(
      `Feature class "${ctor.name}" is missing a static schema. Use defineSchema({...}).`
    );
  }

  const fieldList: SchemaFieldDefinition[] = [];
  const fieldMap = new Map<string, SchemaFieldDefinition>();
  const scalarFieldIds: number[] = [];
  const numericFieldIds: number[] = [];
  const refFieldIds: number[] = [];
  const listFieldIds: number[] = [];

  let index = 0;
  for (const [name, entry] of Object.entries(rawSchema.fields)) {
    if (!name || name.startsWith('_')) {
      throw new FastdbSchemaError(
        `Feature class "${ctor.name}" contains an invalid field name "${name}".`
      );
    }

    const target = resolveRefTarget(entry);
    const def: SchemaFieldDefinition = Object.freeze({
      name,
      index,
      entry,
      target,
    });

    fieldList.push(def);
    fieldMap.set(name, def);

    if (isScalarField(entry)) {
      scalarFieldIds.push(index);
    }
    if (isNumericField(entry)) {
      numericFieldIds.push(index);
    }
    if (isRefField(entry)) {
      refFieldIds.push(index);
    }
    if (isListField(entry)) {
      listFieldIds.push(index);
    }

    index += 1;
  }

  const schema: ClassSchema = Object.freeze({
    fieldList: Object.freeze(fieldList),
    fieldMap,
    scalarFieldIds: Object.freeze(scalarFieldIds),
    numericFieldIds: Object.freeze(numericFieldIds),
    refFieldIds: Object.freeze(refFieldIds),
    listFieldIds: Object.freeze(listFieldIds),
  });

  SCHEMA_CACHE.set(ctor, schema);
  return schema;
}

function resolveRefTarget(entry: SchemaEntry): FeatureClassLike | undefined {
  if (!isRefField(entry) || entry.target === undefined) {
    return undefined;
  }

  const target = entry.target;
  if (typeof target !== 'function') {
    return target;
  }

  if (target.prototype && target.prototype.constructor === target) {
    return target as FeatureClassLike;
  }

  const resolved = (target as () => FeatureClassLike)();
  return resolved;
}

export function getFieldDefinition(
  schema: ClassSchema,
  fieldName: string
): SchemaFieldDefinition | undefined {
  return schema.fieldMap.get(fieldName);
}

export function resolveFeatureClassLike(
  target: FeatureClassLike | (() => FeatureClassLike)
): FeatureClassLike {
  if (typeof target !== 'function') {
    return target;
  }

  if (target.prototype && target.prototype.constructor === target) {
    return target as FeatureClassLike;
  }

  return (target as () => FeatureClassLike)();
}

export function resolveListItem(entry: ListFieldDef): Exclude<ListItemDef, (() => FeatureClassLike)> {
  const item = entry.item;
  if (typeof item !== 'function') {
    return item;
  }
  return resolveFeatureClassLike(item);
}
