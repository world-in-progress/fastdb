import FastdbWasm from './wasm/fastdb4ts.js';

export interface WxMemoryStreamHandle {
  dataView(): ChunkView;
  reset(): void;
  delete(): void;
}

export interface ChunkView {
  data: number;
  size: number;
}

export interface FieldDefView {
  name: string;
  type: number;
  vmin: number;
  vmax: number;
}

export interface WxLayerTableBuildHandle {
  name(): string;
  addField(name: string, fieldType: number, vmin?: number, vmax?: number): number;
  setGeometryType(geometryType: number, coordinateType: number, aabboxEnabled?: boolean): void;
  enableStringTableU32(enabled?: boolean): void;
  setExtent(minx: number, miny: number, maxx: number, maxy: number): void;
  setDbIndex(index: number): void;
  addFeatureBegin(): void;
  setFieldDouble(index: number, value: number): void;
  setFieldInt(index: number, value: number): void;
  setFieldString(index: number, value: string): void;
  setFieldRef(index: number, refPtr: number): void;
  createFeatureRef(index?: number): number;
  freeFeatureRef(refPtr: number): void;
  setGeometryWKT(data: string): void;
  setGeometryWKB(dataPtr: number, size: number): void;
  setGeometryRaw(dataPtr: number, size: number): void;
  addFeatureEnd(): void;
  delete(): void;
}

export interface WxLayerTableHandle {
  name(): string;
  getGeometryType(): number;
  getFieldCount(): number;
  getFieldDefn(index: number): FieldDefView;
  getFieldOffset(index: number): number;
  getFeatureByteSize(): number;
  getExtentMinX(): number;
  getExtentMinY(): number;
  getExtentMaxX(): number;
  getExtentMaxY(): number;
  getFeatureCount(): number;
  rewind(): void;
  next(): boolean;
  row(): number;
  geometryView(): ChunkView;
  getFieldAsFloat(index: number): number;
  getFieldAsInt(index: number): number;
  getFieldAsString(index: number): string;
  getFieldAsRef(index: number): number;
  setFeatureCookie(cookie: number): void;
  getFeatureCookie(): number;
  tryGetFeatureAt(index: number): WxFeatureHandle;
  delete(): void;
}

export interface WxFeatureHandle {
  layer(): WxLayerTableHandle;
  geometryView(): ChunkView;
  getFieldAsFloat(index: number): number;
  getFieldAsInt(index: number): number;
  getFieldAsString(index: number): string;
  getFieldAsRef(index: number): number;
  setFeatureCookie(cookie: number): void;
  getFeatureCookie(): number;
  getAddress(): number;
  setFieldDouble(index: number, value: number): void;
  setFieldInt(index: number, value: number): void;
  setFieldFeature(index: number, featurePtr: number): void;
  getFieldsIntoHeap(fieldIdsPtr: number, nFields: number, outPtr: number): void;
  setFieldsFromHeap(fieldIdsPtr: number, valuesPtr: number, nFields: number): void;
  delete(): void;
}

export interface WxDatabaseHandle {
  getLayerCount(): number;
  getLayer(index: number): WxLayerTableHandle;
  tryGetFeature(refPtr: number): number;
  bufferView(): ChunkView;
  delete(): void;
}

export interface WxDatabaseHandleStatic {
  loadFromHeap(dataPtr: number, size: number): WxDatabaseHandle;
}

export interface WxDatabaseBuildHandle {
  begin(config: string): void;
  truncate(layerName: string, featureCount: number): void;
  createLayerBegin(layerName: string): WxLayerTableBuildHandle;
  addField(name: string, fieldType: number, vmin?: number, vmax?: number): number;
  setGeometryType(geometryType: number, coordinateType: number, aabboxEnabled?: boolean): void;
  enableStringTableU32(enabled?: boolean): void;
  setExtent(minx: number, miny: number, maxx: number, maxy: number): void;
  addFeatureBegin(): void;
  setFieldDouble(index: number, value: number): void;
  setFieldInt(index: number, value: number): void;
  setFieldString(index: number, value: string): void;
  setGeometryWKT(data: string): void;
  setGeometryWKB(dataPtr: number, size: number): void;
  setGeometryRaw(dataPtr: number, size: number): void;
  addFeatureEnd(): void;
  createLayerEnd(): void;
  post(stream: WxMemoryStreamHandle): void;
  delete(): void;
}

export interface FastdbModule {
  WxMemoryStream: new () => WxMemoryStreamHandle;
  WxDatabaseBuild: new () => WxDatabaseBuildHandle;
  WxDatabase: WxDatabaseHandleStatic;
  gtAny: number;
  gtPoint: number;
  gtLineString: number;
  gtPolygon: number;
  gtNone: number;
  cfF32: number;
  cfF64: number;
  cfTx16: number;
  cfTx24: number;
  cfTx32: number;
  cfDefault: number;
  ftU8: number;
  ftU16: number;
  ftU32: number;
  ftI32: number;
  ftU8n: number;
  ftU16n: number;
  ftF32: number;
  ftF64: number;
  ftSTR: number;
  ftWSTR: number;
  ftREF: number;
  HEAPU8: Uint8Array;
  HEAPU32: Uint32Array;
  HEAP32: Int32Array;
  HEAPF64: Float64Array;
  _malloc(size: number): number;
  _free(ptr: number): void;
}

export interface FastdbModuleFactory {
  (moduleOverrides?: Record<string, unknown>): Promise<FastdbModule>;
}

let modulePromise: Promise<FastdbModule> | null = null;

export function initFastdb(): Promise<FastdbModule> {
  if (modulePromise === null) {
    modulePromise = (FastdbWasm as unknown as FastdbModuleFactory)();
  }
  return modulePromise;
}

export async function getFastdbModule(): Promise<FastdbModule> {
  return initFastdb();
}
