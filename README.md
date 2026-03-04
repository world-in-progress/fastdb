# fastdb (WIP)

[![PyPI version](https://badge.fury.io/py/fastdb4py.svg)](https://badge.fury.io/py/fastdb4py)
[![Run Tests](https://github.com/world-in-progress/fastdb/actions/workflows/tests.yml/badge.svg)](https://github.com/world-in-progress/fastdb/actions/workflows/tests.yml)

**Wait and hope for the best...**

A C++ local database library with cross language bindings. Aiming to be a fast, lightweight, and easy-to-use data communication solution for RPC and coupled modeling in scientific computing.

## What's new
- **2026-03-04 (Memory Overflow Improvement)**: Enhanced the `MemoryStream` implementation to handle large data sizes exceeding 4GB without causing size overflow in `chunk_data_t.size` (u32). This improvement allows for more robust handling of large datasets in memory. (PR #22)
- **2026-02-28 (Release Improvement)**: Fix bugs related to build process in Windows. (PR #20)
- **2025-12-31(Bug Fix)**: Fixed an issue where shared memory segments were not being properly unregistered from the resource tracker upon closing, which could lead to resource leaks. (PR #17)
- **2025-12-15 (Release Improvement)**: Enabled distribution of pre-compiled binary wheels for macOS (Intel/Apple Silicon) and Linux (x86_64/aarch64), eliminating the need for local compilation tools during installation. (PR #15)
- **2025-12-10 (Bug Fix)**: Fixed the data type mapping for `U32` fields in Python bindings to ensure correct representation as unsigned 32-bit integers in NumPy arrays. (PR #13)
- **2025-12-10 (Bug Fix)**: Fixed an out-of-bounds access issue in `FastVectorDbLayer::Impl::getFieldOffset()` when the field index is equal to the field count. (PR #12)
- **2025-12-10 (Performance Improvement)**: Modified `ORM.truncate()` to support directly allocating features without initializing them for performance consideration. Note that this change may have side effects; please test thoroughly. (PR #11)

## Installation
You can install the Python package of fastdb via pip:

```bash
pip install fastdb4py
```

**Note:** Pre-compiled binary wheels are provided for major platforms (macOS-Intel/macOS-Apple Silicon, Linux-Ubuntu, Windows-AMD64). For other systems, the package will build from source, requiring a C++ compiler and CMake.

## Usage

### 1. Define a Feature (Schema)

To use `fastdb`, you first need to define your data schema by subclassing `fastdb4py.Feature`. 
Use type hints to define the fields of your feature.

```python
import fastdb4py

class Point(fastdb4py.Feature):
    x: fastdb4py.F64
    y: fastdb4py.F64
```

### 2. Create and Initialize a Database

You can create a new database or truncate an existing one using `fastdb4py.ORM.truncate`. 
This function takes a list of `TableDefn` objects, specifying the feature class and the initial capacity (number of rows).

```python
from pathlib import Path

# specific the path for the database
DB_PATH = "my_fastdb_data"

# Create a new database with a table for 'Point' features, capacity 1000
# The name parameter is optional; if not provided, a default name will be generated based on the feature class name.
# In this example, we explicitly set the table name to 'points'.
db = fastdb4py.ORM.truncate([
    fastdb4py.TableDefn(Point, 1000, name='points'),
])
```

### 3. Write Data

You can access the table using the feature class as a key. 
Features can be accessed by index or iterated over.

```python
# Access the table 'points' with schema defined by the Point feature class
points_table = db[Point]['points']
# If you did not specify the table name when creating the database, you can access it using the default name:
# points_table = db[Point][Point]
# or
# points_table = db[Point]['Point']

# Ensure we are in write mode (if loaded from file later)
# For a newly created DB in memory, we are already good to go.

for i in range(10):
    # Access the feature at index i
    p = points_table[i]
    
    # Set field values
    p.x = i * 1.5
    p.y = i * 2.5
    p.label = f"point_{i}"

# Save the database to disk
db.save(DB_PATH)
```

### 4. Read and Modify Data (Columnar Access)

`fastdb` supports high-performance columnar access using NumPy arrays. 
This allows for vectorized operations on your data.

```python
# Load the database from disk
db = fastdb4py.ORM.load(DB_PATH, from_file=True)
points_table = db[Point]['points']

# The length of the table (number of rows) can be obtained using len()
print(f"Number of points: {len(points_table)}")

# Access fields as numpy arrays via the `.column` property
xs = points_table.column.x
ys = points_table.column.y

print(f"First 5 X values: {xs[:5]}")

# Modify data in bulk using numpy operations
# This modifies the data in memory directly!
xs += 10.0 

# Verify the change via object access
print(f"Point 0 x: {points_table[0].x}")  # Should be 0 * 1.5 + 10.0 = 10.0
```

## Development Environment
This project uses DevContainer for development environment. Please refer to the `.devcontainer/devcontainer.example.json` file for configuration details.

For setting up the development environment, ensure you have Docker / Podman and VSCode DevContainer extension installed. Open the project in VSCode and create the `.devcontainer/devcontainer.json` file based on the example provided.

After connecting to the DevContainer, you can develop and test the project within the containerized environment.

### Python-Related Development

The `py_utils.sh` script is provided to facilitate common development tasks related to the Python bindings of fastdb. When first launching the DevContainer, `py_utils.sh` will automatically set up a Python virtual environment and install the necessary dependencies.

#### Cleaning Builds
```bash
# This operation will remove C++ build artifacts and the core Python bindings (fastdb.core, auto-generated by SWIG) within the Python package.
./py_utils.sh --clean
```

#### Building
```bash
# This operation will build the C++ core library and the Python bindings.
./py_utils.sh --build
```

#### Testing
```bash
# This operation will run the Python unit tests for the fastdb package.
./py_utils.sh --test
```
