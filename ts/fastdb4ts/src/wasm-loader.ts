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

export interface PayloadBackingStatsView {
  reservations: bigint;
  writes: bigint;
  commits: bigint;
  rollbacks: bigint;
  retains: bigint;
  releases: bigint;
}

export interface WxPayloadBackingHandle {
  backingAddress(): number;
  stats(): PayloadBackingStatsView;
  delete(): void;
}

export interface WxPayloadOwnedBytesHandle {
  dataAddress(): number;
  size(): number;
  backingAddress(): number;
  ownerToken(): number;
  delete(): void;
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
  setFieldWString(index: number, value: string): void;
  setFieldRef(index: number, refPtr: number): void;
  addListField(name: string, elementType: number): void;
  setFieldListNumeric(index: number, dataPtr: number, nbytes: number): void;
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
  getFieldAsWString(index: number): string;
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
  getFieldAsWString(index: number): string;
  getFieldAsRef(index: number): number;
  setFeatureCookie(cookie: number): void;
  getFeatureCookie(): number;
  getAddress(): number;
  setFieldDouble(index: number, value: number): void;
  setFieldInt(index: number, value: number): void;
  setFieldFeature(index: number, featurePtr: number): void;
  getFieldListSize(index: number): number;
  getFieldListRefAt(index: number, listIndex: number): number;
  getFieldAsListView(index: number): ChunkView;
  getFieldsIntoHeap(fieldIdsPtr: number, nFields: number, outPtr: number): void;
  setFieldsFromHeap(fieldIdsPtr: number, valuesPtr: number, nFields: number): void;
  delete(): void;
}

export interface WxDatabaseHandle {
  getLayerCount(): number;
  getLayer(index: number): WxLayerTableHandle;
  tryGetFeature(refPtr: number): number;
  tryGetFeatureHandle(refPtr: number): WxFeatureHandle | null;
  bufferView(): ChunkView;
  delete(): void;
}

export interface WxDatabaseHandleStatic {
  loadFromHeap(dataPtr: number, size: number): WxDatabaseHandle;
  loadFromOwnedHeap(dataPtr: number, size: number): WxDatabaseHandle;
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
  setFieldWString(index: number, value: string): void;
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
  WxPayloadBacking: new (
    allowDirect: boolean,
    failWrite: boolean,
    failCommit: boolean,
  ) => WxPayloadBackingHandle;
  WxPayloadOwnedBytes: new (
    source: number,
    size: number,
  ) => WxPayloadOwnedBytesHandle;
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
  ftList: number;
  HEAPU8: Uint8Array;
  HEAPU32: Uint32Array;
  HEAP32: Int32Array;
  HEAPF64: Float64Array;
  _malloc(size: number): number;
  _free(ptr: number): void;
  _fdb_payload_v1_abi_version(): number;
  _fdb_payload_v1_spec_compile_json(
    source: number,
    sourceSize: bigint,
    options: number,
    outSpec: number,
    outError: number,
  ): number;
  _fdb_payload_v1_spec_retain(spec: number): void;
  _fdb_payload_v1_spec_release(spec: number): void;
  _fdb_payload_v1_spec_canonical_json(
    spec: number,
    outBlob: number,
    outError: number,
  ): number;
  _fdb_payload_v1_spec_sha256(
    spec: number,
    outDigest: number,
    outError: number,
  ): number;
  _fdb_payload_v1_spec_manifest_json(
    spec: number,
    outBlob: number,
    outError: number,
  ): number;
  _fdb_payload_v1_spec_profile(
    spec: number,
    outProfile: number,
    outError: number,
  ): number;
  _fdb_payload_v1_capabilities_init(capabilities: number): void;
  _fdb_payload_v1_builder_options_init(options: number): void;
  _fdb_payload_v1_fixed_run_init(run: number): void;
  _fdb_payload_v1_open_options_init(options: number): void;
  _fdb_payload_v1_plan_info_init(info: number): void;
  _fdb_payload_v1_execution_report_init(report: number): void;
  _fdb_payload_v1_spec_capabilities(
    spec: number,
    outCapabilities: number,
    outError: number,
  ): number;
  _fdb_payload_v1_spec_entry_count(
    spec: number,
    outCount: number,
    outError: number,
  ): number;
  _fdb_payload_v1_spec_entry_id(
    spec: number,
    entryIndex: number,
    outId: number,
    outError: number,
  ): number;
  _fdb_payload_v1_spec_entry_index(
    spec: number,
    id: number,
    idSize: bigint,
    outEntryIndex: number,
    outError: number,
  ): number;
  _fdb_payload_v1_spec_component_count(
    spec: number,
    outCount: number,
    outError: number,
  ): number;
  _fdb_payload_v1_spec_component_id(
    spec: number,
    componentIndex: number,
    outId: number,
    outError: number,
  ): number;
  _fdb_payload_v1_spec_component_index(
    spec: number,
    id: number,
    idSize: bigint,
    outComponentIndex: number,
    outError: number,
  ): number;
  _fdb_payload_v1_spec_component_field_count(
    spec: number,
    componentIndex: number,
    outCount: number,
    outError: number,
  ): number;
  _fdb_payload_v1_spec_component_field_id(
    spec: number,
    componentIndex: number,
    fieldIndex: number,
    outId: number,
    outError: number,
  ): number;
  _fdb_payload_v1_spec_component_field_index(
    spec: number,
    componentIndex: number,
    id: number,
    idSize: bigint,
    outFieldIndex: number,
    outError: number,
  ): number;
  _fdb_payload_v1_builder_create(
    spec: number,
    options: number,
    outBuilder: number,
    outError: number,
  ): number;
  _fdb_payload_v1_builder_release(builder: number): void;
  _fdb_payload_v1_builder_entry_begin(
    builder: number,
    entryIndex: number,
    valueCount: bigint,
    outError: number,
  ): number;
  _fdb_payload_v1_builder_object_declare(
    builder: number,
    componentIndex: number,
    outObject: number,
    outError: number,
  ): number;
  _fdb_payload_v1_builder_object_fill_begin(
    builder: number,
    object: bigint,
    outError: number,
  ): number;
  _fdb_payload_v1_builder_value_null(
    builder: number,
    outError: number,
  ): number;
  _fdb_payload_v1_builder_value_bool(
    builder: number,
    value: number,
    outError: number,
  ): number;
  _fdb_payload_v1_builder_value_u8(
    builder: number,
    value: number,
    outError: number,
  ): number;
  _fdb_payload_v1_builder_value_u16(
    builder: number,
    value: number,
    outError: number,
  ): number;
  _fdb_payload_v1_builder_value_u32(
    builder: number,
    value: number,
    outError: number,
  ): number;
  _fdb_payload_v1_builder_value_i32(
    builder: number,
    value: number,
    outError: number,
  ): number;
  _fdb_payload_v1_builder_value_u8n_f64_bits(
    builder: number,
    value: bigint,
    outError: number,
  ): number;
  _fdb_payload_v1_builder_value_u16n_f64_bits(
    builder: number,
    value: bigint,
    outError: number,
  ): number;
  _fdb_payload_v1_builder_value_f32_bits(
    builder: number,
    value: number,
    outError: number,
  ): number;
  _fdb_payload_v1_builder_value_f64_bits(
    builder: number,
    value: bigint,
    outError: number,
  ): number;
  _fdb_payload_v1_builder_value_str(
    builder: number,
    value: number,
    valueSize: bigint,
    outError: number,
  ): number;
  _fdb_payload_v1_builder_value_wstr(
    builder: number,
    value: number,
    valueSize: bigint,
    outError: number,
  ): number;
  _fdb_payload_v1_builder_value_bytes(
    builder: number,
    value: number,
    valueSize: bigint,
    outError: number,
  ): number;
  _fdb_payload_v1_builder_value_fixed_run(
    builder: number,
    run: number,
    outError: number,
  ): number;
  _fdb_payload_v1_builder_value_component_begin(
    builder: number,
    outError: number,
  ): number;
  _fdb_payload_v1_builder_value_list_begin(
    builder: number,
    itemCount: bigint,
    outError: number,
  ): number;
  _fdb_payload_v1_builder_value_object(
    builder: number,
    object: bigint,
    outError: number,
  ): number;
  _fdb_payload_v1_builder_value_ref(
    builder: number,
    object: bigint,
    outError: number,
  ): number;
  _fdb_payload_v1_builder_freeze(
    builder: number,
    outPlan: number,
    outError: number,
  ): number;
  _fdb_payload_v1_plan_retain(plan: number): void;
  _fdb_payload_v1_plan_release(plan: number): void;
  _fdb_payload_v1_plan_info(
    plan: number,
    outInfo: number,
    outError: number,
  ): number;
  _fdb_payload_v1_plan_execute(
    plan: number,
    policy: number,
    backing: number,
    outPayload: number,
    outReport: number,
    outError: number,
  ): number;
  _fdb_payload_v1_payload_open_copy(
    spec: number,
    bytes: number,
    byteCount: bigint,
    options: number,
    outPayload: number,
    outError: number,
  ): number;
  _fdb_payload_v1_payload_open_external(
    spec: number,
    bytes: number,
    byteCount: bigint,
    backing: number,
    ownerToken: number,
    options: number,
    outPayload: number,
    outError: number,
  ): number;
  _fdb_payload_v1_payload_retain(payload: number): void;
  _fdb_payload_v1_payload_release(payload: number): void;
  _fdb_payload_v1_payload_sha256(
    payload: number,
    outDigest: number,
    outError: number,
  ): number;
  _fdb_payload_v1_payload_profile(
    payload: number,
    outProfile: number,
    outError: number,
  ): number;
  _fdb_payload_v1_payload_execution_report(
    payload: number,
    outReport: number,
    outError: number,
  ): number;
  _fdb_payload_v1_payload_binary_blob(
    payload: number,
    outBlob: number,
    outError: number,
  ): number;
  _fdb_payload_v1_blob_data(blob: number): number;
  _fdb_payload_v1_blob_size(blob: number): bigint;
  _fdb_payload_v1_blob_release(blob: number): void;
  _fdb_payload_v1_error_code(error: number): number;
  _fdb_payload_v1_error_symbol(
    error: number,
    outData: number,
    outSize: number,
  ): void;
  _fdb_payload_v1_error_path(
    error: number,
    outData: number,
    outSize: number,
  ): void;
  _fdb_payload_v1_error_message(
    error: number,
    outData: number,
    outSize: number,
  ): void;
  _fdb_payload_v1_error_details_json(
    error: number,
    outData: number,
    outSize: number,
  ): void;
  _fdb_payload_v1_error_release(error: number): void;
}

export interface FastdbModuleFactory {
  (moduleOverrides?: Record<string, unknown>): Promise<FastdbModule>;
}

let modulePromise: Promise<FastdbModule> | null = null;
let loadedModule: FastdbModule | null = null;

export function initFastdb(): Promise<FastdbModule> {
  if (modulePromise === null) {
    modulePromise = (FastdbWasm as unknown as FastdbModuleFactory)().then((module) => {
      loadedModule = module;
      return module;
    });
  }
  return modulePromise;
}

export async function getFastdbModule(): Promise<FastdbModule> {
  return initFastdb();
}

export function getInitializedFastdbModule(): FastdbModule {
  if (loadedModule === null) {
    throw new Error('fastdb4ts has not been initialized. Call await initFastdb() first.');
  }
  return loadedModule;
}
