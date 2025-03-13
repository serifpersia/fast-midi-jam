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

using boost::asio::ip::udp;
namespace beast = boost::beast;
namespace http = beast::http;
using tcp = boost::asio::ip::tcp;
namespace json = boost::json;

class MidiJamClient {
    static constexpr size_t BUFFER_SIZE = 64;      // For MIDI and control messages
    static constexpr size_t JSON_BUFFER_SIZE = 4096; // Larger buffer for JSON data
    static constexpr std::chrono::milliseconds POLL_INTERVAL{1};
    static constexpr auto HEARTBEAT_INTERVAL = std::chrono::seconds(5);
    static constexpr auto CLIENT_LIST_INTERVAL = std::chrono::seconds(5); // Increased to reduce load

    boost::asio::io_context io_context_;
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
    boost::asio::steady_timer heartbeat_timer_;
    boost::asio::steady_timer client_list_timer_;
    std::vector<std::thread> thread_pool_;
    bool connected_ = false;
    json::value last_client_list_;  // Use json::value directly
    mutable std::mutex client_list_mutex_;  // Mutable to allow locking in const method

    int midi_in_port_;
    int midi_out_port_;
    int midi_in_port_2_;

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

public:
    MidiJamClient(const std::string& server_ip, short server_port, const std::string& nickname,
                  int midi_in_port, int midi_out_port, int midi_in_port_2, uint8_t midi_channel)
        : udp_socket_(io_context_, udp::endpoint(udp::v4(), 0)),
          server_endpoint_(boost::asio::ip::make_address(server_ip), server_port),
          nickname_(nickname), midi_channel_(midi_channel), 
          heartbeat_timer_(io_context_), client_list_timer_(io_context_),
          midi_in_port_(midi_in_port), midi_out_port_(midi_out_port), midi_in_port_2_(midi_in_port_2) {
        connect();
    }

    ~MidiJamClient() {
        disconnect();
    }

    void connect() {
        if (connected_) return;
        udp_socket_.set_option(boost::asio::socket_base::send_buffer_size(65536));
        udp_socket_.set_option(boost::asio::socket_base::receive_buffer_size(65536));
        send_nickname();
        setup_midi(midi_in_port_, midi_out_port_, midi_in_port_2_);
        start_receive();
        start_heartbeat();
        start_client_list_requests();
        connected_ = true;
        thread_pool_.emplace_back([this]() { io_context_.run(); });
    }

    void disconnect() {
        if (!connected_) return;
        running_ = false;
        boost::system::error_code ec;
        udp_socket_.send_to(boost::asio::buffer("QUIT", 4), server_endpoint_, 0, ec);
        if (ec) std::cerr << "Error sending QUIT: " << ec.message() << "\n";
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        heartbeat_timer_.cancel(); // Cancel without error_code
        client_list_timer_.cancel();
        io_context_.stop();
        for (auto& t : thread_pool_) if (t.joinable()) t.join();
        thread_pool_.clear();
        connected_ = false;
        std::cout << "Disconnected from server\n";
    }

    bool is_connected() const { return connected_; }

    json::value get_client_list() const {
        std::lock_guard<std::mutex> lock(client_list_mutex_);
        return last_client_list_;
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
        json_buffer_.fill(0);
        udp_socket_.async_receive_from(
            boost::asio::buffer(json_buffer_), *sender,
            [this, sender](const boost::system::error_code& ec, std::size_t bytes) {
                if (!ec && bytes > 0) {
                    if (bytes > JSON_BUFFER_SIZE) {
                        std::cerr << "Received oversized data: " << bytes << " bytes, discarding\n";
                    } else if (bytes == 4 && std::memcmp(json_buffer_.data(), "PING", 4) == 0) {
                        boost::system::error_code send_ec;
                        udp_socket_.send_to(boost::asio::buffer("PONG", 4), server_endpoint_, 0, send_ec);
                        if (send_ec) std::cerr << "PONG send error: " << send_ec.message() << "\n";
                    } else if (bytes >= 1 && (json_buffer_[0] & 0x80)) {
                        std::vector<unsigned char> msg(json_buffer_.begin(), json_buffer_.begin() + bytes);
                        MidiUtils::sendMidiMessage(midi_out_, msg);
                    } else {
                        std::string json_str(json_buffer_.data(), bytes);
                        if (!json_str.empty() && (json_str[0] == '{' || json_str[0] == '[')) {
                            try {
                                std::lock_guard<std::mutex> lock(client_list_mutex_);
                                last_client_list_ = json::parse(json_str);
                            } catch (const std::exception& e) {
                                std::cerr << "JSON parse error: " << e.what() << " Raw data: " << json_str << "\n";
                            }
                        }
                    }
                }
                if (running_) start_receive();
            });
    }

    void start_heartbeat() noexcept {
        heartbeat_timer_.expires_after(HEARTBEAT_INTERVAL);
        heartbeat_timer_.async_wait([this](const boost::system::error_code& ec) {
            if (!ec && running_ && connected_) {
                boost::system::error_code send_ec;
                udp_socket_.send_to(boost::asio::buffer("PONG", 4), server_endpoint_, 0, send_ec);
                if (!send_ec) start_heartbeat();
                else std::cerr << "Heartbeat send error: " << send_ec.message() << "\n";
            }
        });
    }

    void start_client_list_requests() noexcept {
        client_list_timer_.expires_after(CLIENT_LIST_INTERVAL);
        client_list_timer_.async_wait([this](const boost::system::error_code& ec) {
            if (!ec && running_ && connected_) {
                boost::system::error_code send_ec;
                udp_socket_.send_to(boost::asio::buffer("CLIST", 5), server_endpoint_, 0, send_ec);
                if (!send_ec) start_client_list_requests();
                else std::cerr << "CLIST send error: " << send_ec.message() << "\n";
            }
        });
    }
};

// HttpServer and main remain unchanged
class HttpServer {
    boost::asio::io_context& io_context_;
    tcp::acceptor acceptor_;
    std::shared_ptr<MidiJamClient> client_;
    std::filesystem::path static_dir_;

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
            response.body() = readFile(static_dir_ / "index.html");
        } else if (request.method() == http::verb::get && request.target() == "/midi-ports") {
            response.result(http::status::ok);
            response.set(http::field::content_type, "application/json");
            json::object ports;
            ports["inputs"] = json::array();
            ports["outputs"] = json::array();

            RtMidiIn midi_in;
            RtMidiOut midi_out;
            for (unsigned int i = 0; i < midi_in.getPortCount(); i++) {
                ports["inputs"].as_array().emplace_back(midi_in.getPortName(i));
            }
            for (unsigned int i = 0; i < midi_out.getPortCount(); i++) {
                ports["outputs"].as_array().emplace_back(midi_out.getPortName(i));
            }
            response.body() = json::serialize(ports);
        } else if (request.method() == http::verb::post && request.target() == "/stop") {
            if (client_) {
                client_->disconnect();
                client_.reset();
                response.result(http::status::ok);
                response.body() = "Client disconnected!";
            } else {
                response.result(http::status::bad_request);
                response.body() = "No active client to disconnect!";
            }
        } else if (request.method() == http::verb::post && request.target() == "/start") {
            try {
                auto config = json::parse(request.body());
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
                response.body() = "Client connected!";
            } catch (const std::exception& e) {
                response.result(http::status::bad_request);
                response.body() = "Invalid config: " + std::string(e.what());
            }
        } else if (request.method() == http::verb::get && request.target() == "/status") {
            response.result(http::status::ok);
            response.set(http::field::content_type, "application/json");
            json::object status;
            status["isConnected"] = client_ && client_->is_connected();
            response.body() = json::serialize(status);
        } else if (request.method() == http::verb::get && request.target() == "/clients") {
            response.result(http::status::ok);
            response.set(http::field::content_type, "application/json");
            if (client_) {
                response.body() = json::serialize(client_->get_client_list());
            } else {
                json::object empty_list;
                empty_list["clients"] = json::array();
                response.body() = json::serialize(empty_list);
            }
        } else {
            response.result(http::status::not_found);
            response.body() = "Not Found!";
        }

        response.prepare_payload();
        http::async_write(*socket, response, [socket](boost::system::error_code, std::size_t) {});
    }
};

boost::asio::io_context* global_io_context = nullptr;
std::shared_ptr<MidiJamClient> global_client = nullptr;

void signal_handler(int signal) {
    if (signal == SIGINT) {
        std::cout << "\n[Client] SIGINT received! Shutting down...\n";
        if (global_client) {
            global_client->disconnect();
            global_client.reset();
        }
        if (global_io_context) {
            global_io_context->stop();
        }
        std::exit(0);
    }
}

int main() {
    try {
        boost::asio::io_context io_context;
        global_io_context = &io_context;
        global_client = nullptr;

        HttpServer server(io_context, 8080, "static");

        std::string url = "http://localhost:8080";
#ifdef _WIN32
        system(("start " + url).c_str());
#elif __APPLE__
        system(("open " + url).c_str());
#else
        system(("xdg-open " + url).c_str());
#endif

        std::signal(SIGINT, signal_handler);
        io_context.run();

        if (global_client) {
            global_client->disconnect();
            global_client.reset();
        }

    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << "\n";
        return 1;
    }
    return 0;
}