#pragma once
// converter.h — ConverterEngine interface for the RawImport C++ rewrite.
// Phase 0 bootstrap (LEAD). See PRD_RawImport_Pipeline_CppRewrite.md §3.1, §6.2.
#include <string>
#include "pipeline.h"

namespace rawimport {

// Abstract conversion engine. dnglab is the default; adobedng/libraw are stubs.
class ConverterEngine {
public:
    virtual ~ConverterEngine() = default;
    virtual std::string Name() const = 0;
    virtual bool Available() const = 0;

    // Convert src RAW -> dst DNG using settings. Returns true on success.
    // Implementations MUST use the authoritative dnglab CLI (see pipeline.h note):
    //   dnglab convert --input <SRC> --output <DST> -c <compression>
    //          --keep-mtime <true|false> -f
    virtual bool Convert(const std::string& src, const std::string& dst,
                         const ConversionSettings& settings) = 0;
};

// Re-embed a JPEG preview into an existing DNG (used by rotation + re-embed).
// Worker preference: dnglab reembed -> DNG SDK -> exiftool. Fixed seed for idempotency.
class PreviewEmbedder {
public:
    virtual ~PreviewEmbedder() = default;
    virtual std::string Name() const = 0;
    virtual bool Available() const = 0;
    // Embeds jpeg_path into dng_path in place. Returns true on success.
    virtual bool Embed(const std::string& dng_path, const std::string& jpeg_path,
                       const std::string& seed) = 0;
    // Write EXIF Orientation tag (1-8) to dng_path. Returns true on success.
    virtual bool SetOrientation(const std::string& dng_path, int orientation) = 0;
};

// Factory: selects engine by CONVERTER_ENGINE env (default dnglab).
ConverterEngine* MakeConverter(const std::string& engine_name);

// Build the preferred embedder chain (dnglab first, then DNG SDK, then exiftool).
PreviewEmbedder* MakeEmbedder();

} // namespace rawimport