#pragma once

#include <string_view>

namespace frost {

// Identity of the linked frost library, for `frost --version`, bug reports
// and provenance. The values describe the library build, not the headers a
// consumer compiled against.
struct BuildInfo {
    // Release version from the CMake project, "MAJOR.MINOR.PATCH".
    std::string_view version;
    // Source commit. "-dirty" marks local changes; "unknown" means the
    // source was built without git metadata.
    std::string_view revision;
    std::string_view dmt_revision;
    std::string_view psrio_revision;
    std::string_view compiler;
    std::string_view build_type;
    std::string_view gpu_toolchain;
    std::string_view native_arch;
    std::string_view cpu_arch;
};

[[nodiscard]] const BuildInfo& build_info() noexcept;

} // namespace frost
