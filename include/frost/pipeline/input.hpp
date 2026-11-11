#pragma once

#include <psrio/block_source.hpp>

#include <frost/config/config.hpp>

namespace frost::pipeline {

/**
 * Opens one intensity stream from `input`. A file is a Breakthrough Listen
 * HDF5 filterbank when its name or bytes say so, and a sigproc filterbank
 * otherwise. A PSRDADA key opens that ring. `input.fswap` reverses channels
 * on each block. `input.beam` selects the HDF5 beam axis (0-based) and is
 * ignored for sigproc and PSRDADA.
 */
psrio::BlockSource open_input(const config::InputConfig& input);

} // namespace frost::pipeline
