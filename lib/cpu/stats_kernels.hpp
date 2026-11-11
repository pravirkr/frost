#pragma once

#include <algorithm>
#include <array>
#include <stdexcept>
#include <vector>

#include <frost/common/policies.hpp>

namespace frost::stats::kernels {

//=== Helper Median Functions ===

template <Arithmetic T> inline T median3(T a, T b, T c) {
    return a < b ? (b < c ? b : (a < c ? c : a))
                 : (a < c ? a : (b < c ? c : b));
}

template <Arithmetic T> inline T median4(T a, T b, T c, T d) {
    return (std::max(std::min(a, b), std::min(c, d)) +
            std::min(std::max(a, b), std::max(c, d))) /
           2.0F;
}

template <Arithmetic T> inline T median5(T a, T b, T c, T d, T e) {
    // Discards the lowest and highest of the first four, then
    // computes the median of (e, f, g)
    const auto f = std::max(std::min(a, b), std::min(c, d));
    const auto g = std::min(std::max(a, b), std::max(c, d));
    return median3(e, f, g);
}

// Compute the median of an array of floats.
inline float median(const float* data, SizeType size) {
    if (size == 0) {
        throw std::invalid_argument("Cannot compute median of empty data set");
    }
    std::vector<float> temp(data, data + size);
    auto middle = temp.begin() + static_cast<IndexType>(size / 2);
    std::nth_element(temp.begin(), middle, temp.end());
    if (size % 2 == 0) {
        // For even sizes, take the average of the two central values.
        auto max_it = std::max_element(temp.begin(), middle);
        return 0.5F * (*middle + *max_it);
    }
    return *middle;
}

//=== Median Filter Kernels ===

template <SizeType N> struct MedianFilterKernel {
    static_assert(N % 2 == 1, "Filter size must be odd");
    const float* in;
    SizeType in_size;

    constexpr MedianFilterKernel(const float* in, SizeType in_size)
        : in(in),
          in_size(in_size) {}

    float operator()(SizeType i) const {
        if (in_size < N)
            return apply_filter(i);
        if constexpr (N == 3) {
            return apply_filter3(i);
        } else if constexpr (N == 5) {
            return apply_filter5(i);
        } else {
            return apply_filter(i);
        }
    }

private:
    static constexpr auto kHalfSize = static_cast<IndexType>(N / 2);

    float apply_filter3(SizeType i) const {
        if (i > 0 && i < in_size - 1) {
            return median3(in[i - 1], in[i], in[i + 1]);
        }
        // For edge cases, average the available neighbor.
        return (i == 0) ? 0.5F * (in[i] + in[i + 1])
                        : 0.5F * (in[i] + in[i - 1]);
    }

    float apply_filter5(SizeType i) const {
        if (i > 1 && i < in_size - 2) {
            return median5(in[i - 2], in[i - 1], in[i], in[i + 1], in[i + 2]);
        }
        // Handle edges with smaller windows.
        if (i == 0) {
            return median3(in[i], in[i + 1], in[i + 2]);
        }
        if (i == 1) {
            return median4(in[i - 1], in[i], in[i + 1], in[i + 2]);
        }
        if (i == in_size - 1) {
            return median3(in[i], in[i - 1], in[i - 2]);
        }
        // i == in_size - 2
        return median4(in[i + 1], in[i], in[i - 1], in[i - 2]);
    }
    float apply_filter(SizeType i) const {
        std::array<float, N> window{};
        SizeType window_size = 0;

        for (SizeType offset = 0; offset < N; ++offset) {
            const IndexType idx = static_cast<IndexType>(i) +
                                  static_cast<IndexType>(offset) - kHalfSize;
            if (idx >= 0 && idx < static_cast<IndexType>(in_size)) {
                window[window_size++] = in[idx];
            }
        }

        if (window_size == N) {
            auto mid = window.begin() + kHalfSize;
            std::nth_element(window.begin(), mid, window.end());
            return *mid;
        }
        // Edge windows are tiny; keep their sort explicitly bounded by N.
        for (SizeType j = 1; j < window_size && j < N; ++j) {
            const float value = window[j];
            SizeType k        = j;
            while (k > 0 && value < window[k - 1]) {
                window[k] = window[k - 1];
                --k;
            }
            window[k] = value;
        }
        if constexpr (N == 3 || N == 5) {
            if (window_size % 2 == 0) {
                return 0.5F * window[window_size / 2 - 1] +
                       0.5F * window[window_size / 2];
            }
        }
        return window[window_size / 2];
    }
};

inline void median_scrunch5(const float* in, SizeType in_size, float* out) {
    if (in_size == 0) {
        return;
    }
    if (in_size < 5) {
        out[0] = in[0];
        return;
    }
    const SizeType out_size = in_size / 5;
    for (SizeType i = 0; i < out_size; ++i) {
        const SizeType start = i * 5;
        out[i] = median5(in[start], in[start + 1], in[start + 2], in[start + 3],
                         in[start + 4]);
    }
}

template <Arithmetic T>
void linear_stretch(const T* in, SizeType in_size, T* out, SizeType out_size) {
    if (out_size == 0) {
        return;
    }
    if (in_size == 0) {
        std::fill(out, out + out_size, T{});
        return;
    }
    if (out_size == 1) {
        out[0] = in[0];
        return;
    }
    if (in_size == 1) {
        std::fill(out, out + out_size, in[0]);
        return;
    }
    const double scale =
        static_cast<double>(in_size - 1) / static_cast<double>(out_size - 1);
    for (SizeType i = 0; i < out_size; ++i) {
        const double pos  = static_cast<double>(i) * scale;
        const SizeType lo = static_cast<SizeType>(pos);
        const double frac = pos - static_cast<double>(lo);
        const SizeType hi = std::min(lo + 1, in_size - 1);
        out[i] = static_cast<T>(static_cast<double>(in[lo]) * (1.0 - frac) +
                                static_cast<double>(in[hi]) * frac);
    }
}

inline void running_mean(const float* in,
                         SizeType in_size,
                         float* out,
                         SizeType out_size,
                         SizeType window_size) {
    if (window_size == 0 || out_size == 0) {
        return;
    }
    const SizeType half  = window_size / 2;
    const SizeType ahead = window_size - half;
    for (SizeType i = 0; i < out_size; ++i) {
        const SizeType start = (i > half) ? i - half : 0;
        const SizeType end   = i + std::min(ahead, in_size - i);
        double sum           = 0.0;
        for (SizeType j = start; j < end; ++j) {
            sum += in[j];
        }
        out[i] = static_cast<float>(sum / static_cast<double>(end - start));
    }
}

template <SizeType N>
void running_median_impl(const float* in,
                         SizeType in_size,
                         float* out,
                         SizeType out_size) {
    MedianFilterKernel<N> kernel(in, in_size);
    for (SizeType i = 0; i < out_size; ++i) {
        out[i] = kernel(i);
    }
}

inline void running_median_generic(const float* in,
                                   SizeType in_size,
                                   float* out,
                                   SizeType out_size,
                                   SizeType window_size) {
    if (window_size % 2 == 0 || window_size == 0) {
        throw std::invalid_argument(
            "Window size must be a positive odd integer");
    }
    const SizeType half = window_size / 2;
    std::vector<float> window(std::min(window_size, in_size));
    for (SizeType i = 0; i < out_size; ++i) {
        const SizeType start = (i > half) ? i - half : 0;
        const SizeType end   = i + std::min(half + 1, in_size - i);
        const SizeType count = end - start;
        std::copy(in + start, in + end, window.begin());
        auto mid = window.begin() + static_cast<IndexType>(count / 2);
        std::nth_element(window.begin(), mid, window.begin() + count);
        out[i] = *mid;
    }
}

} // namespace frost::stats::kernels
