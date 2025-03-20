cmake_minimum_required(VERSION 3.15)
project(MidiJam VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

# Force static linking of Boost libraries everywhere
set(Boost_USE_STATIC_LIBS ON)
set(Boost_USE_MULTITHREADED ON)
set(Boost_USE_STATIC_RUNTIME OFF) # Use dynamic runtime to avoid glibc issues

# Platform-Specific Library Linking
if(WIN32)
    set(PLATFORM_LIBS ws2_32 mswsock winmm -static -lpthread)
elseif(APPLE)
    set(PLATFORM_LIBS "-framework CoreMIDI" "-framework CoreAudio" "-framework CoreFoundation")
elseif(UNIX)
    find_package(ALSA QUIET)
    if(ALSA_FOUND)
        set(PLATFORM_LIBS asound pthread)
        add_definitions(-D__LINUX_ALSA__)
    else()
        find_package(Jack REQUIRED) # Require JACK
        set(PLATFORM_LIBS ${JACK_LIBRARIES} pthread)
        add_definitions(-D__UNIX_JACK__)
    endif()
endif()

set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -static-libgcc -static-libstdc++")

# Find Boost with system and json components
find_package(Boost 1.83 REQUIRED COMPONENTS system json)
include_directories(${Boost_INCLUDE_DIRS})

# RtMidi setup
set(RTMIDI_DIR ${CMAKE_SOURCE_DIR}/rtmidi)
if(NOT EXISTS ${RTMIDI_DIR}/RtMidi.h)
    message(FATAL_ERROR "RtMidi.h not found in ${RTMIDI_DIR}! Run build.sh first.")
endif()
include_directories(${RTMIDI_DIR})

add_library(rtmidi STATIC
    ${RTMIDI_DIR}/RtMidi.cpp
    ${RTMIDI_DIR}/rtmidi_c.cpp
)

# Platform Specific Definitions
if(WIN32)
    add_definitions(-D__WINDOWS_MM__)
elseif(APPLE)
    add_definitions(-D__MACOSX_CORE__)
endif()

# MIDI Utility Library
add_library(midi_utils STATIC ${CMAKE_SOURCE_DIR}/midi_utils.cpp)
target_link_libraries(midi_utils PRIVATE rtmidi ${PLATFORM_LIBS})

# Server executable
add_executable(MidiJamServer ${CMAKE_SOURCE_DIR}/server.cpp)
target_link_libraries(MidiJamServer PRIVATE Boost::system Boost::json ${PLATFORM_LIBS})

# Client executable
add_executable(MidiJamClient ${CMAKE_SOURCE_DIR}/client.cpp)
target_link_libraries(MidiJamClient PRIVATE Boost::system Boost::json midi_utils rtmidi ${PLATFORM_LIBS})

# Output directory
set_target_properties(MidiJamServer MidiJamClient PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_BINARY_DIR}"
    RUNTIME_OUTPUT_DIRECTORY_DEBUG "${CMAKE_BINARY_DIR}"
)

# Static Assets Handling
file(GLOB STATIC_ASSETS "${CMAKE_SOURCE_DIR}/static/*")
file(MAKE_DIRECTORY "${CMAKE_BINARY_DIR}/static")

foreach(ASSET ${STATIC_ASSETS})
    get_filename_component(ASSET_NAME ${ASSET} NAME)
    add_custom_command(
        TARGET MidiJamClient POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${ASSET}" "${CMAKE_BINARY_DIR}/static/${ASSET_NAME}"
        COMMENT "Copying ${ASSET_NAME} to static directory"
    )
endforeach()

# Install Targets (optional)
install(TARGETS MidiJamServer MidiJamClient DESTINATION bin)
install(DIRECTORY "${CMAKE_SOURCE_DIR}/static" DESTINATION bin/static)