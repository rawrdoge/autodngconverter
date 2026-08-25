#pragma once
// cct_engine.h — abstract CCT analysis engine interface (PRD-CCT-001 §5.1).
// Pixel-level illuminant estimation is delegated to external tools
// (Python/rawpy); the service only manages filesystem/subprocess access.
#include <string>

namespace rawimport {

struct CctInput {
    std::string raw_path;     // Source RAW file path
    std::string dng_path;     // Converted DNG path (fallback source)
    std::string sampled_area; // "full" | "center20" | "center10" | JSON rect
    std::string algorithm;    // API name, e.g. "rawpy_shadesofgrey" (rawpy_ prefix optional)
};

// True when the name is a supported analysis algorithm:
//   rawpy_grayworld / rawpy_shadesofgrey / rawpy_whitepatch /
//   rawpy_maxrgb / rawpy_generalgrayworld / rawpy_bayesian
// ("rawpy_" prefix optional in `name`.)
bool CctAlgorithmSupported(const std::string& name);

struct CctOutput {
    double cct_kelvin = 0;
    double tint = 0;
    double xy_x = 0;
    double xy_y = 0;
    double hue = 0;        // D50-adapted hue in radians
    double chroma = 0;     // D50-adapted chroma
    double confidence = 0;
    std::string error;     // empty on success
};

class CctEngine {
public:
    virtual ~CctEngine() = default;
    virtual std::string Name() const = 0;

    // True when the external toolchain (python3 + rawpy/colour/numpy) exists.
    virtual bool Available() const = 0;

    // Blocking analysis call. Runs on the CctWorker thread only.
    virtual CctOutput Analyze(const CctInput& input) = 0;
};

// Factory: selects engine by name. Supported: "rawpy_grayworld".
// Returns nullptr for unknown / not-yet-implemented algorithm names.
CctEngine* MakeCctEngine(const std::string& name);

// Implemented in cct_rawpy_engine.cpp (called by the factory).
CctEngine* MakeRawPyCctEngine();

} // namespace rawimport