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
#include <filesystem> // Required for working with file paths

using boost::asio::ip::udp;
namespace beast = boost::beast;
namespace http = beast::http;
using tcp = boost::asio::ip::tcp;
using json = boost::json::value;

class MidiJamClient {
    static constexpr size_t BUFFER_SIZE = 64;
    static constexpr std::chrono::milliseconds POLL_INTERVAL{1};
    static constexpr auto HEARTBEAT_INTERVAL = std::chrono::seconds(5);

    boost::asio::io_context io_context_;
    udp::socket udp_socket_;
    udp::endpoint server_endpoint_;
    std::string nickname_;
    RtMidiIn midi_in_;
    RtMidiIn midi_in_2_;
    RtMidiOut midi_out_;
    std::array<unsigned char, BUFFER_SIZE> recv_buffer_;
    volatile bool running_ = true;
    uint8_t midi_channel_;
    bool has_second_input_ = false;
    boost::asio::steady_timer heartbeat_timer_;
    std::vector<std::thread> thread_pool_;

    static void midi_callback(double, std::vector<unsigned char>* msg, void* userData) noexcept {
        if (!msg || msg->empty() || !userData) return;
        auto* client = static_cast<MidiJamClient*>(userData);
        uint8_t status = msg->at(0) & 0xF0;
        if (status != 0x80 && status != 0x90 && status != 0xB0) return;

        std::vector<unsigned char> adjusted = *msg;
        adjusted[0] = status | (client->midi_channel_ & 0x0F);
        client->udp_socket_.async_send_to(
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
            client->udp_socket_.send_to(boost::asio::buffer("QUIT"), client->server_endpoint_);
            std::cout << "Shutting down...\n";
        }
    }

public:
    static void* signal_user_data_;

    MidiJamClient(const std::string& server_ip, short server_port, const std::string& nickname,
                  int midi_in_port, int midi_out_port, int midi_in_port_2, uint8_t midi_channel)
        : udp_socket_(io_context_, udp::endpoint(udp::v4(), 0)),
          server_endpoint_(boost::asio::ip::make_address(server_ip), server_port),
          nickname_(nickname), midi_channel_(midi_channel), heartbeat_timer_(io_context_) {
        udp_socket_.set_option(boost::asio::socket_base::send_buffer_size(65536));
        udp_socket_.set_option(boost::asio::socket_base::receive_buffer_size(65536));

        signal_user_data_ = this;
        std::signal(SIGINT, &signal_handler);
        send_nickname();
        setup_midi(midi_in_port, midi_out_port, midi_in_port_2);
        start_receive();
        start_heartbeat();

        thread_pool_.emplace_back([this]() { io_context_.run(); });
    }

    ~MidiJamClient() {
        if (running_) {
            running_ = false;
            udp_socket_.send_to(boost::asio::buffer("QUIT"), server_endpoint_);
        }
        io_context_.stop();
        for (auto& t : thread_pool_) if (t.joinable()) t.join();
    }

private:
    void send_nickname() noexcept {
        udp_socket_.send_to(boost::asio::buffer(nickname_), server_endpoint_);
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
        udp_socket_.async_receive_from(
            boost::asio::buffer(recv_buffer_), *sender,
            [this, sender](const boost::system::error_code& ec, std::size_t bytes) {
                if (!ec && bytes > 0) {
                    std::vector<unsigned char> msg(recv_buffer_.begin(), recv_buffer_.begin() + bytes);
                    MidiUtils::sendMidiMessage(midi_out_, msg);
                    if (bytes == 4 && std::strncmp(reinterpret_cast<char*>(recv_buffer_.data()), "PING", 4) == 0) {
                        udp_socket_.send_to(boost::asio::buffer("PONG"), server_endpoint_);
                    }
                }
                if (running_) start_receive();
            });
    }

    void start_heartbeat() noexcept {
        heartbeat_timer_.expires_after(HEARTBEAT_INTERVAL);
        heartbeat_timer_.async_wait([this](const boost::system::error_code& ec) {
            if (!ec && running_) {
                udp_socket_.send_to(boost::asio::buffer("PONG"), server_endpoint_);
                start_heartbeat();
            }
        });
    }
};

void* MidiJamClient::signal_user_data_ = nullptr;

class HttpServer {
    boost::asio::io_context& io_context_;
    tcp::acceptor acceptor_;
    std::shared_ptr<MidiJamClient> client_; // Holds the active client instance
    std::filesystem::path static_dir_; // Path to the static directory

public:
    HttpServer(boost::asio::io_context& ioc, short port = 8080, const std::string& static_dir = "static")
        : io_context_(ioc), acceptor_(ioc, tcp::endpoint(tcp::v4(), port)), static_dir_(static_dir) {
        start_accept();
        std::cout << "HTTP server running at http://localhost:" << port << "\n";
    }

private:
    void start_accept() {
        auto socket = std::make_shared<tcp::socket>(acceptor_.get_executor());
        acceptor_.async_accept(*socket, [this, socket](boost::system::error_code ec) {
            if (!ec) {
                handle_request(socket);
            }
            start_accept();
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
            std::cerr << "Failed to open file: " << filepath << std::endl;
            return "Error: Could not load HTML file.";
        }
        std::string content((std::istreambuf_iterator<char>(file)),
                             std::istreambuf_iterator<char>());
        file.close();
        return content;
    }

	void process_request(std::shared_ptr<tcp::socket> socket, http::request<http::string_body>& request) {
		http::response<http::string_body> response;
		response.version(request.version());
		response.set(http::field::server, "MidiJam Client");
		response.keep_alive(false);

		if (request.method() == http::verb::get && request.target() == "/") {
			response.result(http::status::ok);
			response.set(http::field::content_type, "text/html");
			response.body() = readFile(static_dir_ / "index.html");  // Serve from static directory
		} else if (request.method() == http::verb::get && request.target() == "/midi-ports") {
			response.result(http::status::ok);
			response.set(http::field::content_type, "application/json");
			boost::json::object ports;
			ports["inputs"] = boost::json::array();
			ports["outputs"] = boost::json::array();

			RtMidiIn midi_in;
			RtMidiOut midi_out;
			for (unsigned int i = 0; i < midi_in.getPortCount(); i++) {
				ports["inputs"].as_array().push_back(boost::json::value(midi_in.getPortName(i)));
			}
			for (unsigned int i = 0; i < midi_out.getPortCount(); i++) {
				ports["outputs"].as_array().push_back(boost::json::value(midi_out.getPortName(i)));
			}
			response.body() = boost::json::serialize(ports);
		} else if (request.method() == http::verb::post && request.target() == "/start") {
			try {
				auto config = boost::json::parse(request.body());
				std::string server_ip = config.at("server_ip").as_string().c_str();
				short server_port = static_cast<short>(config.at("server_port").as_int64());
				std::string nickname = config.at("nickname").as_string().c_str();
				int midi_in_port = static_cast<int>(config.at("midi_in").as_int64());
				int midi_out_port = static_cast<int>(config.at("midi_out").as_int64());
				int midi_in_port_2 = static_cast<int>(config.at("midi_in_2").as_int64());
				uint8_t midi_channel = static_cast<uint8_t>(config.at("channel").as_int64());

				client_ = std::make_shared<MidiJamClient>(server_ip, server_port, nickname,
														midi_in_port, midi_out_port, midi_in_port_2, midi_channel);
				response.result(http::status::ok);
				response.body() = "Client started!";
			} catch (const std::exception& e) {
				response.result(http::status::bad_request);
				response.body() = "Invalid config: " + std::string(e.what());
			}
		} else if (request.method() == http::verb::post && request.target() == "/stop") {
			if (client_) {
				client_.reset(); // Destroys the client instance
				response.result(http::status::ok);
				response.body() = "Client stopped!";
			} else {
				response.result(http::status::bad_request);
				response.body() = "No active client to stop!";
			}
		} else if (request.method() == http::verb::get && request.target() == "/status") {
			response.result(http::status::ok);
			response.set(http::field::content_type, "application/json");
			boost::json::object status;
			status["isConnected"] = client_ != nullptr;
			response.body() = boost::json::serialize(status);
		} else {
			response.result(http::status::not_found);
			response.body() = "Not Found!";
		}

		response.prepare_payload();
		http::async_write(*socket, response, [socket](boost::system::error_code, std::size_t) {});
	}
};

int main() {
    try {
        boost::asio::io_context io_context;
        HttpServer server(io_context, 8080, "static"); // Pass the static directory name
      
        std::string url = "http://localhost:8080";
      
        // Automatically open browser
        #ifdef _WIN32
            system(("start " + url).c_str());
        #elif __APPLE__
            system(("open " + url).c_str());
        #else
            system(("xdg-open " + url).c_str());
        #endif

        io_context.run();
    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << "\n";
        return 1;
    }
    return 0;
}