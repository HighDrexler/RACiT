/* Desktop mouse runtime for RACiT. C ABI and C++11 atomics.
 * Camera displacement is consumed once per follow-camera update. */
#include <atomic>
#include <cmath>
#include <cstdint>
#include <algorithm>
namespace {
std::atomic<bool> captured{false};
std::atomic<int64_t> camera_x{0}, camera_y{0};
std::atomic<uint64_t> follow_time{0};
}
extern "C" int racit_camera_follow_active(uint64_t now)
{
    const uint64_t last = follow_time.load();
    return last > 0 && (now < last || now - last < 1000000000ULL);
}
extern "C" void racit_camera_feed(uint64_t now, int dx, int dy, int capture, float sensitivity, int invert)
{
    if (!capture) {
        captured.store(false); camera_x.store(0); camera_y.store(0); return;
    }
    if (!racit_camera_follow_active(now)) {
        camera_x.store(0); camera_y.store(0); captured.store(true); return;
    }
    const double gain = 3000000.0 * sensitivity / 100.0;
    camera_x.fetch_add((int64_t)(dx * gain));
    camera_y.fetch_add((int64_t)(dy * gain * (invert ? -1 : 1)));
    captured.store(true);
}
extern "C" int racit_camera_take(double* angles, uint64_t now)
{
    follow_time.store(now);
    if (!captured.load()) return 0;
    angles[0] = (double)camera_x.exchange(0) / 1000000000.0;
    angles[1] = (double)camera_y.exchange(0) / 1000000000.0;
    return angles[0] != 0.0 || angles[1] != 0.0;
}
struct RacitMouseState { uint64_t epoch_ns; int64_t dx, dy; float x, y; };
extern "C" void racit_mouse_step(RacitMouseState* state, uint64_t now, int dx, int dy, int capture, float sensitivity, int invert)
{
    if (!capture) { *state = {}; return; }
    if (!state->epoch_ns) state->epoch_ns = now;
    state->dx += dx; state->dy += dy;
    const uint64_t elapsed = now - state->epoch_ns;
    if (elapsed < 8000000) return;
    const float gain = .11f * sensitivity / 100.f;
    const auto convert = [elapsed, gain](int64_t distance) {
        if (!distance) return 0.f;
        const float speed = std::fabs((float)distance) / ((float)elapsed / 1000000.f) * gain;
        const float value = .22f + .78f * std::min(speed, 1.f);
        return distance < 0 ? -value : value;
    };
    state->x = convert(state->dx); state->y = convert(state->dy) * (invert ? -1.f : 1.f);
    state->dx = state->dy = 0; state->epoch_ns = now;
}
