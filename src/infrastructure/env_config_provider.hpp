/*
 * rawimport-pipeline - Environment ConfigProvider adapter
 */
#pragma once

#include <string>
#include <map>
#include <optional>

#include "domain/ports/config_provider.hpp"

namespace rawimport::infrastructure {

class EnvConfigProvider final : public domain::ConfigProvider {
public:
    // Prefix stripped before lookup, e.g. "RAWIMPORT_OUTPUT_DIR" → key "OUTPUT_DIR".
    explicit EnvConfigProvider(std::string prefix = "RAWIMPORT_");

    std::optional<std::string> get_string(const std::string& key) override;
    std::optional<int>         get_int(const std::string& key) override;
    std::optional<bool>        get_bool(const std::string& key) override;
    std::optional<double>      get_double(const std::string& key) override;
    std::map<std::string, std::string> to_map() override;
    domain::ValidationReport validate(const std::vector<domain::RequiredConfigKey>& required) override;

private:
    std::string prefix_;
    std::string resolve_key(const std::string& key) const;
};

} // namespace rawimport::infrastructure
