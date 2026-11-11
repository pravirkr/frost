#include <algorithm>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include <frost/common/policies.hpp>
#include <frost/search/candidate.hpp>
#include <frost/search/clustering.hpp>

using frost::SizeType;
using frost::search::ClusterLabelPlan;
using frost::search::ClusterMode;
using frost::search::ClusterTolerances;
using frost::search::Giant;

namespace {
Giant make_giant(float snr,
                 SizeType samp_idx,
                 SizeType filter_idx,
                 SizeType dm_idx) {
    Giant g;
    g.snr        = snr;
    g.samp_idx   = samp_idx;
    g.filter_idx = filter_idx;
    g.dm_idx     = dm_idx;
    return g;
}
} // namespace

TEST_CASE("ClusterLabelPlan groups directly coincident candidates under "
          "one label",
          "[clustering]") {
    std::vector<Giant> cands = {
        make_giant(5.0F, 1000, 0, 10),
        make_giant(8.0F, 1002, 0, 11), // close in time/filter/dm to [0]
        make_giant(6.0F, 5000, 0, 10), // far away -> separate cluster
    };

    ClusterLabelPlan plan;
    const ClusterTolerances tol{/*time_tol=*/5, /*filter_tol=*/1, /*dm_tol=*/2};
    const auto labels = plan.execute(cands, tol);

    REQUIRE(labels.size() == 3);
    CHECK(labels[0] == labels[1]);
    CHECK(labels[0] != labels[2]);
}

TEST_CASE("ClusterLabelPlan transitively merges a chain of coincident "
          "candidates",
          "[clustering]") {
    // a-b coincident, b-c coincident, but a-c alone would not be within
    // tolerance -- clustering must still put all three in one group.
    std::vector<Giant> cands = {
        make_giant(5.0F, 1000, 0, 10),
        make_giant(6.0F, 1004, 0, 11),
        make_giant(7.0F, 1008, 0, 12),
    };
    ClusterLabelPlan plan;
    const ClusterTolerances tol{/*time_tol=*/4, /*filter_tol=*/1, /*dm_tol=*/1};
    const auto labels = plan.execute(cands, tol);

    REQUIRE(labels.size() == 3);
    CHECK(labels[0] == labels[1]);
    CHECK(labels[1] == labels[2]);
}

TEST_CASE("ClusterLabelPlan gives every candidate its own label when none "
          "are coincident",
          "[clustering]") {
    std::vector<Giant> cands = {
        make_giant(5.0F, 0, 0, 0),
        make_giant(5.0F, 10000, 0, 0),
        make_giant(5.0F, 20000, 0, 0),
    };
    ClusterLabelPlan plan;
    const ClusterTolerances tol{2, 1, 1};
    const auto labels = plan.execute(cands, tol);

    CHECK(labels[0] != labels[1]);
    CHECK(labels[0] != labels[2]);
    CHECK(labels[1] != labels[2]);
}

TEST_CASE("ClusterLabelPlan keeps different channel extents apart",
          "[clustering]") {
    auto full       = make_giant(8.0F, 1000, 0, 10);
    auto sub        = make_giant(7.0F, 1001, 0, 10);
    full.chan_begin = 0;
    full.chan_end   = 15;
    sub.chan_begin  = 0;
    sub.chan_end    = 3;

    ClusterLabelPlan plan;
    const ClusterTolerances tol{/*time_tol=*/5, /*filter_tol=*/1, /*dm_tol=*/2};
    const auto labels = plan.execute(std::vector<Giant>{full, sub}, tol);
    REQUIRE(labels.size() == 2);
    CHECK(labels[0] != labels[1]);
}

TEST_CASE("ClusterLabelPlan handles an empty candidate list", "[clustering]") {
    ClusterLabelPlan plan;
    const auto labels = plan.execute({}, ClusterTolerances{1, 1, 1});
    CHECK(labels.empty());
}

namespace {

// J0540-like plane: 704-4032 MHz at 512 us on 338 DM trials over 0-250, so
// a trial is 0.74 DM while one sample of band delay is 0.063 DM.
Giant uwl_giant(float snr, SizeType samp_idx, SizeType dm_idx) {
    Giant g;
    g.snr          = snr;
    g.samp_idx     = samp_idx;
    g.filter_idx   = 5;
    g.filter_width = 32;
    g.dm_idx       = dm_idx;
    g.dm           = static_cast<float>(dm_idx) * 250.0F / 337.0F;
    g.f_low        = 704.0F;
    g.f_high       = 4032.0F;
    g.chan_begin   = 0;
    g.chan_end     = 6655;
    return g;
}

} // namespace

TEST_CASE("ClusterLabelPlan does not let a long DM chain swallow a distant "
          "strong pulse",
          "[clustering][robust]") {
    // The default cand_sep_dm of 200 trials spans most of the 338-trial
    // list. 150 weak width-32 giants at adjacent trials chain together;
    // a strong pulse 150 trials away (110 DM) is a different event.
    std::vector<Giant> cands;
    for (SizeType d = 0; d < 150; ++d) {
        cands.push_back(uwl_giant(6.5F, 5000 + (d % 5), d));
    }
    cands.push_back(uwl_giant(30.0F, 5000, 300));

    ClusterLabelPlan plan;
    ClusterTolerances physical{3, 3, 200, /*dm_smear=*/3.0, 512.0e-6};
    const auto labels = plan.execute(cands, physical);
    for (SizeType i = 1; i < 150; ++i) {
        CHECK(labels[i] == labels[0]); // the weak chain is still one cluster
    }
    CHECK(labels[150] != labels[0]);

    // The trial-count-only tolerance swallows it.
    ClusterTolerances trials_only{3, 3, 200};
    const auto by_trials = plan.execute(cands, trials_only);
    CHECK(by_trials[150] == by_trials[0]);
}

TEST_CASE("Physical DM tolerance keeps neighbours and drops distant trials",
          "[clustering]") {
    ClusterTolerances tol{3, 3, 200, 3.0, 512.0e-6};
    const auto base = uwl_giant(10.0F, 5000, 100);
    // 3 x 32 x 0.063 = 6.05 DM, about 8 trials.
    CHECK(frost::search::are_coincident(base, uwl_giant(9.0F, 5000, 101), tol));
    CHECK(frost::search::are_coincident(base, uwl_giant(9.0F, 5000, 106), tol));
    CHECK_FALSE(
        frost::search::are_coincident(base, uwl_giant(9.0F, 5000, 130), tol));
    // Adjacent trials stay neighbours even for a 1-sample boxcar whose
    // physical tolerance is smaller than the trial spacing.
    auto narrow_a         = uwl_giant(10.0F, 5000, 100);
    auto narrow_b         = uwl_giant(9.0F, 5000, 101);
    narrow_a.filter_width = narrow_b.filter_width = 1;
    narrow_a.filter_idx = narrow_b.filter_idx = 0;
    CHECK(frost::search::are_coincident(narrow_a, narrow_b, tol));
}

TEST_CASE("ClusterLabelPlan time-sorted sweep equals the pairwise partition",
          "[clustering][robust]") {
    // The sweep must give exactly the labels of the O(N^2) union-find.
    std::vector<Giant> cands;
    unsigned state  = 12345U;
    const auto next = [&state](unsigned mod) {
        state = (state * 1664525U) + 1013904223U;
        return (state >> 8U) % mod;
    };
    for (int i = 0; i < 1500; ++i) {
        Giant g        = uwl_giant(6.0F + static_cast<float>(next(400)) / 10.0F,
                                   next(20000), next(338));
        g.filter_idx   = next(6);
        g.filter_width = SizeType{1} << g.filter_idx;
        g.chan_end     = (next(3) == 0) ? 3327 : 6655;
        cands.push_back(g);
    }
    for (const auto& tol :
         {ClusterTolerances{3, 3, 200}, ClusterTolerances{3, 3, 10},
          ClusterTolerances{3, 3, 200, 8.0, 512.0e-6}}) {
        // Reference: the pairwise union-find.
        const auto n = cands.size();
        std::vector<SizeType> parent(n);
        for (SizeType i = 0; i < n; ++i) {
            parent[i] = i;
        }
        const auto root = [&](SizeType x) {
            while (parent[x] != x) {
                x = parent[x];
            }
            return x;
        };
        for (SizeType i = 0; i < n; ++i) {
            for (SizeType j = i + 1; j < n; ++j) {
                if (frost::search::are_coincident(cands[i], cands[j], tol)) {
                    const SizeType a = root(i);
                    const SizeType b = root(j);
                    if (a != b) {
                        parent[std::max(a, b)] = std::min(a, b);
                    }
                }
            }
        }
        const auto labels = ClusterLabelPlan{}.execute(cands, tol);
        for (SizeType i = 0; i < n; ++i) {
            REQUIRE(labels[i] == root(i));
        }
    }
}

TEST_CASE("Seeded clustering is bounded by the brightest member and does "
          "not chain",
          "[clustering]") {
    // Four giants at 0.7 DM spacing in time-DM space. Chained, they form one
    // cluster; seeded by S/N, the bright middle one claims only what lies
    // within its own tolerance (here two trials).
    std::vector<Giant> cands = {
        uwl_giant(8.0F, 5000, 100), uwl_giant(20.0F, 5000, 103),
        uwl_giant(7.0F, 5000, 106), uwl_giant(6.5F, 5000, 109)};
    for (auto& g : cands) {
        g.filter_idx   = 0;
        g.filter_width = 1;
    }
    ClusterTolerances tol{3, 3, 200, /*dm_smear=*/10.0, 512.0e-6};
    // 10 x 1 x 0.063 = 0.63 DM < one trial (0.74), so only adjacent trials
    // (none here) merge: every giant is alone.
    const auto none = ClusterLabelPlan{}.execute(cands, tol);
    CHECK(none[0] != none[1]);
    tol.dm_smear = 30.0; // 1.9 DM = 2.5 trials: reaches +-2 trials only
    const auto still =
        ClusterLabelPlan{ClusterMode::kChain}.execute(cands, tol);
    CHECK(still[0] != still[1]);
    // A wider reach links neighbours at 3 trials; chained, all four join,
    // seeded, the bright one claims only its two neighbours.
    tol.dm_smear = 40.0; // 2.5 DM = 3.4 trials
    const auto chain =
        ClusterLabelPlan{ClusterMode::kChain}.execute(cands, tol);
    CHECK(chain[0] == chain[3]);
    const auto seeded =
        ClusterLabelPlan{ClusterMode::kSeeded}.execute(cands, tol);
    CHECK(seeded[0] == 1);
    CHECK(seeded[1] == 1);
    CHECK(seeded[2] == 1);
    CHECK(seeded[3] == 3); // 6 trials from the seed: its own cluster
}
