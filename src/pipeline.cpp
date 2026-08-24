#include "pipeline.h"
#include "util.h"

#include <sys/stat.h>

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

ExifResult exif_from_mtime(const std::string& path) {
    // stat()-based mtime fallback (D18/D26 closure): epoch-portable on every
    // toolchain; replaces both the broken one-shot popen path and the
    // std::filesystem file_clock conversions.
    ExifResult r;
    r.source = DateSource::Mtime;
    std::tm tm{};
    if (!file_mtime_tm(path, tm)) return r;
    std::ostringstream os;
    os << std::put_time(&tm, "%Y-%m-%d %H:%M:%S");
    std::string s = os.str();
    r.date = s.substr(0, 10);
    r.time = s.substr(11, 8);
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

// FNV-1a 64-bit hash (no external dependency).
uint64_t fnv1a_64(const uint8_t* data, size_t len) {
    uint64_t hash = 0xcbf29ce484222325ULL; // FNV offset basis
    for (size_t i = 0; i < len; ++i) {
        hash ^= data[i];
        hash *= 0x100000001b3ULL; // FNV prime
    }
    return hash;
}

FastFingerprint compute_fast_fingerprint(const std::string& path) {
    FastFingerprint fp;

    // stat() supplies size + mtime (Unix-epoch seconds) in one call —
    // epoch-portable, no std::filesystem time conversion involved.
#ifdef _WIN32
    struct _stat st {};
    if (_stat(path.c_str(), &st) != 0) return fp;
#else
    struct stat st {};
    if (stat(path.c_str(), &st) != 0) return fp;
#endif
    fp.size = static_cast<uint64_t>(st.st_size);
    fp.mtime = static_cast<uint64_t>(st.st_mtime);

    // Read first 4KB for FNV-1a hash
    std::ifstream in(path, std::ios::binary);
    if (!in) return fp;
    
    std::array<uint8_t, 4096> buf{};
    in.read(reinterpret_cast<char*>(buf.data()), buf.size());
    std::streamsize n = in.gcount();
    if (n > 0) {
        fp.fnv1a_4k = fnv1a_64(buf.data(), static_cast<size_t>(n));
    }
    
    return fp;
}

} // namespace rawimport
