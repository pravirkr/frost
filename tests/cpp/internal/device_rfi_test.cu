#include "lib/prepare/device_rfi.hpp"

#include <algorithm>
#include <cstdint>
#include <stdexcept>
#include <vector>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <frost/prepare/rfi.hpp>

#include "lib/cuda/gpu_utils.cuh"

using frost::DataType;
using frost::SizeType;
using frost::pipeline::device::DeviceRfi;
using frost::pipeline::device::kFloatFillAbsTol;
using frost::prepare::rfi::BlockLayout;
using frost::prepare::rfi::ChannelRange;
using frost::prepare::rfi::FillMode;
using frost::prepare::rfi::RFICleaner;
using frost::prepare::rfi::RfiConfig;
using frost::prepare::rfi::ZeroDMMode;

namespace {

// fill=noise, zero-DM, zdot and equalise are tolerance-only. This is the
// absolute margin a device comparison of those passes must use on the
// unpacked sample. The default integer neighbour path below is bit-exact.
constexpr float kNamedFloatFillTol = 1.0e-3F;

BlockLayout layout_for(SizeType nchans, SizeType nsamps, DataType dtype) {
    BlockLayout layout;
    layout.nchans     = nchans;
    layout.max_nsamps = nsamps;
    layout.dtype      = dtype;
    layout.tsamp      = 1.0e-3F;
    layout.f0         = 1500.0;
    layout.df         = -1.0;
    return layout;
}

// A missing device is a skip; any other CUDA/HIP failure must fail the test.
bool have_device() {
    int n = 0;
    return cudaGetDeviceCount(&n) == cudaSuccess && n > 0;
}

uint8_t noise_byte(SizeType t, SizeType c) {
    uint32_t x = static_cast<uint32_t>((t * 131U) + (c * 17U) + 3U);
    x          = (x * 1664525U) + 1013904223U;
    return static_cast<uint8_t>(40U + (x % 50U));
}

} // namespace

TEST_CASE("float RFI fills name an absolute tolerance", "[gpu][parity]") {
    REQUIRE(kFloatFillAbsTol == kNamedFloatFillTol);
    RfiConfig cfg;
    cfg.fill = FillMode::kNoise;
    CHECK_FALSE(DeviceRfi::default_integer_neighbour(cfg, DataType::kUInt8));
    cfg         = {};
    cfg.zero_dm = ZeroDMMode::kSubtract;
    CHECK_FALSE(DeviceRfi::default_integer_neighbour(cfg, DataType::kUInt8));
    cfg         = {};
    cfg.zero_dm = ZeroDMMode::kZdot;
    CHECK_FALSE(DeviceRfi::default_integer_neighbour(cfg, DataType::kUInt8));
    cfg          = {};
    cfg.equalize = true;
    CHECK_FALSE(DeviceRfi::default_integer_neighbour(cfg, DataType::kUInt8));
}

TEST_CASE("device integer neighbour RFI matches RFICleaner", "[gpu][parity]") {
    if (!have_device()) {
        SKIP("no CUDA or HIP device is visible");
    }
    constexpr SizeType kNchans = 64;
    constexpr SizeType kNsamps = 200;
    constexpr float kBaseline  = 0.05F;
    const auto layout          = layout_for(kNchans, kNsamps, DataType::kUInt8);
    RfiConfig cfg;
    cfg.zap_chans.push_back(ChannelRange{0, 0});

    std::vector<uint8_t> gulp(kNsamps * kNchans, 0);
    for (SizeType t = 0; t < kNsamps; ++t) {
        for (SizeType c = 0; c < kNchans; ++c) {
            gulp[(t * kNchans) + c] = noise_byte(t, c);
        }
    }
    for (SizeType t = 0; t < kNsamps; ++t) {
        gulp[(t * kNchans) + 4] = 255;
    }
    for (SizeType c = 0; c < kNchans; ++c) {
        gulp[(80 * kNchans) + c] = 255;
    }
    gulp[(20 * kNchans) + 9]   = 255;
    gulp[(21 * kNchans) + 9]   = 0;
    gulp[(120 * kNchans) + 15] = 255;

    std::vector<uint8_t> tiny(3 * kNchans, 80);
    auto tiny_host = tiny;
    auto gulp_host = gulp;
    std::vector<uint8_t> tiny_got(tiny.size());
    std::vector<uint8_t> got(gulp.size());
    std::vector<uint8_t> tiny_mask;
    std::vector<uint8_t> full_mask;
    frost::prepare::rfi::RfiReport full_report;

#if defined(FROST_ENABLE_HIP)
    const auto backend = dmt::Backend::kHIP;
#else
    const auto backend = dmt::Backend::kCUDA;
#endif
    {
        cudaStream_t queue = nullptr;
        FROST_CUDA_CHECK(cudaStreamCreate(&queue));
        DeviceRfi device(backend, 0, cfg, layout, kBaseline);
        const auto tiny_out =
            device.execute(tiny, kNchans, 3, 3, dmt::Stream(queue));
        device.download(tiny_got, kNchans, 3, dmt::Stream(queue));
        const auto full_out =
            device.execute(gulp, kNchans, kNsamps, kNsamps, dmt::Stream(queue));
        device.download(got, kNchans, kNsamps, dmt::Stream(queue));
        FROST_CUDA_CHECK(cudaStreamDestroy(queue));
        tiny_mask   = tiny_out.channel_mask;
        full_mask   = full_out.channel_mask;
        full_report = full_out.report;
    }

    RFICleaner host(cfg, layout, kBaseline);
    host.execute(tiny_host);
    host.execute(gulp_host);
    const auto host_mask = host.channel_mask();

    REQUIRE(tiny_got == tiny_host);
    REQUIRE(tiny_mask.size() == kNchans);
    REQUIRE(got == gulp_host);
    REQUIRE(full_mask.size() == host_mask.size());
    REQUIRE(std::equal(full_mask.begin(), full_mask.end(), host_mask.begin()));
    REQUIRE(full_report.chans_masked == host.report().chans_masked);
    REQUIRE(full_report.samps_flagged == host.report().samps_flagged);
    REQUIRE(full_report.cells_narrow == host.report().cells_narrow);
}

// sample_rows must compute `i * len` without 32-bit overflow: with one window
// longer than ~687k samples a 32-bit product would wrap and read out of
// bounds. A baseline longer than the gulp makes the single
// bandpass window span all 2^20 samples, so `i * len` reaches 3124 * 2^20
// > 2^31; the short baseline covers many windows in one large gulp. The
// overflowed index reads outside the buffer without faulting and, on this
// fixture, without changing the cleaned output, so only compute-sanitizer
// (memcheck) detects a regression in the long-window case.
TEST_CASE("device RFI handles gulps beyond 2^20 samples", "[gpu][parity]") {
    if (!have_device()) {
        SKIP("no CUDA or HIP device is visible");
    }
    constexpr SizeType kNchans = 16;
    constexpr SizeType kNsamps = 1U << 20U;
    const float kBaseline      = GENERATE(0.05F, 2000.0F);
    const auto layout          = layout_for(kNchans, kNsamps, DataType::kUInt8);
    RfiConfig cfg;

    std::vector<uint8_t> gulp(kNsamps * kNchans, 0);
    for (SizeType t = 0; t < kNsamps; ++t) {
        for (SizeType c = 0; c < kNchans; ++c) {
            gulp[(t * kNchans) + c] = noise_byte(t, c);
        }
    }
    gulp[(700000 * kNchans) + 3] = 255;
    auto host_gulp               = gulp;
    std::vector<uint8_t> got(gulp.size());

#if defined(FROST_ENABLE_HIP)
    const auto backend = dmt::Backend::kHIP;
#else
    const auto backend = dmt::Backend::kCUDA;
#endif
    cudaStream_t queue = nullptr;
    FROST_CUDA_CHECK(cudaStreamCreate(&queue));
    {
        DeviceRfi device(backend, 0, cfg, layout, kBaseline);
        device.execute(gulp, kNchans, kNsamps, kNsamps, dmt::Stream(queue));
        device.download(got, kNchans, kNsamps, dmt::Stream(queue));
    }
    FROST_CUDA_CHECK(cudaStreamDestroy(queue));

    RFICleaner host(cfg, layout, kBaseline);
    host.execute(host_gulp);
    CHECK(got == host_gulp);
}

TEST_CASE("device normalization matches host calibration and zeros padding",
          "[gpu][parity]") {
    if (!have_device())
        SKIP("no CUDA or HIP device is visible");
    constexpr SizeType kChans = 8, kBlock = 256;
    const SizeType valid  = GENERATE(SizeType{2}, SizeType{200});
    const bool flip       = GENERATE(false, true);
    const bool all_masked = GENERATE(false, true);
    const bool warm       = GENERATE(false, true);
    auto layout           = layout_for(kChans, kBlock, DataType::kUInt8);
    layout.df             = flip ? -1.0 : 1.0;
    RfiConfig cfg;
    cfg.narrow.enabled = cfg.broad.enabled = false;
    cfg.zap_chans                          = {{0, all_masked ? kChans - 1 : 0}};
    std::vector<uint8_t> input(kChans * kBlock, 255);
    for (SizeType t = 0; t < valid; ++t) {
        for (SizeType c = 0; c < kChans; ++c)
            input[t * kChans + c] = noise_byte(t, c);
    }
    RFICleaner host(cfg, layout, 0.5F);
    auto preceding = input;
    for (SizeType t = 0; t < kBlock; ++t) {
        for (SizeType c = 0; c < kChans; ++c)
            preceding[t * kChans + c] = noise_byte(t, c);
    }
    if (warm)
        host.execute_normalized(preceding);
    const auto expected = host.execute_normalized(input, valid);
#if defined(FROST_ENABLE_HIP)
    const auto backend = dmt::Backend::kHIP;
#else
    const auto backend = dmt::Backend::kCUDA;
#endif
    cudaStream_t queue{};
    FROST_CUDA_CHECK(cudaStreamCreate(&queue));
    std::vector<float> actual(expected.size());
    {
        DeviceRfi device(backend, 0, cfg, layout, 0.5F);
        if (warm)
            device.execute(preceding, kChans, kBlock, kBlock,
                           dmt::Stream(queue));
        device.execute(input, kChans, valid, kBlock, dmt::Stream(queue));
        const auto normalized = device.normalize_to_fdmt(
            kChans, kBlock, valid, flip, dmt::Stream(queue));
        FROST_CUDA_CHECK(cudaMemcpyAsync(actual.data(), normalized.data(),
                                         normalized.size_bytes(),
                                         cudaMemcpyDeviceToHost, queue));
        FROST_CUDA_CHECK(cudaStreamSynchronize(queue));
    }
    FROST_CUDA_CHECK(cudaStreamDestroy(queue));
    REQUIRE(actual.size() == expected.size());
    for (SizeType i = 0; i < actual.size(); ++i) {
        CHECK(actual[i] == Catch::Approx(expected[i]).margin(2.0e-4F));
    }
}
