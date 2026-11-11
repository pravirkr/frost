# Locate an installed PSRDADA for consumers of a static libfrost.
# The frost build uses its pinned CPM target; installed consumers provide
# the same runtime dependency through their prefix or these cache entries.
find_path(PSRDADA_INCLUDE_DIR NAMES dada_hdu.h)
find_library(PSRDADA_LIBRARY NAMES psrdada)

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(PSRDADA REQUIRED_VARS PSRDADA_LIBRARY PSRDADA_INCLUDE_DIR)

if(PSRDADA_FOUND AND NOT TARGET PSRDADA::psrdada)
  add_library(PSRDADA::psrdada UNKNOWN IMPORTED)
  set_target_properties(
    PSRDADA::psrdada PROPERTIES IMPORTED_LOCATION "${PSRDADA_LIBRARY}" INTERFACE_INCLUDE_DIRECTORIES
                                                                       "${PSRDADA_INCLUDE_DIR}"
  )
endif()
mark_as_advanced(PSRDADA_INCLUDE_DIR PSRDADA_LIBRARY)
