#include <filesystem>
#include <fstream>
#include <sstream>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <toml++/toml.h>

#include <frost/output/provenance.hpp>
#include <frost/pipeline/input.hpp>

#include "support/fake_filterbank.hpp"
#include "support/temp_dir.hpp"

TEST_CASE("provenance records effective config and updates run outcome",
          "[provenance]") {
    const auto dir = frost::test::unique_temp_dir("provenance");
    frost::testing::FakeFilterbankSpec spec;
    spec.nchans = 16;
    spec.nsamps = 128;
    frost::config::PipelineConfig cfg;
    cfg.input.sigproc_file = frost::testing::write_fake_filterbank(
        (dir / "input.fil").string(), spec, {});
    cfg.output.provenance    = true;
    cfg.search.detect_thresh = 9.5F;
    auto source              = frost::pipeline::open_input(cfg.input);
    const std::vector<std::string> command{
        "frost",           "--file", cfg.input.sigproc_file,
        "--detect-thresh", "9.5",    "--provenance"};
    const auto path = dir / "run.toml";
    frost::output::RunProvenancePlan writer(path, cfg, source, command);
    writer.execute("started", {});
    CHECK(toml::parse_file(path.string())["status"].value<std::string>() ==
          "started");
    frost::pipeline::RunProfile profile;
    profile.samples_read = 128;
    profile.candidates   = 3;
    writer.execute("complete", profile);
    auto record = toml::parse_file(path.string());
    CHECK(record["status"].value<std::string>() == "complete");
    CHECK(record["run"]["candidates"].value<int64_t>() == 3);
    CHECK(record["input"]["nchans"].value<int64_t>() == 16);
    REQUIRE(record["build"]["frost_revision"].value<std::string>().has_value());
    std::ostringstream config_text;
    config_text << *record["config_effective"].as_table();
    CHECK(frost::config::PipelineConfig::from_toml_string(config_text.str()) ==
          cfg);
    CHECK(record["argv"].as_array()->size() == command.size());
    writer.execute("failed", {}, "read error");
    record = toml::parse_file(path.string());
    CHECK(record["status"].value<std::string>() == "failed");
    CHECK(record["error"].value<std::string>() == "read error");
    CHECK_FALSE(record.contains("run"));
    std::filesystem::remove_all(dir);
}
