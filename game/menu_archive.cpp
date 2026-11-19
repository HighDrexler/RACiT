/* Select the separately prepared menu archive for read-only game opens.
 *
 * RACACIT_MENU_ARCHIVE=<file> names an archive made by scripts/prepare_menu_keycaps.py.
 * Otherwise the game makes the same archive itself, the first time it opens
 * global_cached.psarc: the same steps as the script, with the compiled actions built
 * into the game. It's kept in a menu-keycaps folder next to the settings file and reused
 * while the original's size and time match. On by default on desktops (issue #22: the
 * menus showed controller buttons with a keyboard); RACACIT_MENU_KEYCAPS=0 turns it off.
 * Android needs RACACIT_MENU_KEYCAPS=1. */
#include <algorithm>
#include <array>
#include <atomic>
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <mutex>
#include <stdexcept>
#include <string>
#include <vector>
#include <zlib.h>
#ifdef _WIN32
#include <process.h>
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#define racit_getpid _getpid
#else
#include <csignal>
#include <cerrno>
#include <unistd.h>
#define racit_getpid getpid
#endif

static std::string s_menu_archive;
static std::atomic<bool> s_menu_active{false};
static bool s_menu_auto = false;
static std::filesystem::path s_menu_cache;

static const unsigned char k_menu_actions[] = {
#include "menu_keycaps_actions.inc"
};

namespace {

using Bytes = std::vector<unsigned char>;
constexpr uint64_t kMemberLimit = 8 * 1024 * 1024;

void require(bool condition, const char* message)
{
    if (!condition) throw std::runtime_error(message);
}

uint64_t number(const unsigned char* p, int width)
{
    uint64_t value = 0;
    for (int i = 0; i < width; ++i) value = (value << 8) | p[i];
    return value;
}

void put(unsigned char* p, int width, uint64_t value)
{
    for (int i = width - 1; i >= 0; --i, value >>= 8) p[i] = (unsigned char)value;
}

uint32_t le32(const Bytes& data, size_t at)
{
    return data[at] | data[at + 1] << 8 | data[at + 2] << 16 | (uint32_t)data[at + 3] << 24;
}

void put_le32(Bytes& data, size_t at, uint32_t value)
{
    for (int i = 0; i < 4; ++i) data[at + i] = (unsigned char)(value >> (8 * i));
}

/* Inflates one zlib stream that must produce exactly `wanted` bytes and end there. */
Bytes inflate_exact(const unsigned char* src, size_t size, size_t wanted)
{
    Bytes out(wanted + 1);
    z_stream z{};
    require(inflateInit(&z) == Z_OK, "zlib init failed");
    z.next_in = const_cast<unsigned char*>(src);
    z.avail_in = (uInt)size;
    z.next_out = out.data();
    z.avail_out = (uInt)out.size();
    const int status = inflate(&z, Z_FINISH);
    const size_t produced = z.total_out;
    const bool all_in = z.avail_in == 0;
    inflateEnd(&z);
    require(status == Z_STREAM_END && all_in && produced == wanted, "invalid compressed data");
    out.resize(wanted);
    return out;
}

Bytes deflate9(const unsigned char* src, size_t size)
{
    uLongf length = compressBound((uLong)size);
    Bytes out(length);
    require(compress2(out.data(), &length, src, (uLong)size, 9) == Z_OK, "zlib compress failed");
    out.resize(length);
    return out;
}

struct Member { uint64_t first, length, offset; std::vector<uint32_t> sizes; uint64_t span; };

struct Archive {
    std::FILE* file = nullptr;
    std::array<unsigned char, 32> header{};
    uint32_t toc_size = 0, entry_size = 0, count = 0, block = 0;
    int width = 2;
    Bytes toc;
    uint64_t length = 0;
    std::vector<Member> members;

    explicit Archive(std::FILE* f) : file(f)
    {
        require(std::fread(header.data(), 1, 32, file) == 32 && !std::memcmp(header.data(), "PSAR", 4) &&
                !std::memcmp(header.data() + 8, "zlib", 4), "expected a zlib PSARC archive");
        toc_size = (uint32_t)number(&header[12], 4);
        entry_size = (uint32_t)number(&header[16], 4);
        count = (uint32_t)number(&header[20], 4);
        block = (uint32_t)number(&header[24], 4);
        require(toc_size >= 32 && toc_size <= 64u * 1024 * 1024 && entry_size >= 30 && count > 0 &&
                uint64_t(count) * entry_size <= toc_size - 32 && block > 0 && block <= 1024 * 1024,
                "invalid archive table");
        width = block <= 65536 ? 2 : 3;
        toc.resize(toc_size - 32);
        require(std::fread(toc.data(), 1, toc.size(), file) == toc.size(), "truncated archive table");
        std::fseek(file, 0, SEEK_END);
        length = (uint64_t)std::ftell(file);
        for (uint32_t index = 0; index < count; ++index) {
            const unsigned char* entry = &toc[size_t(index) * entry_size];
            Member m{number(entry + 16, 4), number(entry + 20, 5), number(entry + 25, 5), {}, 0};
            const uint64_t blocks = (m.length + block - 1) / block;
            const uint64_t table = uint64_t(count) * entry_size + m.first * width;
            require(table + blocks * width <= toc.size(), "member exceeds block table");
            for (uint64_t p = table; p < table + blocks * width; p += width) {
                uint32_t size = (uint32_t)number(&toc[p], width);
                if (!size) size = block;
                require(size <= block, "invalid member block size");
                m.sizes.push_back(size);
                m.span += size;
            }
            require(m.offset + m.span <= length && (!m.length || m.offset >= toc_size), "invalid member extent");
            members.push_back(std::move(m));
        }
    }

    Bytes read(uint32_t index)
    {
        const Member& m = members[index];
        require(m.length <= kMemberLimit, "menu or manifest exceeds 8 MiB");
        std::fseek(file, (long)m.offset, SEEK_SET);
        Bytes result;
        Bytes raw;
        for (uint32_t size : m.sizes) {
            raw.resize(size);
            require(std::fread(raw.data(), 1, size, file) == size, "truncated member");
            const size_t wanted = (size_t)std::min<uint64_t>(block, m.length - result.size());
            if (size != wanted) {
                Bytes plain = inflate_exact(raw.data(), size, wanted);
                result.insert(result.end(), plain.begin(), plain.end());
            } else {
                result.insert(result.end(), raw.begin(), raw.end());
            }
        }
        return result;
    }

    void copy(std::FILE* out, uint64_t start, uint64_t end)
    {
        require(end >= start, "replacement physical extents overlap");
        std::fseek(file, (long)start, SEEK_SET);
        std::vector<char> chunk(1024 * 1024);
        while (start < end) {
            const size_t n = std::fread(chunk.data(), 1, (size_t)std::min<uint64_t>(end - start, chunk.size()), file);
            require(n > 0, "truncated archive while copying");
            require(std::fwrite(chunk.data(), 1, n, out) == n, "write failed");
            start += n;
        }
    }

    /* Same layout rules as prepare_menu_keycaps.py's Archive.write. */
    void write(std::FILE* out, const std::map<uint32_t, Bytes>& replacements)
    {
        struct Encoded { Bytes data; std::vector<uint32_t> sizes; };
        std::map<uint32_t, Encoded> encoded;
        for (const auto& [index, data] : replacements) {
            Encoded e;
            for (size_t start = 0; start < data.size(); start += block) {
                const size_t n = std::min<size_t>(block, data.size() - start);
                Bytes z = deflate9(&data[start], n);
                const bool use_z = z.size() < n;
                const unsigned char* chunk = use_z ? z.data() : &data[start];
                const size_t chunk_size = use_z ? z.size() : n;
                e.data.insert(e.data.end(), chunk, chunk + chunk_size);
                e.sizes.push_back(uint32_t(chunk_size % block));
            }
            encoded[index] = std::move(e);
        }
        std::vector<uint32_t> by_first, by_offset;
        for (const auto& kv : encoded) { by_first.push_back(kv.first); by_offset.push_back(kv.first); }
        std::sort(by_first.begin(), by_first.end(), [&](uint32_t a, uint32_t b) { return members[a].first < members[b].first; });
        std::sort(by_offset.begin(), by_offset.end(), [&](uint32_t a, uint32_t b) { return members[a].offset < members[b].offset; });

        const size_t table = size_t(count) * entry_size;
        Bytes out_toc(toc.begin(), toc.begin() + table);
        size_t cursor = table;
        for (uint32_t index : by_first) {
            const Member& m = members[index];
            const size_t start = table + m.first * width;
            require(start >= cursor, "replacement block tables overlap");
            out_toc.insert(out_toc.end(), toc.begin() + cursor, toc.begin() + start);
            for (uint32_t size : encoded[index].sizes) {
                unsigned char w[3];
                put(w, width, size);
                out_toc.insert(out_toc.end(), w, w + width);
            }
            cursor = start + m.sizes.size() * width;
        }
        out_toc.insert(out_toc.end(), toc.begin() + cursor, toc.end());
        const int64_t toc_delta = int64_t(out_toc.size()) - int64_t(toc.size());

        for (uint32_t member = 0; member < count; ++member) {
            const Member& m = members[member];
            int64_t new_first = (int64_t)m.first;
            int64_t new_offset = (m.length || m.offset) ? int64_t(m.offset) + toc_delta : 0;
            for (uint32_t index : by_first) {
                const Member& old = members[index];
                const uint64_t old_end = old.offset + old.span;
                if (member != index && m.length) {
                    require(m.first + m.sizes.size() <= old.first || m.first >= old.first + old.sizes.size(),
                            "replacement shares another member block table");
                    require(m.offset + m.span <= old.offset || m.offset >= old_end,
                            "replacement overlaps another member");
                }
                if (m.first >= old.first + old.sizes.size())
                    new_first += int64_t(encoded[index].sizes.size()) - int64_t(old.sizes.size());
                if (m.offset >= old_end)
                    new_offset += int64_t(encoded[index].data.size()) - int64_t(old.span);
            }
            unsigned char* entry = &out_toc[size_t(member) * entry_size];
            put(entry + 16, 4, (uint64_t)new_first);
            put(entry + 25, 5, (uint64_t)new_offset);
            if (auto r = replacements.find(member); r != replacements.end()) put(entry + 20, 5, r->second.size());
        }
        std::array<unsigned char, 32> out_header = header;
        put(&out_header[12], 4, 32 + out_toc.size());
        require(std::fwrite(out_header.data(), 1, 32, out) == 32 &&
                std::fwrite(out_toc.data(), 1, out_toc.size(), out) == out_toc.size(), "write failed");
        uint64_t pos = toc_size;
        for (uint32_t index : by_offset) {
            const Member& m = members[index];
            copy(out, pos, m.offset);
            const Bytes& data = encoded[index].data;
            require(std::fwrite(data.data(), 1, data.size(), out) == data.size(), "write failed");
            pos = m.offset + m.span;
        }
        copy(out, pos, length);
    }
};

/* Same as prepare_menu_keycaps.py's movie_actions: the actions go in as a DoAction tag
 * before the first ShowFrame. Empty when the movie has no ButtonPrompt. */
Bytes movie_actions(const Bytes& original)
{
    require(original.size() >= 14, "unsupported menu movie format");
    const bool known = !std::memcmp(original.data(), "FWS", 3) || !std::memcmp(original.data(), "CWS", 3) ||
                       !std::memcmp(original.data(), "GFX", 3) || !std::memcmp(original.data(), "CFX", 3);
    require(known, "unsupported menu movie format");
    const uint32_t length = le32(original, 4);
    require(length >= 14 && length <= kMemberLimit, "movie exceeds 8 MiB");
    Bytes movie;
    const bool compressed = original[0] == 'C';
    if (compressed) {
        Bytes body = inflate_exact(original.data() + 8, original.size() - 8, length - 8);
        movie.assign(original.begin(), original.begin() + 8);
        movie[0] = original[1] == 'W' ? 'F' : 'G';
        movie.insert(movie.end(), body.begin(), body.end());
    } else {
        movie = original;
    }
    require(movie.size() == length, "movie length mismatch");
    auto contains = [&movie](const char* text) {
        const size_t n = std::strlen(text);
        return std::search(movie.begin(), movie.end(), text, text + n) != movie.end();
    };
    if (!contains("ButtonPrompt")) return {};
    require(!contains("__rccitPromptDriver"), "input movie already contains keyboard actions");
    size_t position = 8 + (5 + 4 * (movie[8] >> 3) + 7) / 8 + 4;
    size_t insertion = SIZE_MAX;
    unsigned code = ~0u;
    while (position < movie.size()) {
        const size_t start = position;
        require(position + 2 <= movie.size(), "truncated movie tag");
        const unsigned tag = movie[position] | movie[position + 1] << 8;
        code = tag >> 6;
        size_t size = tag & 63;
        position += 2;
        if (size == 63) {
            require(position + 4 <= movie.size(), "truncated long movie tag");
            size = le32(movie, position);
            position += 4;
        }
        position += size;
        require(position <= movie.size(), "movie tag exceeds member");
        if (code == 1 && insertion == SIZE_MAX) insertion = start;
        if (code == 0) {
            require(position == movie.size(), "bytes follow movie End tag");
            break;
        }
    }
    require(insertion != SIZE_MAX && code == 0, "movie needs ShowFrame and End tags");
    Bytes tag(6);
    tag[0] = (unsigned char)((12 << 6) | 63);
    tag[1] = (unsigned char)(((12 << 6) | 63) >> 8);
    put_le32(tag, 2, sizeof k_menu_actions);
    tag.insert(tag.end(), k_menu_actions, k_menu_actions + sizeof k_menu_actions);
    Bytes patched(movie.begin(), movie.begin() + insertion);
    patched.insert(patched.end(), tag.begin(), tag.end());
    patched.insert(patched.end(), movie.begin() + insertion, movie.end());
    put_le32(patched, 4, (uint32_t)patched.size());
    if (!compressed) return patched;
    Bytes out(original.begin(), original.begin() + 4);
    out.insert(out.end(), patched.begin() + 4, patched.begin() + 8);
    Bytes z = deflate9(patched.data() + 8, patched.size() - 8);
    out.insert(out.end(), z.begin(), z.end());
    return out;
}

// A readable PSARC whose header and table are sane. original_size > 0 also asks for a
// size near the original's: the prepared archive only swaps a few menu movies.
bool usable_archive(const std::filesystem::path& path, uint64_t original_size = 0)
{
    std::error_code error;
    if (!std::filesystem::is_regular_file(path, error) || error) return false;
    const uint64_t length = std::filesystem::file_size(path, error);
    if (error) return false;
    std::ifstream input(path, std::ios::binary);
    std::array<unsigned char, 32> header{};
    input.read(reinterpret_cast<char*>(header.data()), header.size());
    const auto word = [&header](size_t offset) {
        unsigned value = 0;
        for (size_t i = offset; i < offset + 4; ++i) value = (value << 8) | header[i];
        return value;
    };
    const unsigned toc = word(12), entry = word(16), count = word(20), block = word(24);
    if (!input || std::memcmp(header.data(), "PSAR", 4) || std::memcmp(header.data() + 8, "zlib", 4) ||
        toc < 32 || toc >= length || entry < 30 || !count || uint64_t(entry) * count > toc - 32 ||
        !block || block > 1024 * 1024)
        return false;
    return !original_size || (length >= original_size / 2 && length <= original_size * 2);
}

// <archive>.ok, written after an archive this game made was checked and published: its
// size and modification time. Only an archive whose marker still matches is reused, so a
// file someone else put there (or one that changed since) is made again.
std::string marker_text(const std::filesystem::path& archive)
{
    std::error_code error;
    const uint64_t length = std::filesystem::file_size(archive, error);
    if (error) return std::string();
    const auto time = std::filesystem::last_write_time(archive, error).time_since_epoch().count();
    if (error) return std::string();
    char text[64];
    std::snprintf(text, sizeof text, "%llu %lld\n", (unsigned long long)length, (long long)time);
    return text;
}

std::filesystem::path marker_of(const std::filesystem::path& archive)
{
    std::filesystem::path marker = archive;
    marker += ".ok";
    return marker;
}

bool trusted_archive(const std::filesystem::path& archive, uint64_t original_size)
{
    if (!usable_archive(archive, original_size)) return false;
    const std::string want = marker_text(archive);
    std::ifstream in(marker_of(archive), std::ios::binary);
    std::string have((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    return !want.empty() && have == want;
}

void write_marker(const std::filesystem::path& archive, const std::filesystem::path& temporary)
{
    std::error_code error;
    const std::string text = marker_text(archive);
    if (text.empty()) return;
    {
        std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
        out << text;
        if (!out) { std::filesystem::remove(temporary, error); return; }
    }
    std::filesystem::rename(temporary, marker_of(archive), error);
    if (error) std::filesystem::remove(temporary, error);
}

// Whether process `pid` is still running (a game whose temporary may be in use).
bool process_alive(long pid)
{
    if (pid <= 0) return false;
    if (pid == (long)racit_getpid()) return true;
#ifdef _WIN32
    HANDLE h = OpenProcess(SYNCHRONIZE, FALSE, (DWORD)pid);
    if (!h) return GetLastError() == ERROR_ACCESS_DENIED;
    const bool running = WaitForSingleObject(h, 0) == WAIT_TIMEOUT;
    CloseHandle(h);
    return running;
#else
    return kill((pid_t)pid, 0) == 0 || errno == EPERM;
#endif
}

// Temporaries ".<archive>.<pid>-<n>.tmp" (and their .ok) left by games that have exited: one
// that couldn't publish its archive used it for its session.
void remove_old_temporaries(const std::filesystem::path& folder)
{
    std::error_code error;
    for (const auto& entry : std::filesystem::directory_iterator(folder, error)) {
        const std::string n = entry.path().filename().string();
        if (n.empty() || n[0] != '.' || n.find(".tmp") == std::string::npos) continue;
        const size_t dash = n.rfind('-', n.find(".tmp"));
        const size_t dot = dash == std::string::npos ? std::string::npos : n.rfind('.', dash);
        if (dot == std::string::npos) continue;
        const long pid = std::strtol(n.c_str() + dot + 1, nullptr, 10);
        if (!process_alive(pid)) std::filesystem::remove(entry.path(), error);
    }
}

std::string prepare(const std::filesystem::path& original)
{
    namespace fs = std::filesystem;
    std::error_code error;
    const uint64_t size = fs::file_size(original, error);
    require(!error, "can't read the original archive");
    const auto stamp = fs::last_write_time(original, error).time_since_epoch().count();
    require(!error, "can't read the original archive");
    const uLong actions_crc = crc32(0, k_menu_actions, sizeof k_menu_actions);
    char name[128];
    std::snprintf(name, sizeof name, "global_cached-%llu-%llx-%08lx.psarc", (unsigned long long)size,
                  (unsigned long long)stamp, (unsigned long)actions_crc);
    const fs::path destination = s_menu_cache / name;
    if (trusted_archive(destination, size)) return destination.string();
    if (fs::exists(destination, error))   // torn, changed or not ours: made again, then renamed over it
        std::printf("[menu-keycaps] %s isn't one this game checked; preparing it again\n", destination.string().c_str());

    fs::create_directories(s_menu_cache, error);
    require(!error, "can't create the menu-keycaps folder");
    remove_old_temporaries(s_menu_cache);
    // One temporary per process and call, so two games starting at once never share it.
    static std::atomic<unsigned> s_temporaries{0};
    char unique[64];
    std::snprintf(unique, sizeof unique, ".%ld-%u.tmp", (long)racit_getpid(), s_temporaries.fetch_add(1));
    const fs::path temporary = s_menu_cache / (std::string(".") + name + unique);
    std::FILE* source = std::fopen(original.string().c_str(), "rb");
    require(source, "can't open the original archive");
    std::FILE* out = nullptr;
    try {
        Archive archive(source);
        const Bytes manifest = archive.read(0);
        std::vector<std::string> names;
        std::string line;
        for (unsigned char c : manifest) {
            if (c == '\n') { names.push_back(line); line.clear(); }
            else if (c != '\r') line += (char)c;
        }
        if (!line.empty()) names.push_back(line);
        require(names.size() + 1 == archive.count, "manifest/member count mismatch");
        std::map<uint32_t, Bytes> replacements;
        for (uint32_t index = 1; index <= names.size(); ++index) {
            const std::string& n = names[index - 1];
            if (n.rfind("/scaleform/", 0) != 0 || n.size() < 4 || n.compare(n.size() - 4, 4, ".swf")) continue;
            Bytes replacement = movie_actions(archive.read(index));
            if (!replacement.empty()) replacements[index] = std::move(replacement);
        }
        require(!replacements.empty(), "no ButtonPrompt menu movies found");
        out = std::fopen(temporary.string().c_str(), "wb");
        require(out, "can't write the menu-keycaps folder");
        archive.write(out, replacements);
        const int closed = std::fclose(out);
        out = nullptr;   // closed even when fclose fails: never close it again
        require(closed == 0, "write failed");
        std::FILE* check = std::fopen(temporary.string().c_str(), "rb");
        require(check, "can't reopen the prepared archive");
        try {
            Archive prepared(check);
            require(prepared.read(0) == manifest, "manifest changed");
            for (const auto& [index, expected] : replacements)
                require(prepared.read(index) == expected, "patched movie readback mismatch");
        } catch (...) { std::fclose(check); throw; }
        std::fclose(check);
        std::fclose(source);
        source = nullptr;
        // Renamed over whatever is at the destination, in one step.
        fs::rename(temporary, destination, error);
        if (error) {
            if (trusted_archive(destination, size)) {   // another game published and checked it first
                fs::remove(temporary, error);
                return destination.string();
            }
            // The checked temporary serves this session; the next start removes it.
            std::printf("[menu-keycaps] can't publish %s (%s); this session uses %s\n", destination.string().c_str(),
                        error.message().c_str(), temporary.string().c_str());
            return temporary.string();
        }
        fs::path marker_temporary = temporary;
        marker_temporary += ".ok";
        write_marker(destination, marker_temporary);
        std::printf("[menu-keycaps] prepared %zu menu movies in %s\n", replacements.size(), destination.string().c_str());
        return destination.string();
    } catch (...) {
        if (out) std::fclose(out);
        if (source) std::fclose(source);
        fs::remove(temporary, error);
        throw;
    }
}

} // namespace

bool racit_menu_archive_init()
{
    const char* configured = std::getenv("RACACIT_MENU_ARCHIVE");
    if (!configured || !*configured) {
        const char* keycaps = std::getenv("RACACIT_MENU_KEYCAPS");
#ifdef __ANDROID__
        const bool wanted = keycaps && !std::strcmp(keycaps, "1");
#else
        const bool wanted = !keycaps || std::strcmp(keycaps, "0");
#endif
        if (!wanted) return true;
        const char* settings = std::getenv("RACACIT_SETTINGS_FILE");
        const char* hdd = std::getenv("PS3_HDD0_ROOT");
        std::filesystem::path base;
        if (settings && *settings) base = std::filesystem::path(settings).parent_path();
        else if (hdd && *hdd) base = std::filesystem::path(hdd).parent_path();
        if (base.empty()) {
            std::printf("[menu-keycaps] no settings file or HDD folder to keep the prepared archive in; off\n");
            return true;
        }
        s_menu_cache = base / "menu-keycaps";
        s_menu_auto = true;
        std::printf("[menu-keycaps] keyboard menus on; the prepared archive lives in %s\n", s_menu_cache.string().c_str());
        return true;
    }
    std::error_code error;
    const auto path = std::filesystem::canonical(configured, error);
    if (error || !std::filesystem::is_regular_file(path, error) || error) {
        std::fprintf(stderr, "Menu archive must be a readable prepared file\n");
        return false;
    }
    if (!usable_archive(path)) {
        std::fprintf(stderr, "Menu archive has an invalid PSARC header\n");
        return false;
    }
    s_menu_archive = path.string();
    s_menu_active = true;
    std::printf("[menu-keycaps] prepared archive %s\n", s_menu_archive.c_str());
    return true;
}

extern "C" int racit_menu_archive_active(void) { return s_menu_active.load(); }

extern "C" void racit_menu_archive_path(char* path, size_t capacity, int read_only)
{
    static const char suffix[] = "/packed/game/global_cached.psarc";
    if (!path || !read_only || (!s_menu_auto && !s_menu_active.load())) return;
    const size_t length = std::strlen(path), tail = sizeof suffix - 1;
    if (length < tail) return;
    for (size_t i = 0; i < tail; ++i) {
        char ch = path[length - tail + i];
        if (ch >= 'A' && ch <= 'Z') ch += 'a' - 'A';
        if (ch == '\\') ch = '/';
        if (ch != suffix[i]) return;
    }
    if (s_menu_auto) {
        // The first open prepares (or finds) the archive; later opens reuse the answer.
        static std::once_flag once;
        std::call_once(once, [path] {
            try {
                s_menu_archive = prepare(path);
                s_menu_active = true;
            } catch (const std::exception& e) {
                std::printf("[menu-keycaps] can't prepare the menu archive (%s); menus keep the controller buttons\n", e.what());
            }
        });
        if (!s_menu_active.load()) return;
    }
    if (s_menu_archive.size() >= capacity) {
        // Make the open fail instead of silently using a truncated filename.
        if (capacity) path[0] = 0;
        return;
    }
    std::memcpy(path, s_menu_archive.c_str(), s_menu_archive.size() + 1);
}
