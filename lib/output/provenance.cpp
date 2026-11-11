#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

#include <toml++/toml.h>
#include <unistd.h>

#include <frost/common/policies.hpp>
#include <frost/config/config.hpp>
#include <frost/output/provenance.hpp>
#include <frost/pipeline/memory.hpp>
#include <frost/pipeline/pipeline.hpp>
#include <frost/pipeline/profile.hpp>

#include "frost_build_info.hpp"

namespace frost::output {
class RunProvenancePlan::Impl {
public:
    Impl(std::filesystem::path path,
         const config::PipelineConfig& cfg,
         const psrio::BlockSource& source,
         std::span<const std::string> argv,
         const pipeline::MemoryCheck* memory)
        : m_path(std::move(path)) {
        m_record.insert("schema_version", 1);
        m_record.insert("started_unix_ns",
                        std::chrono::duration_cast<std::chrono::nanoseconds>(
                            std::chrono::system_clock::now().time_since_epoch())
                            .count());
        if (memory != nullptr) {
            const auto& e = memory->estimate;
            m_record.insert(
                "memory_plan",
                toml::table{
                    {"pool", memory->device_memory ? "device" : "host"},
                    {
                        "available_or_limited_bytes",
                        static_cast<int64_t>(memory->available_bytes),
                    },
                    {"estimated_bytes", static_cast<int64_t>(e.total_bytes)},
                    {"input_bytes", static_cast<int64_t>(e.input_bytes)},
                    {"rfi_bytes", static_cast<int64_t>(e.rfi_bytes)},
                    {"fdmt_bytes", static_cast<int64_t>(e.fdmt_bytes)},
                    {
                        "postprocess_bytes",
                        static_cast<int64_t>(e.postprocess_bytes),
                    },
                    {"reserve_bytes", static_cast<int64_t>(e.reserve_bytes)},
                });
        }
        m_record.insert("config_effective", toml::parse(cfg.to_toml_string()));
        toml::array command;
        for (const auto& arg : argv) {
            command.push_back(arg);
        }
        m_record.insert("argv", std::move(command));
        m_record.insert("build", toml::table{
                                     {"frost_version", detail::kFrostVersion},
                                     {"frost_revision", detail::kFrostRevision},
                                     {"dmt_revision", detail::kDmtRevision},
                                     {"psrio_revision", detail::kPsRioRevision},
                                     {"compiler", detail::kCompiler},
                                     {"build_type", detail::kBuildType},
                                     {"gpu_toolchain", detail::kGpuToolchain},
                                     {"native_arch", detail::kNativeArch},
                                     {"cpu_arch", detail::kCpuArch},
                                 });
        const auto axis = pipeline::data_frequency_axis(source);
        m_record.insert(
            "input",
            toml::table{
                {"nchans", static_cast<int64_t>(source.nchans())},
                {"sample_bits", bits_of(to_data_type(source.sample_type()))},
                {"tsamp_seconds", source.tsamp()},
                {"channel0_mhz", axis.f0},
                {"channel_step_mhz", axis.df},
                {"start_mjd", source.tstart()},
                {"utc_start_unix", static_cast<int64_t>(source.utc_start())},
                {"channel_order", "original reader order after fswap"},
                {
                    "sample_reference",
                    "absolute trailing-window endpoint; "
                    "arrival at searched band bottom edge",
                },
            });
    }
    void execute(std::string_view status,
                 const pipeline::RunProfile& p,
                 std::string_view error) {
        m_record.insert_or_assign("status", status);
        m_record.insert_or_assign(
            "updated_unix_ns",
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::system_clock::now().time_since_epoch())
                .count());
        m_record.insert_or_assign("error", error);
        m_record.insert_or_assign(
            "run",
            toml::table{
                {"threads", p.threads},
                {"gulps", static_cast<int64_t>(p.gulps)},
                {"samples_read", static_cast<int64_t>(p.samples_read)},
                {"samples_searched", static_cast<int64_t>(p.samples_searched)},
                {"dropped_global", static_cast<int64_t>(p.dropped_global)},
                {"dropped_per_dm", static_cast<int64_t>(p.dropped_per_dm)},
                {"candidates", static_cast<int64_t>(p.candidates)},
                {"wall_seconds", p.wall_s},
            });
        m_record.insert_or_assign(
            "memory",
            toml::table{
                {
                    "input_reserved_bytes",
                    static_cast<int64_t>(p.input_reserved_bytes),
                },
                {
                    "host_rfi_reserved_bytes",
                    static_cast<int64_t>(p.rfi_reserved_bytes),
                },
                {
                    "device_rfi_reserved_bytes",
                    static_cast<int64_t>(p.device_rfi_reserved_bytes),
                },
                {
                    "device_upload_reserved_bytes",
                    static_cast<int64_t>(p.device_upload_reserved_bytes),
                },
                {
                    "engine_reported_bytes",
                    static_cast<int64_t>(p.engine_reported_bytes),
                },
                {
                    "postprocess_reserved_bytes",
                    static_cast<int64_t>(p.postprocess_reserved_bytes),
                },
                {
                    "peak_pending_candidates",
                    static_cast<int64_t>(p.peak_pending_candidates),
                },
            });
        if (status == "failed") {
            m_record.erase("run");
            m_record.erase("memory");
        }
        if (!m_path.parent_path().empty()) {
            std::filesystem::create_directories(m_path.parent_path());
        }
        auto tmp = m_path;
        tmp += ".tmp." + std::to_string(static_cast<long>(::getpid()));
        try {
            std::ofstream out(tmp);
            out << m_record << '\n';
            out.close();
            if (!out) {
                throw std::runtime_error("Cannot write run provenance: " +
                                         m_path.string());
            }
            std::filesystem::rename(tmp, m_path);
        } catch (...) {
            std::error_code ec;
            std::filesystem::remove(tmp, ec);
            throw;
        }
    }

private:
    std::filesystem::path m_path;
    toml::table m_record;
};
RunProvenancePlan::RunProvenancePlan(std::filesystem::path path,
                                     const config::PipelineConfig& cfg,
                                     const psrio::BlockSource& source,
                                     std::span<const std::string> argv,
                                     const pipeline::MemoryCheck* memory)
    : m_impl(
          std::make_unique<Impl>(std::move(path), cfg, source, argv, memory)) {}
RunProvenancePlan::~RunProvenancePlan()                            = default;
RunProvenancePlan::RunProvenancePlan(RunProvenancePlan&&) noexcept = default;
RunProvenancePlan&
RunProvenancePlan::operator=(RunProvenancePlan&&) noexcept = default;
void RunProvenancePlan::execute(std::string_view status,
                                const pipeline::RunProfile& profile,
                                std::string_view error) {
    m_impl->execute(status, profile, error);
}
} // namespace frost::output
