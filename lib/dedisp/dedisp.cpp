#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <string>

#include <frost/common/numeric.hpp>
#include <frost/dedisp/dedisp.hpp>

namespace frost::dedisp {

namespace {
// A runaway list would exhaust memory long before FDMT sees it.
constexpr std::size_t kMaxDmTrials = std::size_t{1} << 22;
} // namespace

std::vector<float> generate_dm_list(const DedispPlanConfig& cfg) {
    // dm_tol == 1 gives dm == prev (the list never advances); dm_tol < 1 puts
    // a negative number under the square root. -ffast-math removes NaN
    // guards, so check the inputs up front rather than the result.
    if (!is_finite(cfg.dm_tol) || !(cfg.dm_tol > 1.0F)) {
        throw std::invalid_argument("generate_dm_list: dm_tol must be > 1");
    }
    if (!is_finite(cfg.dm_min) || !is_finite(cfg.dm_max) ||
        !is_finite(cfg.dm_pulse_width) || !is_finite(cfg.dt) ||
        !is_finite(cfg.f0) || !is_finite(cfg.df) || !(cfg.dt > 0.0F) ||
        cfg.nchans == 0) {
        throw std::invalid_argument(
            "generate_dm_list: dm range, dt, f0 and df must be finite");
    }
    // Dispersion-smearing trial spacing (Levin 2012; Barsdell et al. 2012,
    // MNRAS 422, 379), evaluated in double precision.
    const double dt =
        static_cast<double>(cfg.dt) * 1e6; // seconds -> microseconds
    const double ti = static_cast<double>(cfg.dm_pulse_width);
    const double f =
        (static_cast<double>(cfg.f0) +
         (((static_cast<double>(cfg.nchans) / 2.0) - 0.5) * cfg.df)) *
        1e-3; // MHz -> GHz
    const double tol2 = static_cast<double>(cfg.dm_tol) * cfg.dm_tol;
    const double a    = 8.3 * cfg.df / (f * f * f);
    const double a2   = a * a;
    const double b2 =
        a2 * (static_cast<double>(cfg.nchans) * cfg.nchans / 16.0);
    const double c = ((dt * dt) + (ti * ti)) * (tol2 - 1.0);

    std::vector<float> dm_list;
    dm_list.push_back(cfg.dm_min);
    while (dm_list.back() < cfg.dm_max) {
        const double prev  = dm_list.back();
        const double prev2 = prev * prev;
        const double k     = c + (tol2 * a2 * prev2);
        const double dm =
            ((b2 * prev) + std::sqrt((-a2 * b2 * prev2) + ((a2 + b2) * k))) /
            (a2 + b2);
        const auto next = static_cast<float>(dm);
        if (!is_finite(next) || !(next > static_cast<float>(prev))) {
            throw std::runtime_error(
                "generate_dm_list: the DM trial spacing collapsed at DM " +
                std::to_string(prev));
        }
        if (dm_list.size() >= kMaxDmTrials) {
            throw std::invalid_argument(
                "generate_dm_list: more than 4M DM trials; raise dm_tol or "
                "narrow the DM range");
        }
        dm_list.push_back(next);
    }
    return dm_list;
}

dmt::plans::FDMTPlan make_dmt_fdmt_plan(const DedispPlanConfig& cfg) {
    if (cfg.nchans < 2 || !is_finite(cfg.f0) || !is_finite(cfg.df) ||
        cfg.df == 0.0F) {
        throw std::invalid_argument("FDMT requires at least two channels and a "
                                    "finite, nonzero frequency step");
    }
    const float last  = cfg.f0 + static_cast<float>(cfg.nchans - 1) * cfg.df;
    const float f_min = std::min(cfg.f0, last) - 0.5F * std::abs(cfg.df);
    const float f_max =
        f_min + static_cast<float>(cfg.nchans) * std::abs(cfg.df);
    if (!is_positive_finite(f_min) || !is_positive_finite(f_max) ||
        !(f_max > f_min)) {
        throw std::invalid_argument(
            "FDMT requires positive ascending frequency edges");
    }
    return dmt::plans::FDMTPlan(f_min, f_max, cfg.nchans, cfg.block_nsamps,
                                cfg.dt, generate_dm_list(cfg), "valid",
                                cfg.subband_depth);
}

} // namespace frost::dedisp
