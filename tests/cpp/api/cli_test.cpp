// End-to-end checks of the `frost` binary's error handling and exit codes.
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <utility>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>
#include <sys/wait.h>
#include <toml++/toml.h>

#include "support/fake_filterbank.hpp"
#include "support/temp_dir.hpp"

using Catch::Matchers::ContainsSubstring;

namespace {

struct Cmd {
    int status = 0;
    std::string output;
};

Cmd run_frost(const std::string& args) {
    const std::string command =
        std::string(FROST_APP_PATH) + " " + args + " 2>&1";
    Cmd result;
    FILE* pipe = popen(command.c_str(), "r");
    REQUIRE(pipe != nullptr);
    char buf[512];
    while (fgets(buf, sizeof(buf), pipe) != nullptr) {
        result.output += buf;
    }
    const int closed = pclose(pipe);
    result.status    = WEXITSTATUS(closed);
    return result;
}

std::string small_filterbank(const std::filesystem::path& dir) {
    frost::testing::FakeFilterbankSpec spec;
    spec.nchans      = 16;
    spec.nsamps      = 4096;
    spec.noise_sigma = 5.0F;
    return frost::testing::write_fake_filterbank((dir / "in.fil").string(),
                                                 spec, {});
}

const char* kFast =
    "--dm-max 20 --boxcar-max 16 --nsamps-gulp 2048 --baseline-length 0.5";

} // namespace

TEST_CASE("frost reports a bad --config instead of aborting", "[cli][robust]") {
    const auto dir = frost::test::unique_temp_dir("cli_cfg");
    {
        std::ofstream out(dir / "typo.toml");
        out << "nsamps_guls = 4096\n";
    }
    const auto typo =
        run_frost("--config " + (dir / "typo.toml").string() + " --file x.fil");
    CHECK(typo.status == 1);
    CHECK_THAT(typo.output, ContainsSubstring("nsamps_guls"));

    const auto missing =
        run_frost("--config " + (dir / "none.toml").string() + " --file x.fil");
    CHECK(missing.status == 1);
    CHECK_THAT(missing.output, ContainsSubstring("frost: error"));
}

TEST_CASE("frost rejects values that would never terminate or overflow",
          "[cli][robust]") {
    const auto dir = frost::test::unique_temp_dir("cli_bad");
    const auto fil = small_filterbank(dir);
    const auto base =
        "--file " + fil + " --output-dir " + (dir / "o").string() + " ";
    const auto tol1 = run_frost(base + kFast + " --dm-tol 1");
    CHECK(tol1.status == 1);
    CHECK_THAT(tol1.output, ContainsSubstring("dm_tol"));
    const auto tol_half = run_frost(
        base + "--dm-max 20 --boxcar-max 16 --nsamps-gulp 2048 --dm-tol 0.5");
    CHECK(tol_half.status == 1);
    CHECK_THAT(tol_half.output, ContainsSubstring("dm_tol"));
    // boxcar_max must stay below the gulp length.
    const auto box =
        run_frost(base + "--dm-max 20 --nsamps-gulp 2048 --boxcar-max 4096");
    CHECK(box.status == 1);
    CHECK_THAT(box.output, ContainsSubstring("boxcar_max"));
}

TEST_CASE("frost fails on an input with no complete sample", "[cli][robust]") {
    const auto dir = frost::test::unique_temp_dir("cli_trunc");
    const auto fil = small_filterbank(dir);
    // Keep the header and less than one 16-byte row.
    std::ifstream in(fil, std::ios::binary);
    std::vector<char> bytes((std::istreambuf_iterator<char>(in)),
                            std::istreambuf_iterator<char>());
    const std::size_t header = bytes.size() - (4096 * 16);
    {
        std::ofstream out(dir / "t.fil", std::ios::binary);
        out.write(bytes.data(), static_cast<std::streamsize>(header + 5));
    }
    const auto r = run_frost("--file " + (dir / "t.fil").string() + " " +
                             kFast + " --output-dir " + (dir / "o").string());
    CHECK(r.status == 1);
    CHECK_THAT(r.output, ContainsSubstring("no complete samples"));
}

TEST_CASE(
    "frost reports an execution failure once and records failed provenance",
    "[cli][robust][provenance]") {
    const auto dir  = frost::test::unique_temp_dir("cli_execution_failure");
    const auto file = dir / "nonfinite.fil";
    {
        using namespace frost::testing::detail;
        std::ofstream out(file, std::ios::binary);
        write_string_field(out, "HEADER_START");
        write_kv(out, "data_type", 1);
        write_kv(out, "nchans", 16);
        write_kv(out, "nbits", 32);
        write_kv(out, "nifs", 1);
        write_kv(out, "fch1", 1400.0);
        write_kv(out, "foff", -1.0);
        write_kv(out, "tsamp", 0.001);
        write_kv(out, "tstart", 58000.0);
        write_string_field(out, "HEADER_END");
        std::vector<std::uint32_t> samples(4096 * 16, 0x42c80000U); // 100
        samples[2048 * 16 + 2] = 0x7fc00000U;
        out.write(
            reinterpret_cast<const char*>(samples.data()),
            static_cast<std::streamsize>(samples.size() * sizeof(samples[0])));
    }
    const auto result =
        run_frost("--file " + file.string() + " " + kFast +
                  " --provenance --output-dir " + (dir / "out").string());
    CHECK(result.status == 1);
    const std::string message =
        "RFI: non-finite float32 input at sample 0, channel 2";
    const auto first = result.output.find(message);
    REQUIRE(first != std::string::npos);
    CHECK(result.output.find(message, first + message.size()) ==
          std::string::npos);
    const auto provenance =
        toml::parse_file((dir / "out" / "run.toml").string());
    CHECK(provenance["status"].value<std::string>() == "failed");
}

TEST_CASE("frost keeps a TOML verbosity when -v is absent", "[cli][robust]") {
    const auto dir = frost::test::unique_temp_dir("cli_verb");
    const auto fil = small_filterbank(dir);
    {
        std::ofstream out(dir / "v.toml");
        out << "verbosity = 1\n[input]\nsigproc_file = \"" << fil << "\"\n";
    }
    const auto toml_only =
        run_frost("--config " + (dir / "v.toml").string() + " " + kFast +
                  " --output-dir " + (dir / "o1").string());
    CHECK(toml_only.status == 0);
    CHECK_THAT(toml_only.output, ContainsSubstring("device cpu"));

    // Quiet by default.
    const auto quiet = run_frost("--file " + fil + " " + kFast +
                                 " --output-dir " + (dir / "o2").string());
    CHECK(quiet.status == 0);
    CHECK_THAT(quiet.output, !ContainsSubstring("device cpu"));
}

TEST_CASE("frost creates a missing --output-dir", "[cli][robust]") {
    const auto dir = frost::test::unique_temp_dir("cli_out");
    const auto fil = small_filterbank(dir);
    const auto out = dir / "a" / "b";
    const auto r = run_frost("--file " + fil + " " + kFast + " --output-dir " +
                             out.string());
    CHECK(r.status == 0);
    CHECK(std::filesystem::exists(out / "candidates.cand"));
}

TEST_CASE("frost lets --file replace a TOML dada_key and rejects mixed inputs",
          "[cli][robust]") {
    const auto dir = frost::test::unique_temp_dir("cli_src");
    const auto fil = small_filterbank(dir);
    {
        std::ofstream out(dir / "d.toml");
        out << "[input]\ndada_key = \"0xdada\"\n";
    }
    const auto replaced =
        run_frost("--config " + (dir / "d.toml").string() + " --file " + fil +
                  " " + kFast + " --output-dir " + (dir / "o").string());
    CHECK(replaced.status == 0);

    // Naming both on the command line, or a file with --benchmark, is an error.
    const auto both = run_frost("--file " + fil + " --dada-key 0xdada");
    CHECK(both.status == 1);
    CHECK_THAT(both.output, ContainsSubstring("exactly one"));
    const auto bench = run_frost("--benchmark --file " + fil);
    CHECK(bench.status == 1);
    CHECK_THAT(bench.output, ContainsSubstring("exactly one"));
}

TEST_CASE("frost --no-profile turns off a TOML profile", "[cli][robust]") {
    const auto dir = frost::test::unique_temp_dir("cli_prof");
    const auto fil = small_filterbank(dir);
    {
        std::ofstream out(dir / "p.toml");
        out << "profile = true\n[input]\nsigproc_file = \"" << fil << "\"\n";
    }
    const auto cfg = "--config " + (dir / "p.toml").string() + " " + kFast;
    const auto on  = run_frost(cfg + " --output-dir " + (dir / "o1").string());
    CHECK(on.status == 0);
    CHECK_THAT(on.output, ContainsSubstring("wide boxcars"));
    const auto off =
        run_frost(cfg + " --no-profile --output-dir " + (dir / "o2").string());
    CHECK(off.status == 0);
    CHECK_THAT(off.output, !ContainsSubstring("wide boxcars"));
}

TEST_CASE("frost per-gulp files from an earlier run are overwritten",
          "[cli][robust]") {
    const auto dir  = frost::test::unique_temp_dir("cli_pergulp");
    const auto fil  = small_filterbank(dir);
    const auto out  = dir / "o";
    const auto args = "--file " + fil + " " + kFast +
                      " --detect-thresh 3 --per-gulp-files --output-dir " +
                      out.string();
    const auto total_bytes = [&] {
        std::uintmax_t bytes = 0;
        std::size_t files    = 0;
        for (const auto& e : std::filesystem::directory_iterator(out)) {
            if (e.path().extension() == ".cand") {
                bytes += std::filesystem::file_size(e.path());
                ++files;
            }
        }
        return std::pair{files, bytes};
    };
    REQUIRE(run_frost(args).status == 0);
    const auto first = total_bytes();
    REQUIRE(first.first > 0);
    REQUIRE(first.second > 0);
    REQUIRE(run_frost(args).status == 0);
    CHECK(total_bytes() == first);
}

TEST_CASE("frost --version prints the release and build revisions",
          "[cli][version]") {
    const auto r = run_frost("--version");
    CHECK(r.status == 0);
    CHECK(r.output.starts_with(std::string("frost ") + FROST_PROJECT_VERSION +
                               "\n"));
    CHECK_THAT(r.output, ContainsSubstring("revision: "));
    CHECK_THAT(r.output, ContainsSubstring("dmt: "));
    CHECK_THAT(r.output, ContainsSubstring("psrio: "));
}

TEST_CASE("frost exposes fused search controls and rejects unknown flags",
          "[cli]") {
    const auto help = run_frost("--help");
    CHECK_THAT(help.output, ContainsSubstring("--peak-mode"));
    const auto dir = frost::test::unique_temp_dir("cli_peaks");
    const auto fil = small_filterbank(dir);
    for (const auto mode : {"per_width", "across_widths", "all"}) {
        const auto r = run_frost(
            "--file " + fil + " " + kFast + " --peak-mode " + mode +
            " --max-candidates 100 --max-cands-per-dm 10 --output-dir " +
            (dir / mode).string());
        CHECK(r.status == 0);
    }
    CHECK(run_frost("--peak-mode invalid").status != 0);
    CHECK(run_frost("--unknown-flag").status != 0);
}

TEST_CASE("CLI provenance is opt-in and records effective overrides",
          "[cli][provenance]") {
    const auto dir  = frost::test::unique_temp_dir("cli_provenance");
    const auto file = small_filterbank(dir);
    const auto run  = [&](const std::string& output, const std::string& extra) {
        return run_frost("--file " + file + " --output-dir " +
                         (dir / output).string() +
                         " --nsamps-gulp 4096 --boxcar-max 16 --dm-max 10 "
                         "--detect-thresh 9.5 " +
                         extra);
    };
    const auto ordinary = run("ordinary", "");
    REQUIRE(ordinary.status == 0);
    CHECK_FALSE(std::filesystem::exists(dir / "ordinary/run.toml"));
    const auto recorded = run("recorded", "--provenance");
    INFO(recorded.output);
    REQUIRE(recorded.status == 0);
    const auto metadata =
        toml::parse_file((dir / "recorded/run.toml").string());
    CHECK(metadata["status"].value<std::string>() == "complete");
    CHECK(metadata["config_effective"]["search"]["detect_thresh"]
              .value<double>() == 9.5);
    CHECK(metadata["config_effective"]["output"]["provenance"].value<bool>() ==
          true);
    CHECK(metadata["memory_plan"]["pool"].value<std::string>() == "host");
    CHECK(metadata["run"]["samples_read"].value<int64_t>().value_or(0) > 0);
    std::filesystem::remove_all(dir);
}

TEST_CASE("frost --kill-file zaps stored-order channels and checks the count",
          "[cli][rfi]") {
    const auto dir  = frost::test::unique_temp_dir("cli_kill_file");
    const auto file = small_filterbank(dir); // 16 channels
    {
        std::ofstream out(dir / "kill.txt");
        for (int c = 0; c < 16; ++c)
            out << (c == 1 || c == 2 || c == 9 ? 0 : 1) << '\n';
        std::ofstream short_file(dir / "short.txt");
        short_file << "1\n0\n";
    }
    const auto run = [&](const std::string& output, const std::string& extra) {
        return run_frost("--file " + file + " --output-dir " +
                         (dir / output).string() + " " + kFast +
                         " --provenance " + extra);
    };
    const auto zaps = [&](const std::string& output) {
        const auto metadata =
            toml::parse_file((dir / output / "run.toml").string());
        std::vector<std::pair<int64_t, int64_t>> out;
        const auto* ranges =
            metadata["config_effective"]["rfi"]["zap_chans"].as_array();
        REQUIRE(ranges != nullptr);
        for (const auto& range : *ranges) {
            const auto& pair = *range.as_array();
            out.emplace_back(pair[0].value<int64_t>().value(),
                             pair[1].value<int64_t>().value());
        }
        return out;
    };
    const auto plain =
        run("plain", "-v --kill-file " + (dir / "kill.txt").string());
    INFO(plain.output);
    REQUIRE(plain.status == 0);
    CHECK_THAT(plain.output, ContainsSubstring("3 of 16 channels killed"));
    CHECK(zaps("plain") ==
          std::vector<std::pair<int64_t, int64_t>>{{1, 2}, {9, 9}});
    // --fswap reverses the delivered channels; the file stays in stored order.
    const auto swapped =
        run("swapped", "--fswap --kill-file " + (dir / "kill.txt").string());
    INFO(swapped.output);
    REQUIRE(swapped.status == 0);
    CHECK(zaps("swapped") ==
          std::vector<std::pair<int64_t, int64_t>>{{13, 14}, {6, 6}});

    const auto short_run =
        run("short", "--kill-file " + (dir / "short.txt").string());
    CHECK(short_run.status == 1);
    CHECK_THAT(short_run.output, ContainsSubstring("the input has 16"));
    std::filesystem::remove_all(dir);
}
