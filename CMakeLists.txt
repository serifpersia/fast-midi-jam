cmake_minimum_required(VERSION 3.5)
project(MidiJam VERSION 1.0.0 LANGUAGES CXX)

# Set C++ Standard
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Detect cross-compilation
if(CMAKE_CROSSCOMPILING)
    message(STATUS "Cross-compiling for ${CMAKE_SYSTEM_PROCESSOR}")
    
    # Explicitly set paths for Boost
    set(Boost_LIBRARY_DIR /usr/lib/aarch64-linux-gnu)
    set(Boost_INCLUDE_DIR /usr/include)  # Use /usr/include instead of /usr/aarch64-linux-gnu/include
endif()

# Define build type if not set
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

# Force static linking of Boost libraries
set(Boost_USE_STATIC_LIBS ON)
set(Boost_USE_MULTITHREADED ON)
set(Boost_USE_STATIC_RUNTIME OFF) # Dynamic runtime to avoid glibc issues

# Static linking on Windows
if(WIN32)
    set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -static")
endif()

# Find Boost
find_package(Boost 1.66 REQUIRED COMPONENTS system)
if(Boost_FOUND)
    include_directories(${Boost_INCLUDE_DIRS})
else()
    message(FATAL_ERROR "Boost 1.66 or newer not found! Please install Boost 1.66 or later.")
endif()

# RtMidi setup
set(RTMIDI_DIR ${CMAKE_SOURCE_DIR}/rtmidi)
if(NOT EXISTS ${RTMIDI_DIR}/RtMidi.h)
    message(FATAL_ERROR "RtMidi.h not found in ${RTMIDI_DIR}! Run build script first.")
endif()
include_directories(${RTMIDI_DIR})

# Add RtMidi library
add_library(rtmidi STATIC
    ${RTMIDI_DIR}/RtMidi.cpp
    ${RTMIDI_DIR}/rtmidi_c.cpp
)

# Configure RtMidi for Linux
if(UNIX AND NOT APPLE)
    target_compile_definitions(rtmidi PRIVATE __LINUX_ALSA__)
    target_link_libraries(rtmidi PRIVATE pthread asound)
elseif(WIN32)
    target_compile_definitions(rtmidi PRIVATE __WINDOWS_MM__)
    target_link_libraries(rtmidi PRIVATE ws2_32 mswsock winmm)
endif()

# Include nlohmann/json
include_directories(${CMAKE_SOURCE_DIR}/third_party/nlohmann)

# Midi utilities
add_library(midi_utils STATIC ${CMAKE_SOURCE_DIR}/midi_utils.cpp)
target_link_libraries(midi_utils PRIVATE rtmidi)

# Server executable
add_executable(MidiJamServer ${CMAKE_SOURCE_DIR}/server.cpp)
target_link_libraries(MidiJamServer PRIVATE Boost::system)

if(UNIX AND NOT APPLE)
    target_link_libraries(MidiJamServer PRIVATE pthread)
elseif(WIN32)
    target_link_libraries(MidiJamServer PRIVATE ws2_32 mswsock)
endif()

# Client executable (depends on RtMidi)
add_executable(MidiJamClient ${CMAKE_SOURCE_DIR}/client.cpp)
target_link_libraries(MidiJamClient PRIVATE Boost::system midi_utils rtmidi stdc++fs)

# Set output directory
set_target_properties(MidiJamServer MidiJamClient PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
)

# Handle static assets
file(GLOB STATIC_ASSETS "${CMAKE_SOURCE_DIR}/static/*")
file(MAKE_DIRECTORY "${CMAKE_BINARY_DIR}/static")
foreach(ASSET ${STATIC_ASSETS})
    get_filename_component(ASSET_NAME ${ASSET} NAME)
    add_custom_command(
        TARGET MidiJamClient POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${ASSET}" "${CMAKE_BINARY_DIR}/static/${ASSET_NAME}"
    )
endforeach()

# Install targets
install(TARGETS MidiJamServer MidiJamClient DESTINATION bin)
install(DIRECTORY "${CMAKE_SOURCE_DIR}/static" DESTINATION bin/static)