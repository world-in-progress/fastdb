#include "TileBoxTake_p.hpp"
#include "clipper2/clipper.h"
#include <algorithm>
#include <assert.h>
using namespace Clipper2Lib;
const long long MAX_SCALE_INT64 = 0xFFFFFFFFFFL;
namespace wx
{  
    TileBoxTake::Impl::Impl(int maxLevel)
    :m_dirty(true)
    {
        for(int i=0;i<maxLevel;i++)
        {
            m_levels.push_back(new tile_box_level_t());
        }
    }  
    TileBoxTake::Impl::~Impl()
    {
        for(auto i:m_levels)
        {
            delete i;
        }
        
    }
    void TileBoxTake::Impl::registerTileBox(const TileBox &item)
    {
        u8 ix = item.level;
        assert(ix<m_levels.size());
        m_levels[ix]->push_back(item);
    }
    

    template<typename box_like_t>
    PathsD make_path_from_box_like(box_like_t& box,double e=0.0)
    {
        auto path = PathD {
            PointD(box.minx-e,box.miny-e),
            PointD(box.minx-e,box.maxy+e),
            PointD(box.maxx+e,box.maxy+e),
            PointD(box.maxx-e,box.miny-e)
        };
        PathsD paths{path};
        return paths;
    }
    template<typename box_like_t1,typename box_like_t2>
    bool is_box_like_intersect(box_like_t1& t1,box_like_t2& t2)
    {
        return t1.minx<t2.maxx && t1.maxx>t2.minx && t1.miny<t2.maxy && t1.maxy>t2.miny;
    }
    template<typename box_like_t>
    bool is_box_like_in_polygon(box_like_t& box,PathsD& polygon)
    {
        // 创建 Clipper 对象
        ClipperD clipper;
        
        // 添加矩形作为被裁剪的主体 (Subject)
        clipper.AddSubject(make_path_from_box_like(box));
        // 添加多边形作为裁剪框 (Clip)
        clipper.AddClip(polygon);

        // 执行差集操作：计算 Rect - Poly
        PathsD solution;
        clipper.Execute(ClipType::Difference, FillRule::NonZero, solution);

        // 如果结果为空，说明矩形完全在多边形内
        return solution.empty();
    }

    void TileBoxTake::Impl::sort_level_with_time_and_area(tile_box_level_t& level)
    {
        sort(level.begin(),level.end(),[](const TileBox& a, const TileBox& b){
            return (a.t>b.t)||a.area<b.area;
        });
    }

    struct tile_box_filter{
        u32    maxLevel;
        double minx;
        double miny;
        double maxx;
        double maxy;
        static tile_box_filter make(u32 maxLevel,double minx,double miny,double maxx,double maxy){
            tile_box_filter f;
            f.maxLevel = maxLevel;
            f.minx = minx;
            f.miny = miny;
            f.maxx = maxx;
            f.maxy = maxy;
            return f;
        }
    };

    const TileBoxTake::TakeResult *TileBoxTake::Impl::take(u32 maxLevel, double xmin, double ymin, double xmax, double ymax,HandleTileAction* action)
    {
        const double e = 0.0000001;
        if(m_dirty)//resort the levels when dirty
        {
            for(auto& level:m_levels) 
                sort_level_with_time_and_area(*level); 
            
            m_extent = aabbox_t::make(1e10,1e10,-1e10,-1e10);

            for(auto& level:m_levels) 
                {
                    for(auto& bx:*level)
                    {
                        m_extent.minEdge.x = std::min(m_extent.minEdge.x,bx.minx);
                        m_extent.minEdge.y = std::min(m_extent.minEdge.y,bx.miny);
                        m_extent.maxEdge.x = std::max(m_extent.maxEdge.x,bx.maxx);
                        m_extent.maxEdge.y = std::max(m_extent.maxEdge.y,bx.maxy);
                    }
                }
            double d = m_extent.width()/8;
            m_extent.minEdge.x-=d;
            m_extent.maxEdge.x+=d;
            m_extent.minEdge.y-=d;
            m_extent.maxEdge.y+=d;

            m_scale_to_int64 = MAX_SCALE_INT64/(max(m_extent.width(),m_extent.height()));

            m_dirty=false;
        }
        if(maxLevel>m_levels.size()-1)
           return nullptr;
        auto filter_box = tile_box_filter::make(maxLevel,xmin,ymin,xmax,ymax);
        vector<TileBox*>    box_result;
        PathsD             polygon_result; 
        bool done = false;
        for(int i=maxLevel;i>=0&&!done;i--)
        {
            auto& level = *m_levels[i];
            for(auto& box:level)
            {
                if(is_box_like_intersect(filter_box,box))
                {
                    if(!is_box_like_in_polygon(box,polygon_result))
                    {
                        box_result.push_back(&box);
                        if(action&&!action->isTileLoaded(&box))
                            continue;
                        ClipperD clipper;
                        // 添加矩形作为被裁剪的主体 (Subject)
                        clipper.AddSubject(make_path_from_box_like(box,e));
                        PathsD result2;
                        // 添加多边形作为裁剪框 (Clip)
                        clipper.AddClip(polygon_result);
                        if(clipper.Execute(ClipType::Union,FillRule::NonZero,result2))
                        {
                            polygon_result = result2;
                            if(is_box_like_in_polygon(filter_box,polygon_result))
                            {
                                done = true;
                                break;
                            }
                        }

                    }
                }
            }
        }
        if(box_result.size() > 0)
        {
            reverse(box_result.begin(),box_result.end());
            size_t n = sizeof(TileBox*)*(box_result.size()+10);
            TakeResult *result = (TakeResult*)malloc(sizeof(TakeResult) + n);
            memcpy(result->tiles,box_result.data(),sizeof(TakeResult)+sizeof(TileBox*)*box_result.size());
            result->count=box_result.size();
            return result;
        }
        else
        {
            return nullptr;
        }
    }

    void TileBoxTake::Impl::freeResult(const TakeResult *result)
    {
        free((void*)result);
    }
    ///////////////////////////////////////////////////////////////////////////////
    TileBoxTake::TileBoxTake(int maxLevel)
    {
        impl = new Impl(maxLevel);
    }
    TileBoxTake::~TileBoxTake()
    {
        delete impl;
    }
    void TileBoxTake::registerTileBox(const TileBox &item)
    {
        return impl->registerTileBox(item);
    }
    const TileBoxTake::TakeResult *TileBoxTake::take(u32 maxLevel, double xmin, double ymin, double xmax, double ymax,HandleTileAction* action)
    {
        return impl->take(maxLevel, xmin, ymin, xmax, ymax,action);
    }
    void TileBoxTake::freeResult(const TakeResult *result)
    {
        if (!result)
            return;
        impl->freeResult(result);
    }

}