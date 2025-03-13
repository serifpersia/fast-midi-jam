#include <boost/asio.hpp>
#include <boost/json.hpp>
#include <iostream>
#include <vector>
#include <unordered_map>
#include <string>
#include <memory>
#include <array>
#include <thread>
#include <chrono>

using boost::asio::ip::udp;
namespace json = boost::json;

struct Client {
    udp::endpoint endpoint;
    uint8_t channel;
    std::string nickname;
    std::chrono::steady_clock::time_point last_heartbeat;  // For connection status
    std::chrono::steady_clock::time_point last_midi_activity;  // For MIDI activity
    std::chrono::steady_clock::time_point last_ping_sent; // New: Timestamp of last ping
    int64_t latency_ms = -1; // New: Latency in milliseconds (-1 if unknown)
	
    Client() noexcept 
        : endpoint(), channel(0), nickname("unknown"), 
          last_heartbeat(std::chrono::steady_clock::now()), 
          last_midi_activity(std::chrono::steady_clock::time_point()),
          last_ping_sent(std::chrono::steady_clock::time_point()) {}
    Client(udp::endpoint ep, uint8_t ch, std::string name) noexcept
        : endpoint(std::move(ep)), channel(ch), nickname(std::move(name)),
          last_heartbeat(std::chrono::steady_clock::now()),
          last_midi_activity(std::chrono::steady_clock::time_point()),
          last_ping_sent(std::chrono::steady_clock::time_point()) {}
};

class MidiJamServer {
    static constexpr size_t BUFFER_SIZE = 64;
    static constexpr auto HEARTBEAT_TIMEOUT = std::chrono::seconds(20);  // Timeout for connection
    static constexpr auto MIDI_ACTIVITY_TIMEOUT = std::chrono::seconds(2);  // Timeout for MIDI activity
    static constexpr auto HEARTBEAT_INTERVAL = std::chrono::seconds(5);

    boost::asio::io_context io_context_;
    udp::socket socket_;
    std::unordered_map<std::string, Client> clients_;
    std::array<char, BUFFER_SIZE> buffer_;
    boost::asio::steady_timer cleanup_timer_;
    boost::asio::steady_timer ping_timer_;

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
					// Log raw data as a hex string and printable characters
					std::ostringstream log_msg;
					log_msg << "Received " << bytes << " bytes from " 
							<< sender->address().to_string() << ":" << sender->port() << " - Raw: ";
					
					// Hex dump
					for (std::size_t i = 0; i < bytes; ++i) {
						log_msg << std::hex << std::setw(2) << std::setfill('0') 
								<< (static_cast<unsigned int>(buffer_[i]) & 0xFF) << " ";
					}
					
					// Printable characters (if any)
					log_msg << " (";
					for (std::size_t i = 0; i < bytes; ++i) {
						char c = buffer_[i];
						log_msg << (std::isprint(c) ? c : '.');
					}
					log_msg << ")";
					
					//std::cout << log_msg.str() << "\n";
					
					handle_packet(*sender, bytes);
				}
				start_receive();
			});
	}

	void handle_packet(const udp::endpoint& sender, std::size_t bytes) noexcept {
		std::string sender_key = sender.address().to_string() + ":" + std::to_string(sender.port());

		if (bytes == 4 && std::strncmp(buffer_.data(), "QUIT", 4) == 0) {
			if (auto it = clients_.find(sender_key); it != clients_.end()) {
				std::cout << "Client disconnected: " << it->second.nickname << " @ " << sender_key << "\n";
				clients_.erase(it);
			}
			return;
		}

		if (bytes == 5 && std::strncmp(buffer_.data(), "CLIST", 5) == 0) {
			send_client_list(sender);
			return;
		}

		auto [it, inserted] = clients_.try_emplace(sender_key, sender, 0, std::string(buffer_.data(), bytes));
		Client& client = it->second;
		client.last_heartbeat = std::chrono::steady_clock::now();

		if (inserted) {
			std::cout << "New client connected: " << client.nickname << " @ " << sender_key << "\n";
			client.last_ping_sent = std::chrono::steady_clock::now(); // Set initial ping time
			socket_.async_send_to(boost::asio::buffer("PING", 4), client.endpoint,
				[](const boost::system::error_code& ec, std::size_t) {
					if (ec) std::cerr << "Ping send error: " << ec.message() << "\n";
				});
		} else if (bytes == 4 && std::strncmp(buffer_.data(), "PONG", 4) == 0) {
			if (client.last_ping_sent != std::chrono::steady_clock::time_point()) {
				auto now = std::chrono::steady_clock::now();
				client.latency_ms = std::chrono::duration_cast<std::chrono::milliseconds>(now - client.last_ping_sent).count();
				//std::cout << "Latency for " << client.nickname << ": " << client.latency_ms << "ms\n";
			}
		} else if (bytes > 0 && (static_cast<uint8_t>(buffer_[0]) & 0xF0) >= 0x80) {
			client.channel = static_cast<uint8_t>(buffer_[0]) & 0x0F;
			client.last_midi_activity = std::chrono::steady_clock::now();
			forward_midi(sender_key, bytes);
		}
	}

	void send_client_list(const udp::endpoint& sender) noexcept {
		json::object client_list;
		json::array clients_array;

		for (const auto& [id, client] : clients_) {
			json::object client_info;
			client_info["nickname"] = client.nickname;
			client_info["channel"] = client.channel;
			bool is_active = (client.last_midi_activity != std::chrono::steady_clock::time_point() &&
							  std::chrono::steady_clock::now() - client.last_midi_activity < MIDI_ACTIVITY_TIMEOUT);
			client_info["active"] = is_active;
			client_info["latency_ms"] = client.latency_ms; // New: Include latency
			clients_array.push_back(client_info);
		}

		client_list["clients"] = clients_array;
		std::string json_str = json::serialize(client_list);

		socket_.async_send_to(
			boost::asio::buffer(json_str), sender,
			[](const boost::system::error_code& ec, std::size_t) {
				if (ec) std::cerr << "Client list send error: " << ec.message() << "\n";
			});
	}

    void forward_midi(const std::string& sender_key, std::size_t bytes) noexcept {
        auto buffer = std::make_shared<std::vector<char>>(buffer_.begin(), buffer_.begin() + bytes);
        for (const auto& [id, client] : clients_) {
            if (id != sender_key) {
                socket_.async_send_to(
                    boost::asio::buffer(*buffer), client.endpoint,
                    [](const boost::system::error_code& ec, std::size_t) {
                        if (ec) std::cerr << "Send error: " << ec.message() << "\n";
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
                start_cleanup();
            }
        });
    }

	void start_ping() noexcept {
		ping_timer_.expires_after(HEARTBEAT_INTERVAL);
		ping_timer_.async_wait([this](const boost::system::error_code& ec) {
			if (!ec) {
				for (auto& [id, client] : clients_) {
					client.last_ping_sent = std::chrono::steady_clock::now(); // Record ping time
					socket_.async_send_to(boost::asio::buffer("PING", 4), client.endpoint,
						[](const boost::system::error_code& ec, std::size_t) {
							if (ec) std::cerr << "Ping send error: " << ec.message() << "\n";
						});
				}
				start_ping();
			}
		});
	}
};

int main() {
    try {
        MidiJamServer server(5000);
        server.run();
    } catch (const std::exception& e) {
        std::cerr << "Fatal error: " << e.what() << "\n";
        return 1;
    }
    return 0;
}