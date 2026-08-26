// cct_analyzer.cpp — synchronous native CCT analysis (PRD v2.3.0 §6).
//
// Pipeline: libraw decode (camera-native linear 16-bit RGB, no WB applied)
//   → illuminant estimate (shadesofgrey Minkowski p=6 | whitepatch 99th pct)
//   → green-normalized gains
//   → sRGB-matrix XYZ → xy chromaticity (scale-invariant)
//   → CCT/tint via Robertson-method isothermal search on the analytical
//     Planckian locus (piecewise exponential fit, Wyszecki & Stiles 2000).
//     NOTE: PRD §6.3 specifies a discrete 31-line Robertson table; this
//     implementation evaluates the same Planckian locus analytically, which
//     is the continuous superset of that table (no interpolation error).
//     Flagged as deviation D-impl-1 for review.
#include "cct_analyzer.h"

#include <libraw/libraw.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

namespace rawimport {

namespace {

constexpr double kEps = 1e-12;

// Simplified sRGB → XYZ matrix (D65). Acceptable per PRD §6.4; Phase 2 may
// use libraw imgdata.color.rgb_cam / cam_xyz for per-camera conversion.
constexpr double kSrgbToXyz[3][3] = {
    {0.4124564, 0.3575761, 0.1804375},
    {0.2126729, 0.7151522, 0.0721750},
    {0.0193339, 0.1191920, 0.9503041},
};

struct Rgb {
    double r = 0, g = 0, b = 0;
};

// Decode RAW to camera-native linear RGB via libraw.
// Returns row-major float64 pixels in [0, 1]-scaled 16-bit range.
std::vector<Rgb> decode_linear_rgb(const std::string& path, int& w, int& h) {
    LibRaw raw;
    if (raw.open_file(path.c_str()) != LIBRAW_SUCCESS)
        throw std::runtime_error("open failed");
    raw.imgdata.params.use_camera_wb = 0;
    raw.imgdata.params.use_auto_wb   = 0;
    raw.imgdata.params.no_auto_bright = 1;
    raw.imgdata.params.output_bps    = 16;
    raw.imgdata.params.output_color  = 0;  // camera-native RGB

    if (raw.unpack() != LIBRAW_SUCCESS)
        throw std::runtime_error("unpack failed");
    const int ret = raw.dcraw_process();
    if (ret != LIBRAW_SUCCESS)
        throw std::runtime_error("dcraw_process failed");

    w = raw.imgdata.sizes.width;
    h = raw.imgdata.sizes.height;
    if (w <= 0 || h <= 0) throw std::runtime_error("bad dimensions");

    // imgdata.image is interleaved 4-component ushort (RGBG); 4th is unused.
    const auto* px = raw.imgdata.image;  // ushort (*)[4]
    std::vector<Rgb> out(static_cast<size_t>(w) * h);
    for (size_t i = 0; i < out.size(); ++i) {
        out[i].r = static_cast<double>(px[i][0]);
        out[i].g = static_cast<double>(px[i][1]);
        out[i].b = static_cast<double>(px[i][2]);
    }
    return out;  // LibRaw cleans up on destruction
}

// Shades-of-Grey: per-channel Minkowski norm p=6 (Finlayson & Trezzi 2004).
// Samples are scaled to [0, 1] before the norm; the result is scale-invariant
// downstream (chromaticity only depends on channel ratios).
constexpr double kSogP = 6.0;
Rgb shadesofgrey(const std::vector<Rgb>& px) {
    Rgb acc;
    for (const auto& p : px) {
        acc.r += std::pow(p.r / 65535.0 + kEps, kSogP);
        acc.g += std::pow(p.g / 65535.0 + kEps, kSogP);
        acc.b += std::pow(p.b / 65535.0 + kEps, kSogP);
    }
    const double n = static_cast<double>(px.size());
    const double inv = 1.0 / kSogP;
    return {std::pow(acc.r / n, inv),
            std::pow(acc.g / n, inv),
            std::pow(acc.b / n, inv)};
}

// White-patch: per-channel 99th percentile via exact 16-bit histogram.
Rgb whitepatch(const std::vector<Rgb>& px) {
    constexpr int kBins = 65536;
    std::vector<uint64_t> hist[3] = {
        std::vector<uint64_t>(kBins, 0),
        std::vector<uint64_t>(kBins, 0),
        std::vector<uint64_t>(kBins, 0),
    };
    for (const auto& p : px) {
        ++hist[0][static_cast<int>(p.r)];
        ++hist[1][static_cast<int>(p.g)];
        ++hist[2][static_cast<int>(p.b)];
    }
    const double target = 0.99 * static_cast<double>(px.size());
    auto percentile_bin = [&hist, &target](int c) {
        uint64_t acc = 0;
        for (int v = 0; v < kBins; ++v) {
            acc += hist[c][v];
            if (static_cast<double>(acc) >= target) return v;
        }
        return kBins - 1;
    };
    return {static_cast<double>(percentile_bin(0)),
            static_cast<double>(percentile_bin(1)),
            static_cast<double>(percentile_bin(2))};
}

Rgb estimate_illuminant(const std::vector<Rgb>& px, const std::string& method) {
    if (method == "shadesofgrey") return shadesofgrey(px);
    if (method == "whitepatch")   return whitepatch(px);
    throw std::runtime_error("unsupported method: " + method);
}

// Analytical Planckian locus in xy (Wyszecki & Stiles 2000 piecewise fit),
// valid 1667 K – 25000 K.
double locus_x(double t) {
    const double it = 1.0 / t;
    if (t < 4000.0)
        return -0.2661239e9 * it * it * it - 0.2343580e6 * it * it +
               0.8776956e3 * it + 0.179910;
    return -3.0258469e9 * it * it * it + 2.1070379e6 * it * it +
           0.2226347e3 * it + 0.240390;
}

double locus_y_from_x(double t, double x) {
    const double x2 = x * x, x3 = x2 * x;
    if (t < 2222.0)
        return -1.1063814 * x3 - 1.34811020 * x2 + 2.18555832 * x - 0.20219683;
    if (t < 4000.0)
        return -0.9549476 * x3 - 1.37418593 * x2 + 2.09137015 * x - 0.16748867;
    return 3.0817580 * x3 - 5.87338670 * x2 + 3.75112997 * x - 0.37001483;
}

struct Xy { double x = 0, y = 0; };
Xy locus_xy(double t) {
    Xy p;
    p.x = locus_x(t);
    p.y = locus_y_from_x(t, p.x);
    return p;
}

} // namespace

CctResult CctAnalyzer::analyze(const std::string& raw_path,
                               const std::string& method) {
    CctResult result;
    if (method != "shadesofgrey" && method != "whitepatch") {
        result.ok = false;
        return result;
    }

    int w = 0, h = 0;
    std::vector<Rgb> pixels;
    try {
        pixels = decode_linear_rgb(raw_path, w, h);
    } catch (const std::exception&) {
        result.ok = false;
        return result;
    }
    if (pixels.empty()) { result.ok = false; return result; }

    // Illuminant estimate (camera-native linear RGB).
    const Rgb illum = estimate_illuminant(pixels, method);

    // Green-normalized WB gains: gain_c = G / C (green multiplier == 1.0).
    auto safe_gain = [g = illum.g](double c) {
        return c > kEps ? g / c : 1.0;
    };
    result.gains[0] = safe_gain(illum.r);
    result.gains[1] = 1.0;
    result.gains[2] = safe_gain(illum.b);

    // xy chromaticity via sRGB matrix (scale-invariant).
    double X = 0, Y = 0, Z = 0;
    for (int i = 0; i < 3; ++i) {
        const double ch = (&illum.r)[i];
        X += kSrgbToXyz[i][0] * ch;
        Y += kSrgbToXyz[i][1] * ch;
        Z += kSrgbToXyz[i][2] * ch;
    }
    const double sum = X + Y + Z;
    if (sum <= kEps) { result.ok = false; return result; }
    const Xy s{X / sum, Y / sum};

    // Robertson-method CCT/tint: isothermal search on the analytical
    // Planckian locus. Minimize Euclidean distance from the sample to the
    // locus over log-temperature (unimodal in this range); tint is the
    // signed perpendicular deviation (+ toward green).
    auto dist_sq = [&s](double t) {
        const Xy p = locus_xy(t);
        return (s.x - p.x) * (s.x - p.x) + (s.y - p.y) * (s.y - p.y);
    };
    constexpr double kTMin = 1667.0, kTMax = 25000.0;
    double lo = std::log(kTMin), hi = std::log(kTMax);
    for (int iter = 0; iter < 60; ++iter) {   // golden-section search
        const double m1 = lo + (hi - lo) * 0.3819660112501051;
        const double m2 = hi - (hi - lo) * 0.3819660112501051;
        if (dist_sq(std::exp(m1)) < dist_sq(std::exp(m2))) hi = m2; else lo = m1;
    }
    const double t_best = std::exp((lo + hi) / 2.0);
    result.cct = t_best;

    // Signed perpendicular tint at the closest point.
    const Xy p = locus_xy(t_best);
    const double d = 1.0;                     // finite-difference step (K)
    Xy pa = locus_xy(t_best - d), pb = locus_xy(t_best + d);
    double tx = pb.x - pa.x, ty = pb.y - pa.y;
    const double tl = std::sqrt(tx * tx + ty * ty) + kEps;
    tx /= tl; ty /= tl;
    result.tint = (s.x - p.x) * (-ty) + (s.y - p.y) * tx;

    result.ok = true;
    return result;
}

} // namespace rawimport
