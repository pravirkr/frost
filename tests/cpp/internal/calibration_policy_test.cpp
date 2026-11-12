#include "support/calibration_policy.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

using namespace frost::test::calibration;
namespace study = frost::test::calibration::policy;

namespace {
uint32_t seed_count() {
    return study_seeds("FROST_CALIBRATION_POLICY_SEEDS", 2, 2, 64);
}
inline constexpr uint32_t kTrainingSeed   = kSeed + 10000;
inline constexpr uint32_t kValidationSeed = kSeed + 20000;
} // namespace

TEST_CASE(
    "experimental calibration reuses the production fit and excludes padding",
    "[calibration][policy][contracts]") {
    const auto data = study::observation(kSeed, study::Model::kGainRamp, 1);
    RFICleaner production(scenarios().front().config, layout(), 2);
    for (SizeType begin = 0; begin < data.nsamps; begin += kGulp) {
        const auto valid = std::min(kGulp, data.nsamps - begin);
        std::vector<float> raw(kGulp * kChannels, 1e6F);
        std::copy_n(data.raw.begin() + begin * kChannels, valid * kChannels,
                    raw.begin());
        const auto actual = production.execute_normalized(bytes(raw), valid);
        if (valid < 64) {
            for (SizeType c = 0; c < kChannels; ++c)
                for (SizeType t = valid; t < kGulp; ++t)
                    CHECK(actual[c * kGulp + t] == 0);
            continue;
        }
        const auto windows = std::max<SizeType>(valid / study::kWindow, 1);
        double error       = 0;
        for (SizeType w = 0; w < windows; ++w) {
            const auto first = w * study::kWindow;
            const auto count =
                w + 1 == windows ? valid - first : study::kWindow;
            const auto fit = study::fit(raw, first, count, begin);
            for (SizeType c = 0; c < kChannels; ++c) {
                for (SizeType t = first; t < first + count; ++t) {
                    const float expected =
                        (raw[t * kChannels + c] - fit.location[c]) *
                        (1 / fit.scale[c]);
                    error = std::max<double>(
                        error, std::abs(actual[c * kGulp + t] - expected));
                }
            }
        }
        CHECK(error < 0.00001);
    }
}

TEST_CASE("lagged calibration cannot fit a warm pulse into its own window",
          "[calibration][policy][contracts]") {
    std::mt19937 rng(kSeed);
    auto warm     = noise_block(rng);
    auto raw      = noise_block(rng);
    auto injected = raw;
    for (SizeType t = 100; t < 612; ++t)
        for (SizeType c = 0; c < kChannels; ++c)
            injected[t * kChannels + c] += gain(c);
    study::CalibrationPolicyPlan blank(study::Policy::kLagged),
        signal(study::Policy::kLagged);
    blank.execute(warm, 0, kGulp);
    signal.execute(warm, 0, kGulp);
    const auto a = blank.execute(raw, kGulp, kGulp);
    const auto b = signal.execute(injected, kGulp, kGulp);
    for (SizeType c = 0; c < kChannels; ++c) {
        for (SizeType t = 0; t < 100; ++t)
            CHECK(a[c * kGulp + t] == b[c * kGulp + t]);
        CHECK(b[c * kGulp + 100] > a[c * kGulp + 100]);
    }
}

TEST_CASE("calibration interpolation is convex and cold fallbacks share the "
          "same information",
          "[calibration][policy][contracts]") {
    study::Knot left, right;
    left.center  = 10;
    right.center = 30;
    left.location.fill(0);
    left.scale.fill(1);
    right.location.fill(4);
    right.scale.fill(3);
    for (const double time : {0.0, 10.0, 20.0, 30.0, 40.0}) {
        const auto fit      = study::interpolate(left, right, time);
        const auto fraction = std::clamp((time - 10) / 20, 0.0, 1.0);
        CHECK(fit.location[0] == 4 * fraction);
        CHECK(fit.scale[0] == 1 + 2 * fraction);
    }
    const auto data = study::observation(kSeed, study::Model::kCold, 1);
    std::vector<float> raw(kGulp * kChannels, 1e6F);
    std::copy(data.raw.begin(), data.raw.end(), raw.begin());
    study::CalibrationPolicyPlan current(study::Policy::kCurrent);
    const auto expected = current.execute(raw, 0, data.nsamps);
    for (const auto policy :
         {study::Policy::kLagged, study::Policy::kInterpolated}) {
        study::CalibrationPolicyPlan plan(policy);
        const auto result = plan.execute(raw, 0, data.nsamps);
        double error      = 0;
        for (SizeType i = 0; i < result.size(); ++i)
            error = std::max<double>(error, std::abs(expected[i] - result[i]));
        CHECK(error < 0.00001);
        CHECK_THROWS_AS(plan.execute(raw, 0, data.nsamps),
                        std::invalid_argument);
    }
}

TEST_CASE("calibration policies compare held-out recovery at learned blank "
          "thresholds",
          "[calibration][policy][study]") {
    auto noise_csv    = csv("policy_noise.csv");
    auto recovery_csv = csv("policy_recovery.csv");
    if (noise_csv.is_open())
        noise_csv
            << "phase,model,policy,seed,nsamps,threshold,max_score,"
               "collected_points,n_w1,mean_w1,sigma_w1,n_w8,mean_w8,sigma_w8,"
               "n_w512,mean_w512,sigma_w512\n";
    if (recovery_csv.is_open())
        recovery_csv
            << "model,policy,seed,pulse,pulse_start,pulse_dm,pulse_width,has_"
               "forced_dm0,amplitude,threshold,matched_score,"
               "recovered,oracle_matched_score,forced_dm0,blank_forced_dm0,"
               "oracle_signal_dm0,signal_dm0,blank_gate_match\n";
    const auto count = seed_count();
    std::array<std::array<double, study::kPolicies.size()>,
               study::kModels.size()>
        thresholds{};
    for (SizeType m = 0; m < study::kModels.size(); ++m) {
        for (uint32_t s = 0; s < count; ++s) {
            const auto data =
                study::observation(kTrainingSeed + s, study::kModels[m], 0);
            for (SizeType p = 0; p < study::kPolicies.size(); ++p) {
                const auto run =
                    study::score(study::kPolicies[p], study::kModels[m], data);
                thresholds[m][p] = std::max(thresholds[m][p], run.maximum);
                if (noise_csv.is_open()) {
                    noise_csv << "training," << study::name(study::kModels[m])
                              << ',' << study::name(study::kPolicies[p]) << ','
                              << kTrainingSeed + s << ',' << data.nsamps << ",,"
                              << run.maximum << ',' << run.candidates;
                    for (const auto& values : run.response) {
                        if (values.count)
                            noise_csv << ',' << values.count << ','
                                      << values.mean() << ',' << values.sigma();
                        else
                            noise_csv << ",0,,";
                    }
                    noise_csv << '\n';
                }
            }
        }
    }
    for (SizeType m = 0; m < study::kModels.size(); ++m) {
        const auto model = study::kModels[m];
        for (uint32_t s = 0; s < count; ++s) {
            std::array<study::ScoreRun, study::kPolicies.size()> blank;
            for (const float amplitude : {0.0F, 0.5F, 1.0F}) {
                const auto data =
                    study::observation(kValidationSeed + s, model, amplitude);
                std::array<study::ScoreRun, study::kPolicies.size()> runs;
                for (SizeType p = 0; p < study::kPolicies.size(); ++p)
                    runs[p] = study::score(study::kPolicies[p], model, data);
                if (amplitude == 0)
                    blank = runs;
                for (SizeType p = 0; p < study::kPolicies.size(); ++p) {
                    INFO(study::name(model)
                         << '/' << study::name(study::kPolicies[p]) << " seed="
                         << kValidationSeed + s << " amplitude=" << amplitude);
                    CHECK(thresholds[m][p] >= study::kScoreFloor);
                    const auto& run    = runs[p];
                    const auto& oracle = runs.back();
                    if (amplitude == 0 && noise_csv.is_open()) {
                        noise_csv << "validation," << study::name(model) << ','
                                  << study::name(study::kPolicies[p]) << ','
                                  << kValidationSeed + s << ',' << data.nsamps
                                  << ',' << thresholds[m][p] << ','
                                  << run.maximum << ',' << run.candidates;
                        for (const auto& values : run.response) {
                            if (values.count)
                                noise_csv << ',' << values.count << ','
                                          << values.mean() << ','
                                          << values.sigma();
                            else
                                noise_csv << ",0,,";
                        }
                        noise_csv << '\n';
                    }
                    const auto pulses = study::pulses(model);
                    for (SizeType i = 0; i < pulses.size(); ++i) {
                        const double transfer =
                            run.forced_dm0[i] - blank[p].forced_dm0[i];
                        const double oracle_transfer =
                            oracle.forced_dm0[i] - blank.back().forced_dm0[i];
                        if (amplitude > 0 && pulses[i].dm == 0)
                            CHECK(oracle_transfer ==
                                  Catch::Approx(12 * amplitude).margin(0.001));
                        if (recovery_csv.is_open())
                            recovery_csv
                                << study::name(model) << ','
                                << study::name(study::kPolicies[p]) << ','
                                << kValidationSeed + s << ',' << pulses[i].name
                                << ',' << pulses[i].start << ',' << pulses[i].dm
                                << ',' << pulses[i].width << ','
                                << (pulses[i].dm == 0) << ',' << amplitude
                                << ',' << thresholds[m][p] << ','
                                << run.matched[i] << ','
                                << (run.matched[i] > thresholds[m][p]) << ','
                                << oracle.matched[i] << ',' << run.forced_dm0[i]
                                << ',' << blank[p].forced_dm0[i] << ','
                                << oracle_transfer << ',' << transfer << ','
                                << (blank[p].matched[i] > thresholds[m][p])
                                << '\n';
                    }
                }
            }
        }
    }
    for (auto* output : {&noise_csv, &recovery_csv}) {
        if (output->is_open()) {
            output->flush();
            REQUIRE(output->good());
        }
    }
}
