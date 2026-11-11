#include "support/calibration.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iterator>
#include <random>
#include <span>
#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <dmt/algorithms/fdmt.hpp>
#include <dmt/algorithms/fdmt_boxcar.hpp>

#include <frost/dedisp/dedisp.hpp>
#include <frost/prepare/rfi.hpp>
#include <frost/search/postprocess.hpp>

using namespace frost::test::calibration;

TEST_CASE(
    "analytical noise calibration study measures signed unselected samples",
    "[calibration][dedisp][rfi]") {
    auto output = csv("noise.csv");
    if (output.is_open()) {
        output << "scenario,seed,gulps,plane,depth,staggered,chan_begin,chan_"
                  "end,dm,width,n,mean,sigma,above_3,below_minus_3,"
                  "independent_mask_sigma\n";
    }
    for (const auto& scenario : scenarios()) {
        INFO(scenario.name);
        auto fdmt_plan = frost::dedisp::make_dmt_fdmt_plan(observing_setup());
        dmt::BoxcarConfig search;
        search.max_boxcar       = 16;
        search.detect_thresh    = 4;
        search.peak_mode        = dmt::PeakFindingMode::kAllAboveThreshold;
        search.max_candidates   = 500'000;
        search.max_cands_per_dm = 0;
        dmt::algorithms::FDMTBoxcar engine(fdmt_plan, search);
        // Public stepper provides unthresholded DM-time rows, including all
        // staggered bands. It is an observer in this test, not a pipeline
        // stage.
        dmt::algorithms::FDMT observer(fdmt_plan, true, {}, 1,
                                       dmt::algorithms::kFDMTAutoFuse, true,
                                       kGulp);
        std::vector<float> root(fdmt_plan.get_dmt_size());
        RFICleaner cleaner(scenario.config, layout(), 2.0F);
        const auto planes = engine.get_search_plans();
        std::vector<NoiseTrial> trials;
        for (SizeType p = 0; p < planes.size(); ++p) {
            const auto& plane = planes[p];
            std::vector<SizeType> dms{0, plane.dm_grid.size() / 2,
                                      plane.dm_grid.size() - 1};
            std::sort(dms.begin(), dms.end());
            dms.erase(std::unique(dms.begin(), dms.end()), dms.end());
            for (const auto d : dms) {
                for (SizeType f = 0; f < plane.widths.size(); ++f) {
                    if (plane.widths[f] == 1 || plane.widths[f] == 4 ||
                        plane.widths[f] == 16) {
                        trials.push_back({p, d, f, {}});
                    }
                }
            }
        }
        std::mt19937 rng(kSeed);
        SizeType compared          = 0;
        constexpr SizeType kBlocks = 8;
        std::vector<uint8_t> previous_mask;
        for (SizeType gulp = 0; gulp < kBlocks; ++gulp) {
            auto raw              = noise_block(rng);
            const auto normalized = cleaner.execute_normalized(bytes(raw));
            const auto mask       = cleaner.channel_mask();
            // Endpoints straddling a mask change use the OR-mask variance;
            // compare emitted S/N exactly only for unchanged masks.
            const bool steady =
                gulp == 0 || std::ranges::equal(mask, previous_mask);
            previous_mask.assign(mask.begin(), mask.end());
            const auto candidates = engine.execute(normalized, mask);
            REQUIRE_FALSE(candidates.overflowed());
            observer.reset(normalized, root);
            for (SizeType p = 0; p < planes.size(); ++p) {
                const auto& plane = planes[p];
                observer.advance_until_remaining(plane.subband_level);
                const auto view = observer.view_subband(plane.subband_idx);
                REQUIRE(view.chan_begin == plane.chan_begin);
                REQUIRE(view.chan_end == plane.chan_end);
                for (auto& trial : trials) {
                    if (trial.plane != p) {
                        continue;
                    }
                    const auto width = plane.widths[trial.filter];
                    const auto row = view.data.subspan(trial.dm * kGulp, kGulp);
                    // dmt scores each block with its mask's analytical
                    // variance; unit-noise active channels need no rescale.
                    const auto variance = fdmt_plan.get_subband_variance_grid(
                        plane.level, plane.subband_idx, width, true, mask);
                    const double inv = variance[trial.dm] > 0
                                           ? 1.0 / std::sqrt(variance[trial.dm])
                                           : 0.0;
                    trial.active     = trial.active || variance[trial.dm] > 0;
                    // Exclude a conservative cold prefix on every gulp;
                    // window sums here never read observer history.
                    const auto begin =
                        plane.warmup_samples[trial.dm] + width - 1;
                    trial.predicted_variance_sum +=
                        static_cast<double>(kGulp - begin) *
                        variance[trial.dm] * inv * inv;
                    double sum = 0;
                    for (SizeType t = 0; t < row.size(); ++t) {
                        sum += row[t];
                        if (t >= width) {
                            sum -= row[t - width];
                        }
                        if (t >= begin) {
                            trial.values.add(sum * inv);
                        }
                    }
                }
                // Verify that the unthresholded observer measures the same
                // S/N as the fused production engine on emitted interior peaks.
                for (const auto& peak : candidates.candidates) {
                    if (!steady || peak.subband_level != plane.subband_level ||
                        peak.subband_idx != plane.subband_idx ||
                        peak.peak_sample < gulp * kGulp) {
                        continue;
                    }
                    const auto t = peak.peak_sample - gulp * kGulp;
                    if (t <
                        plane.warmup_samples[peak.dm_idx] + peak.width - 1) {
                        continue;
                    }
                    double sum = 0;
                    for (SizeType w = 0; w < peak.width; ++w) {
                        sum += view.data[peak.dm_idx * kGulp + t - w];
                    }
                    const auto variance = fdmt_plan.get_subband_variance_grid(
                        plane.level, plane.subband_idx, peak.width, true, mask);
                    const double inv = 1.0 / std::sqrt(variance[peak.dm_idx]);
                    REQUIRE(std::abs(sum * inv - peak.snr) < 0.002);
                    ++compared;
                }
            }
            observer.finalize();
        }
        if (scenario.name != "all_masked") {
            CHECK(compared > 0);
        }
        for (const auto& trial : trials) {
            const auto& plane = planes[trial.plane];
            const auto& m     = trial.values;
            REQUIRE(m.count > 20'000);
            // Independent noise: every plane with active channels is at
            // unit sigma, masked or not (no global sqrt(N/G) rescaling).
            if (trial.active && (scenario.name == "unmasked" ||
                                 scenario.name == "alternating_mask" ||
                                 scenario.name == "half_band_mask")) {
                INFO("plane=" << trial.plane
                              << " dm=" << plane.dm_grid[trial.dm]
                              << " width=" << plane.widths[trial.filter]
                              << " sigma=" << m.sigma());
                CHECK(std::abs(m.mean()) < 0.15);
                CHECK(std::abs(m.sigma() - 1.0) < 0.15);
            }
            if (output.is_open()) {
                output << scenario.name << ',' << kSeed << ',' << kBlocks << ','
                       << trial.plane << ',' << plane.subband_level << ','
                       << plane.staggered << ',' << plane.chan_begin << ','
                       << plane.chan_end << ',' << plane.dm_grid[trial.dm]
                       << ',' << plane.widths[trial.filter] << ',' << m.count
                       << ',' << m.mean() << ',' << m.sigma() << ','
                       << m.positive_tail << ',' << m.negative_tail << ','
                       << std::sqrt(trial.predicted_variance_sum /
                                    static_cast<double>(m.count))
                       << '\n';
            }
        }
    }
    if (output.is_open()) {
        output.flush();
        REQUIRE(output.good());
    }
}

TEST_CASE("injection recovery study follows peaks through candidate processing",
          "[calibration][recovery][rfi]") {
    auto output    = csv("recovery.csv");
    auto processed = csv("processed.csv");
    if (processed.is_open()) {
        processed
            << "scenario,seed,amplitude_scale,snr,best_snr,sample,dm,width,"
               "chan_begin,chan_end_inclusive,f_low,f_high,best_chan_begin,"
               "best_chan_end_inclusive,members,n_extents,best_sample,"
               "best_dm,best_width,best_f_low,best_f_high\n";
    }
    if (output.is_open()) {
        output
            << "scenario,seed,pulse,start,dm,width,chan_begin,chan_end,"
               "amplitude,raw_matches,final_matches,raw_snr,final_snr,raw_"
               "count,final_count,masked_channel_gulps,flagged_broad_samples,"
               "raw_peak_sample,raw_peak_dm,raw_peak_width,raw_peak_chan_begin,"
               "raw_peak_chan_end,raw_peak_depth,raw_peak_staggered,"
               "best_matches,best_matched_snr\n";
    }
    // Exact whitening is an oracle comparison with the same noise draws and
    // injected signal. It bypasses calibration/cleaning, not FDMT/boxcars.
    auto cases = scenarios();
    cases.insert(cases.begin(), {"known_gain_oracle", {}, true});
    for (uint32_t seed = kSeed; seed < kSeed + 16; ++seed) {
        for (const float amplitude_scale : {1.0F, 0.5F, 0.25F, 0.0F}) {
            INFO("seed=" << seed << " amplitude_scale=" << amplitude_scale);
            const auto observation = make_observation(seed, amplitude_scale);
            std::vector<Recovery> oracle;
            for (const auto& scenario : cases) {
                INFO(scenario.name);
                const auto run = run_recovery(
                    scenario, observation,
                    [&](const frost::search::Giant& peak) {
                        const auto best = peak.strongest_location();
                        if (processed.is_open()) {
                            processed
                                << scenario.name << ',' << seed << ','
                                << amplitude_scale << ',' << peak.snr << ','
                                << peak.best_snr() << ',' << peak.samp_idx
                                << ',' << peak.dm << ',' << peak.filter_width
                                << ',' << peak.chan_begin << ','
                                << peak.chan_end << ',' << peak.f_low << ','
                                << peak.f_high << ',' << peak.best_chan_begin
                                << ',' << peak.best_chan_end << ','
                                << peak.members << ',' << peak.n_extents << ','
                                << best.samp_idx << ',' << best.dm << ','
                                << best.filter_width << ',' << best.f_low << ','
                                << best.f_high << '\n';
                        }
                    });
                const auto& recovered  = run.pulses;
                const auto raw_count   = run.raw_count,
                           final_count = run.final_count;
                const auto masked = run.masked, broad = run.broad;
                if (scenario.name == "known_gain_oracle") {
                    oracle = recovered;
                }
                for (SizeType i = 0; i < recovered.size(); ++i) {
                    const auto& pulse = kPulses[i];
                    const auto& r     = recovered[i];
                    INFO(pulse.name);
                    // Scientific defects in optional transforms/masking are
                    // measured, never asserted as desirable behavior. The
                    // independent-noise baseline must recover these
                    // deliberately strong injections.
                    if (amplitude_scale == 1.0F &&
                        (scenario.name == "known_gain_oracle" ||
                         scenario.name == "unmasked")) {
                        CHECK(r.raw_matches > 0);
                        CHECK(r.final_matches > 0);
                        CHECK(r.raw_snr > 10);
                    }
                    if (amplitude_scale == 1.0F &&
                        scenario.name == "unmasked") {
                        CHECK(std::abs(r.raw_snr / oracle[i].raw_snr - 1.0) <
                              0.15);
                    }
                    if (output.is_open()) {
                        output
                            << scenario.name << ',' << seed << ',' << pulse.name
                            << ',' << pulse.start << ',' << pulse.dm << ','
                            << pulse.width << ',' << pulse.first_channel << ','
                            << pulse.end_channel << ','
                            << pulse.amplitude * amplitude_scale << ','
                            << r.raw_matches << ',' << r.final_matches << ','
                            << r.raw_snr << ',' << r.final_snr << ','
                            << raw_count << ',' << final_count << ',' << masked
                            << ',' << broad << ',' << r.raw_peak.peak_sample
                            << ',' << r.raw_peak.dm << ',' << r.raw_peak.width
                            << ',' << r.raw_peak.chan_begin << ','
                            << r.raw_peak.chan_end << ','
                            << r.raw_peak.subband_level << ','
                            << r.raw_peak.staggered << ',' << r.best_matches
                            << ',' << r.best_matched_snr << '\n';
                    }
                }
            }
        }
    }
    if (output.is_open()) {
        output.flush();
        REQUIRE(output.good());
    }
    if (processed.is_open()) {
        processed.flush();
        REQUIRE(processed.good());
    }
}
