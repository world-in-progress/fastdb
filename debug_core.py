from fastdb4py import core
try:
    print(f"core.gtAny: {core.gtAny}")
except:
    print("core.gtAny not found")

try:
    print(f"core.cfDefault: {core.cfDefault}")
except:
    print("core.cfDefault not found")

try:
    help(core.WxLayerTableBuild.set_geometry_type)
except:
    print("Help not available")
