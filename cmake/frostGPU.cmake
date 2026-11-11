# GPU toolchain of lib/cuda/ (one per build): CUDA (nvcc) or HIP (ROCm
# clang). Sets
#   FROST_GPU_BACKEND  "CUDA", "HIP" or "" (no GPU backend)
#   FROST_DMT_GPU      concrete DMT_GPU value: "CUDA", "HIP" or "OFF"
#   FROST_GPU_OPTIONS  interface target with the GPU compile options (if any)
#   FROST_GPU_LIBRARIES  the GPU runtime and thrust targets to link
# The same CUDA sources are compiled by either toolchain: lib/cuda/gpu_compat.cuh
# maps the CUDA runtime and thrust names onto HIP and rocThrust.
#
# Backend resolution happens here once; dmt is then configured with the
# concrete FROST_DMT_GPU and does not re-probe.

set(FROST_GPU
    "AUTO"
    CACHE STRING "GPU backend: AUTO (CUDA if nvcc is found, else HIP if ROCm is found, else none), \
CUDA, HIP, or OFF"
)
set_property(CACHE FROST_GPU PROPERTY STRINGS "AUTO" "CUDA" "HIP" "OFF")
if(NOT FROST_GPU MATCHES "^(AUTO|CUDA|HIP|OFF)$")
  message(FATAL_ERROR "FROST_GPU must be AUTO, CUDA, HIP or OFF. Got: '${FROST_GPU}'.")
endif()
option(FROST_GPU_CHECK_DEVICE
       "With FROST_GPU=CUDA or HIP, require a visible GPU at configure time (OFF for build-only CI)"
       ON
)

# GPU architecture(s): "native" = detect from the local GPU at build time.
set(FROST_CUDA_ARCHITECTURES
    "native"
    CACHE STRING "CUDA architectures passed to CMAKE_CUDA_ARCHITECTURES (e.g. native, 80, 86;90)"
)
# HIP cannot detect without a GPU; default to the HPC parts (MI200, MI300).
set(FROST_HIP_ARCHITECTURES
    "gfx90a;gfx942"
    CACHE STRING "HIP architectures passed to CMAKE_HIP_ARCHITECTURES (e.g. gfx90a;gfx942;gfx1100)"
)

set(FROST_GPU_BACKEND "")
set(FROST_GPU_OPTIONS "")
set(FROST_GPU_LIBRARIES "")
include(CheckLanguage)

# ---------------------------------------------------------------------------
# CUDA
# ---------------------------------------------------------------------------
if(FROST_GPU STREQUAL "CUDA" OR FROST_GPU STREQUAL "AUTO")
  # nvcc usually lives outside the default PATH; seed CUDACXX before check_language.
  if(NOT DEFINED CMAKE_CUDA_COMPILER AND NOT DEFINED ENV{CUDACXX})
    find_program(
      FROST_NVCC_EXECUTABLE nvcc
      HINTS ENV
            CUDA_PATH
            ENV
            CUDA_HOME
            ENV
            CUDA_ROOT
            /usr/local/cuda
            /opt/cuda
      PATH_SUFFIXES bin
    )
    if(FROST_NVCC_EXECUTABLE)
      set(ENV{CUDACXX} "${FROST_NVCC_EXECUTABLE}")
    endif()
  endif()
  check_language(CUDA)
  if(CMAKE_CUDA_COMPILER)
    enable_language(CUDA)
    set(FROST_GPU_BACKEND "CUDA")
  elseif(FROST_GPU STREQUAL "CUDA")
    message(FATAL_ERROR "FROST_GPU=CUDA but no CUDA compiler (nvcc) was found. "
                        "Install CUDA >= 12.2 or use FROST_GPU=AUTO/OFF."
    )
  endif()
endif()

if(FROST_GPU_BACKEND STREQUAL "CUDA")
  if(FROST_GPU STREQUAL "CUDA" AND FROST_GPU_CHECK_DEVICE)
    # Explicitly required: verify a driver/GPU is present (not just nvcc).
    find_program(FROST_NVIDIA_SMI nvidia-smi)
    if(NOT FROST_NVIDIA_SMI)
      message(FATAL_ERROR "FROST_GPU=CUDA but nvidia-smi was not found. An NVIDIA driver and GPU "
                          "are required (or set FROST_GPU_CHECK_DEVICE=OFF)."
      )
    endif()
    execute_process(
      COMMAND ${FROST_NVIDIA_SMI} --query-gpu=name --format=csv,noheader
      OUTPUT_VARIABLE _frost_gpu_names
      RESULT_VARIABLE _frost_gpu_status
      OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET
    )
    if(NOT _frost_gpu_status EQUAL 0 OR _frost_gpu_names STREQUAL "")
      message(FATAL_ERROR "FROST_GPU=CUDA but nvidia-smi reports no usable GPU. Check driver "
                          "installation (or set FROST_GPU_CHECK_DEVICE=OFF)."
      )
    endif()
    message(STATUS "FROST_GPU=CUDA: detected GPU(s): ${_frost_gpu_names}")
  endif()

  if(CMAKE_CUDA_COMPILER_VERSION VERSION_LESS 12.2.0)
    message(FATAL_ERROR "CUDA >= 12.2 is required. Found: ${CMAKE_CUDA_COMPILER_VERSION}")
  endif()
  message(STATUS "Found CUDA ${CMAKE_CUDA_COMPILER_VERSION}.")

  # Find CUDA Toolkit for proper include dirs and linking
  find_package(CUDAToolkit REQUIRED)
  set(FROST_GPU_LIBRARIES CUDA::cuda_driver CUDA::cudart)

  set(CMAKE_CUDA_STANDARD ${CMAKE_CXX_STANDARD})
  set(CMAKE_CUDA_STANDARD_REQUIRED ON)
  set(CMAKE_CUDA_EXTENSIONS OFF)

  set(FROST_GPU_OPTIONS ${PROJECT_NAME}_gpu_options)
  add_library(${FROST_GPU_OPTIONS} INTERFACE)
  target_compile_options(
    ${FROST_GPU_OPTIONS}
    INTERFACE $<$<COMPILE_LANGUAGE:CUDA>:
              -Wno-pedantic
              --expt-extended-lambda
              --expt-relaxed-constexpr
              -Xcompiler=-Wall,-Wextra
              $<$<CONFIG:Debug>:-G;-g;-O0>
              $<$<CONFIG:Release>:-O3;-use_fast_math;-DNDEBUG>
              $<$<CONFIG:RelWithDebInfo>:-O2;-g;-lineinfo;-use_fast_math;-DNDEBUG>
              $<$<CONFIG:MinSizeRel>:-O2;-use_fast_math;-DNDEBUG>
              >
  )
  set(CMAKE_CUDA_ARCHITECTURES ${FROST_CUDA_ARCHITECTURES})
  message(STATUS "CUDA Architectures: ${CMAKE_CUDA_ARCHITECTURES}")
  # Disable response files for better IDE integration
  set(CMAKE_CUDA_USE_RESPONSE_FILE_FOR_INCLUDES OFF)
endif()

# ---------------------------------------------------------------------------
# HIP
# ---------------------------------------------------------------------------
if(FROST_GPU_BACKEND STREQUAL "" AND (FROST_GPU STREQUAL "HIP" OR FROST_GPU STREQUAL "AUTO"))
  if(CMAKE_VERSION VERSION_LESS 3.21)
    if(FROST_GPU STREQUAL "HIP")
      message(FATAL_ERROR "FROST_GPU=HIP requires CMake >= 3.21 (found ${CMAKE_VERSION}).")
    endif()
  else()
    # ROCm's clang is outside the default PATH; seed HIPCXX before check_language.
    set(_frost_rocm_roots $ENV{ROCM_PATH} $ENV{HIP_PATH} /opt/rocm)
    if(NOT DEFINED CMAKE_HIP_COMPILER AND NOT DEFINED ENV{HIPCXX})
      find_program(
        FROST_HIP_CLANG clang++
        HINTS ${_frost_rocm_roots}
        PATH_SUFFIXES llvm/bin
        NO_DEFAULT_PATH
      )
      if(FROST_HIP_CLANG)
        set(ENV{HIPCXX} "${FROST_HIP_CLANG}")
      endif()
    endif()
    # Without a GPU, CMake cannot detect the architectures: always give them.
    if(NOT DEFINED CMAKE_HIP_ARCHITECTURES)
      set(CMAKE_HIP_ARCHITECTURES ${FROST_HIP_ARCHITECTURES})
    endif()
    check_language(HIP)
    if(CMAKE_HIP_COMPILER)
      enable_language(HIP)
      set(FROST_GPU_BACKEND "HIP")
    elseif(FROST_GPU STREQUAL "HIP")
      message(FATAL_ERROR "FROST_GPU=HIP but no HIP compiler (ROCm clang) was found. Install "
                          "ROCm >= 6.2 (set ROCM_PATH) or use FROST_GPU=AUTO/OFF."
      )
    endif()
  endif()
endif()

if(FROST_GPU_BACKEND STREQUAL "HIP")
  list(APPEND CMAKE_PREFIX_PATH ${_frost_rocm_roots})
  find_package(hip CONFIG REQUIRED)
  if(hip_VERSION VERSION_LESS 6.2)
    message(FATAL_ERROR "ROCm/HIP >= 6.2 is required. Found: ${hip_VERSION}")
  endif()
  message(STATUS "Found HIP ${hip_VERSION} (${CMAKE_HIP_COMPILER}).")
  find_package(rocthrust CONFIG REQUIRED)
  set(FROST_GPU_LIBRARIES hip::host roc::rocthrust)
  # libhipcxx (cuda::std on AMD) is optional: frost falls back to std::span
  # (gpu_compat.cuh) when it is absent.
  find_package(libhipcxx CONFIG QUIET)
  if(libhipcxx_FOUND)
    list(APPEND FROST_GPU_LIBRARIES libhipcxx::libhipcxx)
    message(STATUS "Using libhipcxx ${libhipcxx_VERSION} for cuda::std.")
  endif()

  if(FROST_GPU STREQUAL "HIP" AND FROST_GPU_CHECK_DEVICE)
    find_program(
      FROST_ROCMINFO rocminfo
      HINTS ${_frost_rocm_roots}
      PATH_SUFFIXES bin
    )
    set(_frost_amd_agents "")
    if(FROST_ROCMINFO)
      execute_process(
        COMMAND ${FROST_ROCMINFO}
        OUTPUT_VARIABLE _frost_rocminfo
        RESULT_VARIABLE _frost_rocminfo_status
        ERROR_QUIET
      )
      if(_frost_rocminfo_status EQUAL 0)
        string(REGEX MATCHALL "gfx[0-9a-f]+" _frost_amd_agents "${_frost_rocminfo}")
        list(REMOVE_DUPLICATES _frost_amd_agents)
      endif()
    endif()
    if(_frost_amd_agents STREQUAL "")
      message(FATAL_ERROR "FROST_GPU=HIP but rocminfo reports no AMD GPU. Check the ROCm "
                          "installation (or set FROST_GPU_CHECK_DEVICE=OFF)."
      )
    endif()
    message(STATUS "FROST_GPU=HIP: detected GPU(s): ${_frost_amd_agents}")
  endif()

  set(CMAKE_HIP_STANDARD ${CMAKE_CXX_STANDARD})
  set(CMAKE_HIP_STANDARD_REQUIRED ON)
  set(CMAKE_HIP_EXTENSIONS OFF)

  set(FROST_GPU_OPTIONS ${PROJECT_NAME}_gpu_options)
  add_library(${FROST_GPU_OPTIONS} INTERFACE)
  target_compile_options(
    ${FROST_GPU_OPTIONS}
    INTERFACE $<$<COMPILE_LANGUAGE:HIP>:
              -Wall
              -Wextra
              $<$<CONFIG:Debug>:-O0;-g>
              $<$<CONFIG:Release>:-O3;-ffast-math;-DNDEBUG>
              $<$<CONFIG:RelWithDebInfo>:-O2;-g;-ffast-math;-DNDEBUG>
              $<$<CONFIG:MinSizeRel>:-O2;-ffast-math;-DNDEBUG>
              >
  )
  message(STATUS "HIP Architectures: ${CMAKE_HIP_ARCHITECTURES}")
endif()

if(FROST_GPU_BACKEND STREQUAL "CUDA")
  set(FROST_DMT_GPU "CUDA")
elseif(FROST_GPU_BACKEND STREQUAL "HIP")
  set(FROST_DMT_GPU "HIP")
else()
  set(FROST_DMT_GPU "OFF")
endif()

# dmt is given the concrete backend. It must not require a device that frost
# AUTO already accepted without one, and it must not repeat an explicit check
# that failed above. Explicit CUDA/HIP still forward the user's check flag.
if(FROST_GPU STREQUAL "CUDA" OR FROST_GPU STREQUAL "HIP")
  set(FROST_DMT_GPU_CHECK_DEVICE ${FROST_GPU_CHECK_DEVICE})
else()
  set(FROST_DMT_GPU_CHECK_DEVICE OFF)
endif()

if(FROST_GPU_BACKEND STREQUAL "")
  if(FROST_GPU STREQUAL "OFF")
    message(STATUS "GPU disabled by user (FROST_GPU=OFF).")
  else()
    message(STATUS "No CUDA or HIP compiler found. No GPU backend (FROST_GPU=AUTO).")
  endif()
endif()
