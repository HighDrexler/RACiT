/* Desktop mouse runtime for RACiT. C ABI, C++11.
 *
 * The mouse turns every camera the right stick turns, directly. Each camera's update
 * carries a hook (scripts/patch_lift.py, MOUSE_CAMERAS) that takes the mouse motion since
 * the last camera update, in radians, and adds it once to that camera's own yaw and pitch
 * step. The right stick keeps working alongside it: the hooks add to the stick's turn.
 *
 * Motion only counts while some hooked camera is updating: in menus, cutscenes and loading
 * screens no camera takes it, and it is dropped instead of turning the camera when play
 * resumes. tests/desktop_mouse_test.cpp checks this file without the game. */
#include <atomic>
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
std::atomic<uint64_t> take_time{0};
/* Radians per count, x 1e9, at 100% (public PR #66): 0.003 rad, ~0.17 degree. */
constexpr double kGain = 3000000.0;
constexpr double kNano = 1000000000.0;
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
 * the Settings > Controls mouse speed (percent) and Invert mouse Y. */
extern "C" void racit_camera_feed(uint64_t now, int dx, int dy, int capture, float sensitivity, int invert)
{
    const bool active = capture && racit_camera_active(now);
    const double gain = kGain * sensitivity / 100.0;
    const int64_t x = active ? (int64_t)(dx * gain) : 0, y = active ? (int64_t)(dy * gain * (invert ? -1 : 1)) : 0;
    std::lock_guard<std::mutex> hold(motion_lock);
    if (!active) { camera_x = camera_y = 0; captured = capture != 0; return; }
    camera_x += x;
    camera_y += y;
    captured = true;
}

/* A camera hook: the motion since the last take, yaw (right positive) and pitch (down
 * positive) in radians, each used once. Returns 0 when there is none, so the hook leaves
 * the game's own (stick) turn alone. */
extern "C" int racit_camera_take(double* angles, uint64_t now)
{
    take_time.store(now);
    angles[0] = angles[1] = 0.0;
    int64_t x, y;
    {
        std::lock_guard<std::mutex> hold(motion_lock);
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
    take_time.store(now);
    std::lock_guard<std::mutex> hold(motion_lock);
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
