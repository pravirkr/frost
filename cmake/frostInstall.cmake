# Installation rules. The frost CLI (applications/CMakeLists.txt) is always
# installed; libfrost, its public headers and the frost CMake package
# (find_package(frost) + frost::frost) only with FROST_INSTALL_LIBRARY or a
# shared build.
include(CMakePackageConfigHelpers)

set(FROST_INSTALLS_LIBRARY OFF)
if(FROST_INSTALL_LIBRARY OR BUILD_SHARED_LIBS)
  set(FROST_INSTALLS_LIBRARY ON)
endif()

# Keep prefix installs runnable without a build-tree path or loader env vars.
# The executable finds sibling lib/lib64; shared frost finds dmt/PSRDADA beside it.
if(APPLE)
  set(_frost_loader_path "@loader_path")
elseif(UNIX)
  set(_frost_loader_path "$ORIGIN")
endif()
if(_frost_loader_path)
  file(RELATIVE_PATH _frost_cli_library_path "${CMAKE_INSTALL_FULL_BINDIR}"
       "${CMAKE_INSTALL_FULL_LIBDIR}"
  )
  set_target_properties(
    frost_app PROPERTIES INSTALL_RPATH "${_frost_loader_path}/${_frost_cli_library_path}"
  )
  set_target_properties(${FROST_LIBRARY_NAME} PROPERTIES INSTALL_RPATH "${_frost_loader_path}")
endif()
# Also keep the directories of linked libraries outside the build tree (for
# example Homebrew's libomp); CMake leaves out implicit system directories.
set_target_properties(
  frost_app ${FROST_LIBRARY_NAME} PROPERTIES INSTALL_RPATH_USE_LINK_PATH TRUE
)

# fmt's export names its compiled library although frost uses the header-only
# target. Build it even for a CLI-only install, so that reconfiguring with
# FROST_INSTALL_LIBRARY=ON can install without another build.
if(FROST_INSTALL_BUNDLED_DEPS)
  get_property(_frost_bundled_fmt GLOBAL PROPERTY FROST_BUNDLED_FMT)
  if(_frost_bundled_fmt)
    add_dependencies(${FROST_LIBRARY_NAME} fmt)
  endif()
  unset(_frost_bundled_fmt)
endif()

if(NOT FROST_INSTALLS_LIBRARY)
  # CLI only: the static library is linked into the executable, but PSRDADA is
  # always shared and must sit in the library directory the CLI's RPATH names.
  get_property(_frost_bundled_psrdada GLOBAL PROPERTY FROST_BUNDLED_PSRDADA)
  if(_frost_bundled_psrdada)
    install(TARGETS psrdada LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR} NAMELINK_SKIP)
  endif()
  unset(_frost_bundled_psrdada)
  return()
endif()

set(FROST_BUILT_WITH_LIBHIPCXX OFF)
if(FROST_GPU_BACKEND STREQUAL "HIP" AND libhipcxx_FOUND)
  set(FROST_BUILT_WITH_LIBHIPCXX ON)
endif()

set(FROST_BUILT_AS_SHARED OFF)
if(BUILD_SHARED_LIBS)
  set(FROST_BUILT_AS_SHARED ON)
endif()

set(FROST_BUILT_WITH_PSRDADA OFF)
if(FROST_WITH_PSRDADA)
  set(FROST_BUILT_WITH_PSRDADA ON)
endif()

# Install targets
install(
  TARGETS ${FROST_LIBRARY_NAME}
  EXPORT frostTargets
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
  LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
  ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
  INCLUDES
  DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
)

if(FROST_GPU_OPTIONS)
  install(TARGETS ${FROST_GPU_OPTIONS} EXPORT frostTargets)
endif()

# Install headers
install(DIRECTORY ${PROJECT_SOURCE_DIR}/include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

# Generate and install export file
install(
  EXPORT frostTargets
  FILE frostTargets.cmake
  NAMESPACE ${PROJECT_NAME}::
  DESTINATION ${FROST_CMAKE_PACKAGE_DIR}
)

get_property(FROST_DMT_VERSION GLOBAL PROPERTY FROST_DMT_VERSION)
get_property(FROST_PSRIO_VERSION GLOBAL PROPERTY FROST_PSRIO_VERSION)
configure_package_config_file(
  ${PROJECT_SOURCE_DIR}/cmake/frostConfig.cmake.in ${CMAKE_CURRENT_BINARY_DIR}/frostConfig.cmake
  INSTALL_DESTINATION ${FROST_CMAKE_PACKAGE_DIR}
)

# Before 1.0 any minor release may break the API, so find_package(frost 0.1)
# accepts 0.1.x only. From 1.0 a major release marks a break.
if(PROJECT_VERSION_MAJOR EQUAL 0)
  set(_frost_compatibility SameMinorVersion)
else()
  set(_frost_compatibility SameMajorVersion)
endif()
write_basic_package_version_file(
  ${CMAKE_CURRENT_BINARY_DIR}/frostConfigVersion.cmake
  VERSION ${PROJECT_VERSION}
  COMPATIBILITY ${_frost_compatibility}
)
unset(_frost_compatibility)

set(_frost_cmake_files
    ${CMAKE_CURRENT_BINARY_DIR}/frostConfig.cmake
    ${CMAKE_CURRENT_BINARY_DIR}/frostConfigVersion.cmake
)
if(FROST_WITH_PSRDADA)
  list(APPEND _frost_cmake_files ${PROJECT_SOURCE_DIR}/cmake/FindPSRDADA.cmake)
endif()
install(FILES ${_frost_cmake_files} DESTINATION ${FROST_CMAKE_PACKAGE_DIR})
unset(_frost_cmake_files)

# Bundled dependencies an installed frost needs (listed by
# frostDependencies.cmake). CPM adds them EXCLUDE_FROM_ALL, which drops their
# install rules from this project, so their own install scripts run here.
if(FROST_INSTALL_BUNDLED_DEPS)
  get_property(_frost_install_scripts GLOBAL PROPERTY FROST_BUNDLED_INSTALL_SCRIPTS)
  foreach(frost_script IN LISTS _frost_install_scripts)
    install(SCRIPT "${frost_script}")
  endforeach()
  unset(_frost_install_scripts)
  unset(frost_script)
endif()
