#pragma once

#include <cstdint>
#include <span>

#include <frost/common/policies.hpp>

namespace frost::prepare::spectra {

/**
 * Estimates a per-channel bandpass shape and per-channel RMS from `nsamps`
 * time samples of `nchans` channels (row-major, time-then-channel) in
 * `h_in`, using a robust recursive median-of-5 ("remedian") estimator
 * (Rousseeuw & Bassett 1990). `h_bandpass` and `rms` must each have `nchans`
 * elements. Requires finite input, `nsamps >= 5` and `nchans > 0`.
 * Malformed or overflowing geometry throws before output is changed.
 */
void measure_bandpass(std::span<const float> h_in,
                      std::span<float> h_bandpass,
                      std::span<float> rms,
                      SizeType nsamps,
                      SizeType nchans);

/**
 * Byte-packed overload: unpacks `h_in` (packed at `dtype`) with
 * `psrio::DataExtractor` before delegating to the float-span overload
 * above. The input must contain every packed sample; trailing padding is
 * permitted. Invalid dtype or byte-count geometry throws before decoding.
 */
void measure_bandpass(std::span<const uint8_t> h_in,
                      std::span<float> h_bandpass,
                      std::span<float> rms,
                      SizeType nsamps,
                      SizeType nchans,
                      DataType dtype);

} // namespace frost::prepare::spectra
