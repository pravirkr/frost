#pragma once

#include <memory>

#include <psrio/block_source.hpp>

#include <frost/config/config.hpp>
#include <frost/dedisp/dedisp.hpp>
#include <frost/pipeline/profile.hpp>
#include <frost/prepare/rfi.hpp>

namespace frost::pipeline {

/** Drives RFI calibration, streaming FDMTBoxcar detection and candidate
 * clustering/output. Contiguous peaks can span gulps; execute() flushes
 * pending peaks at EOF. Async input overlaps the next read with search. */
class Pipeline {
public:
    Pipeline(const config::PipelineConfig& cfg,
             psrio::BlockSource& data_source);
    Pipeline(const Pipeline&)            = delete;
    Pipeline& operator=(const Pipeline&) = delete;
    Pipeline(Pipeline&&) noexcept;
    Pipeline& operator=(Pipeline&&) noexcept;
    ~Pipeline();

    /** Reads and processes one gulp. Returns the number of new samples
     * read from the data source (0 once it is exhausted). */
    SizeType execute();

    /** Samples fully searched so far. */
    SizeType nsamps_processed() const;

    /** Stage times and counts for the run so far. */
    const RunProfile& profile() const;

private:
    class Impl;
    std::unique_ptr<Impl> m_impl;
};

/** Centre frequency of data channel 0 and the step to the next channel, in
 * MHz, in the order the reader hands channels over. With `fswap` the reader
 * reverses the channels, so this is the header's axis mirrored; every stage
 * that maps a channel index to a frequency (RFI `zap_freqs`, the FDMT
 * dispersion model, candidate band edges) must use it, not `fch1`/`foff`. */
struct FrequencyAxis {
    double f0 = 0.0;
    double df = 0.0;
};

FrequencyAxis data_frequency_axis(const psrio::BlockSource& data_source);

/** Builds the dedispersion plan for `cfg` against `data_source`'s
 * observing setup (nchans, sampling time, frequency axis as in
 * `data_frequency_axis`). */
dedisp::DedispPlanConfig
make_dedisp_plan_config(const config::PipelineConfig& cfg,
                        const psrio::BlockSource& data_source);

/** Shape and frequency axis of the blocks the RFI stage will see. With
 * `fswap` the reader hands over channels in reverse order, so channel 0 holds
 * the last header channel: the axis is mirrored so that `zap_freqs` in MHz
 * select the channels that actually carry those frequencies. */
prepare::rfi::BlockLayout
rfi_block_layout(const psrio::BlockSource& data_source, SizeType capacity);

} // namespace frost::pipeline
