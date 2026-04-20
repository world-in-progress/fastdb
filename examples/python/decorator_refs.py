"""
Decorator-based ORM2: REF Fields
==================================
Demonstrates nested @feature objects with automatic recursive push.

ORM2.push() automatically:
  1. Detects REF and List[REF] fields
  2. Recursively pushes dependencies first
  3. Deduplicates by object identity (shared refs preserved)
"""

from fastdb4py import feature, ORM2, F64, U32, STR


@feature
class Material:
    name: STR
    roughness: F64


@feature
class Mesh:
    label: STR
    vertex_count: U32
    material: Material   # REF field — auto resolved


if __name__ == '__main__':
    orm = ORM2.create()

    # Shared materials (will be deduplicated across meshes)
    metal = Material()
    metal.name = "brushed_metal"
    metal.roughness = 0.3

    wood = Material()
    wood.name = "oak_wood"
    wood.roughness = 0.7

    # Create meshes that share the same material
    for label, mat, vc in [
        ("floor",   wood,  4),
        ("wall_a",  wood,  4),
        ("frame",   metal, 12),
        ("handle",  metal, 8),
    ]:
        m = Mesh()
        m.label = label
        m.vertex_count = vc
        m.material = mat   # REF — push() handles this automatically
        orm.push(m)

    orm.combine()

    print(f"Meshes:    {orm.count(Mesh)}")
    print(f"Materials: {orm.count(Material)}  (2 unique, deduplicated)\n")

    # Read back — copy mode gives detached instances
    print("=== All meshes ===")
    for mesh in orm.iter(Mesh, mode='copy'):
        print(f"  {mesh.label}: vertices={mesh.vertex_count}")

    print("\n✓ REF fields with deduplication example complete.")
