#include "fastdb.h"
#include "ogrsf_frmts.h"
#include <vector>
#include <iostream>
#include <fstream>
#include <string>
#include <sstream>

using namespace std;
using namespace wx;

// 从字符串获取坐标类型
CoordinateFormatEnum getCoordinateType(const string &ct)
{
    if (ct == "tx16")
        return cfTx16;
    if (ct == "tx24")
        return cfTx24;
    if (ct == "tx32")
        return cfTx32;
    if (ct == "cf32")
        return cfF32;
    if (ct == "cf64")
        return cfF64;
    return cfF32;
}

// 导入OGR数据到FastDB的函数
void importOgrToFastDb(const char *layerName, OGRLayer *ogrLayer,
                       FastVectorDbBuild &build, CoordinateFormatEnum ct,
                       bool stableU32, string aabbox)
{
    auto layerDefn = ogrLayer->GetLayerDefn();
    auto ogrGeomType = layerDefn->GetGeomType();

    GeometryLikeEnum geomType = gtAny;
    if (ogrGeomType == wkbPoint)
    {
        geomType = gtPoint;
    }
    else if (ogrGeomType == wkbLineString || ogrGeomType == wkbMultiLineString)
    {
        geomType = gtLineString;
    }
    else if (ogrGeomType == wkbPolygon || ogrGeomType == wkbMultiPolygon)
    {
        geomType = gtPolygon;
    }

    if (geomType == gtAny)
    {
        cerr << "不支持的几何类型" << endl;
        return;
    }

    build.createLayerBegin(layerName);

    OGREnvelope extent;
    bool aabbox_enabled = true;
    if (ogrLayer->GetExtent(&extent) == OGRERR_NONE)
    {
        if (aabbox == "aabbox(no)")
        {
            aabbox_enabled = false;
        }
        else if (aabbox == "aabbox(auto)")
        {
            aabbox_enabled = true;
        }
        else
        {
            // 格式是aabbox(minx,miny,maxx,maxy)
            // double minx,miny,maxx,maxy;
            sscanf(aabbox.c_str(), "aabbox(%lf,%lf,%lf,%lf)", &extent.MinX, &extent.MinY, &extent.MaxX, &extent.MaxY);
            aabbox = true;
        }

        build.setGeometryType(geomType, ct, aabbox_enabled);
        build.enableStringTableU32(stableU32);
        build.setExtent(extent.MinX, extent.MinY, extent.MaxX, extent.MaxY);
    }
    else
    {
        cerr << "无法获取图层范围" << endl;
        return;
    }

    int fieldCount = layerDefn->GetFieldCount();
    vector<int> validFields;
    vector<OGRFieldType> fieldTypes;

    for (int i = 0; i < fieldCount; i++)
    {
        auto fieldDefn = layerDefn->GetFieldDefn(i);
        const char *fieldName = fieldDefn->GetNameRef();
        auto fieldType = fieldDefn->GetType();

        if (fieldType == OFTString || fieldType == OFTInteger ||
            fieldType == OFTInteger64 || fieldType == OFTReal)
        {
            validFields.push_back(i);
            fieldTypes.push_back(fieldType);

            if (fieldType == OFTString)
            {
                build.addField(fieldName, ftSTR);
                cout << "添加字段: " << fieldName << " \t type: String" << endl;
            }
            else if (fieldType == OFTInteger || fieldType == OFTInteger64)
            {
                build.addField(fieldName, ftI32);
                cout << "添加字段: " << fieldName << " \t type: Integer" << endl;
            }
            else if (fieldType == OFTReal)
            {
                build.addField(fieldName, ftF64);
                cout << "添加字段: " << fieldName << " \t type: Real" << endl;
            }
        }
    }

    ogrLayer->ResetReading();
    OGRFeature *feature = nullptr;
    vector<unsigned char> wkbBuffer;
    int irec = 0;
    printf("开始导入数据..");
    while ((feature = ogrLayer->GetNextFeature()) != nullptr)
    {
        auto geom = feature->GetGeometryRef();
        if (geom)
        {
            build.addFeatureBegin();
            size_t wkbSize = geom->WkbSize();
            wkbBuffer.resize(wkbSize);
            geom->exportToWkb(wkbNDR,wkbBuffer.data());
            build.setGeometry(wkbBuffer.data(), wkbSize, ginWKB);
            for (size_t i = 0; i < validFields.size(); i++)
            {
                if (fieldTypes[i] == OFTString)
                {
                    build.setField(i, feature->GetFieldAsString(validFields[i]));
                }
                else if (fieldTypes[i] == OFTInteger || fieldTypes[i] == OFTInteger64)
                {
                    build.setField(i, feature->GetFieldAsInteger(validFields[i]));
                }
                else if (fieldTypes[i] == OFTReal)
                {
                    build.setField(i, feature->GetFieldAsDouble(validFields[i]));
                }
            }
            build.addFeatureEnd();
            if(irec%1000==0)
            printf(".");
            
        }
        else
        {
            printf("\n第%d条记录几何为空,已忽略\n",irec);
        }
        irec++;
        OGRFeature::DestroyFeature(feature);
    }
    printf(".\n");
    build.createLayerEnd();
}
string trim(const string &str);
int main(int argc, char *argv[])
{
    if (argc != 2)
    {
        cerr << "用法: " << argv[0] << " <配置文件路径>" << endl;
        return 1;
    }

    ifstream configFile(argv[1]);
    if (!configFile)
    {
        cerr << "无法打开配置文件: " << argv[1] << endl;
        return 1;
    }

    // 增加对GDAL文件路径utf-8的支持
    CPLSetConfigOption("GDAL_FILENAME_IS_UTF8", "YES");
    CPLSetConfigOption("SHAPE_ENCODING", ""); // 自动检测Shapefile编码
    OGRRegisterAll();
    GDALAllRegister();

    string outputPath;
    getline(configFile, outputPath);
    outputPath = trim(outputPath); // 需要实现trim函数

    FastVectorDbBuild build;
    build.begin("");

    string line;
    while (getline(configFile, line))
    {
        if (line.empty() || line[0] == '#')
            continue;

        size_t pos = line.find("-l");
        if (pos == string::npos)
            continue;

        string sourcePart = line.substr(0, pos);
        string optionsPart = line.substr(pos + 2);

        // 分割源信息
        string dataPath, srcLayerName, driverName;
        // 检查是否指定了驱动器
        if (sourcePart[0] == '<')
        {
            auto driverEnd = sourcePart.find('>');
            if (driverEnd != string::npos)
            {
                driverName = sourcePart.substr(1, driverEnd - 1);
                sourcePart = sourcePart.substr(driverEnd + 1);
            }
        }
        else if (sourcePart.find(".shp") != string::npos)
        {
            // 如果文件扩展名是.shp，则默认使用Shapefile驱动器
            driverName = "ESRI Shapefile";
        }

        // 检查图层名称
        pos = sourcePart.find('!');
        if (pos != string::npos)
        {
            dataPath = sourcePart.substr(0, pos);
            srcLayerName = sourcePart.substr(pos + 1);
        }
        else
        {
            dataPath = sourcePart;
            srcLayerName = "";
        }

        // 分割选项
        istringstream iss(optionsPart);
        string dstLayerName, coordType, strTableType, aabboxOption;
        iss >> dstLayerName >> coordType >> strTableType >> aabboxOption;

        // 打开数据源
        dataPath = trim(dataPath);
        GDALDriver *driver = nullptr;
        if (!driverName.empty())
        {
            driver = GetGDALDriverManager()->GetDriverByName(driverName.c_str());
            if (!driver)
            {
                cerr << "无法找到驱动: " << driverName << endl;
                continue;
            }
        }
        const char *drivers[2] = {nullptr, nullptr};
        if (driver)
        {
            drivers[0] = driverName.c_str();
        }
        char **openOptions = nullptr;
        if (driverName == "ESRI Shapefile")
        {
            openOptions = CSLSetNameValue(openOptions, "ENCODING", ""); // 自动检测编码
        }
        // OGRDataSource* ds = OGRDataSource::Open(dataPath.c_str(), FALSE, drivers, openOptions);

        GDALDataset *ds = (GDALDataset *)GDALOpenEx(dataPath.c_str(),
                                                    GDAL_OF_VECTOR,
                                                    drivers,
                                                    openOptions,
                                                    nullptr);
        CSLDestroy(openOptions);
        if (!ds)
        {
            cerr << "无法打开数据源: " << dataPath << endl;
            continue;
        }

        OGRLayer *layer = nullptr;
        if (!srcLayerName.empty())
        {
            layer = ds->GetLayerByName(srcLayerName.c_str());
        }
        else
        {
            layer = ds->GetLayer(0); // 使用第一个图层
        }
        if (!layer)
        {
            cerr << "无法获取图层" << (srcLayerName.empty() ? "" : ": " + srcLayerName) << endl;
            GDALClose(ds);
            continue;
        }

        cout << "处理数据源: " << dataPath << "..." << endl;

        importOgrToFastDb(dstLayerName.c_str(),
                          layer,
                          build,
                          getCoordinateType(coordType),
                          strTableType == "st32",
                          aabboxOption);

        GDALClose(ds);
        cout << "...完成处理: " << dataPath << endl;
    }

    build.save(outputPath.c_str());
    cout << "数据导入完成" << endl;

    return 0;
}

// trim函数实现
string trim(const string &str)
{
    const string whitespace = " \t\n\r\f\v";
    size_t start = str.find_first_not_of(whitespace);
    if (start == string::npos)
        return "";

    size_t end = str.find_last_not_of(whitespace);
    return str.substr(start, end - start + 1);
}