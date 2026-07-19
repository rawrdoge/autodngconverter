#pragma once
// util.h — helpers for the RawImport C++ rewrite.
// Phase 0 bootstrap (LEAD). See PRD_RawImport_Pipeline_CppRewrite.md §6.3.
#include <cstdint>
#include <string>
#include <vector>

namespace rawimport {

// Split a SQL dump into individual statements on ';' boundaries.
// Mirrors Go splitSQL (util.go). Does NOT support DELIMITER/stored procs.
std::vector<std::string> split_sql(const std::string& sql);

// Minimal .env loader (key=value, # comments, skips blanks).
// Mirrors Go loadDotEnv (util.go). Existing env vars take precedence.
void load_dotenv(const std::string& path);

// Trim whitespace.
std::string trim(const std::string& s);

// Get env var with default.
std::string env_or(const std::string& key, const std::string& def);

// Parse bool from env ("1","true","yes" -> true). Default if unset/invalid.
bool env_bool(const std::string& key, bool def);

// Move a file (best-effort, logs on failure). Returns true on success.
bool move_file(const std::string& from, const std::string& to);

// Ensure a directory exists (mkdir -p). Returns true on success/exist.
bool ensure_dir(const std::string& path);

// Touch a file's mtime (used for digiKam watch rescan). Returns true on success.
bool touch_mtime(const std::string& path);

// Generate a random short token (for processing_locks worker_id).
std::string random_token();

} // namespace rawimport