#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <stdexcept>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <psrio/packed.hpp>

#include <frost/common/policies.hpp>

namespace {

// Packs `values` (one per (t, c) sample, row-major: t * nchans + c) into
// 32-bit little-endian words. On a little-endian host that is the
// byte-wise LSB layout `psrio::DataExtractor` and the search kernels read.
std::vector<uint8_t> pack_samples(const std::vector<uint32_t>& values,
                                  int nbits) {
    const std::size_t samples_per_word = 32U / static_cast<unsigned>(nbits);
    const std::size_t nwords =
        (values.size() + samples_per_word - 1) / samples_per_word;
    std::vector<uint32_t> words(nwords, 0U);
    for (std::size_t i = 0; i < values.size(); ++i) {
        const std::size_t word_index = i / samples_per_word;
        const std::size_t k          = i % samples_per_word;
        words[word_index] |= (values[i] << (k * static_cast<unsigned>(nbits)));
    }
    std::vector<uint8_t> bytes(nwords * sizeof(uint32_t));
    std::memcpy(bytes.data(), words.data(), bytes.size());
    return bytes;
}

std::span<const std::byte> as_bytes(const std::vector<uint8_t>& bytes) {
    return {reinterpret_cast<const std::byte*>(bytes.data()), bytes.size()};
}

// nbits 32 here is an unsigned integer, not SIGPROC's float convention.
psrio::SampleType integer_sample_type(int nbits) {
    switch (nbits) {
    case 1:
        return psrio::SampleType::kUInt1;
    case 2:
        return psrio::SampleType::kUInt2;
    case 4:
        return psrio::SampleType::kUInt4;
    case 8:
        return psrio::SampleType::kUInt8;
    case 16:
        return psrio::SampleType::kUInt16;
    case 32:
        return psrio::SampleType::kUInt32;
    default:
        throw std::invalid_argument("unsupported nbits");
    }
}

} // namespace

TEST_CASE("psrio DataExtractor unpacks the integer layout the kernels use",
          "[unpacker]") {
    constexpr frost::SizeType kNChans = 4;
    constexpr frost::SizeType kNSamps = 2;

    for (const int nbits : {1, 2, 4, 8, 16, 32}) {
        const uint32_t max_val =
            (nbits == 32) ? 0xABCDU
                          : ((1U << nbits) - 1U); // keep in-range for 32-bit
        std::vector<uint32_t> values(kNChans * kNSamps);
        for (std::size_t i = 0; i < values.size(); ++i) {
            values[i] = static_cast<uint32_t>(i) % (max_val + 1);
        }
        const auto bytes                    = pack_samples(values, nbits);
        const psrio::SampleType sample_type = integer_sample_type(nbits);
        const psrio::DataExtractor extractor(as_bytes(bytes), kNSamps, kNChans,
                                             sample_type);

        CHECK(frost::to_data_type(extractor.sample_type()) ==
              frost::from_nbits(nbits));
        for (frost::SizeType t = 0; t < kNSamps; ++t) {
            for (frost::SizeType c = 0; c < kNChans; ++c) {
                const auto expected = values[t * kNChans + c];
                CHECK(extractor.extract_sample(t, c) ==
                      static_cast<float>(expected));
            }
        }
    }
}

TEST_CASE("psrio DataExtractor unpacks float32 samples", "[unpacker]") {
    constexpr frost::SizeType kNChans = 3;
    constexpr frost::SizeType kNSamps = 2;

    const std::vector<float> values = {1.5F, -2.25F, 0.0F, 42.0F, -7.75F, 3.0F};
    std::vector<uint8_t> bytes(values.size() * sizeof(float));
    std::memcpy(bytes.data(), values.data(), bytes.size());

    const psrio::DataExtractor extractor(as_bytes(bytes), kNSamps, kNChans,
                                         psrio::SampleType::kFloat32);
    CHECK(frost::is_floating(frost::to_data_type(extractor.sample_type())));

    for (frost::SizeType t = 0; t < kNSamps; ++t) {
        for (frost::SizeType c = 0; c < kNChans; ++c) {
            CHECK(extractor.extract_sample(t, c) ==
                  values[static_cast<std::size_t>(t * kNChans + c)]);
        }
    }
}
