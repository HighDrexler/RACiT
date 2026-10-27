#include "util.h"

#include <windows.h>
#include <bcrypt.h>
#include <shellapi.h>
#include <shobjidl.h>
#include <winhttp.h>

#include <cstdio>
#include <ctime>
#include <fstream>
#include <sstream>

std::wstring widen(const std::string& s)
{
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), w.data(), n);
    return w;
}

std::string narrow(const std::wstring& w)
{
    if (w.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), s.data(), n, nullptr, nullptr);
    return s;
}

std::string read_text(const fs::path& p, size_t max)
{
    std::ifstream f(p, std::ios::binary);
    if (!f) return {};
    std::string s;
    char buf[65536];
    while (f && s.size() < max) {
        f.read(buf, sizeof buf);
        s.append(buf, (size_t)f.gcount());
    }
    if (s.size() >= 3 && (unsigned char)s[0] == 0xEF && (unsigned char)s[1] == 0xBB && (unsigned char)s[2] == 0xBF) s.erase(0, 3);
    return s;
}

bool write_text_atomic(const fs::path& p, const std::string& text)
{
    std::error_code ec;
    fs::create_directories(p.parent_path(), ec);
    fs::path tmp = p; tmp += L".tmp";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f) return false;
        f.write(text.data(), (std::streamsize)text.size());
        if (!f) return false;
    }
    return MoveFileExW(tmp.c_str(), p.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
}

std::string trim(const std::string& s)
{
    size_t a = 0, b = s.size();
    while (a < b && (unsigned char)s[a] <= ' ') a++;
    while (b > a && (unsigned char)s[b - 1] <= ' ') b--;
    return s.substr(a, b - a);
}

std::string lower(std::string s) { for (char& c : s) if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a'); return s; }

std::string format_size(uint64_t b)
{
    char t[64];
    if (b >= (1ull << 30)) snprintf(t, sizeof t, "%.1f GB", b / 1073741824.0);
    else if (b >= (1ull << 20)) snprintf(t, sizeof t, "%.1f MB", b / 1048576.0);
    else if (b >= 1024) snprintf(t, sizeof t, "%.0f KB", b / 1024.0);
    else snprintf(t, sizeof t, "%llu bytes", (unsigned long long)b);
    return t;
}

std::string format_duration(uint64_t s)
{
    char t[64];
    if (s >= 3600) snprintf(t, sizeof t, "%llu h %02llu min", (unsigned long long)(s / 3600), (unsigned long long)(s / 60 % 60));
    else if (s >= 60) snprintf(t, sizeof t, "%llu min", (unsigned long long)(s / 60));
    else snprintf(t, sizeof t, "%llu s", (unsigned long long)s);
    return t;
}

std::string stamp_now()
{
    time_t t = time(nullptr); struct tm lt; localtime_s(&lt, &t);
    char b[32]; strftime(b, sizeof b, "%Y%m%d-%H%M%S", &lt);
    return b;
}

std::string file_date(const fs::path& p)
{
    WIN32_FILE_ATTRIBUTE_DATA a;
    if (!GetFileAttributesExW(p.c_str(), GetFileExInfoStandard, &a)) return "?";
    FILETIME lf; SYSTEMTIME st;
    FileTimeToLocalFileTime(&a.ftLastWriteTime, &lf); FileTimeToSystemTime(&lf, &st);
    char b[48]; snprintf(b, sizeof b, "%04u-%02u-%02u %02u:%02u", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute);
    return b;
}

uint64_t tree_size(const fs::path& dir, uint64_t* files)
{
    uint64_t total = 0, n = 0;
    std::error_code ec;
    for (auto it = fs::recursive_directory_iterator(dir, fs::directory_options::skip_permission_denied, ec);
         !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
        if (it->is_regular_file(ec)) { total += it->file_size(ec); n++; }
    }
    if (files) *files = n;
    return total;
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
        HANDLE in = CreateFileW(it->path().c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                                FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
        if (in == INVALID_HANDLE_VALUE) return "cannot read " + u8(it->path());
        fs::create_directories(out.parent_path(), ec);
        HANDLE o = CreateFileW(out.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (o == INVALID_HANDLE_VALUE) { CloseHandle(in); return "cannot write " + u8(out); }
        DWORD got = 0; bool ok = true;
        while (ReadFile(in, buf.data(), (DWORD)buf.size(), &got, nullptr) && got) {
            DWORD put = 0;
            if (!WriteFile(o, buf.data(), got, &put, nullptr) || put != got) { ok = false; break; }
            if (p) p->done += got;
            if (p && p->cancel) { ok = false; break; }
        }
        FILETIME ct, at, wt;
        if (GetFileTime(in, &ct, &at, &wt)) SetFileTime(o, &ct, &at, &wt);
        CloseHandle(in); CloseHandle(o);
        if (!ok) { DeleteFileW(out.c_str()); return p && p->cancel ? "cancelled" : "write failed (disk full?) at " + u8(out); }
    }
    if (ec) return "reading " + u8(src) + ": " + ec.message();
    return {};
}

bool delete_tree(const fs::path& p)
{
    std::error_code ec;
    fs::remove_all(p, ec);
    return !ec;
}

int run_hidden(const std::wstring& cmdline, const fs::path& cwd, std::atomic<bool>* cancel)
{
    STARTUPINFOW si{}; si.cb = sizeof si; si.dwFlags = STARTF_USESHOWWINDOW; si.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION pi{};
    std::wstring cl = cmdline;
    if (!CreateProcessW(nullptr, cl.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr,
                        cwd.empty() ? nullptr : cwd.c_str(), &si, &pi)) return -1;
    for (;;) {
        if (WaitForSingleObject(pi.hProcess, 100) == WAIT_OBJECT_0) break;
        if (cancel && *cancel) { TerminateProcess(pi.hProcess, 1); WaitForSingleObject(pi.hProcess, 5000); break; }
    }
    DWORD code = 1; GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hThread); CloseHandle(pi.hProcess);
    return (int)code;
}

static std::wstring tar_exe()
{
    wchar_t sys[MAX_PATH]; GetSystemDirectoryW(sys, MAX_PATH);
    return std::wstring(L"\"") + sys + L"\\tar.exe\"";
}

std::string zip_create(const fs::path& from_dir, const fs::path& zip)
{
    std::error_code ec;
    fs::create_directories(zip.parent_path(), ec);
    fs::remove(zip, ec);
    // bsdtar picks the zip format from the .zip suffix (-a).
    const int r = run_hidden(tar_exe() + L" -a -c -f \"" + zip.wstring() + L"\" .", from_dir);
    if (r == -1) return "tar.exe (Windows 10 or later) was not found";
    if (r != 0 || !fs::exists(zip)) return "making the zip failed (tar exit " + std::to_string(r) + ")";
    return {};
}

std::string zip_extract(const fs::path& zip, const fs::path& to_dir)
{
    std::error_code ec;
    fs::create_directories(to_dir, ec);
    const int r = run_hidden(tar_exe() + L" -x -f \"" + zip.wstring() + L"\"", to_dir);
    if (r == -1) return "tar.exe (Windows 10 or later) was not found";
    if (r != 0) return "unpacking the zip failed (tar exit " + std::to_string(r) + ")";
    return {};
}

std::string sha256_file(const fs::path& p, Progress* prog)
{
    BCRYPT_ALG_HANDLE alg = nullptr; BCRYPT_HASH_HANDLE h = nullptr;
    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) != 0) return {};
    std::string out;
    if (BCryptCreateHash(alg, &h, nullptr, 0, nullptr, 0, 0) == 0) {
        HANDLE f = CreateFileW(p.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
        if (f != INVALID_HANDLE_VALUE) {
            std::vector<unsigned char> buf(1u << 20); DWORD got = 0; bool ok = true;
            while (ReadFile(f, buf.data(), (DWORD)buf.size(), &got, nullptr) && got) {
                if (BCryptHashData(h, buf.data(), got, 0) != 0) { ok = false; break; }
                if (prog) { prog->done += got; if (prog->cancel) { ok = false; break; } }
            }
            CloseHandle(f);
            unsigned char d[32];
            if (ok && BCryptFinishHash(h, d, 32, 0) == 0) {
                static const char* hx = "0123456789abcdef";
                for (unsigned char c : d) { out += hx[c >> 4]; out += hx[c & 15]; }
            }
        }
        BCryptDestroyHash(h);
    }
    BCryptCloseAlgorithmProvider(alg, 0);
    return out;
}

std::string http_get(const std::string& url, std::string* body, const fs::path* to_file, Progress* p)
{
    std::wstring wurl = widen(url);
    URL_COMPONENTS uc{}; uc.dwStructSize = sizeof uc;
    wchar_t host[256], path[2048];
    uc.lpszHostName = host; uc.dwHostNameLength = 256; uc.lpszUrlPath = path; uc.dwUrlPathLength = 2048;
    wchar_t extra[2048]; uc.lpszExtraInfo = extra; uc.dwExtraInfoLength = 2048;
    if (!WinHttpCrackUrl(wurl.c_str(), 0, 0, &uc)) return "not a valid address: " + url;
    if (uc.nScheme != INTERNET_SCHEME_HTTPS && uc.nScheme != INTERNET_SCHEME_HTTP) return "only http(s) addresses";
    std::wstring object = std::wstring(path, uc.dwUrlPathLength) + std::wstring(extra, uc.dwExtraInfoLength);
    HINTERNET s = WinHttpOpen(L"RaCACiT-Launcher/0.3", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME,
                              WINHTTP_NO_PROXY_BYPASS, 0);
    if (!s) return "network unavailable";
    std::string err;
    HINTERNET c = WinHttpConnect(s, std::wstring(host, uc.dwHostNameLength).c_str(), uc.nPort, 0);
    HINTERNET r = c ? WinHttpOpenRequest(c, L"GET", object.c_str(), nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                         uc.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0) : nullptr;
    if (!r) err = "cannot reach the server";
    if (err.empty() && (!WinHttpSendRequest(r, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
                        !WinHttpReceiveResponse(r, nullptr))) err = "no response from the server";
    DWORD status = 0, sz = sizeof status;
    if (err.empty()) {
        WinHttpQueryHeaders(r, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, nullptr, &status, &sz, nullptr);
        if (status != 200) err = "the server answered " + std::to_string(status);
    }
    HANDLE f = INVALID_HANDLE_VALUE;
    if (err.empty()) {
        if (p) {
            wchar_t len[32]; DWORD ls = sizeof len;
            if (WinHttpQueryHeaders(r, WINHTTP_QUERY_CONTENT_LENGTH, nullptr, len, &ls, nullptr)) p->total = _wcstoui64(len, nullptr, 10);
        }
        if (to_file) {
            std::error_code ec; fs::create_directories(to_file->parent_path(), ec);
            f = CreateFileW(to_file->c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (f == INVALID_HANDLE_VALUE) err = "cannot write " + u8(*to_file);
        }
    }
    if (err.empty()) {
        std::vector<char> buf(1u << 18);
        for (;;) {
            DWORD avail = 0, got = 0;
            if (!WinHttpQueryDataAvailable(r, &avail)) { err = "the download broke off"; break; }
            if (!avail) break;
            if (avail > buf.size()) avail = (DWORD)buf.size();
            if (!WinHttpReadData(r, buf.data(), avail, &got) || !got) { err = "the download broke off"; break; }
            if (to_file) { DWORD put = 0; if (!WriteFile(f, buf.data(), got, &put, nullptr) || put != got) { err = "write failed"; break; } }
            else if (body) body->append(buf.data(), got);
            if (p) { p->done += got; if (p->cancel) { err = "cancelled"; break; } }
        }
    }
    if (f != INVALID_HANDLE_VALUE) { CloseHandle(f); if (!err.empty()) DeleteFileW(to_file->c_str()); }
    if (r) WinHttpCloseHandle(r);
    if (c) WinHttpCloseHandle(c);
    WinHttpCloseHandle(s);
    return err;
}

// ---- JSON ------------------------------------------------------------------
const Json& Json::operator[](const std::string& k) const
{
    static const Json none;
    auto it = o.find(k);
    return it == o.end() ? none : it->second;
}
std::string Json::str(const std::string& k, const std::string& def) const
{
    const Json& v = (*this)[k];
    if (v.type == Str) return v.s;
    if (v.type == Num) { char b[64]; snprintf(b, sizeof b, "%g", v.n); return b; }
    return def;
}
double Json::num(const std::string& k, double def) const
{
    const Json& v = (*this)[k];
    if (v.type == Num) return v.n;
    if (v.type == Str) return atof(v.s.c_str());
    return def;
}

namespace {
struct JP {
    const char* p; const char* e; std::string err;
    void ws() { while (p < e && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')) p++; }
    bool lit(const char* s) { size_t n = strlen(s); if ((size_t)(e - p) >= n && !memcmp(p, s, n)) { p += n; return true; } return false; }
    bool str(std::string& out) {
        if (p >= e || *p != '"') return false;
        p++;
        while (p < e && *p != '"') {
            if (*p == '\\' && p + 1 < e) {
                p++;
                switch (*p) {
                case 'n': out += '\n'; break; case 't': out += '\t'; break; case 'r': out += '\r'; break;
                case 'b': out += '\b'; break; case 'f': out += '\f'; break;
                case 'u': {
                    if (e - p < 5) return false;
                    unsigned cp = (unsigned)strtoul(std::string(p + 1, p + 5).c_str(), nullptr, 16); p += 4;
                    if (cp < 0x80) out += (char)cp;
                    else if (cp < 0x800) { out += (char)(0xC0 | (cp >> 6)); out += (char)(0x80 | (cp & 63)); }
                    else { out += (char)(0xE0 | (cp >> 12)); out += (char)(0x80 | ((cp >> 6) & 63)); out += (char)(0x80 | (cp & 63)); }
                    break; }
                default: out += *p;
                }
                p++;
            } else out += *p++;
        }
        if (p >= e) return false;
        p++;
        return true;
    }
    bool val(Json& v, int depth) {
        if (depth > 64) { err = "too deep"; return false; }
        ws();
        if (p >= e) { err = "unexpected end"; return false; }
        if (*p == '{') {
            p++; v.type = Json::Obj; ws();
            if (p < e && *p == '}') { p++; return true; }
            for (;;) {
                ws(); std::string k;
                if (!str(k)) { err = "expected a key"; return false; }
                ws(); if (p >= e || *p != ':') { err = "expected ':'"; return false; } p++;
                if (!val(v.o[k], depth + 1)) return false;
                ws(); if (p < e && *p == ',') { p++; continue; }
                if (p < e && *p == '}') { p++; return true; }
                err = "expected ',' or '}'"; return false;
            }
        }
        if (*p == '[') {
            p++; v.type = Json::Arr; ws();
            if (p < e && *p == ']') { p++; return true; }
            for (;;) {
                v.a.emplace_back();
                if (!val(v.a.back(), depth + 1)) return false;
                ws(); if (p < e && *p == ',') { p++; continue; }
                if (p < e && *p == ']') { p++; return true; }
                err = "expected ',' or ']'"; return false;
            }
        }
        if (*p == '"') { v.type = Json::Str; if (!str(v.s)) { err = "bad string"; return false; } return true; }
        if (lit("true")) { v.type = Json::Bool; v.b = true; return true; }
        if (lit("false")) { v.type = Json::Bool; v.b = false; return true; }
        if (lit("null")) { v.type = Json::Null; return true; }
        char* end = nullptr; v.n = strtod(p, &end);
        if (end == p) { err = "unexpected character"; return false; }
        v.type = Json::Num; p = end; return true;
    }
};
}

bool json_parse(const std::string& text, Json* out, std::string* err)
{
    JP jp{ text.data(), text.data() + text.size(), {} };
    *out = Json();
    const bool ok = jp.val(*out, 0);
    if (!ok && err) *err = jp.err;
    return ok;
}

// ---- shell -------------------------------------------------------------------
static std::vector<fs::path> g_picks;
void queue_pick(const fs::path& p) { g_picks.push_back(p); }
static bool next_pick(fs::path* r)
{
    if (g_picks.empty()) return false;
    *r = g_picks.front(); g_picks.erase(g_picks.begin());
    return true;
}

fs::path pick_folder(void* hwnd, const wchar_t* title, const fs::path& start)
{
    fs::path r;
    if (next_pick(&r)) return r;
    IFileOpenDialog* d = nullptr;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&d)))) return r;
    DWORD o = 0; d->GetOptions(&o); d->SetOptions(o | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST);
    d->SetTitle(title);
    if (!start.empty()) {
        IShellItem* si = nullptr;
        if (SUCCEEDED(SHCreateItemFromParsingName(start.c_str(), nullptr, IID_PPV_ARGS(&si)))) { d->SetFolder(si); si->Release(); }
    }
    if (SUCCEEDED(d->Show((HWND)hwnd))) {
        IShellItem* it = nullptr;
        if (SUCCEEDED(d->GetResult(&it))) {
            PWSTR p = nullptr;
            if (SUCCEEDED(it->GetDisplayName(SIGDN_FILESYSPATH, &p))) { r = p; CoTaskMemFree(p); }
            it->Release();
        }
    }
    d->Release();
    return r;
}

fs::path pick_open_file(void* hwnd, const wchar_t* title, const wchar_t* filter_name, const wchar_t* filter_spec)
{
    fs::path r;
    if (next_pick(&r)) return r;
    IFileOpenDialog* d = nullptr;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&d)))) return r;
    COMDLG_FILTERSPEC f[2] = { { filter_name, filter_spec }, { L"All files", L"*.*" } };
    d->SetFileTypes(2, f); d->SetTitle(title);
    DWORD o = 0; d->GetOptions(&o); d->SetOptions(o | FOS_FORCEFILESYSTEM | FOS_FILEMUSTEXIST);
    if (SUCCEEDED(d->Show((HWND)hwnd))) {
        IShellItem* it = nullptr;
        if (SUCCEEDED(d->GetResult(&it))) {
            PWSTR p = nullptr;
            if (SUCCEEDED(it->GetDisplayName(SIGDN_FILESYSPATH, &p))) { r = p; CoTaskMemFree(p); }
            it->Release();
        }
    }
    d->Release();
    return r;
}

fs::path pick_save_file(void* hwnd, const wchar_t* title, const wchar_t* default_name,
                        const wchar_t* filter_name, const wchar_t* filter_spec)
{
    fs::path r;
    if (next_pick(&r)) return r;
    IFileSaveDialog* d = nullptr;
    if (FAILED(CoCreateInstance(CLSID_FileSaveDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&d)))) return r;
    COMDLG_FILTERSPEC f[1] = { { filter_name, filter_spec } };
    d->SetFileTypes(1, f); d->SetTitle(title); d->SetFileName(default_name);
    const wchar_t* dot = wcsrchr(filter_spec, L'.');
    if (dot) d->SetDefaultExtension(dot + 1);
    if (SUCCEEDED(d->Show((HWND)hwnd))) {
        IShellItem* it = nullptr;
        if (SUCCEEDED(d->GetResult(&it))) {
            PWSTR p = nullptr;
            if (SUCCEEDED(it->GetDisplayName(SIGDN_FILESYSPATH, &p))) { r = p; CoTaskMemFree(p); }
            it->Release();
        }
    }
    d->Release();
    return r;
}

void shell_open(const fs::path& p)
{
    ShellExecuteW(nullptr, L"open", p.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

fs::path exe_dir()
{
    wchar_t b[MAX_PATH * 2];
    GetModuleFileNameW(nullptr, b, MAX_PATH * 2);
    return fs::path(b).parent_path();
}
