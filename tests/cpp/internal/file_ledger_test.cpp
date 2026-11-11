#include "lib/output/file_ledger.hpp"

#include <filesystem>

#include <catch2/catch_test_macros.hpp>

TEST_CASE("file ledger remembers late writes and removes its markers",
          "[output][memory]") {
    std::filesystem::path path;
    {
        frost::output::FileLedger ledger;
        path = ledger.directory();
        CHECK_FALSE(ledger.seen_before("first.cand"));
        for (int i = 0; i < 200; ++i)
            CHECK_FALSE(ledger.seen_before(std::to_string(i) + ".cand"));
        CHECK(ledger.seen_before("first.cand"));
        CHECK(std::filesystem::exists(path));
    }
    CHECK_FALSE(std::filesystem::exists(path));
}
