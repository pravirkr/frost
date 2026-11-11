#include <algorithm>
#include <cmath>
#include <cstddef>
#include <limits>
#include <memory>
#include <span>
#include <stdexcept>
#include <vector>

#include <psrio/packed.hpp>

#include <frost/prepare/spectra.hpp>

#include "lib/cpu/stats_kernels.hpp"
#include "lib/prepare/bandpass_sampling.hpp"

namespace frost::prepare::spectra {

namespace {

constexpr SizeType kSpectrumCount = 3125; // 5^5 sample spectra
constexpr float kMadToStd = 1.4826F;      // MAD -> Gaussian-equivalent std dev

SizeType validate_geometry(std::span<float> bandpass,
                           std::span<float> rms,
                           SizeType nsamps,
                           SizeType nchans) {
    if (nchans == 0 || nsamps < 5 ||
        nsamps > std::numeric_limits<SizeType>::max() / nchans) {
        throw std::invalid_argument("Bandpass requires nsamps >= 5 and "
                                    "representable nonzero channel geometry");
    }
    if (bandpass.size() != nchans || rms.size() != nchans) {
        throw std::invalid_argument("h_bandpass/rms size must be nchans");
    }
    return nsamps * nchans;
}

// Recursively median-of-5-scrunches `in` (`count` x `nchans`, time-major)
// down to a single `nchans`-wide row. One output row at a time, so the
// inner loop over channels is contiguous and vectorizes; the scratch levels
// are left uninitialised because every element is written.
std::vector<float>
remedian_over_time(std::span<const float> in, SizeType count, SizeType nchans) {
    std::unique_ptr<float[]> level;
    const float* src = in.data();
    while (count > 1) {
        const SizeType next_count = count / 5;
        auto next =
            std::make_unique_for_overwrite<float[]>(next_count * nchans);
#pragma omp parallel for schedule(static)
        for (SizeType o = 0; o < next_count; ++o) {
            const float* r0 = src + (5 * o * nchans);
            const float* r1 = r0 + nchans;
            const float* r2 = r1 + nchans;
            const float* r3 = r2 + nchans;
            const float* r4 = r3 + nchans;
            float* out      = next.get() + (o * nchans);
            for (SizeType c = 0; c < nchans; ++c) {
                out[c] =
                    stats::kernels::median5(r0[c], r1[c], r2[c], r3[c], r4[c]);
            }
        }
        level = std::move(next);
        src   = level.get();
        count = next_count;
    }
    return {src, src + nchans};
}

} // namespace

void measure_bandpass(std::span<const float> h_in,
                      std::span<float> h_bandpass,
                      std::span<float> rms,
                      SizeType nsamps,
                      SizeType nchans) {
    const auto count = validate_geometry(h_bandpass, rms, nsamps, nchans);
    if (h_in.size() != count) {
        throw std::invalid_argument("h_in size must be nsamps*nchans");
    }

    // Sample spectra are taken evenly across the gulp rather than at random
    // offsets: equally representative, and the estimator is deterministic.
    // No channel-domain smoothing is applied to the remedian estimate.
    const SizeType spectrum_count =
        detail::largest_power_of_five_leq(std::min(kSpectrumCount, nsamps));
    if (spectrum_count < 5) {
        throw std::invalid_argument(
            "nsamps too small to estimate a bandpass (need >= 5)");
    }

    // Evenly spaced spectra; when every spectrum is used (the usual case:
    // `nsamps` is a power of 5) the input is read in place.
    std::unique_ptr<float[]> gathered;
    const float* spectra_ptr = h_in.data();
    if (spectrum_count != nsamps) {
        gathered =
            std::make_unique_for_overwrite<float[]>(spectrum_count * nchans);
#pragma omp parallel for schedule(static)
        for (SizeType i = 0; i < spectrum_count; ++i) {
            const SizeType t = (i * nsamps) / spectrum_count;
            std::copy_n(h_in.data() + (t * nchans), nchans,
                        gathered.get() + (i * nchans));
        }
        spectra_ptr = gathered.get();
    }
    const std::span<const float> sample_spectra(spectra_ptr,
                                                spectrum_count * nchans);

    const auto bandpass =
        remedian_over_time(sample_spectra, spectrum_count, nchans);
    std::copy(bandpass.begin(), bandpass.end(), h_bandpass.begin());

    auto abs_diff =
        std::make_unique_for_overwrite<float[]>(spectrum_count * nchans);
#pragma omp parallel for schedule(static)
    for (SizeType i = 0; i < spectrum_count; ++i) {
        for (SizeType c = 0; c < nchans; ++c) {
            abs_diff[(i * nchans) + c] =
                std::abs(sample_spectra[(i * nchans) + c] - h_bandpass[c]);
        }
    }
    const auto mad = remedian_over_time(
        std::span<const float>(abs_diff.get(), spectrum_count * nchans),
        spectrum_count, nchans);
    for (SizeType c = 0; c < nchans; ++c) {
        rms[c] = mad[c] * kMadToStd;
    }
}

void measure_bandpass(std::span<const uint8_t> h_in,
                      std::span<float> h_bandpass,
                      std::span<float> rms,
                      SizeType nsamps,
                      SizeType nchans,
                      DataType dtype) {
    const auto count = validate_geometry(h_bandpass, rms, nsamps, nchans);
    const auto type  = to_sample_type(dtype);
    const auto width = static_cast<SizeType>(bits_of(dtype));
    // The pinned extractor computes total_bits + 7 before dividing by 8.
    // Reject overflow before constructing it or allocating decoded storage.
    if (count > (std::numeric_limits<SizeType>::max() - 7) / width) {
        throw std::invalid_argument(
            "Packed bandpass geometry overflows its bit count");
    }
    const auto bytes_needed = (count * width + 7) / 8;
    if (h_in.size() < bytes_needed)
        throw std::invalid_argument("Packed bandpass input is too short");
    const psrio::DataExtractor extractor(
        std::span<const std::byte>(
            reinterpret_cast<const std::byte*>(h_in.data()), h_in.size()),
        nsamps, nchans, type);
    std::vector<float> unpacked(count);
    for (SizeType t = 0; t < nsamps; ++t) {
        for (SizeType c = 0; c < nchans; ++c) {
            unpacked[(t * nchans) + c] = extractor.extract_sample(t, c);
        }
    }
    measure_bandpass(unpacked, h_bandpass, rms, nsamps, nchans);
}

} // namespace frost::prepare::spectra
