#include "util.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <random>
#include <sstream>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/sendfile.h>

namespace rawimport {
namespace fs = std::filesystem;

std::string trim(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && std::isspace(static_cast<unsigned char>(s[a]))) ++a;
    while (b > a && std::isspace(static_cast<unsigned char>(s[b-1]))) --b;
    return s.substr(a, b - a);
}

std::string env_or(const std::string& key, const std::string& def) {
    const char* v = std::getenv(key.c_str());
    if (!v || v[0] == '\0') return def;
    return std::string(v);
}

bool env_bool(const std::string& key, bool def) {
    const char* v = std::getenv(key.c_str());
    if (!v) return def;
    std::string s = v;
    for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s == "1" || s == "true" || s == "yes";
}

void load_dotenv(const std::string& path) {
    std::ifstream in(path);
    if (!in) return;
    std::string line;
    while (std::getline(in, line)) {
        line = trim(line);
        if (line.empty() || line[0] == '#') continue;
        auto eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string k = trim(line.substr(0, eq));
        std::string v = trim(line.substr(eq + 1));
        if (!v.empty() && (v.front() == '"' || v.front() == '\'')) v = v.substr(1);
        if (!v.empty() && (v.back() == '"' || v.back() == '\'')) v = v.substr(0, v.size() - 1);
        if (std::getenv(k.c_str()) == nullptr) {
#ifdef _WIN32
            _putenv_s(k.c_str(), v.c_str());
#else
            setenv(k.c_str(), v.c_str(), 0);
#endif
        }
    }
}

bool ensure_dir(const std::string& path) {
    std::error_code ec;
    fs::create_directories(path, ec);
    return !ec || fs::is_directory(path);
}

bool touch_mtime(const std::string& path) {
    std::error_code ec;
    auto now = fs::file_time_type::clock::now();
    fs::last_write_time(path, now, ec);
    return !ec;
}

std::string random_token() {
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<> d(0, 15);
    static const char* hex = "0123456789abcdef";
    std::string s;
    s.reserve(16);
    for (int i = 0; i < 16; ++i) s += hex[d(gen)];
    return s;
}

std::string secure_random_token() {
    std::string s;
    s.resize(32);
    int fd = open("/dev/urandom", O_RDONLY | O_CLOEXEC);
    if (fd >= 0) {
        ssize_t n = read(fd, s.data(), s.size());
        close(fd);
        if (n == static_cast<ssize_t>(s.size())) {
            // Convert to hex
            static const char* hex = "0123456789abcdef";
            std::string out;
            out.reserve(s.size() * 2);
            for (unsigned char c : s) {
                out += hex[c >> 4];
                out += hex[c & 0xF];
            }
            return out;
        }
    }
    // Fallback to random_device if /dev/urandom unavailable
    std::random_device rd;
    std::uniform_int_distribution<> d(0, 15);
    static const char* hex = "0123456789abcdef";
    std::string out;
    out.reserve(64);
    for (int i = 0; i < 32; ++i) {
        out += hex[d(rd)];
        out += hex[d(rd)];
    }
    return out;
}

std::vector<std::string> split_sql(const std::string& sql) {
    std::vector<std::string> out;
    std::string cur;
    char quote = 0;  // 0 = none, '\'' = single, '"' = double, '`' = backtick
    bool escaped = false;
    
    for (char c : sql) {
        if (escaped) {
            cur += c;
            escaped = false;
            continue;
        }
        
        if (c == '\\') {
            cur += c;
            escaped = true;
            continue;
        }
        
        if (quote) {
            cur += c;
            if (c == quote) quote = 0;
            continue;
        }
        
        if (c == '\'' || c == '"' || c == '`') {
            quote = c;
            cur += c;
            continue;
        }
        
        if (c == ';') {
            std::string t = trim(cur);
            if (!t.empty()) out.push_back(t);
            cur.clear();
        } else {
            cur += c;
        }
    }
    
    std::string t = trim(cur);
    if (!t.empty()) out.push_back(t);
    return out;
}

bool move_file(const std::string& from, const std::string& to) {
    std::error_code ec;
    fs::rename(from, to, ec);
    if (!ec) return true;
    
    // Cross-device fallback: use copy_file_range or sendfile for efficiency
    int src_fd = open(from.c_str(), O_RDONLY | O_CLOEXEC);
    if (src_fd < 0) return false;
    
    struct stat src_stat;
    if (fstat(src_fd, &src_stat) < 0) {
        close(src_fd);
        return false;
    }
    
    int dst_fd = open(to.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, src_stat.st_mode);
    if (dst_fd < 0) {
        close(src_fd);
        return false;
    }
    
    // Try copy_file_range (Linux 4.5+)
    off_t offset = 0;
    ssize_t copied = 0;
    while (offset < src_stat.st_size) {
        ssize_t n = copy_file_range(src_fd, &offset, dst_fd, nullptr, 
                                     src_stat.st_size - offset, 0);
        if (n <= 0) break;
        copied += n;
    }
    
    bool success = false;
    if (copied == src_stat.st_size) {
        // Ensure data is on disk
        fsync(dst_fd);
        // Also fsync the parent directory
        std::string parent = fs::path(to).parent_path().string();
        int dir_fd = open(parent.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
        if (dir_fd >= 0) {
            fsync(dir_fd);
            close(dir_fd);
        }
        success = true;
    }
    
    close(src_fd);
    close(dst_fd);
    
    if (success) {
        fs::remove(from, ec);
        return !ec;
    }
    
    // Fallback to std::filesystem copy
    fs::copy_file(from, to, fs::copy_options::overwrite_existing, ec);
    if (ec) return false;
    
    // fsync the destination
    int fd = open(to.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd >= 0) {
        fsync(fd);
        close(fd);
    }
    
    fs::remove(from, ec);
    return !ec;
}

std::chrono::system_clock::time_point file_time_to_system(fs::file_time_type ft) {
    // file_clock epoch (C++20 / libstdc++) is 2174-01-01 UTC, i.e. 74,510 days
    // after the system epoch. Reinterpreting the raw duration without this
    // offset shifts dates by ~204 years (2026 mtime -> "1822").
    using namespace std::chrono;
    static constexpr auto kFileToSysOffset = duration_cast<system_clock::duration>(days{74510});
    return system_clock::time_point(
        duration_cast<system_clock::duration>(ft.time_since_epoch()) + kFileToSysOffset);
}

} // namespace rawimport
