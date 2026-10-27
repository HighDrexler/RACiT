#include "iso.h"

#include <windows.h>
#include <bcrypt.h>

#include <algorithm>
#include <cstring>

static const uint32_t SEC = 2048;

static uint32_t be32(const unsigned char* p) { return (uint32_t)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3]; }
static uint32_t le32(const unsigned char* p) { return (uint32_t)p[3] << 24 | p[2] << 16 | p[1] << 8 | p[0]; }

IsoImage::~IsoImage()
{
    if (key_) BCryptDestroyKey((BCRYPT_KEY_HANDLE)key_);
    if (alg_) BCryptCloseAlgorithmProvider((BCRYPT_ALG_HANDLE)alg_, 0);
    if (f_) CloseHandle((HANDLE)f_);
}

bool IsoImage::read_sectors(uint32_t first, uint32_t count, unsigned char* out)
{
    LARGE_INTEGER at; at.QuadPart = (LONGLONG)first * SEC;
    if (!SetFilePointerEx((HANDLE)f_, at, nullptr, FILE_BEGIN)) return false;
    const DWORD want = count * SEC; DWORD got = 0;
    return ReadFile((HANDLE)f_, out, want, &got, nullptr) && got == want;
}

// Each encrypted sector on its own: AES-128-CBC, IV = the sector number (big-endian,
// 16 bytes). ECB over the run, then the CBC chaining by hand -- one BCrypt call per run.
void IsoImage::decrypt(uint32_t first, uint32_t count, unsigned char* buf)
{
    if (!key_ || enc_.empty()) return;
    std::vector<unsigned char> ct;
    for (uint32_t s = first; s < first + count; ) {
        auto r = std::find_if(enc_.begin(), enc_.end(), [s](auto& e) { return s <= e.second; });
        if (r == enc_.end()) return;
        if (s < r->first) { s = std::min(first + count, r->first); continue; }
        const uint32_t end = std::min(first + count - 1, r->second);
        unsigned char* p = buf + (size_t)(s - first) * SEC;
        const ULONG n = (end - s + 1) * SEC;
        ct.assign(p, p + n);
        ULONG got = 0;
        BCryptDecrypt((BCRYPT_KEY_HANDLE)key_, ct.data(), n, nullptr, nullptr, 0, p, n, &got, 0);
        for (uint32_t k = s; k <= end; k++) {
            unsigned char* q = p + (size_t)(k - s) * SEC;
            const unsigned char* c = ct.data() + (size_t)(k - s) * SEC;
            q[12] ^= (unsigned char)(k >> 24); q[13] ^= (unsigned char)(k >> 16); q[14] ^= (unsigned char)(k >> 8); q[15] ^= (unsigned char)k;
            for (uint32_t b = 16; b < SEC; b++) q[b] ^= c[b - 16];
        }
        s = end + 1;
    }
}

void IsoImage::walk(uint32_t sector, uint32_t bytes, const std::string& prefix, bool joliet, int depth)
{
    if (depth > 16 || bytes > (64u << 20)) return;
    const uint32_t n = (bytes + SEC - 1) / SEC;
    std::vector<unsigned char> d((size_t)n * SEC);
    if (!read_sectors(sector, n, d.data())) return;
    for (uint32_t off = 0; off < bytes; ) {
        const unsigned char* r = d.data() + off;
        if (r[0] == 0) { off = (off / SEC + 1) * SEC; continue; }   // records never cross a sector
        if (r[0] < 34 || off + r[0] > d.size()) break;
        const uint32_t ext = le32(r + 2), size = le32(r + 10);
        const unsigned flags = r[25], nl = r[32];
        off += r[0];
        if (nl == 1 && (r[33] == 0 || r[33] == 1)) continue;          // . and ..
        std::string name;
        if (joliet) {
            std::wstring w;
            for (unsigned i = 0; i + 1 < nl; i += 2) w += (wchar_t)(r[33 + i] << 8 | r[34 + i]);
            name = narrow(w);
        } else name.assign((const char*)r + 33, nl);
        if (const size_t semi = name.find(';'); semi != std::string::npos) name.resize(semi);
        if (!joliet && !name.empty() && name.back() == '.') name.pop_back();
        if (name.empty() || name == "." || name == ".." || name.find_first_of("/\\:") != std::string::npos) continue;
        const std::string path = prefix + name;
        if (flags & 2) { walk(ext, size, path + "/", joliet, depth + 1); continue; }
        // A file over 4 GB is several records of the same name; all but the last flagged 0x80.
        if (!files_.empty() && files_.back().path == path && files_.back().extents.size() < 64) {
            files_.back().extents.push_back({ ext, size }); files_.back().size += size;
        } else {
            IsoFile f; f.path = path; f.size = size; f.extents.push_back({ ext, size });
            files_.push_back(f);
        }
    }
}

std::string IsoImage::open(const fs::path& iso)
{
    f_ = CreateFileW(iso.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (f_ == INVALID_HANDLE_VALUE) { f_ = nullptr; return "Cannot open " + u8(iso) + "."; }
    unsigned char s[SEC];
    // Sector 0 of a PS3 disc: the plain (unencrypted) regions as (first, last) sector
    // pairs; the sectors between them are the encrypted ones.
    if (read_sectors(0, 1, s)) {
        const uint32_t n = be32(s);
        if (n >= 1 && n <= 200 && be32(s + 4) == 0) {
            std::vector<uint32_t> b;
            for (uint32_t i = 0; i < 2 * n; i++) b.push_back(be32(s + 8 + i * 4));
            bool sane = b[0] == 0;
            for (size_t i = 1; i < b.size(); i++) sane = sane && b[i] >= b[i - 1];
            if (sane)
                for (uint32_t i = 1; i + 1 < b.size(); i += 2)
                    if (b[i + 1] > b[i] + 1) enc_.push_back({ b[i] + 1, b[i + 1] - 1 });
        }
    }
    // The volume descriptors from sector 16: the primary one, and Joliet's (long, mixed-case names).
    const unsigned char* root = nullptr; unsigned char pvd[SEC], svd[SEC]; bool joliet = false, have_pvd = false;
    for (uint32_t i = 16; i < 64; i++) {
        if (!read_sectors(i, 1, s) || memcmp(s + 1, "CD001", 5) != 0) break;
        if (s[0] == 255) break;
        if (s[0] == 1 && !have_pvd) { memcpy(pvd, s, SEC); have_pvd = true; }
        if (s[0] == 2 && s[88] == '%' && s[89] == '/' && (s[90] == '@' || s[90] == 'C' || s[90] == 'E')) { memcpy(svd, s, SEC); joliet = true; }
    }
    if (!have_pvd && !joliet) return u8(iso.filename()) + " is not a disc image (no ISO 9660 file system).";
    root = joliet ? svd + 156 : pvd + 156;
    walk(le32(root + 2), le32(root + 10), "", joliet, 0);
    if (files_.empty()) return u8(iso.filename()) + ": no files found in the image.";
    return {};
}

const IsoFile* IsoImage::find(const std::string& path) const
{
    const std::string want = lower(path);
    for (auto& f : files_) if (lower(f.path) == want) return &f;
    return nullptr;
}

std::string IsoImage::set_key(const std::string& hex_in)
{
    std::string hex;
    for (char c : hex_in) if (isxdigit((unsigned char)c)) hex += c; else if (!isspace((unsigned char)c)) return "The disc key has a character that is not a hex digit.";
    if (hex.size() != 32) return "A disc key is 32 hex digits (a .dkey file's text); this one has " + std::to_string(hex.size()) + ".";
    unsigned char k[16];
    for (int i = 0; i < 16; i++) k[i] = (unsigned char)strtoul(hex.substr(i * 2, 2).c_str(), nullptr, 16);
    if (!alg_) {
        BCRYPT_ALG_HANDLE a = nullptr;
        if (BCryptOpenAlgorithmProvider(&a, BCRYPT_AES_ALGORITHM, nullptr, 0) != 0) return "AES is not available.";
        BCryptSetProperty(a, BCRYPT_CHAINING_MODE, (PUCHAR)BCRYPT_CHAIN_MODE_ECB, sizeof(BCRYPT_CHAIN_MODE_ECB), 0);
        alg_ = a;
    }
    if (key_) { BCryptDestroyKey((BCRYPT_KEY_HANDLE)key_); key_ = nullptr; }
    BCRYPT_KEY_HANDLE h = nullptr;
    if (BCryptGenerateSymmetricKey((BCRYPT_ALG_HANDLE)alg_, &h, nullptr, 0, k, 16, 0) != 0) return "The disc key was not taken.";
    key_ = h;
    return {};
}

std::string IsoImage::read(const IsoFile& f, uint64_t max)
{
    std::string out;
    for (auto& e : f.extents) {
        if (out.size() >= max) break;
        const uint32_t take = (uint32_t)std::min<uint64_t>(e.second, max - out.size());
        const uint32_t n = (take + SEC - 1) / SEC;
        std::vector<unsigned char> b((size_t)n * SEC);
        if (!read_sectors(e.first, n, b.data())) break;
        decrypt(e.first, n, b.data());
        out.append((const char*)b.data(), take);
    }
    return out;
}

std::string IsoImage::extract(const IsoFile& f, const fs::path& dst, Progress* p)
{
    std::error_code ec;
    fs::create_directories(dst.parent_path(), ec);
    HANDLE o = CreateFileW(dst.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (o == INVALID_HANDLE_VALUE) return "cannot write " + u8(dst);
    const uint32_t chunk = 2048;   // sectors: 4 MB
    std::vector<unsigned char> b((size_t)chunk * SEC);
    bool ok = true;
    for (auto& e : f.extents) {
        uint64_t left = e.second; uint32_t s = e.first;
        while (ok && left) {
            const uint32_t n = (uint32_t)std::min<uint64_t>(chunk, (left + SEC - 1) / SEC);
            if (!read_sectors(s, n, b.data())) { CloseHandle(o); DeleteFileW(dst.c_str()); return "the image ends early (an incomplete .iso?) at " + f.path; }
            decrypt(s, n, b.data());
            const DWORD put_n = (DWORD)std::min<uint64_t>(left, (uint64_t)n * SEC); DWORD put = 0;
            if (!WriteFile(o, b.data(), put_n, &put, nullptr) || put != put_n) ok = false;
            left -= put_n; s += n;
            if (p) { p->done += put_n; if (p->cancel) ok = false; }
        }
    }
    CloseHandle(o);
    if (!ok) { DeleteFileW(dst.c_str()); return p && p->cancel ? "cancelled" : "write failed (disk full?) at " + u8(dst); }
    return {};
}

std::string find_dkey(const fs::path& iso, fs::path* found)
{
    std::error_code ec;
    fs::path k = iso; k.replace_extension(L".dkey");
    if (!fs::exists(k, ec)) {
        k.clear(); int n = 0;
        for (auto& d : fs::directory_iterator(iso.parent_path(), ec))
            if (lower(u8(d.path().extension())) == ".dkey") { k = d.path(); n++; }
        if (n != 1) return {};
    }
    if (found) *found = k;
    return trim(read_text(k, 256));
}
