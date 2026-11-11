#include <array>
#include <stdexcept>
#include <system_error>

#include <fmt/format.h>

#include <frost/output/cand_writer.hpp>

namespace frost::output {

namespace {
void ensure_parent_dir(const std::filesystem::path& path) {
    const auto parent = path.parent_path();
    if (parent.empty()) {
        return;
    }
    std::error_code ec;
    std::filesystem::create_directories(parent, ec);
    if (ec) {
        throw std::runtime_error("Cannot create output directory " +
                                 parent.string() + ": " + ec.message());
    }
}
} // namespace

CandWriter::CandWriter(const std::filesystem::path& path,
                       double dt,
                       OpenMode mode)
    : m_dt(dt) {
    ensure_parent_dir(path);
    m_file.open(path, mode == OpenMode::kAppend
                          ? std::ios::out | std::ios::app
                          : std::ios::out | std::ios::trunc);
    if (!m_file) {
        throw std::runtime_error("Failed to open candidate file: " +
                                 path.string());
    }
}

void CandWriter::write(std::span<const search::Giant> candidates,
                       SizeType first_samp_idx) {
    for (const auto& g : candidates) {
        const auto samp_idx = first_samp_idx + g.samp_idx;
        const auto begin    = first_samp_idx + g.begin;
        const auto end      = first_samp_idx + g.end;
        // double, printed with 10 digits: six significant digits (a float or
        // the stream default) resolve only 0.1 s at 10^4 s.
        const double time = static_cast<double>(samp_idx) * m_dt;
        const auto best_begin =
            g.has_best_extent() ? g.best_chan_begin : g.chan_begin;
        const auto best_end =
            g.has_best_extent() ? g.best_chan_end : g.chan_end;
        const auto best        = g.strongest_location();
        const auto best_sample = first_samp_idx + best.samp_idx;
        const double best_time = static_cast<double>(best_sample) * m_dt;

        m_file << g.snr << '\t' << samp_idx << '\t'
               << fmt::format("{:.10g}", time) << '\t' << g.filter_idx << '\t'
               << g.dm_idx << '\t' << g.dm << '\t' << g.members << '\t' << begin
               << '\t' << end << '\t' << g.f_low << '\t' << g.f_high << '\t'
               << g.chan_begin << '\t' << g.chan_end << '\t' << g.best_snr()
               << '\t' << best_begin << '\t' << best_end << '\t' << g.n_extents
               << '\t' << best_sample << '\t'
               << fmt::format("{:.10g}", best_time) << '\t' << best.dm << '\t'
               << best.filter_width << '\t' << best.f_low << '\t' << best.f_high
               << '\n';
    }
    m_file.flush();
    if (!m_file) {
        throw std::runtime_error("Failed writing candidate file (disk full?)");
    }
}

std::string cand_filename(std::time_t utc, SizeType beam) {
    std::tm tm{};
    if (gmtime_r(&utc, &tm) == nullptr) {
        throw std::invalid_argument("Candidate filename UTC is out of range");
    }
    std::array<char, 32> buf{};
    const auto len =
        std::strftime(buf.data(), buf.size(), "%Y-%m-%d-%H:%M:%S", &tm);
    if (len == 0) {
        throw std::runtime_error("Cannot format candidate filename UTC");
    }
    return fmt::format("{}_{:02d}.cand", std::string_view(buf.data(), len),
                       beam);
}

} // namespace frost::output
