#include "moq/interop/requirements/draft_source.h"

#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>

namespace moq::interop::requirements {
namespace {

const std::filesystem::path kProjectRoot = MOQ_INTEROP_PROJECT_SOURCE_DIR;

class DraftSourceTest : public ::testing::Test {
protected:
    void SetUp() override {
        temp_dir_ = std::filesystem::temp_directory_path() /
                    ("moq-interop-draft-source-" +
                     std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        std::filesystem::create_directories(temp_dir_);
    }

    void TearDown() override { std::filesystem::remove_all(temp_dir_); }

    std::filesystem::path temp_dir_;
};

TEST_F(DraftSourceTest, LoadsDraft18WithVerifiedDigestAndOneBasedLines) {
    const auto draft = load_draft_source(18, kProjectRoot / "docs",
                                         kProjectRoot / "requirements/draft-digests.json");

    EXPECT_EQ(draft.number, 18u);
    EXPECT_EQ(draft.path, kProjectRoot / "docs/draft-ietf-moq-transport-18.txt");
    EXPECT_EQ(draft.sha256, "9e6b32cb7797c151e9e127374c1291af3ed546b2d453cd5bbb15946977eeeeb6");
    EXPECT_EQ(draft.text.size(), 314260u);
    ASSERT_FALSE(draft.line_offsets.empty());
    EXPECT_EQ(draft.line_offsets.front(), 0u);
    const auto last_line = draft.line_offsets.size();
    EXPECT_EQ(draft.lines(1, 1), "\n");
    EXPECT_EQ(draft.lines(16, 16), "                      draft-ietf-moq-transport-18\n");
    EXPECT_EQ(draft.lines(17, 18), "\nAbstract\n");
    EXPECT_EQ(draft.lines(last_line, last_line),
              "Nandakumar, et al.      Expires 13 November 2026              [Page 140]\n");
    EXPECT_THROW(draft.lines(0, 1), std::out_of_range);
    EXPECT_THROW(draft.lines(18, 17), std::out_of_range);
    EXPECT_THROW(draft.lines(last_line + 1, last_line + 1), std::out_of_range);
}

TEST_F(DraftSourceTest, LoadsDraft21WithVerifiedDigestAndOneBasedLines) {
    const auto draft = load_draft_source(21, kProjectRoot / "docs",
                                         kProjectRoot / "requirements/draft-digests.json");

    EXPECT_EQ(draft.number, 21u);
    EXPECT_EQ(draft.path, kProjectRoot / "docs/draft-ietf-moq-transport-21.txt");
    EXPECT_EQ(draft.sha256, "8c1d80849f46026b634fa19f8607ce983be2b1db9ae4ee1990878969c7b7e142");
    EXPECT_EQ(draft.text.size(), 358078u);
    ASSERT_FALSE(draft.line_offsets.empty());
    const auto last_line = draft.line_offsets.size();
    EXPECT_EQ(draft.lines(1, 1), "\n");
    EXPECT_EQ(draft.lines(16, 16), "                      draft-ietf-moq-transport-21\n");
    EXPECT_EQ(draft.lines(17, 18), "\nAbstract\n");
    EXPECT_EQ(draft.lines(last_line, last_line),
              "Nandakumar, et al.        Expires 12 March 2027               [Page 159]\n");
    EXPECT_THROW(draft.lines(last_line + 1, last_line + 1), std::out_of_range);
}

TEST_F(DraftSourceTest, ReportsExpectedAndActualDigestForOneByteChange) {
    const auto altered = temp_dir_ / "draft-ietf-moq-transport-18.txt";
    std::filesystem::copy_file(kProjectRoot / "docs/draft-ietf-moq-transport-18.txt", altered);
    {
        std::fstream file(altered, std::ios::binary | std::ios::in | std::ios::out);
        ASSERT_TRUE(file.is_open());
        file.put('X');
        ASSERT_TRUE(file.good());
    }

    try {
        load_draft_source(18, temp_dir_, kProjectRoot / "requirements/draft-digests.json");
        FAIL() << "Expected a digest mismatch";
    } catch (const std::runtime_error& error) {
        const std::string message = error.what();
        EXPECT_NE(message.find("18"), std::string::npos);
        EXPECT_NE(message.find("9e6b32cb7797c151e9e127374c1291af3ed546b2d453cd5bbb15946977eeeeb6"),
                  std::string::npos);
        EXPECT_NE(message.find("8f9074999c885f0fed2e8352bcb9c3c9e884db6a43a33b99fdbf69606522a3cc"),
                  std::string::npos);
    }
}

TEST_F(DraftSourceTest, RejectsDraftLargerThanTheBound) {
    const auto oversized = temp_dir_ / "draft-ietf-moq-transport-18.txt";
    {
        std::ofstream file(oversized, std::ios::binary);
        file << std::string(1024 * 1024 + 1, 'X');
        ASSERT_TRUE(file.good());
    }

    try {
        load_draft_source(18, temp_dir_, kProjectRoot / "requirements/draft-digests.json");
        FAIL() << "Expected the draft size limit to reject the file";
    } catch (const std::runtime_error& error) {
        EXPECT_NE(std::string(error.what()).find("size limit"), std::string::npos);
    }
}

}  // namespace
}  // namespace moq::interop::requirements
