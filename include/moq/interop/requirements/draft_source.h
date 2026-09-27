#pragma once

#include <cstddef>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace moq::interop::requirements {

struct DraftSource {
    unsigned number;
    std::filesystem::path path;
    std::string sha256;
    std::string text;
    std::vector<std::size_t> line_offsets;

    std::string_view lines(std::size_t first, std::size_t last) const;
};

DraftSource load_draft_source(unsigned draft, const std::filesystem::path& docs_root,
                              const std::filesystem::path& digest_file);

}  // namespace moq::interop::requirements
