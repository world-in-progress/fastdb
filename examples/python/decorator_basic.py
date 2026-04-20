"""
Decorator-based ORM2: Basic Usage
==================================
Demonstrates the @feature decorator with ORM2 lifecycle:
  1. Define plain classes with @feature
  2. Push instances with ORM2.create() / push()
  3. Finalize with combine()
  4. Read back with get(mode='map') or get(mode='copy')
  5. Iterate with iter()
"""

from fastdb4py import feature, ORM2, F64, U32, STR


@feature
class City:
    name: STR
    population: U32
    latitude: F64
    longitude: F64


if __name__ == '__main__':
    # 1. Create an ORM2 build session
    orm = ORM2.create()

    # 2. Push plain Python objects — no subclass needed
    cities_data = [
        ("Tokyo",     13_960_000, 35.6762, 139.6503),
        ("Paris",      2_161_000, 48.8566,   2.3522),
        ("New York",   8_336_000, 40.7128, -74.0060),
        ("Sydney",     5_312_000, -33.8688, 151.2093),
        ("São Paulo", 12_325_000, -23.5505, -46.6333),
    ]
    for name, pop, lat, lng in cities_data:
        c = City()
        c.name = name
        c.population = pop
        c.latitude = lat
        c.longitude = lng
        orm.push(c)

    # 3. Finalize — builds read-only database
    orm.combine()
    print(f"Pushed {orm.count(City)} cities\n")

    # 4a. Read back with 'map' mode (zero-copy proxy, read-only)
    print("=== Map mode (zero-copy, read-only) ===")
    mapped = orm.get(City, 0, mode='map')
    print(f"  {mapped.name}: pop={mapped.population}, "
          f"lat={mapped.latitude:.4f}, lng={mapped.longitude:.4f}")
    print(f"  type: {type(mapped).__name__}")

    # 4b. Read back with 'copy' mode (detached Python instance)
    print("\n=== Copy mode (detached, mutable) ===")
    copied = orm.get(City, 0, mode='copy')
    print(f"  {copied.name}: pop={copied.population}")
    print(f"  type: {type(copied).__name__}")
    copied.population = 14_000_000  # mutable!
    print(f"  after mutation: pop={copied.population}")

    # 5. Iterate all cities
    print("\n=== Iterate all (copy mode) ===")
    for city in orm.iter(City, mode='copy'):
        print(f"  {city.name}: ({city.latitude:.2f}, {city.longitude:.2f})")

    print("\n✓ Basic decorator ORM2 example complete.")
