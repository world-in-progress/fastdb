#include "FastVectorDb_p.h"
#include "FastVectorDbLayer_p.h"
#include <cstring>
#include <limits>
#include <stdlib.h>

#ifdef _WIN32
    #include <io.h>
    #include <sys/stat.h>
    #include <fcntl.h>
#else
    #include <unistd.h>
    #include <sys/stat.h>
    #include <fcntl.h>
#endif

namespace wx
{
    namespace
    {
        template <typename T>
        T load_unaligned(const void* source)
        {
            T value{};
            std::memcpy(&value, source, sizeof(value));
            return value;
        }

        bool valid_layer_bounds(const u8* layer, const layer_header_t& header)
        {
            const size_t tail = size_t(header.total_size) - sizeof(layer_header_t);
            if (header.field_count > tail / sizeof(field_desc_ex_t))
                return false;
            const size_t descriptors = size_t(header.field_count) * sizeof(field_desc_ex_t);
            const u8* data = layer + sizeof(layer_header_t) + descriptors;
            const size_t size = tail - descriptors;
            if (header.offset_table > size)
                return false;
            if (header.field_count) {
                const auto last = load_unaligned<field_desc_ex_t>(
                    layer + sizeof(layer_header_t) + descriptors - sizeof(field_desc_ex_t));
                if (last.offset > std::numeric_limits<u64>::max() - last.size)
                    return false;
                const u64 stride = last.offset + last.size;
                if (stride && header.feature_count > (size - header.offset_table) / stride)
                    return false;
                for (u16 i = 0; i < header.field_count; ++i) {
                    const auto field = load_unaligned<field_desc_ex_t>(
                        layer + sizeof(layer_header_t) + size_t(i) * sizeof(field_desc_ex_t));
                    if (field.offset > stride || field.size > stride - field.offset)
                        return false;
                }
            }
            // Legacy string tables are read during construction, before the
            // bounded optional column sections. Check their terminators here.
            for (unsigned width : {1u, 2u}) {
                u64 offset = width == 1 ? header.offset_strings : header.offset_wstrings;
                if (offset > size || size - offset < sizeof(u32))
                    return false;
                const u32 count = load_unaligned<u32>(data + offset);
                offset += sizeof(u32);
                if (count > (size - offset) / width)
                    return false;
                for (u32 i = 0; i < count; ++i) {
                    for (;;) {
                        if (size - offset < width)
                            return false;
                        const bool terminated = width == 1 ? data[offset] == 0 :
                            load_unaligned<u16>(data + offset) == 0;
                        offset += width;
                        if (terminated) break;
                    }
                }
            }
            return true;
        }
    }

    FastVectorDb::Impl::Impl(void *pdata, size_t size, fnFreeDbBuffer fnFreeBuffer, void *cookie)
        : m_pdata(pdata), m_size(size), m_fnFreeBuffer(fnFreeBuffer), m_cookie(cookie)
    {
        u8 *ptr = (u8 *)pdata;
        ptr += 16;
        const u32 count = load_unaligned<u32>(ptr);
        ptr += sizeof(u32);
        for (u32 i = 0; i < count; i++)
        {
            const layer_header_t layer_header =
                load_unaligned<layer_header_t>(ptr);
            auto layerImpl =
                new FastVectorDbLayer::Impl(ptr, layer_header.total_size);
            

            auto layer = new FastVectorDbLayer(layerImpl);
            layerImpl->m_layer= layer;
            layerImpl->m_layer_index=i;
            m_layers.push_back(layer);
            ptr += layer_header.total_size;
        }
    }
    FastVectorDb::Impl::~Impl()
    {
        for (auto layer : m_layers)
        {
            delete layer;
        }

        if (m_fnFreeBuffer)
        {
            m_fnFreeBuffer(m_pdata, m_size, m_cookie);
        }
    }
    unsigned FastVectorDb::Impl::getLayerCount()
    {
        return m_layers.size();
    }
    FastVectorDbLayer *FastVectorDb::Impl::getLayer(unsigned ix)
    {
        return m_layers[ix];
    }

     chunk_data_t FastVectorDb::Impl::buffer()
     {
        chunk_data_t data;
        data.pdata = (u8*)m_pdata;
        data.size = m_size;
        return data;
     }
    FastVectorDbFeature*  FastVectorDb::Impl::tryGetFeature(FastVectorDbFeatureRef* ref)
    {
        int nsize = sizeof(FastVectorDbFeatureRef);
        if (ref->ilayer >= m_layers.size())
        {
            return nullptr;
        }
        u32 ifeature = FastVectorDbFeatureRef::decodeIFeature(ref);
        return m_layers[ref->ilayer]->impl->tryGetFeatureAt(ifeature);
    }

    ///////////////////////////////////////////////////////
    FastVectorDb::FastVectorDb(Impl *_impl)
        : impl(_impl)
    {
    }

    FastVectorDb::~FastVectorDb()
    {
        delete impl;
    }

    unsigned FastVectorDb::getLayerCount()
    {
        return impl->getLayerCount();
    }

    FastVectorDbLayer *FastVectorDb::getLayer(unsigned ix)
    {
        return impl->getLayer(ix);
    }

    FastVectorDb *FastVectorDb::load(void *pdata, size_t size, fnFreeDbBuffer fnFreeBuffer, void *cookie)
    {
        // Validate the standalone envelope once before reading its headers.
        // The backing callback also releases invalid inputs.
        bool valid = pdata && size >= 20 &&
            std::memcmp(pdata, "FASTVectorDB0.1", sizeof("FASTVectorDB0.1")) == 0;
        if (valid) {
            const u8* ptr = static_cast<const u8*>(pdata) + 20;
            size_t remaining = size - 20;
            const u32 count = load_unaligned<u32>(static_cast<const u8*>(pdata) + 16);
            for (u32 i = 0; i < count; ++i) {
                if (remaining < sizeof(layer_header_t)) { valid = false; break; }
                const auto header = load_unaligned<layer_header_t>(ptr);
                if (header.total_size < sizeof(layer_header_t) || header.total_size > remaining ||
                    !valid_layer_bounds(ptr, header)) {
                    valid = false;
                    break;
                }
                ptr += header.total_size;
                remaining -= header.total_size;
            }
        }
        if (!valid)
        {
            if (fnFreeBuffer) fnFreeBuffer(pdata, size, cookie);
            return nullptr;
        }
        return new FastVectorDb(new Impl(pdata, size, fnFreeBuffer, cookie));
    }

    FastVectorDbFeature* FastVectorDb::tryGetFeature(wx::FastVectorDbFeatureRef* ref)
    {
        return impl->tryGetFeature(ref);
    }

    void free_data_buffer(void* pdata,size_t size,void* pcookie)
    {
        free(pdata);
    }

    FastVectorDb *FastVectorDb::load(const char *filename)
    {
#ifdef DEBUG
printf("\nFastVectorDB:A fast vector database for local cache\n\
Author: wenyongning@njnu.edu.cn\n");
printf("loading [%s] ...",filename);
#endif
#ifdef _WIN32
        int fd = _open(filename, _O_RDONLY | _O_BINARY);
        if (fd == -1)
        {
            printf("Error opening file: %s\n", strerror(errno));
            return NULL;
        }

        struct _stat fileStat;
        if (_fstat(fd, &fileStat) == -1)
        {
            printf("Error getting file status: %s\n", strerror(errno));
            _close(fd);
            return NULL;
        }
        size_t size = fileStat.st_size;
        void* pdata = malloc(sizeof(u8)*size+64);
        
        size_t total_read = 0;
        while(total_read < size) {
            unsigned int chunk = (size - total_read > 0x40000000) ? 0x40000000 : (unsigned int)(size - total_read);
            int r = _read(fd, (char*)pdata + total_read, chunk);
            if (r == -1) {
                printf("Error reading file: %s\n", strerror(errno));
                _close(fd);
                free(pdata);
                return NULL;
            }
            if (r == 0) break;
            total_read += r;
        }
        _close(fd);
#else
        int fd = open(filename, O_RDONLY);
        if (fd == -1)
        {
            printf("Error opening file: %s\n", strerror(errno));
            return NULL;
        }

        struct stat fileStat;
        if (fstat(fd, &fileStat) == -1)
        {
            printf("Error getting file status: %s\n", strerror(errno));
            close(fd);
            return NULL;
        }
        size_t size =  fileStat.st_size;
        void* pdata = malloc(sizeof(u8)*size+64);
        
        size_t total_read = 0;
        while(total_read < size) {
            ssize_t r = read(fd, (char*)pdata + total_read, size - total_read);
            if (r == -1) {
                if (errno == EINTR) continue;
                printf("Error reading file: %s\n", strerror(errno));
                close(fd);
                free(pdata);
                return NULL;
            }
            if (r == 0) break; // EOF
            total_read += r;
        }
        close(fd);
#endif
        auto db = load(pdata,size,free_data_buffer,0);
        if(db)
        {
#ifdef DEBUG
            printf("Fastdb load done!\n");
#endif
        }
        else
        {
            printf("failed!\nFastdb can not open invalid database file!\n");
        }
        return db;
    }

    chunk_data_t FastVectorDb::buffer()
    {
        return impl->buffer();
    }
}

extern "C"
{

    void sqlite3_free(void *d)
    {
        free(d);
    }
    char *sqlite3_mprintf(const char *, ...) { return 0; }
}
