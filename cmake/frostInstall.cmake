# Installation/export rules for frost::frost and its public headers.
# Install / export: find_package(frost) + target frost::frost
include(CMakePackageConfigHelpers)

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

configure_package_config_file(
  ${PROJECT_SOURCE_DIR}/cmake/frostConfig.cmake.in ${CMAKE_CURRENT_BINARY_DIR}/frostConfig.cmake
  INSTALL_DESTINATION ${FROST_CMAKE_PACKAGE_DIR}
)

write_basic_package_version_file(
  ${CMAKE_CURRENT_BINARY_DIR}/frostConfigVersion.cmake
  VERSION ${PROJECT_VERSION}
  COMPATIBILITY SameMajorVersion
)

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
  get_property(_frost_bundled_fmt GLOBAL PROPERTY FROST_BUNDLED_FMT)
  if(_frost_bundled_fmt)
    add_dependencies(${FROST_LIBRARY_NAME} fmt)
  endif()
  get_property(_frost_install_scripts GLOBAL PROPERTY FROST_BUNDLED_INSTALL_SCRIPTS)
  foreach(frost_script IN LISTS _frost_install_scripts)
    install(SCRIPT "${frost_script}")
  endforeach()
  unset(_frost_bundled_fmt)
  unset(_frost_install_scripts)
  unset(frost_script)
endif()
