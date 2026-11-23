/* Unit test for game/desktop_mouse.cpp: needs no game data.
 *   cmake -S tests -B build/tests && cmake --build build/tests && ctest --test-dir build/tests
 * or: c++ -std=c++11 -pthread tests/desktop_mouse_test.cpp game/desktop_mouse.cpp -o mouse_test && ./mouse_test */
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <thread>
extern "C" {
int racit_camera_active(uint64_t now);
void racit_camera_feed(uint64_t now, int dx, int dy, int capture, float sensitivity, int invert);
int racit_camera_take(double* angles, uint64_t now);
int racit_camera_turn(const char* camera, double* angles, uint64_t now);
int racit_camera_peek(uint64_t now);
void racit_mouse_step(void* state, uint64_t now, int dx, int dy, int capture, float sensitivity, int invert);
int racit_camera_follow_active(uint64_t now);
void racit_mouse_feed(uint64_t now, int dx, int dy, int capture, float sensitivity, int invert, int mouse_to_stick, float* push);
int racit_stick_byte(float keys, float mouse);
}
static int failures;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); failures++; } } while (0)
static bool near(double a, double b) { return std::fabs(a - b) < 1e-9; }
static const uint64_t ms = 1000000ULL;

int main()
{
    double a[2];
    uint64_t t = 1000 * ms;
    /* Outside gameplay (no camera has updated yet): motion is dropped. */
    CHECK(!racit_camera_active(t));
    racit_camera_feed(t, 100, 50, 1, 100.f, 0);
    CHECK(racit_camera_take(a, t) == 0 && a[0] == 0.0 && a[1] == 0.0);
    /* A camera updates every frame: motion goes to it, in radians (0.003 per count at 100%). */
    t += 16 * ms; CHECK(racit_camera_active(t));
    racit_camera_feed(t, 100, -50, 1, 100.f, 0);
    t += 16 * ms; CHECK(racit_camera_take(a, t) == 1);
    CHECK(near(a[0], 0.3) && near(a[1], -0.15));
    /* Used once: the next frame gets nothing. */
    t += 16 * ms; CHECK(racit_camera_take(a, t) == 0 && a[0] == 0.0 && a[1] == 0.0);
    /* Motion between two frames adds up, and is still used once. */
    racit_camera_feed(t, 10, 0, 1, 100.f, 0); racit_camera_feed(t, 10, 4, 1, 100.f, 0);
    t += 16 * ms; CHECK(racit_camera_turn("test", a, t) == 1 && near(a[0], 0.06) && near(a[1], 0.012));
    t += 16 * ms; CHECK(racit_camera_turn("test", a, t) == 0);
    /* The hand-back camera sees that the mouse moved without using the motion: the camera
     * it hands to gets all of it. */
    t += 16 * ms; CHECK(racit_camera_peek(t) == 0);
    racit_camera_feed(t, 7, 0, 1, 100.f, 0);
    t += 16 * ms; CHECK(racit_camera_peek(t) == 1 && racit_camera_peek(t) == 1);
    t += 16 * ms; CHECK(racit_camera_take(a, t) == 1 && near(a[0], 0.021));
    t += 16 * ms; CHECK(racit_camera_peek(t) == 0);
    /* Sensitivity scales linearly: 50% is half, 250% two and a half times. */
    racit_camera_feed(t, 100, 100, 1, 50.f, 0);
    t += 16 * ms; racit_camera_take(a, t); CHECK(near(a[0], 0.15) && near(a[1], 0.15));
    racit_camera_feed(t, 100, 100, 1, 250.f, 0);
    t += 16 * ms; racit_camera_take(a, t); CHECK(near(a[0], 0.75) && near(a[1], 0.75));
    /* Invert mouse Y flips pitch only. */
    racit_camera_feed(t, 100, 100, 1, 100.f, 1);
    t += 16 * ms; racit_camera_take(a, t); CHECK(near(a[0], 0.3) && near(a[1], -0.3));
    /* The mouse released (not captured): nothing, and what was pending is dropped. */
    racit_camera_feed(t, 100, 100, 1, 100.f, 0);
    racit_camera_feed(t, 100, 100, 0, 100.f, 0);
    t += 16 * ms; CHECK(racit_camera_take(a, t) == 0);
    /* A menu or cutscene: no camera for 250 ms. Motion made then is dropped and does
     * not turn the camera when play resumes. */
    t += 300 * ms; CHECK(!racit_camera_active(t));
    racit_camera_feed(t, 500, 500, 1, 100.f, 0);
    t += 16 * ms; CHECK(racit_camera_take(a, t) == 0 && a[0] == 0.0 && a[1] == 0.0);
    /* ...and play goes on normally afterwards. */
    racit_camera_feed(t, 1, 0, 1, 100.f, 0);
    t += 16 * ms; CHECK(racit_camera_take(a, t) == 1 && near(a[0], 0.003));
    /* Motion fed just before the cameras pause (no camera and no feed for 300 ms) is
     * dropped, not used when a camera next updates; peek doesn't see it either. */
    t += 16 * ms; racit_camera_take(a, t);
    racit_camera_feed(t, 40, 40, 1, 100.f, 0);
    t += 300 * ms; CHECK(racit_camera_peek(t) == 0);
    racit_camera_feed(t, 30, 30, 1, 100.f, 0);   /* the first move after it still counts */
    CHECK(racit_camera_take(a, t) == 1 && near(a[0], 0.09));
    racit_camera_feed(t, 40, 40, 1, 100.f, 0);
    t += 300 * ms; CHECK(racit_camera_take(a, t) == 0 && a[0] == 0.0 && a[1] == 0.0);
    /* A pause, then play resumes with a move fed right as the first camera updates: the
     * old motion stays dropped, only the new move counts. */
    t += 16 * ms; racit_camera_take(a, t);
    racit_camera_feed(t, 50, 50, 1, 100.f, 0);
    t += 400 * ms;
    racit_camera_feed(t, 50, 50, 1, 100.f, 0);   /* not active yet: nothing */
    CHECK(racit_camera_take(a, t) == 0);
    racit_camera_feed(t, 10, 0, 1, 100.f, 0);    /* the resume move */
    t += 16 * ms; CHECK(racit_camera_take(a, t) == 1 && near(a[0], 0.03) && near(a[1], 0.0));
    /* 1.0.0's runtime patch calls these: they link and do nothing harmful. */
    {
        struct { uint64_t epoch_ns; int64_t dx, dy; float x, y; } old_state = { 5, 6, 7, 0.5f, 0.5f };
        racit_mouse_step(&old_state, t, 10, 10, 1, 100.f, 0);
        CHECK(old_state.x == 0.f && old_state.y == 0.f && old_state.dx == 0);
        CHECK(racit_camera_follow_active(t) == racit_camera_active(t));
    }
    /* The right stick: the mouse pushes it only where no hooked camera is active (the map,
     * the puzzles), never while one is. A poll comes every 4 ms here. */
    {
        float p[2] = { 9.f, 9.f };
        const uint64_t poll = 4 * ms;
        /* A camera active: the motion is the camera's, the stick gets nothing, poll after poll. */
        t += 16 * ms; racit_camera_take(a, t);
        bool stick_while_active = false;
        for (int i = 0; i < 10; i++) {
            t += poll; racit_mouse_feed(t, 40, 40, 1, 100.f, 0, 1, p);
            if (p[0] != 0.f || p[1] != 0.f) stick_while_active = true;
            if (i % 4 == 3) racit_camera_take(a, t);
        }
        CHECK(!stick_while_active);
        t += poll; CHECK(racit_camera_take(a, t) == 1);   /* ...and the camera had it */
        /* The camera goes idle (the map opens). Within the 250 ms window the mouse is still
         * the camera's; past it, the stick's. The motion the camera never took doesn't push it. */
        racit_mouse_feed(t, 500, 500, 1, 100.f, 0, 1, p);
        t += 100 * ms; racit_mouse_feed(t, 30, 0, 1, 100.f, 0, 1, p);
        CHECK(p[0] == 0.f && p[1] == 0.f);
        t += 200 * ms; CHECK(!racit_camera_active(t));
        racit_mouse_feed(t, 0, 0, 1, 100.f, 0, 1, p);   /* the first poll past it starts the stick's window */
        CHECK(p[0] == 0.f && p[1] == 0.f);
        /* A slow move right and down: past the dead zone, the same way, less than full. */
        t += 2 * poll; racit_mouse_feed(t, 2, 1, 1, 100.f, 0, 1, p);
        CHECK(p[0] > 0.2f && p[0] < 1.f && p[1] > 0.2f && p[1] < p[0]);
        const float slow = p[0];
        /* Between windows the push holds; with no motion in the next window it is let go. */
        t += poll; racit_mouse_feed(t, 0, 0, 1, 100.f, 0, 1, p);
        CHECK(near(p[0], slow));
        t += poll; racit_mouse_feed(t, 0, 0, 1, 100.f, 0, 1, p);
        CHECK(p[0] == 0.f && p[1] == 0.f);
        /* Fast is full, left and up negative; and the camera got none of it. */
        t += 2 * poll; racit_mouse_feed(t, -400, -400, 1, 100.f, 0, 1, p);
        CHECK(near(p[0], -1.0) && near(p[1], -1.0));
        /* Sensitivity: the same slow move pushes harder at 250%. */
        t += 2 * poll; racit_mouse_feed(t, 2, 0, 1, 250.f, 0, 1, p);
        CHECK(p[0] > slow);
        /* Invert mouse Y flips the stick's vertical only. */
        t += 2 * poll; racit_mouse_feed(t, 2, 2, 1, 100.f, 1, 1, p);
        CHECK(p[0] > 0.f && p[1] < 0.f && near(p[0], -p[1]));
        /* The quick select ring (mouse_to_stick 0) and a released mouse push nothing, and
         * what was summed before is gone afterwards. */
        t += poll; racit_mouse_feed(t, 50, 50, 1, 100.f, 0, 1, p);
        t += poll; racit_mouse_feed(t, 0, 0, 1, 100.f, 0, 0, p);
        CHECK(p[0] == 0.f && p[1] == 0.f);
        t += 2 * poll; racit_mouse_feed(t, 0, 0, 1, 100.f, 0, 1, p);
        CHECK(p[0] == 0.f && p[1] == 0.f);
        t += poll; racit_mouse_feed(t, 50, 50, 1, 100.f, 0, 1, p);
        t += poll; racit_mouse_feed(t, 0, 0, 0, 100.f, 0, 1, p);
        CHECK(p[0] == 0.f && p[1] == 0.f);
        t += 2 * poll; racit_mouse_feed(t, 0, 0, 1, 100.f, 0, 1, p);
        CHECK(p[0] == 0.f && p[1] == 0.f);
        /* A gap in the polls (a hitch, a load): motion summed before it doesn't push after. */
        t += poll; racit_mouse_feed(t, 50, 50, 1, 100.f, 0, 1, p);
        t += 300 * ms; racit_mouse_feed(t, 0, 0, 1, 100.f, 0, 1, p);
        CHECK(p[0] == 0.f && p[1] == 0.f);
        t += 2 * poll; racit_mouse_feed(t, 0, 0, 1, 100.f, 0, 1, p);
        CHECK(p[0] == 0.f && p[1] == 0.f);
        /* Mid-push, a camera comes back (play resumes): the stick lets go at once, the
         * camera gets only the motion made after it, none from the stick's time. */
        t += 2 * poll; racit_mouse_feed(t, 6, 0, 1, 100.f, 0, 1, p);
        CHECK(p[0] > 0.f);
        t += poll; racit_mouse_feed(t, 6, 0, 1, 100.f, 0, 1, p);   /* half a window summed */
        CHECK(racit_camera_take(a, t) == 0);
        t += poll; racit_mouse_feed(t, 0, 0, 1, 100.f, 0, 1, p);
        CHECK(p[0] == 0.f && p[1] == 0.f);
        racit_mouse_feed(t, 10, 0, 1, 100.f, 0, 1, p);
        CHECK(p[0] == 0.f);
        t += 16 * ms; CHECK(racit_camera_take(a, t) == 1 && near(a[0], 0.03));
        /* The old entry point (the cameras only) never makes a push. */
        t += 300 * ms; racit_camera_feed(t, 0, 0, 1, 100.f, 0);
        t += 2 * poll; racit_camera_feed(t, 400, 400, 1, 100.f, 0);
        racit_mouse_feed(t, 0, 0, 1, 100.f, 0, 1, p);
        CHECK(p[0] == 0.f && p[1] == 0.f);
        racit_camera_take(a, t);
    }
    /* Keys and mouse add up, inside the stick's range: keys alone as before (0.8), the mouse
     * alone, both one way (full, not past it), both against each other. */
    CHECK(racit_stick_byte(1.f, 0.f) == (int)(128.f + 127.f * 0.8f) && racit_stick_byte(-1.f, 0.f) == (int)(128.f - 127.f * 0.8f));
    CHECK(racit_stick_byte(0.f, 0.f) == 128 && racit_stick_byte(0.f, 1.f) == 255 && racit_stick_byte(0.f, -1.f) == 1);
    CHECK(racit_stick_byte(1.f, 1.f) == 255 && racit_stick_byte(-1.f, -1.f) == 1);
    CHECK(racit_stick_byte(1.f, -0.8f) == 128);
    CHECK(racit_stick_byte(1.f, 0.1f) == (int)(128.f + 127.f * 0.9f));
    /* The pad poll and a camera on two threads: each take gets whole moves (x and y of a
     * diagonal together), and every move is taken exactly once. */
    {
        const int moves = 200000;
        t += 16 * ms; racit_camera_take(a, t);
        std::atomic<bool> fed{false};
        std::thread feeder([&] {
            for (int i = 0; i < moves; i++) racit_camera_feed(t, 1, 1, 1, 100.f, 0);
            fed.store(true);
        });
        double sum_x = 0, sum_y = 0; int split = 0;
        for (;;) {
            const bool last = fed.load();
            if (racit_camera_take(a, t)) { sum_x += a[0]; sum_y += a[1]; if (a[0] != a[1]) split++; }
            if (last) break;
        }
        feeder.join();
        if (racit_camera_take(a, t)) { sum_x += a[0]; sum_y += a[1]; if (a[0] != a[1]) split++; }
        CHECK(split == 0);
        CHECK(std::fabs(sum_x - moves * 0.003) < 1e-6 && std::fabs(sum_y - moves * 0.003) < 1e-6);
    }
    std::printf(failures ? "desktop_mouse_test: %d failure(s)\n" : "desktop_mouse_test: all passed\n", failures);
    return failures ? 1 : 0;
}
