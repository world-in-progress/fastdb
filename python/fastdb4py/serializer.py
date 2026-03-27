import struct
import numpy as np
import ctypes
from threading import Lock
from weakref import WeakKeyDictionary
from typing import Type, List, Dict, Any, Tuple, get_origin, get_args
from .feature import Feature, get_all_defns
from .feature._schema import get_class_schema as _get_unified_schema
from .type import OriginFieldType, U32, F64
from . import core

_NUMERIC_LIST_LAYER_PREFIX = "__fastser_list__|"
_BUFFER_LAYER_PREFIX = "__fastser_buf__|"
_BUFFER_REF_MAGIC = 0xBF
_CLASS_SCHEMA_CACHE_LOCK = Lock()
_CLASS_SCHEMA_CACHE: WeakKeyDictionary = WeakKeyDictionary()

# Mapping from numpy dtype to fastdb field type and short kind string
_NUMPY_DTYPE_TO_FDB = {
    np.dtype('float64'): (OriginFieldType.f64, "f64"),
    np.dtype('float32'): (OriginFieldType.f32, "f32"),
    np.dtype('uint32'):  (OriginFieldType.u32, "u32"),
    np.dtype('int32'):   (OriginFieldType.i32, "i32"),
    np.dtype('uint16'):  (OriginFieldType.u16, "u16"),
    np.dtype('uint8'):   (OriginFieldType.u8,  "u8"),
}

# Reverse mapping from kind string to numpy dtype
_KIND_TO_NUMPY_DTYPE = {
    "f64": np.dtype('<f8'), "f32": np.dtype('<f4'),
    "u32": np.dtype('<u4'), "i32": np.dtype('<i4'),
    "u16": np.dtype('<u2'), "u8":  np.dtype('<u1'),
}

# Kind string to struct format character (for fast list→bytes conversion)
_KIND_TO_STRUCT_CHAR = {"f64": "d", "f32": "f", "u32": "I", "i32": "i", "u16": "H", "u8": "B"}

# FastSerializer blob protocol (Mini spec)
#
# The serializer uses a hybrid layout:
# - Scalar fields are stored as fastdb columns.
# - Numeric lists List[U32]/List[F64] are stored in dedicated columnar auxiliary layers.
# - Other complex fields (list/ref/bytes/unknown) are stored in one geometry-like raw blob.
#
# Object reference encoding used by this serializer:
# - Ref = [layer_idx:u16][feature_idx:u32]
# - Null ref sentinel = [0xFFFF:u16][0xFFFFFFFF:u32]
#
# List encoding:
# - List header = [count:u32]
# - List[int] payload = count * i32
# - List[float] payload = count * f64
# - List[str] payload = repeated [byte_len:u32][utf8_bytes]
# - List[Feature] payload = count * Ref
#
# Field order contract:
# - Blob payload is written and read strictly by class field definition order.
# - Encoder and decoder must keep identical traversal order for compatibility.
#
# Numeric-list auxiliary layer schema:
# - owner_fid:u32, geometry_raw = packed numeric list payload
# - Layer name format: __fastser_list__|{ClassName}|{FieldName}|{u32|f64}

class FastSerializer:
    """
    High-performance serializer for fastdb Feature objects, supporting nested types and lists.
    Builds on top of fastdb's direct memory database capabilities.
    """
    
    @staticmethod
    def dumps(obj: Feature) -> bytes:
        if not isinstance(obj, Feature):
            raise TypeError("Only fastdb4py.Feature objects can be serialized.")
            
        ctx = _DumpContext()
        # Pass 1: Object graph traversal and ID assignment
        ctx.register(obj)
        
        # Pass 2: Build database
        db = core.WxDatabaseBuild()
        db.begin("")
        
        # Create layers for all discovered types
        layer_builders = {}
        for cls, l_idx in ctx.type_to_layer.items():
            lb = db.create_layer_begin(cls.__name__)
            layer_builders[l_idx] = lb
            
            # Define fields (add only scalar types as columns)
            lb.set_geometry_type(0, core.cfDefault, False) # 0 is unknown/any which allows blob storage

            schema = _get_class_schema(cls)
            defns = schema["defns"]
            for field_name, origin_type in defns:
                if origin_type in (OriginFieldType.u8, OriginFieldType.u16, OriginFieldType.u32, 
                                   OriginFieldType.i32, OriginFieldType.f32, OriginFieldType.f64,
                                   OriginFieldType.str, OriginFieldType.wstr):
                     lb.add_field(field_name, origin_type.value)

        # Pre-scan for buffer-eligible fields: numpy arrays AND numeric lists
        buf_layers = {}  # id(array) -> (buf_idx, kind, shape) for ndarray dedup
        buf_field_refs = {}  # (id(obj), fn) -> (buf_idx, kind, shape)
        buf_layer_builders = []
        buf_array_cache = {}  # id(array) -> np.ndarray (keep alive)
        _next_buf_idx = [0]

        def _create_buf_layer(cls_name, fn, raw_bytes, kind, shape):
            shape_str = "x".join(str(s) for s in shape)
            layer_name = f"{_BUFFER_LAYER_PREFIX}{cls_name}|{fn}|{kind}|{shape_str}"
            buf_lb = db.create_layer_begin(layer_name)
            buf_lb.set_geometry_type(0, core.cfDefault, False)
            buf_lb.add_feature_begin()
            buf_lb.set_geometry_raw(raw_bytes)
            buf_lb.add_feature_end()
            idx = _next_buf_idx[0]
            _next_buf_idx[0] += 1
            info = (idx, kind, shape)
            buf_layer_builders.append(buf_lb)
            return info

        for obj_wrapper in ctx.objects:
            obj = obj_wrapper.obj
            schema = _get_class_schema(obj.__class__)
            defns = schema["defns"]
            numeric_field_kinds = schema["numeric_field_kinds"]
            for fn, ft in defns:
                val = getattr(obj, fn)
                if val is None:
                    continue
                # numpy ndarray with supported dtype
                if isinstance(val, np.ndarray) and val.dtype in _NUMPY_DTYPE_TO_FDB:
                    arr_id = id(val)
                    if arr_id not in buf_layers:
                        fdb_ft, kind = _NUMPY_DTYPE_TO_FDB[val.dtype]
                        raw = np.ascontiguousarray(val).ravel().tobytes()
                        buf_layers[arr_id] = _create_buf_layer(obj.__class__.__name__, fn, raw, kind, val.shape)
                        buf_array_cache[arr_id] = val
                    buf_field_refs[(id(obj), fn)] = buf_layers[arr_id]
                # Numeric list → struct.pack for fast list→bytes (skip numpy)
                elif isinstance(val, list) and ft == OriginFieldType.list:
                    numeric_kind = numeric_field_kinds.get(fn)
                    if numeric_kind and len(val) > 0:
                        fmt_char = _KIND_TO_STRUCT_CHAR[numeric_kind]
                        try:
                            raw = struct.pack(f'<{len(val)}{fmt_char}', *val)
                        except struct.error as e:
                            raise OverflowError(str(e)) from e
                        shape = (len(val),)
                        info = _create_buf_layer(obj.__class__.__name__, fn, raw, numeric_kind, shape)
                        buf_field_refs[(id(obj), fn)] = info
        
        # Write all objects ordered by layer
        # Skip sort for single-layer case (most common for simple Features)
        if len(ctx.type_to_layer) <= 1:
            write_order = ctx.objects
        else:
            write_order = sorted(ctx.objects, key=lambda x: (x.layer_idx, x.feature_idx))
        
        current_layer_idx = -1
        current_lb = None
        
        for obj_wrapper in write_order:
            obj = obj_wrapper.obj
            l_idx = obj_wrapper.layer_idx
            
            # Switch Layer
            if l_idx != current_layer_idx:
                current_layer_idx = l_idx
                current_lb = layer_builders[l_idx]
            
            lb = current_lb
            lb.add_feature_begin()
            
            schema = _get_class_schema(obj.__class__)
            defns = schema["defns"]
            hints = schema["hints"]
            numeric_field_kinds = schema["numeric_field_kinds"]
            has_blob = schema["has_blob_fields"] or buf_field_refs
            
            # Only create blob buffer when needed
            blob_buffer = bytearray() if has_blob else None
            
            for idx, (fn, ft) in enumerate(defns):
                val = getattr(obj, fn)
                numeric_kind = numeric_field_kinds.get(fn) if ft == OriginFieldType.list else None

                # Buffer layer reference (ndarray or numeric list)
                buf_ref = buf_field_refs.get((id(obj), fn))
                if buf_ref is not None:
                    _pack_buffer_ref(blob_buffer, buf_ref[0], buf_ref[2])
                    continue

                # Empty or None numeric list → null buffer ref
                if numeric_kind is not None:
                    _pack_buffer_ref(blob_buffer, 0xFFFF, (0,))
                    continue
                
                # Strategy: Scalar -> Column, Complex -> Blob
                if ft in (OriginFieldType.list, OriginFieldType.unknown):
                    if isinstance(val, list):
                        _pack_list(blob_buffer, val, hints.get(fn, Any), ctx)
                    elif isinstance(val, Feature):
                        temp_buf = bytearray()
                        temp_buf.extend(struct.pack('<I', 1))
                        _pack_feature_ref(temp_buf, val, ctx)
                        blob_buffer.extend(temp_buf)
                    else:
                        blob_buffer.extend(struct.pack('<I', 0)) 
                elif ft == OriginFieldType.bytes:
                     if isinstance(val, (bytes, bytearray)):
                         blob_buffer.extend(struct.pack('<I', len(val)))
                         blob_buffer.extend(val)
                     else:
                         blob_buffer.extend(struct.pack('<I', 0))
                elif ft == OriginFieldType.ref:
                    _pack_feature_ref(blob_buffer, val, ctx)
                else:
                    # Scalar type: write to column
                    db_idx = ctx.get_db_field_index(obj.__class__, idx)
                    if db_idx != -1 and val is not None:
                        if ft == OriginFieldType.str:
                            lb.set_field_cstring(db_idx, val)
                        elif ft == OriginFieldType.wstr:
                            lb.set_field_wstring(db_idx, val)
                        elif ft in (OriginFieldType.u8, OriginFieldType.u8n):
                             lb.set_field(db_idx, int(val))
                        else:
                             lb.set_field(db_idx, val)
            
            # Write Blob to Geometry field
            if blob_buffer:
                lb.set_geometry_raw(bytes(blob_buffer))

            lb.add_feature_end()
            
        # Finish build
        mem = core.WxMemoryStream()
        db.post(mem)
        
        # Return binary data
        return mem.data().as_array(np.uint8).tobytes()

    @staticmethod
    def loads(data: bytes, root_type: Type[Feature]) -> Feature:
        # Zero-copy load database
        db = core.WxDatabase.load_xbuffer(data)
        if db.get_layer_count() == 0:
            return None
            
        ctx = _LoadContext(db)
        
        # Discover and register related types
        _discover_types(root_type, ctx.type_map)
        
        # Root object is always at Layer 0, Feature 0
        root_layer = db.get_layer(0)
        if root_layer.get_feature_count() == 0:
            return None
            
        return ctx.get_object(0, 0, root_type)

# --- Internal Helpers ---

class _DumpContext:
    def __init__(self):
        self.objects = [] # List[_ObjectWrapper]
        self.obj_to_id = {} # id(obj) -> (layer_idx, feature_idx)
        self.type_to_layer = {} # Type -> layer_idx
        self.layer_counters = {} # layer_idx -> feature_count

    def get_hints(self, cls):
        return _get_class_schema(cls)["hints"]

    def get_db_field_index(self, cls, schema_idx):
        return _get_class_schema(cls)["db_field_index_by_schema"].get(schema_idx, -1)

    def register(self, obj):
        if id(obj) in self.obj_to_id:
            return
        cls = obj.__class__
        if cls not in self.type_to_layer:
            l_idx = len(self.type_to_layer)
            self.type_to_layer[cls] = l_idx
            self.layer_counters[l_idx] = 0
            
        l_idx = self.type_to_layer[cls]
        f_idx = self.layer_counters[l_idx]
        self.layer_counters[l_idx] += 1
        
        self.obj_to_id[id(obj)] = (l_idx, f_idx)
        self.objects.append(_ObjectWrapper(obj, l_idx, f_idx))
        
        # Only traverse fields that might contain Feature refs
        schema = _get_class_schema(cls)
        ref_fields = schema["ref_traversal_fields"]
        if not ref_fields:
            return
        
        for fn, type_hint, kind in ref_fields:
            val = getattr(obj, fn)
            if val is None:
                continue

            if kind == "ref" and isinstance(val, Feature):
                self.register(val)
            elif isinstance(val, list):
                args = get_args(type_hint) if type_hint else None
                inner = args[0] if args else None
                
                if inner and hasattr(inner, '__mro__') and issubclass(inner, Feature):
                     for item in val:
                         if isinstance(item, Feature):
                             self.register(item)
                elif val and isinstance(val[0], Feature):
                     for item in val:
                         if isinstance(item, Feature):
                             self.register(item)
            elif isinstance(val, Feature):
                self.register(val)

    def get_ref(self, obj):
        return self.obj_to_id.get(id(obj))

class _ObjectWrapper:
    def __init__(self, obj, l_idx, f_idx):
        self.obj = obj
        self.layer_idx = l_idx
        self.feature_idx = f_idx

class _LoadContext:
    def __init__(self, db):
        self.db = db
        self.obj_cache = {} # (layer_idx, feature_idx) -> obj
        self.type_map = {} # class_name -> Type
        self._numeric_list_values = None
        self._buffer_layers = None
        self._uses_aux_numeric = None

    @property
    def uses_aux_numeric(self):
        """Check if database uses old-style __fastser_list__ auxiliary layers."""
        if self._uses_aux_numeric is None:
            self._uses_aux_numeric = False
            for i in range(self.db.get_layer_count()):
                name = str(self.db.get_layer(i).name())
                if name.startswith(_NUMERIC_LIST_LAYER_PREFIX):
                    self._uses_aux_numeric = True
                    break
        return self._uses_aux_numeric

    @property
    def numeric_list_values(self):
        if self._numeric_list_values is None:
            self._numeric_list_values = _load_numeric_list_values(self.db)
        return self._numeric_list_values

    @property
    def buffer_layers(self):
        if self._buffer_layers is None:
            self._buffer_layers = _load_buffer_layers(self.db)
        return self._buffer_layers

    def get_object(self, l_idx, f_idx, expected_type):
        key = (l_idx, f_idx)
        if key in self.obj_cache:
            return self.obj_cache[key]
            
        layer = self.db.get_layer(l_idx)
        if layer is None:
             # Fallback: maybe layer name mapping?
             # But here use index.
             # If layer is None, something is critically wrong.
             return None

        feature_data = layer.tryGetFeature(f_idx) 
        if not feature_data: return None
        
        cls_name = layer.name()
        cls = self.type_map.get(cls_name, expected_type)
        
        obj = cls()
        self.obj_cache[key] = obj # Cache to solve cyclic references
        
        # Fill data
        obj._origin = feature_data
        obj._db = self.db
        
        schema = _get_class_schema(cls)
        defns = schema["defns"]
        hints = schema["hints"]
        numeric_field_kinds = schema["numeric_field_kinds"]
        
        # Read Blob data
        blob = feature_data.get_geometry_like_chunk()
        blob_view = None
        if blob.size > 0:
             # Safe access to memory view
             addr = int(blob.pdata) if hasattr(blob.pdata, '__int__') else blob.pdata
             if not isinstance(addr, int): # Fallback
                  try: addr = int(addr)
                  except: pass
             BlobType = ctypes.c_ubyte * blob.size
             blob_array = BlobType.from_address(addr)
             blob_view = memoryview(blob_array)

        curr_blob_offset = 0
        db_idx_map = schema["db_field_index_by_schema"]
        
        for idx, (fn, ft) in enumerate(defns):
            numeric_kind = numeric_field_kinds.get(fn) if ft == OriginFieldType.list else None

            # Numeric list fields (List[F64], List[U32], List[I32])
            if numeric_kind is not None:
                if self.uses_aux_numeric:
                    # Old format: load from __fastser_list__ auxiliary layers
                    obj._cache[fn] = self.numeric_list_values.get((cls.__name__, fn, f_idx), [])
                else:
                    # New format: read buffer ref from blob
                    if blob_view and curr_blob_offset < len(blob_view):
                        magic_byte = struct.unpack_from('B', blob_view, curr_blob_offset)[0]
                        if magic_byte == _BUFFER_REF_MAGIC:
                            result = _unpack_buffer_ref(blob_view, curr_blob_offset)
                            if result is not None:
                                buf_idx, shape, new_offset = result
                                if buf_idx == 0xFFFF:
                                    # Null ref → empty array
                                    dtype = _KIND_TO_NUMPY_DTYPE.get(numeric_kind, np.dtype('<f8'))
                                    obj._cache[fn] = np.array([], dtype=dtype)
                                else:
                                    arr = self.buffer_layers.get(buf_idx)
                                    obj._cache[fn] = arr if arr is not None else np.array([], dtype=_KIND_TO_NUMPY_DTYPE.get(numeric_kind, np.dtype('<f8')))
                                curr_blob_offset = new_offset
                            else:
                                obj._cache[fn] = []
                        else:
                            obj._cache[fn] = []
                    else:
                        obj._cache[fn] = []
                continue

            # Check for buffer layer reference (only for blob-consuming field types)
            if ft in (OriginFieldType.list, OriginFieldType.unknown, OriginFieldType.bytes, OriginFieldType.ref):
                if blob_view and curr_blob_offset < len(blob_view):
                    magic_byte = struct.unpack_from('B', blob_view, curr_blob_offset)[0]
                    if magic_byte == _BUFFER_REF_MAGIC:
                        result = _unpack_buffer_ref(blob_view, curr_blob_offset)
                        if result is not None:
                            buf_idx, shape, new_offset = result
                            arr = self.buffer_layers.get(buf_idx)
                            if arr is not None:
                                obj._cache[fn] = arr
                            curr_blob_offset = new_offset
                            continue

            # Recover complex types from Blob
            if ft in (OriginFieldType.list, OriginFieldType.unknown):
                if blob_view:
                    val, new_offset = _unpack_list(blob_view, curr_blob_offset, hints.get(fn, Any), self)
                    obj._cache[fn] = val
                    curr_blob_offset = new_offset
            elif ft == OriginFieldType.bytes:
                if blob_view:
                    cnt = struct.unpack_from('<I', blob_view, curr_blob_offset)[0]
                    curr_blob_offset += 4
                    val = bytes(blob_view[curr_blob_offset:curr_blob_offset+cnt])
                    obj._cache[fn] = val
                    curr_blob_offset += cnt
            elif ft == OriginFieldType.ref:
                if blob_view:
                    l_idx_ref, f_idx_ref = struct.unpack_from('<HI', blob_view, curr_blob_offset)
                    curr_blob_offset += 6
                    if l_idx_ref != 0xFFFF:
                        ref_type = hints.get(fn, Feature)
                        obj._cache[fn] = self.get_object(l_idx_ref, f_idx_ref, ref_type)
                    else:
                        obj._cache[fn] = None
            else:
                # Recover scalar from Column (use pre-fetched db_idx_map)
                db_idx = db_idx_map.get(idx, -1)
                if db_idx != -1:
                    if ft in (OriginFieldType.u8, OriginFieldType.u16, OriginFieldType.u32, OriginFieldType.i32):
                        obj._cache[fn] = feature_data.get_field_as_int(db_idx)
                    elif ft in (OriginFieldType.f32, OriginFieldType.f64):
                        obj._cache[fn] = feature_data.get_field_as_float(db_idx)
                    elif ft == OriginFieldType.str:
                        obj._cache[fn] = feature_data.get_field_as_string(db_idx)
                    elif ft == OriginFieldType.wstr:
                        obj._cache[fn] = feature_data.get_field_as_wstring(db_idx)

        return obj

def _pack_feature_ref(buffer, val, ctx):
    if val is not None:
        ref_info = ctx.get_ref(val)
        if ref_info:
            buffer.extend(struct.pack('<HI', ref_info[0], ref_info[1]))
            return
    # Null reference
    buffer.extend(struct.pack('<HI', 0xFFFF, 0xFFFFFFFF))

# --- Buffer layer helpers ---

def _pack_buffer_ref(buffer, buf_layer_idx, shape):
    """Encode a buffer reference in the blob: magic(1) + layer(2) + ndim(1) + shape(3×4=12) = 16 bytes."""
    ndim = len(shape)
    dims = list(shape) + [0] * (3 - ndim)  # pad to 3 dims
    buffer.extend(struct.pack('<BBHIII', _BUFFER_REF_MAGIC, ndim, buf_layer_idx, dims[0], dims[1], dims[2]))

def _unpack_buffer_ref(view, offset):
    """Decode buffer reference. Returns (buf_layer_idx, shape, new_offset) or None if not a buf ref."""
    if offset + 16 > len(view):
        return None
    magic = struct.unpack_from('B', view, offset)[0]
    if magic != _BUFFER_REF_MAGIC:
        return None
    ndim, layer_idx, d0, d1, d2 = struct.unpack_from('<BHIII', view, offset + 1)
    dims = [d0, d1, d2][:ndim]
    shape = tuple(dims)
    return layer_idx, shape, offset + 16

def _parse_buffer_layer_name(layer_name):
    """Parse __fastser_buf__|ClassName|FieldName|kind|shape → (class_name, field_name, kind, shape) or None."""
    if not isinstance(layer_name, str):
        try:
            layer_name = layer_name.decode('utf-8')
        except Exception:
            return None
    if not layer_name.startswith(_BUFFER_LAYER_PREFIX):
        return None
    body = layer_name[len(_BUFFER_LAYER_PREFIX):]
    parts = body.split('|', 3)
    if len(parts) != 4:
        return None
    class_name, field_name, kind, shape_str = parts
    if kind not in _KIND_TO_NUMPY_DTYPE:
        return None
    shape = tuple(int(s) for s in shape_str.split('x') if s)
    return class_name, field_name, kind, shape

def _load_buffer_layers(db):
    """Pre-scan all __fastser_buf__ layers and return a dict mapping buffer_layer_index → numpy array."""
    buf_data = {}
    buf_idx = 0
    for layer_idx in range(db.get_layer_count()):
        layer = db.get_layer(layer_idx)
        if layer is None:
            continue
        parsed = _parse_buffer_layer_name(layer.name())
        if parsed is None:
            continue
        class_name, field_name, kind, shape = parsed
        dtype = _KIND_TO_NUMPY_DTYPE[kind]
        # Read the geometry blob from the first (only) feature
        if layer.get_feature_count() > 0:
            row = layer.tryGetFeature(0)
            if row:
                chunk = row.get_geometry_like_chunk()
                if chunk.size > 0:
                    addr = int(chunk.pdata) if hasattr(chunk.pdata, '__int__') else chunk.pdata
                    if not isinstance(addr, int):
                        addr = int(addr)
                    BlobType = ctypes.c_ubyte * chunk.size
                    blob_array = BlobType.from_address(addr)
                    flat = np.frombuffer(blob_array, dtype=dtype).copy()  # copy to own memory
                    arr = flat.reshape(shape)
                    buf_data[buf_idx] = arr
        buf_idx += 1
    return buf_data

def _numeric_list_kind_from_hint(type_hint):
    if type_hint is None:
        return None

    origin = get_origin(type_hint)
    if origin is not list:
        return None

    args = get_args(type_hint)
    if not args:
        return None

    inner = args[0]

    if inner is U32:
        return "u32"
    if inner is F64 or inner is float:
        return "f64"
    if inner is int:
        return "i32"

    if isinstance(inner, str):
        if inner == "U32":
            return "u32"
        if inner == "F64":
            return "f64"
        if inner in ("I32", "int"):
            return "i32"

    if hasattr(inner, '__forward_arg__'):
        arg = inner.__forward_arg__
        if arg == "U32":
            return "u32"
        if arg == "F64":
            return "f64"
        if arg in ("I32", "int"):
            return "i32"

    return None

def _get_numeric_list_fields(hints):
    fields = []
    for field_name, hint in hints.items():
        kind = _numeric_list_kind_from_hint(hint)
        if kind is not None:
            fields.append((field_name, kind))
    return fields

def _make_numeric_list_layer_name(class_name, field_name, kind):
    return f"{_NUMERIC_LIST_LAYER_PREFIX}{class_name}|{field_name}|{kind}"

def _parse_numeric_list_layer_name(layer_name):
    if not isinstance(layer_name, str):
        try:
            layer_name = layer_name.decode('utf-8')
        except Exception:
            return None

    if not layer_name.startswith(_NUMERIC_LIST_LAYER_PREFIX):
        return None

    body = layer_name[len(_NUMERIC_LIST_LAYER_PREFIX):]
    parts = body.split('|', 2)
    if len(parts) != 3:
        return None

    class_name, field_name, kind = parts
    if kind not in ("u32", "f64", "i32"):
        return None
    return class_name, field_name, kind

def _write_numeric_list_chunk(aux_lb, owner_fid, values, kind):
    aux_lb.add_feature_begin()
    aux_lb.set_field(0, int(owner_fid))

    if not values:
        packed = b""
    elif kind == "u32":
        arr = np.array(values, dtype=np.uint32)
        if np.any(arr > 0xFFFFFFFF) or np.any(arr < 0):
            raise ValueError("List[U32] item out of range")
        packed = arr.tobytes()
    elif kind == "i32":
        arr = np.array(values, dtype=np.int32)
        packed = arr.tobytes()
    else:
        arr = np.array(values, dtype=np.float64)
        packed = arr.tobytes()

    aux_lb.set_geometry_raw(packed)
    aux_lb.add_feature_end()

def _load_numeric_list_values(db):
    out = {}

    for layer_idx in range(db.get_layer_count()):
        layer = db.get_layer(layer_idx)
        if layer is None:
            continue

        parsed = _parse_numeric_list_layer_name(layer.name())
        if parsed is None:
            continue

        class_name, field_name, kind = parsed

        for row_idx in range(layer.get_feature_count()):
            row = layer.tryGetFeature(row_idx)
            if not row:
                continue

            owner_fid = int(row.get_field_as_int(0))
            chunk = row.get_geometry_like_chunk()
            values = _decode_numeric_list_chunk(chunk, kind)
            out[(class_name, field_name, owner_fid)] = values

    return out

def _decode_numeric_list_chunk(chunk, kind):
    if chunk.size <= 0:
        return []

    addr = int(chunk.pdata) if hasattr(chunk.pdata, '__int__') else chunk.pdata
    if not isinstance(addr, int):
        addr = int(addr)

    BlobType = ctypes.c_ubyte * chunk.size
    blob_array = BlobType.from_address(addr)

    if kind == "u32":
        return np.frombuffer(blob_array, dtype=np.dtype('<u4')).tolist()
    if kind == "i32":
        return np.frombuffer(blob_array, dtype=np.dtype('<i4')).tolist()
    return np.frombuffer(blob_array, dtype=np.dtype('<f8')).tolist()

def _pack_list(buffer, lst, type_hint, ctx):
    count = len(lst)
    buffer.extend(struct.pack('<I', count))
    
    if count == 0: return

    
    args = get_args(type_hint) if type_hint else None
    inner = args[0] if args else Any
    
    # Simple type inference if hint is Any or string (unresolved ref)
    if (inner is Any or isinstance(inner, str) or hasattr(inner, '__forward_arg__')) and len(lst) > 0:
        first = lst[0]
        if isinstance(first, int): inner = int
        elif isinstance(first, float): inner = float
        elif isinstance(first, str): inner = str
        elif isinstance(first, Feature): inner = Feature

    if inner == int:
        buffer.extend(np.array(lst, dtype=np.dtype('<i4')).tobytes())
    elif inner == float:
        buffer.extend(np.array(lst, dtype=np.dtype('<f8')).tobytes())
    elif inner == str:
        for item in lst:
            encoded = item.encode('utf-8')
            buffer.extend(struct.pack('<I', len(encoded)))
            buffer.extend(encoded)
    # Feature or subclass
    elif (hasattr(inner, '__mro__') and issubclass(inner, Feature)) or inner is Feature:
        for item in lst:
            _pack_feature_ref(buffer, item, ctx)

def _unpack_list(view, offset, type_hint, ctx):
    count = struct.unpack_from('<I', view, offset)[0]
    offset += 4
    
    lst = []
    args = get_args(type_hint) if type_hint else None
    inner = args[0] if args else Any
    
    # Attempt to resolve forward reference if inner is a string or ForwardRef-like
    if isinstance(inner, str):
        inner = ctx.type_map.get(inner, inner)
    elif hasattr(inner, '__forward_arg__'):
        inner = ctx.type_map.get(inner.__forward_arg__, inner)

    # If list is empty, type doesn't matter
    if count == 0:
        return lst, offset

    if inner == int:
        sz = count * 4
        lst = np.frombuffer(bytes(view[offset:offset + sz]), dtype=np.dtype('<i4')).tolist()
        offset += sz
    elif inner == float:
        sz = count * 8
        lst = np.frombuffer(bytes(view[offset:offset + sz]), dtype=np.dtype('<f8')).tolist()
        offset += sz
    elif inner == str:
        for _ in range(count):
            byte_len = struct.unpack_from('<I', view, offset)[0]
            offset += 4
            raw = bytes(view[offset:offset + byte_len])
            offset += byte_len
            lst.append(raw.decode('utf-8'))
    # Check if inner is a Feature subclass
    elif (hasattr(inner, '__mro__') and issubclass(inner, Feature)) or \
         (isinstance(inner, type) and issubclass(inner, Feature)):
        for _ in range(count):
            l_idx, f_idx = struct.unpack_from('<HI', view, offset)
            offset += 6
            if l_idx == 0xFFFF:
                lst.append(None)
            else:
                lst.append(ctx.get_object(l_idx, f_idx, inner)) 
    else:
        # Fallback: Treat as Feature references if unresolved
        # Heuristic: try to unpack as feature refs (6 bytes each)
        # This assumes lists of unrecognized types are lists of Features.
        try:
             for _ in range(count):
                 if offset + 6 > len(view):
                     # Not feature refs, or out of bounds. 
                     # Should we try unpacking as ints?
                     # But we already did 'inner == int' check.
                     break
                 l_idx, f_idx = struct.unpack_from('<HI', view, offset)
                 offset += 6
                 # We don't know the type, so we can't create strong typed object easily
                 # But we can try to look up generic Feature or if we have layer info?
                 # ctx.get_object needs expected_type.
                 # If we pass Feature, it works?
                 if l_idx == 0xFFFF:
                     lst.append(None)
                 else:
                     # Using Feature base class
                     lst.append(ctx.get_object(l_idx, f_idx, Feature))
        except:
             pass
    
    return lst, offset

def _get_db_field_index_for_load(cls, schema_idx):
    return _get_class_schema(cls)["db_field_index_by_schema"].get(schema_idx, -1)

def _discover_types(cls, type_map):
    if cls.__name__ in type_map:
        return
    type_map[cls.__name__] = cls
    try:
        hints = _get_class_schema(cls)["hints"]
    except Exception:
        hints = {}
        
    for t in hints.values():
        origin = get_origin(t)
        args = get_args(t)
        base = t if not origin else origin
        
        # Handle ForwardRef or string
        if isinstance(base, str):
            # We can't easily resolve string to type here without context.
            # But FastSerialier relies on type_map being populated.
            # If the user passed root_type, we hope related types are discoverable.
            pass
        elif hasattr(base, '__forward_arg__'):
             pass
        else:
            try:
                if issubclass(base, Feature):
                     _discover_types(base, type_map)
            except: pass
        
        if origin is list and args:
             inner = args[0]
             # Handle ForwardRef in List
             if isinstance(inner, str):
                 pass
             elif hasattr(inner, '__forward_arg__'):
                 pass
             else:
                 try:
                     if issubclass(inner, Feature):
                         _discover_types(inner, type_map)
                 except: pass

def _get_class_schema(cls):
    with _CLASS_SCHEMA_CACHE_LOCK:
        schema = _CLASS_SCHEMA_CACHE.get(cls)
        if schema is not None:
            return schema

        # Read shared data from unified ClassSchema — no recomputation of get_type_hints().
        base = _get_unified_schema(cls)
        hints = base.hints
        defns = base.ordered_defns

        numeric_field_kinds = {}
        numeric_fields = []
        for field_name, hint in hints.items():
            kind = _numeric_list_kind_from_hint(hint)
            if kind is not None:
                numeric_field_kinds[field_name] = kind
                numeric_fields.append((field_name, kind))

        db_field_index_by_schema = {}
        curr_db_idx = 0
        for i, (_, ft) in enumerate(defns):
            if ft in (
                OriginFieldType.u8,
                OriginFieldType.u16,
                OriginFieldType.u32,
                OriginFieldType.i32,
                OriginFieldType.f32,
                OriginFieldType.f64,
                OriginFieldType.str,
                OriginFieldType.wstr,
            ):
                db_field_index_by_schema[i] = curr_db_idx
                curr_db_idx += 1
            else:
                db_field_index_by_schema[i] = -1

        has_blob_fields = any(
            ft in (OriginFieldType.list, OriginFieldType.unknown, OriginFieldType.bytes, OriginFieldType.ref)
            for _, ft in defns
        )

        # Pre-compute fields that might contain Feature refs (for fast registration)
        ref_traversal_fields = []
        for fn, ft in defns:
            if ft == OriginFieldType.ref:
                ref_traversal_fields.append((fn, hints.get(fn), "ref"))
            elif ft in (OriginFieldType.list, OriginFieldType.unknown):
                # Skip numeric lists (List[F64], List[U32], List[I32]) - they never contain Features
                if fn not in numeric_field_kinds:
                    ref_traversal_fields.append((fn, hints.get(fn), "list_or_unknown"))

        schema = {
            "hints": hints,
            "defns": defns,
            "numeric_field_kinds": numeric_field_kinds,
            "numeric_fields": numeric_fields,
            "db_field_index_by_schema": db_field_index_by_schema,
            "has_blob_fields": has_blob_fields,
            "ref_traversal_fields": ref_traversal_fields,
        }
        _CLASS_SCHEMA_CACHE[cls] = schema
        return schema
