cmake_minimum_required(VERSION 3.15)
project(MidiJam VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

# Platform-Specific Library Linking
if(WIN32)
    set(WIN_LIBS ws2_32 mswsock winmm)  # Winsock & MIDI support
else()
    set(WIN_LIBS "")
endif()

# Static linking for MinGW
if(MINGW)
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -static -static-libgcc -static-libstdc++")
    set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -static")
    set(WIN_LIBS ${WIN_LIBS} -lstdc++ -lgcc)
endif()

# Force static Boost libraries
set(Boost_USE_STATIC_LIBS ON)
find_package(Boost 1.74 REQUIRED COMPONENTS asio beast json)
include_directories(${Boost_INCLUDE_DIRS})

# RtMidi setup
set(RTMIDI_DIR ${CMAKE_SOURCE_DIR}/rtmidi)
if(NOT EXISTS ${RTMIDI_DIR}/RtMidi.h)
    message(FATAL_ERROR "RtMidi.h not found in ${RTMIDI_DIR}! Run build.bat first.")
endif()
include_directories(${RTMIDI_DIR})

add_library(rtmidi STATIC
    ${RTMIDI_DIR}/RtMidi.cpp
    ${RTMIDI_DIR}/rtmidi_c.cpp
)

# MIDI and Networking Libraries
if(WIN32)
    add_definitions(-D__WINDOWS_MM__)
elseif(APPLE)
    add_definitions(-D__MACOSX_CORE__)
    set(WIN_LIBS "-framework CoreMIDI" "-framework CoreAudio" "-framework CoreFoundation")
elseif(UNIX)
    find_package(ALSA QUIET)
    if(ALSA_FOUND)
        add_definitions(-D__LINUX_ALSA__)
        set(WIN_LIBS asound pthread)
    else()
        add_definitions(-D__UNIX_JACK__)
        set(WIN_LIBS jack pthread)
    endif()
endif()

# MIDI Utility Library
add_library(midi_utils STATIC ${CMAKE_SOURCE_DIR}/midi_utils.cpp)
target_link_libraries(midi_utils PRIVATE rtmidi ${WIN_LIBS})

# Server executable
add_executable(MidiJamServer ${CMAKE_SOURCE_DIR}/server.cpp)
target_link_libraries(MidiJamServer PRIVATE Boost::asio ${WIN_LIBS})

# Client executable (Uses HTTP Server Lib)
add_executable(MidiJamClient ${CMAKE_SOURCE_DIR}/client.cpp)
target_link_libraries(MidiJamClient PRIVATE Boost::asio Boost::beast Boost::json midi_utils rtmidi ${WIN_LIBS})

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