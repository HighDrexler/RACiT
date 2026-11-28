// Guest frame times for benchmarks (the headless runner's average FPS and 1% lows).
//
// Off unless RACIT_FRAME_INTERVALS names a file: then each guest present costs one cached
// getenv check. With it set, the game looks for "<file>.request" (seconds, lead-in) twice a
// second. When one appears it waits the lead-in, records every guest frame's interval for
// that many seconds (one clock read and one store per frame into a fixed array, no
// allocation and no IO), then writes the intervals to <file> and a summary to <file>.meta
// once. The runner computes the statistics.
#include <array>
#include <chrono>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <string>

extern "C" void racit_record_present(void)
{
    static const char* path = std::getenv("RACIT_FRAME_INTERVALS");
    if (!path || !*path) return;
    using Clock = std::chrono::steady_clock;
    static std::array<uint64_t, 262144> intervals;
    static size_t count;
    static int state; // idle, lead-in, measuring, finished
    static uint64_t next_check, ready_at, begin, previous, duration;
    const uint64_t now = (uint64_t)std::chrono::duration_cast<std::chrono::nanoseconds>(
        Clock::now().time_since_epoch()).count();
    if (state == 3) return;
    if (state == 0) {
        if (now < next_check) return;
        next_check = now + 500000000;
        FILE* request = std::fopen((std::string(path) + ".request").c_str(), "r");
        if (!request) return;
        unsigned seconds = 0, lead = 0;
        const int fields = std::fscanf(request, "%u %u", &seconds, &lead);
        std::fclose(request);
        if (fields != 2 || seconds < 10 || seconds > 180 || lead > 30) return;
        duration = (uint64_t)seconds * 1000000000;
        ready_at = now + (uint64_t)lead * 1000000000;
        state = 1;
    }
    if (state == 1) {
        if (now < ready_at) return;
        begin = previous = now;
        state = 2;
        return;
    }
    const bool fits = count < intervals.size();
    if (fits) intervals[count++] = now - previous;
    previous = now;
    if (fits && now - begin < duration) return;
    state = 3;
    bool complete = fits;
    FILE* raw = std::fopen(path, "wx");
    if (raw) {
        for (size_t i = 0; i < count; ++i)
            if (std::fprintf(raw, "%" PRIu64 "\n", intervals[i]) < 0) complete = false;
        if (std::fclose(raw) != 0) complete = false;
    } else complete = false;
    const std::string metadata = std::string(path) + ".meta";
    const std::string pending = metadata + ".tmp";
    FILE* meta = std::fopen(pending.c_str(), "wx");
    if (meta) {
        std::fprintf(meta, "{\"scope\":\"completed-headless-guest-flip-present-call\","
                     "\"interval_count\":%zu,\"start_ns\":%" PRIu64 ",\"end_ns\":%" PRIu64 ","
                     "\"complete\":%s}\n", count, begin, now, complete ? "true" : "false");
        if (std::fclose(meta) == 0) std::rename(pending.c_str(), metadata.c_str());
    }
    std::fprintf(stderr, "[racit frame times] window closed: %zu intervals, complete=%d\n", count, (int)complete);
}
