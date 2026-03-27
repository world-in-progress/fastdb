import { createFeature, Feature, type FeatureClass } from './feature.js';
import {
  getClassSchema,
  resolveListItem,
  type SchemaFieldDefinition,
} from './schema.js';
import { FastdbRuntimeError, FastdbUsageError } from './errors.js';
import { getInitializedFastdbModule, type FastdbModule, type WxLayerTableBuildHandle } from './wasm-loader.js';
import { isListField, isRefField, type FeatureClassLike, type FieldTypeDef } from './types.js';

const NUMERIC_LIST_LAYER_PREFIX = '__fastser_list__|';
const TEXT_ENCODER = new TextEncoder();
const TEXT_DECODER = new TextDecoder();

type NumericListKind = 'u32' | 'f64' | 'i32';

interface ObjectWrapper {
  obj: Feature;
  layerIdx: number;
  featureIdx: number;
}

interface SerializerSchema {
  readonly fieldList: readonly SchemaFieldDefinition[];
  readonly numericFieldKinds: ReadonlyMap<string, NumericListKind>;
  readonly dbFieldIndexBySchema: ReadonlyMap<number, number>;
  readonly refTraversalFields: readonly SchemaFieldDefinition[];
}

const SERIALIZER_SCHEMA_CACHE = new WeakMap<FeatureClassLike, SerializerSchema>();

export class FastSerializer {
  static dumps(obj: Feature): Uint8Array {
    if (!(obj instanceof Feature)) {
      throw new FastdbUsageError('Only fastdb4ts.Feature objects can be serialized.');
    }

    const module = getInitializedFastdbModule();
    const ctx = new DumpContext();
    ctx.register(obj);

    const db = new module.WxDatabaseBuild();
    db.begin('');

    const layerBuilders = new Map<number, WxLayerTableBuildHandle>();
    for (const [ctor, layerIdx] of ctx.typeToLayer.entries()) {
      const lb = db.createLayerBegin(ctor.name);
      lb.setGeometryType(module.gtAny, module.cfDefault, false);

      const schema = getSerializerSchema(ctor);
      for (const field of schema.fieldList) {
        const dbFieldIndex = schema.dbFieldIndexBySchema.get(field.index);
        if (dbFieldIndex !== undefined && dbFieldIndex !== -1) {
          lb.addField(field.name, field.entry.originType, 0, 1);
        }
      }
      layerBuilders.set(layerIdx, lb);
    }

    const numericListLayers = new Map<string, { kind: NumericListKind; layer: WxLayerTableBuildHandle }>();
    for (const ctor of ctx.typeToLayer.keys()) {
      const schema = getSerializerSchema(ctor);
      for (const field of schema.fieldList) {
        const kind = schema.numericFieldKinds.get(field.name);
        if (!kind) {
          continue;
        }
        const layerName = makeNumericListLayerName(ctor.name, field.name, kind);
        const aux = db.createLayerBegin(layerName);
        aux.setGeometryType(module.gtAny, module.cfDefault, false);
        aux.addField('owner_fid', module.ftU32, 0, 1);
        numericListLayers.set(`${ctor.name}:${field.name}`, { kind, layer: aux });
      }
    }

    for (const wrapper of ctx.objects) {
      const ctor = wrapper.obj.constructor as FeatureClass;
      const schema = getSerializerSchema(ctor);
      const layer = layerBuilders.get(wrapper.layerIdx);
      if (!layer) {
        throw new FastdbRuntimeError(`Missing layer builder for "${ctor.name}".`);
      }

      layer.addFeatureBegin();
      const blob = new ByteWriter();

      for (const field of schema.fieldList) {
        const value = getFeatureFieldValue(wrapper.obj, field.name);
        const numericKind = schema.numericFieldKinds.get(field.name);

        if (numericKind) {
          const aux = numericListLayers.get(`${ctor.name}:${field.name}`);
          if (!aux) {
            throw new FastdbRuntimeError(`Missing numeric-list layer for "${ctor.name}.${field.name}".`);
          }
          writeNumericListChunk(module, aux.layer, wrapper.featureIdx, value, numericKind);
          continue;
        }

        if (isListField(field.entry)) {
          packList(blob, field, value, ctx);
          continue;
        }

        if (field.entry.kind === 'bytes') {
          const bytes = normalizeBytes(value);
          blob.writeU32(bytes.length);
          blob.writeBytes(bytes);
          continue;
        }

        if (isRefField(field.entry)) {
          packFeatureRef(blob, value instanceof Feature ? value : null, ctx);
          continue;
        }

        const dbFieldIndex = schema.dbFieldIndexBySchema.get(field.index);
        if (dbFieldIndex === undefined || dbFieldIndex === -1 || value === null || value === undefined) {
          continue;
        }

        switch (field.entry.kind) {
          case 'bool':
            layer.setFieldInt(dbFieldIndex, value ? 1 : 0);
            break;
          case 'u8':
          case 'u16':
          case 'u32':
          case 'i32':
          case 'u8n':
          case 'u16n':
            layer.setFieldInt(dbFieldIndex, Math.trunc(Number(value)));
            break;
          case 'f32':
          case 'f64':
            layer.setFieldDouble(dbFieldIndex, Number(value));
            break;
          case 'str':
          case 'wstr':
            layer.setFieldString(dbFieldIndex, String(value));
            break;
          default:
            throw new FastdbUsageError(
              `Unsupported serializer scalar kind "${field.entry.kind}" on field "${field.name}".`
            );
        }
      }

      const blobBytes = blob.finish();
      if (blobBytes.length > 0) {
        withHeapBytes(module, blobBytes, (ptr, size) => {
          layer.setGeometryRaw(ptr, size);
        });
      }
      layer.addFeatureEnd();
    }

    const stream = new module.WxMemoryStream();
    db.post(stream);
    const data = copyChunkToBytes(module, stream.dataView());
    stream.delete();
    db.delete();
    return data;
  }

  static loads<T extends Feature>(
    data: Uint8Array | ArrayBuffer,
    rootType: FeatureClass<T>
  ): T | null {
    const module = getInitializedFastdbModule();
    const bytes = data instanceof Uint8Array ? data : new Uint8Array(data);
    const db = withHeapBytes(module, bytes, (ptr, size) => module.WxDatabase.loadFromHeap(ptr, size));

    if (db.getLayerCount() === 0) {
      return null;
    }

    const rootLayer = db.getLayer(0);
    if (rootLayer.getFeatureCount() === 0) {
      return null;
    }

    const ctx = new LoadContext(db, module);
    discoverTypes(rootType, ctx.typeMap);
    return ctx.getObject(0, 0, rootType);
  }
}

class DumpContext {
  readonly objects: ObjectWrapper[] = [];
  readonly typeToLayer = new Map<FeatureClassLike, number>();
  readonly objToRef = new WeakMap<Feature, { layerIdx: number; featureIdx: number }>();
  private readonly layerCounters = new Map<number, number>();

  register(obj: Feature): void {
    if (this.objToRef.has(obj)) {
      return;
    }

    const ctor = obj.constructor as FeatureClass;
    let layerIdx = this.typeToLayer.get(ctor);
    if (layerIdx === undefined) {
      layerIdx = this.typeToLayer.size;
      this.typeToLayer.set(ctor, layerIdx);
      this.layerCounters.set(layerIdx, 0);
    }

    const featureIdx = this.layerCounters.get(layerIdx) ?? 0;
    this.layerCounters.set(layerIdx, featureIdx + 1);
    this.objToRef.set(obj, { layerIdx, featureIdx });
    this.objects.push({ obj, layerIdx, featureIdx });

    const schema = getSerializerSchema(ctor);
    for (const field of schema.refTraversalFields) {
      const value = getFeatureFieldValue(obj, field.name);
      if (value === null || value === undefined) {
        continue;
      }

      if (isRefField(field.entry)) {
        if (value instanceof Feature) {
          this.register(value);
        }
        continue;
      }

      // Must be list-of-Feature
      if (Array.isArray(value)) {
        for (const nested of value) {
          if (nested instanceof Feature) {
            this.register(nested);
          }
        }
      }
    }
  }

  getRef(obj: Feature | null | undefined): { layerIdx: number; featureIdx: number } | undefined {
    if (!obj) {
      return undefined;
    }
    return this.objToRef.get(obj);
  }
}

class LoadContext {
  readonly objectCache = new Map<string, Feature>();
  readonly typeMap = new Map<string, FeatureClassLike>();
  readonly numericListValues: Map<string, unknown[]>;

  constructor(
    private readonly db: ReturnType<typeof getInitializedFastdbModule>['WxDatabase'] extends never
      ? never
      : import('./wasm-loader.js').WxDatabaseHandle,
    private readonly module: FastdbModule
  ) {
    this.numericListValues = loadNumericListValues(db, module);
  }

  getObject<T extends Feature>(layerIdx: number, featureIdx: number, expectedType: FeatureClass<T>): T {
    const key = `${layerIdx}:${featureIdx}`;
    const cached = this.objectCache.get(key);
    if (cached) {
      return cached as T;
    }

    const layer = this.db.getLayer(layerIdx);
    const ctor = (this.typeMap.get(layer.name()) as FeatureClass<T> | undefined) ?? expectedType;
    const row = layer.tryGetFeatureAt(featureIdx);
    const obj = createFeature(ctor);
    this.objectCache.set(key, obj);

    const cache = obj._getCache();
    const schema = getSerializerSchema(ctor);
    const blob = copyChunkToBytes(this.module, row.geometryView());
    const reader = new ByteReader(blob);

    for (const field of schema.fieldList) {
      const numericKind = schema.numericFieldKinds.get(field.name);
      if (numericKind) {
        cache[field.name] = this.numericListValues.get(`${ctor.name}:${field.name}:${featureIdx}`) ?? [];
        continue;
      }

      if (isListField(field.entry)) {
        cache[field.name] = unpackList(reader, field, this);
        continue;
      }

      if (field.entry.kind === 'bytes') {
        const size = reader.readU32();
        cache[field.name] = reader.readBytes(size);
        continue;
      }

      if (isRefField(field.entry)) {
        const refLayer = reader.readU16();
        const refFeature = reader.readU32();
        if (refLayer === 0xffff) {
          cache[field.name] = null;
          continue;
        }
        if (!field.target) {
          throw new FastdbRuntimeError(`Reference field "${field.name}" is missing a target type.`);
        }
        cache[field.name] = this.getObject(refLayer, refFeature, field.target as FeatureClass);
        continue;
      }

      const dbFieldIndex = schema.dbFieldIndexBySchema.get(field.index);
      if (dbFieldIndex === undefined || dbFieldIndex === -1) {
        continue;
      }

      switch (field.entry.kind) {
        case 'bool':
          cache[field.name] = row.getFieldAsInt(dbFieldIndex) !== 0;
          break;
        case 'u8':
        case 'u16':
        case 'u32':
        case 'i32':
        case 'u8n':
        case 'u16n':
          cache[field.name] = row.getFieldAsInt(dbFieldIndex);
          break;
        case 'f32':
        case 'f64':
          cache[field.name] = row.getFieldAsFloat(dbFieldIndex);
          break;
        case 'str':
        case 'wstr':
          cache[field.name] = row.getFieldAsString(dbFieldIndex);
          break;
        default:
          throw new FastdbRuntimeError(`Unsupported load field kind "${field.entry.kind}".`);
      }
    }

    return obj as T;
  }
}

function getSerializerSchema(ctor: FeatureClassLike): SerializerSchema {
  const cached = SERIALIZER_SCHEMA_CACHE.get(ctor);
  if (cached) {
    return cached;
  }

  const classSchema = getClassSchema(ctor);
  const numericFieldKinds = new Map<string, NumericListKind>();
  const dbFieldIndexBySchema = new Map<number, number>();

  let dbFieldIndex = 0;
  for (const field of classSchema.fieldList) {
    if (isListField(field.entry)) {
      const item = resolveListItem(field.entry);
      if (isFieldType(item) && item.kind === 'u32') {
        numericFieldKinds.set(field.name, 'u32');
      } else if (isFieldType(item) && item.kind === 'f64') {
        numericFieldKinds.set(field.name, 'f64');
      } else if (isFieldType(item) && item.kind === 'i32') {
        numericFieldKinds.set(field.name, 'i32');
      }
      dbFieldIndexBySchema.set(field.index, -1);
      continue;
    }

    if (field.entry.kind === 'ref' || field.entry.kind === 'bytes') {
      dbFieldIndexBySchema.set(field.index, -1);
      continue;
    }

    dbFieldIndexBySchema.set(field.index, dbFieldIndex);
    dbFieldIndex += 1;
  }

  // Pre-compute fields that need traversal during register() — only ref fields and list-of-Feature fields
  const refTraversalFields: SchemaFieldDefinition[] = [];
  for (const field of classSchema.fieldList) {
    if (isRefField(field.entry)) {
      refTraversalFields.push(field);
    } else if (isListField(field.entry)) {
      const item = resolveListItem(field.entry);
      if (!isFieldType(item)) {
        refTraversalFields.push(field);
      }
    }
  }

  const schema: SerializerSchema = {
    fieldList: classSchema.fieldList,
    numericFieldKinds,
    dbFieldIndexBySchema,
    refTraversalFields,
  };
  SERIALIZER_SCHEMA_CACHE.set(ctor, schema);
  return schema;
}

function discoverTypes(ctor: FeatureClassLike, typeMap: Map<string, FeatureClassLike>): void {
  if (typeMap.has(ctor.name)) {
    return;
  }
  typeMap.set(ctor.name, ctor);

  const schema = getSerializerSchema(ctor);
  for (const field of schema.fieldList) {
    if (isRefField(field.entry) && field.target) {
      discoverTypes(field.target, typeMap);
      continue;
    }

    if (!isListField(field.entry)) {
      continue;
    }

    const item = resolveListItem(field.entry);
    if (!isFieldType(item)) {
      discoverTypes(item, typeMap);
    }
  }
}

function packFeatureRef(writer: ByteWriter, value: Feature | null, ctx: DumpContext): void {
  const ref = value ? ctx.getRef(value) : undefined;
  if (!ref) {
    writer.writeU16(0xffff);
    writer.writeU32(0xffffffff);
    return;
  }
  writer.writeU16(ref.layerIdx);
  writer.writeU32(ref.featureIdx);
}

function packList(writer: ByteWriter, field: SchemaFieldDefinition, value: unknown, ctx: DumpContext): void {
  const list = Array.isArray(value) ? value : [];
  writer.writeU32(list.length);
  if (list.length === 0) {
    return;
  }

  if (!isListField(field.entry)) {
    throw new FastdbUsageError(`Field "${field.name}" is not declared as a list.`);
  }
  const item = resolveListItem(field.entry);
  if (isFieldType(item)) {
    switch (item.kind) {
      case 'i32':
      case 'u8':
      case 'u16':
      case 'bool':
        for (const entry of list) {
          writer.writeI32(Math.trunc(Number(entry)));
        }
        return;
      case 'f32':
      case 'f64':
        for (const entry of list) {
          writer.writeF64(Number(entry));
        }
        return;
      case 'str':
      case 'wstr':
        for (const entry of list) {
          const bytes = TEXT_ENCODER.encode(String(entry));
          writer.writeU32(bytes.length);
          writer.writeBytes(bytes);
        }
        return;
      default:
        throw new FastdbUsageError(
          `Unsupported list item kind "${item.kind}" on field "${field.name}".`
        );
    }
  }

  for (const entry of list) {
    packFeatureRef(writer, entry instanceof Feature ? entry : null, ctx);
  }
}

function unpackList(reader: ByteReader, field: SchemaFieldDefinition, ctx: LoadContext): unknown[] {
  const count = reader.readU32();
  if (count === 0) {
    return [];
  }

  if (!isListField(field.entry)) {
    throw new FastdbRuntimeError(`Field "${field.name}" is not declared as a list.`);
  }
  const item = resolveListItem(field.entry);
  const out: unknown[] = [];

  if (isFieldType(item)) {
    switch (item.kind) {
      case 'i32':
      case 'u8':
      case 'u16':
      case 'bool':
        for (let i = 0; i < count; i += 1) {
          out.push(reader.readI32());
        }
        return out;
      case 'f32':
      case 'f64':
        for (let i = 0; i < count; i += 1) {
          out.push(reader.readF64());
        }
        return out;
      case 'str':
      case 'wstr':
        for (let i = 0; i < count; i += 1) {
          out.push(TEXT_DECODER.decode(reader.readBytes(reader.readU32())));
        }
        return out;
      default:
        throw new FastdbRuntimeError(
          `Unsupported list item kind "${item.kind}" on field "${field.name}".`
        );
    }
  }

  for (let i = 0; i < count; i += 1) {
    const refLayer = reader.readU16();
    const refFeature = reader.readU32();
    out.push(refLayer === 0xffff ? null : ctx.getObject(refLayer, refFeature, item as FeatureClass));
  }
  return out;
}

function writeNumericListChunk(
  module: FastdbModule,
  layer: WxLayerTableBuildHandle,
  ownerFeatureId: number,
  value: unknown,
  kind: NumericListKind
): void {
  const list = Array.isArray(value) ? value : [];
  layer.addFeatureBegin();
  layer.setFieldInt(0, ownerFeatureId);

  let payload: Uint8Array;
  if (list.length === 0) {
    payload = new Uint8Array(0);
  } else if (kind === 'u32') {
    const typed = new Uint32Array(list.length);
    for (let i = 0; i < list.length; i++) {
      const iv = Number(list[i]);
      if (!Number.isInteger(iv) || iv < 0 || iv > 0xffffffff) {
        throw new FastdbUsageError(`List[U32] item out of range: ${list[i]}`);
      }
      typed[i] = iv;
    }
    payload = new Uint8Array(typed.buffer);
  } else if (kind === 'i32') {
    const typed = new Int32Array(list.length);
    for (let i = 0; i < list.length; i++) {
      const iv = Math.trunc(Number(list[i]));
      if (iv < -0x80000000 || iv > 0x7fffffff) {
        throw new FastdbUsageError(
          `list[int] item ${list[i]} out of i32 range [-2147483648, 2147483647].`
        );
      }
      typed[i] = iv;
    }
    payload = new Uint8Array(typed.buffer);
  } else {
    const typed = new Float64Array(list.length);
    for (let i = 0; i < list.length; i++) {
      typed[i] = Number(list[i]);
    }
    payload = new Uint8Array(typed.buffer);
  }

  if (payload.length > 0) {
    withHeapBytes(module, payload, (ptr, size) => {
      layer.setGeometryRaw(ptr, size);
    });
  }
  layer.addFeatureEnd();
}

function loadNumericListValues(
  db: import('./wasm-loader.js').WxDatabaseHandle,
  module: FastdbModule
): Map<string, unknown[]> {
  const out = new Map<string, unknown[]>();

  for (let layerIdx = 0; layerIdx < db.getLayerCount(); layerIdx += 1) {
    const layer = db.getLayer(layerIdx);
    const parsed = parseNumericListLayerName(layer.name());
    if (!parsed) {
      continue;
    }

    for (let rowIdx = 0; rowIdx < layer.getFeatureCount(); rowIdx += 1) {
      const row = layer.tryGetFeatureAt(rowIdx);
      const ownerFeatureId = row.getFieldAsInt(0);
      const chunk = copyChunkToBytes(module, row.geometryView());
      out.set(
        `${parsed.className}:${parsed.fieldName}:${ownerFeatureId}`,
        decodeNumericListChunk(chunk, parsed.kind)
      );
    }
  }

  return out;
}

function decodeNumericListChunk(chunk: Uint8Array, kind: NumericListKind): unknown[] {
  if (chunk.length === 0) {
    return [];
  }

  const view = new DataView(chunk.buffer, chunk.byteOffset, chunk.byteLength);
  const out: number[] = [];
  if (kind === 'u32') {
    for (let offset = 0; offset < chunk.length; offset += 4) {
      out.push(view.getUint32(offset, true));
    }
  } else if (kind === 'i32') {
    for (let offset = 0; offset < chunk.length; offset += 4) {
      out.push(view.getInt32(offset, true));
    }
  } else {
    for (let offset = 0; offset < chunk.length; offset += 8) {
      out.push(view.getFloat64(offset, true));
    }
  }
  return out;
}

function makeNumericListLayerName(className: string, fieldName: string, kind: NumericListKind): string {
  return `${NUMERIC_LIST_LAYER_PREFIX}${className}|${fieldName}|${kind}`;
}

function parseNumericListLayerName(
  layerName: string
): { className: string; fieldName: string; kind: NumericListKind } | null {
  if (!layerName.startsWith(NUMERIC_LIST_LAYER_PREFIX)) {
    return null;
  }
  const parts = layerName.slice(NUMERIC_LIST_LAYER_PREFIX.length).split('|');
  if (parts.length !== 3) {
    return null;
  }
  const [className, fieldName, kind] = parts;
  if (kind !== 'u32' && kind !== 'f64' && kind !== 'i32') {
    return null;
  }
  return { className, fieldName, kind };
}

function normalizeBytes(value: unknown): Uint8Array {
  if (value instanceof Uint8Array) {
    return value;
  }
  if (value instanceof ArrayBuffer) {
    return new Uint8Array(value);
  }
  return new Uint8Array(0);
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

function copyChunkToBytes(
  module: FastdbModule,
  chunk: { data: number; size: number }
): Uint8Array {
  return module.HEAPU8.slice(chunk.data, chunk.data + chunk.size);
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

class ByteWriter {
  private buf: ArrayBuffer;
  private view: DataView;
  private offset = 0;

  constructor(initialCapacity = 256) {
    this.buf = new ArrayBuffer(initialCapacity);
    this.view = new DataView(this.buf);
  }

  writeU16(value: number): void {
    this.ensureCapacity(2);
    this.view.setUint16(this.offset, value, true);
    this.offset += 2;
  }

  writeU32(value: number): void {
    this.ensureCapacity(4);
    this.view.setUint32(this.offset, value, true);
    this.offset += 4;
  }

  writeI32(value: number): void {
    this.ensureCapacity(4);
    this.view.setInt32(this.offset, value, true);
    this.offset += 4;
  }

  writeF64(value: number): void {
    this.ensureCapacity(8);
    this.view.setFloat64(this.offset, value, true);
    this.offset += 8;
  }

  writeBytes(bytes: Uint8Array): void {
    if (bytes.length === 0) {
      return;
    }
    this.ensureCapacity(bytes.length);
    new Uint8Array(this.buf).set(bytes, this.offset);
    this.offset += bytes.length;
  }

  finish(): Uint8Array {
    return new Uint8Array(this.buf, 0, this.offset);
  }

  private ensureCapacity(needed: number): void {
    const required = this.offset + needed;
    if (required <= this.buf.byteLength) {
      return;
    }
    let newSize = this.buf.byteLength * 2;
    while (newSize < required) {
      newSize *= 2;
    }
    const newBuf = new ArrayBuffer(newSize);
    new Uint8Array(newBuf).set(new Uint8Array(this.buf, 0, this.offset));
    this.buf = newBuf;
    this.view = new DataView(this.buf);
  }
}

class ByteReader {
  private offset = 0;
  private readonly view: DataView;

  constructor(private readonly bytes: Uint8Array) {
    this.view = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
  }

  private checkRead(size: number): void {
    if (this.offset + size > this.bytes.byteLength) {
      throw new FastdbRuntimeError(
        `ByteReader: attempted to read ${size} byte(s) at offset ${this.offset}, ` +
          `but buffer length is ${this.bytes.byteLength}. The buffer may be truncated or corrupted.`
      );
    }
  }

  readU16(): number {
    this.checkRead(2);
    const value = this.view.getUint16(this.offset, true);
    this.offset += 2;
    return value;
  }

  readU32(): number {
    this.checkRead(4);
    const value = this.view.getUint32(this.offset, true);
    this.offset += 4;
    return value;
  }

  readI32(): number {
    this.checkRead(4);
    const value = this.view.getInt32(this.offset, true);
    this.offset += 4;
    return value;
  }

  readF64(): number {
    this.checkRead(8);
    const value = this.view.getFloat64(this.offset, true);
    this.offset += 8;
    return value;
  }

  readBytes(size: number): Uint8Array {
    this.checkRead(size);
    const out = this.bytes.slice(this.offset, this.offset + size);
    this.offset += size;
    return out;
  }
}

// Exported for unit testing only — not part of the public API.
export { ByteReader, ByteWriter };
