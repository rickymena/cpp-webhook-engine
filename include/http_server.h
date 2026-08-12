#ifndef HTTP_SERVER_H
#define HTTP_SERVER_H

#include "request_handler.h"
#include <string>
#include <thread>
#include <atomic>
#include <memory>
#include <functional>
#include "request_handler.h"
#include <iomanip>

class HttpServer {


    public:
    explicit HttpServer (int port);

    ~HttpServer();

    bool start();

    void stop ();

    bool isRunning () const { return running.load(); }

    void setRequestHandler(std::shared_ptr<RequestHandler> handler){
        request_handler = handler;
    }
    private:
    int serverr_fd;
    int port;
    std::atomic<bool> running;
    std::thread accept_thread;
    std::shared_ptr<RequestHandler>request_handler;


    void acceptLoop();
    void handleClient(int client_fd);
    bool setupSocket();
};

#endif
