#ifndef RACIT_ANDROID_PROFILER_RING_H
#define RACIT_ANDROID_PROFILER_RING_H

#include <stdint.h>

/* The ticket occupies the old padding: keep profiling memory at 16 B/sample.
 * tid is both the published owner and a non-blocking claim (-1 means busy).
 * A signal never waits for a reader/writer that it may have interrupted. */
struct RacitProfileSample { uint64_t pc; int32_t tid; uint32_t ticket; };
static_assert(sizeof(RacitProfileSample) == 16, "Profiler sample footprint changed");
static_assert(__atomic_always_lock_free(sizeof(int32_t), nullptr), "Signal handler requires lock-free claims");
static_assert(__atomic_always_lock_free(sizeof(uint32_t), nullptr), "Signal handler requires lock-free tickets");

static inline bool racit_profile_publish(RacitProfileSample* slot, uint32_t ticket, uint64_t pc, int32_t tid)
{
    if (__atomic_exchange_n(&slot->tid, -1, __ATOMIC_ACQUIRE) == -1) return false;
    slot->pc = pc;
    slot->ticket = ticket;
    __atomic_store_n(&slot->tid, tid, __ATOMIC_RELEASE);
    return true;
}

static inline bool racit_profile_read(RacitProfileSample* slot, uint32_t ticket, RacitProfileSample* out)
{
    const int32_t tid = __atomic_exchange_n(&slot->tid, -1, __ATOMIC_ACQUIRE);
    if (tid == -1) return false;
    const uint64_t pc = slot->pc;
    const uint32_t published = slot->ticket;
    __atomic_store_n(&slot->tid, tid, __ATOMIC_RELEASE);
    // Zero-initialized slots and overwritten/reserved-but-unpublished tickets
    // are skipped, including when the 32-bit ticket counter wraps.
    if (tid <= 0 || published != ticket) return false;
    out->pc = pc; out->tid = tid; out->ticket = published;
    return true;
}

#endif
