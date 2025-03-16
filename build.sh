#!/bin/bash

set -e  # Exit immediately if a command exits with a non-zero status.

echo "Updating package list and installing required dependencies..."
sudo apt-get update
sudo apt-get install -y cmake g++ unzip libasound2-dev libjack-dev libboost-dev wget

# Download RtMidi if the directory doesn't exist
if [ ! -d "rtmidi" ]; then
  echo "Downloading RtMidi..."
  wget https://github.com/thestk/rtmidi/archive/refs/heads/master.zip -O master.zip
  unzip master.zip
  mkdir rtmidi
  cp rtmidi-master/*.h rtmidi/
  cp rtmidi-master/*.cpp rtmidi/
  rm master.zip
  rm -rf rtmidi-master
fi

# Remove existing build directory if it exists
if [ -d "build" ]; then
  echo "Removing existing build directory..."
  rm -rf build
fi

# Create and enter the build directory
mkdir build
cd build

# Configure the build with CMake
echo "Configuring build with CMake..."
cmake ..

# Build the project
echo "Building the project..."
make -j$(nproc)  # Use all available cores for faster building

echo "Build complete! Executables are in the build directory."
cd ..
