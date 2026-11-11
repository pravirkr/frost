// Measurement phases (docs/rfi.md "Bandpass", "Moments", "IQRM", "Mask
// growth", "Broad-band", "Dropouts", "Kadane time/frequency", "zero-DM /
// zdot"): everything that turns raw samples into per-window/channel and
// per-row statistics, before flag combination and write-back (both in
// lib/cpu/rfi_cpu.cpp) consume them.

#include <algorithm>
#include <cmath>
#include <limits>

#include <frost/prepare/spectra.hpp>

#include "lib/cpu/bandpass_kernels.hpp"
#include "lib/cpu/rfi_cpu_detail.hpp"
#include "lib/prepare/bandpass_sampling.hpp"

namespace frost::prepare::rfi {

template <typename C> void RFICleaner::Impl::measure_bandpass() {
    const SizeType n = m_nchans;
    m_loc.assign(m_nwin * n, 0.0F);
    m_scale.assign(m_nwin * n, 0.0F);
    m_inv_scale.assign(m_nwin * n, 0.0F);
    m_unmeasured.assign(n, 0);
    for (SizeType w = 0; w < m_nwin; ++w) {
        const SizeType b   = window_begin(w);
        const SizeType len = window_end(w) - b;
        const SizeType m   = frost::prepare::detail::largest_power_of_five_leq(
            std::min(detail::kMaxBandpassRows, len));
        m_bandpass_rows.resize(m * n);
        float* rows = m_bandpass_rows.data();
#pragma omp parallel for schedule(static)
        for (SizeType i = 0; i < m; ++i) {
            C::unpack(row(b + ((i * len) / m)), 0, n, rows + (i * n));
        }
        std::span<float> loc(m_loc.data() + (w * n), n);
        std::span<float> scale(m_scale.data() + (w * n), n);
        spectra::measure_bandpass(
            std::span<const float>(m_bandpass_rows.data(), m * n), loc, scale,
            m, n);
#pragma omp parallel for schedule(static)
        for (SizeType c = 0; c < n; ++c) {
            if (detail::refine_bandpass(rows + c, m, n, loc[c], scale[c])) {
                m_inv_scale[(w * n) + c] = 1.0F / scale[c];
            } else {
                ++m_unmeasured[c];
            }
        }
    }
}

namespace {

// Flags channels whose statistic lies more than `k` IQRs outside
// [Q1, Q3] across the `good` channels. Does nothing when the IQR is 0.
void flag_iqr_outliers(std::span<const double> stat,
                       std::span<const uint8_t> good,
                       double k,
                       std::span<uint8_t> flags) {
    std::vector<double> vals;
    vals.reserve(stat.size());
    for (SizeType c = 0; c < stat.size(); ++c) {
        if (good[c] != 0) {
            vals.push_back(stat[c]);
        }
    }
    if (vals.size() < 4) {
        return;
    }
    std::sort(vals.begin(), vals.end());
    const double q1  = kernels::quantile_sorted(vals, 0.25);
    const double q3  = kernels::quantile_sorted(vals, 0.75);
    const double iqr = q3 - q1;
    if (!(iqr > 0.0)) {
        return;
    }
    const double lo = q1 - (k * iqr);
    const double hi = q3 + (k * iqr);
    for (SizeType c = 0; c < stat.size(); ++c) {
        if (good[c] != 0 && (stat[c] < lo || stat[c] > hi)) {
            flags[c] = 1;
        }
    }
}

} // namespace

// Per window and channel, raw and clipped moments of the robustly
// normalised samples z = (x - loc) / scale, in one pass parallel over
// (window, channel block).
template <typename C> void RFICleaner::Impl::measure_moments() {
    const SizeType n       = m_nchans;
    const SizeType nblocks = (n + detail::kChanBlock - 1) / detail::kChanBlock;
    m_moments.assign(m_nwin * n, detail::MomentSums{});
    const auto total = static_cast<IndexType>(m_nwin * nblocks);
#pragma omp parallel
    {
        std::vector<float> x(detail::kChanBlock);
        std::vector<double> prev(detail::kChanBlock);
#pragma omp for schedule(dynamic)
        for (IndexType task = 0; task < total; ++task) {
            const auto w = static_cast<SizeType>(task) / nblocks;
            const auto c0 =
                (static_cast<SizeType>(task) % nblocks) * detail::kChanBlock;
            const SizeType nc        = std::min(detail::kChanBlock, n - c0);
            detail::MomentSums* sums = m_moments.data() + (w * n) + c0;
            const float* loc         = m_loc.data() + (w * n) + c0;
            const float* inv         = m_inv_scale.data() + (w * n) + c0;
            const SizeType b         = window_begin(w);
            const SizeType e         = window_end(w);
            for (SizeType t = b; t < e; ++t) {
                C::unpack(row(t), c0, nc, x.data());
                for (SizeType j = 0; j < nc; ++j) {
                    if (m_static_mask[c0 + j] == 0 || inv[j] == 0.0F) {
                        continue;
                    }
                    const double z  = (x[j] - loc[j]) * inv[j];
                    const double z2 = z * z;
                    auto& s         = sums[j];
                    s.n += 1.0;
                    s.s1 += z;
                    s.s2 += z2;
                    s.s3 += z2 * z;
                    s.s4 += z2 * z2;
                    if (t > b) {
                        s.lag += z * prev[j];
                    }
                    prev[j] = z;
                    if (std::abs(z) <= m_clip) {
                        s.nc += 1.0;
                        s.c1 += z;
                        s.c2 += z2;
                    }
                }
            }
        }
    }
}

void RFICleaner::Impl::build_channel_mask() {
    const SizeType n = m_nchans;
    if (m_need_moments) {
        m_clip_mean.assign(m_nwin * n, 0.0F);
        m_clip_inv_std.assign(m_nwin * n, 0.0F);
        for (SizeType i = 0; i < m_nwin * n; ++i) {
            const auto& s = m_moments[i];
            if (s.nc > 1.0) {
                const double mean = s.c1 / s.nc;
                const double var  = (s.c2 / s.nc) - (mean * mean);
                m_clip_mean[i]    = static_cast<float>(mean);
                m_clip_inv_std[i] =
                    var > 0.0 ? static_cast<float>(1.0 / std::sqrt(var)) : 0.0F;
            }
        }
    }

    std::vector<uint8_t> dynamic(n, 0U);
    if (m_cfg.moments.enabled) {
        flag_moment_outliers(dynamic);
        m_report.chans_moments = static_cast<SizeType>(
            std::count(dynamic.begin(), dynamic.end(), 1U));
    }
    if (m_cfg.iqrm.enabled) {
        std::vector<float> std_data(n, 0.0F);
        std::vector<uint8_t> ignore(n, 0U);
        for (SizeType c = 0; c < n; ++c) {
            ignore[c] =
                static_cast<uint8_t>(m_mask[c] == 0 || m_unmeasured[c] > 0);
            double weight = 0.0;
            double acc    = 0.0;
            for (SizeType w = 0; w < m_nwin; ++w) {
                const auto& s = m_moments[(w * n) + c];
                if (s.n > 1.0) {
                    const double mean = s.s1 / s.n;
                    const double var  = (s.s2 / s.n) - (mean * mean);
                    const double sc   = m_scale[(w * n) + c];
                    acc += s.n * std::max(var, 0.0) * sc * sc;
                    weight += s.n;
                }
            }
            std_data[c] = weight > 0.0
                              ? static_cast<float>(std::sqrt(acc / weight))
                              : 0.0F;
        }
        const SizeType radius = m_cfg.iqrm.radius > 0
                                    ? m_cfg.iqrm.radius
                                    : std::max<SizeType>(n / 10, 1);
        const auto flags =
            iqrm_mask(std_data, radius, m_cfg.iqrm.threshold, ignore);
        for (SizeType c = 0; c < n; ++c) {
            if (flags[c] != 0 && ignore[c] == 0 && dynamic[c] == 0) {
                dynamic[c] = 1;
                ++m_report.chans_iqrm;
            }
        }
    }
    if (m_cfg.mask_grow > 0) {
        std::vector<uint8_t> grown = dynamic;
        const auto g               = m_cfg.mask_grow;
        for (SizeType c = 0; c < n; ++c) {
            if (dynamic[c] == 0) {
                continue;
            }
            const SizeType first = c > g ? c - g : 0;
            const SizeType last  = c + std::min(g, n - 1 - c);
            for (SizeType d = first; d <= last; ++d) {
                grown[d] = 1;
            }
        }
        for (SizeType c = 0; c < n; ++c) {
            if (grown[c] != 0 && dynamic[c] == 0 && m_mask[c] != 0) {
                ++m_report.chans_grown;
            }
        }
        dynamic = std::move(grown);
    }
    for (SizeType c = 0; c < n; ++c) {
        if (dynamic[c] != 0) {
            m_mask[c] = 0;
        }
    }
    m_report.chans_masked = detail::count_zeros(m_mask);
    m_good_prefix.assign(n + 1, 0);
    for (SizeType c = 0; c < n; ++c) {
        m_good_prefix[c + 1] = m_good_prefix[c] + (m_mask[c] != 0 ? 1 : 0);
    }
}

void RFICleaner::Impl::flag_moment_outliers(std::span<uint8_t> flags) const {
    const SizeType n = m_nchans;
    const auto& mc   = m_cfg.moments;
    std::vector<double> mean(n, 0.0);
    std::vector<double> stdev(n, 0.0);
    std::vector<double> skew(n, 0.0);
    std::vector<double> kurt(n, 0.0);
    std::vector<double> acf(n, 0.0);
    std::vector<uint8_t> good(n, 0U);
    for (SizeType c = 0; c < n; ++c) {
        if (m_mask[c] == 0) {
            continue;
        }
        detail::MomentSums s;
        double lag_pairs = 0.0;
        for (SizeType w = 0; w < m_nwin; ++w) {
            const auto& sw = m_moments[(w * n) + c];
            s.n += sw.n;
            s.s1 += sw.s1;
            s.s2 += sw.s2;
            s.s3 += sw.s3;
            s.s4 += sw.s4;
            s.lag += sw.lag;
            lag_pairs += std::max(sw.n - 1.0, 0.0);
        }
        if (s.n < 2.0) {
            continue;
        }
        const double m1  = s.s1 / s.n;
        const double m2  = s.s2 / s.n;
        const double m3  = s.s3 / s.n;
        const double m4  = s.s4 / s.n;
        const double var = m2 - (m1 * m1);
        if (!(var > 0.0)) {
            continue;
        }
        good[c]  = 1;
        mean[c]  = m1;
        stdev[c] = std::sqrt(var);
        skew[c] =
            (m3 - (3.0 * m1 * m2) + (2.0 * m1 * m1 * m1)) / (var * stdev[c]);
        kurt[c] = ((m4 - (4.0 * m1 * m3) + (6.0 * m1 * m1 * m2) -
                    (3.0 * m1 * m1 * m1 * m1)) /
                   (var * var)) -
                  3.0;
        acf[c] =
            lag_pairs > 0.0 ? ((s.lag / lag_pairs) - (m1 * m1)) / var : 0.0;
    }
    const double k = mc.threshold;
    if (mc.mean) {
        flag_iqr_outliers(mean, good, k, flags);
    }
    if (mc.std) {
        flag_iqr_outliers(stdev, good, k, flags);
    }
    if (mc.skewness) {
        flag_iqr_outliers(skew, good, k, flags);
    }
    if (mc.kurtosis) {
        flag_iqr_outliers(kurt, good, k, flags);
    }
    if (mc.autocorr) {
        flag_iqr_outliers(acf, good, k, flags);
    }
    if (mc.kurtosis_max > 0.0F) {
        for (SizeType c = 0; c < n; ++c) {
            if (good[c] != 0 && std::abs(kurt[c]) > mc.kurtosis_max) {
                flags[c] = 1;
            }
        }
    }
}

// One pass over every row, feeding all time- and cell-level detectors:
// the zero-DM series, spectral spread (dropouts), Kadane along
// frequency, and per-chunk zdot and Kadane-along-time accumulators.
template <typename C> void RFICleaner::Impl::scan_rows() {
    const SizeType n       = m_nchans;
    const SizeType nchunks = chunk_count();
    const SizeType chunk   = (m_nsamps + nchunks - 1) / nchunks;
    const bool zdot        = m_cfg.zero_dm == ZeroDMMode::kZdot;
    const bool kchan       = m_cfg.kadane_channel.enabled;
    const bool kspec       = m_cfg.kadane_spectrum.enabled;
    const auto& ks         = m_cfg.kadane_spectrum;
    const double min_bw_chans =
        kspec ? static_cast<double>(ks.min_bandwidth) / std::abs(m_layout.df)
              : 0.0;
    const double whole_chans =
        static_cast<double>(ks.whole_band_fraction) * static_cast<double>(n);
    const double kspec_thr = ks.threshold;

    m_s_det.assign(m_nsamps, 0.0F);
    m_s_keep.assign(m_nsamps, 0.0F);
    m_row_std.assign(m_nsamps, 0.0F);
    m_kspec_pos.assign(kspec ? m_nsamps : 0, detail::Span{});
    m_kspec_neg.assign(kspec ? m_nsamps : 0, detail::Span{});
    m_kspec_whole.assign(m_nsamps, 0U);
    m_zdot_sums.assign(zdot ? nchunks * n : 0, detail::ZdotSums{});
    m_kchan_state_pos.assign(kchan ? nchunks * n : 0, kernels::KadaneState{});
    m_kchan_state_neg.assign(kchan ? nchunks * n : 0, kernels::KadaneState{});
    m_chunk_min.assign(nchunks * n, std::numeric_limits<float>::max());
    m_chunk_max.assign(nchunks * n, std::numeric_limits<float>::lowest());
    if (m_good_prefix[n] == 0) {
        return;
    }

    const auto accept_span = [&](const kernels::Run& r, detail::Span& out) {
        if (r.length() <= 0) {
            return false;
        }
        const auto lo    = static_cast<SizeType>(r.start);
        const auto hi    = static_cast<SizeType>(r.end);
        const auto count = m_good_prefix[hi + 1] - m_good_prefix[lo];
        const auto nspan = static_cast<double>(hi - lo + 1);
        const double snr = r.sum / std::sqrt(static_cast<double>(count));
        if (snr > kspec_thr && nspan >= min_bw_chans) {
            out = {static_cast<int32_t>(lo), static_cast<int32_t>(hi)};
            return nspan > whole_chans;
        }
        return false;
    };

#pragma omp parallel
    {
        std::vector<float> x(n);
        std::vector<float> z(n);
#pragma omp for schedule(dynamic)
        for (SizeType k = 0; k < nchunks; ++k) {
            const SizeType t0 = k * chunk;
            const SizeType t1 = std::min(t0 + chunk, m_nsamps);
            detail::ZdotSums* zs =
                zdot ? m_zdot_sums.data() + (k * n) : nullptr;
            auto* kpos  = kchan ? m_kchan_state_pos.data() + (k * n) : nullptr;
            auto* kneg  = kchan ? m_kchan_state_neg.data() + (k * n) : nullptr;
            float* cmin = m_chunk_min.data() + (k * n);
            float* cmax = m_chunk_max.data() + (k * n);
            for (SizeType t = t0; t < t1; ++t) {
                const SizeType w = window_of(t);
                const float* loc = m_loc.data() + (w * n);
                const float* inv = m_inv_scale.data() + (w * n);
                C::unpack(row(t), 0, n, x.data());
                double sum_det  = 0.0;
                double sum_keep = 0.0;
                double n_keep   = 0.0;
                double sx       = 0.0;
                double sxx      = 0.0;
                double n_meas   = 0.0;
                kernels::KadaneState spec_pos;
                kernels::KadaneState spec_neg;
                for (SizeType c = 0; c < n; ++c) {
                    if (m_mask[c] == 0) {
                        continue;
                    }
                    if (inv[c] == 0.0F) {
                        z[c]    = 0.0F;
                        cmin[c] = std::min(cmin[c], x[c]);
                        cmax[c] = std::max(cmax[c], x[c]);
                        continue;
                    }
                    n_meas += 1.0;
                    const float zc = (x[c] - loc[c]) * inv[c];
                    z[c]           = zc;
                    const float cl = std::clamp(zc, -m_clip, m_clip);
                    sum_det += cl;
                    if (std::abs(zc) <= m_clip) {
                        sum_keep += zc;
                        n_keep += 1.0;
                    }
                    sx += x[c];
                    sxx += static_cast<double>(x[c]) * x[c];
                    if (kspec) {
                        spec_pos.append(static_cast<IndexType>(c), cl);
                        spec_neg.append(static_cast<IndexType>(c), -cl);
                    }
                }
                const double dn     = std::max(n_meas, 1.0);
                const double s_keep = n_keep > 0.0 ? sum_keep / n_keep : 0.0;
                m_s_det[t]          = static_cast<float>(sum_det / dn);
                m_s_keep[t]         = static_cast<float>(s_keep);
                const double mx     = sx / dn;
                m_row_std[t]        = static_cast<float>(
                    std::sqrt(std::max((sxx / dn) - (mx * mx), 0.0)));
                if (kspec) {
                    bool whole = accept_span(spec_pos.best, m_kspec_pos[t]);
                    whole = accept_span(spec_neg.best, m_kspec_neg[t]) || whole;
                    m_kspec_whole[t] = static_cast<uint8_t>(whole);
                }
                if (!zdot && !kchan) {
                    continue;
                }
                const float* cmean =
                    kchan ? m_clip_mean.data() + (w * n) : nullptr;
                const float* cinv =
                    kchan ? m_clip_inv_std.data() + (w * n) : nullptr;
                for (SizeType c = 0; c < n; ++c) {
                    if (m_mask[c] == 0) {
                        continue;
                    }
                    const bool outlier = std::abs(z[c]) > m_clip;
                    if (zdot && !outlier && inv[c] != 0.0F) {
                        auto& a = zs[c];
                        a.n += 1.0;
                        a.sz += z[c];
                        a.ss += s_keep;
                        a.sss += s_keep * s_keep;
                        a.szs += z[c] * s_keep;
                    }
                    if (kchan) {
                        const double u =
                            outlier ? 0.0 : (z[c] - cmean[c]) * cinv[c];
                        kpos[c].append(static_cast<IndexType>(t), u);
                        kneg[c].append(static_cast<IndexType>(t), -u);
                    }
                }
            }
        }
    }
}

#define FROST_RFI_INSTANTIATE(C)                                               \
    template void RFICleaner::Impl::measure_bandpass<C>();                     \
    template void RFICleaner::Impl::measure_moments<C>();                      \
    template void RFICleaner::Impl::scan_rows<C>();
FROST_RFI_FOR_EACH_CODEC(FROST_RFI_INSTANTIATE)
#undef FROST_RFI_INSTANTIATE

} // namespace frost::prepare::rfi
