#include "fastdb.h"
#include "fastdb-geometry-utils.h"
#include <vector>
using namespace std;

namespace wx{
    class TileBoxTake::Impl
    {
        using tile_box_level_t      = vector<TileBox>;
    public:
       Impl(int maxLevel);
      ~Impl();
        void                registerTileBox(const TileBox &item);
        const TakeResult*   take(u32 maxLevel, double xmin, double ymin, double xmax, double ymax,HandleTileAction* action);
        void                freeResult(const TakeResult* result);
    private:
        void sort_level_with_time_and_area(tile_box_level_t& level);
    private:
        vector<tile_box_level_t*>   m_levels;
        bool                        m_dirty;
        aabbox_t                    m_extent;
        long long                   m_scale_to_int64;
    };
}