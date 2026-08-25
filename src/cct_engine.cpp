// cct_engine.cpp — CCT engine factory (PRD-CCT-001 §5.1, extended per review).
// All algorithms share the RawPy delegation path and differ only in the
// illuminant estimator the Python script applies (see scripts/cct_analyze.py):
//   rawpy_grayworld        - channel mean (Minkowski p=1)
//   rawpy_shadesofgrey     - Minkowski norm p=6 (Finlayson & Trezzi 2004)
//   rawpy_whitepatch       - per-channel 99th percentile
//   rawpy_maxrgb           - per-channel 95th percentile
//   rawpy_generalgrayworld - Gaussian center-weighted mean (sigma=0.3*min(h,w))
//   rawpy_bayesian         - gray-world blended 30% toward D65 prior
// rawpy_edge_detect / dnglab_histogram remain Phase 2 and are rejected here.
#include "cct_engine.h"

namespace rawimport {

namespace {

const char* const kSupported[] = {
    "rawpy_grayworld", "rawpy_shadesofgrey", "rawpy_whitepatch",
    "rawpy_maxrgb", "rawpy_generalgrayworld", "rawpy_bayesian",
};

} // namespace

bool CctAlgorithmSupported(const std::string& name) {
    for (const char* s : kSupported)
        if (name == s) return true;
    // tolerate a missing rawpy_ prefix
    std::string prefixed = "rawpy_" + name;
    for (const char* s : kSupported)
        if (prefixed == s || name == s) return true;
    return false;
}

CctEngine* MakeCctEngine(const std::string& name) {
    if (!CctAlgorithmSupported(name)) return nullptr;
    return MakeRawPyCctEngine();
}

} // namespace rawimport