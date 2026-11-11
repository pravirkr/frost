#pragma once

#include <vector>

#include <dmt/common/plans.hpp>

#include <frost/common/policies.hpp>

namespace frost::dedisp {

/** Observing setup and physical DM grid for a streaming FDMT plan. */
struct DedispPlanConfig {
    SizeType nchans      = 0;
    float dt             = 0.0F; // sampling interval, seconds
    float f0             = 0.0F; // frequency of channel 0, MHz
    float df             = 0.0F; // channel bandwidth, MHz (may be negative)
    float dm_min         = 0.0F;
    float dm_max         = 0.0F;
    float dm_pulse_width = 0.0F; // expected intrinsic pulse width, microseconds
    float dm_tol         = 1.25F; // smear tolerance factor between DM trials
    DataType dm_dtype    = DataType::kFloat32; // output DMT sample type
    // Contiguous block length for FDMT valid-mode streaming.
    SizeType block_nsamps = 0;
    // Search depth, including staggered bands across tree boundaries.
    SizeType subband_depth = 0;
};

/** Generates trials spaced by the configured dispersion-smearing tolerance. */
std::vector<float> generate_dm_list(const DedispPlanConfig& cfg);

/** Builds a valid-mode FDMT plan with ascending channel-edge frequencies. */
dmt::plans::FDMTPlan make_dmt_fdmt_plan(const DedispPlanConfig& cfg);

} // namespace frost::dedisp
