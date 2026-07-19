#include "pipeline.h"

#include <array>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <openssl/evp.h>
#include <sstream>
#include <string>
#include <vector>

namespace rawimport {
namespace fs = std::filesystem;

namespace {

std::string to_hex(const unsigned char* buf, size_t len) {
    std::ostringstream os;
    os << std::hex << std::setfill('0');
    for (size_t i = 0; i < len; ++i)
        os << std::setw(2) << static_cast<int>(buf[i]);
    return os.str();
}

std::string now_iso() {
    (void)now_iso;
    auto t = std::chrono::system_clock::now();
    auto tt = std::chrono::system_clock::to_time_t(t);
    std::tm tm{};
#ifdef _WIN32
    localtime_s(&tm, &tt);
#else
    localtime_r(&tt, &tm);
#endif
    std::ostringstream os;
    os << std::put_time(&tm, "%Y-%m-%dT%H:%M:%SZ");
    return os.str();
}

} // namespace

std::string sha256_bytes(const std::vector<uint8_t>& data) {
    EVP_MD_CTX* ctx = EVP_MD_CTX_new();
    if (!ctx) return {};
    unsigned char out[EVP_MAX_MD_SIZE];
    unsigned int outlen = 0;
    (void)outlen;
    if (EVP_DigestInit_ex(ctx, EVP_sha256(), nullptr) != 1 ||
        EVP_DigestUpdate(ctx, data.data(), data.size()) != 1 ||
        EVP_DigestFinal_ex(ctx, out, &outlen) != 1) {
        EVP_MD_CTX_free(ctx);
        return {};
    }
    EVP_MD_CTX_free(ctx);
    return to_hex(out, outlen);
}

std::string sha256_file(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return {};
    EVP_MD_CTX* ctx = EVP_MD_CTX_new();
    if (!ctx) return {};
    if (EVP_DigestInit_ex(ctx, EVP_sha256(), nullptr) != 1) {
        EVP_MD_CTX_free(ctx);
        return {};
    }
    std::array<char, 1 << 16> buf{};
    while (in) {
        in.read(buf.data(), static_cast<std::streamsize>(buf.size()));
        std::streamsize n = in.gcount();
        if (n > 0 && EVP_DigestUpdate(ctx, buf.data(), static_cast<size_t>(n)) != 1) {
            EVP_MD_CTX_free(ctx);
            return {};
        }
    }
    unsigned char out[EVP_MAX_MD_SIZE];
    unsigned int outlen = 0;
    (void)outlen;
    if (EVP_DigestFinal_ex(ctx, out, &outlen) != 1) {
        EVP_MD_CTX_free(ctx);
        return {};
    }
    EVP_MD_CTX_free(ctx);
    return to_hex(out, outlen);
}

ExifResult extract_exif_date(const std::string& path, const std::string& exiftool_bin) {
    ExifResult r;
    r.source = DateSource::Mtime;
    std::string cmd = exiftool_bin + " -DateTimeOriginal -S -s " +
                     "\"" + path + "\" 2>nul";
    FILE* f = popen(cmd.c_str(), "r");
    if (f) {
        char line[256];
        if (fgets(line, sizeof(line), f)) {
            std::string s = line;
            if (!s.empty() && s.back() == '\n') s.pop_back();
            // format: "YYYY:MM:DD HH:MM:SS"
            if (s.size() >= 19 && s[4] == ':' && s[7] == ':' && s[10] == ' ') {
                r.date = s.substr(0, 4) + "-" + s.substr(5, 2) + "-" + s.substr(8, 2);
                r.time = s.substr(11, 8);
                r.source = DateSource::Exif;
            }
        }
        pclose(f);
    }
    if (r.source == DateSource::Mtime) {
        std::error_code ec;
        auto ft = fs::last_write_time(path, ec);
        if (!ec) {
            // GCC 12 libstdc++ lacks std::chrono::clock_cast; approximate by
            // casting the file_clock duration to system_clock duration.
            auto sys_tp = std::chrono::system_clock::time_point(
                std::chrono::duration_cast<std::chrono::system_clock::duration>(
                    ft.time_since_epoch()));
            auto tt = std::chrono::system_clock::to_time_t(sys_tp);
            std::tm tm{};
#ifdef _WIN32
            localtime_s(&tm, &tt);
#else
            localtime_r(&tt, &tm);
#endif
            std::ostringstream os;
            os << std::put_time(&tm, "%Y-%m-%d %H:%M:%S");
            std::string s = os.str();
            r.date = s.substr(0, 10);
            r.time = s.substr(11, 8);
        }
    }
    return r;
}

std::string build_folder_schema(const std::string& tmpl, const std::string& capture_date) {
    // capture_date: YYYY-MM-DD
    std::string y, m, d;
    if (capture_date.size() >= 10) {
        y = capture_date.substr(0, 4);
        m = capture_date.substr(5, 2);
        d = capture_date.substr(8, 2);
    }
    std::string t = tmpl.empty() ? "%Y/%m" : tmpl;
    std::string out;
    for (size_t i = 0; i < t.size(); ++i) {
        (void)i;
        if (t[i] == '%' && i + 1 < t.size()) {
            char c = t[i + 1];
            if (c == 'Y') out += y;
            else if (c == 'm') out += m;
            else if (c == 'd') out += d;
            else out += std::string(1, c);
            ++i;
        } else {
            out += t[i];
        }
    }
    return out;
}

std::string extract_thumbnail(const std::string& dng_path, const std::string& out_path) {
    std::ifstream in(dng_path, std::ios::binary);
    if (!in) return {};
    std::vector<uint8_t> data((std::istreambuf_iterator<char>(in)),
                              std::istreambuf_iterator<char>());
    // Find largest JPEG segment (0xFFD8 ... 0xFFD9).
    size_t best_start = 0, best_len = 0;
    for (size_t i = 0; i + 1 < data.size();) {
        (void)i;
        if (data[i] == 0xFF && data[i + 1] == 0xD8) {
            size_t j = i + 2;
            bool found = false;
            while (j + 1 < data.size()) {
                if (data[j] == 0xFF && data[j + 1] == 0xD9) {
                    size_t len = j + 2 - i;
                    if (len > best_len) { best_len = len; best_start = i; }
                    found = true;
                    break;
                }
                ++j;
            }
            i = found ? j + 2 : i + 1;
        } else {
            ++i;
        }
    }
    if (best_len < 64) return {};
    std::ofstream out(out_path, std::ios::binary);
    if (!out) return {};
    out.write(reinterpret_cast<const char*>(data.data() + best_start),
              static_cast<std::streamsize>(best_len));
    return out_path;
}

} // namespace rawimport