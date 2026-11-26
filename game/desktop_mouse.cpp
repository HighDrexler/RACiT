/* Desktop mouse runtime for RACiT. C ABI, C++11.
 *
 * The mouse turns every camera the right stick turns, directly. Each camera's update
 * carries a hook (scripts/patch_lift.py, MOUSE_CAMERAS) that takes the mouse motion since
 * the last camera update, in radians, and adds it once to that camera's own yaw and pitch
 * step. The right stick keeps working alongside it: the hooks add to the stick's turn.
 *
 * Motion only counts for the cameras while some hooked camera is updating: in menus,
 * cutscenes and loading screens no camera takes it, and it is dropped instead of turning
 * the camera when play resumes.
 *
 * Screens with no hooked camera that the right stick drives (the map, the rotate-object
 * puzzles) get the mouse as a right-stick push instead (racit_mouse_feed), as the camera
 * keys are. Only then: while any hooked camera is active the mouse never reaches the stick,
 * so a camera never feels like a thumbstick and never gets the move twice.
 * tests/desktop_mouse_test.cpp checks this file without the game. */
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <mutex>
namespace {
/* The pad poll feeds and the camera hooks take on other threads. X and Y (and whether
 * the mouse is captured) change together under one lock, so a take never gets half of
 * a diagonal move and leaves the rest to the next frame or camera. */
std::mutex motion_lock;
bool captured = false;
int64_t camera_x = 0, camera_y = 0;
uint64_t feed_time = 0;   /* the last motion fed (under the lock) */
std::atomic<uint64_t> take_time{0};
/* Motion older than this when a camera asks for it is dropped: it was made before a
 * pause (a menu, a cutscene) that no camera took it through. */
constexpr uint64_t kStale = 250000000ULL;
/* Under the lock: forget motion fed more than kStale before now. */
void drop_stale(uint64_t now)
{
    if (now > feed_time && now - feed_time >= kStale) { camera_x = camera_y = 0; feed_time = now; }
}
/* Radians per count, x 1e9, at 100% (public PR #66): 0.003 rad, ~0.17 degree. */
constexpr double kGain = 3000000.0;
constexpr double kNano = 1000000000.0;
/* The stick push the mouse makes while no camera is active (under the lock): counts summed
 * over a short window, then a push for that window's speed, as 1.0.0's stick emulation. */
struct Stick { uint64_t epoch = 0, poll = 0; int64_t dx = 0, dy = 0; float x = 0.f, y = 0.f; };
Stick stick;
constexpr uint64_t kStickWindow = 8000000ULL;   /* 8 ms */
/* Speed to push: 0.11 of full per count/ms at 100%, at least 0.22 (past the game's dead zone
 * for any move), at most full. */
float stick_push(int64_t counts, uint64_t elapsed, float sensitivity)
{
    if (!counts) return 0.f;
    const float speed = std::fabs((float)counts) / ((float)elapsed / 1000000.f) * (.11f * sensitivity / 100.f);
    const float v = .22f + .78f * std::min(speed, 1.f);
    return counts < 0 ? -v : v;
}
bool tracing()
{
    static const int on = [] { const char* v = std::getenv("RACACIT_MOUSE_TRACE"); return v && *v == '1'; }();
    return on != 0;
}
}

/* A hooked camera updated in the last 250 ms (public PR #66): longer than a slow frame,
 * short enough that motion made in a menu or a cutscene is dropped. */
extern "C" int racit_camera_active(uint64_t now)
{
    const uint64_t last = take_time.load();
    return last > 0 && (now < last || now - last < 250000000ULL);
}

/* The runtime's pad poll (cellPad.c): the captured mouse's motion since the last poll,
 * the Settings > Controls mouse speed (percent) and Invert mouse Y. The motion goes to the
 * cameras while one is active; otherwise it is a right-stick push, written to push[0]
 * (right positive) and push[1] (down positive), each -1..1 (0 0 while a camera is active).
 * mouse_to_stick 0 keeps the stick out of it (the quick select ring has the mouse then). */
extern "C" void racit_mouse_feed(uint64_t now, int dx, int dy, int capture, float sensitivity, int invert,
                                 int mouse_to_stick, float* push)
{
    /* Whether a camera is active is read under the lock too: the takes set take_time
     * under it, so a poll can't decide "no camera" from a time a take has just moved
     * on from and then clear motion fed after that take. */
    std::lock_guard<std::mutex> hold(motion_lock);
    const bool active = capture && racit_camera_active(now);
    /* The stick: only with no camera active, from motion made since then. A camera, the mouse
     * let go or a gap in the polls (a hitch, a load) starts it over, so no older move, and
     * none a camera had, ever pushes the stick. */
    const bool gap = stick.poll && now > stick.poll && now - stick.poll >= kStale;
    stick.poll = now;
    if (!capture || active || !mouse_to_stick || gap) stick = Stick(), stick.poll = now;
    else {
        if (!stick.epoch) stick.epoch = now;
        stick.dx += dx; stick.dy += dy;
        if (now - stick.epoch >= kStickWindow) {
            const uint64_t el = now - stick.epoch;
            stick.x = stick_push(stick.dx, el, sensitivity);
            stick.y = stick_push(stick.dy, el, sensitivity) * (invert ? -1.f : 1.f);
            stick.dx = stick.dy = 0; stick.epoch = now;
        }
    }
    if (push) { push[0] = stick.x; push[1] = stick.y; }
    const double gain = kGain * sensitivity / 100.0;
    const int64_t x = active ? (int64_t)(dx * gain) : 0, y = active ? (int64_t)(dy * gain * (invert ? -1 : 1)) : 0;
    if (!active) { camera_x = camera_y = 0; captured = capture != 0; return; }
    drop_stale(now);   /* a new move never joins motion that expired in a pause */
    camera_x += x;
    camera_y += y;
    if (x || y) feed_time = now;
    captured = true;
}

/* The cameras only (1.0.1's first runtime patch called this): no stick push. */
extern "C" void racit_camera_feed(uint64_t now, int dx, int dy, int capture, float sensitivity, int invert)
{
    racit_mouse_feed(now, dx, dy, capture, sensitivity, invert, 0, nullptr);
}

/* A right-stick axis from the camera keys (-1, 0 or 1, at 0.8 as before) and the mouse's
 * push, added and kept inside the stick's range, as the pad byte (128 is centre, 1..255). */
extern "C" int racit_stick_byte(float keys, float mouse)
{
    const float v = std::max(-1.f, std::min(1.f, keys * .8f + mouse));
    return (int)(128.f + 127.f * v);   /* truncated, as the keys' push always was */
}

/* A camera hook: the motion since the last take, yaw (right positive) and pitch (down
 * positive) in radians, each used once. Returns 0 when there is none, so the hook leaves
 * the game's own (stick) turn alone. */
extern "C" int racit_camera_take(double* angles, uint64_t now)
{
    angles[0] = angles[1] = 0.0;
    int64_t x, y;
    {
        /* Expired motion goes first, then the camera says it's active again, both under
         * the lock: a move the poll feeds right as play resumes can't revive motion from
         * before the pause. */
        std::lock_guard<std::mutex> hold(motion_lock);
        drop_stale(now);
        take_time.store(now);
        if (!captured) return 0;
        x = camera_x; y = camera_y;
        camera_x = camera_y = 0;
    }
    angles[0] = (double)x / kNano;
    angles[1] = (double)y / kNano;
    return angles[0] != 0.0 || angles[1] != 0.0;
}

/* The camera that hands back to the default one when the stick moves: whether the mouse
 * moved, without using the motion (the camera it hands to takes it). */
extern "C" int racit_camera_peek(uint64_t now)
{
    std::lock_guard<std::mutex> hold(motion_lock);
    drop_stale(now);
    take_time.store(now);
    return captured && (camera_x != 0 || camera_y != 0);
}

/* The hooks call this one: racit_camera_take, plus a line per turn with
 * RACACIT_MOUSE_TRACE=1 (tests/mac_mouse_cameras.py reads them). */
extern "C" int racit_camera_turn(const char* camera, double* angles, uint64_t now)
{
    const int r = racit_camera_take(angles, now);
    if (r && tracing()) std::fprintf(stderr, "[mouse] %s turn %.5f %.5f\n", camera, angles[0], angles[1]);
    return r;
}

/* A hook's view of the result: what its camera angle became (RACACIT_MOUSE_TRACE=1). */
extern "C" void racit_camera_trace_angle(const char* camera, double before, double after)
{
    if (tracing()) std::fprintf(stderr, "[mouse] %s angle %.5f -> %.5f\n", camera, before, after);
}

/* 1.0.0's runtime patch (cellPad.c) called these two. A tools/ps3recomp still patched
 * with it links against these and works: the stick emulation does nothing (the camera
 * hooks take the mouse now), and the follow camera check is the camera check. */
struct RacitMouseState { uint64_t epoch_ns; int64_t dx, dy; float x, y; };
extern "C" void racit_mouse_step(RacitMouseState* state, uint64_t, int, int, int, float, int)
{
    if (state) *state = RacitMouseState();
}
extern "C" int racit_camera_follow_active(uint64_t now) { return racit_camera_active(now); }
