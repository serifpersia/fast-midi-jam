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
#include <iomanip> // For std::setw, std::setfill
#include <ctime>   // For std::time_t, std::ctime
#include <mutex>   // For std::mutex
#include <algorithm>
#include <csignal> // For signal handling

using boost::asio::ip::udp;
namespace json = boost::json;

// Simple logging utility
class Logger {
    //std::ofstream log_file_; // Remove file logging
    std::mutex log_mutex_;
    bool debug_mode_ = false;

public:
    Logger(bool debug_mode = false) : debug_mode_(debug_mode) {
        //log_file_.open(filename, std::ios::app); // Remove file logging
        //if (!log_file_.is_open()) { // Remove file logging
        //    std::cerr << "Failed to open log file: " << filename << "\n"; // Remove file logging
        //} // Remove file logging
    }

    void log(const std::string& message) {
        std::lock_guard<std::mutex> lock(log_mutex_);
        auto now = std::chrono::system_clock::now();
        auto time = std::chrono::system_clock::to_time_t(now);
        //log_file_ << std::ctime(&time) << ": " << message << "\n"; // Remove file logging
        std::cout << std::ctime(&time) << ": " << message << "\n";
        //log_file_.flush(); // Remove file logging
    }

    void log_verbose(const std::string& message) {
        if (debug_mode_) {
            log(message);
        }
    }

    void set_debug_mode(bool debug) {
        debug_mode_ = debug;
    }
};

static Logger logger; // Global logger instance

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
    static constexpr size_t BUFFER_SIZE = 128;
    static constexpr auto HEARTBEAT_TIMEOUT = std::chrono::seconds(20);  // Timeout for connection
    static constexpr auto MIDI_ACTIVITY_TIMEOUT = std::chrono::seconds(2);  // Timeout for MIDI activity
    static constexpr auto HEARTBEAT_INTERVAL = std::chrono::seconds(5);

    boost::asio::io_context io_context_;
    udp::socket socket_;
    std::unordered_map<std::string, Client> clients_;
    std::array<char, BUFFER_SIZE> buffer_;
    boost::asio::steady_timer cleanup_timer_;
    boost::asio::steady_timer ping_timer_;
    bool is_running_ = true; // Flag to control server loop

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
        logger.log("Server started on UDP port " + std::to_string(port));
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

    void stop() {
        is_running_ = false;
        cleanup_timer_.cancel();
        ping_timer_.cancel();
        socket_.close();
        io_context_.stop();
        logger.log("Server stopped.");
    }

private:
    // Helper function to log raw data
    void log_data(const std::string& direction, const udp::endpoint& endpoint, const char* buffer, std::size_t bytes) {
        std::ostringstream log_msg;
        log_msg << direction << " " << bytes << " bytes to/from "
                << endpoint.address().to_string() << ":" << endpoint.port() << " - Raw: ";

        // Hex dump
        for (std::size_t i = 0; i < bytes; ++i) {
            log_msg << std::hex << std::setw(2) << std::setfill('0')
                    << (static_cast<unsigned int>(buffer[i]) & 0xFF) << " ";
        }

        // Printable characters (if any)
        log_msg << " (";
        for (std::size_t i = 0; i < bytes; ++i) {
            char c = buffer[i];
            log_msg << (std::isprint(c) ? c : '.');
        }
        log_msg << ")";

        logger.log_verbose(log_msg.str());
    }

    void start_receive() noexcept {
        auto sender = std::make_shared<udp::endpoint>();
        socket_.async_receive_from(
            boost::asio::buffer(buffer_), *sender,
            [this, sender](const boost::system::error_code& ec, std::size_t bytes) {
                if (!ec && bytes > 0) {
                    // Log the incoming data
                    log_data("Received", *sender, buffer_.data(), bytes);

                    handle_packet(*sender, bytes);
                }
                if (is_running_) start_receive(); // Conditionally restart receive
            });
    }

	void handle_packet(const udp::endpoint& sender, std::size_t bytes) noexcept {
		std::string sender_key = sender.address().to_string() + ":" + std::to_string(sender.port());

		if (bytes == 4 && std::strncmp(buffer_.data(), "QUIT", 4) == 0) {
			if (auto it = clients_.find(sender_key); it != clients_.end()) {
				logger.log("Client disconnected: " + it->second.nickname + " @ " + sender_key);
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
			logger.log("New client connected: " + client.nickname + " @ " + sender_key);

			// Send ACK to the client
			socket_.async_send_to( // Fixed: Replace `udp_socket_` with `socket_`
				boost::asio::buffer("ACK", 3), client.endpoint,
				[](const boost::system::error_code& ec, std::size_t) {
					if (ec) logger.log_verbose("ACK send error: " + ec.message());
				});

			client.last_ping_sent = std::chrono::steady_clock::now(); // Set initial ping time
			socket_.async_send_to(boost::asio::buffer("PING", 4), client.endpoint,
				[](const boost::system::error_code& ec, std::size_t) {
					if (ec) logger.log_verbose("Ping send error: " + ec.message());
				});
		} else if (bytes == 4 && std::strncmp(buffer_.data(), "PONG", 4) == 0) {
			if (client.last_ping_sent != std::chrono::steady_clock::time_point()) {
				auto now = std::chrono::steady_clock::now();
				client.latency_ms = std::chrono::duration_cast<std::chrono::milliseconds>(now - client.last_ping_sent).count();
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

        // Log the outgoing client list
        log_data("Sending", sender, json_str.data(), json_str.size());

        socket_.async_send_to(
            boost::asio::buffer(json_str), sender,
            [](const boost::system::error_code& ec, std::size_t) {
                if (ec) logger.log_verbose("Client list send error: " + ec.message());
            });
    }

    void forward_midi(const std::string& sender_key, std::size_t bytes) noexcept {
        auto buffer = std::make_shared<std::vector<char>>(buffer_.begin(), buffer_.begin() + bytes);
        for (const auto& [id, client] : clients_) {
            if (id != sender_key) {
                // Log the outgoing data
                log_data("Sending", client.endpoint, buffer->data(), buffer->size());

                socket_.async_send_to(
                    boost::asio::buffer(*buffer), client.endpoint,
                    [](const boost::system::error_code& ec, std::size_t) {
                        if (ec) logger.log_verbose("Send error: " + ec.message());
                    });
            }
        }
    }

    void start_cleanup() noexcept {
        cleanup_timer_.expires_after(HEARTBEAT_TIMEOUT);
        cleanup_timer_.async_wait([this](const boost::system::error_code& ec) {
            if (!ec && is_running_) {  // Check is_running_ before executing
                auto now = std::chrono::steady_clock::now();
                for (auto it = clients_.begin(); it != clients_.end();) {
                    if (now - it->second.last_heartbeat > HEARTBEAT_TIMEOUT) {
                        logger.log("Client timed out: " + it->second.nickname + " @ " + it->first);
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
            if (!ec && is_running_) { // Check is_running_ before executing
                for (auto& [id, client] : clients_) {
                    client.last_ping_sent = std::chrono::steady_clock::now(); // Record ping time

                    // Log the outgoing PING
                    log_data("Sending", client.endpoint, "PING", 4);

                    socket_.async_send_to(boost::asio::buffer("PING", 4), client.endpoint,
                        [](const boost::system::error_code& ec, std::size_t) {
                            if (ec) logger.log_verbose("Ping send error: " + ec.message());
                        });
                }
                start_ping();
            }
        });
    }

    friend int main(int argc, char* argv[]); // Allow main to access is_running_
};

MidiJamServer* global_server = nullptr; // Global pointer to the server instance

void signal_handler(int signal) {
    if (signal == SIGINT) {
        logger.log("SIGINT received! Shutting down server...");
        if (global_server) {
            global_server->stop(); // Stop the server gracefully
        }
        std::exit(0);
    }
}

int main(int argc, char* argv[]) {
    try {
        short port = 5000; // Default port
        bool debug_mode = false;

        // Check for debug argument
        if (argc > 1) {
            for (int i = 1; i < argc; ++i) {
                if (std::string(argv[i]) == "-debug") {
                    debug_mode = true;
                    break;
                }
            }
        }

        logger.set_debug_mode(debug_mode);


        // Prompt the user for the port number
        logger.log("Enter the UDP port number for the server (default: 5000): ");
        std::string input;
        std::getline(std::cin, input);

        if (!input.empty()) {
            try {
                port = static_cast<short>(std::stoi(input));
                if (port <= 0 || port > 65535) {
                    logger.log("Invalid port number. Using default port 5000.");
                    port = 5000; // Fallback to default
                }
            } catch (const std::exception&) {
                logger.log("Invalid input. Using default port 5000.");
                port = 5000; // Fallback to default
            }
        }

        MidiJamServer server(port);
        global_server = &server; // Assign the server instance to the global pointer

        // Register the signal handler
        std::signal(SIGINT, signal_handler);

        server.run();

    } catch (const std::exception& e) {
        logger.log("Fatal error: " + std::string(e.what()));
        return 1;
    }
    return 0;
}