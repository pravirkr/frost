#include <regex>
#include <string>

#include <catch2/catch_test_macros.hpp>

#include <frost/common/version.hpp>

TEST_CASE("build_info reports the project version and a source revision",
          "[version]") {
    const auto& info = frost::build_info();
    CHECK(info.version == FROST_PROJECT_VERSION);
    CHECK(std::regex_match(std::string(info.version),
                           std::regex(R"(\d+\.\d+\.\d+)")));
    // A full commit (with "-dirty" for local changes) or "unknown" for a
    // tree without git metadata.
    CHECK(std::regex_match(std::string(info.revision),
                           std::regex(R"(([0-9a-f]{40}(-dirty)?)|unknown)")));
    CHECK_FALSE(info.dmt_revision.empty());
    CHECK_FALSE(info.psrio_revision.empty());
    CHECK_FALSE(info.compiler.empty());
}
