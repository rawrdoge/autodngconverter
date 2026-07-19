// dng_previewembed.cpp — standalone best-effort preview re-embed tool.
// CLI: dng_previewembed <dng_path> <jpeg_path> <width> <height> <quality>
// Replaces (or appends) an embedded JPEG preview in a DNG file.
// v1: segment-scan approach (no full TIFF muxer). See PRD §5 / orchestration.
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

static std::vector<uint8_t> read_file(const std::string& p) {
    std::ifstream in(p, std::ios::binary);
    if (!in) return {};
    return std::vector<uint8_t>((std::istreambuf_iterator<char>(in)),
                              std::istreambuf_iterator<char>());
}

static bool write_file(const std::string& p, const std::vector<uint8_t>& d) {
    std::ofstream out(p, std::ios::binary | std::ios::trunc);
    if (!out) return false;
    out.write(reinterpret_cast<const char*>(d.data()),
              static_cast<std::streamsize>(d.size()));
    return true;
}

// Find first JPEG segment (0xFFD8 ... 0xFFD9). Returns {start,len} or {0,0}.
static std::pair<size_t, size_t> find_jpeg(const std::vector<uint8_t>& d) {
    for (size_t i = 0; i + 1 < d.size();) {
        if (d[i] == 0xFF && d[i + 1] == 0xD8) {
            size_t j = i + 2;
            while (j + 1 < d.size()) {
                if (d[j] == 0xFF && d[j + 1] == 0xD9)
                    return {i, j + 2 - i};
                ++j;
            }
            return {i, d.size() - i};
        }
        ++i;
    }
    return {0, 0};
}

int main(int argc, char** argv) {
    if (argc < 6) {
        std::cerr << "usage: dng_previewembed <dng> <jpeg> <w> <h> <q>\n";
        return 2;
    }
    std::string dng = argv[1];
    std::string jpeg = argv[2];
    int w = std::stoi(argv[3]);
    int h = std::stoi(argv[4]);
    int q = std::stoi(argv[5]);

    if (!fs::exists(dng)) { std::cerr << "dng not found\n"; return 1; }
    auto jdata = read_file(jpeg);
    if (jdata.size() < 64 || jdata[0] != 0xFF || jdata[1] != 0xD8) {
        std::cerr << "jpeg invalid (no SOI)\n"; return 1;
    }
    auto dng_data = read_file(dng);
    if (dng_data.empty()) { std::cerr << "dng read failed\n"; return 1; }

    auto [start, len] = find_jpeg(dng_data);
    std::vector<uint8_t> out;
    bool replaced = false;
    if (len > 0) {
        // replace in place
        out.reserve(dng_data.size() - len + jdata.size());
        out.insert(out.end(), dng_data.begin(), dng_data.begin() + start);
        out.insert(out.end(), jdata.begin(), jdata.end());
        out.insert(out.end(), dng_data.begin() + start + len, dng_data.end());
        replaced = true;
    } else {
        // append (IFD linkage not updated — best effort)
        std::cerr << "warning: no preview segment found, appending JPEG (IFD not relinked)\n";
        out = std::move(dng_data);
        out.insert(out.end(), jdata.begin(), jdata.end());
    }

    if (!write_file(dng, out)) { std::cerr << "write failed\n"; return 1; }

    std::cout << "{\"dng\":\"" << dng << "\",\"jpeg\":\"" << jpeg
              << "\",\"width\":" << w << ",\"height\":" << h
              << ",\"quality\":" << q << ",\"bytes\":" << jdata.size()
              << ",\"status\":\"ok\"}" << std::endl;
    if (replaced) std::cerr << "replaced existing preview segment\n";
    return 0;
}