#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>

#include <emscripten/bind.h>

#include "fastdb.h"

using namespace emscripten;
using namespace wx;

namespace {

struct ChunkView {
    uintptr_t data;
    size_t size;
};

struct FieldDefView {
    std::string name;
    size_t type;
    double vmin;
    double vmax;
};

ChunkView chunk_to_view(chunk_data_t chunk) {
    return ChunkView{
        reinterpret_cast<uintptr_t>(chunk.pdata),
        chunk.size,
    };
}

void db_build_begin(FastVectorDbBuild& db, const std::string& config) {
    db.begin(config.c_str());
}

void db_build_truncate(FastVectorDbBuild& db, const std::string& layer_name, unsigned nfeatures) {
    db.truncate(layer_name.c_str(), nfeatures);
}

FastVectorDbLayerBuild* db_build_create_layer_begin(FastVectorDbBuild& db, const std::string& layer_name) {
    return db.createLayerBegin(layer_name.c_str());
}

int db_build_add_field(FastVectorDbBuild& db, const std::string& name, unsigned ft, double vmin, double vmax) {
    return db.addField(name.c_str(), ft, vmin, vmax);
}

void db_build_set_geometry_type(FastVectorDbBuild& db, int geometry_type, int coordinate_type, bool aabbox_enabled) {
    db.setGeometryType(
        static_cast<GeometryLikeEnum>(geometry_type),
        static_cast<CoordinateFormatEnum>(coordinate_type),
        aabbox_enabled
    );
}

void db_build_set_field_string(FastVectorDbBuild& db, unsigned ix, const std::string& value) {
    db.setField(ix, value.c_str());
}

void db_build_set_geometry_wkt(FastVectorDbBuild& db, const std::string& value) {
    db.setGeometryWKT(value.c_str());
}

void db_build_post_to_memory_stream(FastVectorDbBuild& db, MemoryStream& stream) {
    db.post(&stream);
}

std::string layer_build_name(FastVectorDbLayerBuild& layer) {
    const char* name = layer.name();
    return name ? std::string(name) : std::string();
}

int layer_build_add_field(FastVectorDbLayerBuild& layer, const std::string& name, unsigned ft, double vmin, double vmax) {
    return layer.addField(name.c_str(), ft, vmin, vmax);
}

void layer_build_set_geometry_type(FastVectorDbLayerBuild& layer, int geometry_type, int coordinate_type, bool aabbox_enabled) {
    layer.setGeometryType(
        static_cast<GeometryLikeEnum>(geometry_type),
        static_cast<CoordinateFormatEnum>(coordinate_type),
        aabbox_enabled
    );
}

void layer_build_set_field_string(FastVectorDbLayerBuild& layer, unsigned ix, const std::string& value) {
    layer.setField(ix, value.c_str());
}

void layer_build_set_geometry_wkt(FastVectorDbLayerBuild& layer, const std::string& value) {
    layer.setGeometryWKT(value.c_str());
}

FieldDefView get_field_defn_view(FastVectorDbLayer& layer, unsigned ix) {
    size_t type = 0;
    double vmin = 0.0;
    double vmax = 0.0;
    const char* name = layer.getFieldDefn_p(ix, &type, &vmin, &vmax);
    return FieldDefView{
        name ? std::string(name) : std::string(),
        type,
        vmin,
        vmax,
    };
}

std::string layer_name(FastVectorDbLayer& layer) {
    const char* name = layer.name();
    return name ? std::string(name) : std::string();
}

int layer_get_geometry_type(FastVectorDbLayer& layer) {
    return static_cast<int>(layer.getGeometryType());
}

double layer_get_extent_minx(FastVectorDbLayer& layer) {
    double minx = 0.0;
    double miny = 0.0;
    double maxx = 0.0;
    double maxy = 0.0;
    layer.getExtent(minx, miny, maxx, maxy);
    return minx;
}

double layer_get_extent_miny(FastVectorDbLayer& layer) {
    double minx = 0.0;
    double miny = 0.0;
    double maxx = 0.0;
    double maxy = 0.0;
    layer.getExtent(minx, miny, maxx, maxy);
    return miny;
}

double layer_get_extent_maxx(FastVectorDbLayer& layer) {
    double minx = 0.0;
    double miny = 0.0;
    double maxx = 0.0;
    double maxy = 0.0;
    layer.getExtent(minx, miny, maxx, maxy);
    return maxx;
}

double layer_get_extent_maxy(FastVectorDbLayer& layer) {
    double minx = 0.0;
    double miny = 0.0;
    double maxx = 0.0;
    double maxy = 0.0;
    layer.getExtent(minx, miny, maxx, maxy);
    return maxy;
}

uintptr_t feature_address(FastVectorDbFeature& feature) {
    return reinterpret_cast<uintptr_t>(feature.getAddress());
}

std::string layer_get_field_as_string(FastVectorDbLayer& layer, u32 ix) {
    const char* value = layer.getFieldAsString(ix);
    return value ? std::string(value) : std::string();
}

std::string feature_get_field_as_string(FastVectorDbFeature& feature, u32 ix) {
    const char* value = feature.getFieldAsString(ix);
    return value ? std::string(value) : std::string();
}

uintptr_t feature_cookie_get(FastVectorDbFeature& feature) {
    return reinterpret_cast<uintptr_t>(feature.getFeatureCookie());
}

uintptr_t layer_cookie_get(FastVectorDbLayer& layer) {
    return reinterpret_cast<uintptr_t>(layer.getFeatureCookie());
}

uintptr_t feature_ref_from_layer(FastVectorDbLayer& layer, u32 ix) {
    return reinterpret_cast<uintptr_t>(layer.getFieldAsFeatureRef(ix));
}

uintptr_t feature_ref_from_feature(FastVectorDbFeature& feature, u32 ix) {
    return reinterpret_cast<uintptr_t>(feature.getFieldAsFeatureRef(ix));
}

void layer_cookie_set(FastVectorDbLayer& layer, uintptr_t cookie) {
    layer.setFeatureCookie(reinterpret_cast<void*>(cookie));
}

void feature_cookie_set(FastVectorDbFeature& feature, uintptr_t cookie) {
    feature.setFeatureCookie(reinterpret_cast<void*>(cookie));
}

void layer_build_set_field_ref(FastVectorDbLayerBuild& layer, unsigned ix, uintptr_t ref_ptr) {
    layer.setField(ix, reinterpret_cast<const FastVectorDbFeatureRef*>(ref_ptr));
}

uintptr_t layer_build_create_feature_ref(FastVectorDbLayerBuild& layer, u32 ix) {
    return reinterpret_cast<uintptr_t>(layer.createFeatureRef(ix));
}

void layer_build_free_feature_ref(FastVectorDbLayerBuild& layer, uintptr_t ref_ptr) {
    layer.freeFeatureRef(reinterpret_cast<FastVectorDbFeatureRef*>(ref_ptr));
}

uintptr_t database_try_get_feature(FastVectorDb& db, uintptr_t ref_ptr) {
    return reinterpret_cast<uintptr_t>(db.tryGetFeature(reinterpret_cast<FastVectorDbFeatureRef*>(ref_ptr)));
}

void feature_set_field_feature(FastVectorDbFeature& feature, u32 ix, uintptr_t feature_ptr) {
    feature.setField(ix, reinterpret_cast<FastVectorDbFeature*>(feature_ptr));
}

void feature_get_fields_into(FastVectorDbFeature& feature, uintptr_t field_ids_ptr, int n_fields, uintptr_t out_ptr) {
    feature.getFieldsAsDoubles(
        reinterpret_cast<const u32*>(field_ids_ptr),
        n_fields,
        reinterpret_cast<double*>(out_ptr)
    );
}

void feature_set_fields_from_heap(FastVectorDbFeature& feature, uintptr_t field_ids_ptr, uintptr_t values_ptr, int n_fields) {
    feature.setFieldsFromDoubles(
        reinterpret_cast<const u32*>(field_ids_ptr),
        reinterpret_cast<const double*>(values_ptr),
        n_fields
    );
}

void free_loaded_db_buffer(void* pdata, size_t, void*) {
    std::free(pdata);
}

FastVectorDb* db_load_from_heap(uintptr_t data_ptr, size_t size) {
    void* copied = std::malloc(size);
    if (copied == nullptr && size != 0) {
        return nullptr;
    }
    if (size != 0) {
        std::memcpy(copied, reinterpret_cast<void*>(data_ptr), size);
    }
    return FastVectorDb::load(copied, size, free_loaded_db_buffer, nullptr);
}

ChunkView db_buffer_view(FastVectorDb& db) {
    return chunk_to_view(db.buffer());
}

ChunkView memory_stream_view(MemoryStream& stream) {
    return chunk_to_view(stream.data());
}

ChunkView layer_geometry_view(FastVectorDbLayer& layer) {
    return chunk_to_view(layer.getGeometryLikeChunk());
}

ChunkView feature_geometry_view(FastVectorDbFeature& feature) {
    return chunk_to_view(feature.getGeometryLikeChunk());
}

}  // namespace

EMSCRIPTEN_BINDINGS(fastdb4ts) {
    value_object<ChunkView>("ChunkView")
        .field("data", &ChunkView::data)
        .field("size", &ChunkView::size);

    value_object<FieldDefView>("FieldDefView")
        .field("name", &FieldDefView::name)
        .field("type", &FieldDefView::type)
        .field("vmin", &FieldDefView::vmin)
        .field("vmax", &FieldDefView::vmax);

    constant("gtAny", static_cast<int>(gtAny));
    constant("gtPoint", static_cast<int>(gtPoint));
    constant("gtLineString", static_cast<int>(gtLineString));
    constant("gtPolygon", static_cast<int>(gtPolygon));
    constant("gtNone", static_cast<int>(gtNone));

    constant("cfF32", static_cast<int>(cfF32));
    constant("cfF64", static_cast<int>(cfF64));
    constant("cfTx16", static_cast<int>(cfTx16));
    constant("cfTx24", static_cast<int>(cfTx24));
    constant("cfTx32", static_cast<int>(cfTx32));
    constant("cfDefault", static_cast<int>(cfDefault));

    constant("ftU8", static_cast<int>(ftU8));
    constant("ftU16", static_cast<int>(ftU16));
    constant("ftU32", static_cast<int>(ftU32));
    constant("ftI32", static_cast<int>(ftI32));
    constant("ftU8n", static_cast<int>(ftU8n));
    constant("ftU16n", static_cast<int>(ftU16n));
    constant("ftF32", static_cast<int>(ftF32));
    constant("ftF64", static_cast<int>(ftF64));
    constant("ftSTR", static_cast<int>(ftSTR));
    constant("ftWSTR", static_cast<int>(ftWSTR));
    constant("ftREF", static_cast<int>(ftREF));

    class_<MemoryStream>("WxMemoryStream")
        .constructor<>()
        .function("dataView", &memory_stream_view)
        .function("reset", &MemoryStream::reset);

    class_<FastVectorDbBuild>("WxDatabaseBuild")
        .constructor<>()
        .function("begin", &db_build_begin)
        .function("truncate", &db_build_truncate)
        .function("createLayerBegin", &db_build_create_layer_begin, allow_raw_pointers())
        .function("addField", &db_build_add_field)
        .function("setGeometryType", &db_build_set_geometry_type)
        .function("enableStringTableU32", &FastVectorDbBuild::enableStringTableU32)
        .function("setExtent", &FastVectorDbBuild::setExtent)
        .function("addFeatureBegin", &FastVectorDbBuild::addFeatureBegin)
        .function("setFieldDouble", select_overload<void(unsigned, double)>(&FastVectorDbBuild::setField))
        .function("setFieldInt", select_overload<void(unsigned, int)>(&FastVectorDbBuild::setField))
        .function("setFieldString", &db_build_set_field_string)
        .function("setGeometryWKT", &db_build_set_geometry_wkt)
        .function("setGeometryWKB", &FastVectorDbBuild::setGeometryWKB, allow_raw_pointers())
        .function("setGeometryRaw", &FastVectorDbBuild::setGeometryRaw, allow_raw_pointers())
        .function("addFeatureEnd", &FastVectorDbBuild::addFeatureEnd)
        .function("createLayerEnd", &FastVectorDbBuild::createLayerEnd)
        .function("post", &db_build_post_to_memory_stream, allow_raw_pointers());

    class_<FastVectorDbLayerBuild>("WxLayerTableBuild")
        .function("name", &layer_build_name)
        .function("addField", &layer_build_add_field)
        .function("setGeometryType", &layer_build_set_geometry_type)
        .function("enableStringTableU32", &FastVectorDbLayerBuild::enableStringTableU32)
        .function("setExtent", &FastVectorDbLayerBuild::setExtent)
        .function("setDbIndex", &FastVectorDbLayerBuild::setDbIndex)
        .function("addFeatureBegin", &FastVectorDbLayerBuild::addFeatureBegin)
        .function("setFieldDouble", select_overload<void(unsigned, double)>(&FastVectorDbLayerBuild::setField))
        .function("setFieldInt", select_overload<void(unsigned, int)>(&FastVectorDbLayerBuild::setField))
        .function("setFieldString", &layer_build_set_field_string)
        .function("setFieldRef", &layer_build_set_field_ref)
        .function("createFeatureRef", &layer_build_create_feature_ref)
        .function("freeFeatureRef", &layer_build_free_feature_ref)
        .function("setGeometryWKT", &layer_build_set_geometry_wkt)
        .function("setGeometryWKB", &FastVectorDbLayerBuild::setGeometryWKB, allow_raw_pointers())
        .function("setGeometryRaw", &FastVectorDbLayerBuild::setGeometryRaw, allow_raw_pointers())
        .function("addFeatureEnd", &FastVectorDbLayerBuild::addFeatureEnd);

    class_<FastVectorDb>("WxDatabase")
        .function("getLayerCount", &FastVectorDb::getLayerCount)
        .function("getLayer", &FastVectorDb::getLayer, allow_raw_pointers())
        .function("tryGetFeature", &database_try_get_feature)
        .function("bufferView", &db_buffer_view)
        .class_function("loadFromHeap", &db_load_from_heap, allow_raw_pointers());

    class_<FastVectorDbLayer>("WxLayerTable")
        .function("name", &layer_name)
        .function("getGeometryType", &layer_get_geometry_type)
        .function("getFieldCount", &FastVectorDbLayer::getFieldCount)
        .function("getFieldDefn", &get_field_defn_view)
        .function("getFieldOffset", &FastVectorDbLayer::getFieldOffset)
        .function("getFeatureByteSize", &FastVectorDbLayer::getFeatureByteSize)
        .function("getExtentMinX", &layer_get_extent_minx)
        .function("getExtentMinY", &layer_get_extent_miny)
        .function("getExtentMaxX", &layer_get_extent_maxx)
        .function("getExtentMaxY", &layer_get_extent_maxy)
        .function("getFeatureCount", &FastVectorDbLayer::getFeatureCount)
        .function("rewind", &FastVectorDbLayer::rewind)
        .function("next", &FastVectorDbLayer::next)
        .function("row", &FastVectorDbLayer::row)
        .function("geometryView", &layer_geometry_view)
        .function("getFieldAsFloat", &FastVectorDbLayer::getFieldAsFloat)
        .function("getFieldAsInt", &FastVectorDbLayer::getFieldAsInt)
        .function("getFieldAsString", &layer_get_field_as_string)
        .function("getFieldAsRef", &feature_ref_from_layer)
        .function("setFeatureCookie", &layer_cookie_set)
        .function("getFeatureCookie", &layer_cookie_get)
        .function("tryGetFeatureAt", &FastVectorDbLayer::tryGetFeatureAt, allow_raw_pointers());

    class_<FastVectorDbFeature>("WxFeature")
        .function("layer", &FastVectorDbFeature::layer, allow_raw_pointers())
        .function("geometryView", &feature_geometry_view)
        .function("getFieldAsFloat", &FastVectorDbFeature::getFieldAsFloat)
        .function("getFieldAsInt", &FastVectorDbFeature::getFieldAsInt)
        .function("getFieldAsString", &feature_get_field_as_string)
        .function("getFieldAsRef", &feature_ref_from_feature)
        .function("setFeatureCookie", &feature_cookie_set)
        .function("getFeatureCookie", &feature_cookie_get)
        .function("getAddress", &feature_address)
        .function("setFieldDouble", select_overload<void(u32, double)>(&FastVectorDbFeature::setField))
        .function("setFieldInt", select_overload<void(u32, int)>(&FastVectorDbFeature::setField))
        .function("setFieldFeature", &feature_set_field_feature)
        .function("getFieldsIntoHeap", &feature_get_fields_into)
        .function("setFieldsFromHeap", &feature_set_fields_from_heap);
}
