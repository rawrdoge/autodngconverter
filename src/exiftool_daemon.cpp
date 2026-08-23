#include "exiftool_daemon.h"
#include "util.h"

#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <spdlog/spdlog.h>
#ifndef _WIN32
#include <sys/wait.h>
#include <unistd.h>
#endif
namespace rawimport {
namespace fs = std::filesystem;

#ifndef _WIN32
// ---------------------------------------------------------------------
// POSIX implementation (fork + pipes + waitpid)
// ---------------------------------------------------------------------

ExifToolDaemon::ExifToolDaemon(const std::string& exiftool_bin)
    : exiftool_bin_(exiftool_bin) {
    if (!start()) {
        SPDLOG_WARN("[exiftool] daemon failed to start, will use one-shot fallback");
    }
}

ExifToolDaemon::~ExifToolDaemon() {
    stop();
}

bool ExifToolDaemon::start() {
    int stdin_pipe[2], stdout_pipe[2];
    if (pipe(stdin_pipe) != 0 || pipe(stdout_pipe) != 0) {
        SPDLOG_ERROR("[exiftool] pipe creation failed");
        return false;
    }
    
    child_pid_ = fork();
    if (child_pid_ < 0) {
        SPDLOG_ERROR("[exiftool] fork failed");
        close(stdin_pipe[0]); close(stdin_pipe[1]);
        close(stdout_pipe[0]); close(stdout_pipe[1]);
        return false;
    }
    
    if (child_pid_ == 0) {
        // Child process
        close(stdin_pipe[1]);  // Close write end of stdin pipe
        close(stdout_pipe[0]); // Close read end of stdout pipe
        
        dup2(stdin_pipe[0], STDIN_FILENO);
        dup2(stdout_pipe[1], STDOUT_FILENO);
        
        close(stdin_pipe[0]);
        close(stdout_pipe[1]);
        
        // Execute exiftool with stay_open
        execlp(exiftool_bin_.c_str(), exiftool_bin_.c_str(),
               "-stay_open", "True", "-@", "-", (char*)nullptr);
        _exit(127);
    }
    
    // Parent process
    close(stdin_pipe[0]);  // Close read end of stdin pipe
    close(stdout_pipe[1]); // Close write end of stdout pipe
    
    stdin_fd_ = stdin_pipe[1];
    stdout_fd_ = stdout_pipe[0];
    
    // Wrap the stdout fd in ONE long-lived buffered stream (A7: no dup/fdopen
    // per command — the old per-call dup leaked nothing but was wasteful and
    // made EOF detection impossible).
    FILE* stdout_f = fdopen(stdout_fd_, "r");
    if (!stdout_f) {
        SPDLOG_ERROR("[exiftool] fdopen failed");
        stop();
        return false;
    }
    stdout_f_ = stdout_f;
    
    // NOTE: exiftool does NOT print an initial "{ready}" at startup — it only
    // emits "{ready}" after each "-execute". The -ver test command below is
    // therefore the real startup handshake (and doubles as a liveness check).
    // Waiting for a phantom initial sentinel here deadlocked Worker startup.
    
    // Mark running BEFORE the test: send_command() refuses to run while
    // !running_, so setting it after the test made the test always fail and
    // the daemon silently never start.
    running_.store(true);
    
    // Sanity-test the daemon with a trivial command; reads until "{ready}".
    // EOF here means exec failed or exiftool died immediately.
    std::string test = send_command("-ver\n-execute\n");
    if (test.empty()) {
        SPDLOG_ERROR("[exiftool] daemon test command failed");
        stop();
        return false;
    }
    
    SPDLOG_INFO("[exiftool] daemon started (pid={})", child_pid_);
    return true;
}

std::string ExifToolDaemon::send_command(const std::string& cmd) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    if (!running_.load() || child_pid_ <= 0 || !stdout_f_) return "";
    
    // Write command to stdin
    ssize_t written = write(stdin_fd_, cmd.c_str(), cmd.size());
    if (written != static_cast<ssize_t>(cmd.size())) {
        SPDLOG_ERROR("[exiftool] write failed; disabling daemon (falling back to one-shot)");
        running_.store(false);
        return "";
    }
    
    // Read response until "{ready}" sentinel from the shared stream.
    // EOF before the sentinel means exiftool died -> disable daemon so
    // callers fall back to one-shot mode (PRD §5.2 fallback).
    std::string result;
    char line[1024];
    bool got_ready = false;
    while (fgets(line, sizeof(line), stdout_f_)) {
        std::string s = line;
        if (!s.empty() && s.back() == '\n') s.pop_back();
        if (s == "{ready}") { got_ready = true; break; }
        if (!result.empty()) result += "\n";
        result += s;
    }
    if (!got_ready) {
        SPDLOG_ERROR("[exiftool] EOF before '{ready}'; disabling daemon (falling back to one-shot)");
        running_.store(false);
        return "";
    }
    
    return result;
}

ExifResult ExifToolDaemon::extract_date(const std::string& path) {
    ExifResult r;
    r.source = DateSource::Mtime;
    
    if (!running_.load()) {
        // Fallback to mtime
        std::error_code ec;
        auto ft = fs::last_write_time(path, ec);
        if (!ec) {
            auto sys_tp = file_time_to_system(ft);
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
        return r;
    }
    
    // Use daemon: -S -DateTimeOriginal -FileModifyDate <file>
    // Sanitize the path for the -@ argfile protocol: a leading '-' would be
    // parsed as an exiftool option, and a newline would split it into two args.
    std::string response;
    std::string safe_path = path;
    if (!safe_path.empty() && safe_path[0] == '-') safe_path.insert(0, "./");
    if (safe_path.find('\n') == std::string::npos) {
        // -@ argfile protocol: each argument must be on its OWN line —
        // a single "-S -Tag <path>" line is parsed as one bogus tag
        // assignment ("Invalid TAG name").
        std::string cmd = "-S\n-DateTimeOriginal\n-FileModifyDate\n" + safe_path + "\n-execute\n";
        response = send_command(cmd);
    } else {
        SPDLOG_WARN("[exiftool] refusing path containing newline, falling back to mtime: {}", path);
    }
    
    SPDLOG_DEBUG("[exiftool] raw response for {}: [{}]", path, response);

    if (response.empty()) {
        SPDLOG_WARN("[exiftool] empty response for {}, falling back to mtime", path);
    } else {
        // Parse response: "DateTimeOriginal: 2026:07:05 14:30:22"
        //                 "FileModifyDate: 2026:07:05 14:30:22"
        std::istringstream iss(response);
        std::string line;
        while (std::getline(iss, line)) {
            if (line.rfind("DateTimeOriginal:", 0) == 0) {
                std::string val = line.substr(17); // skip "DateTimeOriginal:"
                // trim leading whitespace ("Tag: value") before format checks
                size_t p = val.find_first_not_of(" \t");
                if (p != std::string::npos) val = val.substr(p);
                if (val.size() >= 19 && val[4] == ':' && val[7] == ':' && val[10] == ' ') {
                    r.date = val.substr(0, 4) + "-" + val.substr(5, 2) + "-" + val.substr(8, 2);
                    r.time = val.substr(11, 8);
                    r.source = DateSource::Exif;
                    break;
                }
            }
        }
    }
    
    // Fallback to mtime if EXIF not found
    if (r.source == DateSource::Mtime) {
        std::error_code ec;
        auto ft = fs::last_write_time(path, ec);
        if (!ec) {
            auto sys_tp = file_time_to_system(ft);
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

void ExifToolDaemon::stop() {
    if (stdin_fd_ >= 0) {
        // Ask exiftool to terminate (-stay_open False), then signal EOF by
        // closing stdin so the process exits even if it missed the command.
        std::string cmd = "-stay_open\nFalse\n-execute\n";
        write(stdin_fd_, cmd.c_str(), cmd.size());
        close(stdin_fd_);
        stdin_fd_ = -1;
    }
    
    if (stdout_f_) {
        fclose(stdout_f_);   // also closes the underlying stdout_fd_
        stdout_f_ = nullptr;
        stdout_fd_ = -1;
    } else if (stdout_fd_ >= 0) {
        close(stdout_fd_);
        stdout_fd_ = -1;
    }
    
    if (child_pid_ > 0) {
        int status;
        waitpid(static_cast<pid_t>(child_pid_), &status, 0);
        child_pid_ = -1;
    }
    
    if (running_.exchange(false)) {
        SPDLOG_INFO("[exiftool] daemon stopped");
    }
}

#else // _WIN32
// ---------------------------------------------------------------------
// Windows stub: fork/pipes/waitpid are POSIX-only, so the persistent
// daemon is unavailable. healthy() reports false and extract_date()
// delegates to the one-shot exiftool path (PRD §5.2 fallback), so all
// Worker logic stays identical across platforms.
// ---------------------------------------------------------------------

ExifToolDaemon::ExifToolDaemon(const std::string& exiftool_bin)
    : exiftool_bin_(exiftool_bin) {
    SPDLOG_INFO("[exiftool] persistent daemon not supported on Windows; using one-shot exiftool");
}

ExifToolDaemon::~ExifToolDaemon() {}

void ExifToolDaemon::stop() {}

std::string ExifToolDaemon::send_command(const std::string&) { return ""; }

ExifResult ExifToolDaemon::extract_date(const std::string& path) {
    return extract_exif_date(path, exiftool_bin_);
}
#endif

} // namespace rawimport