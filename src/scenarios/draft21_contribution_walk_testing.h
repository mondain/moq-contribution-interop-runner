#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <utility>
#include <vector>

namespace moq::interop::scenarios::d21c {

// Test seams over the two file-local Message Parameter readers, so a test can pin what each reports for
// a given body. Not used by production code.

struct BlockParameterForTest {
    std::uint64_t type{0};
    std::optional<std::uint64_t> number;
    std::optional<std::pair<std::uint64_t, std::uint64_t>> location;
    std::vector<std::byte> bytes;
    bool operator==(const BlockParameterForTest&) const = default;
};

struct BlockForTest {
    bool ok{false};
    std::vector<BlockParameterForTest> values;
    std::size_t consumed{0};  // bytes of `body` the reader advanced over
};

// d21b's parse_block over `body`, which starts with the Number of Parameters.
BlockForTest d21b_parse_block_for_test(std::span<const std::byte> body);

struct SessionWalkForTest {
    std::size_t declared{0};
    std::size_t parsed{0};
    bool structure{true};
    bool overflow{false};
    bool unknown{false};
    bool forbidden_repeat{false};
    std::size_t consumed{0};
    bool operator==(const SessionWalkForTest&) const = default;
};

// The session scenarios' walk_parameters over `body` (the parameters after the count).
SessionWalkForTest session_walk_parameters_for_test(std::span<const std::byte> body, std::uint64_t count);

}  // namespace moq::interop::scenarios::d21c
