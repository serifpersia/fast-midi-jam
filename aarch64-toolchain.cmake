set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR aarch64)

# Use ARM64 cross-compiler
set(CMAKE_C_COMPILER aarch64-linux-gnu-gcc)
set(CMAKE_CXX_COMPILER aarch64-linux-gnu-g++)

# Linker settings to avoid glibc issues
set(CMAKE_EXE_LINKER_FLAGS "-static-libgcc -static-libstdc++")
