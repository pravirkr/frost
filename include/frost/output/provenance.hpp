#pragma once
#include <filesystem>
#include <memory>
#include <span>
#include <string>
#include <string_view>

#include <psrio/block_source.hpp>

#include <frost/config/config.hpp>
#include <frost/pipeline/memory.hpp>
#include <frost/pipeline/profile.hpp>

namespace frost::output {
/** Records effective configuration, input geometry, build identity and run
 * outcome in a companion TOML file. Each update replaces the file atomically.
 */
class RunProvenancePlan {
public:
    RunProvenancePlan(std::filesystem::path path,
                      const config::PipelineConfig& cfg,
                      const psrio::BlockSource& source,
                      std::span<const std::string> argv,
                      const pipeline::MemoryCheck* memory = nullptr);
    ~RunProvenancePlan();
    RunProvenancePlan(RunProvenancePlan&&) noexcept;
    RunProvenancePlan& operator=(RunProvenancePlan&&) noexcept;
    RunProvenancePlan(const RunProvenancePlan&)            = delete;
    RunProvenancePlan& operator=(const RunProvenancePlan&) = delete;
    void execute(std::string_view status,
                 const pipeline::RunProfile& profile,
                 std::string_view error = {});

private:
    class Impl;
    std::unique_ptr<Impl> m_impl;
};
} // namespace frost::output
