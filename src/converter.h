#pragma once
// converter.h — ConverterEngine interface for the RawImport portable build.
#include <string>
#include "config.h"
#include "pipeline.h"

namespace rawimport {

// Abstract conversion engine. dnglab is the only engine in the portable build.
class ConverterEngine {
public:
    virtual ~ConverterEngine() = default;
    virtual std::string Name() const = 0;
    virtual bool Available() const = 0;

    // Convert src RAW -> dst DNG using settings. Returns true on success.
    // Implementations MUST use the dnglab CLI form:
    //   dnglab convert <INPUT> <OUTPUT> -c <lossless|uncompressed> -f
    virtual bool Convert(const std::string& src, const std::string& dst,
                         const ConversionSettings& settings) = 0;
};

// Factory: resolves the dnglab binary via config precedence
// (CONVERTER_ENGINE_BIN -> <root>/tools/dnglab -> PATH).
ConverterEngine* MakeConverter(const Config& cfg);

} // namespace rawimport