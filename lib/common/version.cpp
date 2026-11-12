#include <frost/common/version.hpp>

#include "frost_build_info.hpp"

namespace frost {

const BuildInfo& build_info() noexcept {
    static constexpr BuildInfo kInfo{
        .version        = detail::kFrostVersion,
        .revision       = detail::kFrostRevision,
        .dmt_revision   = detail::kDmtRevision,
        .psrio_revision = detail::kPsRioRevision,
        .compiler       = detail::kCompiler,
        .build_type     = detail::kBuildType,
        .gpu_toolchain  = detail::kGpuToolchain,
        .native_arch    = detail::kNativeArch,
        .cpu_arch       = detail::kCpuArch,
    };
    return kInfo;
}

} // namespace frost
