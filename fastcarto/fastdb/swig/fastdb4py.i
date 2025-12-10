%module fastdb4py
%{
    #define SWIG_FILE_WITH_INIT
    #include "fastdb.h"
    #include "fastdb-geometry-utils.h"
    using namespace wx;
%}

// Tell SWIG to ignore fastdb_api when parsing headers
#define fastdb_api

%include "typemaps.i"
%include "cstring.i"
%include "cpointer.i"
%include "carrays.i"
%include "numpy.i"  // 包含 numpy.i 支持
%init %{
    import_array(); // 初始化 NumPy C-API
%}

//%array_class(point2_t, LineString);

%typemap(in)(const u8* data,size_t size){
    if (!PyBytes_Check($input)) {
        SWIG_exception_fail(SWIG_TypeError, "Expected a bytes object");
    }
    $1 = (unsigned char *)PyBytes_AsString($input);
    $2 = PyBytes_Size($input);
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
%nodefaultctor FastVectorDbLayerBuild;
%nodefaultdtor FastVectorDbLayerBuild;
%nodefaultctor FastVectorDbFeature;
%nodefaultdtor FastVectorDbFeature;
%nodefaultctor FastVectorDbLayer;
%nodefaultdtor FastVectorDbLayer;

%rename(WxMemoryStream)     wx::MemoryStream;
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
%rename(get_field_as_ref)       getFieldAsFeatureRef;   
%rename(set_feature_cookie)     setFeatureCookie;   
%rename(get_feature_cookie)     getFeatureCookie;   
%rename(tryGetFeature)          tryGetFeatureAt;
%rename(get_layer_count)        getLayerCount;
%rename(get_layer)              getLayer;
%rename(get_address)            getAddress;
%rename(get_field_offset)       getFieldOffset;
%rename(get_feature_byte_size)  getFeatureByteSize;;

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
        
        // direct memory copy - fastest way
        memcpy(view.buf, $self->pdata, $self->size);
        
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