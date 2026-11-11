#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <span>
#include <type_traits>

#include <frost/common/policies.hpp>

#include "lib/prepare/sample_codec.hpp"

namespace frost::prepare::rfi::kernels {

constexpr uint64_t splitmix64(uint64_t x) {
    x += 0x9E3779B97F4A7C15ULL;
    x = (x ^ (x >> 30U)) * 0xBF58476D1CE4E5B9ULL;
    x = (x ^ (x >> 27U)) * 0x94D049BB133111EBULL;
    return x ^ (x >> 31U);
}

constexpr uint64_t
hash(uint64_t a, uint64_t b, uint64_t c, uint64_t d, uint64_t e) {
    return splitmix64(
        splitmix64(splitmix64(splitmix64(splitmix64(a) ^ b) ^ c) ^ d) ^ e);
}

/** Uniform integer in [lo, hi]. */
constexpr SizeType uniform_index(uint64_t h, SizeType lo, SizeType hi) {
    return lo + static_cast<SizeType>(h % static_cast<uint64_t>(hi - lo + 1));
}

/** Uniform double in (0, 1). */
constexpr double uniform_open(uint64_t h) {
    return (static_cast<double>(h >> 11U) + 0.5) * 0x1.0p-53;
}

/** Standard normal deviate (Box-Muller). */
inline float normal(uint64_t h) {
    constexpr double kTwoPi = 6.283185307179586;
    const double u1         = uniform_open(h);
    const double u2         = uniform_open(splitmix64(h));
    return static_cast<float>(std::sqrt(-2.0 * std::log(u1)) *
                              std::cos(kTwoPi * u2));
}

/** A run [start, end] of a sequence and its sum; empty when end < start. */
struct Run {
    double sum      = 0.0;
    IndexType start = 0;
    IndexType end   = -1;

    IndexType length() const { return end - start + 1; }
};

/**
 * Maximum-sum contiguous run (Kadane) over a sequence indexed by
 * increasing, not necessarily consecutive, positions. Built by appending
 * elements in order; states of adjacent pieces `combine` associatively,
 * so a long sequence can be split into chunks scanned independently.
 * Empty runs (sum 0) are allowed, so only positive runs are reported.
 */
struct KadaneState {
    double total    = 0.0;
    IndexType first = 0;
    IndexType last  = -1;
    bool empty      = true;
    Run prefix; // best run starting at `first`
    Run suffix; // best run ending at `last`
    Run best;

    void append(IndexType i, double u) {
        if (empty) {
            empty  = false;
            first  = i;
            prefix = {0.0, i, i - 1};
            suffix = {0.0, i, i - 1};
            best   = suffix;
        }
        last = i;
        total += u;
        if (total > prefix.sum) {
            prefix = {total, first, i};
        }
        if (suffix.sum + u > 0.0) {
            if (suffix.length() <= 0) {
                suffix.start = i;
            }
            suffix.sum += u;
            suffix.end = i;
        } else {
            suffix = {0.0, i + 1, i};
        }
        if (suffix.sum > best.sum) {
            best = suffix;
        }
    }

    /** State of the concatenation `a` then `b`. */
    static KadaneState combine(const KadaneState& a, const KadaneState& b) {
        if (a.empty) {
            return b;
        }
        if (b.empty) {
            return a;
        }
        KadaneState r;
        r.empty  = false;
        r.first  = a.first;
        r.last   = b.last;
        r.total  = a.total + b.total;
        r.prefix = a.prefix;
        if (a.total + b.prefix.sum > r.prefix.sum) {
            r.prefix = {a.total + b.prefix.sum, a.first, b.prefix.end};
        }
        const IndexType join = a.suffix.length() > 0 ? a.suffix.start : b.first;
        r.suffix             = b.suffix;
        if (b.total + a.suffix.sum > r.suffix.sum) {
            r.suffix = {b.total + a.suffix.sum, join, b.last};
        }
        r.best = a.best;
        if (b.best.sum > r.best.sum) {
            r.best = b.best;
        }
        const double cross = a.suffix.sum + b.prefix.sum;
        if (cross > r.best.sum) {
            r.best = {cross, join, b.prefix.end};
        }
        return r;
    }
};

/** numpy-style (linear interpolation) quantile of ascending `sorted`. */
inline double quantile_sorted(std::span<const double> sorted, double q) {
    const double h  = static_cast<double>(sorted.size() - 1) * q;
    const auto lo   = static_cast<SizeType>(h);
    const auto hi   = std::min(lo + 1, sorted.size() - 1);
    const double fr = h - static_cast<double>(lo);
    return sorted[lo] + (fr * (sorted[hi] - sorted[lo]));
}

} // namespace frost::prepare::rfi::kernels
