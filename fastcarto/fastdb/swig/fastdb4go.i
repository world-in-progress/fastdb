%module fastdb4go

%insert(cgo_comment_typedefs) %{
// #cgo CXXFLAGS: -I/workspaces/fastdb/fastcarto/fastdb/include
// #cgo LDFLAGS: -L/workspaces/fastdb/fastcarto/build/fastdb -lfastdb -Wl,-rpath,/workspaces/fastdb/fastcarto/build/fastdb
#cgo CXXFLAGS: -I${SRCDIR}/include
#cgo LDFLAGS: -L${SRCDIR} -lfastdb -Wl,-rpath,${SRCDIR}
%}

%{
    #include "fastdb.h"
    #include "fastdb-geometry-utils.h"
    using namespace wx;
%}

// Tell SWIG to ignore fastdb_api when parsing headers
#define fastdb_api

%include "typemaps.i"
%include "std_string.i"
%include "std_vector.i"

%include "cstring.i"
%include "cpointer.i"
%include "carrays.i"

// Instantiate templates used by helpers
%template(ByteVector) std::vector<unsigned char>;

// Ignore specific C++ constructs
%ignore wx::GeometryReturn;
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

// Renames for Go idiomatic names (optional, SWIG auto-capitalizes, but explicit is safe)
%rename(WxMemoryStream)     wx::MemoryStream;
%rename(WxLayerTable)       wx::FastVectorDbLayer;
%rename(WxDatabase)         wx::FastVectorDb;
%rename(WxFeature)          wx::FastVectorDbFeature;
%rename(WxFeatureRef)       wx::FastVectorDbFeatureRef;
%rename(WxDatabaseBuild)    wx::FastVectorDbBuild;
%rename(WxLayerTableBuild)  wx::FastVectorDbLayerBuild;

// Helper to convert chunk_data_t to Go slice (via vector)
%extend wx::chunk_data_t {
    std::vector<unsigned char> ToBytes() {
        std::vector<unsigned char> v;
        if ($self->pdata && $self->size > 0) {
            v.assign($self->pdata, $self->pdata + $self->size);
        }
        return v;
    }
}

%typemap(gotype) size_t *OUTPUT "[]int64"
%typemap(in) size_t *OUTPUT (long long temp) {
    if ($input.len == 0) {
        _swig_gopanic("array must contain at least 1 element");
    }
    $1 = (size_t*)&temp;
}
%typemap(argout) size_t *OUTPUT {
    long long* a = (long long *) $input.array;
    a[0] = (long long)temp$argnum;
}

%apply double* OUTPUT {double *vmin, double *vmax,double* minx,double* miny,double* maxx,double* maxy};
%apply size_t* OUTPUT {size_t* ft};
%apply enum wx::FieldTypeEnum { unsigned int ft };

%include "../include/fastdb.h"
%include "../include/fastdb-geometry-utils.h"
