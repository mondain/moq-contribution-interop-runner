#pragma once

// Draft-21 completeness-gap slice A: raw probe contexts that observe how the
// publisher behaves as the responder of subscriptions, discovery and fetches
// opened by this runner. Every expectation below cites draft-ietf-moq-
// transport-21; see the evaluator comments for the exact lines.
//
// Fixture contract. The track named by the run is published by the driven
// publisher and retains Group 7 from its first Object, including Object 9.
// Probes that need more than the track identity say so below.

#include "moq/interop/scenarios/raw_probe.h"

namespace moq::interop::scenarios {

enum class Draft21GapAspect {
    // Section 6.4.2.2: a responder sends its response before FIN.
    ResponseBeforeFin,
    // Section 6.4.2.2: the publisher of an Established subscription sends
    // PUBLISH_DONE before FIN.
    PublishDoneBeforeFin,
    // Section 6.4.2.2: no FIN before every message required for the request
    // type has been sent (FETCH and SUBSCRIBE are exercised together).
    TerminalMessageOrder,
    // Section 2.2: FIRST_OBJECT on the first stream of each new Subgroup.
    FirstObjectBit,
    // Section 3.6: Mandatory Track Properties never appear as Object Properties.
    MandatoryPropertyScope,
    // Section 4.2: NAMESPACE for the original publisher's namespace.
    NamespaceDiscovery,
    // Section 6.3: the publisher's control stream stays open during the session.
    ControlStreamLifetime,
    // Section 6.2: the QUIC DATAGRAM extension is supported and negotiated.
    DatagramSupport,
    // Section 6.2: the same extension must also be negotiated, so a connection
    // without it is not a valid MOQT session (row D21-6-2-MUST-140).
    DatagramNegotiation,
    // Section 3.3.1: a subscription with a bounded Location filter.
    BoundedRange,
    // Section 3.3.1 and 9.5.1: a REQUEST_UPDATE that sets the Location filter.
    UpdatedRange,
    // Section 2.2: one Subgroup is not split across streams unless a stream
    // was reset prematurely or Objects are forced out of Object ID order.
    SingleSubgroup,
    SubgroupRestartAfterReset,
};

struct Draft21GapProbe {
    std::string requirement_id;
    std::string evaluator_id;
    Draft21GapAspect aspect;
    RawProbeDefinition definition;
};

std::vector<Draft21GapProbe> draft21_gap_a_probes(
    std::chrono::milliseconds deadline = std::chrono::milliseconds{1000},
    std::vector<std::vector<std::byte>> track_namespace = {},
    std::vector<std::byte> track_name = {std::byte{'x'}});

// Returns no value unless the transcript proves the probe's stimulus and the
// observed evidence settles the requirement.
std::optional<bool> evaluate_draft21_gap_a_probe(
    const RawProbeTranscript& transcript, const Draft21GapProbe& probe);

}  // namespace moq::interop::scenarios
