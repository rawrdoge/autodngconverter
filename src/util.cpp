#include "util.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <random>
#include <sstream>

namespace rawimport {
namespace fs = std::filesystem;

std::string trim(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && std::isspace(static_cast<unsigned char>(s[a]))) ++a;
    while (b > a && std::isspace(static_cast<unsigned char>(s[b-1]))) --b;
    return s.substr(a, b - a);
}

std::vector<std::string> split_sql(const std::string& sql) {
    std::vector<std::string> out;
    std::string cur;
    for (char c : sql) {
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

bool move_file(const std::string& from, const std::string& to) {
    std::error_code ec;
    fs::rename(from, to, ec);
    if (!ec) return true;
    // fallback: copy + remove
    std::error_code ec2;
    fs::copy_file(from, to, fs::copy_options::overwrite_existing, ec2);
    if (ec2) return false;
    fs::remove(from, ec2);
    return !ec2;
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

} // namespace rawimport