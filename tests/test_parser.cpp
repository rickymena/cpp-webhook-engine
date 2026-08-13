// Request-parsing tests: plain asserts, no framework.
#include "request_handler.h"
#include <cassert>
#include <iostream>
#include <string>

int main() {
    RequestHandler handler;

    // Simple GET request line + header
    {
        HttpRequest req;
        std::string raw = "GET /health HTTP/1.1\r\nHost: localhost\r\n\r\n";
        assert(handler.parseRequest(raw, req));
        assert(req.method == HttpMethod::GET);
        assert(req.path == "/health");
        assert(req.version == "HTTP/1.1");
        assert(req.headers.at("host") == "localhost");
        assert(req.body.empty());
    }

    // Query parameters split off the path
    {
        HttpRequest req;
        std::string raw = "GET /hooks/deploy?ref=main&dry=1 HTTP/1.1\r\n\r\n";
        assert(handler.parseRequest(raw, req));
        assert(req.path == "/hooks/deploy");
        assert(req.query_params.at("ref") == "main");
        assert(req.query_params.at("dry") == "1");
    }

    // Header names are case-insensitive: stored lowercase, values kept
    {
        HttpRequest req;
        std::string raw =
            "POST /webhook HTTP/1.1\r\n"
            "X-Hub-Signature-256: sha256=abc\r\n"
            "CONTENT-TYPE:   application/json\r\n"
            "\r\n";
        assert(handler.parseRequest(raw, req));
        assert(req.headers.at("x-hub-signature-256") == "sha256=abc");
        assert(req.headers.at("content-type") == "application/json");
    }

    // Binary body preserved byte-for-byte: NUL bytes, high bytes, and a
    // CRLFCRLF sequence *inside* the body must all survive
    {
        HttpRequest req;
        std::string body("\x00\x01\x02\r\n\r\nPNG\xff\x89more", 16);
        std::string raw = "POST /webhook HTTP/1.1\r\nContent-Length: 16\r\n\r\n" + body;
        assert(handler.parseRequest(raw, req));
        assert(req.body.size() == body.size());
        assert(req.body == body);
    }

    // Malformed input rejected
    {
        HttpRequest req;
        assert(!handler.parseRequest("garbage\r\n\r\n", req));
        assert(!handler.parseRequest("no header terminator", req));
        assert(!handler.parseRequest("GET / HTTP/1.1 extra-token\r\n\r\n", req));
        assert(!handler.parseRequest("", req));
    }

    std::cout << "test_parser: all assertions passed" << std::endl;
    return 0;
}
