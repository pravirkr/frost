#include <cstddef>
#include <cstdint>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <functional>
#include <random>
#include <span>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#include <ascii_header.h>
#include <catch2/catch_test_macros.hpp>
#include <ipcbuf.h>
#include <ipcio.h>
#include <psrio/block_source.hpp>
#include <psrio/formats/psrdada.hpp>
#include <unistd.h>

#include <frost/config/config.hpp>
#include <frost/dedisp/dedisp.hpp>
#include <frost/pipeline/pipeline.hpp>

#include "support/temp_dir.hpp"

using frost::SizeType;
using psrio::formats::psrdada::RingReader;

namespace {

void set_header(char* header, const char* key, const char* fmt, auto value) {
    if (ascii_header_set(header, key, fmt, value) < 0) {
        throw std::runtime_error(std::string("ascii_header_set failed for ") +
                                 key);
    }
}

class RingPair {
public:
    RingPair(const RingPair&)            = delete;
    RingPair& operator=(const RingPair&) = delete;

    RingPair(RingPair&& other) noexcept
        : m_key(other.m_key),
          m_header(other.m_header),
          m_data(other.m_data),
          m_header_bufsz(other.m_header_bufsz),
          m_live(other.m_live) {
        other.m_live = false;
    }

    RingPair& operator=(RingPair&&) = delete;

    ~RingPair() {
        if (!m_live) {
            return;
        }
        ipcio_destroy(&m_data);
        ipcbuf_destroy(&m_header);
    }

    static RingPair create(SizeType data_bytes) {
        const auto page = static_cast<uint64_t>(sysconf(_SC_PAGESIZE));
        const uint64_t header_bufsz = page;
        const uint64_t pages =
            data_bytes == 0
                ? 4
                : ((static_cast<uint64_t>(data_bytes) + page - 1) / page) + 2;
        std::mt19937 rng{std::random_device{}()};
        std::uniform_int_distribution<int> dist(0x1000, 0x7ffe);
        for (int attempt = 0; attempt < 32; ++attempt) {
            const auto key  = static_cast<key_t>(dist(rng) & ~1);
            ipcbuf_t header = IPCBUF_INIT;
            ipcio_t data    = IPCIO_INIT;
            if (ipcbuf_create(&header, key + 1, 4, header_bufsz, 1) < 0) {
                continue;
            }
            if (ipcio_create(&data, key, pages, page, 1) < 0) {
                ipcbuf_destroy(&header);
                continue;
            }
            return RingPair(key, header, data, header_bufsz);
        }
        throw std::runtime_error("could not create a PSRDADA ring");
    }

    key_t key() const { return m_key; }

    void write_header(const std::function<void(char*)>& fill) {
        ipcbuf_t writer = IPCBUF_INIT;
        if (ipcbuf_connect(&writer, m_key + 1) < 0) {
            throw std::runtime_error("header connect failed");
        }
        struct Disconnect {
            ipcbuf_t* buf;
            ~Disconnect() { ipcbuf_disconnect(buf); }
        } disconnect{&writer};

        if (ipcbuf_lock_write(&writer) < 0) {
            throw std::runtime_error("header lock_write failed");
        }
        char* buf = ipcbuf_get_next_write(&writer);
        if (buf == nullptr) {
            throw std::runtime_error("header get_next_write failed");
        }
        std::memset(buf, 0, static_cast<std::size_t>(m_header_bufsz));
        fill(buf);
        if (ipcbuf_mark_filled(&writer, m_header_bufsz) < 0) {
            throw std::runtime_error("header mark_filled failed");
        }
        if (ipcbuf_unlock_write(&writer) < 0) {
            throw std::runtime_error("header unlock_write failed");
        }
    }

    void write_data(std::vector<uint8_t>& bytes) {
        ipcio_t writer = IPCIO_INIT;
        if (ipcio_connect(&writer, m_key) < 0) {
            throw std::runtime_error("data connect failed");
        }
        struct Disconnect {
            ipcio_t* buf;
            bool open = false;
            ~Disconnect() {
                if (open) {
                    ipcio_close(buf);
                }
                ipcio_disconnect(buf);
            }
        } disconnect{&writer, false};

        // 'W' raises start-of-data on the first buffer. 'w' leaves the
        // ring in the inactive-writer state, which never publishes a
        // partial buffer, so the reader blocks forever.
        if (ipcio_open(&writer, 'W') < 0) {
            throw std::runtime_error("data open failed");
        }
        disconnect.open = true;
        if (!bytes.empty()) {
            const auto wrote = ipcio_write(
                &writer, reinterpret_cast<char*>(bytes.data()), bytes.size());
            if (wrote != static_cast<ssize_t>(bytes.size())) {
                throw std::runtime_error("data write failed");
            }
        }
        if (ipcio_stop(&writer) < 0) {
            throw std::runtime_error("data stop failed");
        }
    }

private:
    RingPair(key_t key, ipcbuf_t header, ipcio_t data, uint64_t header_bufsz)
        : m_key(key),
          m_header(header),
          m_data(data),
          m_header_bufsz(header_bufsz) {}

    key_t m_key;
    ipcbuf_t m_header;
    ipcio_t m_data;
    uint64_t m_header_bufsz;
    bool m_live = true;
};

void fill_base(char* header, int nchan, int nbit) {
    set_header(header, "NCHAN", "%d", nchan);
    set_header(header, "NBIT", "%d", nbit);
    set_header(header, "NPOL", "%d", 1);
    set_header(header, "NDIM", "%d", 1);
    set_header(header, "ORDER", "%s", "TF");
    set_header(header, "CFREQ", "%f", 1400.0);
    set_header(header, "BANDWIDTH", "%f", -200.0);
    set_header(header, "TSAMP", "%f", 64.0);
    set_header(header, "UTC_START", "%s", "2020-01-02-03:04:05");
    set_header(header, "BEAM", "%d", 7);
}

std::time_t expected_utc() {
    std::tm tm{};
    if (strptime("2020-01-02-03:04:05", "%Y-%m-%d-%H:%M:%S", &tm) == nullptr) {
        throw std::runtime_error("strptime failed");
    }
    return timegm(&tm);
}

std::uint64_t
read_into(auto& source, std::span<std::uint8_t> dest, std::uint64_t count) {
    return source.read_block(
        count, std::span<std::byte>(reinterpret_cast<std::byte*>(dest.data()),
                                    dest.size()));
}

} // namespace

TEST_CASE("PSRDADA header sets frequency, sampling and beam", "[psrdada]") {
    auto ring = RingPair::create(0);
    ring.write_header([](char* header) {
        set_header(header, "NCHAN", "%d", 4);
        set_header(header, "NBIT", "%d", 8);
        set_header(header, "NPOL", "%d", 1);
        set_header(header, "NDIM", "%d", 1);
        set_header(header, "ORDER", "%s", "TF");
        set_header(header, "CFREQ", "%f", 1400.0);
        set_header(header, "BANDWIDTH", "%f", -200.0);
        set_header(header, "TSAMP", "%f", 64.0);
        set_header(header, "UTC_START", "%s", "2020-01-02-03:04:05");
        set_header(header, "BEAM", "%d", 7);
    });
    std::vector<uint8_t> empty;
    ring.write_data(empty);

    RingReader source(static_cast<std::uint32_t>(ring.key()));
    CHECK(source.nchans() == 4);
    CHECK(source.sample_type() == psrio::SampleType::kUInt8);
    CHECK(source.bytes_per_sample() == 4);
    CHECK(source.fch1() == 1475.0);
    CHECK(source.foff() == -50.0);
    CHECK(source.spectra_rate() == 1.0e6 / 64.0);
    CHECK(source.tsamp() == 64.0 / 1.0e6);
    CHECK(source.utc_start() == expected_utc());
    CHECK(source.beam() == 7);
}

TEST_CASE("PSRDADA read returns the byte pattern then end of data",
          "[psrdada]") {
    constexpr SizeType kNChan  = 4;
    constexpr SizeType kNSamps = 16;
    auto ring                  = RingPair::create(kNChan * kNSamps);
    ring.write_header([](char* header) { fill_base(header, 4, 8); });
    std::vector<uint8_t> bytes(kNChan * kNSamps);
    for (SizeType i = 0; i < bytes.size(); ++i) {
        bytes[i] = static_cast<uint8_t>(i);
    }
    ring.write_data(bytes);

    RingReader source(static_cast<std::uint32_t>(ring.key()));
    std::vector<uint8_t> got(bytes.size(), 0);
    CHECK(read_into(source, got, kNSamps) == kNSamps);
    CHECK(got == bytes);
    std::vector<uint8_t> extra(bytes.size(), 7);
    CHECK(read_into(source, extra, kNSamps) == 0);
}

TEST_CASE("PSRDADA 4-bit samples use a packed stride", "[psrdada]") {
    constexpr int kNChan       = 8;
    constexpr SizeType kStride = 4; // 8 channels * 4 bits
    constexpr SizeType kNSamps = 10;
    auto ring                  = RingPair::create(kStride * kNSamps);
    ring.write_header([](char* header) { fill_base(header, kNChan, 4); });
    std::vector<uint8_t> bytes(kStride * kNSamps);
    for (SizeType i = 0; i < bytes.size(); ++i) {
        bytes[i] = static_cast<uint8_t>(0xA0 + i);
    }
    ring.write_data(bytes);

    RingReader source(static_cast<std::uint32_t>(ring.key()));
    CHECK(source.sample_type() == psrio::SampleType::kUInt4);
    CHECK(source.bytes_per_sample() == kStride);
    std::vector<uint8_t> got(bytes.size(), 0);
    CHECK(read_into(source, got, kNSamps) == kNSamps);
    CHECK(got == bytes);
}

TEST_CASE("PSRDADA fswap reverses 8-bit channels", "[psrdada]") {
    auto ring = RingPair::create(4);
    ring.write_header([](char* header) { fill_base(header, 2, 8); });
    std::vector<uint8_t> bytes{1, 2, 3, 4};
    ring.write_data(bytes);

    RingReader source(static_cast<std::uint32_t>(ring.key()));
    source.set_fswap(true);
    std::vector<uint8_t> got(4, 0);
    CHECK(read_into(source, got, 2) == 2);
    CHECK(got == std::vector<uint8_t>{2, 1, 4, 3});
}

TEST_CASE("PSRDADA rejects non-intensity headers and a missing ring",
          "[psrdada]") {
    auto expect_invalid = [](const std::function<void(char*)>& fill) {
        auto ring = RingPair::create(0);
        ring.write_header(fill);
        REQUIRE_THROWS_AS(RingReader(static_cast<std::uint32_t>(ring.key())),
                          std::runtime_error);
    };

    expect_invalid([](char* header) {
        fill_base(header, 4, 8);
        set_header(header, "NPOL", "%d", 2);
    });
    expect_invalid([](char* header) {
        fill_base(header, 4, 8);
        set_header(header, "ORDER", "%s", "FT");
    });
    expect_invalid([](char* header) {
        fill_base(header, 4, 8);
        set_header(header, "NDIM", "%d", 2);
    });
    expect_invalid([](char* header) {
        set_header(header, "NBIT", "%d", 8);
        set_header(header, "NPOL", "%d", 1);
        set_header(header, "TSAMP", "%f", 64.0);
        set_header(header, "CFREQ", "%f", 1400.0);
        set_header(header, "BANDWIDTH", "%f", -200.0);
        set_header(header, "UTC_START", "%s", "2020-01-02-03:04:05");
    });

    std::mt19937 rng{std::random_device{}()};
    std::uniform_int_distribution<int> dist(0x1000, 0x7ffe);
    bool threw = false;
    for (int attempt = 0; attempt < 32; ++attempt) {
        const auto key = static_cast<key_t>(dist(rng) & ~1);
        ipcio_t probe  = IPCIO_INIT;
        if (ipcio_connect(&probe, key) == 0) {
            ipcio_disconnect(&probe);
            continue;
        }
        REQUIRE_THROWS_AS(RingReader(static_cast<std::uint32_t>(key)),
                          std::runtime_error);
        threw = true;
        break;
    }
    REQUIRE(threw);
}

TEST_CASE("Pipeline reads one PSRDADA observation and writes a candidate",
          "[psrdada]") {
    constexpr SizeType kNChans     = 32;
    constexpr SizeType kNSamps     = 8192;
    constexpr SizeType kPulseStart = 4000;
    constexpr SizeType kPulseWidth = 4;
    constexpr uint8_t kBaseline    = 100;
    constexpr uint8_t kAmplitude   = 60;

    std::vector<uint8_t> bytes(kNChans * kNSamps, kBaseline);
    for (SizeType t = kPulseStart; t < kPulseStart + kPulseWidth; ++t) {
        for (SizeType c = 0; c < kNChans; ++c) {
            bytes[(t * kNChans) + c] =
                static_cast<uint8_t>(kBaseline + kAmplitude);
        }
    }

    auto ring = RingPair::create(bytes.size());
    ring.write_header([](char* header) {
        set_header(header, "NCHAN", "%d", 32);
        set_header(header, "NBIT", "%d", 8);
        set_header(header, "NPOL", "%d", 1);
        set_header(header, "NDIM", "%d", 1);
        set_header(header, "ORDER", "%s", "TF");
        set_header(header, "CFREQ", "%f", 1384.5);
        set_header(header, "BANDWIDTH", "%f", -32.0);
        set_header(header, "TSAMP", "%f", 1000.0);
        set_header(header, "UTC_START", "%s", "2020-01-02-03:04:05");
        set_header(header, "BEAM", "%d", 1);
    });
    ring.write_data(bytes);

    const auto out_dir = frost::test::unique_temp_dir("psrdada_e2e");

    frost::config::PipelineConfig cfg;
    cfg.rfi.broad.enabled        = false;
    cfg.rfi.narrow.enabled       = false;
    cfg.dedisp.dm_min            = 0.0F;
    cfg.dedisp.dm_max            = 50.0F;
    cfg.dedisp.dm_tol            = 1.25F;
    cfg.dedisp.dm_pulse_width    = 1000.0F;
    cfg.search.boxcar_max        = 32;
    cfg.search.detect_thresh     = 5.0F;
    cfg.search.cand_sep_time     = 5;
    cfg.search.cand_sep_filter   = 2;
    cfg.search.cand_sep_dm       = 1'000'000;
    cfg.search.cand_sep_dm_smear = 0.0F; // trial-count merge only
    cfg.search.max_giant_rate    = 0.0F;
    cfg.nsamps_gulp              = kNSamps;
    cfg.baseline_length          = 1.0F;
    cfg.output.output_dir        = out_dir.string();
    cfg.async_io                 = false;

    {
        psrio::BlockSource source(
            RingReader(static_cast<std::uint32_t>(ring.key())));
        CHECK(source.fch1() == 1400.0);
        CHECK(source.foff() == -1.0);
        auto dedisperser = frost::dedisp::make_dmt_fdmt_plan(
            frost::pipeline::make_dedisp_plan_config(cfg, source));
        REQUIRE(kNSamps > dedisperser.get_dt_max() + cfg.search.boxcar_max);
        frost::pipeline::Pipeline pipeline(cfg, source);
        SizeType gulps = 0;
        while (pipeline.execute() != 0) {
            ++gulps;
        }
        CHECK(gulps == 1);
        CHECK(pipeline.profile().samples_read == kNSamps);
    }

    const auto cand_path = out_dir / "candidates.cand";
    std::ifstream cand_file(cand_path);
    REQUIRE(cand_file.is_open());
    std::string line;
    std::vector<std::string> lines;
    while (std::getline(cand_file, line)) {
        if (!line.empty()) {
            lines.push_back(line);
        }
    }
    REQUIRE_FALSE(lines.empty());
    float best_snr           = -1.0F;
    SizeType best_samp_idx   = 0;
    SizeType best_filter_idx = 0;
    for (const auto& row : lines) {
        std::istringstream iss(row);
        float snr           = 0.0F;
        SizeType samp_idx   = 0;
        float time          = 0.0F;
        SizeType filter_idx = 0;
        iss >> snr >> samp_idx >> time >> filter_idx;
        if (snr > best_snr) {
            best_snr        = snr;
            best_samp_idx   = samp_idx;
            best_filter_idx = filter_idx;
        }
    }
    CHECK(best_snr > cfg.search.detect_thresh);
    CHECK(best_filter_idx == 2);
    CHECK(best_samp_idx >= kPulseStart);
    CHECK(best_samp_idx < kPulseStart + kPulseWidth + cfg.search.boxcar_max);
    std::filesystem::remove_all(out_dir);
}
