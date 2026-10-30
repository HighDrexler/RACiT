#include "util.h"

#include <windows.h>
#include <bcrypt.h>
#include <shellapi.h>
#include <shobjidl.h>
#include <tlhelp32.h>
#include <winhttp.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
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

// ---- the launcher's own zip reader ----------------------------------------------------------
namespace {
struct MappedFile {
    HANDLE f = INVALID_HANDLE_VALUE, m = nullptr;
    const unsigned char* d = nullptr;
    uint64_t size = 0;
    explicit MappedFile(const fs::path& p)
    {
        f = CreateFileW(p.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
        LARGE_INTEGER n;
        if (f == INVALID_HANDLE_VALUE || !GetFileSizeEx(f, &n) || !n.QuadPart) return;
        size = (uint64_t)n.QuadPart;
        m = CreateFileMappingW(f, nullptr, PAGE_READONLY, 0, 0, nullptr);
        if (m) d = (const unsigned char*)MapViewOfFile(m, FILE_MAP_READ, 0, 0, 0);
    }
    ~MappedFile()
    {
        if (d) UnmapViewOfFile(d);
        if (m) CloseHandle(m);
        if (f != INVALID_HANDLE_VALUE) CloseHandle(f);
    }
};
uint16_t rd16(const unsigned char* p) { return (uint16_t)(p[0] | p[1] << 8); }
uint32_t rd32(const unsigned char* p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }

uint32_t crc32_add(uint32_t crc, const unsigned char* p, size_t n)
{
    static const std::vector<uint32_t> table = [] {
        std::vector<uint32_t> t(256);
        for (uint32_t i = 0; i < 256; i++) { uint32_t c = i; for (int k = 0; k < 8; k++) c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1; t[i] = c; }
        return t;
    }();
    crc = ~crc;
    while (n--) crc = table[(crc ^ *p++) & 255] ^ (crc >> 8);
    return ~crc;
}

// What comes out of an entry: to a file, counted and CRC'd, stopping at the size it declared.
struct ZipOut {
    HANDLE f; uint64_t limit; Progress* p;
    uint64_t total = 0; uint32_t crc = 0; std::string err;
    bool write(const unsigned char* d, size_t n)
    {
        if (total + n > limit) { err = "bigger than it says"; return false; }
        DWORD put = 0;
        if (!WriteFile(f, d, (DWORD)n, &put, nullptr) || put != n) { err = "write failed (disk full?)"; return false; }
        crc = crc32_add(crc, d, n);
        total += n;
        if (p) { p->done += n; if (p->cancel) { err = "cancelled"; return false; } }
        return true;
    }
};

// Deflate (RFC 1951), one bit at a time through canonical codes. Keeps 32 KB of what it wrote.
struct Inflate {
    const unsigned char* in; size_t n; ZipOut* out;
    size_t pos = 0; uint32_t hold = 0; int cnt = 0; bool bad = false;
    std::vector<unsigned char> buf = std::vector<unsigned char>(1u << 20);
    size_t have = 0, flushed = 0;   // buf[flushed..have) is not written yet; the 32 KB before it is the window

    struct Code { uint16_t count[16]; uint16_t sym[320]; };
    uint32_t bits(int need)
    {
        while (cnt < need) {
            if (pos >= n) { bad = true; return 0; }
            hold |= (uint32_t)in[pos++] << cnt; cnt += 8;
        }
        const uint32_t v = need ? hold & ((1u << need) - 1) : 0;
        hold >>= need; cnt -= need;
        return v;
    }
    static void build(Code& c, const unsigned char* len, int syms)
    {
        memset(c.count, 0, sizeof c.count);
        for (int i = 0; i < syms; i++) c.count[len[i]]++;
        c.count[0] = 0;
        uint16_t offs[16]; offs[1] = 0;
        for (int i = 1; i < 15; i++) offs[i + 1] = (uint16_t)(offs[i] + c.count[i]);
        for (int i = 0; i < syms; i++) if (len[i]) c.sym[offs[len[i]]++] = (uint16_t)i;
    }
    int decode(const Code& c)
    {
        int code = 0, first = 0, index = 0;
        for (int l = 1; l <= 15; l++) {
            code |= (int)bits(1);
            if (bad) return -1;
            const int k = c.count[l];
            if (code - k < first) return c.sym[index + (code - first)];
            index += k; first += k; first <<= 1; code <<= 1;
        }
        return -1;
    }
    bool flush()
    {
        if (have > flushed && !out->write(buf.data() + flushed, have - flushed)) return false;
        flushed = have;
        return true;
    }
    bool room(size_t need)
    {
        if (have + need <= buf.size()) return true;
        if (!flush()) return false;
        const size_t keep = std::min<size_t>(have, 32768);
        memmove(buf.data(), buf.data() + have - keep, keep);
        have = flushed = keep;
        return true;
    }
    bool codes(const Code& lit, const Code& dist)
    {
        static const uint16_t lbase[29] = { 3,4,5,6,7,8,9,10,11,13,15,17,19,23,27,31,35,43,51,59,67,83,99,115,131,163,195,227,258 };
        static const uint8_t lext[29] = { 0,0,0,0,0,0,0,0,1,1,1,1,2,2,2,2,3,3,3,3,4,4,4,4,5,5,5,5,0 };
        static const uint16_t dbase[30] = { 1,2,3,4,5,7,9,13,17,25,33,49,65,97,129,193,257,385,513,769,1025,1537,2049,3073,4097,6145,8193,12289,16385,24577 };
        static const uint8_t dext[30] = { 0,0,0,0,1,1,2,2,3,3,4,4,5,5,6,6,7,7,8,8,9,9,10,10,11,11,12,12,13,13 };
        for (;;) {
            const int s = decode(lit);
            if (s < 0) return false;
            if (s == 256) return true;
            if (!room(258)) return false;
            if (s < 256) { buf[have++] = (unsigned char)s; continue; }
            if (s - 257 >= 29) return false;
            const size_t len = lbase[s - 257] + bits(lext[s - 257]);
            const int ds = decode(dist);
            if (ds < 0 || ds >= 30) return false;
            const size_t d = dbase[ds] + bits(dext[ds]);
            if (bad || d > have) return false;
            for (size_t i = 0; i < len; i++) { buf[have] = buf[have - d]; have++; }
        }
    }
    bool run()
    {
        static Code fixed_l, fixed_d;
        static const bool once = [] {
            unsigned char l[320];
            int i = 0;
            for (; i < 144; i++) l[i] = 8;
            for (; i < 256; i++) l[i] = 9;
            for (; i < 280; i++) l[i] = 7;
            for (; i < 288; i++) l[i] = 8;
            build(fixed_l, l, 288);
            for (i = 0; i < 30; i++) l[i] = 5;
            build(fixed_d, l, 30);
            return true;
        }();
        (void)once;
        for (int last = 0; !last;) {
            last = (int)bits(1);
            const int type = (int)bits(2);
            if (bad) return false;
            if (type == 0) {
                hold = 0; cnt = 0;
                if (pos + 4 > n) return false;
                const size_t len = rd16(in + pos), nlen = rd16(in + pos + 2);
                pos += 4;
                if ((len ^ 0xFFFF) != nlen || pos + len > n) return false;
                if (!room(len)) return false;
                memcpy(buf.data() + have, in + pos, len);
                have += len; pos += len;
            } else if (type == 1) {
                if (!codes(fixed_l, fixed_d)) return false;
            } else if (type == 2) {
                static const uint8_t order[19] = { 16,17,18,0,8,7,9,6,10,5,11,4,12,3,13,2,14,1,15 };
                const int nlen = (int)bits(5) + 257, ndist = (int)bits(5) + 1, ncode = (int)bits(4) + 4;
                if (bad || nlen > 286 || ndist > 30) return false;
                unsigned char lens[320] = {};
                for (int i = 0; i < ncode; i++) lens[order[i]] = (unsigned char)bits(3);
                Code cl; build(cl, lens, 19);
                memset(lens, 0, sizeof lens);
                for (int i = 0; i < nlen + ndist;) {
                    const int s = decode(cl);
                    if (s < 0) return false;
                    if (s < 16) { lens[i++] = (unsigned char)s; continue; }
                    int rep; unsigned char v = 0;
                    if (s == 16) { if (!i) return false; v = lens[i - 1]; rep = 3 + (int)bits(2); }
                    else if (s == 17) rep = 3 + (int)bits(3);
                    else rep = 11 + (int)bits(7);
                    if (bad || i + rep > nlen + ndist) return false;
                    while (rep--) lens[i++] = v;
                }
                Code lit, dist;
                build(lit, lens, nlen);
                build(dist, lens + nlen, ndist);
                if (!codes(lit, dist)) return false;
            } else return false;
        }
        return flush();
    }
};

// Only a plain relative path: nothing Windows would read as a drive, a parent folder or a device stream.
bool zip_name_safe(const std::string& name)
{
    if (name.empty() || name[0] == '/' || name.find(':') != std::string::npos) return false;
    size_t i = 0;
    while (i <= name.size()) {
        size_t j = name.find('/', i);
        if (j == std::string::npos) j = name.size();
        const std::string part = name.substr(i, j - i);
        if (part.empty() && j != name.size()) return false;
        if (!part.empty() && (part.back() == '.' || part.back() == ' ')) return false;
        i = j + 1;
    }
    for (unsigned char c : name) if (c < 32) return false;
    return true;
}
}  // namespace

std::string zip_list(const fs::path& zip, std::vector<ZipEntry>* out)
{
    out->clear();
    MappedFile z(zip);
    if (!z.d) return "cannot read " + u8(zip);
    if (z.size < 22) return "not a zip file";
    const unsigned char* eocd = nullptr;
    for (uint64_t back = 22; back <= std::min<uint64_t>(z.size, 22 + 65535); back++)
        if (rd32(z.d + z.size - back) == 0x06054B50) { eocd = z.d + z.size - back; break; }
    if (!eocd) return "not a zip file";
    const uint32_t count = rd16(eocd + 10), cd_size = rd32(eocd + 12), cd_at = rd32(eocd + 16);
    if (count == 0xFFFF || cd_size == 0xFFFFFFFFu || cd_at == 0xFFFFFFFFu) return "the zip is over 4 GB, which is not supported";
    if ((uint64_t)cd_at + cd_size > z.size) return "the zip is damaged";
    const unsigned char* p = z.d + cd_at; const unsigned char* const end = p + cd_size;
    for (uint32_t i = 0; i < count; i++) {
        if (end - p < 46 || rd32(p) != 0x02014B50) return "the zip is damaged";
        const size_t nl = rd16(p + 28), xl = rd16(p + 30), cl = rd16(p + 32);
        if ((size_t)(end - p) < 46 + nl + xl + cl) return "the zip is damaged";
        ZipEntry e;
        e.name.assign((const char*)p + 46, nl);
        for (char& c : e.name) if (c == '\\') c = '/';
        e.is_dir = !e.name.empty() && e.name.back() == '/';
        if (e.is_dir) e.name.pop_back();
        e.method = rd16(p + 10); e.crc = rd32(p + 16); e.packed = rd32(p + 20); e.size = rd32(p + 24); e.offset = rd32(p + 42);
        if (e.packed == 0xFFFFFFFFu || e.size == 0xFFFFFFFFu || e.offset == 0xFFFFFFFFu) return "the zip is over 4 GB, which is not supported";
        if (rd16(p + 8) & 1) return "the zip is password protected";
        if (!e.is_dir && e.method != 0 && e.method != 8) return e.name + " is packed in a way the launcher cannot read (zip method " + std::to_string(e.method) + ": it needs Deflate)";
        if (!zip_name_safe(e.name)) return "the zip has an unsafe file name: " + e.name;
        out->push_back(std::move(e));
        p += 46 + nl + xl + cl;
    }
    return {};
}

std::string zip_unpack_entry(const fs::path& zip, const ZipEntry& e, const fs::path& to, Progress* p)
{
    MappedFile z(zip);
    if (!z.d) return "cannot read " + u8(zip);
    if (e.offset + 30 > z.size || rd32(z.d + e.offset) != 0x04034B50) return "the zip is damaged at " + e.name;
    const uint64_t at = e.offset + 30 + rd16(z.d + e.offset + 26) + rd16(z.d + e.offset + 28);
    if (at + e.packed > z.size) return "the zip is damaged at " + e.name;
    std::error_code ec;
    fs::create_directories(to.parent_path(), ec);
    HANDLE f = CreateFileW(to.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return "cannot write " + u8(to);
    ZipOut out{ f, e.size, p };
    bool ok;
    if (e.method == 0) {
        ok = e.packed == e.size;
        for (uint64_t i = 0; ok && i < e.packed;) {   // in slices, so Cancel is heard
            const size_t n = (size_t)std::min<uint64_t>(e.packed - i, 1u << 20);
            ok = out.write(z.d + at + i, n); i += n;
        }
    } else {
        auto inf = std::make_unique<Inflate>(Inflate{ z.d + at, (size_t)e.packed, &out });
        ok = inf->run();
    }
    CloseHandle(f);
    std::string err = out.err;
    if (ok && (out.total != e.size || out.crc != e.crc)) err = "the zip is damaged at " + e.name + " (its check does not match)";
    else if (!ok && err.empty()) err = "the zip is damaged at " + e.name;
    if (!err.empty()) DeleteFileW(to.c_str());
    return err;
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

std::string http_get(const std::string& url, std::string* body, const fs::path* to_file, Progress* p, int* status_out, const wchar_t* headers)
{
    if (status_out) *status_out = 0;
    std::wstring wurl = widen(url);
    URL_COMPONENTS uc{}; uc.dwStructSize = sizeof uc;
    wchar_t host[256], path[2048];
    uc.lpszHostName = host; uc.dwHostNameLength = 256; uc.lpszUrlPath = path; uc.dwUrlPathLength = 2048;
    wchar_t extra[2048]; uc.lpszExtraInfo = extra; uc.dwExtraInfoLength = 2048;
    if (!WinHttpCrackUrl(wurl.c_str(), 0, 0, &uc)) return "not a valid address: " + url;
    if (uc.nScheme != INTERNET_SCHEME_HTTPS && uc.nScheme != INTERNET_SCHEME_HTTP) return "only http(s) addresses";
    std::wstring object = std::wstring(path, uc.dwUrlPathLength) + std::wstring(extra, uc.dwExtraInfoLength);
    HINTERNET s = WinHttpOpen(L"RACiT-Launcher", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME,
                              WINHTTP_NO_PROXY_BYPASS, 0);
    if (!s) return "network unavailable";
    WinHttpSetTimeouts(s, 10000, 10000, 15000, 30000);   // a dead connection says so soon
    DWORD redirects = WINHTTP_OPTION_REDIRECT_POLICY_DISALLOW_HTTPS_TO_HTTP;
    WinHttpSetOption(s, WINHTTP_OPTION_REDIRECT_POLICY, &redirects, sizeof redirects);
    std::string err;
    HINTERNET c = WinHttpConnect(s, std::wstring(host, uc.dwHostNameLength).c_str(), uc.nPort, 0);
    HINTERNET r = c ? WinHttpOpenRequest(c, L"GET", object.c_str(), nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                         uc.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0) : nullptr;
    if (!r) err = "cannot reach the server";
    if (err.empty() && (!WinHttpSendRequest(r, headers ? headers : WINHTTP_NO_ADDITIONAL_HEADERS, headers ? (DWORD)-1L : 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
                        !WinHttpReceiveResponse(r, nullptr))) err = "no response from the server";
    DWORD status = 0, sz = sizeof status;
    if (err.empty()) {
        WinHttpQueryHeaders(r, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, nullptr, &status, &sz, nullptr);
        if (status_out) *status_out = (int)status;
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

fs::path exe_path()
{
    wchar_t b[MAX_PATH * 2];
    GetModuleFileNameW(nullptr, b, MAX_PATH * 2);
    return fs::path(b);
}

fs::path exe_dir() { return exe_path().parent_path(); }

bool process_running_under(const fs::path& dir)
{
    const std::wstring base = widen(lower(u8(dir)) + "\\");
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return false;
    PROCESSENTRY32W e{}; e.dwSize = sizeof e;
    bool found = false;
    for (BOOL ok = Process32FirstW(snap, &e); ok && !found; ok = Process32NextW(snap, &e)) {
        HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, e.th32ProcessID);
        if (!h) continue;
        wchar_t b[MAX_PATH * 2]; DWORD n = MAX_PATH * 2;
        if (QueryFullProcessImageNameW(h, 0, b, &n) && widen(lower(narrow(std::wstring(b, n)))).rfind(base, 0) == 0) found = true;
        CloseHandle(h);
    }
    CloseHandle(snap);
    return found;
}

uint64_t free_space(const fs::path& dir)
{
    ULARGE_INTEGER a;
    return GetDiskFreeSpaceExW(dir.c_str(), &a, nullptr, nullptr) ? a.QuadPart : 0;
}
