# FastDB C++ Core Architecture Analysis for WebAssembly Bindings

## 1. Core C++ Classes Overview

### Public API Classes (in `fastcarto/fastdb/include/fastdb.h`)

**Building/Writing Database:**
- **`FastVectorDbBuild`** - Main builder for creating databases
- **`FastVectorDbLayerBuild`** - Layer builder for multi-layer databases

**Reading Database:**
- **`FastVectorDb`** - Main database container (read-only after load)
- **`FastVectorDbLayer`** - Represents a single layer with geometry and attributes
- **`FastVectorDbFeature`** - Individual feature access within a layer

**Utilities:**
- **`MemoryStream`** - WriteStream implementation for in-memory buffering
- **`TileBoxTake`** - Spatial index for tile-based queries
- **`FastVectorTileDb`** - Multi-tile database manager

**Supporting Types:**
- `WriteStream` (abstract base)
- `GeometryReturn` (callback for geometry deserialization)
- `chunk_data_t` - Buffer descriptor (size, pdata)
- `point2_t` - 2D point (x,y doubles)
- `aabbox_t` - Axis-aligned bounding box
- `FastVectorDbFeatureRef` - Cross-layer feature reference

### Enums

```
GeometryLikeEnum: gtAny, gtPoint, gtLineString, gtPolygon, gtNone
CoordinateFormatEnum: cfF32, cfF64, cfTx16, cfTx24, cfTx32
FieldTypeEnum: ftU8, ftU16, ftU32, ftI32, ftU8n, ftU16n, ftF32, ftF64, ftSTR, ftWSTR, ftFeatureRef
GeometryLikeFormat: ginWKT, ginWKB, ginPoint2, ginLineString, ginRAW
```

---

## 2. SWIG Interface Exposure (fastdb4py.i)

### Exposed Methods by Class

#### FastVectorDbBuild (Builder)
```python
# Init & metadata
begin(cfg)                          # Start building
set_extent(minx, miny, maxx, maxy) # Set database bounds
truncate(layerName, nfeatures)      # Pre-allocate features

# Layer creation
create_layer_begin(layerName)       # Start layer (returns FastVectorDbLayerBuild)
create_layer_end()                  # Finish layer

# Field definition
add_field(name, ft, vmin, vmax)     # Add attribute field

# Geometry & features
set_geometry_type(gt, ct, aabboxEnabled)
add_feature_begin()
set_geometry_wkt(data)              # Set as WKT string
set_geometry_wkb(data, size)        # Set as WKB binary
set_geometry_raw(data, size)        # Set as raw geometry
set_field(ix, value)                # Set field (overloaded for double/int/string)
add_feature_end()

# Serialization
post(WriteStream)                   # Write to stream
save(filename)                      # Write to file
```

#### FastVectorDbLayerBuild (Layer Builder)
```python
name()                              # Get layer name
add_field(name, ft, vmin, vmax)
set_geometry_type(gt, ct, aabboxEnabled)
set_extent(minx, miny, maxx, maxy)
set_db_index(ix)
enable_st32(bool)                   # String table 32-bit mode

# Feature building (same as parent)
add_feature_begin()
set_geometry_wkt/wkb/raw(...)
set_field(...)
create_feature_ref()                # Create reference for cross-layer links
add_feature_end()
```

#### FastVectorDb (Reader - Main Database)
```python
get_layer_count()                   # Number of layers
get_layer(ix)                       # Get layer by index
try_get_feature(ref)                # Get feature by reference
buffer()                            # Get raw buffer as chunk_data_t
load(pdata, size)                   # Static: load from buffer (xbuffer variant)
load_xbuffer(pdata, size)           # Alternative loading (SWIG-specific)
```

#### FastVectorDbLayer (Layer - Reader)
```python
# Metadata
name()                              # Layer name
get_geometry_type()                 # Returns GeometryLikeEnum
get_field_count()                   # Number of attribute fields
get_extent_p(minx, miny, maxx, maxy) # Output params
get_feature_count()                 # Number of features

# Field introspection
get_field_defn(ix)                  # Returns (name, type, vmin, vmax)
get_field_offset(ix)                # Memory offset in feature struct
get_feature_byte_size()             # Total size per feature (for layout)

# Feature iteration
rewind()                            # Reset iterator
next()                              # Advance to next feature
row()                               # Current feature index

# Feature access
try_get_feature_at(ix)              # Direct access to feature by index

# Data access (current feature)
get_field_as_float(ix)
get_field_as_int(ix)
get_field_as_string(ix)
get_field_as_wstring(ix)
get_field_as_ref(ix)                # Cross-layer reference

# Geometry
fetch_geometry(callback)             # Invoke callback with geometry parts
get_geometry_like_chunk()            # Raw geometry buffer

# Cookie support
set_feature_cookie(cookie)          # Store void* per feature
get_feature_cookie()

# NumPy integration (Python-only)
get_column(index)                   # Returns __array_interface__ compatible object
```

#### FastVectorDbFeature (Individual Feature)
```python
layer()                             # Get parent layer
get_address()                       # Get memory address as long

# Field access (same as layer current feature)
get_field_as_float(ix)
get_field_as_int(ix)
get_field_as_string(ix)
get_field_as_wstring(ix)
get_field_as_ref(ix)

# Batch field operations
get_fields_as_doubles(field_ids)    # Returns numpy float64 array
get_fields_into(field_ids, out_array) # Read into pre-allocated array
set_fields_from_doubles(field_ids, values) # Write multiple fields

# Geometry
fetch_geometry(callback)
get_geometry_like_chunk()

# Field modification
set_field(ix, value)                # Set field value
set_field(ix, feature)              # Set reference to another feature

# Cookie support
set_feature_cookie(cookie)
get_feature_cookie()
```

#### MemoryStream (Buffer)
```python
get_bytes()                         # Returns Python bytes object
data()                              # Returns chunk_data_t
reset()                             # Clear buffer
write(data, size)                   # Append data
```

#### chunk_data_t (Output)
```python
# NumPy integration (Python-only)
as_array(npType)                    # Create zero-copy NumPy array
to_bytes()                          # Copy to bytes
copy_to_buffer(dest)                # Copy to writable buffer
```

---

## 3. NumPy Integration (Zero-Copy Arrays)

### Architecture

**Location:** `fastdb4py.i` lines 130-196 (chunk_data_t extensions)

**Zero-Copy Pattern:**
```
C++ buffer (pdata, size) 
   ↓
chunk_data_t wrapper
   ↓
PyArray_SimpleNewFromData()  [no copy, no ownership transfer]
   ↓
NumPy array with data=(address, False)
   ↓
NPY_ARRAY_C_CONTIGUOUS | NPY_ARRAY_WRITEABLE (flags set)
   ↓
NPY_ARRAY_OWNDATA cleared (NumPy won't free the buffer)
```

**chunk_data_t Methods:**
1. **`as_array(npType)`** - Creates 1D array from raw buffer
   - Accepts NumPy dtype object
   - Returns PyArrayObject with 1D shape
   - Automatically calculates element count via dtype size
   - Sets proper strides for contiguous layout

2. **`to_bytes()`** - Safe copy to Python bytes
   - Uses PyBytes_FromStringAndSize()
   - Creates independent copy (safe for layer iteration)

3. **`copy_to_buffer(dest_buffer)`** - Direct memcpy to writable buffer
   - Validates destination is writable
   - Fast path: no Python allocation
   - Used for shared memory, bytearray, memoryview

### FastVectorDbLayer Column Access

**Python-only helper** (`get_column(index)`):
```python
class __column_np_interface__:
    @property
    def __array_interface__:
        return {
            'version': 3,
            'typestr': '<f8',           # Type string (little-endian float64)
            'shape': (nfeatures,),
            'data': (address, False),   # Pointer + no ownership
            'strides': (stride,)        # feature_byte_size = stride
        }
```

**Mechanism:**
- Extracts first feature address via `tryGetFeature(0).get_address()`
- Adds field offset
- Computes stride = feature_byte_size (row-major layout)
- NumPy reads __array_interface__ for direct memory access
- Result: zero-copy column access for field data

### NumPy 1.x and 2.x Compatibility

```c
#if defined(NPY_ABI_VERSION) && NPY_ABI_VERSION >= 0x02000000
    item_size = PyDataType_ELSIZE(descr);  // NumPy 2.x
#else
    item_size = descr->elsize;              // NumPy 1.x
#endif
```

---

## 4. Memory Management Patterns

### Pimpl (Pointer to Implementation) Pattern

All public classes use private `Impl` inner class:

```cpp
class FastVectorDb {
public:
    class Impl;
private:
    Impl *impl;
};
```

**Benefits:**
- Binary compatibility (hide internal structure changes)
- Lazy initialization
- Separation of interface and implementation

**Located in:** `src/*_p.h` and `src/*.cpp`

### Memory Layout

**database binary format** starts with:
```
[16 bytes] magic: "FASTVectorDB0.1"
[4 bytes]  layer_count (u32)
[layers...]
```

Each layer:
```
layer_header_t:
  - name[64]
  - feature_count, geometry_type, field_count
  - bounds (minx, miny, maxx, maxy)
  - offsets (table, strings, wstrings)
  - total_size
feature table: fixed-size records
geometry data: variable-size compressed
string table
wstring table
```

**Feature structure** (row-major):
```
[field 0][field 1]...[field N][geometry_data]
stride = get_feature_byte_size()
```

### Ownership & Lifetime

**Building Phase:**
- `FastVectorDbBuild` owns `vector<FastVectorDbLayerBuild*> m_layers`
- Each layer owns `vector<FastVectorDbLayerBuild*> m_created_feature_refs`
- `MemoryStream` owns `vector<u8> m_buffer`
- Manual allocation: `createFeatureRef()` returns `new FastVectorDbFeatureRef*`
- Manual deallocation: `freeFeatureRef(ref)` required

**Loading Phase:**
- `FastVectorDb` owns loaded data via callback: `fnFreeDbBuffer(pdata, size, cookie)`
- Two loading modes:
  1. **Managed:** Pass `nullptr` for fnFreeBuffer → no cleanup (caller retains ownership)
  2. **Unmanaged:** Provide callback → database frees buffer on destruction
- Layer/Feature objects have stack-lifetime references to parent

**Memory Diagram:**
```
Database Buffer (external)
    ↓ (pointer + callback)
FastVectorDb::Impl {m_pdata, m_fnFreeBuffer, m_cookie}
    ↓ (owns vector)
vector<FastVectorDbLayer*> m_layers
    ↓ (holds references)
FastVectorDbLayer/Feature (non-owning views into m_pdata)
```

### Buffer Management

**MemoryStream (builder output):**
```cpp
class MemoryStream::Impl {
    vector<u8> m_buffer;  // Grows via append, no fixed allocation
};
```
- Incremental writes via `write(pdata, size)`
- Retrieved via `data()` returning `chunk_data_t{size, buffer.data()}`
- Reset via `reset()` (clear_to_fit)

**FastVectorDbLayerBuild (feature assembly):**
```cpp
vector<u8> m_current_line_buffer;    // Feature record (fields + geometry)
vector<u8> m_current_geom_buffer;    // Geometry data
```

**String & WString tables:**
- Stored separately in binary format
- Accessed via offset tables
- Support both u16 and u32 indexing

---

## 5. External Dependencies

### Direct Dependencies (in CMakeLists.txt)

1. **gaiageo** (Spatial/Geometry library)
   - Location: `fastcarto/lib/gaiageo`
   - Headers: `lib/gaiageo/headers/spatialite/*.h`
   - Function: WKT/WKB parsing, geometry manipulation
   - Linked: `target_link_libraries(fastdb PRIVATE gaiageo)`

2. **Clipper2** (Polygon clipping library)
   - Location: `fastcarto/lib/clipper/Clipper2Lib`
   - Headers: `lib/clipper/Clipper2Lib/include/clipper2/*.h`
   - Function: Geometry boolean operations
   - Linked: `target_link_libraries(fastdb PRIVATE Clipper2)`

### Indirect Dependencies

**Python (via SWIG):**
- `Python3_INCLUDE_DIRS`
- `Python3_NumPy_INCLUDE_DIRS`
- Required for Python bindings (fastdb4py)

**Node.js (optional):**
- `node-addon-api` (node_modules/node-addon-api)
- `node-api-headers` (node_modules/node-api-headers)
- Required for Node.js bindings (fastdb4node) - USE_SWIG_NODE flag

**Go (optional):**
- Standard Go cgo support
- Required for Go bindings (fastdb4go) - USE_SWIG_GO flag

### C++ Standard Library

- `<vector>` - Vector containers (m_buffer, m_layers)
- `<string>` - String handling (layer names, field names)
- `<map>` - Used in build layer (field indexing)
- `<cstring>` - Memory operations (memcpy, strcmp)
- `<stdlib.h>` - Memory allocation

### Emscripten Support (WebAssembly)

```cmake
if(is_emscripten)
    set(BUILD_TYPE STATIC)
else()
    set(BUILD_TYPE SHARED)
endif()

# Emscripten linking via emcc:
# emcc -O2 -s WASM=0 -s ALLOW_MEMORY_GROWTH=1 
#      -s INITIAL_MEMORY=16MB -s TOTAL_STACK=5MB
#      --post-js fastdb4em_i.js
```

---

## 6. WebAssembly Binding Considerations

### Surface Area to Expose

**Must expose:**
- [x] `FastVectorDbBuild` (full builder API)
- [x] `FastVectorDb` (full reader API)
- [x] `FastVectorDbLayer` (layer iteration & metadata)
- [x] `FastVectorDbFeature` (feature access)
- [x] `MemoryStream` (serialization)
- [x] `chunk_data_t` (buffer handling with typed arrays)
- [ ] `GeometryReturn` (callback - complex in WASM)
- [ ] `WriteStream` (abstract - abstract in WASM)
- [ ] `TileBoxTake` / `FastVectorTileDb` (currently ignored in SWIG)

**Challenge Areas:**

1. **Callbacks (GeometryReturn)**
   - WASM lacks direct callback support
   - Solution: Return geometry data as serialized chunks, deserialize in JS

2. **Feature Iteration**
   - SWIG layers hide `next()` / `rewind()` complexity
   - WASM: Expose direct index-based access via `tryGetFeatureAt()`

3. **Memory Management**
   - Pimpl pattern requires FFI knowledge
   - Need explicit lifecycle management (new/delete → malloc/free in WASM)

4. **String Handling**
   - C++ strings need marshaling
   - UTF-8 assumptions required

5. **Typed Array Integration**
   - NumPy uses __array_interface__ (Python-specific)
   - WASM: Expose raw pointers + strides for JavaScript typed arrays
   - Must handle lifetime carefully (no auto-freeing)

6. **Zero-Copy Data Access**
   - `chunk_data_t` works well in WASM (raw pointer + size)
   - Layer column access needs stride/offset info in JS
   - Use WebAssembly Memory for direct access

### WASM-Specific Implementation

**Recommended approach (Emscripten):**
```javascript
// Get feature data as TypedArray (zero-copy)
const feature = layer.tryGetFeatureAt(0);
const fieldValue = feature.getFieldAsDouble(fieldIdx);

// Get geometry as ArrayBuffer
const geometryChunk = feature.getGeometryLikeChunk();
const buffer = new Uint8Array(Module.HEAP8, geometryChunk.pdata, geometryChunk.size);

// Iterate layer with direct indexing (no callbacks)
for (let i = 0; i < layer.getFeatureCount(); i++) {
    const feature = layer.tryGetFeatureAt(i);
    // ... process feature
}

// Column access with stride
const address = layer.tryGetFeatureAt(0).getAddress();
const stride = layer.getFeatureByteSize();
const typeSize = 8; // float64
const numFeatures = layer.getFeatureCount();
const offset = layer.getFieldOffset(fieldIdx);

const view = new Float64Array(
    Module.HEAP8.buffer,
    address + offset,
    numFeatures
);
// Must handle stride manually (row-major: address + i * stride)
```

### TypeScript Binding Layer

Key exposed types:
```typescript
interface IGeometry {
    type: GeometryType;
    coordinates: number[][];
}

interface IFeatureRef {
    layerId: number;
    featureId: number;
}

class Database {
    getLayerCount(): number;
    getLayer(idx: number): Layer;
    tryGetFeature(ref: IFeatureRef): Feature | null;
}

class Layer {
    getGeometryType(): string;
    getFieldCount(): number;
    getFeatureCount(): number;
    getFieldDefn(idx: number): FieldDef;
    getFieldOffset(idx: number): number;
    getFeatureByteSize(): number;
    tryGetFeatureAt(idx: number): Feature;
    getExtent(): [number, number, number, number];
}

class Feature {
    getFieldAsDouble(idx: number): number;
    getFieldAsInt(idx: number): number;
    getFieldAsString(idx: number): string;
    getFieldAsRef(idx: number): IFeatureRef;
    getGeometryLikeChunk(): Uint8Array;
    getAddress(): number;
    getFieldsAsDoubles(fieldIds: Uint32Array): Float64Array;
}

class MemoryStream {
    write(data: Uint8Array): void;
    getBytes(): Uint8Array;
    reset(): void;
}
```

---

## Summary Table

| Category | Count | Notes |
|----------|-------|-------|
| **Public Classes** | 9 | FastVectorDb, Layer, Feature, Build, Layer Build, MemoryStream, TileBox, TileDb, Geometry Return |
| **Pimpl Classes** | 8 | Each public class has nested Impl |
| **Core Enums** | 4 | Geometry type, coordinate format, field type, geometry format |
| **Core Structs** | 8+ | point2_t, aabbox_t, chunk_data_t, FastVectorDbFeatureRef, + format-specific |
| **Public Methods** | 60+ | Read operations, write operations, iteration, field access |
| **External Libraries** | 2 | gaiageo (WKT/WKB), Clipper2 (geometry) |
| **Source Files** | 14 | 6 implementation, 3 headers, 2 SWIG interface files, 3 tile-related |
| **Total C++ LoC** | 2,396 | Core implementation |

