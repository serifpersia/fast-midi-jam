#include <boost/asio.hpp>
#include <iostream>
#include <vector>
#include <unordered_map>
#include <string>
#include <chrono>
#include "midi_utils.h"

using boost::asio::ip::udp;

struct Client {
    udp::endpoint endpoint;
    uint8_t channel;
    std::string nickname;
    std::chrono::steady_clock::time_point last_active;

    Client() : endpoint(), channel(0), nickname("unknown"), last_active(std::chrono::steady_clock::now()) {}
    Client(udp::endpoint ep, uint8_t ch, std::string name) : endpoint(std::move(ep)), channel(ch), nickname(std::move(name)), last_active(std::chrono::steady_clock::now()) {}
};

class MidiJamServer {
    udp::socket socket_;
    std::unordered_map<std::string, Client> clients_;
    std::array<char, 1024> buffer_;
    static constexpr std::chrono::seconds INACTIVITY_TIMEOUT{30};

public:
    MidiJamServer(boost::asio::io_context& io_context, short port = 5000)
        : socket_(io_context, udp::endpoint(udp::v4(), port)) {
        std::cout << "Server started on UDP port " << port << "\n";
        start_receive();
        start_cleanup();
    }

    void start_receive() {
        auto sender = std::make_shared<udp::endpoint>();
        socket_.async_receive_from(
            boost::asio::buffer(buffer_), *sender,
            [this, sender](boost::system::error_code ec, std::size_t bytes) {
                if (!ec) {
                    std::string sender_key = sender->address().to_string() + ":" + std::to_string(sender->port());

                    if (clients_.find(sender_key) == clients_.end()) {
                        std::string nickname(buffer_.begin(), buffer_.begin() + bytes);
                        clients_[sender_key] = { *sender, 0, nickname };
                        std::cout << "New client: " << nickname << " @ " << sender_key << "\n";
                    } else {
                        Client& client = clients_[sender_key];
                        client.last_active = std::chrono::steady_clock::now();

                        std::string msg(buffer_.begin(), buffer_.begin() + bytes);
                        if (msg == "QUIT") {
                            std::cout << "Client " << client.nickname << " @ " << sender_key << " disconnected\n";
                            clients_.erase(sender_key);
                        } else {
                            if (bytes > 0) {
                                uint8_t status_byte = static_cast<uint8_t>(buffer_[0]);
                                if (status_byte >= 0x80 && status_byte <= 0xEF) {
                                    client.channel = status_byte & 0x0F;
                                }
                            }
                            forward_midi(sender_key, bytes);
                        }
                    }
                } else if (ec != boost::asio::error::operation_aborted && ec != boost::asio::error::connection_reset) {
                    std::cerr << "Error receiving data: " << ec.message() << " (code: " << ec.value() << ")\n";
                }
                start_receive();
            });
    }

    void forward_midi(const std::string& sender_key, std::size_t bytes) {
        for (auto it = clients_.begin(); it != clients_.end(); ) {
            const auto& [id, c] = *it;
            if (id != sender_key) {
                socket_.async_send_to(
                    boost::asio::buffer(buffer_, bytes), c.endpoint,
                    [this, id, c](boost::system::error_code ec, std::size_t bytes_sent) {
                        if (ec) {
                            std::cerr << "Failed to send to " << c.nickname << " @ " << c.endpoint << ": " << ec.message() << "\n";
                            clients_.erase(id);
                        }
                    });
                ++it;
            } else {
                ++it;
            }
        }
    }

    void start_cleanup() {
        boost::asio::steady_timer timer(socket_.get_executor(), std::chrono::seconds(10));
        timer.async_wait([this](const boost::system::error_code& ec) {
            if (!ec) {
                cleanup_inactive_clients();
                start_cleanup();
            }
        });
    }

    void cleanup_inactive_clients() {
        auto now = std::chrono::steady_clock::now();
        for (auto it = clients_.begin(); it != clients_.end();) {
            if (now - it->second.last_active > INACTIVITY_TIMEOUT) {
                std::cout << "Removing inactive client: " << it->second.nickname << " @ " << it->first << "\n";
                it = clients_.erase(it);
            } else {
                ++it;
            }
        }
    }
};

int main() {
    try {
        boost::asio::io_context io_context;
        MidiJamServer server(io_context);
        io_context.run();
    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << "\n";
        return 1;
    }
    return 0;
}