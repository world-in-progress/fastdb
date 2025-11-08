let fastdb = require("../scripts/fastdb")
console.log(fastdb)
let db = fastdb.WxDatabase.load("./wx-demo.db")
console.log(db)
let layer0 = db.get_layer(0)
console.log(layer0)

let c1 = layer0.get_column_as_array(1)
console.log(c1.length)
for(var i=0;i<c1.length;i++){
    console.log(c1[i])
}

let f0 = layer0.tryGetFeature(0)
console.log(f0)
let chunk=f0.get_geometry_like_chunk()
console.log(chunk)
let bytes = chunk.asBufferArray('uint8');
console.log(bytes)