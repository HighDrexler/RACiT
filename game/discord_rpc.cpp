/*
 * Discord Rich Presence for Into the Nexus (Windows). RACiT's (rac-acit
 * game/discord_rpc.cpp, 2026-11-17) with Into the Nexus's levels: if the Discord app
 * is running, the profile shows "Into the Nexus: Planet Silox", the area and who is
 * playing and the time played. The Settings menu's "Discord status" switch turns it off.
 *
 * The level: a level loading opens its levels/<name>/level_uncached.psarc once
 * (runtime/ppu/ppu_fs.cpp calls nexus_level_file); "lobby" is the main menu.
 *
 * The Application ID: Discord prints the application's name after "Playing". The
 * default is the RACiT application's (so it works with no setup); an application of
 * its own named "Ratchet & Clank: Into the Nexus" is set with RACACIT_DISCORD_APP_ID
 * in settings.txt (no rebuild) or the CMake option. Its art keys: "nexus_logo" and the
 * level keys below; a key the application has no art for shows no picture.
 */
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <ctime>
#include <string>

#include "rsx_host_ui.h"   /* the "Discord status" switch in the Settings menu */

#define RACACIT_STR2(x) #x
#define RACACIT_STR(x) RACACIT_STR2(x)
#ifdef RACACIT_DISCORD_APP_ID
static const char kAppId[] = RACACIT_STR(RACACIT_DISCORD_APP_ID);
#else
static const char kAppId[] = "1564760311637213254";   /* the RACiT app (see above) */
#endif

extern "C" uint8_t* vm_base;

namespace {

enum Who { NOBODY, RATCHET, CLANK, APHELION };

/* Into the Nexus's levels, by their folder under packed/levels (names from the game's
 * own text: core_text_tags, rac-nexus HANDOFF 4v). Index 0 = starting up. */
struct Level { const char* dir; const char* name; const char* area; const char* art; Who who; };
const Level kLevels[] = {
    { "",                 nullptr,                nullptr,               "nexus_logo",       NOBODY  },   /* 0 */
    { "lobby",            "Main menu",            nullptr,               "nexus_logo",       NOBODY  },   /* 1 */
    { "prison_transport", "The Prison Transport", nullptr,               "prison_transport", RATCHET },
    { "meridian_city",    "Planet Igliak",        "Meridian City",       "meridian_city",    RATCHET },
    { "haunted_city",     "Planet Silox",         "Weeblesnog City",     "silox",            RATCHET },
    { "swamp_wasteland",  "Planet Thram",         "Hagrow Swampland",    "thram",            RATCHET },
    { "battle_arena",     "Planet Kragg",         "Destructapalooza",    "kragg",            RATCHET },
    { "vendra_homeworld", "Planet Yerek",         nullptr,               "yerek",            RATCHET },
    { "clank_puzzles",    "The Netherverse",      nullptr,               "netherverse",      CLANK   },
};
const int kNumLevels = (int)(sizeof kLevels / sizeof kLevels[0]);
const int kMenu = 1;

struct Badge { const char* art; const char* name; };
const Badge kBadges[] = { { nullptr, nullptr }, { "ratchet", "Ratchet" }, { "clank", "Clank" },
                          { "aphelion", "Aphelion" } };

volatile int g_level = 0;   /* nexus_level_file */

uint32_t current_level() { return (uint32_t)g_level; }

void json_str(std::string& out, const char* s)
{
    out += '"';
    for (; *s; s++) {
        const unsigned char c = (unsigned char)*s;
        if (c == '"' || c == '\\') { out += '\\'; out += (char)c; }
        else if (c >= 0x20) out += (char)c;
    }
    out += '"';
}

}  // namespace

/* Code mods' level id (stubs.cpp racit_current_level): this table's index. */
uint32_t nexus_level_index() { return current_level(); }

/* ppu_fs.cpp: a level's level_uncached.psarc was opened (".../packed/levels/<dir>/..."). */
void nexus_level_file(const char* path)
{
    const char* p = strstr(path, "/levels/");
    if (!p) return;
    p += 8;
    for (int i = 1; i < kNumLevels; i++) {
        const size_t n = strlen(kLevels[i].dir);
        if (!strncmp(p, kLevels[i].dir, n) && p[n] == '/') {
            if (g_level != i) fprintf(stderr, "[discord] level: %s%s%s%c", kLevels[i].name, kLevels[i].area ? ", " : "", kLevels[i].area ? kLevels[i].area : "", 10);
            g_level = i; return; }
    }
}

#ifdef _WIN32
#include <windows.h>

namespace {

HANDLE g_pipe = INVALID_HANDLE_VALUE;
const char* g_app_id = kAppId;
unsigned g_nonce;

bool pipe_write(uint32_t op, const std::string& json)
{
    std::string frame(8 + json.size(), '\0');
    const uint32_t len = (uint32_t)json.size();
    memcpy(&frame[0], &op, 4);          /* little-endian header: opcode, length */
    memcpy(&frame[4], &len, 4);
    memcpy(&frame[8], json.data(), json.size());
    DWORD done = 0;
    return WriteFile(g_pipe, frame.data(), (DWORD)frame.size(), &done, nullptr) && done == frame.size();
}

/* One whole frame, waiting up to `wait_ms` for it to start arriving. */
bool pipe_read(DWORD wait_ms, std::string& out)
{
    const ULONGLONG until = GetTickCount64() + wait_ms;
    DWORD avail = 0;
    while (PeekNamedPipe(g_pipe, nullptr, 0, nullptr, &avail, nullptr) && avail < 8) {
        if (GetTickCount64() >= until) return false;
        Sleep(20);
    }
    if (avail < 8) return false;
    uint32_t hdr[2]; DWORD n = 0;
    if (!ReadFile(g_pipe, hdr, 8, &n, nullptr) || n != 8 || hdr[1] > (1u << 20)) return false;
    out.assign(hdr[1], '\0');
    for (DWORD got = 0; got < hdr[1]; got += n)
        if (!ReadFile(g_pipe, &out[got], hdr[1] - got, &n, nullptr) || !n) return false;
    return true;
}

/* Discard replies; false once the pipe is gone (Discord closed). */
bool pipe_alive()
{
    for (;;) {
        DWORD avail = 0;
        if (!PeekNamedPipe(g_pipe, nullptr, 0, nullptr, &avail, nullptr)) return false;
        if (!avail) return true;
        std::string junk;
        if (!pipe_read(0, junk)) return false;
    }
}

void disconnect()
{
    if (g_pipe != INVALID_HANDLE_VALUE) CloseHandle(g_pipe);
    g_pipe = INVALID_HANDLE_VALUE;
}

bool connect()
{
    for (int i = 0; i < 10; i++) {
        char name[64];
        snprintf(name, sizeof name, "\\\\.\\pipe\\discord-ipc-%d", i);
        HANDLE h = CreateFileA(name, GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
        if (h == INVALID_HANDLE_VALUE) continue;
        g_pipe = h;
        std::string hello = "{\"v\":1,\"client_id\":";
        json_str(hello, g_app_id);
        hello += '}';
        std::string reply;
        if (pipe_write(0, hello) && pipe_read(3000, reply)) {
            if (reply.find("\"READY\"") != std::string::npos) {
                fprintf(stderr, "[discord] connected (discord-ipc-%d)%c", i, 10);
                return true;
            }
            static int told;
            if (!told++) fprintf(stderr, "[discord] the Discord app refused the connection: %.200s%c", reply.c_str(), 10);
        }
        disconnect();
    }
    return false;
}

/* `level` < 0: clear the status. */
bool set_activity(int level, bool movie, long long start)
{
    std::string j = "{\"cmd\":\"SET_ACTIVITY\",\"nonce\":\"" + std::to_string(++g_nonce) +
                    "\",\"args\":{\"pid\":" + std::to_string(GetCurrentProcessId());
    if (level < 0) {
        j += "}}";
        return pipe_write(1, j);
    }
    const Level& L = kLevels[level];
    /* The game's name in the lines the game controls: Discord's top line is the
     * application's name (RACiT's app by default), e.g.
     *   Into the Nexus: Planet Silox
     *   Weeblesnog City - Playing as Ratchet */
    const std::string details = std::string("Into the Nexus: ") + (level ? L.name : "Starting up");
    const char* who = level == kMenu ? "In the menus" : !level ? "Loading"
                    : L.who == CLANK ? "Playing as Clank" : "Playing as Ratchet";
    const std::string state = L.area && level != kMenu ? std::string(L.area) + " - " + who : std::string(who);
    j += ",\"activity\":{\"details\":"; json_str(j, details.c_str());
    j += ",\"state\":"; json_str(j, state.c_str());
    j += ",\"timestamps\":{\"start\":" + std::to_string(start) + "}";
    j += ",\"assets\":{\"large_image\":"; json_str(j, L.art);
    j += ",\"large_text\":"; json_str(j, "Ratchet & Clank: Into the Nexus");
    if (L.who != NOBODY && !movie) {
        j += ",\"small_image\":"; json_str(j, kBadges[L.who].art);
        j += ",\"small_text\":"; json_str(j, kBadges[L.who].name);
    }
    j += "}}}}";
    return pipe_write(1, j);
}

DWORD WINAPI presence_thread(LPVOID)
{
    const long long session_start = (long long)time(nullptr);
    ULONGLONG next_try = 0;
    int shown_level = -2; int shown_movie = -1; int shown_on = 1;
    for (;;) {
        Sleep(2000);
        rsx_host_options o; rsx_host_ui_options(&o);
        const int movie = 0;   /* Into the Nexus's movie player is not hooked */
        const int level = (int)current_level();

        if (g_pipe == INVALID_HANDLE_VALUE) {
            if (!o.discord || GetTickCount64() < next_try) continue;
            next_try = GetTickCount64() + 10000;
            if (!connect()) continue;
            shown_level = -2;                     /* a new connection: show it afresh */
        }
        if (!pipe_alive()) { disconnect(); continue; }
        if (!o.discord) {                         /* switched off in the menu */
            if (shown_on) { set_activity(-1, false, 0); shown_on = 0; }
            disconnect();
            continue;
        }
        shown_on = 1;
        if (level == shown_level && movie == shown_movie) continue;
        if (!set_activity(level, movie != 0, session_start)) { disconnect(); continue; }
        shown_level = level; shown_movie = movie;
    }
    return 0;
}

}  // namespace

extern "C" void racacit_discord_start(void)
{
    /* Developers can try another application without rebuilding. */
    const char* id = getenv("RACACIT_DISCORD_APP_ID");
    if (id && *id) g_app_id = id;
    if (!g_app_id[0]) {
        fprintf(stderr, "[discord] this build has no Discord Application ID -- Rich Presence off%c", 10);
        return;
    }
    for (const char* p = g_app_id; *p; p++)
        if (*p < '0' || *p > '9') { fprintf(stderr, "[discord] the Application ID must be a number%c", 10); return; }
    CreateThread(nullptr, 0, presence_thread, nullptr, 0, nullptr);
}

#else

extern "C" void racacit_discord_start(void) {}

#endif
