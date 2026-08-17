#include "../include/request_handler.h"
#include <sstream>
#include <algorithm>
#include <cctype>


RequestHandler::RequestHandler() {

}


bool RequestHandler::parseRequest(const std::string& raw_data, HttpRequest& request) {
    // Headers end at the first blank line; everything after it is the
    // body, taken as exact bytes — bodies may be binary and must never
    // pass through line-oriented parsing.
    size_t header_end = raw_data.find("\r\n\r\n");
    if (header_end == std::string::npos) {
        return false;
    }

    size_t line_end = raw_data.find("\r\n");
    std::istringstream request_line(raw_data.substr(0, line_end));
    std::string method_str, path_with_query, version, extra;

    if (!(request_line >> method_str >> path_with_query >> version)) {
        return false;
    }
    if (request_line >> extra) {
        return false;
    }

    request.method = parseMethod(method_str);
    request.version = version;


    request.path = path_with_query;
    parseQueryParams(request.path, request);

    if (header_end > line_end) {
        parseHeaders(raw_data.substr(line_end + 2, header_end - (line_end + 2)), request);
    }

    request.body = raw_data.substr(header_end + 4);

    return true;
}


HttpResponse RequestHandler::handleRequest(const HttpRequest& request) {

    std::string route_key = buildRouteKey(request.method, request.path);


    auto it = routes.find(route_key);
    if (it != routes.end()) {
        return it->second(request);
    }


    for (const auto& [key, handler] : routes) {
        size_t colon_pos = key.find(':');
        if (colon_pos != std::string::npos) {
            std::string registered_path = key.substr(colon_pos + 1);
            if (registered_path == request.path) {
                return handleMethodNotAllowed();
            }
        }
    }

    return handleNotFound();
}


bool RequestHandler::registerRoute(const std::string& path, HttpMethod method, RouteHandler handler) {
    std::string route_key = buildRouteKey(method, path);
    // emplace leaves the incumbent in place on collision and tells us.
    return routes.emplace(route_key, std::move(handler)).second;
}


HttpMethod RequestHandler::parseMethod(const std::string& method_str) {
    std::string upper_method = method_str;
    std::transform(upper_method.begin(), upper_method.end(), upper_method.begin(), ::toupper);

    if (upper_method == "GET") return HttpMethod::GET;
    if (upper_method == "POST") return HttpMethod::POST;
    if (upper_method == "PUT") return HttpMethod::PUT;
    if (upper_method == "PATCH") return HttpMethod::PATCH;
    if (upper_method == "DELETE") return HttpMethod::DELETE;
    if (upper_method == "HEAD") return HttpMethod::HEAD;
    if (upper_method == "OPTIONS") return HttpMethod::OPTIONS;

    return HttpMethod::UNKNOWN;
}


void RequestHandler::parseHeaders(const std::string& headers_section, HttpRequest& request) {
    size_t pos = 0;
    while (pos < headers_section.size()) {
        size_t eol = headers_section.find("\r\n", pos);
        std::string line = headers_section.substr(
            pos, eol == std::string::npos ? std::string::npos : eol - pos);
        pos = (eol == std::string::npos) ? headers_section.size() : eol + 2;

        if (line.empty()) continue;

        size_t colon_pos = line.find(':');
        if (colon_pos != std::string::npos) {
            std::string key = line.substr(0, colon_pos);
            std::string value = line.substr(colon_pos + 1);

            // Header names are case-insensitive (RFC 7230) — store lowercase
            std::transform(key.begin(), key.end(), key.begin(),
                           [](unsigned char c) { return std::tolower(c); });

            size_t first = value.find_first_not_of(" \t");
            size_t last = value.find_last_not_of(" \t");
            value = (first == std::string::npos) ? "" : value.substr(first, last - first + 1);

            request.headers[key] = value;
        }
    }
}


void RequestHandler::parseQueryParams(std::string& path, HttpRequest& request) {
    size_t query_pos = path.find('?');

    if (query_pos != std::string::npos) {
        std::string query_string = path.substr(query_pos + 1);
        path = path.substr(0, query_pos);  // Remove query from path


        std::istringstream query_stream(query_string);
        std::string pair;

        while (std::getline(query_stream, pair, '&')) {
            size_t equals_pos = pair.find('=');
            if (equals_pos != std::string::npos) {
                std::string key = pair.substr(0, equals_pos);
                std::string value = pair.substr(equals_pos + 1);
                request.query_params[key] = value;
            }
        }
    }
}


std::string RequestHandler::buildRouteKey(HttpMethod method, const std::string& path) {
    std::string method_str;

    switch (method) {
        case HttpMethod::GET: method_str = "GET"; break;
        case HttpMethod::POST: method_str = "POST"; break;
        case HttpMethod::PUT: method_str = "PUT"; break;
        case HttpMethod::PATCH: method_str = "PATCH"; break;
        case HttpMethod::DELETE: method_str = "DELETE"; break;
        case HttpMethod::HEAD: method_str = "HEAD"; break;
        case HttpMethod::OPTIONS: method_str = "OPTIONS"; break;
        default: method_str = "UNKNOWN"; break;
    }

    return method_str + ":" + path;
}


HttpResponse RequestHandler::handleNotFound() {
    HttpResponse response;
    response.status_code = 404;
    response.status_text = "Not Found";
    response.body = "404 - Page not found";
    response.headers["Content-Type"] = "text/plain";
    response.headers["Content-Length"] = std::to_string(response.body.length());
    return response;
}

HttpResponse RequestHandler::handleMethodNotAllowed() {
    HttpResponse response;
    response.status_code = 405;
    response.status_text = "Method Not Allowed";
    response.body = "405 - Method not allowed";
    response.headers["Content-Type"] = "text/plain";
    response.headers["Content-Length"] = std::to_string(response.body.length());
    return response;
}

HttpResponse RequestHandler::handleBadRequest() {
    HttpResponse response;
    response.status_code = 400;
    response.status_text = "Bad Request";
    response.body = "400 - Bad request";
    response.headers["Content-Type"] = "text/plain";
    response.headers["Content-Length"] = std::to_string(response.body.length());
    return response;
}


std::string HttpResponse::toString() const {
    std::ostringstream response_stream;


    response_stream << "HTTP/1.1 " << status_code << " " << status_text << "\r\n";


    for (const auto& [key, value] : headers) {
        response_stream << key << ": " << value << "\r\n";
    }


    if (headers.find("Content-Length") == headers.end()) {
        response_stream << "Content-Length: " << body.length() << "\r\n";
    }


    response_stream << "\r\n";


    response_stream << body;

    return response_stream.str();
}
