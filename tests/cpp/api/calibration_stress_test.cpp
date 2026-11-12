#include <array>
#include <cmath>
#include <cstdlib>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "support/calibration.hpp"

using namespace frost::test::calibration;

namespace {
constexpr std::array kModels{NoiseModel::kWhite, NoiseModel::kAr1,
                             NoiseModel::kGainRamp, NoiseModel::kTailGainStep};
constexpr uint32_t kStressSeed = kSeed + 128;
constexpr std::array<SizeType, 3> kWidths{1, 4, 16};

uint32_t seed_count() {
    return study_seeds("FROST_CALIBRATION_STRESS_SEEDS", 1, 1, 128);
}

std::vector<Scenario> cleaners(uint32_t seed) {
    auto quiet = scenarios().front().config;
    std::vector<Scenario> out{{"oracle", quiet, true}, {"calibration", quiet}};
    for (const auto fill :
         {FillMode::kNeighbour, FillMode::kMean, FillMode::kNoise}) {
        RfiConfig cfg;
        cfg.fill         = fill;
        cfg.seed         = seed ^ uint64_t{0x5EED5EED};
        const char* name = fill == FillMode::kNeighbour ? "neighbour"
                           : fill == FillMode::kMean    ? "mean"
                                                        : "noise";
        out.push_back({name, cfg});
    }
    return out;
}

double ar_sigma(SizeType width, NoiseModel model) {
    if (model != NoiseModel::kAr1)
        return 1;
    double variance   = width;
    double covariance = 0.8;
    for (SizeType lag = 1; lag < width; ++lag) {
        variance += 2 * static_cast<double>(width - lag) * covariance;
        covariance *= 0.8;
    }
    return std::sqrt(variance / static_cast<double>(width));
}
} // namespace

TEST_CASE("stress fixtures preserve stream state and local signal noise units",
          "[calibration][stress]") {
    for (const auto model : kModels) {
        INFO(model_name(model));
        NoiseStream whole(kStressSeed, model, 512),
            split(kStressSeed, model, 512);
        const auto expected = whole.next(0, 128);
        const auto first    = split.next(0, 64);
        const auto second   = split.next(64, 64);
        for (SizeType i = 0; i < 64 * kChannels; ++i) {
            CHECK(first[i] == Catch::Approx(expected[i]).margin(0.0001));
            CHECK(second[i] ==
                  Catch::Approx(expected[64 * kChannels + i]).margin(0.0001));
        }
        const auto blank    = make_observation(kStressSeed, 0, model);
        const auto injected = make_observation(kStressSeed, 1, model);
        for (const auto& pulse : kPulses) {
            const auto c = pulse.end_channel - 1;
            const auto t =
                pulse.start + delay(pulse, kFirstFrequency + c * kChannelStep);
            CHECK(injected.known_gain[t * kChannels + c] -
                      blank.known_gain[t * kChannels + c] ==
                  Catch::Approx(pulse.amplitude).margin(0.0001));
        }
    }
}

TEST_CASE("noise stress study measures correlation gain drift and replacements",
          "[calibration][stress][rfi]") {
    auto output = csv("stress_noise.csv");
    if (output.is_open()) {
        output << "model,corrupt,cleaner,seed,fill_seed,segment,width,n,mean,"
                  "response_sigma,response_rms,above_3,below_minus_3,raw_model_"
                  "sigma,masked_"
                  "channel_gulps,flagged_broad_samples,narrow_cells,fill_"
                  "fallbacks\n";
    }
    for (uint32_t seed = kStressSeed; seed < kStressSeed + seed_count();
         ++seed) {
        for (const auto model : kModels) {
            for (const bool corrupt : {false, true}) {
                for (const auto& scenario : cleaners(seed)) {
                    INFO(model_name(model)
                         << " corrupt=" << corrupt
                         << " cleaner=" << scenario.name << " seed=" << seed);
                    auto cfg = observing_setup();
                    // dmt also requires a nonzero-delay trial. Measure DM 0.
                    cfg.dm_max        = 1;
                    cfg.subband_depth = 0;
                    dmt::BoxcarConfig search;
                    search.max_boxcar = 16;
                    search.peak_mode = dmt::PeakFindingMode::kAllAboveThreshold;
                    search.max_candidates =
                        65536; // At most 2*5*4096 points/call.
                    search.max_cands_per_dm = 0;
                    dmt::algorithms::FDMTBoxcar engine(
                        frost::dedisp::make_dmt_fdmt_plan(cfg), search);
                    RFICleaner cleaner(scenario.config, layout(), 2);
                    const auto& plane = engine.get_search_plans().front();
                    REQUIRE(plane.dm_grid.size() == 2);
                    REQUIRE(plane.dm_grid.front() == 0);
                    // full: full-gulp endpoints; tail: 37 real final samples;
                    // burst: endpoints inside the injected 4-row broadband RFI.
                    std::array<std::array<Moments, 3>, 3> measured{};
                    std::array<std::vector<double>, 3> response;
                    std::vector<double> history;
                    std::vector<float> oracle(kGulp * kChannels);
                    constexpr SizeType kTotal = 8 * kGulp + 37;
                    NoiseStream noise(seed, model, kTotal);
                    SizeType broad = 0, narrow = 0, masked = 0, fallbacks = 0;
                    for (SizeType begin = 0; begin < kTotal; begin += kGulp) {
                        const auto valid = std::min(kGulp, kTotal - begin);
                        auto raw         = noise.next(begin, valid);
                        if (corrupt)
                            add_rfi(raw, model, begin, valid, kTotal);
                        std::span<const float> normalized;
                        if (scenario.known_gain) {
                            std::fill(oracle.begin(), oracle.end(), 0);
                            for (SizeType c = 0; c < kChannels; ++c) {
                                for (SizeType t = 0; t < valid; ++t) {
                                    oracle[c * kGulp + t] =
                                        (raw[t * kChannels + c] - offset(c)) /
                                        input_gain(model, begin + t, c, kTotal);
                                }
                            }
                            normalized = oracle;
                        } else {
                            normalized =
                                cleaner.execute_normalized(bytes(raw), valid);
                            broad += cleaner.report().samps_broad;
                            narrow += cleaner.report().cells_narrow;
                            masked += cleaner.report().chans_masked;
                            fallbacks += cleaner.report().fill_fallbacks;
                        }
                        std::vector<double> band(valid, 0);
                        for (SizeType c = 0; c < kChannels; ++c) {
                            for (SizeType t = 0; t < valid; ++t)
                                band[t] += normalized[c * kGulp + t];
                        }
                        auto joined = history;
                        joined.insert(joined.end(), band.begin(), band.end());
                        std::vector<double> prefix(joined.size() + 1, 0);
                        for (SizeType t = 0; t < joined.size(); ++t)
                            prefix[t + 1] = prefix[t] + joined[t];
                        for (SizeType w = 0; w < kWidths.size(); ++w) {
                            const auto width = kWidths[w];
                            const auto found =
                                std::find(plane.widths.begin(),
                                          plane.widths.end(), width);
                            REQUIRE(found != plane.widths.end());
                            const auto inv =
                                plane.inv_sigma_lut[found -
                                                    plane.widths.begin()];
                            response[w].assign(valid, 0);
                            for (SizeType t = 0; t < valid; ++t) {
                                const auto end = history.size() + t + 1;
                                if (end < width)
                                    continue;
                                const auto sum =
                                    prefix[end] - prefix[end - width];
                                const double snr = sum * inv;
                                response[w][t]   = snr;
                                measured[valid == kGulp ? 0 : 1][w].add(snr);
                                if (begin + t >= 4100 && begin + t < 4104)
                                    measured[2][w].add(snr);
                            }
                        }
                        const auto peaks = engine.execute(normalized);
                        REQUIRE_FALSE(peaks.overflowed());
                        for (const auto& peak : peaks.candidates) {
                            if (peak.dm_idx != 0)
                                continue;
                            if (peak.peak_sample < begin)
                                continue;
                            const auto t = peak.peak_sample - begin;
                            if (t >= valid || begin + t + 1 < peak.width)
                                continue;
                            const auto found = std::find(
                                kWidths.begin(), kWidths.end(), peak.width);
                            if (found != kWidths.end()) {
                                REQUIRE(
                                    std::abs(
                                        response[found - kWidths.begin()][t] -
                                        peak.snr) < 0.002);
                            }
                        }
                        const auto keep = std::min<SizeType>(
                            joined.size(), kWidths.back() - 1);
                        history.assign(joined.end() - keep, joined.end());
                    }
                    constexpr std::array<std::string_view, 3> kSegments{
                        "full", "tail", "burst"};
                    for (SizeType s = 0; s < measured.size(); ++s) {
                        for (SizeType w = 0; w < kWidths.size(); ++w) {
                            const auto& m = measured[s][w];
                            REQUIRE(m.count > 0);
                            if (scenario.known_gain && !corrupt && s == 0) {
                                CHECK(m.sigma() ==
                                      Catch::Approx(ar_sigma(kWidths[w], model))
                                          .epsilon(0.12));
                            }
                            if (corrupt && scenario.name == "mean" && s == 2 &&
                                w == 0) {
                                CHECK(m.mean() == 0);
                                CHECK(m.sigma() == 0);
                            }
                            if (output.is_open()) {
                                output
                                    << model_name(model) << ',' << corrupt
                                    << ',' << scenario.name << ',' << seed
                                    << ',' << scenario.config.seed << ','
                                    << kSegments[s] << ',' << kWidths[w] << ','
                                    << m.count << ',' << m.mean() << ','
                                    << m.sigma() << ',' << m.rms() << ','
                                    << m.positive_tail << ',' << m.negative_tail
                                    << ',' << ar_sigma(kWidths[w], model) << ','
                                    << masked << ',' << broad << ',' << narrow
                                    << ',' << fallbacks << '\n';
                            }
                        }
                    }
                }
            }
        }
    }
    if (output.is_open()) {
        output.flush();
        REQUIRE(output.good());
    }
}

TEST_CASE(
    "recovery stress study pairs contaminated and correlated observations",
    "[calibration][stress][recovery]") {
    auto output    = csv("stress_recovery.csv");
    auto processed = csv("stress_processed.csv");
    if (processed.is_open()) {
        processed
            << "model,corrupt,cleaner,seed,fill_seed,amplitude_scale,snr,best_"
               "snr,sample,dm,width,f_low,f_high,chan_begin,chan_end_inclusive,"
               "members,n_extents,best_sample,best_dm,best_width,best_f_low,"
               "best_f_high,best_chan_begin,best_chan_end_inclusive\n";
    }
    if (output.is_open()) {
        output << "model,corrupt,cleaner,seed,fill_seed,pulse,amplitude_scale,"
                  "raw_matches,representative_matches,best_matches,raw_snr,"
                  "best_matched_snr,oracle_raw_snr,raw_count,final_count,"
                  "masked_channel_gulps,flagged_broad_samples,narrow_cells,"
                  "fill_fallbacks,reused_tails,raw_peak_sample,raw_peak_dm,raw_"
                  "peak_width,raw_peak_f_low,raw_peak_f_high\n";
    }
    for (uint32_t seed = kStressSeed; seed < kStressSeed + seed_count();
         ++seed) {
        for (const auto model : kModels) {
            for (const bool corrupt : {false, true}) {
                for (const float amplitude : {1.0F, 0.25F, 0.0F}) {
                    const auto observation =
                        make_observation(seed, amplitude, model, corrupt);
                    std::vector<Recovery> oracle;
                    std::array<SizeType, 3> neighbour_flags{};
                    for (const auto& scenario : cleaners(seed)) {
                        INFO(model_name(model) << " corrupt=" << corrupt
                                               << " cleaner=" << scenario.name
                                               << " seed=" << seed);
                        const auto run = run_recovery(
                            scenario, observation,
                            [&](const frost::search::Giant& peak) {
                                if (!processed.is_open())
                                    return;
                                const auto best = peak.strongest_location();
                                processed
                                    << model_name(model) << ',' << corrupt
                                    << ',' << scenario.name << ',' << seed
                                    << ',' << scenario.config.seed << ','
                                    << amplitude << ',' << peak.snr << ','
                                    << peak.best_snr() << ',' << peak.samp_idx
                                    << ',' << peak.dm << ','
                                    << peak.filter_width << ',' << peak.f_low
                                    << ',' << peak.f_high << ','
                                    << peak.chan_begin << ',' << peak.chan_end
                                    << ',' << peak.members << ','
                                    << peak.n_extents << ',' << best.samp_idx
                                    << ',' << best.dm << ','
                                    << best.filter_width << ',' << best.f_low
                                    << ',' << best.f_high << ','
                                    << peak.best_chan_begin << ','
                                    << peak.best_chan_end << '\n';
                            });
                        if (scenario.known_gain)
                            oracle = run.pulses;
                        if (scenario.name == "neighbour")
                            neighbour_flags = {run.narrow, run.broad,
                                               run.masked};
                        if (scenario.name == "mean" ||
                            scenario.name == "noise") {
                            CHECK((std::array<SizeType, 3>{
                                      run.narrow, run.broad, run.masked}) ==
                                  neighbour_flags);
                        }
                        if (corrupt && !scenario.known_gain &&
                            scenario.name != "calibration") {
                            CHECK(run.narrow > 0);
                            CHECK(run.broad >= 4);
                        }
                        for (SizeType i = 0; i < run.pulses.size(); ++i) {
                            const auto& r = run.pulses[i];
                            if (model == NoiseModel::kWhite && !corrupt &&
                                amplitude == 1 &&
                                (scenario.known_gain ||
                                 scenario.name == "calibration")) {
                                CHECK(r.raw_matches > 0);
                                CHECK(r.best_matches > 0);
                            }
                            if (output.is_open()) {
                                output
                                    << model_name(model) << ',' << corrupt
                                    << ',' << scenario.name << ',' << seed
                                    << ',' << scenario.config.seed << ','
                                    << kPulses[i].name << ',' << amplitude
                                    << ',' << r.raw_matches << ','
                                    << r.final_matches << ',' << r.best_matches
                                    << ',' << r.raw_snr << ','
                                    << r.best_matched_snr << ','
                                    << oracle[i].raw_snr << ',' << run.raw_count
                                    << ',' << run.final_count << ','
                                    << run.masked << ',' << run.broad << ','
                                    << run.narrow << ',' << run.fallbacks << ','
                                    << run.reused << ','
                                    << r.raw_peak.peak_sample << ','
                                    << r.raw_peak.dm << ',' << r.raw_peak.width
                                    << ',' << r.raw_peak.f_low_mhz << ','
                                    << r.raw_peak.f_high_mhz << '\n';
                            }
                        }
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
