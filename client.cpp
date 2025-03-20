#include <boost/asio.hpp>
#include <boost/beast.hpp>
#include <boost/json.hpp>
#include "RtMidi.h"
#include "midi_utils.h"
#include <iostream>
#include <fstream>
#include <thread>
#include <csignal>
#include <memory>
#include <filesystem>
#include <mutex>
#include <chrono>
#include <algorithm>

using boost::asio::ip::udp;
namespace beast = boost::beast;
namespace http = beast::http;
using tcp = boost::asio::ip::tcp;
namespace json = boost::json;

// Simple logging utility
class Logger {
    std::mutex log_mutex_;
    bool debug_mode_ = false;

public:
    Logger() = default;

    void set_debug_mode(bool debug) {
        debug_mode_ = debug;
    }

    void log(const std::string& message) {
        std::lock_guard<std::mutex> lock(log_mutex_);
        std::cout << message << "\n";
    }

    void log_simple(const std::string& message) { // Log only if debug mode is disabled
        if (!debug_mode_) {
            std::lock_guard<std::mutex> lock(log_mutex_);
            std::cout << message << "\n";
        }
    }
    bool is_debug_mode() const { return debug_mode_; }
};

static Logger logger;


class MidiJamClient {
private:
    static constexpr size_t BUFFER_SIZE = 128;
    static constexpr size_t JSON_BUFFER_SIZE = 1024;
    static constexpr auto CLIENT_LIST_INTERVAL = std::chrono::seconds(5);
    static constexpr auto CLIENT_LOG_INTERVAL = std::chrono::seconds(5);

    boost::asio::io_context io_context_;
    boost::asio::executor_work_guard<boost::asio::io_context::executor_type> work_guard_; // Keep io_context alive
    udp::socket udp_socket_;
    udp::endpoint server_endpoint_;
    std::string nickname_;
    RtMidiIn midi_in_;
    RtMidiIn midi_in_2_;
    RtMidiOut midi_out_;
    std::array<unsigned char, BUFFER_SIZE> midi_buffer_;
    std::array<char, JSON_BUFFER_SIZE> json_buffer_;
    volatile bool running_ = true;
    uint8_t midi_channel_;
    bool has_second_input_ = false;

    boost::asio::steady_timer client_list_timer_;
    boost::asio::steady_timer log_timer_;  // Separate timer for logging
    std::vector<std::thread> thread_pool_;
    bool connected_ = false;
    json::value last_client_list_;
    mutable std::mutex client_list_mutex_;
    int midi_in_port_;
    int midi_out_port_;
    int midi_in_port_2_;

    static void midi_callback(double, std::vector<unsigned char>* msg, void* userData) noexcept {
        if (!msg || msg->empty() || !userData) return;
        auto* client = static_cast<MidiJamClient*>(userData);

        // Filter MIDI messages: only allow Note On/Off, Aftertouch, and CC
        uint8_t status = msg->at(0) & 0xF0;
        if (status != 0x80 && // Note Off
            status != 0x90 && // Note On
            status != 0xA0 && // Aftertouch
            status != 0xB0 && // CC
            status != 0xD0) // Channel Pressure (Aftertouch)
            return;

        std::vector<unsigned char> adjusted = *msg;
        if(status != 0xD0)
        {
            adjusted[0] = (msg->at(0) & 0xF0) | (client->midi_channel_ & 0x0F);
        } else
        {
            adjusted[0] = 0xD0 | (client->midi_channel_ & 0x0F);
        }

        std::ostringstream log_msg;
        log_msg << "Sending MIDI: ";
        for (auto byte : adjusted) {
            log_msg << std::hex << std::setw(2) << std::setfill('0') << (int)byte << " ";
        }
        if (logger.is_debug_mode()) {
            logger.log(log_msg.str());
        }


        client->udp_socket_.async_send_to(
            boost::asio::buffer(adjusted), client->server_endpoint_,
            [](const boost::system::error_code& ec, std::size_t) {
                if (ec) {
                    logger.log("MIDI send error: " + ec.message());
                }
                else {
                    if (logger.is_debug_mode()) {
                        logger.log("MIDI sent successfully");
                    }

                }

            });
        MidiUtils::sendMidiMessage(client->midi_out_, adjusted);
    }

public:
    MidiJamClient(const std::string& server_ip, short server_port, const std::string& nickname,
                  int midi_in_port, int midi_out_port, int midi_in_port_2, uint8_t midi_channel)
        : io_context_(),
          work_guard_(boost::asio::make_work_guard(io_context_)), // Initialize work guard
          udp_socket_(io_context_, udp::endpoint(udp::v4(), 0)),
          server_endpoint_(boost::asio::ip::make_address(server_ip), server_port),
          nickname_(nickname), midi_channel_(midi_channel), client_list_timer_(io_context_), log_timer_(io_context_),
          midi_in_port_(midi_in_port), midi_out_port_(midi_out_port), midi_in_port_2_(midi_in_port_2) {
        try {
            connect();
        } catch (const std::exception& e) {
            logger.log("Failed to initialize MidiJamClient: " + std::string(e.what()));
            throw;
        }
    }

    ~MidiJamClient() {
        disconnect();
        work_guard_.reset(); // Release work guard on destruction
    }

	bool connect_with_handshake(int max_retries = 5, std::chrono::seconds timeout = std::chrono::seconds(1)) {
		int retry_count = 0;
		bool acknowledged = false;

		udp_socket_.set_option(boost::asio::socket_base::send_buffer_size(65536));
		udp_socket_.set_option(boost::asio::socket_base::receive_buffer_size(65536));

		while (retry_count < max_retries && !acknowledged) {
			try {
				if (logger.is_debug_mode()) {
					logger.log("Sending nickname: " + nickname_);
				}
				udp_socket_.send_to(boost::asio::buffer(nickname_), server_endpoint_);

				// Set up a timer for the timeout
				boost::asio::steady_timer timer(io_context_);
				timer.expires_after(timeout);

				// Asynchronously wait for the timer
				boost::system::error_code timer_ec;
				timer.async_wait([&timer_ec](const boost::system::error_code& ec) {
					if (!ec) {
						timer_ec = boost::asio::error::timed_out; // Mark as timed out
					}
				});

				// Asynchronously wait for the server's response
				boost::system::error_code receive_ec;
				size_t bytes = 0;
				udp::endpoint sender_endpoint;
				auto receive_handler = [&](const boost::system::error_code& ec, std::size_t length) {
					if (!ec) {
						bytes = length;
						receive_ec = ec;
					} else {
						receive_ec = ec;
					}
					timer.cancel(); // Cancel the timer since we received a response
				};

				udp_socket_.async_receive_from(
					boost::asio::buffer(json_buffer_), sender_endpoint, receive_handler);

				// Run the io_context to process the asynchronous operations
				io_context_.run_for(timeout);

				// Check the results
				if (timer_ec == boost::asio::error::timed_out) {
					logger.log("Handshake failed: Server did not respond within the timeout period.");
				} else if (receive_ec) {
					logger.log("Handshake failed: " + receive_ec.message());
				} else if (bytes == 3 && std::memcmp(json_buffer_.data(), "ACK", 3) == 0) {
					if (logger.is_debug_mode()) {
						logger.log("Received ACK from server");
					}
					acknowledged = true;
				} else {
					logger.log("Handshake failed: Invalid response");
				}

				if (!acknowledged) {
					retry_count++;
					std::this_thread::sleep_for(std::chrono::milliseconds(500));
				}
			} catch (const std::exception& e) {
				logger.log("Handshake exception: " + std::string(e.what()));
				retry_count++;
				std::this_thread::sleep_for(std::chrono::milliseconds(500));
			}
		}

		if (!acknowledged) {
			logger.log("Failed to connect after " + std::to_string(max_retries) + " retries");
		}

		return acknowledged;
	}

    void connect() {
        if (connected_) return;

        if (!connect_with_handshake()) {
            logger.log("Failed to connect to the server. Retrying will be possible via the HTTP API.");
            throw std::runtime_error("Failed to establish connection with the server");
        } else {
            setup_midi(midi_in_port_, midi_out_port_, midi_in_port_2_);
            start_receive();
            start_client_list_requests();
            start_log_state(); // Start logging
            connected_ = true;
            thread_pool_.emplace_back([this]() {
                if (logger.is_debug_mode()) {
                    logger.log("Starting io_context thread");
                }

                io_context_.run();
                if (logger.is_debug_mode()) {
                    logger.log("io_context thread stopped");
                }

            });
            logger.log_simple("Successfully connected to server: " + server_endpoint_.address().to_string() + ":" + std::to_string(server_endpoint_.port()));
            logger.log_simple("Client started successfully");
        }
    }

    void start_log_state() noexcept {
        log_timer_.expires_after(CLIENT_LOG_INTERVAL);
        log_timer_.async_wait([this](const boost::system::error_code& ec) {
            if (!ec) {
                if (logger.is_debug_mode()) {
                    logger.log("Client state: running=" + std::to_string(running_) + ", connected=" + std::to_string(connected_));
                }

                if (running_) {
                    start_log_state(); // Reschedule
                }
            } else if (ec == boost::asio::error::operation_aborted) {
                if (logger.is_debug_mode()) {
                    logger.log("Log timer cancelled (expected)."); // Expected on shutdown
                }

            } else {
                logger.log("Log timer error: " + ec.message());
            }
        });
    }



	void disconnect() {
		if (!connected_) return;
		running_ = false;

		// Cancel timers
		boost::system::error_code ec;
		size_t cancelled_clist = client_list_timer_.cancel();
		if (cancelled_clist > 0) {
            if (logger.is_debug_mode()) {
                logger.log("Successfully cancelled CLIST timer.");
            }

		} else {
            if (logger.is_debug_mode()) {
                logger.log("CLIST timer was already expired or cancelled.");
            }

		}
		size_t cancelled_log = log_timer_.cancel();
		if (cancelled_log > 0) {
            if (logger.is_debug_mode()) {
                logger.log("Successfully cancelled log timer.");
            }

		} else {
            if (logger.is_debug_mode()) {
                logger.log("Log timer was already expired or cancelled.");
            }
		}
		udp_socket_.send_to(boost::asio::buffer("QUIT", 4), server_endpoint_, 0, ec);
		if (ec) logger.log("Error sending QUIT: " + ec.message());

		std::this_thread::sleep_for(std::chrono::milliseconds(100));


		io_context_.stop(); // Stop the io_context AFTER cancelling the timer and sending QUIT
		for (auto& t : thread_pool_) {
			if (t.joinable()) {
				t.join();
			}
		}
		thread_pool_.clear();
		connected_ = false;
        logger.log_simple("Disconnected from server");
        logger.log_simple("Client stopped successfully");
	}

    bool is_connected() const { return connected_; }

    json::value get_client_list() const {
        std::lock_guard<std::mutex> lock(client_list_mutex_);
        if (!last_client_list_.is_object()) {
            // Return an empty object if the client list is invalid or not set
            return json::object();
        }
        return last_client_list_;
    }

    json::object get_config() const {
        json::object config;
        config["server_ip"] = server_endpoint_.address().to_string();
        config["server_port"] = server_endpoint_.port();
        config["nickname"] = nickname_;
        config["midi_in"] = midi_in_port_;
        config["midi_out"] = midi_out_port_;
        config["midi_in_2"] = midi_in_port_2_;
        config["channel"] = static_cast<int64_t>(midi_channel_);
        return config;
    }

private:
    void send_nickname() noexcept {
        udp_socket_.send_to(boost::asio::buffer(nickname_), server_endpoint_);
        if (logger.is_debug_mode()) {
            logger.log("Connected as " + nickname_ + " to " + server_endpoint_.address().to_string() +
                       ":" + std::to_string(server_endpoint_.port()) + " on MIDI channel " + std::to_string((int)(midi_channel_ + 1)));
        }

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
            logger.log("MIDI ports opened: in=" + std::to_string(in_port) + ", out=" + std::to_string(out_port) +
                       ", in2=" + std::to_string(in_port_2));
        } catch (const RtMidiError& e) {
            throw std::runtime_error(std::string("MIDI setup error: ") + e.what());
        }
    }

    void start_receive() noexcept {
        auto sender = std::make_shared<udp::endpoint>();
        json_buffer_.fill(0);
        if (logger.is_debug_mode()) {
            logger.log("Starting async receive from " + server_endpoint_.address().to_string() + ":" + std::to_string(server_endpoint_.port()));
        }

        udp_socket_.async_receive_from(
            boost::asio::buffer(json_buffer_), *sender,
            [this, sender](const boost::system::error_code& ec, std::size_t bytes) {
                if (ec) {
                    logger.log("Receive error: " + ec.message() + " (code: " + std::to_string(ec.value()) + ")");
                } else if (bytes > 0) {
                    std::ostringstream log_msg;
                    log_msg << "Received " << bytes << " bytes from "
                            << sender->address().to_string() << ":" << sender->port() << " - Raw: ";
                    for (std::size_t i = 0; i < bytes; ++i) {
                        log_msg << std::hex << std::setw(2) << std::setfill('0')
                                << (static_cast<unsigned int>(json_buffer_[i]) & 0xFF) << " ";
                    }
                    if (logger.is_debug_mode()) {
                        logger.log(log_msg.str());
                    }


                    if (bytes == 4 && std::memcmp(json_buffer_.data(), "PING", 4) == 0) {
                        if (logger.is_debug_mode()) {
                            logger.log("Received PING, sending PONG");
                        }

                        boost::system::error_code send_ec;
                        udp_socket_.send_to(boost::asio::buffer("PONG", 4), server_endpoint_, 0, send_ec);
                        if (send_ec) {
                            logger.log("PONG send error: " + send_ec.message());
                        } else {
                            if (logger.is_debug_mode()) {
                                logger.log("PONG sent successfully");
                            }

                        }
                    } else if (bytes >= 1 && (json_buffer_[0] & 0x80)) {
                        if (logger.is_debug_mode()) {
                            logger.log("Received MIDI data");
                        }

                        std::vector<unsigned char> msg(json_buffer_.begin(), json_buffer_.begin() + bytes);
                        MidiUtils::sendMidiMessage(midi_out_, msg);
                    } else {
                        std::string json_str(json_buffer_.data(), bytes);
                        if (logger.is_debug_mode()) {
                            logger.log("Received potential JSON: " + json_str);
                        }

                        try {
                            auto parsed_json = json::parse(json_str);
                            if (parsed_json.is_object()) {
                                std::lock_guard<std::mutex> lock(client_list_mutex_);
                                last_client_list_ = parsed_json;
                                if (logger.is_debug_mode()) {
                                    logger.log("Updated client list");
                                }

                            } else {
                                logger.log("Received invalid JSON (not an object): " + json_str);
                            }
                        } catch (const std::exception& e) {
                            logger.log("JSON parse error: " + std::string(e.what()) + " Raw data: " + json_str);
                        }
                    }
                } else {
                    logger.log("Received 0 bytes");
                }
                if (running_) {
                    if (logger.is_debug_mode()) {
                        logger.log("Scheduling next receive");
                    }

                    start_receive(); // Reschedule even if there’s an error, as long as running_
                } else {
                    if (logger.is_debug_mode()) {
                        logger.log("Stopping receive loop (running_ = false)");
                    }

                }
            });
    }

    void start_client_list_requests() noexcept {
        client_list_timer_.expires_after(CLIENT_LIST_INTERVAL);
        client_list_timer_.async_wait([this](const boost::system::error_code& ec) {
            if (ec) {
                if (ec == boost::asio::error::operation_aborted) {
                    if (logger.is_debug_mode()) {
                        logger.log("CLIST timer cancelled (expected)."); // Expected on shutdown
                    }

                } else {
                    logger.log("CLIST timer error: " + ec.message());
                }
                return;
            }

            if (!running_ || !connected_) {
                if (logger.is_debug_mode()) {
                    logger.log("CLIST request skipped: running=" + std::to_string(running_) + ", connected=" + std::to_string(connected_));
                }

                return;
            }

            if (logger.is_debug_mode()) {
                logger.log("Sending CLIST request to server");
            }

            boost::system::error_code send_ec;
            udp_socket_.send_to(boost::asio::buffer("CLIST", 5), server_endpoint_, 0, send_ec);
            if (send_ec) {
                logger.log("CLIST send error: " + send_ec.message());
            } else {
                if (logger.is_debug_mode()) {
                    logger.log("CLIST sent successfully, rescheduling");
                }

            }

            if (running_ && connected_) {
                start_client_list_requests(); // Reschedule if still running
            } else {
                if (logger.is_debug_mode()) {
                    logger.log("CLIST request skipped (stopped)");
                }

            }
        });
    }
};


class HttpServer {
    boost::asio::io_context& io_context_;
    tcp::acceptor acceptor_;
    std::shared_ptr<MidiJamClient> client_;
    std::filesystem::path static_dir_;
    json::object cached_midi_ports_; // Added: Cache for MIDI ports
    std::chrono::steady_clock::time_point last_midi_update_; // Added: Last update timestamp
    mutable std::mutex client_mutex_; // Protect access to `client_`
    static constexpr auto MIDI_UPDATE_INTERVAL = std::chrono::seconds(30); // Added: Update interval

public:
    HttpServer(boost::asio::io_context& ioc, short port = 8080, const std::string& static_dir = "static")
        : io_context_(ioc), acceptor_(ioc, tcp::endpoint(tcp::v4(), port)), static_dir_(static_dir) {
        update_midi_ports(); // Initialize MIDI port cache
        start_accept();
        logger.log("HTTP server running at http://localhost:" + std::to_string(port));
    }

private:
    void start_accept() {
        auto socket = std::make_shared<tcp::socket>(acceptor_.get_executor());
        acceptor_.async_accept(*socket, [this, socket](boost::system::error_code ec) {
            if (!ec) {
                handle_request(socket);
            }
            boost::asio::post(io_context_, [this]() {
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
                start_accept();
            });
        });
    }

    void handle_request(std::shared_ptr<tcp::socket> socket) {
        auto buffer = std::make_shared<beast::flat_buffer>();
        auto request = std::make_shared<http::request<http::string_body>>();
        http::async_read(*socket, *buffer, *request,
            [this, socket, buffer, request](boost::system::error_code ec, std::size_t) {
                if (!ec) {
                    process_request(socket, *request);
                }
            });
    }

    std::string readFile(const std::filesystem::path& filepath) {
        std::ifstream file(filepath, std::ios::in);
        if (!file.is_open()) {
            logger.log("Failed to open file: " + filepath.string());
            return "Error: Could not load HTML file.";
        }
        std::string content((std::istreambuf_iterator<char>(file)),
                            std::istreambuf_iterator<char>());
        file.close();
        return content;
    }

    void update_midi_ports() {
        RtMidiIn midi_in;
        RtMidiOut midi_out;
        json::object ports;
        ports["inputs"] = json::array();
        ports["outputs"] = json::array();
        unsigned int in_count = midi_in.getPortCount();
        unsigned int out_count = midi_out.getPortCount();
        logger.log("MIDI ports detected: inputs=" + std::to_string(in_count) + ", outputs=" + std::to_string(out_count));
        for (unsigned int i = 0; i < in_count; i++) {
            ports["inputs"].as_array().emplace_back(midi_in.getPortName(i));
        }
        for (unsigned int i = 0; i < out_count; i++) {
            ports["outputs"].as_array().emplace_back(midi_out.getPortName(i));
        }
        cached_midi_ports_ = ports;
        last_midi_update_ = std::chrono::steady_clock::now();
    }


    void process_request(std::shared_ptr<tcp::socket> socket, http::request<http::string_body>& request) {
        http::response<http::string_body> response;
        response.version(request.version());
        response.set(http::field::server, "MidiJam Client");
        response.keep_alive(false);

        try {
            // Handle GET requests
            if (request.method() == http::verb::get && request.target() == "/") {
                // Serve the main HTML page
                response.result(http::status::ok);
                response.set(http::field::content_type, "text/html");
                response.body() = readFile(static_dir_ / "index.html");

            } else if (request.method() == http::verb::get && request.target() == "/midi-ports") {
                // Return the list of available MIDI ports as JSON
                response.result(http::status::ok);
                response.set(http::field::content_type, "application/json");
                if (std::chrono::steady_clock::now() - last_midi_update_ > MIDI_UPDATE_INTERVAL) {
                    update_midi_ports(); // Refresh MIDI port cache if outdated
                }
                response.body() = json::serialize(cached_midi_ports_);

            } else if (request.method() == http::verb::get && request.target() == "/status") {
                // Return the connection status of the client
                response.result(http::status::ok);
                response.set(http::field::content_type, "application/json");
                json::object status;
                {
                    std::lock_guard<std::mutex> lock(client_mutex_);
                    status["isConnected"] = client_ && client_->is_connected();
                }
                response.body() = json::serialize(status);

            } else if (request.method() == http::verb::get && request.target() == "/config") {
                response.result(http::status::ok);
                response.set(http::field::content_type, "application/json");
                json::object config;
                {
                    std::lock_guard<std::mutex> lock(client_mutex_);
                    if (client_ && client_->is_connected()) {
                        config = client_->get_config();
                    } else {
                        // Provide defaults when no client is connected
                        config["server_ip"] = "127.0.0.1";
                        config["server_port"] = 5000;
                        config["nickname"] = "";
                        config["midi_in"] = 0;  // Default to first device
                        config["midi_out"] = 0; // Default to first device
                        config["midi_in_2"] = -1; // No second input by default
                        config["channel"] = 0;  // Default to channel 1 (0-based)
                    }
                }
                response.body() = json::serialize(config);
            } else if (request.method() == http::verb::get && request.target() == "/clients") {
                // Return the list of connected clients from the server
                response.result(http::status::ok);
                response.set(http::field::content_type, "application/json");
                json::object client_list;
                {
                    std::lock_guard<std::mutex> lock(client_mutex_);
                    if (client_) {
                        auto client_list_value = client_->get_client_list();
                        if (client_list_value.is_object()) {
                            client_list = client_list_value.as_object();
                        } else {
                            client_list["clients"] = json::array();
                        }
                    } else {
                        client_list["clients"] = json::array(); // Return an empty array if no client is connected
                    }
                }
                response.body() = json::serialize(client_list);
            }
            // Handle POST requests
            else if (request.method() == http::verb::post && request.target() == "/stop") {
                // Stop the client if it's running
                std::lock_guard<std::mutex> lock(client_mutex_);
                if (client_) {
                    client_->disconnect();
                    client_.reset();
                    response.result(http::status::ok);
                    response.body() = "Client disconnected!";
                    if (logger.is_debug_mode()) {
                        logger.log("Client stopped successfully");
                    }

                } else {
                    response.result(http::status::bad_request);
                    response.body() = "No active client to disconnect!";
                }

            }
            else if (request.method() == http::verb::post && request.target() == "/start") {
                try {
                    auto config = json::parse(request.body());
                    std::string server_ip = config.at("server_ip").as_string().c_str();
                    short server_port = static_cast<short>(config.at("server_port").as_int64());
                    std::string nickname = config.at("nickname").as_string().c_str();
                    int midi_in_port = static_cast<int>(config.at("midi_in").as_int64());
                    int midi_out_port = static_cast<int>(config.at("midi_out").as_int64());
                    int midi_in_port_2 = static_cast<int>(config.at("midi_in_2").as_int64());
                    uint8_t midi_channel = static_cast<uint8_t>(config.at("channel").as_int64());

                    std::lock_guard<std::mutex> lock(client_mutex_);
                    client_ = std::make_shared<MidiJamClient>(server_ip, server_port, nickname,
                        midi_in_port, midi_out_port, midi_in_port_2, midi_channel);
                    response.result(http::status::ok);
                    response.body() = "Client connected!";
                    if (logger.is_debug_mode()) {
                        logger.log("Client started successfully");
                    }

                } catch (const std::exception& e) {
                    response.result(http::status::bad_request);
                    response.body() = "Connection failed: " + std::string(e.what());
                    logger.log("Connection error: " + std::string(e.what()));
                }
            }
            else {
                // Handle unknown endpoints
                response.result(http::status::not_found);
                response.body() = "Not Found!";
            }
        } catch (const std::exception& e) {
            // Handle unexpected errors
            response.result(http::status::internal_server_error);
            response.body() = "Server error: " + std::string(e.what());
            logger.log("HTTP server error: " + std::string(e.what()));
        }

        // Send the response back to the client
        response.prepare_payload();
        http::async_write(*socket, response, [socket](boost::system::error_code, std::size_t) {});
    }
};

boost::asio::io_context* global_io_context = nullptr;
std::shared_ptr<MidiJamClient> global_client = nullptr;

void signal_handler(int signal) {
    if (signal == SIGINT) {
        logger.log("[Client] SIGINT received! Shutting down...");
        {
            std::lock_guard<std::mutex> lock(*reinterpret_cast<std::mutex*>(&global_client)); // Ensure thread safety
            if (global_client) {
                global_client->disconnect();
                global_client.reset();
            }
        }
        if (global_io_context) {
            global_io_context->stop();
        }
        std::exit(0);
    }
}

int main(int argc, char* argv[]) {
    try {
        // Check for debug argument
        bool debug_mode = false;
        for (int i = 1; i < argc; ++i) {
            if (std::string(argv[i]) == "-debug") {
                debug_mode = true;
                break;
            }
        }
        logger.set_debug_mode(debug_mode);
        logger.log("Debug mode: " + std::string(debug_mode ? "enabled" : "disabled")); // Log initial debug state

        boost::asio::io_context io_context;
        global_io_context = &io_context;

        auto work = boost::asio::make_work_guard(io_context);
        std::vector<std::thread> io_threads;
        int thread_count = std::max(1, static_cast<int>(std::thread::hardware_concurrency()) - 1); // Leave 1 core for OS
        for (int i = 0; i < thread_count; ++i) {
            io_threads.emplace_back([&io_context]() {
                while (!io_context.stopped()) {
                    size_t tasks = io_context.poll(); // Process tasks without busy-waiting
                    if (tasks == 0) std::this_thread::sleep_for(std::chrono::milliseconds(10)); // Sleep when idle
                }
            });
        }

        HttpServer server(io_context, 8080, "static");

        global_client = nullptr;
        std::string url = "http://localhost:8080";
#ifdef _WIN32
        int result = system(("start " + url).c_str());
        (void)result; // Suppress warning
#elif __APPLE__
        int result = system(("open " + url).c_str());
        (void)result; // Suppress warning
#else
        int result = system(("xdg-open " + url).c_str());
        (void)result; // Suppress warning
#endif

        std::signal(SIGINT, signal_handler);

        for (auto& t : io_threads) {
            if (t.joinable()) t.join();
        }

        if (global_client) {
            global_client->disconnect();
            global_client.reset();
        }
    } catch (const std::exception& e) {
        logger.log("Main error: " + std::string(e.what()));
        std::cerr << "Error: " << e.what() << "\n";
        return 1;
    }
    return 0;
}