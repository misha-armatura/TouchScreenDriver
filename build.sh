#!/bin/bash

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

# Default values
BUILD_TYPE="Release"
BUILD_CSHARP=true
BUILD_TEST=true
CLEAN_BUILD=false

# Parse command line arguments
while [[ $# -gt 0 ]]; do
    case $1 in
        --debug)
            BUILD_TYPE="Debug"
            shift
            ;;
        --no-csharp)
            BUILD_CSHARP=false
            shift
            ;;
        --no-test)
            BUILD_TEST=false
            shift
            ;;
        --clean)
            CLEAN_BUILD=true
            shift
            ;;
        *)
            echo -e "${RED}Unknown option: $1${NC}"
            exit 1
            ;;
    esac
done

# Project root directory
PROJECT_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD_DIR="${PROJECT_ROOT}/build"

# Clean build if requested
if [ "$CLEAN_BUILD" = true ]; then
    echo -e "${YELLOW}Cleaning build directory...${NC}"
    rm -rf "${BUILD_DIR}"
fi

# Create build directory if it doesn't exist
mkdir -p "${BUILD_DIR}"
cd "${BUILD_DIR}"

# Configure CMake
echo -e "${YELLOW}Configuring CMake...${NC}"
cmake_args=(
    "-DCMAKE_BUILD_TYPE=${BUILD_TYPE}"
    "-DBUILD_TEST_PROGRAM=$([ "$BUILD_TEST" = true ] && echo ON || echo OFF)"
    "-DBUILD_CSHARP_EXAMPLE=$([ "$BUILD_CSHARP" = true ] && echo ON || echo OFF)"
)

cmake "${PROJECT_ROOT}" "${cmake_args[@]}"

# Build C++ library and test program
echo -e "${YELLOW}Building C++ components...${NC}"
cmake --build . --config "${BUILD_TYPE}" -j$(nproc)

if [ "$BUILD_CSHARP" = true ]; then
    echo -e "${YELLOW}Building C# components...${NC}"
    # First ensure the native library is built
    cmake --build . --target touch_reader --config "${BUILD_TYPE}"
    
    # Then build C# directly with dotnet to have more control
    cd "${PROJECT_ROOT}/src/csharp"
    dotnet build -p:RunAnalyzers=false -p:UseSharedCompilation=false -c "${BUILD_TYPE}" -o "${BUILD_DIR}/csharp" TouchScreenExample.csproj
    cd "${BUILD_DIR}"
    
    # Copy the native library to C# output
    cp "${BUILD_DIR}/lib/libtouch_reader.so" "${BUILD_DIR}/csharp/"
fi

# Check if build was successful
if [ $? -eq 0 ]; then
    echo -e "${GREEN}Build completed successfully!${NC}"
    echo -e "\nBuild artifacts:"
    echo -e "${YELLOW}C++ Library:${NC} ${BUILD_DIR}/lib/libtouch_reader.so"
    if [ "$BUILD_TEST" = true ]; then
        echo -e "${YELLOW}C++ Test Program:${NC} ${BUILD_DIR}/bin/touch_test"
    fi
    if [ "$BUILD_CSHARP" = true ]; then
        echo -e "${YELLOW}C# Program:${NC} ${BUILD_DIR}/csharp/TouchScreenExample"
    fi
else
    echo -e "${RED}Build failed!${NC}"
    exit 1
fi