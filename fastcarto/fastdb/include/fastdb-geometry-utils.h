#pragma once
#include <vector>
#include <string>
#include "fastdb.h"
using namespace std;

#ifndef WIN32
#include <cmath>
#endif

namespace wx
{
    class GeometryUtils : public GeometryReturn
    {
    public:
        double minx;
        double miny;
        double maxx;
        double maxy;
        GeometryLikeEnum gtype;
        point2_t point;
        std::vector<point2_t> points;
        std::vector<int> part_length;
        std::vector<size_t> part_first_point;
        std::vector<GeometryPartEnum> part_type;
        std::vector<int> polygon_part_count;
        std::vector<int> polygon_first_part;

    public:
        inline int get_point_count()
        {
            return points.size();
        }

        template <class pointT = point2_t>
        inline const pointT *point_data(int ix = 0)
        {
            if (sizeof(pointT) == sizeof(point2_t))
                return (const pointT *)points.data() + get_part_first_point(ix);
            else
                return NULL;
        }
        inline const int *part_data()
        {
            return part_length.data();
        }

        inline int get_part_count()
        {
            return part_length.size();
        }
        inline int get_part_length(int ix)
        {
            return part_length[ix];
        }

        inline const point2_t &get_point_at(int ix)
        {
            return points[ix];
        }

        inline int get_part_first_point(int ix)
        {
            return part_first_point[ix];
        }

        inline int get_polygon_part_count(int ix)
        {
            return polygon_part_count[ix];
        }

        inline int get_polygon_count()
        {
            return polygon_part_count.size();
        }

        inline int get_polygon_first_part(int ix)
        {
            return polygon_first_part[ix];
        }

        inline int np()
        {
            return points.size();
        }

    public:
        template <typename pprocT>
        void post_process_points(pprocT proc)
        {
            if (points.size())
                for (auto &p : points)
                    proc(p);
            else
            {
                proc(point);
            }
        }
        void udpate_bounding_box()
        {
            minx = points[0].x;
            miny = points[0].y;
            maxx = points[0].x;
            maxy = points[0].y;

            for (int i = 1, np = points.size(); i < np; i++)
            {
                if (points[i].x < minx)
                    minx = points[i].x;
                if (points[i].x > maxx)
                    maxx = points[i].x;
                if (points[i].y < miny)
                    miny = points[i].y;
                if (points[i].y > maxy)
                    maxy = points[i].y;
            }
        }

    public:
        virtual bool begin(const double aabbox[4])
        {
            if (aabbox)
            {
                minx = aabbox[0];
                miny = aabbox[1];
                maxx = aabbox[2];
                maxy = aabbox[3];
            }
            else
            {
                minx = miny = maxx = maxy = NAN;
            }
            points.clear();
            part_length.clear();
            part_type.clear();
            part_first_point.clear();
            polygon_first_part.clear();
            polygon_part_count.clear();

            return true;
        }
        virtual void returnGeomrtryPart(GeometryPartEnum partType, point2_t *pts, int np)
        {
            if (partType == gptPoint2)
            {
                gtype = gtPoint;
                point = pts[0];
            }
            else if (partType == gptLineString)
            {
                gtype = gtLineString;
            }
            else if (partType == gptRingExternal)
            {
                gtype = gtPolygon;
                polygon_first_part.push_back(part_length.size());
                polygon_part_count.push_back(1);
            }
            else if (partType == gptRingInternal)
            {
                polygon_part_count.back() += 1;
            }
            if (gtype != gtPoint)
            {
                part_first_point.push_back(points.size());
                part_length.push_back(np);
                part_type.push_back(partType);
                points.insert(points.end(), pts, pts + np);
            }
        }
        virtual void end()
        {
        }
    };
}