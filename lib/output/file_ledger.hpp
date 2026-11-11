#pragma once
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <system_error>

#include <unistd.h>

namespace frost::output {
/** First-write bookkeeping for per-gulp files. Markers live on disk so a
 * long observation cannot grow a process-lifetime set of output filenames. */
class FileLedger {
public:
    FileLedger() {
        static std::atomic<uint64_t> next{0};
        const auto base = std::filesystem::temp_directory_path();
        for (;;) {
            m_directory =
                base / ("frost_files_" +
                        std::to_string(static_cast<long>(::getpid())) + "_" +
                        std::to_string(next.fetch_add(1)));
            std::error_code ec;
            if (std::filesystem::create_directory(m_directory, ec))
                break;
            if (ec)
                throw std::runtime_error("Cannot create output ledger: " +
                                         ec.message());
        }
    }
    FileLedger(const FileLedger&)            = delete;
    FileLedger& operator=(const FileLedger&) = delete;
    ~FileLedger() {
        std::error_code ec;
        std::filesystem::remove_all(m_directory, ec);
    }
    bool seen_before(const std::filesystem::path& output) {
        const auto marker = m_directory / output.filename();
        if (std::filesystem::exists(marker))
            return true;
        std::ofstream file(marker);
        file.close();
        if (!file)
            throw std::runtime_error("Cannot record candidate output file: " +
                                     output.string());
        return false;
    }
    const std::filesystem::path& directory() const { return m_directory; }

private:
    std::filesystem::path m_directory;
};
} // namespace frost::output
