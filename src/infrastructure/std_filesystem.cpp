/*
 * rawimport-pipeline - std::filesystem FileSystem adapter implementation
 */
#include "infrastructure/std_filesystem.hpp"

#include <algorithm>
#include <fstream>
#include <system_error>
#include <vector>

namespace rawimport::infrastructure {

StdFileSystem::StdFileSystem(domain::HashService& hash_svc) : hash_svc_(hash_svc) {}

domain::FilePath StdFileSystem::to_fp(const std::filesystem::path& p) {
    return domain::FilePath{p.string()};
}

std::filesystem::path StdFileSystem::to_fs(const domain::FilePath& fp) {
    return std::filesystem::path(fp.native);
}

domain::HealthReport StdFileSystem::health_check() {
    return {domain::HealthStatus::Healthy, "std::filesystem OK", domain::now()};
}

std::optional<domain::FilePath> StdFileSystem::move_file(const domain::FilePath& src, const domain::FilePath& dst) {
    const auto sp = to_fs(src);
    const auto dp = to_fs(dst);

    // Hash source BEFORE any rename (for cross-device verification).
    auto h_src = sha256_file(src);

    // Try direct rename first (same filesystem).
    std::error_code ec;
    std::filesystem::rename(sp, dp, ec);
    if (!ec) {
        // Verify hash on destination side.
        auto h_dst = sha256_file(dst);
        if (h_src && h_dst && *h_src == *h_dst) {
            return dst;
        }
        // Hash mismatch — move file back (best-effort).
        std::filesystem::rename(dp, sp, ec);
        return std::nullopt;
    }

    // Cross-device fallback: copy + verify + remove source.
    if (std::filesystem::copy_file(sp, dp, ec)) {
        auto h_dst = sha256_file(dst);
        if (h_src && h_dst && *h_src == *h_dst) {
            delete_file(src);
            return dst;
        }
        std::filesystem::remove(dp, ec);
    }
    return std::nullopt;
}

bool StdFileSystem::copy_file(const domain::FilePath& src, const domain::FilePath& dst) {
    std::error_code ec;
    const bool ok = std::filesystem::copy_file(to_fs(src), to_fs(dst), ec);
    if (!ok) return false;
    auto h_src = sha256_file(src);
    auto h_dst = sha256_file(dst);
    if (h_src && h_dst && *h_src != *h_dst) {
        std::filesystem::remove(to_fs(dst), ec);
        return false;
    }
    return true;
}

bool StdFileSystem::delete_file(const domain::FilePath& path) {
    std::error_code ec;
    const bool existed = std::filesystem::exists(to_fs(path), ec);
    if (existed) std::filesystem::remove(to_fs(path), ec);
    return existed;
}

std::optional<domain::SHA256Hash> StdFileSystem::sha256_file(const domain::FilePath& path, int* errno_out) {
    std::ifstream f(to_fs(path), std::ios::binary);
    if (!f) {
        if (errno_out) *errno_out = errno;
        return std::nullopt;
    }

    auto hasher = hash_svc_.create_sha256_hasher();
    std::vector<uint8_t> buf(65536);
    while (f.read(reinterpret_cast<char*>(buf.data()), buf.size())) {
        auto chunk_span = std::span<const uint8_t>(buf.data(), f.gcount());
        hasher->update(chunk_span);
    }
    // Append any remaining bytes
    if (f.gcount() > 0) {
        auto chunk_span = std::span<const uint8_t>(buf.data(), f.gcount());
        hasher->update(chunk_span);
    }
    return hasher->finalize();
}

domain::FastFingerprint StdFileSystem::compute_fast_fingerprint(const domain::FilePath& path) {
    domain::FastFingerprint fp{};
    const auto p = to_fs(path);
    if (!std::filesystem::exists(p)) return fp;

    fp.size_bytes = static_cast<uint64_t>(std::filesystem::file_size(p));
    // Convert filesystem::file_time_type to system_clock::time_point portably.
    // On Linux/macOS, file_time_type uses the same epoch as system_clock (Unix epoch).
    // On Windows, it uses FILETIME (1601 epoch). Use std::chrono::clock_cast (C++20)
    // when available, otherwise use a portable approximation.
    const auto wt = std::filesystem::last_write_time(p);
#if defined(__cpp_lib_chrono) && __cpp_lib_chrono >= 201907L
    // C++20 clock_cast for portable conversion (available on GCC 11+, Clang 12+, MSVC 19.28+)
    try {
        fp.mtime = std::chrono::clock_cast<std::chrono::system_clock>(wt);
    } catch (...) {
        // Fallback for platforms where clock_cast is not fully implemented
        const auto secs = std::chrono::duration_cast<std::chrono::seconds>(
                              wt.time_since_epoch()).count();
        fp.mtime = std::chrono::system_clock::from_time_t(secs);
    }
#else
    // Pre-C++20 or platforms without clock_cast: assume Unix epoch (Linux/macOS).
    // This is correct on Linux/macOS but not on Windows.
    const auto secs = std::chrono::duration_cast<std::chrono::seconds>(
                          wt.time_since_epoch()).count();
    fp.mtime = std::chrono::system_clock::from_time_t(secs);
#endif

    // FNV-1a of first 4 KiB.
    constexpr std::size_t CHUNK = 4096;
    std::ifstream f(p, std::ios::binary);
    std::vector<uint8_t> buf(CHUNK);
    f.read(reinterpret_cast<char*>(buf.data()), static_cast<std::streamsize>(CHUNK));
    const auto n = static_cast<std::size_t>(f.gcount());
    fp.fnv1a = hash_svc_.fnv1a_64(std::span<const uint8_t>(buf.data(), n));
    return fp;
}

std::vector<domain::FilePath> StdFileSystem::list_directory(const std::string& dir_path,
                                                              const std::string& extension_filter) {
    std::vector<domain::FilePath> result;
    std::error_code ec;
    for (const auto& entry : std::filesystem::directory_iterator(dir_path, ec)) {
        if (ec) break;
        if (!entry.is_regular_file(ec)) continue;
        if (!extension_filter.empty()) {
            const auto ext = entry.path().extension().string();
            if (ext != extension_filter) continue;
        }
        result.push_back(to_fp(entry.path()));
    }
    return result;
}

bool StdFileSystem::exists(const domain::FilePath& path) {
    return std::filesystem::exists(to_fs(path));
}

bool StdFileSystem::create_directories(const domain::FilePath& path) {
    std::error_code ec;
    std::filesystem::create_directories(to_fs(path), ec);
    return !ec;
}

} // namespace rawimport::infrastructure
