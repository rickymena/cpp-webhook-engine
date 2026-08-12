#ifndef MAIN_CPP_COMPONENTS
#define MAIN_CPP_COMPONENTS
#include <iomanip>
#include <fstream>
#include <iostream>
#include <csignal>
#include <memory>
#include <chrono>
#include <ctime>
#include "../include/http_server.h"
#include "../include/request_handler.h"
#include <sys/stat.h>


std::unique_ptr<HttpServer> g_server;

void signalHandler(int signal);
void logMessage(const std::string& level, const std::string& message);
void logWebhook(const HttpRequest& request);


struct ServerConfig{
    int port = 8080;
    std::string log_file = "./logs/webhook_server.log";
    bool verbose = false;
    bool save_payloads = false;
    std::string payload_dir = "./logs";
};

ServerConfig g_config;
void saveWebhookPayload(const HttpRequest& request, const ServerConfig& config);
ServerConfig loadConfiguration(int argc, char* argv[]);

HttpResponse handleGenericWebhook(const HttpRequest& request);
HttpResponse handleHealthCheck(const HttpRequest& request);

#endif



int main(int argc, char* argv[]){
    g_config = loadConfiguration(argc, argv);


    std::signal(SIGINT, signalHandler);
    std::signal(SIGTERM, signalHandler);


    g_server = std::make_unique<HttpServer>(g_config.port);
    auto handler = std::make_shared<RequestHandler>();





    handler->registerRoute("/webhook", HttpMethod::POST, handleGenericWebhook );
    handler->registerRoute("/health", HttpMethod::GET, handleHealthCheck );

    g_server->setRequestHandler(handler);
    if (!g_server->start()){
        std::cerr << "Failed to start server on port" << g_config.port <<std::endl;
        return 1;
    }

    logMessage("INFO","Server started on port " + std::to_string(g_config.port));

    while (g_server->isRunning()){
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }

    logMessage("INFO","Server shutdown complete");

    return 0;
}

void signalHandler(int signal){
    if (signal == SIGINT || signal == SIGTERM){
        logMessage("INFO", "Shutdown signal received");
        if (g_server){
            g_server->stop();
        }
    }
}

void logMessage(const std::string& level, const std::string& message){
    auto now = std::chrono::system_clock::now();
    auto time_t = std::chrono::system_clock::to_time_t(now);

    std::cout << "[" << std::put_time(std::localtime(&time_t), "%Y-%m-%d %H:%M:%S")
    << "] [" << level << "] " << message << std::endl;
}

void logWebhook (const HttpRequest& request){
    logMessage("INFO", "Webhook received at" + request.path);
    if (!request.body.empty()){
        logMessage("DEBUG", "Payload size: " + std::to_string(request.body.size()));
    }

}


void saveWebhookPayload(const HttpRequest& request, const ServerConfig& config) {
      if (!config.save_payloads) return;

      // Create directory if it doesn't exist
      mkdir(config.payload_dir.c_str(), 0755);

      // Generate unique filename with timestamp
      auto now = std::chrono::system_clock::now();
      auto time_t = std::chrono::system_clock::to_time_t(now);
      auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
          now.time_since_epoch()) % 1000;

      std::stringstream filename;
      filename << config.payload_dir << "/webhook_"
               << std::put_time(std::localtime(&time_t), "%Y%m%d_%H%M%S")
               << "_" << ms.count() << ".json";

      // Write to file
      std::ofstream file(filename.str());
      file << request.body;
      file.close();

      logMessage("INFO", "Saved payload to " + filename.str());
  }

ServerConfig loadConfiguration(int argc, char* argv[]) {
    ServerConfig config;

    for (int i = 1; i < argc; i++) {
        std::string arg = argv[i];
        if (arg == "-p" || arg == "--port") {
            if (i + 1 < argc) {
                config.port = std::stoi(argv[++i]);
            }
        } else if (arg == "-v" || arg == "--verbose") {
            config.verbose = true;
        } else if (arg == "--save-payloads") {
            config.save_payloads = true;
        }
    }

    return config;
}

HttpResponse handleGenericWebhook(const HttpRequest& request) {
    HttpResponse response;


    logWebhook(request);
    saveWebhookPayload(request, g_config);

    if (request.body.empty()) {
        response.status_code = 400;
        response.status_text = "Bad Request";
        response.body = "Empty payload";
        return response;
    }


    response.status_code = 200;
    response.status_text = "OK";
    response.body = "{\"status\":\"received\"}";
    response.headers["Content-Type"] = "application/json";

    return response;
}

HttpResponse handleHealthCheck(const HttpRequest& /* request */) {
    HttpResponse response;
    response.status_code = 200;
    response.status_text = "OK";
    response.headers["Content-Type"] = "application/json";
    response.body = "{\"status\":\"healthy\"}";
    return response;
}
