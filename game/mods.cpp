/*
 * Code mods: the DLLs of the enabled mods, loaded before the game's first
 * instruction, and the hooks they put on recompiled game functions.
 * The API mods see is game/racit_mod.h; how to write one, docs/MODDING.md.
 *
 * The launcher lists them in RACACIT_CODE_MODS, highest priority first:
 *     id|C:/.../mods/id/my_mod.dll;id2|...
 * (RACACIT_CODE_MODS_OFF=1, the launcher's safe mode, loads none.)
 *
 * Hooks. Every lifted function is built with -fpatchable-function-entry=16,14
 * (CMakeLists.txt): 14 bytes of nop before its entry and a 2-byte nop at it,
 * 16-aligned from the first pad byte. A hook turns them into
 *     pad+0   dq   target            (8-aligned: retargeted by one atomic store)
 *     pad+8   jmp  [rip-14]          (FF 25 F2 FF FF FF)
 *     entry   jmp  short pad+8       (EB F8)
 * and the code after the entry's 2 bytes is the game's own function, which is
 * what the innermost hook calls as its original. Chains: the newest hook is
 * the target; it is handed the previous target as its original.
 */
#ifdef _WIN32
#include <windows.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <vector>
#include <mutex>
#include <unordered_map>
#include <cstddef>

#include "ppu_recomp.h"
#include "racit_mod.h"

extern uint8_t* vm_base;
uint32_t racit_current_level();   /* discord_rpc.cpp */

static_assert(offsetof(racit_ppu, fpr) == offsetof(ppu_context, fpr), "racit_ppu layout");
static_assert(offsetof(racit_ppu, vr)  == offsetof(ppu_context, vr),  "racit_ppu layout");
static_assert(offsetof(racit_ppu, cr)  == offsetof(ppu_context, cr),  "racit_ppu layout");
static_assert(offsetof(racit_ppu, lr)  == offsetof(ppu_context, lr),  "racit_ppu layout");
static_assert(offsetof(racit_ppu, cia) == offsetof(ppu_context, cia), "racit_ppu layout");

struct racit_mod {
    std::string id, dir, dll;
    HMODULE h = NULL;
    bool alive = false;
};

namespace {

struct Hook { racit_mod* mod; racit_func fn; racit_func prev; };
struct Site { uint8_t* entry; std::vector<Hook> chain; };   /* chain[0] innermost */

std::mutex g_mu;
std::vector<racit_mod*> g_mods;               /* load order: lowest priority first */
std::unordered_map<uint32_t, Site> g_sites;   /* guest address -> its patch site */
std::unordered_map<uint32_t, racit_func> g_funcs;
struct FrameCb { racit_mod* m; void (*cb)(uint32_t, void*); void* user; };
struct LevelCb { racit_mod* m; void (*cb)(uint32_t, uint32_t, void*); void* user; };
std::vector<FrameCb> g_frame_cbs;
std::vector<LevelCb> g_level_cbs;
uint32_t g_frame = 0, g_level = 0xFFFFFFFFu;
std::string g_game_version = "Into the Nexus";   /* RACIT_GAME_VERSION (the launcher) overrides */

void build_func_map()
{
    if (!g_funcs.empty()) return;
    g_funcs.reserve((size_t)function_table_count);
    for (uint64_t i = 0; i < function_table_count; i++)
        g_funcs.emplace((uint32_t)function_table[i].addr, (racit_func)function_table[i].func);
}

bool is_nop_pad(const uint8_t* e)
{
    for (int i = -14; i < 0; i++) if (e[i] != 0x90) return false;
    return e[0] == 0x66 && e[1] == 0x90;
}
bool is_our_patch(const uint8_t* e)
{
    static const uint8_t jmp[6] = { 0xFF, 0x25, 0xF2, 0xFF, 0xFF, 0xFF };
    return memcmp(e - 6, jmp, 6) == 0 && e[0] == 0xEB && e[1] == 0xF8;
}

/* Point the site at `target`, or (target == NULL) put the original nops back. */
void write_site(uint8_t* e, void* target)
{
    uint8_t* pad = e - 14;
    DWORD old;
    VirtualProtect(pad, 16, PAGE_EXECUTE_READWRITE, &old);
    if (target) {
        if (!is_our_patch(e)) {
            *(void* volatile*)pad = target;
            static const uint8_t jmp[6] = { 0xFF, 0x25, 0xF2, 0xFF, 0xFF, 0xFF };
            memcpy(pad + 8, jmp, 6);
            *(volatile uint16_t*)e = 0xF8EB;   /* EB F8: the last write, one aligned store */
        } else {
            *(void* volatile*)pad = target;    /* retarget a live chain atomically */
        }
    } else {
        *(volatile uint16_t*)e = 0x9066;       /* 66 90 first: nothing enters the pad after this */
        memset(pad, 0x90, 14);
    }
    VirtualProtect(pad, 16, old, &old);
    FlushInstructionCache(GetCurrentProcess(), pad, 16);
}

void mlog(racit_mod* m, const char* fmt, va_list ap)
{
    char buf[1024];
    vsnprintf(buf, sizeof buf, fmt, ap);
    size_t n = strlen(buf);
    while (n && (buf[n - 1] == '\n' || buf[n - 1] == '\r')) buf[--n] = 0;
    fprintf(stderr, "[mod %s] %s%c", m ? m->id.c_str() : "?", buf, 10);
}
void glog(const char* fmt, ...)
{
    va_list ap; va_start(ap, fmt);
    char buf[1024]; vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    fprintf(stderr, "[mods] %s%c", buf, 10);
}

/* ---- the API --------------------------------------------------------------- */
void api_log(racit_mod* self, const char* fmt, ...) { va_list ap; va_start(ap, fmt); mlog(self, fmt, ap); va_end(ap); }
const char* api_mod_dir(racit_mod* self) { return self ? self->dir.c_str() : ""; }
const char* api_mod_id(racit_mod* self) { return self ? self->id.c_str() : ""; }
const char* api_setting(racit_mod*, const char* name) { return name ? getenv(name) : NULL; }

racit_func api_find_function(uint32_t a)
{
    auto it = g_funcs.find(a);
    if (it == g_funcs.end()) return NULL;
    return (racit_func)((uint8_t*)(void*)it->second + 2);   /* past the entry's 2-byte nop/patch */
}

int api_hook(racit_mod* self, uint32_t a, racit_func fn, racit_func* original)
{
    if (!self || !fn) return -1;
    std::lock_guard<std::mutex> lk(g_mu);
    auto f = g_funcs.find(a);
    if (f == g_funcs.end()) return -1;
    uint8_t* e = (uint8_t*)(void*)f->second;
    Site& s = g_sites[a];
    if (!s.entry) {
        if (!is_nop_pad(e)) { g_sites.erase(a); glog("%s: 0x%08X has no hook pad -- not hookable in this build", self->id.c_str(), a); return -2; }
        s.entry = e;
    }
    for (auto& h : s.chain) if (h.fn == fn) return -3;
    const racit_func prev = s.chain.empty() ? (racit_func)(void*)(e + 2) : s.chain.back().fn;
    s.chain.push_back({ self, fn, prev });
    if (original) *original = prev;   /* before the site goes live: the hook may run at once */
    write_site(e, (void*)fn);
    glog("%s hooks 0x%08X%s", self->id.c_str(), a, s.chain.size() > 1 ? " (chained)" : "");
    return 0;
}

/* Remove one hook; the hook above it (if any) gets its predecessor as original.
 * A mod that keeps `original` in a static is told about the change only through
 * a re-hook, so removal in the middle of a chain is allowed only at the top or
 * for a mod's own whole set during a failed init (both safe: nothing outside
 * holds the removed hook as its original). */
int unhook_locked(racit_mod* self, uint32_t a, racit_func fn)
{
    auto it = g_sites.find(a);
    if (it == g_sites.end()) return -1;
    Site& s = it->second;
    for (size_t i = 0; i < s.chain.size(); i++) {
        if (s.chain[i].fn != fn || s.chain[i].mod != self) continue;
        if (i + 1 != s.chain.size()) {
            glog("%s: unhook of 0x%08X refused -- another mod's hook wraps it", self->id.c_str(), a);
            return -2;
        }
        s.chain.pop_back();
        if (s.chain.empty()) { write_site(s.entry, NULL); g_sites.erase(it); }
        else write_site(s.entry, (void*)s.chain.back().fn);
        return 0;
    }
    return -1;
}
int api_unhook(racit_mod* self, uint32_t a, racit_func fn)
{
    std::lock_guard<std::mutex> lk(g_mu);
    return unhook_locked(self, a, fn);
}

int api_guest_valid(uint32_t addr, uint32_t len)
{
    if (!vm_base || !len) return 0;
    uint64_t a = addr, end = (uint64_t)addr + len;
    if (end > 0x100000000ull) return 0;
    while (a < end) {
        MEMORY_BASIC_INFORMATION mbi;
        if (!VirtualQuery(vm_base + a, &mbi, sizeof mbi)) return 0;
        if (mbi.State != MEM_COMMIT || (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD))) return 0;
        a = (uint64_t)((uint8_t*)mbi.BaseAddress + mbi.RegionSize - vm_base);
    }
    return 1;
}

int api_on_frame(racit_mod* self, void (*cb)(uint32_t, void*), void* user)
{
    if (!cb) return -1;
    std::lock_guard<std::mutex> lk(g_mu);
    g_frame_cbs.push_back({ self, cb, user });
    return 0;
}
int api_on_level(racit_mod* self, void (*cb)(uint32_t, uint32_t, void*), void* user)
{
    if (!cb) return -1;
    std::lock_guard<std::mutex> lk(g_mu);
    g_level_cbs.push_back({ self, cb, user });
    return 0;
}
uint32_t api_current_level(void) { return racit_current_level(); }

racit_api g_api;

void drop_mod_locked(racit_mod* m)
{
    /* Its hooks, newest first: during its own init nothing has wrapped them. */
    for (int pass = 0; pass < 64; pass++) {
        bool any = false;
        for (auto& kv : g_sites)
            for (size_t i = kv.second.chain.size(); i-- > 0; )
                if (kv.second.chain[i].mod == m) {
                    const uint32_t a = kv.first; const racit_func fn = kv.second.chain[i].fn;
                    if (unhook_locked(m, a, fn) == 0) { any = true; goto again; }
                }
        again:
        if (!any) break;
    }
    for (size_t i = g_frame_cbs.size(); i-- > 0; ) if (g_frame_cbs[i].m == m) g_frame_cbs.erase(g_frame_cbs.begin() + i);
    for (size_t i = g_level_cbs.size(); i-- > 0; ) if (g_level_cbs[i].m == m) g_level_cbs.erase(g_level_cbs.begin() + i);
}

/* The environment as UTF-8 (getenv is the ANSI code page: a mod path with any
 * other character would not load). */
std::string env_utf8(const wchar_t* name)
{
    DWORD n = GetEnvironmentVariableW(name, NULL, 0);
    if (!n) return std::string();
    std::wstring w(n, L' ');
    n = GetEnvironmentVariableW(name, &w[0], n);
    w.resize(n);
    int k = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), NULL, 0, NULL, NULL);
    std::string o(k > 0 ? k : 0, ' ');
    if (k > 0) WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), &o[0], k, NULL, NULL);
    return o;
}

std::wstring widen(const std::string& s)
{
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, NULL, 0);
    std::wstring w(n > 0 ? n - 1 : 0, L'\0');
    if (n > 1) MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, &w[0], n);
    return w;
}

} // namespace

/* main(): after the game's image is in memory, before ppu_run. */
extern "C" void racit_mods_load(void)
{
    const char* off = getenv("RACACIT_CODE_MODS_OFF");
    const std::string list = env_utf8(L"RACACIT_CODE_MODS");
    if (list.empty()) return;
    if (off && *off && *off != '0') { glog("safe mode: code mods not loaded"); return; }
    if (const char* v = getenv("RACIT_GAME_VERSION")) g_game_version = v;

    build_func_map();
    g_api.api_version = RACIT_API_VERSION;
    g_api.size = sizeof g_api;
    g_api.game_version = g_game_version.c_str();
    g_api.log = api_log; g_api.mod_dir = api_mod_dir; g_api.mod_id = api_mod_id; g_api.setting = api_setting;
    g_api.hook = api_hook; g_api.unhook = api_unhook; g_api.find_function = api_find_function;
    g_api.guest_base = vm_base; g_api.guest_valid = api_guest_valid;
    g_api.on_frame = api_on_frame; g_api.on_level = api_on_level; g_api.current_level = api_current_level;

    /* Highest priority first in the list; load the other way round. */
    std::vector<racit_mod*> order;
    const std::string& s = list;
    for (size_t i = 0; i < s.size(); ) {
        size_t j = s.find(';', i); if (j == std::string::npos) j = s.size();
        const std::string item = s.substr(i, j - i);
        const size_t bar = item.find('|');
        if (bar != std::string::npos && bar + 1 < item.size()) {
            racit_mod* m = new racit_mod;
            m->id = item.substr(0, bar); m->dll = item.substr(bar + 1);
            const size_t sl = m->dll.find_last_of("/\\");
            m->dir = sl == std::string::npos ? std::string(".") : m->dll.substr(0, sl);
            order.insert(order.begin(), m);
        }
        i = j + 1;
    }

    for (racit_mod* m : order) {
        /* The mod's folder is searched for its own dependencies. */
        m->h = LoadLibraryExW(widen(m->dll).c_str(), NULL, LOAD_WITH_ALTERED_SEARCH_PATH);
        if (!m->h) { glog("%s: could not load %s (error %lu)", m->id.c_str(), m->dll.c_str(), GetLastError()); delete m; continue; }
        typedef uint32_t (*VerFn)(void);
        typedef int (*InitFn)(const racit_api*, racit_mod*);
        VerFn ver = (VerFn)(void*)GetProcAddress(m->h, "racit_mod_api_version");
        InitFn init = (InitFn)(void*)GetProcAddress(m->h, "racit_mod_init");
        if (!ver || !init) {
            glog("%s: %s exports no racit_mod_api_version / racit_mod_init -- not a RACiT mod", m->id.c_str(), m->dll.c_str());
            FreeLibrary(m->h); delete m; continue;
        }
        const uint32_t want = ver();
        if (want > RACIT_API_VERSION || want == 0) {
            glog("%s: needs mod API %u, this game has %u -- update the game", m->id.c_str(), want, RACIT_API_VERSION);
            FreeLibrary(m->h); delete m; continue;
        }
        {
            std::lock_guard<std::mutex> lk(g_mu);
            g_mods.push_back(m);
        }
        glog("%s: loading %s (API %u)", m->id.c_str(), m->dll.c_str(), want);
        const int rc = init(&g_api, m);
        if (rc != 0) {
            glog("%s: init returned %d -- unloaded", m->id.c_str(), rc);
            std::lock_guard<std::mutex> lk(g_mu);
            drop_mod_locked(m);
            g_mods.pop_back();
            FreeLibrary(m->h); delete m; continue;
        }
        m->alive = true;
    }
    if (!g_mods.empty()) glog("%u code mod(s) loaded", (unsigned)g_mods.size());
}

/* The frame thread, once per presented frame. */
extern "C" void racit_mods_frame(void)
{
    if (g_mods.empty()) return;
    std::vector<FrameCb> f; std::vector<LevelCb> l;
    {
        std::lock_guard<std::mutex> lk(g_mu);
        f = g_frame_cbs; l = g_level_cbs;
    }
    const uint32_t frame = ++g_frame;
    for (auto& c : f) c.cb(frame, c.user);
    if (!l.empty()) {
        const uint32_t lv = racit_current_level();
        if (g_level == 0xFFFFFFFFu) g_level = lv;   /* the first frame: a start, not a change */
        if (lv != g_level) {
            const uint32_t old = g_level;
            g_level = lv;
            for (auto& c : l) c.cb(old, lv, c.user);
        }
    }
}

/* The crash report: which code mods were in, and the hooks they held. */
extern "C" int racit_mods_crash_note(char* buf, int cap)
{
    if (g_mods.empty() || cap <= 0) return 0;
    int n = snprintf(buf, cap, "\n[CRASH] code mods:");
    for (racit_mod* m : g_mods) {
        if (n >= cap) break;
        n += snprintf(buf + n, cap - n, " %s", m->id.c_str());
        int k = 0;
        for (auto& kv : g_sites) for (auto& h : kv.second.chain)
            if (h.mod == m && n < cap) n += snprintf(buf + n, cap - n, "%s0x%08X", k++ ? "," : " (hooks ", kv.first);
        if (k && n < cap) n += snprintf(buf + n, cap - n, ")");
    }
    return n < cap ? n : cap - 1;
}
#else
extern "C" void racit_mods_load(void) {}
extern "C" void racit_mods_frame(void) {}
extern "C" int racit_mods_crash_note(char*, int) { return 0; }
#endif
