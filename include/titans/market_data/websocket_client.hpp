/**
 * @file websocket_client.hpp
 * @brief WebSocket Client for Real-time Market Data
 *
 * Implements a high-performance WebSocket client for connecting
 * to cryptocurrency exchanges (Binance, etc.) and receiving
 * real-time market data feeds.
 */

#pragma once

#include "titans/core/types.hpp"
#include "titans/core/event_bus.hpp"
#include "titans/core/event_loop.hpp"

#include <string>
#include <vector>
#include <functional>
#include <memory>
#include <queue>
#include <atomic>
#include <thread>

#include <sys/socket.h>
#include <netinet/in.h>
#include <netdb.h>
#include <arpa/inet.h>

namespace titans {

/**
 * @brief WebSocket connection state
 */
enum class WSState {
    Disconnected,
    Connecting,
    Connected,
    Closing,
    Error
};

/**
 * @brief WebSocket frame opcodes
 */
enum class WSOpcode : uint8_t {
    Continuation = 0x0,
    Text = 0x1,
    Binary = 0x2,
    Close = 0x8,
    Ping = 0x9,
    Pong = 0xA
};

/**
 * @brief WebSocket configuration
 */
struct WSConfig {
    std::string host;
    int port = 443;
    std::string path = "/";
    bool use_ssl = true;
    Duration ping_interval_ms = 30000;
    Duration reconnect_delay_ms = 5000;
    int max_reconnect_attempts = 10;
    size_t max_message_size = 1024 * 1024;  // 1MB
};

/**
 * @brief WebSocket message
 */
struct WSMessage {
    WSOpcode opcode;
    std::vector<char> data;
    Timestamp received_at;
};

/**
 * @brief WebSocket client callbacks
 */
struct WSCallbacks {
    std::function<void()> on_connect;
    std::function<void(int code, const std::string& reason)> on_disconnect;
    std::function<void(const std::string& message)> on_message;
    std::function<void(const void* data, size_t size)> on_binary;
    std::function<void(const std::string& error)> on_error;
};

/**
 * @brief Low-level WebSocket client
 *
 * Note: This is a simplified implementation. Production use should
 * integrate with a proper WebSocket library like libwebsockets or Beast.
 */
class WebSocketClient {
public:
    explicit WebSocketClient(EventLoop& loop, const WSConfig& config = {})
        : loop_(loop), config_(config), state_(WSState::Disconnected),
          socket_fd_(-1), reconnect_attempts_(0) {}

    ~WebSocketClient() {
        disconnect();
    }

    /**
     * @brief Connect to the WebSocket server
     */
    bool connect() {
        if (state_ == WSState::Connected || state_ == WSState::Connecting) {
            return false;
        }

        state_ = WSState::Connecting;

        // Resolve hostname
        struct hostent* host = gethostbyname(config_.host.c_str());
        if (!host) {
            handle_error("Failed to resolve hostname");
            return false;
        }

        // Create socket
        socket_fd_ = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);
        if (socket_fd_ < 0) {
            handle_error("Failed to create socket");
            return false;
        }

        // Connect
        struct sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(config_.port);
        std::memcpy(&addr.sin_addr, host->h_addr, host->h_length);

        int result = ::connect(socket_fd_, (struct sockaddr*)&addr, sizeof(addr));

        if (result < 0 && errno != EINPROGRESS) {
            handle_error("Connection failed");
            return false;
        }

        // Add to event loop
        loop_.add_fd(socket_fd_, EPOLLOUT | EPOLLIN | EPOLLET,
            [this](uint32_t events) {
                handle_events(events);
            });

        return true;
    }

    /**
     * @brief Disconnect from the server
     */
    void disconnect() {
        if (socket_fd_ >= 0) {
            loop_.remove_fd(socket_fd_);
            ::close(socket_fd_);
            socket_fd_ = -1;
        }
        state_ = WSState::Disconnected;
    }

    /**
     * @brief Send a text message
     */
    bool send(const std::string& message) {
        return send_frame(WSOpcode::Text, message.data(), message.size());
    }

    /**
     * @brief Send a binary message
     */
    bool send_binary(const void* data, size_t size) {
        return send_frame(WSOpcode::Binary, data, size);
    }

    /**
     * @brief Send a ping
     */
    void ping() {
        send_frame(WSOpcode::Ping, nullptr, 0);
    }

    /**
     * @brief Set callbacks
     */
    void set_callbacks(const WSCallbacks& callbacks) {
        callbacks_ = callbacks;
    }

    WSState state() const { return state_; }
    bool is_connected() const { return state_ == WSState::Connected; }

private:
    void handle_events(uint32_t events) {
        if (events & EPOLLERR) {
            handle_error("Socket error");
            return;
        }

        if (state_ == WSState::Connecting && (events & EPOLLOUT)) {
            // Check connection result
            int error = 0;
            socklen_t len = sizeof(error);
            if (getsockopt(socket_fd_, SOL_SOCKET, SO_ERROR, &error, &len) < 0 || error != 0) {
                handle_error("Connection failed");
                return;
            }

            // Send WebSocket handshake
            if (send_handshake()) {
                state_ = WSState::Connected;
                reconnect_attempts_ = 0;
                if (callbacks_.on_connect) {
                    callbacks_.on_connect();
                }
            }
        }

        if (events & EPOLLIN) {
            read_data();
        }

        if (events & EPOLLHUP) {
            handle_disconnect(1000, "Connection closed");
        }
    }

    bool send_handshake() {
        std::string key = generate_ws_key();

        std::string request =
            "GET " + config_.path + " HTTP/1.1\r\n"
            "Host: " + config_.host + "\r\n"
            "Upgrade: websocket\r\n"
            "Connection: Upgrade\r\n"
            "Sec-WebSocket-Key: " + key + "\r\n"
            "Sec-WebSocket-Version: 13\r\n"
            "\r\n";

        return ::send(socket_fd_, request.data(), request.size(), 0) > 0;
    }

    std::string generate_ws_key() {
        // Simplified - should use proper random base64
        return "dGhlIHNhbXBsZSBub25jZQ==";
    }

    void read_data() {
        char buffer[65536];
        ssize_t n;

        while ((n = ::recv(socket_fd_, buffer, sizeof(buffer), 0)) > 0) {
            recv_buffer_.insert(recv_buffer_.end(), buffer, buffer + n);
            process_received_data();
        }

        if (n == 0) {
            handle_disconnect(1000, "Connection closed by peer");
        } else if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
            handle_error("Read error");
        }
    }

    void process_received_data() {
        // Parse WebSocket frames
        while (recv_buffer_.size() >= 2) {
            size_t frame_size;
            WSMessage msg;

            if (!parse_frame(recv_buffer_.data(), recv_buffer_.size(),
                            frame_size, msg)) {
                break;  // Incomplete frame
            }

            // Remove processed data
            recv_buffer_.erase(recv_buffer_.begin(),
                              recv_buffer_.begin() + frame_size);

            // Handle frame
            handle_frame(msg);
        }
    }

    bool parse_frame(const char* data, size_t size,
                     size_t& frame_size, WSMessage& msg) {
        if (size < 2) return false;

        uint8_t byte0 = static_cast<uint8_t>(data[0]);
        uint8_t byte1 = static_cast<uint8_t>(data[1]);

        msg.opcode = static_cast<WSOpcode>(byte0 & 0x0F);
        bool masked = (byte1 & 0x80) != 0;
        uint64_t payload_len = byte1 & 0x7F;

        size_t header_size = 2;

        if (payload_len == 126) {
            if (size < 4) return false;
            payload_len = (static_cast<uint16_t>(data[2]) << 8) |
                          static_cast<uint16_t>(data[3]);
            header_size = 4;
        } else if (payload_len == 127) {
            if (size < 10) return false;
            payload_len = 0;
            for (int i = 0; i < 8; ++i) {
                payload_len = (payload_len << 8) |
                              static_cast<uint8_t>(data[2 + i]);
            }
            header_size = 10;
        }

        if (masked) header_size += 4;

        frame_size = header_size + payload_len;
        if (size < frame_size) return false;

        // Extract payload
        msg.data.resize(payload_len);
        const char* payload = data + header_size - (masked ? 4 : 0);

        if (masked) {
            uint8_t mask[4];
            std::memcpy(mask, data + header_size - 4, 4);
            for (size_t i = 0; i < payload_len; ++i) {
                msg.data[i] = payload[4 + i] ^ mask[i % 4];
            }
        } else {
            std::memcpy(msg.data.data(), payload, payload_len);
        }

        msg.received_at = now_ns();
        return true;
    }

    void handle_frame(const WSMessage& msg) {
        switch (msg.opcode) {
            case WSOpcode::Text:
                if (callbacks_.on_message) {
                    callbacks_.on_message(std::string(msg.data.begin(), msg.data.end()));
                }
                break;

            case WSOpcode::Binary:
                if (callbacks_.on_binary) {
                    callbacks_.on_binary(msg.data.data(), msg.data.size());
                }
                break;

            case WSOpcode::Ping:
                send_frame(WSOpcode::Pong, msg.data.data(), msg.data.size());
                break;

            case WSOpcode::Pong:
                // Pong received - connection is alive
                break;

            case WSOpcode::Close:
                handle_disconnect(1000, "Close frame received");
                break;

            default:
                break;
        }
    }

    bool send_frame(WSOpcode opcode, const void* data, size_t size) {
        if (socket_fd_ < 0) return false;

        std::vector<char> frame;

        // Header
        frame.push_back(static_cast<char>(0x80 | static_cast<uint8_t>(opcode)));

        // Payload length (client must mask)
        if (size < 126) {
            frame.push_back(static_cast<char>(0x80 | size));
        } else if (size < 65536) {
            frame.push_back(static_cast<char>(0x80 | 126));
            frame.push_back(static_cast<char>((size >> 8) & 0xFF));
            frame.push_back(static_cast<char>(size & 0xFF));
        } else {
            frame.push_back(static_cast<char>(0x80 | 127));
            for (int i = 7; i >= 0; --i) {
                frame.push_back(static_cast<char>((size >> (i * 8)) & 0xFF));
            }
        }

        // Masking key (simplified)
        uint8_t mask[4] = {0x12, 0x34, 0x56, 0x78};
        frame.insert(frame.end(), mask, mask + 4);

        // Masked payload
        const char* payload = static_cast<const char*>(data);
        for (size_t i = 0; i < size; ++i) {
            frame.push_back(payload[i] ^ mask[i % 4]);
        }

        return ::send(socket_fd_, frame.data(), frame.size(), 0) > 0;
    }

    void handle_error(const std::string& error) {
        state_ = WSState::Error;
        if (callbacks_.on_error) {
            callbacks_.on_error(error);
        }
        schedule_reconnect();
    }

    void handle_disconnect(int code, const std::string& reason) {
        disconnect();
        if (callbacks_.on_disconnect) {
            callbacks_.on_disconnect(code, reason);
        }
        schedule_reconnect();
    }

    void schedule_reconnect() {
        if (reconnect_attempts_ >= config_.max_reconnect_attempts) {
            return;
        }

        ++reconnect_attempts_;
        Duration delay = config_.reconnect_delay_ms * reconnect_attempts_ * 1000000;

        loop_.schedule_timer(delay, [this]() {
            connect();
        });
    }

    EventLoop& loop_;
    WSConfig config_;
    WSCallbacks callbacks_;
    WSState state_;
    int socket_fd_;
    int reconnect_attempts_;
    std::vector<char> recv_buffer_;
};

/**
 * @brief Binance WebSocket feed handler
 */
class BinanceFeedHandler : public EventPublisher {
public:
    BinanceFeedHandler(EventBus& bus, EventLoop& loop)
        : EventPublisher(bus), loop_(loop),
          ws_(loop, WSConfig{
              .host = "stream.binance.com",
              .port = 9443,
              .path = "/ws",
              .use_ssl = true
          }) {

        ws_.set_callbacks({
            .on_connect = [this]() { on_connect(); },
            .on_disconnect = [this](int code, const std::string& reason) {
                on_disconnect(code, reason);
            },
            .on_message = [this](const std::string& msg) {
                on_message(msg);
            },
            .on_error = [this](const std::string& error) {
                on_error(error);
            }
        });
    }

    /**
     * @brief Connect to Binance
     */
    bool connect() {
        return ws_.connect();
    }

    /**
     * @brief Subscribe to a symbol's depth stream
     */
    void subscribe_depth(const std::string& symbol, int levels = 20) {
        std::string stream = symbol + "@depth" + std::to_string(levels) + "@100ms";
        subscriptions_.push_back(stream);

        if (ws_.is_connected()) {
            send_subscribe({stream});
        }
    }

    /**
     * @brief Subscribe to trades
     */
    void subscribe_trades(const std::string& symbol) {
        std::string stream = symbol + "@trade";
        subscriptions_.push_back(stream);

        if (ws_.is_connected()) {
            send_subscribe({stream});
        }
    }

private:
    void on_connect() {
        if (!subscriptions_.empty()) {
            send_subscribe(subscriptions_);
        }
    }

    void on_disconnect(int code, const std::string& reason) {
        // Will auto-reconnect
    }

    void on_message(const std::string& msg) {
        // Parse JSON and create events
        // This is simplified - use a proper JSON parser
        parse_message(msg);
    }

    void on_error(const std::string& error) {
        // Log error
    }

    void send_subscribe(const std::vector<std::string>& streams) {
        // Build subscription message
        std::string msg = "{\"method\":\"SUBSCRIBE\",\"params\":[";
        for (size_t i = 0; i < streams.size(); ++i) {
            if (i > 0) msg += ",";
            msg += "\"" + streams[i] + "\"";
        }
        msg += "],\"id\":1}";

        ws_.send(msg);
    }

    void parse_message(const std::string& msg) {
        // Simplified JSON parsing - use nlohmann/json or simdjson in production

        if (msg.find("\"e\":\"depthUpdate\"") != std::string::npos) {
            // Parse depth update
            BookUpdateEvent event;
            // ... parse fields ...
            publish(event);
        } else if (msg.find("\"e\":\"trade\"") != std::string::npos) {
            // Parse trade
            TradeEvent event;
            // ... parse fields ...
            publish(event);
        }
    }

    EventLoop& loop_;
    WebSocketClient ws_;
    std::vector<std::string> subscriptions_;
};

}  // namespace titans
