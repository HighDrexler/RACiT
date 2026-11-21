/* Skip puzzles (Settings > General > Puzzles, RACACIT_SKIP_PUZZLES). C ABI and C++11 atomics.
 *
 *   off   play every puzzle (the default)
 *   auto  each puzzle is solved as soon as it starts
 *   key   the skip key (RACACIT_BIND_SKIP_PUZZLE, F2 by default) or L3 + R3 on a controller
 *         solves the puzzle being played
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
std::atomic<int> was_down{0};              /* the key or the combo was down last poll */
/* A press asks for one skip; it lapses if no puzzle takes it within this time, so a press
 * outside a puzzle does not solve the next one later. */
constexpr uint64_t kRequestMs = 1000;
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

/* Every pad poll (cellPad.c): the skip key, and L3 / R3 of any controller. A request is the
 * moment the key, or the second of L3 + R3, goes down; holding them asks once. */
extern "C" void racit_skip_puzzle_input_at(unsigned long long now_ms, int key_held, int l3_held, int r3_held)
{
    const int down = key_held || (l3_held && r3_held);
    const int before = was_down.exchange(down);
    if (down && !before && current_mode() == 2) request_ms.store(now_ms ? now_ms : 1);
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
extern "C" void racit_skip_puzzle_input(int key_held, int l3_held, int r3_held)
{
    racit_skip_puzzle_input_at(now_ms(), key_held, l3_held, r3_held);
}
extern "C" int racit_skip_puzzle_now(const char* puzzle)
{
    return racit_skip_puzzle_now_at(puzzle, now_ms());
}
