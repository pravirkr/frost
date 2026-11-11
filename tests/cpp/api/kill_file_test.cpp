#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <frost/prepare/rfi.hpp>

#include "support/temp_dir.hpp"

using Catch::Matchers::ContainsSubstring;
using frost::SizeType;
using frost::prepare::rfi::ChannelRange;
using frost::prepare::rfi::read_kill_file;

namespace {
std::filesystem::path write(const std::filesystem::path& dir,
                            const std::string& text) {
    const auto path = dir / "kill.txt";
    std::ofstream(path, std::ios::binary) << text;
    return path;
}
} // namespace

TEST_CASE("kill file merges killed channels in stored order",
          "[rfi][kill_file]") {
    const auto dir = frost::test::unique_temp_dir("kill_file_ok");
    // Comments, blank lines, padding and CRLF endings are ignored.
    const auto path = write(dir, "# header comment\n"
                                 "0\n0  # dead\n1\n\n  1\r\n0\n1\n0\t\n0\n");
    CHECK(read_kill_file(path, 8) ==
          std::vector<ChannelRange>{{0, 1}, {4, 4}, {6, 7}});
    CHECK(read_kill_file(write(dir, "1\n1\n1"), 3).empty());
    CHECK(read_kill_file(write(dir, "0\n0\n"), 2) ==
          std::vector<ChannelRange>{{0, 1}});
}

TEST_CASE("kill file is strict about channel count and values",
          "[rfi][kill_file][contracts]") {
    const auto dir = frost::test::unique_temp_dir("kill_file_bad");
    CHECK_THROWS_WITH(read_kill_file(write(dir, "1\n0\n"), 3),
                      ContainsSubstring("has 2 channel entries"));
    CHECK_THROWS_WITH(read_kill_file(write(dir, "1\n0\n1\n1\n"), 3),
                      ContainsSubstring("more than 3"));
    for (const auto* bad : {"2", "1 0", "0.0", "yes", "-1", "01"}) {
        INFO(bad);
        CHECK_THROWS_WITH(
            read_kill_file(write(dir, std::string("1\n") + bad + "\n1\n"), 3),
            ContainsSubstring("line 2"));
    }
    CHECK_THROWS_AS(read_kill_file(dir / "missing.txt", 3), std::runtime_error);
}
