# fastdb (v0.1.0 WIP)

A C++ local database library with cross language bindings. Aiming to be a fast, lightweight, and easy-to-use data communication solution for RPC and coupled modeling in scientific computing.

Wait and hope for the best...

## Installation
You can install the Python bindings of fastdb via pip:

```bash
pip install fastdb4py
```

Note: The package will build from source during installation for your specific platform. Ensure you have the necessary build tools and dependencies (e.g., a C++ compiler, CMake) installed.

## Development Environment
This project uses DevContainer for development environment. Please refer to the `.devcontainer/devcontainer.example.json` file for configuration details.

For setting up the development environment, ensure you have Docker / Podman and VSCode DevContainer extension installed. Open the project in VSCode and create the `.devcontainer/devcontainer.json` file based on the example provided.

After connecting to the DevContainer, you can develop and test the project within the containerized environment.

### Python-Related Development

#### Cleaning Builds
```bash
# Inside the DevContainer
./py_utils.sh --clean
```

#### Building
```bash
# Inside the DevContainer
./py_utils.sh --build
```

#### Testing
```bash
# Inside the DevContainer
./py_utils.sh --test
```
