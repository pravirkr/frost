// Channel kill files: the sigproc one-flag-per-channel convention, parsed
// strictly (see read_kill_file in frost/prepare/rfi.hpp).

#include <fstream>
#include <stdexcept>
#include <string>
#include <string_view>

#include <fmt/format.h>

#include <frost/prepare/rfi.hpp>

namespace frost::prepare::rfi {

std::vector<ChannelRange> read_kill_file(const std::filesystem::path& path,
                                         SizeType nchans) {
    std::ifstream in(path);
    if (!in) {
        throw std::runtime_error(
            fmt::format("cannot read kill file {}", path.string()));
    }
    std::vector<ChannelRange> killed;
    SizeType channel = 0;
    std::string line;
    for (SizeType number = 1; std::getline(in, line); ++number) {
        std::string_view text(line);
        if (const auto hash = text.find('#'); hash != std::string_view::npos)
            text = text.substr(0, hash);
        constexpr std::string_view kSpace = " \t\r\f\v";
        const auto first                  = text.find_first_not_of(kSpace);
        if (first == std::string_view::npos)
            continue;
        text = text.substr(first, text.find_last_not_of(kSpace) - first + 1);
        if (text != "0" && text != "1") {
            throw std::invalid_argument(fmt::format(
                "kill file {} line {}: expected 0 (kill) or 1 (keep), got "
                "'{}'",
                path.string(), number, text));
        }
        if (channel == nchans) {
            throw std::invalid_argument(
                fmt::format("kill file {} has more than {} channel entries",
                            path.string(), nchans));
        }
        if (text == "0") {
            if (!killed.empty() && killed.back().end + 1 == channel)
                killed.back().end = channel;
            else
                killed.push_back({channel, channel});
        }
        ++channel;
    }
    if (in.bad()) {
        throw std::runtime_error(
            fmt::format("error reading kill file {}", path.string()));
    }
    if (channel != nchans) {
        throw std::invalid_argument(
            fmt::format("kill file {} has {} channel entries; the input has {}",
                        path.string(), channel, nchans));
    }
    return killed;
}

} // namespace frost::prepare::rfi
