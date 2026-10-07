#include "moq/interop/requirements/draft_source.h"

#include <nlohmann/json.hpp>
#include <openssl/evp.h>

#include <array>
#include <cstdint>
#include <fstream>
#include <stdexcept>

namespace moq::interop::requirements {
namespace {

constexpr std::uintmax_t kMaxDraftBytes = 1024 * 1024;
constexpr std::uintmax_t kMaxDigestBytes = 16 * 1024;

std::string read_bounded_file(const std::filesystem::path& path, std::uintmax_t max_bytes) {
    const auto size = std::filesystem::file_size(path);
    if (size > max_bytes) {
        throw std::runtime_error("File exceeds size limit: " + path.string());
    }
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        throw std::runtime_error("Cannot open file: " + path.string());
    }

    std::string content(static_cast<std::size_t>(size), '\0');
    input.read(content.data(), static_cast<std::streamsize>(size));
    if (input.gcount() != static_cast<std::streamsize>(size) ||
        input.peek() != std::char_traits<char>::eof()) {
        throw std::runtime_error("File changed while reading: " + path.string());
    }
    return content;
}

std::string sha256_hex(std::string_view text) {
    std::array<unsigned char, EVP_MAX_MD_SIZE> digest{};
    unsigned digest_size = 0;
    if (EVP_Digest(text.data(), text.size(), digest.data(), &digest_size, EVP_sha256(), nullptr) != 1 ||
        digest_size != 32) {
        throw std::runtime_error("SHA-256 computation failed");
    }

    constexpr char kHex[] = "0123456789abcdef";
    std::string result;
    result.reserve(digest_size * 2);
    for (unsigned i = 0; i < digest_size; ++i) {
        result.push_back(kHex[digest[i] >> 4]);
        result.push_back(kHex[digest[i] & 0x0f]);
    }
    return result;
}

}  // namespace

std::string_view DraftSource::lines(std::size_t first, std::size_t last) const {
    if (first == 0 || first > last || last > line_offsets.size()) {
        throw std::out_of_range("Draft line range is outside the source");
    }
    const auto begin = line_offsets[first - 1];
    const auto end = last < line_offsets.size() ? line_offsets[last] : text.size();
    return std::string_view(text).substr(begin, end - begin);
}

std::string draft_source_filename(unsigned draft) {
    switch (draft) {
        case 18:
        case 21:
        case 22:
            return "draft-ietf-moq-transport-" + std::to_string(draft) + ".txt";
        case 106:
            return "draft-lcurley-moq-lite-06.txt";
        default:
            throw std::invalid_argument("No draft source file for draft " + std::to_string(draft));
    }
}

DraftSource load_draft_source(unsigned draft, const std::filesystem::path& docs_root,
                              const std::filesystem::path& digest_file) {
    const auto manifest = nlohmann::json::parse(read_bounded_file(digest_file, kMaxDigestBytes));
    const auto key = std::to_string(draft);
    if (!manifest.is_object() || !manifest.contains(key) || !manifest.at(key).is_string()) {
        throw std::runtime_error("No SHA-256 digest for draft " + key);
    }
    const auto expected = manifest.at(key).get<std::string>();

    DraftSource source;
    source.number = draft;
    source.path = docs_root / draft_source_filename(draft);
    source.text = read_bounded_file(source.path, kMaxDraftBytes);
    source.sha256 = sha256_hex(source.text);
    if (source.sha256 != expected) {
        throw std::runtime_error("Draft " + key + " SHA-256 mismatch: expected " + expected +
                                 ", actual " + source.sha256);
    }

    source.line_offsets.push_back(0);
    for (std::size_t i = 0; i < source.text.size(); ++i) {
        if (source.text[i] == '\n' && i + 1 < source.text.size()) {
            source.line_offsets.push_back(i + 1);
        }
    }
    return source;
}

}  // namespace moq::interop::requirements
