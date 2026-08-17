#ifndef REQUEST_HANDLER_H
#define REQUEST_HANDLER_H

#include <string>
#include <unordered_map>
#include <vector>
#include <functional>

enum class HttpMethod {
    GET,
    POST,
    PUT,
    PATCH,
    DELETE,
    HEAD,
    OPTIONS,
    UNKNOWN
};

struct HttpRequest {
HttpMethod method = HttpMethod::UNKNOWN;
std::string path;
std::string version;
std::unordered_map<std::string, std::string> headers;
std::string body;
std::unordered_map<std::string, std::string> query_params;
};

struct HttpResponse {
    int status_code = 200;
    std::string status_text = "OK";
    std::unordered_map<std::string, std::string> headers;
    std::string body;

    std::string toString() const;
};

class RequestHandler {
    public:

    using RouteHandler = std::function<HttpResponse(const HttpRequest&)>;
    RequestHandler();
    ~RequestHandler ()= default;

    bool parseRequest(const std::string& raw_data, HttpRequest& request);
    HttpResponse handleRequest(const HttpRequest& request);
    // Returns false if (method, path) is already registered; the existing
    // handler is kept. Silently overwriting is the n8n path-collision bug
    // documented in docs/prior-art.md — the Phase 1 config loader relies
    // on this rejection to refuse duplicate endpoints at startup.
    bool registerRoute(const std::string& path, HttpMethod method, RouteHandler handler);

    private:

    std::unordered_map<std::string, RouteHandler> routes;


    HttpMethod parseMethod (const std::string& method_str);
    void parseHeaders(const std::string& headers_section, HttpRequest& request);
    void parseQueryParams(std::string& path, HttpRequest& request);
    std::string buildRouteKey(HttpMethod method, const std::string& path);


    HttpResponse handleNotFound();
    HttpResponse handleMethodNotAllowed();
    HttpResponse handleBadRequest();
};


#endif
