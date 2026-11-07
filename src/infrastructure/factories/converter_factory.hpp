/*
 * rawimport-pipeline - Converter Factory interface
 */
#pragma once

#include <memory>

#include "domain/ports/config_provider.hpp"
#include "domain/ports/converter_engine.hpp"
#include "domain/ports/log_sink.hpp"

namespace rawimport::infrastructure {

// Create a converter engine based on config and log.
std::unique_ptr<domain::ConverterEngine>
MakeConverterEngine(domain::ConfigProvider& config,
                   const std::shared_ptr<domain::LogSink>& log);

} // namespace rawimport::infrastructure
