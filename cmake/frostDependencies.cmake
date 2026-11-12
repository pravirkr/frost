#=============================================================================
# frost — Dependency management (CPU + shared CPM deps)
#
# Design rules:
#   1. OpenMP, HDF5, and FFTW (single precision, required by dmt)
#      are hard system dependencies — never fetched.
#   2. OpenMP and HDF5 are internal. For a SHARED frost they are linked PRIVATE;
#      for STATIC frost they are linked PUBLIC so consumers can resolve symbols.
#      dmt follows the same rule (its static archive does not absorb FFTW).
#   3. fmt and spdlog are CPM-pinned header-only libraries (single translation
#      unit, shared formatting convention across the sister projects).
#   4. CLI11, xsimd, tomlplusplus, HighFive, and psrio are
#      header-only internal dependencies -> BUILD_INTERFACE only.
#      psrio is the intensity and baseband reader. FBH5 and PSRDADA headers
#      are included directly; frost already links HighFive and the CPM
#      psrdada target, so PSRIO_WITH_HDF5 and PSRIO_WITH_PSRDADA stay off
#      (the latter looks up a system libpsrdada, not this build's target).
#   5. dmt (FDMT) is a CPM-pinned C++ library. Its own fmt/spdlog fetches
#      reuse the packages above when the versions match.
#=============================================================================

# -----------------------------------------------------------------------
# 1. System dependencies — must be supplied by the user, never fetched
# -----------------------------------------------------------------------
message(STATUS "Searching for required system dependencies...")
find_package(OpenMP REQUIRED COMPONENTS CXX)
find_package(HDF5 REQUIRED) # For HighFive

# -----------------------------------------------------------------------
# 2. CPM-managed dependencies (pinned versions, bundled by default)
# -----------------------------------------------------------------------
if(FROST_USE_SYSTEM_DEPS)
  message(STATUS "FROST_USE_SYSTEM_DEPS=ON: compatible system packages may be used.")
else()
  message(STATUS "FROST_USE_SYSTEM_DEPS=OFF: using pinned CPM dependencies.")
endif()

# fmt — keep version in sync with spdlog's expected fmt release.
CPMAddPackage(
  NAME fmt
  VERSION 12.1.0
  URL https://github.com/fmtlib/fmt/archive/refs/tags/12.1.0.tar.gz
  OPTIONS "FMT_INSTALL ON" "FMT_HEADER_ONLY ON"
  EXCLUDE_FROM_ALL YES
  SYSTEM YES
)

# spdlog — use the fmt package above (header-only external fmt).
CPMAddPackage(
  NAME spdlog
  VERSION 1.17.0
  URL https://github.com/gabime/spdlog/archive/refs/tags/v1.17.0.tar.gz
  OPTIONS "SPDLOG_INSTALL ON" "SPDLOG_FMT_EXTERNAL_HO ON" "SPDLOG_BUILD_SHARED OFF"
  EXCLUDE_FROM_ALL YES
  SYSTEM YES
)

if(NOT TARGET fmt::fmt-header-only)
  message(FATAL_ERROR "fmt::fmt-header-only target not available after CPM setup.")
endif()
if(NOT TARGET spdlog::spdlog_header_only)
  message(FATAL_ERROR "spdlog::spdlog_header_only target not available after CPM setup.")
endif()

# dmt (FDMT and analytical boxcar search), pinned to v0.8.0.
# Per-call FDMTBoxcar kill masks, candidate frontier and drain() need 0.8.
# FFTW single precision is found by dmt and is not fetched.
# dmt selects its backend with DMT_GPU=AUTO|CUDA|HIP|OFF and rejects a
# defined DMT_CUDA at configure time, so clear any such cache entry.
# frostGPU.cmake already resolved FROST_GPU to a concrete backend. Forward
# that string (OFF/CUDA/HIP, never AUTO) so dmt does not probe a different
# toolchain. PSRDADA's own CUDA probe is suppressed below and must not see
# this choice.
unset(DMT_CUDA CACHE)
# Development checkouts are opt-in: -DCPM_dmt_SOURCE=/path/to/dmt.
# An adjacent checkout must not silently replace the pinned dependency.
set(FROST_DMT_REVISION v0.8.0)
set(FROST_DMT_VERSION 0.8.0)
CPMAddPackage(
  NAME dmt
  VERSION ${FROST_DMT_VERSION}
  GIT_REPOSITORY https://github.com/pravirkr/dmt.git
  GIT_TAG ${FROST_DMT_REVISION}
  OPTIONS "DMT_BUILD_PYTHON OFF"
          "DMT_BUILD_TESTING OFF"
          "DMT_BUILD_BENCHMARKS OFF"
          "DMT_BUILD_DOCS OFF"
          "DMT_GPU ${FROST_DMT_GPU}"
          "DMT_GPU_CHECK_DEVICE ${FROST_DMT_GPU_CHECK_DEVICE}"
          "DMT_CUDA_ARCHITECTURES ${FROST_CUDA_ARCHITECTURES}"
          "DMT_HIP_ARCHITECTURES ${FROST_HIP_ARCHITECTURES}"
          "DMT_ENABLE_NATIVE_ARCH ${FROST_ENABLE_NATIVE_ARCH}"
          "DMT_CPU_ARCH ${FROST_CPU_ARCH}"
          "DMT_USE_SYSTEM_DEPS ${FROST_USE_SYSTEM_DEPS}"
  EXCLUDE_FROM_ALL YES
  SYSTEM YES
)
if(NOT EXISTS "${dmt_SOURCE_DIR}/include/dmt/algorithms/fdmt_boxcar.hpp")
  message(
    FATAL_ERROR "dmt with FDMTBoxcar is required; set CPM_dmt_SOURCE to a compatible checkout"
  )
endif()
if(NOT TARGET dmt::dmt)
  message(FATAL_ERROR "dmt::dmt target not available after CPM setup.")
endif()

# PSRDADA — shared-memory ring buffer for live intensity streams (Linux).
# Pinned to 1.3.0. The library is AFL-2.1 and is linked, not vendored.
# Its CMake enables CUDA whenever nvcc is visible; frost only needs the
# CPU ring, so that find is suppressed for this subdirectory. BUILD_TESTING
# is a normal variable here (CMP0077) so PSRDADA's own add_test() calls
# are not registered with frost's CTest run.
if(FROST_WITH_PSRDADA)
  set(CMAKE_DISABLE_FIND_PACKAGE_CUDAToolkit TRUE)
  set(CMAKE_DISABLE_FIND_PACKAGE_CUDA TRUE)
  set(BUILD_TESTING OFF)
  CPMAddPackage(
    NAME psrdada
    GIT_REPOSITORY https://git.code.sf.net/p/psrdada/code
    GIT_TAG 151198ea3ec4a3a237f51e6217a5a2d7ff39194e
    EXCLUDE_FROM_ALL YES
    SYSTEM YES
  )
  unset(CMAKE_DISABLE_FIND_PACKAGE_CUDAToolkit)
  unset(CMAKE_DISABLE_FIND_PACKAGE_CUDA)
  unset(BUILD_TESTING)
  if(NOT TARGET psrdada)
    message(FATAL_ERROR "psrdada target not available after CPM setup.")
  endif()
endif()

# -----------------------------------------------------------------------
# 3. Header-only CPM dependencies (build-time only)
# -----------------------------------------------------------------------

# psrio 0.5.0 shares fmt and the GCC 11 baseline with dmt.
set(FROST_PSRIO_REVISION b2d0da516580614bf8341924e51d153f48c4d9bb) # v0.5.0
set(FROST_PSRIO_VERSION 0.5.0)
CPMAddPackage(
  NAME psrio
  VERSION ${FROST_PSRIO_VERSION}
  GIT_REPOSITORY https://github.com/pravirkr/psrio.git
  GIT_TAG ${FROST_PSRIO_REVISION}
  OPTIONS "PSRIO_BUILD_TESTS OFF" "PSRIO_WITH_HDF5 OFF" "PSRIO_WITH_PSRDADA OFF" "PSRIO_INSTALL ON"
  EXCLUDE_FROM_ALL YES
  SYSTEM YES
)
if(NOT TARGET psrio::psrio)
  message(FATAL_ERROR "psrio::psrio target not available after CPM setup.")
endif()

# Fetched packages an installed frost needs: frostConfig.cmake finds dmt and
# psrio, psrio's config finds fmt, and a PSRDADA build finds libpsrdada.
# CPM's <name>_ADDED and <name>_BINARY_DIR exist only in this directory
# scope, so record their install scripts for frostInstall.cmake. A package
# taken from the system (FROST_USE_SYSTEM_DEPS) is not ADDED and not listed.
# PSRDADA's top-level install writes package metadata to an absolute path
# outside the prefix; FindPSRDADA.cmake needs only the library and headers
# that its src/ directory installs.
foreach(frost_package IN ITEMS fmt psrio dmt)
  if(${frost_package}_ADDED)
    set_property(
      GLOBAL APPEND PROPERTY FROST_BUNDLED_INSTALL_SCRIPTS
                             "${${frost_package}_BINARY_DIR}/cmake_install.cmake"
    )
  endif()
endforeach()
unset(frost_package)
if(FROST_WITH_PSRDADA AND psrdada_ADDED)
  set_property(
    GLOBAL APPEND PROPERTY FROST_BUNDLED_INSTALL_SCRIPTS
                           "${psrdada_BINARY_DIR}/src/cmake_install.cmake"
  )
endif()
# fmt's export names its compiled library although frost uses the
# header-only target; frostInstall.cmake builds it when it installs fmt.
set_property(GLOBAL PROPERTY FROST_BUNDLED_FMT ${fmt_ADDED})
# The pinned versions are the minimums frostConfig.cmake asks for.
set_property(GLOBAL PROPERTY FROST_DMT_VERSION ${FROST_DMT_VERSION})
set_property(GLOBAL PROPERTY FROST_PSRIO_VERSION ${FROST_PSRIO_VERSION})

# -----------------------------------------------------------------------
# 3b. Other header-only CPM dependencies
# -----------------------------------------------------------------------

CPMAddPackage(
  NAME HighFive
  VERSION 3.3.0
  URL https://github.com/highfive-devs/highfive/archive/refs/tags/v3.3.0.tar.gz
  OPTIONS "HIGHFIVE_FIND_HDF5 ON"
  EXCLUDE_FROM_ALL YES
  SYSTEM YES
)
if(NOT TARGET HighFive::HighFive)
  message(FATAL_ERROR "HighFive::HighFive target not available after CPM setup.")
endif()

# xsimd (SIMD library) - Header-only
CPMAddPackage(
  NAME xsimd
  VERSION 14.0.0
  URL https://github.com/xtensor-stack/xsimd/archive/refs/tags/14.0.0.tar.gz
  DOWNLOAD_ONLY YES
  EXCLUDE_FROM_ALL YES
  SYSTEM YES
)
if(xsimd_ADDED)
  add_library(xsimd INTERFACE IMPORTED GLOBAL)
  target_include_directories(xsimd INTERFACE "${xsimd_SOURCE_DIR}/include")
elseif(NOT TARGET xsimd)
  message(FATAL_ERROR "xsimd is required but was not found or downloaded.")
endif()

CPMAddPackage(
  NAME tomlplusplus
  VERSION 3.4.0
  URL https://github.com/marzer/tomlplusplus/archive/refs/tags/v3.4.0.tar.gz
  DOWNLOAD_ONLY YES
  EXCLUDE_FROM_ALL YES
  SYSTEM YES
)
if(tomlplusplus_ADDED)
  add_library(tomlplusplus INTERFACE IMPORTED GLOBAL)
  target_include_directories(tomlplusplus INTERFACE "${tomlplusplus_SOURCE_DIR}/include")
elseif(NOT TARGET tomlplusplus)
  message(FATAL_ERROR "tomlplusplus is required but was not found or downloaded.")
endif()

CPMAddPackage(
  NAME CLI11
  VERSION 2.7.0
  URL https://github.com/CLIUtils/CLI11/archive/refs/tags/v2.7.0.tar.gz
  DOWNLOAD_ONLY YES
  EXCLUDE_FROM_ALL YES
  SYSTEM YES
)
if(CLI11_ADDED)
  add_library(CLI11::CLI11 INTERFACE IMPORTED GLOBAL)
  target_include_directories(CLI11::CLI11 INTERFACE "${CLI11_SOURCE_DIR}/include")
elseif(NOT TARGET CLI11::CLI11)
  message(FATAL_ERROR "CLI11 is required but was not found or downloaded.")
endif()
