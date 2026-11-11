#include <frost/config/config.hpp>
#include <frost/dedisp/dedisp.hpp>
#include <frost/pipeline/pipeline.hpp>
#include <frost/pipeline/profile.hpp>

#if defined(FROST_ENABLE_GPU) || defined(FROST_ENABLE_CUDA) ||                 \
    defined(FROST_ENABLE_HIP)
#error "Backend implementation macros must not reach consumers"
#endif
#if __has_include("lib/prepare/device_rfi.hpp")
#error "Private library include roots must not reach consumers"
#endif

int main() {
    frost::config::PipelineConfig config;
    config.validate();
    frost::dedisp::DedispPlanConfig plan_config;
    plan_config.nchans       = 8;
    plan_config.dt           = 0.001F;
    plan_config.f0           = 1400.0F;
    plan_config.df           = -1.0F;
    plan_config.dm_max       = 1.0F;
    plan_config.block_nsamps = 64;
    const auto plan          = frost::dedisp::make_dmt_fdmt_plan(plan_config);
    return plan.get_dm_grid_final().empty() ||
           frost::pipeline::format_run_profile({}).empty();
}
