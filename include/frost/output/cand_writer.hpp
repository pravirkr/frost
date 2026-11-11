#pragma once

#include <cstdint>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <span>
#include <string>

#include <frost/common/policies.hpp>
#include <frost/search/candidate.hpp>

namespace frost::output {

/**
 * Writes single-pulse candidates to a tab-separated `.cand` file whose
 * first nine columns follow the widely used single-pulse candidate layout
 * for downstream tools, followed by fourteen frost columns:
 *
 *   snr  samp_idx  time  filter_idx  dm_idx  dm  members  begin  end
 *   f_low  f_high  chan_begin  chan_end
 *   snr_best  chan_begin_best  chan_end_best  n_extents
 *   samp_idx_best  time_best  dm_best  width_best  f_low_best  f_high_best
 *
 * Columns 10-13 are the frequency
 * and original-channel extent of the band the candidate was found in, so
 * a sub-band hit is distinguishable from a full-band hit. Columns 14-17
 * report the strongest sub-band row folded into this one by the sub-band
 * dedup (its S/N and channel extent) and how many distinct extents were
 * merged; without dedup they repeat the row's own S/N and extent, and 1.
 * Columns 18-23 preserve the strongest member's own coordinates, without
 * shifting its timestamp to the representative's frequency plane. Width
 * is in samples. Without dedup these repeat the representative's location.
 */
class CandWriter {
public:
    enum class OpenMode : uint8_t { kTruncate, kAppend };

    /** `dt` is the sampling interval in seconds, used to derive the
     * `time` column from `samp_idx`. Creates missing parent directories and
     * throws `std::runtime_error` if the file cannot be opened; `write`
     * throws if the stream fails (e.g. disk full). */
    CandWriter(const std::filesystem::path& path,
               double dt,
               OpenMode mode = OpenMode::kTruncate);

    /**
     * Appends one row per candidate; does not truncate between calls,
     * so repeated calls accumulate candidates across gulps.
     * `first_samp_idx` offsets every sample-index column (samp_idx/
     * begin/end/samp_idx_best) -- e.g. to a running global sample counter
     * across gulps.
     */
    void write(std::span<const search::Giant> candidates,
               SizeType first_samp_idx = 0);

private:
    std::ofstream m_file;
    double m_dt;
};

/** Per-gulp candidate file name, `YYYY-MM-DD-HH:MM:SS_BB.cand`
 * (`utc` rendered in UTC, `beam` 1-based and zero-padded to 2 digits). */
std::string cand_filename(std::time_t utc, SizeType beam);

} // namespace frost::output
