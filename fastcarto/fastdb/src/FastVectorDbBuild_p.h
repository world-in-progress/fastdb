#ifndef __FAST_VECTOR_DB_BUILD_P_H__
#define __FAST_VECTOR_DB_BUILD_P_H__

//FastVectorDB:A fast vector database for local cache
//Just A DEMO,No Any Warranty for Commercial Use
//Author wenyongning@njnu.edu.cn

#include "fastdb.h"
#include "fastdb-geometry-utils.h"
#include <vector>
using namespace std;
namespace wx
{

    struct point2_f32_t
    {
        f32 x;
        f32 y;
    };
    struct point2_x16_t
    {
        u16 x;
        u16 y;
    };

#ifdef _MSC_VER
#pragma pack(push, 1)
#endif

struct
#ifndef _MSC_VER
        __attribute__((packed))
#endif
        point2_x24_t
    {
        u32 x : 24;
        u32 y : 24;
    };
#ifdef _MSC_VER
#pragma pack(pop)
#endif

    struct point2_x32_t
    {
        u32 x;
        u32 y;
    };

    struct aabbox_x16_t
    {
        point2_x16_t minEdge;
        point2_x16_t maxEdge;
    };

#ifdef _MSC_VER
    #pragma pack(push, 1)
#endif
    struct 
#ifndef _MSC_VER
    __attribute__((packed))
#endif 
    FastVectorDbFeatureRef
    {
       u16 ilayer;
       u8  ifeature;
       u16 ifeatureH; 
       static FastVectorDbFeatureRef make(u32 ilayer, u32 ifeature)
       {
          FastVectorDbFeatureRef ref;
          ref.ilayer = ilayer;
          ref.ifeature = ifeature & 0xFF;           // low 8 bits
          ref.ifeatureH= (ifeature >> 8) & 0xFFFF;  // high 16 bits
          return ref;
       }
        static FastVectorDbFeatureRef* make_ref(u32 ilayer, u32 ifeature)
       {
          FastVectorDbFeatureRef* ret = new FastVectorDbFeatureRef();
          *ret = make(ilayer, ifeature);
          return ret;
       }
       static u32 decodeIFeature(FastVectorDbFeatureRef* ref)
       {
          u32 ifeature = ref->ifeature | (((u32)ref->ifeatureH) << 8);
          return ifeature;
       }
    };

#ifdef _MSC_VER
    #pragma pack(pop)
#endif

    class MemoryStream::Impl
    {
    public:
        Impl();
        ~Impl();
        chunk_data_t data();
        void reset();   // reset buffer
        void write(void *pdata, size_t size);
    private:
        vector<u8> m_buffer;
    };

    class FastVectorDbLayerBuild;
    class FastVectorDbBuild::Impl
    {
    public:
        Impl(FastVectorDbBuild* thiz);
        ~Impl();
        void begin(const char *cfg);
        void truncate(const char *layerName, unsigned nfeatures);
        FastVectorDbLayerBuild* createLayerBegin(const char *layerName);
        void enableStringTableU32(bool b);
        int  addField(const char *name, unsigned ft, double vmin = 0, double vmax = 1.0);
        void setGeometryType(GeometryLikeEnum gt, CoordinateFormatEnum ct, bool aaboxEnable);
        void setExtent(double minx, double miny, double maxx, double maxy);
        void addFeatureBegin();
        void setGeometry(const char *data, size_t size, GeometryLikeFormat fmt);
        void setField(unsigned ix, double value);
        void setField(unsigned ix, int value);
        void setField(unsigned ix, const char *text);
        void setField(unsigned ix, const wchar_t *text);
        void addFeatureEnd();
        void createLayerEnd();
        void save(WriteStream *stream);
        void save(const char *filename);

    private:
        vector<FastVectorDbLayerBuild *> m_layers;
        FastVectorDbLayerBuild *m_current_layer;
        aabbox_t m_extent;
        bool m_aabbox_enable;
        GeometryLikeEnum m_gt;
        CoordinateFormatEnum m_ct;
        bool m_string_table_u32;
        string m_cfg;
        FastVectorDbBuild* m_thiz;
    };
    static const char *get_geometry_type_name(GeometryLikeEnum geomType)
    {
        switch (geomType)
        {
        case gtPoint:
            return "Point";
        case gtLineString:
            return "LineString";
        case gtPolygon:
            return "Polygon";
        default:
            return "gtAny!UnsupportedGeometryType!";
        }
    }

    static const char* get_field_type_name(int ft)
    {
        switch (ft)
        {
        case ftF32:
            return "F32";
        case ftF64:
            return "F64";
        case ftI32:
            return "I32";
        case ftSTR:
            return "STR";
        case ftU16:
            return "U16";
        case ftU16n:
            return "U16n";
        case ftU32:
            return "U32";
        case ftU8:
            return "U8";
        case ftU8n:
            return "U8n";
        case ftWSTR:
            return "WSTR";
        }

        return "ftUnknown";
    }
    static const char *get_coord_type_name(CoordinateFormatEnum coordType)
    {
        switch (coordType)
        {
        case cfF32:
            return "F32";
        case cfF64:
            return "F64";
        case cfTx16:
            return "Tx16";
        case cfTx24:
            return "Tx24";
        case cfTx32:
            return "Tx32";
        default:
            return "UnsupportedCoordinateFormat";
        }
    }
    void warning(const char *fmt);
}
#endif