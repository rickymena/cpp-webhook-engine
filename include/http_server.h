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
