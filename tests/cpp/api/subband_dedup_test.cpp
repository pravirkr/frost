#include <cmath>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include <frost/search/subband_dedup.hpp>

using frost::SizeType;
using frost::search::ClusterTolerances;
using frost::search::Giant;
using frost::search::subband_dm_step;
using frost::search::subband_time_shift_samples;
using frost::search::SubbandDedupPlan;

namespace {

constexpr float kTsamp        = 0.001F;
constexpr SizeType kTimeTol   = 3;
constexpr SizeType kFilterTol = 1;
constexpr float kDm           = 100.0F;

Giant band(float snr,
           SizeType samp_idx,
           float dm,
           float f_low,
           float f_high,
           SizeType chan_begin,
           SizeType chan_end) {
    Giant g;
    g.snr          = snr;
    g.dm           = dm;
    g.samp_idx     = samp_idx;
    g.filter_idx   = 2;
    g.filter_width = 4;
    g.dm_idx       = 0;
    g.members      = 1;
    g.f_low        = f_low;
    g.f_high       = f_high;
    g.chan_begin   = chan_begin;
    g.chan_end     = chan_end;
    return g;
}

Giant full_band(float snr, SizeType samp_idx, float dm = kDm) {
    return band(snr, samp_idx, dm, 1000.0F, 1256.0F, 0, 63);
}

Giant nested_band(float snr, SizeType samp_idx, float dm = kDm) {
    return band(snr, samp_idx, dm, 1100.0F, 1164.0F, 16, 31);
}

SizeType
shifted_sample(SizeType wide_samp, float dm, float f_wide, float f_narrow) {
    const double shift =
        subband_time_shift_samples(dm, f_wide, f_narrow, kTsamp);
    return wide_samp - static_cast<SizeType>(std::llround(shift));
}

const ClusterTolerances kTol{kTimeTol, kFilterTol, /*dm_tol=*/10};

} // namespace

TEST_CASE("Subband dedup merges a nested pair delayed by the dispersion law",
          "[clustering]") {
    const double shift =
        subband_time_shift_samples(kDm, 1000.0F, 1100.0F, kTsamp);
    REQUIRE(shift > static_cast<double>(kTimeTol * 4) + 1.0);

    auto wide   = full_band(/*snr=*/8.0F, /*samp_idx=*/5000);
    auto narrow = nested_band(
        /*snr=*/9.0F, shifted_sample(5000, kDm, 1000.0F, 1100.0F));
    narrow.members = 2;

    SubbandDedupPlan plan;
    const auto merged =
        plan.execute(std::vector<Giant>{wide, narrow}, kTol, kTsamp);
    REQUIRE(merged.size() == 1);
    CHECK(merged[0].chan_begin == 0);
    CHECK(merged[0].chan_end == 63);
    CHECK(merged[0].samp_idx == 5000);
    CHECK(merged[0].dm == kDm);
    CHECK(merged[0].snr == 8.0F);
    CHECK(merged[0].members == 3);
}

TEST_CASE("Subband dedup leaves a pair apart when the delay is missing",
          "[clustering]") {
    auto wide   = full_band(8.0F, 5000);
    auto narrow = nested_band(9.0F, 5000);

    SubbandDedupPlan plan;
    const auto merged =
        plan.execute(std::vector<Giant>{wide, narrow}, kTol, kTsamp);
    CHECK(merged.size() == 2);
}

TEST_CASE("Subband dedup does not join disjoint channel ranges",
          "[clustering]") {
    auto bottom = band(10.0F, 5000, kDm, 1000.0F, 1064.0F, 0, 15);
    auto top    = band(12.0F, shifted_sample(5000, kDm, 1000.0F, 1192.0F), kDm,
                       1192.0F, 1256.0F, 48, 63);

    SubbandDedupPlan plan;
    const auto merged =
        plan.execute(std::vector<Giant>{bottom, top}, kTol, kTsamp);
    CHECK(merged.size() == 2);
}

TEST_CASE("Subband dedup joins both quarters through the full band",
          "[clustering]") {
    auto wide = full_band(8.0F, 5000);
    auto low  = band(9.0F, 5000, kDm, 1000.0F, 1064.0F, 0, 15);
    auto high = band(10.0F, shifted_sample(5000, kDm, 1000.0F, 1192.0F), kDm,
                     1192.0F, 1256.0F, 48, 63);

    SubbandDedupPlan plan;
    const auto merged =
        plan.execute(std::vector<Giant>{low, wide, high}, kTol, kTsamp);
    REQUIRE(merged.size() == 1);
    CHECK(merged[0].chan_begin == 0);
    CHECK(merged[0].chan_end == 63);
    CHECK(merged[0].samp_idx == 5000);
    CHECK(merged[0].dm == kDm);
    CHECK(merged[0].snr == 8.0F);
    CHECK(merged[0].members == 3);
}

TEST_CASE("Subband dedup accepts a DM within the boxcar delay and rejects "
          "past it",
          "[clustering]") {
    const auto narrow_proto = nested_band(9.0F, 5000);
    const double step =
        subband_dm_step(narrow_proto.f_low, narrow_proto.f_high, kTsamp);
    REQUIRE(step > 0.0);
    // filter_width is 4, so the gate is 4 narrow-band delay samples.
    constexpr double kDelaySamples = 4.0;

    auto wide                  = full_band(8.0F, 5000);
    const SizeType narrow_samp = shifted_sample(5000, kDm, 1000.0F, 1100.0F);
    auto inside =
        nested_band(9.0F, narrow_samp, kDm + static_cast<float>(step));
    auto sidelobe =
        nested_band(9.0F, narrow_samp, kDm + static_cast<float>(2.5 * step));
    auto outside =
        nested_band(9.0F, narrow_samp,
                    kDm + static_cast<float>((kDelaySamples + 2.0) * step));
    const auto delta_of = [&](const Giant& g) {
        return std::abs(static_cast<double>(g.dm) -
                        static_cast<double>(wide.dm));
    };
    REQUIRE(delta_of(sidelobe) > (2.0 * step));
    REQUIRE(delta_of(sidelobe) <= (kDelaySamples * step));
    REQUIRE(delta_of(outside) > (kDelaySamples * step));

    SubbandDedupPlan plan;
    CHECK(plan.execute(std::vector<Giant>{wide, inside}, kTol, kTsamp).size() ==
          1);
    CHECK(
        plan.execute(std::vector<Giant>{wide, sidelobe}, kTol, kTsamp).size() ==
        1);
    CHECK(
        plan.execute(std::vector<Giant>{wide, outside}, kTol, kTsamp).size() ==
        2);
}

TEST_CASE("Subband dedup rejects a filter index outside tolerance",
          "[clustering]") {
    auto wide = full_band(8.0F, 5000);
    auto narrow =
        nested_band(9.0F, shifted_sample(5000, kDm, 1000.0F, 1100.0F));
    narrow.filter_idx = wide.filter_idx + kFilterTol + 1;

    SubbandDedupPlan plan;
    CHECK(plan.execute(std::vector<Giant>{wide, narrow}, kTol, kTsamp).size() ==
          2);
}

TEST_CASE("Subband dedup breaks an equal-span tie by S/N", "[clustering]") {
    // Equal extents only share a group across planes (one node searched
    // twice); within a plane pass 1 already kept them apart.
    auto quiet = full_band(7.0F, 5000);
    auto loud  = full_band(9.0F, 5000);
    auto narrow =
        nested_band(9.0F, shifted_sample(5000, kDm, 1000.0F, 1100.0F));
    quiet.members = 1;
    quiet.plane   = 1;
    loud.members  = 4;

    SubbandDedupPlan plan;
    const auto merged =
        plan.execute(std::vector<Giant>{quiet, narrow, loud}, kTol, kTsamp);
    REQUIRE(merged.size() == 1);
    CHECK(merged[0].snr == 9.0F);
    CHECK(merged[0].members == 6);
    CHECK(merged[0].chan_end == 63);
}

TEST_CASE("Subband dedup leaves equal extents for the within-plane pass",
          "[clustering]") {
    auto a = full_band(8.0F, 5000);
    auto b = full_band(12.0F, 5001);
    SubbandDedupPlan plan;
    CHECK(plan.execute(std::vector<Giant>{a, b}, kTol, kTsamp).size() == 2);
}

TEST_CASE("Seeded subband dedup does not bridge two wide events through a "
          "nested row",
          "[clustering]") {
    // Pass 1 kept these full-band rows apart (20 samples > 3 * width 4). A
    // nested row half way between matches both. Transitive mode (the
    // default) joins all three; seeded mode lets it join one of them and
    // keeps the two events apart.
    auto early = full_band(8.0F, 5000);
    auto late  = full_band(9.0F, 5020);
    auto narrow =
        nested_band(9.0F, shifted_sample(5010, kDm, 1000.0F, 1100.0F));
    const SubbandDedupPlan transitive;
    REQUIRE(transitive.execute(std::vector<Giant>{early, narrow}, kTol, kTsamp)
                .size() == 1);
    REQUIRE(transitive.execute(std::vector<Giant>{late, narrow}, kTol, kTsamp)
                .size() == 1);
    CHECK(transitive
              .execute(std::vector<Giant>{early, narrow, late}, kTol, kTsamp)
              .size() == 1);

    const SubbandDedupPlan plan(frost::search::SubbandDedupMode::kSeeded);

    for (const auto& input : {std::vector<Giant>{early, narrow, late},
                              std::vector<Giant>{narrow, late, early}}) {
        const auto merged = plan.execute(input, kTol, kTsamp);
        REQUIRE(merged.size() == 2);
        SizeType members = 0;
        bool saw_early   = false;
        bool saw_late    = false;
        for (const auto& g : merged) {
            CHECK(g.chan_end == 63);
            members += g.members;
            saw_early = saw_early || g.samp_idx == 5000;
            saw_late  = saw_late || g.samp_idx == 5020;
            if (g.samp_idx == 5020) {
                CHECK(g.members == 2); // the brighter wide row takes it
            }
        }
        CHECK(saw_early);
        CHECK(saw_late);
        CHECK(members == 3);
    }
}

TEST_CASE("Seeded subband dedup still joins nested extents into the full "
          "band",
          "[clustering]") {
    auto wide = full_band(8.0F, 5000);
    auto low  = band(9.0F, 5000, kDm, 1000.0F, 1064.0F, 0, 15);
    auto high = band(10.0F, shifted_sample(5000, kDm, 1000.0F, 1192.0F), kDm,
                     1192.0F, 1256.0F, 48, 63);
    const SubbandDedupPlan plan(frost::search::SubbandDedupMode::kSeeded);
    const auto merged =
        plan.execute(std::vector<Giant>{low, wide, high}, kTol, kTsamp);
    REQUIRE(merged.size() == 1);
    CHECK(merged[0].chan_end == 63);
    CHECK(merged[0].members == 3);
    CHECK(merged[0].n_extents == 3);
}

TEST_CASE("Subband dedup returns an empty list unchanged", "[clustering]") {
    SubbandDedupPlan plan;
    CHECK(plan.execute({}, kTol, kTsamp).empty());
}

namespace {

// UWL-like geometry from the J0540 benchmark: 704-4032 MHz, 512 us.
constexpr float kUwlTsamp = 512.0e-6F;

Giant uwl(float snr,
          SizeType samp_idx,
          float dm,
          float f_low,
          float f_high,
          SizeType chan_begin,
          SizeType chan_end,
          SizeType width) {
    Giant g = band(snr, samp_idx, dm, f_low, f_high, chan_begin, chan_end);
    g.filter_width = width;
    g.filter_idx   = 0;
    for (SizeType w = width; w > 1; w >>= 1) {
        ++g.filter_idx;
    }
    return g;
}

const ClusterTolerances kUwlTol{3, 3, 200, /*dm_smear=*/3.0,
                                static_cast<double>(kUwlTsamp)};

} // namespace

TEST_CASE("Subband dedup keeps a band-limited pulse apart from an unrelated "
          "wide row",
          "[clustering][subband]") {
    // A 256 MHz band at 3.9 GHz cannot constrain DM (about 14 DM per
    // sample of delay), so a wide row at the dispersion-consistent time
    // matches by chance. Its S/N 6.5 cannot belong to a flat event that
    // is 18.9 in a band 1/26 as wide, so the 18.9 row must survive.
    const float wide_dm      = 104.5F;
    const SizeType wide_samp = 7245;
    const auto wide =
        uwl(6.5F, wide_samp, wide_dm, 704.0F, 4032.0F, 0, 6655, 32);
    const double shift =
        subband_time_shift_samples(wide_dm, 704.0F, 3776.0F, kUwlTsamp);
    REQUIRE(shift > 1000.0);
    const auto narrow =
        uwl(18.9F, wide_samp - static_cast<SizeType>(std::llround(shift)),
            200.0F, 3776.0F, 4032.0F, 0, 511, 32);
    REQUIRE(subband_dm_step(3776.0F, 4032.0F, kUwlTsamp) > 10.0);

    SubbandDedupPlan plan;
    const auto merged =
        plan.execute(std::vector<Giant>{wide, narrow}, kUwlTol, kUwlTsamp);
    REQUIRE(merged.size() == 2);
    bool found = false;
    for (const auto& g : merged) {
        found = found || (g.snr == 18.9F && g.chan_end == 511);
    }
    CHECK(found);

    // The same pair with a consistent S/N is one event.
    const auto strong_wide =
        uwl(70.0F, wide_samp, wide_dm, 704.0F, 4032.0F, 0, 6655, 32);
    const auto weak_narrow =
        uwl(18.9F, narrow.samp_idx, 200.0F, 3776.0F, 4032.0F, 0, 511, 32);
    const auto one = plan.execute(std::vector<Giant>{strong_wide, weak_narrow},
                                  kUwlTol, kUwlTsamp);
    REQUIRE(one.size() == 1);
    CHECK(one[0].chan_end == 6655);
    CHECK(one[0].snr == 70.0F);
    CHECK(one[0].n_extents == 2);
}

TEST_CASE("Subband dedup merges rows whose DMs differ by one pass-1 scatter",
          "[clustering][subband]") {
    // DM 119.29 vs 120.06 on a 704-2752 MHz band is 0.77 DM, within the
    // physical smearing gate.
    const auto wide = uwl(27.9F, 5000, 120.06F, 704.0F, 4032.0F, 0, 6655, 8);
    const auto narrow =
        uwl(26.4F, 5000, 119.29F, 704.0F, 2752.0F, 2560, 6655, 8);
    REQUIRE(std::abs(wide.dm - narrow.dm) > 0.7F);

    SubbandDedupPlan plan;
    const auto merged =
        plan.execute(std::vector<Giant>{narrow, wide}, kUwlTol, kUwlTsamp);
    REQUIRE(merged.size() == 1);
    CHECK(merged[0].chan_end == 6655);

    // With dm_smear == 0 the narrower 1-boxcar tolerance applies and the pair
    // stays split.
    ClusterTolerances unscaled = kUwlTol;
    unscaled.dm_smear          = 0.0;
    CHECK(plan.execute(std::vector<Giant>{narrow, wide}, unscaled, kUwlTsamp)
              .size() == 2);
}

TEST_CASE("Subband dedup reports the strongest member and extent count",
          "[clustering][subband]") {
    // C-like: 83 in the 704-1728 band, 54 in the full band.
    const auto wide = uwl(54.5F, 5000, 120.0F, 704.0F, 4032.0F, 0, 6655, 8);
    const auto low  = uwl(82.9F, 5000, 120.1F, 704.0F, 1728.0F, 4608, 6655, 8);
    SubbandDedupPlan plan;
    for (const auto& order :
         {std::vector<Giant>{wide, low}, std::vector<Giant>{low, wide}}) {
        const auto merged = plan.execute(order, kUwlTol, kUwlTsamp);
        REQUIRE(merged.size() == 1);
        CHECK(merged[0].chan_end == 6655);
        CHECK(merged[0].snr == 54.5F);
        CHECK(merged[0].best_snr() == 82.9F);
        CHECK(merged[0].has_best_extent());
        CHECK(merged[0].best_chan_begin == 4608);
        CHECK(merged[0].best_chan_end == 6655);
        CHECK(merged[0].n_extents == 2);
        const auto location = merged[0].strongest_location();
        CHECK(location.samp_idx == low.samp_idx);
        CHECK(location.dm == low.dm);
        CHECK(location.filter_width == frost::search::giant_boxcar_width(low));
        CHECK(location.f_low == low.f_low);
        CHECK(location.f_high == low.f_high);
        // A subsequent association must keep the original strongest peak,
        // rather than pairing its S/N with the widest representative's DM.
        const auto twice = plan.execute(merged, kUwlTol, kUwlTsamp);
        REQUIRE(twice.size() == 1);
        CHECK(twice[0].strongest_location().dm == low.dm);
    }
}

TEST_CASE("Subband dedup merges one node searched as two planes",
          "[clustering][subband]") {
    // An unpaired FDMT child is copied up a level, so two planes
    // report the same extent. Within one plane equal extents stay apart.
    auto a  = uwl(12.0F, 5000, 50.0F, 704.0F, 1728.0F, 4608, 6655, 8);
    auto b  = a;
    a.plane = 0;
    b.plane = 1;
    b.snr   = 11.0F;
    SubbandDedupPlan plan;
    const auto merged =
        plan.execute(std::vector<Giant>{a, b}, kUwlTol, kUwlTsamp);
    REQUIRE(merged.size() == 1);
    CHECK(merged[0].snr == 12.0F);
    CHECK(merged[0].n_extents == 1);

    b.plane = 0;
    CHECK(plan.execute(std::vector<Giant>{a, b}, kUwlTol, kUwlTsamp).size() ==
          2);

    // dm_idx is a trial on each plane's own grid: an equal index does not
    // make the DMs agree. 5 DM is far outside 3 x 8 samples of delay
    // (about 1.8 DM on 704-1728 MHz).
    b.plane  = 1;
    b.dm_idx = a.dm_idx;
    b.dm     = a.dm + 5.0F;
    CHECK(plan.execute(std::vector<Giant>{a, b}, kUwlTol, kUwlTsamp).size() ==
          2);
}
