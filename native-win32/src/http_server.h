#pragma once

#include "integration_manager.h"
#include "task_store.h"

#include <atomic>
#include <cstdint>
#include <string>
#include <thread>

class HttpServer final {
public:
    explicit HttpServer(TaskStore &store, IntegrationManager &integrations);
    ~HttpServer();

    HttpServer(const HttpServer &) = delete;
    HttpServer &operator=(const HttpServer &) = delete;

    bool start(unsigned short port = 17891);
    void stop();
    bool running() const { return running_.load(); }
    unsigned short port() const { return port_; }

private:
    void run();
    void handleClient(std::uintptr_t socket);
    std::string route(const std::string &method, const std::string &target,
                      const std::string &body, int &status, std::string &statusText);

    TaskStore &store_;
    IntegrationManager &integrations_;
    std::atomic<bool> stopping_{false};
    std::atomic<bool> running_{false};
    std::thread worker_;
    std::uintptr_t listener_ = 0;
    unsigned short port_ = 0;
};
