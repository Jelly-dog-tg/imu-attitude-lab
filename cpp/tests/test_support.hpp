// Shared helpers for the C++ test suite.
#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <system_error>

namespace imu_lab::test {

/// Unique-ish suffix so parallel CTest processes never share a directory.
inline std::string unique_suffix() {
    static std::atomic<std::uint64_t> counter{0};
    return std::to_string(static_cast<unsigned long long>(
                              std::chrono::steady_clock::now().time_since_epoch().count())) +
           "_" + std::to_string(counter.fetch_add(1));
}

/// RAII scratch directory, removed when the test finishes.
class ScratchDir {
public:
    ScratchDir() {
        path_ = std::filesystem::temp_directory_path() / ("imu_lab_test_" + unique_suffix());
        std::filesystem::create_directories(path_);
    }

    ScratchDir(const ScratchDir&) = delete;
    ScratchDir& operator=(const ScratchDir&) = delete;

    ~ScratchDir() {
        std::error_code ignored;
        std::filesystem::remove_all(path_, ignored);
    }

    [[nodiscard]] const std::filesystem::path& path() const { return path_; }

    [[nodiscard]] std::filesystem::path file(const std::string& name) const {
        return path_ / name;
    }

    /// Write `content` to `name` and return the path as a string.
    [[nodiscard]] std::string write(const std::string& name, const std::string& content) const {
        const std::filesystem::path target = file(name);
        std::ofstream stream(target, std::ios::binary);
        stream << content;
        stream.close();
        return target.string();
    }

    /// Path inside the directory that does not exist yet.
    [[nodiscard]] std::string missing(const std::string& name) const {
        return file(name).string();
    }

private:
    std::filesystem::path path_;
};

/// Read a whole file, returning an empty string when it cannot be opened.
inline std::string read_file(const std::string& path) {
    std::ifstream stream(path, std::ios::binary);
    if (!stream) {
        return {};
    }
    return std::string(std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>());
}

}  // namespace imu_lab::test
