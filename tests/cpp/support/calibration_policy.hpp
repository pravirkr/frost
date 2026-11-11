#pragma once

// White-box study support: only internal tests may include the private fitter.
// These experimental policies are not part of the Frost library or CLI.
#include <array>
#include <optional>

#include <frost/prepare/spectra.hpp>

#include "lib/cpu/bandpass_kernels.hpp"
#include "lib/prepare/bandpass_sampling.hpp"

#include "support/calibration.hpp"

namespace frost::test::calibration::policy {
inline constexpr SizeType kWindow   = 2000;
inline constexpr SizeType kTotal    = 4 * kGulp + 37;
inline constexpr SizeType kMaxWidth = 512;
inline constexpr float kScoreFloor  = 3;

enum class Policy { kCurrent, kLagged, kInterpolated, kOracle };
inline constexpr std::array kPolicies{Policy::kCurrent, Policy::kLagged,
                                      Policy::kInterpolated, Policy::kOracle};
inline std::string_view name(Policy policy) {
    switch (policy) {
    case Policy::kCurrent:
        return "current";
    case Policy::kLagged:
        return "lagged";
    case Policy::kInterpolated:
        return "interpolated";
    case Policy::kOracle:
        return "oracle";
    }
    throw std::invalid_argument("unknown study policy");
}

enum class Model {
    kWhite,
    kGainRamp,
    kGainStep,
    kOffsetRamp,
    kTailStep,
    kAr1,
    kCold
};
inline constexpr std::array kModels{
    Model::kWhite,    Model::kGainRamp, Model::kGainStep, Model::kOffsetRamp,
    Model::kTailStep, Model::kAr1,      Model::kCold};
inline std::string_view name(Model model) {
    switch (model) {
    case Model::kWhite:
        return "white";
    case Model::kGainRamp:
        return "gain_ramp";
    case Model::kGainStep:
        return "gain_step";
    case Model::kOffsetRamp:
        return "offset_ramp";
    case Model::kTailStep:
        return "tail_step";
    case Model::kAr1:
        return "ar1";
    case Model::kCold:
        return "cold";
    }
    throw std::invalid_argument("unknown study model");
}

inline std::vector<Injection> pulses(Model model) {
    if (model == Model::kCold)
        return {{"cold", 8, 4, 0, 0, kChannels, 1.060660172F}};
    // Equal ideal full-band boxcar S/N (12), before FDMT smear/scalloping.
    return {{"startup", 64, 8, 0, 0, kChannels, 0.75F},
            {"interior", 5000, 8, 80, 0, kChannels, 0.75F},
            {"boundary", 2 * kGulp - 2, 8, 80, 0, kChannels, 0.75F},
            {"long", 11000, 512, 0, 0, kChannels, 0.09375F},
            {"tail", 4 * kGulp + 8, 4, 0, 0, kChannels, 1.060660172F}};
}
inline float gain_factor(Model model, SizeType t, SizeType total) {
    if (model == Model::kGainRamp)
        return 1 + static_cast<float>(t) / static_cast<float>(total - 1);
    if (model == Model::kGainStep && t >= 2 * kGulp)
        return 2;
    if (model == Model::kTailStep && t >= 4 * kGulp)
        return 2;
    return 1;
}
inline float offset_drift(Model model, SizeType t, SizeType total) {
    return model == Model::kOffsetRamp
               ? static_cast<float>(t) / static_cast<float>(total - 1)
               : 0;
}
inline Observation observation(uint32_t seed, Model model, float amplitude) {
    Observation out;
    out.nsamps = model == Model::kCold ? 37 : kTotal;
    out.raw.resize(out.nsamps * kChannels);
    out.known_gain.resize(out.raw.size());
    NoiseStream noise(
        seed, model == Model::kAr1 ? NoiseModel::kAr1 : NoiseModel::kWhite,
        out.nsamps);
    for (SizeType begin = 0; begin < out.nsamps; begin += kGulp) {
        const auto valid = std::min(kGulp, out.nsamps - begin);
        const auto raw   = noise.next(begin, valid);
        for (SizeType t = 0; t < valid; ++t) {
            for (SizeType c = 0; c < kChannels; ++c) {
                const auto i = (begin + t) * kChannels + c;
                float z      = (raw[t * kChannels + c] - offset(c)) / gain(c);
                for (const auto& pulse : pulses(model)) {
                    const auto start =
                        pulse.start +
                        delay(pulse, kFirstFrequency + c * kChannelStep);
                    if (begin + t >= start && begin + t - start < pulse.width)
                        z += amplitude * pulse.amplitude;
                }
                const auto physical_gain =
                    gain(c) * gain_factor(model, begin + t, out.nsamps);
                const auto physical_offset =
                    offset(c) +
                    gain(c) * offset_drift(model, begin + t, out.nsamps);
                out.raw[i] = physical_offset + physical_gain * z;
                // Remove exact receiver effects, retaining the same rounded
                // input bytes. This reference does not whiten AR noise.
                out.known_gain[i] =
                    (out.raw[i] - physical_offset) / physical_gain;
            }
        }
    }
    return out;
}

struct Knot {
    double center = 0;
    std::array<float, kChannels> location{}, scale{};
};
inline Knot interpolate(const Knot& a, const Knot& b, double sample) {
    const auto fraction =
        std::clamp((sample - a.center) / (b.center - a.center), 0.0, 1.0);
    Knot out;
    for (SizeType c = 0; c < kChannels; ++c) {
        out.location[c] = static_cast<float>((1 - fraction) * a.location[c] +
                                             fraction * b.location[c]);
        out.scale[c]    = static_cast<float>((1 - fraction) * a.scale[c] +
                                             fraction * b.scale[c]);
    }
    return out;
}

inline Knot fit(std::span<const float> raw,
                SizeType first,
                SizeType count,
                SizeType absolute) {
    Knot out;
    out.center = static_cast<double>(absolute + first) +
                 0.5 * static_cast<double>(count - 1);
    if (count < 64) {
        for (SizeType c = 0; c < kChannels; ++c) {
            double mean = 0, ss = 0;
            for (SizeType t = 0; t < count; ++t) {
                const double x     = raw[(first + t) * kChannels + c];
                const double delta = x - mean;
                mean += delta / static_cast<double>(t + 1);
                ss += delta * (x - mean);
            }
            out.location[c] = static_cast<float>(mean);
            out.scale[c] =
                static_cast<float>(std::sqrt(ss / static_cast<double>(count)));
        }
        return out;
    }
    const auto sampled = frost::prepare::detail::largest_power_of_five_leq(
        std::min<SizeType>(3125, count));
    std::vector<float> rows(sampled * kChannels);
    for (SizeType i = 0; i < sampled; ++i) {
        std::copy_n(raw.data() + (first + i * count / sampled) * kChannels,
                    kChannels, rows.data() + i * kChannels);
    }
    frost::prepare::spectra::measure_bandpass(rows, out.location, out.scale,
                                              sampled, kChannels);
    for (SizeType c = 0; c < kChannels; ++c) {
        if (!frost::prepare::rfi::detail::refine_bandpass(
                rows.data() + c, sampled, kChannels, out.location[c],
                out.scale[c])) {
            throw std::runtime_error("study requires measurable channel noise");
        }
    }
    return out;
}

// Current calls the public production cleaner. Alternatives reuse its exact
// fit, changing only the estimate applied to a sample. No truth enters them.
class CalibrationPolicyPlan {
public:
    explicit CalibrationPolicyPlan(Policy policy)
        : m_policy(policy),
          m_current(scenarios().front().config, layout(), 2) {}
    std::span<const float> execute(std::span<float> raw,
                                   SizeType begin,
                                   SizeType valid,
                                   std::span<const float> known = {}) {
        if (raw.size() != kGulp * kChannels || valid == 0 || valid > kGulp ||
            begin != m_next) {
            throw std::invalid_argument("invalid study stream geometry");
        }
        if (m_policy == Policy::kOracle && known.size() != valid * kChannels) {
            throw std::invalid_argument("oracle requires known-gain samples");
        }
        m_next += valid;
        if (m_policy == Policy::kCurrent) {
            return m_current.execute_normalized(
                {reinterpret_cast<uint8_t*>(raw.data()), raw.size_bytes()},
                valid);
        }
        m_output.assign(kGulp * kChannels, 0);
        if (m_policy == Policy::kOracle) {
            for (SizeType c = 0; c < kChannels; ++c)
                for (SizeType t = 0; t < valid; ++t)
                    m_output[c * kGulp + t] = known[t * kChannels + c];
            return m_output;
        }
        std::vector<Knot> knots;
        if (valid < 64) {
            knots.push_back(m_previous.value_or(Knot{}));
            if (!m_previous)
                knots[0] = fit(raw, 0, valid, begin);
        } else {
            const auto windows = std::max<SizeType>(valid / kWindow, 1);
            for (SizeType w = 0; w < windows; ++w) {
                const auto first = w * kWindow;
                const auto count = w + 1 == windows ? valid - first : kWindow;
                knots.push_back(fit(raw, first, count, begin));
            }
        }
        for (SizeType t = 0; t < valid; ++t) {
            const auto window = std::min(t / kWindow, knots.size() - 1);
            Knot applied;
            if (m_policy == Policy::kLagged) {
                applied = window > 0 ? knots[window - 1]
                                     : m_previous.value_or(knots[0]);
            } else {
                const auto absolute = static_cast<double>(begin + t);
                auto upper =
                    std::upper_bound(knots.begin(), knots.end(), absolute,
                                     [](double sample, const Knot& knot) {
                                         return sample < knot.center;
                                     });
                if (upper == knots.begin()) {
                    applied = m_previous ? interpolate(*m_previous,
                                                       knots.front(), absolute)
                                         : knots.front();
                } else if (upper == knots.end()) {
                    applied = knots.back();
                } else {
                    applied = interpolate(*(upper - 1), *upper, absolute);
                }
            }
            for (SizeType c = 0; c < kChannels; ++c) {
                const auto inverse =
                    applied.scale[c] > 0 ? 1 / applied.scale[c] : 0;
                m_output[c * kGulp + t] =
                    (raw[t * kChannels + c] - applied.location[c]) * inverse;
            }
        }
        if (valid >= 64)
            m_previous = knots.back();
        return m_output;
    }

private:
    Policy m_policy;
    RFICleaner m_current;
    std::optional<Knot> m_previous;
    std::vector<float> m_output;
    SizeType m_next = 0;
};

struct ScoreRun {
    double maximum = kScoreFloor; // Censored below the collection floor.
    std::vector<double> matched, forced_dm0;
    double maximum_dm0_error = 0;
    SizeType candidates      = 0;
    // Noise response statistics, including every real sample (no selection).
    std::array<Moments, 3> response{}; // widths 1, 8, 512 at DM zero
};
inline ScoreRun score(Policy policy, Model model, const Observation& data) {
    // Two physical DM trials isolate calibration from dense-grid clustering.
    // All-above-threshold preserves every valid endpoint; maxima calibrate a
    // whole-observation false-alarm test, accounting for dependent windows.
    const dmt::plans::FDMTPlan plan(796, 1052, kChannels, kGulp, kTsamp,
                                    std::vector<float>{0, 80}, "valid", 0);
    dmt::BoxcarConfig search;
    search.max_boxcar       = kMaxWidth;
    search.detect_thresh    = kScoreFloor;
    search.peak_mode        = dmt::PeakFindingMode::kAllAboveThreshold;
    search.max_candidates   = 2 * 10 * kGulp; // exact point-count upper bound
    search.max_cands_per_dm = 0;
    dmt::algorithms::FDMTBoxcar engine(plan, search);
    REQUIRE(engine.get_search_plans().size() == 1);
    REQUIRE(engine.get_search_plans().front().dm_grid.size() == 2);
    REQUIRE(engine.get_search_plans().front().widths.size() == 10);
    CalibrationPolicyPlan calibrate(policy);
    ScoreRun out;
    const auto injected = pulses(model);
    out.matched.resize(injected.size());
    out.forced_dm0.resize(injected.size());
    std::vector<float> raw(kGulp * kChannels);
    std::vector<double> prefix(1, 0);
    prefix.reserve(data.nsamps +
                   1); // Bounded by this finite study observation.
    const auto consume = [&](dmt::CandidateResultView found) {
        REQUIRE_FALSE(found.overflowed());
        for (const auto& candidate : found.candidates) {
            if (candidate.peak_sample >= data.nsamps)
                continue;
            ++out.candidates;
            if (candidate.dm_idx == 0) {
                if (candidate.peak_sample + 1 < candidate.width)
                    throw std::runtime_error(
                        "incomplete engine window in study");
                const auto end = candidate.peak_sample + 1;
                const auto expected =
                    (prefix[end] - prefix[end - candidate.width]) /
                    std::sqrt(static_cast<double>(kChannels * candidate.width));
                out.maximum_dm0_error = std::max(
                    out.maximum_dm0_error, std::abs(expected - candidate.snr));
            }
            out.maximum = std::max<double>(out.maximum, candidate.snr);
            for (SizeType i = 0; i < injected.size(); ++i) {
                // Width must also agree: a very wide red-noise peak in a
                // broad truth gate must not count as a narrow pulse recovery.
                if (candidate.width < injected[i].width / 2 ||
                    candidate.width > 2 * injected[i].width)
                    continue;
                if (matches(injected[i], candidate.peak_sample, candidate.dm,
                            candidate.f_low_mhz, candidate.f_high_mhz,
                            candidate.width)) {
                    out.matched[i] =
                        std::max<double>(out.matched[i], candidate.snr);
                }
            }
        }
    };
    for (SizeType begin = 0; begin < data.nsamps; begin += kGulp) {
        const auto valid = std::min(kGulp, data.nsamps - begin);
        std::fill(raw.begin(), raw.end(), 0);
        std::copy_n(data.raw.begin() + begin * kChannels, valid * kChannels,
                    raw.begin());
        const auto truth =
            policy == Policy::kOracle
                ? std::span<const float>(data.known_gain)
                      .subspan(begin * kChannels, valid * kChannels)
                : std::span<const float>{};
        const auto normalized = calibrate.execute(raw, begin, valid, truth);
        for (SizeType t = 0; t < valid; ++t) {
            double sum = 0;
            for (SizeType c = 0; c < kChannels; ++c)
                sum += normalized[c * kGulp + t];
            prefix.push_back(prefix.back() + sum);
            for (SizeType w = 0; w < 3; ++w) {
                const SizeType width = std::array<SizeType, 3>{1, 8, 512}[w];
                if (prefix.size() <= width)
                    continue;
                const double response =
                    (prefix.back() - prefix[prefix.size() - width - 1]) /
                    std::sqrt(static_cast<double>(kChannels * width));
                out.response[w].add(response);
            }
        }
        consume(engine.execute(normalized));
    }
    consume(engine.flush());
    CHECK(out.maximum_dm0_error < 0.003);
    for (SizeType i = 0; i < injected.size(); ++i) {
        const auto& pulse = injected[i];
        if (pulse.dm != 0)
            continue;
        const auto end = pulse.start + pulse.width;
        out.forced_dm0[i] =
            (prefix[end] - prefix[end - pulse.width]) /
            std::sqrt(static_cast<double>(kChannels * pulse.width));
    }
    return out;
}
} // namespace frost::test::calibration::policy
