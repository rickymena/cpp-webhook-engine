#ifndef HTTP_SERVER_H
#define HTTP_SERVER_H

#include "request_handler.h"
#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <memory>
#include <mutex>
#include <thread>

// Hard limits enforced before/while reading a request off the wire
// (see docs/security.md "Parser robustness"). Phase 1 makes the body
// cap per-endpoint config; until then these are compile-time.
namespace http_limits {
    constexpr std::size_t kMaxHeaderBytes = 16 * 1024;
    constexpr std::size_t kMaxBodyBytes = 1024 * 1024;   // 1 MiB default cap
    constexpr int kRecvTimeoutSec = 10;      // per-recv() idle timeout
    constexpr int kRequestDeadlineSec = 30;  // whole-request deadline (slowloris)
    constexpr int kMaxConcurrentClients = 64;
    constexpr int kDrainTimeoutSec = 30;     // graceful-shutdown wait for in-flight clients
}

// Body framing, decided purely from the bytes received so far — no
// sockets, no I/O. Kept separate from processClient() so the decision
// that smuggling attacks target can be unit-tested and fuzzed directly;
// the CL.CL bug fixed on 2026-08-17 lived here and was unreachable from
// a test before this split.
namespace http_framing {

enum class Result {
    kIncomplete,      // headers not terminated yet — read more
    kOk,              // framing decided; body is content_length bytes
    kBadRequest,      // 400: unparsable or conflicting Content-Length
    kNotImplemented,  // 501: Transfer-Encoding
    kPayloadTooLarge, // 413: Content-Length over the cap
    kHeadersTooLarge  // 431
};

// On kOk, header_end_out is the offset of the terminating CRLFCRLF and
// content_length_out the declared body size; the full request occupies
// header_end_out + 4 + content_length_out bytes.
Result analyze(const std::string& data, std::size_t& header_end_out,
               std::size_t& content_length_out);

} // namespace http_framing

class HttpServer {
    public:
    explicit HttpServer(int port);

    ~HttpServer();

    bool start();

    void stop();

    bool isRunning() const { return running.load(); }

    // Actual bound port (useful when constructed with port 0)
    int getPort() const { return port; }

    void setRequestHandler(std::shared_ptr<RequestHandler> handler){
        request_handler = handler;
    }
    private:
    int server_fd;
    int port;
    std::atomic<bool> running;
    std::atomic<int> active_clients;
    std::thread accept_thread;
    std::shared_ptr<RequestHandler> request_handler;
    std::mutex drain_mutex;
    std::condition_variable drain_cv;

    void acceptLoop();
    void handleClient(int client_fd);
    void processClient(int client_fd);
    bool setupSocket();
};

#endif
