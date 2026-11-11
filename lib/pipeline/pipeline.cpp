#include <algorithm>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <limits>
#include <map>
#include <mutex>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <thread>
#include <tuple>
#include <utility>
#include <vector>

#include <dmt/algorithms/fdmt_boxcar.hpp>
#include <dmt/common/backend.hpp>
#include <fmt/format.h>
#include <omp.h>
#include <spdlog/spdlog.h>

#include <frost/output/cand_writer.hpp>
#include <frost/pipeline/memory.hpp>
#include <frost/pipeline/pipeline.hpp>
#include <frost/prepare/rfi.hpp>
#include <frost/search/candidate.hpp>
#include <frost/search/postprocess.hpp>

#include "lib/output/file_ledger.hpp"
#include "lib/prepare/device_rfi.hpp"

namespace frost::pipeline {

namespace {

using Clock = std::chrono::steady_clock;

double seconds_since(Clock::time_point start) {
    return std::chrono::duration<double>(Clock::now() - start).count();
}

SizeType giant_width(const search::Giant& giant) {
    return search::giant_boxcar_width(giant);
}

void set_log_level(int verbosity) {
    if (verbosity <= 0) {
        spdlog::set_level(spdlog::level::warn);
    } else if (verbosity == 1) {
        spdlog::set_level(spdlog::level::info);
    } else {
        spdlog::set_level(spdlog::level::debug);
    }
}

SizeType read_block(psrio::BlockSource& source,
                    std::span<uint8_t> dest,
                    SizeType nsamps) {
    const auto nread = source.read_block(
        nsamps, std::span<std::byte>(reinterpret_cast<std::byte*>(dest.data()),
                                     dest.size()));
    return static_cast<SizeType>(nread);
}

SizeType resolve_beam(const config::OutputConfig& out,
                      const psrio::BlockSource& ds) {
    if (out.beam > 0) {
        return out.beam;
    }
    return ds.beam() > 0 ? static_cast<SizeType>(ds.beam()) : 1;
}

// open_input checks this for files and rings. Memory sources skip that path.
void require_searchable(const psrio::BlockSource& source) {
    if (source.nifs() != 1) {
        throw std::invalid_argument(fmt::format(
            "frost: search reads one IF; this input has {}", source.nifs()));
    }
    if (source.nchans() == 0 || source.bytes_per_sample() == 0) {
        throw std::invalid_argument("frost: input has no frequency channels");
    }
    (void)to_data_type(source.sample_type());
}

void log_rfi_report(const prepare::rfi::RfiReport& r) {
    spdlog::debug("RFI: channels masked {} (zapped {}, dead {}, moments {}, "
                  "iqrm {}, grown {}); samples flagged {} (broad {}, dropout "
                  "{}, kadane {}); cells narrow {}, kadane_spectrum {}, "
                  "kadane_channel {}; fill fallbacks {}",
                  r.chans_masked, r.chans_zapped, r.chans_dead, r.chans_moments,
                  r.chans_iqrm, r.chans_grown, r.samps_flagged, r.samps_broad,
                  r.samps_dropout, r.samps_kadane, r.cells_narrow,
                  r.cells_kadane_spectrum, r.cells_kadane_channel,
                  r.fill_fallbacks);
    spdlog::debug("RFI timing (s): bandpass {:.3f}, moments {:.3f}, scan "
                  "{:.3f}, flags {:.3f}, apply {:.3f}",
                  r.t_bandpass, r.t_moments, r.t_scan, r.t_flags, r.t_apply);
}

} // namespace

// One beam, one raw buffer (two with prefetch). The reader thread fills the
// idle buffer while the caller searches the other. FDMT history stays on the
// caller.
class GulpPrefetch {
public:
    GulpPrefetch(psrio::BlockSource& source,
                 SizeType block,
                 SizeType stride,
                 bool async)
        : m_source(&source),
          m_block(block),
          m_async(async),
          m_bufs{std::vector<uint8_t>(block * stride),
                 std::vector<uint8_t>(async ? block * stride : 0)} {}

    GulpPrefetch(const GulpPrefetch&)            = delete;
    GulpPrefetch& operator=(const GulpPrefetch&) = delete;

    ~GulpPrefetch() { shutdown(); }

    struct Gulp {
        uint8_t* data  = nullptr;
        SizeType nread = 0;
    };

    Gulp next() {
        if (!m_async) {
            return read_sync();
        }
        ensure_thread();
        if (!m_have_current) {
            request(0);
            m_current      = 0;
            m_have_current = true;
        }
        wait_ready(m_current);
        rethrow();
        const int done = m_current;
        Gulp gulp{m_bufs[static_cast<std::size_t>(done)].data(), m_nread[done]};
        if (gulp.nread == m_block) {
            const int other = 1 - done;
            request(other);
            m_current = other;
        }
        return gulp;
    }

    double read_seconds() const {
        std::lock_guard<std::mutex> lock(m_mu);
        return m_read_s;
    }

private:
    void ensure_thread() {
        if (!m_thread.joinable()) {
            m_thread = std::thread([this] { run(); });
        }
    }

    void shutdown() {
        {
            std::lock_guard<std::mutex> lock(m_mu);
            m_stop = true;
            m_cv.notify_all();
        }
        if (m_thread.joinable()) {
            m_thread.join();
        }
    }

    void request(int slot) {
        std::lock_guard<std::mutex> lock(m_mu);
        m_ready[slot] = false;
        m_requested   = slot;
        m_cv.notify_all();
    }

    void wait_ready(int slot) {
        std::unique_lock<std::mutex> lock(m_mu);
        m_cv.wait(lock, [&] { return m_ready[slot] || m_eptr; });
    }

    void rethrow() {
        std::exception_ptr eptr;
        {
            std::lock_guard<std::mutex> lock(m_mu);
            eptr = m_eptr;
        }
        if (eptr) {
            std::rethrow_exception(eptr);
        }
    }

    Gulp read_sync() {
        const auto start     = Clock::now();
        const SizeType nread = read_block(*m_source, m_bufs[0], m_block);
        {
            std::lock_guard<std::mutex> lock(m_mu);
            m_read_s += seconds_since(start);
        }
        return {m_bufs[0].data(), nread};
    }

    void run() {
        while (true) {
            int slot = -1;
            {
                std::unique_lock<std::mutex> lock(m_mu);
                m_cv.wait(lock, [&] { return m_stop || m_requested >= 0; });
                if (m_stop) {
                    return;
                }
                slot        = m_requested;
                m_requested = -1;
            }
            SizeType nread = 0;
            double seconds = 0.0;
            std::exception_ptr eptr;
            try {
                const auto start = Clock::now();
                nread            = read_block(
                    *m_source, m_bufs[static_cast<std::size_t>(slot)], m_block);
                seconds = seconds_since(start);
            } catch (...) {
                eptr = std::current_exception();
            }
            {
                std::lock_guard<std::mutex> lock(m_mu);
                if (eptr) {
                    m_eptr = eptr;
                    m_cv.notify_all();
                    return;
                }
                m_nread[slot] = nread;
                m_read_s += seconds;
                m_ready[slot] = true;
                m_cv.notify_all();
            }
        }
    }

    psrio::BlockSource* m_source = nullptr;
    SizeType m_block             = 0;
    bool m_async                 = false;
    std::vector<uint8_t> m_bufs[2];
    SizeType m_nread[2] = {0, 0};
    bool m_ready[2]     = {false, false};
    int m_current       = 0;
    bool m_have_current = false;
    int m_requested     = -1;
    bool m_stop         = false;
    double m_read_s     = 0.0;
    std::exception_ptr m_eptr;
    mutable std::mutex m_mu;
    std::condition_variable m_cv;
    std::thread m_thread;
};

FrequencyAxis data_frequency_axis(const psrio::BlockSource& ds) {
    FrequencyAxis axis{.f0 = ds.fch1(), .df = ds.foff()};
    const auto nchans = static_cast<SizeType>(ds.nchans());
    if (ds.fswap_enabled() && nchans > 0) {
        axis.f0 += static_cast<double>(nchans - 1) * axis.df;
        axis.df = -axis.df;
    }
    return axis;
}

prepare::rfi::BlockLayout rfi_block_layout(const psrio::BlockSource& ds,
                                           SizeType capacity) {
    prepare::rfi::BlockLayout layout;
    layout.nchans     = static_cast<SizeType>(ds.nchans());
    layout.max_nsamps = capacity;
    layout.dtype      = to_data_type(ds.sample_type());
    layout.tsamp      = ds.tsamp();
    const auto axis   = data_frequency_axis(ds);
    layout.f0         = axis.f0;
    layout.df         = axis.df;
    return layout;
}

dedisp::DedispPlanConfig
make_dedisp_plan_config(const config::PipelineConfig& cfg,
                        const psrio::BlockSource& data_source) {
    dedisp::DedispPlanConfig dd;
    dd.nchans         = static_cast<SizeType>(data_source.nchans());
    dd.dt             = static_cast<float>(data_source.tsamp());
    const auto axis   = data_frequency_axis(data_source);
    dd.f0             = static_cast<float>(axis.f0);
    dd.df             = static_cast<float>(axis.df);
    dd.dm_min         = cfg.dedisp.dm_min;
    dd.dm_max         = cfg.dedisp.dm_max;
    dd.dm_tol         = cfg.dedisp.dm_tol;
    dd.dm_pulse_width = cfg.dedisp.dm_pulse_width;
    dd.dm_dtype       = cfg.dedisp.dm_dtype;
    dd.block_nsamps   = cfg.nsamps_gulp;
    dd.subband_depth  = cfg.search.subband_levels;
    return dd;
}

// Validate before constructing the engine and allocating gulp storage.
config::PipelineConfig validated(config::PipelineConfig cfg,
                                 const psrio::BlockSource& source) {
    cfg.validate();
    if (cfg.memory_limit_mib)
        (void)check_memory(cfg, source, false);
    return cfg;
}

namespace {
dmt::BoxcarConfig boxcar_config(const config::SearchConfig& cfg) {
    dmt::BoxcarConfig out;
    out.max_boxcar       = cfg.boxcar_max;
    out.wtsp             = cfg.wtsp;
    out.use_trapezoid    = cfg.use_trapezoid;
    out.detect_thresh    = cfg.detect_thresh;
    out.noise_sigma      = 1.0F;
    out.peak_mode        = cfg.peak_mode;
    out.max_candidates   = cfg.max_candidates;
    out.max_cands_per_dm = cfg.max_cands_per_dm;
    out.skip_cold_prefix = true;
    out.input_nbits      = 32;
    out.input_mean       = 0.0F;
    return out;
}

// FDMT order (low frequency first) for an RFI mask in input channel order.
// Empty when it matches `same`, so dmt keeps its constructor-mask path.
std::span<const uint8_t> fdmt_kill_mask(std::span<const uint8_t> mask,
                                        std::span<const uint8_t> same,
                                        bool descending,
                                        std::vector<uint8_t>& out) {
    if (std::ranges::equal(mask, same))
        return {};
    out.assign(mask.begin(), mask.end());
    if (descending)
        std::ranges::reverse(out);
    return out;
}

dmt::algorithms::FDMTBoxcar make_search(const config::PipelineConfig& cfg,
                                        const psrio::BlockSource& source,
                                        dmt::Exec exec,
                                        std::span<const uint8_t> kill_mask) {
    auto plan =
        dedisp::make_dmt_fdmt_plan(make_dedisp_plan_config(cfg, source));
    const auto delay = static_cast<SizeType>(plan.get_dt_max());
    if (cfg.nsamps_gulp <= delay + cfg.search.boxcar_max)
        throw std::invalid_argument(
            "nsamps_gulp must exceed the dedispersion max_delay (" +
            std::to_string(delay) + ") plus boxcar_max (" +
            std::to_string(cfg.search.boxcar_max) + ")");
    constexpr SizeType kMiB = SizeType{1} << 20U;
    // memory_limit_mib bounds the selected backend pool; give the engine
    // the whole limit (check_memory already budgeted the other owners).
    const SizeType budget = cfg.memory_limit_mib != 0
                                ? cfg.memory_limit_mib * kMiB
                                : dmt::algorithms::kFDMTDefaultMaxMemory;
    return dmt::algorithms::FDMTBoxcar(
        std::move(plan), boxcar_config(cfg.search), true, exec, 1,
        dmt::algorithms::kFDMTAutoFuse, 0, budget, kill_mask);
}

dmt::Exec execution(const config::DedispConfig& cfg) {
    return {.backend  = cfg.device,
            .nthreads = std::max(omp_get_max_threads(), 1),
            .device   = cfg.device_id};
}
} // namespace

class Pipeline::Impl {
public:
    Impl(config::PipelineConfig cfg, psrio::BlockSource& source)
        : m_cfg(validated(std::move(cfg), source)),
          m_source(source),
          m_nchans(source.nchans()),
          m_stride(source.bytes_per_sample()),
          m_block(m_cfg.nsamps_gulp),
          m_dt(source.tsamp()),
          m_df(data_frequency_axis(source).df),
          m_dtype(to_data_type(source.sample_type())),
          m_exec(execution(m_cfg.dedisp)),
          m_rfi(m_cfg.rfi,
                rfi_block_layout(source, m_block),
                m_cfg.baseline_length),
          m_static_mask(m_rfi.channel_mask().begin(),
                        m_rfi.channel_mask().end()),
          m_engine(
              make_search(m_cfg,
                          source,
                          m_exec,
                          fdmt_kill_mask(m_static_mask,
                                         std::vector<uint8_t>(m_nchans, 1U),
                                         m_df < 0.0,
                                         m_fdmt_mask))),
          m_postprocess(m_cfg.search, {m_nchans, m_dt, m_df}),
          m_prefetch(source, m_block, m_stride, m_cfg.async_io) {
        require_searchable(source);
        set_log_level(m_cfg.verbosity);
        m_max_delay = static_cast<SizeType>(m_engine.get_plan().get_dt_max());
#ifdef FROST_ENABLE_GPU
        if (m_exec.backend != dmt::Backend::kCPU) {
            m_device_buffer.emplace(m_exec.backend, m_exec.device,
                                    m_nchans * m_block);
            if (device::DeviceRfi::default_integer_neighbour(m_cfg.rfi,
                                                             m_dtype)) {
                m_device_rfi.emplace(m_exec.backend, m_exec.device, m_cfg.rfi,
                                     rfi_block_layout(source, m_block),
                                     m_cfg.baseline_length);
            }
        }
#endif
        spdlog::info("device {}; FDMTBoxcar search; rfi={}",
                     dmt::to_string(m_exec.backend),
#ifdef FROST_ENABLE_GPU
                     m_device_rfi ? "device" : "host"
#else
                     "host"
#endif
        );
        if (!m_cfg.output.per_gulp_files) {
            m_writer.emplace(std::filesystem::path(m_cfg.output.output_dir) /
                                 "candidates.cand",
                             m_dt);
        }
        m_profile.input_reserved_bytes =
            m_block * m_stride * (m_cfg.async_io ? 2 : 1);
        m_profile.engine_reported_bytes = m_engine.get_memory_usage().total();
        m_profile.postprocess_reserved_bytes = m_postprocess.memory_usage();
        m_profile.threads                    = m_exec.nthreads;
        m_profile.dm_trials = m_engine.get_plan().get_dm_grid_final().size();
        m_profile.tsamp     = static_cast<float>(m_dt);
    }

    SizeType execute() {
        const auto start = Clock::now();
        const auto nread = execute_block();
        m_profile.wall_s += seconds_since(start);
        return nread;
    }
    SizeType nsamps_processed() const { return m_profile.samples_searched; }
    const RunProfile& profile() const { return m_profile; }

private:
    void write_candidates(std::span<const search::Giant> candidates) {
        if (m_writer) {
            m_writer->write(candidates);
            return;
        }
        // Carried peaks belong to the gulp containing the peak, including
        // when the engine closes them during a later call or EOF flush.
        std::map<SizeType, std::vector<search::Giant>> gulps;
        for (const auto& giant : candidates)
            gulps[(giant.samp_idx / m_block) * m_block].push_back(giant);
        for (const auto& [origin, rows] : gulps) {
            const auto offset = static_cast<std::time_t>(
                std::floor(static_cast<double>(origin) * m_dt));
            const auto path =
                std::filesystem::path(m_cfg.output.output_dir) /
                output::cand_filename(m_source.utc_start() + offset,
                                      resolve_beam(m_cfg.output, m_source));
            if (!m_file_ledger)
                m_file_ledger.emplace();
            const bool seen = m_file_ledger->seen_before(path);
            output::CandWriter writer(
                path, m_dt,
                seen ? output::CandWriter::OpenMode::kAppend
                     : output::CandWriter::OpenMode::kTruncate);
            writer.write(rows);
        }
    }

    void publish(const dmt::CandidateResultView& result,
                 SizeType duration_samples,
                 bool final = false) {
        m_profile.dropped_global += result.dropped_global;
        m_profile.dropped_per_dm += result.dropped_per_dm;
        if (result.overflowed() && !m_overflow_warned) {
            spdlog::warn("FDMTBoxcar candidate capacity exceeded: dropped {} "
                         "per DM and {} globally",
                         result.dropped_per_dm, result.dropped_global);
            m_overflow_warned = true;
        }
        // Host results are synchronous; device results were copied back
        // (stream synchronized) before publish, so the frontier is current.
        const auto processed = m_postprocess.execute(
            result, m_profile.samples_read, duration_samples, final,
            static_cast<SizeType>(m_engine.get_candidate_frontier()));
        m_profile.cluster_s += processed.cluster_s;
        m_profile.dedup_s += processed.dedup_s;
        m_profile.peak_pending_candidates = std::max(
            m_profile.peak_pending_candidates, processed.pending_candidates);
        const auto all = processed.candidates;
        const auto t   = Clock::now();
        write_candidates(all);
        m_profile.write_s += seconds_since(t);
        m_profile.candidates += all.size();
        for (const auto& giant : all) {
            if (giant_width(giant) >= kWideBoxcarSamples)
                ++m_profile.wide_boxcars;
        }
    }

    void flush() {
        if (m_flushed)
            return;
        const auto t = Clock::now();
        // drain() also searches the samples still behind the skewed-window
        // search latency; flush() would leave them unsearched. It is a host
        // call: every device execute was followed by a synchronizing
        // copy_result().
        const auto result = m_engine.drain();
        m_profile.dedisp_s += seconds_since(t);
        publish(result, m_last_nread, true);
        m_flushed = true;
        if (m_profile.samples_read > 0 && m_profile.samples_searched == 0 &&
            m_profile.candidates == 0) {
            spdlog::warn("Cold prefix of {} samples left nothing to search: "
                         "read {} sample(s), searched 0",
                         m_max_delay, m_profile.samples_read);
        }
    }

    SizeType execute_block() {
        if (m_eof) {
            flush();
            return 0;
        }
        const auto gulp      = m_prefetch.next();
        m_profile.read_s     = m_prefetch.read_seconds();
        const SizeType nread = gulp.nread;
        m_eof                = nread < m_block;
        if (nread == 0) {
            flush();
            return 0;
        }
        m_profile.samples_read += nread;
        ++m_profile.gulps;
        m_last_nread = nread;
        auto t       = Clock::now();
        prepare::rfi::RfiReport report;
        std::span<const float> waterfall;
        // Effective channel mask of this gulp, in input channel order.
        std::span<const uint8_t> mask;
#ifdef FROST_ENABLE_GPU
        dmt::DeviceSpan<const float> device_waterfall;
        if (m_device_rfi) {
            const auto cleaned = m_device_rfi->execute(
                {gulp.data, nread * m_stride}, m_stride, nread, m_block,
                m_device_buffer->stream());
            report           = cleaned.report;
            m_gulp_mask      = cleaned.channel_mask;
            mask             = m_gulp_mask;
            device_waterfall = m_device_rfi->normalize_to_fdmt(
                m_nchans, m_block, nread, m_df < 0.0,
                m_device_buffer->stream());
        } else
#endif
        {
            // Only real samples contribute to robust window statistics.
            // The normalized buffer has the full stride and a zero tail.
            waterfall = m_rfi.execute_normalized(
                {gulp.data, m_block * m_stride}, nread);
            report = m_rfi.report();
            mask   = m_rfi.channel_mask();
#ifdef FROST_ENABLE_GPU
            if (m_device_buffer)
                device_waterfall = m_device_buffer->upload(waterfall);
#endif
        }
        m_profile.rfi_reserved_bytes =
            std::max(m_profile.rfi_reserved_bytes, m_rfi.memory_usage());
#ifdef FROST_ENABLE_GPU
        if (m_device_rfi)
            m_profile.device_rfi_reserved_bytes =
                std::max(m_profile.device_rfi_reserved_bytes,
                         m_device_rfi->memory_usage());
        if (m_device_buffer)
            m_profile.device_upload_reserved_bytes =
                std::max(m_profile.device_upload_reserved_bytes,
                         m_device_buffer->memory_usage());
#endif
        log_rfi_report(report);
        m_profile.rfi_s += seconds_since(t);
        m_profile.rfi_bandpass_s += report.t_bandpass;
        m_profile.rfi_moments_s += report.t_moments;
        m_profile.rfi_scan_s += report.t_scan;
        m_profile.rfi_flags_s += report.t_flags;
        m_profile.rfi_apply_s += report.t_apply;
        t = Clock::now();
        // dmt leaves killed channels out of every row's noise model, so the
        // calibrated input needs no global sqrt(nchans / good) rescaling.
        const auto kill_mask =
            fdmt_kill_mask(mask, m_static_mask, m_df < 0.0, m_fdmt_mask);
        dmt::CandidateResultView result;
#ifdef FROST_ENABLE_GPU
        if (m_device_buffer) {
            (void)m_engine.execute(device_waterfall, kill_mask,
                                   m_device_buffer->stream());
            result = m_engine.copy_result(m_device_buffer->stream());
        } else
#endif
            result = m_engine.execute(waterfall, kill_mask);
        m_profile.dedisp_s += seconds_since(t);
        const SizeType previous = m_profile.samples_read - nread;
        m_profile.samples_searched +=
            m_profile.samples_read > m_max_delay
                ? m_profile.samples_read - std::max(previous, m_max_delay)
                : 0;
        publish(result, nread);
        if (m_eof)
            flush();
        return nread;
    }

    config::PipelineConfig m_cfg;
    psrio::BlockSource& m_source;
    SizeType m_nchans, m_stride, m_block;
    double m_dt, m_df;
    DataType m_dtype;
    dmt::Exec m_exec;
    prepare::rfi::RFICleaner m_rfi;
    // Static zaps in input order: the engine's constructor mask. Per-gulp
    // masks equal to it are passed as empty spans.
    std::vector<uint8_t> m_static_mask, m_fdmt_mask, m_gulp_mask;
    dmt::algorithms::FDMTBoxcar m_engine;
    search::CandidatePostprocessPlan m_postprocess;
#ifdef FROST_ENABLE_GPU
    std::optional<device::WaterfallBuffer> m_device_buffer;
    std::optional<device::DeviceRfi> m_device_rfi;
#endif
    std::optional<output::CandWriter> m_writer;
    std::optional<output::FileLedger> m_file_ledger;
    RunProfile m_profile;
    SizeType m_max_delay = 0, m_last_nread = 0;
    bool m_eof = false, m_flushed = false, m_overflow_warned = false;
    GulpPrefetch m_prefetch;
};

Pipeline::Pipeline(const config::PipelineConfig& cfg,
                   psrio::BlockSource& source)
    : m_impl(std::make_unique<Impl>(cfg, source)) {}

Pipeline::~Pipeline()                              = default;
Pipeline::Pipeline(Pipeline&&) noexcept            = default;
Pipeline& Pipeline::operator=(Pipeline&&) noexcept = default;

SizeType Pipeline::execute() { return m_impl->execute(); }
SizeType Pipeline::nsamps_processed() const {
    return m_impl->nsamps_processed();
}
const RunProfile& Pipeline::profile() const { return m_impl->profile(); }

} // namespace frost::pipeline
