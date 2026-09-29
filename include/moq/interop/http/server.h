#pragma once

#include "moq/interop/app/version.h"
#include "moq/interop/app/native_run_manager.h"
#include "moq/interop/requirements/catalog.h"
#include "moq/interop/storage/run_store.h"

#include <cstdint>
#include <memory>
#include <string>

namespace moq::interop::http {

struct ServerConfig {
    std::string bind_address = "127.0.0.1";
    std::uint16_t port = 8080;
};

struct ApiError {
    int status;
    std::string code;
    std::string message;
};

class HttpServer {
public:
    HttpServer(std::shared_ptr<const requirements::RequirementCatalog> draft18,
               std::shared_ptr<const requirements::RequirementCatalog> draft21,
               std::shared_ptr<storage::RunStore> store, app::BuildInfo build,
               ServerConfig config = {},
               std::shared_ptr<app::NativeRunManager> runs = nullptr);
    ~HttpServer();

    HttpServer(const HttpServer&) = delete;
    HttpServer& operator=(const HttpServer&) = delete;

    bool start();
    void stop();
    bool running() const;
    std::uint16_t port() const;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace moq::interop::http
