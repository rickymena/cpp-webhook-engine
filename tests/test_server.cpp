// Integration tests against a live HttpServer on a loopback socket:
// fragmented large uploads, oversize rejection, chunked rejection,
// graceful stop. Plain asserts, no framework.
#include "http_server.h"
#include "request_handler.h"
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#include <cassert>
#include <chrono>
#include <csignal>
#include <cstring>
#include <iostream>
#include <memory>
#include <string>
#include <thread>

static int connectTo(int port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    assert(fd >= 0);
    struct sockaddr_in addr;
    std::memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<uint16_t>(port));
    assert(inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr) == 1);
    assert(connect(fd, (struct sockaddr*)&addr, sizeof(addr)) == 0);
    return fd;
}

static void sendRaw(int fd, const std::string& data) {
    size_t off = 0;
    while (off < data.size()) {
        ssize_t n = send(fd, data.data() + off, data.size() - off, MSG_NOSIGNAL);
        if (n <= 0) return;  // server may close early (413/501 paths)
        off += static_cast<size_t>(n);
    }
}

// Read until the server closes the connection (it always does)
static std::string readResponse(int fd) {
    std::string out;
    char buf[4096];
    ssize_t n;
    while ((n = recv(fd, buf, sizeof(buf), 0)) > 0) {
        out.append(buf, static_cast<size_t>(n));
    }
    return out;
}

int main() {
    std::signal(SIGPIPE, SIG_IGN);

    HttpServer server(0);  // ephemeral port
    auto handler = std::make_shared<RequestHandler>();
    handler->registerRoute("/webhook", HttpMethod::POST, [](const HttpRequest& req) {
        HttpResponse r;
        r.headers["Content-Type"] = "text/plain";
        r.body = std::to_string(req.body.size());
        return r;
    });
    server.setRequestHandler(handler);
    assert(server.start());
    int port = server.getPort();
    assert(port > 0);

    // 100 KiB binary body, sent in small fragments with pauses — must be
    // reassembled completely (the old single-recv() code truncated this)
    {
        std::string body(100 * 1024, 'x');
        body[0] = '\0';
        body[1] = '\xff';
        std::string request =
            "POST /webhook HTTP/1.1\r\nHost: test\r\nContent-Length: "
            + std::to_string(body.size()) + "\r\n\r\n" + body;

        int fd = connectTo(port);
        size_t off = 0;
        while (off < request.size()) {
            size_t chunk = std::min<size_t>(7000, request.size() - off);
            sendRaw(fd, request.substr(off, chunk));
            off += chunk;
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        std::string resp = readResponse(fd);
        close(fd);
        assert(resp.find("HTTP/1.1 200 OK") == 0);
        assert(resp.find("\r\n\r\n102400") != std::string::npos);
    }

    // Content-Length above the cap → 413 before any body is read
    {
        int fd = connectTo(port);
        sendRaw(fd, "POST /webhook HTTP/1.1\r\nContent-Length: "
                    + std::to_string(http_limits::kMaxBodyBytes + 1) + "\r\n\r\n");
        std::string resp = readResponse(fd);
        close(fd);
        assert(resp.find("HTTP/1.1 413") == 0);
    }

    // Transfer-Encoding: chunked → 501, case-insensitive header match
    {
        int fd = connectTo(port);
        sendRaw(fd, "POST /webhook HTTP/1.1\r\nTransfer-Encoding: chunked\r\n\r\n");
        std::string resp = readResponse(fd);
        close(fd);
        assert(resp.find("HTTP/1.1 501") == 0);
    }
    {
        int fd = connectTo(port);
        sendRaw(fd, "POST /webhook HTTP/1.1\r\nTRANSFER-ENCODING: chunked\r\n\r\n");
        std::string resp = readResponse(fd);
        close(fd);
        assert(resp.find("HTTP/1.1 501") == 0);
    }

    // Garbage Content-Length → 400
    {
        int fd = connectTo(port);
        sendRaw(fd, "POST /webhook HTTP/1.1\r\nContent-Length: banana\r\n\r\n");
        std::string resp = readResponse(fd);
        close(fd);
        assert(resp.find("HTTP/1.1 400") == 0);
    }

    // Unknown route → 404
    {
        int fd = connectTo(port);
        sendRaw(fd, "GET /nope HTTP/1.1\r\n\r\n");
        std::string resp = readResponse(fd);
        close(fd);
        assert(resp.find("HTTP/1.1 404") == 0);
    }

    // Two Content-Length headers → 400, never "pick one and proceed".
    // Framing on the first value while a front proxy frames on the
    // second is CL.CL request smuggling (security.md, RFC 7230 §3.3.3).
    {
        int fd = connectTo(port);
        sendRaw(fd, "POST /webhook HTTP/1.1\r\nHost: x\r\n"
                    "Content-Length: 5\r\nContent-Length: 44\r\n\r\nhello");
        std::string resp = readResponse(fd);
        close(fd);
        assert(resp.find("HTTP/1.1 400") == 0);
    }

    // Same, with the duplicate spelled in a different case
    {
        int fd = connectTo(port);
        sendRaw(fd, "POST /webhook HTTP/1.1\r\nContent-Length: 2\r\n"
                    "CONTENT-LENGTH: 2\r\n\r\nhi");
        std::string resp = readResponse(fd);
        close(fd);
        assert(resp.find("HTTP/1.1 400") == 0);
    }

    // List form "5, 44" is the same attack spelled differently → 400
    {
        int fd = connectTo(port);
        sendRaw(fd, "POST /webhook HTTP/1.1\r\nContent-Length: 5, 44\r\n\r\nhello");
        std::string resp = readResponse(fd);
        close(fd);
        assert(resp.find("HTTP/1.1 400") == 0);
    }

    // A single Content-Length still works (the fix must not over-reject)
    {
        int fd = connectTo(port);
        sendRaw(fd, "POST /webhook HTTP/1.1\r\nContent-Length: 5\r\n\r\nhello");
        std::string resp = readResponse(fd);
        close(fd);
        assert(resp.find("HTTP/1.1 200 OK") == 0);
    }

    // An absolute-form target carries a colon; it must not be scanned as
    // a header field (the header block now starts after the request line)
    {
        int fd = connectTo(port);
        sendRaw(fd, "POST http://127.0.0.1:8080/webhook HTTP/1.1\r\n"
                    "Content-Length: 5\r\n\r\nhello");
        std::string resp = readResponse(fd);
        close(fd);
        assert(resp.find("HTTP/1.1 404") == 0);  // parsed fine, just no such route
    }

    // Graceful stop: an in-flight request finishes before stop() returns
    {
        std::string body(10 * 1024, 'y');
        std::string request =
            "POST /webhook HTTP/1.1\r\nContent-Length: "
            + std::to_string(body.size()) + "\r\n\r\n" + body;

        int fd = connectTo(port);
        sendRaw(fd, request.substr(0, 100));
        std::thread finisher([&] {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            sendRaw(fd, request.substr(100));
        });
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        server.stop();  // must wait for the in-flight request to drain
        finisher.join();
        std::string resp = readResponse(fd);
        close(fd);
        assert(resp.find("HTTP/1.1 200 OK") == 0);
        assert(resp.find("\r\n\r\n10240") != std::string::npos);
        assert(!server.isRunning());
    }

    std::cout << "test_server: all assertions passed" << std::endl;
    return 0;
}
