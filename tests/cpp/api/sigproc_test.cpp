#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <span>
#include <sstream>
#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <psrio/astro.hpp>
#include <psrio/formats/sigproc.hpp>
#include <psrio/packed.hpp>

#include <frost/common/policies.hpp>
#include <frost/config/config.hpp>
#include <frost/pipeline/input.hpp>
#include <frost/pipeline/pipeline.hpp>

using frost::DataType;

namespace {

void write_string_field(std::ostream& os, const std::string& s) {
    const auto len = static_cast<int32_t>(s.size());
    os.write(reinterpret_cast<const char*>(&len), sizeof(len));
    os.write(s.data(), len);
}

template <typename T>
void write_kv(std::ostream& os, const std::string& key, const T& value) {
    write_string_field(os, key);
    os.write(reinterpret_cast<const char*>(&value), sizeof(T));
}

// Builds a minimal synthetic sigproc filterbank in memory: header + a
// caller-supplied packed data block.
std::string
make_filterbank(int nchans, int nbits, double tsamp, const std::string& data) {
    std::ostringstream os(std::ios::binary);
    write_string_field(os, "HEADER_START");
    write_kv(os, "telescope_id", 4);
    write_kv(os, "machine_id", 10);
    write_kv(os, "data_type", 1);
    write_kv(os, "fch1", 1400.0);
    write_kv(os, "foff", -1.0);
    write_kv(os, "nchans", nchans);
    write_kv(os, "nbits", nbits);
    write_kv(os, "nifs", 1);
    write_kv(os, "tsamp", tsamp);
    write_kv(os, "tstart", 58000.0);
    write_kv(os, "ibeam", 3);
    write_string_field(os, "HEADER_END");
    os.write(data.data(), static_cast<std::streamsize>(data.size()));
    return os.str();
}

std::string write_temp(const std::string& path, const std::string& blob) {
    std::ofstream out(path, std::ios::binary);
    out.write(blob.data(), static_cast<std::streamsize>(blob.size()));
    return path;
}

std::uint64_t read_into(psrio::BlockSource& source,
                        std::span<std::uint8_t> dest,
                        std::uint64_t count) {
    return source.read_block(
        count, std::span<std::byte>(reinterpret_cast<std::byte*>(dest.data()),
                                    dest.size()));
}

} // namespace

TEST_CASE("psrio reads a synthetic sigproc header", "[sigproc]") {
    const auto path =
        write_temp("sigproc_test_header.fil",
                   make_filterbank(4, 8, 0.001, std::string(8, '\0')));
    const psrio::formats::sigproc::FilterbankReader reader(path);

    CHECK(reader.nchans() == 4);
    CHECK(reader.nbits() == 8);
    CHECK(reader.tsamp() == 0.001);
    CHECK(reader.fch1() == 1400.0);
    CHECK(reader.foff() == -1.0);
    CHECK(reader.beam() == 3);
    // nsamples omitted from the header -> inferred from remaining file size:
    // 8 bytes of data / 4 chans / (8 bits / 8) = 2 samples.
    CHECK(reader.nsamples() == 2);
    CHECK(reader.utc_start() == psrio::astro::mjd_to_time(58000.0));
    std::remove(path.c_str());
}

TEST_CASE("a non-sigproc file is rejected", "[sigproc]") {
    const auto path =
        write_temp("sigproc_test_reject.fil", "not a sigproc file");
    CHECK_THROWS_AS(psrio::formats::sigproc::FilterbankReader(path),
                    std::runtime_error);
    std::remove(path.c_str());
}

TEST_CASE("open_input reads 8-bit sigproc blocks unchanged", "[sigproc]") {
    constexpr int kNChans = 4;
    constexpr int kNSamps = 3;
    std::vector<uint8_t> values(kNChans * kNSamps);
    for (std::size_t i = 0; i < values.size(); ++i) {
        values[i] = static_cast<uint8_t>(i);
    }
    const std::string data(reinterpret_cast<const char*>(values.data()),
                           values.size());
    const auto path = write_temp("sigproc_test_8bit.fil",
                                 make_filterbank(kNChans, 8, 0.001, data));

    frost::config::InputConfig input;
    input.sigproc_file = path;
    auto file          = frost::pipeline::open_input(input);
    CHECK(file.nchans() == kNChans);
    CHECK(frost::to_data_type(file.sample_type()) == DataType::kUInt8);
    CHECK(file.bytes_per_sample() == kNChans);
    CHECK(file.beam() == 3);

    std::vector<uint8_t> gulp(kNChans * kNSamps);
    CHECK(read_into(file, gulp, kNSamps) == kNSamps);
    CHECK(std::memcmp(gulp.data(), values.data(), values.size()) == 0);

    std::vector<uint8_t> gulp2(kNChans * kNSamps);
    CHECK(read_into(file, gulp2, kNSamps) == 0);
    std::remove(path.c_str());
}

TEST_CASE("32-bit sigproc samples stay IEEE float", "[sigproc]") {
    constexpr int kNChans           = 2;
    constexpr int kNSamps           = 4;
    const std::vector<float> values = {1.0F, 2.0F, 3.0F, 4.0F,
                                       5.0F, 6.0F, 7.0F, 8.0F};
    std::string data(values.size() * sizeof(float), '\0');
    std::memcpy(data.data(), values.data(), data.size());
    const auto path = write_temp("sigproc_test_32bit.fil",
                                 make_filterbank(kNChans, 32, 0.001, data));

    frost::config::InputConfig input;
    input.sigproc_file = path;
    auto file          = frost::pipeline::open_input(input);
    CHECK(file.sample_type() == psrio::SampleType::kFloat32);
    CHECK(frost::to_data_type(file.sample_type()) == DataType::kFloat32);

    std::vector<uint8_t> gulp(kNChans * kNSamps * sizeof(float));
    CHECK(read_into(file, gulp, kNSamps) == kNSamps);
    std::vector<float> got(values.size());
    std::memcpy(got.data(), gulp.data(), gulp.size());
    CHECK(got == values);
    std::remove(path.c_str());
}

TEST_CASE("fswap reverses 8-bit channels through open_input", "[sigproc]") {
    const std::vector<uint8_t> values{10, 20, 30, 40};
    const std::string data(reinterpret_cast<const char*>(values.data()),
                           values.size());
    const auto path = write_temp("sigproc_test_fswap.fil",
                                 make_filterbank(4, 8, 0.001, data));
    frost::config::InputConfig input;
    input.sigproc_file = path;
    input.fswap        = true;
    auto file          = frost::pipeline::open_input(input);
    std::vector<uint8_t> gulp(4);
    CHECK(read_into(file, gulp, 1) == 1);
    CHECK(gulp == std::vector<uint8_t>{40, 30, 20, 10});
    std::remove(path.c_str());
}

TEST_CASE("the RFI layout mirrors the frequency axis under fswap",
          "[sigproc][robust]") {
    // Header: fch1 1400, foff -1, 4 channels (1400, 1399, 1398, 1397 MHz).
    // fswap hands channel 3 over first, so data channel 0 is at 1397 MHz and
    // zap_freqs in MHz must index that axis, not the header's.
    const std::vector<uint8_t> values{10, 20, 30, 40};
    const std::string data(reinterpret_cast<const char*>(values.data()),
                           values.size());
    const auto path = write_temp("sigproc_test_fswap_layout.fil",
                                 make_filterbank(4, 8, 0.001, data));
    frost::config::InputConfig input;
    input.sigproc_file = path;

    auto plain    = frost::pipeline::open_input(input);
    const auto l0 = frost::pipeline::rfi_block_layout(plain, 16);
    CHECK(l0.f0 == 1400.0);
    CHECK(l0.df == -1.0);

    input.fswap   = true;
    auto swapped  = frost::pipeline::open_input(input);
    const auto l1 = frost::pipeline::rfi_block_layout(swapped, 16);
    CHECK(l1.f0 == 1397.0);
    CHECK(l1.df == 1.0);
    CHECK(l1.tsamp == 0.001);
    std::remove(path.c_str());
}

TEST_CASE("channel reversal swaps sub-byte fields and is its own inverse",
          "[sigproc]") {
    std::vector<std::byte> nibbles{std::byte{0x12}};
    psrio::reverse_channels(nibbles, 1, 2, 4);
    CHECK(std::to_integer<uint8_t>(nibbles[0]) == 0x21);

    std::vector<std::byte> pairs{std::byte{0xE4}};
    psrio::reverse_channels(pairs, 1, 4, 2);
    CHECK(std::to_integer<uint8_t>(pairs[0]) == 0x1B);

    for (const int nbits : {2, 4, 8}) {
        std::vector<std::byte> original = {
            std::byte{0x21}, std::byte{0x43}, std::byte{0x65}, std::byte{0x87},
            std::byte{0x12}, std::byte{0x9A}, std::byte{0xBC}, std::byte{0xDE}};
        auto gulp = original;
        psrio::reverse_channels(gulp, 1, 8, nbits);
        CHECK(gulp != original);
        psrio::reverse_channels(gulp, 1, 8, nbits);
        CHECK(gulp == original);
    }
}

TEST_CASE("channel reversal rejects an unsupported bit depth", "[sigproc]") {
    std::vector<std::byte> gulp(4);
    CHECK_THROWS_AS(psrio::reverse_channels(gulp, 1, 4, 3), std::runtime_error);
}

TEST_CASE("mjd_to_time converts MJD to whole UTC seconds", "[sigproc]") {
    CHECK(psrio::astro::mjd_to_time(40587.0) == 0);
    CHECK(psrio::astro::mjd_to_time(40587.5) == 43200);
    CHECK(psrio::astro::mjd_to_time(58000.0) == 1504483200); // 2017-09-04
    // Fractional seconds truncate (no unconditional +1 s).
    CHECK(psrio::astro::mjd_to_time(58000.0 + (1.5 / 86400.0)) == 1504483201);
}
