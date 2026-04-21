#include "fastdb.h"
#include "FastVectorDbLayer_p.h"
#include "FastVectorDbLayerBuild_p.h"
namespace wx
{
    namespace
    {
        template <typename TStringFields>
        auto* find_string_field(TStringFields& string_fields, u32 field_id)
        {
            for (auto& field : string_fields)
            {
                if (field.field_id == field_id)
                    return &field;
            }
            return static_cast<typename TStringFields::value_type*>(nullptr);
        }

        u16 count_varlen_string_fields(const field_desc_ex_t* field_descs, u16 field_count)
        {
            u16 count = 0;
            for (u16 ix = 0; ix < field_count; ++ix)
            {
                const auto& field = field_descs[ix];
                if (field.type == ftSTR && field.size == 0)
                    count++;
            }
            return count;
        }
    }

    size_t ustring_len(const uchar_t *str)
    {
        size_t len = 0;
        while (str[len] != 0)
            len++;
        return len;
    }

    class FastVectorDbFeature::Impl
    { 
    public:
        FastVectorDbLayer*     layer;
        u32                    ifeature;
    };

    FastVectorDbLayer::Impl::Impl(u8 *pdata, size_t size)
        :m_data(pdata), m_size(size), m_ifeature(-1)
    {
         m_header = (layer_header_t *)m_data;
        m_field_descs = (field_desc_ex_t *)(m_data + sizeof(layer_header_t));
        m_data_ptr0 = m_data + sizeof(layer_header_t) + m_header->field_count * sizeof(field_desc_ex_t);
        auto last_fd = m_field_descs + m_header->field_count - 1;
        m_table_line_size = last_fd->offset + last_fd->size;
        m_table_data_ptr0 = m_data_ptr0 + m_header->offset_table;
        m_geometry_ptr0=m_data_ptr0;
        // load string tables
        const u8 *ptr = m_data_ptr0 + m_header->offset_strings;
        u32 count = *(u32 *)ptr;
        ptr += 4;
        for (int i = 0; i < count; i++)
        {
            m_string_table.push_back((const char *)ptr);
            ptr += strlen((const char *)ptr) + 1;
        }
        ptr = m_data_ptr0 + m_header->offset_wstrings;
        count = *(u32 *)ptr;
        ptr += 4;
        for (int i = 0; i < count; i++)
        {
            m_wstring_table.push_back((const uchar_t *)ptr);
            ptr += (ustring_len((const uchar_t *)ptr) + 1) * sizeof(uchar_t);
        }
        // parse list data section (n_list_fields > 0 only in databases built with list support)
        if (m_header->n_list_fields > 0) {
            for (u16 li = 0; li < m_header->n_list_fields; li++) {
                ListFieldData lfd;
                lfd.field_id       = *(u32*)ptr; ptr += 4;
                lfd.elem_size      = *(u32*)ptr; ptr += 4;
                lfd.total_elements = *(u64*)ptr; ptr += 8;
                lfd.data_ptr       = const_cast<u8*>(ptr);
                ptr += lfd.total_elements * lfd.elem_size;
                m_list_fields.push_back(lfd);
            }
        }
        u16 string_field_count = count_varlen_string_fields(m_field_descs, m_header->field_count);
        for (u16 si = 0; si < string_field_count; ++si)
        {
            StringFieldData sfd;
            sfd.field_id = *(u32*)ptr; ptr += 4;
            sfd.codec = *(u32*)ptr; ptr += 4;
            sfd.offset_count = *(u32*)ptr; ptr += 4;
            sfd.byte_count = *(u64*)ptr; ptr += 8;
            sfd.offsets_ptr = sfd.offset_count ? (u32*)ptr : nullptr;
            ptr += size_t(sfd.offset_count) * sizeof(u32);
            sfd.data_ptr = sfd.byte_count ? const_cast<u8*>(ptr) : nullptr;
            ptr += size_t(sfd.byte_count);
            m_string_fields.push_back(sfd);
        }
    }
    FastVectorDbLayer::Impl::~Impl() {
        if(m_feature_cache.size())
        {
            for (auto it = m_feature_cache.begin(); it != m_feature_cache.end(); it++)
            {
                if(*it!=nullptr)
                    delete *it;
            }
        }
    }
    const char *FastVectorDbLayer::Impl::name()
    {
        return m_header->name;
    }
    unsigned FastVectorDbLayer::Impl::getFieldCount()
    {
        return m_header->field_count;
    }
    const char *FastVectorDbLayer::Impl::getFieldDefn(unsigned ix, FieldTypeEnum &ft, double &vmin, double &vmax)
    {
        if (ix >= m_header->field_count)
            return nullptr;
        const field_desc_ex_t *fd = m_field_descs + ix;
        ft = (FieldTypeEnum)fd->type;
        vmin = fd->vmin;
        vmax = fd->vmax;
        return fd->name;
    }
    GeometryLikeEnum FastVectorDbLayer::Impl::getGeometryType()
    {
        return (GeometryLikeEnum)m_header->geometry_type;
    }
    void FastVectorDbLayer::Impl::rewind()
    {
        m_ifeature = -1;
        m_geometry_ptr = m_geometry_ptr0;
    }

    template <class coord_type_t>
    size_t FastVectorDbLayer::Impl::get_geometry_byte_size(const u8 *geom_ptr, GeometryLikeEnum geomType)
    {
        size_t move_bytes = 0;
        if (geomType == gtPoint)
        {
            move_bytes += sizeof(coord_type_t);
        }
        else
        {
            if(m_header->aabbox_enable){
                geom_ptr+=sizeof(aabbox_x16_t);
                move_bytes+=sizeof(aabbox_x16_t);
            }
            u16 npart = *(u16 *)geom_ptr;
            move_bytes+= sizeof(u16);
            geom_ptr += sizeof(u16);
            for (int i = 0; i < npart; i++)
            {
                geom_ptr += sizeof(u8); // parttype
                move_bytes += sizeof(u8);
                u16 npoint = *(u16 *)geom_ptr;
                move_bytes += sizeof(u16);
                geom_ptr += sizeof(u16);
                move_bytes += npoint * sizeof(coord_type_t);
                geom_ptr += npoint * sizeof(coord_type_t);
            }
        }
        return move_bytes;
    }
    void FastVectorDbLayer::Impl::move_next_geometry_ptr()
    {
        size_t move_bytes = get_geometry_like_size(m_geometry_ptr);
        m_geometry_ptr = m_geometry_ptr + move_bytes;
    }
    size_t FastVectorDbLayer::Impl::get_geometry_like_size(const u8* pdata)
    {
        size_t move_bytes = 0;
        if(m_header->geometry_type==gtAny)
        {
            move_bytes=*(u32*)m_geometry_ptr+sizeof(u32); 
        }
        else if (m_header->coord_format == cfF64)
        {
            move_bytes = get_geometry_byte_size<point2_t>(pdata, (wx::GeometryLikeEnum)m_header->geometry_type);
        }
        else if (m_header->coord_format == cfF32)
        {
            move_bytes = get_geometry_byte_size<point2_f32_t>(pdata, (wx::GeometryLikeEnum)m_header->geometry_type);
        }
        else if (m_header->coord_format == cfTx16)
        {
            move_bytes = get_geometry_byte_size<point2_x16_t>(pdata, (wx::GeometryLikeEnum)m_header->geometry_type);
        }
        else if (m_header->coord_format == cfTx24)
        {
            move_bytes = get_geometry_byte_size<point2_x24_t>(pdata, (wx::GeometryLikeEnum)m_header->geometry_type);
        }
        else if (m_header->coord_format == cfTx32)
        {
            move_bytes = get_geometry_byte_size<point2_x32_t>(pdata, (wx::GeometryLikeEnum)m_header->geometry_type);
        }
        return move_bytes;
    }

    int FastVectorDbLayer::Impl::row()
    {
        return m_ifeature;
    }
    u32 FastVectorDbLayer::Impl::getFeatureCount()
    {
        return m_header->feature_count;
    }
    void  FastVectorDbLayer::Impl::getExtent(double& minx,double& miny,double& maxx,double& maxy)
    {
        minx = m_header->minx;
        miny = m_header->miny;
        maxx = m_header->maxx;
        maxy = m_header->maxy;
    }

    bool FastVectorDbLayer::Impl::next()
    {
        if(m_ifeature == -1)
            m_ifeature = 0;
        else
            m_ifeature++;

        if (m_ifeature > ((int)m_header->feature_count)-1)
            return false;
    
        if (m_ifeature == 0)
        {
            m_geometry_ptr = m_geometry_ptr0;
        }
        else
        {
            move_next_geometry_ptr();
        }
        return true;
    }
    //__attribute__((thread)) vector<point2_t> points;
    template <class coord_type_t>
    class FastVectorDbLayer::Impl::return_geometry
    {
        vector<point2_t>& points;
    public:
        return_geometry(vector<point2_t>& _points,FastVectorDbLayer::Impl &impl, GeometryReturn *cb, const u8 *geom_ptr, GeometryLikeEnum geomType)
            :points(_points)
        {
            points.clear();
            if (geomType == gtPoint)
            {
                coord_type_t c = *(coord_type_t *)geom_ptr;
                point2_t p;
                impl.convert_coord_format(c, p);
                points.push_back(p);
                cb->begin(NULL);
                cb->returnGeomrtryPart(gptPoint2, points.data(), 1);
                cb->end();
            }
            else
            {
                aabbox_t aabbox;
                aabbox_x16_t aabbox16;
                double* boxptr=NULL;
                if(impl.m_header->aabbox_enable)
                {
                    aabbox16=*(aabbox_x16_t*)geom_ptr;
                    geom_ptr+=sizeof(aabbox_x16_t);
                    impl.convert_coord_format(aabbox16.minEdge,aabbox.minEdge);
                    impl.convert_coord_format(aabbox16.maxEdge,aabbox.maxEdge);
                    boxptr=&aabbox.minEdge.x;
                }

                u16 npart = *(u16 *)geom_ptr;
                geom_ptr += sizeof(u16);
                if(cb->begin(boxptr))
                {
                    for (int i = 0; i < npart; i++)
                    {
    GeometryReturn::GeometryPartEnum partType = (GeometryReturn::GeometryPartEnum) * (u8 *)geom_ptr;
    geom_ptr += sizeof(u8);
    u16 npoint = *(u16 *)geom_ptr;
    geom_ptr += sizeof(u16);
    points.clear();
    for (int j = 0; j < npoint; j++)
    {
        coord_type_t c = *(coord_type_t *)geom_ptr;
        geom_ptr += sizeof(coord_type_t);
        point2_t p;
        impl.convert_coord_format(c, p);
        points.push_back(p);
    }
    cb->returnGeomrtryPart(partType, points.data(), npoint);
                    }
                    cb->end();
                }
            }
        }
        GeometryReturn *cb;
        GeometryLikeEnum geomType;
        CoordinateFormatEnum coordFormat;
    };

    chunk_data_t FastVectorDbLayer::Impl::getGeometryLikeChunk()
    {
        chunk_data_t data;
        if(m_header->geometry_type==(u16)gtNone)
        {
            data.pdata=NULL;
            data.size=0;
        }
        else if(m_header->geometry_type==gtAny)
        {
            data.size=*(u32*)m_geometry_ptr;
            data.pdata=m_geometry_ptr+sizeof(u32);
        }
        else
        { 
            data.size=get_geometry_like_size(m_geometry_ptr); 
            data.pdata = m_geometry_ptr;
        }
        return data;
    }


    chunk_data_t FastVectorDbLayer::Impl::getGeometryLikeChunk_internal(u32 ifeature)
    {
        chunk_data_t data;
        u8* geometry_ptr = m_geometry_ptr_map[ifeature];
        if(m_header->geometry_type==(u16)gtNone)
        {
            data.pdata=NULL;
            data.size=0;
        }
        else if(m_header->geometry_type==gtAny)
        {
            data.size=*(u32*)geometry_ptr;
            data.pdata=geometry_ptr+sizeof(u32);
        }
        else
        { 
            data.size=get_geometry_like_size(geometry_ptr); 
            data.pdata = geometry_ptr;
        }
        return data;
    }
    void FastVectorDbLayer::Impl::fetchGeometry(GeometryReturn *cb)
    {
        fetchGeometry_internal(m_geometry_ptr,cb);
    }
    void FastVectorDbLayer::Impl::fetchGeometry_internal(const u8* geometry_data_ptr,GeometryReturn *cb)
    {
        // if (m_it < 0 || m_it >= (int)m_header->feature_count)
        //     return;
        if(m_header->geometry_type==gtAny)
        {
            //nothing todo for anyting data
        }
        else if (m_header->coord_format == cfF64)
        {
            return_geometry<point2_t> rg(points,*this, cb, geometry_data_ptr, (wx::GeometryLikeEnum)m_header->geometry_type);
        }
        else if (m_header->coord_format == cfF32)
        {
            return_geometry<point2_f32_t> rg(points,*this, cb, geometry_data_ptr, (wx::GeometryLikeEnum)m_header->geometry_type);
        }
        else if (m_header->coord_format == cfTx16)
        {
            return_geometry<point2_x16_t> rg(points,*this, cb, geometry_data_ptr, (wx::GeometryLikeEnum)m_header->geometry_type);
        }
        else if (m_header->coord_format == cfTx24)
        {
            return_geometry<point2_x24_t> rg(points,*this, cb, geometry_data_ptr, (wx::GeometryLikeEnum)m_header->geometry_type);
        }
        else if (m_header->coord_format == cfTx32)
        {
            return_geometry<point2_x32_t> rg(points,*this, cb, geometry_data_ptr, (wx::GeometryLikeEnum)m_header->geometry_type);
        }
    }

    void FastVectorDbLayer::Impl::fetchGeometry_internal(u32 ifeature,GeometryReturn *cb)
    {
        if(ifeature>=m_geometry_ptr_map.size())
            return;
        fetchGeometry_internal(m_geometry_ptr_map[ifeature],cb);
    }

    double FastVectorDbLayer::Impl::getFieldAsFloat(u32 ix)
    {
        return getFieldAsFloat_internal(m_ifeature,ix);
    }

    double FastVectorDbLayer::Impl::getFieldAsFloat_internal(u32 ifeature,u32 ix)
    {
        if (ix >= m_header->field_count||ifeature>=m_header->feature_count)
            return 0;
        const field_desc_ex_t *fd = m_field_descs + ix;
        const u8 *ptr = m_table_data_ptr0 + m_table_line_size*ifeature+fd->offset;
        switch (fd->type)
        {
        case ftF32:
            return *(f32 *)ptr;
        case ftF64:
            return *(f64 *)ptr;
        case ftU8n:
        {
            u8 v = *(u8 *)ptr;
            return fd->vmin + (fd->vmax - fd->vmin) * v / 255.0;
        }
        case ftU16n:
        {
            u16 v = *(u16 *)ptr;
            return fd->vmin + (fd->vmax - fd->vmin) * v / 65535.0;
        }
        case ftU8:  return *(u8  *)ptr;
        case ftU16: return *(u16 *)ptr;
        case ftU32: return *(u32 *)ptr;
        case ftI32: return (int)*(i32 *)ptr;  // i32 is unsigned int; cast to int for signed reading
        }
        return NAN;
    }

    int FastVectorDbLayer::Impl::getFieldAsInt(u32 ix)
    {
       return getFieldAsInt_internal(m_ifeature,ix);
    }
    int FastVectorDbLayer::Impl::getFieldAsInt_internal(u32 ifeature,u32 ix)
    {
        if (ix >= m_header->field_count||ifeature>=m_header->feature_count)
            return 0;
        const field_desc_ex_t *fd = m_field_descs + ix;
        const u8 *ptr = m_table_data_ptr0 + m_table_line_size*ifeature+fd->offset;
        switch (fd->type)
        {
        case ftU8:
            return *(u8 *)ptr;
        case ftU16:
            return *(u16 *)ptr;
        case ftU32:
            return *(u32 *)ptr;
        case ftI32:
            return *(i32 *)ptr;
        }
        return 0x0baddaf0; // a bad data mask
    }
    const char *FastVectorDbLayer::Impl::getFieldAsString_internal(u32 ifeature,u32 ix)
    {
        if (ix >= m_header->field_count)
            return nullptr;
        const field_desc_ex_t *fd = m_field_descs + ix;
        if (fd->type != ftSTR)
            return nullptr;
        const u8 *ptr = m_table_data_ptr0 + m_table_line_size*ifeature+fd->offset;
        u32 id =m_header->string_table_u32?(*(u32 *)ptr):(u32(*(u16*)ptr));
        if (id >= m_string_table.size())
            return nullptr;
        return m_string_table[id];
    }
    const char *FastVectorDbLayer::Impl::getFieldAsString(u32 ix)
    {
        return getFieldAsString_internal(m_ifeature,ix);
    }
    const uchar_t *FastVectorDbLayer::Impl::getFieldAsWString_internal(u32 ifeature,u32 ix)
    {
        if (ix >= m_header->field_count)
            return nullptr;
        const field_desc_ex_t *fd = m_field_descs + ix;
        if (fd->type != ftWSTR)
            return nullptr;
        const u8 *ptr = m_table_data_ptr0 +m_table_line_size*ifeature+fd->offset;
        u32 id =m_header->string_table_u32?(*(u32 *)ptr):(u32(*(u16*)ptr));
        if (id >= m_wstring_table.size())
            return nullptr;
        return m_wstring_table[id];
    }
    const uchar_t *FastVectorDbLayer::Impl::getFieldAsWString(u32 ix)
    {
        return getFieldAsWString_internal(m_ifeature,ix);
    }

    chunk_data_t FastVectorDbLayer::Impl::getFieldAsStringView_internal(u32 ifeature, u32 ix)
    {
        if (ix >= m_header->field_count || ifeature >= m_header->feature_count)
            return {0, nullptr};
        const field_desc_ex_t* fd = m_field_descs + ix;
        if (fd->type != ftSTR)
            return {0, nullptr};
        if (fd->size == 0)
        {
            auto* sfd = find_string_field(m_string_fields, ix);
            if (sfd == nullptr || ifeature + 1 >= sfd->offset_count)
                return {0, nullptr};
            u32 start = sfd->offsets_ptr[ifeature];
            u32 end = sfd->offsets_ptr[ifeature + 1];
            if (end < start || end > sfd->byte_count)
                return {0, nullptr};
            u8* data_ptr = sfd->data_ptr;
            return {(size_t)(end - start), data_ptr ? (data_ptr + start) : nullptr};
        }
        const char* text = getFieldAsString_internal(ifeature, ix);
        return text ? chunk_data_t{strlen(text), (u8*)text} : chunk_data_t{0, nullptr};
    }

    chunk_data_t FastVectorDbLayer::Impl::getFieldAsStringView(u32 ix)
    {
        return getFieldAsStringView_internal(m_ifeature, ix);
    }

    chunk_data_t FastVectorDbLayer::Impl::getStringColumnOffsets_internal(u32 ix)
    {
        if (ix >= m_header->field_count)
            return {0, nullptr};
        const field_desc_ex_t* fd = m_field_descs + ix;
        if (fd->type != ftSTR || fd->size != 0)
            return {0, nullptr};
        auto* sfd = find_string_field(m_string_fields, ix);
        if (sfd == nullptr)
            return {0, nullptr};
        return {size_t(sfd->offset_count) * sizeof(u32), (u8*)sfd->offsets_ptr};
    }

    chunk_data_t FastVectorDbLayer::Impl::getStringColumnData_internal(u32 ix)
    {
        if (ix >= m_header->field_count)
            return {0, nullptr};
        const field_desc_ex_t* fd = m_field_descs + ix;
        if (fd->type != ftSTR || fd->size != 0)
            return {0, nullptr};
        auto* sfd = find_string_field(m_string_fields, ix);
        if (sfd == nullptr)
            return {0, nullptr};
        return {(size_t)sfd->byte_count, sfd->data_ptr};
    }

    void* FastVectorDbLayer::Impl::setFeatureCookie_internal(u32 ifeature,void* cookie)
    {
        u32 featureCount = m_header->feature_count;
        if(ifeature<0||ifeature>featureCount-1)
        {
            return (void*)size_t(-1);
        }
        if(m_feature_cookie_map.size()==0)
        {
            m_feature_cookie_map.resize(featureCount);
            memset(m_feature_cookie_map.data(),0,sizeof(void*)*featureCount);
        }

        void* pre = m_feature_cookie_map[ifeature];
        m_feature_cookie_map[ifeature]=cookie;

        return pre;
    }
    void* FastVectorDbLayer::Impl::setFeatureCookie(void* cookie)
    {
        return setFeatureCookie_internal(m_ifeature,cookie);
    }   
    void* FastVectorDbLayer::Impl::getFeatureCookie_internal(u32 ifeature)
    {
        u32 featureCount = m_header->feature_count;
        if(m_feature_cookie_map.size()==0)
        {
            m_feature_cookie_map.resize(featureCount);
            memset(m_feature_cookie_map.data(),0,sizeof(void*)*featureCount);
        }
        if(ifeature<0||ifeature>m_feature_cookie_map.size()-1)
        {
            return (void*)size_t(-1);
        }
        return m_feature_cookie_map[ifeature];
    }
    void* FastVectorDbLayer::Impl::getFeatureCookie()
    {
        return getFeatureCookie_internal(m_ifeature);
    }
    FastVectorDbFeatureRef* FastVectorDbLayer::Impl::getFieldAsFeatureRef_internal(u32 ifeature,u32 ix)
    {
        if (ix >= m_header->field_count||m_field_descs[ix].type != ftFeatureRef)
            return nullptr;
        auto* p = m_table_data_ptr0 +m_table_line_size*ifeature+m_field_descs[ix].offset;

        return (FastVectorDbFeatureRef*)p;
    }
    FastVectorDbFeatureRef* FastVectorDbLayer::Impl::getFieldAsFeatureRef(u32 ix)
    {
        return getFieldAsFeatureRef_internal(m_ifeature,ix);
    }

    void    FastVectorDbLayer::Impl::setField_internal(u32 ifeature,u32 ix,double value)
    {
        if(ix >= m_header->field_count||ifeature>=m_header->feature_count)
            return;
        u8* buffer = (u8*)getFeatureAddress(ifeature); 
        set_field_value_t(buffer,m_field_descs[ix],value);
    }
    void    FastVectorDbLayer::Impl::setField_internal(u32 ifeature,u32 ix,int    value)
    {
        if(ix >= m_header->field_count||ifeature>=m_header->feature_count)
            return;
        u8* buffer = (u8*)getFeatureAddress(ifeature); 
        set_field_value_t(buffer,m_field_descs[ix],value);
    }

    void FastVectorDbLayer::Impl::setFeatureRef_internal(u32 ifeature, u32 ix, FastVectorDbFeature* feature)
    {
        if(ix >= m_header->field_count || ifeature >= m_header->feature_count)
            return;
        u8* buffer = (u8*)getFeatureAddress(ifeature);

        // Create a FastVectorDbFeatureRef from the FastVectorDbFeature
        auto iFeature = feature->impl->ifeature;
        auto iLayer = feature->impl->layer->impl->m_layer_index;
        auto ref = FastVectorDbFeatureRef::make(iLayer, iFeature);

        memcpy(buffer + m_field_descs[ix].offset, &ref, sizeof(FastVectorDbFeatureRef));
    }

    void*   FastVectorDbLayer::Impl::getFeatureAddress(u32 ifeature)
    {
        return (void*)(m_table_data_ptr0+ifeature*m_table_line_size);
    }
    /////////////////////////////////////////////////////////////

    FastVectorDbFeature*    FastVectorDbLayer::Impl::tryGetFeatureAt(u32 ix)
    {
        if(ix>=m_header->feature_count)
            return nullptr;
        if(m_feature_cache.size()==0)
        {   
            auto last_it = m_ifeature;
            auto geometry_ptr = m_geometry_ptr;
            m_feature_cache.resize(m_header->feature_count,NULL);
            rewind();
            while(next())
            {
                m_geometry_ptr_map.push_back(m_geometry_ptr);
            }
            m_geometry_ptr= geometry_ptr;
            m_ifeature = last_it;
        }
        if(m_feature_cache[ix]==NULL)
        {
            auto implx = new FastVectorDbFeature::Impl;
            implx->layer = this->m_layer;
            implx->ifeature = ix;
            m_feature_cache[ix] = new FastVectorDbFeature;
            m_feature_cache[ix]->impl = implx;
        }   
        return m_feature_cache[ix];  
    }

    size_t  FastVectorDbLayer::Impl::getFieldOffset(unsigned ix)
    {
        if(ix>=m_header->field_count)
            return -1;
        return  m_field_descs[ix].offset;
    }
    size_t  FastVectorDbLayer::Impl::getFeatureByteSize()
    {
        return m_table_line_size;
    }
    ///////////////////////////////////////////

    FastVectorDbLayer::FastVectorDbLayer(Impl *_impl) : impl(_impl) {}

    FastVectorDbLayer::~FastVectorDbLayer()
    {
        delete impl;
    }
    const char *FastVectorDbLayer::name()
    {
        return impl->name();
    }
    unsigned FastVectorDbLayer::getFieldCount()
    {
        return impl->getFieldCount();
    }
    const char *FastVectorDbLayer::getFieldDefn(unsigned ix, FieldTypeEnum &ft, double &vmin, double &vmax)
    {
        return impl->getFieldDefn(ix, ft, vmin, vmax);
    }
    
    void   FastVectorDbLayer::getExtent(double& minx,double& miny,double& maxx,double& maxy)
    {
        impl->getExtent(minx,miny,maxx,maxy);
    }

    void FastVectorDbLayer::rewind()
    {
        impl->rewind();
    }
    bool FastVectorDbLayer::next()
    {
        return impl->next();
    }
    int FastVectorDbLayer::row()
    {
        return impl->row();
    }
    void FastVectorDbLayer::fetchGeometry(GeometryReturn *cb)
    {
        impl->fetchGeometry(cb);
    }
    double FastVectorDbLayer::getFieldAsFloat(u32 ix)
    {
        return impl->getFieldAsFloat(ix);
    }
    int FastVectorDbLayer::getFieldAsInt(u32 ix)
    {
        return impl->getFieldAsInt(ix);
    }
    
    const char *FastVectorDbLayer::getFieldAsString(u32 ix)
    {
        return impl->getFieldAsString(ix);
    }

    chunk_data_t    FastVectorDbLayer::getGeometryLikeChunk()
    {
        return impl->getGeometryLikeChunk();   
    }
    
    const uchar_t *FastVectorDbLayer::getFieldAsWString(u32 ix)
    {
        return impl->getFieldAsWString(ix);
    }

    chunk_data_t FastVectorDbLayer::getFieldAsStringView(u32 ix)
    {
        return impl->getFieldAsStringView(ix);
    }

    chunk_data_t FastVectorDbLayer::getStringColumnOffsets(u32 ix)
    {
        return impl->getStringColumnOffsets_internal(ix);
    }

    chunk_data_t FastVectorDbLayer::getStringColumnData(u32 ix)
    {
        return impl->getStringColumnData_internal(ix);
    }
    
    u32 FastVectorDbLayer::getFeatureCount()
    {
        return impl->getFeatureCount();
    }
    void* FastVectorDbLayer::setFeatureCookie(void* cookie)
    {
        return impl->setFeatureCookie(cookie);
    }

    void* FastVectorDbLayer::getFeatureCookie()
    {
        return impl->getFeatureCookie();
    }
    GeometryLikeEnum FastVectorDbLayer::getGeometryType()
    {
        return impl->getGeometryType();
    }

    FastVectorDbFeatureRef* FastVectorDbLayer::getFieldAsFeatureRef(u32 ix)
    {
        return impl->getFieldAsFeatureRef(ix);
    }
    FastVectorDbFeature*    FastVectorDbLayer::tryGetFeatureAt(u32 ix)
    {
        return impl->tryGetFeatureAt(ix);
    }
    size_t  FastVectorDbLayer::getFieldOffset(unsigned ix)
    {
        return  impl->getFieldOffset(ix);
    }
    size_t  FastVectorDbLayer::getFeatureByteSize()
    {
        return impl->getFeatureByteSize();
    }

    // ---- List column read helpers ----
    chunk_data_t FastVectorDbLayer::Impl::getFieldAsListView_internal(u32 ifeature, u32 ix)
    {
        if (ix >= m_header->field_count || ifeature >= m_header->feature_count)
            return {0, nullptr};
        const field_desc_ex_t* fd = m_field_descs + ix;
        if (fd->type != ftList)
            return {0, nullptr};
        const u8* row_ptr = m_table_data_ptr0 + m_table_line_size * ifeature + fd->offset;
        u32 start = *(u32*)row_ptr;
        u32 cnt   = *(u32*)(row_ptr + 4);
        if (cnt == 0)
            return {0, nullptr};
        // find matching list field data by field_id
        for (auto& lfd : m_list_fields) {
            if (lfd.field_id == ix) {
                return {(size_t)(cnt * lfd.elem_size), lfd.data_ptr + start * lfd.elem_size};
            }
        }
        return {0, nullptr};
    }

    u32 FastVectorDbLayer::Impl::getFieldListSize_internal(u32 ifeature, u32 ix)
    {
        if (ix >= m_header->field_count || ifeature >= m_header->feature_count)
            return 0;
        const field_desc_ex_t* fd = m_field_descs + ix;
        if (fd->type != ftList)
            return 0;
        const u8* row_ptr = m_table_data_ptr0 + m_table_line_size * ifeature + fd->offset;
        return *(u32*)(row_ptr + 4);
    }

    FastVectorDbFeatureRef* FastVectorDbLayer::Impl::getFieldListRefAt_internal(u32 ifeature, u32 ix, u32 list_idx)
    {
        if (ix >= m_header->field_count || ifeature >= m_header->feature_count)
            return nullptr;
        const field_desc_ex_t* fd = m_field_descs + ix;
        if (fd->type != ftList)
            return nullptr;
        const u8* row_ptr = m_table_data_ptr0 + m_table_line_size * ifeature + fd->offset;
        u32 start = *(u32*)row_ptr;
        u32 cnt   = *(u32*)(row_ptr + 4);
        if (list_idx >= cnt)
            return nullptr;
        for (auto& lfd : m_list_fields) {
            if (lfd.field_id == ix) {
                return (FastVectorDbFeatureRef*)(lfd.data_ptr + (start + list_idx) * lfd.elem_size);
            }
        }
        return nullptr;
    }
    FastVectorDbFeature::~FastVectorDbFeature()
    {
        delete impl;
    }
    FastVectorDbLayer*      FastVectorDbFeature::layer()
    {
        return impl->layer;
    }
    void FastVectorDbFeature::fetchGeometry(GeometryReturn *cb)
    {
        return impl->layer->impl->fetchGeometry_internal(impl->ifeature,cb);
    }
    double FastVectorDbFeature::getFieldAsFloat(u32 ix)
    {
        return impl->layer->impl->getFieldAsFloat_internal(impl->ifeature,ix);
    }
    int FastVectorDbFeature::getFieldAsInt(u32 ix)
    {
        return impl->layer->impl->getFieldAsInt_internal(impl->ifeature,ix);
    }
    const char* FastVectorDbFeature::getFieldAsString(u32 ix)
    {
        return impl->layer->impl->getFieldAsString_internal(impl->ifeature,ix);
    }
    const uchar_t* FastVectorDbFeature::getFieldAsWString(u32 ix)
    {
        return impl->layer->impl->getFieldAsWString_internal(impl->ifeature,ix);
    }
    chunk_data_t FastVectorDbFeature::getFieldAsStringView(u32 ix)
    {
        return impl->layer->impl->getFieldAsStringView_internal(impl->ifeature, ix);
    }
    FastVectorDbFeatureRef* FastVectorDbFeature::getFieldAsFeatureRef(u32 ix)
    {
        return impl->layer->impl->getFieldAsFeatureRef_internal(impl->ifeature,ix);
    }
    void*   FastVectorDbFeature::setFeatureCookie(void *cookie)
    {
        return impl->layer->impl->setFeatureCookie_internal(impl->ifeature,cookie);
    }
    void*   FastVectorDbFeature::getFeatureCookie()
    {
        return impl->layer->impl->getFeatureCookie_internal(impl->ifeature);
    }

    chunk_data_t FastVectorDbFeature::getGeometryLikeChunk()
    {
        return impl->layer->impl->getGeometryLikeChunk_internal(impl->ifeature);
    }

    void*   FastVectorDbFeature::getAddress()
    {
        return impl->layer->impl->getFeatureAddress(impl->ifeature);
    }
    
    void    FastVectorDbFeature::setField(u32 ix,double value)
    {
        impl->layer->impl->setField_internal(impl->ifeature,ix,value);
    }

    void    FastVectorDbFeature::setField(u32 ix,int    value)
    {
        impl->layer->impl->setField_internal(impl->ifeature,ix,value);
    }

    void FastVectorDbFeature::setField(u32 ix, FastVectorDbFeature* feature)
    {
        impl->layer->impl->setFeatureRef_internal(impl->ifeature, ix, feature);
    }

    void FastVectorDbFeature::getFieldsAsDoubles(const u32* field_ids, int n_fields, double* out)
    {
        auto* li = impl->layer->impl;   // cache pointer chain once
        u32 ifeature = impl->ifeature;
        for (int i = 0; i < n_fields; i++)
            out[i] = li->getFieldAsFloat_internal(ifeature, field_ids[i]);
    }

    void FastVectorDbFeature::setFieldsFromDoubles(const u32* field_ids, const double* values, int n_fields)
    {
        auto* li = impl->layer->impl;
        u8* buf = (u8*)li->getFeatureAddress(impl->ifeature);
        for (int i = 0; i < n_fields; i++)
            set_field_value_t(buf, li->m_field_descs[field_ids[i]], values[i]);
    }
    chunk_data_t FastVectorDbFeature::getFieldAsListView(u32 ix)
    {
        return impl->layer->impl->getFieldAsListView_internal(impl->ifeature, ix);
    }
    u32 FastVectorDbFeature::getFieldListSize(u32 ix)
    {
        return impl->layer->impl->getFieldListSize_internal(impl->ifeature, ix);
    }
    FastVectorDbFeatureRef* FastVectorDbFeature::getFieldListRefAt(u32 ix, u32 list_idx)
    {
        return impl->layer->impl->getFieldListRefAt_internal(impl->ifeature, ix, list_idx);
    }
}
