// The macOS side of util.h: the parts util.cpp keeps behind _WIN32. POSIX files and
// processes, /usr/bin/tar (bsdtar, as tar.exe is on Windows), CommonCrypto for SHA-256,
// libcurl (the one in macOS) for HTTPS, NSOpenPanel / NSSavePanel, libproc, statfs.
#include "util.h"
#include "platform.h"

#import <AppKit/AppKit.h>
#include <CommonCrypto/CommonDigest.h>
#include <curl/curl.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <libproc.h>
#include <mach-o/dyld.h>
#include <signal.h>
#include <spawn.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <sys/xattr.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <ctime>
#include <thread>

extern char** environ;

// ---- text -------------------------------------------------------------------------------------
// wchar_t is 32 bits here: UTF-8 <-> UTF-32. A byte that is no UTF-8 becomes U+FFFD.
std::wstring widen(const std::string& s)
{
    std::wstring w;
    w.reserve(s.size());
    const unsigned char* p = (const unsigned char*)s.data();
    const size_t n = s.size();
    for (size_t i = 0; i < n;) {
        const unsigned c = p[i];
        unsigned cp = 0xFFFD; size_t len = 1;
        if (c < 0x80) cp = c;
        else if ((c & 0xE0) == 0xC0) len = 2, cp = c & 0x1F;
        else if ((c & 0xF0) == 0xE0) len = 3, cp = c & 0x0F;
        else if ((c & 0xF8) == 0xF0) len = 4, cp = c & 0x07;
        else { w += (wchar_t)0xFFFD; i++; continue; }
        if (len > 1) {
            if (i + len > n) { w += (wchar_t)0xFFFD; i++; continue; }
            bool ok = true;
            for (size_t k = 1; k < len; k++) {
                if ((p[i + k] & 0xC0) != 0x80) { ok = false; break; }
                cp = cp << 6 | (p[i + k] & 0x3F);
            }
            if (!ok) { w += (wchar_t)0xFFFD; i++; continue; }
        }
        w += (wchar_t)cp;
        i += len;
    }
    return w;
}

std::string narrow(const std::wstring& w)
{
    std::string s;
    s.reserve(w.size());
    for (wchar_t wc : w) {
        unsigned cp = (unsigned)wc;
        if (cp > 0x10FFFF || (cp >= 0xD800 && cp < 0xE000)) cp = 0xFFFD;
        if (cp < 0x80) s += (char)cp;
        else if (cp < 0x800) { s += (char)(0xC0 | cp >> 6); s += (char)(0x80 | (cp & 63)); }
        else if (cp < 0x10000) { s += (char)(0xE0 | cp >> 12); s += (char)(0x80 | ((cp >> 6) & 63)); s += (char)(0x80 | (cp & 63)); }
        else { s += (char)(0xF0 | cp >> 18); s += (char)(0x80 | ((cp >> 12) & 63)); s += (char)(0x80 | ((cp >> 6) & 63)); s += (char)(0x80 | (cp & 63)); }
    }
    return s;
}

std::string ui_text(const std::string& in)
{
    std::string s = in;
    auto swap = [&](const char* a, const char* b) {
        for (size_t at = 0; (at = s.find(a, at)) != std::string::npos; at += strlen(b)) s.replace(at, strlen(a), b);
    };
    swap("RaCACiT Launcher.exe", "RaCACiT Launcher.app");
    swap("RaCACiT.exe", "RaCACiT");
    swap("tar.exe", "tar");
    swap("this PC", "this Mac");
    swap("Explorer", "Finder");
    for (char& c : s) if (c == '\\') c = '/';
    return s;
}

// ---- files ------------------------------------------------------------------------------------
static bool write_all(int fd, const void* d, size_t n)
{
    const char* p = (const char*)d;
    while (n) {
        const ssize_t w = write(fd, p, n);
        if (w < 0 && errno == EINTR) continue;
        if (w <= 0) return false;
        p += w; n -= (size_t)w;
    }
    return true;
}

bool write_text_atomic(const fs::path& p, const std::string& text)
{
    std::error_code ec;
    fs::create_directories(p.parent_path(), ec);
    fs::path tmp = p; tmp += L".tmp";
    const int fd = open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (fd < 0) return false;
    // Written through, as MOVEFILE_WRITE_THROUGH: on disk before it replaces the old one.
    const bool ok = write_all(fd, text.data(), text.size()) && fsync(fd) == 0;
    close(fd);
    if (!ok) { unlink(tmp.c_str()); return false; }
    return rename(tmp.c_str(), p.c_str()) == 0;
}

std::string stamp_now()
{
    time_t t = time(nullptr); struct tm lt; localtime_r(&t, &lt);
    char b[32]; strftime(b, sizeof b, "%Y%m%d-%H%M%S", &lt);
    return b;
}

std::string file_date(const fs::path& p)
{
    struct stat st;
    if (stat(p.c_str(), &st) != 0) return "?";
    struct tm lt; time_t t = st.st_mtimespec.tv_sec; localtime_r(&t, &lt);
    char b[48]; snprintf(b, sizeof b, "%04d-%02d-%02d %02d:%02d", lt.tm_year + 1900, lt.tm_mon + 1, lt.tm_mday, lt.tm_hour, lt.tm_min);
    return b;
}

std::string copy_tree(const fs::path& src, const fs::path& dst, Progress* p)
{
    std::error_code ec;
    fs::create_directories(dst, ec);
    if (ec) return "cannot create " + u8(dst) + ": " + ec.message();
    std::vector<char> buf(4u << 20);
    for (auto it = fs::recursive_directory_iterator(src, fs::directory_options::skip_permission_denied, ec);
         !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
        if (p && p->cancel) return "cancelled";
        const fs::path rel = fs::relative(it->path(), src, ec);
        const fs::path out = dst / rel;
        if (it->is_directory(ec)) { fs::create_directories(out, ec); continue; }
        if (!it->is_regular_file(ec)) continue;
        if (p) p->set_line(u8(rel));
        const int in = open(it->path().c_str(), O_RDONLY | O_CLOEXEC);
        if (in < 0) return "cannot read " + u8(it->path());
        struct stat st; fstat(in, &st);
        fs::create_directories(out.parent_path(), ec);
        const int o = open(out.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, st.st_mode & 0777);
        if (o < 0) { close(in); return "cannot write " + u8(out); }
        bool ok = true;
        for (;;) {
            const ssize_t got = read(in, buf.data(), buf.size());
            if (got < 0 && errno == EINTR) continue;
            if (got < 0) { ok = false; break; }
            if (got == 0) break;
            if (!write_all(o, buf.data(), (size_t)got)) { ok = false; break; }
            if (p) p->done += (uint64_t)got;
            if (p && p->cancel) { ok = false; break; }
        }
        // The file's times, as SetFileTime on Windows.
        const struct timespec ts[2] = { st.st_atimespec, st.st_mtimespec };
        futimens(o, ts);
        fchmod(o, st.st_mode & 0777);
        close(in); close(o);
        if (!ok) { unlink(out.c_str()); return p && p->cancel ? "cancelled" : "write failed (disk full?) at " + u8(out); }
    }
    if (ec) return "reading " + u8(src) + ": " + ec.message();
    return {};
}

void clear_quarantine(const fs::path& dir)
{
    std::error_code ec;
    removexattr(dir.c_str(), "com.apple.quarantine", XATTR_NOFOLLOW);
    for (auto it = fs::recursive_directory_iterator(dir, fs::directory_options::skip_permission_denied, ec);
         !ec && it != fs::recursive_directory_iterator(); it.increment(ec))
        removexattr(it->path().c_str(), "com.apple.quarantine", XATTR_NOFOLLOW);
}

// ---- processes --------------------------------------------------------------------------------
struct ChildProcess {
    pid_t pid = -1;
    std::mutex m;
    bool ended = false;          // reaped: the pid may belong to someone else now
    bool terminated = false;
    int terminate_code = 1;
    int code = 0;
};

ChildProcess* child_start(const std::vector<std::string>& argv, const std::vector<std::string>* env,
                          const fs::path& cwd, const fs::path& out, std::string* err)
{
    if (argv.empty()) { if (err) *err = "nothing to run"; return nullptr; }
    posix_spawn_file_actions_t fa; posix_spawn_file_actions_init(&fa);
    posix_spawnattr_t at; posix_spawnattr_init(&at);
    // Nothing of the launcher's leaks into the child but stdin/out/err; its own process
    // group, so the launcher's terminal (when run from one) does not reach it.
    posix_spawnattr_setflags(&at, POSIX_SPAWN_CLOEXEC_DEFAULT | POSIX_SPAWN_SETPGROUP | POSIX_SPAWN_SETSIGDEF | POSIX_SPAWN_SETSIGMASK);
    posix_spawnattr_setpgroup(&at, 0);
    sigset_t none, all; sigemptyset(&none); sigfillset(&all);
    posix_spawnattr_setsigmask(&at, &none);
    posix_spawnattr_setsigdefault(&at, &all);
    posix_spawn_file_actions_addopen(&fa, 0, "/dev/null", O_RDONLY, 0);
    const std::string o = out.empty() ? std::string("/dev/null") : u8(out);
    if (!out.empty()) { std::error_code ec; fs::create_directories(out.parent_path(), ec); }
    posix_spawn_file_actions_addopen(&fa, 1, o.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    posix_spawn_file_actions_adddup2(&fa, 1, 2);
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"   // addchdir (no _np) is macOS 26+; this one is 10.15+
    if (!cwd.empty()) posix_spawn_file_actions_addchdir_np(&fa, cwd.c_str());
#pragma clang diagnostic pop
    std::vector<char*> av, ev;
    for (auto& a : argv) av.push_back((char*)a.c_str());
    av.push_back(nullptr);
    if (env) { for (auto& e : *env) ev.push_back((char*)e.c_str()); ev.push_back(nullptr); }
    pid_t pid = -1;
    const std::string prog = u8(fs::absolute(argv[0]));   // the child's cwd is not ours
    const int r = posix_spawn(&pid, prog.c_str(), &fa, &at, av.data(), env ? ev.data() : environ);
    posix_spawn_file_actions_destroy(&fa);
    posix_spawnattr_destroy(&at);
    if (r != 0) { if (err) *err = strerror(r); return nullptr; }
    auto* c = new ChildProcess();
    c->pid = pid;
    return c;
}

// Waits without reaping (WNOWAIT), so child_terminate can never signal a reused pid; then reaps.
bool child_wait(ChildProcess* c, int timeout_ms)
{
    {
        std::lock_guard<std::mutex> l(c->m);
        if (c->ended) return true;
    }
    const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms < 0 ? 0 : timeout_ms);
    for (;;) {
        siginfo_t si{};
        const int r = waitid(P_PID, (id_t)c->pid, &si, WEXITED | WNOWAIT | (timeout_ms < 0 ? 0 : WNOHANG));
        if (r == 0 && si.si_pid == c->pid) break;
        if (r != 0 && errno == EINTR) continue;
        if (r != 0 && errno == ECHILD) break;   // already gone (reaped elsewhere)
        if (timeout_ms >= 0 && std::chrono::steady_clock::now() >= until) return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    std::lock_guard<std::mutex> l(c->m);
    if (!c->ended) {
        int st = 0;
        pid_t w;
        do w = waitpid(c->pid, &st, 0); while (w < 0 && errno == EINTR);
        c->code = w < 0 ? 1 : WIFEXITED(st) ? WEXITSTATUS(st) : WIFSIGNALED(st) ? 128 + WTERMSIG(st) : 1;
        if (c->terminated) c->code = c->terminate_code;
        c->ended = true;
    }
    return true;
}

int child_exit_code(ChildProcess* c) { std::lock_guard<std::mutex> l(c->m); return c->ended ? c->code : 259; /* STILL_ACTIVE */ }

void child_terminate(ChildProcess* c, int code)
{
    std::lock_guard<std::mutex> l(c->m);
    if (c->ended) return;
    c->terminated = true; c->terminate_code = code;
    kill(c->pid, SIGKILL);   // TerminateProcess: at once, no clean-up in the child
}

void child_free(ChildProcess* c)
{
    if (!c) return;
    if (!child_wait(c, 0)) {   // still running: let it go, reaped in the background
        const pid_t pid = c->pid;
        std::thread([pid] { int st; while (waitpid(pid, &st, 0) < 0 && errno == EINTR) {} }).detach();
    }
    delete c;
}

int run_argv(const std::vector<std::string>& argv, const fs::path& cwd, std::atomic<bool>* cancel)
{
    ChildProcess* c = child_start(argv, nullptr, cwd, {});
    if (!c) return -1;
    for (;;) {
        if (child_wait(c, 100)) break;
        if (cancel && *cancel) { child_terminate(c, 1); child_wait(c, 5000); break; }
    }
    const int code = child_exit_code(c);
    child_free(c);
    return code;
}

// A Windows command line ("C:\a b\x.exe" --flag), split as CommandLineToArgvW does for the
// plain cases the launcher writes: quotes group, backslashes are literal.
int run_hidden(const std::wstring& cmdline, const fs::path& cwd, std::atomic<bool>* cancel)
{
    std::vector<std::string> argv;
    std::wstring cur; bool quoted = false, have = false;
    for (wchar_t c : cmdline) {
        if (c == L'"') { quoted = !quoted; have = true; continue; }
        if (!quoted && (c == L' ' || c == L'\t')) { if (have) argv.push_back(narrow(cur)); cur.clear(); have = false; continue; }
        cur += c; have = true;
    }
    if (have) argv.push_back(narrow(cur));
    return run_argv(argv, cwd, cancel);
}

// bsdtar, the same program as Windows' tar.exe. --no-mac-metadata: no ._ AppleDouble files.
std::string zip_create(const fs::path& from_dir, const fs::path& zip)
{
    std::error_code ec;
    fs::create_directories(zip.parent_path(), ec);
    fs::remove(zip, ec);
    const int r = run_argv({ "/usr/bin/tar", "--no-mac-metadata", "-a", "-c", "-f", u8(fs::absolute(zip)), "." }, from_dir);
    if (r == -1) return "/usr/bin/tar was not found";
    if (r != 0 || !fs::exists(zip)) return "making the zip failed (tar exit " + std::to_string(r) + ")";
    return {};
}

std::string zip_extract(const fs::path& zip, const fs::path& to_dir)
{
    std::error_code ec;
    fs::create_directories(to_dir, ec);
    const int r = run_argv({ "/usr/bin/tar", "-x", "-f", u8(fs::absolute(zip)) }, to_dir);
    if (r == -1) return "/usr/bin/tar was not found";
    if (r != 0) return "unpacking the zip failed (tar exit " + std::to_string(r) + ")";
    return {};
}

// ---- SHA-256 ----------------------------------------------------------------------------------
std::string sha256_file(const fs::path& p, Progress* prog)
{
    const int f = open(p.c_str(), O_RDONLY | O_CLOEXEC);
    if (f < 0) return {};
    CC_SHA256_CTX h; CC_SHA256_Init(&h);
    std::vector<unsigned char> buf(1u << 20);
    bool ok = true;
    for (;;) {
        const ssize_t got = read(f, buf.data(), buf.size());
        if (got < 0 && errno == EINTR) continue;
        if (got < 0) { ok = false; break; }
        if (got == 0) break;
        CC_SHA256_Update(&h, buf.data(), (CC_LONG)got);
        if (prog) { prog->done += (uint64_t)got; if (prog->cancel) { ok = false; break; } }
    }
    close(f);
    unsigned char d[CC_SHA256_DIGEST_LENGTH];
    CC_SHA256_Final(d, &h);
    if (!ok) return {};
    std::string out;
    static const char* hx = "0123456789abcdef";
    for (unsigned char c : d) { out += hx[c >> 4]; out += hx[c & 15]; }
    return out;
}

// ---- HTTP -------------------------------------------------------------------------------------
// libcurl, set up as the WinHTTP session in util.cpp: user agent RACiT-Launcher, 10 s to
// resolve and connect, a stalled transfer given up after 30 s, redirects followed but never
// from https to http, only a 200 read, Content-Length as the progress total.
namespace {
struct Get {
    std::string* body; const fs::path* to_file; Progress* p;
    CURL* c = nullptr;
    int fd = -1;
    bool started = false, bad_status = false, write_failed = false, open_failed = false, got_data = false;
    long status = 0;
    bool start()
    {
        started = true;
        curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &status);
        if (status != 200) { bad_status = true; return false; }
        if (p) {
            curl_off_t len = -1;
            if (curl_easy_getinfo(c, CURLINFO_CONTENT_LENGTH_DOWNLOAD_T, &len) == CURLE_OK && len >= 0) p->total = (uint64_t)len;
        }
        if (to_file) {
            std::error_code ec; fs::create_directories(to_file->parent_path(), ec);
            fd = open(to_file->c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
            if (fd < 0) { open_failed = true; return false; }
        }
        return true;
    }
};
size_t on_data(char* d, size_t sz, size_t n, void* u)
{
    Get* g = (Get*)u;
    const size_t got = sz * n;
    if (!g->started && !g->start()) return 0;
    g->got_data = true;
    if (g->to_file) { if (!write_all(g->fd, d, got)) { g->write_failed = true; return 0; } }
    else if (g->body) g->body->append(d, got);
    if (g->p) g->p->done += got;
    return got;
}
int on_progress(void* u, curl_off_t, curl_off_t, curl_off_t, curl_off_t)
{
    Get* g = (Get*)u;
    return g->p && g->p->cancel ? 1 : 0;
}
}  // namespace

std::string http_get(const std::string& url, std::string* body, const fs::path* to_file, Progress* p, int* status_out, const wchar_t* headers)
{
    if (status_out) *status_out = 0;
    const std::string low = lower(url);
    const bool https = low.rfind("https://", 0) == 0;
    if (!https && low.rfind("http://", 0) != 0) return low.find("://") == std::string::npos ? "not a valid address: " + url : "only http(s) addresses";
    static const bool once = curl_global_init(CURL_GLOBAL_DEFAULT) == CURLE_OK;
    if (!once) return "network unavailable";
    CURL* c = curl_easy_init();
    if (!c) return "network unavailable";
    Get g{ body, to_file, p };
    g.c = c;
    curl_slist* hl = nullptr;
    if (headers) {   // "Name: value\r\nName: value\r\n"
        const std::string h = narrow(headers);
        size_t i = 0;
        while (i < h.size()) {
            size_t j = h.find("\r\n", i);
            if (j == std::string::npos) j = h.size();
            const std::string line = trim(h.substr(i, j - i));
            if (!line.empty()) hl = curl_slist_append(hl, line.c_str());
            i = j + 2;
        }
    }
    curl_easy_setopt(c, CURLOPT_URL, url.c_str());
    curl_easy_setopt(c, CURLOPT_USERAGENT, "RACiT-Launcher");
    if (hl) curl_easy_setopt(c, CURLOPT_HTTPHEADER, hl);
    curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(c, CURLOPT_MAXREDIRS, 10L);
    curl_easy_setopt(c, CURLOPT_PROTOCOLS_STR, "http,https");
    curl_easy_setopt(c, CURLOPT_REDIR_PROTOCOLS_STR, https ? "https" : "http,https");
    curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT_MS, 10000L);
    curl_easy_setopt(c, CURLOPT_LOW_SPEED_LIMIT, 1L);
    curl_easy_setopt(c, CURLOPT_LOW_SPEED_TIME, 30L);
    curl_easy_setopt(c, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, on_data);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, &g);
    curl_easy_setopt(c, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(c, CURLOPT_XFERINFOFUNCTION, on_progress);
    curl_easy_setopt(c, CURLOPT_XFERINFODATA, &g);
    const CURLcode r = curl_easy_perform(c);
    long status = g.status;
    if (!g.started) curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &status);
    std::string err;
    if (r == CURLE_ABORTED_BY_CALLBACK) err = "cancelled";
    else if (g.bad_status) err = "the server answered " + std::to_string(status);
    else if (g.open_failed) err = "cannot write " + u8(*to_file);
    else if (g.write_failed) err = "write failed";
    else if (r == CURLE_URL_MALFORMAT) err = "not a valid address: " + url;
    else if (r != CURLE_OK && status == 0) err = "no response from the server";
    else if (r != CURLE_OK && status != 200) err = "the server answered " + std::to_string(status);   // a redirect not followed (https -> http)
    else if (r != CURLE_OK) err = "the download broke off";
    else if (status != 200) err = "the server answered " + std::to_string(status);
    else if (!g.started && !g.start()) err = g.open_failed ? "cannot write " + u8(*to_file) : "the server answered " + std::to_string(status);   // an empty 200
    if (status_out) *status_out = (int)status;
    if (g.fd >= 0) { close(g.fd); if (!err.empty()) unlink(to_file->c_str()); }
    if (hl) curl_slist_free_all(hl);
    curl_easy_cleanup(c);
    return err;
}

// ---- pickers ----------------------------------------------------------------------------------
static NSString* ns(const std::wstring& w) { return [NSString stringWithUTF8String:narrow(w).c_str()] ?: @""; }
static NSString* ns_title(const wchar_t* t) { return t ? [NSString stringWithUTF8String:ui_text(narrow(t)).c_str()] ?: @"" : @""; }
static fs::path from_url(NSURL* u) { return u && u.fileSystemRepresentation ? fs::path(u.fileSystemRepresentation) : fs::path(); }

// "*.iso", "*.elf;*.ELF", "*.zip" -> the extensions, lower case; empty = anything.
static std::vector<std::string> extensions(const wchar_t* spec)
{
    std::vector<std::string> out;
    if (!spec) return out;
    const std::string s = narrow(spec);
    size_t i = 0;
    while (i <= s.size()) {
        size_t j = s.find(';', i); if (j == std::string::npos) j = s.size();
        std::string e = lower(trim(s.substr(i, j - i)));
        if (e.rfind("*.", 0) == 0) e = e.substr(2);
        if (!e.empty() && e != "*" && std::find(out.begin(), out.end(), e) == out.end()) out.push_back(e);
        i = j + 1;
    }
    return out;
}

@interface RacitPickDelegate : NSObject <NSOpenSavePanelDelegate>
@property (nonatomic) std::vector<std::string> exts;
@end
@implementation RacitPickDelegate
- (BOOL)panel:(id)sender shouldEnableURL:(NSURL*)url
{
    (void)sender;
    NSNumber* dir = nil;
    [url getResourceValue:&dir forKey:NSURLIsDirectoryKey error:nil];
    if (dir.boolValue || self.exts.empty()) return YES;
    const std::string e = lower(url.pathExtension.UTF8String ?: "");
    return std::find(self.exts.begin(), self.exts.end(), e) != self.exts.end();
}
@end

namespace {
fs::path run_panel(NSSavePanel* panel)
{
    NSWindow* key = NSApp.keyWindow;
    const NSModalResponse r = [panel runModal];
    [key makeKeyAndOrderFront:nil];
    if (r != NSModalResponseOK) return {};
    if ([panel isKindOfClass:[NSOpenPanel class]]) return from_url(((NSOpenPanel*)panel).URLs.firstObject);
    return from_url(panel.URL);
}
}  // namespace

fs::path mac_pick_folder(const wchar_t* title, const fs::path& start)
{
    @autoreleasepool {
        NSOpenPanel* p = [NSOpenPanel openPanel];
        p.canChooseDirectories = YES; p.canChooseFiles = NO; p.allowsMultipleSelection = NO;
        p.message = ns_title(title); p.title = ns_title(title);
        if (!start.empty()) p.directoryURL = [NSURL fileURLWithPath:ns(start.wstring()) isDirectory:YES];
        return run_panel(p);
    }
}

fs::path mac_pick_open_file(const wchar_t* title, const wchar_t* filter_spec)
{
    @autoreleasepool {
        NSOpenPanel* p = [NSOpenPanel openPanel];
        p.canChooseDirectories = NO; p.canChooseFiles = YES; p.allowsMultipleSelection = NO;
        p.message = ns_title(title); p.title = ns_title(title);
        RacitPickDelegate* d = [RacitPickDelegate new];
        d.exts = extensions(filter_spec);   // EBOOT.elf and EBOOT.ELF alike: matched without case
        p.delegate = d;
        fs::path r = run_panel(p);
        p.delegate = nil;
        return r;
    }
}

fs::path mac_pick_save_file(const wchar_t* title, const wchar_t* default_name, const wchar_t* filter_spec)
{
    @autoreleasepool {
        NSSavePanel* p = [NSSavePanel savePanel];
        p.message = ns_title(title); p.title = ns_title(title);
        if (default_name) p.nameFieldStringValue = ns(default_name);
        p.canCreateDirectories = YES;
        fs::path r = run_panel(p);
        const auto ex = extensions(filter_spec);
        if (!r.empty() && !ex.empty() && lower(u8(r.extension())) != "." + ex[0]) r += widen("." + ex[0]);
        return r;
    }
}

// ---- shell ------------------------------------------------------------------------------------
void shell_open(const fs::path& p)
{
    // open(1): a folder in Finder, a file in its app, an https:// address in the browser.
    ChildProcess* c = child_start({ "/usr/bin/open", u8(p) }, nullptr, {}, {});
    if (c) std::thread([c] { child_wait(c, -1); child_free(c); }).detach();
}

fs::path exe_path()
{
    uint32_t n = 0;
    _NSGetExecutablePath(nullptr, &n);
    std::string b(n, '\0');
    if (_NSGetExecutablePath(b.data(), &n) != 0) return {};
    b.resize(strlen(b.c_str()));
    char real[PATH_MAX];
    return fs::path(realpath(b.c_str(), real) ? real : b.c_str());
}

// Gatekeeper runs a downloaded app that was not moved from a read-only copy elsewhere
// ("App Translocation"); the player's folder is where the original is.
static fs::path untranslocated(const fs::path& app)
{
    if (u8(app).find("/AppTranslocation/") == std::string::npos) return app;
    typedef Boolean (*is_fn)(CFURLRef, bool*, CFErrorRef*);
    typedef CFURLRef (*orig_fn)(CFURLRef, CFErrorRef*);
    void* sec = dlopen("/System/Library/Frameworks/Security.framework/Security", RTLD_LAZY);
    auto is_t = sec ? (is_fn)dlsym(sec, "SecTranslocateIsTranslocatedURL") : nullptr;
    auto orig = sec ? (orig_fn)dlsym(sec, "SecTranslocateCreateOriginalPathForURL") : nullptr;
    if (!is_t || !orig) return app;
    fs::path r = app;
    const std::string s = u8(app);
    CFURLRef u = CFURLCreateFromFileSystemRepresentation(nullptr, (const UInt8*)s.data(), (CFIndex)s.size(), true);
    bool t = false;
    if (u && is_t(u, &t, nullptr) && t) {
        if (CFURLRef o = orig(u, nullptr)) {
            char b[PATH_MAX];
            if (CFURLGetFileSystemRepresentation(o, true, (UInt8*)b, sizeof b)) r = b;
            CFRelease(o);
        }
    }
    if (u) CFRelease(u);
    return r;
}

fs::path app_bundle_path()
{
    // .../RaCACiT Launcher.app/Contents/MacOS/RaCACiT Launcher
    const fs::path exe = exe_path();
    const fs::path app = exe.parent_path().parent_path().parent_path();
    if (lower(u8(app.extension())) != ".app" || exe.parent_path().filename() != "MacOS") return exe;   // a bare binary (a dev build)
    return untranslocated(app);
}

fs::path launcher_root() { return app_bundle_path().parent_path(); }

bool process_running_under(const fs::path& dir)
{
    std::error_code ec;
    const fs::path real = fs::weakly_canonical(dir, ec);   // proc_pidpath names the real path (/private/tmp, no ..)
    std::string base = lower(u8(ec ? dir : real));
    if (base.empty() || base.back() != '/') base += '/';
    std::vector<pid_t> pids(4096);
    const int bytes = proc_listpids(PROC_ALL_PIDS, 0, pids.data(), (int)(pids.size() * sizeof(pid_t)));
    if (bytes <= 0) return false;
    pids.resize((size_t)bytes / sizeof(pid_t));
    char b[PROC_PIDPATHINFO_MAXSIZE];
    for (pid_t pid : pids) {
        if (pid <= 0) continue;
        if (proc_pidpath(pid, b, sizeof b) > 0 && lower(b).rfind(base, 0) == 0) return true;
    }
    return false;
}

uint64_t free_space(const fs::path& dir)
{
    struct statfs s;
    return statfs(dir.c_str(), &s) == 0 ? (uint64_t)s.f_bavail * (uint64_t)s.f_bsize : 0;
}
