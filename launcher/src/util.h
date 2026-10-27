// Small Windows helpers for the launcher: text, files, processes, zip (via the
// system's tar.exe), SHA-256, HTTPS downloads, a tiny JSON reader, pickers.
#pragma once
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace fs = std::filesystem;

std::wstring widen(const std::string& s);
std::string narrow(const std::wstring& s);
inline std::string u8(const fs::path& p) { return narrow(p.wstring()); }
inline fs::path upath(const std::string& s) { return fs::path(widen(s)); }

std::string read_text(const fs::path& p, size_t max = 16u << 20);
bool write_text_atomic(const fs::path& p, const std::string& text);   // beside, then renamed over
std::string trim(const std::string& s);
std::string lower(std::string s);
std::string format_size(uint64_t bytes);
std::string format_duration(uint64_t seconds);
std::string stamp_now();                         // 20261024-071530
std::string file_date(const fs::path& p);        // 2026-10-24 07:15

// Progress of a long job, read by the UI thread.
struct Progress {
    std::atomic<uint64_t> done{0}, total{0};
    std::atomic<bool> cancel{false};
    void set_line(const std::string& s) { std::lock_guard<std::mutex> l(m_); line_ = s; }
    std::string line() { std::lock_guard<std::mutex> l(m_); return line_; }
private:
    std::mutex m_;
    std::string line_;                           // the file or step being worked on
};

uint64_t tree_size(const fs::path& dir, uint64_t* files = nullptr);
// Copies dir's contents into dst (created), file by file, adding to p->done.
// Returns "" on success, else why it stopped.
std::string copy_tree(const fs::path& src, const fs::path& dst, Progress* p);
bool delete_tree(const fs::path& p);

// Hidden child process; waits. Returns the exit code, -1 if it did not start.
int run_hidden(const std::wstring& cmdline, const fs::path& cwd, std::atomic<bool>* cancel = nullptr);
std::string zip_create(const fs::path& from_dir, const fs::path& zip);   // "" = ok
std::string zip_extract(const fs::path& zip, const fs::path& to_dir);

std::string sha256_file(const fs::path& p, Progress* prog = nullptr);    // lowercase hex, "" on error

// HTTPS (or HTTP) GET. to_file: streamed with progress. Returns "" on success.
std::string http_get(const std::string& url, std::string* body, const fs::path* to_file, Progress* p);

// JSON (objects, arrays, strings, numbers, true/false/null), enough for mod.json and update manifests.
struct Json {
    enum Type { Null, Bool, Num, Str, Arr, Obj } type = Null;
    bool b = false; double n = 0; std::string s;
    std::vector<Json> a; std::map<std::string, Json> o;
    const Json& operator[](const std::string& k) const;
    std::string str(const std::string& k, const std::string& def = "") const;
    double num(const std::string& k, double def = 0) const;
};
bool json_parse(const std::string& text, Json* out, std::string* err = nullptr);

// Shell pickers (null-safe parent). Empty path = cancelled. A path queued with
// queue_pick() (the launcher's test script) is returned instead of asking.
void queue_pick(const fs::path& p);
fs::path pick_folder(void* hwnd, const wchar_t* title, const fs::path& start = {});
fs::path pick_open_file(void* hwnd, const wchar_t* title, const wchar_t* filter_name, const wchar_t* filter_spec);
fs::path pick_save_file(void* hwnd, const wchar_t* title, const wchar_t* default_name,
                        const wchar_t* filter_name, const wchar_t* filter_spec);
void shell_open(const fs::path& p);              // a folder in Explorer, a file in its app
fs::path exe_dir();
