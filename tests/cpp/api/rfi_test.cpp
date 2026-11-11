#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <limits>
#include <numeric>
#include <random>
#include <span>
#include <stdexcept>
#include <vector>

#ifdef _OPENMP
#include <omp.h>
#endif

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>
#include <psrio/packed.hpp>

#include <frost/common/numeric.hpp>
#include <frost/common/policies.hpp>
#include <frost/prepare/rfi.hpp>

using frost::DataType;
using frost::IndexType;
using frost::SizeType;
using frost::prepare::rfi::apply_manual_killmask;
using frost::prepare::rfi::BlockLayout;
using frost::prepare::rfi::ChannelRange;
using frost::prepare::rfi::FillMode;
using frost::prepare::rfi::iqrm_mask;
using frost::prepare::rfi::RFICleaner;
using frost::prepare::rfi::RfiConfig;
using frost::prepare::rfi::ZeroDMMode;

namespace {

constexpr float kTsamp = 1.0e-3F;

// A time x frequency block of float values, row-major (t * nchans + c).
struct Block {
    SizeType nsamps;
    SizeType nchans;
    std::vector<float> x;
    std::vector<float> base;  // per-channel level
    std::vector<float> sigma; // per-channel rms

    float& at(SizeType t, SizeType c) { return x[(t * nchans) + c]; }
    float at(SizeType t, SizeType c) const { return x[(t * nchans) + c]; }
};

// Gaussian noise on a smooth, non-flat bandpass (level and rms both vary
// across the band, as real data does).
Block make_noise(SizeType nsamps,
                 SizeType nchans,
                 unsigned seed,
                 float level = 100.0F,
                 float rms   = 5.0F) {
    Block b{nsamps, nchans, std::vector<float>(nsamps * nchans),
            std::vector<float>(nchans), std::vector<float>(nchans)};
    for (SizeType c = 0; c < nchans; ++c) {
        const float phase =
            3.0F * static_cast<float>(c) / static_cast<float>(nchans);
        b.base[c]  = level * (1.0F + (0.2F * std::sin(phase)));
        b.sigma[c] = rms * (1.0F + (0.2F * std::cos(phase)));
    }
    std::mt19937 rng(seed);
    std::normal_distribution<float> noise(0.0F, 1.0F);
    for (SizeType t = 0; t < nsamps; ++t) {
        for (SizeType c = 0; c < nchans; ++c) {
            b.at(t, c) = b.base[c] + (b.sigma[c] * noise(rng));
        }
    }
    return b;
}

std::vector<uint8_t> pack(const Block& b, DataType dtype) {
    const SizeType row_bytes =
        b.nchans * static_cast<SizeType>(frost::bits_of(dtype)) / 8;
    std::vector<uint8_t> out(b.nsamps * row_bytes, 0);
    const auto bits       = static_cast<SizeType>(frost::bits_of(dtype));
    const float max_value = bits == 32
                                ? std::nextafter(4294967296.0F, 0.0F)
                                : static_cast<float>((uint64_t{1} << bits) - 1);
    for (SizeType t = 0; t < b.nsamps; ++t) {
        auto* row = out.data() + t * row_bytes;
        for (SizeType c = 0; c < b.nchans; ++c) {
            const float value = b.at(t, c);
            if (dtype == DataType::kFloat32) {
                std::memcpy(row + c * sizeof(float), &value, sizeof(float));
            } else {
                const auto raw = static_cast<uint32_t>(
                    std::clamp(std::nearbyint(value), 0.0F, max_value));
                if (bits < 8) {
                    row[c * bits / 8] |=
                        static_cast<uint8_t>(raw << (c * bits % 8));
                } else {
                    std::memcpy(row + c * bits / 8, &raw, bits / 8);
                }
            }
        }
    }
    return out;
}

Block unpack(const std::vector<uint8_t>& data,
             SizeType nchans,
             DataType dtype) {
    const SizeType row_bytes =
        nchans * static_cast<SizeType>(frost::bits_of(dtype)) / 8;
    const SizeType nsamps = data.size() / row_bytes;
    Block b{nsamps, nchans, std::vector<float>(nsamps * nchans), {}, {}};
    const auto bytes = std::as_bytes(std::span(data));
    const psrio::DataExtractor extractor(bytes, nsamps, nchans,
                                         frost::to_sample_type(dtype));
    for (SizeType t = 0; t < nsamps; ++t) {
        for (SizeType c = 0; c < nchans; ++c) {
            b.at(t, c) = extractor.extract_sample(t, c);
        }
    }
    return b;
}

BlockLayout layout_for(const Block& b, DataType dtype) {
    BlockLayout l;
    l.nchans     = b.nchans;
    l.max_nsamps = b.nsamps;
    l.dtype      = dtype;
    l.tsamp      = kTsamp;
    l.f0         = 1500.0;
    l.df         = -1.0;
    return l;
}

// Every strategy off: a starting point for testing one at a time.
RfiConfig quiet() {
    RfiConfig cfg;
    cfg.narrow.enabled = false;
    cfg.broad.enabled  = false;
    return cfg;
}

struct Result {
    Block out;
    std::vector<uint8_t> packed;
    frost::prepare::rfi::RfiReport report;
    std::vector<uint8_t> mask;
};

Result clean(const Block& in,
             const RfiConfig& cfg,
             DataType dtype        = DataType::kUInt8,
             float baseline_length = 1.0F) {
    auto data = pack(in, dtype);
    RFICleaner cleaner(cfg, layout_for(in, dtype), baseline_length);
    cleaner.execute(data);
    const auto mask = cleaner.channel_mask();
    return {unpack(data, in.nchans, dtype), data, cleaner.report(),
            std::vector<uint8_t>(mask.begin(), mask.end())};
}

double channel_mean(const Block& b, SizeType c, SizeType t0, SizeType t1) {
    double s = 0.0;
    for (SizeType t = t0; t < t1; ++t) {
        s += b.at(t, c);
    }
    return s / static_cast<double>(t1 - t0);
}

double channel_std(const Block& b, SizeType c, SizeType t0, SizeType t1) {
    const double m = channel_mean(b, c, t0, t1);
    double s       = 0.0;
    for (SizeType t = t0; t < t1; ++t) {
        s += (b.at(t, c) - m) * (b.at(t, c) - m);
    }
    return std::sqrt(s / static_cast<double>(t1 - t0));
}

// Regression coefficient of y on x.
double slope(const std::vector<double>& y, const std::vector<double>& x) {
    const auto n    = static_cast<double>(x.size());
    const double mx = std::accumulate(x.begin(), x.end(), 0.0) / n;
    const double my = std::accumulate(y.begin(), y.end(), 0.0) / n;
    double sxy      = 0.0;
    double sxx      = 0.0;
    for (SizeType i = 0; i < x.size(); ++i) {
        sxy += (x[i] - mx) * (y[i] - my);
        sxx += (x[i] - mx) * (x[i] - mx);
    }
    return sxy / sxx;
}

} // namespace

TEST_CASE("apply_manual_killmask zeros channels within given ranges", "[rfi]") {
    std::vector<uint8_t> killmask(16, 1U);
    const std::vector<ChannelRange> zaps = {{2, 4}, {10, 10}};
    apply_manual_killmask(killmask, zaps);
    const std::vector<uint8_t> expected = {1, 1, 0, 0, 0, 1, 1, 1,
                                           1, 1, 0, 1, 1, 1, 1, 1};
    CHECK(killmask == expected);
}

TEST_CASE("apply_manual_killmask clamps an out-of-range end index", "[rfi]") {
    std::vector<uint8_t> killmask(4, 1U);
    const std::vector<ChannelRange> zaps = {{2, 100}, {50, 60}};
    apply_manual_killmask(killmask, zaps);
    CHECK(killmask == std::vector<uint8_t>{1, 1, 0, 0});
}

TEST_CASE("iqrm_mask matches Morello et al. (2022) on synthetic channels",
          "[rfi][iqrm]") {
    // Seeded standard-normal draws (rounded to 1/256) with injected
    // outliers at 10, 11, 40, 50..53 and 63; expected masks follow
    // Morello et al. (2022, MNRAS 511, 1116).
    const std::vector<float> x = {
        0.12500000F,  -0.13281250F, 0.64062500F,  0.10546875F,  -0.53515625F,
        0.36328125F,  1.30468750F,  0.94531250F,  -0.70312500F, -1.26562500F,
        7.37500000F,  5.04296875F,  -2.32421875F, -0.21875000F, -1.24609375F,
        -0.73046875F, -0.54296875F, -0.31640625F, 0.41015625F,  1.04296875F,
        -0.12890625F, 1.36718750F,  -0.66406250F, 0.35156250F,  0.90234375F,
        0.09375000F,  -0.74218750F, -0.92187500F, -0.45703125F, 0.21875000F,
        -1.00781250F, -0.21093750F, -0.16015625F, 0.53906250F,  0.21484375F,
        0.35546875F,  -0.65234375F, -0.12890625F, 0.78515625F,  1.49218750F,
        18.74218750F, 1.51562500F,  1.34765625F,  0.78125000F,  0.26562500F,
        -0.31250000F, 1.45703125F,  1.96093750F,  1.80078125F,  1.31640625F,
        6.35546875F,  4.79296875F,  5.99609375F,  6.65625000F,  -1.28906250F,
        0.39453125F,  0.42968750F,  0.69531250F,  -1.18359375F, -0.66015625F,
        -0.43750000F, -1.16796875F, 1.73828125F,  8.50390625F};
    const auto flagged = [](const std::vector<uint8_t>& m) {
        std::vector<SizeType> idx;
        for (SizeType i = 0; i < m.size(); ++i) {
            if (m[i] != 0) {
                idx.push_back(i);
            }
        }
        return idx;
    };
    const auto ignore_of = [&](std::initializer_list<SizeType> chans) {
        std::vector<uint8_t> ig(x.size(), 0U);
        for (const auto c : chans) {
            ig[c] = 1U;
        }
        return ig;
    };

    CHECK(flagged(iqrm_mask(x, 6, 3.0F)) ==
          std::vector<SizeType>{10, 11, 40, 50, 51, 52, 53, 63});
    // Ignoring channel 40 removes its vote for 51.
    CHECK(flagged(iqrm_mask(x, 6, 3.0F, ignore_of({0, 1, 40}))) ==
          std::vector<SizeType>{0, 1, 10, 11, 40, 50, 52, 53, 63});
    CHECK(flagged(iqrm_mask(x, 1, 2.0F)) == std::vector<SizeType>{10, 40});
    CHECK(flagged(iqrm_mask(x, 3, 1.5F, ignore_of({5, 6, 7}))) ==
          std::vector<SizeType>{5, 6, 7, 10, 11, 40, 50, 51, 52, 53, 63});

    SECTION("pure noise is not flagged") {
        std::mt19937 rng(3);
        std::normal_distribution<float> noise;
        std::vector<float> y(1024);
        for (auto& v : y) {
            v = noise(rng);
        }
        for (const SizeType radius : {1U, 2U, 5U, 100U}) {
            CHECK(flagged(iqrm_mask(y, radius, 4.0F)).empty());
        }
    }
    SECTION("a top-hat is masked once the largest lag exceeds its width") {
        std::vector<float> y(256, 0.0F);
        std::mt19937 rng(5);
        std::normal_distribution<float> noise;
        for (auto& v : y) {
            v = noise(rng);
        }
        for (SizeType c = 100; c < 104; ++c) {
            y[c] += 50.0F;
        }
        CHECK(flagged(iqrm_mask(y, 6, 3.0F)) ==
              std::vector<SizeType>{100, 101, 102, 103});
    }
    CHECK_THROWS_AS(iqrm_mask(x, 0, 3.0F), std::invalid_argument);
    CHECK_THROWS_AS(iqrm_mask(x, 3, 0.0F), std::invalid_argument);
}

TEST_CASE("RFICleaner with every strategy off leaves the block bit-exact",
          "[rfi]") {
    for (const auto dtype : {DataType::kUInt2, DataType::kUInt8,
                             DataType::kUInt16, DataType::kFloat32}) {
        const auto in     = dtype == DataType::kUInt2
                                ? make_noise(3000, 32, 2, 1.5F, 0.8F)
                                : make_noise(3000, 32, 2);
        const auto packed = pack(in, dtype);
        const auto r      = clean(in, quiet(), dtype);
        CHECK(r.packed == packed);
        CHECK(r.report.chans_masked == 0);
        CHECK(r.report.samps_flagged == 0);
    }
}

TEST_CASE("RFICleaner defaults have a Gaussian false-positive "
          "rate on 8-bit noise",
          "[rfi]") {
    // At 5 sigma, 10 blocks of 8192 x 64 expect ~0.05 flagged time samples
    // and ~3 flagged cells. A quantised-MAD rms estimate would give ~100x
    // more.
    SizeType samps   = 0;
    SizeType cells   = 0;
    SizeType changed = 0;
    for (unsigned seed = 100; seed < 110; ++seed) {
        const auto in     = make_noise(8192, 64, seed);
        const auto packed = pack(in, DataType::kUInt8);
        const auto r      = clean(in, RfiConfig{});
        samps += r.report.samps_flagged;
        cells += r.report.cells_narrow;
        for (SizeType i = 0; i < packed.size(); ++i) {
            changed += packed[i] != r.packed[i] ? 1 : 0;
        }
    }
    CHECK(samps <= 1);
    CHECK(cells <= 12);
    CHECK(changed <= cells + (samps * 64));
}

TEST_CASE("Narrow-band excision replaces intermittent spikes from "
          "neighbouring channels and leaves every other sample bit-exact",
          "[rfi]") {
    auto in                 = make_noise(4096, 64, 7);
    constexpr SizeType kBad = 20;
    std::mt19937 rng(8);
    std::bernoulli_distribution hit(0.15);
    std::vector<uint8_t> spiked(in.nsamps, 0U);
    SizeType nspikes = 0;
    for (SizeType t = 0; t < in.nsamps; ++t) {
        if (hit(rng)) {
            in.at(t, kBad) = 255.0F;
            spiked[t]      = 1U;
            ++nspikes;
        }
    }
    auto cfg           = quiet();
    cfg.narrow.enabled = true;
    const auto packed  = pack(in, DataType::kUInt8);
    const auto r       = clean(in, cfg);

    CHECK(r.report.cells_narrow >= nspikes);
    CHECK(r.report.cells_narrow <= nspikes + 2);
    CHECK(r.report.fill_fallbacks == 0);
    for (SizeType t = 0; t < in.nsamps; ++t) {
        for (SizeType c = 0; c < in.nchans; ++c) {
            const auto i = (t * in.nchans) + c;
            if (c == kBad && spiked[t] != 0) {
                // Rescaled into this channel from a clean neighbour.
                CHECK(std::abs(r.out.at(t, c) - in.base[c]) <
                      5.0F * in.sigma[c]);
            } else if (std::abs(in.at(t, c) - in.base[c]) <
                       4.0F * in.sigma[c]) {
                REQUIRE(r.packed[i] == packed[i]);
            }
        }
    }

    SECTION("skipped for 4-bit data") {
        auto small = make_noise(2048, 16, 4, 7.0F, 1.5F);
        for (SizeType t = 0; t < small.nsamps; t += 7) {
            small.at(t, 5) = 15.0F;
        }
        const auto r4 = clean(small, cfg, DataType::kUInt4);
        CHECK(r4.report.cells_narrow == 0);
        CHECK(r4.packed == pack(small, DataType::kUInt4));
    }
}

TEST_CASE("Broad-band excision replaces a wideband burst with nearby "
          "unflagged samples of the same channel",
          "[rfi]") {
    auto in = make_noise(4096, 64, 11);
    for (SizeType t = 2000; t < 2004; ++t) {
        for (SizeType c = 0; c < in.nchans; ++c) {
            in.at(t, c) += 8.0F * in.sigma[c];
        }
    }
    // A dispersed 3-sigma pulse, well below both thresholds.
    std::vector<std::pair<SizeType, SizeType>> pulse;
    for (SizeType c = 0; c < in.nchans; ++c) {
        for (SizeType k = 0; k < 4; ++k) {
            const SizeType t = 1000 + (c * 5) + k;
            in.at(t, c) += 3.0F * in.sigma[c];
            pulse.emplace_back(t, c);
        }
    }
    auto cfg          = quiet();
    cfg.broad.enabled = true;
    const auto r      = clean(in, cfg, DataType::kFloat32);

    CHECK(r.report.samps_broad >= 4);
    CHECK(r.report.samps_broad <= 6);
    // Time-fill radius: baseline_length / (8 tsamp).
    constexpr SizeType kRadius = 125;
    for (SizeType t = 2000; t < 2004; ++t) {
        for (SizeType c = 0; c < in.nchans; ++c) {
            bool from_neighbour = false;
            for (SizeType s = t - kRadius; s <= t + kRadius; ++s) {
                if ((s < 2000 || s >= 2004) && r.out.at(t, c) == in.at(s, c)) {
                    from_neighbour = true;
                    break;
                }
            }
            REQUIRE(from_neighbour);
        }
    }
    for (const auto& [t, c] : pulse) {
        REQUIRE(r.out.at(t, c) == in.at(t, c));
    }
}

TEST_CASE("Dropout detection flags blanked spectra and their neighbours",
          "[rfi]") {
    auto in = make_noise(4096, 64, 12);
    for (SizeType t = 3000; t < 3010; ++t) {
        for (SizeType c = 0; c < in.nchans; ++c) {
            in.at(t, c) = 0.0F;
        }
    }
    auto cfg            = quiet();
    cfg.dropout.enabled = true;
    const auto r        = clean(in, cfg);
    CHECK(r.report.samps_dropout == 12);
    for (SizeType t = 3000; t < 3010; ++t) {
        CHECK(channel_mean(r.out, 30, t, t + 1) > 50.0);
    }
    const auto untouched = clean(in, quiet());
    CHECK(untouched.out.at(3005, 30) == 0.0F);
}

TEST_CASE("Static zaps and dead channels are masked and filled with the "
          "local bandpass level",
          "[rfi]") {
    auto in = make_noise(4096, 64, 13);
    for (SizeType t = 0; t < in.nsamps; ++t) {
        in.at(t, 7) = 50.0F;
    }
    auto cfg      = quiet();
    cfg.zap_chans = {{2, 4}};
    // f = 1500 - c MHz: 48 and 49.
    cfg.zap_freqs = {{1450.5, 1452.5}};
    const auto r  = clean(in, cfg);

    CHECK(r.report.chans_zapped == 5);
    CHECK(r.report.chans_dead == 1);
    CHECK(r.report.chans_masked == 6);
    for (SizeType c = 0; c < in.nchans; ++c) {
        const bool masked = c == 7 || (c >= 2 && c <= 4) || c == 48 || c == 49;
        CHECK(r.mask[c] == (masked ? 0U : 1U));
    }
    // One constant per window (4 windows of 1000+ samples).
    for (const SizeType c : {3U, 48U}) {
        std::vector<float> vals;
        for (SizeType t = 0; t < in.nsamps; ++t) {
            vals.push_back(r.out.at(t, c));
        }
        std::sort(vals.begin(), vals.end());
        vals.erase(std::unique(vals.begin(), vals.end()), vals.end());
        CHECK(vals.size() <= 4);
        CHECK(std::abs(vals.front() - in.base[c]) < 2.0F);
    }
}

TEST_CASE("Moment outliers mask heavy-tailed and autocorrelated channels",
          "[rfi]") {
    auto in = make_noise(4096, 64, 14);
    std::mt19937 rng(15);
    std::bernoulli_distribution hit(0.01);
    std::normal_distribution<float> noise;
    float ar = 0.0F;
    for (SizeType t = 0; t < in.nsamps; ++t) {
        if (hit(rng)) {
            in.at(t, 10) += 6.0F * in.sigma[10];
        }
        ar           = (0.9F * ar) + (std::sqrt(1.0F - 0.81F) * noise(rng));
        in.at(t, 30) = in.base[30] + (in.sigma[30] * ar);
    }
    auto cfg            = quiet();
    cfg.moments.enabled = true;
    const auto r        = clean(in, cfg);
    CHECK(r.report.chans_moments == 2);
    CHECK(r.mask[10] == 0);
    CHECK(r.mask[30] == 0);

    SECTION("only the selected statistics are used") {
        cfg.moments.autocorr = false;
        const auto r2        = clean(in, cfg);
        CHECK(r2.mask[10] == 0);
        CHECK(r2.mask[30] == 1);
    }
    SECTION("absolute kurtosis limit") {
        cfg.moments.threshold    = 1000.0F;
        cfg.moments.kurtosis_max = 3.0F;
        const auto r2            = clean(in, cfg);
        CHECK(r2.report.chans_moments == 1);
        CHECK(r2.mask[10] == 0);
    }
}

TEST_CASE("IQRM masks a channel noisier than its neighbours, and mask_grow "
          "widens it",
          "[rfi][iqrm]") {
    auto in = make_noise(4096, 64, 16);
    for (SizeType t = 0; t < in.nsamps; ++t) {
        in.at(t, 20) = in.base[20] + (3.0F * (in.at(t, 20) - in.base[20]));
    }
    auto cfg         = quiet();
    cfg.iqrm.enabled = true;
    const auto r     = clean(in, cfg);
    CHECK(r.report.chans_iqrm == 1);
    CHECK(r.mask[20] == 0);
    CHECK(r.report.chans_masked == 1);

    cfg.mask_grow = 1;
    const auto g  = clean(in, cfg);
    CHECK(g.report.chans_grown == 2);
    CHECK(g.mask[19] == 0);
    CHECK(g.mask[21] == 0);
    CHECK(g.report.chans_masked == 3);
}

TEST_CASE("Kadane along time flags a weak long-lived step but not a short "
          "strong pulse",
          "[rfi][kadane]") {
    auto in = make_noise(4096, 64, 17);
    for (SizeType t = 1500; t < 1700; ++t) {
        in.at(t, 12) += in.sigma[12];
    }
    for (SizeType t = 3000; t < 3010; ++t) {
        in.at(t, 40) += 4.0F * in.sigma[40];
    }
    auto cfg                   = quiet();
    cfg.kadane_channel.enabled = true;
    const auto packed          = pack(in, DataType::kUInt8);
    const auto r = clean(in, cfg, DataType::kUInt8, /*baseline_length=*/4.0F);

    CHECK(r.report.cells_kadane_channel >= 180);
    CHECK(r.report.cells_kadane_channel <= 400);
    for (SizeType t = 0; t < in.nsamps; ++t) {
        for (SizeType c = 0; c < in.nchans; ++c) {
            const auto i = (t * in.nchans) + c;
            if (c != 12 || t < 1300 || t >= 1900) {
                REQUIRE(r.packed[i] == packed[i]);
            }
        }
    }
    const auto noise_only =
        clean(make_noise(4096, 64, 18), cfg, DataType::kUInt8, 4.0F);
    CHECK(noise_only.report.cells_kadane_channel == 0);
}

TEST_CASE("RFI mask growth clips huge radii to the available channel span",
          "[rfi][iqrm][contracts]") {
    auto in = make_noise(4096, 64, 16);
    for (SizeType t = 0; t < in.nsamps; ++t) {
        in.at(t, 20) = in.base[20] + (3.0F * (in.at(t, 20) - in.base[20]));
    }
    auto cfg            = quiet();
    cfg.iqrm.enabled    = true;
    constexpr auto kMax = std::numeric_limits<SizeType>::max();
    for (const SizeType radius : {SizeType{0}, SizeType{1}, SizeType{3},
                                  SizeType{64}, kMax / 2, kMax / 2 + 1, kMax}) {
        CAPTURE(radius);
        cfg.mask_grow     = radius;
        const auto result = clean(in, cfg);
        REQUIRE(result.report.chans_iqrm == 1);
        SizeType masked = 0;
        for (SizeType c = 0; c < in.nchans; ++c) {
            const auto distance = c > 20 ? c - 20 : 20 - c;
            const bool expected = distance <= radius;
            CHECK((result.mask[c] == 0) == expected);
            masked += static_cast<SizeType>(expected);
        }
        CHECK(result.report.chans_masked == masked);
        CHECK(result.report.chans_grown == masked - 1);
    }
    cfg.iqrm.enabled       = false;
    cfg.zap_chans          = {{20, 20}};
    cfg.mask_grow          = kMax;
    const auto manual_only = clean(in, cfg);
    CHECK(manual_only.report.chans_masked == 1);
    CHECK(manual_only.report.chans_grown == 0);
    for (SizeType c = 0; c < in.nchans; ++c) {
        CHECK((manual_only.mask[c] == 0) == (c == 20));
    }
}

TEST_CASE("Kadane across frequency flags wide partial-band bursts and "
          "whole-band spectra",
          "[rfi][kadane]") {
    auto in = make_noise(4096, 64, 19);
    for (SizeType c = 20; c < 40; ++c) { // 20 MHz
        in.at(1000, c) += 2.0F * in.sigma[c];
    }
    for (SizeType c = 0; c < 60; ++c) { // 94% of the band
        in.at(2000, c) -= 2.0F * in.sigma[c];
    }
    for (SizeType c = 5; c < 8; ++c) { // 3 MHz: too narrow
        in.at(3000, c) += 6.0F * in.sigma[c];
    }
    auto cfg                    = quiet();
    cfg.kadane_spectrum.enabled = true;
    const auto r                = clean(in, cfg);

    CHECK(r.report.samps_kadane == 1);
    CHECK(r.report.samps_flagged == 1);
    CHECK(r.report.cells_kadane_spectrum >= 18);
    CHECK(r.report.cells_kadane_spectrum <= 26);
    for (SizeType c = 5; c < 8; ++c) {
        CHECK(r.out.at(3000, c) == std::nearbyint(in.at(3000, c)));
    }
    CHECK(clean(make_noise(4096, 64, 20), cfg).report.cells_kadane_spectrum ==
          0);
}

TEST_CASE("Zero-DM subtraction removes a common-mode signal; zdot also "
          "removes it when channel gains differ",
          "[rfi][zero_dm]") {
    auto in = make_noise(4096, 64, 21);
    std::mt19937 rng(22);
    std::normal_distribution<float> noise;
    std::vector<double> common(in.nsamps);
    for (SizeType t = 0; t < in.nsamps; ++t) {
        common[t] = noise(rng);
        for (SizeType c = 0; c < in.nchans; ++c) {
            const float gain = 0.5F + (static_cast<float>(c) /
                                       static_cast<float>(in.nchans - 1));
            in.at(t, c) += in.sigma[c] * gain * static_cast<float>(common[t]);
        }
    }
    // Mean |regression coefficient| of each channel's (standardised)
    // output on the common signal.
    const auto residual = [&](const Block& out) {
        double sum = 0.0;
        for (SizeType c = 0; c < in.nchans; ++c) {
            std::vector<double> y(in.nsamps);
            const double sd = channel_std(out, c, 0, in.nsamps);
            for (SizeType t = 0; t < in.nsamps; ++t) {
                y[t] = out.at(t, c) / sd;
            }
            sum += std::abs(slope(y, common));
        }
        return sum / static_cast<double>(in.nchans);
    };
    auto cfg          = quiet();
    const auto before = residual(clean(in, cfg, DataType::kFloat32).out);
    cfg.zero_dm       = ZeroDMMode::kSubtract;
    const auto sub    = residual(clean(in, cfg, DataType::kFloat32).out);
    cfg.zero_dm       = ZeroDMMode::kZdot;
    const auto zdot   = residual(clean(in, cfg, DataType::kFloat32).out);

    CHECK(before > 0.6);
    CHECK(sub < 0.2);
    CHECK(sub > 0.05);
    CHECK(zdot < 0.03);
}

TEST_CASE("Equalize brings every channel to a common level and rms", "[rfi]") {
    const auto in = make_noise(4096, 64, 23, 100.0F, 6.0F);
    auto cfg      = quiet();
    cfg.equalize  = true;
    const auto u8 = clean(in, cfg);
    const auto f  = clean(in, cfg, DataType::kFloat32);
    for (SizeType c = 0; c < in.nchans; ++c) {
        CHECK(std::abs(channel_mean(u8.out, c, 0, in.nsamps) - 128.0) < 1.0);
        CHECK(std::abs(channel_std(u8.out, c, 0, in.nsamps) - 16.0) < 1.5);
        CHECK(std::abs(channel_mean(f.out, c, 0, in.nsamps)) < 0.06);
        CHECK(std::abs(channel_std(f.out, c, 0, in.nsamps) - 1.0) < 0.06);
    }
}

TEST_CASE("With zero-DM on, refilled spectra cancel across the band like "
          "kept ones",
          "[rfi][zero_dm]") {
    const auto mode = GENERATE(ZeroDMMode::kSubtract, ZeroDMMode::kZdot);
    const bool eq   = GENERATE(false, true);
    auto in         = make_noise(4096, 64, 31);
    for (SizeType t = 2000; t < 2004; ++t) {
        for (SizeType c = 0; c < in.nchans; ++c) {
            in.at(t, c) += 8.0F * in.sigma[c];
        }
    }
    auto cfg          = quiet();
    cfg.broad.enabled = true;
    cfg.zero_dm       = mode;
    cfg.equalize      = eq;
    const auto r      = clean(in, cfg);
    REQUIRE(r.report.samps_broad >= 4);

    // The zero-DM series of the output is now only quantisation noise; the
    // refilled spectra must not stand out of it.
    std::vector<double> sums(in.nsamps, 0.0);
    for (SizeType t = 0; t < in.nsamps; ++t) {
        for (SizeType c = 0; c < in.nchans; ++c) {
            sums[t] += r.out.at(t, c);
        }
    }
    double mean  = 0.0;
    double var   = 0.0;
    SizeType cnt = 0;
    for (SizeType t = 0; t < in.nsamps; ++t) {
        if (t < 1990 || t >= 2014) {
            mean += sums[t];
            ++cnt;
        }
    }
    mean /= static_cast<double>(cnt);
    for (SizeType t = 0; t < in.nsamps; ++t) {
        if (t < 1990 || t >= 2014) {
            var += (sums[t] - mean) * (sums[t] - mean);
        }
    }
    const double sd = std::sqrt(var / static_cast<double>(cnt));
    for (SizeType t = 2000; t < 2004; ++t) {
        CHECK(std::abs(sums[t] - mean) < 4.0 * sd);
    }
}

TEST_CASE("Fill modes replace flagged samples with the local mean or with "
          "noise",
          "[rfi]") {
    auto in = make_noise(4096, 64, 24);
    for (SizeType t = 2100; t < 2150; ++t) {
        for (SizeType c = 0; c < in.nchans; ++c) {
            in.at(t, c) += 10.0F * in.sigma[c];
        }
    }
    auto cfg          = quiet();
    cfg.broad.enabled = true;
    cfg.fill          = FillMode::kMean;
    const auto mean   = clean(in, cfg, DataType::kFloat32);
    REQUIRE(mean.report.samps_broad >= 50);
    cfg.fill        = FillMode::kNoise;
    const auto rand = clean(in, cfg, DataType::kFloat32);
    for (const SizeType c : {5U, 33U}) {
        CHECK(channel_std(mean.out, c, 2100, 2150) == 0.0);
        CHECK(std::abs(channel_mean(mean.out, c, 2100, 2150) - in.base[c]) <
              1.0);
        CHECK(std::abs(channel_mean(rand.out, c, 2100, 2150) - in.base[c]) <
              0.6 * in.sigma[c]);
        CHECK(std::abs(channel_std(rand.out, c, 2100, 2150) - in.sigma[c]) <
              0.3 * in.sigma[c]);
    }
}

TEST_CASE("RFICleaner is deterministic: independent of thread count and of "
          "in-place vs out-of-place",
          "[rfi]") {
    auto in = make_noise(6000, 64, 25);
    std::mt19937 rng(26);
    std::bernoulli_distribution hit(0.05);
    for (SizeType t = 0; t < in.nsamps; ++t) {
        if (hit(rng)) {
            in.at(t, 9) = 250.0F;
        }
    }
    for (SizeType t = 4000; t < 4010; ++t) {
        for (SizeType c = 0; c < in.nchans; ++c) {
            in.at(t, c) = 240.0F;
        }
    }
    RfiConfig cfg;
    cfg.dropout.enabled         = true;
    cfg.moments.enabled         = true;
    cfg.iqrm.enabled            = true;
    cfg.kadane_channel.enabled  = true;
    cfg.kadane_spectrum.enabled = true;
    cfg.zero_dm                 = ZeroDMMode::kZdot;
    const auto data             = pack(in, DataType::kUInt8);
    const auto layout           = layout_for(in, DataType::kUInt8);

    const auto run = [&](int threads) {
#ifdef _OPENMP
        const int saved = omp_get_max_threads();
        omp_set_num_threads(threads);
#else
        (void)threads;
#endif
        auto copy = data;
        RFICleaner cleaner(cfg, layout, 1.0F);
        cleaner.execute(copy);
#ifdef _OPENMP
        omp_set_num_threads(saved);
#endif
        return copy;
    };
    const auto one  = run(1);
    const auto four = run(4);
    CHECK(one != data);
    CHECK(one == four);

    RFICleaner cleaner(cfg, layout, 1.0F);
    std::vector<uint8_t> out(data.size() + 100, 0);
    cleaner.execute(data, out);
    CHECK(std::equal(one.begin(), one.end(), out.begin()));

    cfg.seed = 7;
    CHECK(run(4) != one);
}

TEST_CASE("RFICleaner cleans every dtype and leaves unflagged samples of "
          "32-bit integers bit-exact",
          "[rfi]") {
    struct Case {
        DataType dtype;
        float level;
        float rms;
        float burst;
    };
    for (const auto& k : {Case{DataType::kUInt2, 1.5F, 0.8F, 3.0F},
                          Case{DataType::kUInt4, 7.5F, 2.0F, 15.0F},
                          Case{DataType::kUInt8, 100.0F, 5.0F, 255.0F},
                          Case{DataType::kUInt16, 30000.0F, 500.0F, 60000.0F},
                          Case{DataType::kUInt32, 1.0e8F, 1.0e5F, 2.0e8F},
                          Case{DataType::kFloat32, 0.0F, 1.0F, 20.0F}}) {
        auto in = make_noise(4096, 64, 27, k.level, k.rms);
        for (SizeType t = 1000; t < 1004; ++t) {
            for (SizeType c = 0; c < in.nchans; ++c) {
                in.at(t, c) = k.burst;
            }
        }
        const auto packed = pack(in, k.dtype);
        const auto r      = clean(in, RfiConfig{}, k.dtype);
        CHECK(r.report.samps_broad >= 4);
        const SizeType row_bytes =
            in.nchans * static_cast<SizeType>(frost::bits_of(k.dtype)) / 8;
        for (SizeType t = 0; t < 900; ++t) {
            bool same = true;
            for (SizeType b = 0; b < row_bytes; ++b) {
                same = same && packed[(t * row_bytes) + b] ==
                                   r.packed[(t * row_bytes) + b];
            }
            if (!same) {
                // Only narrow-band outliers may change an unflagged row.
                REQUIRE(r.report.cells_narrow > 0);
            }
        }
        if (frost::bits_of(k.dtype) <= 4) {
            CHECK(r.report.cells_narrow == 0);
        }
    }
}

TEST_CASE("RFICleaner accepts shorter blocks and rejects malformed input or "
          "config",
          "[rfi]") {
    const auto in = make_noise(2048, 32, 28);
    auto data     = pack(in, DataType::kUInt8);
    auto layout   = layout_for(in, DataType::kUInt8);
    RFICleaner cleaner(RfiConfig{}, layout, 1.0F);

    std::vector<uint8_t> half(data.begin(), data.begin() + (1024 * 32));
    CHECK_NOTHROW(cleaner.execute(half));
    CHECK(cleaner.report().nsamps == 1024);
    std::vector<uint8_t> tiny(data.begin(), data.begin() + (3 * 32));
    const auto tiny_copy = tiny;
    cleaner.execute(tiny);
    CHECK(tiny == tiny_copy);
    // A short tail has too few spectra for robust statistics. A channel that
    // is constant over 40 samples must not be declared dead, and the block
    // is left as it was.
    std::vector<uint8_t> tail(data.begin(), data.begin() + (40 * 32));
    for (SizeType t = 0; t < 40; ++t) {
        tail[(t * 32) + 7] = 100;
    }
    const auto tail_copy = tail;
    cleaner.execute(tail);
    CHECK(tail == tail_copy);
    CHECK(cleaner.report().chans_dead == 0);
    CHECK(std::ranges::all_of(cleaner.channel_mask(),
                              [](uint8_t m) { return m != 0; }));
    std::vector<uint8_t> partial(data.begin(), data.end() - 1);
    CHECK_THROWS_AS(cleaner.execute(partial), std::invalid_argument);
    std::vector<uint8_t> oversized(data.size() + 32, 100);
    CHECK_THROWS_AS(cleaner.execute(oversized), std::invalid_argument);
    std::vector<uint8_t> small_out(10);
    CHECK_THROWS_AS(cleaner.execute(data, small_out), std::invalid_argument);

    const auto rejects = [&](RfiConfig cfg, BlockLayout l, float bl = 1.0F) {
        CHECK_THROWS_AS(RFICleaner(std::move(cfg), l, bl),
                        std::invalid_argument);
    };
    rejects(RfiConfig{}, layout, 0.0F);
    RfiConfig bad;
    bad.narrow.threshold = 0.0F;
    rejects(bad, layout);
    bad                  = RfiConfig{};
    bad.broad.boxcar_max = 0;
    rejects(bad, layout);
    bad         = RfiConfig{};
    bad.zero_dm = ZeroDMMode::kSubtract;
    auto l4     = layout;
    l4.dtype    = DataType::kUInt4;
    rejects(bad, l4);
    bad           = RfiConfig{};
    bad.zap_freqs = {{1400.0, 1410.0}};
    auto no_df    = layout;
    no_df.df      = 0.0;
    rejects(bad, no_df);
    bad           = RfiConfig{};
    bad.zap_chans = {{5, 2}};
    rejects(bad, layout);
    auto odd   = layout;
    odd.nchans = 3;
    odd.dtype  = DataType::kUInt2;
    rejects(RfiConfig{}, odd);
}

TEST_CASE("normalized RFI calibrates channel noise and preserves input",
          "[rfi][normalized]") {
    auto b = make_noise(8192, 8, 947);
    // Independent offsets and gains must disappear before analytical search.
    for (SizeType t = 0; t < b.nsamps; ++t) {
        for (SizeType c = 0; c < b.nchans; ++c) {
            b.at(t, c) = 40.0F + 20.0F * static_cast<float>(c) +
                         (0.35F + 0.2F * static_cast<float>(c)) *
                             (b.at(t, c) - b.base[c]);
        }
    }
    const auto dtype    = GENERATE(DataType::kFloat32, DataType::kUInt8);
    auto bytes          = pack(b, dtype);
    const auto original = bytes;
    auto layout         = layout_for(b, dtype);
    layout.df           = GENERATE(-1.0, 1.0);
    RfiConfig cfg;
    cfg.narrow.enabled    = false;
    cfg.broad.enabled     = false;
    const SizeType killed = GENERATE(SizeType{0}, SizeType{2});
    if (killed)
        cfg.zap_chans = {{0, killed - 1}};
    RFICleaner cleaner(cfg, layout, 10.0F);
    const auto out = cleaner.execute_normalized(bytes);
    REQUIRE(out.size() == b.nsamps * b.nchans);
    CHECK(bytes == original);
    // Active channels are unit variance whatever is masked: dmt leaves
    // masked channels out of the noise model, so there is no global rescale.
    const double expected_variance = 1.0;
    for (SizeType c = 0; c < b.nchans; ++c) {
        const SizeType dst = layout.df < 0.0 ? b.nchans - 1 - c : c;
        double sum = 0.0, sumsq = 0.0;
        for (SizeType t = 0; t < b.nsamps; ++t) {
            const double x = out[dst * b.nsamps + t];
            sum += x;
            sumsq += x * x;
        }
        if (c < killed) {
            CHECK(sumsq == 0.0);
            continue;
        }
        CHECK(std::abs(sum / b.nsamps) < 0.10);
        CHECK(sumsq / b.nsamps ==
              Catch::Approx(expected_variance).epsilon(0.12));
        // The channel flip must preserve the exact time series order.
        const double delta = out[dst * b.nsamps + 1] - out[dst * b.nsamps];
        CHECK(delta * (b.at(1, c) - b.at(0, c)) >= 0.0);
    }
}

TEST_CASE("broadband RFI does not mistake a late noise majority for outliers",
          "[rfi][mad]") {
    Block block{8192, 8, std::vector<float>(8192 * 8), {}, {}};
    for (SizeType t = 0; t < block.nsamps; ++t) {
        const float amplitude = t < 3125 ? 1.0F : 8.0F;
        for (SizeType c = 0; c < block.nchans; ++c) {
            block.at(t, c) =
                100.0F + 2.0F * c +
                (1.0F + 0.2F * c) * (t % 2 ? -amplitude : amplitude);
        }
    }
    auto cfg            = quiet();
    cfg.broad.enabled   = true;
    auto data           = pack(block, DataType::kFloat32);
    const auto original = data;
    // The long baseline retains the zero-centered zero-DM series, isolating
    // the whole-gulp scale estimate from running-baseline effects.
    RFICleaner cleaner(cfg, layout_for(block, DataType::kFloat32), 100);
    cleaner.execute(data);
    CHECK(cleaner.report().samps_broad == 0);
    CHECK(data == original);
}

TEST_CASE(
    "RFI rejects raw non-finite float32 samples and ignores normalized padding",
    "[rfi][normalized][robust]") {
    constexpr SizeType kCapacity = 512;
    const SizeType valid         = GENERATE(SizeType{2}, SizeType{256});
    const std::uint32_t invalid =
        GENERATE(0x7F800000U, 0xFF800000U, 0x7FC00000U, 0xFFC00000U,
                 0x7F800001U, 0xFF800001U);
    const auto b      = make_noise(kCapacity, 8, 153);
    const auto packed = pack(b, DataType::kFloat32);
    // Deliberately unaligned input: validation must inspect bytes safely.
    std::vector<uint8_t> storage(packed.size() + 1);
    std::copy(packed.begin(), packed.end(), storage.begin() + 1);
    auto bytes = std::span<uint8_t>(storage).subspan(1);
    for (SizeType i = valid * b.nchans; i < kCapacity * b.nchans; ++i)
        std::memcpy(bytes.data() + i * sizeof(invalid), &invalid,
                    sizeof(invalid));
    RfiConfig cfg;
    cfg.narrow.enabled = cfg.broad.enabled = false;
    RFICleaner cleaner(cfg, layout_for(b, DataType::kFloat32), 1.0F);
    CHECK_NOTHROW(cleaner.execute_normalized(bytes, valid));
    const SizeType bad_cell = (valid - 1) * b.nchans + 3;
    std::memcpy(bytes.data() + bad_cell * sizeof(invalid), &invalid,
                sizeof(invalid));
    const auto original = storage;
    const auto expected = "RFI: non-finite float32 input at sample " +
                          std::to_string(valid - 1) + ", channel 3";
    CHECK_THROWS_WITH(cleaner.execute_normalized(bytes, valid), expected);
    CHECK_THROWS_WITH(
        cleaner.execute(bytes.first(valid * b.nchans * sizeof(float))),
        expected);
    CHECK(storage == original);
    std::memcpy(bytes.data() + bad_cell * sizeof(float),
                packed.data() + bad_cell * sizeof(float), sizeof(float));
    CHECK_NOTHROW(cleaner.execute_normalized(bytes, valid));
}

TEST_CASE("normalized RFI cold-start tails exclude padding from statistics",
          "[rfi][normalized]") {
    auto b      = make_noise(256, 8, 93);
    auto layout = layout_for(b, DataType::kFloat32);
    layout.df   = 1.0;
    RfiConfig cfg;
    cfg.narrow.enabled = false;
    cfg.broad.enabled  = false;
    RFICleaner cleaner(cfg, layout, 1.0F);
    // A two-row final block: alternating levels, exact mean and sigma.
    std::vector<float> tail(256 * 8, 9999.0F);
    for (SizeType c = 0; c < 8; ++c) {
        tail[c]     = 20.0F + c;
        tail[8 + c] = 24.0F + c;
    }
    std::vector<uint8_t> packed(tail.size() * sizeof(float));
    std::memcpy(packed.data(), tail.data(), packed.size());
    const auto out = cleaner.execute_normalized(packed, 2);
    CHECK_FALSE(cleaner.report().calibration_reused);
    for (SizeType c = 0; c < 8; ++c) {
        CHECK(out[c * 256] == -1.0F);
        CHECK(out[c * 256 + 1] == 1.0F);
        for (SizeType t = 2; t < 256; ++t)
            CHECK(out[c * 256 + t] == 0.0F);
    }
    CHECK_THROWS_AS(cleaner.execute_normalized(packed, 257),
                    std::invalid_argument);
}

TEST_CASE("normalized RFI warm tails preserve pulses using the last window",
          "[rfi][normalized]") {
    auto b = make_noise(8192, 16, 326);
    // A real change in gain between reliable windows must update the cache.
    for (SizeType t = b.nsamps / 2; t < b.nsamps; ++t) {
        for (SizeType c = 0; c < b.nchans; ++c) {
            b.at(t, c) = 50.0F + 2.0F * b.at(t, c);
        }
    }
    const auto dtype = GENERATE(DataType::kFloat32, DataType::kUInt16);
    auto data        = pack(b, dtype);
    auto layout      = layout_for(b, dtype);
    layout.df        = GENERATE(-1.0, 1.0);
    RFICleaner cleaner(quiet(), layout, 4.096F);
    cleaner.execute_normalized(data);
    for (const float amplitude : {0.0F, 3.0F, 7.0F}) {
        Block tail{37, b.nchans, std::vector<float>(37 * b.nchans), {}, {}};
        for (SizeType t = 0; t < tail.nsamps; ++t) {
            for (SizeType c = 0; c < b.nchans; ++c) {
                tail.at(t, c) =
                    50.0F + 2.0F * b.base[c] + amplitude * 2.0F * b.sigma[c];
            }
        }
        auto packed = pack(tail, dtype);
        packed.resize(b.nsamps * b.nchans * (frost::bits_of(dtype) / 8), 0xFF);
        const auto original = packed;
        const auto out      = cleaner.execute_normalized(packed, tail.nsamps);
        REQUIRE(cleaner.report().calibration_reused);
        CHECK(packed == original);
        for (SizeType c = 0; c < b.nchans; ++c) {
            CHECK(out[c * b.nsamps] == Catch::Approx(amplitude).margin(0.35));
            CHECK(out[c * b.nsamps + 36] == out[c * b.nsamps]);
            CHECK(out[c * b.nsamps + 37] == 0);
            CHECK(out[(c + 1) * b.nsamps - 1] == 0);
        }
    }
    // Calling the byte API breaks the normalized stream's calibration state.
    cleaner.execute(data);
    Block cold{2, b.nchans, std::vector<float>(2 * b.nchans, 42), {}, {}};
    auto packed           = pack(cold, dtype);
    const auto normalized = cleaner.execute_normalized(packed);
    CHECK_FALSE(cleaner.report().calibration_reused);
    for (const auto value : normalized)
        CHECK(value == 0);
}

TEST_CASE("normalized RFI warm tails retain zero-DM transforms and mask",
          "[rfi][normalized]") {
    auto b = make_noise(8192, 32, 327);
    for (SizeType t = 0; t < b.nsamps; ++t)
        b.at(t, 4) = b.base[4];
    auto cfg      = quiet();
    cfg.zero_dm   = GENERATE(ZeroDMMode::kSubtract, ZeroDMMode::kZdot);
    cfg.equalize  = GENERATE(false, true);
    cfg.zap_chans = {{0, 3}};
    auto data     = pack(b, DataType::kFloat32);
    auto layout   = layout_for(b, DataType::kFloat32);
    layout.df     = 1;
    RFICleaner cleaner(cfg, layout, 10);
    cleaner.execute_normalized(data);
    Block tail{37, b.nchans, std::vector<float>(37 * b.nchans), {}, {}};
    for (SizeType t = 0; t < tail.nsamps; ++t) {
        for (SizeType c = 0; c < b.nchans; ++c) {
            tail.at(t, c) = b.base[c] + 3.0F * b.sigma[c];
        }
    }
    data           = pack(tail, DataType::kFloat32);
    const auto out = cleaner.execute_normalized(data);
    REQUIRE(cleaner.report().calibration_reused);
    CHECK(cleaner.report().chans_masked == 5);
    double sum = 0;
    for (SizeType c = 0; c < b.nchans; ++c) {
        sum += out[c * tail.nsamps];
        if (c < 5) {
            CHECK(out[c * tail.nsamps] == 0);
        }
    }
    CHECK(std::abs(sum / std::sqrt(27.0)) < 1.0);
}

TEST_CASE(
    "normalized RFI preserves channel and time coordinates at the FDMT handoff",
    "[rfi][normalized]") {
    constexpr SizeType kChans = 8, kCapacity = 16, kValid = 8;
    Block b{kCapacity,
            kChans,
            std::vector<float>(kCapacity * kChans, 255.0F),
            {},
            {}};
    // Distinct impulse time in each channel exposes either a transpose or
    // frequency reversal error. Offsets and gains differ by channel.
    for (SizeType c = 0; c < kChans; ++c) {
        for (SizeType t = 0; t < kValid; ++t) {
            b.at(t, c) = 100.0F + 5.0F * static_cast<float>(c) +
                         (t == c ? 8.0F * static_cast<float>(c + 1) : 0.0F);
        }
    }
    const auto dtype = GENERATE(DataType::kFloat32, DataType::kUInt8);
    auto bytes       = pack(b, dtype);
    auto layout      = layout_for(b, dtype);
    layout.df        = GENERATE(-1.0, 1.0);
    RfiConfig cfg;
    cfg.narrow.enabled = cfg.broad.enabled = false;
    RFICleaner cleaner(cfg, layout, 1.0F);
    const auto out    = cleaner.execute_normalized(bytes, kValid);
    const float sigma = std::sqrt(7.0F);
    for (SizeType c = 0; c < kChans; ++c) {
        const SizeType dst = layout.df < 0.0 ? kChans - 1 - c : c;
        for (SizeType t = 0; t < kCapacity; ++t) {
            const float expected = t >= kValid ? 0.0F
                                   : t == c    ? sigma
                                               : -1.0F / sigma;
            CHECK(out[dst * kCapacity + t] ==
                  Catch::Approx(expected).margin(1.0e-6F));
        }
    }
}

TEST_CASE("normalized RFI outputs zeros when every channel is killed",
          "[rfi][normalized]") {
    auto b     = make_noise(256, 8, 73);
    auto bytes = pack(b, DataType::kUInt8);
    RfiConfig cfg;
    cfg.zap_chans = {{0, 7}};
    RFICleaner cleaner(cfg, layout_for(b, DataType::kUInt8), 1.0F);
    const auto out = cleaner.execute_normalized(bytes);
    CHECK(
        std::all_of(out.begin(), out.end(), [](float x) { return x == 0.0F; }));
}

TEST_CASE("normalized RFI recalibrates zero-DM and equalized output",
          "[rfi][normalized]") {
    auto b     = make_noise(8192, 16, 947);
    auto bytes = pack(b, DataType::kFloat32);
    RfiConfig cfg;
    cfg.narrow.enabled = cfg.broad.enabled = false;
    cfg.zero_dm  = GENERATE(ZeroDMMode::kSubtract, ZeroDMMode::kZdot);
    cfg.equalize = GENERATE(false, true);
    RFICleaner cleaner(cfg, layout_for(b, DataType::kFloat32), 10.0F);
    const auto out = cleaner.execute_normalized(bytes);
    for (SizeType c = 0; c < b.nchans; ++c) {
        double mean = 0.0, power = 0.0;
        for (SizeType t = 0; t < b.nsamps; ++t) {
            const double x = out[c * b.nsamps + t];
            mean += x;
            power += x * x;
        }
        CHECK(std::abs(mean / b.nsamps) < 1.0e-4);
        CHECK(power / b.nsamps == Catch::Approx(1.0).epsilon(1.0e-4));
    }
}

TEST_CASE(
    "RFI construction and memory planning reject unrepresentable geometry",
    "[rfi][contracts][memory]") {
    BlockLayout valid;
    valid.nchans       = 8;
    valid.max_nsamps   = 512;
    valid.dtype        = DataType::kUInt8;
    valid.tsamp        = 0.001;
    valid.f0           = 1400;
    valid.df           = -1;
    const auto rejects = [&](BlockLayout layout, float baseline = 1) {
        CHECK_THROWS_AS(
            RFICleaner::estimate_memory(RfiConfig{}, layout, baseline, 1),
            std::invalid_argument);
        CHECK_THROWS_AS(RFICleaner(RfiConfig{}, layout, baseline),
                        std::invalid_argument);
    };
    auto bad  = valid;
    bad.tsamp = 1.0e-30;
    rejects(bad); // finite, but baseline/tsamp cannot become a sample count
    bad.tsamp = std::ldexp(1.0, -std::numeric_limits<SizeType>::digits);
    rejects(bad); // exact upper boundary of the unsigned conversion
    rejects(valid, std::numeric_limits<float>::max());
    bad        = valid;
    bad.nchans = std::numeric_limits<SizeType>::max() / 32 + 1;
    bad.dtype  = DataType::kFloat32;
    rejects(bad);
    bad = valid;
    bad.max_nsamps =
        std::numeric_limits<SizeType>::max() / (8 * sizeof(float)) + 1;
    rejects(bad);
    auto long_samples  = valid;
    long_samples.tsamp = std::numeric_limits<double>::max();
    CHECK_NOTHROW(RFICleaner(RfiConfig{}, long_samples, 1));
    CHECK_NOTHROW(RFICleaner::estimate_memory(RfiConfig{}, long_samples, 1, 1));
}

TEST_CASE("RFI planning bounds persistent workspace and scales with geometry",
          "[rfi][memory]") {
    auto block        = make_noise(256, 16, 42);
    const auto layout = layout_for(block, DataType::kUInt8);
    RfiConfig cfg;
    cfg.moments.enabled = true;
    cfg.iqrm.enabled    = true;
    RFICleaner plan(cfg, layout, 0.1F);
    auto data = pack(block, DataType::kUInt8);
    plan.execute_normalized(data);
    const auto estimate = RFICleaner::estimate_memory(cfg, layout, 0.1F, 4);
    CHECK(plan.memory_usage() <= estimate);
    auto bigger = layout;
    bigger.max_nsamps *= 2;
    CHECK(RFICleaner::estimate_memory(cfg, bigger, 0.1F, 4) > estimate);
}
