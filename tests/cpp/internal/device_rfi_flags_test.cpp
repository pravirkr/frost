#include <catch2/catch_test_macros.hpp>

#include <frost/prepare/rfi.hpp>

#include "lib/prepare/device_rfi.hpp"

TEST_CASE("default integer neighbour is the device RFI path", "[device]") {
    using frost::pipeline::device::DeviceRfi;
    using frost::pipeline::device::kFloatFillAbsTol;
    frost::prepare::rfi::RfiConfig cfg;
    CHECK(DeviceRfi::default_integer_neighbour(cfg, frost::DataType::kUInt8));
    CHECK(DeviceRfi::default_integer_neighbour(cfg, frost::DataType::kUInt4));
    CHECK_FALSE(
        DeviceRfi::default_integer_neighbour(cfg, frost::DataType::kFloat32));

    cfg.fill = frost::prepare::rfi::FillMode::kNoise;
    CHECK_FALSE(
        DeviceRfi::default_integer_neighbour(cfg, frost::DataType::kUInt8));
    cfg.fill    = frost::prepare::rfi::FillMode::kNeighbour;
    cfg.zero_dm = frost::prepare::rfi::ZeroDMMode::kSubtract;
    CHECK_FALSE(
        DeviceRfi::default_integer_neighbour(cfg, frost::DataType::kUInt8));
    cfg.zero_dm = frost::prepare::rfi::ZeroDMMode::kZdot;
    CHECK_FALSE(
        DeviceRfi::default_integer_neighbour(cfg, frost::DataType::kUInt8));
    cfg.zero_dm  = frost::prepare::rfi::ZeroDMMode::kOff;
    cfg.equalize = true;
    CHECK_FALSE(
        DeviceRfi::default_integer_neighbour(cfg, frost::DataType::kUInt8));
    // fill=noise, zero-DM, zdot and equalise are not bit-exact. A device
    // comparison of those passes uses this absolute margin on the unpacked
    // sample. They stay on the host cleaner.
    CHECK(kFloatFillAbsTol == 1.0e-3F);
}

TEST_CASE("broadband flags use whole-series noise and retain existing flags",
          "[device][rfi][mad]") {
    constexpr frost::SizeType kCount = 8192;
    std::vector<float> series(kCount);
    for (frost::SizeType t = 0; t < kCount; ++t) {
        const float amplitude = t < 3125 ? 1.0F : 8.0F;
        series[t]             = t % 2 ? -amplitude : amplitude;
    }
    series[6000] = 80;
    series[7000] = -80;
    std::vector<uint8_t> flags(kCount, 0);
    flags[10] = 1;
    // One baseline knot leaves the already zero-centered series unchanged.
    const auto count = frost::pipeline::device::broadband_time_flags(
        series, kCount, 5, 1, flags);
    CHECK(count == 2);
    for (frost::SizeType t = 0; t < kCount; ++t) {
        CHECK(flags[t] == (t == 10 || t == 6000 || t == 7000 ? 1 : 0));
    }
}
