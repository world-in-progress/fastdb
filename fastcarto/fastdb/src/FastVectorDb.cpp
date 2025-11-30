#include "FastVectorDb_p.h"
#include "FastVectorDbLayer_p.h"
#include <stdlib.h>
#include <sys/stat.h>

#ifdef _WIN32
    #include <io.h>
#else
    #include <unistd.h>
#endif

#include <fcntl.h>

namespace wx
{  
    FastVectorDb::Impl::Impl(void *pdata, size_t size, fnFreeDbBuffer fnFreeBuffer, void *cookie)
        : m_pdata(pdata), m_size(size), m_fnFreeBuffer(fnFreeBuffer), m_cookie(cookie)
    {
        u8 *ptr = (u8 *)pdata;
        bool check_mask = strcmp((const char *)ptr, "FASTVectorDB0.1") == 0;
        assert(check_mask);
        m_mask_check_ok=check_mask;
        ptr += 16;
        u32 count = *(u32 *)ptr;
        ptr += sizeof(u32);
        for (int i = 0; i < count; i++)
        {
            layer_header_t *lh = (layer_header_t *)ptr;
            auto layerImpl = new FastVectorDbLayer::Impl(ptr, lh->total_size);
            

            auto layer = new FastVectorDbLayer(layerImpl);
            layerImpl->m_layer= layer;
            layerImpl->m_layer_index=i;
            m_layers.push_back(layer);
            ptr += lh->total_size;
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
        FastVectorDb::Impl *impl = new FastVectorDb::Impl(pdata, size, fnFreeBuffer, cookie);
        if(impl->m_mask_check_ok)
        {
            auto db = new FastVectorDb(impl);
            return db;
        }
        else
        {
            delete impl;
            return nullptr;
        }
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
        int fd = open(filename, O_RDONLY); // 打开文件获取描述符
        if (fd == -1)
        { 
            printf("Error opening file: %s\n", strerror(errno));
            return NULL;
        }

        struct stat fileStat;
        if (fstat(fd, &fileStat) == -1)
        { // 通过描述符获取状态
            printf("Error getting file status: %s\n", strerror(errno));
            close(fd);
            return NULL;
        }
        size_t size =  fileStat.st_size;
        void* pdata = malloc(sizeof(u8)*size+64);
        read(fd,pdata,size);
        close(fd);
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
