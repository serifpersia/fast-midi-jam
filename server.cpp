#include <boost/asio.hpp>
#include <iostream>
#include <vector>
#include <unordered_map>
#include <string>
#include <memory>
#include <array>
#include <thread>

using boost::asio::ip::udp;

struct Client {
    udp::endpoint endpoint;
    uint8_t channel;
    std::string nickname;

    Client() noexcept : endpoint(), channel(0), nickname("unknown") {}
    Client(udp::endpoint ep, uint8_t ch, std::string name) noexcept
        : endpoint(std::move(ep)), channel(ch), nickname(std::move(name)) {}
};

class MidiJamServer {
    static constexpr size_t BUFFER_SIZE = 64;
    
    boost::asio::io_context io_context_;
    udp::socket socket_;
    std::unordered_map<std::string, Client> clients_;
    std::array<char, BUFFER_SIZE> buffer_;

public:
    MidiJamServer(short port = 5000)
        : socket_(io_context_, udp::endpoint(udp::v4(), port)) {
        socket_.set_option(boost::asio::socket_base::reuse_address(true));
        socket_.set_option(boost::asio::socket_base::receive_buffer_size(65536));
        socket_.set_option(boost::asio::socket_base::send_buffer_size(65536));
        start_receive();
        std::cout << "Server started on UDP port " << port << "\n";
    }

    void run() {
        std::vector<std::thread> threads;
        for (size_t i = 0; i < std::thread::hardware_concurrency(); ++i) {
            threads.emplace_back([this]() { io_context_.run(); });
        }
        for (auto& t : threads) {
            if (t.joinable()) t.join();
        }
    }

private:
    void start_receive() noexcept {
        auto sender = std::make_shared<udp::endpoint>();
        socket_.async_receive_from(
            boost::asio::buffer(buffer_), *sender,
            [this, sender](const boost::system::error_code& ec, std::size_t bytes) {
                if (!ec && bytes > 0) {
                    handle_packet(*sender, bytes);
                } else if (ec && ec != boost::asio::error::operation_aborted) {
                    if (ec.value() != 10054) {
                        std::cerr << "Receive error: " << ec.message() << " (code: " << ec.value() << ")\n";
                    }
                }
                start_receive();
            });
    }

    void handle_packet(const udp::endpoint& sender, std::size_t bytes) noexcept {
        std::string sender_key = sender.address().to_string() + ":" + std::to_string(sender.port());
        
        auto [it, inserted] = clients_.try_emplace(sender_key, sender, 0, std::string(buffer_.data(), bytes));
        Client& client = it->second;
        
        if (inserted) {
            std::cout << "New client: " << client.nickname << " @ " << sender_key << "\n";
        } else {
            if (bytes == 4 && std::strncmp(buffer_.data(), "QUIT", 4) == 0) {
                std::cout << "Client " << client.nickname << " disconnected\n";
                clients_.erase(it);
            } else if (bytes > 0 && (static_cast<uint8_t>(buffer_[0]) & 0xF0) >= 0x80) {
                client.channel = static_cast<uint8_t>(buffer_[0]) & 0x0F;
                forward_midi(sender_key, bytes);
            }
        }
    }

    void forward_midi(const std::string& sender_key, std::size_t bytes) noexcept {
        auto buffer = std::make_shared<std::vector<char>>(buffer_.begin(), buffer_.begin() + bytes);
        for (auto it = clients_.begin(); it != clients_.end();) {
            const auto& [id, client] = *it;
            if (id != sender_key) {
                socket_.async_send_to(
                    boost::asio::buffer(*buffer), client.endpoint,
                    [this, id, buffer](const boost::system::error_code& ec, std::size_t) {
                        if (ec) {
                            if (ec == boost::asio::error::connection_refused ||
                                ec == boost::asio::error::host_unreachable ||
                                ec.value() == 10054) {
                                std::cout << "Client " << clients_[id].nickname << " @ " << id << " unreachable, removing\n";
                                clients_.erase(id);
                            } else {
                                std::cerr << "Send error to " << id << ": " << ec.message() << "\n";
                            }
                        }
                    });
                ++it;
            } else {
                ++it;
            }
        }
    }
};

int main() {
    try {
        MidiJamServer server;
        server.run();
    } catch (const std::exception& e) {
        std::cerr << "Fatal error: " << e.what() << "\n";
        return 1;
    }
    return 0;
}
