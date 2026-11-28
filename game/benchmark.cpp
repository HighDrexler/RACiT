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

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>
#endif
extern "C" void (*g_kb_init_hook)(void);   // libs/input/cellKb.c

namespace {

constexpr unsigned kSeconds = 120, kLead = 10;
std::atomic<int> s_on{ -1 };
std::atomic<bool> s_menu{ false };
std::atomic<unsigned> s_kb_inits{ 0 };
std::atomic<bool> s_done{ false };
fs::path s_dir, s_run, s_pad, s_frames, s_result;

// A run holds an exclusive lock on its folder's .racit-benchmark-live for as long as the
// game is running, and the system lets it go when the process ends, however it ends. A
// later run removes a folder only when it can take that lock itself.
#ifdef _WIN32
using LiveLock = HANDLE;
const LiveLock kNoLock = INVALID_HANDLE_VALUE;
LiveLock live_lock(const fs::path& p)
{
    return CreateFileW(p.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
}
void live_unlock(LiveLock h) { CloseHandle(h); }
#else
using LiveLock = int;
const LiveLock kNoLock = -1;
LiveLock live_lock(const fs::path& p)
{
    const int fd = open(p.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0644);
    if (fd < 0) return kNoLock;
    if (flock(fd, LOCK_EX | LOCK_NB) != 0) { close(fd); return kNoLock; }
    return fd;
}
void live_unlock(LiveLock fd) { close(fd); }
#endif
LiveLock s_live = kNoLock;   // this run's, held until the process ends

bool put_env(const char* key, const std::string& value)
{
#ifdef _WIN32
    return _putenv_s(key, value.c_str()) == 0;
#else
    return setenv(key, value.c_str(), 1) == 0;
#endif
}

// Before the game starts: write the failed result and leave.
[[noreturn]] void fail_setup(const std::string& why)
{
    const std::string text = "failed: " + why + "\n";
    std::ofstream(s_result, std::ios::binary | std::ios::trunc) << text;
    std::fprintf(stderr, "[benchmark] %s", text.c_str());
    std::fflush(stderr);
    std::_Exit(1);
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

    // frame_times.cpp marks a recording it couldn't write whole complete:false.
    std::string m;
    { std::ifstream f(meta, std::ios::binary); m.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>()); }
    if (m.find("\"complete\":true") == std::string::npos) finish("failed: the frame timing was cut short\n");
    size_t lines = 0;
    std::vector<uint64_t> intervals;
    std::ifstream in(s_frames);
    for (uint64_t ns; in >> ns; ++lines)
        if (ns) intervals.push_back(ns);
    const size_t at = m.find("\"interval_count\":");
    if (at == std::string::npos || std::strtoull(m.c_str() + at + 17, nullptr, 10) != lines)
        finish("failed: the frame timing file doesn't match its record\n");
    if (intervals.size() < 100) finish("failed: too few frames were timed\n");
    finish(report(intervals));
}

}  // namespace

// The launcher looks for this text in the game program before it offers a benchmark run
// (launcher/src/store.cpp): an older build would ignore RACACIT_BENCHMARK and play from
// the real saves. racit_benchmark_setup reads it, so no linker drops it. Version 2: the
// result goes to the file the launcher names in RACACIT_BENCHMARK_RESULT.
extern "C" const char racit_feature_benchmark[] = "RACIT-FEATURE:benchmark-2";

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
    // The launcher names a result file per run. Run by hand, it goes beside settings.txt.
    const char* result = std::getenv("RACACIT_BENCHMARK_RESULT");
    s_result = result && *result ? fs::path(result) : s_dir / "benchmark.txt";
    std::error_code ec;

    // Where the saves are, in the runtime's order (cellSaveData.c save_root):
    // PS3_HDD0_HOME_ROOT, then PS3_HDD0_ROOT/home, then PS3_GAMEDATA_ROOT (or ./gamedata)
    // /dev_hdd0/home.
    const char* home = std::getenv("PS3_HDD0_HOME_ROOT");
    const char* hdd = std::getenv("PS3_HDD0_ROOT");
    const char* gamedata = std::getenv("PS3_GAMEDATA_ROOT");
    const fs::path from = home && *home ? fs::path(home)
                        : hdd && *hdd   ? fs::path(hdd) / "home"
                                        : fs::path(gamedata && *gamedata ? gamedata : "./gamedata") / "dev_hdd0" / "home";
    // Each run works in a new folder of its own: the scripted pad, the frame timing and
    // the copy of the saves it plays from. A save home inside one of these is refused.
    const fs::path real = fs::weakly_canonical(fs::absolute(from, ec), ec);
    const std::string stem = (fs::weakly_canonical(fs::absolute(s_dir, ec), ec) / "benchmark-run-").string();
    if (real.string().rfind(stem, 0) == 0)
        fail_setup("the saves are in " + real.string() + ", a benchmark run's copy; set the real save folder");
    // Earlier runs' folders are removed only when this mode made them (the marker) and no
    // game holds them: a run still going in another game window, even one stopped in a
    // debugger for days, keeps its lock and its files.
    for (const auto& d : fs::directory_iterator(s_dir, ec)) {
        const fs::path p = d.path();
        if (!d.is_directory(ec) || p.filename().string().rfind("benchmark-run-", 0) != 0) continue;
        if (!fs::exists(p / ".racit-benchmark-run", ec) || !fs::exists(p / ".racit-benchmark-live", ec)) continue;
        const LiveLock other = live_lock(p / ".racit-benchmark-live");
        if (other == kNoLock) continue;
        live_unlock(other);
        fs::remove_all(p, ec);
        ec.clear();
    }
    ec.clear();
    s_run = s_dir / ("benchmark-run-" + std::to_string((unsigned long long)
        std::chrono::system_clock::now().time_since_epoch().count()));
    if (!fs::create_directory(s_run, ec) || ec)   // false with no error when it already exists
        fail_setup("could not make a new folder for the run at " + s_run.string());
    s_live = live_lock(s_run / ".racit-benchmark-live");
    if (s_live == kNoLock) fail_setup("could not lock the run's folder " + s_run.string());
    std::ofstream(s_run / ".racit-benchmark-run", std::ios::binary) << "made by RACACIT_BENCHMARK\n";
    s_pad = s_run / "pad.txt";
    s_frames = s_run / "frames.txt";
    const fs::path copy = s_run / "home";
    if (!fs::create_directory(copy, ec) || ec) fail_setup("could not make " + copy.string());
    if (fs::is_directory(from, ec))
        fs::copy(from, copy, fs::copy_options::recursive, ec);
    // Without a full copy, or if the game would not read it, the run could reach the real
    // saves: stop before the game starts.
    if (ec) fail_setup("could not copy the saves to " + copy.string() + " (" + ec.message() + ")");
    if (!put_env("PS3_HDD0_HOME_ROOT", copy.string())) fail_setup("could not point the game at the copy of the saves");

    pad("");
    put_env("PAD_FILE", s_pad.string());
    if (!put_env("PAD_IGNORE_REAL", "1")) fail_setup("could not turn off the real controllers");
    put_env("RACIT_FRAME_INTERVALS", s_frames.string());
    put_env("RACACIT_SKIP_INTRO", "1");
    put_env("RACACIT_SKIP_PRESS_START", "1");
    g_kb_init_hook = on_kb_init;
    std::fprintf(stderr, "[benchmark] on: %u s of strafing after the level loads, saves from a copy in %s\n",
                 kSeconds, copy.string().c_str());
    std::thread(run).detach();
}
