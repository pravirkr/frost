#pragma once

#include <algorithm>
#include <cmath>

#include <frost/common/policies.hpp>

namespace frost::prepare::rfi::detail {

// The remedian median/MAD seed is refined by sigma clipping: on integer
// data with an rms of a few counts the MAD is itself quantised and can be
// ~30% off, which would turn a 5-sigma threshold into ~3.7 sigma. Returns
// false for a channel with no variance at all.
constexpr double kClipSigma   = 4.5;
constexpr int kClipIterations = 2;

inline bool refine_bandpass(
    const float* v, SizeType m, SizeType stride, float& loc, float& scale) {
    float lo = v[0];
    float hi = v[0];
    for (SizeType i = 1; i < m; ++i) {
        lo = std::min(lo, v[i * stride]);
        hi = std::max(hi, v[i * stride]);
    }
    if (!(hi > lo)) {
        loc   = lo;
        scale = 0.0F;
        return false;
    }
    double centre = loc;
    double s      = scale;
    if (!(s > 0.0)) {
        double mean = 0.0;
        for (SizeType i = 0; i < m; ++i) {
            mean += v[i * stride];
        }
        mean /= static_cast<double>(m);
        double ss = 0.0;
        for (SizeType i = 0; i < m; ++i) {
            ss += (v[i * stride] - mean) * (v[i * stride] - mean);
        }
        s = std::sqrt(ss / static_cast<double>(m));
    }
    for (int it = 0; it < kClipIterations; ++it) {
        const double cut = kClipSigma * s;
        double count     = 0.0;
        double sum       = 0.0;
        for (SizeType i = 0; i < m; ++i) {
            const double d = v[i * stride] - centre;
            if (std::abs(d) <= cut) {
                count += 1.0;
                sum += d;
            }
        }
        if (count < 2.0) {
            break;
        }
        const double shift = sum / count;
        double ss          = 0.0;
        for (SizeType i = 0; i < m; ++i) {
            const double d = v[i * stride] - centre;
            if (std::abs(d) <= cut) {
                ss += (d - shift) * (d - shift);
            }
        }
        const double clipped = std::sqrt(ss / count);
        if (!(clipped > 0.0)) {
            break;
        }
        centre += shift;
        s = clipped;
    }
    loc   = static_cast<float>(centre);
    scale = static_cast<float>(s);
    return true;
}

} // namespace frost::prepare::rfi::detail
