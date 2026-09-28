#include "moq/interop/app/version.h"
#include "moq/interop/http/server.h"
#include "moq/interop/requirements/draft_source.h"
#include "moq/interop/storage/run_store.h"

#include <charconv>
#include <chrono>
#include <csignal>
#include <filesystem>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>

namespace {

volatile std::sig_atomic_t keep_running = 1;

void request_stop(int) { keep_running = 0; }

struct Options {
    moq::interop::http::ServerConfig server;
    std::filesystem::path database = "interop-runs.sqlite3";
    std::filesystem::path docs = std::filesystem::path(MOQ_INTEROP_PROJECT_SOURCE_DIR) / "docs";
    std::filesystem::path requirements =
        std::filesystem::path(MOQ_INTEROP_PROJECT_SOURCE_DIR) / "requirements";
};

void usage(std::ostream& output) {
    output << "Usage: moq-interop-runner [options]\n"
              "  --bind ADDRESS          HTTP bind address (default 127.0.0.1)\n"
              "  --port PORT             HTTP port (default 8080)\n"
              "  --database PATH         SQLite database path\n"
              "  --docs PATH             checked-in draft text directory\n"
              "  --requirements PATH     requirement catalog directory\n"
              "  --version               print build identity\n"
              "  --help                  show this help\n";
}

std::uint16_t parse_port(std::string_view value) {
    unsigned port = 0;
    const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), port);
    if (error != std::errc{} || end != value.data() + value.size() || port == 0 || port > 65535) {
        throw std::invalid_argument("port must be an integer between 1 and 65535");
    }
    return static_cast<std::uint16_t>(port);
}

Options parse_options(int argc, char* argv[]) {
    Options options;
    for (int index = 1; index < argc; ++index) {
        const std::string_view argument(argv[index]);
        auto value = [&](std::string_view option) -> std::string_view {
            if (++index >= argc) throw std::invalid_argument(std::string(option) + " requires a value");
            return argv[index];
        };
        if (argument == "--bind") options.server.bind_address = value(argument);
        else if (argument == "--port") options.server.port = parse_port(value(argument));
        else if (argument == "--database") options.database = value(argument);
        else if (argument == "--docs") options.docs = value(argument);
        else if (argument == "--requirements") options.requirements = value(argument);
        else throw std::invalid_argument("unknown option: " + std::string(argument));
    }
    return options;
}

}  // namespace

int main(int argc, char* argv[]) {
    if (argc == 2 && std::string_view(argv[1]) == "--version") {
        const moq::interop::app::BuildInfo info = moq::interop::app::build_info();
        std::cout << "moq-interop-runner " << info.version << "\n"
                  << "source: " << info.source_revision << '\n';
        for (const auto& [name, revision] : info.dependencies) {
            std::cout << name << ": " << revision << '\n';
        }
        return 0;
    }
    if (argc == 2 && std::string_view(argv[1]) == "--help") {
        usage(std::cout);
        return 0;
    }

    try {
        const auto options = parse_options(argc, argv);
        const auto build = moq::interop::app::build_info();
        const auto digest_file = options.requirements / "draft-digests.json";
        const auto source18 = moq::interop::requirements::load_draft_source(18, options.docs, digest_file);
        const auto source21 = moq::interop::requirements::load_draft_source(21, options.docs, digest_file);
        auto draft18 = std::make_shared<const moq::interop::requirements::RequirementCatalog>(
            moq::interop::requirements::RequirementCatalog::load(
                source18, options.requirements / "draft18.json"));
        auto draft21 = std::make_shared<const moq::interop::requirements::RequirementCatalog>(
            moq::interop::requirements::RequirementCatalog::load(
                source21, options.requirements / "draft21.json"));
        auto store = std::make_shared<moq::interop::storage::SqliteRunStore>(options.database, build);
        moq::interop::http::HttpServer server(draft18, draft21, store, build, options.server);
        if (!server.start()) throw std::runtime_error("could not bind the HTTP listener");

        std::signal(SIGINT, request_stop);
        std::signal(SIGTERM, request_stop);
        std::cout << "HTTP service listening on " << options.server.bind_address << ':'
                  << server.port() << '\n';
        while (keep_running && server.running()) std::this_thread::sleep_for(std::chrono::milliseconds(100));
        server.stop();
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "moq-interop-runner: " << error.what() << '\n';
        usage(std::cerr);
        return 2;
    }
}
