#pragma once

// Publisher stand-ins for draft 22 runs (moqt-22), free of test-framework macros so both live tests and the
// audit fixture helper can play them. Each returns whether the publisher side went as scripted.
#include "moq/interop/app/types.h"
#include "moq/interop/storage/run_store.h"
#include "support/picoquic_client.h"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace moq::interop::d22pub {

using namespace std::chrono_literals;
using Bytes = std::vector<std::byte>;
using Client = transport::test::PicoquicTestClient;

inline Bytes bytes(std::initializer_list<unsigned> values) {
    Bytes result;
    for (const auto value : values) result.push_back(static_cast<std::byte>(value));
    return result;
}

inline Bytes alpn22() { return bytes({'m', 'o', 'q', 't', '-', '2', '2'}); }

// The SETUP both sides send (Section 9.1): Type 0x2F00, Length 0.
inline Bytes setup() { return bytes({0xaf, 0, 0, 0}); }

template <class Predicate>
bool pump_until(Client& client, Predicate predicate, std::chrono::milliseconds limit = 4s) {
    const auto deadline = std::chrono::steady_clock::now() + limit;
    while (std::chrono::steady_clock::now() < deadline) {
        if (!client.pump()) return false;
        if (predicate()) return true;
        std::this_thread::sleep_for(1ms);
    }
    return false;
}

// Waits until the run's context `ordinal` (1-based) is listening.
inline bool context_ready(storage::RunStore& store, const app::RunId& id, unsigned ordinal) {
    const auto deadline = std::chrono::steady_clock::now() + 3s;
    const auto suffix = " ordinal=" + std::to_string(ordinal);
    while (std::chrono::steady_clock::now() < deadline) {
        const auto run = store.load(id);
        if (std::any_of(run.events.begin(), run.events.end(), [&](const auto& event) {
                return event.kind == "context_ready" && event.detail.ends_with(suffix);
            }))
            return true;
        std::this_thread::sleep_for(1ms);
    }
    return false;
}

inline storage::RunRecord finalized(storage::RunStore& store, const app::RunId& id) {
    const auto deadline = std::chrono::steady_clock::now() + 10s;
    while (std::chrono::steady_clock::now() < deadline && store.load(id).state != storage::RunState::Finalized)
        std::this_thread::sleep_for(1ms);
    return store.load(id);
}

// FETCH for track (n)/t, Request ID 1, with the draft 22 LOCATION_FILTER {0, 0, u64max} (Type 0x03).
inline Bytes fetch_for_n_t() {
    auto fetch = bytes({0x16, 0, 20, 1, 1, 1, 'n', 1, 't', 1, 0x21, 0x03, 0, 0});
    for (unsigned index = 0; index < 9; ++index) fetch.push_back(std::byte{0xff});
    return fetch;
}
// FETCH_OK and REQUEST_ERROR answering it.
inline Bytes fetch_ok() { return bytes({0x18, 0, 4, 0, 0, 1, 0}); }
inline Bytes fetch_error() { return bytes({5, 0, 3, 1, 0, 0}); }

// One FETCH context of d22-fetch-accepted / d22-fetch-rejected (fixture (n)/t): answers the runner's FETCH
// with `reply` and FIN.
inline bool play_fetch(std::uint16_t port, const Bytes& reply) {
    auto client = Client::create({.port = port, .alpn = alpn22()});
    if (!client) return false;
    if (!pump_until(*client, [&] { const auto control = client->stream(3); return control && control->data == setup(); }))
        return false;
    if (!client->send_stream(2, setup(), false)) return false;
    const auto expected = fetch_for_n_t();
    if (!pump_until(*client, [&] {
            const auto request = client->stream(1);
            return request && request->fin && request->data == expected;
        }))
        return false;
    if (!client->send_stream(1, reply, true)) return false;
    (void)pump_until(*client, [] { return false; }, 100ms);
    return true;
}

// Plays both contexts of the shared raw pair {d22-fetch-accepted, d22-fetch-rejected} in that order.
inline bool play_fetch_pair(storage::RunStore& store, const app::RunId& id, std::uint16_t port) {
    return context_ready(store, id, 1) && play_fetch(port, fetch_ok()) && context_ready(store, id, 2) &&
           play_fetch(port, fetch_error());
}

// d22-request-stream-before-peer-setup (fixture (n)/t): the runner's SUBSCRIBE arrives before its SETUP is
// complete and the publisher resets the request stream (the MAY of D22-6-3-MAY-159).
inline bool play_pre_setup_reset(storage::RunStore& store, const app::RunId& id, std::uint16_t port) {
    if (!context_ready(store, id, 1)) return false;
    auto client = Client::create({.port = port, .alpn = alpn22()});
    if (!client) return false;
    if (!pump_until(*client, [&] { return client->established(); })) return false;
    if (!client->send_stream(2, setup(), false)) return false;
    const auto subscribe = bytes({3, 0, 9, 1, 1, 1, 'n', 1, 't', 1, 0x10, 0});
    if (!pump_until(*client, [&] {
            const auto request = client->stream(1);
            return request && request->data.size() >= subscribe.size();
        }))
        return false;
    if (client->stream(1)->data != subscribe || !client->reset_stream(1, 1)) return false;
    (void)pump_until(*client, [&] { return store.load(id).state == storage::RunState::Finalized; }, 10s);
    return true;
}

}  // namespace moq::interop::d22pub
