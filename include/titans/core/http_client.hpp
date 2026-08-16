/**
 * @file http_client.hpp
 * @brief Lightweight HTTP Client for LLM API Integration
 *
 * Minimal dependency HTTP client using POSIX sockets.
 * Supports both blocking and async requests.
 */

#pragma once

#include <string>
#include <vector>
#include <unordered_map>
#include <functional>
#include <memory>
#include <future>
#include <chrono>
#include <stdexcept>
#include <sstream>
#include <cstring>
#include <netdb.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <poll.h>
#include <fcntl.h>

namespace titans {
namespace http {

// ============================================================================
// HTTP Types
// ============================================================================

enum class Method {
    GET,
    POST,
    PUT,
    DELETE
};

struct Header {
    std::string name;
    std::string value;
};

struct Request {
    Method method = Method::GET;
    std::string url;
    std::string host;
    int port = 80;
    std::string path = "/";
    std::vector<Header> headers;
    std::string body;
    int timeout_ms = 30000;
};

struct Response {
    int status_code = 0;
    std::string status_text;
    std::vector<Header> headers;
    std::string body;
    double elapsed_ms = 0;
    bool success = false;
    std::string error;
};

// ============================================================================
// URL Parser
// ============================================================================

class URLParser {
public:
    static bool parse(const std::string& url, std::string& host, int& port, std::string& path) {
        size_t proto_end = url.find("://");
        size_t host_start = (proto_end != std::string::npos) ? proto_end + 3 : 0;

        bool is_https = (proto_end != std::string::npos && url.substr(0, proto_end) == "https");
        port = is_https ? 443 : 80;

        size_t path_start = url.find('/', host_start);
        size_t port_start = url.find(':', host_start);

        if (port_start != std::string::npos && (path_start == std::string::npos || port_start < path_start)) {
            host = url.substr(host_start, port_start - host_start);
            size_t port_end = (path_start != std::string::npos) ? path_start : url.length();
            port = std::stoi(url.substr(port_start + 1, port_end - port_start - 1));
        } else {
            size_t host_end = (path_start != std::string::npos) ? path_start : url.length();
            host = url.substr(host_start, host_end - host_start);
        }

        path = (path_start != std::string::npos) ? url.substr(path_start) : "/";
        return !host.empty();
    }
};

// ============================================================================
// Socket Connection
// ============================================================================

class SocketConnection {
public:
    SocketConnection() : fd_(-1) {}

    ~SocketConnection() {
        close();
    }

    bool connect(const std::string& host, int port, int timeout_ms = 5000) {
        struct addrinfo hints{}, *result;
        hints.ai_family = AF_INET;
        hints.ai_socktype = SOCK_STREAM;

        std::string port_str = std::to_string(port);
        int rv = getaddrinfo(host.c_str(), port_str.c_str(), &hints, &result);
        if (rv != 0) {
            last_error_ = "DNS resolution failed: " + std::string(gai_strerror(rv));
            return false;
        }

        fd_ = socket(result->ai_family, result->ai_socktype, result->ai_protocol);
        if (fd_ < 0) {
            freeaddrinfo(result);
            last_error_ = "Socket creation failed";
            return false;
        }

        // Set non-blocking for timeout support
        set_nonblocking(true);

        int conn_result = ::connect(fd_, result->ai_addr, result->ai_addrlen);
        freeaddrinfo(result);

        if (conn_result < 0) {
            if (errno != EINPROGRESS) {
                close();
                last_error_ = "Connection failed: " + std::string(strerror(errno));
                return false;
            }

            // Wait for connection with timeout
            struct pollfd pfd;
            pfd.fd = fd_;
            pfd.events = POLLOUT;

            int poll_result = poll(&pfd, 1, timeout_ms);
            if (poll_result <= 0) {
                close();
                last_error_ = poll_result == 0 ? "Connection timeout" : "Poll failed";
                return false;
            }

            // Check for connection errors
            int error = 0;
            socklen_t len = sizeof(error);
            getsockopt(fd_, SOL_SOCKET, SO_ERROR, &error, &len);
            if (error != 0) {
                close();
                last_error_ = "Connection error: " + std::string(strerror(error));
                return false;
            }
        }

        set_nonblocking(false);
        return true;
    }

    bool send(const std::string& data) {
        size_t total_sent = 0;
        while (total_sent < data.size()) {
            ssize_t sent = ::send(fd_, data.c_str() + total_sent, data.size() - total_sent, 0);
            if (sent < 0) {
                last_error_ = "Send failed: " + std::string(strerror(errno));
                return false;
            }
            total_sent += sent;
        }
        return true;
    }

    std::string receive(int timeout_ms = 30000) {
        std::string result;
        char buffer[8192];

        struct pollfd pfd;
        pfd.fd = fd_;
        pfd.events = POLLIN;

        while (true) {
            int poll_result = poll(&pfd, 1, timeout_ms);
            if (poll_result < 0) {
                last_error_ = "Poll failed";
                break;
            }
            if (poll_result == 0) {
                if (result.empty()) {
                    last_error_ = "Receive timeout";
                }
                break;
            }

            ssize_t received = ::recv(fd_, buffer, sizeof(buffer), 0);
            if (received < 0) {
                last_error_ = "Receive failed: " + std::string(strerror(errno));
                break;
            }
            if (received == 0) {
                break;  // Connection closed
            }

            result.append(buffer, received);

            // Check for complete HTTP response
            if (is_response_complete(result)) {
                break;
            }
        }

        return result;
    }

    /**
     * @brief Read up to max_len bytes (single recv, for streaming consumers).
     * @return bytes read, 0 on orderly shutdown, -1 on timeout/error.
     */
    ssize_t receive_some(char* buffer, size_t max_len, int timeout_ms = 30000) {
        if (fd_ < 0) {
            last_error_ = "Socket not connected";
            return -1;
        }

        struct pollfd pfd;
        pfd.fd = fd_;
        pfd.events = POLLIN;

        int poll_result = poll(&pfd, 1, timeout_ms);
        if (poll_result < 0) {
            last_error_ = "Poll failed: " + std::string(strerror(errno));
            return -1;
        }
        if (poll_result == 0) {
            last_error_ = "Receive timeout";
            return -1;
        }

        ssize_t received = ::recv(fd_, buffer, max_len, 0);
        if (received < 0) {
            last_error_ = "Receive failed: " + std::string(strerror(errno));
        }
        return received;
    }

    void close() {
        if (fd_ >= 0) {
            ::close(fd_);
            fd_ = -1;
        }
    }

    bool is_open() const { return fd_ >= 0; }
    const std::string& last_error() const { return last_error_; }

private:
    void set_nonblocking(bool nonblocking) {
        int flags = fcntl(fd_, F_GETFL, 0);
        if (nonblocking) {
            fcntl(fd_, F_SETFL, flags | O_NONBLOCK);
        } else {
            fcntl(fd_, F_SETFL, flags & ~O_NONBLOCK);
        }
    }

    bool is_response_complete(const std::string& response) {
        // Find end of headers
        size_t header_end = response.find("\r\n\r\n");
        if (header_end == std::string::npos) return false;

        // Check for Content-Length
        size_t cl_pos = response.find("Content-Length:");
        if (cl_pos != std::string::npos && cl_pos < header_end) {
            size_t cl_start = cl_pos + 15;
            size_t cl_end = response.find("\r\n", cl_start);
            int content_length = std::stoi(response.substr(cl_start, cl_end - cl_start));
            size_t body_start = header_end + 4;
            return response.size() >= body_start + content_length;
        }

        // Check for chunked transfer encoding
        if (response.find("Transfer-Encoding: chunked") != std::string::npos) {
            return response.find("\r\n0\r\n\r\n") != std::string::npos;
        }

        // No Content-Length, assume complete if we got data
        return response.size() > header_end + 4;
    }

    int fd_;
    std::string last_error_;
};

// ============================================================================
// HTTP Client
// ============================================================================

class HttpClient {
public:
    HttpClient() = default;

    Response request(const Request& req) {
        auto start = std::chrono::high_resolution_clock::now();

        Response resp;
        resp.success = false;

        // Parse URL if needed
        std::string host = req.host;
        int port = req.port;
        std::string path = req.path;

        if (!req.url.empty()) {
            URLParser::parse(req.url, host, port, path);
        }

        // Connect
        SocketConnection conn;
        if (!conn.connect(host, port, 5000)) {
            resp.error = conn.last_error();
            return resp;
        }

        // Build HTTP request
        std::string http_request = build_request(req.method, path, host, req.headers, req.body);

        // Send
        if (!conn.send(http_request)) {
            resp.error = conn.last_error();
            return resp;
        }

        // Receive
        std::string raw_response = conn.receive(req.timeout_ms);
        if (raw_response.empty()) {
            resp.error = conn.last_error();
            return resp;
        }

        // Parse response
        parse_response(raw_response, resp);

        auto end = std::chrono::high_resolution_clock::now();
        resp.elapsed_ms = std::chrono::duration<double, std::milli>(end - start).count();
        resp.success = (resp.status_code >= 200 && resp.status_code < 300);

        return resp;
    }

    Response get(const std::string& url, int timeout_ms = 30000) {
        Request req;
        req.method = Method::GET;
        req.url = url;
        req.timeout_ms = timeout_ms;
        return request(req);
    }

    Response post(const std::string& url, const std::string& body,
                  const std::string& content_type = "application/json",
                  int timeout_ms = 30000) {
        Request req;
        req.method = Method::POST;
        req.url = url;
        req.body = body;
        req.headers = {{"Content-Type", content_type}};
        req.timeout_ms = timeout_ms;
        return request(req);
    }

    std::future<Response> async_request(const Request& req) {
        return std::async(std::launch::async, [this, req]() {
            return request(req);
        });
    }

    std::future<Response> async_post(const std::string& url, const std::string& body,
                                     int timeout_ms = 30000) {
        return std::async(std::launch::async, [this, url, body, timeout_ms]() {
            return post(url, body, "application/json", timeout_ms);
        });
    }

private:
    std::string build_request(Method method, const std::string& path,
                              const std::string& host,
                              const std::vector<Header>& headers,
                              const std::string& body) {
        std::ostringstream ss;

        // Request line
        switch (method) {
            case Method::GET: ss << "GET"; break;
            case Method::POST: ss << "POST"; break;
            case Method::PUT: ss << "PUT"; break;
            case Method::DELETE: ss << "DELETE"; break;
        }
        ss << " " << path << " HTTP/1.1\r\n";

        // Headers
        ss << "Host: " << host << "\r\n";
        ss << "Connection: close\r\n";

        for (const auto& h : headers) {
            ss << h.name << ": " << h.value << "\r\n";
        }

        if (!body.empty()) {
            ss << "Content-Length: " << body.size() << "\r\n";
        }

        ss << "\r\n";
        ss << body;

        return ss.str();
    }

    void parse_response(const std::string& raw, Response& resp) {
        size_t pos = 0;

        // Parse status line
        size_t line_end = raw.find("\r\n", pos);
        if (line_end == std::string::npos) return;

        std::string status_line = raw.substr(pos, line_end - pos);
        size_t space1 = status_line.find(' ');
        size_t space2 = status_line.find(' ', space1 + 1);

        if (space1 != std::string::npos && space2 != std::string::npos) {
            resp.status_code = std::stoi(status_line.substr(space1 + 1, space2 - space1 - 1));
            resp.status_text = status_line.substr(space2 + 1);
        }

        pos = line_end + 2;

        // Parse headers
        while (pos < raw.size()) {
            line_end = raw.find("\r\n", pos);
            if (line_end == std::string::npos) break;
            if (line_end == pos) {
                pos += 2;
                break;  // End of headers
            }

            std::string header_line = raw.substr(pos, line_end - pos);
            size_t colon = header_line.find(':');
            if (colon != std::string::npos) {
                std::string name = header_line.substr(0, colon);
                std::string value = header_line.substr(colon + 1);
                // Trim whitespace
                while (!value.empty() && value[0] == ' ') value.erase(0, 1);
                resp.headers.push_back({name, value});
            }

            pos = line_end + 2;
        }

        // Body
        resp.body = raw.substr(pos);

        // Handle chunked encoding
        for (const auto& h : resp.headers) {
            if (h.name == "Transfer-Encoding" && h.value == "chunked") {
                resp.body = decode_chunked(resp.body);
                break;
            }
        }
    }

    std::string decode_chunked(const std::string& chunked) {
        std::string result;
        size_t pos = 0;

        while (pos < chunked.size()) {
            size_t size_end = chunked.find("\r\n", pos);
            if (size_end == std::string::npos) break;

            int chunk_size = std::stoi(chunked.substr(pos, size_end - pos), nullptr, 16);
            if (chunk_size == 0) break;

            pos = size_end + 2;
            result.append(chunked.substr(pos, chunk_size));
            pos += chunk_size + 2;  // Skip chunk data and trailing \r\n
        }

        return result;
    }
};

// ============================================================================
// Connection Pool (for high-throughput scenarios)
// ============================================================================

class ConnectionPool {
public:
    explicit ConnectionPool(const std::string& host, int port, size_t max_connections = 10)
        : host_(host), port_(port), max_connections_(max_connections) {}

    std::shared_ptr<SocketConnection> acquire() {
        std::lock_guard<std::mutex> lock(mutex_);

        // Try to find an available connection
        for (auto& conn : connections_) {
            if (conn.use_count() == 1 && conn->is_open()) {
                return conn;
            }
        }

        // Create new connection if under limit
        if (connections_.size() < max_connections_) {
            auto conn = std::make_shared<SocketConnection>();
            if (conn->connect(host_, port_)) {
                connections_.push_back(conn);
                return conn;
            }
        }

        // Wait for available connection (simple approach)
        return nullptr;
    }

    void clear() {
        std::lock_guard<std::mutex> lock(mutex_);
        connections_.clear();
    }

private:
    std::string host_;
    int port_;
    size_t max_connections_;
    std::vector<std::shared_ptr<SocketConnection>> connections_;
    std::mutex mutex_;
};

}  // namespace http
}  // namespace titans
