#include "FastVectorDbBuild_p.h"
#include "FastVectorDbLayerBuild_p.h"
#include <assert.h>
#include <string.h>
#include <stdio.h>
#include <memory>
#include <vector> 
namespace wx
{
    MemoryStream::MemoryStream()
    {
        impl = new MemoryStream::Impl();
    }

    MemoryStream::~MemoryStream()
    {
        delete impl;
    }

    chunk_data_t MemoryStream::data()
    {
        return impl->data();
    }

    void MemoryStream::reset()
    {
        impl->reset();
    }

    void MemoryStream::write(void *pdata, size_t size)
    {
        impl->write(pdata, size);
    }

    MemoryStream::Impl::Impl()
        :m_buffer()
    {
    }

    MemoryStream::Impl::~Impl()
    {
    }

    chunk_data_t MemoryStream::Impl::data()
    {
        return chunk_data_t{ m_buffer.size(), m_buffer.data() };
    }

    void MemoryStream::Impl::reset()
    {
        m_buffer.clear();
        m_buffer.shrink_to_fit();
    }

    void MemoryStream::Impl::write(void *pdata, size_t size)
    {
        m_buffer.insert(m_buffer.end(), (u8*)pdata, ((u8*)pdata) + size);
    }

    FixedBufferWriteStream::FixedBufferWriteStream(void* pdata, size_t size)
        : m_data(reinterpret_cast<u8*>(pdata)), m_size(size), m_offset(0), m_overflowed(false)
    {
    }

    void FixedBufferWriteStream::write(void *pdata, size_t size)
    {
        if (m_overflowed)
            return;
        if (size > m_size - m_offset)
        {
            m_overflowed = true;
            return;
        }
        if (size > 0)
            memcpy(m_data + m_offset, pdata, size);
        m_offset += size;
    }

    size_t FixedBufferWriteStream::bytesWritten() const
    {
        return m_offset;
    }

    bool FixedBufferWriteStream::overflowed() const
    {
        return m_overflowed;
    }

    struct HeapScratchStats
    {
        size_t allocation_count = 0;
        size_t release_count = 0;
    };

    class HeapScratchAllocator::Impl
    {
    public:
        Impl()
            : stats(std::make_shared<HeapScratchStats>())
        {
        }

        std::shared_ptr<HeapScratchStats> stats;
    };

    class HeapScratchAllocation::Impl
    {
    public:
        Impl(size_t size, std::shared_ptr<HeapScratchStats> stats)
            : buffer(size), stats(stats)
        {
        }

        vector<u8> buffer;
        std::shared_ptr<HeapScratchStats> stats;
    };

    HeapScratchAllocation::HeapScratchAllocation(size_t size, HeapScratchAllocator* owner)
        : impl(new HeapScratchAllocation::Impl(
            size,
            owner ? owner->impl->stats : std::shared_ptr<HeapScratchStats>()
        ))
    {
    }

    HeapScratchAllocation::~HeapScratchAllocation()
    {
        if (impl->stats)
            impl->stats->release_count++;
        delete impl;
    }

    void* HeapScratchAllocation::data()
    {
        return impl->buffer.empty() ? nullptr : impl->buffer.data();
    }

    size_t HeapScratchAllocation::size() const
    {
        return impl->buffer.size();
    }

    HeapScratchAllocator::HeapScratchAllocator()
        : impl(new HeapScratchAllocator::Impl())
    {
    }

    HeapScratchAllocator::~HeapScratchAllocator()
    {
        delete impl;
    }

    ScratchAllocation* HeapScratchAllocator::allocate(size_t size, size_t alignment)
    {
        (void)alignment;
        impl->stats->allocation_count++;
        return new HeapScratchAllocation(size, this);
    }

    size_t HeapScratchAllocator::allocationCount() const
    {
        return impl->stats->allocation_count;
    }

    size_t HeapScratchAllocator::releaseCount() const
    {
        return impl->stats->release_count;
    }

    struct HeapFinalBackingStats
    {
        size_t allocation_count = 0;
        size_t commit_count = 0;
        size_t rollback_count = 0;
    };

    class HeapFinalBackingResource::Impl
    {
    public:
        Impl()
            : stats(std::make_shared<HeapFinalBackingStats>())
        {
        }

        std::shared_ptr<HeapFinalBackingStats> stats;
    };

    class HeapFinalBackingAllocation::Impl
    {
    public:
        Impl(size_t size, std::shared_ptr<HeapFinalBackingStats> stats)
            : buffer(size), used_size(0), committed(false), rolled_back(false), stats(stats)
        {
        }

        vector<u8> buffer;
        size_t used_size;
        bool committed;
        bool rolled_back;
        std::shared_ptr<HeapFinalBackingStats> stats;
    };

    HeapFinalBackingAllocation::HeapFinalBackingAllocation(size_t size, HeapFinalBackingResource* owner)
        : impl(new HeapFinalBackingAllocation::Impl(
            size,
            owner ? owner->impl->stats : std::shared_ptr<HeapFinalBackingStats>()
        ))
    {
    }

    HeapFinalBackingAllocation::~HeapFinalBackingAllocation()
    {
        delete impl;
    }

    void* HeapFinalBackingAllocation::data()
    {
        if (impl->rolled_back)
            return nullptr;
        return impl->buffer.empty() ? nullptr : impl->buffer.data();
    }

    size_t HeapFinalBackingAllocation::size() const
    {
        return impl->buffer.size();
    }

    size_t HeapFinalBackingAllocation::usedSize() const
    {
        return impl->used_size;
    }

    bool HeapFinalBackingAllocation::committed() const
    {
        return impl->committed;
    }

    bool HeapFinalBackingAllocation::rolledBack() const
    {
        return impl->rolled_back;
    }

    bool HeapFinalBackingAllocation::commit(size_t used_size)
    {
        if (impl->rolled_back || impl->committed || used_size > impl->buffer.size())
            return false;
        impl->used_size = used_size;
        impl->committed = true;
        if (impl->stats)
            impl->stats->commit_count++;
        return true;
    }

    void HeapFinalBackingAllocation::rollback()
    {
        if (impl->committed || impl->rolled_back)
            return;
        impl->rolled_back = true;
        impl->used_size = 0;
        impl->buffer.clear();
        impl->buffer.shrink_to_fit();
        if (impl->stats)
            impl->stats->rollback_count++;
    }

    HeapFinalBackingResource::HeapFinalBackingResource()
        : impl(new HeapFinalBackingResource::Impl())
    {
    }

    HeapFinalBackingResource::~HeapFinalBackingResource()
    {
        delete impl;
    }

    FinalBackingAllocation* HeapFinalBackingResource::allocate(size_t size, size_t alignment)
    {
        (void)alignment;
        impl->stats->allocation_count++;
        return new HeapFinalBackingAllocation(size, this);
    }

    size_t HeapFinalBackingResource::allocationCount() const
    {
        return impl->stats->allocation_count;
    }

    size_t HeapFinalBackingResource::commitCount() const
    {
        return impl->stats->commit_count;
    }

    size_t HeapFinalBackingResource::rollbackCount() const
    {
        return impl->stats->rollback_count;
    }

    FastVectorDbBuild::Impl::Impl(FastVectorDbBuild* thiz)
        :m_thiz(thiz)
    {
        m_current_layer=nullptr;
        m_string_table_u32 = false;
        m_gt=gtPoint;
        m_ct=cfF32;
        m_string_table_u32 = false;
        m_aabbox_enable = false;
        m_extent.minEdge={-180.0,-90.0};
        m_extent.maxEdge={180,90};
    }

    FastVectorDbBuild::Impl::~Impl()
    {
    }

    void FastVectorDbBuild::Impl::begin(const char *cfg)
    {
#ifdef DEBUG
        printf("FastVectorDB:A fast vector database for local cache\nAuthor: wenyongning@njnu.edu.cn\nbegin...\n");
#endif
        m_cfg = cfg;
    }

    FastVectorDbLayerBuild*  FastVectorDbBuild::Impl::createLayerBegin(const char *layerName)
    {
        auto layer = new FastVectorDbLayerBuild(m_thiz,layerName);
        layer->enableStringTableU32(m_string_table_u32);
        layer->setExtent(m_extent.minEdge.x, m_extent.minEdge.y, m_extent.maxEdge.x, m_extent.maxEdge.y);
        layer->setGeometryType(m_gt, m_ct, m_aabbox_enable);
        layer->setDbIndex((int)m_layers.size());

#ifdef DEBUG
        printf(
"\nfastdb is creating layer[%s] with last(default) params:\n\
geometry type:%s, coord format:%s, aabbox:%s\n\
extent:(%lf,%lf,%lf,%lf)}\n\
string table:%s\n\
you should check and reset them before adding any feature!!\n",
                layerName,
                get_geometry_type_name(m_gt),
                get_coord_type_name(m_ct),
                m_aabbox_enable?"yes":"no",
                m_extent.minEdge.x, m_extent.minEdge.y, m_extent.maxEdge.x, m_extent.maxEdge.y,
                m_string_table_u32?"u32":"u16");
#endif

        m_layers.push_back(layer);
        m_current_layer = layer;
        return layer;
    }

    void FastVectorDbBuild::Impl::truncate(const char *layerName, unsigned nfeatures)
    {
        for(auto layer:m_layers)
        {
            if(strcmp(layer->name(), layerName) == 0)
            {
                layer->impl->truncate(nfeatures);
                return;
            }
        }   
    }

    void FastVectorDbBuild::Impl::enableStringTableU32(bool b)
    {
        m_string_table_u32 = b;
        if(!m_current_layer)
            return;
        m_current_layer->enableStringTableU32(b);
    }

    int FastVectorDbBuild::Impl::addField(const char *name, unsigned ft, double vmin, double vmax) 
    {
        if (!m_current_layer)
            return -1;
        return m_current_layer->addField(name, ft, vmin, vmax);
    }
    void FastVectorDbBuild::Impl::setGeometryType(GeometryLikeEnum gt, CoordinateFormatEnum ct,bool aaboxEnable) 
    {
        m_gt=gt;
        m_ct=ct;
        m_aabbox_enable = aaboxEnable;
        if (!m_current_layer)
            return;
        m_current_layer->setGeometryType(gt, ct,aaboxEnable);
    }
    void FastVectorDbBuild::Impl::setExtent(double minx, double miny, double maxx, double maxy) 
    {   
        m_extent.minEdge={minx,miny};
        m_extent.maxEdge={maxx,maxy};
        if (!m_current_layer)
            return;
        m_current_layer->setExtent(minx, miny, maxx, maxy);
    }
    void FastVectorDbBuild::Impl::addFeatureBegin(){
        if (!m_current_layer)
            return;
        m_current_layer->addFeatureBegin();
    }
    void FastVectorDbBuild::Impl::setGeometry(const char *data, size_t size, GeometryLikeFormat fmt) 
    {
        if (!m_current_layer)
            return;
        m_current_layer->setGeometry((void*)data, size, fmt);
    }
    void FastVectorDbBuild::Impl::setField(unsigned ix, double value) 
    {
        if (!m_current_layer)
            return;
        m_current_layer->setField(ix, value);
    }
    void FastVectorDbBuild::Impl::setField(unsigned ix, int value) 
    {
        if (!m_current_layer)
            return;
        m_current_layer->setField(ix, value);
    }
    void FastVectorDbBuild::Impl::setField(unsigned ix, const char *text) 
    {
        if (!m_current_layer)
            return;
        m_current_layer->setField(ix, text);
    }
    void FastVectorDbBuild::Impl::setField(unsigned ix, const wchar_t *text) 
    {       
        if (!m_current_layer)
            return;
        m_current_layer->setField(ix, text);
    }
    void FastVectorDbBuild::Impl::addFeatureEnd() 
    {       
        if (!m_current_layer)
            return;
        m_current_layer->addFeatureEnd();
    }
    void FastVectorDbBuild::Impl::createLayerEnd() {
        if(m_current_layer)
            m_current_layer->impl->post();
        m_current_layer = nullptr;
    }
    size_t FastVectorDbBuild::Impl::byteLength()
    {
        size_t total = 16 + sizeof(u32);
        for (auto layer : m_layers)
        {
            total += layer->impl->get_total_size();
        }
        return total;
    }
    size_t FastVectorDbBuild::Impl::tableBufferBytes()
    {
        size_t total = 0;
        for (auto layer : m_layers)
        {
            total += layer->impl->tableBufferBytes();
        }
        return total;
    }
    size_t FastVectorDbBuild::Impl::postToBuffer(void* pdata, size_t size)
    {
        size_t expected = byteLength();
        if (pdata == nullptr || size < expected)
            return 0;
        FixedBufferWriteStream stream(pdata, size);
        save(&stream);
        if (stream.overflowed() || stream.bytesWritten() != expected)
            return 0;
        return stream.bytesWritten();
    }
    FinalBackingAllocation* FastVectorDbBuild::Impl::postToFinalBacking(FinalBackingResource* resource)
    {
        if (resource == nullptr)
            return nullptr;
        size_t expected = byteLength();
        FinalBackingAllocation* allocation = resource->allocate(expected, alignof(u64));
        if (allocation == nullptr)
            return nullptr;
        if (allocation->size() < expected || (expected > 0 && allocation->data() == nullptr))
        {
            allocation->rollback();
            delete allocation;
            return nullptr;
        }
        size_t written = postToBuffer(allocation->data(), allocation->size());
        if (written != expected)
        {
            allocation->rollback();
            delete allocation;
            return nullptr;
        }
        if (!allocation->commit(written))
        {
            allocation->rollback();
            delete allocation;
            return nullptr;
        }
        return allocation;
    }
    void FastVectorDbBuild::Impl::save(WriteStream *stream) 
    {
        const char magic[] = "FASTVectorDB0.1";
        stream->write((void*)magic, 16);
        u32 layer_count = (u32)m_layers.size();
        stream->write((void*)&layer_count, sizeof(layer_count));
        for (auto layer : m_layers)
        {   
            layer->impl->write(stream);
        }
    }
    void FastVectorDbBuild::Impl::save(const char *stream)
    {
        FILE* fp = fopen(stream, "wb");
        if (!fp)
            return;
        class FileWriteStream :public WriteStream
        {
        public:
            FileWriteStream(FILE* f) :fp(f) {}
            void write(void* pdata, size_t size) override
            {
                fwrite(pdata, 1, size, fp);
            }
        public:
            FILE* fp;
        };
        FileWriteStream fws(fp);
        save(&fws);
        fclose(fp);
    } 
    ///////////////////////////////////////////////////
    FastVectorDbBuild::FastVectorDbBuild()
    {
        impl = new wx::FastVectorDbBuild::Impl(this);
    }

    FastVectorDbBuild::~FastVectorDbBuild()
    {
        delete impl;
    }

    void FastVectorDbBuild::truncate(const char *layerName, unsigned nfeatures)
    {
        impl->truncate(layerName, nfeatures);
    }

    void FastVectorDbBuild::begin(const char *cfg)
    {
        impl->begin(cfg);
    }

    FastVectorDbLayerBuild*  FastVectorDbBuild::createLayerBegin(const char *layerName)
    {
        return impl->createLayerBegin(layerName);
    }

    void FastVectorDbBuild::enableStringTableU32(bool b)
    {
        impl->enableStringTableU32(b);
    }

    int FastVectorDbBuild::addField(const char *name, unsigned ft, double vmin, double vmax)
    {
        return impl->addField(name, ft, vmin, vmax);
    }

    void FastVectorDbBuild::setGeometryType(GeometryLikeEnum gt, CoordinateFormatEnum ct,bool aabboxEnable)
    {
        return impl->setGeometryType(gt, ct,aabboxEnable);
    }

    void FastVectorDbBuild::setExtent(double minx, double miny, double maxx, double maxy)
    {
        impl->setExtent(minx, miny, maxx, maxy);
    }

    void FastVectorDbBuild::addFeatureBegin()
    {
        impl->addFeatureBegin();
    }

    void FastVectorDbBuild::setGeometry(void *data, size_t size, GeometryLikeFormat fmt)
    {
        impl->setGeometry((const char*)data, size, fmt);
    }

    void FastVectorDbBuild::setField(unsigned ix, double value)
    {
        impl->setField(ix, value);
    }

    void FastVectorDbBuild::setField(unsigned ix, int value)
    {
        impl->setField(ix, value);
    }

    void FastVectorDbBuild::setField(unsigned ix, const char *text)
    {
        impl->setField(ix, text);
    }

    void FastVectorDbBuild::setField(unsigned ix, const wchar_t *text)
    {
        impl->setField(ix, text);
    }

    void FastVectorDbBuild::addFeatureEnd()
    {
        impl->addFeatureEnd();
    }

    void FastVectorDbBuild::createLayerEnd()
    {
        impl->createLayerEnd();
    }

    size_t FastVectorDbBuild::byteLength()
    {
        return impl->byteLength();
    }

    size_t FastVectorDbBuild::tableBufferBytes()
    {
        return impl->tableBufferBytes();
    }

    size_t FastVectorDbBuild::postToBuffer(void* pdata, size_t size)
    {
        return impl->postToBuffer(pdata, size);
    }

    FinalBackingAllocation* FastVectorDbBuild::postToFinalBacking(FinalBackingResource* resource)
    {
        return impl->postToFinalBacking(resource);
    }

    void FastVectorDbBuild::post(WriteStream *stream)
    { 
        impl->save(stream);
    }

    void FastVectorDbBuild::save(const char *filename)
    {
#ifdef DEBUG
        printf("\nFastVectorDB:A fast vector database for local cache\n\
        saving [%s] ...",filename);
#endif
        impl->save(filename);
#ifdef DEBUG
        printf("done!\n");
#endif
    }
    
    void warning(const char* message)
    {
        printf("FastDb WARNING:%s\n",message);
    }

}
