#pragma once

// Test-only support header: a lightweight synthetic-pulse injector for
// building sigproc filterbank fixtures. Catch2 fixtures only.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <random>
#include <string>
#include <vector>

#include <frost/common/policies.hpp>

namespace frost::testing {

struct InjectedPulse {
    SizeType start; // arrival sample at the highest-frequency channel
    SizeType width;
    float amplitude; // added to the baseline, before per-sample noise
    float dm = 0.0F; // dispersed with the cold-plasma law when > 0
    // Inclusive channel range the pulse occupies (default: every channel).
    SizeType chan_start = 0;
    SizeType chan_end   = static_cast<SizeType>(-1);
};

struct FakeFilterbankSpec {
    SizeType nchans   = 32;
    SizeType nsamps   = 8192;
    double tsamp      = 0.001;   // seconds
    double fch1       = 1400.0;  // MHz
    double foff       = -1.0;    // MHz
    double tstart     = 58000.0; // MJD
    uint8_t baseline  = 100;
    float noise_sigma = 0.0F;
};

namespace detail {

inline void write_string_field(std::ostream& os, const std::string& s) {
    const auto len = static_cast<int32_t>(s.size());
    os.write(reinterpret_cast<const char*>(&len), sizeof(len));
    os.write(s.data(), len);
}

template <typename T>
void write_kv(std::ostream& os, const std::string& key, const T& value) {
    write_string_field(os, key);
    os.write(reinterpret_cast<const char*>(&value), sizeof(T));
}

// Dispersion delay (samples) of channel `c` relative to the band's
// highest frequency, using the physical constant rather than the one the
// code under test uses, as real data would.
inline SizeType
dispersion_delay(const FakeFilterbankSpec& spec, SizeType c, float dm) {
    constexpr double kDispersionConst = 4.148808e3; // s MHz^2 / (pc cm^-3)
    const double f_last =
        spec.fch1 + (static_cast<double>(spec.nchans - 1) * spec.foff);
    const double f_top = std::max(spec.fch1, f_last);
    const double f     = spec.fch1 + (static_cast<double>(c) * spec.foff);
    const double delay_s =
        kDispersionConst * dm * ((1.0 / (f * f)) - (1.0 / (f_top * f_top)));
    return static_cast<SizeType>(std::lround(delay_s / spec.tsamp));
}

} // namespace detail

/**
 * Writes a synthetic 8-bit sigproc filterbank at `path`: a Gaussian-noise
 * baseline plus boxcar-shaped pulses, each dispersed by its own DM.
 * Returns `path` for convenience.
 */
inline std::string
write_fake_filterbank(const std::string& path,
                      const FakeFilterbankSpec& spec,
                      const std::vector<InjectedPulse>& pulses,
                      unsigned seed = 1234) {
    std::ofstream os(path, std::ios::binary);

    detail::write_string_field(os, "HEADER_START");
    detail::write_kv(os, "telescope_id", 4);
    detail::write_kv(os, "machine_id", 10);
    detail::write_kv(os, "data_type", 1);
    detail::write_kv(os, "fch1", spec.fch1);
    detail::write_kv(os, "foff", spec.foff);
    detail::write_kv(os, "nchans", static_cast<int>(spec.nchans));
    detail::write_kv(os, "nbits", 8);
    detail::write_kv(os, "nifs", 1);
    detail::write_kv(os, "tsamp", spec.tsamp);
    detail::write_kv(os, "tstart", spec.tstart);
    detail::write_kv(os, "ibeam", 0);
    detail::write_string_field(os, "HEADER_END");

    std::vector<float> excess(spec.nsamps * spec.nchans, 0.0F);
    for (const auto& pulse : pulses) {
        const SizeType c_end = std::min(pulse.chan_end, spec.nchans - 1);
        for (SizeType c = pulse.chan_start; c <= c_end; ++c) {
            const SizeType t0 =
                pulse.start + detail::dispersion_delay(spec, c, pulse.dm);
            for (SizeType t = t0; t < std::min(t0 + pulse.width, spec.nsamps);
                 ++t) {
                excess[(t * spec.nchans) + c] += pulse.amplitude;
            }
        }
    }

    std::mt19937 rng(seed);
    std::normal_distribution<float> noise(0.0F,
                                          std::max(spec.noise_sigma, 1e-12F));
    std::vector<uint8_t> row(spec.nchans);
    for (SizeType t = 0; t < spec.nsamps; ++t) {
        for (SizeType c = 0; c < spec.nchans; ++c) {
            const float n     = spec.noise_sigma > 0.0F ? noise(rng) : 0.0F;
            const float value = static_cast<float>(spec.baseline) +
                                excess[(t * spec.nchans) + c] + n;
            row[c]            = static_cast<uint8_t>(
                std::clamp(std::round(value), 0.0F, 255.0F));
        }
        os.write(reinterpret_cast<const char*>(row.data()),
                 static_cast<std::streamsize>(row.size()));
    }
    return path;
}

/** Undispersed-pulse convenience overload. */
inline std::string
write_fake_filterbank(const std::string& path,
                      SizeType nchans,
                      SizeType nsamps,
                      uint8_t baseline,
                      float noise_sigma,
                      const std::vector<InjectedPulse>& pulses,
                      unsigned seed = 1234) {
    FakeFilterbankSpec spec;
    spec.nchans      = nchans;
    spec.nsamps      = nsamps;
    spec.baseline    = baseline;
    spec.noise_sigma = noise_sigma;
    return write_fake_filterbank(path, spec, pulses, seed);
}

} // namespace frost::testing
