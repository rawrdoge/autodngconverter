#include "converter.h"
#include "config.h"
#include "pipeline.h"

#include <array>
#include <chrono>
#include <cstdlib>
#include <memory>
#include <string>
#include <thread>

#ifdef _WIN32
#include <windows.h>
#else
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace rawimport {

namespace {

// Run an external command, return its exit code. Applies a timeout (ms);
// if exceeded, the child is killed and we return -1.
int run_timed(const std::string& cmd, int timeout_ms) {
#ifdef _WIN32
    STARTUPINFOA si{};
    PROCESS_INFORMATION pi{};
    si.cb = sizeof(si);
    std::string c = cmd;
    if (!CreateProcessA(nullptr, c.data(), nullptr, nullptr, FALSE, 0,
                        nullptr, nullptr, &si, &pi))
        return -1;
    DWORD waited = WaitForSingleObject(pi.hProcess, timeout_ms);
    if (waited == WAIT_TIMEOUT) {
        TerminateProcess(pi.hProcess, 1);
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
        return -1;
    }
    DWORD code = 0;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    return static_cast<int>(code);
#else
    pid_t pid = fork();
    if (pid < 0) return -1;
    if (pid == 0) {
        execl("/bin/sh", "sh", "-c", cmd.c_str(), (char*)nullptr);
        _exit(127);
    }
    // parent: poll for exit with timeout
    auto deadline = std::chrono::steady_clock::now() +
                     std::chrono::milliseconds(timeout_ms);
    int status = 0;
    while (true) {
        int w = waitpid(pid, &status, WNOHANG);
        if (w == pid) {
            if (WIFEXITED(status)) return WEXITSTATUS(status);
            return -1;
        }
        if (std::chrono::steady_clock::now() >= deadline) {
            kill(pid, SIGKILL);
            waitpid(pid, &status, 0);
            return -1;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
#endif
}

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

} // namespace

// ---------------------------------------------------------------------------
// dnglab engine
// ---------------------------------------------------------------------------
class DnglabEngine : public ConverterEngine {
public:
    explicit DnglabEngine(std::string bin) : bin_(std::move(bin)) {}
    std::string Name() const override { return "dnglab"; }
    bool Available() const override {
        std::string cmd = bin_ + " convert --help 2>nul";
        return run_timed(cmd, 5000) >= 0;
    }
    bool Convert(const std::string& src, const std::string& dst,
                 const ConversionSettings& s) override {
        std::string comp = s.compression;
        if (comp != "lossless" && comp != "uncompressed") comp = "lossless";
        std::string cmd = bin_ + " convert --input " + shell_quote(src) +
            " --output " + shell_quote(dst) + " -c " + comp +
            " --keep-mtime true -f";
        int r = run_timed(cmd, 300000);
        return r == 0;
    }
private:
    std::string bin_;
};

// ---------------------------------------------------------------------------
// PreviewEmbedder: dnglab reembed -> exiftool fallback
// ---------------------------------------------------------------------------
class DnglabEmbedder : public PreviewEmbedder {
public:
    explicit DnglabEmbedder(std::string bin, std::string exif)
        : bin_(std::move(bin)), exif_(std::move(exif)) {}
    std::string Name() const override { return "dnglab"; }
    bool Available() const override {
        std::string cmd = bin_ + " reembed --help 2>nul";
        return run_timed(cmd, 5000) >= 0;
    }
    bool Embed(const std::string& dng_path, const std::string& jpeg_path,
               const std::string& seed) override {
        std::string cmd = bin_ + " reembed --dng " + shell_quote(dng_path) +
            " --preview " + shell_quote(jpeg_path) +
            " --output " + shell_quote(dng_path) + " --seed " + shell_quote(seed);
        return run_timed(cmd, 120000) == 0;
    }
    bool SetOrientation(const std::string& dng_path, int orientation) override {
        std::string cmd = exif_ + " -Orientation=" + std::to_string(orientation) +
            " -n -overwrite_original_in_place " + shell_quote(dng_path);
        return run_timed(cmd, 30000) == 0;
    }
private:
    std::string bin_;
    std::string exif_;
};

// ---------------------------------------------------------------------------
// Factories
// ---------------------------------------------------------------------------
ConverterEngine* MakeConverter(const std::string& engine_name) {
    std::string e = engine_name.empty() ? "dnglab" : engine_name;
    if (e == "dnglab") return new DnglabEngine("dnglab");
    // stubs for adobedng / libraw
    return new DnglabEngine("dnglab");
}

PreviewEmbedder* MakeEmbedder() {
    // dnglab first; exiftool is the fallback inside the embedder chain.
    return new DnglabEmbedder("dnglab", "exiftool");
}

} // namespace rawimport