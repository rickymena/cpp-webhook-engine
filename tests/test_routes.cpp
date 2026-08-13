// Route-matching tests: plain asserts, no framework.
#include "request_handler.h"
#include <cassert>
#include <iostream>
#include <string>

int main() {
    RequestHandler handler;

    handler.registerRoute("/webhook", HttpMethod::POST, [](const HttpRequest&) {
        HttpResponse r;
        r.body = "hook";
        return r;
    });
    handler.registerRoute("/health", HttpMethod::GET, [](const HttpRequest&) {
        HttpResponse r;
        r.body = "healthy";
        return r;
    });

    // Exact match dispatches to the registered handler
    {
        HttpRequest req;
        req.method = HttpMethod::POST;
        req.path = "/webhook";
        HttpResponse resp = handler.handleRequest(req);
        assert(resp.status_code == 200);
        assert(resp.body == "hook");
    }
    {
        HttpRequest req;
        req.method = HttpMethod::GET;
        req.path = "/health";
        assert(handler.handleRequest(req).body == "healthy");
    }

    // Known path, wrong method → 405
    {
        HttpRequest req;
        req.method = HttpMethod::GET;
        req.path = "/webhook";
        assert(handler.handleRequest(req).status_code == 405);
    }

    // Unknown path → 404
    {
        HttpRequest req;
        req.method = HttpMethod::GET;
        req.path = "/nope";
        assert(handler.handleRequest(req).status_code == 404);
    }

    // Parse-then-route: query string must not break matching
    {
        HttpRequest req;
        std::string raw =
            "POST /webhook?src=github HTTP/1.1\r\nContent-Length: 2\r\n\r\nhi";
        assert(handler.parseRequest(raw, req));
        HttpResponse resp = handler.handleRequest(req);
        assert(resp.status_code == 200);
        assert(resp.body == "hook");
    }

    // Response serialization carries status line, headers, and body
    {
        HttpResponse resp;
        resp.status_code = 413;
        resp.status_text = "Payload Too Large";
        resp.body = "too big";
        std::string wire = resp.toString();
        assert(wire.find("HTTP/1.1 413 Payload Too Large\r\n") == 0);
        assert(wire.find("Content-Length: 7\r\n") != std::string::npos);
        assert(wire.find("\r\n\r\ntoo big") != std::string::npos);
    }

    std::cout << "test_routes: all assertions passed" << std::endl;
    return 0;
}
