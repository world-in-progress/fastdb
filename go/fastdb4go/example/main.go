package main

import (
	"fmt"
	"math/rand"

	core "github.com/world-in-progress/fastdb/go/fastdb4go/core"
)

func main() {
	db := core.NewWxDatabaseBuild()
	defer core.DeleteWxDatabaseBuild(db)

	layer0 := db.CreateLayerBegin("Layer0")

	layer0.SetGeometryType(core.GtPoint, core.CfTx32, true)
	layer0.SetExtent(-180, -90, 180, 90)

	layer0.AddField("name", uint(core.FtSTR))
	layer0.AddField("population", uint(core.FtI32))
	layer0.AddField("x", uint(core.FtF32))
	layer0.AddField("y", uint(core.FtF32))

	for i := 0; i < 100; i++ {
		layer0.AddFeatureBegin()

		layer0.SetField_cstring(uint(0), fmt.Sprintf("Point[%d]", i))
		layer0.SetField(uint(1), i)

		wkt := fmt.Sprintf("POINT(%f %f)", float64(i), float64(i))
		layer0.SetGeometryWKT(wkt)

		layer0.SetField(uint(2), float64(i))
		layer0.SetField(uint(3), float64(i))

		layer0.AddFeatureEnd()
	}

	layerRef := db.CreateLayerBegin("ref")

	layerRef.SetGeometryType(core.GtAny, core.CfTx32, true)
	layerRef.AddField("name", uint(core.FtSTR))
	layerRef.AddField("ref", uint(core.FtREF))

	for i := 0; i < 10; i++ {
		iref := rand.Intn(100)

		// CreateFeatureRef with no parameters uses auto-indexing
		fref := layer0.CreateFeatureRef()

		layerRef.AddFeatureBegin()
		layerRef.SetField_cstring(uint(0), fmt.Sprintf("Ref %d", iref))
		layerRef.SetField(uint(1), fref)

		layerRef.AddFeatureEnd()
	}

	db.Save("example_fastdb.fastdb")
	fmt.Println("Database created successfully!")
}
