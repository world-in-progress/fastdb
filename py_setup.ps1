# PowerShell script for setting up FastDB Python bindings on Windows

param(
    [switch]$Clean,
    [switch]$Build
)

# Color output functions
function Write-ColorOutput($ForegroundColor) {
    $fc = $host.UI.RawUI.ForegroundColor
    $host.UI.RawUI.ForegroundColor = $ForegroundColor
    if ($args) {
        Write-Output $args
    }
    $host.UI.RawUI.ForegroundColor = $fc
}

function Write-Red($message) { Write-ColorOutput Red $message }
function Write-Green($message) { Write-ColorOutput Green $message }
function Write-Yellow($message) { Write-ColorOutput Yellow $message }

# Check conda environment
Write-Yellow "Checking conda environment..."
$condaCmd = Get-Command conda -ErrorAction SilentlyContinue
if (-not $condaCmd) {
    Write-Red "Error: No conda found."
    Write-Host "Please install Anaconda or Miniconda from https://www.anaconda.com/products/distribution"
    exit 1
}

Write-Yellow "Conda found. Verifying environment..."

# Check GDAL
python -c "from osgeo import gdal_array" 2>$null
if ($LASTEXITCODE -ne 0) {
    Write-Red "Error: GDAL is not installed in the current conda environment."
    Write-Host "Please create a conda environment with GDAL using the following command:"
    Write-Green "conda create -n myenv gdal python=3.8"
    Write-Host "Then activate it with:"
    Write-Green "conda activate myenv"
    exit 1
}

# Check NumPy
python -c "import numpy" 2>$null
if ($LASTEXITCODE -ne 0) {
    Write-Red "Error: NumPy is not installed in the current conda environment."
    Write-Host "Please install it using the following command:"
    Write-Green "conda install numpy"
    exit 1
}

Write-Green "Conda environment is properly set up with GDAL and NumPy."

# Handle --clean flag
if ($Clean) {
    Write-Yellow "Cleaning previous builds..."
    if (Test-Path "./build") {
        Remove-Item -Recurse -Force "./build"
    }
    if (Test-Path "./python/fastdb/core") {
        Remove-Item -Recurse -Force "./python/fastdb/core"
    }
    Write-Green "Cleaned previous builds."
    exit 0
}

# Handle --build flag
if ($Build) {
    Write-Yellow "Checking build tools..."

    # Check for CMake
    $cmakeCmd = Get-Command cmake -ErrorAction SilentlyContinue
    if (-not $cmakeCmd) {
        Write-Red "Error: CMake is not installed."
        Write-Host "Please install CMake from https://cmake.org/download/ or via conda:"
        Write-Green "conda install -c conda-forge cmake"
        exit 1
    }

    # Check for SWIG
    $swigCmd = Get-Command swig -ErrorAction SilentlyContinue
    if (-not $swigCmd) {
        Write-Red "Error: SWIG is not installed."
        Write-Host "Please install SWIG from http://www.swig.org/download.html or via conda:"
        Write-Green "conda install -c conda-forge swig"
        exit 1
    }

    # Check for a C++ compiler (cl.exe for MSVC or g++/clang++)
    # $compilerFound = $false
    # $clCmd = Get-Command cl -ErrorAction SilentlyContinue
    # $gppCmd = Get-Command g++ -ErrorAction SilentlyContinue
    # $clangCmd = Get-Command clang++ -ErrorAction SilentlyContinue
    
    # if ($clCmd -or $gppCmd -or $clangCmd) {
    #     $compilerFound = $true
    # }

    # if (-not $compilerFound) {
    #     Write-Red "Error: No C++ compiler found."
    #     Write-Host "Please install Visual Studio with C++ tools, MinGW, or LLVM/Clang."
    #     exit 1
    # }

    Write-Green "All build tools are available."

    Write-Yellow "Starting build process..."
    if (-not (Test-Path "./build")) {
        New-Item -ItemType Directory -Path "./build" | Out-Null
    }
    
    Set-Location "./build"

    if (-not $env:CONDA_PREFIX) {
        Write-Red "Error: CONDA_PREFIX environment variable is not set."
        Write-Host "Please activate your conda environment first."
        exit 1
    }
    Write-Host "CONDA_PREFIX is set to: $env:CONDA_PREFIX"
    
    $condaPrefix = $env:CONDA_PREFIX    
    cmake .. `
        -DCMAKE_BUILD_TYPE=Release `
        -DCMAKE_PREFIX_PATH="$condaPrefix" `
        -DGDAL_INCLUDE_DIR="$condaPrefix/Library/include" `
        -DGDAL_LIBRARY="$condaPrefix/Library/lib/gdal.lib" `
        -DUSE_SWIG_PYTHON=ON
    
    if ($LASTEXITCODE -ne 0) {
        Write-Red "CMake configuration failed."
        Set-Location ..
        exit 1
    }
    
    cmake --build . --config Release --parallel
    
    if ($LASTEXITCODE -ne 0) {
        Write-Red "Build failed."
        Set-Location ..
        exit 1
    }
    
    Set-Location ..

    Write-Green "Build completed successfully!"

    Write-Yellow "Let's check if fastdb is setup correctly..."
    
    python ./examples/python/truncate_block.py
    if ($LASTEXITCODE -ne 0) {
        Write-Red "Opps, something must have gone wrong with the truncate functionality check."
        exit 1
    }
    Write-Green "[1] Truncate functionality working!"

    python ./examples/python/shared_memory.py
    if ($LASTEXITCODE -ne 0) {
        Write-Red "Opps, something must have gone wrong with the memory sharing functionality check."
        exit 1
    }
    Write-Green "[2] Memory sharing functionality working!"

    python ./examples/python/column_way.py
    if ($LASTEXITCODE -ne 0) {
        Write-Red "Opps, something must have gone wrong with the columnar storage functionality check."
        exit 1
    }
    Write-Green "[3] Columnar storage functionality working!"

    Write-Green "All checks passed. Fastdb is built and functioning correctly!"
    exit 0
}

# No valid flags provided
Write-Yellow "No valid flags provided. Use -Clean to clean builds or -Build to build the project."
Write-Host ""
Write-Host "Examples:"
Write-Host "  .\py_setup.ps1 -Clean"
Write-Host "  .\py_setup.ps1 -Build"
exit 1
