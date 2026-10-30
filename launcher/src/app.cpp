// RaCACiT Launcher (Windows): imports the player's own copy of A Crack in Time,
// manages settings, saves, mods and builds, and starts the game. The Windows
// counterpart of the Android launcher (android/), in the same look.
#include <windows.h>
#include <windowsx.h>
#include <shellapi.h>
#include <shlobj.h>
#include <SDL2/SDL.h>

#include <algorithm>
#include <cmath>
#include <ctime>
#include <functional>
#include <thread>
#include <vector>

#include "gfx.h"
#include "store.h"

static const char* kLauncherVersion = "0.9";
enum { WM_APP_TASK = WM_APP + 1, WM_APP_GAME_EXIT, WM_APP_UPDATE, TIMER_TICK = 1 };

// ---- screen model ------------------------------------------------------------------------
enum Kind { K_BUTTON, K_OPTION, K_HEADER, K_FIELD };
struct Item {
    Kind kind = K_BUTTON;
    std::string label, sub;                       // K_OPTION: sub = the value; K_FIELD: sub = the text
    std::function<void()> act;
    std::function<void(int)> change;              // K_OPTION: -1 / +1
    std::function<void(const std::string&)> commit;   // K_FIELD
    bool enabled = true, accent = false;
    Rect rc{};
};
struct Info { std::string label, value; uint32_t color = col::TEXT; };
struct Screen {
    std::string title, subtitle;
    std::vector<Info> info;
    std::vector<Item> items;
    std::function<void()> back;
    std::function<void()> build;
};
struct Overlay {
    enum Type { NONE, MESSAGE, CONFIRM, PROGRESS, CAPTURE } type = NONE;
    std::string title, text, yes = "OK", no = "Cancel";
    std::function<void()> on_yes;
    std::function<void(const std::string&)> on_input;   // CAPTURE: the key or mouse input's name
    ULONGLONG deadline = 0;                              // CAPTURE: cancelled then
    int focus = 0;
    Rect btn[2]{};
};

static HWND g_hwnd;
static Gfx g;
static Store* S;
static Screen scr;
static int focus = 0;
static float scroll = 0, scroll_to = 0;
static Overlay ov;
static Progress* prog = nullptr;
static std::thread worker;
static std::function<void(const std::string&)> task_done;
static std::string task_title;
static bool editing = false;
static std::string edit_text;
static HANDLE game_proc = nullptr;
static ULONGLONG game_start = 0;
static std::string last_session;
static int hover = -1;
static ULONGLONG t0 = GetTickCount64();
static bool pad_ps = true;
static SDL_GameController* pad = nullptr;
static bool sdl_ok = false;
static int pad_dir = 0; static ULONGLONG pad_next = 0;
static bool s_swallow_up = false;   // a mouse button that was captured as a binding: its release is not a click

static void redraw() { InvalidateRect(g_hwnd, nullptr, FALSE); }
static bool focusable(const Item& i) { return i.kind != K_HEADER; }

static void set_screen(std::function<void()> build)
{
    editing = false;
    build();
    scr.build = build;
    focus = 0;
    while (focus < (int)scr.items.size() && !focusable(scr.items[focus])) focus++;
    for (int i = 0; i < (int)scr.items.size(); i++) if (scr.items[i].accent && focusable(scr.items[i])) { focus = i; break; }   // the next step
    scroll = scroll_to = 0;
    redraw();
}

static void refresh()
{
    if (!scr.build) return;
    const int f = focus;
    auto build = scr.build;   // the builder resets scr (and so scr.build): run a copy, then put it back
    build();
    scr.build = build;
    focus = std::min(f, (int)scr.items.size() - 1);
    while (focus > 0 && !focusable(scr.items[focus])) focus--;
    redraw();
}

static void message(const std::string& title, const std::string& text)
{
    ov = Overlay(); ov.type = Overlay::MESSAGE; ov.title = title; ov.text = text; ov.yes = "OK";
    redraw();
}

static void confirm(const std::string& title, const std::string& text, const std::string& yes, std::function<void()> fn)
{
    ov = Overlay(); ov.type = Overlay::CONFIRM; ov.title = title; ov.text = text; ov.yes = yes; ov.no = "Cancel"; ov.on_yes = std::move(fn);
    ov.focus = 1;   // Cancel first: nothing happens by accident
    redraw();
}

// A job off the UI thread with the progress overlay; `done` runs back on the UI thread.
// Waits for one key, mouse button or wheel turn (wndproc) and hands its name, as the
// game's bindings spell it (SDL key names, Mouse1-5, WheelUp/WheelDown), to fn. A
// controller's Circle / B, or 6 seconds, cancels: every key can be a binding.
static void capture_input(const std::string& title, const std::string& text, std::function<void(const std::string&)> fn)
{
    ov = Overlay(); ov.type = Overlay::CAPTURE; ov.title = title; ov.text = text; ov.on_input = std::move(fn);
    ov.deadline = GetTickCount64() + 6000;
    redraw();
}
static void capture_done(const std::string& name)
{
    if (ov.type != Overlay::CAPTURE || name.empty()) return;
    auto fn = ov.on_input;
    ov.type = Overlay::NONE; redraw();
    if (fn) fn(name);
}

// A key's SDL name from its hardware scan code (WM_KEYDOWN's lParam), the way SDL
// itself reads the keyboard on Windows: by position, so the game sees the same key.
static std::string key_name_from_scan(LPARAM l)
{
    const unsigned sc = (unsigned)(l >> 16) & 0xFF; const bool ext = (l >> 24) & 1;
    static const SDL_Scancode base[0x59] = {
        SDL_SCANCODE_UNKNOWN, SDL_SCANCODE_ESCAPE, SDL_SCANCODE_1, SDL_SCANCODE_2, SDL_SCANCODE_3, SDL_SCANCODE_4, SDL_SCANCODE_5,
        SDL_SCANCODE_6, SDL_SCANCODE_7, SDL_SCANCODE_8, SDL_SCANCODE_9, SDL_SCANCODE_0, SDL_SCANCODE_MINUS, SDL_SCANCODE_EQUALS,
        SDL_SCANCODE_BACKSPACE, SDL_SCANCODE_TAB, SDL_SCANCODE_Q, SDL_SCANCODE_W, SDL_SCANCODE_E, SDL_SCANCODE_R, SDL_SCANCODE_T,
        SDL_SCANCODE_Y, SDL_SCANCODE_U, SDL_SCANCODE_I, SDL_SCANCODE_O, SDL_SCANCODE_P, SDL_SCANCODE_LEFTBRACKET,
        SDL_SCANCODE_RIGHTBRACKET, SDL_SCANCODE_RETURN, SDL_SCANCODE_LCTRL, SDL_SCANCODE_A, SDL_SCANCODE_S, SDL_SCANCODE_D,
        SDL_SCANCODE_F, SDL_SCANCODE_G, SDL_SCANCODE_H, SDL_SCANCODE_J, SDL_SCANCODE_K, SDL_SCANCODE_L, SDL_SCANCODE_SEMICOLON,
        SDL_SCANCODE_APOSTROPHE, SDL_SCANCODE_GRAVE, SDL_SCANCODE_LSHIFT, SDL_SCANCODE_BACKSLASH, SDL_SCANCODE_Z, SDL_SCANCODE_X,
        SDL_SCANCODE_C, SDL_SCANCODE_V, SDL_SCANCODE_B, SDL_SCANCODE_N, SDL_SCANCODE_M, SDL_SCANCODE_COMMA, SDL_SCANCODE_PERIOD,
        SDL_SCANCODE_SLASH, SDL_SCANCODE_RSHIFT, SDL_SCANCODE_KP_MULTIPLY, SDL_SCANCODE_LALT, SDL_SCANCODE_SPACE,
        SDL_SCANCODE_CAPSLOCK, SDL_SCANCODE_F1, SDL_SCANCODE_F2, SDL_SCANCODE_F3, SDL_SCANCODE_F4, SDL_SCANCODE_F5, SDL_SCANCODE_F6,
        SDL_SCANCODE_F7, SDL_SCANCODE_F8, SDL_SCANCODE_F9, SDL_SCANCODE_F10, SDL_SCANCODE_NUMLOCKCLEAR, SDL_SCANCODE_SCROLLLOCK,
        SDL_SCANCODE_KP_7, SDL_SCANCODE_KP_8, SDL_SCANCODE_KP_9, SDL_SCANCODE_KP_MINUS, SDL_SCANCODE_KP_4, SDL_SCANCODE_KP_5,
        SDL_SCANCODE_KP_6, SDL_SCANCODE_KP_PLUS, SDL_SCANCODE_KP_1, SDL_SCANCODE_KP_2, SDL_SCANCODE_KP_3, SDL_SCANCODE_KP_0,
        SDL_SCANCODE_KP_PERIOD, SDL_SCANCODE_UNKNOWN, SDL_SCANCODE_UNKNOWN, SDL_SCANCODE_NONUSBACKSLASH, SDL_SCANCODE_F11,
        SDL_SCANCODE_F12 };
    SDL_Scancode s = SDL_SCANCODE_UNKNOWN;
    if (ext) {
        switch (sc) {
        case 0x1C: s = SDL_SCANCODE_KP_ENTER; break;     case 0x1D: s = SDL_SCANCODE_RCTRL; break;
        case 0x35: s = SDL_SCANCODE_KP_DIVIDE; break;    case 0x38: s = SDL_SCANCODE_RALT; break;
        case 0x47: s = SDL_SCANCODE_HOME; break;         case 0x48: s = SDL_SCANCODE_UP; break;
        case 0x49: s = SDL_SCANCODE_PAGEUP; break;       case 0x4B: s = SDL_SCANCODE_LEFT; break;
        case 0x4D: s = SDL_SCANCODE_RIGHT; break;        case 0x4F: s = SDL_SCANCODE_END; break;
        case 0x50: s = SDL_SCANCODE_DOWN; break;         case 0x51: s = SDL_SCANCODE_PAGEDOWN; break;
        case 0x52: s = SDL_SCANCODE_INSERT; break;       case 0x53: s = SDL_SCANCODE_DELETE; break;
        case 0x5B: s = SDL_SCANCODE_LGUI; break;         case 0x5C: s = SDL_SCANCODE_RGUI; break;
        case 0x5D: s = SDL_SCANCODE_APPLICATION; break;
        }
    } else if (sc < 0x59) s = base[sc];
    return s == SDL_SCANCODE_UNKNOWN ? std::string() : std::string(SDL_GetScancodeName(s));
}

static void run_task(const std::string& title, std::function<std::string(Progress*)> fn, std::function<void(const std::string&)> done)
{
    if (prog) return;
    prog = new Progress();
    task_title = title;
    task_done = std::move(done);
    ov = Overlay(); ov.type = Overlay::PROGRESS; ov.title = title; ov.no = "Cancel";
    Progress* p = prog;
    worker = std::thread([fn, p] {
        CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        std::string r = fn(p);
        CoUninitialize();
        PostMessageW(g_hwnd, WM_APP_TASK, 0, (LPARAM) new std::string(r));
    });
    redraw();
}

static void done_or_error(const std::string& ok_title, const std::string& ok_text, const std::string& r)
{
    if (r.empty()) message(ok_title, ok_text); else message("Could not finish", r);
    refresh();
}

// ---- settings screen definitions -----------------------------------------------------------
struct Opt { const char* section; const char* key; const char* label; const char* def; std::vector<std::pair<std::string, std::string>> ch; };
static const std::vector<Opt>& opts()
{
    static const std::vector<Opt> o = {
        { "Display", "RACACIT_DISPLAY_MODE", "Display mode", "windowed", { { "windowed", "Windowed" }, { "borderless", "Borderless fullscreen" }, { "fullscreen", "Exclusive fullscreen" } } },
        { "Display", "RACACIT_WINDOW_SIZE", "Window size", "", { { "", "Default" }, { "1280x720", "1280 x 720" }, { "1600x900", "1600 x 900" }, { "1920x1080", "1920 x 1080" }, { "2560x1440", "2560 x 1440" }, { "3840x2160", "3840 x 2160" } } },
        { "Display", "RACACIT_PRESENT_MODE", "VSync", "mailbox", { { "mailbox", "Off (no tearing)" }, { "fifo", "On" }, { "immediate", "Off (allow tearing)" } } },
        { "Display", "RACACIT_ASPECT", "Aspect ratio", "keep", { { "keep", "16:9" }, { "stretch", "Stretch to window" } } },
        { "Image", "RACACIT_RES_SCALE", "Internal resolution", "1", { { "0.5", "0.5x (480x352)" }, { "0.75", "0.75x (720x528)" }, { "1", "1x (960x704, PS3)" }, { "1.5", "1.5x (1440x1056)" }, { "2", "2x (1920x1408)" }, { "3", "3x (2880x2112)" }, { "4", "4x (3840x2816)" } } },
        { "Image", "RACACIT_UPSCALE", "Upscaling", "bilinear", { { "bilinear", "Bilinear" }, { "sharp", "Sharp (bicubic + sharpen)" }, { "nearest", "Nearest (pixelated)" } } },
        { "Image", "RACACIT_SHARPNESS", "Sharpness", "5", { { "0", "0%" }, { "1", "10%" }, { "2", "20%" }, { "3", "30%" }, { "4", "40%" }, { "5", "50%" }, { "6", "60%" }, { "7", "70%" }, { "8", "80%" }, { "9", "90%" }, { "10", "100%" } } },
        { "Image", "RACACIT_BRIGHTNESS", "Brightness", "0", { { "-5", "-5" }, { "-4", "-4" }, { "-3", "-3" }, { "-2", "-2" }, { "-1", "-1" }, { "0", "Default" }, { "1", "+1" }, { "2", "+2" }, { "3", "+3" }, { "4", "+4" }, { "5", "+5" } } },
        { "Image", "RACACIT_ANISO", "Anisotropic filtering", "0", { { "0", "Off" }, { "2", "2x" }, { "4", "4x" }, { "8", "8x" }, { "16", "16x" } } },
        { "Image", "RACACIT_TRILINEAR", "Trilinear filtering", "0", { { "0", "Off" }, { "1", "On" } } },
        { "Image", "RACACIT_TEX_QUALITY", "Texture quality", "high", { { "high", "High (original)" }, { "medium", "Medium" }, { "low", "Low" } } },
        { "Performance", "RACACIT_PRESENT_PACE", "Frame pacing", "1", { { "1", "On (even 60 Hz)" }, { "0", "Off" } } },
        { "Performance", "RACACIT_CPU_MODE", "CPU usage", "efficient", { { "efficient", "Efficient" }, { "max", "Maximum (spin on all cores)" } } },
        { "Controller", "RACACIT_DS_SWAP", "Fire button", "1", { { "1", "R2 / RT (R1 and R2 swapped)" }, { "0", "R1 / RB (PS3 layout)" } } },
        { "Controller", "RACACIT_DS_RUMBLE", "Rumble", "100", {} },
        { "Controller", "RACACIT_DS_TRIGGERS", "Adaptive triggers", "1", { { "1", "On" }, { "0", "Off" } } },
        { "Controller", "RACACIT_DS_TRIG_STR", "Trigger strength", "100", { { "10", "10%" }, { "20", "20%" }, { "30", "30%" }, { "40", "40%" }, { "50", "50%" },
                                                                         { "60", "60%" }, { "70", "70%" }, { "80", "80%" }, { "90", "90%" }, { "100", "100%" } } },
        { "DualSense controller", "RACACIT_DS_SPEAKER", "Controller speaker", "1", { { "1", "On" }, { "0", "Off" } } },
        { "DualSense controller", "RACACIT_DS_SPK_VOL", "Speaker volume", "49", {} },
        { "DualSense controller", "RACACIT_DS_HAPTICS", "Haptic feedback", "1", { { "1", "On" }, { "0", "Off (rumble motors)" } } },
        { "DualSense controller", "RACACIT_DS_HAPTIC_VOL", "Haptic strength", "50", {} },
        { "DualSense controller", "RACACIT_DS_LIGHT", "Light bar", "1", { { "1", "On" }, { "0", "Off" } } },
        { "DualSense controller", "RACACIT_DS_MOTION", "Motion sensors", "1", { { "1", "On" }, { "0", "Off" } } },
        { "Keyboard and mouse", "RACACIT_KBM", "Play with keyboard and mouse", "1", { { "1", "On" }, { "0", "Off" } } },
        { "Keyboard and mouse", "RACACIT_MOUSE_SENS", "Mouse sensitivity", "1", { { "0.25", "25%" }, { "0.5", "50%" }, { "0.75", "75%" }, { "1", "100%" }, { "1.25", "125%" }, { "1.5", "150%" }, { "2", "200%" }, { "3", "300%" }, { "4", "400%" } } },
        { "Keyboard and mouse", "RACACIT_MOUSE_INVERT_Y", "Invert mouse Y", "0", { { "0", "Off" }, { "1", "On" } } },
        // The disc's own translations: the game picks its language from the PS3's system
        // language, which the build answers from this (game/stubs.cpp). Unset = English.
        { "Game", "RACACIT_LANGUAGE", "Language", "", { { "", "English" }, { "fr", "Français (French)" }, { "es", "Español (Spanish)" } } },
        { "Game", "RACACIT_START_SKIPS_MOVIES", "START skips movies", "1", { { "1", "Yes" }, { "0", "No" } } },
        { "Game", "RACACIT_ALL_CUTSCENES_SKIPPABLE", "Every cutscene skippable", "1", { { "1", "Yes" }, { "0", "No (as the game)" } } },
        { "Game", "RACACIT_SPACE_FREE_LANDING", "Land anywhere in space", "1", { { "1", "Yes" }, { "0", "No (as the game)" } } },
        { "Game", "RACACIT_SAVE_UI", "Save list screen", "1", { { "1", "On" }, { "0", "Off" } } },
        { "Game", "RACACIT_FPS", "FPS counter", "0", { { "0", "Off" }, { "1", "On" } } },
        { "Game", "RACACIT_DISCORD", "Discord status", "1", { { "1", "On" }, { "0", "Off" } } },
        { "Game", "RACACIT_SKIP_CLANK", "Clank levels (testing)", "0", { { "0", "Play" }, { "1", "Skip" } } },
    };
    return o;
}

static std::vector<std::pair<std::string, std::string>> percent_choices(int step, bool off_at_zero)
{
    std::vector<std::pair<std::string, std::string>> v;
    for (int p = 0; p <= 100; p += step) v.push_back({ std::to_string(p), p == 0 && off_at_zero ? "Off" : std::to_string(p) + "%" });
    return v;
}

static void option_value(const Opt& o, std::string* value, std::string* label, int* index, std::vector<std::pair<std::string, std::string>>* choices)
{
    *choices = o.ch;
    if (choices->empty()) *choices = percent_choices(!strcmp(o.key, "RACACIT_DS_RUMBLE") ? 10 : 5, !strcmp(o.key, "RACACIT_DS_RUMBLE"));
    *value = S->settings.get(o.key, o.def);
    *index = -1;
    for (size_t i = 0; i < choices->size(); i++) {
        const std::string& c = (*choices)[i].first;
        if (c == *value || (!c.empty() && !value->empty() && atof(c.c_str()) == atof(value->c_str()) && isdigit((unsigned char)c.back())))
            *index = (int)i;
    }
    *label = *index >= 0 ? (*choices)[*index].second : o.ch.empty() ? *value + "%" : *value + " (custom)";
}

// The next choice from `value` in direction d: from a value between two choices
// (29 on a 5% grid), the nearest one that way.
static int step_choice(const std::vector<std::pair<std::string, std::string>>& ch, const std::string& value, int idx, int d)
{
    const int n = (int)ch.size();
    if (idx >= 0) return ((idx + d) % n + n) % n;
    const bool num = !value.empty() && (isdigit((unsigned char)value[0]) || value[0] == '-' || value[0] == '.');
    if (!num) return 0;
    const double v = atof(value.c_str());
    if (d > 0) { for (int i = 0; i < n; i++) if (!ch[i].first.empty() && atof(ch[i].first.c_str()) > v) return i; return 0; }
    for (int i = n - 1; i >= 0; i--) if (!ch[i].first.empty() && atof(ch[i].first.c_str()) < v) return i;
    return n - 1;
}

// ---- screens ------------------------------------------------------------------------------
static void home();
static void play();

static Item button(const std::string& label, const std::string& sub, std::function<void()> act, bool enabled = true)
{
    Item i; i.kind = K_BUTTON; i.label = label; i.sub = sub; i.act = std::move(act); i.enabled = enabled; return i;
}
static Item header(const std::string& label) { Item i; i.kind = K_HEADER; i.label = label; return i; }

// Set up: one screen. The player brings two things from their own copy of the
// game -- the disc image (.iso) and EBOOT.elf, its main program unlocked with
// RPCS3 (the launcher ships no PS3 keys) -- in either order.
static void import_screen();
static void add_elf_picked(const fs::path& p)
{
    bool known = false;
    const std::string bad = Store::check_elf(p, &known);
    if (!bad.empty()) { message("EBOOT.elf", bad); return; }
    auto put = [p] {
        const std::string r = S->add_elf(p);
        if (!r.empty()) { message("EBOOT.elf", r); return; }
        if (S->game().ok) { message("Ready to play", "Your game is set up. Press Play."); set_screen(home); }
        else set_screen(import_screen);
    };
    if (known) put();
    else confirm("A different EBOOT.elf", "This is a PS3 program, but not the EBOOT.elf of the USA disc this build is made "
                 "from (BCUS98124, version 1.00 -- not an update's). The game may not start with it.", "Use it anyway", put);
}

// An earlier launcher's folder (Store::migrate_from_launcher).
static void bring_over_launcher(const fs::path& old)
{
    const std::string name = u8(old.filename());
    confirm("Bring over 0.8", "From " + name + ". Copies its saves, settings, shader caches and save backups into this launcher, and plays its game from "
            "where it is. Save slots with the same name are replaced; the saves here are backed up first. " + name + " itself is not changed.",
            "Bring it over", [old] {
        std::string* sum = new std::string();
        run_task("Bringing over " + u8(old.filename()), [old, sum](Progress* pr) { return S->migrate_from_launcher(old, pr, sum); },
                 [sum](const std::string& r) { done_or_error("Done", *sum, r); delete sum; if (r.empty()) set_screen(home); });
    });
}
static void pick_old_launcher()
{
    const fs::path p = pick_folder(g_hwnd, L"Pick your 0.8 launcher's folder (it holds RaCACiT Launcher.exe and data)");
    if (p.empty()) return;
    std::error_code ec;
    if (!fs::exists(p / L"data" / L"settings.txt", ec)) {
        message("Bring over a 0.8 launcher", "No data\\settings.txt in " + u8(p) + ". Pick the folder that holds RaCACiT Launcher.exe.");
        return;
    }
    bring_over_launcher(p);
}

static void import_screen()
{
    const GameInfo gi = S->game();
    std::error_code ec;
    const bool wrong = gi.status.rfind("This is", 0) == 0 || gi.status.rfind("PS3_GAME not found", 0) == 0;
    const bool have_game = fs::is_directory(gi.root / L"PS3_GAME", ec) && !wrong;
    const bool have_elf = gi.ok || fs::exists(S->pending_elf(), ec);
    scr = Screen(); scr.title = gi.ok ? "Your game" : "Set up"; scr.subtitle = "";
    scr.back = [] { set_screen(home); };
    if (gi.ok) scr.info.push_back({ "Ready", u8(gi.root), col::TEXT_GOOD });
    else if (wrong) scr.info.push_back({ "Problem", gi.status, col::TEXT_BAD });
    else if (have_game) scr.info.push_back({ "Game found", (gi.title.empty() ? std::string() : gi.title + " " + gi.app_ver + "\n") + u8(gi.root), col::TEXT_GOOD });
    scr.info.push_back({ "What you need", "Your own copy of A Crack in Time (USA, BCUS98124):\n"
                                          "1. The game: its disc image (.iso), or the dumped game's folder -- pick "
                                          "its USRDIR. A Redump .iso is encrypted: keep its .dkey file beside it, "
                                          "with the same name, and the launcher unlocks it.\n"
                                          "2. EBOOT.elf, the game's main program unlocked with RPCS3. Already in "
                                          "USRDIR? Then it is found by itself." });
    scr.info.push_back({ "Making EBOOT.elf", "You must provide your own legally dumped copy of Ratchet & Clank Future: "
                                             "A Crack in Time, and the EBOOT.elf file from your dump. If your EBOOT.elf "
                                             "reads as encrypted, RPCS3 has a decryption tool available. RACiT does "
                                             "not and will not assist with acquiring the .iso dump of the game." });
    Item iso = button(have_game ? "1. Game found: import another .iso" : "1. Import the game: a .iso",
                      "Unpacks it into the launcher (about 25 GB)", [] {
        const fs::path p = pick_open_file(g_hwnd, L"Pick your A Crack in Time disc image", L"Disc image (*.iso)", L"*.iso");
        if (p.empty()) return;
        std::string* sum = new std::string();
        run_task("Importing the game", [p, sum](Progress* pr) { return S->import_iso(p, pr, sum); },
                 [sum](const std::string& r) {
                     if (!r.empty()) message("Could not import", r);
                     else if (S->game().ok) { message("Ready to play", *sum + "\nPress Play."); set_screen(home); }
                     else { message("Game imported", *sum + "\nNow add EBOOT.elf (step 2)."); set_screen(import_screen); }
                     delete sum; refresh();
                 });
    });
    iso.accent = !have_game;
    scr.items.push_back(iso);
    // Or a dumped game folder, played where it is (Store::game_folder_from_pick takes USRDIR,
    // PS3_GAME or the folder holding it). A decrypted EBOOT.elf already in USRDIR finishes set up.
    scr.items.push_back(button(have_game ? "   or another game folder" : "   or pick the game's folder (USRDIR)",
                               "A dumped game, played where it is; its EBOOT.elf is found by itself", [] {
        const fs::path p = pick_folder(g_hwnd, L"Pick your game's USRDIR folder (in PS3_GAME)");
        if (p.empty()) return;
        const std::string r = S->use_game_in_place(p);
        std::error_code ec;
        if (r.empty() && !S->game().ok && fs::exists(S->pending_elf(), ec)) S->add_elf(S->pending_elf());
        const GameInfo g = S->game();
        if (!r.empty()) message("Game folder", r);
        else if (g.ok) { message("Ready to play", "Found your game and its EBOOT.elf. Press Play."); set_screen(home); return; }
        else if (g.need_decrypt) message("Game found", g.status + ". Add EBOOT.elf (step 2).");
        else message("Game folder", "Found it, but: " + g.status);
        set_screen(import_screen);
    }));
    Item elf = button(gi.ok ? "2. EBOOT.elf added" : have_elf ? "2. EBOOT.elf added (waits for the game)" : "2. Add EBOOT.elf",
                      gi.ok || have_elf ? "Pick another to replace it" : "The file RPCS3 made", [] {
        const fs::path p = pick_open_file(g_hwnd, L"Pick EBOOT.elf (made by RPCS3)", L"PS3 program (*.elf)", L"*.elf;*.ELF");
        if (!p.empty()) add_elf_picked(p);
    });
    elf.accent = have_game && !gi.ok;
    scr.items.push_back(elf);
    if (have_game && !gi.ok) {
        const fs::path usrdir = gi.root / L"PS3_GAME" / L"USRDIR";
        scr.items.push_back(button("Show EBOOT.BIN", "Its folder, to decrypt it in RPCS3", [usrdir] { shell_open(usrdir); }));
    }
    if (gi.ok) { Item pl = button("Play", "", [] { set_screen(home); play(); }); pl.accent = true; scr.items.push_back(pl); }
    // The way most players arrive: from their 0.8 launcher, its saves, settings and game in one step.
    scr.items.push_back(header("Other ways"));
    // The label stays "0.8" whatever the folder is called; another copy of this version is no source.
    fs::path old;
    for (const auto& o : S->find_old_launchers())
        if (u8(o.filename()).find(kLauncherVersion) == std::string::npos) { old = o; break; }
    if (!old.empty()) {
        scr.items.push_back(button("Bring over 0.8", "From " + u8(old.filename()) + ": saves, settings, game and shader caches",
                                   [old] { bring_over_launcher(old); }));
    } else {
        scr.items.push_back(button("Bring over a 0.8 launcher", "Your saves, settings, game and shader caches: pick its folder", [] { pick_old_launcher(); }));
    }
    scr.items.push_back(button("Back", "", [] { set_screen(home); }));
}

// The DualSense Plus mod (mods\dualsense_plus, mod.json settings RACACIT_DUALSENSE_PLUS=1): the
// DualSense extras. Off, every controller is a plain pad, and the DualSense-only options are
// hidden (Rumble stays: every controller's).
static const char* const k_ds_plus_mod = "dualsense_plus";
static bool ds_plus_on()
{
    const auto en = S->enabled_mods();
    for (auto& m : S->mods())
        if (m.problem.empty() && std::find(en.begin(), en.end(), m.id) != en.end())
            for (auto& kv : m.settings)
                if (lower(kv.first) == "racacit_dualsense_plus" && kv.second == "1") return true;
    return S->settings.get("RACACIT_DUALSENSE_PLUS") == "1";
}
static bool ds_plus_only(const char* key) { return !strncmp(key, "RACACIT_DS_", 11) && strcmp(key, "RACACIT_DS_RUMBLE"); }
static Item ds_plus_item()
{
    bool have = false;
    for (auto& m : S->mods()) if (m.id == k_ds_plus_mod && m.problem.empty()) have = true;
    const bool on = ds_plus_on();
    Item it; it.kind = K_OPTION; it.label = "DualSense Plus";
    it.sub = on ? "On (PS5 DualSense: speaker, haptics, adaptive triggers, light bar, motion, fire on R2)"
         : have ? "Off (every controller plays as a plain pad)" : "Not installed (mods\\dualsense_plus)";
    auto flip = [have, on](int) { if (have) { S->set_mod_enabled(k_ds_plus_mod, !on); refresh(); } };
    it.change = flip;
    it.act = [flip] { flip(+1); };
    return it;
}

static int s_bind_tab = 0;   // Key bindings: 0 by button, 1 by key
static void bindings_screen();
static void add_options(const std::vector<std::string>& sections)
{
    std::string section;
    const bool plus = ds_plus_on();
    for (const Opt& o : opts()) {
        if (!sections.empty() && std::find(sections.begin(), sections.end(), std::string(o.section)) == sections.end()) continue;
        if (!plus && ds_plus_only(o.key)) continue;
        if (section != o.section) {
            section = o.section; scr.items.push_back(header(section));
            if (section == "Controller") scr.items.push_back(ds_plus_item());
        }
        std::string value, label; int idx; std::vector<std::pair<std::string, std::string>> ch;
        option_value(o, &value, &label, &idx, &ch);
        Item it; it.kind = K_OPTION; it.label = o.label; it.sub = label;
        const Opt* op = &o;
        auto step = [op](int d) {
            std::string value, label; int idx; std::vector<std::pair<std::string, std::string>> ch;
            option_value(*op, &value, &label, &idx, &ch);
            idx = step_choice(ch, value, idx, d);
            if (ch[idx].first.empty()) S->settings.remove(op->key); else S->settings.put(op->key, ch[idx].first);
            S->settings.save();
            refresh();
        };
        it.change = step;
        it.act = [step] { step(+1); };
        scr.items.push_back(it);
        if (!strcmp(o.key, "RACACIT_MOUSE_INVERT_Y"))
            scr.items.push_back(button("Key bindings", "Every button on keys and mouse buttons", [] { s_bind_tab = 0; set_screen(bindings_screen); }));
    }
}

// ---- key bindings -----------------------------------------------------------------------------
// The game's keyboard and mouse bindings (cellPad.c k_kb, the same names and defaults):
// RACACIT_BIND_<BUTTON>=<keys> in settings.txt, up to 4 keys a button, "none" for none.
// A key on several buttons presses them all.
struct Bind { const char* name; const char* group; const char* label; const char* def; };
static const Bind k_binds[] = {
    { "CROSS", "Buttons", "Cross (A): jump", "Space,Return" },
    { "CIRCLE", "Buttons", "Circle (B)", "E,Backspace" },
    { "SQUARE", "Buttons", "Square (X): melee", "F" },
    { "TRIANGLE", "Buttons", "Triangle (Y): quick select", "Q" },
    { "L1", "Buttons", "L1 (LB): strafe", "Mouse3" },
    { "R1", "Buttons", "R1 (RB): fire", "Mouse1" },
    { "L2", "Buttons", "L2 (LT)", "Left Ctrl" },
    { "R2", "Buttons", "R2 (RT)", "Left Shift" },
    { "L3", "Buttons", "L3 (left stick click)", "V" },
    { "R3", "Buttons", "R3 (right stick click)", "Mouse2,B" },
    { "START", "Buttons", "Start: pause", "Escape,P" },
    { "SELECT", "Buttons", "Select (Back): map", "Tab,M" },
    { "UP", "D-pad", "D-pad up", "Up,1" },
    { "DOWN", "D-pad", "D-pad down", "Down,3" },
    { "LEFT", "D-pad", "D-pad left", "Left,4" },
    { "RIGHT", "D-pad", "D-pad right", "Right,2" },
    { "LS_UP", "Move (left stick)", "Forward", "W" },
    { "LS_DOWN", "Move (left stick)", "Back", "S" },
    { "LS_LEFT", "Move (left stick)", "Left", "A" },
    { "LS_RIGHT", "Move (left stick)", "Right", "D" },
    { "WALK", "Move (left stick)", "Walk (hold: half speed)", "Left Alt" },
    { "RS_UP", "Camera keys (right stick; the mouse turns it too)", "Camera up", "I" },
    { "RS_DOWN", "Camera keys (right stick; the mouse turns it too)", "Camera down", "K" },
    { "RS_LEFT", "Camera keys (right stick; the mouse turns it too)", "Camera left", "J" },
    { "RS_RIGHT", "Camera keys (right stick; the mouse turns it too)", "Camera right", "L" },
};
static const int k_max_keys = 4;

static std::vector<std::string> split_keys(const std::string& v)
{
    std::vector<std::string> out; size_t a = 0;
    for (;;) {
        const size_t c = v.find(',', a);
        const std::string t = trim(v.substr(a, c == std::string::npos ? std::string::npos : c - a));
        if (!t.empty() && lower(t) != "none") out.push_back(t);
        if (c == std::string::npos) break;
        a = c + 1;
    }
    return out;
}
static std::string join_keys(const std::vector<std::string>& k)
{
    std::string s; for (auto& x : k) s += (s.empty() ? "" : ",") + x; return s;
}
static std::vector<std::string> bind_keys(const Bind& b)
{
    const std::string v = S->settings.get(std::string("RACACIT_BIND_") + b.name);
    return split_keys(v.empty() ? b.def : v);   // the game: empty = the default
}
static void set_bind_keys(const Bind& b, const std::vector<std::string>& k)
{
    const std::string key = std::string("RACACIT_BIND_") + b.name, v = join_keys(k);
    if (lower(v) == lower(b.def)) S->settings.remove(key);
    else S->settings.put(key, v.empty() ? "none" : v);
    S->settings.save();
}
static bool same_input(const std::string& a, const std::string& b) { return lower(a) == lower(b); }
static std::string input_label(const std::string& k)
{
    static const std::pair<const char*, const char*> m[] = { { "mouse1", "Left click" }, { "mouse2", "Middle click" },
        { "mouse3", "Right click" }, { "mouse4", "Mouse back" }, { "mouse5", "Mouse forward" }, { "wheelup", "Wheel up" },
        { "wheeldown", "Wheel down" } };
    for (auto& e : m) if (lower(k) == e.first) return e.second;
    return k;
}
static std::string keys_label(const std::vector<std::string>& k)
{
    std::string s; for (auto& x : k) s += (s.empty() ? "" : ", ") + input_label(x);
    return s.empty() ? "Nothing" : s;
}

static void settings_screen();
static void bindings_screen();
static void key_detail(const std::string& key)
{
    scr = Screen(); scr.title = input_label(key); scr.subtitle = "Key bindings: by key";
    scr.back = [] { set_screen(bindings_screen); };
    scr.info.push_back({ "This key presses", "Every button turned on here, all at once. A button holds up to "
                                             + std::to_string(k_max_keys) + " keys. Changes apply the next time the game starts." });
    std::string group;
    for (const Bind& b : k_binds) {
        if (group != b.group) { group = b.group; scr.items.push_back(header(group)); }
        const auto keys = bind_keys(b);
        const bool on = std::any_of(keys.begin(), keys.end(), [&](const std::string& x) { return same_input(x, key); });
        Item it; it.kind = K_OPTION; it.label = b.label; it.sub = on ? "On" : "Off";
        const Bind* bp = &b;
        auto flip = [bp, key](int) {
            auto k = bind_keys(*bp);
            auto at = std::find_if(k.begin(), k.end(), [&](const std::string& x) { return same_input(x, key); });
            if (at != k.end()) k.erase(at);
            else if ((int)k.size() >= k_max_keys) { message("Full", std::string(bp->label) + " already has " + std::to_string(k_max_keys) +
                                                             " keys (" + keys_label(k) + "). Take one off first."); return; }
            else k.push_back(key);
            set_bind_keys(*bp, k); refresh();
        };
        it.change = flip; it.act = [flip] { flip(+1); };
        scr.items.push_back(it);
    }
    scr.items.push_back(header("This key"));
    scr.items.push_back(button("Take it off every button", "", [key] {
        for (const Bind& b : k_binds) {
            auto k = bind_keys(b);
            const auto n = k.size();
            k.erase(std::remove_if(k.begin(), k.end(), [&](const std::string& x) { return same_input(x, key); }), k.end());
            if (k.size() != n) set_bind_keys(b, k);
        }
        set_screen(bindings_screen);
    }));
    scr.items.push_back(button("Back", "", [] { set_screen(bindings_screen); }));
}

static void bindings_screen()
{
    scr = Screen(); scr.title = "Key bindings"; scr.subtitle = s_bind_tab ? "By key" : "By button";
    scr.back = [] { set_screen(settings_screen); };
    if (!s_bind_tab)
        scr.info.push_back({ "Rebinding", "Pick a button, then press the key or mouse button (or turn the wheel) "
                                          "for it; it replaces that button's keys. To put one key on several "
                                          "buttons, or more keys on one, use By key." });
    else
        scr.info.push_back({ "By key", "Each key and what it presses. Pick one to change its buttons, or add a "
                                       "key: one key can press several buttons at once (say H for D-pad right "
                                       "and R2)." });
    scr.info.push_back({ "When", "Changes apply the next time the game starts. Moving the mouse turns the camera "
                                 "(speed and invert in Settings). Keyboard and mouse work alongside a controller." });
    Item view; view.kind = K_OPTION; view.label = "View"; view.sub = s_bind_tab ? "By key" : "By button";
    view.change = [](int) { s_bind_tab ^= 1; set_screen(bindings_screen); };
    view.act = [] { s_bind_tab ^= 1; set_screen(bindings_screen); };
    scr.items.push_back(view);
    if (!s_bind_tab) {
        std::string group;
        for (const Bind& b : k_binds) {
            if (group != b.group) { group = b.group; scr.items.push_back(header(group)); }
            const Bind* bp = &b;
            scr.items.push_back(button(b.label, keys_label(bind_keys(b)), [bp] {
                capture_input(bp->label, "Press the key or mouse button for " + std::string(bp->label) + " (now: " +
                              keys_label(bind_keys(*bp)) + ").", [bp](const std::string& k) { set_bind_keys(*bp, { k }); refresh(); });
            }));
        }
    } else {
        scr.items.push_back(button("Add a key", "Press it, then pick its buttons", [] {
            capture_input("Add a key", "Press the key or mouse button to set up.", [](const std::string& k) {
                const std::string key = k;
                set_screen([key] { key_detail(key); });
            });
        }));
        scr.items.push_back(header("Keys in use"));
        std::vector<std::string> keys;   // in the order the buttons list them
        for (const Bind& b : k_binds)
            for (auto& k : bind_keys(b))
                if (std::none_of(keys.begin(), keys.end(), [&](const std::string& x) { return same_input(x, k); })) keys.push_back(k);
        for (const std::string& k : keys) {
            std::string what;
            for (const Bind& b : k_binds) {
                const auto bk = bind_keys(b);
                if (std::any_of(bk.begin(), bk.end(), [&](const std::string& x) { return same_input(x, k); }))
                    what += (what.empty() ? "" : ", ") + std::string(b.label);
            }
            scr.items.push_back(button(input_label(k), what, [k] { set_screen([k] { key_detail(k); }); }));
        }
    }
    scr.items.push_back(header(""));
    scr.items.push_back(button("Reset all to the defaults", "", [] {
        confirm("Reset the key bindings?", "Every button goes back to its default keys.", "Reset", [] {
            for (const Bind& b : k_binds) S->settings.remove(std::string("RACACIT_BIND_") + b.name);
            S->settings.save(); refresh();
        });
    }));
    scr.items.push_back(button("Back", "", [] { set_screen(settings_screen); }));
}

static void settings_screen()
{
    scr = Screen(); scr.title = "Settings"; scr.subtitle = ""; scr.back = [] { set_screen(home); };
    scr.info.push_back({ "Changing a setting", "Left / Right. It applies the next time the game starts; "
                                               "in the game, F1 (or the DualSense touchpad, or Select held on other controllers) opens the same settings." });
    add_options({});
    scr.items.push_back(button("Back", "", [] { set_screen(home); }));
}

static void mods_screen();
static void mod_detail(const std::string& id)
{
    ModInfo m; bool found = false;
    for (auto& x : S->mods()) if (x.id == id) { m = x; found = true; }
    if (!found) { set_screen(mods_screen); return; }
    const auto en = S->enabled_mods();
    const auto pos = std::find(en.begin(), en.end(), id);
    const bool on = pos != en.end();
    scr = Screen(); scr.title = m.name; scr.subtitle = "Mod"; scr.back = [] { set_screen(mods_screen); };
    scr.info.push_back({ "State", on ? "On -- priority " + std::to_string(pos - en.begin() + 1) + " of " + std::to_string(en.size()) : "Off",
                         on ? col::ORANGE : col::TEXT });
    if (!m.version.empty()) scr.info.push_back({ "Version", m.version });
    if (!m.author.empty()) scr.info.push_back({ "Author", m.author });
    if (!m.description.empty()) scr.info.push_back({ "About", m.description });
    if (!m.problem.empty()) scr.info.push_back({ "Problem", m.problem, col::TEXT_BAD });
    scr.info.push_back({ "Folder", u8(m.dir), col::TEXT_DIM });
    scr.items.push_back(button(on ? "Turn off" : "Turn on", on ? "" : "It goes first: it wins over mods already on", [id, on] { S->set_mod_enabled(id, !on); refresh(); }, m.problem.empty() || on));
    scr.items.push_back(button("Higher priority", "Wins over the mod above it", [id] { S->move_mod(id, -1); refresh(); }, on && pos != en.begin()));
    scr.items.push_back(button("Lower priority", "", [id] { S->move_mod(id, +1); refresh(); }, on && pos + 1 != en.end()));
    scr.items.push_back(button("Open its folder", "", [m] { shell_open(m.dir); }));
    scr.items.push_back(button("Remove", "Deletes its folder", [id, m] {
        confirm("Remove " + m.name, "Deletes " + u8(m.dir) + ".", "Remove", [id] {
            const std::string r = S->remove_mod(id);
            if (!r.empty()) message("Mods", r);
            set_screen(mods_screen);
        });
    }));
    scr.items.push_back(button("Back", "", [] { set_screen(mods_screen); }));
}

static void mods_screen()
{
    const auto all = S->mods();
    const auto en = S->enabled_mods();
    scr = Screen(); scr.title = "Mods"; scr.subtitle = "Replace game files"; scr.back = [] { set_screen(home); };
    scr.info.push_back({ "How", "A mod is a folder in mods\\ (or a .zip installed here): mod.json "
                                "{\"id\", \"name\", \"version\", \"author\", \"description\"} and files\\, laid out like the game folder "
                                "(files\\PS3_GAME\\USRDIR\\packed\\...). Whole files are replaced, e.g. a patched .psarc." });
    scr.info.push_back({ "Priority", "When two mods replace the same file, the one higher in the list wins. Turning a mod on puts it first." });
    scr.info.push_back({ "Folder", u8(S->mods_dir()), col::TEXT_DIM });
    scr.items.push_back(header(all.empty() ? "No mods yet" : "Installed"));
    std::vector<ModInfo> ordered;
    for (auto& id : en) for (auto& m : all) if (m.id == id) ordered.push_back(m);
    for (auto& m : all) if (std::find(en.begin(), en.end(), m.id) == en.end()) ordered.push_back(m);
    for (auto& m : ordered) {
        const auto pos = std::find(en.begin(), en.end(), m.id);
        const bool on = pos != en.end();
        Item it; it.kind = K_OPTION;
        it.label = m.name + (m.version.empty() ? "" : "  v" + m.version);
        it.sub = !m.problem.empty() && !on ? "Problem" : on ? "On (" + std::to_string(pos - en.begin() + 1) + ")" : "Off";
        const std::string id = m.id;
        const bool ok = m.problem.empty();
        it.change = [id, on, ok](int) { if (ok || on) { S->set_mod_enabled(id, !on); refresh(); } };
        it.act = [id] { set_screen([id] { mod_detail(id); }); };
        scr.items.push_back(it);
    }
    scr.items.push_back(header("Add"));
    scr.items.push_back(button("Install a mod", "From a .zip with mod.json", [] {
        const fs::path z = pick_open_file(g_hwnd, L"Pick a mod (.zip)", L"Mod (.zip)", L"*.zip");
        if (z.empty()) return;
        std::string* id = new std::string();
        run_task("Installing the mod", [z, id](Progress*) { return S->install_mod_zip(z, id); },
                 [id](const std::string& r) { done_or_error("Mods", "Installed \"" + *id + "\". Turn it on in the list.", r); delete id; });
    }));
    scr.items.push_back(button("Install a mod folder", "A folder with mod.json and files\\", [] {
        const fs::path d = pick_folder(g_hwnd, L"Pick the mod's folder (it holds mod.json and files)");
        if (d.empty()) return;
        std::string* id = new std::string();
        run_task("Installing the mod", [d, id](Progress*) { return S->install_mod_folder(d, id); },
                 [id](const std::string& r) { done_or_error("Mods", "Installed \"" + *id + "\". Turn it on in the list.", r); delete id; });
    }));
    scr.items.push_back(button("Open the mods folder", "", [] { shell_open(S->mods_dir()); }));
    scr.items.push_back(button("Back", "", [] { set_screen(home); }));
}

static void saves_screen();
static void backups_screen()
{
    auto bk = S->backup_list();
    scr = Screen(); scr.title = "Backups"; scr.subtitle = "Restore your saves"; scr.back = [] { set_screen(saves_screen); };
    scr.info.push_back({ "How", "Restoring puts that backup's saves back. Your current saves are backed up first, so it can be undone." });
    scr.info.push_back({ "Folder", u8(S->backups()), col::TEXT_DIM });
    if (bk.empty()) scr.items.push_back(header("No backups yet"));
    for (auto it = bk.rbegin(); it != bk.rend(); ++it) {
        const fs::path z = *it;
        std::error_code ec;
        scr.items.push_back(button(u8(z.stem()), file_date(z) + "  --  " +
                                   (fs::is_directory(z, ec) ? std::string("folder") : format_size(fs::file_size(z, ec))), [z] {
            confirm("Restore this backup?", u8(z.filename()) + "\n\nYour current saves are backed up first.", "Restore", [z] {
                run_task("Restoring", [z](Progress*) { return S->restore_backup(z); },
                         [](const std::string& r) { done_or_error("Saves", "The backup was restored.", r); });
            });
        }));
    }
    scr.items.push_back(button("Back", "", [] { set_screen(saves_screen); }));
}

static void saves_screen()
{
    const auto slots = S->save_slots();
    const auto bk = S->backup_list();
    scr = Screen(); scr.title = "Saves"; scr.subtitle = "Export, import, backups"; scr.back = [] { set_screen(home); };
    if (slots.empty()) scr.info.push_back({ "Slots", "No saves yet.", col::TEXT_DIM });
    for (auto& s : slots) scr.info.push_back({ u8(s.filename()), "Saved " + file_date(s / L"GAME.SAV") });
    scr.info.push_back({ "Backups", bk.empty() ? "None yet (one is made each time you press Play)" :
                                    std::to_string(bk.size()) + ", newest " + u8(bk.back().stem()), col::TEXT_DIM });
    scr.info.push_back({ "Folder", u8(S->saves()), col::TEXT_DIM });
    scr.items.push_back(button("Export saves", "To a .zip you choose", [] {
        const fs::path z = pick_save_file(g_hwnd, L"Export saves", widen("acit-saves-" + stamp_now() + ".zip").c_str(), L"Zip", L"*.zip");
        if (z.empty()) return;
        run_task("Exporting saves", [z](Progress*) { return S->export_saves(z); },
                 [z](const std::string& r) { done_or_error("Saves", "Saved to " + u8(z), r); });
    }, !slots.empty()));
    scr.items.push_back(button("Import saves", "From a .zip (your saves are backed up first)", [] {
        const fs::path z = pick_open_file(g_hwnd, L"Import saves", L"Zip", L"*.zip");
        if (z.empty()) return;
        confirm("Import saves?", "Slots in " + u8(z.filename()) + " replace the slots with the same name.\n\nYour current saves are backed up first.", "Import", [z] {
            run_task("Importing saves", [z](Progress*) { return S->import_saves(z); },
                     [](const std::string& r) { done_or_error("Saves", "Imported.", r); });
        });
    }));
    scr.items.push_back(button("Back up now", "", [] {
        fs::path* made = new fs::path();
        run_task("Backing up", [made](Progress*) { return S->backup_saves("manual", made); },
                 [made](const std::string& r) { done_or_error("Saves", made->empty() ? "No saves to back up." : "Backed up to " + u8(made->filename()), r); delete made; });
    }, !slots.empty()));
    scr.items.push_back(button("Restore a backup", bk.empty() ? "None yet" : std::to_string(bk.size()) + " to choose from",
                               [] { set_screen(backups_screen); }, !bk.empty()));
    scr.items.push_back(button("Bring over a 0.8 launcher", "Its saves, settings, game and shader caches", [] { pick_old_launcher(); }));
    scr.items.push_back(button("Bring over an older release", "Its settings and saves", [] {
        const fs::path p = pick_folder(g_hwnd, L"Pick a release folder (it holds RaCACiT.exe and settings.txt)");
        if (p.empty()) return;
        std::string* sum = new std::string();
        run_task("Bringing over " + u8(p.filename()), [p, sum](Progress* pr) { return S->migrate_from(p, pr, sum); },
                 [sum](const std::string& r) { done_or_error("Done", *sum, r); delete sum; if (r.empty()) set_screen(home); });
    }));
    scr.items.push_back(button("Open the saves folder", "", [] { std::error_code ec; fs::create_directories(S->saves(), ec); shell_open(S->saves()); }));
    scr.items.push_back(button("Back", "", [] { set_screen(home); }));
}

// ---- updates -------------------------------------------------------------------------------
// The newest release on GitHub: what is installed against what is there, its notes, the
// download, then a confirmation, then the install. A quiet check when the launcher opens only
// adds a button on the home screen; it never asks anything.
static ReleaseInfo g_rel;                       // what GitHub named last
static bool g_rel_known = false;                // ... in this session
static bool g_update_ready = false;             // a newer release than this one is out there
static std::string g_update_status;
static bool g_update_bad = false;
static bool g_restart = false;                  // a new launcher is in place: start it on the way out
static void updates_screen();

static bool release_newer(const std::string& tag) { return Store::compare_versions(tag, S->installed_release(kLauncherVersion)) > 0; }

static void remember_release(const ReleaseInfo& rel)
{
    g_rel = rel; g_rel_known = true;
    g_update_ready = release_newer(rel.tag);
    g_update_bad = false;
    g_update_status = g_update_ready ? rel.name + " is available." :
                      Store::compare_versions(rel.tag, "0") < 0 ? "The newest release (" + rel.tag + ") has no version number in its tag, so it cannot be compared." :
                      "You have the newest release.";
    S->ini.put("update_last_check", std::to_string((long long)time(nullptr)));
    S->ini.put("update_seen", rel.tag);
    S->ini.save();
}

static void check_for_updates()
{
    ReleaseInfo* res = new ReleaseInfo();
    run_task("Checking for updates", [res](Progress*) { return Store::fetch_latest_release(res); }, [res](const std::string& r) {
        if (r.empty()) remember_release(*res);
        else { g_update_status = r; g_update_bad = true; }
        delete res;
        refresh();
    });
}

static void restart_launcher() { g_restart = true; PostMessageW(g_hwnd, WM_CLOSE, 0, 0); }

static void install_update(const ReleaseInfo rel)
{
    std::string* sum = new std::string(); bool* again = new bool(false);
    run_task("Installing " + rel.tag, [rel, sum, again](Progress* p) { return S->install_release(rel, p, sum, again); }, [sum, again](const std::string& r) {
        if (r == "cancelled") { refresh(); }
        else if (!r.empty()) message("Could not finish", r);
        else {
            g_update_ready = false; g_update_bad = false; g_update_status = "You have the newest release.";
            message("Updated", *sum + (*again ? "\n\nThe launcher starts again with its new version." : ""));
            if (*again) ov.on_yes = restart_launcher;
        }
        delete sum; delete again;
        refresh();
    });
}

static void download_update(const ReleaseInfo rel)
{
    if (game_proc) { message("Updates", "Close the game first."); return; }
    run_task("Downloading " + rel.tag, [rel](Progress* p) { return S->download_release(rel, p); }, [rel](const std::string& r) {
        if (r == "cancelled") { refresh(); return; }
        if (!r.empty()) { message("Could not finish", r); refresh(); return; }
        confirm("Install " + rel.tag + "?", "The download is complete and checked. Installing backs up your saves, then puts in the new game build"
                " and launcher. Your saves, settings, shader caches, mods you added and your game stay as they are.", "Install", [rel] { install_update(rel); });
    });
}

static void updates_screen()
{
    const BuildInfo b = S->current_build();
    scr = Screen(); scr.title = "Updates"; scr.subtitle = "New releases on GitHub"; scr.back = [] { set_screen(home); };
    std::string inst = S->installed_release(kLauncherVersion);
    if (!inst.empty() && (inst[0] == 'v' || inst[0] == 'V')) inst.erase(0, 1);
    scr.info.push_back({ "Installed", "RACiT " + inst + (b.version ? ", game build v" + std::to_string(b.version) : std::string()) });
    if (!g_update_status.empty()) scr.info.push_back({ "Last check", g_update_status, g_update_bad ? col::TEXT_BAD : col::TEXT });
    const bool can = g_rel_known && g_update_ready;
    if (can) {
        scr.info.push_back({ "Available", g_rel.name + (g_rel.size ? ", a " + format_size(g_rel.size) + " download" : std::string()), col::TEXT_GOOD });
        if (g_rel.url.empty())
            scr.info.push_back({ "Cannot install from here", "This release has no launcher .zip" + (g_rel.other_assets.empty() ? std::string() : " (it has " + g_rel.other_assets + ")") +
                                 ". Download it from its page and bring your saves over in Set up.", col::TEXT_BAD });
        if (game_proc) scr.info.push_back({ "The game is running", "Close it to install the update.", col::TEXT_BAD });
        if (!g_rel.notes.empty()) scr.info.push_back({ "What's new", g_rel.notes });
    }
    if (can && !g_rel.url.empty()) {
        Item d = button("Download " + g_rel.tag, "You confirm before anything is installed", [] { download_update(g_rel); }, !game_proc);
        d.accent = true; scr.items.push_back(d);
    }
    scr.items.push_back(button(g_rel_known ? "Check again" : "Check for updates", "", check_for_updates));
    if (g_rel_known && !g_rel.page.empty()) scr.items.push_back(button("Open the release's page", "The full notes, on GitHub", [] { shell_open(upath(g_rel.page)); }));
    const bool on = S->ini.get("check_updates", "1") != "0";
    Item it; it.kind = K_OPTION; it.label = "Look when the launcher opens"; it.sub = on ? "On" : "Off";
    auto flip = [on](int) { S->ini.put("check_updates", on ? "0" : "1"); S->ini.save(); refresh(); };
    it.change = flip; it.act = [flip] { flip(+1); };
    scr.items.push_back(it);
    scr.items.push_back(button("Back", "", [] { set_screen(home); }));
}

static void open_updates()
{
    set_screen(updates_screen);
    if (!g_rel_known) check_for_updates();
}

// When the launcher opens: at most one question to GitHub every 6 hours (a newer release it
// already heard of shows at once). Off line or refused: nothing happens, nothing is shown.
static void quiet_update_check()
{
    if (S->ini.get("check_updates", "1") == "0") return;
    const std::string seen = trim(S->ini.get("update_seen"));
    if (!seen.empty() && release_newer(seen)) g_update_ready = true;
    const long long last = atoll(S->ini.get("update_last_check", "0").c_str()), now = (long long)time(nullptr);
    if (now >= last && now - last < 6 * 3600) return;
    std::thread([] {
        ReleaseInfo* rel = new ReleaseInfo();
        if (!Store::fetch_latest_release(rel).empty() || !PostMessageW(g_hwnd, WM_APP_UPDATE, 0, (LPARAM)rel)) delete rel;
    }).detach();
}

static void logs_screen()
{
    std::error_code ec;
    const bool has = fs::exists(S->log(), ec);
    const auto old = S->old_logs();
    scr = Screen(); scr.title = "Logs"; scr.subtitle = "For bug reports"; scr.back = [] { set_screen(home); };
    scr.info.push_back({ "Last session", has ? u8(S->log()) + "  (" + format_size(fs::file_size(S->log(), ec)) + ", " + file_date(S->log()) + ")" : "No log yet" });
    scr.info.push_back({ "Earlier", old.empty() ? "None" : std::to_string(old.size()) + " in " + u8(S->logs()) + " (each Play keeps the last one)", col::TEXT_DIM });
    scr.info.push_back({ "Sending", "After a session with a problem, send log.txt (Copy to the Desktop makes a copy named with the date)." });
    scr.items.push_back(button("Open the last log", "", [] { shell_open(S->log()); }, has));
    scr.items.push_back(button("Copy the last log to the Desktop", "", [] {
        wchar_t* desk = nullptr;
        if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Desktop, 0, nullptr, &desk))) {
            std::error_code ec;
            const fs::path to = fs::path(desk) / widen("acit-log-" + stamp_now() + ".txt");
            fs::copy_file(S->log(), to, fs::copy_options::overwrite_existing, ec);
            message("Logs", ec ? "Copy failed: " + ec.message() : "Copied to " + u8(to));
            CoTaskMemFree(desk);
        }
    }, has));
    scr.items.push_back(button("Open the logs folder", "", [] { shell_open(S->logs()); }));
    scr.items.push_back(button("Back", "", [] { set_screen(home); }));
}

static void help_screen()
{
    scr = Screen(); scr.title = "Help"; scr.subtitle = "Folders and controls"; scr.back = [] { set_screen(home); };
    scr.info.push_back({ "Launcher folder", u8(S->root) });
    scr.info.push_back({ "builds\\", "Installed builds (RaCACiT.exe per version); Play starts the newest." });
    scr.info.push_back({ "data\\", "settings.txt, dev_hdd0 (the PS3 hard drive with your saves), cache (shaders), log.txt and logs." });
    scr.info.push_back({ "game\\  mods\\  backups\\", "Your game (imported from the .iso); mods; save backups (one per Play, the newest kept)." });
    scr.info.push_back({ "launcher.ini", "The launcher's options: looking for updates, what the window does while you play, backups and logs kept." });
    scr.info.push_back({ "Controls", "Mouse; arrows, Enter, Esc; or a controller: Cross (A) select, Circle (B) back, START plays." });
    scr.info.push_back({ "In the game", "Any controller, any time (bottom button = Cross). Keyboard: WASD, mouse camera "
                         "(click the game), Space jump, left click fire, Esc pause, F1 settings." });
    scr.items.push_back(button("Open the launcher folder", "", [] { shell_open(S->root); }));
    scr.items.push_back(button("Back", "", [] { set_screen(home); }));
}

static void stop_game()
{
    confirm("Stop the game?", "Unsaved progress is lost.", "Stop it", [] { if (game_proc) TerminateProcess(game_proc, 1); });
}

static void more_screen();
static void home()
{
    const GameInfo gi = S->game();
    const BuildInfo b = S->current_build();
    const auto slots = S->save_slots();
    const auto en = S->enabled_mods();
    scr = Screen(); scr.title = "A Crack in Time"; scr.subtitle = "";
    if (!gi.ok) scr.info.push_back({ "Status", "Set up the game to play", col::TEXT_BAD });
    else if (!b.version) scr.info.push_back({ "Status", "Game files missing: reinstall the launcher", col::TEXT_BAD });
    else scr.info.push_back({ "Status", "Ready to play", col::TEXT_GOOD });
    if (!slots.empty()) {
        fs::path newest; for (auto& sl : slots) { std::error_code ec; if (newest.empty() || fs::last_write_time(sl / L"GAME.SAV", ec) > fs::last_write_time(newest / L"GAME.SAV", ec)) newest = sl; }
        scr.info.push_back({ "Last saved", file_date(newest / L"GAME.SAV") });
    }
    if (!en.empty()) scr.info.push_back({ "Mods", std::to_string(en.size()) + " on" });
    if (!last_session.empty()) scr.info.push_back({ "Last session", last_session, col::TEXT_DIM });
    const bool show_update = g_update_ready && !game_proc;
    if (game_proc) {
        scr.items.push_back(button("Playing", "Started " + format_duration((GetTickCount64() - game_start) / 1000) + " ago", [] {}, false));
        scr.items.push_back(button("Stop the game", "Unsaved progress is lost", stop_game));
    } else if (!gi.ok) {
        Item p = button("Set up the game", "Import your .iso and EBOOT.elf", [] { set_screen(import_screen); });
        p.accent = true; scr.items.push_back(p);
    } else {
        Item p = button("Play", "", play, b.version != 0);
        p.accent = true; scr.items.push_back(p);
    }
    if (show_update) scr.items.push_back(button("Update available", "See what is new, then download and install it", open_updates));
    scr.items.push_back(button("Settings", "Display, graphics, controller", [] { set_screen(settings_screen); }));
    scr.items.push_back(button("Saves", "Back up, export, restore", [] { set_screen(saves_screen); }));
    scr.items.push_back(button("More", "Your game, mods, logs", [] { set_screen(more_screen); }));
    scr.items.push_back(button("Quit", "", [] { PostMessageW(g_hwnd, WM_CLOSE, 0, 0); }));
}

static void more_screen()
{
    const BuildInfo b = S->current_build();
    scr = Screen(); scr.title = "More"; scr.subtitle = ""; scr.back = [] { set_screen(home); };
    scr.items.push_back(button("Your game", "Import it again, or use another copy", [] { set_screen(import_screen); }));
    scr.items.push_back(button("Mods", "", [] { set_screen(mods_screen); }));
    scr.items.push_back(button("Logs", "For bug reports", [] { set_screen(logs_screen); }));
    scr.items.push_back(button("Check for updates", g_update_ready ? "An update is available" : "", open_updates));
    scr.items.push_back(button("Help", "", [] { set_screen(help_screen); }));
    scr.items.push_back(button("Back", "", [] { set_screen(home); }));
}

// ---- the game process -------------------------------------------------------------------------
static void pad_close();
static void pad_open();

static void play()
{
    if (game_proc) return;
    // The controller goes to the game: SDL's HID access here would compete with it.
    pad_close();
    void* h = nullptr;
    const std::string r = S->launch(&h);
    if (!r.empty()) { pad_open(); message("Cannot start", r); return; }
    game_proc = (HANDLE)h;
    game_start = GetTickCount64();
    std::thread([h] {
        WaitForSingleObject((HANDLE)h, INFINITE);
        DWORD code = 0; GetExitCodeProcess((HANDLE)h, &code);
        PostMessageW(g_hwnd, WM_APP_GAME_EXIT, code, 0);
    }).detach();
    const std::string w = S->ini.get("while_playing", "minimize");
    if (w == "hide") ShowWindow(g_hwnd, SW_HIDE);
    else if (w != "stay") ShowWindow(g_hwnd, SW_MINIMIZE);
    refresh();
}

static void game_exited(DWORD code)
{
    S->collect_log();
    const uint64_t secs = (GetTickCount64() - game_start) / 1000;
    CloseHandle(game_proc); game_proc = nullptr;
    char hx[32]; snprintf(hx, sizeof hx, "0x%08lX", (unsigned long)code);
    // 0xC000001D, an illegal instruction: a CPU without AVX2 (the game's own check now says so
    // before it starts; builds before v96 died at start instead -- an i7-3770, v95).
    last_session = format_duration(secs) + ", " + (code == 0 ? std::string("ended normally") :
                   code == 1 ? std::string("stopped") :
                   code == 0xC000001Du ? std::string("this processor lacks AVX2, which the game needs (Intel 2013+ / AMD Ryzen)") :
                   std::string("ended with code ") + hx + " (send the log)");
    ShowWindow(g_hwnd, SW_RESTORE);
    SetForegroundWindow(g_hwnd);
    pad_open();
    if (scr.title == "A Crack in Time") set_screen(home);   // back on Play
    else refresh();
}

// ---- gamepad (SDL) ------------------------------------------------------------------------------
static void pad_open()
{
    if (!sdl_ok) return;
    if (SDL_InitSubSystem(SDL_INIT_GAMECONTROLLER) != 0) return;
    for (int i = 0; i < SDL_NumJoysticks() && !pad; i++)
        if (SDL_IsGameController(i)) pad = SDL_GameControllerOpen(i);
    if (pad) { const SDL_GameControllerType t = SDL_GameControllerGetType(pad);
               pad_ps = t == SDL_CONTROLLER_TYPE_PS3 || t == SDL_CONTROLLER_TYPE_PS4 || t == SDL_CONTROLLER_TYPE_PS5; }
}
static void pad_close()
{
    if (!sdl_ok) return;
    if (pad) { SDL_GameControllerClose(pad); pad = nullptr; }
    SDL_QuitSubSystem(SDL_INIT_GAMECONTROLLER);
}

// ---- input --------------------------------------------------------------------------------------
static void move_focus(int d)
{
    if (scr.items.empty()) return;
    int f = focus;
    for (int n = 0; n < (int)scr.items.size(); n++) {
        f += d;
        if (f < 0 || f >= (int)scr.items.size()) return;
        if (focusable(scr.items[f])) { focus = f; editing = false; redraw(); return; }
    }
}

static void activate()
{
    if (focus < 0 || focus >= (int)scr.items.size()) return;
    Item& it = scr.items[focus];
    if (!it.enabled) return;
    if (it.kind == K_FIELD) {
        if (!editing) { editing = true; edit_text = it.sub; }
        else { editing = false; if (it.commit) it.commit(edit_text); }
        redraw();
        return;
    }
    if (it.act) { auto fn = it.act; fn(); }
}

static void change(int d)
{
    if (focus < 0 || focus >= (int)scr.items.size()) return;
    Item& it = scr.items[focus];
    if (it.kind == K_OPTION && it.change && it.enabled) { auto fn = it.change; fn(d); }
}

static void go_back()
{
    if (editing) { editing = false; redraw(); return; }
    if (scr.back) { auto fn = scr.back; fn(); }
}

static void overlay_key(int key)   // VK_LEFT/RIGHT/RETURN/ESCAPE
{
    // Only a controller (and the test script) reaches here while waiting for an input:
    // the keyboard and mouse go to capture_done in wndproc. Circle / B cancels.
    if (ov.type == Overlay::CAPTURE) { if (key == VK_ESCAPE) { ov.type = Overlay::NONE; redraw(); } return; }
    if (ov.type == Overlay::PROGRESS) { if (key == VK_ESCAPE || key == VK_RETURN) prog->cancel = true; return; }
    if (ov.type == Overlay::CONFIRM) {
        if (key == VK_LEFT || key == VK_RIGHT || key == VK_TAB) { ov.focus ^= 1; redraw(); return; }
        if (key == VK_ESCAPE) { ov.type = Overlay::NONE; redraw(); return; }
        if (key == VK_RETURN) {
            const bool yes = ov.focus == 0; auto fn = ov.on_yes;
            ov.type = Overlay::NONE; redraw();
            if (yes && fn) fn();
        }
        return;
    }
    if (ov.type == Overlay::MESSAGE && (key == VK_RETURN || key == VK_ESCAPE)) {
        auto fn = ov.on_yes;
        ov.type = Overlay::NONE; redraw();
        if (fn) fn();
    }
}

static void key(int vk)
{
    if (ov.type != Overlay::NONE) { overlay_key(vk); return; }
    if (editing) {
        if (vk == VK_RETURN) activate();
        else if (vk == VK_ESCAPE) { editing = false; redraw(); }
        return;
    }
    switch (vk) {
    case VK_UP: move_focus(-1); break;
    case VK_DOWN: move_focus(+1); break;
    case VK_LEFT: change(-1); break;
    case VK_RIGHT: change(+1); break;
    case VK_RETURN: case VK_SPACE: activate(); break;
    case VK_ESCAPE: case VK_BACK: go_back(); break;
    case VK_PRIOR: for (int i = 0; i < 6; i++) move_focus(-1); break;
    case VK_NEXT: for (int i = 0; i < 6; i++) move_focus(+1); break;
    case VK_HOME: focus = 0; move_focus(+1); move_focus(-1); break;
    case VK_END: focus = (int)scr.items.size() - 1; move_focus(-1); move_focus(+1); break;
    }
}

static void pad_event(const SDL_Event& e)
{
    if (e.type == SDL_CONTROLLERDEVICEADDED && !pad) {
        pad = SDL_GameControllerOpen(e.cdevice.which);
        if (pad) { const SDL_GameControllerType t = SDL_GameControllerGetType(pad);
                   pad_ps = t == SDL_CONTROLLER_TYPE_PS3 || t == SDL_CONTROLLER_TYPE_PS4 || t == SDL_CONTROLLER_TYPE_PS5; redraw(); }
        return;
    }
    if (e.type == SDL_CONTROLLERDEVICEREMOVED && pad && SDL_GameControllerFromInstanceID(e.cdevice.which) == pad) {
        SDL_GameControllerClose(pad); pad = nullptr; return;
    }
    if (e.type != SDL_CONTROLLERBUTTONDOWN || GetForegroundWindow() != g_hwnd) return;
    switch (e.cbutton.button) {
    case SDL_CONTROLLER_BUTTON_DPAD_UP: key(VK_UP); break;
    case SDL_CONTROLLER_BUTTON_DPAD_DOWN: key(VK_DOWN); break;
    case SDL_CONTROLLER_BUTTON_DPAD_LEFT: key(VK_LEFT); break;
    case SDL_CONTROLLER_BUTTON_DPAD_RIGHT: key(VK_RIGHT); break;
    case SDL_CONTROLLER_BUTTON_A: key(VK_RETURN); break;
    case SDL_CONTROLLER_BUTTON_B: key(VK_ESCAPE); break;
    case SDL_CONTROLLER_BUTTON_START:
        if (ov.type == Overlay::NONE && scr.build && !game_proc && S->game().ok && S->current_build().version) play();
        break;
    }
}

static void pad_poll()
{
    if (!sdl_ok) return;
    SDL_Event e;
    while (SDL_PollEvent(&e)) pad_event(e);
    if (!pad || GetForegroundWindow() != g_hwnd) return;
    const int y = SDL_GameControllerGetAxis(pad, SDL_CONTROLLER_AXIS_LEFTY), x = SDL_GameControllerGetAxis(pad, SDL_CONTROLLER_AXIS_LEFTX);
    const int d = y < -20000 ? VK_UP : y > 20000 ? VK_DOWN : x < -20000 ? VK_LEFT : x > 20000 ? VK_RIGHT : 0;
    const ULONGLONG now = GetTickCount64();
    if (d != pad_dir) { pad_dir = d; if (d) { key(d); pad_next = now + 380; } }
    else if (d && now >= pad_next) { key(d); pad_next = now + 120; }
}

// ---- layout and drawing ---------------------------------------------------------------------------
static const float M = 40, HEADER_H = 150, FOOTER_H = 52;

static Rect right_col() { const float lw = std::max(380.0f, g.width() * 0.42f); return { M + lw + 32, HEADER_H, g.width() - M, g.height() - FOOTER_H }; }
static Rect left_col() { const float lw = std::max(380.0f, g.width() * 0.42f); return { M, HEADER_H, M + lw, g.height() - FOOTER_H }; }

static float item_h(const Item& i)
{
    if (i.kind == K_HEADER) return 34;
    if (i.kind == K_OPTION) return 50;
    if (i.kind == K_FIELD) return 64;
    return i.sub.empty() ? 52 : 66;
}

static void layout()
{
    const Rect rc = right_col();
    float y = rc.t - scroll;
    for (auto& i : scr.items) { const float h = item_h(i); i.rc = { rc.l, y, rc.r, y + h }; y += h + 8; }
    // keep the focused item in view
    if (focus >= 0 && focus < (int)scr.items.size()) {
        const Rect& f = scr.items[focus].rc;
        if (f.b > rc.b - 4) scroll_to = scroll + (f.b - (rc.b - 4));
        if (f.t < rc.t + 4) scroll_to = scroll - ((rc.t + 4) - f.t);
        const float content = y + scroll - rc.t;
        scroll_to = std::max(0.0f, std::min(scroll_to, std::max(0.0f, content - rc.h())));
    }
}

static void draw_glyph_hint(float x, float cy, int which, const char* text)   // 0 cross, 1 circle, 2 dpad, 3 start
{
    const float r = 9;
    if (pad_ps) {
        if (which == 0) { g.circle(x + r, cy, r, col::TEXT_DIM, 1.2f); g.line(x + r - 4, cy - 4, x + r + 4, cy + 4, 0xFF7FB2FF, 1.8f); g.line(x + r + 4, cy - 4, x + r - 4, cy + 4, 0xFF7FB2FF, 1.8f); }
        if (which == 1) { g.circle(x + r, cy, r, col::TEXT_DIM, 1.2f); g.circle(x + r, cy, 4.5f, 0xFFFF6E6E, 1.8f); }
    } else {
        if (which == 0) { g.circle(x + r, cy, r, 0xFF6CCB5F, 1.6f); g.text("A", { x, cy - r, x + 2 * r, cy + r }, F_HINT, 0xFF6CCB5F, A_CENTER); }
        if (which == 1) { g.circle(x + r, cy, r, 0xFFE0504A, 1.6f); g.text("B", { x, cy - r, x + 2 * r, cy + r }, F_HINT, 0xFFE0504A, A_CENTER); }
    }
    if (which == 2) { g.fill({ x + r - 2.5f, cy - r, x + r + 2.5f, cy + r }, col::TEXT_DIM); g.fill({ x, cy - 2.5f, x + 2 * r, cy + 2.5f }, col::TEXT_DIM); }
    const float tw = g.text_width(text, F_HINT);
    g.text(text, { x + 2 * r + 6, cy - 12, x + 2 * r + 10 + tw, cy + 12 }, F_HINT, col::TEXT_DIM);
}

static void draw_background()
{
    const float W = g.width(), H = g.height();
    g.vgradient({ 0, 0, W, H }, col::BG_TOP, col::BG_BOTTOM);
    // The game's own key art, full strength on the right; darkened only where
    // the text sits.
    if (g.image(S->background(), { 0, 0, W, H }, 1.0f, true)) {
        g.hgradient({ 0, 0, W, H }, 0xE806141A, 0x3006141A);
        g.vgradient({ 0, 0, W, 150 }, 0xA006141A, 0x0006141A);
        g.vgradient({ 0, H - 90, W, H }, 0x0006141A, 0xD006141A);
    }
}

// The time, as the Great Clock keeps it: a brass face with Zoni-teal marks,
// hour, minute and a second hand that ticks each second (the local clock).
static void draw_clock(float cx, float cy, float R)
{
    SYSTEMTIME st; GetLocalTime(&st);
    g.hexagon(cx, cy, R * 1.08f, 0xB0081519, 0xFFFFB43C, 2.0f);
    g.circle(cx, cy, R * 0.94f, 0xA0FFB43C, 1.2f);
    g.circle(cx, cy, R * 0.80f, 0x506FE6F0, 1.0f);
    for (int k = 0; k < 60; k++) {
        const float a = k * 6.2831853f / 60.0f - 1.5707963f, big = k % 5 == 0;
        const float r0 = R * (big ? 0.76f : 0.85f), r1 = R * 0.92f;
        g.line(cx + cosf(a) * r0, cy + sinf(a) * r0, cx + cosf(a) * r1, cy + sinf(a) * r1,
               big ? 0xFFFFB43C : 0x906FE6F0, big ? 2.2f : 1.0f);
    }
    const float sec = (float)st.wSecond, mn = st.wMinute + sec / 60.0f, hr = (st.wHour % 12) + mn / 60.0f;
    auto hand = [&](float turns, float len, float back, uint32_t c, float w) {
        const float a = turns * 6.2831853f - 1.5707963f;
        g.line(cx - cosf(a) * back, cy - sinf(a) * back, cx + cosf(a) * len, cy + sinf(a) * len, c, w);
    };
    hand(hr / 12.0f, R * 0.48f, R * 0.08f, 0xFFF4FBFF, 4.5f);
    hand(mn / 60.0f, R * 0.70f, R * 0.10f, 0xFFF4FBFF, 3.0f);
    hand(sec / 60.0f, R * 0.84f, R * 0.18f, 0xFF6FE6F0, 1.6f);
    g.hexagon(cx, cy, R * 0.07f, 0xFFFFB43C, 0xFFFFB43C, 1.0f);
    char t[16]; snprintf(t, sizeof t, "%02d:%02d:%02d", st.wHour, st.wMinute, st.wSecond);
    g.text(t, { cx - R, cy + R * 1.12f, cx + R, cy + R * 1.12f + 26 }, F_BUTTON_SUB, col::TEXT_DIM, A_CENTER);
}

static void draw_header()
{
    /* The main screen, once the game is in: the game's own logo (its
     * ICON0.PNG, copied to assets\ by refresh_art) in place of the title text.
     * The disc's PIC0.PNG is blank, so not that one. */
    if (scr.title == "A Crack in Time") {
        float iw = 0, ih = 0;
        if (g.image_size(S->icon(), &iw, &ih) && ih > 0) {
            const float h = 112, w = h * iw / ih;
            if (g.image(S->icon(), { M - 8, 12, M - 8 + w, 12 + h }, 1.0f, false)) { draw_clock(g.width() - 92, 70, 46); return; }
        }
    }
    g.text("RATCHET & CLANK FUTURE", { M, 30, g.width() - 260, 52 }, F_BRAND, col::ORANGE);
    std::string t = scr.title; for (auto& c : t) c = (char)toupper((unsigned char)c);
    g.text(t, { M, 48, g.width() - 260, 100 }, F_TITLE, col::TEXT);
    if (!scr.subtitle.empty()) g.text(scr.subtitle, { M, 98, g.width() - 260, 124 }, F_SUB, col::TEXT_DIM);
    draw_clock(g.width() - 92, 70, 46);
}

static void draw_info()
{
    const Rect lc = left_col();
    float h = 26;
    const float tw = lc.w() - 48;
    for (auto& i : scr.info) h += 20 + g.text_height(i.value, F_BODY, tw) + 14;
    Rect p = { lc.l, lc.t, lc.r, std::min(lc.b, lc.t + h) };
    g.chamfer_fill(p, 18, col::PANEL_DARK);
    g.chamfer_stroke(p, 18, col::EDGE_DIM, 1.5f);
    g.clip_push(p);
    float y = p.t + 18;
    for (auto& i : scr.info) {
        std::string lab = i.label; for (auto& c : lab) c = (char)toupper((unsigned char)c);
        g.text(lab, { p.l + 24, y, p.r - 24, y + 20 }, F_LABEL, col::EDGE);
        y += 20;
        const float th = g.text_height(i.value, F_BODY, tw);
        g.text(i.value, { p.l + 24, y, p.r - 24, y + th + 2 }, F_BODY, i.color, A_LEFT, true, false);
        y += th + 14;
    }
    g.clip_pop();
}

static void draw_items()
{
    const Rect rc = right_col();
    const float pulse = 0.65f + 0.35f * (float)sin((GetTickCount64() - t0) / 260.0);
    g.clip_push({ rc.l - 20, rc.t - 6, rc.r + 20, rc.b + 4 });
    for (int idx = 0; idx < (int)scr.items.size(); idx++) {
        const Item& i = scr.items[idx];
        const Rect& r = i.rc;
        if (r.b < rc.t - 10 || r.t > rc.b + 10) continue;
        if (i.kind == K_HEADER) {
            std::string lab = i.label; for (auto& c : lab) c = (char)toupper((unsigned char)c);
            g.text(lab, { r.l + 4, r.t + 6, r.r, r.b }, F_LABEL, col::ORANGE);
            g.line(r.l + 4, r.b - 3, r.r, r.b - 3, 0x60FFB43C, 1);
            continue;
        }
        const bool f = idx == focus, en = i.enabled;
        const uint32_t edge = f ? col::ORANGE : i.accent && en ? col::ORANGE : en ? col::EDGE : col::EDGE_DIM;
        if (f) g.chamfer_glow(r, 14, (uint32_t)(0xA0 * pulse) << 24 | 0xFFB43C, 2);
        else if (i.accent && en) g.chamfer_glow(r, 14, col::ORANGE_GLOW & 0x60FFFFFF, 2);
        g.chamfer_fill(r, 14, f ? col::PANEL_HOT : col::PANEL);
        g.chamfer_stroke(r, 14, edge, f ? 2.2f : 1.5f);
        const uint32_t tc = en ? col::TEXT : col::TEXT_OFF, sc = en ? col::TEXT_DIM : col::TEXT_OFF;
        // a small hexagon bullet
        g.hexagon(r.l + 24, (r.t + r.b) / 2, 7, f ? col::ORANGE : 0x00000000, f ? col::ORANGE : edge, 1.3f);
        const float tx = r.l + 44;
        if (i.kind == K_OPTION) {
            const float vw = std::min(r.w() * 0.55f, g.text_width(i.sub, F_BUTTON_SUB) + (f ? 44 : 8));
            g.text(i.label, { tx, r.t, r.r - vw - 16, r.b }, F_BUTTON, tc);
            const std::string v = f ? "<  " + i.sub + "  >" : i.sub;
            g.text(v, { r.r - vw - 18, r.t, r.r - 18, r.b }, F_BUTTON_SUB, f ? col::ORANGE : 0xFFFFCD8C, A_RIGHT);
        } else if (i.kind == K_FIELD) {
            g.text(i.label, { tx, r.t + 6, r.r - 18, r.t + 32 }, F_BUTTON, tc);
            const std::string v = editing && f ? edit_text + ((GetTickCount64() / 500) % 2 ? "|" : " ") :
                                  i.sub.empty() ? (f ? "Enter to type an address" : "Not set") : i.sub;
            g.text(v, { tx, r.t + 34, r.r - 18, r.b - 6 }, F_BUTTON_SUB, editing && f ? col::ORANGE : col::TEXT_DIM);
        } else if (i.sub.empty()) {
            g.text(i.label, { tx, r.t, r.r - 18, r.b }, F_BUTTON, tc);
        } else {
            g.text(i.label, { tx, r.t + 6, r.r - 18, r.t + 36 }, F_BUTTON, tc);
            g.text(i.sub, { tx, r.t + 36, r.r - 18, r.b - 6 }, F_BUTTON_SUB, sc);
        }
    }
    g.clip_pop();
    // scroll marks
    if (!scr.items.empty() && scr.items.front().rc.t < rc.t - 1) g.fill({ rc.l, rc.t - 6, rc.r, rc.t - 4 }, 0x806FE6F0);
    if (!scr.items.empty() && scr.items.back().rc.b > rc.b + 1) g.fill({ rc.l, rc.b + 2, rc.r, rc.b + 4 }, 0x806FE6F0);
}

static void draw_footer()
{
    const float y = g.height() - FOOTER_H / 2;
    float x = M;
    draw_glyph_hint(x, y, 2, "Choose / change"); x += 170;
    draw_glyph_hint(x, y, 0, pad_ps ? "Select  (Enter)" : "Select  (Enter)"); x += 150;
    draw_glyph_hint(x, y, 1, "Back  (Esc)");
    g.text(std::string("v") + kLauncherVersion, { g.width() - 200, y - 12, g.width() - M, y + 12 }, F_HINT, col::TEXT_OFF, A_RIGHT);
}

static void draw_overlay()
{
    if (ov.type == Overlay::NONE) return;
    const float W = g.width(), H = g.height();
    g.fill({ 0, 0, W, H }, 0xB0050A14);
    const float pw = std::min(720.0f, W - 80);
    const float tw = pw - 64;
    std::string text = ov.text;
    if (ov.type == Overlay::PROGRESS) {
        const uint64_t d = prog->done, t = prog->total;
        text = prog->line();
        if (t) text += "\n" + format_size(d) + " of " + format_size(t);
    }
    if (ov.type == Overlay::CAPTURE) {
        const ULONGLONG now = GetTickCount64();
        text += "\n\nCancels in " + std::to_string(ov.deadline > now ? (int)((ov.deadline - now + 999) / 1000) : 0) +
                " s, or press Circle (B) on a controller.";
    }
    const float th = std::min(H * 0.5f, g.text_height(text, F_BODY, tw));
    const float ph = 70 + th + (ov.type == Overlay::PROGRESS ? 40 : 0) + 84;
    const Rect p = { (W - pw) / 2, (H - ph) / 2, (W + pw) / 2, (H + ph) / 2 };
    g.chamfer_glow(p, 22, col::GLOW, 2);
    g.chamfer_fill(p, 22, 0xF40A182B);
    g.chamfer_stroke(p, 22, col::EDGE, 2);
    std::string t = ov.title; for (auto& c : t) c = (char)toupper((unsigned char)c);
    g.text(t, { p.l + 32, p.t + 18, p.r - 32, p.t + 60 }, F_BIG, col::TEXT);
    g.text(text, { p.l + 32, p.t + 70, p.r - 32, p.t + 70 + th + 4 }, F_BODY, col::TEXT_DIM, A_LEFT, true, false);
    float by = p.t + 70 + th + 20;
    if (ov.type == Overlay::PROGRESS) {
        const Rect bar = { p.l + 32, by, p.r - 32, by + 16 };
        g.chamfer_stroke(bar, 6, col::EDGE_DIM, 1.2f);
        const uint64_t d = prog->done, tt = prog->total;
        if (tt) {
            const float f = std::min(1.0f, (float)((double)d / (double)tt));
            g.chamfer_fill({ bar.l + 3, bar.t + 3, bar.l + 3 + (bar.w() - 6) * f, bar.b - 3 }, 4, col::ORANGE);
        } else {   // unknown length: a sweeping block
            const float ph2 = (float)((GetTickCount64() - t0) % 1600) / 1600.0f, bw = bar.w() * 0.2f;
            const float x0 = bar.l + 3 + (bar.w() - 6 - bw) * (0.5f + 0.5f * (float)sin(ph2 * 6.2831853f));
            g.chamfer_fill({ x0, bar.t + 3, x0 + bw, bar.b - 3 }, 4, col::ORANGE);
        }
        by += 40;
    }
    const int nb = ov.type == Overlay::CONFIRM ? 2 : ov.type == Overlay::CAPTURE ? 0 : 1;
    const float bw = 200, gap = 18;
    float bx = p.r - 32 - nb * bw - (nb - 1) * gap;
    for (int k = 0; k < nb; k++) {
        const std::string lab = ov.type == Overlay::PROGRESS ? (prog->cancel ? "Cancelling..." : "Cancel") : k == 0 ? ov.yes : ov.no;
        const Rect b = { bx, by, bx + bw, by + 50 };
        ov.btn[k] = b;
        const bool f = ov.type == Overlay::CONFIRM ? ov.focus == k : true;
        if (f) g.chamfer_glow(b, 12, col::ORANGE_GLOW, 2);
        g.chamfer_fill(b, 12, f ? col::PANEL_HOT : col::PANEL);
        g.chamfer_stroke(b, 12, f ? col::ORANGE : col::EDGE, 1.8f);
        g.text(lab, b, F_BUTTON, col::TEXT, A_CENTER);
        bx += bw + gap;
    }
}

static void draw_frame()
{
    layout();
    draw_background();
    draw_header();
    draw_info();
    draw_items();
    draw_footer();
    draw_overlay();
}

static void paint()
{
    if (!g.begin()) return;
    // ease the scroll toward its target
    scroll += (scroll_to - scroll) * 0.35f;
    if (fabsf(scroll_to - scroll) < 0.5f) scroll = scroll_to;
    draw_frame();
    g.end();
}

// ---- test hook --------------------------------------------------------------------------------------
// RACACIT_LAUNCHER_SCRIPT=<file>: one command per line, consumed one at a time
// (the game's RACACIT_UI_FILE in spirit): key UP|DOWN|LEFT|RIGHT|ENTER|ESC,
// pick <path> (the next file/folder dialog answers that), shot <file.png>,
// wait <ms>, quit; scan <hex> [ext] and mouse <1-5|wheelup|wheeldown> send real
// keyboard and mouse messages (the Key bindings capture).
static void draw_frame();
static void save_shot(const std::wstring& path) { g.snapshot(path, draw_frame); }

static void script_poll()
{
    static const char* path = (const char*)1;
    if (path == (const char*)1) path = getenv("RACACIT_LAUNCHER_SCRIPT");
    if (!path || prog) return;
    static ULONGLONG next;
    if (GetTickCount64() < next) return;
    next = GetTickCount64() + 60;
    const fs::path p = upath(path);
    std::string t = read_text(p);
    if (t.empty()) return;
    const size_t nl = t.find('\n');
    std::string line = trim(t.substr(0, nl));
    write_text_atomic(p, nl == std::string::npos ? std::string() : t.substr(nl + 1));
    const size_t sp = line.find(' ');
    const std::string cmd = line.substr(0, sp), arg = sp == std::string::npos ? "" : trim(line.substr(sp + 1));
    if (cmd == "key") {
        static const std::pair<const char*, int> keys[] = { { "UP", VK_UP }, { "DOWN", VK_DOWN }, { "LEFT", VK_LEFT }, { "RIGHT", VK_RIGHT },
                                                            { "ENTER", VK_RETURN }, { "ESC", VK_ESCAPE }, { "HOME", VK_HOME }, { "END", VK_END } };
        for (auto& k : keys) if (arg == k.first) key(k.second);
    } else if (cmd == "pick") queue_pick(upath(arg));
    else if (cmd == "shot") save_shot(widen(arg));
    else if (cmd == "wait") next = GetTickCount64() + (ULONGLONG)atoi(arg.c_str());
    else if (cmd == "type") { edit_text += arg; redraw(); }
    else if (cmd == "scan") {   // scan <hex scan code> [ext]: a real WM_KEYDOWN, as the keyboard sends it
        const unsigned sc = (unsigned)strtoul(arg.c_str(), nullptr, 16);
        PostMessageW(g_hwnd, WM_KEYDOWN, MapVirtualKeyW(sc, MAPVK_VSC_TO_VK), (LPARAM)(1 | sc << 16 | (arg.find("ext") != std::string::npos ? 1 << 24 : 0)));
    } else if (cmd == "mouse") {   // mouse 1-5 | wheelup | wheeldown
        const int b = atoi(arg.c_str());
        if (arg == "wheelup" || arg == "wheeldown") PostMessageW(g_hwnd, WM_MOUSEWHEEL, MAKEWPARAM(0, arg == "wheelup" ? 120 : -120), 0);
        else if (b == 1) { PostMessageW(g_hwnd, WM_LBUTTONDOWN, 0, 0); PostMessageW(g_hwnd, WM_LBUTTONUP, 0, 0); }
        else if (b == 2) { PostMessageW(g_hwnd, WM_MBUTTONDOWN, 0, 0); PostMessageW(g_hwnd, WM_MBUTTONUP, 0, 0); }
        else if (b == 3) { PostMessageW(g_hwnd, WM_RBUTTONDOWN, 0, 0); PostMessageW(g_hwnd, WM_RBUTTONUP, 0, 0); }
        else if (b == 4 || b == 5) { PostMessageW(g_hwnd, WM_XBUTTONDOWN, MAKEWPARAM(0, b == 4 ? XBUTTON1 : XBUTTON2), 0);
                                     PostMessageW(g_hwnd, WM_XBUTTONUP, MAKEWPARAM(0, b == 4 ? XBUTTON1 : XBUTTON2), 0); }
    }
    else if (cmd == "quit") PostMessageW(g_hwnd, WM_CLOSE, 0, 0);
}

// ---- window ----------------------------------------------------------------------------------------
static float to_dip(int px) { return px * 96.0f / g.dpi(); }

static int hit_item(float x, float y)
{
    if (!right_col().in(x, y)) return -1;
    for (int i = 0; i < (int)scr.items.size(); i++) if (focusable(scr.items[i]) && scr.items[i].rc.in(x, y)) return i;
    return -1;
}

static LRESULT CALLBACK wndproc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    switch (m) {
    case WM_PAINT: { PAINTSTRUCT ps; BeginPaint(h, &ps); paint(); EndPaint(h, &ps); return 0; }
    case WM_ERASEBKGND: return 1;
    case WM_SIZE: g.resize(); redraw(); return 0;
    case WM_GETMINMAXINFO: { auto* mm = (MINMAXINFO*)l; const float s = g.dpi() / 96.0f; mm->ptMinTrackSize = { (LONG)(960 * s), (LONG)(600 * s) }; return 0; }
    case WM_DPICHANGED: {
        g.set_dpi((float)HIWORD(w));
        const RECT* r = (const RECT*)l;
        SetWindowPos(h, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE);
        return 0; }
    case WM_TIMER:
        if (ov.type == Overlay::CAPTURE && GetTickCount64() > ov.deadline) { ov.type = Overlay::NONE; redraw(); }
        pad_poll();
        script_poll();
        if (!IsIconic(h) && IsWindowVisible(h)) {
            // 30 Hz only while the launcher is in front and the game is not running (the
            // focus glow, scrolling, typing); otherwise once a second for the clock, so a
            // launcher left open beside the game takes no GPU time from it.
            static ULONGLONG last;
            const ULONGLONG now = GetTickCount64();
            const bool lively = (!game_proc && GetForegroundWindow() == h) || ov.type == Overlay::PROGRESS || scroll != scroll_to;
            if (lively || now - last >= 1000) {
                last = now;
                // "Playing: started ... ago" in place; rebuilding the screen read the game
                // folder, the saves and the builds from disk every second.
                if (game_proc)
                    for (auto& it : scr.items)
                        if (it.label == "Playing" && !it.enabled) it.sub = "Started " + format_duration((now - game_start) / 1000) + " ago";
                redraw();
            }
        }
        return 0;
    case WM_KEYDOWN: case WM_SYSKEYDOWN:
        if (ov.type == Overlay::CAPTURE) { if (!(l & (1 << 30))) capture_done(key_name_from_scan(l)); return 0; }   // not a held key's repeat
        if (m == WM_SYSKEYDOWN) break;
        key((int)w); return 0;
    case WM_LBUTTONDOWN: case WM_RBUTTONDOWN: case WM_MBUTTONDOWN: case WM_XBUTTONDOWN:
        if (ov.type == Overlay::CAPTURE) {   // SDL's numbering: 1 left, 2 middle, 3 right, 4/5 the side buttons
            s_swallow_up = true;
            capture_done(m == WM_LBUTTONDOWN ? "Mouse1" : m == WM_MBUTTONDOWN ? "Mouse2" : m == WM_RBUTTONDOWN ? "Mouse3"
                         : HIWORD(w) == XBUTTON1 ? "Mouse4" : "Mouse5");
            return m == WM_XBUTTONDOWN ? TRUE : 0;
        }
        break;
    case WM_CHAR:
        if (editing && ov.type == Overlay::NONE) {
            if (w == 8) { if (!edit_text.empty()) { do edit_text.pop_back(); while (!edit_text.empty() && ((unsigned char)edit_text.back() & 0xC0) == 0x80); } }
            else if (w == 22) {   // Ctrl+V
                if (OpenClipboard(h)) {
                    if (HANDLE d = GetClipboardData(CF_UNICODETEXT)) { if (auto* t = (const wchar_t*)GlobalLock(d)) { edit_text += narrow(t); GlobalUnlock(d); } }
                    CloseClipboard();
                }
            } else if (w >= 32) { wchar_t c[2] = { (wchar_t)w, 0 }; edit_text += narrow(c); }
            redraw();
        }
        return 0;
    case WM_MOUSEMOVE: {
        const float x = to_dip(GET_X_LPARAM(l)), y = to_dip(GET_Y_LPARAM(l));
        if (ov.type == Overlay::CONFIRM) { for (int k = 0; k < 2; k++) if (ov.btn[k].in(x, y) && ov.focus != k) { ov.focus = k; redraw(); } return 0; }
        if (ov.type != Overlay::NONE) return 0;
        const int i = hit_item(x, y);
        if (i >= 0 && i != hover) { hover = i; if (focus != i) { focus = i; editing = false; } redraw(); }
        if (i < 0) hover = -1;
        return 0; }
    case WM_LBUTTONUP: {
        if (s_swallow_up) { s_swallow_up = false; return 0; }   // the press was a binding
        const float x = to_dip(GET_X_LPARAM(l)), y = to_dip(GET_Y_LPARAM(l));
        if (ov.type == Overlay::PROGRESS) { if (ov.btn[0].in(x, y)) prog->cancel = true; return 0; }
        if (ov.type == Overlay::MESSAGE) { if (ov.btn[0].in(x, y)) overlay_key(VK_RETURN); return 0; }
        if (ov.type == Overlay::CONFIRM) {
            for (int k = 0; k < 2; k++) if (ov.btn[k].in(x, y)) { ov.focus = k; overlay_key(VK_RETURN); }
            return 0;
        }
        const int i = hit_item(x, y);
        if (i < 0) return 0;
        focus = i;
        Item& it = scr.items[i];
        if (it.kind == K_OPTION && it.change) { change(+1); return 0; }   // a click steps to the next value
        activate();
        return 0; }
    case WM_RBUTTONUP: if (s_swallow_up) { s_swallow_up = false; return 0; } if (ov.type == Overlay::NONE) go_back(); return 0;
    case WM_MBUTTONUP: case WM_XBUTTONUP: s_swallow_up = false; break;
    case WM_MOUSEWHEEL:
        if (ov.type == Overlay::CAPTURE) { capture_done(GET_WHEEL_DELTA_WPARAM(w) > 0 ? "WheelUp" : "WheelDown"); return 0; }
        if (ov.type == Overlay::NONE) { scroll_to -= GET_WHEEL_DELTA_WPARAM(w) / 120.0f * 90.0f; if (scroll_to < 0) scroll_to = 0; redraw(); }
        return 0;
    case WM_APP_TASK: {
        std::string* r = (std::string*)l;
        if (worker.joinable()) worker.join();
        delete prog; prog = nullptr;
        ov.type = Overlay::NONE;
        auto fn = std::move(task_done); task_done = nullptr;
        if (fn) fn(*r); else refresh();
        delete r;
        redraw();
        return 0; }
    case WM_APP_GAME_EXIT: game_exited((DWORD)w); return 0;
    case WM_APP_UPDATE: {
        ReleaseInfo* rel = (ReleaseInfo*)l;
        remember_release(*rel);
        delete rel;
        if (!prog && (scr.title == "A Crack in Time" || scr.title == "Updates" || scr.title == "More")) refresh();
        return 0; }
    case WM_CLOSE:
        if (prog) { prog->cancel = true; return 0; }   // let the job stop first
        DestroyWindow(h);
        return 0;
    case WM_DESTROY: PostQuitMessage(0); return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR, int show)
{
    // An update tries a new launcher this way before it replaces the running one.
    if (wcsstr(GetCommandLineW(), L" --self-test")) return 0;
    // One launcher at a time.
    HANDLE mtx = getenv("RACACIT_LAUNCHER_SCRIPT") ? nullptr : CreateMutexW(nullptr, TRUE, L"RaCACiT-Launcher-single");   /* scripted test runs: any number */
    if (mtx && GetLastError() == ERROR_ALREADY_EXISTS) {
        if (HWND o = FindWindowW(L"RaCACiTLauncher", nullptr)) { ShowWindow(o, SW_RESTORE); SetForegroundWindow(o); }
        return 0;
    }
    typedef BOOL (WINAPI *SetCtx)(DPI_AWARENESS_CONTEXT);
    if (auto f = (SetCtx)GetProcAddress(GetModuleHandleW(L"user32.dll"), "SetProcessDpiAwarenessContext")) f(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    Store store(exe_dir());
    S = &store;
    store.clean_update_leftovers();

    WNDCLASSEXW wc{}; wc.cbSize = sizeof wc; wc.lpfnWndProc = wndproc; wc.hInstance = inst;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW); wc.lpszClassName = L"RaCACiTLauncher";
    wc.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(1)); wc.hIconSm = wc.hIcon;
    RegisterClassExW(&wc);
    const UINT dpi = GetDpiForSystem();
    const int ww = MulDiv(1280, dpi, 96), wh = MulDiv(800, dpi, 96);
    RECT wa; SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0);
    g_hwnd = CreateWindowExW(0, wc.lpszClassName, L"Ratchet & Clank Future: A Crack in Time", WS_OVERLAPPEDWINDOW,
                             wa.left + std::max(0, (int)(wa.right - wa.left - ww) / 2), wa.top + std::max(0, (int)(wa.bottom - wa.top - wh) / 2),
                             std::min(ww, (int)(wa.right - wa.left)), std::min(wh, (int)(wa.bottom - wa.top)), nullptr, nullptr, inst, nullptr);
    if (!g_hwnd || !g.init(g_hwnd)) { MessageBoxW(nullptr, L"Direct2D is not available.", L"RACiT Launcher", MB_ICONERROR); return 1; }

    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "0");
    SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI_PS5_PLAYER_LED, "0");
    sdl_ok = SDL_Init(0) == 0;
    pad_open();

    if (S->game().ok && !fs::exists(S->logo())) S->refresh_art();   // refresh_art_once: art from before 0.4
    set_screen(home);
    if (!S->game().ok) set_screen(import_screen);   // first run: where the game comes from
    ShowWindow(g_hwnd, show == SW_HIDE ? SW_SHOWNORMAL : show);
    SetTimer(g_hwnd, TIMER_TICK, 33, nullptr);
    quiet_update_check();

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) { TranslateMessage(&msg); DispatchMessageW(&msg); }

    if (worker.joinable()) worker.join();
    pad_close();
    if (sdl_ok) SDL_Quit();
    g.shutdown();
    CoUninitialize();
    if (mtx) CloseHandle(mtx);   // first: the new launcher takes it
    if (g_restart) {
        STARTUPINFOW si{}; si.cb = sizeof si; PROCESS_INFORMATION pi{};
        const fs::path self = exe_path();
        std::wstring cl = L"\"" + self.wstring() + L"\"";
        if (CreateProcessW(self.c_str(), cl.data(), nullptr, nullptr, FALSE, 0, nullptr, self.parent_path().c_str(), &si, &pi)) { CloseHandle(pi.hThread); CloseHandle(pi.hProcess); }
        else MessageBoxW(nullptr, L"The launcher was updated. Please open it again.", L"RACiT Launcher", MB_ICONINFORMATION);
    }
    return 0;
}
