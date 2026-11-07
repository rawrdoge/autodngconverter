/*
 * rawimport-pipeline - Adobe DNG Converter Engine adapter
 * Spawns the Adobe DNG Converter CLI binary.
 */
#pragma once

#include <string>
#include <vector>
#include <optional>

#include "domain/ports/converter_engine.hpp"

namespace rawimport::infrastructure {

class AdobeDngConverterEngine final : public domain::ConverterEngine {
public:
    // Path to the Adobe DNG Converter binary. Empty = search PATH.
    explicit AdobeDngConverterEngine(std::string binary_path = "");

    domain::HealthReport                     health_check() override;
    std::vector<std::string>        supported_extensions() override;
    bool                            supports_linear_dng() override;
    bool                            supports_lossy_compression() override;
    int                             max_parallel_workers() override;
    std::optional<domain::FilePath> convert(const domain::FilePath& source,
                                             const domain::FilePath& dest,
                                             const domain::ConversionSettings& settings) override;

private:
    std::string binary_path_;
    std::string find_binary() const;
    int run_subprocess(const std::vector<std::string>& args) const;
};

} // namespace rawimport::infrastructure
