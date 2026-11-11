#include <algorithm>
#include <chrono>
#include <limits>
#include <memory>
#include <numeric>
#include <stdexcept>
#include <tuple>
#include <vector>

#include <dmt/common/candidate.hpp>
#include <spdlog/spdlog.h>

#include <frost/common/numeric.hpp>
#include <frost/common/policies.hpp>
#include <frost/config/config.hpp>
#include <frost/search/candidate.hpp>
#include <frost/search/clustering.hpp>
#include <frost/search/merge.hpp>
#include <frost/search/postprocess.hpp>
#include <frost/search/rate_limiter.hpp>
#include <frost/search/subband_dedup.hpp>

namespace frost::search {
namespace {
using Clock = std::chrono::steady_clock;
double seconds_since(Clock::time_point start) {
    return std::chrono::duration<double>(Clock::now() - start).count();
}
SizeType pending_limit(const config::SearchConfig& cfg) {
    return (cfg.max_pending_candidates != 0u) ? cfg.max_pending_candidates
                                              : cfg.max_candidates;
}
SizeType combined_limit(const config::SearchConfig& cfg) {
    const auto pending = pending_limit(cfg);
    if (cfg.max_candidates == 0 ||
        pending > std::numeric_limits<SizeType>::max() - cfg.max_candidates) {
        throw std::invalid_argument(
            "Invalid candidate post-processing capacity");
    }
    const auto total = cfg.max_candidates + pending;
    if (total >
        std::numeric_limits<SizeType>::max() / ((8 * sizeof(Giant)) + 512)) {
        throw std::invalid_argument("Candidate workspace byte count overflows");
    }
    return total;
}
} // namespace
class CandidatePostprocessPlan::Impl {
public:
    Impl(const config::SearchConfig& cfg, CandidatePostprocessLayout layout)
        : m_cfg(cfg),
          m_layout(layout),
          m_dedup(cfg.subband_dedup) {
        if (layout.nchans < 2 || !is_positive_finite(layout.tsamp) ||
            !is_finite(layout.frequency_step) || layout.frequency_step == 0 ||
            cfg.boxcar_max == 0) {
            throw std::invalid_argument(
                "Invalid candidate post-processing layout");
        }
        const auto total = combined_limit(cfg);
        m_incoming.reserve(cfg.max_candidates);
        m_work.reserve(total);
        m_ready.reserve(total);
        m_pending.reserve(pending_limit(cfg));
        m_next_pending.reserve(pending_limit(cfg));
    }
    CandidatePostprocessResult execute(dmt::CandidateResultView result,
                                       SizeType samples_read,
                                       SizeType duration_samples,
                                       bool final,
                                       SizeType candidate_frontier) {
        if (result.candidates.size() > m_cfg.max_candidates) {
            throw std::runtime_error(
                "Post-processing input exceeds max_candidates");
        }
        m_incoming.clear();
        m_ready.clear();
        m_next_pending.clear();
        for (const auto& raw : result.candidates) {
            if (raw.peak_sample >= samples_read) {
                continue;
            }
            if (raw.chan_begin >= raw.chan_end ||
                raw.chan_end > m_layout.nchans) {
                throw std::invalid_argument(
                    "Invalid raw candidate channel extent");
            }
            if (raw.beam_idx != 0 || raw.width == 0 ||
                raw.width > m_cfg.boxcar_max ||
                raw.begin_sample > raw.peak_sample ||
                raw.end_sample < raw.peak_sample) {
                throw std::invalid_argument(
                    "Invalid raw candidate beam, width or sample interval");
            }
            Giant g;
            g.snr        = raw.snr;
            g.samp_idx   = raw.peak_sample;
            g.begin      = raw.begin_sample;
            g.end        = std::min<SizeType>(raw.end_sample, samples_read - 1);
            g.filter_idx = raw.filter_idx;
            g.filter_width = raw.width;
            g.dm_idx       = raw.dm_idx;
            g.dm           = raw.dm;
            g.f_low        = raw.f_low_mhz;
            g.f_high       = raw.f_high_mhz;
            g.chan_begin   = m_layout.frequency_step < 0
                                 ? m_layout.nchans - raw.chan_end
                                 : raw.chan_begin;
            g.chan_end     = m_layout.frequency_step < 0
                                 ? m_layout.nchans - 1 - raw.chan_begin
                                 : raw.chan_end - 1;
            g.plane =
                (static_cast<SizeType>(raw.subband_level) * m_layout.nchans) +
                raw.subband_idx;
            m_incoming.push_back(g);
        }
        std::sort(
            m_incoming.begin(), m_incoming.end(),
            [](const Giant& a, const Giant& b) {
                return std::tie(a.plane, a.dm_idx, a.filter_idx, a.samp_idx) <
                       std::tie(b.plane, b.dm_idx, b.filter_idx, b.samp_idx);
            });
        // Every later peak is at or after the engine's candidate frontier,
        // and links only within cand_sep_time * width of a member.
        const auto max_size  = std::numeric_limits<SizeType>::max();
        const auto width     = m_cfg.boxcar_max;
        const SizeType reach = m_cfg.cand_sep_time >= (max_size / width) - 1
                                   ? max_size
                                   : (m_cfg.cand_sep_time + 1) * width;
        const SizeType watermark =
            candidate_frontier > reach ? candidate_frontier - reach : 0;
        double cluster_s = 0.0, dedup_s = 0.0;
        SizeType i = 0, j = 0;
        // Both streams are in plane order. Keep incoming rows before carried
        // rows, preserving merge and tie-breaking order in flat storage.
        while (i < m_incoming.size() || j < m_pending.size()) {
            const auto plane =
                i == m_incoming.size() ? m_pending[j].plane
                : j == m_pending.size()
                    ? m_incoming[i].plane
                    : std::min(m_incoming[i].plane, m_pending[j].plane);
            m_work.clear();
            RateLimiter limiter(m_cfg.max_giant_rate);
            limiter.record(0, static_cast<double>(duration_samples) *
                                  m_layout.tsamp);
            while (i < m_incoming.size() && m_incoming[i].plane == plane) {
                if (!limiter.exceeded()) {
                    m_work.push_back(m_incoming[i]);
                    limiter.record(1, 0.0);
                }
                ++i;
            }
            if (limiter.exceeded()) {
                spdlog::warn("Exceeded max giant rate ({} /min)",
                             m_cfg.max_giant_rate);
            }
            while (j < m_pending.size() && m_pending[j].plane == plane) {
                m_work.push_back(m_pending[j++]);
            }
            if (m_cfg.cluster && !m_work.empty()) {
                const auto start = Clock::now();
                reduce_plane(watermark, final);
                cluster_s += seconds_since(start);
            } else {
                m_ready.insert(m_ready.end(), m_work.begin(), m_work.end());
            }
        }
        m_pending.swap(m_next_pending);
        if (m_cfg.cluster) {
            const auto start   = Clock::now();
            const auto deduped = m_dedup.execute(
                m_ready, tolerances(), static_cast<float>(m_layout.tsamp));
            m_ready.assign(deduped.begin(), deduped.end());
            dedup_s = seconds_since(start);
        }
        return {.candidates         = m_ready,
                .cluster_s          = cluster_s,
                .dedup_s            = dedup_s,
                .pending_candidates = m_pending.size()};
    }
    SizeType memory_usage() const noexcept {
        return (m_incoming.capacity() + m_work.capacity() + m_ready.capacity() +
                m_pending.capacity() + m_next_pending.capacity()) *
               sizeof(Giant);
    }

private:
    void reduce_plane(SizeType watermark, bool final) {
        auto tol          = tolerances();
        tol.snr_floor     = 0;
        const auto labels = m_cluster.execute(m_work, tol);
        auto merged       = m_merge.execute(m_work, labels);
        // Chain roots are minimum input indices and MergeCandidatesPlan emits
        // them in ascending order. Map each raw member to its merged row.
        std::vector<SizeType> merged_index(m_work.size());
        SizeType rank = 0;
        for (SizeType i = 0; i < labels.size(); ++i) {
            if (labels[i] == i)
                merged_index[i] = rank++;
        }
        std::vector<SizeType> groups(merged.size());
        std::iota(groups.begin(), groups.end(), SizeType{0});
        if (tol.dm_smear > 0 && merged.size() > 1) {
            groups = m_wings.execute(merged, tolerances());
        }
        const auto reduced = tol.dm_smear > 0 && merged.size() > 1
                                 ? m_merge.execute(merged, groups)
                                 : std::move(merged);
        if (final) {
            m_ready.insert(m_ready.end(), reduced.begin(), reduced.end());
            return;
        }

        // Retain every member of a group with a recent excursion, rather
        // than just its brightest representative. A weaker member can be
        // the only edge joining a future peak to this component.
        std::vector<uint8_t> recent(groups.size(), 0);
        for (SizeType i = 0; i < m_work.size(); ++i) {
            if (m_work[i].end >= watermark) {
                recent[groups[merged_index[labels[i]]]] = 1;
            }
        }
        for (SizeType i = 0; i < m_work.size(); ++i) {
            if (recent[groups[merged_index[labels[i]]]] == 0)
                continue;
            if (m_next_pending.size() == pending_limit(m_cfg)) {
                throw std::runtime_error(
                    "Pending candidate capacity exceeded; increase "
                    "max_pending_candidates or reduce candidate production");
            }
            m_next_pending.push_back(m_work[i]);
        }
        rank = 0;
        for (SizeType group = 0; group < groups.size(); ++group) {
            if (groups[group] != group)
                continue;
            if (recent[group] == 0)
                m_ready.push_back(reduced[rank]);
            ++rank;
        }
    }

    ClusterTolerances tolerances() const {
        return {
            .time_tol   = m_cfg.cand_sep_time,
            .filter_tol = m_cfg.cand_sep_filter,
            .dm_tol     = m_cfg.cand_sep_dm,
            .dm_smear   = m_cfg.cand_sep_dm_smear,
            .tsamp      = m_layout.tsamp,
            .snr_floor  = m_cfg.detect_thresh,
        };
    }
    config::SearchConfig m_cfg;
    CandidatePostprocessLayout m_layout;
    ClusterLabelPlan m_cluster;
    ClusterLabelPlan m_wings{ClusterMode::kSeeded};
    MergeCandidatesPlan m_merge;
    SubbandDedupPlan m_dedup;
    std::vector<Giant> m_incoming, m_work, m_ready, m_pending, m_next_pending;
};
CandidatePostprocessPlan::CandidatePostprocessPlan(
    const config::SearchConfig& cfg, CandidatePostprocessLayout layout)
    : m_impl(std::make_unique<Impl>(cfg, layout)) {}
CandidatePostprocessPlan::~CandidatePostprocessPlan() = default;
CandidatePostprocessPlan::CandidatePostprocessPlan(
    CandidatePostprocessPlan&&) noexcept = default;
CandidatePostprocessPlan& CandidatePostprocessPlan::operator=(
    CandidatePostprocessPlan&&) noexcept = default;
CandidatePostprocessResult
CandidatePostprocessPlan::execute(dmt::CandidateResultView result,
                                  SizeType samples_read,
                                  SizeType duration_samples,
                                  bool final,
                                  SizeType candidate_frontier) {
    return m_impl->execute(result, samples_read, duration_samples, final,
                           candidate_frontier);
}
SizeType CandidatePostprocessPlan::memory_usage() const noexcept {
    return m_impl->memory_usage();
}
SizeType
CandidatePostprocessPlan::estimate_memory(const config::SearchConfig& cfg) {
    // Persistent bounded vectors plus conservative cluster/merge/dedup and
    // per-gulp writer scratch. No engine or candidate buffers are allocated.
    return combined_limit(cfg) * ((8 * sizeof(Giant)) + 512);
}
} // namespace frost::search
