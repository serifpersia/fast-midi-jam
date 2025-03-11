cmake_minimum_required(VERSION 3.15)
project(MidiJam VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

# Add static flag for MinGW
if(MINGW)
    # Add flags for static linking
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -static -static-libgcc -static-libstdc++")
    
    # Ensure the system libraries (winmm, ws2_32, etc.) are linked statically
    set(MIDI_LIBS 
        ${MIDI_LIBS} 
        -lwinmm -lws2_32 -lstdc++ -lgcc
    )
endif()

# Find Boost
find_package(Boost 1.74 REQUIRED COMPONENTS asio)
include_directories(${Boost_INCLUDE_DIRS})

# RtMidi setup
set(RTMIDI_DIR ${CMAKE_SOURCE_DIR}/rtmidi)
if(NOT EXISTS ${RTMIDI_DIR}/RtMidi.h)
    message(FATAL_ERROR "RtMidi.h not found in ${RTMIDI_DIR}! Run build.bat first.")
endif()
include_directories(${RTMIDI_DIR})
message(STATUS "RtMidi include dir: ${RTMIDI_DIR}")

add_library(rtmidi STATIC
    ${RTMIDI_DIR}/RtMidi.cpp
    ${RTMIDI_DIR}/rtmidi_c.cpp
)

# Platform-specific MIDI and networking libraries
if(WIN32)
    add_definitions(-D__WINDOWS_MM__)
    set(MIDI_LIBS winmm ws2_32) # Added ws2_32 for Winsock
elseif(APPLE)
    add_definitions(-D__MACOSX_CORE__)
    set(MIDI_LIBS "-framework CoreMIDI" "-framework CoreAudio" "-framework CoreFoundation")
elseif(UNIX)
    find_package(ALSA QUIET)
    if(ALSA_FOUND)
        add_definitions(-D__LINUX_ALSA__)
        set(MIDI_LIBS asound pthread)
    else()
        add_definitions(-D__UNIX_JACK__)
        set(MIDI_LIBS jack pthread)
    endif()
endif()

# Midi Utils library
add_library(midi_utils STATIC
    ${CMAKE_SOURCE_DIR}/midi_utils.cpp
)
target_link_libraries(midi_utils PRIVATE rtmidi ${MIDI_LIBS})

# Server executable
add_executable(MidiJamServer ${CMAKE_SOURCE_DIR}/server.cpp)
target_link_libraries(MidiJamServer PRIVATE Boost::asio ${MIDI_LIBS}) # Link ws2_32 here

# Client executable
add_executable(MidiJamClient ${CMAKE_SOURCE_DIR}/client.cpp)
target_link_libraries(MidiJamClient PRIVATE Boost::asio midi_utils rtmidi ${MIDI_LIBS}) # Link ws2_32 here

# Output directory
set_target_properties(MidiJamServer MidiJamClient PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_BINARY_DIR}"
    RUNTIME_OUTPUT_DIRECTORY_DEBUG "${CMAKE_BINARY_DIR}"
)
