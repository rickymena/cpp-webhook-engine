#include "../include/http_server.h"
#include "../include/request_handler.h"
#include <algorithm>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <iostream>
#include <string>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

namespace {

// send() may write fewer bytes than asked; loop until done or the peer
// is gone. MSG_NOSIGNAL so a hung-up client yields EPIPE, not SIGPIPE.
bool sendAll(int fd, const std::string& data) {
    const char* p = data.data();
    size_t remaining = data.size();
    while (remaining > 0) {
        ssize_t n = send(fd, p, remaining, MSG_NOSIGNAL);
        if (n < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        if (n == 0) return false;
        p += n;
        remaining -= static_cast<size_t>(n);
    }
    return true;
}

void sendSimpleResponse(int fd, int code, const std::string& text) {
    std::string body = std::to_string(code) + " " + text;
    std::string response =
        "HTTP/1.1 " + std::to_string(code) + " " + text + "\r\n"
        "Content-Type: text/plain\r\n"
        "Content-Length: " + std::to_string(body.size()) + "\r\n"
        "Connection: close\r\n"
        "\r\n" + body;
    sendAll(fd, response);
}

std::string trimWhitespace(const std::string& s) {
    size_t first = s.find_first_not_of(" \t");
    if (first == std::string::npos) return "";
    size_t last = s.find_last_not_of(" \t");
    return s.substr(first, last - first + 1);
}

// Case-insensitive header lookup in the raw header block (header names
// are case-insensitive per RFC 7230).
bool findHeaderValue(const std::string& header_block, const std::string& lower_name,
                     std::string& value_out) {
    size_t pos = 0;
    while (pos < header_block.size()) {
        size_t eol = header_block.find("\r\n", pos);
        if (eol == std::string::npos) eol = header_block.size();
        size_t colon = header_block.find(':', pos);
        if (colon != std::string::npos && colon < eol) {
            std::string key = header_block.substr(pos, colon - pos);
            std::transform(key.begin(), key.end(), key.begin(),
                           [](unsigned char c) { return std::tolower(c); });
            if (key == lower_name) {
                value_out = trimWhitespace(header_block.substr(colon + 1, eol - colon - 1));
                return true;
            }
        }
        if (eol == header_block.size()) break;
        pos = eol + 2;
    }
    return false;
}

bool parseContentLength(const std::string& value, size_t& out) {
    if (value.empty() || value.size() > 12) return false;
    size_t result = 0;
    for (char c : value) {
        if (!std::isdigit(static_cast<unsigned char>(c))) return false;
        result = result * 10 + static_cast<size_t>(c - '0');
    }
    out = result;
    return true;
}

} // namespace


HttpServer::HttpServer(int port)
    : server_fd(-1), port(port), running(false), active_clients(0) {}


HttpServer::~HttpServer() {
    stop();
}


bool HttpServer::start() {
    if (!setupSocket()) {
        return false;
    }

    running = true;
    accept_thread = std::thread(&HttpServer::acceptLoop, this);
    return true;
}

void HttpServer::stop() {
    running = false;

    // Close socket to interrupt accept()
    if (server_fd != -1) {
        shutdown(server_fd, SHUT_RDWR);
        close(server_fd);
        server_fd = -1;
    }

    // Wait for accept thread to finish
    if (accept_thread.joinable()) {
        accept_thread.join();
    }

    // Let in-flight requests finish (socket timeouts bound how long a
    // client thread can live, so this converges).
    std::unique_lock<std::mutex> lock(drain_mutex);
    bool drained = drain_cv.wait_for(lock, std::chrono::seconds(http_limits::kDrainTimeoutSec),
                                     [this] { return active_clients.load() == 0; });
    if (!drained) {
        std::cerr << "Shutdown: " << active_clients.load()
                  << " client(s) still active after drain timeout" << std::endl;
    }
}


bool HttpServer::setupSocket() {

    server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) {
        std::cerr << "Failed to create socket: " << strerror(errno) << std::endl;
        return false;
    }


    int opt = 1;
    if (setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
        std::cerr << "Failed to set socket options: " << strerror(errno) << std::endl;
        close(server_fd);
        server_fd = -1;
        return false;
    }


    struct sockaddr_in address;
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = INADDR_ANY;
    address.sin_port = htons(port);

    if (bind(server_fd, (struct sockaddr*)&address, sizeof(address)) < 0) {
        std::cerr << "Failed to bind to port " << port << ": " << strerror(errno) << std::endl;
        close(server_fd);
        server_fd = -1;
        return false;
    }

    if (listen(server_fd, 128) < 0) {
        std::cerr << "Failed to listen: " << strerror(errno) << std::endl;
        close(server_fd);
        server_fd = -1;
        return false;
    }

    if (port == 0) {
        struct sockaddr_in bound;
        socklen_t len = sizeof(bound);
        if (getsockname(server_fd, (struct sockaddr*)&bound, &len) == 0) {
            port = ntohs(bound.sin_port);
        }
    }

    return true;
}


void HttpServer::acceptLoop() {
    while (running) {
        struct sockaddr_in client_addr;
        socklen_t client_len = sizeof(client_addr);


        int client_fd = accept(server_fd, (struct sockaddr*)&client_addr, &client_len);

        if (client_fd < 0) {
            if (running) {
                std::cerr << "Accept failed: " << strerror(errno) << std::endl;
            }
            continue;
        }

        if (active_clients.load() >= http_limits::kMaxConcurrentClients) {
            sendSimpleResponse(client_fd, 503, "Service Unavailable");
            close(client_fd);
            continue;
        }

        active_clients.fetch_add(1);
        std::thread client_thread(&HttpServer::handleClient, this, client_fd);
        client_thread.detach();
    }
}


void HttpServer::handleClient(int client_fd) {
    processClient(client_fd);
    close(client_fd);

    active_clients.fetch_sub(1);
    {
        std::lock_guard<std::mutex> lock(drain_mutex);
    }
    drain_cv.notify_all();
}


void HttpServer::processClient(int client_fd) {
    struct timeval tv;
    tv.tv_sec = http_limits::kRecvTimeoutSec;
    tv.tv_usec = 0;
    setsockopt(client_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(client_fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    auto deadline = std::chrono::steady_clock::now()
                    + std::chrono::seconds(http_limits::kRequestDeadlineSec);
    auto expired = [&deadline] { return std::chrono::steady_clock::now() > deadline; };

    char buffer[8192];
    std::string data;

    // Phase 1: read until the blank line that ends the headers
    size_t header_end;
    while ((header_end = data.find("\r\n\r\n")) == std::string::npos) {
        if (data.size() > http_limits::kMaxHeaderBytes) {
            sendSimpleResponse(client_fd, 431, "Request Header Fields Too Large");
            return;
        }
        if (expired()) return;
        ssize_t n = recv(client_fd, buffer, sizeof(buffer), 0);
        if (n <= 0) return;  // peer closed, error, or idle timeout
        data.append(buffer, static_cast<size_t>(n));
    }

    std::string header_block = data.substr(0, header_end);

    // Content-Length is the only body framing honored; chunked requests
    // are refused rather than half-implemented (request smuggling risk).
    std::string te_value;
    if (findHeaderValue(header_block, "transfer-encoding", te_value)) {
        sendSimpleResponse(client_fd, 501, "Not Implemented");
        return;
    }

    size_t content_length = 0;
    std::string cl_value;
    if (findHeaderValue(header_block, "content-length", cl_value)) {
        if (!parseContentLength(cl_value, content_length)) {
            sendSimpleResponse(client_fd, 400, "Bad Request");
            return;
        }
        if (content_length > http_limits::kMaxBodyBytes) {
            sendSimpleResponse(client_fd, 413, "Payload Too Large");
            return;
        }
    }

    // Phase 2: read exactly Content-Length body bytes
    const size_t total_size = header_end + 4 + content_length;
    while (data.size() < total_size) {
        if (expired()) return;
        ssize_t n = recv(client_fd, buffer, sizeof(buffer), 0);
        if (n <= 0) return;
        data.append(buffer, static_cast<size_t>(n));
    }
    data.resize(total_size);  // discard any pipelined extra bytes

    if (!request_handler) {
        sendSimpleResponse(client_fd, 503, "Service Unavailable");
        return;
    }

    HttpRequest request;
    if (!request_handler->parseRequest(data, request)) {
        sendSimpleResponse(client_fd, 400, "Bad Request");
        return;
    }

    HttpResponse response = request_handler->handleRequest(request);
    response.headers["Connection"] = "close";
    sendAll(client_fd, response.toString());
}
