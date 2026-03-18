# FastDB → WebAssembly Binding Quick Reference

## Executive Summary

**FastDB** is a fast, columnar vector database optimized for:
- **Writing:** Building layered geo-spatial databases with WKT/WKB geometries
- **Reading:** Efficient in-memory queries with zero-copy column access
- **Serialization:** Compact binary format with optional memory callbacks

**For WASM:** Core architecture is FFI-friendly (pointer-based, pimpl pattern, no templates in public API)

This reference is stored under `ts/analysis/` together with the full architecture analysis.

---

## Quick API Surface

### Database Builder (Write Path)
```
FastVectorDbBuild
├── begin(config)
├── createLayerBegin(name) → FastVectorDbLayerBuild
│   ├── addField(name, type, min, max)
│   ├── setGeometryType(geometry, coordinates, aabbox)
│   ├── addFeatureBegin()
│   ├── setGeometry(data, size, format) [WKT/WKB/RAW]
│   ├── setField(index, value) [double/int/string variants]
│   └── addFeatureEnd()
├── createLayerEnd()
└── save(stream) → MemoryStream.getBytes()
```

### Database Reader (Read Path)
```
FastVectorDb
├── load_xbuffer(data, size)
├── getLayerCount()
└── getLayer(index) → FastVectorDbLayer
    ├── Metadata: getGeometryType(), getFieldCount(), getFeatureCount()
    ├── Layout: getFieldDefn(ix), getFieldOffset(ix), getFeatureByteSize()
    ├── Bounds: getExtent_p(minx, miny, maxx, maxy)
    ├── Iteration: rewind(), next()
    ├── Access: tryGetFeatureAt(ix) → FastVectorDbFeature
    │   ├── getFieldAsFloat/Int/String/Ref(ix)
    │   ├── getFieldsAsDoubles(ids) [batch]
    │   ├── getAddress() [raw pointer for memory access]
    │   └── getGeometryLikeChunk() → chunk_data_t
    └── Column: get_column(ix) [Python NumPy only]
```

### Buffer Operations
```
chunk_data_t
├── as_array(npType) → NumPy array [zero-copy, Python only]
├── to_bytes() → Python bytes [copy]
├── copy_to_buffer(dest) → int [direct memcpy]
```

---

## What's Already SWIG-Wrapped (Reusable for WASM)

✅ **Type Mappings:**
- `(void *pdata, size_t size)` → buffer handling
- `(double *minx, ...)` → output parameters
- `enum FieldTypeEnum` → properly typed

✅ **Special Methods:**
- `getFieldDefn_p()` → handles output parameters
- `getExtent_p()` → handles output parameters
- `get_swig_ptr_as_long()` → pointer-to-integer conversion
- `chunk_data_t.as_array()` → memory interface pattern

✅ **Memory Handling:**
- `MemoryStream` → simple write/read/reset/getBytes
- `chunk_data_t` → pointer + size abstraction
- Pimpl pattern → hidden memory management

❌ **Not Wrapped (Architecture Limits):**
- `GeometryReturn` callback → no virtual callbacks in Python/Go
- `WriteStream` abstract class → Python doesn't use it
- `TileBoxTake` / `FastVectorTileDb` → explicitly ignored

---

## NumPy Integration Pattern (Model for WASM)

### Zero-Copy Technique
```
C++ vector<u8> m_buffer
     ↓
chunk_data_t {size=N, pdata=ptr}
     ↓
PyArray_SimpleNewFromData(
    shape=(N/itemsize),
    dtype=float64,
    data=ptr,
    strides=(itemsize)
)
     ↓
FLAGS: C_CONTIGUOUS | WRITEABLE | ~OWNDATA
     ↓
NumPy array (no copy, no auto-free)
```

**WASM Equivalent:**
```javascript
const buf = db.buffer();  // chunk_data_t
const view = new Float64Array(
    Module.HEAP8.buffer,  // WebAssembly.Memory
    buf.pdata,            // pointer
    buf.size / 8          // elements
);
// Zero-copy, but must manage lifetime manually
```

### Column Access (__array_interface__ pattern)
```python
# Computes: address + field_offset + i * feature_byte_size
col = layer.get_column(field_idx)
arr = np.array(col, copy=False)
```

**WASM Equivalent:**
```javascript
const address = layer.tryGetFeatureAt(0).getAddress();
const offset = layer.getFieldOffset(fieldIdx);
const stride = layer.getFeatureByteSize();
const count = layer.getFeatureCount();
const typeSize = 8; // float64

// Build column view
const view = new Float64Array(count);
const heap = new Float64Array(Module.HEAP8.buffer);
for (let i = 0; i < count; i++) {
    view[i] = heap[(address + offset + i * stride) / 8];
}
// Or use direct memory if Emscripten supports it
```

---

## Memory Layout (Important for WASM Direct Access)

### Database File Format
```
Offset    Size      Description
0         16        Magic: "FASTVectorDB0.1"
16        4         Layer count (u32)
20+       N*M       Layers (variable size)
```

### Per-Layer Structure
```
layer_header_t:
  name[64]                  ← Layer name
  feature_count (u32)       ← Number of features
  geometry_type (u16)       ← Point/LineString/Polygon
  field_count (u16)         ← Number of attributes
  coord_format (u16)        ← F32/F64/Tx16/Tx24/Tx32
  aabbox_enable (bool)      ← Has bounding box
  minx, miny, maxx, maxy    ← Bounds
  offset_table (size_t)     ← Where features start
  offset_strings (size_t)   ← Where string data starts
  offset_wstrings (size_t)  ← Where unicode data starts
  
feature records:
  [field0][field1]...[fieldN][geometry]
  └─────── stride ────────┘
  stride = getFeatureByteSize()
  
  field access:
    value_ptr = base + feature_idx * stride + field_offset
```

### Field Type Sizes
| Type | Size | Notes |
|------|------|-------|
| ftU8 | 1 | unsigned 8-bit |
| ftU16 | 2 | unsigned 16-bit |
| ftU32 | 4 | unsigned 32-bit |
| ftI32 | 4 | signed 32-bit |
| ftF32 | 4 | float32 |
| ftF64 | 8 | float64 |
| ftSTR | 4 | u16 string offset |
| ftWSTR | 4 | u16 unicode offset |
| ftFeatureRef | 5 | packed reference |

---

## WASM Implementation Strategy

### Tier 1: Essential (Minimal WASM Binding)
**Expose:**
- `FastVectorDbBuild` - full builder API
- `FastVectorDb` - load + getLayerCount
- `FastVectorDbLayer` - metadata + iteration
- `FastVectorDbFeature` - field access
- `MemoryStream` - serialization

**Don't Expose:**
- Callbacks (`GeometryReturn`, `WriteStream`)
- Tile management (`TileBoxTake`, `FastVectorTileDb`)

**TypeScript APIs:**
```typescript
class Database {
    static loadBuffer(data: Uint8Array): Database;
    getLayerCount(): number;
    getLayer(idx: number): Layer;
}

class Layer {
    getFeatureCount(): number;
    getFieldCount(): number;
    getFieldDefn(idx: number): FieldDefinition;
    getFeatureByteSize(): number;
    getFieldOffset(idx: number): number;
    tryGetFeatureAt(idx: number): Feature;
}

class Feature {
    getFieldAsDouble(idx: number): number;
    getFieldAsInt(idx: number): number;
    getFieldAsString(idx: number): string;
    getAddress(): number; // For direct memory access
}
```

### Tier 2: Performance (WASM Memory Tricks)
**Add:**
- Direct `WebAssembly.Memory` access for columns
- Stride information for vectorized ops
- Batch field operations (`getFieldsAsDoubles`)

### Tier 3: Advanced (Full Compatibility)
**Add:**
- Geometry deserialization (serialize in C++, deserialize in JS)
- Multi-tile support (implement in JS)
- Custom callbacks via JS/WASM bridge

---

## Dependency Mapping

### External C++ Libraries (Must Link)
- **gaiageo**: WKT/WKB geometry parsing
  - Headers: `lib/gaiageo/headers/spatialite/*.h`
  - Emscripten: Must have compiled `.a` or `.so`

- **Clipper2**: Polygon clipping
  - Headers: `lib/clipper/Clipper2Lib/include/clipper2/*.h`
  - Emscripten: Must have compiled `.a` or `.so`

### Emscripten Flags (from CMakeLists.txt)
```cmake
emcc -O2 -s WASM=0 \
     -s ALLOW_MEMORY_GROWTH=1 \
     -s INITIAL_MEMORY=16777216 \
     -s TOTAL_MEMORY=16777216 \
     -s TOTAL_STACK=5242880 \
     -s ERROR_ON_UNDEFINED_SYMBOLS=0 \
     --post-js fastdb4em_i.js \
     fastdb.a gaiageo.a Clipper2.a -o fastdb.js
```

---

## File Reference

| File | Lines | Purpose |
|------|-------|---------|
| `include/fastdb.h` | 483 | Public API header |
| `include/fastdb-config.h` | 33 | Type definitions |
| `include/fastdb-geometry-utils.h` | 175 | Geometry helper (C++ only) |
| `swig/fastdb4py.i` | 338 | Python bindings template |
| `swig/fastdb4go.i` | 82 | Go bindings template |
| `src/FastVectorDb.cpp` | 225 | Database loading |
| `src/FastVectorDb_p.h` | 26 | Database pimpl |
| `src/FastVectorDbLayer.cpp` | 737 | Layer implementation |
| `src/FastVectorDbBuild.cpp` | 340 | Builder implementation |

---

## Critical Things for WASM Developer

1. **Memory Ownership**
   - `FastVectorDb::load(ptr, size, nullptr, nullptr)` → database does NOT own buffer
   - Pass buffer as `Uint8Array`, keep it alive for entire DB lifetime
   - Use `MemoryStream` for building (it owns its vector<u8>)

2. **String Handling**
   - `getFieldAsString()` returns C string pointer (valid only during DB lifetime)
   - Copy to JS immediately: `new TextDecoder().decode(...)`
   - No UTF-16 support (only ASCII in ftSTR, full Unicode in ftWSTR)

3. **Geometry Data**
   - `getGeometryLikeChunk()` returns raw binary
   - Format depends on `setGeometryType(format)`
   - Must implement geometry parser in JS or use gaiageo

4. **Iteration**
   - No callback-based iteration in WASM (not like Python)
   - Use indexed access: `for (i = 0; i < count; i++) { layer.tryGetFeatureAt(i) }`

5. **Zero-Copy Caveats**
   - Reading is safe: layer iterates, no data movement
   - Writing requires creating new database (builders allocate internally)
   - Column access needs manual stride calculation in JS

---

## WASM Binding Checklist

- [ ] Compile gaiageo to Emscripten
- [ ] Compile Clipper2 to Emscripten
- [ ] Generate SWIG wrapper for Emscripten (or manual glue code)
- [ ] Expose `FastVectorDbBuild` + `FastVectorDbLayerBuild`
- [ ] Expose `FastVectorDb` + `FastVectorDbLayer` + `FastVectorDbFeature`
- [ ] Handle `chunk_data_t` for buffer exchange
- [ ] Implement `MemoryStream` → Uint8Array bridge
- [ ] Create TypeScript definitions
- [ ] Test zero-copy column access with WebAssembly.Memory
- [ ] Document string lifetime management
- [ ] Implement geometry serialization strategy
- [ ] Benchmark vs. native (Python) bindings

---

## Further Resources

- **Full Analysis:** `FASTDB_WASM_ARCHITECTURE_ANALYSIS.md`
- **Header API:** `fastcarto/fastdb/include/fastdb.h`
- **Python Model:** `fastcarto/fastdb/swig/fastdb4py.i`
- **Go Model:** `fastcarto/fastdb/swig/fastdb4go.i`
