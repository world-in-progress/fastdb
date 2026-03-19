import fastdb4py as fdb
from fastdb4py import FastSerializer

class Point(fdb.Feature):
    name: str
    x: float
    y: float
    z: float
    uv: list[fdb.F64]

class PointCloud(fdb.Feature):
    points: list[Point]

if __name__ == '__main__':
    p1 = Point(name='Point 1', x=1.0, y=2.0, z=3.0, uv=[0.0, 0.0])
    p2 = Point(name='Point 2', x=4.0, y=5.0, z=6.0, uv=[1.0, 1.0])
    point_cloud = PointCloud()
    point_cloud.points.append(p1)
    point_cloud.points.append(p2)
    
    # Serialize to bytes
    serialized_data = FastSerializer.dumps(point_cloud)
    
    # Deserialize from bytes
    deserialized_point_cloud = FastSerializer.loads(serialized_data, PointCloud)
    
    for i, point in enumerate(deserialized_point_cloud.points):
        print(f'Point {i}: x={point.x}, y={point.y}, z={point.z}, name={point.name}, uv={point.uv}')
