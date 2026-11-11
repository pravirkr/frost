#include "support/temp_dir.hpp"

#include <filesystem>
#include <fstream>

#include <catch2/catch_test_macros.hpp>

TEST_CASE(
    "test temporary directories survive helpers and clean up nested files",
    "[test-support]") {
    std::filesystem::path first, second;
    {
        frost::test::TempDirectoryRegistry directories;
        first  = directories.create("registry");
        second = directories.create("registry");
        REQUIRE(first != second);
        std::filesystem::create_directory(first / "nested");
        std::ofstream(first / "nested" / "data") << "fixture";
        REQUIRE(std::filesystem::exists(first / "nested" / "data"));
        REQUIRE(std::filesystem::is_directory(second));
    }
    CHECK_FALSE(std::filesystem::exists(first));
    CHECK_FALSE(std::filesystem::exists(second));
}
