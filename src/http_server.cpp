#include "../include/http_server.h"
#include "../include/request_handler.h"
#include <iostream>
#include <cstring>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <sstream>


HttpServer::HttpServer(int port) : serverr_fd(-1), port(port), running(false) {}


HttpServer::~HttpServer() {
    stop();
    if (serverr_fd != -1) {
        close(serverr_fd);
    }
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
    if (serverr_fd != -1) {
        shutdown(serverr_fd, SHUT_RDWR);
        close(serverr_fd);
        serverr_fd = -1;
    }

    // Wait for accept thread to finish
    if (accept_thread.joinable()) {
        accept_thread.join();
    }
}


bool HttpServer::setupSocket() {

    serverr_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (serverr_fd < 0) {
        std::cerr << "Failed to create socket: " << strerror(errno) << std::endl;
        return false;
    }


    int opt = 1;
    if (setsockopt(serverr_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
        std::cerr << "Failed to set socket options: " << strerror(errno) << std::endl;
        return false;
    }


    struct sockaddr_in address;
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = INADDR_ANY;
    address.sin_port = htons(port);

    if (bind(serverr_fd, (struct sockaddr*)&address, sizeof(address)) < 0) {
        std::cerr << "Failed to bind to port " << port << ": " << strerror(errno) << std::endl;
        close(serverr_fd);
        return false;
    }

    if (listen(serverr_fd, 128) < 0) {
        std::cerr << "Failed to listen: " << strerror(errno) << std::endl;
        close(serverr_fd);
        return false;
    }

    return true;
}


void HttpServer::acceptLoop() {
    while (running) {
        struct sockaddr_in client_addr;
        socklen_t client_len = sizeof(client_addr);


        int client_fd = accept(serverr_fd, (struct sockaddr*)&client_addr, &client_len);

        if (client_fd < 0) {
            if (running) {
                std::cerr << "Accept failed: " << strerror(errno) << std::endl;
            }
            continue;
        }


        std::thread client_thread(&HttpServer::handleClient, this, client_fd);
        client_thread.detach();
    }
}


void HttpServer::handleClient(int client_fd) {
    // Read request
    char buffer[8192] = {0};
    ssize_t bytes_read = recv(client_fd, buffer, sizeof(buffer) - 1, 0);

    if (bytes_read <= 0) {
        close(client_fd);
        return;
    }


    if (request_handler) {
        HttpRequest request;
        std::string raw_request(buffer, bytes_read);

        if (request_handler->parseRequest(raw_request, request)) {
            HttpResponse response = request_handler->handleRequest(request);


            std::string response_str = response.toString();
            send(client_fd, response_str.c_str(), response_str.length(), 0);
        } else {

            std::string error_response = "HTTP/1.1 400 Bad Request\r\n"
                                       "Content-Length: 11\r\n"
                                       "\r\n"
                                       "Bad Request";
            send(client_fd, error_response.c_str(), error_response.length(), 0);
        }
    }

    close(client_fd);
}
