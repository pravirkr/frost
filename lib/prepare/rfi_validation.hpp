#pragma once

#include <span>

#include <frost/prepare/rfi.hpp>

namespace frost::prepare::rfi::detail {

// Host-compiled validation shared by CPU and device orchestration. Device
// fast-math flags must not erase checks on observing metadata.
void validate_layout(const BlockLayout& layout, float baseline_length);

// Inspect packed IEEE float32 representations before decoding or arithmetic.
void validate_float_samples(std::span<const uint8_t> bytes, SizeType nchans);

} // namespace frost::prepare::rfi::detail
