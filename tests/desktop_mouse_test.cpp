/* Unit test for game/desktop_mouse.cpp: needs no game data.
 *   cmake -S tests -B build/tests && cmake --build build/tests && ctest --test-dir build/tests
 * or: c++ -std=c++11 tests/desktop_mouse_test.cpp game/desktop_mouse.cpp -o mouse_test && ./mouse_test */
#include <cmath>
#include <cstdint>
#include <cstdio>
extern "C" {
int racit_camera_active(uint64_t now);
void racit_camera_feed(uint64_t now, int dx, int dy, int capture, float sensitivity, int invert);
int racit_camera_take(double* angles, uint64_t now);
int racit_camera_turn(const char* camera, double* angles, uint64_t now);
int racit_camera_peek(uint64_t now);
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
    std::printf(failures ? "desktop_mouse_test: %d failure(s)\n" : "desktop_mouse_test: all passed\n", failures);
    return failures ? 1 : 0;
}
