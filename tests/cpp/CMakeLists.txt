# Catch2 test framework (CPM).
CPMFindPackage(
  NAME Catch2
  VERSION 3.16.0
  GITHUB_REPOSITORY catchorg/Catch2
  OPTIONS "CATCH_INSTALL_DOCS OFF" "CATCH_INSTALL_EXTRAS OFF"
)
if(NOT Catch2_FOUND AND NOT TARGET Catch2::Catch2WithMain)
  message(
    FATAL_ERROR "Catch2 is required for FROST_BUILD_TESTING=ON but was not found or downloaded."
  )
endif()

list(APPEND CMAKE_MODULE_PATH ${Catch2_DIR})
list(APPEND CMAKE_MODULE_PATH ${Catch2_SOURCE_DIR}/extras)

include(CTest)
# Imported system targets created under lib/ are directory-scoped. API test
# fixtures use these explicitly rather than gaining private library access.
find_package(OpenMP REQUIRED COMPONENTS CXX)
find_package(HDF5 REQUIRED)
# Compile each public header in isolation using only public usage requirements.
# This catches missing includes and backend/private leaks that an umbrella
# include or a white-box test can hide. No runtime test is needed.
file(GLOB_RECURSE FROST_PUBLIC_HEADERS CONFIGURE_DEPENDS
     "${PROJECT_SOURCE_DIR}/include/frost/*.hpp"
)
set(_frost_header_checks "")
foreach(header IN LISTS FROST_PUBLIC_HEADERS)
  file(RELATIVE_PATH _header_path "${PROJECT_SOURCE_DIR}/include" "${header}")
  string(REPLACE "/" "_" _header_name "${_header_path}")
  set(_header_source "${CMAKE_CURRENT_BINARY_DIR}/headers/${_header_name}.cpp")
  file(
    GENERATE
    OUTPUT "${_header_source}"
    CONTENT "#include <${_header_path}>\n"
  )
  list(APPEND _frost_header_checks "${_header_source}")
endforeach()
add_library(frost_public_headers OBJECT ${_frost_header_checks})
target_link_libraries(frost_public_headers PRIVATE frost::frost)

# API tests have the same include boundary as the CLI/downstream consumer.
# White-box tests explicitly opt into frost::internal and toolkit details.
include(Catch)

# Create a boundary-specific Catch2 executable with CPU/GPU discovery.
function(frost_add_test_suite target suite)
  file(GLOB sources CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/${suite}/*.cpp")
  if(suite STREQUAL "api" AND NOT FROST_GPU_BACKEND)
    list(REMOVE_ITEM sources "${CMAKE_CURRENT_SOURCE_DIR}/api/gpu_parity_test.cpp")
  endif()
  if(suite STREQUAL "api" AND NOT FROST_WITH_PSRDADA)
    list(REMOVE_ITEM sources "${CMAKE_CURRENT_SOURCE_DIR}/api/psrdada_test.cpp")
  endif()
  if(suite STREQUAL "internal" AND FROST_GPU_BACKEND)
    file(GLOB device_sources CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/internal/*.cu")
    set_source_files_properties(${device_sources} PROPERTIES LANGUAGE ${FROST_GPU_BACKEND})
    list(APPEND sources ${device_sources})
  endif()
  add_executable(${target} ${sources})
  add_dependencies(${target} frost_app)
  target_include_directories(${target} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
  target_link_libraries(
    ${target}
    PRIVATE frost::frost
            Catch2::Catch2WithMain
            ${FROST_COMPILE_OPTIONS}
            tomlplusplus
            OpenMP::OpenMP_CXX
            spdlog::spdlog_header_only
            HighFive::HighFive
            HDF5::HDF5
  )
  if(FROST_WITH_PSRDADA)
    target_link_libraries(${target} PRIVATE psrdada)
  endif()
  if(suite STREQUAL "internal")
    target_link_libraries(${target} PRIVATE frost::internal)
  endif()
  target_compile_definitions(
    ${target} PRIVATE FROST_APP_PATH="$<TARGET_FILE:frost_app>"
                      FROST_TEST_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/data"
                      FROST_PROJECT_VERSION="${PROJECT_VERSION}"
                      FROST_CLI_REFERENCE="${PROJECT_SOURCE_DIR}/docs/reference/cli.md"
  )
  frost_enable_coverage(${target})
  catch_discover_tests(
    ${target}
    TEST_SPEC
    "~[gpu]"
    TEST_PREFIX
    "${suite}:"
    PROPERTIES
    LABELS
    "cpu"
    ENVIRONMENT
    "OMP_NUM_THREADS=4"
  )
  if(FROST_GPU_BACKEND)
    string(TOLOWER ${FROST_GPU_BACKEND} gpu_name)
    catch_discover_tests(
      ${target}
      TEST_SPEC
      "[gpu]"
      TEST_PREFIX
      "${gpu_name}:${suite}:"
      PROPERTIES
      LABELS
      "gpu"
      ENVIRONMENT
      "OMP_NUM_THREADS=4"
    )
  endif()
endfunction()
frost_add_test_suite(frost_api_tests api)
frost_add_test_suite(frost_internal_tests internal)
# Aggregate target building both test suites.
add_custom_target(frost_tests DEPENDS frost_api_tests frost_internal_tests)

add_test(NAME architecture COMMAND env SKIP_COMPILE=1
                                   ${PROJECT_SOURCE_DIR}/scripts/check_architecture.sh
)
set_tests_properties(architecture PROPERTIES LABELS "cpu;architecture")
