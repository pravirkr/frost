# Changelog

All notable changes to frost are recorded here. The format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/), and frost uses
[Semantic Versioning](https://semver.org/spec/v2.0.0.html): before 1.0, a
minor release may change the command-line interface, the `.cand` format or
the library API.

## [Unreleased]

The first release, 0.1.0.

### Added

- `frost`, one command-line search for channelised intensity data:
  sigproc and Breakthrough Listen HDF5 filterbanks, and PSRDADA ring
  buffers on Linux.
- RFI excision with narrow- and broad-band excision on by default, and
  opt-in dropouts, channel moments, IQRM, Kadane runs, zero-DM, zdot,
  equalisation, static zaps and sigproc kill files.
- Streaming FDMT search with boxcar or trapezoid matched filters through
  dmt 0.8.0, on the full band and optionally on half and quarter
  sub-bands.
- Friends-of-friends candidate clustering, sub-band merging, and a
  23-column tab-separated `.cand` format.
- TOML configuration files, memory planning, stage profiles, `run.toml`
  provenance and a fixed synthetic benchmark.
- Experimental CUDA and HIP backends.
- `frost --version` with source revisions and build settings.
- Installation of the CLI with `cmake --install`; the library, headers and
  CMake package are opt-in with `FROST_INSTALL_LIBRARY`.
- User documentation on Read the Docs, and a conda `environment.yml`.

[Unreleased]: https://github.com/pravirkr/frost/commits/main
