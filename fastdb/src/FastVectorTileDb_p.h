#pragma once
#include "fastdb.h"
#include <vector>
#include <string>
#include <set>
#include <mutex>
#include <thread>
using namespace std;
namespace wx{
    struct TileDbHandle_ext:public FastVectorTileDb::TileDataHandle
    {
        string _path;
        u32    lastFrameCounter;
        TileDbHandle_ext(const char* pth)
        {
            _path =pth;
            path = _path.c_str();
            lastFrameCounter = 0;
            tileData = NULL;
        }
        ~TileDbHandle_ext()
        {
            if(tileData)
                delete tileData;
        }
        void shrink()
        {
            if(tileData)
                delete tileData;
            tileData = NULL;
        }
    };
    class FastVectorTileDb::Impl:public TileBoxTake::HandleTileAction
    {
    public:
        Impl(FastVectorTileDb* host,bool mt);
       ~Impl();
        void registerTile(const char* path,u8 level, double t, double xmin, double ymin, double xmax, double ymax);
        const TakeResult*   take(u32 maxLevel, double xmin, double ymin, double xmax, double ymax);
        void                freeResult(const TakeResult* result);
        void                shrink(u32 maxTileCache);
        virtual bool        isTileLoaded(const TileBoxTake::TileBox* box);

    public:
        static int          _load_in_thread(Impl *pThis);
        int                 load_in_thread();
    private:
        vector<TileDbHandle_ext*>      m_tile_handles;
        vector<TileDbHandle_ext*>      m_tiles_loaded;
        set<TileDbHandle_ext*>      m_tiles_loading;

        FastVectorTileDb*          m_host;
        u32                        m_last_frame;
        mutex                      m_mutex_take;
        mutex                      m_mutex_load;
        TileBoxTake*               m_tile_box_take;
        u32                        m_max_level;
        bool                       m_mt;
        bool                       m_thread_runing;
        std::thread*               m_thread;
    };

}