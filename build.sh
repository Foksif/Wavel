
#!/usr/bin/env bash

set -e

BUILD_DIR="build"
BUILD_TYPE="${1:-Debug}"

echo "Building Wavel ($BUILD_TYPE)..."

cmake -B "$BUILD_DIR" -G Ninja \
    -DCMAKE_BUILD_TYPE="$BUILD_TYPE"

cmake --build "$BUILD_DIR" --parallel

echo "Build completed!"

if [ -f "$BUILD_DIR/wavel" ]; then
    echo "Binary: $BUILD_DIR/wavel"
fi

