/*
 * rawimport-pipeline - ConfigProvider port interface
 */
#pragma once

#include <string>
#include <map>
#include <optional>
#include <vector>

#include "domain/types.hpp"

namespace rawimport::domain {

class ConfigProvider {
public:
    virtual ~ConfigProvider() = default;

    virtual std::optional<std::string> get_string(const std::string& key) = 0;
    virtual std::optional<int>         get_int(const std::string& key) = 0;
    virtual std::optional<bool>        get_bool(const std::string& key) = 0;
    virtual std::optional<double>      get_double(const std::string& key) = 0;

    // Return all config as a flat map (for display / validation).
    virtual std::map<std::string, std::string> to_map() = 0;

    // Validate required keys are present and non-empty.
    virtual ValidationReport validate(const std::vector<RequiredConfigKey>& required) = 0;
};

} // namespace rawimport::domain
