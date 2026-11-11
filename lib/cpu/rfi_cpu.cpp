// RFICleaner orchestration, flag combination and write-back: construction,
// the public execute() entry points, memory planning, combining the row
// scan's statistics into final flags, and the fill/transform pass that
// writes cleaned samples back. Measurement (bandpass, moments, scan) lives
// in lib/cpu/rfi_cpu_measure.cpp; see docs/rfi.md for the strategy
// taxonomy and lib/cpu/rfi_cpu_detail.hpp for the shared
// RFICleaner::Impl declaration.

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <string>
#include <utility>

#include <frost/common/numeric.hpp>

#include "lib/cpu/rfi_cpu_detail.hpp"
#include "lib/prepare/device_rfi.hpp"
#include "lib/prepare/rfi_validation.hpp"

namespace frost::prepare::rfi {

RFICleaner::Impl::Impl(RfiConfig cfg,
                       const BlockLayout& layout,
                       float baseline_length)
    : m_cfg(std::move(cfg)),
      m_layout(layout),
      m_nchans(layout.nchans),
      m_row_bytes(0) {
    validate(baseline_length);
    m_row_bytes =
        layout.nchans * static_cast<SizeType>(bits_of(layout.dtype)) / 8;
    const double window_samples =
        static_cast<double>(baseline_length) / layout.tsamp;
    m_window_len =
        std::max(static_cast<SizeType>(window_samples), detail::kMinWindow);
    m_smooth_radius =
        std::max<SizeType>(static_cast<SizeType>(window_samples / 2), 1);
    m_time_fill_radius = std::max<SizeType>(m_smooth_radius / 4, 1);
    m_chan_fill_radius = std::max<SizeType>(m_nchans / 60, 1);
    m_clip             = m_cfg.narrow.threshold;
    m_narrow_active = m_cfg.narrow.enabled &&
                      (is_floating(layout.dtype) || bits_of(layout.dtype) > 4);
    m_transform     = m_cfg.zero_dm != ZeroDMMode::kOff || m_cfg.equalize;
    m_need_moments  = m_cfg.moments.enabled || m_cfg.iqrm.enabled ||
                      m_cfg.kadane_channel.enabled;
    if (is_floating(layout.dtype)) {
        m_eq_loc   = 0.0F;
        m_eq_scale = 1.0F;
    } else {
        const int nbits = bits_of(layout.dtype);
        m_eq_loc        = static_cast<float>(uint64_t{1} << (nbits - 1));
        m_eq_scale      = static_cast<float>(uint64_t{1} << nbits) / 16.0F;
    }
    build_static_mask();
    m_mask                = m_static_mask;
    m_report.chans_zapped = detail::count_zeros(m_static_mask);
    m_report.chans_masked = m_report.chans_zapped;
}

void RFICleaner::Impl::execute(std::span<uint8_t> block, bool validated) {
    // The byte API can change calibration/masks independently of normalized
    // execution. Only a successful normalized full block re-establishes it.
    m_tail_ready = false;
    if (block.size() % m_row_bytes != 0) {
        throw std::invalid_argument(
            "RFICleaner: block must hold a whole number of time samples");
    }
    const SizeType nsamps = block.size() / m_row_bytes;
    if (nsamps > m_layout.max_nsamps) {
        throw std::invalid_argument(
            "RFICleaner: block exceeds the layout's max_nsamps (" +
            std::to_string(m_layout.max_nsamps) + ")");
    }
    if (!validated && is_floating(m_layout.dtype))
        detail::validate_float_samples(block, m_nchans);
    m_report              = RfiReport{};
    m_report.nsamps       = nsamps;
    m_mask                = m_static_mask;
    m_report.chans_zapped = detail::count_zeros(m_static_mask);
    m_report.chans_masked = m_report.chans_zapped;
    if (nsamps < std::max(detail::kMinWindow, detail::kMinBlockSamps)) {
        ++m_block_index;
        // Too short for stable statistics: only the manual zap list
        // applies, and the (in-place) data are left unchanged.
        return;
    }
    m_data   = block.data();
    m_nsamps = nsamps;
    m_nwin   = std::max<SizeType>(nsamps / m_window_len, 1);

    kernels::dispatch(m_layout.dtype, [&](auto codec) {
        using C = decltype(codec);
        auto t  = detail::Clock::now();
        measure_bandpass<C>();
        m_report.t_bandpass =
            std::chrono::duration<double>(detail::Clock::now() - t).count();

        t = detail::Clock::now();
        if (m_need_moments) {
            measure_moments<C>();
        }
        build_channel_mask();
        m_report.t_moments =
            std::chrono::duration<double>(detail::Clock::now() - t).count();

        t = detail::Clock::now();
        scan_rows<C>();
        m_report.t_scan =
            std::chrono::duration<double>(detail::Clock::now() - t).count();

        t = detail::Clock::now();
        build_flags();
        m_report.t_flags =
            std::chrono::duration<double>(detail::Clock::now() - t).count();

        t = detail::Clock::now();
        apply<C>();
        m_report.t_apply =
            std::chrono::duration<double>(detail::Clock::now() - t).count();
    });
    ++m_block_index;
}

SizeType RFICleaner::Impl::memory_usage() const noexcept {
    return m_static_mask.capacity() * sizeof(uint8_t) +
           m_mask.capacity() * sizeof(uint8_t) +
           m_work.capacity() * sizeof(float) +
           m_normalized.capacity() * sizeof(float) +
           (m_tail_loc.capacity() + m_tail_scale.capacity() +
            m_tail_output_loc.capacity() + m_tail_output_scale.capacity()) *
               sizeof(float) +
           m_loc.capacity() * sizeof(float) +
           m_scale.capacity() * sizeof(float) +
           m_inv_scale.capacity() * sizeof(float) +
           m_moments.capacity() * sizeof(detail::MomentSums) +
           m_clip_mean.capacity() * sizeof(float) +
           m_clip_inv_std.capacity() * sizeof(float) +
           m_bandpass_rows.capacity() * sizeof(float) +
           m_unmeasured.capacity() * sizeof(SizeType) +
           m_s_det.capacity() * sizeof(float) +
           m_s_keep.capacity() * sizeof(float) +
           m_row_std.capacity() * sizeof(float) +
           m_kspec_pos.capacity() * sizeof(detail::Span) +
           m_kspec_neg.capacity() * sizeof(detail::Span) +
           m_kspec_whole.capacity() * sizeof(uint8_t) +
           m_time_flags.capacity() * sizeof(uint8_t) +
           m_good_prefix.capacity() * sizeof(SizeType) +
           m_zdot_a.capacity() * sizeof(float) +
           m_zdot_b.capacity() * sizeof(float) +
           m_kchan_pos.capacity() * sizeof(detail::Segment) +
           m_kchan_neg.capacity() * sizeof(detail::Segment) +
           m_zdot_sums.capacity() * sizeof(detail::ZdotSums) +
           m_kchan_state_pos.capacity() * sizeof(kernels::KadaneState) +
           m_kchan_state_neg.capacity() * sizeof(kernels::KadaneState) +
           m_chunk_min.capacity() * sizeof(float) +
           m_chunk_max.capacity() * sizeof(float);
}

std::span<const float>
RFICleaner::Impl::execute_normalized(std::span<uint8_t> block, SizeType valid) {
    if (block.size() % m_row_bytes != 0 ||
        block.size() / m_row_bytes > m_layout.max_nsamps) {
        throw std::invalid_argument(
            "RFICleaner: invalid normalized block size");
    }
    const SizeType capacity = block.size() / m_row_bytes;
    if (valid == 0)
        valid = capacity;
    if (valid > capacity)
        throw std::invalid_argument("RFICleaner: nsamps exceeds block");
    if (is_floating(m_layout.dtype))
        detail::validate_float_samples(block.first(valid * m_row_bytes),
                                       m_nchans);
    m_work.resize(valid * m_nchans);
    kernels::dispatch(m_layout.dtype, [&](auto codec) {
        using C = decltype(codec);
#pragma omp parallel for schedule(static)
        for (SizeType t = 0; t < valid; ++t) {
            C::unpack(block.data() + t * m_row_bytes, 0, m_nchans,
                      m_work.data() + t * m_nchans);
        }
    });
    const bool short_block =
        valid < std::max(detail::kMinWindow, detail::kMinBlockSamps);
    if (short_block && m_tail_ready) {
        normalize_short(capacity, valid);
        return m_normalized;
    }
    // Reuse the full suite on float samples so fills and transforms are
    // not rounded back into the input's integer representation. Keep the
    // original narrow-band eligibility and equalisation calibration.
    const auto dtype     = m_layout.dtype;
    const auto row_bytes = m_row_bytes;
    m_layout.dtype       = DataType::kFloat32;
    m_row_bytes          = m_nchans * sizeof(float);
    try {
        execute({reinterpret_cast<uint8_t*>(m_work.data()),
                 m_work.size() * sizeof(float)},
                true);
    } catch (...) {
        m_layout.dtype = dtype;
        m_row_bytes    = row_bytes;
        throw;
    }
    m_layout.dtype = dtype;
    m_row_bytes    = row_bytes;
    if (short_block) {
        m_nsamps = valid;
        m_nwin   = 1;
        m_loc.assign(m_nchans, 0.0F);
        m_scale.assign(m_nchans, 0.0F);
    } else {
        const auto last = (m_nwin - 1) * m_nchans;
        m_tail_loc.assign(m_loc.begin() + last, m_loc.end());
        m_tail_scale.assign(m_scale.begin() + last, m_scale.end());
    }
    if (short_block || m_transform) {
        // A zero-DM/equalisation transform changes channel statistics.
        // Measure the transformed, cleaned data, excluding padded rows.
#pragma omp parallel for schedule(static)
        for (SizeType i = 0; i < m_nwin * m_nchans; ++i) {
            const SizeType w = i / m_nchans, c = i % m_nchans;
            const SizeType begin = window_begin(w), end = window_end(w);
            double mean = 0.0, variance = 0.0;
            for (SizeType t = begin; t < end; ++t) {
                const double x     = m_work[t * m_nchans + c];
                const double delta = x - mean;
                mean += delta / static_cast<double>(t - begin + 1);
                variance += delta * (x - mean);
            }
            m_loc[i]   = static_cast<float>(mean);
            m_scale[i] = end > begin
                             ? static_cast<float>(std::sqrt(
                                   variance / static_cast<double>(end - begin)))
                             : 0.0F;
        }
    }
    if (!short_block) {
        const auto last = (m_nwin - 1) * m_nchans;
        m_tail_output_loc.assign(m_loc.begin() + last, m_loc.end());
        m_tail_output_scale.assign(m_scale.begin() + last, m_scale.end());
    }
    m_normalized.assign(capacity * m_nchans, 0.0F);
#pragma omp parallel for schedule(static)
    for (SizeType c = 0; c < m_nchans; ++c) {
        if (m_mask[c] == 0)
            continue;
        const SizeType dst = m_layout.df < 0.0 ? m_nchans - 1 - c : c;
        for (SizeType t = 0; t < valid; ++t) {
            const SizeType i  = window_of(t) * m_nchans + c;
            const float scale = m_scale[i] > 0.0F ? 1.0F / m_scale[i] : 1.0F;
            m_normalized[dst * capacity + t] =
                (m_work[t * m_nchans + c] - m_loc[i]) * scale;
        }
    }
    m_tail_ready = !short_block;
    return m_normalized;
}

void RFICleaner::Impl::normalize_short(SizeType capacity, SizeType valid) {
    m_report                    = RfiReport{};
    m_report.nsamps             = valid;
    m_report.chans_zapped       = detail::count_zeros(m_static_mask);
    m_report.chans_masked       = detail::count_zeros(m_mask);
    m_report.calibration_reused = true;
    m_normalized.assign(capacity * m_nchans, 0.0F);
    // Only transforms are applied: the small tail cannot support a new
    // channel mask or time/cell-excision statistics. Zdot keeps the preceding
    // regression, rather than fitting the pulse in this tail.
#pragma omp parallel
    {
        std::vector<float> z(m_nchans);
#pragma omp for schedule(static)
        for (SizeType t = 0; t < valid; ++t) {
            double sum     = 0;
            SizeType count = 0;
            for (SizeType c = 0; c < m_nchans; ++c) {
                z[c] = (m_work[t * m_nchans + c] - m_tail_loc[c]) *
                       (m_tail_scale[c] > 0 ? 1.0F / m_tail_scale[c] : 1.0F);
                if (m_mask[c] && m_tail_scale[c] > 0 &&
                    std::abs(z[c]) <= m_clip) {
                    sum += z[c];
                    ++count;
                }
            }
            const float band_mean = count ? static_cast<float>(sum / count) : 0;
            for (SizeType c = 0; c < m_nchans; ++c) {
                if (!m_mask[c]) {
                    continue;
                }
                float term = 0;
                if (m_cfg.zero_dm == ZeroDMMode::kSubtract) {
                    term = band_mean;
                } else if (m_cfg.zero_dm == ZeroDMMode::kZdot) {
                    term = m_zdot_a[c] * band_mean + m_zdot_b[c];
                }
                const float transformed =
                    m_tail_scale[c] <= 0 ? m_work[t * m_nchans + c]
                    : m_cfg.equalize     ? m_eq_loc + m_eq_scale * (z[c] - term)
                    : m_transform
                        ? m_tail_loc[c] + m_tail_scale[c] * (z[c] - term)
                        : m_work[t * m_nchans + c];
                const float scale  = m_tail_output_scale[c] > 0
                                         ? 1.0F / m_tail_output_scale[c]
                                         : 1.0F;
                const SizeType dst = m_layout.df < 0 ? m_nchans - 1 - c : c;
                m_normalized[dst * capacity + t] =
                    (transformed - m_tail_output_loc[c]) * scale;
            }
        }
    }
    ++m_block_index;
}

void RFICleaner::Impl::validate(float baseline_length) const {
    m_cfg.validate();
    detail::validate_layout(m_layout, baseline_length);
    const auto& l = m_layout;
    if ((m_cfg.zero_dm != ZeroDMMode::kOff || m_cfg.equalize) &&
        !is_floating(l.dtype) && bits_of(l.dtype) < 8) {
        throw std::invalid_argument(
            "RFICleaner: zero_dm and equalize need >= 8-bit data");
    }
    if ((m_cfg.kadane_spectrum.enabled || !m_cfg.zap_freqs.empty()) &&
        l.df == 0.0) {
        throw std::invalid_argument(
            "RFICleaner: zap_freqs and kadane_spectrum need the channel "
            "spacing (df)");
    }
}

void RFICleaner::Impl::build_static_mask() {
    m_static_mask.assign(m_nchans, 1U);
    apply_manual_killmask(m_static_mask, m_cfg.zap_chans);
    for (SizeType c = 0; c < m_nchans; ++c) {
        const double f = m_layout.f0 + (static_cast<double>(c) * m_layout.df);
        for (const auto& r : m_cfg.zap_freqs) {
            if (f >= r.lo && f <= r.hi) {
                m_static_mask[c] = 0;
            }
        }
    }
}

void RFICleaner::Impl::build_flags() {
    const SizeType n       = m_nchans;
    const SizeType nchunks = chunk_count();
    // Dead: no variance in the bandpass sample of any window, and
    // constant over the whole block.
    for (SizeType c = 0; c < n; ++c) {
        if (m_mask[c] == 0 || m_unmeasured[c] < m_nwin) {
            continue;
        }
        float lo = std::numeric_limits<float>::max();
        float hi = std::numeric_limits<float>::lowest();
        for (SizeType k = 0; k < nchunks; ++k) {
            lo = std::min(lo, m_chunk_min[(k * n) + c]);
            hi = std::max(hi, m_chunk_max[(k * n) + c]);
        }
        if (!(hi > lo)) {
            m_mask[c] = 0;
            ++m_report.chans_dead;
        }
    }
    m_report.chans_masked = detail::count_zeros(m_mask);

    m_time_flags.assign(m_nsamps, 0U);
    if (m_good_prefix[n] == 0) {
        return;
    }
    if (m_cfg.broad.enabled) {
        m_report.samps_broad = pipeline::device::broadband_time_flags(
            m_s_det, m_smooth_radius, m_cfg.broad.threshold,
            m_cfg.broad.boxcar_max, m_time_flags);
    }
    if (m_cfg.dropout.enabled) {
        std::vector<float> spread = m_row_std;
        const auto mid = spread.begin() + static_cast<IndexType>(m_nsamps / 2);
        std::nth_element(spread.begin(), mid, spread.end());
        const float cut = m_cfg.dropout.min_std_fraction * *mid;
        std::vector<uint8_t> drop(m_nsamps, 0U);
        for (SizeType t = 0; t < m_nsamps; ++t) {
            if (m_row_std[t] < cut) {
                drop[t] = 1;
                if (t > 0) {
                    drop[t - 1] = 1;
                }
                if (t + 1 < m_nsamps) {
                    drop[t + 1] = 1;
                }
            }
        }
        for (SizeType t = 0; t < m_nsamps; ++t) {
            if (drop[t] != 0 && m_time_flags[t] == 0) {
                m_time_flags[t] = 1;
                ++m_report.samps_dropout;
            }
        }
    }
    if (m_cfg.kadane_spectrum.enabled) {
        for (SizeType t = 0; t < m_nsamps; ++t) {
            if (m_kspec_whole[t] != 0 && m_time_flags[t] == 0) {
                m_time_flags[t] = 1;
                ++m_report.samps_kadane;
            }
        }
    }
    m_report.samps_flagged = static_cast<SizeType>(
        std::count(m_time_flags.begin(), m_time_flags.end(), 1U));

    if (m_cfg.zero_dm == ZeroDMMode::kZdot) {
        m_zdot_a.assign(n, 0.0F);
        m_zdot_b.assign(n, 0.0F);
        for (SizeType c = 0; c < n; ++c) {
            detail::ZdotSums s;
            for (SizeType k = 0; k < nchunks; ++k) {
                const auto& p = m_zdot_sums[(k * n) + c];
                s.n += p.n;
                s.sz += p.sz;
                s.ss += p.ss;
                s.sss += p.sss;
                s.szs += p.szs;
            }
            if (s.n < 2.0) {
                continue;
            }
            const double mz    = s.sz / s.n;
            const double ms    = s.ss / s.n;
            const double var_s = (s.sss / s.n) - (ms * ms);
            const double cov   = (s.szs / s.n) - (mz * ms);
            const double a     = var_s > 0.0 ? cov / var_s : 0.0;
            m_zdot_a[c]        = static_cast<float>(a);
            m_zdot_b[c]        = static_cast<float>(mz - (a * ms));
        }
    }
    if (m_cfg.kadane_channel.enabled) {
        const auto& kc = m_cfg.kadane_channel;
        const double min_len =
            static_cast<double>(kc.min_duration) / m_layout.tsamp;
        m_kchan_pos.assign(n, detail::Segment{});
        m_kchan_neg.assign(n, detail::Segment{});
        const auto accept = [&](const kernels::Run& r, detail::Segment& out) {
            const auto len = static_cast<double>(r.length());
            if (len > 0.0 && len >= min_len &&
                r.sum / std::sqrt(len) > kc.threshold) {
                out = {r.start, r.end};
            }
        };
#pragma omp parallel for schedule(static)
        for (SizeType c = 0; c < n; ++c) {
            if (m_mask[c] == 0) {
                continue;
            }
            kernels::KadaneState pos;
            kernels::KadaneState neg;
            for (SizeType k = 0; k < nchunks; ++k) {
                pos = kernels::KadaneState::combine(
                    pos, m_kchan_state_pos[(k * n) + c]);
                neg = kernels::KadaneState::combine(
                    neg, m_kchan_state_neg[(k * n) + c]);
            }
            accept(pos.best, m_kchan_pos[c]);
            accept(neg.best, m_kchan_neg[c]);
        }
    }
}

float RFICleaner::Impl::zero_dm_term(SizeType c, SizeType t) const {
    switch (m_cfg.zero_dm) {
    case ZeroDMMode::kSubtract:
        return m_s_keep[t];
    case ZeroDMMode::kZdot:
        return (m_zdot_a[c] * m_s_keep[t]) + m_zdot_b[c];
    case ZeroDMMode::kOff:
        break;
    }
    return 0.0F;
}

uint64_t RFICleaner::Impl::draw(detail::Stream stream,
                                SizeType t,
                                SizeType c,
                                int attempt) const {
    return kernels::hash(m_cfg.seed, m_block_index,
                         (static_cast<uint64_t>(stream) << 56U) ^ t, c,
                         static_cast<uint64_t>(attempt));
}

// Value `v` (in noise units about the channel's local level) in the
// output's units.
float RFICleaner::Impl::level(SizeType w, SizeType c, float v) const {
    return m_cfg.equalize
               ? m_eq_loc + (m_eq_scale * v)
               : m_loc[(w * m_nchans) + c] + (m_scale[(w * m_nchans) + c] * v);
}

template <typename C>
float RFICleaner::Impl::denormalise(SizeType w, SizeType c, float v) const {
    return C::quantise(level(w, c, v));
}

// Inverse of denormalise, for an already-written output sample.
float RFICleaner::Impl::to_noise_units(SizeType w,
                                       SizeType c,
                                       float out) const {
    if (m_cfg.equalize) {
        return (out - m_eq_loc) / m_eq_scale;
    }
    const SizeType i = (w * m_nchans) + c;
    return (out - m_loc[i]) * m_inv_scale[i];
}

template <typename C>
float RFICleaner::Impl::fill_value(detail::Stream stream,
                                   SizeType w,
                                   SizeType t,
                                   SizeType c) const {
    const float v = m_cfg.fill == FillMode::kNoise
                        ? kernels::normal(draw(stream, t, c, 0))
                        : 0.0F;
    return denormalise<C>(w, c, v);
}

template <typename C> void RFICleaner::Impl::apply() {
    const SizeType n      = m_nchans;
    const bool kchan      = m_cfg.kadane_channel.enabled;
    const bool kspec      = m_cfg.kadane_spectrum.enabled;
    const bool nbr        = m_cfg.fill == FillMode::kNeighbour;
    const auto ns         = static_cast<IndexType>(m_nsamps);
    SizeType cells_narrow = 0;
    SizeType cells_kspec  = 0;
    SizeType cells_kchan  = 0;
    SizeType fallbacks    = 0;

    // Unflagged rows first: each reads and writes only itself.
#pragma omp parallel reduction(+ : cells_narrow, cells_kspec, cells_kchan,     \
                                   fallbacks)
    {
        std::vector<float> x(n);
        std::vector<float> z(n);
        std::vector<float> v(n);
        std::vector<uint8_t> state(n);
#pragma omp for schedule(static)
        for (IndexType ti = 0; ti < ns; ++ti) {
            const auto t = static_cast<SizeType>(ti);
            if (m_time_flags[t] != 0) {
                continue;
            }
            const SizeType w = window_of(t);
            const float* loc = m_loc.data() + (w * n);
            const float* inv = m_inv_scale.data() + (w * n);
            uint8_t* out     = row(t);
            C::unpack(out, 0, n, x.data());
            for (SizeType c = 0; c < n; ++c) {
                if (m_mask[c] == 0) {
                    state[c] = detail::kMasked;
                    continue;
                }
                if (inv[c] == 0.0F) {
                    state[c] = detail::kUnmeasured;
                    continue;
                }
                z[c] = (x[c] - loc[c]) * inv[c];
                if (m_narrow_active && std::abs(z[c]) > m_clip) {
                    state[c] = detail::kNarrow;
                } else if (kspec && (m_kspec_pos[t].contains(c) ||
                                     m_kspec_neg[t].contains(c))) {
                    state[c] = detail::kKadaneSpectrum;
                } else if (kchan && (m_kchan_pos[c].contains(t) ||
                                     m_kchan_neg[c].contains(t))) {
                    state[c] = detail::kKadaneChannel;
                } else {
                    state[c] = detail::kClean;
                    v[c]     = z[c] - zero_dm_term(c, t);
                }
            }
            for (SizeType c = 0; c < n; ++c) {
                switch (state[c]) {
                case detail::kClean:
                    if (m_transform) {
                        C::write(out, c, denormalise<C>(w, c, v[c]));
                    }
                    continue;
                case detail::kUnmeasured:
                    continue;
                case detail::kMasked:
                    C::write(out, c,
                             fill_value<C>(detail::kStreamMasked, w, t, c));
                    continue;
                case detail::kNarrow:
                    ++cells_narrow;
                    break;
                case detail::kKadaneSpectrum:
                    ++cells_kspec;
                    break;
                default:
                    ++cells_kchan;
                    break;
                }
                if (!nbr) {
                    C::write(out, c,
                             fill_value<C>(detail::kStreamNoise, w, t, c));
                    continue;
                }
                const SizeType r  = m_chan_fill_radius;
                const SizeType lo = c > r ? c - r : 0;
                const SizeType hi = std::min(c + r, n - 1);
                float value       = 0.0F;
                bool found        = false;
                for (int a = 0; a < detail::kMaxDraws && !found; ++a) {
                    const SizeType src = kernels::uniform_index(
                        draw(detail::kStreamCell, t, c, a), lo, hi);
                    if (state[src] == detail::kClean) {
                        value = v[src];
                        found = true;
                    }
                }
                fallbacks += found ? 0 : 1;
                C::write(out, c, denormalise<C>(w, c, value));
            }
        }
    }

    // Then flagged rows, which copy from the now-final unflagged rows.
    std::vector<SizeType> flagged;
    for (SizeType t = 0; t < m_nsamps; ++t) {
        if (m_time_flags[t] != 0) {
            flagged.push_back(t);
        }
    }
    // With zero-DM on, a refilled spectrum is rebuilt in noise units and
    // has its band mean removed too; otherwise its channels, drawn from
    // different times, would not cancel like every kept spectrum's do
    // and it would stand out in the (now flat) zero-DM series.
    const bool rezero = m_cfg.zero_dm != ZeroDMMode::kOff;
    const auto nf     = static_cast<IndexType>(flagged.size());
#pragma omp parallel reduction(+ : fallbacks)
    {
        std::vector<float> v(n);
#pragma omp for schedule(static)
        for (IndexType fi = 0; fi < nf; ++fi) {
            const SizeType t  = flagged[static_cast<SizeType>(fi)];
            const SizeType w  = window_of(t);
            const SizeType r  = m_time_fill_radius;
            const SizeType lo = t > r ? t - r : 0;
            const SizeType hi = std::min(t + r, m_nsamps - 1);
            uint8_t* out      = row(t);
            double sum        = 0.0;
            double count      = 0.0;
            for (SizeType c = 0; c < n; ++c) {
                if (m_mask[c] == 0) {
                    C::write(out, c,
                             fill_value<C>(detail::kStreamMasked, w, t, c));
                    continue;
                }
                v[c] = 0.0F;
                if (!nbr) {
                    v[c] = m_cfg.fill == FillMode::kNoise
                               ? kernels::normal(
                                     draw(detail::kStreamNoise, t, c, 0))
                               : 0.0F;
                } else {
                    bool found = false;
                    for (int a = 0; a < detail::kMaxDraws && !found; ++a) {
                        const SizeType src = kernels::uniform_index(
                            draw(detail::kStreamTime, t, c, a), lo, hi);
                        if (m_time_flags[src] != 0) {
                            continue;
                        }
                        found = true;
                        if (rezero) {
                            v[c] = to_noise_units(window_of(src), c,
                                                  C::read(row(src), c));
                        } else {
                            C::copy(row(src), out, c);
                        }
                    }
                    if (!found) {
                        ++fallbacks;
                    }
                    if (found && !rezero) {
                        continue;
                    }
                }
                if (rezero && m_inv_scale[(w * n) + c] != 0.0F) {
                    sum += v[c];
                    count += 1.0;
                }
                if (!rezero) {
                    C::write(out, c, denormalise<C>(w, c, v[c]));
                }
            }
            if (!rezero) {
                continue;
            }
            const auto mean =
                static_cast<float>(count > 0.0 ? sum / count : 0.0);
            // The sources sit on the output's quantisation grid, so after
            // the common shift every cell would round the same way and
            // bias the band sum by up to nchans/2 counts; carrying the
            // rounding error across channels keeps it within half a count.
            float carry = 0.0F;
            for (SizeType c = 0; c < n; ++c) {
                if (m_mask[c] == 0) {
                    continue;
                }
                const float want = level(w, c, v[c] - mean) + carry;
                const float q    = C::quantise(want);
                carry            = std::clamp(want - q, -0.5F, 0.5F);
                C::write(out, c, q);
            }
        }
    }
    m_report.cells_narrow          = cells_narrow;
    m_report.cells_kadane_spectrum = cells_kspec;
    m_report.cells_kadane_channel  = cells_kchan;
    m_report.fill_fallbacks        = fallbacks;
}

RFICleaner::RFICleaner(RfiConfig cfg,
                       const BlockLayout& layout,
                       float baseline_length)
    : m_impl(std::make_unique<Impl>(std::move(cfg), layout, baseline_length)) {}

RFICleaner::~RFICleaner()                                = default;
RFICleaner::RFICleaner(RFICleaner&&) noexcept            = default;
RFICleaner& RFICleaner::operator=(RFICleaner&&) noexcept = default;

void RFICleaner::execute(std::span<uint8_t> block) { m_impl->execute(block); }

void RFICleaner::execute(std::span<const uint8_t> in, std::span<uint8_t> out) {
    if (out.size() < in.size()) {
        throw std::invalid_argument("RFICleaner: out smaller than in");
    }
    if (in.data() != out.data()) {
        std::copy(in.begin(), in.end(), out.begin());
    }
    m_impl->execute(out.first(in.size()));
}

std::span<const float> RFICleaner::execute_normalized(std::span<uint8_t> block,
                                                      SizeType nsamps) {
    return m_impl->execute_normalized(block, nsamps);
}

std::span<const uint8_t> RFICleaner::channel_mask() const {
    return m_impl->channel_mask();
}

const RfiReport& RFICleaner::report() const { return m_impl->report(); }

const RfiConfig& RFICleaner::config() const { return m_impl->config(); }

SizeType RFICleaner::estimate_memory(const RfiConfig& cfg,
                                     const BlockLayout& layout,
                                     float baseline_length,
                                     SizeType threads) {
    cfg.validate();
    detail::validate_layout(layout, baseline_length);
    const auto mul = [](SizeType x, SizeType y) {
        if (x && y > std::numeric_limits<SizeType>::max() / x)
            throw std::overflow_error("RFI memory estimate overflows");
        return x * y;
    };
    const auto add = [](SizeType x, SizeType y) {
        if (y > std::numeric_limits<SizeType>::max() - x)
            throw std::overflow_error("RFI memory estimate overflows");
        return x + y;
    };
    const auto window = std::max<SizeType>(
        detail::kMinWindow,
        static_cast<SizeType>(
            std::min(static_cast<double>(layout.max_nsamps),
                     static_cast<double>(baseline_length) / layout.tsamp)));
    const auto windows = std::max<SizeType>(1, layout.max_nsamps / window);
    const bool moments =
        cfg.moments.enabled || cfg.iqrm.enabled || cfg.kadane_channel.enabled;
    const auto stats =
        mul(mul(windows, layout.nchans),
            5 * sizeof(float) + (moments ? sizeof(detail::MomentSums) : 0));
    // Normalized and time-major float data, row flags/series and filter
    // scratch; channel allowance covers bandpass/remedian, bounded chunks,
    // moments/zero-DM/Kadane arrays and IQRM's bounded lag-vote lists.
    SizeType bytes =
        mul(mul(layout.max_nsamps, layout.nchans), 2 * sizeof(float));
    bytes = add(bytes, stats);
    bytes = add(bytes, mul(layout.nchans, 4 * sizeof(float)));
    bytes = add(bytes, mul(layout.max_nsamps, 128));
    bytes = add(bytes, mul(layout.nchans, 40 * 1024));
    bytes = add(bytes, mul(std::max<SizeType>(threads, 1),
                           add(4 * detail::kMaxBandpassRows * sizeof(double),
                               mul(layout.nchans, 4 * sizeof(float)))));
    return bytes;
}

SizeType RFICleaner::memory_usage() const noexcept {
    return m_impl->memory_usage();
}

#define FROST_RFI_INSTANTIATE(C) template void RFICleaner::Impl::apply<C>();
FROST_RFI_FOR_EACH_CODEC(FROST_RFI_INSTANTIATE)
#undef FROST_RFI_INSTANTIATE

} // namespace frost::prepare::rfi
