/* Select the separately prepared menu archive for read-only game opens. */
#include <array>
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>

static std::string s_menu_archive;

bool racit_menu_archive_init()
{
    const char* configured = std::getenv("RACACIT_MENU_ARCHIVE");
    if (!configured || !*configured) return true;
    std::error_code error;
    const auto path = std::filesystem::canonical(configured, error);
    if (error || !std::filesystem::is_regular_file(path, error) || error) {
        std::fprintf(stderr, "Menu archive must be a readable prepared file\n");
        return false;
    }
    std::ifstream input(path, std::ios::binary);
    std::array<unsigned char, 32> header{};
    input.read(reinterpret_cast<char*>(header.data()), header.size());
    const auto word = [&header](size_t offset) {
        unsigned value = 0;
        for (size_t i = offset; i < offset + 4; ++i) value = (value << 8) | header[i];
        return value;
    };
    const auto length = std::filesystem::file_size(path, error);
    const unsigned toc = word(12), entry = word(16), count = word(20), block = word(24);
    if (!input || error || std::memcmp(header.data(), "PSAR", 4) ||
        std::memcmp(header.data() + 8, "zlib", 4) || toc < 32 || toc > length ||
        entry < 30 || !count || uint64_t(entry) * count > toc - 32 ||
        !block || block > 1024 * 1024) {
        std::fprintf(stderr, "Menu archive has an invalid PSARC header\n");
        return false;
    }
    s_menu_archive = path.string();
    std::printf("[menu-keycaps] prepared archive %s\n", s_menu_archive.c_str());
    return true;
}

extern "C" void racit_menu_archive_path(char* path, size_t capacity, int read_only)
{
    static const char suffix[] = "/packed/game/global_cached.psarc";
    if (!path || !read_only || s_menu_archive.empty()) return;
    const size_t length = std::strlen(path), tail = sizeof suffix - 1;
    if (length < tail) return;
    for (size_t i = 0; i < tail; ++i) {
        char ch = path[length - tail + i];
        if (ch >= 'A' && ch <= 'Z') ch += 'a' - 'A';
        if (ch == '\\') ch = '/';
        if (ch != suffix[i]) return;
    }
    if (s_menu_archive.size() >= capacity) {
        // Make the open fail instead of silently using a truncated filename.
        if (capacity) path[0] = 0;
        return;
    }
    std::memcpy(path, s_menu_archive.c_str(), s_menu_archive.size() + 1);
}
