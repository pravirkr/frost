#pragma once

#include <atomic>
#include <filesystem>
#include <mutex>
#include <string>
#include <vector>

#ifdef _WIN32
#include <process.h>
#else
#include <unistd.h>
#endif

namespace frost::test {

// Paths remain valid for the test process, including helpers returning a path
// into a temporary directory. Normal exit (also after failed assertions)
// removes all registered trees; cleanup errors must not terminate the runner.
class TempDirectoryRegistry {
public:
    TempDirectoryRegistry()                                        = default;
    TempDirectoryRegistry(const TempDirectoryRegistry&)            = delete;
    TempDirectoryRegistry& operator=(const TempDirectoryRegistry&) = delete;
    ~TempDirectoryRegistry() {
        for (const auto& path : m_paths) {
            std::error_code error;
            std::filesystem::remove_all(path, error);
        }
    }

    std::filesystem::path create(const std::string& name) {
        static std::atomic<unsigned> counter{0};
#ifdef _WIN32
        const auto pid = static_cast<long>(_getpid());
#else
        const auto pid = static_cast<long>(::getpid());
#endif
        const auto prefix = std::filesystem::temp_directory_path() /
                            ("frost_" + name + "_" + std::to_string(pid));
        std::lock_guard lock(m_mutex);
        for (;;) {
            auto path = std::filesystem::path(
                prefix.string() + "_" + std::to_string(counter.fetch_add(1)));
            // Never delete an existing directory: a recycled process ID may
            // collide with an older interrupted run's artifacts.
            if (!std::filesystem::create_directory(path))
                continue;
            m_paths.push_back(path);
            return path;
        }
    }

private:
    std::mutex m_mutex;
    std::vector<std::filesystem::path> m_paths;
};

// A fresh, empty directory unique to this process and call. Catch2 tests are
// registered as separate CTest processes that may run concurrently, so a fixed
// name under the system temp dir lets one test delete another's files.
inline std::filesystem::path unique_temp_dir(const std::string& name) {
    static TempDirectoryRegistry directories;
    return directories.create(name);
}

} // namespace frost::test
