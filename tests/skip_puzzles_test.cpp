/* Unit test for game/skip_puzzles.cpp: needs no game data.
 *   cmake -S tests -B build/tests && cmake --build build/tests && ctest --test-dir build/tests
 * or: c++ -std=c++11 tests/skip_puzzles_test.cpp game/skip_puzzles.cpp -o skip_test && ./skip_test */
#include <cstdio>
#include <cstdlib>
extern "C" {
int racit_skip_puzzles_mode(const char* setting);
void racit_skip_puzzles_set(int mode);
void racit_skip_puzzle_input_at(unsigned long long now_ms, int key_held, int l3_held, int r3_held);
int racit_skip_puzzle_now_at(const char* puzzle, unsigned long long now_ms);
}
static int failures;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); failures++; } } while (0)

int main()
{
    /* The setting (settings.txt RACACIT_SKIP_PUZZLES): words or numbers, any case, else off. */
    CHECK(racit_skip_puzzles_mode(nullptr) == 0);
    CHECK(racit_skip_puzzles_mode("") == 0);
    CHECK(racit_skip_puzzles_mode("off") == 0 && racit_skip_puzzles_mode("0") == 0);
    CHECK(racit_skip_puzzles_mode("auto") == 1 && racit_skip_puzzles_mode("AUTO") == 1 && racit_skip_puzzles_mode("1") == 1);
    CHECK(racit_skip_puzzles_mode("key") == 2 && racit_skip_puzzles_mode(" Key\r\n") == 2 && racit_skip_puzzles_mode("2") == 2);
    CHECK(racit_skip_puzzles_mode("yes") == 0 && racit_skip_puzzles_mode("3") == 0 && racit_skip_puzzles_mode("keys") == 0);

    /* Unset in the environment: off, and nothing solves, key or not. */
    unsigned long long t = 1000;
    racit_skip_puzzle_input_at(t, 1, 0, 0); racit_skip_puzzle_input_at(t + 16, 0, 0, 0);
    CHECK(!racit_skip_puzzle_now_at("p", t + 20));

    /* Auto: every puzzle, every time it asks. */
    racit_skip_puzzles_set(1);
    CHECK(racit_skip_puzzle_now_at("p", t) && racit_skip_puzzle_now_at("p", t + 1));

    /* Key: one press solves one puzzle, once. */
    racit_skip_puzzles_set(2);
    t = 5000;
    CHECK(!racit_skip_puzzle_now_at("p", t));
    racit_skip_puzzle_input_at(t, 1, 0, 0);
    racit_skip_puzzle_input_at(t + 16, 1, 0, 0);            /* held: still one press */
    CHECK(racit_skip_puzzle_now_at("p", t + 20));
    CHECK(!racit_skip_puzzle_now_at("p", t + 40));
    racit_skip_puzzle_input_at(t + 50, 0, 0, 0);
    /* L3 + R3: the second going down is the press; one alone is nothing. */
    racit_skip_puzzle_input_at(t + 100, 0, 1, 0);
    CHECK(!racit_skip_puzzle_now_at("p", t + 110));
    racit_skip_puzzle_input_at(t + 120, 0, 1, 1);
    CHECK(racit_skip_puzzle_now_at("p", t + 130));
    racit_skip_puzzle_input_at(t + 140, 0, 0, 0);
    /* A press with no puzzle to take it lapses after a second: it does not solve the next one. */
    racit_skip_puzzle_input_at(t + 200, 1, 0, 0); racit_skip_puzzle_input_at(t + 216, 0, 0, 0);
    CHECK(!racit_skip_puzzle_now_at("p", t + 1300));
    CHECK(!racit_skip_puzzle_now_at("p", t + 1310));
    /* Pressed while off: nothing pending once the key mode is chosen. */
    racit_skip_puzzles_set(0);
    racit_skip_puzzle_input_at(t + 2000, 1, 0, 0); racit_skip_puzzle_input_at(t + 2016, 0, 0, 0);
    racit_skip_puzzles_set(2);
    CHECK(!racit_skip_puzzle_now_at("p", t + 2020));
    std::printf(failures ? "skip_puzzles_test: %d failure(s)\n" : "skip_puzzles_test: all passed\n", failures);
    return failures ? 1 : 0;
}
