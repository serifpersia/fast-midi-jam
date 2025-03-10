#ifndef MIDI_UTILS_H
#define MIDI_UTILS_H

#include "rtmidi/RtMidi.h"
#include <string>
#include <vector>

class MidiUtils {
public:
    static void listDevices(RtMidiIn& midiIn, RtMidiOut& midiOut);
    static unsigned int selectInputDevice(RtMidiIn& midiIn);
    static void sendMidiMessage(RtMidiOut& midiOut, const std::vector<unsigned char>& message);
};

#endif
