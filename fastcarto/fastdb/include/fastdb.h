#ifndef __FASTDB_H__
#define __FASTDB_H__
#include "fastdb-config.h"
namespace wx
{
    enum GeometryLikeEnum
    {
        gtAny,//not just geometry
        gtPoint = 1,
        gtLineString,
        gtPolygon,
        gtNone=-1
    };


    enum CoordinateFormatEnum
    {
        cfF32 = 1,
        cfF64,
        // cfTx12,
        cfTx16, // 16bits coords, with bounding box clipping and normalized
        cfTx24,
        cfTx32,
        cfDefault=cfF32
    };

    enum FieldTypeEnum
    {
        ftU8 = 1,
        ftU16,
        ftU32,
        ftI32,
        ftU8n,
        ftU16n,
        ftF32,
        ftF64,
        ftSTR,
        ftWSTR,
        ftFeatureRef,
        ftREF=ftFeatureRef
    };
    enum GeometryLikeFormat
    {
        ginWKT = 1,
        ginWKB,
        ginPoint2,
        ginLineString,
        ginRAW
    };

    class WriteStream
    {
    public:
        virtual void write(void *pdata, size_t size) = 0;
    };

    struct chunk_data_t
    {
        u32 size;
        u8* pdata;
    };

    class MemoryStream :public WriteStream
    {
    public:
        class Impl;
    public:
        MemoryStream();
        ~MemoryStream();
        chunk_data_t data();  
        void reset();
        void write(void *pdata, size_t size) override;
    private:
        Impl* impl;
    };

    struct point2_t
    {
        double x;
        double y;
        static point2_t make(double x, double y)
        {
            return point2_t{x, y};
        }
    };

    class  FastVectorDbBuild;
    class  FastVectorDbLayerBuild;
    class  FastVectorDb;
    class  FastVectorDbLayer;
    class  FastVectorDbFeature;
    struct FastVectorDbFeatureRef;

    class fastdb_api FastVectorDbBuild
    {
    public:
        class Impl;
    public:
        FastVectorDbBuild();
        ~FastVectorDbBuild();
        //!
        void begin(const char *cfg);
        void truncate(const char *layerName, unsigned nfeatures);
        FastVectorDbLayerBuild*  createLayerBegin(const char *layerName);
        int  addField(const char *name, unsigned ft, double vmin = 0, double vmax = 1.0);
        void setGeometryType(GeometryLikeEnum gt, CoordinateFormatEnum ct=cfDefault, bool aabboxEnabled = false);
        void enableStringTableU32(bool b = true);
        void setExtent(double minx, double miny, double maxx, double maxy);
        void addFeatureBegin();
        void setGeometry(void *data, size_t size, GeometryLikeFormat fmt);
        void setField(unsigned ix, double value);
        void setField(unsigned ix, int value);
        void setField(unsigned ix, const char *text);
        void setField(unsigned ix, const wchar_t *text);
        void addFeatureEnd();
        void createLayerEnd();
        void post(WriteStream *stream);
        void save(const char *filename);

    public:
        inline void setGeometryWKT(const char *data)
        {
            setGeometry((void *)data, strlen(data), ginWKT);
        }
        inline void setGeometryWKB(const u8 *data, size_t size)
        {
            setGeometry((void *)data, size, ginWKB);
        }
        inline void setGeometryRaw(const u8 *data, size_t size)
        {
            setGeometry((void *)data, size, ginRAW);
        }
        inline void setField_cstring(unsigned ix, const char *text)
        {
            setField(ix, text);
        }
        inline void setField_wstring(unsigned ix, const wchar_t *text)
        {
            setField(ix, text);
        }    
    private:
        Impl *impl;
    };
    //
    class  fastdb_api FastVectorDbLayerBuild
    { 
    public:
        class Impl;
    public:
        FastVectorDbLayerBuild(FastVectorDbBuild* db,const char* name);
       ~FastVectorDbLayerBuild();
        const char* name();

        int  addField(const char *name, unsigned ft, double vmin = 0, double vmax = 1.0);
        void setGeometryType(GeometryLikeEnum gt, CoordinateFormatEnum ct, bool aabboxEnabled = false);
        void enableStringTableU32(bool b = true);
        void setExtent(double minx, double miny, double maxx, double maxy);
        void setDbIndex(int ix);
        void addFeatureBegin();
        void setGeometry(void *data, size_t size, GeometryLikeFormat fmt);
        void setField(unsigned ix, double value);
        void setField(unsigned ix, int value);
        void setField(unsigned ix, const char *text);
        void setField(unsigned ix, const wchar_t *text);
        void setField(unsigned ix, const FastVectorDbFeatureRef* ref);
        FastVectorDbFeatureRef* createFeatureRef(u32 ix=-1);
        void freeFeatureRef(FastVectorDbFeatureRef* ref);
        void addFeatureEnd();
    public:
        inline void setGeometryWKT(const char *data)
        {
            setGeometry((void *)data, strlen(data), ginWKT);
        }
        inline void setGeometryWKB(const u8 *data, size_t size)
        {
            setGeometry((void *)data, size, ginWKB);
        }
        inline void setGeometryRaw(const u8 *data, size_t size)
        {
            setGeometry((void *)data, size, ginRAW);
        }
        inline void setField_cstring(unsigned ix, const char *text)
        {
            setField(ix, text);
        }
        inline void setField_wstring(unsigned ix, const wchar_t *text)
        {
            setField(ix, text);
        }
    private:
        Impl* impl;
        friend class FastVectorDbBuild::Impl;
    };

    enum GeometryPartEnum
    {
        gptPoint2 = 1,
        // gptPoint3,
        gptLineString,
        gptRingExternal,
        gptRingInternal
    };

    class GeometryReturn
    {
    public:
        using GeometryPartEnum = wx::GeometryPartEnum;
    public:
        virtual bool begin(const double aabox[4]) = 0;
        virtual void returnGeomrtryPart(GeometryPartEnum partType, point2_t *points, int np) = 0;
        virtual void end() = 0;
    };

    typedef void (*fnFreeDbBuffer)(void *pdata, size_t size, void *pcookie);
    
    class fastdb_api FastVectorDb
    {
    public:
        class Impl;
    public:
        ~FastVectorDb();
        unsigned                getLayerCount();
        FastVectorDbLayer*      getLayer(unsigned ix);
        FastVectorDbFeature*    tryGetFeature(FastVectorDbFeatureRef* ref);
        chunk_data_t            buffer();
    public:
        static FastVectorDb *load(void *pdata, size_t size, fnFreeDbBuffer fnFreeBuffer, void *cookie);
        static FastVectorDb *load(const char *filename);
        static FastVectorDb *load_xbuffer(void* pdata, size_t size)//just for swig
        {
            return load((void*)pdata,size,NULL,NULL);
        }
    private:
        FastVectorDb(Impl *impl);
        Impl *impl;
    };

    struct aabbox_t
    {
        point2_t maxEdge;
        point2_t minEdge;
        double width() const { return maxEdge.x - minEdge.x; }
        double height() const { return maxEdge.y - minEdge.y; }
        double minx() const { return minEdge.x; }
        double miny() const { return minEdge.y; }
        double maxx() const { return maxEdge.x; }
        double maxy() const { return maxEdge.y; }

        static aabbox_t make(point2_t min, point2_t max)
        {
            aabbox_t r;
            r.minEdge = min;
            r.maxEdge = max;
            r.repair();
            return r;
        }
        void repair()
        {
            if (minEdge.x > maxEdge.x){
                double t = minEdge.x;
                minEdge.x = maxEdge.x;
                maxEdge.x = t;
            }
            if (minEdge.y > maxEdge.y)
            {
                double t = minEdge.y;
                minEdge.y = maxEdge.y;
                maxEdge.y = t;
            }
        }
        static aabbox_t make(double minx, double miny, double maxx, double maxy)
        {
            return make(point2_t::make(minx, miny), point2_t::make(maxx, maxy));
        }

        bool contains(const point2_t &p) const
        {
            return p.x >= minx() && p.x <= maxx() && p.y >= miny() && p.y <= maxy();
        }

        bool intersects(const aabbox_t &b) const
        {
            return maxx() >= b.minx() && minx() <= b.maxx() && maxy() >= b.miny() && miny() <= b.maxy();
        }

        bool contains(const aabbox_t &b) const
        {
            return maxx() >= b.maxx() && minx() <= b.minx() && maxy() >= b.maxy() && miny() <= b.miny();
        }
    };
    
    class  fastdb_api FastVectorDbLayer
    {
    public:
        class Impl;
    public:
        FastVectorDbLayer(Impl *impl);
       ~FastVectorDbLayer();
        const char*             name();
        GeometryLikeEnum        getGeometryType();
        unsigned                getFieldCount();
        const char*             getFieldDefn(unsigned ix, FieldTypeEnum &ft, double &vmin, double &vmax);
        size_t                  getFieldOffset(unsigned ix);
        size_t                  getFeatureByteSize();
        void                    getExtent(double &minx, double &miny, double &maxx, double &maxy);
        u32                     getFeatureCount();
        void                    rewind();
        bool                    next();
        int                     row();
        void                    fetchGeometry(GeometryReturn *cb);
        chunk_data_t            getGeometryLikeChunk();
        double                  getFieldAsFloat(u32 ix);
        int                     getFieldAsInt(u32 ix);
        const char*             getFieldAsString(u32 ix);
        const uchar_t*          getFieldAsWString(u32 ix);
        FastVectorDbFeatureRef* getFieldAsFeatureRef(u32 ix);
        void*                   setFeatureCookie(void *cookie);
        void*                   getFeatureCookie();
        FastVectorDbFeature*    tryGetFeatureAt(u32 ix);
    public:
        inline const char* getFieldDefn_p(unsigned ix, size_t *ft, double *vmin, double *vmax)
        {
            FieldTypeEnum ft2;
            auto retval =  getFieldDefn(ix, ft2, *vmin, *vmax);
            *ft = ft2;
            return retval;
        }
        inline void getExtent_p(double *minx, double *miny, double *maxx, double *maxy)
        {
            getExtent(*minx, *miny, *maxx, *maxy);
        }

        inline aabbox_t getAABBox()
        {
            aabbox_t box;
            getExtent(box.minEdge.x, box.minEdge.y, box.maxEdge.x, box.maxEdge.y);
            return box;
        }

        inline const char* getFieldName(unsigned ix)
        {
            FieldTypeEnum ft;
            double vmin, vmax;
            return getFieldDefn(ix, ft, vmin, vmax);
        }
        inline FieldTypeEnum getFieldType(unsigned ix)
        {
            FieldTypeEnum ft;
            double vmin, vmax;
            getFieldDefn(ix, ft, vmin, vmax);
            return ft;
        }
        
    private:
        Impl *impl;
        friend class FastVectorDbFeature;
        friend class FastVectorDb::Impl; 
    };
    
    class  fastdb_api FastVectorDbFeature
    {
    public:
        class Impl;
    public:
       ~FastVectorDbFeature();
        FastVectorDbLayer*      layer();
        void                    fetchGeometry(GeometryReturn *cb);
        chunk_data_t            getGeometryLikeChunk();
        double                  getFieldAsFloat(u32 ix);
        int                     getFieldAsInt(u32 ix);
        const char*             getFieldAsString(u32 ix);
        const uchar_t*          getFieldAsWString(u32 ix);
        FastVectorDbFeatureRef* getFieldAsFeatureRef(u32 ix);
        void*                   setFeatureCookie(void *cookie);
        void*                   getFeatureCookie();
    public:
        void*                   getAddress();
        void                    setField(u32 ix, double value);
        void                    setField(u32 ix, int    value);
        void                    setField(u32 ix, FastVectorDbFeature* feature);
    private:
        Impl*  impl;
        friend class FastVectorDbLayer::Impl;
    }; 
    class fastdb_api TileBoxTake
    {
        class Impl;
    public:
        struct TileBox
        {
            int id;         //图幅id
            u8  level;      //图幅比例尺
            double t;       //图幅更新时间
            double minx, miny, maxx, maxy;//图幅包围盒子
            double area;   //图幅面积
            void*  cookie; //图幅用户数据
            static TileBox make(int id, u8 level, double t, double minx, double miny, double maxx, double maxy, void *cookie = NULL)
            {
                auto area = (maxx-minx)*(maxy-miny);
                return TileBox{id, level, t, minx, miny, maxx, maxy,area,cookie};
            }
        };

        class HandleTileAction
        {
        public:
            virtual bool isTileLoaded(const TileBox* box)=0;
        };

        struct TakeResult
        {
        public:
            u32         count;
            TileBox*    tiles[1];
        };

    public:
        TileBoxTake(int maxLevel = 32);
        ~TileBoxTake();
        void                registerTileBox(const TileBox &item);
        const TakeResult*   take(u32 maxLevel, double xmin, double ymin, double xmax, double ymax,HandleTileAction* action=NULL);
        void                freeResult(const TakeResult* result);
    public:
        inline void registerTileBox(int id, u8 level, double t, double xmin, double ymin, double xmax, double ymax, void *cookie)
        {
            registerTileBox(TileBox::make(id,level,t,xmin,ymin,xmax,ymax,cookie));
        }
    private:
        Impl* impl;
    };

    class  fastdb_api FastVectorTileDb
    {
    public:
        class Impl;
    public:
        using TileData = FastVectorDb;
        struct TileDataHandle
        {
            const char* path;
            TileData*   tileData;
        };
        struct TileDbBox
        {
            int             id;     //图幅id
            u8              level;  //图幅比例尺
            double          t;      //图幅更新时间
            double          minx, miny, maxx, maxy;//图幅包围盒子
            double          area;   //图幅面积
            TileDataHandle* handle;//图幅用户数据
        };
        struct TakeResult
        {
        public:
            u32         count;
            TileDbBox*  tiles[1];
        };
    public:
        FastVectorTileDb(bool mt=false);
       ~FastVectorTileDb();
        void registerTile(const char* path,u8 level, double t, double xmin, double ymin, double xmax, double ymax);
        const TakeResult*   take(u32 maxLevel, double xmin, double ymin, double xmax, double ymax);
        void                freeResult(const TakeResult* result);
        void                shrink(u32 maxTileCache=32);
    protected:
        virtual TileData*   loadTileDataInternal(TileDataHandle*  loading);
    private:
        Impl* impl;
    };
#ifndef SWIG
    using WxDatabase = FastVectorDb;
    using WxLayerTable = FastVectorDbLayer;
    using WxFeature = FastVectorDbFeature;
    using WxFeatureRef = FastVectorDbFeatureRef;
    using WxTileDatabase = FastVectorTileDb;
    using WxDatabaseBuild = FastVectorDbBuild;
    using WxLayerTableBuild = FastVectorDbLayerBuild;
#endif
}
#endif