#include <cmath>
#include <cstddef>
#include <cstring>
#include <filesystem>
#include <string>
#include <system_error>
#include <vector>

#include <dmt/dmt.hpp>
#include <fmt/format.h>
#include <psrio/formats/memory.hpp>
#include <unistd.h>

#include <frost/dedisp/dedisp.hpp>
#include <frost/pipeline/benchmark.hpp>
#include <frost/pipeline/pipeline.hpp>

namespace frost::pipeline {

namespace {

constexpr SizeType kNChan      = 512;
constexpr SizeType kNSamps     = 515625; // 33 s at 64 us
constexpr float kTsamp         = 64.0e-6F;
constexpr double kF0           = 1535.5; // centre of the top channel
constexpr double kDf           = -1.0;
constexpr float kDmInject      = 100.0F;
constexpr SizeType kPulseStart = 100000;
constexpr SizeType kPulseWidth = 8;
constexpr int kPulseAmplitude  = 80;
constexpr double kNoiseMean    = 32.0;
constexpr double kNoiseSigma   = 4.0;
constexpr uint64_t kNoiseSeed  = 0x46524F5354ULL;

struct Rng {
    explicit Rng(uint64_t seed) : state(seed) {}

    uint64_t next() {
        state ^= state >> 12U;
        state ^= state << 25U;
        state ^= state >> 27U;
        return state * 0x2545F4914F6CDD1DULL;
    }

    double uniform() {
        // (0, 1) from the top 53 bits. Never returns 0.
        const double u =
            static_cast<double>(next() >> 11U) * (1.0 / 9007199254740992.0);
        return u > 0.0 ? u : 1.0e-12;
    }

    double normal() {
        const double u1 = uniform();
        const double u2 = uniform();
        return std::sqrt(-2.0 * std::log(u1)) *
               std::cos(6.283185307179586 * u2);
    }

    uint64_t state;
};

uint8_t clip_u8(double value) {
    const auto rounded = std::llround(value);
    if (rounded < 0) {
        return 0;
    }
    if (rounded > 255) {
        return 255;
    }
    return static_cast<uint8_t>(rounded);
}

std::vector<uint8_t> make_samples() {
    std::vector<uint8_t> samples(kNChan * kNSamps);
    Rng rng(kNoiseSeed);
    for (auto& sample : samples) {
        sample = clip_u8(kNoiseMean + (kNoiseSigma * rng.normal()));
    }
    for (SizeType chan = 0; chan < kNChan; ++chan) {
        const double freq  = kF0 + (static_cast<double>(chan) * kDf);
        const double inv   = (1.0 / (freq * freq)) - (1.0 / (kF0 * kF0));
        const double delay = (static_cast<double>(kDmInject) *
                              static_cast<double>(dmt::kDispConst) * inv) /
                             static_cast<double>(kTsamp);
        const auto shift =
            static_cast<SizeType>(std::llround(delay > 0.0 ? delay : 0.0));
        for (SizeType w = 0; w < kPulseWidth; ++w) {
            const SizeType t = kPulseStart + shift + w;
            if (t >= kNSamps) {
                break;
            }
            auto& sample     = samples[(t * kNChan) + chan];
            const int raised = static_cast<int>(sample) + kPulseAmplitude;
            sample           = raised > 255 ? static_cast<uint8_t>(255)
                                            : static_cast<uint8_t>(raised);
        }
    }
    return samples;
}

std::string workload_text(bool async_io) {
    return fmt::format(
        "benchmark: {} channels, 8-bit, tsamp {:.6g} s, {} samples (33 s), "
        "f0 {:.1f} MHz, df {:.1f} MHz, DM 0-1000, boxcar_max 4096, wtsp 2, "
        "threshold 6, baseline 2 s, RFI off, async_io {}, "
        "noise seed {:#x}, injected DM {} width {} at sample {}\n",
        kNChan, kTsamp, kNSamps, kF0, kDf, async_io ? "on" : "off", kNoiseSeed,
        kDmInject, kPulseWidth, kPulseStart);
}

} // namespace

BenchmarkReport run_benchmark(config::PipelineConfig cfg) {
    cfg.rfi                     = {};
    cfg.rfi.narrow.enabled      = false;
    cfg.rfi.broad.enabled       = false;
    cfg.dedisp.dm_min           = 0.0F;
    cfg.dedisp.dm_max           = 1000.0F;
    cfg.dedisp.dm_tol           = 1.25F;
    cfg.dedisp.dm_pulse_width   = 40.0F;
    cfg.dedisp.dm_dtype         = DataType::kFloat32;
    cfg.search.boxcar_max       = 4096;
    cfg.search.peak_mode        = dmt::PeakFindingMode::kContiguousPeakPerWidth;
    cfg.search.max_candidates   = 131'072;
    cfg.search.max_cands_per_dm = 4096;
    cfg.search.wtsp             = 2.0F;
    cfg.search.detect_thresh    = 6.0F;
    cfg.search.cand_sep_time    = 3;
    cfg.search.cand_sep_filter  = 3;
    cfg.search.cand_sep_dm      = 200;
    cfg.search.max_giant_rate   = 0.0F;
    cfg.search.cluster          = true;
    cfg.search.subband_levels   = 0;
    cfg.nsamps_gulp             = 262144;
    cfg.baseline_length         = 2.0F;
    cfg.profile                 = true;

    // The benchmark writes candidates only to keep the writer in the profile.
    // Give it a private directory: the user's `--output-dir` may hold a real
    // candidates.cand that a truncating open would destroy.
    const auto scratch =
        std::filesystem::temp_directory_path() /
        fmt::format("frost_benchmark_{}", static_cast<long>(::getpid()));
    std::filesystem::create_directories(scratch);
    struct ScratchCleanup {
        std::filesystem::path path;
        ~ScratchCleanup() {
            std::error_code ec;
            std::filesystem::remove_all(path, ec);
        }
    } cleanup{scratch};
    cfg.output.output_dir     = scratch.string();
    cfg.output.per_gulp_files = false;

    psrio::MemoryInfo info;
    info.nchans        = kNChan;
    info.nbits         = 8;
    info.beam          = 1;
    info.tsamp         = static_cast<double>(kTsamp);
    info.fch1          = kF0;
    info.foff          = kDf;
    const auto samples = make_samples();
    std::vector<std::byte> packed(samples.size());
    std::memcpy(packed.data(), samples.data(), samples.size());
    psrio::BlockSource source(psrio::MemoryBlock(info, std::move(packed)));

    Pipeline pipeline(cfg, source);
    while (pipeline.execute() != 0) {
    }

    BenchmarkReport report;
    report.profile = pipeline.profile();
    report.text =
        workload_text(cfg.async_io) + format_run_profile(report.profile);
    return report;
}

} // namespace frost::pipeline
