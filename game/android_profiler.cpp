/*
 * Android only: a sampling profiler built into the game, for devices that
 * cannot stay on a USB cable while they are played (simpleperf needs one).
 * RACACIT_PROFILE=1 (launcher > Diagnostics > Performance profile) turns it
 * on for a run; off otherwise (v113-android.36: the measuring builds .13-.35
 * had it on, and in the .35 profile at the owner's worst spot this thread was
 * 6.8% of all the CPU time the game used, 97.6% of that in the linker's
 * symbol search behind dladdr -- now cached -- besides the 500 signals per
 * second of CPU time it sends every busy thread).
 *
 * Every thread of the game gets a timer on its own CPU-time clock: each 2 ms
 * a thread spends running, SIGPROF interrupts it and the handler records where
 * it was (pc). Every 30 s the samples of that window become a report in
 * <files>/profile.txt (the last three windows, newest first, then the run's slowest one): the frame rate,
 * each thread's share of a core (500 samples a second = one full core), and
 * for the busiest threads the functions they spent it in. Exported functions
 * -- the lifted guest code, the runtime's -- are named here (dladdr);
 * anything else is listed as libmain.so+offset, for symbolizing on the build
 * PC with the unstripped library of the same build.
 */
#ifdef __ANDROID__
#include <cxxabi.h>
#include <dirent.h>
#include <dlfcn.h>
#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
#include <time.h>
#include <ucontext.h>
#include <unistd.h>
#include <algorithm>
#include <string>
#include <unordered_map>
#include <vector>
#include "android_profiler_ring.h"

extern "C" unsigned cellGcm_flip_request_count(void);
extern "C" void racacit_background_thread(const char* name);   /* main.cpp */

namespace {

constexpr uint32_t kCap = 1u << 19;      /* samples kept: two windows of 16 busy threads */
constexpr long kPeriodNs = 2000000;      /* a sample per 2 ms of a thread's CPU time */
constexpr int kWindowS = 30;
constexpr int kSamplesPerCore = 500;     /* a second of one full core */

using Sample = RacitProfileSample;
Sample* s_ring;
uint32_t s_n;
char s_path[600];

void on_sample(int, siginfo_t*, void* uc)
{
    const int saved_errno = errno;
    const uint32_t ticket = __atomic_fetch_add(&s_n, 1u, __ATOMIC_RELAXED);
    racit_profile_publish(&s_ring[ticket & (kCap - 1u)], ticket,
        ((ucontext_t*)uc)->uc_mcontext.pc, (int32_t)syscall(SYS_gettid));
    errno = saved_errno;
}

struct Armed { int tid; timer_t t; };
std::vector<Armed> s_armed;

/* Arm a timer for every thread of the process (but this one); drop gone ones. */
void scan(int self)
{
    std::vector<int> live;
    if (DIR* d = opendir("/proc/self/task")) {
        while (dirent* e = readdir(d)) { const int t = atoi(e->d_name); if (t > 0) live.push_back(t); }
        closedir(d);
    }
    for (size_t i = 0; i < s_armed.size();) {
        if (std::find(live.begin(), live.end(), s_armed[i].tid) == live.end()) {
            timer_delete(s_armed[i].t); s_armed[i] = s_armed.back(); s_armed.pop_back();
        } else i++;
    }
    for (const int t : live) {
        if (t == self) continue;
        bool have = false;
        for (const Armed& a : s_armed) if (a.tid == t) { have = true; break; }
        if (have) continue;
        sigevent sev; memset(&sev, 0, sizeof sev);
        sev.sigev_notify = SIGEV_THREAD_ID; sev.sigev_signo = SIGPROF; sev.sigev_notify_thread_id = t;
        const clockid_t clk = (clockid_t)((~(unsigned)t << 3) | 6u);   /* that thread's CPU-time clock */
        timer_t tm;
        if (timer_create(clk, &sev, &tm) != 0) continue;
        itimerspec its; memset(&its, 0, sizeof its);
        its.it_interval.tv_nsec = its.it_value.tv_nsec = kPeriodNs;
        timer_settime(tm, 0, &its, nullptr);
        s_armed.push_back({ t, tm });
    }
}

extern "C" const char* racacit_thread_label(int tid);   /* android_companion.cpp */

std::string thread_name(int tid)
{
    if (const char* l = racacit_thread_label(tid)) return l;
    char p[64], b[64] = "";
    snprintf(p, sizeof p, "/proc/self/task/%d/comm", tid);
    if (FILE* f = fopen(p, "r")) { if (!fgets(b, sizeof b, f)) b[0] = 0; fclose(f); }
    std::string s(b);
    while (!s.empty() && (s.back() == '\n' || s.back() == ' ')) s.pop_back();
    return s.empty() ? "(ended) " + std::to_string(tid) : s;
}

/* dladdr is a linear search of the library's symbol table (libmain.so exports the lifted code:
 * hundreds of thousands of symbols), and a report asks it for every distinct pc twice. The same
 * pcs come back window after window: each is looked up once. */
struct PcInfo { std::string name; const char* lib; uint64_t base; };
std::unordered_map<uint64_t, PcInfo> s_pcinfo;
const PcInfo& pc_info(uint64_t pc)
{
    auto it = s_pcinfo.find(pc);
    if (it != s_pcinfo.end()) return it->second;
    if (s_pcinfo.size() > 150000) s_pcinfo.clear();
    PcInfo w{ std::string(), "?", 0 };
    Dl_info di;
    if (dladdr((void*)pc, &di) && di.dli_fbase) {
        const char* lib = di.dli_fname ? strrchr(di.dli_fname, '/') : nullptr;
        w.lib = lib ? lib + 1 : (di.dli_fname ? di.dli_fname : "?");   /* the loader's string: lives as long as the library */
        w.base = (uint64_t)di.dli_fbase;
        if (di.dli_sname) {
            int st = 0; char* dm = abi::__cxa_demangle(di.dli_sname, nullptr, nullptr, &st);
            w.name = (st == 0 && dm) ? dm : di.dli_sname;
            free(dm);
            const size_t paren = w.name.find('(');
            if (paren != std::string::npos && paren > 0) w.name.resize(paren);
        }
    }
    return s_pcinfo.emplace(pc, std::move(w)).first->second;
}

/* What a pc is: the exported function it is in, else its library and offset
 * (rounded to 256 bytes, so one static function's samples mostly stay together). */
std::string where(uint64_t pc)
{
    const PcInfo& w = pc_info(pc);
    if (!w.base) { char b[32]; snprintf(b, sizeof b, "0x%llx", (unsigned long long)pc); return b; }
    if (!w.name.empty()) return w.name;
    char b[160];
    snprintf(b, sizeof b, "%s+0x%llx", w.lib, (unsigned long long)((pc - w.base) & ~0xFFull));
    return b;
}

std::vector<std::string> s_reports, s_raws;
/* The slowest window of the run so far (at least 10 fps: not a load or a stall), kept besides the
 * last three: the owner's 2026-11-15 test dipped to 20 fps on a moon 2 minutes before the end, and
 * that window had already rolled out of profile.txt when the game was closed. v113-android.52. */
std::string s_slow_report, s_slow_raw;
double s_slow_fps = 1e9;
int s_slow_window;

std::string clocks()
{
    std::string out;
    for (int cpu = 0; cpu < 12; cpu++) {
        char p[96], b[32];
        snprintf(p, sizeof p, "/sys/devices/system/cpu/cpufreq/policy%d/scaling_cur_freq", cpu);
        FILE* f = fopen(p, "r");
        if (!f) continue;
        if (fgets(b, sizeof b, f)) { char o[48]; snprintf(o, sizeof o, "%scpu%d %.2f GHz", out.empty() ? "" : ", ", cpu, atof(b) / 1e6); out += o; }
        fclose(f);
    }
    return out.empty() ? "not readable" : out;
}

void report(int window, uint32_t from, uint32_t to, double secs, unsigned flips)
{
    std::unordered_map<int, std::unordered_map<uint64_t, uint32_t>> per;
    std::unordered_map<int, uint32_t> total;
    uint32_t all = 0;
    for (uint32_t i = from; i != to; i++) {
        Sample s;
        if (!racit_profile_read(&s_ring[i & (kCap - 1u)], i, &s)) continue;
        per[s.tid][s.pc]++; total[s.tid]++; all++;
    }
    std::vector<std::pair<uint32_t, int>> threads;
    for (const auto& kv : total) threads.push_back({ kv.second, kv.first });
    std::sort(threads.begin(), threads.end(), [](const auto& a, const auto& b) { return a.first > b.first; });

    std::string r;
    char line[512];
    snprintf(line, sizeof line, "== window %d: %.0f s, %.1f fps (guest flips), %.1f cores busy in all ==\n",
             window, secs, secs > 0 ? flips / secs : 0.0, secs > 0 ? all / (secs * kSamplesPerCore) : 0.0);
    r += line;
    r += "CPU clocks: " + clocks() + "\n";
    r += "Threads, % of one core:\n";
    for (size_t i = 0; i < threads.size() && i < 14; i++) {
        snprintf(line, sizeof line, "  %5.1f%%  %s\n", threads[i].first / (secs * kSamplesPerCore) * 100.0, thread_name(threads[i].second).c_str());
        r += line;
    }
    std::string brief;
    /* profile_raw.txt (newest window only): each listed thread's exact sample addresses as
     * library + offset, for symbolizing on the build PC with the unstripped library of the same
     * build (static functions and inlined helpers included, which the names here are not). */
    std::string raw;
    snprintf(line, sizeof line, "# window %d: %.0f s, %.1f fps\n", window, secs, secs > 0 ? flips / secs : 0.0);
    raw += line;
    for (size_t t = 0; t < threads.size() && t < 10; t++) {
        const int tid = threads[t].second;
        const uint32_t n = threads[t].first;
        if (n < (uint32_t)(secs * kSamplesPerCore * 0.03)) break;   /* under 3% of a core: not worth listing */
        std::vector<std::pair<uint32_t, uint64_t>> pcs;
        for (const auto& kv : per[tid]) pcs.push_back({ kv.second, kv.first });
        std::sort(pcs.begin(), pcs.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
        std::unordered_map<std::string, uint32_t> fn;
        uint32_t named = 0;
        for (size_t i = 0; i < pcs.size() && i < 3000; i++) { fn[where(pcs[i].second)] += pcs[i].first; named += pcs[i].first; }
        std::vector<std::pair<uint32_t, std::string>> fs;
        for (const auto& kv : fn) fs.push_back({ kv.second, kv.first });
        std::sort(fs.begin(), fs.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
        const std::string tn = thread_name(tid);
        snprintf(line, sizeof line, "%s (%u samples):\n", tn.c_str(), n);
        r += line;
        snprintf(line, sizeof line, "thread\t%s\t%u\n", tn.c_str(), n);
        raw += line;
        for (size_t i = 0; i < pcs.size() && i < 1500; i++) {
            const PcInfo& w = pc_info(pcs[i].second);
            const char* lib = w.lib; const uint64_t off = w.base ? pcs[i].second - w.base : pcs[i].second;
            snprintf(line, sizeof line, "%u\t%s\t0x%llx\n", pcs[i].first, lib, (unsigned long long)off);
            raw += line;
        }
        for (size_t i = 0; i < fs.size() && i < 24; i++) {
            snprintf(line, sizeof line, "  %5.1f%%  %s\n", fs[i].first * 100.0 / n, fs[i].second.c_str());
            r += line;
        }
        if (named < n) { snprintf(line, sizeof line, "  %5.1f%%  (spread over other places)\n", (n - named) * 100.0 / n); r += line; }
        if (t < 4) { snprintf(line, sizeof line, "%s%s %.0f%%", brief.empty() ? "" : ", ", tn.c_str(), n / (secs * kSamplesPerCore) * 100.0); brief += line; }
    }
    fprintf(stderr, "[prof] window %d: %.1f fps; %s%c", window, secs > 0 ? flips / secs : 0.0, brief.c_str(), 10);

    const double fps = secs > 0 ? flips / secs : 0.0;
    if (fps >= 10.0 && fps < s_slow_fps) { s_slow_fps = fps; s_slow_window = window; s_slow_report = r; s_slow_raw = raw; }
    s_reports.insert(s_reports.begin(), r);
    if (s_reports.size() > 3) s_reports.resize(3);
    s_raws.insert(s_raws.begin(), raw);
    if (s_raws.size() > 3) s_raws.resize(3);
    const bool slow_extra = s_slow_window && s_slow_window <= window - 3;   /* not one of the three above */
    char tmp[620]; snprintf(tmp, sizeof tmp, "%s.tmp", s_path);
    if (FILE* f = fopen(tmp, "w")) {
        fputs("A Crack in Time (Android) -- performance profile, newest window first.\n"
              "Samples every 2 ms of each thread's CPU time; 100% = one CPU core busy the whole window.\n\n", f);
        for (const std::string& s : s_reports) { fputs(s.c_str(), f); fputc('\n', f); }
        if (slow_extra) { fputs("-- and the slowest window of this run so far --\n", f); fputs(s_slow_report.c_str(), f); fputc('\n', f); }
        fclose(f);
        rename(tmp, s_path);
    }
    char rawp[620]; snprintf(rawp, sizeof rawp, "%s", s_path);
    if (char* dot = strrchr(rawp, '.')) snprintf(dot, sizeof rawp - (size_t)(dot - rawp), "_raw.txt");
    snprintf(tmp, sizeof tmp, "%s.tmp", rawp);
    if (FILE* f = fopen(tmp, "w")) {
        for (const std::string& s : s_raws) fputs(s.c_str(), f);
        if (slow_extra) fputs(s_slow_raw.c_str(), f);
        fclose(f);
        rename(tmp, rawp);
    }
}

void* run(void*)
{
    const int self = (int)syscall(SYS_gettid);
    racacit_background_thread("profiler");   /* main.cpp: the small cores, low priority */
    timespec t0; clock_gettime(CLOCK_MONOTONIC, &t0);
    uint32_t from = __atomic_load_n(&s_n, __ATOMIC_RELAXED); unsigned flips0 = cellGcm_flip_request_count();
    for (int window = 1, tick = 0;; tick++) {
        if (tick % 2 == 0) scan(self);
        sleep(1);
        timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
        const double secs = (t.tv_sec - t0.tv_sec) + (t.tv_nsec - t0.tv_nsec) / 1e9;
        if (secs < kWindowS) continue;
        const uint32_t to = __atomic_load_n(&s_n, __ATOMIC_RELAXED); const unsigned flips = cellGcm_flip_request_count();
        report(window++, to - from > kCap ? to - kCap : from, to, secs, flips - flips0);
        from = to; flips0 = flips; t0 = t;
    }
    return nullptr;
}

}  // namespace

/* android_setup (main.cpp): only with RACACIT_PROFILE=1 (launcher > Diagnostics > Performance
 * profile): a sample per 2 ms of each thread's CPU time. */
extern "C" void racacit_profiler_start(const char* files_dir)
{
    const char* e = getenv("RACACIT_PROFILE");
    if (!(e && e[0] == '1')) return;
    s_ring = (Sample*)calloc(kCap, sizeof(Sample));
    if (!s_ring) return;
    snprintf(s_path, sizeof s_path, "%s/profile.txt", files_dir);
    struct sigaction sa; memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = on_sample; sa.sa_flags = SA_SIGINFO | SA_RESTART;
    sigemptyset(&sa.sa_mask);
    if (sigaction(SIGPROF, &sa, nullptr) != 0) return;
    pthread_t t;
    if (pthread_create(&t, nullptr, run, nullptr) == 0) pthread_detach(t);
    fprintf(stderr, "[prof] sampling profiler on: a report every %d s in %s%c", kWindowS, s_path, 10);
}
#endif
