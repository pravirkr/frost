# Build-tree compiler policy. Device translation units use frostGPU.cmake;
# this target contributes host C++ options only and is never installed.
# Compiler flags (Interface target to hold global compile options).
# Host flags stay off HIP/CUDA sources: those languages get frost_gpu_options.
# Host Release builds use -O3 -ffast-math (see docs/architecture.md).
set(FROST_COMPILE_OPTIONS ${PROJECT_NAME}_compile_options)
add_library(${FROST_COMPILE_OPTIONS} INTERFACE)
set(_frost_host "$<COMPILE_LANGUAGE:CXX>")
if(FROST_CPU_ARCH)
  set(_frost_march "-march=${FROST_CPU_ARCH}")
elseif(FROST_ENABLE_NATIVE_ARCH)
  set(_frost_march "-march=native")
else()
  set(_frost_march "")
endif()
set(_frost_native_arch "$<$<CONFIG:Release>:${_frost_march}>")
target_compile_options(
  ${FROST_COMPILE_OPTIONS} INTERFACE $<${_frost_host}:-Wall;-Wextra> $<${_frost_host}:-Wpedantic>
)
target_compile_options(
  ${FROST_COMPILE_OPTIONS}
  INTERFACE $<${_frost_host}:$<$<CONFIG:Debug>:${FROST_DEBUG_OPTIMIZATION};-g>>
            $<${_frost_host}:$<$<CONFIG:Release>:-O3;-DNDEBUG;-ffast-math>>
            $<${_frost_host}:${_frost_native_arch}>
            $<${_frost_host}:$<$<CONFIG:RelWithDebInfo>:-O2;-DNDEBUG;-g>>
            $<${_frost_host}:$<$<CONFIG:MinSizeRel>:-Os;-DNDEBUG>>
)

# Clang 17 can leave correct OpenMP SIMD loops unvectorized at a portable
# CPU target. Keep that optimization remark visible without failing CI.
if(CMAKE_CXX_COMPILER_ID STREQUAL "Clang" AND CMAKE_CXX_COMPILER_VERSION VERSION_LESS 18.0)
  target_compile_options(
    ${FROST_COMPILE_OPTIONS} INTERFACE $<${_frost_host}:-Wno-error=pass-failed>
  )
endif()

# Apply strict diagnostics to product targets. Tests intentionally construct
# invalid/non-finite inputs and retain warnings without promoting them.
function(frost_enable_warnings_as_errors target)
  target_compile_options(
    ${target} PRIVATE $<$<AND:$<COMPILE_LANGUAGE:CXX>,$<BOOL:${FROST_WARNINGS_AS_ERRORS}>>:-Werror>
  )
endfunction()

# BUILD_INTERFACE keeps flags off the installed export set. PUBLIC so in-tree
# consumers (tests, CLI) inherit compile and -lgcov link flags.
function(frost_enable_coverage target)
  if(NOT FROST_ENABLE_COVERAGE)
    return()
  endif()
  if(NOT CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    message(
      FATAL_ERROR
        "Code coverage requested, but not supported for compiler ${CMAKE_CXX_COMPILER_ID}."
    )
  endif()
  target_compile_options(
    ${target}
    PUBLIC $<BUILD_INTERFACE:$<$<COMPILE_LANGUAGE:CXX>:-O0>>
           $<BUILD_INTERFACE:$<$<COMPILE_LANGUAGE:CXX>:-g>>
           $<BUILD_INTERFACE:$<$<COMPILE_LANGUAGE:CXX>:--coverage>>
  )
  target_link_options(${target} PUBLIC $<BUILD_INTERFACE:--coverage>)
endfunction()
