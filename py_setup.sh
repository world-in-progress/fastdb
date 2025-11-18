#!/bin/bash

set -e

RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m' # no color

# Check conda environment
echo -e "${YELLOW}Checking conda environment...${NC}"
if ! command -v conda &> /dev/null; then
    echo -e "${RED}Error: No conda found.${NC}"
    echo "Please install Anaconda or Miniconda from https://www.anaconda.com/products/distribution"
    exit 1
fi

echo -e "${YELLOW}Conda found. Verifying environment...${NC}"
python -c "from osgeo import gdal_array" 2>/dev/null || {
    echo -e "${RED}Error: GDAL is not installed in the current conda environment.${NC}"
    echo "Please create a conda environment with GDAL using the following command:"
    echo -e "${GREEN}conda create -n myenv gdal python=3.8${NC}"
    echo "Then activate it with:"
    echo -e "${GREEN}conda activate myenv${NC}"
    exit 1
}

python -c "import numpy" 2>/dev/null || {
    echo -e "${RED}Error: NumPy is not installed in the current conda environment.${NC}"
    echo "Please install it using the following command:"
    echo -e "${GREEN}conda install numpy${NC}"
    exit 1
}

echo -e "${GREEN}Conda environment is properly set up with GDAL and NumPy.${NC}"

# Handle --clean flag
if [[ "$1" == "--clean" ]]; then
    echo -e "${TELLOW}Cleaning previous builds...${NC}"
    rm -rf ./build
    rm -rf ./python/fastdb/core
    echo -e "${GREEN}Cleaned previous builds.${NC}"
    exit 0
fi

# Handle --build flag
if [[ "$1" == "--build" ]]; then
    echo -e "${YELLOW}Checking build tools...${NC}"

    # Check for CMake
    if ! command -v cmake &> /dev/null; then
        echo -e "${RED}Error: CMake is not installed.${NC}"
        echo "Please install CMake from https://cmake.org/download/ or via conda:"
        echo -e "${GREEN}conda install -c conda-forge cmake${NC}"
        exit 1
    fi

    # Check for SWIG
    if ! command -v swig &> /dev/null; then
        echo -e "${RED}Error: SWIG is not installed.${NC}"
        echo "Please install SWIG from http://www.swig.org/download.html or via conda:"
        echo -e "${GREEN}conda install -c conda-forge swig${NC}"
        exit 1
    fi

    # Check for a C++ compiler
    if ! command -v g++ &> /dev/null && ! command -v clang++ &> /dev/null; then
        echo -e "${RED}Error: No C++ compiler found.${NC}"
        echo "Please install g++ or clang++."
        exit 1
    fi

    echo -e "${GREEN}All build tools are available.${NC}"

    echo -e "${YELLOW}Starting build process...${NC}"
    mkdir -p build
    cd build
    cmake .. \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_PREFIX_PATH=$CONDA_PREFIX \
        -DUSE_SWIG_PYTHON=ON
    
    cmake --build . --config Release --parallel
    cd ..

    echo -e "${GREEN}Build completed successfully!${NC}"

    echo -e "${YELLOW}Let's check if fastdb is setup correctly...${NC}"
    if ! python ./examples/python/truncate_block.py; then
        echo -e "${RED}Opps, something must have gone wrong with the truncate functionality check.${NC}"
        exit 1
    fi
    echo -e "${GREEN}[1] Truncate functionality working!${NC}"

    if ! python ./examples/python/shared_memory.py; then
        echo -e "${RED}Opps, something must have gone wrong with the memory sharing functionality check.${NC}"
        exit 1
    fi
    echo -e "${GREEN}[2] Memory sharing functionality working!${NC}"

    if ! python ./examples/python/column_way.py; then
        echo -e "${RED}Opps, something must have gone wrong with the columnar storage functionality check.${NC}"
        exit 1
    fi
    echo -e "${GREEN}[3] Columnar storage functionality working!${NC}"

    echo -e "${GREEN}All checks passed. Fastdb is built and functioning correctly!${NC}"
    exit 0
fi

echo -e "${YELLOW}No valid flags provided. Use --clean to clean builds or --build to build the project.${NC}"
exit 1