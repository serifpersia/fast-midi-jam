#include "midi_utils.h"
#include <limits> // Added for std::numeric_limits
#include <iostream>

void MidiUtils::listDevices(RtMidiIn& midiIn, RtMidiOut& midiOut) {
    std::cout << "Available MIDI input ports:\n";
    for (unsigned int i = 0; i < midiIn.getPortCount(); ++i) {
        try {
            std::cout << i << ": " << midiIn.getPortName(i) << "\n";
        } catch (...) {
            std::cout << i << ": <error reading port name>\n";
        }
    }
    std::cout << "Available MIDI output ports:\n";
    for (unsigned int i = 0; i < midiOut.getPortCount(); ++i) {
        try {
            std::cout << i << ": " << midiOut.getPortName(i) << "\n";
        } catch (...) {
            std::cout << i << ": <error reading port name>\n";
        }
    }
    std::cout.flush();
}

unsigned int MidiUtils::selectInputDevice(RtMidiIn& midiIn) {
    while (true) {
        std::cout << "Select input port (0 to " << (midiIn.getPortCount() - 1) << "): ";
        unsigned int deviceIndex;
        if (std::cin >> deviceIndex && deviceIndex < midiIn.getPortCount()) {
            return deviceIndex;
        }
        std::cin.clear();
        std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
        std::cerr << "Invalid port number! Try again.\n";
    }
}

void MidiUtils::sendMidiMessage(RtMidiOut& midiOut, const std::vector<unsigned char>& message) {
    try {
        midiOut.sendMessage(&message);
    } catch (...) {
        std::cerr << "Failed to send MIDI message\n";
    }
}