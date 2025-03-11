#include <boost/asio.hpp>
#include <iostream>
#include <vector>
#include <string>
#include <thread>
#include <chrono>
#include <csignal>
#include "RtMidi.h"
#include "midi_utils.h"

using boost::asio::ip::udp;

class MidiJamClient {
    static constexpr size_t BUFFER_SIZE = 64;
    static constexpr std::chrono::milliseconds POLL_INTERVAL{1};

    udp::socket socket_;
    udp::endpoint server_endpoint_;
    std::string nickname_;
    RtMidiIn midi_in_;
    RtMidiIn midi_in_2_;
    RtMidiOut midi_out_;
    std::array<unsigned char, BUFFER_SIZE> recv_buffer_;
    volatile bool running_ = true;
    uint8_t midi_channel_;
    bool has_second_input_ = false;

    static void midi_callback(double, std::vector<unsigned char>* msg, void* userData) noexcept {
        if (!msg || msg->empty() || !userData) return;
        auto* client = static_cast<MidiJamClient*>(userData);
        
        uint8_t status = msg->at(0) & 0xF0;
        if (status != 0x80 && status != 0x90 && status != 0xB0) return;

        std::vector<unsigned char> adjusted = *msg;
        adjusted[0] = status | (client->midi_channel_ & 0x0F);
        
        client->socket_.async_send_to(
            boost::asio::buffer(adjusted), client->server_endpoint_,
            [](const boost::system::error_code& ec, std::size_t) {
                if (ec) std::cerr << "MIDI send error: " << ec.message() << "\n";
            });
        MidiUtils::sendMidiMessage(client->midi_out_, adjusted);
    }

    static void signal_handler(int signal) noexcept {
        if (signal == SIGINT && signal_user_data_) {
            auto* client = static_cast<MidiJamClient*>(signal_user_data_);
            client->running_ = false;
            client->socket_.send_to(boost::asio::buffer("QUIT"), client->server_endpoint_);
            std::cout << "Shutting down...\n";
        }
    }

public:
    static void* signal_user_data_;

    MidiJamClient(boost::asio::io_context& io_context, const std::string& server_ip, short server_port,
                  const std::string& nickname, int midi_in_port, int midi_out_port,
                  int midi_in_port_2, uint8_t midi_channel)
        : socket_(io_context, udp::endpoint(udp::v4(), 0)),
          server_endpoint_(boost::asio::ip::make_address(server_ip), server_port),
          nickname_(nickname), midi_channel_(midi_channel) {
        socket_.set_option(boost::asio::socket_base::send_buffer_size(65536));
        socket_.set_option(boost::asio::socket_base::receive_buffer_size(65536));
        signal_user_data_ = this;
        std::signal(SIGINT, &signal_handler);
        send_nickname();
        setup_midi(midi_in_port, midi_out_port, midi_in_port_2);
        start_receive();
    }

    ~MidiJamClient() {
        if (running_) {
            running_ = false;
            socket_.send_to(boost::asio::buffer("QUIT"), server_endpoint_);
        }
    }

private:
    void send_nickname() noexcept {
        socket_.send_to(boost::asio::buffer(nickname_), server_endpoint_);
        std::cout << "Connected as " << nickname_ << " to " << server_endpoint_
                  << " on MIDI channel " << (int)(midi_channel_ + 1) << "\n";
    }

    void setup_midi(int in_port, int out_port, int in_port_2) {
        try {
            midi_in_.openPort(in_port);
            midi_in_.ignoreTypes(true, true, true);
            midi_in_.setCallback(&midi_callback, this);

            if (in_port_2 >= 0 && in_port_2 != in_port) {
                midi_in_2_.openPort(in_port_2);
                midi_in_2_.ignoreTypes(true, true, true);
                midi_in_2_.setCallback(&midi_callback, this);
                has_second_input_ = true;
            }

            midi_out_.openPort(out_port);
        } catch (const RtMidiError& e) {
            throw std::runtime_error(std::string("MIDI setup error: ") + e.what());
        }
    }

    void start_receive() noexcept {
        auto sender = std::make_shared<udp::endpoint>();
        socket_.async_receive_from(
            boost::asio::buffer(recv_buffer_), *sender,
            [this, sender](const boost::system::error_code& ec, std::size_t bytes) {
                if (!ec && bytes > 0) {
                    std::vector<unsigned char> msg(recv_buffer_.begin(), recv_buffer_.begin() + bytes);
                    MidiUtils::sendMidiMessage(midi_out_, msg);
                }
                if (running_) start_receive();
            });
    }
};

void* MidiJamClient::signal_user_data_ = nullptr;

int main() {
    try {
        boost::asio::io_context io_context(1);
        
        // Nickname validation
        std::string nickname;
        while (true) {
            std::cout << "Enter nickname (cannot be empty): ";
            std::getline(std::cin, nickname);
            if (!nickname.empty()) break;
            std::cout << "Nickname cannot be empty. Try again.\n";
        }

        // Server IP:Port validation
        std::string server_address, server_ip;
        short server_port;
        while (true) {
            std::cout << "Enter server IP:port (e.g., 127.0.0.1:5000): ";
            std::getline(std::cin, server_address);
            std::stringstream ss(server_address);
            if (std::getline(ss, server_ip, ':') && !server_ip.empty() && ss >> server_port) {
                if (server_port > 0 && server_port <= 65535) break;
                std::cout << "Port must be between 1 and 65535. Try again.\n";
            } else {
                std::cout << "Invalid format or empty IP. Use IP:port (e.g., 127.0.0.1:5000). Try again.\n";
            }
            ss.clear();
        }

        // MIDI channel validation
        int channel_input;
        while (true) {
            std::cout << "Enter MIDI channel (1-16): ";
            std::string input;
            std::getline(std::cin, input);
            std::stringstream ss(input);
            if (ss >> channel_input && ss.eof() && channel_input >= 1 && channel_input <= 16) break;
            std::cout << "Invalid input. Enter a number between 1 and 16. Try again.\n";
        }
        uint8_t midi_channel = static_cast<uint8_t>(channel_input - 1);

        // MIDI device setup
        RtMidiIn midi_in;
        RtMidiOut midi_out;
        MidiUtils::listDevices(midi_in, midi_out);

        // First MIDI input port validation
        int midi_in_port;
        while (true) {
            std::cout << "Select MIDI input port (0 to " << (midi_in.getPortCount() - 1) << "): ";
            std::string input;
            std::getline(std::cin, input);
            std::stringstream ss(input);
            if (ss >> midi_in_port && ss.eof() && midi_in_port >= 0 && midi_in_port < static_cast<int>(midi_in.getPortCount())) break;
            std::cout << "Invalid input. Enter a number between 0 and " << (midi_in.getPortCount() - 1) << ". Try again.\n";
        }

        // MIDI output port validation
        int midi_out_port;
        while (true) {
            std::cout << "Select MIDI output port (0 to " << (midi_out.getPortCount() - 1) << "): ";
            std::string input;
            std::getline(std::cin, input);
            std::stringstream ss(input);
            if (ss >> midi_out_port && ss.eof() && midi_out_port >= 0 && midi_out_port < static_cast<int>(midi_out.getPortCount())) break;
            std::cout << "Invalid input. Enter a number between 0 and " << (midi_out.getPortCount() - 1) << ". Try again.\n";
        }

        // Second MIDI input option validation
        int midi_in_port_2 = -1;
        while (true) {
            std::cout << "Add second MIDI input? (y/n): ";
            std::string input;
            std::getline(std::cin, input);
            if (input.length() == 1 && (tolower(input[0]) == 'y' || tolower(input[0]) == 'n')) {
                if (tolower(input[0]) == 'y') {
                    while (true) {
                        std::cout << "Select second MIDI input port (0 to " << (midi_in.getPortCount() - 1) << ", not " << midi_in_port << "): ";
                        std::getline(std::cin, input);
                        std::stringstream ss(input);
                        if (ss >> midi_in_port_2 && ss.eof() && 
                            midi_in_port_2 >= 0 && midi_in_port_2 < static_cast<int>(midi_in.getPortCount()) && 
                            midi_in_port_2 != midi_in_port) break;
                        std::cout << "Invalid input or duplicate port. Enter a number between 0 and " << (midi_in.getPortCount() - 1) << ", different from " << midi_in_port << ". Try again.\n";
                    }
                }
                break;
            }
            std::cout << "Invalid input. Enter 'y' or 'n'. Try again.\n";
        }

        MidiJamClient client(io_context, server_ip, server_port, nickname, 
                            midi_in_port, midi_out_port, midi_in_port_2, midi_channel);
        io_context.run();
    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << "\n";
        return 1;
    }
    return 0;
}