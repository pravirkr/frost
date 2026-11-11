#include <array>
#include <limits>
#include <random>
#include <span>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include <frost/common/policies.hpp>
#include <frost/prepare/spectra.hpp>

using frost::DataType;
using frost::SizeType;
using frost::prepare::spectra::measure_bandpass;

TEST_CASE("measure_bandpass recovers a per-channel constant offset with "
          "zero RMS",
          "[spectra]") {
    constexpr SizeType kNChans = 8;
    constexpr SizeType kNSamps = 3125; // 5^5: exactly the internal sample count

    const std::vector<float> offsets = {10.0F, 20.0F, 30.0F, 40.0F,
                                        50.0F, 60.0F, 70.0F, 80.0F};
    std::vector<float> data(kNSamps * kNChans);
    for (SizeType t = 0; t < kNSamps; ++t) {
        for (SizeType c = 0; c < kNChans; ++c) {
            data[(t * kNChans) + c] = offsets[c];
        }
    }

    std::vector<float> bandpass(kNChans);
    std::vector<float> rms(kNChans);
    measure_bandpass(data, bandpass, rms, kNSamps, kNChans);

    for (SizeType c = 0; c < kNChans; ++c) {
        CHECK(bandpass[c] == offsets[c]);
        CHECK(rms[c] == 0.0F);
    }
}

TEST_CASE("measure_bandpass reports higher RMS for a noisier channel",
          "[spectra]") {
    constexpr SizeType kNChans = 2;
    constexpr SizeType kNSamps = 3125;
    constexpr float kOffset    = 100.0F;
    constexpr float kAmplitude = 15.0F;

    // A two-valued (binary +/-amplitude) noise pattern -- periodic or
    // random -- would make the median trivially collapse to a majority
    // value with more than half the samples matching it exactly, so the
    // MAD (a *median*-based estimator) would trivially collapse to zero
    // too. A continuous noise distribution avoids that degeneracy.
    std::mt19937 rng(42);
    std::normal_distribution<float> noise(0.0F, kAmplitude);
    std::vector<float> data(kNSamps * kNChans);
    for (SizeType t = 0; t < kNSamps; ++t) {
        data[(t * kNChans) + 0] = kOffset;              // quiet channel
        data[(t * kNChans) + 1] = kOffset + noise(rng); // noisy channel
    }

    std::vector<float> bandpass(kNChans);
    std::vector<float> rms(kNChans);
    measure_bandpass(data, bandpass, rms, kNSamps, kNChans);

    CHECK(rms[0] == 0.0F);
    CHECK(rms[1] > 0.0F);
}

TEST_CASE("measure_bandpass rejects malformed spans", "[spectra]") {
    constexpr SizeType kNChans = 4;
    constexpr SizeType kNSamps = 3125;
    std::vector<float> data(kNSamps * kNChans, 0.0F);
    std::vector<float> bandpass(kNChans);
    std::vector<float> rms(kNChans);

    CHECK_THROWS_AS(measure_bandpass(data, bandpass, rms, kNSamps + 1, kNChans),
                    std::invalid_argument);

    std::vector<float> wrong_size_bandpass(kNChans + 1);
    CHECK_THROWS_AS(
        measure_bandpass(data, wrong_size_bandpass, rms, kNSamps, kNChans),
        std::invalid_argument);

    std::vector<float> too_few_samples(4 * kNChans, 0.0F);
    CHECK_THROWS_AS(
        measure_bandpass(too_few_samples, bandpass, rms, 4, kNChans),
        std::invalid_argument);
}

TEST_CASE("measure_bandpass byte-packed overload unpacks with psrio",
          "[spectra]") {
    constexpr SizeType kNChans         = 4;
    constexpr SizeType kNSamps         = 3125;
    const std::vector<uint8_t> offsets = {10U, 20U, 30U, 40U};

    std::vector<uint8_t> data(kNSamps * kNChans);
    for (SizeType t = 0; t < kNSamps; ++t) {
        for (SizeType c = 0; c < kNChans; ++c) {
            data[(t * kNChans) + c] = offsets[c];
        }
    }

    std::vector<float> bandpass(kNChans);
    std::vector<float> rms(kNChans);
    measure_bandpass(data, bandpass, rms, kNSamps, kNChans, DataType::kUInt8);

    for (SizeType c = 0; c < kNChans; ++c) {
        CHECK(bandpass[c] == static_cast<float>(offsets[c]));
        CHECK(rms[c] == 0.0F);
    }
}

TEST_CASE("measure_bandpass rejects overflowing and truncated packed geometry",
          "[spectra][contracts]") {
    const std::span<const float> floats;
    const std::span<const uint8_t> bytes;
    std::array<float, 2> bandpass{99, 99}, rms{99, 99};
    const auto huge = std::numeric_limits<SizeType>::max();
    CHECK_THROWS_AS(measure_bandpass(floats, bandpass, rms, huge, 2),
                    std::invalid_argument);
    CHECK_THROWS_AS(
        measure_bandpass(bytes, bandpass, rms, huge, 2, DataType::kUInt8),
        std::invalid_argument);
    CHECK_THROWS_AS(measure_bandpass(bytes, std::span(bandpass).first(1),
                                     std::span(rms).first(1), huge / 8, 1,
                                     DataType::kFloat32),
                    std::invalid_argument);
    CHECK_THROWS_AS(
        measure_bandpass(bytes, bandpass, rms, 5, 2, DataType::kUInt8),
        std::invalid_argument);
    CHECK_THROWS_AS(measure_bandpass(bytes, bandpass, rms, 5, 2,
                                     static_cast<DataType>(127)),
                    std::invalid_argument);
    CHECK_THROWS_AS(measure_bandpass(bytes, {}, {}, 5, 0, DataType::kUInt8),
                    std::invalid_argument);
    CHECK_THROWS_AS(
        measure_bandpass(bytes, bandpass, rms, 4, 2, DataType::kUInt8),
        std::invalid_argument);
    CHECK((bandpass == std::array<float, 2>{99, 99}));
    CHECK((rms == std::array<float, 2>{99, 99}));
}

TEST_CASE("packed bandpass accepts complete subbyte samples with padding",
          "[spectra][contracts]") {
    // Ten 2-bit samples, alternating channel values 1 and 2, least significant
    // sample first. Only the first 20 bits belong to the five spectra.
    const std::array<uint8_t, 4> bytes{0x99, 0x99, 0x09, 0xff};
    std::array<float, 2> bandpass{}, rms{};
    measure_bandpass(bytes, bandpass, rms, 5, 2, DataType::kUInt2);
    CHECK((bandpass == std::array<float, 2>{1, 2}));
    CHECK((rms == std::array<float, 2>{0, 0}));
    CHECK_THROWS_AS(measure_bandpass(std::span(bytes).first(2), bandpass, rms,
                                     5, 2, DataType::kUInt2),
                    std::invalid_argument);
}
