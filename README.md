# fastdb (v0.0.0 WIP)

A C++ local database library with cross language bindings. Aiming to be a fast, lightweight, and easy-to-use data communication solution for RPC and coupled modeling in scientific computing.

Wait and hope for the best...

## Building

Fastdb uses CMake as its build system. Below are instructions for building the core library and cross language bindings.

### Python Bindings

```bash
mkdir build
cd build
cmake .. -DCMAKE_BUILD_TYPE=Release -DBUILD_PYTHON_BINDINGS=ON
make fastdb4py -j4
```