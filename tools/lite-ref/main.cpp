// moq-interop-lite-ref-publisher: the conforming moq-lite-06 reference publisher (L2c). It dials the runner, serves the
// fixture broadcast and, with --defect, departs from the draft in exactly one named way (see --help).
//
// Exit codes: 0 the session ended (the runner closed it, or SIGINT/SIGTERM); 1 the dial or the handshake failed;
// 2 a usage error. One JSON line on standard output when it ends; diagnostics on standard error.
#include "lite_ref_options.h"
#include "lite_ref_session.h"

#include <nlohmann/json.hpp>

#include <chrono>
#include <csignal>
#include <cstdio>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace {

volatile std::sig_atomic_t g_stop = 0;
void on_signal(int) { g_stop = 1; }

}  // namespace

int main(int argc, char** argv) {
    using namespace moq::interop::lite_ref;
    std::vector<std::string_view> args(argv + 1, argv + argc);
    const auto parsed = parse_options(args);
    if (!parsed.options) {
        std::fprintf(stderr, "moq-interop-lite-ref-publisher: %s\n%s", parsed.error.c_str(), usage().c_str());
        return 2;
    }
    if (parsed.options->help) {
        std::fputs(usage().c_str(), stdout);
        return 0;
    }
    std::signal(SIGINT, on_signal);
    std::signal(SIGTERM, on_signal);

    std::string error;
    auto session = RefSession::dial(*parsed.options, error);
    if (!session) {
        std::fprintf(stderr, "moq-interop-lite-ref-publisher: %s\n", error.c_str());
        return 1;
    }
    using Clock = std::chrono::steady_clock;
    const auto handshake_deadline = Clock::now() + std::chrono::seconds(10);
    bool was_established = false;
    while (!g_stop) {
        if (!session->step()) break;
        if (session->established()) was_established = true;
        else if (Clock::now() > handshake_deadline) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    const auto summary = session->summary();
    nlohmann::json line = {{"transport", summary.transport},
                           {"defect", summary.defect},
                           {"groups_sent", summary.groups_sent},
                           {"datagrams_sent", summary.datagrams_sent},
                           {"close_reason", summary.close_reason}};
    std::printf("%s\n", line.dump().c_str());
    if (!was_established) {
        std::fprintf(stderr, "moq-interop-lite-ref-publisher: the session was never established\n");
        return 1;
    }
    return 0;
}
