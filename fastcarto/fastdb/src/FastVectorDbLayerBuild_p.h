#pragma once
#include "fastdb.h"
#ifndef __FAST_VECTOR_DB_LAYER_BUILD_H__
#define __FAST_VECTOR_DB_LAYER_BUILD_H__
#include "FastVectorDbBuild_p.h"
#include <vector>
#include <string>
#include <map>
#include <assert.h>
using namespace std;
namespace wx{
    struct field_desc_ex_t
    {
        char    name[16];
        u16     type;
        double  vmin;
        double  vmax;

        size_t  size;
        size_t  offset;
    };
    
    struct layer_header_t
    {
        char    name[64];
        u32     feature_count;
        u16     geometry_type;
        u16     field_count;
        u16     coord_format;
        bool    aabbox_enable;
        bool    string_table_u32;
        double  minx;
        double  miny;
        double  maxx;
        double  maxy;
        size_t  offset_table;
        size_t  offset_strings;
        size_t  offset_wstrings;
        size_t  total_size;
    };
    class FastVectorDbBuild;
    class FastVectorDbLayerBuild::Impl
    {
    public:
        Impl(FastVectorDbBuild* db,const char* name);
       ~Impl();
        const char* name();
        void   setDbIndex(int ix);
        int    addField(const char* name,unsigned ft,double vmin,double vmax);
        void   setGeometryType(GeometryLikeEnum gt,CoordinateFormatEnum ct,bool aabboxEnabled);
        void   enableStringTableU32(bool b);
        void   setExtent(double minx,double miny,double maxx,double maxy);
        void   addFeatureBegin();
        void   truncate(unsigned nfeatures);
        void   setGeometry(const char* data,size_t size,GeometryLikeFormat fmt);
        void   setField(unsigned ix,double value);
        void   setField(unsigned ix,int    value);
        void   setField(unsigned ix,const char* text);
        void   setField(unsigned ix,const wchar_t* text);
        void   setField(unsigned ix,const FastVectorDbFeatureRef* ref);
        FastVectorDbFeatureRef* createFeatureRef(u32 ix);
        void   freeFeatureRef(FastVectorDbFeatureRef* ref);
        void   addFeatureEnd();
        void   post();
        size_t get_total_size();
        void   write(WriteStream* stream);
    public:
        template<class point2_tt>
        inline void convert_coord_format(const point2_tt& p,point2_t& out){
            out = p;
        }
        template<class point2_tt>          
        inline void convert_coord_format(const point2_tt& p,point2_f32_t& out){
            out.x = p.x;
            out.y = p.y;
        }
        template<class point2_tt>       
        inline void convert_coord_format(const point2_tt& p,point2_x16_t& out){
            out.x = (u16)(0xFFFF*(p.x-m_minx)/(m_maxx-m_minx));
            out.y = (u16)(0xFFFF*(p.y-m_miny)/(m_maxy-m_miny));
        } 
        template<class point2_tt> 
        inline void convert_coord_format(const point2_tt& p,point2_x24_t& out){
            out.x = (u32)(0xFFFFFF*(p.x-m_minx)/(m_maxx-m_minx));
            out.y = (u32)(0xFFFFFF*(p.y-m_miny)/(m_maxy-m_miny));
        } 
        template<class point2_tt> 
        inline void convert_coord_format(const point2_tt& p,point2_x32_t& out){
            out.x = (u32)(0xFFFFFFFF*(p.x-m_minx)/(m_maxx-m_minx));
            out.y = (u32)(0xFFFFFFFF*(p.y-m_miny)/(m_maxy-m_miny));
        }
    private:
        void validate_coord(const point2_t& p);
        size_t field_type_byte_size(unsigned ft);
    private:
        vector<u8>       m_table_buffer;
        vector<u8>       m_geometries_buffer;
        vector<string*>  m_string_table;
        map<string,int>  m_string_map;
        vector<wstring*> m_wstring_table;
        map<wstring,int> m_wstring_map;
        size_t           m_string_total_size;
        size_t           m_wstring_total_size;
        size_t           m_feature_count;
        size_t           m_table_line_size;
        GeometryLikeEnum        m_geometry_type;
        CoordinateFormatEnum    m_coord_format;
        vector<field_desc_ex_t> m_field_descs;
        vector<u8>       m_current_line_buffer;
        vector<u8>       m_current_geom_buffer;
        bool             m_extent_done;
        bool             m_string_table_u32;
        bool             m_aabbox_enable;
        double           m_tcx;
        double           m_tcy;
        string m_name;
        double m_minx;
        double m_miny;
        double m_maxx;
        double m_maxy;
        u32    m_index_in_db;
        vector<FastVectorDbFeatureRef*> m_created_feature_refs;

        template <class coord_type>
        friend bool build_geometry_buffer_from_buffer(vector<u8> &buffer, FastVectorDbLayerBuild::Impl &build, const char *data, size_t size, GeometryLikeFormat inputFormat, GeometryLikeEnum declType);

    };

    template <typename T>
    inline void set_field_value_t(u8* buffer, const field_desc_ex_t &fdx, T value,bool stringTableU32=0xff)
    {
        if (fdx.type == ftU8)
        {
            u8 v = (u8)value;
            memcpy(buffer + fdx.offset, &v, sizeof(v));
        }
        else if (fdx.type == ftU16)
        {
            u16 v = (u16)value;
            memcpy(buffer + fdx.offset, &v, sizeof(v));
        }
        else if (fdx.type == ftU32)
        {
            u32 v = (u32)value;
            memcpy(buffer + fdx.offset, &v, sizeof(v));
        }
        else if (fdx.type == ftI32)
        {
            i32 v = (i32)value;
            memcpy(buffer + fdx.offset, &v, sizeof(v));
        }
        else if (fdx.type == ftF32)
        {
            f32 v = (f32)value;
            memcpy(buffer + fdx.offset, &v, sizeof(v));
        }
        else if (fdx.type == ftF64)
        {
            f64 v = (f64)value;
            memcpy(buffer + fdx.offset, &v, sizeof(v));
        }
        else if (fdx.type == ftU16n)
        {
            u16 v = (u16)(0xFFFF * (value - fdx.vmin) / (fdx.vmax - fdx.vmin));
        }
        else if (fdx.type == ftU8n)
        {
            u8 v = (u8)(0xFF * (value - fdx.vmin) / (fdx.vmax - fdx.vmin));
            memcpy(buffer + fdx.offset, &v, sizeof(v));
        }

        else if (fdx.type == ftSTR || fdx.type == ftWSTR)
        {
            if(stringTableU32)
            {
                int v = (int)value;
                memcpy(buffer + fdx.offset, &v, sizeof(v));
            }
            else
            {
                if(value>0xFFFF)
                {
                    warning("string index is greater than 0xFFFF with u16 string table index!");
                }
                u16 v = (u16)value;
                memcpy(buffer + fdx.offset, &v, sizeof(v));
            }
        }
        else
        {
            assert(false);
        }
    }

}
#endif