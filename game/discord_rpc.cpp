/*
 * Discord Rich Presence (Windows).
 *
 * Works the way it does for any PC game: if the Discord app is running and
 * signed in, the player's profile shows "Playing Ratchet & Clank: A Crack in
 * Time" with the level, what Ratchet or Clank is doing, the level's artwork
 * and the time played. Nothing to set up and nothing in settings.txt; the
 * Settings menu (F1 / Esc) has an on/off switch. When Discord is not running
 * the game checks again every 10 s, so starting Discord later also works.
 *
 * How it talks to Discord: the Discord client's local IPC pipe
 * (\\.\pipe\discord-ipc-N), the same channel Discord's own SDKs use. Nothing
 * leaves this PC except through the Discord app itself.
 *
 * Discord identifies the game by an Application ID registered once, by the
 * people shipping the game, at https://discord.com/developers/applications;
 * every copy of the game uses that same ID (it is public, not a secret).
 * kAppId below is that ID -- or set RACACIT_DISCORD_APP_ID when configuring
 * the build. The application's name is what Discord prints after
 * "Playing", and the artwork is uploaded there (Rich Presence -> Art Assets)
 * under the keys in kLevels / kBadges below, plus "logo".
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
static const char kAppId[] = "1564760311637213254";   /* the RACiT app (A Crack in Time) */
#endif

extern "C" uint8_t* vm_base;
extern "C" volatile unsigned g_racacit_movie_frames;   /* game/stubs.cpp: Bink frames played */

namespace {

enum Who { NOBODY, RATCHET, CLANK, APHELION };

/* By the level ids of the title's table at 0xDE1820. */
struct Level { const char* name; const char* art; Who who; };
const Level kLevels[] = {
    { nullptr,                  "logo",               NOBODY   },   /*  0 */
    { "The Great Clock",        "great_clock",        CLANK    },   /*  1 great_clock_a */
    { "Zolar Forest",           "zolar_forest",       RATCHET  },   /*  2 */
    { "Space",                  "space",              APHELION },   /*  3 space_sector_1 */
    { "Vorselon's Ship",        "vorselons_ship",     RATCHET  },   /*  4 galacton_ship */
    { "The Great Clock",        "great_clock",        CLANK    },   /*  5 great_clock_b */
    { "Space",                  "space",              APHELION },   /*  6 */
    { "Molonoth Fields",        "molonoth_fields",    RATCHET  },   /*  7 */
    { "Axiom City",             "axiom_city",         RATCHET  },   /*  8 */
    { "The Great Clock",        "great_clock",        CLANK    },   /*  9 great_clock_c */
    { "Space",                  "space",              APHELION },   /* 10 */
    { "Krell Canyon",           "krell_canyon",       RATCHET  },   /* 11 */
    { "Agorian Battleplex",     "agorian_battleplex", RATCHET  },   /* 12 */
    { "Tombli Outpost",         "tombli_outpost",     RATCHET  },   /* 13 */
    { "The Great Clock",        "great_clock",        CLANK    },   /* 14 great_clock_d */
    { "Space",                  "space",              APHELION },   /* 15 */
    { "Valkyrie Fleet",         "valkyrie_fleet",     RATCHET  },   /* 16 */
    { "Nefarious Station",      "nefarious_station",  RATCHET  },   /* 17 */
    { "Space",                  "space",              APHELION },   /* 18 */
    { "Gimlick Valley",         "gimlick_valley",     RATCHET  },   /* 19 */
    { "The Great Clock",        "great_clock",        CLANK    },   /* 20 great_clock_e */
    { "Insomniac Museum",       "insomniac_museum",   RATCHET  },   /* 21 */
    { "Main menu",              "logo",               NOBODY   },   /* 22 front_end */
};
const int kNumLevels = (int)(sizeof kLevels / sizeof kLevels[0]);

struct Badge { const char* art; const char* name; };
const Badge kBadges[] = { { nullptr, nullptr }, { "ratchet", "Ratchet" }, { "clank", "Clank" },
                          { "aphelion", "Aphelion" } };

/* The current level id: the global func_0029A4F4 stores it in. */
uint32_t current_level()
{
    if (!vm_base) return 0;
    const uint8_t* p = vm_base + 0xEF7C90u;
    const uint32_t v = ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
    return (int)v < kNumLevels && kLevels[v].name ? v : 0;
}

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
    const char* details = level ? L.name : "Starting up";
    const char* state = movie ? "Watching a cutscene"
                      : level == 22 ? "In the menus"
                      : !level ? "Loading"
                      : L.who == CLANK ? "Playing as Clank"
                      : L.who == APHELION ? "Flying the Aphelion" : "Playing as Ratchet";
    j += ",\"activity\":{\"details\":"; json_str(j, details);
    j += ",\"state\":"; json_str(j, state);
    j += ",\"timestamps\":{\"start\":" + std::to_string(start) + "}";
    j += ",\"assets\":{\"large_image\":"; json_str(j, L.art);
    j += ",\"large_text\":"; json_str(j, level && level != 22 ? L.name : "Ratchet & Clank Future: A Crack in Time");
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
    unsigned last_frames = g_racacit_movie_frames;
    for (;;) {
        Sleep(2000);
        rsx_host_options o; rsx_host_ui_options(&o);
        const unsigned frames = g_racacit_movie_frames;
        const int movie = frames != last_frames;
        last_frames = frames;
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
