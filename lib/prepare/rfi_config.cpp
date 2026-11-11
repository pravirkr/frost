#include <algorithm>
#include <limits>
#include <stdexcept>

#include <fmt/format.h>

#include <frost/common/numeric.hpp>
#include <frost/prepare/rfi.hpp>

#include "lib/prepare/rfi_validation.hpp"

namespace frost::prepare::rfi {

void RfiConfig::validate() const {
    const auto positive = [](const float& value, const char* name) {
        if (!is_positive_finite(value)) {
            throw std::invalid_argument(
                fmt::format("rfi.{} must be finite and > 0", name));
        }
    };
    const auto nonnegative = [](const float& value, const char* name) {
        if (!is_finite(value) || value < 0.0F) {
            throw std::invalid_argument(
                fmt::format("rfi.{} must be finite and >= 0", name));
        }
    };
    const auto fraction = [](const float& value, const char* name) {
        if (!is_finite(value) || value < 0.0F || value > 1.0F) {
            throw std::invalid_argument(
                fmt::format("rfi.{} must be finite and in [0, 1]", name));
        }
    };
    positive(narrow.threshold, "narrow.threshold");
    positive(broad.threshold, "broad.threshold");
    positive(moments.threshold, "moments.threshold");
    positive(iqrm.threshold, "iqrm.threshold");
    positive(kadane_channel.threshold, "kadane_channel.threshold");
    positive(kadane_spectrum.threshold, "kadane_spectrum.threshold");
    nonnegative(moments.kurtosis_max, "moments.kurtosis_max");
    nonnegative(kadane_channel.min_duration, "kadane_channel.min_duration");
    nonnegative(kadane_spectrum.min_bandwidth, "kadane_spectrum.min_bandwidth");
    fraction(dropout.min_std_fraction, "dropout.min_std_fraction");
    fraction(kadane_spectrum.whole_band_fraction,
             "kadane_spectrum.whole_band_fraction");
    if (broad.boxcar_max == 0) {
        throw std::invalid_argument("rfi.broad.boxcar_max must be >= 1");
    }
    for (const auto& range : zap_chans) {
        if (range.start > range.end) {
            throw std::invalid_argument(
                "rfi.zap_chans ranges need start <= end");
        }
    }
    for (const auto& range : zap_freqs) {
        if (!is_finite(range.lo) || !is_finite(range.hi) ||
            range.lo > range.hi) {
            throw std::invalid_argument(
                "rfi.zap_freqs ranges must be finite with lo <= hi");
        }
    }
    switch (fill) {
    case FillMode::kNeighbour:
    case FillMode::kMean:
    case FillMode::kNoise:
        break;
    default:
        throw std::invalid_argument("Invalid rfi.fill");
    }
    switch (zero_dm) {
    case ZeroDMMode::kOff:
    case ZeroDMMode::kSubtract:
    case ZeroDMMode::kZdot:
        break;
    default:
        throw std::invalid_argument("Invalid rfi.zero_dm");
    }
}

void detail::validate_layout(const BlockLayout& layout, float baseline_length) {
    if (layout.nchans == 0 || layout.max_nsamps == 0) {
        throw std::invalid_argument("RFI: nchans and max_nsamps must be > 0");
    }
    const auto width    = static_cast<SizeType>(bits_of(layout.dtype));
    constexpr auto kMax = std::numeric_limits<SizeType>::max();
    if (layout.nchans > kMax / width ||
        layout.nchans > (kMax / sizeof(float)) / layout.max_nsamps) {
        throw std::invalid_argument(
            "RFI: packed or normalized block geometry overflows");
    }
    if ((layout.nchans * width) % 8 != 0) {
        throw std::invalid_argument(
            "RFI: each time sample must be a whole number of bytes");
    }
    if (!is_positive_finite(layout.tsamp) ||
        !is_positive_finite(baseline_length)) {
        throw std::invalid_argument(
            "RFI: tsamp and baseline_length must be finite and > 0");
    }
    if (!is_finite(layout.f0) || !is_finite(layout.df)) {
        throw std::invalid_argument("RFI: f0 and df must be finite");
    }
    // Compare before dividing: finite metadata can still imply a sample
    // count outside SizeType. Dividing the float baseline by kMax is safe
    // in double and avoids overflowing either baseline/tsamp or kMax*tsamp.
    if (layout.tsamp <=
        static_cast<double>(baseline_length) / static_cast<double>(kMax)) {
        throw std::invalid_argument(
            "RFI: baseline sample count is not representable");
    }
}

void detail::validate_float_samples(std::span<const uint8_t> bytes,
                                    SizeType nchans) {
    const SizeType count = bytes.size() / sizeof(std::uint32_t);
    SizeType first_bad   = count;
#pragma omp parallel for reduction(min : first_bad) schedule(static)
    for (SizeType i = 0; i < count; ++i) {
        std::uint32_t bits;
        std::memcpy(&bits, bytes.data() + i * sizeof(bits), sizeof(bits));
        if (!is_finite_bits(bits))
            first_bad = std::min(first_bad, i);
    }
    if (first_bad != count) {
        throw std::invalid_argument(fmt::format(
            "RFI: non-finite float32 input at sample {}, channel {}",
            first_bad / nchans, first_bad % nchans));
    }
}

} // namespace frost::prepare::rfi
