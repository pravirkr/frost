cmake_minimum_required(VERSION 3.21...3.30)

project(
  frost
  VERSION 0.1.0
  LANGUAGES CXX
  DESCRIPTION "FDMT-accelerated Radio Optimal Search for Transients"
)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Compiler version requirements
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
  if(CMAKE_CXX_COMPILER_VERSION VERSION_LESS 11.0)
    message(
      FATAL_ERROR
        "GCC 11.0 or higher is required for C++20 support. Found: ${CMAKE_CXX_COMPILER_VERSION}"
    )
  endif()
elseif(CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
  if(CMAKE_CXX_COMPILER_VERSION VERSION_LESS 17.0)
    message(
      FATAL_ERROR
        "Clang 17.0 or higher is required for C++20 support. Found: ${CMAKE_CXX_COMPILER_VERSION}"
    )
  endif()
else()
  message(
    WARNING
      "Compiler '${CMAKE_CXX_COMPILER_ID}' is not officially tested. Proceed at your own risk."
  )
endif()

# Configuration options
# Tests and install rules default on only when frost is the top-level project,
# so a parent that adds frost with add_subdirectory or CPM gets neither.
option(FROST_BUILD_TESTING "Build C++ tests" ${PROJECT_IS_TOP_LEVEL})
option(FROST_INSTALL "Generate frost's install and package rules" ${PROJECT_IS_TOP_LEVEL})
# A self-contained prefix: also install the fetched packages that an installed
# frost needs (dmt, psrio, fmt, PSRDADA). Packagers that supply them turn it off.
option(FROST_INSTALL_BUNDLED_DEPS "Install the fetched dependencies an installed frost needs" ON)
option(FROST_ENABLE_COVERAGE "Enable coverage reporting" OFF)
option(FROST_ENABLE_IPO "Enable interprocedural optimization (LTO) on Release builds" OFF)
option(FROST_ENABLE_NATIVE_ARCH "Use -march=native in Release builds" ON)
set(FROST_CPU_ARCH
    ""
    CACHE STRING "Release -march target; overrides native when set"
)
set(FROST_DEBUG_OPTIMIZATION
    "-O0"
    CACHE STRING "Debug optimization (-O1 for sanitizers)"
)
option(FROST_WARNINGS_AS_ERRORS "Treat warnings in frost library/CLI C++ sources as errors" OFF)
option(FROST_USE_CCACHE "Use ccache when available, unless a launcher is already set" ON)
option(FROST_USE_SYSTEM_DEPS "Prefer compatible system packages over pinned bundled dependencies"
       OFF
)
set(_frost_default_psrdada OFF)
if(UNIX AND NOT APPLE)
  set(_frost_default_psrdada ON)
endif()
option(FROST_WITH_PSRDADA "Enable live PSRDADA ring-buffer input (Linux)"
       ${_frost_default_psrdada}
)
unset(_frost_default_psrdada)
# BUILD_SHARED_LIBS is a standard CMake option (ON = shared frost, OFF = static frost).
option(BUILD_SHARED_LIBS "Build shared libraries" OFF)

# Set C++ standard
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
# No C++ modules: avoid dependency scans and compiler-specific modmap arguments.
set(CMAKE_CXX_SCAN_FOR_MODULES OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON) # Required for shared libraries

# Default to Release when no build type is specified (packaging-friendly).
# A parent project owns the build type.
if(PROJECT_IS_TOP_LEVEL
   AND NOT CMAKE_BUILD_TYPE
   AND NOT CMAKE_CONFIGURATION_TYPES
)
  message(STATUS "Setting build type to 'Release' as none was specified.")
  set(CMAKE_BUILD_TYPE
      Release
      CACHE STRING "Choose the type of build: Debug, Release, MinSizeRel, RelWithDebInfo." FORCE
  )
  set_property(
    CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS "Debug" "Release" "MinSizeRel" "RelWithDebInfo"
  )
endif()

# Optional LTO for Release builds.
if(CMAKE_BUILD_TYPE MATCHES "Release" AND FROST_ENABLE_IPO)
  include(CheckIPOSupported)
  check_ipo_supported(RESULT ipo_supported)
  if(ipo_supported)
    set(CMAKE_INTERPROCEDURAL_OPTIMIZATION TRUE)
    message(STATUS "IPO/LTO enabled for Release build.")
  else()
    message(FATAL_ERROR "FROST_ENABLE_IPO=ON but IPO/LTO is not supported by this toolchain.")
  endif()
else()
  set(CMAKE_INTERPROCEDURAL_OPTIMIZATION FALSE)
endif()

# Optional system compiler cache. Never fetch it or replace a supplied launcher.
if(FROST_USE_CCACHE)
  find_program(FROST_CCACHE_PROGRAM ccache)
  if(FROST_CCACHE_PROGRAM)
    foreach(language C CXX CUDA HIP)
      if(NOT CMAKE_${language}_COMPILER_LAUNCHER)
        set(CMAKE_${language}_COMPILER_LAUNCHER "${FROST_CCACHE_PROGRAM}")
      endif()
    endforeach()
  endif()
endif()

# Project structure and paths
list(APPEND CMAKE_MODULE_PATH ${PROJECT_SOURCE_DIR}/cmake)
include(GNUInstallDirs)

include(frostCompilerOptions)

# GPU backend: FROST_GPU selects the toolchain of lib/cuda/ (CUDA or HIP).
# Private C++ macros (FROST_ENABLE_CUDA/HIP/GPU) follow from it
# (lib/CMakeLists.txt). Resolved once; dmt receives the concrete backend.
include(frostGPU)

# CPM.cmake 0.43.1 is vendored unmodified (upstream release asset, SHA-256
# 1c40fc10...2934), so configuring never downloads CPM itself. dmt and psrio
# bootstrap the same release from CPM_PATH and skip their download when the
# file there matches its hash. They get a build-tree copy: a dependency that
# pins another CPM release would redownload over that copy, never over the
# source file.
file(COPY "${PROJECT_SOURCE_DIR}/cmake/CPM.cmake" DESTINATION "${PROJECT_BINARY_DIR}/cmake/cpm")
set(CPM_PATH "${PROJECT_BINARY_DIR}/cmake/cpm")
# CPM skips loading when its cached CPM_DIRECTORY names another directory,
# assuming a parent already loaded it. Without CPMAddPackage defined in this
# run, that entry is stale (an earlier configure used another location).
if(NOT COMMAND CPMAddPackage)
  unset(CPM_DIRECTORY CACHE)
endif()
include("${CPM_PATH}/CPM.cmake")
# A directory-scope variable: it applies to frost's dependencies and leaves
# a parent project's CPM_USE_LOCAL_PACKAGES cache entry alone.
set(CPM_USE_LOCAL_PACKAGES ${FROST_USE_SYSTEM_DEPS})
if(DEFINED ENV{CPM_SOURCE_CACHE})
  set(CPM_SOURCE_CACHE
      $ENV{CPM_SOURCE_CACHE}
      CACHE PATH "Directory to cache downloaded CPM sources"
  )
endif()

# Target naming: library target is `frost`, exported alias is `frost::frost`.
set(FROST_LIBRARY_NAME ${PROJECT_NAME})
set(FROST_CMAKE_PACKAGE_DIR "${CMAKE_INSTALL_LIBDIR}/cmake/${PROJECT_NAME}")

# Add subdirectories
add_subdirectory(lib)
add_subdirectory(applications)
if(FROST_BUILD_TESTING)
  enable_testing()
  add_subdirectory(tests/cpp)
endif()

if(FROST_INSTALL)
  include(frostInstall)
endif()

# Print feature summary
include(FeatureSummary)
feature_summary(WHAT ALL)

set(_frost_modes "CPU")
if(FROST_GPU_BACKEND STREQUAL "CUDA")
  string(APPEND _frost_modes " + CUDA (${CMAKE_CUDA_ARCHITECTURES})")
elseif(FROST_GPU_BACKEND STREQUAL "HIP")
  string(APPEND _frost_modes " + HIP (${CMAKE_HIP_ARCHITECTURES})")
endif()
message(STATUS "frost build mode: ${_frost_modes} (FROST_GPU=${FROST_GPU})")
