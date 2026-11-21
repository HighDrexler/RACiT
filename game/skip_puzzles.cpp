/* Skip puzzles (Settings > General > Puzzles, RACACIT_SKIP_PUZZLES). C ABI and C++11 atomics.
 *
 *   off   play every puzzle (the default)
 *   auto  each puzzle is solved as soon as it starts
 *   key   the skip key (RACACIT_BIND_SKIP_PUZZLE, F2 by default), or R3 + d-pad Down held
 *         together for a second on one controller, solves the puzzle being played
 *
 * Solving is the game's own: each puzzle's hook (scripts/patch_lift.py, PUZZLE_HOOKS) asks
 * racit_skip_puzzle_now() where the puzzle checks whether it is solved, and on a yes takes the
 * path a solved puzzle takes, so rewards and progress are exactly the ones playing it gives.
 * Nothing else in a save changes. tests/skip_puzzles_test.cpp checks this file without the game. */
#include <atomic>
#include <chrono>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {
std::atomic<int> mode{-1};                 /* -1: not read yet */
std::atomic<uint64_t> request_ms{0};       /* when the skip key last went down (0: none pending) */
std::atomic<int> was_down{0};              /* the key was down last poll */
std::atomic<uint64_t> combo_since{0};      /* when the controller hold began (0: not held) */
std::atomic<int> combo_fired{0};           /* this hold already asked */
/* A press asks for one skip; it lapses if no puzzle takes it within this time, so a press
 * outside a puzzle does not solve the next one later. */
constexpr uint64_t kRequestMs = 1000;
/* The controller hold: long enough that brushing Down while clicking the right stick never skips. */
constexpr uint64_t kHoldMs = 1000;
}

/* The setting's text: "off" / "auto" / "key", or 0 / 1 / 2. Anything else is off. */
extern "C" int racit_skip_puzzles_mode(const char* v)
{
    if (!v) return 0;
    while (*v == ' ') v++;
    char w[8] = {0};
    for (int i = 0; i < 7 && v[i] && v[i] != ' ' && v[i] != '\r' && v[i] != '\n'; i++)
        w[i] = (char)std::tolower((unsigned char)v[i]);
    if (!std::strcmp(w, "1") || !std::strcmp(w, "auto")) return 1;
    if (!std::strcmp(w, "2") || !std::strcmp(w, "key")) return 2;
    return 0;
}

static int current_mode()
{
    int m = mode.load();
    if (m < 0) { m = racit_skip_puzzles_mode(std::getenv("RACACIT_SKIP_PUZZLES")); mode.store(m); }
    return m;
}

/* Settings menu: the new value at once (rsx_host_ui.c keeps RACACIT_SKIP_PUZZLES in step). */
extern "C" void racit_skip_puzzles_set(int m)
{
    mode.store(m < 0 || m > 2 ? 0 : m);
    request_ms.store(0);
}

/* Every pad poll (cellPad.c): the skip key, the controller hold (R3 + Down on one pad), and
 * whether the Settings menu is open or opening. A request is the moment the key goes down
 * (holding it asks once), or the moment the hold reaches a second (once per hold). While the
 * menu is open or opening nothing asks and any pending request is dropped, so the inputs that
 * open the menu (L3 + R3, a long Select, F1) can never also skip. */
extern "C" void racit_skip_puzzle_input_at(unsigned long long now_ms, int key_held, int combo_held, int menu_open)
{
    if (!now_ms) now_ms = 1;
    const int before = was_down.exchange(key_held ? 1 : 0);
    if (menu_open) {
        request_ms.store(0);
        combo_since.store(0);
        combo_fired.store(1);          /* a hold carried out of the menu has to start again */
        return;
    }
    if (current_mode() != 2) { combo_since.store(0); return; }
    if (key_held && !before) request_ms.store(now_ms);
    if (!combo_held) { combo_since.store(0); combo_fired.store(0); return; }
    if (combo_fired.load()) return;
    uint64_t since = combo_since.load();
    if (!since) { combo_since.store(now_ms); return; }
    if (now_ms - since >= kHoldMs) { request_ms.store(now_ms); combo_fired.store(1); }
}

/* A puzzle's hook: solve it now? In auto, always; with the key, once per press made in the last
 * second. Off: never. */
extern "C" int racit_skip_puzzle_now_at(const char* puzzle, unsigned long long now_ms)
{
    const int m = current_mode();
    int yes = 0;
    if (m == 1) yes = 1;
    else if (m == 2) {
        uint64_t t = request_ms.load();
        while (t && now_ms >= t && now_ms - t <= kRequestMs) {
            if (request_ms.compare_exchange_weak(t, 0)) { yes = 1; break; }
        }
        if (t && now_ms > t && now_ms - t > kRequestMs) request_ms.compare_exchange_strong(t, 0);
    }
    if (yes && puzzle) std::fprintf(stderr, "[racacit] puzzle %s solved (Settings: Puzzles = %s)\n", puzzle, m == 1 ? "Solve at once" : "skip key");
    return yes;
}

/* The same, on the steady clock: what the pad poll and the puzzle hooks call. */
static unsigned long long now_ms()
{
    return (unsigned long long)std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count() + 1;
}
extern "C" void racit_skip_puzzle_input(int key_held, int combo_held, int menu_open)
{
    racit_skip_puzzle_input_at(now_ms(), key_held, combo_held, menu_open);
}
extern "C" int racit_skip_puzzle_now(const char* puzzle)
{
    return racit_skip_puzzle_now_at(puzzle, now_ms());
}
