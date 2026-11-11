cmake_minimum_required(VERSION 3.21)
project(frost_consumer LANGUAGES CXX)

# FROST_SOURCE_DIR checks build-tree usage requirements. Without it this is
# an ordinary downstream find_package consumer of an installed frost.
if(FROST_SOURCE_DIR)
  set(FROST_BUILD_TESTING
      OFF
      CACHE BOOL "" FORCE
  )
  add_subdirectory("${FROST_SOURCE_DIR}" frost)
else()
  find_package(frost CONFIG REQUIRED)
endif()

add_executable(frost_consumer main.cpp)
target_link_libraries(frost_consumer PRIVATE frost::frost)
