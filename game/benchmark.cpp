// Built-in benchmark (RACACIT_BENCHMARK=1), on every platform and both game versions.
//
// The game boots straight to the main menu, presses Cross (Continue, so it loads the latest
// save), waits for the level to come up, then strafes left and right for two minutes and
// times every frame. The result goes to benchmark.txt next to settings.txt and the game
// quits. Saves are never touched: the run plays from a copy of the save folder
// (PS3_HDD0_HOME_ROOT, which both 1.00 and update 1.20 read their saves through).
//
// It drives the same pieces the headless test runner does: the PAD_FILE scripted pad
// (real controllers and touch controls are ignored while it runs), frame_times.cpp for
// the frame intervals, and the runtime's cellKbInit hook, since the game sets the
// keyboard up again once a level is playable. Average FPS and the 1% and 0.1% lows are
// worked out the way the runner does: the lows are the average frame rate over the
// slowest 1% and 0.1% of frames.
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

namespace fs = std::filesystem;
using namespace std::chrono_literals;

extern "C" void (*g_kb_init_hook)(void);   // libs/input/cellKb.c

namespace {

constexpr unsigned kSeconds = 120, kLead = 10;
std::atomic<int> s_on{ -1 };
std::atomic<bool> s_menu{ false };
std::atomic<unsigned> s_kb_inits{ 0 };
std::atomic<bool> s_done{ false };
fs::path s_dir, s_pad, s_frames, s_result;

void put_env(const char* key, const std::string& value)
{
#ifdef _WIN32
    _putenv_s(key, value.c_str());
#else
    setenv(key, value.c_str(), 1);
#endif
}

void on_kb_init(void) { s_kb_inits.fetch_add(1, std::memory_order_release); }

void pad(const char* line)
{
    std::ofstream(s_pad, std::ios::binary | std::ios::trunc) << line << "\n";
}

bool pad_consumed()
{
    std::error_code ec;
    return fs::file_size(s_pad, ec) == 0 && !ec;
}

template <class Pred>
bool wait_for(Pred done, std::chrono::seconds limit)
{
    const auto end = std::chrono::steady_clock::now() + limit;
    while (!done()) {
        if (std::chrono::steady_clock::now() >= end) return false;
        std::this_thread::sleep_for(100ms);
    }
    return true;
}

void finish(const std::string& text)
{
    std::ofstream(s_result, std::ios::binary | std::ios::trunc) << text;
    std::fprintf(stderr, "[benchmark] %s", text.c_str());
    std::fflush(stderr);
    // main() leaves the way it does when the window is closed. If its loop is not
    // pumping for some reason, leave anyway once the result is written.
    s_done.store(true, std::memory_order_release);
    std::this_thread::sleep_for(20s);
    std::_Exit(0);
}

std::string report(const std::vector<uint64_t>& intervals)
{
    std::vector<uint64_t> sorted(intervals);
    std::sort(sorted.begin(), sorted.end());
    const size_t n = sorted.size();
    uint64_t total = 0;
    for (uint64_t ns : sorted) total += ns;
    const size_t k1 = (n + 99) / 100, k01 = (n + 999) / 1000;
    uint64_t tail1 = 0, tail01 = 0;
    for (size_t i = n - k1; i < n; ++i) tail1 += sorted[i];
    for (size_t i = n - k01; i < n; ++i) tail01 += sorted[i];
    char buf[256];
    std::snprintf(buf, sizeof buf,
                  "average %.1f FPS, 1%% low %.1f FPS, 0.1%% low %.1f FPS, %zu frames in %.1f s\n",
                  n * 1e9 / (double)total, k1 * 1e9 / (double)tail1, k01 * 1e9 / (double)tail01,
                  n, total / 1e9);
    return buf;
}

void run()
{
    if (!wait_for([] { return s_menu.load(std::memory_order_acquire); }, 300s))
        finish("failed: the main menu never opened\n");
    std::this_thread::sleep_for(10s);
    const unsigned inits = s_kb_inits.load(std::memory_order_acquire);
    pad("0x4000");   // Cross on Continue
    if (!wait_for([&] { return s_kb_inits.load(std::memory_order_acquire) > inits; }, 300s))
        finish("failed: no level came up after Continue\n");
    std::this_thread::sleep_for(10s);

    std::ofstream(s_frames.string() + ".request", std::ios::binary | std::ios::trunc)
        << kSeconds << " " << kLead << "\n";
    const fs::path meta = s_frames.string() + ".meta";
    const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(kSeconds + kLead + 60);
    bool left = true;
    std::error_code ec;
    while (!fs::exists(meta, ec)) {
        if (std::chrono::steady_clock::now() >= end) {
            pad("0 1 128 128 128 128");
            finish("failed: the frame timing never finished\n");
        }
        if (pad_consumed()) {
            pad(left ? "0 24 64 128 128 128" : "0 24 192 128 128 128");
            left = !left;
        }
        std::this_thread::sleep_for(250ms);
    }
    pad("0 1 128 128 128 128");

    std::vector<uint64_t> intervals;
    std::ifstream in(s_frames);
    for (uint64_t ns; in >> ns;)
        if (ns) intervals.push_back(ns);
    if (intervals.size() < 100) finish("failed: too few frames were timed\n");
    finish(report(intervals));
}

}  // namespace

// The launcher looks for this text in the game program before it offers a benchmark run
// (launcher/src/store.cpp): an older build would ignore RACACIT_BENCHMARK and play from
// the real saves. racit_benchmark_setup reads it, so no linker drops it.
extern "C" const char racit_feature_benchmark[] = "RACIT-FEATURE:benchmark-1";

extern "C" int racit_benchmark_on(void)
{
    int on = s_on.load(std::memory_order_relaxed);
    if (on < 0) {
        const char* e = std::getenv("RACACIT_BENCHMARK");
        on = e && *e == '1';
        s_on.store(on, std::memory_order_relaxed);
    }
    return on;
}

// The run is over and its result written: main() quits (game/main.cpp).
extern "C" int racit_benchmark_done(void) { return s_done.load(std::memory_order_acquire); }

// The main menu is open (stubs.cpp, after PRESS START is skipped).
extern "C" void racit_benchmark_menu_open(void)
{
    if (racit_benchmark_on()) s_menu.store(true, std::memory_order_release);
}

// From main(), once every platform has read its settings and before the game starts.
extern "C" void racit_benchmark_setup(void)
{
    if (!racit_benchmark_on()) return;
    if (!*(volatile const char*)racit_feature_benchmark) return;
    const char* settings = std::getenv("RACACIT_SETTINGS_FILE");
    s_dir = settings && *settings ? fs::path(settings).parent_path() : fs::current_path();
    s_pad = s_dir / "benchmark-pad.txt";
    s_frames = s_dir / "benchmark-frames.txt";
    s_result = s_dir / "benchmark.txt";
    std::error_code ec;
    // Everything a run leaves, an interrupted one included: frame_times.cpp makes its
    // files exclusively ("wx"), so a stale .meta.tmp would keep this run from finishing.
    for (const fs::path& p : { s_frames, fs::path(s_frames.string() + ".meta"),
                               fs::path(s_frames.string() + ".meta.tmp"),
                               fs::path(s_frames.string() + ".request"), s_result })
        fs::remove(p, ec);

    // Play from a copy of the saves, so nothing the run does reaches the player's own.
    const char* home = std::getenv("PS3_HDD0_HOME_ROOT");
    const char* hdd = std::getenv("PS3_HDD0_ROOT");
    const fs::path from = home && *home ? fs::path(home) : fs::path(hdd ? hdd : ".") / "home";
    const fs::path copy = s_dir / "benchmark-home";
    fs::remove_all(copy, ec);
    if (!ec) fs::create_directories(copy, ec);
    if (!ec && fs::is_directory(from, ec))
        fs::copy(from, copy, fs::copy_options::recursive | fs::copy_options::overwrite_existing, ec);
    if (ec) {
        // Without a full copy the run could reach the real saves: stop before the game starts.
        const std::string text = "failed: could not copy the saves to " + copy.string() + " (" + ec.message() + ")\n";
        std::ofstream(s_result, std::ios::binary | std::ios::trunc) << text;
        std::fprintf(stderr, "[benchmark] %s", text.c_str());
        std::fflush(stderr);
        std::_Exit(1);
    }
    put_env("PS3_HDD0_HOME_ROOT", copy.string());

    pad("");
    put_env("PAD_FILE", s_pad.string());
    put_env("PAD_IGNORE_REAL", "1");
    put_env("RACIT_FRAME_INTERVALS", s_frames.string());
    put_env("RACACIT_SKIP_INTRO", "1");
    put_env("RACACIT_SKIP_PRESS_START", "1");
    g_kb_init_hook = on_kb_init;
    std::fprintf(stderr, "[benchmark] on: %u s of strafing after the level loads, saves from a copy in %s\n",
                 kSeconds, copy.string().c_str());
    std::thread(run).detach();
}
