#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <span>
#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>
#include <highfive/highfive.hpp>
#include <psrio/formats/fbh5.hpp>

#include <frost/config/config.hpp>
#include <frost/pipeline/input.hpp>

using Catch::Matchers::ContainsSubstring;
using psrio::formats::fbh5::FilterbankFile;
using psrio::formats::fbh5::is_hdf5_filterbank;

namespace {

struct TempFile {
    std::string path;
    explicit TempFile(std::string name) : path(std::move(name)) {}
    ~TempFile() { std::remove(path.c_str()); }
    TempFile(const TempFile&)            = delete;
    TempFile& operator=(const TempFile&) = delete;
};

struct HeaderSpec {
    std::string source_name = "B0329+54";
    double tstart           = 58000.0;
    double tsamp            = 1.0e-3;
    double fch1             = 1400.0;
    double foff             = -1.0;
    int nbits               = 8;
    int telescope_id        = 6;
    int ibeam               = 3;
    double src_raj          = 123456.7;
    bool write_tsamp        = true;
    bool write_data         = true;
    bool gzip               = false;
};

template <typename T>
void write_filterbank(const std::string& path,
                      std::size_t ntime,
                      std::size_t nbeams,
                      std::size_t nchans,
                      const HeaderSpec& spec,
                      const std::vector<T>& cube) {
    REQUIRE(cube.size() == ntime * nbeams * nchans);
    HighFive::File file(path, HighFive::File::ReadWrite |
                                  HighFive::File::Create |
                                  HighFive::File::Truncate);
    file.createAttribute("CLASS", std::string("FILTERBANK"));
    file.createAttribute("VERSION", std::string("1.0"));
    if (!spec.write_data) {
        return;
    }

    HighFive::DataSetCreateProps props;
    if (spec.gzip) {
        props.add(HighFive::Chunking(std::vector<hsize_t>{
            std::min<hsize_t>(ntime, 2), 1, static_cast<hsize_t>(nchans)}));
        props.add(HighFive::Deflate(4));
    }
    const std::vector<std::size_t> dims{ntime, nbeams, nchans};
    auto data = file.createDataSet<T>("data", HighFive::DataSpace(dims), props);
    data.write_raw(cube.data());
    data.createAttribute("source_name", spec.source_name);
    data.createAttribute("tstart", spec.tstart);
    if (spec.write_tsamp) {
        data.createAttribute("tsamp", spec.tsamp);
    }
    data.createAttribute("fch1", spec.fch1);
    data.createAttribute("foff", spec.foff);
    data.createAttribute("nchans", static_cast<int>(nchans));
    data.createAttribute("nifs", static_cast<int>(nbeams));
    data.createAttribute("nbits", spec.nbits);
    data.createAttribute("telescope_id", spec.telescope_id);
    data.createAttribute("ibeam", spec.ibeam);
    data.createAttribute("src_raj", spec.src_raj);
}

std::uint64_t
read_into(auto& source, std::span<std::uint8_t> dest, std::uint64_t count) {
    return source.read_block(
        count, std::span<std::byte>(reinterpret_cast<std::byte*>(dest.data()),
                                    dest.size()));
}

} // namespace

TEST_CASE("HDF5 filterbank header round-trips", "[hdf5]") {
    constexpr std::size_t kNTime  = 2;
    constexpr std::size_t kNBeams = 1;
    constexpr std::size_t kNChans = 4;
    const TempFile file("frost_hdf5_header.h5");
    HeaderSpec spec;
    spec.foff = -0.5;
    const std::vector<std::uint8_t> cube(kNTime * kNBeams * kNChans, 0);
    write_filterbank(file.path, kNTime, kNBeams, kNChans, spec, cube);

    const FilterbankFile source(file.path);
    const auto& header = source.header();
    CHECK(header.source_name == "B0329+54");
    CHECK(header.version == "1.0");
    CHECK(header.tstart == 58000.0);
    CHECK(header.tsamp == 1.0e-3);
    CHECK(header.fch1 == 1400.0);
    CHECK(header.foff == -0.5);
    CHECK(header.nchans == 4);
    CHECK(header.nifs == 1);
    CHECK(header.nbeams == 1);
    CHECK(header.nbits == 8);
    CHECK(header.nsamples == 2);
    CHECK(header.telescope_id == 6);
    CHECK(header.has_telescope_id);
    CHECK(header.ibeam == 3);
    CHECK(header.has_src_raj);
    CHECK(header.src_raj == 123456.7);
    CHECK(source.nchans() == kNChans);
    CHECK(source.sample_type() == psrio::SampleType::kUInt8);
    CHECK(source.bytes_per_sample() == kNChans);
    CHECK(source.tsamp() == header.tsamp);
    CHECK(source.beam() == 3);
    CHECK(source.fch1() == 1400.0);
    CHECK(source.foff() == -0.5);
}

TEST_CASE("HDF5 filterbank reads a channel ramp in pipeline order", "[hdf5]") {
    constexpr std::size_t kNTime  = 3;
    constexpr std::size_t kNBeams = 1;
    constexpr std::size_t kNChans = 4;
    const TempFile file("frost_hdf5_ramp.h5");
    std::vector<std::uint8_t> cube(kNTime * kNChans);
    for (std::size_t t = 0; t < kNTime; ++t) {
        for (std::size_t c = 0; c < kNChans; ++c) {
            cube[(t * kNChans) + c] =
                static_cast<std::uint8_t>((t * 10) + c + 1);
        }
    }
    write_filterbank(file.path, kNTime, kNBeams, kNChans, HeaderSpec{}, cube);

    FilterbankFile source(file.path);
    std::vector<std::uint8_t> gulp(kNTime * kNChans, 0);
    CHECK(read_into(source, gulp, kNTime) == kNTime);
    CHECK(std::memcmp(gulp.data(), cube.data(), cube.size()) == 0);
    std::vector<std::uint8_t> extra(kNChans, 0xFF);
    CHECK(read_into(source, extra, 1) == 0);
}

TEST_CASE("HDF5 filterbank gulps concatenate without overlap or gap",
          "[hdf5]") {
    constexpr std::size_t kNTime  = 5;
    constexpr std::size_t kNBeams = 1;
    constexpr std::size_t kNChans = 3;
    const TempFile file("frost_hdf5_gulps.h5");
    std::vector<std::uint8_t> cube(kNTime * kNChans);
    for (std::size_t i = 0; i < cube.size(); ++i) {
        cube[i] = static_cast<std::uint8_t>(i + 1);
    }
    HeaderSpec spec;
    spec.gzip = true;
    write_filterbank(file.path, kNTime, kNBeams, kNChans, spec, cube);

    FilterbankFile source(file.path);
    std::vector<std::uint8_t> got;
    got.reserve(cube.size());
    std::vector<std::uint8_t> gulp(2 * kNChans);
    std::size_t reads = 0;
    while (true) {
        const auto nread = read_into(source, gulp, 2);
        if (nread == 0) {
            break;
        }
        REQUIRE(nread <= 2);
        got.insert(got.end(), gulp.begin(),
                   gulp.begin() + static_cast<std::ptrdiff_t>(nread * kNChans));
        ++reads;
    }
    CHECK(reads == 3);
    REQUIRE(got.size() == cube.size());
    CHECK(std::memcmp(got.data(), cube.data(), cube.size()) == 0);
}

TEST_CASE("negative foff keeps sigproc channel order", "[hdf5]") {
    // Sigproc stores channel 0 at fch1 and steps by signed foff. foff < 0
    // means the buffer is high-frequency-first; FDMT flips internally
    // when df < 0. Reversing channels in the reader would flip DM delays,
    // so frequency index 0 must stay channel 0 of the gulp.
    constexpr std::size_t kNTime  = 1;
    constexpr std::size_t kNBeams = 1;
    constexpr std::size_t kNChans = 4;
    const TempFile file("frost_hdf5_foff.h5");
    const std::vector<std::uint8_t> cube{10, 20, 30, 40};
    HeaderSpec spec;
    spec.fch1 = 1600.0;
    spec.foff = -2.5;
    write_filterbank(file.path, kNTime, kNBeams, kNChans, spec, cube);

    FilterbankFile source(file.path);
    CHECK(source.fch1() == 1600.0);
    CHECK(source.foff() == -2.5);
    CHECK(source.header().fch1 == 1600.0);
    CHECK(source.header().foff == -2.5);
    std::vector<std::uint8_t> gulp(kNChans, 0);
    CHECK(read_into(source, gulp, 1) == 1);
    CHECK(gulp[0] == 10);
    CHECK(gulp[1] == 20);
    CHECK(gulp[3] == 40);
}

TEST_CASE("HDF5 filterbank reads the selected beam", "[hdf5]") {
    constexpr std::size_t kNTime  = 2;
    constexpr std::size_t kNBeams = 2;
    constexpr std::size_t kNChans = 3;
    const TempFile file("frost_hdf5_beams.h5");
    std::vector<std::uint8_t> cube(kNTime * kNBeams * kNChans, 0);
    for (std::size_t t = 0; t < kNTime; ++t) {
        for (std::size_t c = 0; c < kNChans; ++c) {
            cube[(t * kNBeams * kNChans) + c]           = 1;
            cube[(t * kNBeams * kNChans) + kNChans + c] = 2;
        }
    }
    write_filterbank(file.path, kNTime, kNBeams, kNChans, HeaderSpec{}, cube);

    FilterbankFile beam0(file.path);
    CHECK(beam0.header().nifs == 2);
    CHECK(beam0.beam() == 1);
    std::vector<std::uint8_t> gulp(kNTime * kNChans, 0);
    CHECK(read_into(beam0, gulp, kNTime) == kNTime);
    CHECK(gulp[0] == 1);
    CHECK(gulp[kNChans] == 1);

    FilterbankFile beam1(file.path, 1);
    CHECK(beam1.beam() == 2);
    std::fill(gulp.begin(), gulp.end(), 0);
    CHECK(read_into(beam1, gulp, kNTime) == kNTime);
    CHECK(gulp[0] == 2);
    CHECK(gulp[(kNChans * 2) - 1] == 2);

    CHECK_THROWS_WITH(FilterbankFile(file.path, 2),
                      ContainsSubstring("out of range"));
}

TEST_CASE("HDF5 filterbank reads float32 samples", "[hdf5]") {
    constexpr std::size_t kNTime  = 2;
    constexpr std::size_t kNBeams = 1;
    constexpr std::size_t kNChans = 2;
    const TempFile file("frost_hdf5_f32.h5");
    const std::vector<float> cube{1.0F, 2.0F, 3.0F, 4.0F};
    HeaderSpec spec;
    spec.nbits = 32;
    write_filterbank(file.path, kNTime, kNBeams, kNChans, spec, cube);

    FilterbankFile source(file.path);
    CHECK(source.sample_type() == psrio::SampleType::kFloat32);
    CHECK(source.bytes_per_sample() == kNChans * sizeof(float));
    std::vector<std::uint8_t> bytes(cube.size() * sizeof(float));
    CHECK(read_into(source, bytes, kNTime) == kNTime);
    std::vector<float> got(cube.size());
    std::memcpy(got.data(), bytes.data(), bytes.size());
    CHECK(got == cube);
}

TEST_CASE("HDF5 filterbank reads uint16 samples", "[hdf5]") {
    constexpr std::size_t kNTime  = 1;
    constexpr std::size_t kNBeams = 1;
    constexpr std::size_t kNChans = 3;
    const TempFile file("frost_hdf5_u16.h5");
    const std::vector<std::uint16_t> cube{1000, 2000, 3000};
    HeaderSpec spec;
    spec.nbits = 16;
    write_filterbank(file.path, kNTime, kNBeams, kNChans, spec, cube);

    FilterbankFile source(file.path);
    CHECK(source.sample_type() == psrio::SampleType::kUInt16);
    std::vector<std::uint8_t> bytes(cube.size() * sizeof(std::uint16_t));
    CHECK(read_into(source, bytes, 1) == 1);
    std::vector<std::uint16_t> got(cube.size());
    std::memcpy(got.data(), bytes.data(), bytes.size());
    CHECK(got == cube);
}

TEST_CASE("HDF5 filterbank missing tsamp throws", "[hdf5]") {
    const TempFile file("frost_hdf5_no_tsamp.h5");
    HeaderSpec spec;
    spec.write_tsamp = false;
    const std::vector<std::uint8_t> cube(4, 1);
    write_filterbank(file.path, 1, 1, 4, spec, cube);
    CHECK_THROWS_AS(FilterbankFile(file.path), std::runtime_error);
    CHECK_THROWS_WITH(FilterbankFile(file.path), ContainsSubstring("tsamp"));
}

TEST_CASE("HDF5 filterbank missing data dataset throws", "[hdf5]") {
    const TempFile file("frost_hdf5_no_data.h5");
    HeaderSpec spec;
    spec.write_data = false;
    write_filterbank(file.path, 0, 0, 0, spec, std::vector<std::uint8_t>{});
    CHECK_THROWS_AS(FilterbankFile(file.path), std::runtime_error);
    CHECK_THROWS_WITH(FilterbankFile(file.path), ContainsSubstring("data"));
}

TEST_CASE("HDF5 filterbank rejects an unsupported dtype", "[hdf5]") {
    const TempFile file("frost_hdf5_i32.h5");
    HeaderSpec spec;
    spec.nbits = 32;
    const std::vector<std::int32_t> cube{1, 2, 3, 4};
    write_filterbank(file.path, 1, 1, 4, spec, cube);
    CHECK_THROWS_WITH(FilterbankFile(file.path),
                      ContainsSubstring("unsupported HDF5 sample type"));
}

TEST_CASE("HDF5 magic selects the filterbank reader without a .h5 suffix",
          "[hdf5]") {
    const TempFile file("frost_hdf5_sniff.fil");
    const std::vector<std::uint8_t> cube{4, 5, 6, 7};
    write_filterbank(file.path, 1, 1, 4, HeaderSpec{}, cube);
    CHECK(is_hdf5_filterbank(file.path));
    CHECK(is_hdf5_filterbank("anything.h5"));
    CHECK(is_hdf5_filterbank("anything.HDF5"));
    CHECK_FALSE(is_hdf5_filterbank("missing-not-hdf5.fil"));

    FilterbankFile source(file.path);
    std::vector<std::uint8_t> gulp(4);
    CHECK(read_into(source, gulp, 1) == 1);
    CHECK(gulp == cube);

    frost::config::InputConfig input;
    input.sigproc_file = file.path;
    auto opened        = frost::pipeline::open_input(input);
    CHECK(opened.nchans() == 4);
    CHECK(opened.sample_type() == psrio::SampleType::kUInt8);
    std::vector<std::uint8_t> via_open(4);
    CHECK(read_into(opened, via_open, 1) == 1);
    CHECK(via_open == cube);
}
