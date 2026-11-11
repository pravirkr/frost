#include <filesystem>
#include <fstream>
#include <limits>
#include <sstream>
#include <string>
#include <vector>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <frost/output/cand_writer.hpp>
#include <frost/search/candidate.hpp>

#include "support/temp_dir.hpp"

using frost::SizeType;
using frost::output::CandWriter;
using frost::search::Giant;

namespace {
Giant make_giant() {
    Giant g;
    g.snr        = 12.5F;
    g.samp_idx   = 100;
    g.filter_idx = 3;
    g.dm_idx     = 7;
    g.dm         = 42.5F;
    g.members    = 4;
    g.begin      = 98;
    g.end        = 102;
    g.f_low      = 1200.5F;
    g.f_high     = 1500.25F;
    g.chan_begin = 4;
    g.chan_end   = 19;
    return g;
}
} // namespace

TEST_CASE("CandWriter writes the standard tab-separated column "
          "layout",
          "[cand_writer]") {
    const std::filesystem::path path = "cand_writer_test.cand";
    std::filesystem::remove(path);

    {
        CandWriter writer(path, /*dt=*/0.001);
        writer.write(std::vector<Giant>{make_giant()});
    }

    std::ifstream in(path);
    std::string line;
    REQUIRE(std::getline(in, line));

    std::istringstream iss(line);
    float snr           = 0.0F;
    SizeType samp_idx   = 0;
    float time          = 0.0F;
    SizeType filter_idx = 0;
    SizeType dm_idx     = 0;
    float dm            = 0.0F;
    SizeType members    = 0;
    SizeType begin      = 0;
    SizeType end        = 0;
    float f_low         = 0.0F;
    float f_high        = 0.0F;
    SizeType chan_begin = 0;
    SizeType chan_end   = 0;
    iss >> snr >> samp_idx >> time >> filter_idx >> dm_idx >> dm >> members >>
        begin >> end >> f_low >> f_high >> chan_begin >> chan_end;

    CHECK(snr == 12.5F);
    CHECK(samp_idx == 100);
    CHECK(time == 0.1F);
    CHECK(filter_idx == 3);
    CHECK(dm_idx == 7);
    CHECK(dm == 42.5F);
    CHECK(members == 4);
    CHECK(begin == 98);
    CHECK(end == 102);
    CHECK(f_low == 1200.5F);
    CHECK(f_high == 1500.25F);
    CHECK(chan_begin == 4);
    CHECK(chan_end == 19);

    // A row that never went through sub-band dedup repeats its own S/N and
    // extent, and reports one extent.
    float snr_best      = 0.0F;
    SizeType best_begin = 99;
    SizeType best_end   = 99;
    SizeType n_extents  = 0;
    iss >> snr_best >> best_begin >> best_end >> n_extents;
    CHECK(snr_best == 12.5F);
    CHECK(best_begin == 4);
    CHECK(best_end == 19);
    CHECK(n_extents == 1);

    std::filesystem::remove(path);
}

TEST_CASE("CandWriter writes the strongest sub-band of a deduplicated row",
          "[cand_writer]") {
    const auto dir    = frost::test::unique_temp_dir("cand_best");
    const auto path   = dir / "best.cand";
    auto g            = make_giant();
    g.snr_best        = 40.0F;
    g.best_chan_begin = 0;
    g.best_chan_end   = 3;
    g.n_extents       = 3;
    {
        CandWriter writer(path, 0.001);
        writer.write(std::vector<Giant>{g});
    }
    std::ifstream in(path);
    std::string line;
    REQUIRE(std::getline(in, line));
    std::istringstream iss(line);
    std::vector<std::string> cols;
    for (std::string c; iss >> c;) {
        cols.push_back(c);
    }
    REQUIRE(cols.size() == 23);
    CHECK(cols[13] == "40");
    CHECK(cols[14] == "0");
    CHECK(cols[15] == "3");
    CHECK(cols[16] == "3");
}

TEST_CASE("CandWriter preserves and offsets the strongest member's coordinates",
          "[cand_writer]") {
    const auto dir    = frost::test::unique_temp_dir("cand_location");
    const auto path   = dir / "best.cand";
    auto g            = make_giant();
    g.snr_best        = 30;
    g.best_chan_begin = 8;
    g.best_chan_end   = 15;
    g.location_best =
        frost::search::CandidateLocation{120, 80.5F, 16, 1300, 1400};
    {
        CandWriter writer(path, 0.001);
        writer.write(std::vector<Giant>{g}, 1000);
    }
    std::ifstream in(path);
    std::string line;
    REQUIRE(std::getline(in, line));
    std::istringstream iss(line);
    std::vector<std::string> cols;
    for (std::string column; iss >> column;)
        cols.push_back(column);
    REQUIRE(cols.size() == 23);
    CHECK(cols[1] == "1100");
    CHECK(cols[5] == "42.5");
    CHECK(cols[17] == "1120");
    CHECK(std::stod(cols[18]) == Catch::Approx(1.12));
    CHECK(cols[19] == "80.5");
    CHECK(cols[20] == "16");
    CHECK(cols[21] == "1300");
    CHECK(cols[22] == "1400");
    std::filesystem::remove_all(dir);
}

TEST_CASE("CandWriter offsets sample-index columns by first_samp_idx",
          "[cand_writer]") {
    const std::filesystem::path path = "cand_writer_test_offset.cand";
    std::filesystem::remove(path);

    {
        CandWriter writer(path, /*dt=*/0.001);
        writer.write(std::vector<Giant>{make_giant()}, /*first_samp_idx=*/1000);
    }

    std::ifstream in(path);
    std::string line;
    REQUIRE(std::getline(in, line));
    std::istringstream iss(line);
    float snr         = 0.0F;
    SizeType samp_idx = 0;
    iss >> snr >> samp_idx;
    CHECK(samp_idx == 1100); // 1000 + 100

    std::filesystem::remove(path);
}

TEST_CASE("CandWriter appends across multiple write() calls", "[cand_writer]") {
    const std::filesystem::path path = "cand_writer_test_append.cand";
    std::filesystem::remove(path);

    {
        CandWriter writer(path, 0.001);
        writer.write(std::vector<Giant>{make_giant()});
        writer.write(std::vector<Giant>{make_giant()});
    }

    std::ifstream in(path);
    SizeType line_count = 0;
    std::string line;
    while (std::getline(in, line)) {
        ++line_count;
    }
    CHECK(line_count == 2);

    std::filesystem::remove(path);
}

namespace {
SizeType count_lines(const std::filesystem::path& path) {
    std::ifstream in(path);
    SizeType n = 0;
    std::string line;
    while (std::getline(in, line)) {
        ++n;
    }
    return n;
}
} // namespace

TEST_CASE("CandWriter truncates a stale file by default and appends in "
          "kAppend mode",
          "[cand_writer]") {
    const std::filesystem::path path = "cand_writer_test_mode.cand";
    std::filesystem::remove(path);

    for (int run = 0; run < 2; ++run) {
        CandWriter writer(path, 0.001);
        writer.write(std::vector<Giant>{make_giant()});
    }
    CHECK(count_lines(path) == 1);

    {
        CandWriter writer(path, 0.001F, CandWriter::OpenMode::kAppend);
        writer.write(std::vector<Giant>{make_giant()});
    }
    CHECK(count_lines(path) == 2);

    std::filesystem::remove(path);
}

TEST_CASE("CandWriter write() flushes so rows are visible before "
          "destruction",
          "[cand_writer]") {
    const std::filesystem::path path = "cand_writer_test_flush.cand";
    std::filesystem::remove(path);

    CandWriter writer(path, 0.001);
    writer.write(std::vector<Giant>{make_giant()});
    CHECK(count_lines(path) == 1);

    std::filesystem::remove(path);
}

TEST_CASE("cand_filename renders the gulp start in UTC with a "
          "2-digit beam",
          "[cand_writer]") {
    // MJD 58000.0 = 2017-09-04 00:00:00 UTC.
    CHECK(frost::output::cand_filename(1504483200, 3) ==
          "2017-09-04-00:00:00_03.cand");
    CHECK(frost::output::cand_filename(1504483200 + 3661, 13) ==
          "2017-09-04-01:01:01_13.cand");
}

TEST_CASE("candidate filenames reject unrepresentable UTC calendars",
          "[cand_writer][contracts]") {
    if constexpr (std::numeric_limits<std::time_t>::digits > 32) {
        CHECK_THROWS_AS(frost::output::cand_filename(
                            std::numeric_limits<std::time_t>::max(), 0),
                        std::invalid_argument);
        CHECK_THROWS_AS(frost::output::cand_filename(
                            std::numeric_limits<std::time_t>::lowest(), 0),
                        std::invalid_argument);
    }
}

TEST_CASE("CandWriter creates missing output directories", "[cand_writer]") {
    const auto dir  = frost::test::unique_temp_dir("cand_writer_dir");
    const auto path = dir / "nested" / "deeper" / "x.cand";
    {
        CandWriter writer(path, 0.001);
        writer.write(std::vector<Giant>{make_giant()});
    }
    CHECK(std::filesystem::exists(path));
    std::filesystem::remove_all(dir);
}

TEST_CASE("CandWriter keeps sub-millisecond time at large sample indices",
          "[cand_writer]") {
    // 1e8 samples * 123.456789 us = 12345.6789 s. Six significant digits
    // (a float, or the stream default) would print "12345.7"; the double
    // time printed with 10 digits keeps the 0.1 ms part.
    const auto dir  = frost::test::unique_temp_dir("cand_writer_time");
    const auto path = dir / "t.cand";
    {
        CandWriter writer(path, 0.000'123'456'789);
        auto g     = make_giant();
        g.samp_idx = 0;
        g.begin    = 0;
        g.end      = 0;
        writer.write(std::vector<Giant>{g}, /*first_samp_idx=*/100'000'000);
    }
    std::ifstream in(path);
    std::string line;
    REQUIRE(std::getline(in, line));
    std::istringstream iss(line);
    std::string snr;
    std::string samp;
    std::string time;
    iss >> snr >> samp >> time;
    CHECK(samp == "100000000");
    CHECK(time == "12345.6789");
    std::filesystem::remove_all(dir);
}
