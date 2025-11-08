#include "fastdb.h"
using namespace wx;

const char* get_field_type_name(int ft)
{   
    switch(ft)
    {
    case ftF32:
        return "F32";
    case ftF64:
        return "F64";
    case ftI32:
        return "I32";
    case ftSTR:
        return "STR";
    case ftU16:
        return "U16";
    case ftU16n:
        return "U16n";
    case ftU32:
        return "U32";
    case ftU8:
        return "U8";
    case ftU8n:
        return "U8n";
    case ftREF:
        return "REF";    
    } 
    return "UNKNOWN";
}

const char* get_geometry_type_name(int gt)
{
    switch(gt)
    {
    case gtAny:
        return "Any";
    case gtLineString:
        return "LineString";
    case gtPoint:
        return "Point";
    case gtPolygon:
        return "Polygon";
    case gtNone:
        return "None";
    }
    return "UNKNOWN";
}

int main(int argc,char** argv) {
    printf("\n\
****************************************************\n\
            fastdb dumpping utility\n\
****************************************************\n");

    FastVectorDb* db = FastVectorDb::load(argv[1]);
    if (db) {
        for(int ix=0;ix<db->getLayerCount();ix++)
        {
            FastVectorDbLayer* layer = db->getLayer(ix);
            //枚举图层信息
            printf("\nLayer[%d]:%s geometry type:%s\n",ix,layer->name(),get_geometry_type_name(layer->getGeometryType()));
            double minx,miny,maxx,maxy;
            layer->getExtent(minx,miny,maxx,maxy);
            printf("\textent:(%lf,%lf,%lf,%lf)\n",minx,miny,maxx,maxy);
            int fc = layer->getFeatureCount();
            printf("\tfeature count:%d\n",fc);
            for(int fi=0;fi<layer->getFieldCount();fi++)
            {
                const char* name;
                FieldTypeEnum ft;
                double vmin,vmax;
                name = layer->getFieldDefn(fi,ft,vmin,vmax);
                printf("\tField[%d]:%s type:%s range(%lf,%lf)\n",fi,name,get_field_type_name(ft),vmin,vmax);
            }

            layer->rewind();
            printf("\tvalidate geometry.");
            int irec=0;
            while(layer->next())
            { 
                if(irec%100==0)
                    printf(".%d.",irec);
                irec++;
            }
            printf("...done!\n\tfeature count:%d\n",irec);
        }
        delete db;
    }
    else
    {
        printf("Can't open %s\n",argv[1]);
    }
    fflush(stdout);
}