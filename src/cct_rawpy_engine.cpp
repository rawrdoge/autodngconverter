// cct_rawpy_engine.cpp — pixel-level CCT analysis delegated to Python/rawpy
// (PRD-CCT-001 §5.2). The script prints one JSON line to stdout; we parse it
// with nlohmann::json (already linked on this branch).
#include "cct_engine.h"

#include <nlohmann/json.hpp>

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#ifdef _WIN32
#define RI_POPEN _popen
#define RI_PCLOSE _pclose
#define RI_PYTHON "python"
#else
#define RI_POPEN popen
#define RI_PCLOSE pclose
#define RI_PYTHON "python3"
#endif

namespace rawimport {

namespace {

std::string shell_quote(const std::string& s) {
#ifdef _WIN32
    return "\"" + s + "\"";
#else
    std::string out = "'";
    for (char c : s) { if (c == '\'') out += "'\\''"; else out += c; }
    out += "'";
    return out;
#endif
}

class RawPyCctEngine : public CctEngine {
public:
    std::string Name() const override { return "rawpy_grayworld"; }

    bool Available() const override {
        int rc = std::system(
            RI_PYTHON " -c 'import rawpy, colour, numpy' >NUL 2>&1");
        return rc == 0;
    }

    CctOutput Analyze(const CctInput& input) override {
        CctOutput out;
        const char* script = std::getenv("CCT_SCRIPT_PATH");
        std::string script_path =
            script && *script ? script : "scripts/cct_analyze.py";

        // Algorithm name for the script (strips the optional rawpy_ prefix;
        // the script dispatches on grayworld/shadesofgrey/whitepatch/...).
        std::string algo = input.algorithm.empty()
                               ? "rawpy_grayworld" : input.algorithm;
        const std::string prefix = "rawpy_";
        if (algo.rfind(prefix, 0) == 0) algo = algo.substr(prefix.size());

        std::string cmd = shell_quote(std::string(RI_PYTHON)) + " " +
                          shell_quote(script_path) + " " +
                          shell_quote(input.raw_path) + " " +
                          shell_quote(input.sampled_area.empty()
                                          ? "full"
                                          : input.sampled_area) + " " +
                          shell_quote(algo);

        FILE* f = RI_POPEN(cmd.c_str(), "r");
        if (!f) { out.error = "failed to spawn analyzer"; return out; }

        char buf[4096];
        std::string json_str;
        if (fgets(buf, sizeof(buf), f)) json_str = buf;
        int rc = RI_PCLOSE(f);
        if (json_str.empty()) {
            out.error = "analyzer produced no output";
            return out;
        }

        try {
            auto j = nlohmann::json::parse(json_str);
            if (j.contains("error")) {
                out.error = j.value("error", "analyzer error");
                return out;
            }
            if (!j.contains("cct")) {
                out.error = "analyzer output missing 'cct'";
                return out;
            }
            out.cct_kelvin = j.value("cct", 0.0);
            out.tint       = j.value("tint", 0.0);
            if (j.contains("xy") && j["xy"].is_array() && j["xy"].size() >= 2) {
                out.xy_x = j["xy"][0].get<double>();
                out.xy_y = j["xy"][1].get<double>();
            }
            out.hue        = j.value("hue", 0.0);
            out.chroma     = j.value("chroma", 0.0);
            out.confidence = j.value("confidence", 0.0);
        } catch (const std::exception& e) {
            out.error = std::string("failed to parse analyzer output: ") + e.what();
        }
        (void)rc; // non-zero exit already implies empty/error output above
        return out;
    }
};

} // namespace

CctEngine* MakeRawPyCctEngine() { return new RawPyCctEngine(); }

} // namespace rawimport