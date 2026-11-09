/*
 * rawimport-pipeline - std::filesystem FileSystem adapter
 */
#pragma once

#include <string>
#include <vector>
#include <optional>
#include <filesystem>
#include <fstream>
#include <sys/stat.h>

#include "domain/ports/file_system.hpp"
#include "domain/ports/hash_service.hpp"

namespace rawimport::infrastructure {

class StdFileSystem final : public domain::FileSystem {
public:
    explicit StdFileSystem(domain::HashService& hash_svc);

    domain::HealthReport health_check() override;
    std::optional<domain::FilePath> move_file(const domain::FilePath& src, const domain::FilePath& dst) override;
    bool                copy_file(const domain::FilePath& src, const domain::FilePath& dst) override;
    bool                delete_file(const domain::FilePath& path) override;
    std::optional<domain::SHA256Hash> sha256_file(const domain::FilePath& path, int* errno_out = nullptr) override;
    domain::FastFingerprint compute_fast_fingerprint(const domain::FilePath& path) override;
    std::vector<domain::FilePath> list_directory(const std::string& dir_path,
                                                  const std::string& extension_filter) override;
    bool                exists(const domain::FilePath& path) override;
    bool                create_directories(const domain::FilePath& path) override;

private:
    domain::HashService& hash_svc_;

    static domain::FilePath to_fp(const std::filesystem::path& p);
    static std::filesystem::path to_fs(const domain::FilePath& fp);
};

} // namespace rawimport::infrastructure
