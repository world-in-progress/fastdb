#!/bin/bash

set -e

RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m' # no color

# Handle --clean flag
if [[ "$1" == "--clean" ]]; then
    echo -e "${TELLOW}Cleaning previous builds...${NC}"
    rm -rf ./dist
    rm -rf ./fastcarto/build
    rm -rf ./python/fastdb4py/core
    rm -rf ./python/fastdb4py.egg-info
    rm -rf .venv
    uv cache clean
    echo -e "${GREEN}Cleaned previous builds.${NC}"
    exit 0
fi

# Handle --test flag
if [[ "$1" == "--test" ]]; then
    echo -e "${YELLOW}Running tests...${NC}"

    if ! uv run pytest ./tests/python; then
        echo -e "${RED}Some tests failed. Please check the output above for details.${NC}"
        exit 1
    fi

    echo -e "${GREEN}All checks passed. Fastdb is built and functioning correctly!${NC}"
    exit 0
fi

# Handle --setup flag with --py flag (for python version)
if [[ "$1" == "--setup" || "$2" == "--py" ]]; then
    echo -e "${YELLOW}Checking setup tools...${NC}"

    # Check for UV
    if ! command -v uv &> /dev/null; then
        echo -e "${RED}Error: UV is not installed.${NC}"
        echo "Please install libuv from https://docs.astral.sh/uv/:"
        exit 1
    fi

    # Install specifc Python version if needed
    PY_VERSION="$3"
    if [[ -n "$PY_VERSION" ]]; then
        echo -e "${YELLOW}Setting up Python $PY_VERSION environment...${NC}"
        if ! uv python install "$PY_VERSION"; then
            echo -e "${RED}Error: Failed to install Python $PY_VERSION using UV.${NC}"
            exit 1
        fi
        uv python pin "$PY_VERSION"
        echo -e "${GREEN}Using Python version: $(uv run python --version)${NC}"
    else
        echo -e "${YELLOW}Using current Python environment: $(uv run python --version)${NC}"
    fi

    # Setup process
    echo -e "${YELLOW}Starting setup process...${NC}"
    if ! uv sync; then
        echo -e "${RED}Error: UV sync failed.${NC}"
        exit 1
    fi
    if ! uv pip install build; then
        echo -e "${RED}Error: Failed to install build module in UV environment.${NC}"
        exit 1
    fi
    if ! uv pip install -e .; then
        echo -e "${RED}Error: Failed to install fastdb in editable mode using UV.${NC}"
        exit 1
    fi

    echo -e "${GREEN}Setup completed successfully!${NC}"
    exit 0
fi

echo -e "${YELLOW}No valid flags provided. Use --clean to clean builds, --setup to setup the project, or --test to run tests.${NC}"
exit 1