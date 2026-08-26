#pragma once
// cct_analyzer.h — synchronous native CCT analysis (PRD v2.3.0 §5).
// libraw decode + C++ pixel statistics; no worker, no persistence.
#include <array>
#include <string>

namespace rawimport {

struct CctResult {
    std::array<double, 3> gains;  // R, G, B multipliers (green normalized to 1.0)
    double cct = 0;               // Correlated color temperature (Kelvin)
    double tint = 0;              // Signed deviation from Planckian locus
                                  // (+ = green side), xy distance units
    bool ok = false;              // false if RAW decode failed
};

class CctAnalyzer {
public:
    // Supported methods: "grayworld" | "whitepatch"
    // Any other value returns ok=false.
    static CctResult analyze(const std::string& raw_path,
                             const std::string& method);
};

} // namespace rawimport