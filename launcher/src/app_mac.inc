// macOS: the launcher's main() -- wWinMain's counterpart, included at the end of app.cpp so
// it reaches the same screens and window procedure. One SDL window (SDL is the launcher's
// controller library already); its events become the messages wndproc takes from Windows,
// in the same units: positions in pixels (wndproc turns them into points with g.dpi()), keys
// as VK_ codes plus SDL's scan code (for the Key bindings capture), text as WM_CHAR.
//
// Hidden flags, for checks without a window:
//   --self-test            start, load SDL, leave with 0 (an update tries a new launcher so)
//   --root <folder>        the launcher folder (default: the folder the .app is in)
//   --snapshot <folder>    every screen and overlay drawn offscreen to PNGs; no window

static int mac_vk(SDL_Scancode sc)
{
    switch (sc) {
    case SDL_SCANCODE_UP: return VK_UP;          case SDL_SCANCODE_DOWN: return VK_DOWN;
    case SDL_SCANCODE_LEFT: return VK_LEFT;      case SDL_SCANCODE_RIGHT: return VK_RIGHT;
    case SDL_SCANCODE_RETURN: case SDL_SCANCODE_KP_ENTER: return VK_RETURN;
    case SDL_SCANCODE_ESCAPE: return VK_ESCAPE;  case SDL_SCANCODE_SPACE: return VK_SPACE;
    case SDL_SCANCODE_BACKSPACE: return VK_BACK; case SDL_SCANCODE_TAB: return VK_TAB;
    case SDL_SCANCODE_PAGEUP: return VK_PRIOR;   case SDL_SCANCODE_PAGEDOWN: return VK_NEXT;
    case SDL_SCANCODE_HOME: return VK_HOME;      case SDL_SCANCODE_END: return VK_END;
    default: return 0;
    }
}

// A position in the window (points) as WM_MOUSEMOVE's lParam (pixels).
static LPARAM mac_xy(int x, int y)
{
    const float s = g.dpi() / 96.0f;
    return MAKELPARAM((int)lroundf(x * s), (int)lroundf(y * s));
}

static void mac_event(const SDL_Event& e)
{
    switch (e.type) {
    case SDL_QUIT: wndproc(g_hwnd, WM_CLOSE, 0, 0); return;   // the close button, Cmd+Q
    case SDL_WINDOWEVENT:
        switch (e.window.event) {
        case SDL_WINDOWEVENT_SIZE_CHANGED: case SDL_WINDOWEVENT_RESIZED: case SDL_WINDOWEVENT_DISPLAY_CHANGED:
            wndproc(g_hwnd, WM_SIZE, 0, 0); break;   // Gfx::resize reads the new scale too (WM_DPICHANGED)
        case SDL_WINDOWEVENT_EXPOSED: case SDL_WINDOWEVENT_RESTORED: case SDL_WINDOWEVENT_SHOWN: mac::dirty = true; break;
        }
        return;
    case SDL_KEYDOWN: {
        const SDL_Scancode sc = e.key.keysym.scancode;
        const LPARAM l = (LPARAM)(1 | (unsigned)sc << 16 | (e.key.repeat ? 1u << 30 : 0u));
        if (ov.type == Overlay::CAPTURE) { wndproc(g_hwnd, WM_KEYDOWN, (WPARAM)mac_vk(sc), l); return; }   // any key, Cmd too
        if (e.key.keysym.mod & KMOD_GUI) {   // Cmd+V pastes (Ctrl+V's WM_CHAR 22); other Cmd keys are the system's
            if (sc == SDL_SCANCODE_V) wndproc(g_hwnd, WM_CHAR, 22, 0);
            return;
        }
        if (const int vk = mac_vk(sc)) wndproc(g_hwnd, WM_KEYDOWN, (WPARAM)vk, l);
        if (sc == SDL_SCANCODE_BACKSPACE) wndproc(g_hwnd, WM_CHAR, 8, 0);   // as Windows sends it with the key
        return; }
    case SDL_TEXTINPUT:
        for (wchar_t c : widen(e.text.text)) if ((unsigned)c >= 32) wndproc(g_hwnd, WM_CHAR, (WPARAM)c, 0);
        return;
    case SDL_MOUSEMOTION: wndproc(g_hwnd, WM_MOUSEMOVE, 0, mac_xy(e.motion.x, e.motion.y)); return;
    case SDL_MOUSEBUTTONDOWN: case SDL_MOUSEBUTTONUP: {
        const bool down = e.type == SDL_MOUSEBUTTONDOWN;
        const LPARAM l = mac_xy(e.button.x, e.button.y);
        switch (e.button.button) {
        case SDL_BUTTON_LEFT: wndproc(g_hwnd, down ? WM_LBUTTONDOWN : WM_LBUTTONUP, 0, l); break;
        case SDL_BUTTON_MIDDLE: wndproc(g_hwnd, down ? WM_MBUTTONDOWN : WM_MBUTTONUP, 0, l); break;
        case SDL_BUTTON_RIGHT: wndproc(g_hwnd, down ? WM_RBUTTONDOWN : WM_RBUTTONUP, 0, l); break;
        case SDL_BUTTON_X1: wndproc(g_hwnd, down ? WM_XBUTTONDOWN : WM_XBUTTONUP, MAKEWPARAM(0, XBUTTON1), l); break;
        case SDL_BUTTON_X2: wndproc(g_hwnd, down ? WM_XBUTTONDOWN : WM_XBUTTONUP, MAKEWPARAM(0, XBUTTON2), l); break;
        }
        return; }
    case SDL_MOUSEWHEEL: {
        // 120 a notch, as WM_MOUSEWHEEL; a trackpad's finer steps in proportion. The
        // direction is the system's ("natural" scrolling included).
        const float y = e.wheel.preciseY != 0 ? e.wheel.preciseY : (float)e.wheel.y;
        const int d = (int)lroundf(std::max(-32000.0f, std::min(32000.0f, y * 120.0f)));
        if (d) wndproc(g_hwnd, WM_MOUSEWHEEL, MAKEWPARAM(0, (short)d), 0);
        return; }
    case SDL_CONTROLLERDEVICEADDED: case SDL_CONTROLLERDEVICEREMOVED: case SDL_CONTROLLERBUTTONDOWN:
        pad_event(e);
        return;
    }
}

// Live resizing runs inside Cocoa's own loop: repaint from SDL's event watch meanwhile.
static int mac_resize_watch(void*, SDL_Event* e)
{
    if (e->type == SDL_WINDOWEVENT && (e->window.event == SDL_WINDOWEVENT_SIZE_CHANGED || e->window.event == SDL_WINDOWEVENT_EXPOSED)) {
        g.resize();
        paint();
    }
    return 0;
}

// --snapshot: each screen as the player sees it, and each kind of overlay, as PNGs.
static int mac_snapshots(const fs::path& dir)
{
    std::error_code ec;
    fs::create_directories(dir, ec);
    if (!g.init(nullptr)) { fprintf(stderr, "snapshot: no drawing\n"); return 1; }
    g.set_offscreen(1280, 800, 2);
    int n = 0, bad = 0;
    auto shot = [&](const std::string& name) {
        layout(); scroll = scroll_to; layout();   // where the eased scroll ends up
        char num[8]; snprintf(num, sizeof num, "%02d-", ++n);
        const fs::path p = dir / widen(num + name + ".png");
        if (!g.snapshot(p.wstring(), draw_frame)) { fprintf(stderr, "snapshot: %s failed\n", u8(p).c_str()); bad++; }
        else printf("%s\n", u8(p).c_str());
    };
    auto screen = [&](const std::string& name, std::function<void()> b) { ov = Overlay(); set_screen(b); shot(name); };
    auto at_end = [&](const std::string& name) { key(VK_END); shot(name); };

    screen("home", home);
    screen("setup", import_screen);
    screen("settings", settings_screen);
    for (int i = 0; i < 14; i++) key(VK_DOWN);
    shot("settings-middle");
    at_end("settings-end");
    s_bind_tab = 0; screen("bindings-by-button", bindings_screen);
    at_end("bindings-by-button-end");
    s_bind_tab = 1; screen("bindings-by-key", bindings_screen);
    screen("key-detail", [] { key_detail("W"); });
    screen("mods", mods_screen);
    if (!S->mods().empty()) { const std::string id = S->mods().front().id; screen("mod-detail", [id] { mod_detail(id); }); }
    screen("saves", saves_screen);
    screen("backups", backups_screen);
    screen("updates", updates_screen);
    screen("logs", logs_screen);
    screen("help", help_screen);
    screen("more", more_screen);

    set_screen(home);
    message("Ready to play", "Your game is set up. Press Play.");
    shot("overlay-message");
    confirm("A different EBOOT.elf", "This is a PS3 program, but not the EBOOT.elf of the USA disc this build is made "
            "from (BCUS98124, version 1.00 -- not an update's). The game may not start with it.", "Use it anyway", [] {});
    shot("overlay-confirm");
    ov = Overlay(); ov.type = Overlay::PROGRESS; ov.title = "Importing the game"; ov.no = "Cancel";
    Progress fake; fake.set_line("PS3_GAME/USRDIR/packed/level00.psarc"); fake.total = 25ull << 30; fake.done = 9ull << 30;
    prog = &fake;
    shot("overlay-progress");
    prog = nullptr;
    set_screen(bindings_screen);
    capture_input("Cross (A): jump", "Press the key or mouse button for Cross (A): jump (now: Space, Return).", [](const std::string&) {});
    shot("overlay-capture");
    ov = Overlay();
    g.shutdown();
    return bad ? 1 : 0;
}

int main(int argc, char** argv)
{
    fs::path root, shots;
    for (int i = 1; i < argc; i++) {
        const std::string a = argv[i];
        if (a == "--self-test") {   // as on Windows; and SDL (in the bundle) must load
            const bool ok = SDL_Init(0) == 0;
            SDL_Quit();
            return ok ? 0 : 3;
        }
        if (a == "--root" && i + 1 < argc) root = argv[++i];
        else if (a == "--snapshot" && i + 1 < argc) shots = argv[++i];
    }
    root = root.empty() ? launcher_root() : fs::absolute(root);

    Store store(root);
    S = &store;
    store.clean_update_leftovers();
    if (!shots.empty()) {
        if (S->game().ok && !fs::exists(S->logo())) S->refresh_art();
        return mac_snapshots(shots);
    }

    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "0");
    SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI_PS5_PLAYER_LED, "0");
    SDL_SetHint(SDL_HINT_MOUSE_FOCUS_CLICKTHROUGH, "1");
    sdl_ok = SDL_Init(SDL_INIT_VIDEO) == 0;
    SDL_Window* w = sdl_ok ? SDL_CreateWindow("Ratchet & Clank Future: A Crack in Time", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                                              1280, 800, SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI | SDL_WINDOW_HIDDEN) : nullptr;
    if (w) {   // no bigger than the screen it opens on
        SDL_Rect u;
        if (SDL_GetDisplayUsableBounds(SDL_GetWindowDisplayIndex(w), &u) == 0 && (u.w < 1280 || u.h < 800)) {
            SDL_SetWindowSize(w, std::min(1280, u.w), std::min(800, u.h));
            SDL_SetWindowPosition(w, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED);
        }
        SDL_SetWindowMinimumSize(w, 960, 600);
    }
    g_hwnd = w; g_mac_window_for_focus = w;
    if (!w || !g.init(w)) {
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "RACiT Launcher", "The launcher could not open its window.", nullptr);
        return 1;
    }
    mac::wake_event = SDL_RegisterEvents(1);
    SDL_AddEventWatch(mac_resize_watch, nullptr);
    pad_open();

    if (S->game().ok && !fs::exists(S->logo())) S->refresh_art();   // refresh_art_once: art from before 0.4
    set_screen(home);
    if (!S->game().ok) set_screen(import_screen);   // first run: where the game comes from
    SDL_ShowWindow(w);
    SDL_RaiseWindow(w);
    SDL_StartTextInput();
    quiet_update_check();

    ULONGLONG next_tick = GetTickCount64();
    while (!mac::quit) {
        const ULONGLONG now = GetTickCount64();
        SDL_Event e;
        if (SDL_WaitEventTimeout(&e, next_tick > now ? (int)(next_tick - now) : 0)) {
            mac_event(e);
            while (!mac::quit && SDL_PollEvent(&e)) mac_event(e);
        }
        std::deque<mac::Msg> posted;
        { std::lock_guard<std::mutex> l(mac::queue_lock); posted.swap(mac::queue); }
        for (const auto& m : posted) wndproc(g_hwnd, m.m, m.w, m.l);
        if (GetTickCount64() >= next_tick) { wndproc(g_hwnd, WM_TIMER, TIMER_TICK, 0); next_tick = GetTickCount64() + 33; }
        if (mac::dirty && !mac::quit) { mac::dirty = false; wndproc(g_hwnd, WM_PAINT, 0, 0); }
    }

    if (worker.joinable()) worker.join();
    SDL_DelEventWatch(mac_resize_watch, nullptr);
    pad_close();
    g.shutdown();
    SDL_DestroyWindow(w);
    if (sdl_ok) SDL_Quit();
    if (g_restart) {   // the new .app, opened as Finder would
        if (run_argv({ "/usr/bin/open", "-n", u8(app_bundle_path()) }, {}) != 0)
            SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_INFORMATION, "RACiT Launcher", "The launcher was updated. Please open it again.", nullptr);
    }
    return 0;
}
