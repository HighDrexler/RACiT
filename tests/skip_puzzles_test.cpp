/* Unit test for game/skip_puzzles.cpp: needs no game data.
 *   cmake -S tests -B build/tests && cmake --build build/tests && ctest --test-dir build/tests
 * or: c++ -std=c++11 tests/skip_puzzles_test.cpp game/skip_puzzles.cpp -o skip_test && ./skip_test */
#include <cstdio>
#include <cstdlib>
extern "C" {
int racit_skip_puzzles_mode(const char* setting);
int racit_skip_puzzle_prompt_at(unsigned long long now_ms);
void racit_skip_puzzles_set(int mode);
void racit_skip_puzzle_input_at(unsigned long long now_ms, int key_held, int combo_held, int menu_open);
int racit_skip_puzzle_now_at(const char* puzzle, unsigned long long now_ms);
}
static int failures;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); failures++; } } while (0)

int main()
{
    /* The setting (settings.txt RACACIT_SKIP_PUZZLES): words or numbers, any case, else off. */
    CHECK(racit_skip_puzzles_mode(nullptr) == 2);   /* unset: the Skip button */
    CHECK(racit_skip_puzzles_mode("") == 2);
    CHECK(racit_skip_puzzles_mode("off") == 0 && racit_skip_puzzles_mode("0") == 0);
    CHECK(racit_skip_puzzles_mode("auto") == 1 && racit_skip_puzzles_mode("AUTO") == 1 && racit_skip_puzzles_mode("1") == 1);
    CHECK(racit_skip_puzzles_mode("key") == 2 && racit_skip_puzzles_mode(" Key\r\n") == 2 && racit_skip_puzzles_mode("2") == 2);
    CHECK(racit_skip_puzzles_mode("yes") == 0 && racit_skip_puzzles_mode("3") == 0 && racit_skip_puzzles_mode("keys") == 0);

    /* Unset in the environment: the Skip button, so a press solves the puzzle being played and
     * nothing solves without one. */
    unsigned long long t = 1000;
    CHECK(!racit_skip_puzzle_now_at("p", t));
    racit_skip_puzzle_input_at(t, 1, 0, 0); racit_skip_puzzle_input_at(t + 16, 0, 0, 0);
    CHECK(racit_skip_puzzle_now_at("p", t + 20));

    /* Auto: every puzzle, every time it asks. */
    racit_skip_puzzles_set(1);
    CHECK(racit_skip_puzzle_now_at("p", t) && racit_skip_puzzle_now_at("p", t + 1));

    /* Key: one press solves one puzzle, once, however long the key stays down. */
    racit_skip_puzzles_set(2);
    t = 5000;
    CHECK(!racit_skip_puzzle_now_at("p", t));
    racit_skip_puzzle_input_at(t, 1, 0, 0);
    racit_skip_puzzle_input_at(t + 16, 1, 0, 0);            /* held: still one press */
    CHECK(racit_skip_puzzle_now_at("p", t + 20));
    racit_skip_puzzle_input_at(t + 32, 1, 0, 0);            /* still held after the skip was used */
    racit_skip_puzzle_input_at(t + 48, 1, 0, 0);
    CHECK(!racit_skip_puzzle_now_at("p", t + 50));          /* no second skip queued */
    racit_skip_puzzle_input_at(t + 60, 0, 0, 0);
    racit_skip_puzzle_input_at(t + 70, 1, 0, 0);            /* a new press is a new skip */
    CHECK(racit_skip_puzzle_now_at("p", t + 75));
    racit_skip_puzzle_input_at(t + 80, 0, 0, 0);

    /* The controller hold: R3 + Down held a full second asks once; a shorter hold is nothing. */
    t = 8000;
    racit_skip_puzzle_input_at(t, 0, 1, 0);
    racit_skip_puzzle_input_at(t + 500, 0, 1, 0);
    CHECK(!racit_skip_puzzle_now_at("p", t + 510));
    racit_skip_puzzle_input_at(t + 600, 0, 0, 0);           /* let go early */
    racit_skip_puzzle_input_at(t + 700, 0, 1, 0);
    racit_skip_puzzle_input_at(t + 1600, 0, 1, 0);          /* 900 ms of the new hold */
    CHECK(!racit_skip_puzzle_now_at("p", t + 1610));
    racit_skip_puzzle_input_at(t + 1700, 0, 1, 0);          /* 1 s */
    CHECK(racit_skip_puzzle_now_at("p", t + 1710));
    racit_skip_puzzle_input_at(t + 2800, 0, 1, 0);          /* kept held: no second skip */
    racit_skip_puzzle_input_at(t + 4000, 0, 1, 0);
    CHECK(!racit_skip_puzzle_now_at("p", t + 4010));
    racit_skip_puzzle_input_at(t + 4100, 0, 0, 0);

    /* The Settings menu open or opening: no input asks, and a pending skip is dropped. */
    t = 14000;
    racit_skip_puzzle_input_at(t, 1, 0, 1);                 /* F2 with the menu open */
    CHECK(!racit_skip_puzzle_now_at("p", t + 10));
    racit_skip_puzzle_input_at(t + 20, 0, 0, 1);
    racit_skip_puzzle_input_at(t + 30, 1, 0, 0);            /* a press after it closes counts */
    racit_skip_puzzle_input_at(t + 40, 0, 0, 1);            /* ...but the menu opens before a puzzle takes it */
    CHECK(!racit_skip_puzzle_now_at("p", t + 50));
    racit_skip_puzzle_input_at(t + 60, 0, 0, 0);
    racit_skip_puzzle_input_at(t + 100, 0, 1, 0);           /* hold begins */
    racit_skip_puzzle_input_at(t + 600, 0, 1, 1);           /* L3 joins: the menu opens */
    racit_skip_puzzle_input_at(t + 1200, 0, 1, 1);
    racit_skip_puzzle_input_at(t + 1300, 0, 1, 0);          /* menu closed, still held */
    racit_skip_puzzle_input_at(t + 2500, 0, 1, 0);
    CHECK(!racit_skip_puzzle_now_at("p", t + 2510));        /* the hold must start again */
    racit_skip_puzzle_input_at(t + 2600, 0, 0, 0);
    racit_skip_puzzle_input_at(t + 2700, 1, 0, 1);          /* key down while the menu is open */
    racit_skip_puzzle_input_at(t + 2800, 1, 0, 0);          /* menu closes, key still down */
    CHECK(!racit_skip_puzzle_now_at("p", t + 2810));        /* not a press */
    racit_skip_puzzle_input_at(t + 2900, 0, 0, 0);
    t = 20000;
    /* A press with no puzzle to take it lapses after a second: it does not solve the next one. */
    racit_skip_puzzle_input_at(t + 200, 1, 0, 0); racit_skip_puzzle_input_at(t + 216, 0, 0, 0);
    CHECK(!racit_skip_puzzle_now_at("p", t + 1300));
    CHECK(!racit_skip_puzzle_now_at("p", t + 1310));
    /* Pressed while off: nothing pending once the key mode is chosen. */
    racit_skip_puzzles_set(0);
    racit_skip_puzzle_input_at(t + 2000, 1, 0, 0); racit_skip_puzzle_input_at(t + 2016, 0, 0, 0);
    racit_skip_puzzles_set(2);
    CHECK(!racit_skip_puzzle_now_at("p", t + 2020));
    /* The Skip prompt shows while a puzzle hook keeps asking, goes 300 ms after it stops, and
     * only in the Skip button mode. */
    t = 40000;
    racit_skip_puzzle_now_at("p", t);
    CHECK(racit_skip_puzzle_prompt_at(t + 16) && racit_skip_puzzle_prompt_at(t + 300));
    CHECK(!racit_skip_puzzle_prompt_at(t + 400));
    racit_skip_puzzles_set(1);
    racit_skip_puzzle_now_at("p", t + 500);
    CHECK(!racit_skip_puzzle_prompt_at(t + 516));
    racit_skip_puzzles_set(0);
    racit_skip_puzzle_now_at("p", t + 600);
    CHECK(!racit_skip_puzzle_prompt_at(t + 616));
    std::printf(failures ? "skip_puzzles_test: %d failure(s)\n" : "skip_puzzles_test: all passed\n", failures);
    return failures ? 1 : 0;
}
