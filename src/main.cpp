#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include "../include/http_server.h"
#include "../include/request_handler.h"
#include <sys/stat.h>


std::unique_ptr<HttpServer> g_server;
// Signal handlers may only touch sig_atomic_t; the main loop does the
// actual shutdown work.
volatile std::sig_atomic_t g_shutdown_requested = 0;
std::mutex g_log_mutex;

void signalHandler(int signal);
void logMessage(const std::string& level, const std::string& message);
void logWebhook(const HttpRequest& request);


struct ServerConfig{
    int port = 8080;
    std::string log_file = "./logs/webhook_server.log";
    bool verbose = false;
    bool save_payloads = false;
    std::string payload_dir = "./logs/payloads";
};

ServerConfig g_config;
void saveWebhookPayload(const HttpRequest& request, const ServerConfig& config);
ServerConfig loadConfiguration(int argc, char* argv[]);
bool ensureDirectories(const std::string& path, mode_t mode);

HttpResponse handleGenericWebhook(const HttpRequest& request);
HttpResponse handleHealthCheck(const HttpRequest& request);



int main(int argc, char* argv[]){
    g_config = loadConfiguration(argc, argv);


    std::signal(SIGINT, signalHandler);
    std::signal(SIGTERM, signalHandler);
    std::signal(SIGPIPE, SIG_IGN);  // a hung-up client must not kill the process


    size_t slash = g_config.log_file.rfind('/');
    if (slash != std::string::npos && slash > 0) {
        std::string log_dir = g_config.log_file.substr(0, slash);
        if (!ensureDirectories(log_dir, 0755)) {
            std::cerr << "Failed to create log directory " << log_dir
                      << ": " << strerror(errno) << std::endl;
            return 1;
        }
    }


    g_server = std::make_unique<HttpServer>(g_config.port);
    auto handler = std::make_shared<RequestHandler>();


    handler->registerRoute("/webhook", HttpMethod::POST, handleGenericWebhook );
    handler->registerRoute("/health", HttpMethod::GET, handleHealthCheck );

    g_server->setRequestHandler(handler);
    if (!g_server->start()){
        std::cerr << "Failed to start server on port " << g_config.port <<std::endl;
        return 1;
    }

    logMessage("INFO","Server started on port " + std::to_string(g_server->getPort()));
    logMessage("DEBUG","Verbose logging enabled");

    while (!g_shutdown_requested && g_server->isRunning()){
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }

    logMessage("INFO","Shutdown signal received, draining in-flight requests");
    g_server->stop();
    logMessage("INFO","Server shutdown complete");

    return 0;
}

void signalHandler(int signal){
    if (signal == SIGINT || signal == SIGTERM){
        g_shutdown_requested = 1;
    }
}

bool ensureDirectories(const std::string& path, mode_t mode){
    size_t pos = 0;
    while (pos <= path.size()) {
        size_t next = path.find('/', pos);
        std::string partial = path.substr(0, next == std::string::npos ? path.size() : next);
        if (!partial.empty() && partial != ".") {
            if (mkdir(partial.c_str(), mode) != 0 && errno != EEXIST) {
                return false;
            }
        }
        if (next == std::string::npos) break;
        pos = next + 1;
    }
    return true;
}

void logMessage(const std::string& level, const std::string& message){
    if (level == "DEBUG" && !g_config.verbose) return;

    auto now = std::chrono::system_clock::now();
    auto now_time = std::chrono::system_clock::to_time_t(now);
    std::tm tm_buf;
    localtime_r(&now_time, &tm_buf);

    std::ostringstream line;
    line << "[" << std::put_time(&tm_buf, "%Y-%m-%d %H:%M:%S")
         << "] [" << level << "] " << message;

    std::lock_guard<std::mutex> lock(g_log_mutex);
    std::cout << line.str() << std::endl;
    std::ofstream log_file(g_config.log_file, std::ios::app);
    if (log_file) {
        log_file << line.str() << "\n";
    }
}

void logWebhook (const HttpRequest& request){
    logMessage("INFO", "Webhook received at " + request.path);
    if (!request.body.empty()){
        logMessage("DEBUG", "Payload size: " + std::to_string(request.body.size()));
    }

}


void saveWebhookPayload(const HttpRequest& request, const ServerConfig& config) {
      if (!config.save_payloads) return;

      // Payloads may hold tokens/PII: 0700 dir, 0600 files (security.md §5)
      if (!ensureDirectories(config.payload_dir, 0700)) {
          logMessage("ERROR", "Failed to create payload directory " + config.payload_dir);
          return;
      }

      // Generate unique filename with timestamp
      auto now = std::chrono::system_clock::now();
      auto now_time = std::chrono::system_clock::to_time_t(now);
      auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
          now.time_since_epoch()) % 1000;
      std::tm tm_buf;
      localtime_r(&now_time, &tm_buf);

      std::string extension = ".bin";
      auto content_type = request.headers.find("content-type");
      if (content_type != request.headers.end()
          && content_type->second.find("json") != std::string::npos) {
          extension = ".json";
      }

      std::stringstream filename;
      filename << config.payload_dir << "/webhook_"
               << std::put_time(&tm_buf, "%Y%m%d_%H%M%S")
               << "_" << ms.count() << extension;

      std::ofstream file(filename.str(), std::ios::binary);
      if (!file) {
          logMessage("ERROR", "Failed to open payload file " + filename.str());
          return;
      }
      file.write(request.body.data(), static_cast<std::streamsize>(request.body.size()));
      file.close();
      chmod(filename.str().c_str(), 0600);

      logMessage("INFO", "Saved payload to " + filename.str());
  }

ServerConfig loadConfiguration(int argc, char* argv[]) {
    ServerConfig config;

    for (int i = 1; i < argc; i++) {
        std::string arg = argv[i];
        if (arg == "-p" || arg == "--port") {
            if (i + 1 >= argc) {
                std::cerr << "Missing value for " << arg << std::endl;
                std::exit(1);
            }
            std::string value = argv[++i];
            try {
                size_t parsed_len = 0;
                int port = std::stoi(value, &parsed_len);
                if (parsed_len != value.size() || port < 1 || port > 65535) {
                    throw std::invalid_argument("out of range");
                }
                config.port = port;
            } catch (const std::exception&) {
                std::cerr << "Invalid port '" << value << "' (expected 1-65535)" << std::endl;
                std::exit(1);
            }
        } else if (arg == "-v" || arg == "--verbose") {
            config.verbose = true;
        } else if (arg == "--save-payloads") {
            config.save_payloads = true;
        } else if (arg == "-h" || arg == "--help") {
            std::cout << "Usage: webhook_server [-p|--port <1-65535>] [-v|--verbose]"
                         " [--save-payloads]" << std::endl;
            std::exit(0);
        } else {
            std::cerr << "Unknown option: " << arg << std::endl;
            std::exit(1);
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
