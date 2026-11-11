# Layout: lib/<domain> is orchestration and backend-neutral code;
# lib/cpu contains CPU implementations; lib/cuda contains CUDA/HIP sources.
# GLOB_RECURSE discovers host sources in all domains, never device code.
file(GLOB_RECURSE FROST_LIBRARY_CXX_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/*.cpp)
if(FROST_LIBRARY_CXX_SOURCES STREQUAL "")
  message(FATAL_ERROR "No C++ sources (*.cpp) found in ${CMAKE_CURRENT_SOURCE_DIR}.")
endif()
set(FROST_LIBRARY_ALL_SOURCES ${FROST_LIBRARY_CXX_SOURCES})

if(FROST_GPU_BACKEND)
  file(GLOB FROST_LIBRARY_GPU_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/cuda/*.cu)
  if(FROST_LIBRARY_GPU_SOURCES STREQUAL "")
    message(FATAL_ERROR "A GPU backend was selected, but lib/cuda has no .cu sources.")
  endif()
  if(NOT FROST_LIBRARY_GPU_SOURCES STREQUAL "")
    set_source_files_properties(
      ${FROST_LIBRARY_GPU_SOURCES} PROPERTIES LANGUAGE ${FROST_GPU_BACKEND}
    )
    list(APPEND FROST_LIBRARY_ALL_SOURCES ${FROST_LIBRARY_GPU_SOURCES})
  endif()
endif()

add_library(${FROST_LIBRARY_NAME} ${FROST_LIBRARY_ALL_SOURCES})
add_library(${PROJECT_NAME}::${FROST_LIBRARY_NAME} ALIAS ${FROST_LIBRARY_NAME})
frost_enable_warnings_as_errors(${FROST_LIBRARY_NAME})

target_compile_features(${FROST_LIBRARY_NAME} PUBLIC cxx_std_20)

# Private, matching dmt: they select which engines this build can construct.
# Installed headers do not mention them; callers ask dmt::is_available().
set(FROST_BACKEND_DEFINITIONS "")
if(FROST_GPU_BACKEND STREQUAL "CUDA")
  list(APPEND FROST_BACKEND_DEFINITIONS FROST_ENABLE_CUDA FROST_ENABLE_GPU)
elseif(FROST_GPU_BACKEND STREQUAL "HIP")
  list(APPEND FROST_BACKEND_DEFINITIONS FROST_ENABLE_HIP FROST_ENABLE_GPU)
endif()
target_compile_definitions(${FROST_LIBRARY_NAME} PRIVATE ${FROST_BACKEND_DEFINITIONS})

# Dependencies
include(${PROJECT_SOURCE_DIR}/cmake/frostDependencies.cmake)
include(${PROJECT_SOURCE_DIR}/cmake/frostBuildInfo.cmake)

if(FROST_WITH_PSRDADA)
  target_compile_definitions(${FROST_LIBRARY_NAME} PRIVATE FROST_ENABLE_PSRDADA)
endif()

# Set include directories
target_include_directories(
  ${FROST_LIBRARY_NAME}
  PUBLIC $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include/>
         $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
  PRIVATE ${PROJECT_SOURCE_DIR}
)

# Toolkit include roots are shared with the white-box test interface below.
set(FROST_GPU_INCLUDE_DIRS "")
if(FROST_GPU_BACKEND STREQUAL "CUDA")
  set(FROST_GPU_INCLUDE_DIRS ${CUDAToolkit_INCLUDE_DIRS})
  if(CUDAToolkit_VERSION VERSION_GREATER_EQUAL "13.0")
    list(PREPEND FROST_GPU_INCLUDE_DIRS "${CUDAToolkit_INCLUDE_DIRS}/cccl")
  endif()
  target_include_directories(${FROST_LIBRARY_NAME} SYSTEM PRIVATE ${FROST_GPU_INCLUDE_DIRS})
endif()

# Header-only internal deps: BUILD_INTERFACE only (never exported to consumers).
target_link_libraries(
  ${FROST_LIBRARY_NAME}
  PUBLIC psrio::psrio dmt::dmt
  PRIVATE $<BUILD_INTERFACE:${FROST_COMPILE_OPTIONS}>
          $<BUILD_INTERFACE:CLI11::CLI11>
          $<BUILD_INTERFACE:xsimd>
          $<BUILD_INTERFACE:tomlplusplus>
          $<BUILD_INTERFACE:HighFive::HighFive>
          $<BUILD_INTERFACE:fmt::fmt-header-only>
          $<BUILD_INTERFACE:spdlog::spdlog_header_only>
)

# System/runtime deps: PRIVATE for shared frost, PUBLIC for static frost.
# psrdada is not in an install export set (and it publicly links OpenMP::OpenMP_C,
# which is not exported either). BUILD_INTERFACE still propagates it to in-tree
# static consumers, which is what the frost binary and the tests need.
if(BUILD_SHARED_LIBS)
  target_link_libraries(${FROST_LIBRARY_NAME} PRIVATE OpenMP::OpenMP_CXX HDF5::HDF5)
  if(FROST_WITH_PSRDADA)
    # PSRDADA does not export a CMake target. Linking its file here avoids
    # putting the fetched, unexported target into IMPORTED_LINK_DEPENDENT_LIBRARIES.
    # Installed consumers use the imported target from FindPSRDADA instead.
    add_dependencies(${FROST_LIBRARY_NAME} psrdada)
    set_property(
      TARGET ${FROST_LIBRARY_NAME}
      APPEND
      PROPERTY BUILD_RPATH "$<TARGET_FILE_DIR:psrdada>"
    )
    target_include_directories(
      ${FROST_LIBRARY_NAME} PRIVATE $<TARGET_PROPERTY:psrdada,INTERFACE_INCLUDE_DIRECTORIES>
    )
    target_link_libraries(
      ${FROST_LIBRARY_NAME}
      PRIVATE $<BUILD_INTERFACE:$<TARGET_LINKER_FILE:psrdada>>
              $<INSTALL_INTERFACE:PSRDADA::psrdada>
    )
  endif()
else()
  target_link_libraries(${FROST_LIBRARY_NAME} PUBLIC OpenMP::OpenMP_CXX HDF5::HDF5)
  if(FROST_WITH_PSRDADA)
    target_link_libraries(
      ${FROST_LIBRARY_NAME} PUBLIC $<BUILD_INTERFACE:psrdada>
                                   $<INSTALL_INTERFACE:PSRDADA::psrdada>
    )
  endif()
endif()

# GPU options and libraries (FROST_GPU_LIBRARIES, see cmake/frostGPU.cmake).
# Private: a static frost still carries them onto the consumer's link line.
if(FROST_GPU_BACKEND)
  target_link_libraries(${FROST_LIBRARY_NAME} PRIVATE ${FROST_GPU_OPTIONS} ${FROST_GPU_LIBRARIES})
endif()

# In-tree white-box access. This target is deliberately not exported: only
# tests that link it may include "lib/..." or use backend/toolkit details.
add_library(frost_internal INTERFACE)
add_library(frost::internal ALIAS frost_internal)
target_include_directories(frost_internal INTERFACE ${PROJECT_SOURCE_DIR})
target_compile_definitions(frost_internal INTERFACE ${FROST_BACKEND_DEFINITIONS})
target_link_libraries(frost_internal INTERFACE ${FROST_COMPILE_OPTIONS} OpenMP::OpenMP_CXX)
if(FROST_GPU_BACKEND)
  target_link_libraries(frost_internal INTERFACE ${FROST_GPU_OPTIONS} ${FROST_GPU_LIBRARIES})
  if(FROST_GPU_INCLUDE_DIRS)
    target_include_directories(frost_internal SYSTEM INTERFACE ${FROST_GPU_INCLUDE_DIRS})
  endif()
endif()
