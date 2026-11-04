#include "moq/interop/session/draft23_probes.h"

#include "moq/interop/wire/cursor.h"

#include <array>

namespace moq::interop::session {
namespace {

constexpr std::uint64_t kUnexpectedMessage = 0x303;
constexpr std::uint64_t kMalformedMessage = 0x300;
constexpr std::uint64_t kFieldLengthExceeded = 0x401;
constexpr std::uint64_t kInvalidAuthority = 0x19;
constexpr std::uint64_t kInvalidPath = 0x8;
constexpr std::uint64_t kInvalidStreamHeader = 0x200;

constexpr std::array<Draft23Probe, 7> kProbes{{
    // An unassigned message type on a request stream (Section 9).
    {"d23-probe-unknown-control-message", "D23-9-MUST-257", Draft23ProbeChannel::Bidi, kUnexpectedMessage},
    // SUBSCRIBE_OK may not open a bidirectional stream (Section 6.3).
    {"d23-probe-bad-first-bidi-message", "D23-6-3-MUST-137", Draft23ProbeChannel::Bidi, kUnexpectedMessage},
    // PUBLISH_NAMESPACE whose declared length is shorter than the message it needs (Section 9).
    {"d23-probe-length-mismatch", "D23-9-MUST-258", Draft23ProbeChannel::Bidi, kMalformedMessage},
    // A control message longer than the limit the publisher advertised (Section 9).
    {"d23-probe-oversize-control-message", "D23-9-MUST-260", Draft23ProbeChannel::Bidi, kFieldLengthExceeded},
    // AUTHORITY and PATH are client-only options; a server must not send them (Sections 9.1.1 and 9.1.2).
    {"d23-probe-server-authority", "D23-9-1-1-MUST-268", Draft23ProbeChannel::SetupOption, kInvalidAuthority},
    {"d23-probe-server-path", "D23-9-1-2-MUST-272", Draft23ProbeChannel::SetupOption, kInvalidPath},
    // A unidirectional stream type outside Table 2 (Section 6.4.1).
    {"d23-probe-unknown-uni-stream", "D23-6-4-1-MUST-146", Draft23ProbeChannel::Uni, kInvalidStreamHeader},
}};

std::vector<std::byte> bytes_of(std::initializer_list<unsigned> values) {
    std::vector<std::byte> result;
    for (const auto value : values) result.push_back(static_cast<std::byte>(value));
    return result;
}

std::vector<std::byte> text(std::string_view value) {
    const auto view = std::as_bytes(std::span(value));
    return {view.begin(), view.end()};
}

}  // namespace

std::span<const Draft23Probe> draft23_probes() { return kProbes; }

const Draft23Probe* find_draft23_probe(std::string_view scenario_id) {
    for (const auto& probe : kProbes) if (probe.scenario_id == scenario_id) return &probe;
    return nullptr;
}

std::vector<std::byte> draft23_probe_bytes(const Draft23Probe& probe, std::uint64_t peer_message_limit) {
    if (probe.scenario_id == "d23-probe-unknown-control-message") return bytes_of({0x3f, 0});
    if (probe.scenario_id == "d23-probe-bad-first-bidi-message") return bytes_of({4, 2, 7, 0});
    if (probe.scenario_id == "d23-probe-length-mismatch") return bytes_of({6, 1, 0});
    if (probe.scenario_id == "d23-probe-unknown-uni-stream") return bytes_of({0x0a});
    if (probe.scenario_id == "d23-probe-oversize-control-message") {
        wire::ByteWriter writer(16);
        (void)wire::write_vi64(6, writer);  // PUBLISH_NAMESPACE
        (void)wire::write_vi64(peer_message_limit + 1, writer);
        const auto encoded = writer.bytes();
        return {encoded.begin(), encoded.end()};
    }
    return {};
}

void draft23_probe_setup_options(const Draft23Probe& probe, wire::draft21::KeyValues& options) {
    if (probe.scenario_id == "d23-probe-server-authority") options.push_back({5, text("example.test")});
    if (probe.scenario_id == "d23-probe-server-path") options.push_back({1, text("/moq")});
}

}  // namespace moq::interop::session
