#include "FastVectorDbLayerBuild_p.h"
#include "fastdb.h"
#include "fastdb-geometry-utils.h"
#include "gaiageo.h"
namespace wx
{
    namespace
    {
        constexpr u64 kMaxStringFieldBytes = 0xFFFFFFFFull;

        size_t field_type_storage_size(u32 ft, bool string_table_u32)
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
                return string_table_u32 ? 4 : 2;
            case ftFeatureRef:
                return sizeof(FastVectorDbFeatureRef);
            case ftList:
                return 8;
            }
            assert(false);
            return 0;
        }

        bool has_plain_string_field(const vector<field_desc_ex_t>& field_descs)
        {
            for (const auto& fd : field_descs)
            {
                if (fd.type == ftSTR)
                    return true;
            }
            return false;
        }

        template <typename TStringFields>
        auto* find_string_field(TStringFields& string_fields, unsigned field_id)
        {
            for (auto& field : string_fields)
            {
                if (field.field_id == field_id)
                    return &field;
            }
            return static_cast<typename TStringFields::value_type*>(nullptr);
        }

        template <typename TStringFields>
        void register_varlen_string_field(TStringFields& string_fields, u32 field_id)
        {
            string_fields.push_back(typename TStringFields::value_type{
                field_id, 1, {0}, {}
            });
        }

        template <typename TStringFields>
        void activate_varlen_string_columns(vector<field_desc_ex_t>& field_descs,
                                            size_t& table_line_size,
                                            TStringFields& string_fields,
                                            bool& enable_varlen_string_columns,
                                            bool string_table_u32)
        {
            enable_varlen_string_columns = true;
            table_line_size = 0;
            string_fields.clear();
            for (u32 ix = 0; ix < (u32)field_descs.size(); ++ix)
            {
                auto& fd = field_descs[ix];
                fd.offset = table_line_size;
                fd.size = (fd.type == ftSTR) ? 0 : field_type_storage_size(fd.type, string_table_u32);
                if (fd.type == ftSTR)
                    register_varlen_string_field(string_fields, ix);
                table_line_size += fd.size;
            }
        }

        template <typename TStringField>
        bool validate_string_field_for_write(const TStringField& sfd, size_t feature_count)
        {
            assert(sfd.data.size() <= kMaxStringFieldBytes);
            if (sfd.data.size() > kMaxStringFieldBytes)
                return false;
            assert(sfd.offsets.size() <= kMaxStringFieldBytes);
            if (sfd.offsets.size() > kMaxStringFieldBytes)
                return false;
            assert(sfd.offsets.size() == feature_count + 1);
            if (sfd.offsets.size() != feature_count + 1)
                return false;
            assert(!sfd.offsets.empty());
            if (sfd.offsets.empty())
                return false;
            assert(sfd.offsets.back() == sfd.data.size());
            if (sfd.offsets.back() != sfd.data.size())
                return false;
            return true;
        }
    }

    FastVectorDbLayerBuild::Impl::Impl(FastVectorDbBuild* db,const char *name)
    {
        m_name = name;
        m_feature_count = 0;
        m_string_total_size = 0;
        m_wstring_total_size = 0;
        m_string_map.reserve(1 << 17);  // 131072 slots: avoids rehashes for up to ~65K unique strings
        m_wstring_map.reserve(64);
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
        // m_string_table and m_wstring_table store pointers into the maps' keys.
        // The maps own the memory; no manual deletion needed here.
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
        return field_type_storage_size(ft, m_string_table_u32);
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
        bool use_varlen_string = (ft == ftSTR && m_enable_varlen_string_columns);
        fd.size = use_varlen_string ? 0 : field_type_byte_size(ft);
        if (m_field_descs.size() > 0)
        {
            fd.offset = m_field_descs.back().offset + m_field_descs.back().size;
        }
        else
        {
            fd.offset = 0;
        }
        m_field_descs.push_back(fd);
        if (use_varlen_string)
            register_varlen_string_field(m_string_fields, (u32)(m_field_descs.size() - 1));
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
        if (fdx.size == 0 && find_string_field(m_string_fields, ix) != nullptr)
        {
            setFieldStringView(ix, text, (unsigned)strlen(text));
            return;
        }
        // try_emplace: single hash lookup (find+insert in one op), value = next ID if inserted.
        // Storing pointer-to-map-key avoids the separate `new string(text)` heap allocation.
        auto [it, inserted] = m_string_map.try_emplace(text, (int)m_string_table.size());
        if (inserted) {
            m_string_table.push_back(&it->first);
            m_string_total_size += it->first.size() + 1;
        }
        set_field_value_t(m_current_line_buffer.data(), fdx, it->second, m_string_table_u32);
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
        auto [it, inserted] = m_wstring_map.try_emplace(wtext, (int)m_wstring_table.size());
        if (inserted) {
            m_wstring_table.push_back(&it->first);
            m_wstring_total_size += it->first.size() * 2 + 2;
        }
        set_field_value_t(m_current_line_buffer.data(), fdx, it->second, m_string_table_u32);
    }
    void FastVectorDbLayerBuild::Impl::setFieldStringView(unsigned ix, const char* data, unsigned len)
    {
        if (ix >= m_field_descs.size())
            return;
        auto& fd = m_field_descs[ix];
        if (fd.type != ftSTR)
            return;
        auto* sfd = find_string_field(m_string_fields, ix);
        if (sfd == nullptr)
            return;
        assert(data != nullptr || len == 0);
        if (data == nullptr && len > 0)
            return;
        u64 next_size = (u64)sfd->data.size() + (u64)len;
        assert(next_size <= kMaxStringFieldBytes);
        if (next_size > kMaxStringFieldBytes)
            return;
        if (data != nullptr && len > 0)
        {
            const u8* p = reinterpret_cast<const u8*>(data);
            sfd->data.insert(sfd->data.end(), p, p + len);
        }
        sfd->offsets.push_back((u32)sfd->data.size());
    }
    void FastVectorDbLayerBuild::Impl::setStringColumnBulk(unsigned field_id, const u32* offsets, unsigned n_offsets, const u8* data, u64 nbytes)
    {
        auto* sfd = find_string_field(m_string_fields, field_id);
        if (sfd == nullptr)
            return;
        assert(n_offsets == m_feature_count + 1);
        if (n_offsets != m_feature_count + 1)
            return;
        assert(nbytes <= kMaxStringFieldBytes);
        if (nbytes > kMaxStringFieldBytes)
            return;
        assert(offsets != nullptr || n_offsets == 0);
        if (offsets == nullptr)
            return;
        assert(offsets[0] == 0);
        if (offsets[0] != 0)
            return;
        for (unsigned i = 1; i < n_offsets; ++i)
        {
            assert(offsets[i - 1] <= offsets[i]);
            if (offsets[i - 1] > offsets[i])
                return;
        }
        assert((u64)offsets[n_offsets - 1] == nbytes);
        if ((u64)offsets[n_offsets - 1] != nbytes)
            return;
        assert(data != nullptr || nbytes == 0);
        if (data == nullptr && nbytes > 0)
            return;
        sfd->offsets.assign(offsets, offsets + n_offsets);
        sfd->data.assign(data, data + nbytes);
    }
    void FastVectorDbLayerBuild::Impl::setNumericColumnBulk(unsigned field_id, const void* data, u64 nbytes)
    {
        assert(field_id < m_field_descs.size());
        if (field_id >= m_field_descs.size())
            return;
        auto& fd = m_field_descs[field_id];
        assert(fd.type != ftSTR && fd.type != ftWSTR && fd.type != ftList && fd.type != ftFeatureRef);
        if (fd.type == ftSTR || fd.type == ftWSTR || fd.type == ftList || fd.type == ftFeatureRef)
            return;
        assert(fd.size > 0);
        assert(data != nullptr || nbytes == 0);
        if (fd.size == 0 || data == nullptr || nbytes == 0)
            return;
        assert(nbytes == (u64)m_feature_count * (u64)fd.size);
        if (nbytes != (u64)m_feature_count * (u64)fd.size)
            return;
        const u8* src = reinterpret_cast<const u8*>(data);
        for (size_t row = 0; row < m_feature_count; ++row)
            memcpy(m_table_buffer.data() + (u64)row * m_table_line_size + fd.offset, src + (u64)row * fd.size, fd.size);
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

    // Element sizes indexed by FieldTypeEnum value
    static const u32 kListElemSizes[] = {
        0,                                  // 0 unused
        1, 2, 4, 4,                         // u8=1, u16=2, u32=3, i32=4
        1, 2,                               // u8n=5, u16n=6
        4, 8,                               // f32=7, f64=8
        0, 0,                               // str=9, wstr=10 (unsupported)
        sizeof(FastVectorDbFeatureRef),     // ref=11
    };

    void FastVectorDbLayerBuild::Impl::add_list_field(const char* name, u16 element_type)
    {
        // addField returns 1-based count; field_id = count - 1
        int fid = addField(name, ftList, 0, 0) - 1;
        m_field_descs.back().element_type = element_type;

        u32 esz = (element_type < (u16)(sizeof(kListElemSizes)/sizeof(kListElemSizes[0])))
                  ? kListElemSizes[element_type] : 0;
        assert(esz > 0 && "unsupported list element type");

        ListFieldBuildData lfd;
        lfd.field_id  = fid;
        lfd.elem_size = esz;
        m_list_fields.push_back(lfd);
    }

    void FastVectorDbLayerBuild::Impl::set_field_list_numeric(int field_id, const void* data, u32 nbytes)
    {
        for (auto& lfd : m_list_fields) {
            if (lfd.field_id != field_id) continue;
            u32 start = (u32)(lfd.data.size() / lfd.elem_size);
            u32 count = (lfd.elem_size > 0) ? nbytes / lfd.elem_size : 0;
            memcpy(m_current_line_buffer.data() + m_field_descs[field_id].offset,     &start, 4);
            memcpy(m_current_line_buffer.data() + m_field_descs[field_id].offset + 4, &count, 4);
            const u8* src = (const u8*)data;
            lfd.data.insert(lfd.data.end(), src, src + nbytes);
            return;
        }
    }

    void FastVectorDbLayerBuild::Impl::set_field_list_refs(int field_id, const FastVectorDbFeatureRef* refs, u32 count)
    {
        set_field_list_numeric(field_id, refs, count * (u32)sizeof(FastVectorDbFeatureRef));
    }

    void FastVectorDbLayerBuild::Impl::update_feature_ref(u32 feat_idx, int field_id, const FastVectorDbFeatureRef* ref)
    {
        assert(field_id >= 0 && field_id < (int)m_field_descs.size());
        const field_desc_ex_t& fd = m_field_descs[field_id];
        assert(fd.type == ftFeatureRef);
        u8* row = m_table_buffer.data() + feat_idx * m_table_line_size;
        memcpy(row + fd.offset, ref, sizeof(FastVectorDbFeatureRef));
    }

    void FastVectorDbLayerBuild::Impl::update_list_ref_at(u32 feat_idx, int field_id, u32 list_idx, const FastVectorDbFeatureRef* ref)
    {
        for (auto& lfd : m_list_fields) {
            if (lfd.field_id != field_id) continue;
            const u8* row = m_table_buffer.data() + feat_idx * m_table_line_size;
            u32 start;
            memcpy(&start, row + m_field_descs[field_id].offset, 4);
            u32 abs_idx = start + list_idx;
            assert((u64)abs_idx * lfd.elem_size + lfd.elem_size <= lfd.data.size());
            memcpy(lfd.data.data() + (u64)abs_idx * lfd.elem_size, ref, sizeof(FastVectorDbFeatureRef));
            return;
        }
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
        if (!m_enable_varlen_string_columns && has_plain_string_field(m_field_descs))
        {
            activate_varlen_string_columns(
                m_field_descs,
                m_table_line_size,
                m_string_fields,
                m_enable_varlen_string_columns,
                m_string_table_u32);
        }
        m_feature_count=nfeatures;
        m_table_buffer.resize(nfeatures * m_table_line_size);
        for (auto& sfd : m_string_fields)
        {
            sfd.offsets.assign(nfeatures + 1, 0);
            sfd.data.clear();
        }
        // TODO(Dsssyc): need to recalc geometry buffer size
    }
    size_t FastVectorDbLayerBuild::Impl::get_total_size()
    {
        size_t list_section_size = 0;
        for (const auto& lfd : m_list_fields) {
            list_section_size += sizeof(u32) + sizeof(u32) + sizeof(u64); // field_index + elem_size + total_elements
            list_section_size += lfd.data.size();
        }
        size_t string_section_size = 0;
        for (const auto& sfd : m_string_fields) {
            string_section_size += sizeof(u32) + sizeof(u32) + sizeof(u32) + sizeof(u64);
            string_section_size += sfd.offsets.size() * sizeof(u32);
            string_section_size += sfd.data.size();
        }
        return sizeof(layer_header_t) +
               m_field_descs.size() * sizeof(field_desc_ex_t) +
               m_geometries_buffer.size() +
               m_table_buffer.size() +
               sizeof(u32) + m_string_total_size +
               sizeof(u32) + m_wstring_total_size +
               list_section_size +
               string_section_size;
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
        lh.n_list_fields = (u16)m_list_fields.size();
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
        // List data section
        for (const auto& lfd : m_list_fields) {
            u32 field_index  = (u32)lfd.field_id;
            u32 elem_size    = lfd.elem_size;
            u64 total_elems  = (u64)(lfd.elem_size > 0 ? lfd.data.size() / lfd.elem_size : 0);
            stream->write(&field_index, sizeof(field_index));
            stream->write(&elem_size,   sizeof(elem_size));
            stream->write(&total_elems, sizeof(total_elems));
            if (!lfd.data.empty())
                stream->write((void*)lfd.data.data(), lfd.data.size());
        }
        // String sections follow list sections so later reader work can scan trailing column payloads in order.
        for (const auto& sfd : m_string_fields) {
            if (!validate_string_field_for_write(sfd, m_feature_count))
                return;
            stream->write((void*)&sfd.field_id, sizeof(u32));
            stream->write((void*)&sfd.codec, sizeof(u32));
            u32 offset_count = (u32)sfd.offsets.size();
            u64 byte_count = (u64)sfd.data.size();
            stream->write(&offset_count, sizeof(offset_count));
            stream->write(&byte_count, sizeof(byte_count));
            stream->write((void*)sfd.offsets.data(), offset_count * sizeof(u32));
            if (!sfd.data.empty()) stream->write((void*)sfd.data.data(), sfd.data.size());
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
        void FastVectorDbLayerBuild::setFieldStringView(unsigned ix, const char* data, unsigned len)
        {
            impl->setFieldStringView(ix, data, len);
        }
        void FastVectorDbLayerBuild::setNumericColumnBulk(unsigned field_id, const void* data, u64 nbytes)
        {
            impl->setNumericColumnBulk(field_id, data, nbytes);
        }
        void FastVectorDbLayerBuild::setStringColumnBulk(unsigned field_id, const u32* offsets, unsigned n_offsets, const u8* data, u64 nbytes)
        {
            impl->setStringColumnBulk(field_id, offsets, n_offsets, data, nbytes);
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
        void FastVectorDbLayerBuild::add_list_field(const char* name, unsigned element_type)
        { impl->add_list_field(name, (u16)element_type); }
        void FastVectorDbLayerBuild::set_field_list_numeric(unsigned fid, const void* data, unsigned count)
        { impl->set_field_list_numeric((int)fid, data, (u32)count); }
        void FastVectorDbLayerBuild::set_field_list_refs(unsigned fid, const FastVectorDbFeatureRef* refs, unsigned count)
        { impl->set_field_list_refs((int)fid, refs, (u32)count); }
        void FastVectorDbLayerBuild::update_feature_ref(unsigned feat_idx, unsigned fid, const FastVectorDbFeatureRef* ref)
        { impl->update_feature_ref((u32)feat_idx, (int)fid, ref); }
        void FastVectorDbLayerBuild::update_list_ref_at(unsigned feat_idx, unsigned fid, unsigned list_idx, const FastVectorDbFeatureRef* ref)
        { impl->update_list_ref_at((u32)feat_idx, (int)fid, (u32)list_idx, ref); }

}
