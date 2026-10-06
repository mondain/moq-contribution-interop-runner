// Result pins for drafts 18 and 21: fixed scenario selections run through the production NativeRunManager
// against a picoquic test client that plays the publisher (observed mode, as in raw_family_driver_test.cpp
// and draft18_contribution_live_test.cpp), the stored run rendered through the functions the server uses
// for /results/<id>.json (http::serialize_result) and /results/<id>.tap (http::serialize_tap14), and the
// normalized text compared with the golden files in tests/golden/results/.
//
// Runs pinned (each covers the typed or the raw path; the second draft 21 run also covers a capability skip):
//   draft18-typed     subscribe-namespace-at-publisher (typed controller), REQUEST_ERROR reply
//   draft18-raw       receive-setup-with-duplicate-unknown-options, observe-publisher-setup-options (raw probes)
//   draft21-typed     d21-setup-duplicate-unknown-options (typed announcement path)
//   draft21-raw       d21-duplicate-request-goaway, d21-fetch-accepted (skipped: fetch=false),
//                     d21-goaway-on-distinct-request-streams (raw probes)
//
// Normalization (explicit, nothing else is touched):
//   - the run id (a random UUID) becomes "<run-id>" everywhere, including the events href and TAP run_id;
//   - run created_at_unix_ns / finalized_at_unix_ns and each event's monotonic_time_ns / wall_time_unix_ns
//     become 0 (timestamps and durations);
//   - each event's connection_id and the local_connection_id= / peer_connection_id= values in event detail
//     text (random QUIC connection ids) become "<connection-N>", numbered by first appearance in event order;
//     the same ids anywhere in detail text are replaced the same way;
//   - the listener port (ephemeral) becomes "<port>" in event, error-reason and truncation detail text;
//   - requirement rows keep their id and every field serialize_result computes for the run (outcome,
//     observations, evidence_sequences, not_applicable_reason, weight, required, score_eligible) but drop
//     the fields it copies verbatim from the catalog (kCatalogCopies), which the catalog tests pin and which
//     would otherwise make each golden file most of a megabyte.
// Layout: the JSON document is written with its long arrays (run.outcomes, requirements, evidence) one compact
// element per line so a diff names the element that changed; the TAP text is compared as served.
//
// Regenerate (after reviewing that a change in the output is intended):
//   MOQ_UPDATE_GOLDEN=1 build/moq-interop-draft-result-pins-tests
// then review the diff under tests/golden/results/ and commit it. Without the variable the test only compares.
#include "moq/interop/app/native_run_manager.h"
#include "moq/interop/http/result_schema.h"
#include "moq/interop/requirements/draft_source.h"
#include "moq/interop/storage/run_store.h"
#include "support/picoquic_client.h"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <regex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace moq::interop {
namespace {
using namespace std::chrono_literals;
using Bytes = std::vector<std::byte>;
using Client = transport::test::PicoquicTestClient;
using Json = nlohmann::json;

const std::filesystem::path kSourceRoot{MOQ_INTEROP_PROJECT_SOURCE_DIR};

Bytes bytes(std::initializer_list<unsigned> values) {
    Bytes result;
    for (const auto value : values) result.push_back(static_cast<std::byte>(value));
    return result;
}

Bytes text(std::string_view value) {
    Bytes result;
    for (const char byte : value) result.push_back(static_cast<std::byte>(byte));
    return result;
}

std::shared_ptr<const requirements::RequirementCatalog> catalog(unsigned draft) {
    const auto source = requirements::load_draft_source(draft, kSourceRoot / "docs",
                                                        kSourceRoot / "requirements/draft-digests.json");
    return std::make_shared<const requirements::RequirementCatalog>(requirements::RequirementCatalog::load(
        source, kSourceRoot / "requirements" / ("draft" + std::to_string(draft) + ".json")));
}

template <class Predicate>
bool pump_until(Client& client, Predicate predicate, std::chrono::milliseconds limit = 3s) {
    const auto deadline = std::chrono::steady_clock::now() + limit;
    while (std::chrono::steady_clock::now() < deadline) {
        if (!client.pump()) return false;
        if (predicate()) return true;
        std::this_thread::sleep_for(1ms);
    }
    return false;
}

template <class Predicate>
bool wait_for(Predicate predicate, std::chrono::milliseconds limit = 5s) {
    const auto deadline = std::chrono::steady_clock::now() + limit;
    while (std::chrono::steady_clock::now() < deadline) {
        if (predicate()) return true;
        std::this_thread::sleep_for(1ms);
    }
    return false;
}

bool context_ready(const storage::RunRecord& run, std::string_view scenario) {
    return std::any_of(run.events.begin(), run.events.end(), [&](const auto& event) {
        return event.kind == "context_ready" && event.scenario_id == scenario;
    });
}

struct Harness {
    std::shared_ptr<const requirements::RequirementCatalog> draft18 = catalog(18);
    std::shared_ptr<const requirements::RequirementCatalog> draft21 = catalog(21);
    std::shared_ptr<storage::SqliteRunStore> store =
        std::make_shared<storage::SqliteRunStore>(":memory:", app::BuildInfo{"test", "test", {}});
    app::NativeRunManager manager{draft18, draft21, store,
        {.bind_address = "127.0.0.1", .advertised_address = "127.0.0.1",
         .port_start = 0, .port_end = 0, .maximum_active_runs = 1,
         .certificate_path = std::filesystem::path(PICOQUIC_TEST_CERT_DIR) / "cert.pem",
         .private_key_path = std::filesystem::path(PICOQUIC_TEST_CERT_DIR) / "key.pem"}};

    const requirements::RequirementCatalog& catalog_of(app::DraftVersion draft) const {
        return draft == app::DraftVersion::Draft18 ? *draft18 : *draft21;
    }
};

// Plays one publisher session per played context (in selection order; a scenario the publisher's declaration
// skips is not played), each on a fresh connection: `play(index, client)` scripts the session. Raw probe
// contexts each announce context_ready; a typed run has one session and listens as soon as it starts.
template <class Play>
storage::RunRecord run_selection(Harness& harness, const app::RunConfig& config, std::string_view alpn,
                                 const std::vector<std::string>& played, bool raw, Play play, std::uint16_t& port) {
    const auto started = harness.manager.start(config);
    EXPECT_EQ(started.status, app::RunStartStatus::Started);
    if (started.status != app::RunStartStatus::Started) return {};
    port = started.endpoint.port;
    for (std::size_t index = 0; index < played.size(); ++index) {
        SCOPED_TRACE(played[index]);
        if (raw) {
            EXPECT_TRUE(wait_for([&] { return context_ready(harness.store->load(started.id), played[index]); }));
        }
        auto client = Client::create({.port = port, .alpn = text(alpn)});
        EXPECT_NE(client, nullptr);
        if (!client) break;
        play(index, *client);
        // The context ends when the next one is listening, or the run finalizes.
        EXPECT_TRUE(wait_for([&] {
            client->pump();
            const auto run = harness.store->load(started.id);
            return index + 1 == played.size() ? run.state == storage::RunState::Finalized
                                              : context_ready(run, played[index + 1]);
        }));
    }
    EXPECT_TRUE(wait_for([&] { return harness.store->load(started.id).state == storage::RunState::Finalized; }));
    harness.manager.stop(started.id);
    return harness.store->load(started.id);
}

// ---- Normalization -------------------------------------------------------------------------------

// Requirement row fields serialize_result copies verbatim from the catalog (requirement_json); the catalog
// tests pin them. The row keeps its id and every field serialize_result computes for the run.
constexpr const char* kCatalogCopies[] = {"strength", "source", "actor", "summary", "applicability",
                                          "testability", "scenarios", "evaluators", "rationale"};

std::string replace_all(std::string value, std::string_view from, std::string_view to) {
    if (from.empty()) return value;
    for (std::size_t at = value.find(from); at != std::string::npos; at = value.find(from, at + to.size()))
        value.replace(at, from.size(), to);
    return value;
}

class Normalizer {
public:
    Normalizer(std::string run_id, std::uint16_t port) : run_id_(std::move(run_id)), port_(std::to_string(port)) {}

    std::string connection(const std::string& id) {
        const auto [found, inserted] = connections_.emplace(id, "<connection-" + std::to_string(connections_.size() + 1) + ">");
        (void)inserted;
        return found->second;
    }

    // Registers the QUIC connection ids a detail names (local_connection_id=, peer_connection_id=).
    void scan(const std::string& value) {
        static const std::regex named("(local|peer)_connection_id=([0-9a-f]+)");
        for (auto it = std::sregex_iterator(value.begin(), value.end(), named); it != std::sregex_iterator(); ++it)
            connection((*it)[2].str());
    }

    std::string detail(std::string value) const {
        for (const auto& [id, name] : connections_) value = replace_all(std::move(value), id, name);
        // The port as a whole number, e.g. "127.0.0.1:<port>/moq" (not inside a hex run or a longer number).
        return std::regex_replace(value, std::regex("(^|[^0-9a-fA-F])" + port_ + "($|[^0-9])"), "$1<port>$2");
    }

    std::string text(std::string value) const { return replace_all(std::move(value), run_id_, "<run-id>"); }

private:
    std::string run_id_;
    std::string port_;
    std::map<std::string, std::string> connections_;
};

std::string normalized_result(const storage::RunRecord& run, const requirements::RequirementCatalog& catalog,
                              std::uint16_t port) {
    auto result = http::serialize_result(run, catalog);
    Normalizer normalizer(run.id, port);
    auto& stored = result.at("run");
    for (const auto* key : {"created_at_unix_ns", "finalized_at_unix_ns"})
        if (!stored.at(key).is_null()) stored[key] = 0;
    for (auto& event : result.at("evidence")) {
        event["monotonic_time_ns"] = 0;
        event["wall_time_unix_ns"] = 0;
        if (event.at("connection_id").is_string())
            event["connection_id"] = normalizer.connection(event.at("connection_id").get<std::string>());
        normalizer.scan(event.at("detail").get<std::string>());
    }
    for (auto& event : result.at("evidence")) event["detail"] = normalizer.detail(event.at("detail").get<std::string>());
    for (auto& reason : stored.at("error_reasons")) reason["detail"] = normalizer.detail(reason.at("detail").get<std::string>());
    for (auto& context : stored.at("truncated_contexts"))
        context["detail"] = normalizer.detail(context.at("detail").get<std::string>());
    if (stored.at("run_error_reason").is_string())
        stored["run_error_reason"] = normalizer.detail(stored.at("run_error_reason").get<std::string>());
    for (auto& row : result.at("requirements"))
        for (const auto* key : kCatalogCopies) row.erase(key);
    // Layout only: the document without its long arrays, then one compact line per stored outcome, per
    // requirement row and per evidence event, so a diff names the outcome, row or event that changed.
    std::ostringstream out;
    auto head = result;
    head.erase("requirements");
    head.erase("evidence");
    head.at("run").erase("outcomes");
    out << head.dump(1) << "\nrun.outcomes:\n";
    for (const auto& outcome : stored.at("outcomes")) out << outcome.dump() << '\n';
    out << "requirements:\n";
    for (const auto& row : result.at("requirements")) out << row.dump() << '\n';
    out << "evidence:\n";
    for (const auto& event : result.at("evidence")) out << event.dump() << '\n';
    return normalizer.text(out.str());
}

std::string normalized_tap(const storage::RunRecord& run, const requirements::RequirementCatalog& catalog) {
    return Normalizer(run.id, 0).text(http::serialize_tap14(run, catalog));
}

void expect_golden(const std::string& name, const std::string& actual) {
    const auto path = kSourceRoot / "tests/golden/results" / name;
    const char* update = std::getenv("MOQ_UPDATE_GOLDEN");
    if (update && std::string_view(update) == "1") {
        std::filesystem::create_directories(path.parent_path());
        std::ofstream(path, std::ios::binary) << actual;
        return;
    }
    std::ifstream input(path, std::ios::binary);
    ASSERT_TRUE(input) << "missing golden file " << path << " (regenerate with MOQ_UPDATE_GOLDEN=1)";
    std::stringstream expected;
    expected << input.rdbuf();
    if (expected.str() == actual) return;
    // Name the first differing line, then fail with the whole text (gtest prints a diff for strings).
    std::istringstream left(expected.str());
    std::istringstream right(actual);
    std::string a;
    std::string b;
    std::size_t line = 0;
    while (true) {
        ++line;
        const bool more_a = static_cast<bool>(std::getline(left, a));
        const bool more_b = static_cast<bool>(std::getline(right, b));
        if (!more_a && !more_b) break;
        if (!more_a || !more_b || a != b) {
            ADD_FAILURE() << path << " differs at line " << line << "\n  golden: " << (more_a ? a : "<end>")
                          << "\n  actual: " << (more_b ? b : "<end>");
            break;
        }
    }
    EXPECT_EQ(expected.str(), actual) << path;
}

void pin(const Harness& harness, const std::string& name, const storage::RunRecord& run, std::uint16_t port) {
    ASSERT_EQ(run.state, storage::RunState::Finalized);
    const auto& catalog = harness.catalog_of(run.config.draft);
    expect_golden(name + ".result.json", normalized_result(run, catalog, port));
    expect_golden(name + ".tap", normalized_tap(run, catalog));
}

// ---- Draft 18 ------------------------------------------------------------------------------------

TEST(DraftResultPins, Draft18TypedDiscovery) {
    Harness harness;
    const std::vector<std::string> ids{"subscribe-namespace-at-publisher"};
    std::uint16_t port = 0;
    const auto run = run_selection(harness,
        {app::DraftVersion::Draft18, app::TransportKind::NativeQuic, app::RunMode::Observed, ids, 1500ms,
         app::TrackFixture{{"n"}, "x"}},
        "moqt-18", ids, false, [](std::size_t, Client& client) {
            ASSERT_TRUE(pump_until(client, [&] { const auto s = client.stream(3); return s && s->data.size() == 4; }));
            ASSERT_TRUE(client.send_stream(2, bytes({0xaf, 0, 0, 0}), false));
            ASSERT_TRUE(pump_until(client, [&] { const auto s = client.stream(1); return s && s->data.size() == 8; }));
            ASSERT_TRUE(client.send_stream(1, bytes({0x05, 0x00, 0x03, 0x11, 0x00, 0x00}), false));
        }, port);
    pin(harness, "draft18-typed", run, port);
}

TEST(DraftResultPins, Draft18RawSetupProbes) {
    Harness harness;
    const std::vector<std::string> ids{"receive-setup-with-duplicate-unknown-options",
                                       "observe-publisher-setup-options"};
    std::uint16_t port = 0;
    const auto run = run_selection(harness,
        {app::DraftVersion::Draft18, app::TransportKind::NativeQuic, app::RunMode::Observed, ids, 1500ms,
         std::nullopt},
        "moqt-18", ids, true, [](std::size_t index, Client& client) {
            ASSERT_TRUE(pump_until(client, [&] { const auto s = client.stream(3); return s && s->data.size() >= 4; }));
            ASSERT_TRUE(client.send_stream(2, bytes({0xaf, 0, 0, 0}), false));
            if (index == 0) {
                ASSERT_TRUE(pump_until(client, [&] { const auto s = client.stream(1); return s && !s->data.empty(); }));
                ASSERT_TRUE(client.send_stream(1, bytes({7, 0, 1, 0}), false));
            }
        }, port);
    pin(harness, "draft18-raw", run, port);
}

// ---- Draft 21 ------------------------------------------------------------------------------------

TEST(DraftResultPins, Draft21TypedAnnouncement) {
    Harness harness;
    const std::vector<std::string> ids{"d21-setup-duplicate-unknown-options"};
    std::uint16_t port = 0;
    const auto run = run_selection(harness,
        {app::DraftVersion::Draft21, app::TransportKind::NativeQuic, app::RunMode::Observed, ids, 1000ms,
         app::TrackFixture{{"media"}, "test"}},
        "moqt-21", ids, false, [](std::size_t, Client& client) {
            ASSERT_TRUE(pump_until(client, [&] {
                const auto setup = client.stream(3);
                return setup && setup->data == bytes({0xaf, 0x00, 0x00, 0x07, 0x80, 0x9d, 0x01, 0xaa, 0x00, 0x01, 0xbb});
            }));
            ASSERT_TRUE(client.send_stream(2, bytes({0xaf, 0x00, 0x00, 0x00}), false));
            ASSERT_TRUE(client.send_stream(0, bytes({0x1d, 0x00, 0x0f, 0x00, 0x01, 0x05, 'm', 'e', 'd', 'i', 'a',
                                                     0x04, 't', 'e', 's', 't', 0x02, 0x00}), false));
            ASSERT_TRUE(pump_until(client, [&] {
                const auto response = client.stream(0);
                return response && response->data == bytes({0x07, 0x00, 0x01, 0x00});
            }));
        }, port);
    pin(harness, "draft21-typed", run, port);
}

// The publisher half of the two request-GOAWAY contexts (as in raw_family_driver_test.cpp): `duplicate` is
// d21-duplicate-request-goaway, otherwise d21-goaway-on-distinct-request-streams.
void goaway_publisher(Client& client, bool duplicate) {
    ASSERT_TRUE(pump_until(client, [&] { const auto setup = client.stream(3); return setup && setup->data == bytes({0xaf, 0, 0, 0}); }));
    ASSERT_TRUE(client.send_stream(2, bytes({0xaf, 0, 0, 0}), false));
    const auto opening = [](unsigned id, unsigned field) { return bytes({0x50, 0, 5, id, 1, 1, field, 0}); };
    const auto a = opening(1, 'a');
    const auto b = opening(3, 'b');
    ASSERT_TRUE(pump_until(client, [&] { const auto stream = client.stream(1); return stream && stream->data == a; }));
    if (!duplicate) {
        ASSERT_TRUE(pump_until(client, [&] { const auto stream = client.stream(5); return stream && stream->data == b; }));
        ASSERT_TRUE(client.send_stream(5, bytes({7, 0, 1, 0}), false));
    }
    ASSERT_TRUE(client.send_stream(1, bytes({7, 0, 1, 0}), false));
    const auto goaway = bytes({0x10, 0, 3, 0, 0xa7, 0x10});
    auto expected = a;
    expected.insert(expected.end(), goaway.begin(), goaway.end());
    if (duplicate) expected.insert(expected.end(), goaway.begin(), goaway.end());
    ASSERT_TRUE(pump_until(client, [&] { const auto stream = client.stream(1); return stream && stream->data == expected && !stream->fin; }));
    if (duplicate) {
        ASSERT_TRUE(client.close(3, {}));
    } else {
        auto expected_b = b;
        expected_b.insert(expected_b.end(), goaway.begin(), goaway.end());
        ASSERT_TRUE(pump_until(client, [&] { const auto stream = client.stream(5); return stream && stream->data == expected_b && !stream->fin; }));
        ASSERT_TRUE(pump_until(client, [&] { const auto stream = client.stream(9); return stream && stream->data == opening(5, 'c'); }));
        ASSERT_TRUE(client.send_stream(9, bytes({7, 0, 1, 0}), false));
    }
}

TEST(DraftResultPins, Draft21RawGoawayWithCapabilitySkip) {
    Harness harness;
    const std::vector<std::string> ids{"d21-duplicate-request-goaway", "d21-fetch-accepted",
                                       "d21-goaway-on-distinct-request-streams"};
    const std::vector<std::string> played{"d21-duplicate-request-goaway", "d21-goaway-on-distinct-request-streams"};
    std::uint16_t port = 0;
    const auto run = run_selection(harness,
        {app::DraftVersion::Draft21, app::TransportKind::NativeQuic, app::RunMode::Observed, ids, 2000ms,
         app::TrackFixture{{"n"}, "t"}, {.fetch = false}},
        "moqt-21", played, true, [](std::size_t index, Client& client) { goaway_publisher(client, index == 0); }, port);
    pin(harness, "draft21-raw", run, port);
}

}  // namespace
}  // namespace moq::interop
