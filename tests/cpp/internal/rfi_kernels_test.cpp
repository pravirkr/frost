#include "lib/cpu/rfi_kernels.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <random>
#include <span>
#include <vector>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <psrio/packed.hpp>

#include <frost/common/policies.hpp>
using frost::DataType;
using frost::IndexType;
using frost::SizeType;
namespace kernels = frost::prepare::rfi::kernels;

TEST_CASE("KadaneState finds the maximum-sum run, sequentially or in "
          "combined chunks",
          "[rfi][kadane]") {
    std::mt19937 rng(9);
    std::normal_distribution<double> noise(-0.1, 1.0);
    for (int trial = 0; trial < 20; ++trial) {
        std::vector<double> u(200);
        for (auto& v : u) {
            v = noise(rng);
        }
        // Brute force (empty run allowed).
        double best      = 0.0;
        IndexType bstart = 0;
        IndexType bend   = -1;
        for (IndexType i = 0; i < 200; ++i) {
            double s = 0.0;
            for (IndexType j = i; j < 200; ++j) {
                s += u[static_cast<SizeType>(j)];
                if (s > best) {
                    best   = s;
                    bstart = i;
                    bend   = j;
                }
            }
        }
        kernels::KadaneState seq;
        for (IndexType i = 0; i < 200; ++i) {
            seq.append(i, u[static_cast<SizeType>(i)]);
        }
        CHECK(seq.best.sum == Catch::Approx(best));
        CHECK(seq.best.start == bstart);
        CHECK(seq.best.end == bend);

        std::uniform_int_distribution<IndexType> cut(1, 60);
        kernels::KadaneState combined;
        for (IndexType i0 = 0; i0 < 200;) {
            const IndexType i1 = std::min<IndexType>(i0 + cut(rng), 200);
            kernels::KadaneState chunk;
            for (IndexType i = i0; i < i1; ++i) {
                chunk.append(i, u[static_cast<SizeType>(i)]);
            }
            combined = kernels::KadaneState::combine(combined, chunk);
            i0       = i1;
        }
        CHECK(combined.best.sum == Catch::Approx(best));
        CHECK(combined.best.start == bstart);
        CHECK(combined.best.end == bend);
        CHECK(combined.total == Catch::Approx(seq.total));
    }

    SECTION("positions may skip indices") {
        kernels::KadaneState s;
        s.append(0, -1.0);
        s.append(3, 2.0);
        s.append(4, 1.0);
        s.append(9, -5.0);
        s.append(12, 0.5);
        CHECK(s.best.sum == 3.0);
        CHECK(s.best.start == 3);
        CHECK(s.best.end == 4);
    }
}

TEST_CASE("Codec reads the sigproc packing and round-trips every dtype",
          "[rfi]") {
    std::mt19937 rng(1);
    std::uniform_int_distribution<int> byte(0, 255);
    std::vector<uint8_t> data(256);
    for (auto& b : data) {
        b = static_cast<uint8_t>(byte(rng));
    }
    for (const auto dtype :
         {DataType::kUInt1, DataType::kUInt2, DataType::kUInt4,
          DataType::kUInt8, DataType::kUInt16, DataType::kUInt32}) {
        const SizeType nchans =
            (data.size() * 8) / static_cast<SizeType>(frost::bits_of(dtype));
        const auto bytes = std::span<const std::byte>(
            reinterpret_cast<const std::byte*>(data.data()), data.size());
        const psrio::DataExtractor ref(bytes, 1, nchans,
                                       frost::to_sample_type(dtype));
        kernels::dispatch(dtype, [&](auto codec) {
            using C = decltype(codec);
            std::vector<uint8_t> copy(data.size(), 0);
            for (SizeType c = 0; c < nchans; ++c) {
                const float v = C::read(data.data(), c);
                REQUIRE(v == ref.extract_sample(0, c));
                C::write(copy.data(), c, v);
            }
            if constexpr (C::kBits < 32) {
                CHECK(copy == data);
            }
            std::vector<uint8_t> copied(data.size(), 0);
            for (SizeType c = 0; c < nchans; ++c) {
                C::copy(data.data(), copied.data(), c);
            }
            CHECK(copied == data);
        });
    }
    CHECK(kernels::Codec<DataType::kUInt8>::quantise(300.0F) == 255.0F);
    CHECK(kernels::Codec<DataType::kUInt8>::quantise(-4.0F) == 0.0F);
    CHECK(kernels::Codec<DataType::kUInt2>::quantise(2.6F) == 3.0F);
    CHECK(kernels::Codec<DataType::kFloat32>::quantise(-4.25F) == -4.25F);
}
