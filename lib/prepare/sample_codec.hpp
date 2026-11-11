#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <type_traits>

#include <frost/common/policies.hpp>

namespace frost::prepare::rfi::kernels {

// Rows are byte-aligned. Sub-byte samples sit LSB-first within each byte,
// which matches `psrio::DataExtractor`'s LSB-first layout on a
// little-endian host.
template <DataType D> struct Codec {
    static constexpr int kBits     = bits_of(D);
    static constexpr bool kIsFloat = is_floating(D);

    static float read(const uint8_t* __restrict__ row, SizeType c) {
        if constexpr (D == DataType::kUInt8) {
            return static_cast<float>(row[c]);
        } else if constexpr (D == DataType::kUInt16) {
            uint16_t v = 0;
            std::memcpy(&v, row + (c * 2), sizeof(v));
            return static_cast<float>(v);
        } else if constexpr (D == DataType::kUInt32) {
            uint32_t v = 0;
            std::memcpy(&v, row + (c * 4), sizeof(v));
            return static_cast<float>(v);
        } else if constexpr (D == DataType::kFloat32) {
            float v = 0.0F;
            std::memcpy(&v, row + (c * 4), sizeof(v));
            return v;
        } else {
            const SizeType bit       = c * kBits;
            constexpr unsigned kMask = (1U << kBits) - 1U;
            return static_cast<float>((row[bit >> 3U] >> (bit & 7U)) & kMask);
        }
    }

    // `v` must already be quantised (see `quantise`).
    static void write(uint8_t* __restrict__ row, SizeType c, float v) {
        if constexpr (D == DataType::kUInt8) {
            row[c] = static_cast<uint8_t>(v);
        } else if constexpr (D == DataType::kUInt16) {
            const auto raw = static_cast<uint16_t>(v);
            std::memcpy(row + (c * 2), &raw, sizeof(raw));
        } else if constexpr (D == DataType::kUInt32) {
            const auto raw = static_cast<uint32_t>(v);
            std::memcpy(row + (c * 4), &raw, sizeof(raw));
        } else if constexpr (D == DataType::kFloat32) {
            std::memcpy(row + (c * 4), &v, sizeof(v));
        } else {
            const SizeType bit       = c * kBits;
            constexpr unsigned kMask = (1U << kBits) - 1U;
            const unsigned shift     = bit & 7U;
            auto& byte               = row[bit >> 3U];
            byte                     = static_cast<uint8_t>(
                (byte & ~(kMask << shift)) |
                ((static_cast<unsigned>(v) & kMask) << shift));
        }
    }

    // Bit-exact copy of sample `c` from `src` to `dst`.
    static void copy(const uint8_t* __restrict__ src,
                     uint8_t* __restrict__ dst,
                     SizeType c) {
        if constexpr (kBits >= 8) {
            constexpr SizeType kBytes = kBits / 8;
            std::memcpy(dst + (c * kBytes), src + (c * kBytes), kBytes);
        } else {
            write(dst, c, read(src, c));
        }
    }

    static void unpack(const uint8_t* __restrict__ row,
                       SizeType c0,
                       SizeType n,
                       float* __restrict__ out) {
        for (SizeType i = 0; i < n; ++i) {
            out[i] = read(row, c0 + i);
        }
    }

    // Largest float that still converts to a representable sample.
    static float max_value() {
        if constexpr (kIsFloat) {
            return std::numeric_limits<float>::max();
        } else if constexpr (kBits < 32) {
            return static_cast<float>((1U << kBits) - 1U);
        } else {
            // 2^32 - 1 rounds up to 2^32 in float.
            constexpr double kMax =
                static_cast<double>((uint64_t{1} << kBits) - 1U);
            return std::nextafter(static_cast<float>(kMax + 1.0), 0.0F);
        }
    }

    static float quantise(float v) {
        if constexpr (kIsFloat) {
            return v;
        } else {
            return std::clamp(std::nearbyint(v), 0.0F, max_value());
        }
    }
};

/** Calls `f(Codec<D>{})` for the runtime `dtype`. */
template <typename F> decltype(auto) dispatch(DataType dtype, F&& f) {
    switch (dtype) {
    case DataType::kUInt1:
        return f(Codec<DataType::kUInt1>{});
    case DataType::kUInt2:
        return f(Codec<DataType::kUInt2>{});
    case DataType::kUInt4:
        return f(Codec<DataType::kUInt4>{});
    case DataType::kUInt8:
        return f(Codec<DataType::kUInt8>{});
    case DataType::kUInt16:
        return f(Codec<DataType::kUInt16>{});
    case DataType::kUInt32:
        return f(Codec<DataType::kUInt32>{});
    case DataType::kFloat32:
        break;
    }
    return f(Codec<DataType::kFloat32>{});
}

} // namespace frost::prepare::rfi::kernels
