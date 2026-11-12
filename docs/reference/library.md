# Using libfrost

frost is meant to be used through the `frost` command. The pipeline is also
a C++20 library, `frost::frost`, for the rare program that needs to embed
it. The library has no API reference on this site: its public headers in
[`include/frost/`](https://github.com/pravirkr/frost/tree/main/include/frost)
document each class. There are no Python bindings, and none are planned.

```{note}
Before 1.0, any minor release may change the library's API and ABI.
`find_package(frost 0.1)` accepts only 0.1.x releases.
```

## Fetching frost with CPM

The simplest way is to fetch frost at configure time with
[CPM.cmake](https://github.com/cpm-cmake/CPM.cmake), pinned to a release
tag:

```cmake
CPMAddPackage(
  NAME frost
  GIT_REPOSITORY https://github.com/pravirkr/frost.git
  GIT_TAG v0.1.0
)
target_link_libraries(my_app PRIVATE frost::frost)
```

As a subproject, frost builds no tests, adds no install rules and leaves
your build type alone. It reuses CPM if your project already loaded it.

## Installing the library

To install libfrost, its headers and its CMake package alongside the CLI,
configure with `FROST_INSTALL_LIBRARY`:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DFROST_BUILD_TESTING=OFF -DFROST_INSTALL_LIBRARY=ON
cmake --build build -j
cmake --install build --prefix /opt/frost
```

The prefix is self-contained: frost also installs the fetched dependencies
its package needs (dmt, psrio, fmt and, on Linux, PSRDADA). Set
`-DFROST_INSTALL_BUNDLED_DEPS=OFF` if you provide them yourself. A shared
build (`-DBUILD_SHARED_LIBS=ON`) always installs the library, because the
CLI links it.

Then, in your project:

```cmake
find_package(frost 0.1 CONFIG REQUIRED)
target_link_libraries(my_app PRIVATE frost::frost)
```

with `-DCMAKE_PREFIX_PATH=/opt/frost` if the prefix is not a default one.

## Version information

`frost/common/version.hpp` provides `frost::build_info()`, the same
version, revisions and build settings that `frost --version` prints.
