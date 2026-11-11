# Frost has one application; new capabilities are modes/flags on this target.
add_executable(frost_app frost.cpp)
frost_enable_warnings_as_errors(frost_app)
set_target_properties(frost_app PROPERTIES OUTPUT_NAME frost)
target_link_libraries(
  frost_app PRIVATE frost::frost CLI11::CLI11 ${FROST_COMPILE_OPTIONS} fmt::fmt-header-only
                    spdlog::spdlog_header_only
)
install(TARGETS frost_app RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
