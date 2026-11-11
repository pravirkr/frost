#include <algorithm>
#include <bit>
#include <cerrno>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <limits>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <vector>

#include <dmt/common/backend.hpp>
#include <fmt/format.h>
#include <toml++/toml.h>

#include <frost/common/numeric.hpp>
#include <frost/config/config.hpp>

namespace frost::config {

namespace {

// toml++ is header-only and inherits fast-math. Check its numeric tokens'
// source text before converting values: parsing nan/inf by value can already
// lose their representation under finite-only compiler assumptions.
void validate_toml_literals(const toml::node& node, std::string_view text) {
    if (const auto* table = node.as_table()) {
        for (const auto& [key, value] : *table)
            validate_toml_literals(value, text);
    } else if (const auto* array = node.as_array()) {
        for (const auto& value : *array)
            validate_toml_literals(value, text);
    } else if (node.is_floating_point()) {
        const auto position = node.source().begin;
        std::size_t offset  = 0;
        for (std::size_t line = 1; line < position.line; ++line)
            offset = text.find('\n', offset) + 1;
        // TOML source columns count Unicode code points, not UTF-8 bytes.
        for (std::size_t column = 1; column < position.column; ++column) {
            ++offset;
            while (offset < text.size() &&
                   (static_cast<unsigned char>(text[offset]) & 0xC0U) == 0x80U)
                ++offset;
        }
        auto token = text.substr(offset);
        if (token.starts_with('+') || token.starts_with('-'))
            token.remove_prefix(1);
        if (token.starts_with("nan") || token.starts_with("inf")) {
            throw std::invalid_argument(
                fmt::format("Non-finite TOML value at line {}, column {}",
                            position.line, position.column));
        }
    }
}

search::SubbandDedupMode parse_subband_dedup(std::string_view s) {
    if (s == "transitive") {
        return search::SubbandDedupMode::kTransitive;
    }
    if (s == "seeded") {
        return search::SubbandDedupMode::kSeeded;
    }
    throw std::invalid_argument(
        "Unknown [search].subband_dedup value: " + std::string(s) +
        " (expected transitive or seeded)");
}

// One TOML table plus its dotted path. Construction rejects keys outside
// `allowed` (so a typo such as `subbnad_levels` is not silently ignored),
// and the typed getters reject a present key of the wrong type or a
// negative size (which would otherwise wrap to a huge SizeType).
class Section {
public:
    Section(const toml::table& table,
            std::string path,
            std::vector<std::string_view> allowed)
        : m_table(table),
          m_path(std::move(path)) {
        for (const auto& [key, node] : table) {
            const std::string_view name = key.str();
            if (std::find(allowed.begin(), allowed.end(), name) ==
                allowed.end()) {
                throw std::invalid_argument("Unknown config key '" +
                                            qualified(name) + "'");
            }
        }
    }

    template <typename T> T get(std::string_view key, T fallback) const {
        const auto* node = m_table.get(key);
        if (node == nullptr) {
            return fallback;
        }
        if constexpr (std::is_floating_point_v<T>) {
            if (const auto* raw = node->as_floating_point();
                raw != nullptr && !is_finite(raw->get())) {
                throw std::invalid_argument("Config key '" + qualified(key) +
                                            "' must be finite");
            }
        }
        if (const auto value = node->template value<T>()) {
            return *value;
        }
        throw std::invalid_argument("Config key '" + qualified(key) +
                                    "' has the wrong type");
    }

    SizeType size(std::string_view key, SizeType fallback) const {
        const auto* node = m_table.get(key);
        if (node == nullptr) {
            return fallback;
        }
        const auto value = node->template value<int64_t>();
        if (!value || *value < 0) {
            throw std::invalid_argument("Config key '" + qualified(key) +
                                        "' must be an integer >= 0");
        }
        return static_cast<SizeType>(*value);
    }

    std::optional<std::string> str(std::string_view key) const {
        const auto* node = m_table.get(key);
        if (node == nullptr) {
            return std::nullopt;
        }
        if (auto value = node->template value<std::string>()) {
            return value;
        }
        throw std::invalid_argument("Config key '" + qualified(key) +
                                    "' must be a string");
    }

    const toml::array* array(std::string_view key) const {
        const auto* node = m_table.get(key);
        if (node == nullptr) {
            return nullptr;
        }
        if (const auto* arr = node->as_array()) {
            return arr;
        }
        throw std::invalid_argument("Config key '" + qualified(key) +
                                    "' must be an array");
    }

    std::optional<Section> table(std::string_view key,
                                 std::vector<std::string_view> allowed) const {
        const auto* node = m_table.get(key);
        if (node == nullptr) {
            return std::nullopt;
        }
        if (const auto* tbl = node->as_table()) {
            return Section(*tbl, qualified(key), std::move(allowed));
        }
        throw std::invalid_argument("Config key '" + qualified(key) +
                                    "' must be a table");
    }

private:
    std::string qualified(std::string_view key) const {
        return m_path.empty() ? std::string(key)
                              : m_path + "." + std::string(key);
    }

    const toml::table& m_table;
    std::string m_path;
};

std::vector<prepare::rfi::ChannelRange>
parse_zap_chans(const toml::array& arr) {
    std::vector<prepare::rfi::ChannelRange> ranges;
    for (const auto& item : arr) {
        const auto* pair = item.as_array();
        if (pair == nullptr || pair->size() != 2) {
            throw std::invalid_argument(
                "[rfi].zap_chans entries must be [start, end] pairs");
        }
        const auto start = (*pair)[0].value<int64_t>();
        const auto end   = (*pair)[1].value<int64_t>();
        if (!start || !end || *start < 0 || *end < *start) {
            throw std::invalid_argument(
                "[rfi].zap_chans entries must satisfy 0 <= start <= end");
        }
        ranges.push_back(
            {static_cast<SizeType>(*start), static_cast<SizeType>(*end)});
    }
    return ranges;
}

std::vector<prepare::rfi::FrequencyRange>
parse_zap_freqs(const toml::array& arr) {
    std::vector<prepare::rfi::FrequencyRange> ranges;
    for (const auto& item : arr) {
        const auto* pair = item.as_array();
        if (pair == nullptr || pair->size() != 2) {
            throw std::invalid_argument(
                "[rfi].zap_freqs entries must be [lo, hi] pairs (MHz)");
        }
        const auto lo = (*pair)[0].value<double>();
        const auto hi = (*pair)[1].value<double>();
        if (!lo || !hi || *hi < *lo) {
            throw std::invalid_argument(
                "[rfi].zap_freqs entries must satisfy lo <= hi");
        }
        ranges.push_back({*lo, *hi});
    }
    return ranges;
}

void parse_rfi(const Section& rfi, RfiConfig& cfg) {
    if (const auto* zaps = rfi.array("zap_chans")) {
        cfg.zap_chans = parse_zap_chans(*zaps);
    }
    if (const auto* zaps = rfi.array("zap_freqs")) {
        cfg.zap_freqs = parse_zap_freqs(*zaps);
    }
    if (const auto s = rfi.str("fill")) {
        cfg.fill = fill_mode_from_string(*s);
    }
    if (const auto s = rfi.str("zero_dm")) {
        cfg.zero_dm = zero_dm_from_string(*s);
    }
    cfg.seed = static_cast<uint64_t>(
        rfi.get<int64_t>("seed", static_cast<int64_t>(cfg.seed)));
    cfg.mask_grow = rfi.size("mask_grow", cfg.mask_grow);
    cfg.equalize  = rfi.get("equalize", cfg.equalize);

    if (const auto t = rfi.table("narrow", {"enabled", "threshold"})) {
        cfg.narrow.enabled   = t->get("enabled", cfg.narrow.enabled);
        cfg.narrow.threshold = t->get("threshold", cfg.narrow.threshold);
    }
    if (const auto t =
            rfi.table("broad", {"enabled", "threshold", "boxcar_max"})) {
        cfg.broad.enabled    = t->get("enabled", cfg.broad.enabled);
        cfg.broad.threshold  = t->get("threshold", cfg.broad.threshold);
        cfg.broad.boxcar_max = t->size("boxcar_max", cfg.broad.boxcar_max);
    }
    if (const auto t = rfi.table("dropout", {"enabled", "min_std_fraction"})) {
        cfg.dropout.enabled = t->get("enabled", cfg.dropout.enabled);
        cfg.dropout.min_std_fraction =
            t->get("min_std_fraction", cfg.dropout.min_std_fraction);
    }
    if (const auto t = rfi.table("moments", {"enabled", "threshold", "mean",
                                             "std", "skewness", "kurtosis",
                                             "autocorr", "kurtosis_max"})) {
        auto& m        = cfg.moments;
        m.enabled      = t->get("enabled", m.enabled);
        m.threshold    = t->get("threshold", m.threshold);
        m.mean         = t->get("mean", m.mean);
        m.std          = t->get("std", m.std);
        m.skewness     = t->get("skewness", m.skewness);
        m.kurtosis     = t->get("kurtosis", m.kurtosis);
        m.autocorr     = t->get("autocorr", m.autocorr);
        m.kurtosis_max = t->get("kurtosis_max", m.kurtosis_max);
    }
    if (const auto t = rfi.table("iqrm", {"enabled", "threshold", "radius"})) {
        cfg.iqrm.enabled   = t->get("enabled", cfg.iqrm.enabled);
        cfg.iqrm.threshold = t->get("threshold", cfg.iqrm.threshold);
        cfg.iqrm.radius    = t->size("radius", cfg.iqrm.radius);
    }
    if (const auto t = rfi.table("kadane_channel",
                                 {"enabled", "threshold", "min_duration"})) {
        auto& k        = cfg.kadane_channel;
        k.enabled      = t->get("enabled", k.enabled);
        k.threshold    = t->get("threshold", k.threshold);
        k.min_duration = t->get("min_duration", k.min_duration);
    }
    if (const auto t = rfi.table(
            "kadane_spectrum",
            {"enabled", "threshold", "min_bandwidth", "whole_band_fraction"})) {
        auto& k         = cfg.kadane_spectrum;
        k.enabled       = t->get("enabled", k.enabled);
        k.threshold     = t->get("threshold", k.threshold);
        k.min_bandwidth = t->get("min_bandwidth", k.min_bandwidth);
        k.whole_band_fraction =
            t->get("whole_band_fraction", k.whole_band_fraction);
    }
}

} // namespace

prepare::rfi::FillMode fill_mode_from_string(std::string_view s) {
    using prepare::rfi::FillMode;
    if (s == "neighbour") {
        return FillMode::kNeighbour;
    }
    if (s == "mean") {
        return FillMode::kMean;
    }
    if (s == "noise") {
        return FillMode::kNoise;
    }
    throw std::invalid_argument("Unknown RFI fill mode: " + std::string(s) +
                                " (neighbour|mean|noise)");
}

prepare::rfi::ZeroDMMode zero_dm_from_string(std::string_view s) {
    using prepare::rfi::ZeroDMMode;
    if (s == "off") {
        return ZeroDMMode::kOff;
    }
    if (s == "subtract") {
        return ZeroDMMode::kSubtract;
    }
    if (s == "zdot") {
        return ZeroDMMode::kZdot;
    }
    throw std::invalid_argument("Unknown zero-DM mode: " + std::string(s) +
                                " (off|subtract|zdot)");
}

std::string to_string(prepare::rfi::FillMode mode) {
    switch (mode) {
    case prepare::rfi::FillMode::kMean:
        return "mean";
    case prepare::rfi::FillMode::kNoise:
        return "noise";
    case prepare::rfi::FillMode::kNeighbour:
        break;
    }
    return "neighbour";
}

std::string to_string(prepare::rfi::ZeroDMMode mode) {
    switch (mode) {
    case prepare::rfi::ZeroDMMode::kSubtract:
        return "subtract";
    case prepare::rfi::ZeroDMMode::kZdot:
        return "zdot";
    case prepare::rfi::ZeroDMMode::kOff:
        break;
    }
    return "off";
}

std::uint32_t parse_dada_key(std::string_view text) {
    if (text.empty()) {
        throw std::invalid_argument("PSRDADA key is empty");
    }
    std::string hex(text);
    if (hex.size() > 2 && hex[0] == '0' && (hex[1] == 'x' || hex[1] == 'X')) {
        hex.erase(0, 2);
    }
    if (hex.empty()) {
        throw std::invalid_argument("PSRDADA key is empty");
    }
    char* end        = nullptr;
    errno            = 0;
    const auto value = std::strtoul(hex.c_str(), &end, 16);
    if (end == hex.c_str() || *end != '\0' || errno == ERANGE ||
        value > 0xFFFFFFFFUL) {
        throw std::invalid_argument(
            "PSRDADA key must be a hex id such as dada or 0xdada");
    }
    return static_cast<std::uint32_t>(value);
}

DataType dm_dtype_from_nbits(int nbits) {
    switch (nbits) {
    case 8:
        return DataType::kUInt8;
    case 16:
        return DataType::kUInt16;
    case 32:
        return DataType::kFloat32;
    default:
        throw std::invalid_argument("dm_nbits must be 8, 16 or 32");
    }
}

namespace {
PipelineConfig parse_config(const toml::table& tbl) {
    PipelineConfig cfg;
    const Section root(tbl, "",
                       {"input", "rfi", "dedisp", "search", "output",
                        "nsamps_gulp", "baseline_length", "verbosity",
                        "async_io", "profile", "memory_limit_mib"});

    if (const auto in = root.table(
            "input", {"sigproc_file", "dada_key", "fswap", "beam"})) {
        cfg.input.sigproc_file =
            in->get<std::string>("sigproc_file", cfg.input.sigproc_file);
        cfg.input.dada_key =
            in->get<std::string>("dada_key", cfg.input.dada_key);
        if (!cfg.input.dada_key.empty()) {
            (void)parse_dada_key(cfg.input.dada_key);
        }
        cfg.input.fswap = in->get("fswap", cfg.input.fswap);
        cfg.input.beam  = in->size("beam", cfg.input.beam);
    }
    if (const auto rfi = root.table(
            "rfi", {"zap_chans", "zap_freqs", "fill", "zero_dm", "seed",
                    "mask_grow", "equalize", "narrow", "broad", "dropout",
                    "moments", "iqrm", "kadane_channel", "kadane_spectrum"})) {
        parse_rfi(*rfi, cfg.rfi);
    }
    if (const auto d = root.table("dedisp", {"dm_min", "dm_max", "dm_tol",
                                             "dm_pulse_width", "dm_nbits",
                                             "device", "device_id"})) {
        cfg.dedisp.dm_min = d->get("dm_min", cfg.dedisp.dm_min);
        cfg.dedisp.dm_max = d->get("dm_max", cfg.dedisp.dm_max);
        cfg.dedisp.dm_tol = d->get("dm_tol", cfg.dedisp.dm_tol);
        cfg.dedisp.dm_pulse_width =
            d->get("dm_pulse_width", cfg.dedisp.dm_pulse_width);
        if (const auto nbits = d->get<int64_t>("dm_nbits", 0); nbits != 0) {
            cfg.dedisp.dm_dtype = dm_dtype_from_nbits(static_cast<int>(nbits));
        }
        if (const auto device = d->str("device")) {
            try {
                cfg.dedisp.device = dmt::parse_backend(*device);
            } catch (const std::invalid_argument& err) {
                throw std::invalid_argument("Unknown [dedisp].device value: " +
                                            std::string(err.what()));
            }
        }
        const auto id = d->get<int64_t>("device_id", cfg.dedisp.device_id);
        if (id < 0 || id > 65535) {
            throw std::invalid_argument(
                "[dedisp].device_id must be an integer >= 0");
        }
        cfg.dedisp.device_id = static_cast<int>(id);
    }
    if (const auto s = root.table(
            "search",
            {"boxcar_max", "wtsp", "use_trapezoid", "peak_mode",
             "max_candidates", "max_cands_per_dm", "max_pending_candidates",
             "detect_thresh", "cand_sep_time", "cand_sep_filter", "cand_sep_dm",
             "cand_sep_dm_smear", "max_giant_rate", "cluster", "subband_levels",
             "subband_dedup"})) {
        cfg.search.boxcar_max = s->size("boxcar_max", cfg.search.boxcar_max);
        cfg.search.wtsp       = s->get("wtsp", cfg.search.wtsp);
        cfg.search.use_trapezoid =
            s->get("use_trapezoid", cfg.search.use_trapezoid);
        if (const auto mode = s->str("peak_mode")) {
            cfg.search.peak_mode = peak_mode_from_string(*mode);
        }
        cfg.search.max_pending_candidates = s->size(
            "max_pending_candidates", cfg.search.max_pending_candidates);
        cfg.search.max_candidates =
            s->size("max_candidates", cfg.search.max_candidates);
        cfg.search.max_cands_per_dm =
            s->size("max_cands_per_dm", cfg.search.max_cands_per_dm);
        cfg.search.detect_thresh =
            s->get("detect_thresh", cfg.search.detect_thresh);
        cfg.search.cand_sep_time =
            s->size("cand_sep_time", cfg.search.cand_sep_time);
        cfg.search.cand_sep_filter =
            s->size("cand_sep_filter", cfg.search.cand_sep_filter);
        cfg.search.cand_sep_dm = s->size("cand_sep_dm", cfg.search.cand_sep_dm);
        cfg.search.cand_sep_dm_smear =
            s->get("cand_sep_dm_smear", cfg.search.cand_sep_dm_smear);
        cfg.search.max_giant_rate =
            s->get("max_giant_rate", cfg.search.max_giant_rate);
        cfg.search.cluster = s->get("cluster", cfg.search.cluster);
        cfg.search.subband_levels =
            s->size("subband_levels", cfg.search.subband_levels);
        if (cfg.search.subband_levels > 2) {
            throw std::invalid_argument(
                "[search].subband_levels must be 0, 1 or 2");
        }
        if (const auto mode = s->str("subband_dedup")) {
            cfg.search.subband_dedup = parse_subband_dedup(*mode);
        }
    }
    if (const auto o = root.table(
            "output", {"output_dir", "beam", "per_gulp_files", "provenance"})) {
        cfg.output.output_dir =
            o->get<std::string>("output_dir", cfg.output.output_dir);
        cfg.output.beam = o->size("beam", cfg.output.beam);
        cfg.output.per_gulp_files =
            o->get("per_gulp_files", cfg.output.per_gulp_files);
        cfg.output.provenance = o->get("provenance", cfg.output.provenance);
    }
    cfg.memory_limit_mib = root.size("memory_limit_mib", cfg.memory_limit_mib);
    cfg.nsamps_gulp      = root.size("nsamps_gulp", cfg.nsamps_gulp);
    cfg.baseline_length  = root.get("baseline_length", cfg.baseline_length);
    cfg.verbosity        = root.get("verbosity", cfg.verbosity);
    cfg.async_io         = root.get("async_io", cfg.async_io);
    cfg.profile          = root.get("profile", cfg.profile);

    return cfg;
}

} // namespace
PipelineConfig PipelineConfig::from_toml(const std::string& path) {
    std::ifstream input(path);
    if (!input)
        throw std::runtime_error(
            fmt::format("Cannot open config file '{}'", path));
    const std::string text(std::istreambuf_iterator<char>{input}, {});
    const auto table = toml::parse(text, path);
    validate_toml_literals(table, text);
    return parse_config(table);
}
PipelineConfig PipelineConfig::from_toml_string(std::string_view text) {
    const auto table = toml::parse(text);
    validate_toml_literals(table, text);
    return parse_config(table);
}

std::string PipelineConfig::to_toml_string() const {
    const auto integer = [](SizeType n) {
        if (n > static_cast<SizeType>(std::numeric_limits<int64_t>::max()))
            throw std::invalid_argument(
                "Config size exceeds TOML signed integer range");
        return static_cast<int64_t>(n);
    };
    toml::table root_table;
    root_table.insert("memory_limit_mib", integer(memory_limit_mib));
    root_table.insert("nsamps_gulp", integer(nsamps_gulp));
    root_table.insert("baseline_length", baseline_length);
    root_table.insert("verbosity", verbosity);
    root_table.insert("async_io", async_io);
    root_table.insert("profile", profile);
    toml::table input_table;
    input_table.insert("sigproc_file", input.sigproc_file);
    input_table.insert("dada_key", input.dada_key);
    input_table.insert("fswap", input.fswap);
    input_table.insert("beam", integer(input.beam));
    toml::table dedisp_table;
    dedisp_table.insert("dm_min", dedisp.dm_min);
    dedisp_table.insert("dm_max", dedisp.dm_max);
    dedisp_table.insert("dm_tol", dedisp.dm_tol);
    dedisp_table.insert("dm_pulse_width", dedisp.dm_pulse_width);
    dedisp_table.insert("device_id", dedisp.device_id);
    dedisp_table.insert("dm_nbits", bits_of(dedisp.dm_dtype));
    dedisp_table.insert("device", dmt::to_string(dedisp.device));
    toml::table search_table;
    search_table.insert("boxcar_max", integer(search.boxcar_max));
    search_table.insert("max_pending_candidates",
                        integer(search.max_pending_candidates));
    search_table.insert("max_candidates", integer(search.max_candidates));
    search_table.insert("max_cands_per_dm", integer(search.max_cands_per_dm));
    search_table.insert("cand_sep_time", integer(search.cand_sep_time));
    search_table.insert("cand_sep_filter", integer(search.cand_sep_filter));
    search_table.insert("cand_sep_dm", integer(search.cand_sep_dm));
    search_table.insert("subband_levels", integer(search.subband_levels));
    search_table.insert("wtsp", search.wtsp);
    search_table.insert("use_trapezoid", search.use_trapezoid);
    search_table.insert("detect_thresh", search.detect_thresh);
    search_table.insert("cand_sep_dm_smear", search.cand_sep_dm_smear);
    search_table.insert("max_giant_rate", search.max_giant_rate);
    search_table.insert("cluster", search.cluster);
    search_table.insert("peak_mode", config::to_string(search.peak_mode));
    search_table.insert("subband_dedup",
                        search.subband_dedup ==
                                search::SubbandDedupMode::kSeeded
                            ? "seeded"
                            : "transitive");
    toml::table output_table;
    output_table.insert("output_dir", output.output_dir);
    output_table.insert("beam", integer(output.beam));
    output_table.insert("per_gulp_files", output.per_gulp_files);
    output_table.insert("provenance", output.provenance);
    toml::table rfi_table;
    rfi_table.insert("fill", config::to_string(rfi.fill));
    rfi_table.insert("zero_dm", config::to_string(rfi.zero_dm));
    rfi_table.insert("seed", std::bit_cast<int64_t>(rfi.seed));
    rfi_table.insert("mask_grow", integer(rfi.mask_grow));
    rfi_table.insert("equalize", rfi.equalize);
    toml::table rfi_narrow_table;
    rfi_narrow_table.insert("enabled", rfi.narrow.enabled);
    rfi_narrow_table.insert("threshold", rfi.narrow.threshold);
    toml::table rfi_broad_table;
    rfi_broad_table.insert("enabled", rfi.broad.enabled);
    rfi_broad_table.insert("threshold", rfi.broad.threshold);
    rfi_broad_table.insert("boxcar_max", integer(rfi.broad.boxcar_max));
    toml::table rfi_dropout_table;
    rfi_dropout_table.insert("enabled", rfi.dropout.enabled);
    rfi_dropout_table.insert("min_std_fraction", rfi.dropout.min_std_fraction);
    toml::table rfi_moments_table;
    rfi_moments_table.insert("enabled", rfi.moments.enabled);
    rfi_moments_table.insert("threshold", rfi.moments.threshold);
    rfi_moments_table.insert("mean", rfi.moments.mean);
    rfi_moments_table.insert("std", rfi.moments.std);
    rfi_moments_table.insert("skewness", rfi.moments.skewness);
    rfi_moments_table.insert("kurtosis", rfi.moments.kurtosis);
    rfi_moments_table.insert("autocorr", rfi.moments.autocorr);
    rfi_moments_table.insert("kurtosis_max", rfi.moments.kurtosis_max);
    toml::table rfi_iqrm_table;
    rfi_iqrm_table.insert("enabled", rfi.iqrm.enabled);
    rfi_iqrm_table.insert("threshold", rfi.iqrm.threshold);
    rfi_iqrm_table.insert("radius", integer(rfi.iqrm.radius));
    toml::table rfi_kadane_channel_table;
    rfi_kadane_channel_table.insert("enabled", rfi.kadane_channel.enabled);
    rfi_kadane_channel_table.insert("threshold", rfi.kadane_channel.threshold);
    rfi_kadane_channel_table.insert("min_duration",
                                    rfi.kadane_channel.min_duration);
    toml::table rfi_kadane_spectrum_table;
    rfi_kadane_spectrum_table.insert("enabled", rfi.kadane_spectrum.enabled);
    rfi_kadane_spectrum_table.insert("threshold",
                                     rfi.kadane_spectrum.threshold);
    rfi_kadane_spectrum_table.insert("min_bandwidth",
                                     rfi.kadane_spectrum.min_bandwidth);
    rfi_kadane_spectrum_table.insert("whole_band_fraction",
                                     rfi.kadane_spectrum.whole_band_fraction);
    toml::array channels, frequencies;
    for (const auto& range : rfi.zap_chans)
        channels.push_back(
            toml::array{integer(range.start), integer(range.end)});
    for (const auto& range : rfi.zap_freqs)
        frequencies.push_back(toml::array{range.lo, range.hi});
    rfi_table.insert("zap_chans", std::move(channels));
    rfi_table.insert("zap_freqs", std::move(frequencies));
    rfi_table.insert("narrow", std::move(rfi_narrow_table));
    rfi_table.insert("broad", std::move(rfi_broad_table));
    rfi_table.insert("dropout", std::move(rfi_dropout_table));
    rfi_table.insert("moments", std::move(rfi_moments_table));
    rfi_table.insert("iqrm", std::move(rfi_iqrm_table));
    rfi_table.insert("kadane_channel", std::move(rfi_kadane_channel_table));
    rfi_table.insert("kadane_spectrum", std::move(rfi_kadane_spectrum_table));
    root_table.insert("input", std::move(input_table));
    root_table.insert("rfi", std::move(rfi_table));
    root_table.insert("dedisp", std::move(dedisp_table));
    root_table.insert("search", std::move(search_table));
    root_table.insert("output", std::move(output_table));
    std::ostringstream out;
    out << root_table;
    return out.str();
}

void PipelineConfig::validate() const {
    const auto positive = [](const float& v, const char* name) {
        if (!is_positive_finite(v)) {
            throw std::invalid_argument(std::string(name) +
                                        " must be a finite number > 0");
        }
    };
    if (!is_finite(dedisp.dm_min) || !is_finite(dedisp.dm_max) ||
        dedisp.dm_min < 0.0F || !(dedisp.dm_max > dedisp.dm_min)) {
        throw std::invalid_argument(
            "dm range must be finite with 0 <= dm_min < dm_max");
    }
    // generate_dm_list never advances at dm_tol == 1 and returns NaN below.
    if (!is_finite(dedisp.dm_tol) || !(dedisp.dm_tol > 1.0F)) {
        throw std::invalid_argument("dm_tol must be a finite number > 1");
    }
    if (memory_limit_mib >
        std::numeric_limits<SizeType>::max() / (SizeType{1} << 20U))
        throw std::invalid_argument("memory_limit_mib overflows byte count");
    positive(dedisp.dm_pulse_width, "dm_pulse_width");
    if (dedisp.device_id < 0) {
        throw std::invalid_argument("device_id must be >= 0");
    }
    if (search.max_candidates == 0 || search.max_cands_per_dm == 0) {
        throw std::invalid_argument(
            "max_candidates and max_cands_per_dm must be > 0");
    }
    (void)to_string(search.peak_mode);
    if (search.boxcar_max == 0 || search.boxcar_max > kMaxBoxcar) {
        throw std::invalid_argument("boxcar_max must be in [1, " +
                                    std::to_string(kMaxBoxcar) + "]");
    }
    if (!is_finite(search.wtsp) || !(search.wtsp >= 1.0F)) {
        throw std::invalid_argument("wtsp must be a finite number >= 1");
    }
    positive(search.detect_thresh, "detect_thresh");
    if (!is_finite(search.cand_sep_dm_smear) ||
        search.cand_sep_dm_smear < 0.0F) {
        throw std::invalid_argument(
            "cand_sep_dm_smear must be a finite number >= 0");
    }
    if (!is_finite(search.max_giant_rate)) {
        throw std::invalid_argument("max_giant_rate must be finite");
    }
    if (search.subband_levels > 2) {
        throw std::invalid_argument("subband_levels must be 0, 1 or 2");
    }
    if (nsamps_gulp == 0 || nsamps_gulp > kMaxGulp) {
        throw std::invalid_argument("nsamps_gulp must be in [1, " +
                                    std::to_string(kMaxGulp) + "]");
    }
    if (search.boxcar_max >= nsamps_gulp) {
        throw std::invalid_argument(
            "boxcar_max must be smaller than nsamps_gulp");
    }
    positive(baseline_length, "baseline_length");
    rfi.validate();
}

dmt::PeakFindingMode peak_mode_from_string(std::string_view s) {
    if (s == "per_width")
        return dmt::PeakFindingMode::kContiguousPeakPerWidth;
    if (s == "across_widths")
        return dmt::PeakFindingMode::kContiguousPeakAcrossWidths;
    if (s == "all")
        return dmt::PeakFindingMode::kAllAboveThreshold;
    throw std::invalid_argument("Unknown peak_mode: " + std::string(s));
}

std::string to_string(dmt::PeakFindingMode mode) {
    switch (mode) {
    case dmt::PeakFindingMode::kContiguousPeakPerWidth:
        return "per_width";
    case dmt::PeakFindingMode::kContiguousPeakAcrossWidths:
        return "across_widths";
    case dmt::PeakFindingMode::kAllAboveThreshold:
        return "all";
    }
    throw std::invalid_argument("Invalid peak_mode");
}

} // namespace frost::config
