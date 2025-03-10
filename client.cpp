#include <boost/asio.hpp>
#include <iostream>
#include <string>
#include <sstream>
#include <csignal>
#include <thread>
#include <fstream>
#include "RtMidi.h"
#include "midi_utils.h"
#include <windows.h> // Include for Windows API functions

using boost::asio::ip::udp;

class MidiJamClient {
    udp::socket socket_;
    udp::endpoint server_endpoint_;
    std::string nickname_;
    RtMidiIn midi_in_;
    RtMidiIn second_midi_in_;
    RtMidiOut midi_out_;
    std::vector<unsigned char> midi_buffer_;
    std::vector<unsigned char> midi_recv_buffer_;
    volatile bool running_ = true;
    uint8_t midi_channel_;
    bool second_midi_port_open_ = false;

    static void midi_callback(double deltaTime, std::vector<unsigned char>* message, void* userData) {
        if (!message || message->empty() || !userData) return;
        auto* client = static_cast<MidiJamClient*>(userData);
        uint8_t status = message->at(0);
        uint8_t status_type = status & 0xF0;
        if ((status_type != 0x80) && (status_type != 0x90) && (status_type != 0xA0) && (status_type != 0xB0)) return;

        client->midi_buffer_.clear();
        client->midi_buffer_.insert(client->midi_buffer_.begin(), message->begin(), message->end());
        client->midi_buffer_[0] = (client->midi_buffer_[0] & 0xF0) | (client->midi_channel_ & 0x0F);
        client->socket_.send_to(boost::asio::buffer(client->midi_buffer_), client->server_endpoint_);
        client->route_midi_to_output(client->midi_buffer_);
    }
    // New console control handler function
    static BOOL WINAPI console_handler(DWORD ctrl_type) {
        if (ctrl_type == CTRL_CLOSE_EVENT || ctrl_type == CTRL_SHUTDOWN_EVENT) {
            if (signal_user_data_) {
                auto* client = static_cast<MidiJamClient*>(signal_user_data_);
                client->running_ = false;
                client->send_message("QUIT");
                std::cout << "Sending QUIT message on close...\n";
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
            return TRUE; // Indicate that we handled the event
        }
        return FALSE; // Let other handlers process the event
    }


    static void signal_handler(int signal) {
        if (signal == SIGINT && signal_user_data_) {
            auto* client = static_cast<MidiJamClient*>(signal_user_data_);
            client->running_ = false;
            client->send_message("QUIT");
            std::cout << "Shutting down...\n";
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            exit(0);
        }
    }

    void start_receive() {
        socket_.async_receive_from(
            boost::asio::buffer(midi_recv_buffer_), server_endpoint_,
            [this](boost::system::error_code ec, std::size_t len) {
                if (!ec && len > 0) {
                    std::vector<unsigned char> message(midi_recv_buffer_.begin(), midi_recv_buffer_.begin() + len);
                    route_midi_to_output(message);
                }
                if (running_) start_receive();
            });
    }

    void send_message(const std::string& message) {
        socket_.send_to(boost::asio::buffer(message), server_endpoint_);
    }

public:
    static void* signal_user_data_;

    MidiJamClient(boost::asio::io_context& io_context,
        const std::string& server_ip,
        short server_port,
        const std::string& nickname,
        int midi_in_port,
        int midi_out_port,
        uint8_t midi_channel,
        bool second_midi_port_open,
        int second_midi_in_port)
        : socket_(io_context, udp::endpoint(udp::v4(), 0)),
        server_endpoint_(boost::asio::ip::make_address(server_ip), server_port),
        nickname_(nickname),
        midi_channel_(midi_channel),
        second_midi_port_open_(second_midi_port_open) {
        midi_recv_buffer_.resize(1024);
        midi_buffer_.reserve(16);
        signal_user_data_ = this;
        std::signal(SIGINT, &signal_handler);
        send_nickname();
        setup_midi(midi_in_port, midi_out_port);

        if (second_midi_port_open_) {
            setup_second_midi_port(second_midi_in_port);
        }

        start_receive();

         // Set the console control handler
        if (!SetConsoleCtrlHandler(console_handler, TRUE)) {
            std::cerr << "Error setting console control handler: " << GetLastError() << std::endl;
        }
    }

    void send_nickname() {
        socket_.send_to(boost::asio::buffer(nickname_), server_endpoint_);
        std::cout << "Connected as " << nickname_ << " to " << server_endpoint_
            << " on MIDI channel " << (int)(midi_channel_ + 1) << "\n";
    }

    void setup_midi(int midi_in_port, int midi_out_port) {
        try {
            midi_in_.openPort(midi_in_port);
            midi_in_.ignoreTypes(false, false, false);
            midi_in_.setCallback(&midi_callback, this);
            std::cout << "MIDI input device opened on port " << midi_in_port << "\n";
            midi_out_.openPort(midi_out_port);
            std::cout << "MIDI output device opened on port " << midi_out_port << "\n";
        } catch (RtMidiError& error) {
            std::cerr << "Error opening MIDI port: " << error.what() << "\n";
            exit(1);
        }
    }

    void setup_second_midi_port(int second_midi_in_port) {
        try {
            second_midi_in_.openPort(second_midi_in_port);
            second_midi_in_.ignoreTypes(false, false, false);
            second_midi_in_.setCallback(&midi_callback, this);
            std::cout << "Second MIDI input device opened on port " << second_midi_in_port << "\n";
        } catch (RtMidiError& error) {
            std::cerr << "Error opening second MIDI input port: " << error.what() << "\n";
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
};

void* MidiJamClient::signal_user_data_ = nullptr;

struct ClientConfig {
    std::string server_ip = "127.0.0.1";
    short server_port = 5000;
    std::string nickname = "DefaultNickname";
    int midi_in_port = 0;
    int midi_out_port = 0;
    uint8_t midi_channel = 0;
    bool second_midi_port_open = false;
    int second_midi_in_port = 1;
};

ClientConfig load_client_config(const std::string& filename) {
    ClientConfig config;
    std::ifstream file(filename);
    if (file.is_open()) {
        std::string line;
        while (std::getline(file, line)) {
            std::istringstream iss(line);
            std::string key;
            if (std::getline(iss, key, '=')) {
                std::string value;
                if (std::getline(iss, value)) {
                    try {
                        if (key == "server_ip") {
                            config.server_ip = value;
                        } else if (key == "server_port") {
                            config.server_port = std::stoi(value);
                        } else if (key == "nickname") {
                            config.nickname = value;
                        } else if (key == "midi_in_port") {
                            config.midi_in_port = std::stoi(value);
                        } else if (key == "midi_out_port") {
                            config.midi_out_port = std::stoi(value);
                        } else if (key == "midi_channel") {
                            config.midi_channel = static_cast<uint8_t>(std::stoi(value) - 1); // Client expects 1-16, server expects 0-15
                        } else if (key == "second_midi_port_open") {
                            config.second_midi_port_open = (value == "true" || value == "1");
                        } else if (key == "second_midi_in_port") {
                            config.second_midi_in_port = std::stoi(value);
                        }
                    } catch (const std::invalid_argument& e) {
                        std::cerr << "Invalid config value for " << key << ": " << value << "\n";
                    }
                }
            }
        }
        file.close();
    } else {
        std::cerr << "Failed to open config file: " << filename << ". Using default settings.\n";
    }
    return config;
}

void save_client_config(const std::string& filename, const ClientConfig& config) {
    std::ofstream file(filename);
    if (file.is_open()) {
        file << "server_ip=" << config.server_ip << "\n";
        file << "server_port=" << config.server_port << "\n";
        file << "nickname=" << config.nickname << "\n";
        file << "midi_in_port=" << config.midi_in_port << "\n";
        file << "midi_out_port=" << config.midi_out_port << "\n";
        file << "midi_channel=" << static_cast<int>(config.midi_channel + 1) << "\n";
        file << "second_midi_port_open=" << (config.second_midi_port_open ? "true" : "false") << "\n";
        file << "second_midi_in_port=" << config.second_midi_in_port << "\n";
        file.close();
    } else {
        std::cerr << "Error saving client config file: " << filename << "\n";
    }
}

int main() {
    try {
        boost::asio::io_context io_context;
        ClientConfig config = load_client_config("client_config.cfg");
        bool config_loaded = true;

        if (config.nickname == "DefaultNickname") {
            config_loaded = false;
        }

        RtMidiIn midi_in;
        RtMidiOut midi_out;
        MidiUtils::listDevices(midi_in, midi_out);

        if (!config_loaded) {
            std::cout << "Enter nickname: ";
            std::getline(std::cin, config.nickname);

            std::cout << "Enter server IP:port (e.g., 127.0.0.1:5000): ";
            std::string server_address;
            std::getline(std::cin, server_address);

            std::stringstream ss(server_address);
            std::getline(ss, config.server_ip, ':');
            ss >> config.server_port;

            int channel_input;
            do {
                std::cout << "Enter MIDI channel (1-16): ";
                std::cin >> channel_input;
                if (channel_input < 1 || channel_input > 16) {
                    std::cout << "Please enter a valid channel between 1 and 16\n";
                }
            } while (channel_input < 1 || channel_input > 16);
            config.midi_channel = static_cast<uint8_t>(channel_input - 1);
            std::cin.ignore();

            config.midi_in_port = MidiUtils::selectInputDevice(midi_in);
            std::cout << "Select MIDI output port (0 to " << (midi_out.getPortCount() - 1) << "): ";
            std::cin >> config.midi_out_port;
            std::cin.ignore();

            char open_second_port;
            std::cout << "Do you want to open a second MIDI input port? (y/n): ";
            std::cin >> open_second_port;
            std::cin.ignore();

            config.second_midi_port_open = (open_second_port == 'y' || open_second_port == 'Y');

            if (config.second_midi_port_open) {
                int midi_in_count = midi_in.getPortCount();
                std::cout << "Select second MIDI input port (0 to " << (midi_in_count - 1) << "): ";
                std::cin >> config.second_midi_in_port;
                std::cin.ignore();
            }

            save_client_config("client_config.cfg", config);
        }

        MidiJamClient client(io_context, config.server_ip, config.server_port, config.nickname,
            config.midi_in_port, config.midi_out_port, config.midi_channel,
            config.second_midi_port_open, config.second_midi_in_port);
        io_context.run();
    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << "\n";
    }
    return 0;
}