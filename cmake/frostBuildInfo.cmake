# A configure-time snapshot; source overrides must never masquerade as the pin.
function(frost_source_revision source fallback output)
  set(revision "${fallback}")
  if(EXISTS "${source}/.git")
    execute_process(
      COMMAND git -C "${source}" rev-parse HEAD
      RESULT_VARIABLE status
      OUTPUT_VARIABLE git_revision
      OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET
    )
    if(status EQUAL 0)
      set(revision "${git_revision}")
      execute_process(
        COMMAND git -C "${source}" status --porcelain
        OUTPUT_VARIABLE dirty
        OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET
      )
      if(dirty)
        string(APPEND revision "-dirty")
      endif()
    endif()
  endif()
  set(${output}
      "${revision}"
      PARENT_SCOPE
  )
endfunction()
frost_source_revision("${PROJECT_SOURCE_DIR}" unknown FROST_BUILD_REVISION)
set(_dmt_revision "${FROST_DMT_REVISION}")
set(_psrio_revision "${FROST_PSRIO_REVISION}")
if(CPM_dmt_SOURCE OR NOT dmt_ADDED)
  set(_dmt_revision "unknown-source-override")
endif()
if(CPM_psrio_SOURCE OR NOT psrio_ADDED)
  set(_psrio_revision "unknown-source-override")
endif()
frost_source_revision("${dmt_SOURCE_DIR}" "${_dmt_revision}" FROST_DMT_BUILD_REVISION)
frost_source_revision("${psrio_SOURCE_DIR}" "${_psrio_revision}" FROST_PSRIO_BUILD_REVISION)
configure_file(
  "${PROJECT_SOURCE_DIR}/cmake/frostBuildInfo.hpp.in"
  "${CMAKE_CURRENT_BINARY_DIR}/generated/frost_build_info.hpp" @ONLY
)
target_include_directories(${FROST_LIBRARY_NAME} PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/generated")
