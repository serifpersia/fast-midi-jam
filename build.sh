#!/bin/bash

# Function to print usage
usage() {
    echo "Usage: $0 {server|client|both} [arm64]"
    exit 1
}

# Default target is both
TARGET="both"
ARCH="native"

# Parse arguments
if [ "$#" -ge 1 ]; then
    TARGET=$1
fi

if [ "$#" -eq 2 ] && [ "$2" == "arm64" ]; then
    ARCH="arm64"
fi

# Validate target
if [[ "$TARGET" != "server" && "$TARGET" != "client" && "$TARGET" != "both" ]]; then
    usage
fi

# Define paths
JSON_DIR="$(pwd)/third_party/nlohmann"
JSON_PATH="$JSON_DIR/json.hpp"

# Download RtMidi if the directory doesn't exist
if [ ! -d "rtmidi" ]; then
    echo "Downloading RtMidi..."
    wget -q "https://github.com/thestk/rtmidi/archive/refs/heads/master.zip" -O "master.zip" || {
        echo "Failed to download RtMidi!"
        exit 1
    }
    unzip -q "master.zip" || {
        echo "Failed to unzip RtMidi!"
        exit 1
    }
    mkdir -p rtmidi
    cp rtmidi-master/*.h rtmidi/
    cp rtmidi-master/*.cpp rtmidi/
    rm -f "master.zip"
    rm -rf rtmidi-master
fi

# Download json.hpp if it doesn't exist
if [ ! -f "$JSON_PATH" ]; then
    echo "Downloading nlohmann/json.hpp..."
    mkdir -p "$JSON_DIR"
    wget -q "https://raw.githubusercontent.com/nlohmann/json/v3.11.3/single_include/nlohmann/json.hpp" -O "$JSON_PATH" || {
        echo "Failed to download json.hpp!"
        exit 1
    }
    echo "json.hpp downloaded successfully."
else
    echo "json.hpp already exists. Skipping download."
fi

# Define paths
BUILD_DIR="build"
TOOLCHAIN_FILE="../aarch64-toolchain.cmake"

# Create build directory if it doesn't exist
mkdir -p "$BUILD_DIR" || { echo "Failed to create build directory"; exit 1; }
cd "$BUILD_DIR" || { echo "Failed to enter build directory"; exit 1; }

# Configure CMake
if [ "$ARCH" == "arm64" ]; then
    echo "Configuring CMake for ARM64 cross-compilation..."
    cmake -DCMAKE_TOOLCHAIN_FILE="$TOOLCHAIN_FILE" -DCMAKE_BUILD_TYPE=Release .. || {
        echo "CMake configuration failed!"
        exit 1
    }
else
    echo "Configuring CMake for native build..."
    cmake -DCMAKE_BUILD_TYPE=Release .. || {
        echo "CMake configuration failed!"
        exit 1
    }
fi

# Confirm target architecture
if [ "$ARCH" == "arm64" ]; then
    echo "Building for ARM64 (aarch64)..."
else
    echo "Building for native architecture ($(uname -m))..."
fi

# Build the target
case "$TARGET" in
    server)
        echo "Building MidiJamServer..."
        make -j$(nproc) MidiJamServer || { echo "Build failed!"; exit 1; }
        ;;
    client)
        echo "Building MidiJamClient..."
        make -j$(nproc) MidiJamClient || { echo "Build failed!"; exit 1; }
        ;;
    both)
        echo "Building both MidiJamServer and MidiJamClient..."
        make -j$(nproc) MidiJamServer MidiJamClient || { echo "Build failed!"; exit 1; }
        ;;
    *)
        usage
        ;;
esac

# Return to the original directory
cd ..