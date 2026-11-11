#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iomanip>
#include <iterator>
#include <random>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <dmt/algorithms/fdmt.hpp>
#include <dmt/algorithms/fdmt_boxcar.hpp>

#include <frost/dedisp/dedisp.hpp>
#include <frost/prepare/rfi.hpp>
#include <frost/search/postprocess.hpp>

namespace frost::test::calibration {
using frost::SizeType;
using namespace frost::prepare::rfi;

inline constexpr SizeType kChannels     = 32;
inline constexpr SizeType kGulp         = 4096;
inline constexpr double kTsamp          = 0.001;
inline constexpr double kFirstFrequency = 800.0;
inline constexpr double kChannelStep    = 8.0;
inline constexpr uint32_t kSeed         = 0xCA11B;

inline frost::dedisp::DedispPlanConfig observing_setup() {
    frost::dedisp::DedispPlanConfig cfg;
    cfg.nchans         = kChannels;
    cfg.dt             = kTsamp;
    cfg.f0             = kFirstFrequency;
    cfg.df             = kChannelStep;
    cfg.dm_max         = 100.0F;
    cfg.dm_pulse_width = 1000.0F;
    cfg.block_nsamps   = kGulp;
    cfg.subband_depth  = 2;
    return cfg;
}

inline BlockLayout layout() {
    return {kChannels, kGulp,           frost::DataType::kFloat32,
            kTsamp,    kFirstFrequency, kChannelStep};
}

struct Scenario {
    std::string name;
    RfiConfig config;
    bool known_gain = false;
};

inline std::vector<Scenario> scenarios() {
    RfiConfig quiet;
    quiet.narrow.enabled = false;
    quiet.broad.enabled  = false;
    std::vector<Scenario> out{{"unmasked", quiet}};
    auto cfg = quiet;
    for (SizeType c = 0; c < kChannels; c += 2) {
        cfg.zap_chans.push_back({c, c});
    }
    out.push_back({"alternating_mask", cfg});
    cfg           = quiet;
    cfg.zap_chans = {{0, kChannels / 2 - 1}};
    out.push_back({"half_band_mask", cfg});
    cfg.zap_chans = {{0, kChannels - 1}};
    out.push_back({"all_masked", cfg});
    cfg         = quiet;
    cfg.zero_dm = ZeroDMMode::kSubtract;
    out.push_back({"zero_dm", cfg});
    cfg.zero_dm = ZeroDMMode::kZdot;
    out.push_back({"zdot", cfg});
    cfg          = quiet;
    cfg.equalize = true;
    out.push_back({"equalize", cfg});
    out.push_back({"default_rfi", RfiConfig{}});
    cfg                 = RfiConfig{};
    cfg.moments.enabled = true;
    cfg.iqrm.enabled    = true;
    out.push_back({"moments_iqrm", cfg});
    return out;
}

// Gaussian draws are independent in time/channel. Strongly different channel
// gains/offsets exercise calibration rather than supplying pre-whitened input.
inline float gain(SizeType c) { return 1.0F + static_cast<float>(c % 7); }
inline float offset(SizeType c) {
    return 100.0F + 13.0F * static_cast<float>(c);
}

inline std::vector<float> noise_block(std::mt19937& rng,
                                      SizeType valid = kGulp) {
    std::normal_distribution<float> gaussian;
    std::vector<float> out(kChannels * kGulp, 0.0F);
    for (SizeType t = 0; t < valid; ++t) {
        for (SizeType c = 0; c < kChannels; ++c) {
            out[t * kChannels + c] = offset(c) + gain(c) * gaussian(rng);
        }
    }
    return out;
}

inline std::span<uint8_t> bytes(std::vector<float>& block) {
    return {reinterpret_cast<uint8_t*>(block.data()),
            block.size() * sizeof(float)};
}

// Optional study artifacts. Each test owns a different file, so CTest may run
// them concurrently. Fail visibly if an explicitly requested artifact cannot
// be written. No files are written during ordinary test runs.
inline std::ofstream csv(const char* name) {
    const char* dir = std::getenv("FROST_CALIBRATION_OUTPUT");
    if (dir == nullptr || *dir == '\0') {
        return {};
    }
    std::filesystem::create_directories(dir);
    std::ofstream out(std::filesystem::path(dir) / name);
    REQUIRE(out.good());
    out << std::setprecision(10);
    return out;
}

struct Moments {
    uint64_t count         = 0;
    uint64_t positive_tail = 0;
    uint64_t negative_tail = 0;
    double sum             = 0;
    double sumsq           = 0;
    void add(double value) {
        ++count;
        sum += value;
        sumsq += value * value;
        positive_tail += value > 3.0;
        negative_tail += value < -3.0;
    }
    double mean() const { return sum / static_cast<double>(count); }
    double rms() const { return std::sqrt(sumsq / static_cast<double>(count)); }
    double sigma() const {
        return std::sqrt(std::max(0.0, sumsq / static_cast<double>(count) -
                                           mean() * mean()));
    }
};

struct NoiseTrial {
    SizeType plane;
    SizeType dm;
    SizeType filter;
    Moments values;
    double predicted_variance_sum = 0;
    bool active                   = false; // Some gulp left it channels.
};
struct Injection {
    const char* name;
    SizeType start;
    SizeType width;
    float dm;
    SizeType first_channel;
    SizeType end_channel;
    float amplitude;
};

inline constexpr Injection kPulses[] = {
    {"interior_dm0", 1000, 4, 0, 0, kChannels, 3},
    {"interior_dm80", 2300, 4, 80, 0, kChannels, 3},
    {"boundary_dm80", kGulp - 2, 4, 80, 0, kChannels, 3},
    {"staggered_dm80", 7000, 16, 80, 8, 24, 2},
    {"short_tail_dm0", 2 * kGulp + 8, 4, 0, 0, kChannels, 3},
};

// Independent centre-frequency dispersion law, not the engine's delay table.
inline SizeType delay(const Injection& pulse, double frequency) {
    constexpr double kDispersion = 4148.808; // seconds MHz^2 / (pc cm^-3)
    constexpr double kTop = kFirstFrequency + (kChannels - 1) * kChannelStep;
    return static_cast<SizeType>(std::llround(
        kDispersion * pulse.dm *
        (1 / (frequency * frequency) - 1 / (kTop * kTop)) / kTsamp));
}

inline bool matches(const Injection& pulse,
                    SizeType sample,
                    float dm,
                    float f_low,
                    float f_high,
                    SizeType width) {
    // Require substantial overlap with the injected band. A DM-time match
    // in a disjoint noise-only plane must never count as recovery.
    const double lo =
        kFirstFrequency +
        (static_cast<double>(pulse.first_channel) - 0.5) * kChannelStep;
    const double hi =
        kFirstFrequency +
        (static_cast<double>(pulse.end_channel) - 0.5) * kChannelStep;
    if (std::min<double>(f_high, hi) - std::max<double>(f_low, lo) <
        (hi - lo) / 2) {
        return false;
    }
    const auto expected =
        pulse.start + delay(pulse, f_low + kChannelStep / 2) + pulse.width - 1;
    const auto separation =
        std::abs(static_cast<double>(sample) - static_cast<double>(expected));
    return std::abs(dm - pulse.dm) < 10.0F &&
           separation <= 2 * std::max(width, pulse.width) + 16;
}

struct Recovery {
    double raw_snr          = 0;
    double final_snr        = 0;
    SizeType raw_matches    = 0;
    SizeType final_matches  = 0;
    SizeType best_matches   = 0;
    double best_matched_snr = 0;
    dmt::RawCandidate raw_peak;
};

enum class NoiseModel { kWhite, kAr1, kGainRamp, kTailGainStep };

inline std::string_view model_name(NoiseModel model) {
    switch (model) {
    case NoiseModel::kWhite:
        return "white";
    case NoiseModel::kAr1:
        return "ar1";
    case NoiseModel::kGainRamp:
        return "gain_ramp";
    case NoiseModel::kTailGainStep:
        return "tail_gain_step";
    }
    throw std::invalid_argument("unknown calibration noise model");
}

inline float
input_gain(NoiseModel model, SizeType t, SizeType c, SizeType total) {
    const float factor =
        model == NoiseModel::kGainRamp
            ? 1.0F + static_cast<float>(t) / static_cast<float>(total - 1)
        : model == NoiseModel::kTailGainStep && t >= total - 37 ? 2.0F
                                                                : 1.0F;
    return gain(c) * factor;
}

// Stationary, independent channels. AR(1) state persists across gulps; gain
// models modulate the noise and the injected signal together.
class NoiseStream {
public:
    NoiseStream(uint32_t seed, NoiseModel model, SizeType total)
        : m_rng(seed),
          m_model(model),
          m_total(total) {
        if (model == NoiseModel::kAr1) {
            std::normal_distribution<float> gaussian;
            for (auto& value : m_previous)
                value = gaussian(m_rng);
        }
    }

    std::vector<float> next(SizeType begin, SizeType valid) {
        auto raw = noise_block(m_rng, valid);
        if (m_model == NoiseModel::kWhite)
            return raw;
        for (SizeType t = 0; t < valid; ++t) {
            for (SizeType c = 0; c < kChannels; ++c) {
                float z = (raw[t * kChannels + c] - offset(c)) / gain(c);
                if (m_model == NoiseModel::kAr1) {
                    // rho=0.8, sqrt(1-rho^2)=0.6, marginal variance one.
                    z             = 0.8F * m_previous[c] + 0.6F * z;
                    m_previous[c] = z;
                }
                raw[t * kChannels + c] =
                    offset(c) + input_gain(m_model, begin + t, c, m_total) * z;
            }
        }
        return raw;
    }

private:
    std::mt19937 m_rng;
    NoiseModel m_model;
    SizeType m_total;
    std::array<float, kChannels> m_previous{};
};

inline void add_rfi(std::vector<float>& raw,
                    NoiseModel model,
                    SizeType begin,
                    SizeType valid,
                    SizeType total) {
    for (SizeType t = 0; t < valid; ++t) {
        const auto absolute = begin + t;
        for (SizeType c = 0; c < kChannels; ++c) {
            // A narrow-band patch overlaps one dispersed pulse channel;
            // a four-row broadband burst overlaps the boundary sweep.
            if ((c == 12 && absolute >= 2400 && absolute < 2440) ||
                (absolute >= 4100 && absolute < 4104)) {
                raw[t * kChannels + c] +=
                    12.0F * input_gain(model, absolute, c, total);
            }
        }
    }
}

struct Observation {
    SizeType nsamps = 2 * kGulp + 37;
    std::vector<float> raw;
    std::vector<float> known_gain;
};

inline Observation make_observation(uint32_t seed,
                                    float amplitude_scale,
                                    NoiseModel model = NoiseModel::kWhite,
                                    bool corrupt     = false) {
    Observation out;
    out.raw.resize(out.nsamps * kChannels);
    out.known_gain.resize(out.raw.size());
    NoiseStream noise(seed, model, out.nsamps);
    for (SizeType begin = 0; begin < out.nsamps; begin += kGulp) {
        const auto valid = std::min(kGulp, out.nsamps - begin);
        auto raw         = noise.next(begin, valid);
        if (corrupt)
            add_rfi(raw, model, begin, valid, out.nsamps);
        for (const auto& pulse : kPulses) {
            for (SizeType c = pulse.first_channel; c < pulse.end_channel; ++c) {
                const auto start =
                    pulse.start +
                    delay(pulse, kFirstFrequency + c * kChannelStep);
                for (SizeType t = std::max(begin, start);
                     t < std::min(begin + valid, start + pulse.width); ++t) {
                    raw[(t - begin) * kChannels + c] +=
                        amplitude_scale * pulse.amplitude *
                        input_gain(model, t, c, out.nsamps);
                }
            }
        }
        for (SizeType t = 0; t < valid; ++t) {
            for (SizeType c = 0; c < kChannels; ++c) {
                const auto i      = (begin + t) * kChannels + c;
                out.raw[i]        = raw[t * kChannels + c];
                out.known_gain[i] = (out.raw[i] - offset(c)) /
                                    input_gain(model, begin + t, c, out.nsamps);
            }
        }
    }
    return out;
}

struct RecoveryRun {
    std::vector<Recovery> pulses;
    SizeType raw_count = 0, final_count = 0, masked = 0, broad = 0;
    SizeType narrow = 0, fallbacks = 0, reused = 0;
};
// The same public stages and settings drive baseline and stress studies.
inline RecoveryRun run_recovery(
    const Scenario& scenario,
    const Observation& observation,
    const std::function<void(const frost::search::Giant&)>& record = {}) {
    const auto fdmt_plan = frost::dedisp::make_dmt_fdmt_plan(observing_setup());
    frost::config::SearchConfig cfg;
    cfg.boxcar_max       = 16;
    cfg.detect_thresh    = 6;
    cfg.max_candidates   = 50'000;
    cfg.max_cands_per_dm = 0;
    cfg.subband_levels   = 2;
    dmt::BoxcarConfig search;
    search.max_boxcar       = cfg.boxcar_max;
    search.detect_thresh    = cfg.detect_thresh;
    search.max_candidates   = cfg.max_candidates;
    search.max_cands_per_dm = cfg.max_cands_per_dm;
    dmt::algorithms::FDMTBoxcar engine(fdmt_plan, search);
    RFICleaner cleaner(scenario.config, layout(), 2.0F);
    frost::search::CandidatePostprocessPlan postprocess(
        cfg, {kChannels, kTsamp, kChannelStep});
    std::vector<Recovery> recovered(std::size(kPulses));
    SizeType raw_count = 0, final_count = 0, masked = 0, broad = 0;
    auto consume = [&](dmt::CandidateResultView found, SizeType end,
                       SizeType duration, bool final) {
        REQUIRE_FALSE(found.overflowed());
        raw_count += found.candidates.size();
        for (const auto& peak : found.candidates) {
            for (SizeType i = 0; i < recovered.size(); ++i) {
                if (matches(kPulses[i], peak.peak_sample, peak.dm,
                            peak.f_low_mhz, peak.f_high_mhz, peak.width)) {
                    ++recovered[i].raw_matches;
                    if (peak.snr > recovered[i].raw_snr) {
                        recovered[i].raw_snr  = peak.snr;
                        recovered[i].raw_peak = peak;
                    }
                }
            }
        }
        const auto result = postprocess.execute(
            found, end, duration, final, engine.get_candidate_frontier());
        if (final) {
            REQUIRE(result.pending_candidates == 0);
        }
        final_count += result.candidates.size();
        for (const auto& peak : result.candidates) {
            const auto best = peak.strongest_location();
            if (record)
                record(peak);
            for (SizeType i = 0; i < recovered.size(); ++i) {
                if (matches(kPulses[i], best.samp_idx, best.dm, best.f_low,
                            best.f_high, best.filter_width)) {
                    ++recovered[i].best_matches;
                    recovered[i].best_matched_snr = std::max<double>(
                        recovered[i].best_matched_snr, peak.best_snr());
                }
                if (matches(kPulses[i], peak.samp_idx, peak.dm, peak.f_low,
                            peak.f_high, peak.filter_width)) {
                    ++recovered[i].final_matches;
                    recovered[i].final_snr = std::max<double>(
                        recovered[i].final_snr, peak.best_snr());
                }
            }
        }
    };
    std::vector<float> raw(kGulp * kChannels);
    std::vector<float> known(kGulp * kChannels);
    SizeType narrow = 0, fallbacks = 0, reused = 0;
    for (SizeType begin = 0; begin < observation.nsamps; begin += kGulp) {
        const auto valid = std::min(kGulp, observation.nsamps - begin);
        std::fill(raw.begin(), raw.end(), 0.0F);
        std::copy_n(observation.raw.begin() + begin * kChannels,
                    valid * kChannels, raw.begin());
        std::span<const float> normalized;
        // As in Pipeline: dmt excludes the cleaner's masked channels from
        // its noise model (ascending frequencies, so no reversal).
        std::span<const uint8_t> kill_mask;
        if (scenario.known_gain) {
            std::fill(known.begin(), known.end(), 0.0F);
            for (SizeType c = 0; c < kChannels; ++c) {
                for (SizeType t = 0; t < valid; ++t) {
                    known[c * kGulp + t] =
                        observation.known_gain[(begin + t) * kChannels + c];
                }
            }
            normalized = known;
        } else {
            normalized = cleaner.execute_normalized(bytes(raw), valid);
            masked += cleaner.report().chans_masked;
            broad += cleaner.report().samps_broad;
            narrow += cleaner.report().cells_narrow;
            fallbacks += cleaner.report().fill_fallbacks;
            reused += cleaner.report().calibration_reused;
            kill_mask = cleaner.channel_mask();
        }
        consume(engine.execute(normalized, kill_mask), begin + valid, valid,
                false);
    }
    consume(engine.drain(), observation.nsamps, 0, true);
    return {std::move(recovered),
            raw_count,
            final_count,
            masked,
            broad,
            narrow,
            fallbacks,
            reused};
}
} // namespace frost::test::calibration
