import sys
import ctypes
sys.path.append("../scripts")
import fastdb4py as fastdb
db = None

bytes_data = open("./wx-demo.db", "rb").read()
#pdata = ctypes.c_char_p(bytes_data)
# address= ctypes.cast(pdata, ctypes.POINTER(ctypes.c_voidp))
#db = fastdb.WxDatabase.load_xbuffer(bytes_data)
db = fastdb.WxDatabase.load("./wx-demo.db")


layer0 = db.get_layer(0)
for i in range(layer0.get_field_count()):
    print(layer0.get_field_defn(i))
print(layer0.get_extent())
layer0.rewind()
gut = fastdb.GeometryUtils()
while(layer0.next()):
    r = layer0.row()
    f1 = layer0.get_field_as_string(0)
    f2 = layer0.get_field_as_int(1)
    x = layer0.get_field_as_float(2)
    y = layer0.get_field_as_float(3)
    layer0.fetchGeometry(gut)
    print(r,f1, f2,x,y,gut.point.x, gut.point.y)
print("layer ref----")
layer_ref = db.get_layer(1)
for i in range(layer_ref.get_feature_count()):
    ft = layer_ref.tryGetFeature(i)
    f1 = ft.get_field_as_string(0)
    fref = ft.get_field_as_ref(1)
    ft_l0 = db.tryGetFeature(fref)
    f1 = ft_l0.get_field_as_string(0)
    f2 = ft_l0.get_field_as_int(1)
    print("layer ref:",i,f1,"layer0",f1,f2)
    ft_l0.set_field(1,f2+123)
    f1 = ft_l0.get_field_as_string(0)
    f2 = ft_l0.get_field_as_int(1)
    print("updated ..\nlayer ref:",i,f1,"layer0",f1,f2)
    print("..")
    

    

    