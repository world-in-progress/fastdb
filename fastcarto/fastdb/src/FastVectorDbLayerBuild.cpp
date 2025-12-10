#include "FastVectorDbLayerBuild_p.h"
#include "fastdb.h"
#include "fastdb-geometry-utils.h"
#include "gaiageo.h"
namespace wx
{
    FastVectorDbLayerBuild::Impl::Impl(FastVectorDbBuild* db,const char *name)
    {
        m_name = name;
        m_feature_count = 0;
        m_string_total_size = 0;
        m_wstring_total_size = 0;
        m_minx=m_miny=-1e10;
        m_maxx=m_maxy= 1e10;
        m_geometry_type = gtAny;
        m_coord_format = cfF64;
        m_table_line_size = 0;
        m_extent_done = false;
        m_string_table_u32=false;
        m_aabbox_enable=false;
        m_tcx=1;
        m_tcy=1;
    }

    FastVectorDbLayerBuild::Impl::~Impl()
    {
        for (auto pstr : m_string_table)
        {
            delete pstr;
        }
        for (auto pwstr : m_wstring_table)
        {
            delete pwstr;
        }
        for(auto ref :m_created_feature_refs)
        {
            delete ref;
        }
        m_created_feature_refs.clear();
    }
    void   FastVectorDbLayerBuild::Impl::enableStringTableU32(bool b)
    {
        if(m_field_descs.size()>0)
            warning("resetting string table offset size is dangerous\n,when the field count is not zero!");
        m_string_table_u32=b;
    }

    size_t FastVectorDbLayerBuild::Impl::field_type_byte_size(u32 ft)
    {
        switch (ft)
        {
        case ftU8:
            return 1;
        case ftU16:
            return 2;
        case ftU32:
            return 4;
        case ftI32:
            return 4;
        case ftU8n:
            return 1;
        case ftU16n:
            return 2;
        case ftF32:
            return 4;
        case ftF64:
            return 8;
        case ftSTR:
        case ftWSTR:
            return m_string_table_u32?4:2;
        case ftFeatureRef:
            return sizeof(FastVectorDbFeatureRef);
        }
        assert(false);
        return 0;
    }
    const char* FastVectorDbLayerBuild::Impl::name()
    {
        return m_name.c_str();
    }
    void FastVectorDbLayerBuild::Impl::setDbIndex(int ix)
    {
        m_index_in_db=ix;
#ifdef DEBUG
        printf("\ncreate feature ref to layer %d",m_index_in_db);
#endif
    }
    int FastVectorDbLayerBuild::Impl::addField(const char *name, unsigned ft, double vmin, double vmax)
    {
        field_desc_ex_t fd;
        memset(fd.name, 0, 16);
        memcpy(fd.name, name, strlen(name));
        fd.type = ft;
        fd.vmin = vmin;
        fd.vmax = vmax;
        fd.size = field_type_byte_size(ft);
        if (m_field_descs.size() > 0)
        {
            fd.offset = m_field_descs.back().offset + m_field_descs.back().size;
        }
        else
        {
            fd.offset = 0;
        }
        m_field_descs.push_back(fd);
        m_table_line_size += fd.size;
        return m_field_descs.size();
    }

    void FastVectorDbLayerBuild::Impl::setGeometryType(GeometryLikeEnum gt, CoordinateFormatEnum ct,bool aabboxEnable)
    {
        m_geometry_type = gt;
        m_aabbox_enable = (gt!=gtPoint)&&aabboxEnable;
        m_coord_format = ct;
    }

    void FastVectorDbLayerBuild::Impl::setExtent(double minx, double miny, double maxx, double maxy)
    {
        m_extent_done = true;
        m_minx = minx;
        m_miny = miny;
        m_maxx = maxx;
        m_maxy = maxy;
        m_tcx = (maxx-minx)/0xFFFF;
        m_tcy = (maxy-miny)/0xFFFF;
    }

    void FastVectorDbLayerBuild::Impl::addFeatureBegin()
    {
        if (!m_extent_done && ((m_coord_format!=cfF64 && m_coord_format!=cfF32) || m_aabbox_enable))
        {
            char text[1024];
            snprintf(text,1024,"the layer[%s] extent has no been set!", this->m_name.c_str());
            warning(text);
        }
        m_current_line_buffer.resize(m_table_line_size);
        memset(m_current_line_buffer.data(), 0, m_table_line_size);
        m_current_geom_buffer.clear();
    }

    template <class valT>
    inline void write_buffer_t(vector<u8> &buffer, const valT &v)
    {
        const u8 *p = (const u8 *)&v;
        buffer.insert(buffer.end(), p, p + sizeof(v));
    }
    template <class pointT, class coord_type>
    inline void write_points_to_buffer_t(vector<u8> &buffer, FastVectorDbLayerBuild::Impl &build, u8 partType, pointT *points, u16 np)
    {
        write_buffer_t(buffer, partType);
        write_buffer_t(buffer, np);
        for (int i = 0; i < np; i++)
        {
            coord_type c;
            build.convert_coord_format(points[i], c);
            write_buffer_t(buffer, c);
        }
    }
   aabbox_t get_line_string_aabbox(const point2_t* points,size_t np)
    {
        aabbox_t box;
        box.minEdge = points[0];
        box.maxEdge = points[0];
        for(int i=1;i<np;i++)
        {
            auto& p = points[i];
            if(p.x<box.minEdge.x)
                box.minEdge.x = p.x;
            else if(p.x>box.maxEdge.x)
                box.maxEdge.x = p.x;
            
            if(p.y<box.minEdge.y)
                box.minEdge.y = p.y;
            else if(p.y>box.maxEdge.y)
                box.maxEdge.y = p.y;
        }
        return box;
    }

    template <class coord_type>
    bool build_geometry_buffer_from_buffer(vector<u8> &buffer,typename FastVectorDbLayerBuild::Impl &build, const char *data, size_t size, GeometryLikeFormat inputFormat, GeometryLikeEnum declType)
    {
        aabbox_t aabbox;
        gaiaGeomCollPtr gaiaHandle = NULL;
        if (inputFormat == ginWKT)
        {
            gaiaHandle = gaiaParseWkt((const unsigned char *)data, -1);
        }
        else if (inputFormat == ginWKB)
        {
            gaiaHandle = gaiaFromWkb((const unsigned char *)data, size);
        }
        u32 gaiaType = gaiaGeometryType(gaiaHandle);
     
        if (declType == gtPoint)
        {
            coord_type coord;
            if (inputFormat == ginPoint2)
                build.convert_coord_format(*(point2_t *)data, coord);
            else if (gaiaHandle && gaiaGeometryType(gaiaHandle) == GAIA_POINT)
                build.convert_coord_format(*(point2_t *)gaiaHandle->FirstPoint, coord);
            else
            {
                warning("write geometry error,type=point!");
                return(false);
            }
            write_buffer_t(buffer, coord);
        }

        else if (declType == gtLineString)
        {
            if (inputFormat == ginLineString)
            {
                u32 point_count = size;
                point2_t *points = (point2_t *)data;
                u16 npart = 1;
                aabbox=get_line_string_aabbox(points,point_count);
                write_buffer_t(buffer, npart);
                write_points_to_buffer_t<point2_t, coord_type>(buffer, build, gptLineString, points, point_count);
            }
            else if (gaiaHandle &&
                     (gaiaType == GAIA_LINESTRING ||
                      gaiaType == GAIA_MULTILINESTRING))
            {
                u16 npart = 0;
                auto lineString = gaiaHandle->FirstLinestring;
                while (lineString)
                {
                    write_points_to_buffer_t<point2_t, coord_type>(buffer, build,gptLineString, (point2_t *)lineString->Coords, lineString->Points);
                    lineString = lineString->Next;
                    npart++;
                }
                buffer.insert(buffer.begin(), (u8 *)&npart, (u8 *)&npart + sizeof(npart));
            }
            else
            {
                warning("write geometry error,type=linestring!");
                return (false);
            }
        }
        else if (declType == gtPolygon)
        {
            if (gaiaHandle &&
                (gaiaType == GAIA_POLYGON ||
                 gaiaType == GAIA_MULTIPOLYGON))
            {
                u16 npart = 0;
                auto polygon = gaiaHandle->FirstPolygon;
                while (polygon)
                {
                    write_points_to_buffer_t<point2_t, coord_type>(buffer, build, gptRingExternal, (point2_t *)polygon->Exterior->Coords, polygon->Exterior->Points);
                    for (int i = 0; i < polygon->NumInteriors; i++)
                    {
                        auto ring = polygon->Interiors + i;
                        write_points_to_buffer_t<point2_t, coord_type>(buffer, build, gptRingInternal, (point2_t *)ring->Coords, ring->Points);
                    }
                    npart += polygon->NumInteriors + 1;
                    polygon = polygon->Next;
                }
                buffer.insert(buffer.begin(), (u8 *)&npart, (u8 *)&npart + sizeof(npart));
            }
            else
            {
                warning("write geometry error,type=polygon!");
                return (false);
            }
        }
        else
        {
            return (false);
        }
        if (gaiaHandle)
        {
            aabbox.minEdge.x = gaiaHandle->MinX;
            aabbox.minEdge.y = gaiaHandle->MinY;
            aabbox.maxEdge.x = gaiaHandle->MaxX;
            aabbox.maxEdge.y = gaiaHandle->MaxY;
            gaiaFreeGeomColl(gaiaHandle);
        }
        if(build.m_aabbox_enable)
        {
            aabbox_x16_t box16;
            aabbox.minEdge.x-=build.m_tcx;//confirm the bounding box has a actual size;
            aabbox.minEdge.y-=build.m_tcy;
            aabbox.maxEdge.x+=build.m_tcx;
            aabbox.maxEdge.y+=build.m_tcy;

            build.convert_coord_format(aabbox.minEdge,box16.minEdge);
            build.convert_coord_format(aabbox.maxEdge,box16.maxEdge);
            buffer.insert(buffer.begin(), (u8 *)&box16, (u8 *)&box16 + sizeof(aabbox_x16_t));
        }
        return true;
    }

    void FastVectorDbLayerBuild::Impl::validate_coord(const point2_t &p)
    {
        if (p.x < m_minx || p.y < m_miny || p.x > m_maxx || p.y > m_maxy)
        {
            char text[1024];
            snprintf(text,1024, "point(%lf,%lf) is out of layer[%s]'s extent", p.x, p.y, m_name.c_str());
            warning(text);
        }
    }

    void FastVectorDbLayerBuild::Impl::setGeometry(const char *data, size_t size, GeometryLikeFormat fmt)
    {
        m_current_geom_buffer.clear();
        if(m_geometry_type == gtNone)
        {
            return;
        }
        else if(m_geometry_type == gtAny)
        {
            if(fmt!=ginRAW)
            {
                printf("\nWarning: geometry type is set to gtAny, but geometry input has set not ginRAW!\n");
            }
            u32 sizex= size;
            m_current_geom_buffer.insert(m_current_geom_buffer.end(), (u8 *)&sizex, (u8 *)&sizex + sizeof(sizex));
            m_current_geom_buffer.insert(m_current_geom_buffer.end(),data, data + size);
        }   
        else if (m_coord_format == cfF64)
        {
            build_geometry_buffer_from_buffer<point2_t>(m_current_geom_buffer, *this, data, size, fmt, m_geometry_type);
        }
        else if (m_coord_format == cfF32)
        {
            build_geometry_buffer_from_buffer<point2_f32_t>(m_current_geom_buffer, *this, data, size, fmt, m_geometry_type);
        }
        else if (m_coord_format == cfTx16)
        {
            build_geometry_buffer_from_buffer<point2_x16_t>(m_current_geom_buffer, *this, data, size, fmt, m_geometry_type);
        }
        else if (m_coord_format == cfTx24)
        {
            build_geometry_buffer_from_buffer<point2_x24_t>(m_current_geom_buffer, *this, data, size, fmt, m_geometry_type);
        }
        else if (m_coord_format == cfTx32)
        {
            build_geometry_buffer_from_buffer<point2_x32_t>(m_current_geom_buffer, *this, data, size, fmt, m_geometry_type);
        }
        else
        {
            assert(false);
        }
    }

    // template <typename T>
    // inline void set_field_value_t(vector<u8> &buffer, const field_desc_ex_t &fdx, T value,bool stringTableU32=0xff)
    // {
    //     if (fdx.type == ftU8)
    //     {
    //         u8 v = (u8)value;
    //         memcpy(buffer.data() + fdx.offset, &v, sizeof(v));
    //     }
    //     else if (fdx.type == ftU16)
    //     {
    //         u16 v = (u16)value;
    //         memcpy(buffer.data() + fdx.offset, &v, sizeof(v));
    //     }
    //     else if (fdx.type == ftU32)
    //     {
    //         u32 v = (u32)value;
    //         memcpy(buffer.data() + fdx.offset, &v, sizeof(v));
    //     }
    //     else if (fdx.type == ftI32)
    //     {
    //         i32 v = (i32)value;
    //         memcpy(buffer.data() + fdx.offset, &v, sizeof(v));
    //     }
    //     else if (fdx.type == ftF32)
    //     {
    //         f32 v = (f32)value;
    //         memcpy(buffer.data() + fdx.offset, &v, sizeof(v));
    //     }
    //     else if (fdx.type == ftF64)
    //     {
    //         f64 v = (f64)value;
    //         memcpy(buffer.data() + fdx.offset, &v, sizeof(v));
    //     }
    //     else if (fdx.type == ftU16n)
    //     {
    //         u16 v = (u16)(0xFFFF * (value - fdx.vmin) / (fdx.vmax - fdx.vmin));
    //     }
    //     else if (fdx.type == ftU8n)
    //     {
    //         u8 v = (u8)(0xFF * (value - fdx.vmin) / (fdx.vmax - fdx.vmin));
    //         memcpy(buffer.data() + fdx.offset, &v, sizeof(v));
    //     }

    //     else if (fdx.type == ftSTR || fdx.type == ftWSTR)
    //     {
    //         if(stringTableU32)
    //         {
    //             int v = (int)value;
    //             memcpy(buffer.data() + fdx.offset, &v, sizeof(v));
    //         }
    //         else
    //         {
    //             if(value>0xFFFF)
    //             {
    //                 warning("string index is greater than 0xFFFF with u16 string table index!");
    //             }
    //             u16 v = (u16)value;
    //             memcpy(buffer.data() + fdx.offset, &v, sizeof(v));
    //         }
    //     }
    //     else
    //     {
    //         assert(false);
    //     }
    // }

    void FastVectorDbLayerBuild::Impl::setField(unsigned ix, double value)
    {
        if (ix >= m_field_descs.size())
            return;
        auto &fdx = m_field_descs[ix];
        set_field_value_t(m_current_line_buffer.data(), fdx, value);
    }

    void FastVectorDbLayerBuild::Impl::setField(unsigned ix, int value)
    {
        if (ix >= m_field_descs.size())
            return;
        auto &fdx = m_field_descs[ix];
        set_field_value_t(m_current_line_buffer.data(), fdx, value);
    }

    void FastVectorDbLayerBuild::Impl::setField(unsigned ix, const char *text)
    {
        if (ix >= m_field_descs.size())
            return;
        auto &fdx = m_field_descs[ix];
        if (fdx.type != ftSTR)
            return;
        if (!text)
            text = "";
        auto it = m_string_map.find(text);
        int id = -1;
        if (it == m_string_map.end())
        {
            id = m_string_table.size();
            auto pstr = new string(text);
            m_string_table.push_back(pstr);
            m_string_map[text] = id;
            m_string_total_size += pstr->size() + 1;
        }
        else
        {
            id = it->second;
        }
        set_field_value_t(m_current_line_buffer.data(), fdx, id,m_string_table_u32);
    }
    void FastVectorDbLayerBuild::Impl::setField(unsigned ix, const wchar_t *text)
    {
        if (ix >= m_field_descs.size())
            return;
        auto &fdx = m_field_descs[ix];
        if (fdx.type != ftWSTR)
            return;
        if (!text)
            text = L"";
        wstring wtext(text);
        auto it = m_wstring_map.find(wtext);
        int id = -1;
        if (it == m_wstring_map.end())
        {
            id = m_wstring_table.size();
            auto pwstr = new wstring(wtext);
            m_wstring_table.push_back(pwstr);
            m_wstring_map[wtext] = id;
            m_wstring_total_size += pwstr->size() * 2 + 2;
        }
        else
        {
            id = it->second;
        }
        set_field_value_t(m_current_line_buffer.data(), fdx, id,m_string_table_u32);
    }
    void FastVectorDbLayerBuild::Impl::post()
    {
        printf("\nlayer [%s] has been created with the fellowing params:\n\
geometry type:%s, coord format:%s, aabbox:%s\n\
extent:(%lf,%lf,%lf,%lf)\n\
feature count:%ld\n\
string table:%s\n",
                m_name.c_str(),
                get_geometry_type_name(m_geometry_type),
                get_coord_type_name(m_coord_format),
                m_aabbox_enable?"yes":"no",
                m_minx,m_miny,m_maxx,m_maxy,
                m_feature_count,
                m_string_table_u32?"u32":"u16"); 
        for(unsigned ix=0;ix<m_field_descs.size();ix++)
        {
            field_desc_ex_t *fd=&m_field_descs[ix];
            if(fd->type==ftU8n||fd->type==ftU16n)
            {
                printf("field[%d]: %s \ttype:%s range:(%lf,%lf)\n",
                    ix,
                    fd->name,
                    get_field_type_name(fd->type),
                    fd->vmin,
                    fd->vmax);
            }
            else
            {
                printf("field[%d]: %s \ttype:%s\n",
                    ix,
                    fd->name,
                    get_field_type_name(fd->type));
                
            }
        }
    }

    void   FastVectorDbLayerBuild::Impl::setField(unsigned ix,const FastVectorDbFeatureRef* ref)
    {
        if (ix >= m_field_descs.size())
            return;
        auto &fdx = m_field_descs[ix];
        if (fdx.type != ftFeatureRef)
            return;
        
        memcpy(m_current_line_buffer.data() + fdx.offset, ref, sizeof(FastVectorDbFeatureRef));
    }
    
    FastVectorDbFeatureRef* FastVectorDbLayerBuild::Impl::createFeatureRef(u32 ix)
    {   
        if(ix==-1)
        {
            ix = m_feature_count-1;//using the last one as feature ref
        }
        auto ref =  FastVectorDbFeatureRef::make_ref(m_index_in_db,ix);
        m_created_feature_refs.push_back(ref);//he we should have a better way 
        return ref;
    }
    void   FastVectorDbLayerBuild::Impl::freeFeatureRef(FastVectorDbFeatureRef* ref)
    {
        //a dummy function for future use
    }
    
    void FastVectorDbLayerBuild::Impl::addFeatureEnd()
    {
        m_table_buffer.insert(m_table_buffer.end(), m_current_line_buffer.begin(), m_current_line_buffer.end());
        m_geometries_buffer.insert(m_geometries_buffer.end(), m_current_geom_buffer.begin(), m_current_geom_buffer.end());
        m_feature_count++;
#ifdef DEBUG
        if(m_feature_count%100==0)
        {
            printf(".");
        }
#endif
    }

    void FastVectorDbLayerBuild::Impl::truncate(unsigned nfeatures)
    {
        m_feature_count=nfeatures;
        m_table_buffer.resize(nfeatures * m_table_line_size);
        // TODO(Dsssyc): need to recalc geometry buffer size
    }
    size_t FastVectorDbLayerBuild::Impl::get_total_size()
    {
        return sizeof(layer_header_t) +
               m_field_descs.size() * sizeof(field_desc_ex_t) +
               m_geometries_buffer.size() +
               m_table_buffer.size() +
               sizeof(u32) + m_string_total_size +
               sizeof(u32) + m_wstring_total_size;
    }

    void FastVectorDbLayerBuild::Impl::write(WriteStream *stream)
    {
        layer_header_t lh;
        memset(&lh, 0, sizeof(lh));
        strcpy(lh.name, m_name.c_str());
        lh.feature_count = (u32)m_feature_count;
        lh.geometry_type = (u16)m_geometry_type;
        lh.field_count = (u16)m_field_descs.size();
        lh.coord_format = (u16)m_coord_format;
        lh.minx = m_minx;
        lh.miny = m_miny;
        lh.maxx = m_maxx;
        lh.maxy = m_maxy;
        lh.aabbox_enable=m_aabbox_enable;
        lh.string_table_u32=m_string_table_u32;
        lh.offset_table = /*sizeof(lh) + m_field_descs.size() * sizeof(field_desc_t) +*/ m_geometries_buffer.size();
        lh.offset_strings = lh.offset_table + m_table_buffer.size();
        lh.offset_wstrings = lh.offset_strings + sizeof(u32) + m_string_total_size;
        lh.total_size = get_total_size();
        stream->write(&lh, sizeof(lh));
        for (auto &fd : m_field_descs)
        {
            stream->write(&fd, sizeof(field_desc_ex_t));
        }
        if (m_geometries_buffer.size() > 0)
            stream->write(m_geometries_buffer.data(), m_geometries_buffer.size());
        if (m_table_buffer.size() > 0)
            stream->write(m_table_buffer.data(), m_table_buffer.size());
        u32 str_count = (u32)m_string_table.size();
        stream->write(&str_count, sizeof(str_count));
        for (auto pstr : m_string_table)
        {
            stream->write((void *)pstr->c_str(), pstr->size() + 1);
        }
        u32 wstr_count = (u32)m_wstring_table.size();
        stream->write(&wstr_count, sizeof(wstr_count));
        for (auto pwstr : m_wstring_table)
        {
            if (sizeof(wchar_t) != 2)
            {
                // need convert to u16
                static vector<u16> pbuf;
                pbuf.resize(pwstr->size() + 1);
                memset(pbuf.data(), 0, (pwstr->size() + 1) * 2);
                for (int i = 0; i < pwstr->size(); i++)
                {
                    pbuf[i] = (u16)pwstr->at(i);
                }
                pbuf[pwstr->size()] = 0;
                stream->write((void *)pbuf.data(), (pwstr->size() + 1) * 2);
            }
            else
            {
                stream->write((void *)pwstr->c_str(), (pwstr->size() + 1) * 2);
            }
        }
    }

        FastVectorDbLayerBuild::FastVectorDbLayerBuild(FastVectorDbBuild* db,const char* name)
        {
            impl = new FastVectorDbLayerBuild::Impl(db,name);
        }
        FastVectorDbLayerBuild::~FastVectorDbLayerBuild()
        {
            delete impl;
        }
        const char* FastVectorDbLayerBuild::name()
        {
            return impl->name();
        }
        int    FastVectorDbLayerBuild::addField(const char* name,unsigned ft,double vmin,double vmax)
        {
            return impl->addField(name,ft,vmin,vmax);
        }
        void   FastVectorDbLayerBuild::setGeometryType(GeometryLikeEnum gt,CoordinateFormatEnum ct,bool aabboxEnabled)
        {
            impl->setGeometryType(gt,ct,aabboxEnabled);
        }
        void   FastVectorDbLayerBuild::enableStringTableU32(bool b)
        {
            impl->enableStringTableU32(b);
        }
        void   FastVectorDbLayerBuild::setExtent(double minx,double miny,double maxx,double maxy)
        {
            impl->setExtent(minx,miny,maxx,maxy);
        }
        void   FastVectorDbLayerBuild::setDbIndex(int ix)
        {
            impl->setDbIndex(ix);
        }
        void   FastVectorDbLayerBuild::addFeatureBegin()
        {
            impl->addFeatureBegin();
        }
        void   FastVectorDbLayerBuild::setGeometry(void* data,size_t size,GeometryLikeFormat fmt)
        {
            impl->setGeometry((const char*)data,size,fmt);
        }
        void   FastVectorDbLayerBuild::setField(unsigned ix,double value)
        {
            impl->setField(ix,value);
        }
        void   FastVectorDbLayerBuild::setField(unsigned ix,int    value)
        {
            impl->setField(ix,value);
        }
        void   FastVectorDbLayerBuild::setField(unsigned ix,const char* text)
        {
            impl->setField(ix,text);
        }
        void   FastVectorDbLayerBuild::setField(unsigned ix,const wchar_t* text)
        {
            impl->setField(ix,text);
        }
        void   FastVectorDbLayerBuild::addFeatureEnd()
        {
            impl->addFeatureEnd();
        }

        void   FastVectorDbLayerBuild::setField(unsigned ix,const FastVectorDbFeatureRef* ref)
        {
            impl->setField(ix,ref);
        }
        FastVectorDbFeatureRef* FastVectorDbLayerBuild::createFeatureRef(u32 ix)
        {
            return impl->createFeatureRef(ix);
        }
        void   FastVectorDbLayerBuild::freeFeatureRef(FastVectorDbFeatureRef* ref)
        {
            impl->freeFeatureRef(ref);
        }

}