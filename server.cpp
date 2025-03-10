#include <boost/asio.hpp>
#include <iostream>
#include <unordered_map>
#include <string>
#include <chrono>
#include <fstream>
#include <sstream> // For stringstream

using boost::asio::ip::udp;

struct Client {
    udp::endpoint endpoint;
    uint8_t channel;
    std::string nickname;
    std::chrono::steady_clock::time_point last_active;

    Client() : channel(0), nickname("unknown"), last_active(std::chrono::steady_clock::now()) {}

    Client(udp::endpoint ep, std::string name) : endpoint(std::move(ep)), channel(0), nickname(std::move(name)), last_active(std::chrono::steady_clock::now()) {}
};

struct EndpointHash {
    std::size_t operator()(const udp::endpoint& ep) const {
        return std::hash<std::string>()(ep.address().to_string() + std::to_string(ep.port()));
    }
};
struct EndpointEqual {
    bool operator()(const udp::endpoint& lhs, const udp::endpoint& rhs) const {
        return lhs.address() == rhs.address() && lhs.port() == rhs.port();
    }
};

class MidiJamServer {
    udp::socket socket_;
    std::unordered_map<udp::endpoint, Client, EndpointHash, EndpointEqual> clients_;
    std::array<char, 1024> buffer_;
    const std::chrono::seconds INACTIVITY_TIMEOUT;

public:
    MidiJamServer(boost::asio::io_context& io_context, short port, int inactivity_timeout)
        : socket_(io_context, udp::endpoint(udp::v4(), port)),
        INACTIVITY_TIMEOUT(inactivity_timeout) {
        clients_.reserve(50);
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
                    std::string msg(buffer_.begin(), buffer_.begin() + bytes);
                    auto client_it = clients_.find(*sender);

                    if (client_it == clients_.end()) {
                        std::string nickname(buffer_.begin(), buffer_.begin() + bytes);
                        clients_[*sender] = { *sender, nickname };
                        std::cout << "New client: " << nickname << " @ " << sender->address() << ":" << sender->port() << "\n";
                    } else {
                        Client& client = client_it->second;
                        client.last_active = std::chrono::steady_clock::now();

                        if (msg == "QUIT") {
                            std::cout << "Client " << client.nickname << " disconnected\n";
                            clients_.erase(*sender);
                        }
                        else {
                            if (bytes > 0) {
                                uint8_t status_byte = static_cast<uint8_t>(buffer_[0]);
                                if (status_byte >= 0x80 && status_byte <= 0xEF) {
                                    client.channel = status_byte & 0x0F;
                                }
                            }
                            forward_midi(*sender, bytes);
                        }
                    }
                }
                start_receive();
            });
    }

    void forward_midi(const udp::endpoint& sender, std::size_t bytes) {
        for (auto& [endpoint, client] : clients_) {
            if (endpoint != sender) {
                socket_.async_send_to(boost::asio::buffer(buffer_, bytes), endpoint,
                    [](boost::system::error_code ec, std::size_t) {
                        if (ec) { /* Handle error, maybe log it */ }
                    });
            }
        }
    }

    void start_cleanup() {
        boost::asio::steady_timer timer(socket_.get_executor(), std::chrono::seconds(1)); // Check more often
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
                std::cout << "Removing inactive client: " << it->second.nickname << "\n";
                it = clients_.erase(it);
            } else {
                ++it;
            }
        }
    }
};

struct Config {
    short port = 5000;
    int inactivity_timeout = 30;
};

Config load_config(const std::string& filename) {
    Config cfg;
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
                        if (key == "port") {
                            cfg.port = std::stoi(value);
                        } else if (key == "inactivity_timeout") {
                            cfg.inactivity_timeout = std::stoi(value);
                        }
                    } catch (const std::invalid_argument& e) {
                        std::cerr << "Invalid config value for " << key << ": " << value << "\n";
                    }
                }
            }
        }
        file.close();
    }
    return cfg;
}

void save_config(const std::string& filename, const Config& config) {
    std::ofstream file(filename);
    if (file.is_open()) {
        file << "port=" << config.port << "\n";
        file << "inactivity_timeout=" << config.inactivity_timeout << "\n";
        file.close();
    } else {
        std::cerr << "Error saving config file: " << filename << "\n";
    }
}

int main() {
    try {
        Config config = load_config("server_config.cfg");
        bool config_loaded = true;

        if (config.port == 0) {
            config_loaded = false;
        }

        if (!config_loaded) {
            std::cout << "Enter UDP port: ";
            std::cin >> config.port;
            std::cout << "Enter inactivity timeout (seconds): ";
            std::cin >> config.inactivity_timeout;
        }

        save_config("server_config.cfg", config);

        boost::asio::io_context io_context;
        MidiJamServer server(io_context, config.port, config.inactivity_timeout);
        io_context.run();
    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << "\n";
        return 1;
    }
    return 0;
}