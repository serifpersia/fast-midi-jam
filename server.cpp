#include <boost/asio.hpp>
#include <iostream>
#include <vector>
#include <unordered_map>
#include <string>
#include <memory>
#include <array>
#include <thread>
#include <chrono>

using boost::asio::ip::udp;

// Define Client struct outside or before its use
struct Client {
    udp::endpoint endpoint;
    uint8_t channel;
    std::string nickname;
    std::chrono::steady_clock::time_point last_heartbeat;  // Last activity time

    Client() noexcept 
        : endpoint(), channel(0), nickname("unknown"), 
          last_heartbeat(std::chrono::steady_clock::now()) {}
    Client(udp::endpoint ep, uint8_t ch, std::string name) noexcept
        : endpoint(std::move(ep)), channel(ch), nickname(std::move(name)),
          last_heartbeat(std::chrono::steady_clock::now()) {}
};

class MidiJamServer {
    static constexpr size_t BUFFER_SIZE = 64;
    static constexpr auto HEARTBEAT_TIMEOUT = std::chrono::seconds(10);  // Timeout for inactive clients
    static constexpr auto HEARTBEAT_INTERVAL = std::chrono::seconds(5);  // Server ping interval

    boost::asio::io_context io_context_;
    udp::socket socket_;
    std::unordered_map<std::string, Client> clients_;  // Now Client is known
    std::array<char, BUFFER_SIZE> buffer_;
    boost::asio::steady_timer cleanup_timer_;  // Timer for checking client heartbeats
    boost::asio::steady_timer ping_timer_;     // Timer for sending periodic pings

public:
    MidiJamServer(short port = 5000)
        : socket_(io_context_, udp::endpoint(udp::v4(), port)),
          cleanup_timer_(io_context_),
          ping_timer_(io_context_) {
        socket_.set_option(boost::asio::socket_base::reuse_address(true));
        socket_.set_option(boost::asio::socket_base::receive_buffer_size(65536));
        socket_.set_option(boost::asio::socket_base::send_buffer_size(65536));
        start_receive();
        start_cleanup();
        start_ping();
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
					//std::cout << "[Server] Received " << bytes << " bytes from " << sender->address().to_string() << ":" << sender->port() << " - Data: ";
					for (std::size_t i = 0; i < bytes; ++i) {
						std::cout << std::hex << static_cast<int>(buffer_[i]) << " ";
					}
					std::cout << std::dec << std::endl;

					handle_packet(*sender, bytes);
				}
				start_receive();  // Continue listening
			});
	}


    void handle_packet(const udp::endpoint& sender, std::size_t bytes) noexcept {
        std::string sender_key = sender.address().to_string() + ":" + std::to_string(sender.port());

        // Handle explicit disconnect
        if (bytes == 4 && std::strncmp(buffer_.data(), "QUIT", 4) == 0) {
            if (auto it = clients_.find(sender_key); it != clients_.end()) {
                std::cout << "Client disconnected: " << it->second.nickname << " @ " << sender_key << "\n";
                clients_.erase(it);
            }
            return;
        }

        // Handle new connection or update existing client
        auto [it, inserted] = clients_.try_emplace(sender_key, sender, 0, std::string(buffer_.data(), bytes));
        Client& client = it->second;
        client.last_heartbeat = std::chrono::steady_clock::now();  // Update heartbeat timestamp

        if (inserted) {
            std::cout << "New client connected: " << client.nickname << " @ " << sender_key << "\n";
            socket_.async_send_to(boost::asio::buffer("PING", 4), client.endpoint,
                [](const boost::system::error_code& ec, std::size_t) {
                    if (ec) std::cerr << "Ping send error: " << ec.message() << "\n";
                });
        } else if (bytes == 4 && std::strncmp(buffer_.data(), "PONG", 4) == 0) {
            // Heartbeat response received
            std::cout << "Heartbeat received from " << client.nickname << " @ " << sender_key << "\n";
        } else if (bytes > 0 && (static_cast<uint8_t>(buffer_[0]) & 0xF0) >= 0x80) {
            // MIDI message
            client.channel = static_cast<uint8_t>(buffer_[0]) & 0x0F;
            forward_midi(sender_key, bytes);
        }
    }

    void forward_midi(const std::string& sender_key, std::size_t bytes) noexcept {
        auto buffer = std::make_shared<std::vector<char>>(buffer_.begin(), buffer_.begin() + bytes);
        for (const auto& [id, client] : clients_) {
            if (id != sender_key) {
                socket_.async_send_to(
                    boost::asio::buffer(*buffer), client.endpoint,
                    [](const boost::system::error_code& ec, std::size_t) {
                        if (ec) {
                            std::cerr << "Send error: " << ec.message() << "\n";
                        }
                    });
            }
        }
    }

    void start_cleanup() noexcept {
        cleanup_timer_.expires_after(HEARTBEAT_TIMEOUT);
        cleanup_timer_.async_wait([this](const boost::system::error_code& ec) {
            if (!ec) {
                auto now = std::chrono::steady_clock::now();
                for (auto it = clients_.begin(); it != clients_.end();) {
                    if (now - it->second.last_heartbeat > HEARTBEAT_TIMEOUT) {
                        std::cout << "Client timed out: " << it->second.nickname << " @ " << it->first << "\n";
                        it = clients_.erase(it);
                    } else {
                        ++it;
                    }
                }
                start_cleanup();  // Schedule next cleanup
            }
        });
    }

    void start_ping() noexcept {
        ping_timer_.expires_after(HEARTBEAT_INTERVAL);
        ping_timer_.async_wait([this](const boost::system::error_code& ec) {
            if (!ec) {
                for (const auto& [id, client] : clients_) {
                    socket_.async_send_to(boost::asio::buffer("PING", 4), client.endpoint,
                        [](const boost::system::error_code& ec, std::size_t) {
                            if (ec) std::cerr << "Ping send error: " << ec.message() << "\n";
                        });
                }
                start_ping();  // Schedule next ping
            }
        });
    }
};

int main() {
    try {
        MidiJamServer server(5000);  // Start server on port 5000
        server.run();
    } catch (const std::exception& e) {
        std::cerr << "Fatal error: " << e.what() << "\n";
        return 1;
    }
    return 0;
}
