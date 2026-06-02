%module fastdb4py
%{
    #define SWIG_FILE_WITH_INIT
    #include "fastdb.h"
    #include "fastdb-geometry-utils.h"
    using namespace wx;
%}

// Tell SWIG to ignore fastdb_api when parsing headers
#define fastdb_api

// Release the GIL for pure C++ operations that do not touch Python objects.
// This benefits both standard Python (better multi-threaded throughput) and
// free-threaded Python 3.13+ (PEP 703).
%feature("threadallow") wx::FastVectorDb::load;
%feature("threadallow") wx::FastVectorDbBuild::post;
%feature("threadallow") wx::FastVectorDbBuild::byteLength;
%feature("threadallow") wx::FastVectorDbBuild::tableBufferBytes;
%feature("threadallow") wx::FastVectorDbBuild::postToFinalBacking;
%feature("threadallow") wx::FastVectorDbBuild::save;
%feature("threadallow") wx::FastVectorDbBuild::truncate;
%feature("threadallow") wx::FastVectorDbFeature::getFieldsAsDoubles;
%feature("threadallow") wx::FastVectorDbFeature::setFieldsFromDoubles;

%include "typemaps.i"
%include "cstring.i"
%include "cpointer.i"
%include "carrays.i"
%include "numpy.i"  // 包含 numpy.i 支持
%init %{
    import_array(); // 初始化 NumPy C-API
    // Free-threading support (PEP 703) is handled by SWIG 4.4+ via the -nogil flag.
    // When built on free-threaded Python, SWIG automatically declares Py_MOD_GIL_NOT_USED.
%}

//%array_class(point2_t, LineString);

%typemap(in) const wchar_t * (wchar_t *temp = NULL) {
    if ($input == Py_None) {
        $1 = NULL;
    } else if (PyUnicode_Check($input)) {
        temp = PyUnicode_AsWideCharString($input, NULL);
        if (temp == NULL) {
            SWIG_fail;
        }
        $1 = temp;
    } else {
        SWIG_exception_fail(SWIG_TypeError, "Expected a str object for wchar_t input");
    }
}

%typemap(freearg) const wchar_t * {
    if (temp$argnum != NULL) {
        PyMem_Free(temp$argnum);
    }
}

%typemap(out) const uchar_t * {
    if ($1 == NULL) {
        Py_INCREF(Py_None);
        $result = Py_None;
    } else {
        size_t len = 0;
        while ($1[len] != 0) {
            ++len;
        }
        $result = PyUnicode_FromKindAndData(PyUnicode_2BYTE_KIND, $1, (Py_ssize_t)len);
        if ($result == NULL) {
            SWIG_fail;
        }
    }
}

%typemap(out) uchar_t * {
    if ($1 == NULL) {
        Py_INCREF(Py_None);
        $result = Py_None;
    } else {
        size_t len = 0;
        while ($1[len] != 0) {
            ++len;
        }
        $result = PyUnicode_FromKindAndData(PyUnicode_2BYTE_KIND, $1, (Py_ssize_t)len);
        if ($result == NULL) {
            SWIG_fail;
        }
    }
}

%typemap(in)(const u8* data,size_t size) (Py_buffer view) {
    if (PyObject_GetBuffer($input, &view, PyBUF_SIMPLE) < 0) {
        SWIG_exception_fail(SWIG_TypeError, "Expected a bytes-like object");
    }
    $1 = (u8 *)view.buf;
    $2 = (size_t)view.len;
}

%typemap(freearg)(const u8* data,size_t size) {
    if (view$argnum.obj) {
        PyBuffer_Release(&view$argnum);
    }
}

%typemap(default) (const u8* data,size_t size) {};
// 在你的 .i 文件中对目标函数应用此类型映射
%typemap(in) (void* pdata, size_t size) (Py_buffer view) {
    // 检查输入对象是否支持缓冲区接口
    if (PyObject_CheckBuffer($input) == 0) {
        SWIG_exception_fail(SWIG_TypeError, "Expected a bufferable object (e.g., bytes, bytearray)");
    }
    // 获取缓冲区视图，PyBUF_SIMPLE 请求一个简单的连续字节缓冲区
    if (PyObject_GetBuffer($input, &view, PyBUF_SIMPLE) != 0) {
        SWIG_exception_fail(SWIG_TypeError, "Failed to get buffer from object.");
    }
    // 将获取到的缓冲区地址和长度赋给参数
    $1 = view.buf;  // void* pdata 指向数据
    $2 = view.len;  // size_t size 设置数据长度
}

// 定义 freearg 类型映射，在函数调用后释放缓冲区
%typemap(freearg) (void* pdata, size_t size) {
    if (view$argnum.obj) {
        PyBuffer_Release(&view$argnum);
    }
}

// Typemap for set_field_list_numeric: accepts any buffer object; nbytes = byte length of buffer
%typemap(in) (const void* data, unsigned nbytes) (Py_buffer view) {
    if (PyObject_GetBuffer($input, &view, PyBUF_SIMPLE) < 0) {
        SWIG_exception_fail(SWIG_TypeError, "Expected a buffer (bytes, bytearray, memoryview, ndarray)");
    }
    $1 = view.buf;
    $2 = (unsigned)(view.len);
}
%typemap(freearg) (const void* data, unsigned nbytes) {
    if (view$argnum.obj) {
        PyBuffer_Release(&view$argnum);
    }
}

%typemap(in) (const u32* offsets, unsigned n_offsets) (Py_buffer view) {
    if (PyObject_GetBuffer($input, &view, PyBUF_SIMPLE) < 0) {
        SWIG_exception_fail(SWIG_TypeError, "Expected a buffer for uint32 offsets");
    }
    if (view.len % sizeof(u32) != 0) {
        PyBuffer_Release(&view);
        SWIG_exception_fail(SWIG_ValueError, "Offset buffer size must be a multiple of 4 bytes");
    }
    $1 = (u32*)view.buf;
    $2 = (unsigned)(view.len / sizeof(u32));
}
%typemap(freearg) (const u32* offsets, unsigned n_offsets) {
    if (view$argnum.obj) {
        PyBuffer_Release(&view$argnum);
    }
}

%typemap(in) (const u8* data, u64 nbytes) (Py_buffer view) {
    if (PyObject_GetBuffer($input, &view, PyBUF_SIMPLE) < 0) {
        SWIG_exception_fail(SWIG_TypeError, "Expected a buffer for UTF-8 data bytes");
    }
    $1 = (u8*)view.buf;
    $2 = (u64)view.len;
}
%typemap(freearg) (const u8* data, u64 nbytes) {
    if (view$argnum.obj) {
        PyBuffer_Release(&view$argnum);
    }
}

// Exception handling for copy_to_buffer
%typemap(out) int copy_to_buffer {
    if ($1 < 0) {
        // error already set by PyErr_SetString/PyErr_Format
        SWIG_fail;
    }
    // success - return None
    $result = SWIG_Py_Void();
}

%apply  double* OUTPUT {double *vmin, double *vmax,double* minx,double* miny,double* maxx,double* maxy};
%apply  size_t* OUTPUT {size_t* ft};

%ignore wx::GeometryReturn;
// %ignore wx::WriteStream;
%ignore setGeometry;
%ignore wx::TileBoxTake;
%ignore wx::FastVectorTileDb;
%ignore wx::FastVectorDbLayerBuild::FastVectorDbLayerBuild(FastVectorDbBuild* db,const char* name);
%ignore wx::FastVectorDbLayerBuild::~FastVectorDbLayerBuild();
%ignore wx::FastVectorDbLayer::FastVectorDbLayer(FastVectorDbLayer::Impl *impl);
%ignore wx::FastVectorDbLayer::getFieldDefn(unsigned ix, FieldTypeEnum &ft, double &vmin, double &vmax);
%ignore wx::FastVectorDbLayer::~FastVectorDbLayer();
%ignore wx::FastVectorDbFeature::FastVectorDbFeature();
%ignore wx::FastVectorDbFeature::~FastVectorDbFeature();
%ignore wx::FastVectorDb::load(void *pdata, size_t size, fnFreeDbBuffer fnFreeBuffer, void *cookie);
%ignore wx::FastVectorDbBuild::postToBuffer;
%ignore wx::FastVectorDbLayerBuild::setNumericColumnBulk;
%ignore wx::ScratchAllocation::data;
%ignore wx::FinalBackingAllocation::data;
%ignore wx::FinalBackingResource::allocate;
%ignore wx::HeapFinalBackingResource::allocate;
%ignore wx::HeapScratchAllocation::HeapScratchAllocation;
%ignore wx::HeapFinalBackingAllocation::HeapFinalBackingAllocation;
%nodefaultctor wx::ScratchAllocation;
%nodefaultctor wx::ScratchAllocator;
%nodefaultctor wx::HeapScratchAllocation;
%nodefaultctor wx::FinalBackingAllocation;
%nodefaultctor wx::FinalBackingResource;
%nodefaultctor wx::HeapFinalBackingAllocation;
%nodefaultctor FastVectorDbLayerBuild;
%nodefaultdtor FastVectorDbLayerBuild;
%nodefaultctor FastVectorDbFeature;
%nodefaultdtor FastVectorDbFeature;
%nodefaultctor FastVectorDbLayer;
%nodefaultdtor FastVectorDbLayer;

%rename(WxMemoryStream)     wx::MemoryStream;
%rename(WxScratchAllocation) wx::ScratchAllocation;
%rename(WxScratchAllocator)   wx::ScratchAllocator;
%rename(WxHeapScratchAllocator) wx::HeapScratchAllocator;
%rename(WxHeapScratchAllocation) wx::HeapScratchAllocation;
%rename(WxFinalBackingAllocation) wx::FinalBackingAllocation;
%rename(WxFinalBackingResource)   wx::FinalBackingResource;
%rename(WxHeapFinalBackingResource) wx::HeapFinalBackingResource;
%rename(WxHeapFinalBackingAllocation) wx::HeapFinalBackingAllocation;
%rename(WxLayerTable)       wx::FastVectorDbLayer;
%rename(WxDatabase)         wx::FastVectorDb;
%rename(WxFeature)          wx::FastVectorDbFeature;
%rename(WxFeatureRef)       wx::FastVectorDbFeatureRef;
%rename(WxDatabaseBuild)    wx::FastVectorDbBuild;
%rename(WxLayerTableBuild)  wx::FastVectorDbLayerBuild;
//make the name just python like
%rename(add_field)         addField;
%rename(set_geometry_type) setGeometryType;
%rename(enable_st32)       enableStringTableU32;
%rename(set_extent)        setExtent;
%rename(add_feature_begin) addFeatureBegin;
%rename(set_geometry_wkt)  setGeometryWKT;
%rename(set_geometry_wkb)  setGeometryWKB;
%rename(set_geometry_raw)  setGeometryRaw;
%rename(set_field)             setField;
%rename(create_feature_ref)    createFeatureRef;
%rename(add_feature_end)       addFeatureEnd;
%rename(set_field_cstring)     setField_cstring;
%rename(set_field_wstring)     setField_wstring;
%rename(create_layer_begin)    createLayerBegin;
%rename(create_layer_end)      createLayerEnd;
%rename(get_geometry_type)     getGeometryType;
%rename(get_field_count)       getFieldCount;
%rename(get_field_defn)        getFieldDefn_p;
%rename(get_extent)            getExtent_p;
%rename(get_feature_count)     getFeatureCount;       
%rename(get_geometry_like_chunk) getGeometryLikeChunk;       
%rename(get_field_as_float)     getFieldAsFloat;       
%rename(get_field_as_int)       getFieldAsInt;       
%rename(get_field_as_string)    getFieldAsString;       
%rename(get_field_as_wstring)   getFieldAsWString;       
%rename(get_field_as_string_view) getFieldAsStringView;
%rename(get_string_column_offsets) getStringColumnOffsets;
%rename(get_string_column_data) getStringColumnData;
%rename(set_field_string_view) setFieldStringView;
%rename(set_string_column_bulk) setStringColumnBulk;
%rename(get_field_as_ref)       getFieldAsFeatureRef;   
%rename(set_feature_cookie)     setFeatureCookie;   
%rename(get_feature_cookie)     getFeatureCookie;   
%rename(tryGetFeature)          tryGetFeatureAt;
%rename(get_layer_count)        getLayerCount;
%rename(get_layer)              getLayer;
%rename(get_address)            getAddress;
%rename(get_field_offset)       getFieldOffset;
%rename(get_feature_byte_size)  getFeatureByteSize;;
%rename(byte_length)            byteLength;
%rename(table_buffer_bytes)     tableBufferBytes;
%rename(_set_table_buffer_materialized) setTableBufferMaterialized;
%rename(post_to_final_backing)  postToFinalBacking;
%rename(used_size)              usedSize;
%rename(rolled_back)            rolledBack;
%rename(allocation_count)       allocationCount;
%rename(commit_count)           commitCount;
%rename(rollback_count)         rollbackCount;
%rename(release_count)          releaseCount;
%newobject wx::ScratchAllocator::allocate;
%newobject wx::HeapScratchAllocator::allocate;
%newobject wx::FastVectorDbBuild::postToFinalBacking;
%newobject wx::ScratchAllocator::_allocate_for_context;
%newobject wx::FinalBackingResource::_allocate_for_context;

%rename(add_list_field)         add_list_field;
%rename(set_field_list_numeric) set_field_list_numeric;
%rename(set_field_list_refs)    set_field_list_refs;
%rename(update_feature_ref)     update_feature_ref;
%rename(update_list_ref_at)     update_list_ref_at;
%rename(get_field_as_list_view) getFieldAsListView;
%rename(get_field_list_size)    getFieldListSize;
%rename(get_field_list_ref_at)  getFieldListRefAt;

%extend wx::chunk_data_t {
    PyObject *as_array(PyObject* npType) {
        int typenum;
        PyArray_Descr* descr = PyArray_DescrFromTypeObject(npType);
        if (descr == NULL) { 
            PyErr_Clear(); // 或进行其他错误处理
            return NULL;
        } else { 
            typenum = descr->type_num;
        }

        // get element size - compatible with NumPy 1.x and 2.x
        npy_intp item_size;
        %#if defined(NPY_ABI_VERSION) && NPY_ABI_VERSION >= 0x02000000
            item_size = PyDataType_ELSIZE(descr);
        %#else
            item_size = descr->elsize;
        %#endif

        Py_DECREF(descr);
        
        npy_intp num_elements = $self->size / item_size;
        
        if ($self->size % item_size != 0) {
            PyErr_SetString(PyExc_ValueError, "Buffer size does not match an integer number of elements for the given data type.");
            return NULL;
        }
        npy_intp dims[1] = {num_elements}; 
        PyObject *array = PyArray_SimpleNewFromData(1, dims, typenum, (void*)($self->pdata));
        if (!array) {
            PyErr_SetString(PyExc_RuntimeError, "Failed to create NumPy array");
            return NULL;
        }
        PyArray_ENABLEFLAGS((PyArrayObject*)array, NPY_ARRAY_C_CONTIGUOUS);
        PyArray_ENABLEFLAGS((PyArrayObject*)array, NPY_ARRAY_WRITEABLE);
        PyArray_CLEARFLAGS((PyArrayObject*)array, NPY_ARRAY_OWNDATA);
        return array;
    }
    
    PyObject *to_bytes() {
        return PyBytes_FromStringAndSize((const char*)$self->pdata, $self->size);
    }
    
    // copy data directly to a writable buffer (memoryview, bytearray, shared memory, etc.)
    int copy_to_buffer(PyObject* dest_buffer) {
        Py_buffer view;
        // get buffer from destination object
        if (PyObject_GetBuffer(dest_buffer, &view, PyBUF_WRITABLE | PyBUF_SIMPLE) != 0) {
            PyErr_SetString(PyExc_TypeError, "Destination must be a writable buffer");
            return -1;
        }
        
        // check size
        if ((size_t)view.len < $self->size) {
            PyBuffer_Release(&view);
            PyErr_Format(PyExc_ValueError, 
                "Destination buffer too small: need %u bytes, got %zd bytes", 
                $self->size, view.len);
            return -1;
        }
        
        // direct memory copy with GIL released for better threading throughput
        Py_BEGIN_ALLOW_THREADS
        memcpy(view.buf, $self->pdata, $self->size);
        Py_END_ALLOW_THREADS
        
        PyBuffer_Release(&view);
        return 0;
    }
}

%pythoncode %{
    import numpy as np
%}
%extend wx::FastVectorDbLayer {
   %pythoncode %{
        def get_column(self,index):
            class __column_np_interface__:
                def __init__(self, table,index,tystr,address,stride,length):
                    self.table  = table
                    self.name = table.get_field_defn(index)
                    self.index  = index
                    self.tystr   = tystr
                    self.address= address
                    self.stride = stride
                    self.length = length
                @property
                def __array_interface__(self):
                    """返回一个字典，描述数组的接口"""
                    return {
                        'version': 3,
                        'typestr': self.tystr,
                        'shape': (self.length,),  
                        'data':  (self.address, False), 
                        'strides':(self.stride,)
                    }
                def as_nparray(self):
                    """创建一个numpy数组"""
                    return np.array(self,copy=False)

                def create_from_table(table,index):
                    fieldfn = table.get_field_defn(index)
                    tystr=''
                    tp = fieldfn[1]
                    if(fieldfn[1]==ftU8):
                        tystr='u1'
                    elif (tp==ftU16):
                        tystr='u2'
                    elif (tp==ftU32):
                        tystr='<u4'
                    elif (tp==ftI32):
                        tystr='<i4'
                    elif (tp==ftF32):
                        tystr='<f4'
                    elif (tp==ftF64):
                        tystr='<f8'

                    if(not tystr):
                        return None
                    ptr = _fastdb4py.get_swig_ptr_as_long(table.tryGetFeature(0).get_address())
                    offset = table.get_field_offset(index)
                    ptr+=offset
                    return __column_np_interface__(
                            table,index,
                            tystr,ptr,
                            table.get_feature_byte_size(),table.get_feature_count()
                        )
            return __column_np_interface__.create_from_table(self,index)
    %}
}

%extend wx::FastVectorDbLayerBuild {
    PyObject* set_numeric_column_bulk(unsigned field_id, PyObject* py_values) {
        Py_buffer view;
        if (PyObject_GetBuffer(py_values, &view, PyBUF_CONTIG_RO) != 0)
            SWIG_exception_fail(SWIG_TypeError, "Expected a contiguous buffer-compatible object.");
        $self->setNumericColumnBulk(field_id, view.buf, (u64)view.len);
        PyBuffer_Release(&view);
        Py_RETURN_NONE;
    fail:
        return NULL;
    }

    PyObject* set_string_column_from_sequence(unsigned field_id, PyObject* py_values) {
        PyObject* seq = PySequence_Fast(py_values, "Expected a sequence of str or None");
        if (!seq)
            return NULL;
        Py_ssize_t count = PySequence_Fast_GET_SIZE(seq);
        vector<wx::utf8_view_t> views;
        views.reserve((size_t)count);
        for (Py_ssize_t i = 0; i < count; ++i) {
            PyObject* item = PySequence_Fast_GET_ITEM(seq, i);
            if (item == Py_None) {
                views.push_back(wx::utf8_view_t{"", 0});
                continue;
            }
            if (!PyUnicode_Check(item)) {
                Py_DECREF(seq);
                SWIG_exception_fail(SWIG_TypeError, "Expected a sequence of str or None");
            }
            Py_ssize_t len = 0;
            const char* data = PyUnicode_AsUTF8AndSize(item, &len);
            if (!data) {
                Py_DECREF(seq);
                return NULL;
            }
            if (len < 0 || len > (Py_ssize_t)UINT32_MAX) {
                Py_DECREF(seq);
                SWIG_exception_fail(SWIG_ValueError, "String item is too large");
            }
            views.push_back(wx::utf8_view_t{data, (u32)len});
        }
        $self->setStringColumnFromViews(field_id, views.data(), (unsigned)views.size(), nullptr);
        Py_DECREF(seq);
        Py_RETURN_NONE;
    fail:
        return NULL;
    }
}

%extend wx::FastVectorDbLayerBuild {
    // Push one complete feature from a Python cache dict in a single Python→C transition.
    // Pre-computed schema data (num_names, num_ids, str_names, str_ids) must be passed as
    // pre-built Python list objects and numpy uint32 arrays (computed once at schema build time).
    // This eliminates per-field SWIG wrapper overhead (~150ns × N_fields per feature).
    void push_from_dict(PyObject* py_dict,
                        PyObject* py_num_names, PyObject* py_num_ids,
                        PyObject* py_str_names, PyObject* py_str_ids) {
        int n_num = (int)PyList_GET_SIZE(py_num_names);
        int n_str = (int)PyList_GET_SIZE(py_str_names);
        const u32* num_ids = (const u32*)PyArray_DATA((PyArrayObject*)py_num_ids);
        const u32* str_ids = (const u32*)PyArray_DATA((PyArrayObject*)py_str_ids);
        self->addFeatureBegin();
        for (int i = 0; i < n_num; i++) {
            PyObject* name = PyList_GET_ITEM(py_num_names, i);
            PyObject* val = PyDict_GetItem(py_dict, name);
            double v;
            if (!val || val == Py_None) {
                v = 0.0;
            } else if (PyFloat_Check(val)) {
                v = PyFloat_AS_DOUBLE(val);
            } else if (PyLong_Check(val)) {
                v = PyLong_AsDouble(val);
            } else {
                v = PyFloat_AsDouble(val);
            }
            self->setField(num_ids[i], v);
        }
        for (int i = 0; i < n_str; i++) {
            PyObject* name = PyList_GET_ITEM(py_str_names, i);
            PyObject* val = PyDict_GetItem(py_dict, name);
            if (val && val != Py_None) {
                self->setField_cstring(str_ids[i], PyUnicode_AsUTF8(val));
            } else {
                self->setField_cstring(str_ids[i], "");
            }
        }
        self->addFeatureEnd();
    }

    // Variant with feature-count increment: args reordered so cache is LAST to enable
    // functools.partial(push_from_dict_fc, self, nn, ni, sn, si, fc_arr)(cache).
    // fc_arr is a 1-element numpy int64 array; incremented in C to avoid Python attr overhead.
    void push_from_dict_fc(PyObject* py_num_names, PyObject* py_num_ids,
                           PyObject* py_str_names, PyObject* py_str_ids,
                           PyObject* py_fc_arr, PyObject* py_dict) {
        int n_num = (int)PyList_GET_SIZE(py_num_names);
        int n_str = (int)PyList_GET_SIZE(py_str_names);
        const u32* num_ids = (const u32*)PyArray_DATA((PyArrayObject*)py_num_ids);
        const u32* str_ids = (const u32*)PyArray_DATA((PyArrayObject*)py_str_ids);
        self->addFeatureBegin();
        for (int i = 0; i < n_num; i++) {
            PyObject* name = PyList_GET_ITEM(py_num_names, i);
            PyObject* val = PyDict_GetItem(py_dict, name);
            double v;
            if (!val || val == Py_None) {
                v = 0.0;
            } else if (PyFloat_Check(val)) {
                v = PyFloat_AS_DOUBLE(val);
            } else if (PyLong_Check(val)) {
                v = PyLong_AsDouble(val);
            } else {
                v = PyFloat_AsDouble(val);
            }
            self->setField(num_ids[i], v);
        }
        for (int i = 0; i < n_str; i++) {
            PyObject* name = PyList_GET_ITEM(py_str_names, i);
            PyObject* val = PyDict_GetItem(py_dict, name);
            if (val && val != Py_None) {
                self->setField_cstring(str_ids[i], PyUnicode_AsUTF8(val));
            } else {
                self->setField_cstring(str_ids[i], "");
            }
        }
        self->addFeatureEnd();
        /* increment feature counter stored in numpy int64 array (avoids Python attr overhead) */
        int64_t* fc_ptr = (int64_t*)PyArray_DATA((PyArrayObject*)py_fc_arr);
        (*fc_ptr)++;
    }

    // Batch variant: processes a Python list of cache dicts in a single C call.
    // Eliminates per-feature Python→C bridge overhead; all features are handled in a tight C loop.
    // Signature mirrors push_from_dict_fc but accepts py_list_of_dicts (last arg) instead of py_dict.
    void push_many_from_dicts_fc(PyObject* py_num_names, PyObject* py_num_ids,
                                  PyObject* py_str_names, PyObject* py_str_ids,
                                  PyObject* py_fc_arr, PyObject* py_list_of_dicts) {
        int n_num = (int)PyList_GET_SIZE(py_num_names);
        int n_str = (int)PyList_GET_SIZE(py_str_names);
        const u32* num_ids = (const u32*)PyArray_DATA((PyArrayObject*)py_num_ids);
        const u32* str_ids = (const u32*)PyArray_DATA((PyArrayObject*)py_str_ids);
        int64_t* fc_ptr = (int64_t*)PyArray_DATA((PyArrayObject*)py_fc_arr);
        Py_ssize_t n_features = PyList_GET_SIZE(py_list_of_dicts);
        for (Py_ssize_t feat_idx = 0; feat_idx < n_features; feat_idx++) {
            PyObject* py_dict = PyList_GET_ITEM(py_list_of_dicts, feat_idx);
            self->addFeatureBegin();
            for (int i = 0; i < n_num; i++) {
                PyObject* name = PyList_GET_ITEM(py_num_names, i);
                PyObject* val = PyDict_GetItem(py_dict, name);
                double v;
                if (!val || val == Py_None) {
                    v = 0.0;
                } else if (PyFloat_Check(val)) {
                    v = PyFloat_AS_DOUBLE(val);
                } else if (PyLong_Check(val)) {
                    v = PyLong_AsDouble(val);
                } else {
                    v = PyFloat_AsDouble(val);
                }
                self->setField(num_ids[i], v);
            }
            for (int i = 0; i < n_str; i++) {
                PyObject* name = PyList_GET_ITEM(py_str_names, i);
                PyObject* val = PyDict_GetItem(py_dict, name);
                if (val && val != Py_None) {
                    self->setField_cstring(str_ids[i], PyUnicode_AsUTF8(val));
                } else {
                    self->setField_cstring(str_ids[i], "");
                }
            }
            self->addFeatureEnd();
            (*fc_ptr)++;
        }
    }

}

%extend wx::FastVectorDbBuild {
    PyObject* post_into_buffer(PyObject* dest_buffer) {
        Py_buffer view;
        if (PyObject_GetBuffer(dest_buffer, &view, PyBUF_WRITABLE | PyBUF_SIMPLE) != 0) {
            PyErr_SetString(PyExc_TypeError, "destination buffer must be writable");
            return NULL;
        }

        size_t expected = $self->byteLength();
        if ((size_t)view.len < expected) {
            PyBuffer_Release(&view);
            PyErr_Format(
                PyExc_ValueError,
                "Destination buffer too small: need %zu bytes, got %zd bytes",
                expected,
                view.len
            );
            return NULL;
        }

        size_t written = 0;
        Py_BEGIN_ALLOW_THREADS
        written = $self->postToBuffer(view.buf, (size_t)view.len);
        Py_END_ALLOW_THREADS
        PyBuffer_Release(&view);

        if (written != expected) {
            PyErr_SetString(PyExc_RuntimeError, "FastDB final backing write failed");
            return NULL;
        }
        return PyLong_FromSize_t(written);
    }
}

%extend wx::ScratchAllocation {
    PyObject* _writable_buffer() {
        size_t size = $self->size();
        void* data = $self->data();
        if (size > 0 && data == nullptr) {
            PyErr_SetString(PyExc_RuntimeError, "FastDB scratch allocation has no writable data");
            return NULL;
        }
        return PyMemoryView_FromMemory((char*)data, (Py_ssize_t)size, PyBUF_WRITE);
    }
}

%extend wx::ScratchAllocator {
    wx::ScratchAllocation* _allocate_for_context(size_t size) {
        return $self->allocate(size, alignof(u64));
    }
}

%extend wx::FinalBackingAllocation {
    PyObject* _writable_buffer() {
        if ($self->committed()) {
            PyErr_SetString(PyExc_RuntimeError, "FastDB final backing allocation is already committed");
            return NULL;
        }
        if ($self->rolledBack()) {
            PyErr_SetString(PyExc_RuntimeError, "FastDB final backing allocation is rolled back");
            return NULL;
        }
        size_t size = $self->size();
        void* data = $self->data();
        if (size > 0 && data == nullptr) {
            PyErr_SetString(PyExc_RuntimeError, "FastDB final backing allocation has no writable data");
            return NULL;
        }
        return PyMemoryView_FromMemory((char*)data, (Py_ssize_t)size, PyBUF_WRITE);
    }

    PyObject* _readonly_buffer() {
        if (!$self->committed()) {
            PyErr_SetString(PyExc_RuntimeError, "FastDB final backing allocation is not committed");
            return NULL;
        }
        if ($self->rolledBack()) {
            PyErr_SetString(PyExc_RuntimeError, "FastDB final backing allocation is rolled back");
            return NULL;
        }
        size_t size = $self->usedSize();
        void* data = $self->data();
        if (size > 0 && data == nullptr) {
            PyErr_SetString(PyExc_RuntimeError, "FastDB final backing allocation has no committed data");
            return NULL;
        }
        return PyMemoryView_FromMemory((char*)data, (Py_ssize_t)size, PyBUF_READ);
    }

    PyObject* to_bytes() {
        if (!$self->committed()) {
            PyErr_SetString(PyExc_RuntimeError, "FastDB final backing allocation is not committed");
            return NULL;
        }
        if ($self->rolledBack()) {
            PyErr_SetString(PyExc_RuntimeError, "FastDB final backing allocation is rolled back");
            return NULL;
        }
        size_t size = $self->usedSize();
        void* data = $self->data();
        if (size > 0 && data == nullptr) {
            PyErr_SetString(PyExc_RuntimeError, "FastDB final backing allocation has no committed data");
            return NULL;
        }
        return PyBytes_FromStringAndSize((const char*)data, size);
    }
}

%extend wx::FinalBackingResource {
    wx::FinalBackingAllocation* _allocate_for_context(size_t size) {
        return $self->allocate(size, alignof(u64));
    }
}

%extend wx::FastVectorDbFeature {
    // Batch-read: read multiple scalar fields into a freshly allocated numpy float64 array.
    PyObject* get_fields_as_doubles(PyObject* py_field_ids) {
        if (!PyArray_Check(py_field_ids)) {
            PyErr_SetString(PyExc_TypeError, "field_ids must be a numpy uint32 array");
            return NULL;
        }
        PyArrayObject* arr_fids = (PyArrayObject*)py_field_ids;
        npy_intp n = PyArray_SIZE(arr_fids);
        npy_intp dims[1] = {n};
        PyObject* out = PyArray_SimpleNew(1, dims, NPY_DOUBLE);
        if (!out) return NULL;
        $self->getFieldsAsDoubles(
            (const u32*)PyArray_DATA(arr_fids), (int)n,
            (double*)PyArray_DATA((PyArrayObject*)out)
        );
        return out;
    }

    // Batch-read into a pre-allocated numpy float64 array (hot-path, no allocation).
    void get_fields_into(PyObject* py_field_ids, PyObject* py_out) {
        PyArrayObject* arr_fids = (PyArrayObject*)py_field_ids;
        PyArrayObject* arr_out  = (PyArrayObject*)py_out;
        $self->getFieldsAsDoubles(
            (const u32*)PyArray_DATA(arr_fids),
            (int)PyArray_SIZE(arr_fids),
            (double*)PyArray_DATA(arr_out)
        );
    }

    // Batch-write scalar fields from numpy float64 values array.
    void set_fields_from_doubles(PyObject* py_field_ids, PyObject* py_values) {
        PyArrayObject* arr_fids = (PyArrayObject*)py_field_ids;
        PyArrayObject* arr_vals = (PyArrayObject*)py_values;
        $self->setFieldsFromDoubles(
            (const u32*)PyArray_DATA(arr_fids),
            (const double*)PyArray_DATA(arr_vals),
            (int)PyArray_SIZE(arr_fids)
        );
    }
}

%extend wx::MemoryStream {
    PyObject* get_bytes() {
        chunk_data_t cd = $self->data();
        return PyBytes_FromStringAndSize((const char*)cd.pdata, cd.size);
    }
}

%apply  char            {i8};
%apply  unsigned char   {u8};
%apply  unsigned int    {i32};
%apply  unsigned int    {u32};
%apply           short  {i16};
%apply  unsigned short  {u16};
%apply  float           {f32};
%apply  double          {f64};
%apply  unsigned short  {uchar_t};

//%ignore fastdb_api;
%include "../include/fastdb.h"
%include "../include/fastdb-geometry-utils.h"

%inline %{
namespace wx{
    PyObject* get_swig_ptr_as_long(PyObject* py_obj) {
        void* ptr = NULL; 
        int res = SWIG_ConvertPtr(py_obj, (void**)&ptr, SWIGTYPE_p_void, 0);
        if (SWIG_IsOK(res)) 
        {
            PyObject* py_long = PyLong_FromVoidPtr(ptr);
            return py_long;
        } else {
            // conversion failed, handle error
            PyErr_SetString(PyExc_TypeError, "Failed to convert Python object to void*");
            Py_RETURN_NONE;
        }
    }
}
%}
