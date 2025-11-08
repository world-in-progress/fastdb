import sys
sys.path.append("../scripts")
import fastdb4py as fastdb
import random
db = fastdb.WxDatabaseBuild()
layer0 = db.create_layer_begin("layer0")
layer0.set_geometry_type(fastdb.gtPoint,fastdb.cfTx32,aabboxEnabled=True)
layer0.set_extent(-180, -90, 180, 90)
layer0.add_field("name", fastdb.ftSTR)
layer0.add_field("population", fastdb.ftI32)
layer0.add_field("x", fastdb.ftF32)
layer0.add_field("y", fastdb.ftF32)

for i in range(100):
    layer0.add_feature_begin()
    layer0.set_field_cstring(0,"Point[%d]" % i)
    layer0.set_field(1, i)
    layer0.set_geometry_wkt("POINT(%f %f)" % (i, i))
    layer0.set_field(2, float(i))
    layer0.set_field(3, i)
    layer0.add_feature_end()
    
layer_ref = db.create_layer_begin("ref")
layer_ref.set_geometry_type(fastdb.gtAny,fastdb.cfTx32,aabboxEnabled=True)
layer_ref.add_field("name", fastdb.ftSTR)
layer_ref.add_field("ref", fastdb.ftREF)
for i in range(10):
    iref = random.randint(0,99)
    fref = layer0.create_feature_ref(iref)
    layer_ref.add_feature_begin()
    layer_ref.set_field_cstring(0,"Ref %d" % iref)
    layer_ref.set_field(1, fref)
    layer_ref.set_geometry_raw(("POINT(%f %f)" % (i, i)).encode("utf-8"))
    layer_ref.add_feature_end()

db.save("./wx-demo.db")
    
    
    


