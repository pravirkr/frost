#include <filesystem>
#include <stdexcept>
#include <utility>

#include <fmt/format.h>
#include <psrio/formats/fbh5.hpp>
#ifdef FROST_ENABLE_PSRDADA
#include <psrio/formats/psrdada.hpp>
#endif
#include <psrio/formats/sigproc.hpp>

#include <frost/pipeline/input.hpp>

namespace frost::pipeline {

namespace {

// Pipeline checks this again for sources that never go through open_input.
void require_searchable(const psrio::BlockSource& source) {
    if (source.nifs() != 1) {
        throw std::invalid_argument(fmt::format(
            "frost: search reads one IF; this input has {}", source.nifs()));
    }
    if (source.nchans() == 0 || source.bytes_per_sample() == 0) {
        throw std::invalid_argument("frost: input has no frequency channels");
    }
    (void)to_data_type(source.sample_type());
}

} // namespace

psrio::BlockSource open_input(const config::InputConfig& input) {
    const bool have_file = !input.sigproc_file.empty();
    const bool have_dada = !input.dada_key.empty();
    if (have_file == have_dada) {
        throw std::invalid_argument(
            "frost: pass exactly one of a filterbank file or a PSRDADA key");
    }

    if (have_dada) {
        const auto key = config::parse_dada_key(input.dada_key);
#ifdef FROST_ENABLE_PSRDADA
        psrio::formats::psrdada::RingReader reader(key);
        reader.set_fswap(input.fswap);
        psrio::BlockSource source(std::move(reader));
        require_searchable(source);
        return source;
#else
        (void)key;
        throw std::runtime_error(
            "frost: built without PSRDADA support (FROST_WITH_PSRDADA=OFF)");
#endif
    }

    const std::filesystem::path path(input.sigproc_file);
    if (psrio::formats::fbh5::is_hdf5_filterbank(path)) {
        psrio::formats::fbh5::FilterbankFile reader(path, input.beam);
        reader.set_fswap(input.fswap);
        psrio::BlockSource source(std::move(reader));
        require_searchable(source);
        return source;
    }

    psrio::formats::sigproc::FilterbankReader reader(path);
    reader.set_fswap(input.fswap);
    psrio::BlockSource source(std::move(reader));
    require_searchable(source);
    return source;
}

} // namespace frost::pipeline
