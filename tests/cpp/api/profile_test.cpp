#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <psrio/block_source.hpp>
#include <psrio/formats/memory.hpp>

#include <frost/config/config.hpp>
#include <frost/dedisp/dedisp.hpp>
#include <frost/pipeline/pipeline.hpp>
#include <frost/pipeline/profile.hpp>

#include "support/temp_dir.hpp"

using frost::SizeType;

namespace {

class CountingSource {
public:
    CountingSource(SizeType nsamps, SizeType nchan, std::atomic<int>& reads)
        : m_reads(reads),
          m_nchan(nchan),
          m_nsamps(nsamps) {
        m_samples.assign(nsamps * nchan, static_cast<uint8_t>(100));
    }

    [[nodiscard]] std::uint64_t nchans() const { return m_nchan; }
    [[nodiscard]] std::uint64_t nifs() const { return 1; }
    [[nodiscard]] int nbits() const { return 8; }
    [[nodiscard]] psrio::SampleType sample_type() const {
        return psrio::SampleType::kUInt8;
    }
    [[nodiscard]] std::uint64_t bytes_per_sample() const { return m_nchan; }
    [[nodiscard]] std::uint64_t nsamples() const { return m_nsamps; }
    [[nodiscard]] bool has_nsamples() const { return true; }
    [[nodiscard]] double tsamp() const { return 0.001; }
    [[nodiscard]] double tstart() const { return 0.0; }
    [[nodiscard]] std::string_view source_name() const { return "count"; }
    [[nodiscard]] std::string_view telescope() const { return "test"; }
    [[nodiscard]] double raj() const { return 0.0; }
    [[nodiscard]] double dej() const { return 0.0; }
    [[nodiscard]] double fch1() const { return 1400.0; }
    [[nodiscard]] double foff() const { return -1.0; }
    [[nodiscard]] double bandwidth() const {
        return static_cast<double>(m_nchan);
    }
    [[nodiscard]] double center_frequency() const { return 1400.0; }
    [[nodiscard]] int beam() const { return 1; }
    [[nodiscard]] double spectra_rate() const { return 1000.0; }
    [[nodiscard]] std::time_t utc_start() const { return 0; }
    [[nodiscard]] std::uint64_t tell() const { return m_cursor; }
    void set_fswap(bool) {}
    [[nodiscard]] bool fswap_enabled() const { return false; }
    void seek(std::uint64_t sample) { m_cursor = sample; }
    void rewind() { m_cursor = 0; }
    void skip(std::int64_t delta) {
        m_cursor = static_cast<std::uint64_t>(
            static_cast<std::int64_t>(m_cursor) + delta);
    }

    std::uint64_t read_block(std::uint64_t count, std::span<std::byte> dest) {
        ++m_reads;
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        if (dest.size() < count * m_nchan) {
            throw std::invalid_argument("read_block buffer is too small");
        }
        if (m_cursor >= m_nsamps || count == 0) {
            return 0;
        }
        const auto remain = m_nsamps - m_cursor;
        const auto nread  = count < remain ? count : remain;
        std::memcpy(dest.data(), m_samples.data() + (m_cursor * m_nchan),
                    static_cast<std::size_t>(nread * m_nchan));
        m_cursor += nread;
        return nread;
    }

    std::uint64_t read_samples(std::uint64_t, std::span<float>) {
        throw std::logic_error("CountingSource::read_samples is unused");
    }
    std::uint64_t read_samples(std::uint64_t, std::span<std::uint8_t>) {
        throw std::logic_error("CountingSource::read_samples is unused");
    }
    std::uint64_t read_samples(std::uint64_t, std::span<std::uint16_t>) {
        throw std::logic_error("CountingSource::read_samples is unused");
    }
    std::uint64_t read_bytes(std::uint64_t, std::span<std::byte>) {
        throw std::logic_error("CountingSource::read_bytes is unused");
    }

private:
    std::atomic<int>& m_reads;
    std::vector<uint8_t> m_samples;
    SizeType m_nchan  = 0;
    SizeType m_nsamps = 0;
    SizeType m_cursor = 0;
};

frost::config::PipelineConfig
tiny_config(const std::string& out_dir, bool async_io, SizeType gulp) {
    frost::config::PipelineConfig cfg;
    cfg.rfi.broad.enabled        = false;
    cfg.rfi.narrow.enabled       = false;
    cfg.dedisp.dm_min            = 0.0F;
    cfg.dedisp.dm_max            = 50.0F;
    cfg.dedisp.dm_tol            = 1.25F;
    cfg.dedisp.dm_pulse_width    = 1000.0F;
    cfg.search.boxcar_max        = 32;
    cfg.search.detect_thresh     = 5.0F;
    cfg.search.cand_sep_time     = 5;
    cfg.search.cand_sep_filter   = 2;
    cfg.search.cand_sep_dm       = 1'000'000;
    cfg.search.cand_sep_dm_smear = 0.0F; // trial-count merge only
    cfg.search.max_giant_rate    = 0.0F;
    cfg.nsamps_gulp              = gulp;
    cfg.baseline_length          = 1.0F;
    cfg.output.output_dir        = out_dir;
    cfg.async_io                 = async_io;
    cfg.verbosity                = 0;
    return cfg;
}

std::string read_file(const std::filesystem::path& path) {
    std::ifstream in(path);
    REQUIRE(in.is_open());
    return std::string(std::istreambuf_iterator<char>(in),
                       std::istreambuf_iterator<char>());
}

std::string run_memory(const std::vector<uint8_t>& bytes,
                       bool async_io,
                       const std::filesystem::path& dir,
                       frost::pipeline::RunProfile* profile) {
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    auto cfg = tiny_config(dir.string(), async_io, /*gulp=*/8192);
    psrio::MemoryInfo info;
    info.nchans = 32;
    info.nbits  = 8;
    info.beam   = 1;
    info.tsamp  = 0.001;
    info.fch1   = 1400.0;
    info.foff   = -1.0;
    std::vector<std::byte> packed(bytes.size());
    std::memcpy(packed.data(), bytes.data(), bytes.size());
    psrio::BlockSource source(psrio::MemoryBlock(info, std::move(packed)));
    auto dedisperser = frost::dedisp::make_dmt_fdmt_plan(
        frost::pipeline::make_dedisp_plan_config(cfg, source));
    REQUIRE(cfg.nsamps_gulp > dedisperser.get_dt_max() + cfg.search.boxcar_max);
    frost::pipeline::Pipeline pipeline(cfg, source);
    while (pipeline.execute() != 0) {
    }
    if (profile != nullptr) {
        *profile = pipeline.profile();
    }
    return read_file(dir / "candidates.cand");
}

} // namespace

TEST_CASE("format_run_profile prints realtime and wide boxcars", "[profile]") {
    frost::pipeline::RunProfile profile;
    profile.wall_s       = 2.0;
    profile.read_s       = 1.0;
    profile.samples_read = 1000;
    profile.tsamp        = 0.001F;
    profile.wide_boxcars = 2;
    profile.dedisp_steps.push_back({"execute", 0.25});
    const auto text = frost::pipeline::format_run_profile(profile);
    CHECK(text.find("1.0x") != std::string::npos);
    CHECK(text.find("wide boxcars") != std::string::npos);
    CHECK(text.find("execute") != std::string::npos);

    profile.read_s = 3.0;
    profile.wall_s = 2.0;
    CHECK(frost::pipeline::format_run_profile(profile).find(
              "read overlapped compute") != std::string::npos);
}

TEST_CASE("async reads the next gulp before the first execute returns",
          "[profile]") {
    constexpr SizeType kNChan  = 32;
    constexpr SizeType kNSamps = 8192;
    constexpr SizeType kGulp   = 4096;
    const auto dir             = frost::test::unique_temp_dir("profile_async");

    std::atomic<int> reads{0};
    psrio::BlockSource source(CountingSource(kNSamps, kNChan, reads));
    auto cfg         = tiny_config(dir.string(), /*async_io=*/true, kGulp);
    auto dedisperser = frost::dedisp::make_dmt_fdmt_plan(
        frost::pipeline::make_dedisp_plan_config(cfg, source));
    REQUIRE(kGulp > dedisperser.get_dt_max() + cfg.search.boxcar_max);
    frost::pipeline::Pipeline pipeline(cfg, source);
    REQUIRE(pipeline.execute() == kGulp);

    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (reads.load() < 2 && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    CHECK(reads.load() >= 2);
    CHECK(pipeline.profile().samples_read == kGulp);
    CHECK(pipeline.profile().gulps == 1);
    CHECK(pipeline.profile().read_s > 0.0);
    CHECK(pipeline.profile().dedisp_s > 0.0);
    CHECK(pipeline.profile().wall_s > 0.0);
    std::filesystem::remove_all(dir);
}

TEST_CASE("synchronous reads stay one gulp behind", "[profile]") {
    constexpr SizeType kNChan  = 32;
    constexpr SizeType kNSamps = 8192;
    constexpr SizeType kGulp   = 4096;
    const auto dir             = frost::test::unique_temp_dir("profile_sync");
    std::filesystem::create_directories(dir);

    std::atomic<int> reads{0};
    psrio::BlockSource source(CountingSource(kNSamps, kNChan, reads));
    auto cfg         = tiny_config(dir.string(), /*async_io=*/false, kGulp);
    auto dedisperser = frost::dedisp::make_dmt_fdmt_plan(
        frost::pipeline::make_dedisp_plan_config(cfg, source));
    frost::pipeline::Pipeline pipeline(cfg, source);
    REQUIRE(pipeline.execute() == kGulp);
    CHECK(reads.load() == 1);
    std::filesystem::remove_all(dir);
}

TEST_CASE("async and sync runs write the same candidates", "[profile]") {
    constexpr SizeType kNChan      = 32;
    constexpr SizeType kNSamps     = 8192;
    constexpr SizeType kPulseStart = 4000;
    constexpr SizeType kPulseWidth = 4;
    std::vector<uint8_t> bytes(kNChan * kNSamps, 100);
    for (SizeType t = kPulseStart; t < kPulseStart + kPulseWidth; ++t) {
        for (SizeType c = 0; c < kNChan; ++c) {
            bytes[(t * kNChan) + c] = 160;
        }
    }

    const auto root = frost::test::unique_temp_dir("profile_equal");
    frost::pipeline::RunProfile profile;
    const auto async_cand = run_memory(bytes, true, root / "async", &profile);
    const auto sync_cand  = run_memory(bytes, false, root / "sync", nullptr);
    CHECK(async_cand == sync_cand);
    REQUIRE_FALSE(async_cand.empty());
    CHECK(profile.samples_read == kNSamps);
    CHECK(profile.gulps == 1);
    CHECK(profile.dedisp_s > 0.0);
    CHECK(profile.read_s > 0.0);
    CHECK(profile.wall_s > 0.0);
    CHECK(profile.candidates >= 1);
    CHECK(profile.dm_trials >= 1);
    std::filesystem::remove_all(root);
}
