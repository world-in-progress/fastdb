#include "FastVectorTileDb_p.h"
#include <algorithm>
namespace wx
{

    FastVectorTileDb::Impl::Impl(FastVectorTileDb *host,bool mt)
        : m_host(host),m_mt(mt)
    {
        m_tile_box_take = new TileBoxTake(32);
        m_thread_runing=mt;
        if(mt)
            m_thread = new std::thread(FastVectorTileDb::Impl::_load_in_thread,this);
        else
            m_thread = nullptr;
    }
    FastVectorTileDb::Impl::~Impl()
    {
        m_thread_runing=false;
        if(m_thread)
        {
            m_thread->join();
            delete m_thread;
        }
        for (auto &tile : m_tiles_loaded)
        {
            delete tile;
        }
        delete m_tile_box_take;

    }

    void FastVectorTileDb::Impl::registerTile(const char *path, u8 level, double t, double xmin, double ymin, double xmax, double ymax)
    {
        u32 id = m_tile_handles.size();
        TileDbHandle_ext *loading = new TileDbHandle_ext(path);
        m_tile_handles.push_back(loading);

        m_tile_box_take->registerTileBox(id, level, t, xmin, ymin, xmax, ymax, loading);
    }

    bool FastVectorTileDb::Impl::isTileLoaded(const TileBoxTake::TileBox* box)
    {
        return ((TileDbBox*)box)->handle->tileData!=NULL;
    }
    int FastVectorTileDb::Impl::load_in_thread()
    {
        while (m_thread_runing)
        {
            TileDbHandle_ext *loading = NULL;
            m_mutex_load.lock();
            if(m_tiles_loading.size()>0)
            {
                loading = *m_tiles_loading.begin();
            }
            m_mutex_load.unlock();

            if(loading)
            {
                printf("threading load>>");
                auto db = this->m_host->loadTileDataInternal(loading);
                if(db)
                {
                    loading->tileData=db;
                    m_mutex_load.lock();
                    m_tiles_loading.erase(m_tiles_loading.begin());
                    m_tiles_loaded.push_back(loading);
                    m_mutex_load.unlock();
                }
            }
            else
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
        }
        return 0;
    }

     int FastVectorTileDb::Impl::_load_in_thread(Impl *pThis)
     {

        return pThis->load_in_thread();
     }

    const FastVectorTileDb::TakeResult *FastVectorTileDb::Impl::take(u32 maxLevel, double xmin, double ymin, double xmax, double ymax)
    {
        m_mutex_take.lock();
        auto rawResult = m_tile_box_take->take(maxLevel, xmin, ymin, xmax, ymax,this);
        if(rawResult)
        {
            for (int i = 0; i < rawResult->count; i++)
            {
                auto tile = (TileDbBox *)rawResult->tiles[i];
                auto loading = (TileDbHandle_ext *)tile->handle;
                if (loading->tileData == NULL)
                {
                    if(!m_mt)
                    {
                        loading->tileData = m_host->loadTileDataInternal(loading);
                        m_tiles_loaded.push_back(loading);
                    }
                    else
                    {
                        m_mutex_load.lock();
                        if(m_tiles_loading.find(loading) == m_tiles_loading.end())
                        {
                            m_tiles_loading.insert(loading);
                        }
                        else
                        {
                            printf("tile already loading %s\n",loading->_path.c_str());
                        }
                        m_mutex_load.unlock();
                    }
                    
                }
                loading->lastFrameCounter = m_last_frame;
            }
            return (TakeResult *)rawResult;
        }
        else
        {
            m_mutex_take.unlock();
            return nullptr;
        }
    }
    void FastVectorTileDb::Impl::freeResult(const TakeResult *result)
    {
        m_tile_box_take->freeResult((TileBoxTake::TakeResult *)result);
        if(result)
            m_mutex_take.unlock();
    }
    void FastVectorTileDb::Impl::shrink(u32 maxTileCache)
    {
        m_mutex_take.lock();
        m_mutex_load.lock();
        stable_sort<>(m_tiles_loaded.begin(), m_tiles_loaded.end(),
            [](wx::TileDbHandle_ext* a,wx::TileDbHandle_ext* b)
            {
                return a->lastFrameCounter < b->lastFrameCounter;
            }
        );

        while (m_tiles_loaded.size() > maxTileCache)
        {
            m_tiles_loaded.back()->shrink();
            m_tiles_loaded.pop_back();
        }
        m_mutex_load.unlock();
        m_mutex_take.unlock();
    }

    ///////////////////////////////////////////////////////////////
    FastVectorTileDb::FastVectorTileDb(bool mt)
    {
        impl = new Impl(this,mt);
    }
    FastVectorTileDb::~FastVectorTileDb()
    {
        delete impl;
    }
    void FastVectorTileDb::registerTile(const char *path, u8 level, double t, double xmin, double ymin, double xmax, double ymax)
    {
        impl->registerTile(path,level,t,xmin,ymin,xmax,ymax);
    }
    const FastVectorTileDb::TakeResult *FastVectorTileDb::take(u32 maxLevel, double xmin, double ymin, double xmax, double ymax)
    {
        return impl->take(maxLevel,xmin,ymin,xmax,ymax);
    }
    void FastVectorTileDb::freeResult(const TakeResult *result)
    {
        impl->freeResult(result);
    }
    void FastVectorTileDb::shrink(u32 maxTileCache)
    {
        impl->shrink(maxTileCache);
    }

    FastVectorTileDb::TileData *FastVectorTileDb::loadTileDataInternal(TileDataHandle *loading)
    {
        auto path = loading->path;
        auto db = FastVectorDb::load(path);
        if(!db)
        {
            printf("Can't load tile db %s\n",path);
        }
        return db;
    }
}