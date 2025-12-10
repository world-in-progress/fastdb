#pragma once
#ifndef __FAST_VECTOR_DB_LAYER_P_H__
#define __FAST_VECTOR_DB_LAYER_P_H__
#include "fastdb.h"
#include "FastVectorDbBuild_p.h"
#include "FastVectorDbLayerBuild_p.h"
#include <vector>
using namespace std;

namespace wx
{
    class FastVectorDb;
    class FastVectorDbLayer::Impl
    {
    public:
        Impl(u8 *pdata, size_t size);
       ~Impl();
        const char*     name();
        GeometryLikeEnum getGeometryType();
        unsigned        getFieldCount();
        const char*     getFieldDefn(unsigned ix,FieldTypeEnum& ft,double& vmin,double& vmax);
        void            getExtent(double& minx,double& miny,double& maxx,double& maxy);
        u32             getFeatureCount();
        void            rewind();
        int             row();
        void            fetchGeometry(GeometryReturn* cb);
        chunk_data_t    getGeometryLikeChunk();
        double          getFieldAsFloat(u32 ix);
        int             getFieldAsInt(u32 ix);
        const char*     getFieldAsString(u32 ix);
        const uchar_t*  getFieldAsWString(u32 ix);
        FastVectorDbFeatureRef*  
                        getFieldAsFeatureRef(u32 ifield);
        void*           setFeatureCookie(void* cookie);
        void*           getFeatureCookie();
        bool            next();
        FastVectorDbFeature*  tryGetFeatureAt(u32 ifeature);
    public:
        void            fetchGeometry_internal(u32 ifeature,GeometryReturn* cb);
        void            fetchGeometry_internal(const u8* geometry_data_ptr,GeometryReturn* cb);
        void*           setFeatureCookie_internal(u32 ifeature,void* cookie);
        void*           getFeatureCookie_internal(u32 ifeature);
        double          getFieldAsFloat_internal(u32 ifeature,u32 ix);
        int             getFieldAsInt_internal(u32 ifeature,u32 ix);
        const char*     getFieldAsString_internal(u32 ifeature,u32 ix);
        const uchar_t*  getFieldAsWString_internal(u32 ifeature,u32 ix);
        FastVectorDbFeatureRef* 
                        getFieldAsFeatureRef_internal(u32 ifeature,u32 ix);
        chunk_data_t    getGeometryLikeChunk_internal(u32 ifeature);

        void            setField_internal(u32 ifeature,u32 ix,double value);
        void            setField_internal(u32 ifeature,u32 ix,int    value);
        
        void            setFeatureRef_internal(u32 ifeature, u32 ix, FastVectorDbFeature* feature); // Add by Dsssyc
        void*           getFeatureAddress(u32 ifeature);

        size_t          getFieldOffset(unsigned ix);
        size_t          getFeatureByteSize();
    private:
        void            move_next_geometry_ptr();
        size_t          get_geometry_like_size(const u8* pdata);
    public:
        inline void convert_coord_format(const point2_t& p,point2_t& out){
            out = p;
        }
        inline void convert_coord_format(const point2_f32_t& p,point2_t& out){
            out.x = p.x;     
            out.y = p.y;
        }
        inline void convert_coord_format(const point2_x16_t& p,point2_t& out){
            out.x = m_header->minx + (m_header->maxx - m_header->minx)*p.x/0xFFFF;
            out.y = m_header->miny + (m_header->maxy - m_header->miny)*p.y/0xFFFF;
        }
        inline void convert_coord_format(const point2_x24_t& p,point2_t& out){
            out.x = m_header->minx + (m_header->maxx - m_header->minx)*double(p.x)/0xFFFFFF;
            out.y = m_header->miny + (m_header->maxy - m_header->miny)*double(p.y)/0xFFFFFF;
        }
        inline void convert_coord_format(const point2_x32_t& p,point2_t& out){
            out.x = m_header->minx + (m_header->maxx - m_header->minx)*p.x/0xFFFFFFFF;
            out.y = m_header->miny + (m_header->maxy - m_header->miny)*p.y/0xFFFFFFFF;
        }

        template<class coord_type_t>
        class return_geometry;

        template <class coord_type_t>
        size_t get_geometry_byte_size(const u8 *geom_ptr, GeometryLikeEnum geomType);
       
        
    private:
        FastVectorDb*           m_db;
        FastVectorDbLayer*      m_layer;
        u32                     m_layer_index;
        u8*                     m_data;
        size_t                  m_size;
        layer_header_t*         m_header;
        u8*                     m_data_ptr0;
        size_t                  m_table_line_size;
        int                     m_ifeature;
        const field_desc_ex_t*  m_field_descs;
        //const u8*               m_table_data_ptr;
        u8*                     m_table_data_ptr0;
        u8*                     m_geometry_ptr0;
        u8*                     m_geometry_ptr;
        vector<const char *>    m_string_table;
        vector<const uchar_t *> m_wstring_table;
        vector<void*>           m_feature_cookie_map;
        vector<point2_t>        points;//a variant for return temp points
        vector<FastVectorDbFeature*>    m_feature_cache;
        vector<u8*>             m_geometry_ptr_map;
        friend class FastVectorDbFeature;
        friend class FastVectorDb::Impl;
    };

}
#endif
