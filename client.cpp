#include <boost/asio.hpp>
#include <iostream>
#include <vector>
#include <string>
#include <sstream>
#include <thread>
#include <chrono>
#include <csignal>
#include "RtMidi.h"
#include "midi_utils.h"

using boost::asio::ip::udp;

class MidiJamClient {
    udp::socket socket_;
    udp::endpoint server_endpoint_;
    std::string nickname_;
    RtMidiIn midi_in_;
    RtMidiIn midi_in_2_;  // Second MIDI input
    RtMidiOut midi_out_;
    std::vector<unsigned char> midi_send_buffer_;
    std::vector<unsigned char> midi_recv_buffer_;
    volatile bool running_ = true;
    uint8_t midi_channel_;
    bool has_second_input_ = false;

    static void midi_callback(double deltaTime, std::vector<unsigned char>* message, void* userData) {
        if (!message || message->empty() || !userData) return;
        auto* client = static_cast<MidiJamClient*>(userData);

        uint8_t status_type = message->at(0) & 0xF0; // Extract message type

        // Only allow Note Off (0x80), Note On (0x90), and Control Change (0xB0)
        if (!(status_type == 0x80 || status_type == 0x90 || status_type == 0xB0)) {
            return; // Ignore all other messages
        }

        std::vector<unsigned char> adjusted_message = *message;
        adjusted_message[0] = status_type | (client->midi_channel_ & 0x0F); // Set MIDI channel

        client->socket_.send_to(boost::asio::buffer(adjusted_message), client->server_endpoint_);
        client->route_midi_to_output(adjusted_message);
    }

    static void signal_handler(int signal) {
        if (signal == SIGINT && signal_user_data_) {
            auto* client = static_cast<MidiJamClient*>(signal_user_data_);
            client->running_ = false;
            client->socket_.send_to(boost::asio::buffer("QUIT"), client->server_endpoint_);
            std::cout << "Shutting down...\n";
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            exit(0);
        }
    }

public:
    static void* signal_user_data_;

    MidiJamClient(boost::asio::io_context& io_context, 
                  const std::string& server_ip, 
                  short server_port, 
                  const std::string& nickname, 
                  int midi_in_port, 
                  int midi_out_port,
                  int midi_in_port_2,  // Added second MIDI in port parameter
                  uint8_t midi_channel)
        : socket_(io_context, udp::endpoint(udp::v4(), 0)),
          server_endpoint_(boost::asio::ip::make_address(server_ip), server_port),
          nickname_(nickname),
          midi_channel_(midi_channel) {
        midi_recv_buffer_.resize(1024);
        signal_user_data_ = this;
        std::signal(SIGINT, &signal_handler);
        send_nickname();
        setup_midi(midi_in_port, midi_out_port, midi_in_port_2);
    }

    void send_nickname() {
        socket_.send_to(boost::asio::buffer(nickname_), server_endpoint_);
        std::cout << "Connected as " << nickname_ << " to " << server_endpoint_ 
                  << " on MIDI channel " << (int)(midi_channel_ + 1) << "\n";
    }

    void setup_midi(int midi_in_port, int midi_out_port, int midi_in_port_2) {
        try {
            // Setup first MIDI input
            midi_in_.openPort(midi_in_port);
            midi_in_.ignoreTypes(false, false, false);
            midi_in_.setCallback(&midi_callback, this);
            std::cout << "MIDI input device 1 opened on port " << midi_in_port << "\n";

            // Setup second MIDI input if specified
            if (midi_in_port_2 >= 0) {
                midi_in_2_.openPort(midi_in_port_2);
                midi_in_2_.ignoreTypes(false, false, false);
                midi_in_2_.setCallback(&midi_callback, this);
                has_second_input_ = true;
                std::cout << "MIDI input device 2 opened on port " << midi_in_port_2 << "\n";
            }

            // Setup MIDI output
            midi_out_.openPort(midi_out_port);
            std::cout << "MIDI output device opened on port " << midi_out_port << "\n";
        } catch (RtMidiError& error) {
            std::cerr << "Error opening MIDI port: " << error.what() << "\n";
            exit(1);
        }
    }

    void route_midi_to_output(const std::vector<unsigned char>& message) {
        try {
            midi_out_.sendMessage(&message);
        } catch (RtMidiError& error) {
            std::cerr << "Error sending MIDI to output: " << error.what() << "\n";
        }
    }

    void process_incoming_midi() {
        while (running_) {
            boost::system::error_code ec;
            size_t len = socket_.receive_from(boost::asio::buffer(midi_recv_buffer_), server_endpoint_, 0, ec);

            if (ec && ec != boost::asio::error::operation_aborted) {
                std::cerr << "Error receiving from server: " << ec.message() << "\n";
            } else if (len > 0) {
                std::vector<unsigned char> message(midi_recv_buffer_.begin(), midi_recv_buffer_.begin() + len);
                route_midi_to_output(message);
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    }
};

void* MidiJamClient::signal_user_data_ = nullptr;

int main() {
    try {
        boost::asio::io_context io_context;

        std::string server_address, nickname;
        std::cout << "Enter nickname: ";
        std::getline(std::cin, nickname);
        std::cout << "Enter server IP:port (e.g., 127.0.0.1:5000): ";
        std::getline(std::cin, server_address);

        int channel_input;
        do {
            std::cout << "Enter MIDI channel (1-16): ";
            std::cin >> channel_input;
            if (channel_input < 1 || channel_input > 16) {
                std::cout << "Please enter a valid channel between 1 and 16\n";
            }
        } while (channel_input < 1 || channel_input > 16);
        uint8_t midi_channel = static_cast<uint8_t>(channel_input - 1);
        std::cin.ignore();

        std::string server_ip;
        int server_port;
        std::stringstream ss(server_address);
        std::getline(ss, server_ip, ':');
        ss >> server_port;

        RtMidiIn midi_in;
        RtMidiOut midi_out;
        MidiUtils::listDevices(midi_in, midi_out);
        
        int midi_in_port = MidiUtils::selectInputDevice(midi_in);
        std::cout << "Select MIDI output port (0 to " << (midi_out.getPortCount() - 1) << "): ";
        int midi_out_port;
        std::cin >> midi_out_port;
        
        // Ask for second MIDI input
        int midi_in_port_2 = -1;
        std::cout << "Would you like to add a second MIDI input device? (y/n): ";
        char response;
        std::cin >> response;
        std::cin.ignore();
        
        if (tolower(response) == 'y') {
            std::cout << "Available MIDI input devices:\n";
            for (unsigned int i = 0; i < midi_in.getPortCount(); i++) {
                if (static_cast<int>(i) != midi_in_port) {  // Don't show already selected port
                    std::cout << "  " << i << ": " << midi_in.getPortName(i) << "\n";
                }
            }
            std::cout << "Select second MIDI input port (0 to " << (midi_in.getPortCount() - 1) << "): ";
            std::cin >> midi_in_port_2;
            std::cin.ignore();
            // Validate second input isn't same as first
            while (midi_in_port_2 == midi_in_port) {
                std::cout << "Please select a different port than the first input: ";
                std::cin >> midi_in_port_2;
                std::cin.ignore();
            }
        }

        MidiJamClient client(io_context, server_ip, server_port, nickname, 
                           midi_in_port, midi_out_port, midi_in_port_2, midi_channel);
        std::thread receive_thread(&MidiJamClient::process_incoming_midi, &client);
        receive_thread.join();
    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << "\n";
    }
    return 0;
}