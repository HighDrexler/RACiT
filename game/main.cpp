/*
 * ps3recomp game project -- entry point template.
 *
 * This is the runner. It boots a lifted title through the toolkit's PPU
 * scaffold: allocate the flat guest VM, load the PPU ELF named on the command
 * line, register the lifted function table and the HLE NID handlers, start a
 * frame clock, and dispatch the entry OPD.
 *
 * The scaffold does the work; nothing below reimplements it. ppu_load_elf,
 * ppu_recomp_register, ppu_hle_init, ppu_sysprx_register, ppu_fs_register,
 * lv2_init_syscalls and ppu_run all come from the toolkit, compiled into this
 * project from PS3RECOMP_DIR (see CMakeLists.txt). What is left here is the
 * part a port owns: which backend to present through, how the frame clock is
 * paced, and whatever diagnostics the title turns out to need.
 *
 * runtime/ppu/tests/boot_main.cpp in the toolkit is the same boot in its
 * fully-instrumented form -- crash filter, hang watchdog, guest-PC sampling
 * profiler. Read it when a boot goes wrong; copy from it what the title
 * needs. It is deliberately not what a fresh project starts with.
 *
 * The one part that IS duplicated from it is the frame clock below, because a
 * port is expected to change its pacing and a shared one would be the wrong
 * shape for that. It was copied faithfully, comments and all. If a pacing bug
 * is fixed in one of the two, look at the other.
 *
 * This builds on Windows, macOS and Linux. CI builds and runs it against the
 * boot smoke title on the last two, and compile-checks it against clang-cl on
 * the first (tools/check_ppu_scaffold.py).
 */

/* The lifter's generated header, which declares ppu_context and the function
 * table. It comes first: everything below is written against that struct. */
#if defined(__x86_64__)
#include <cpuid.h>
#endif
#include "ppu_recomp.h"
/* Stall watch (the frame clock loop below): game/stubs.cpp, sys_ppu_thread.c,
 * cellGcmSys.c. */
extern "C" ppu_context* volatile g_racacit_main_ctx;
extern "C" void ppu_threads_dump_stacks(ppu_context* main_ctx);

/* The Win32 names -- Sleep, GetTickCount64, CreateThread, InterlockedIncrement,
 * VirtualAlloc, the scalar typedefs -- from one place. On Windows this is a
 * passthrough to <windows.h>; off Windows the toolkit's shim supplies the same
 * names over pthreads and mmap, so the call sites below are written once. */
#include "win32_compat.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#ifndef _WIN32
#include <sys/mman.h>
#include <unistd.h>   /* _exit */
#endif

#ifdef __ANDROID__
/* SDLActivity loads libmain.so and calls SDL_main: SDL.h renames main(). */
#include <SDL.h>
#include <android/log.h>
#include <fcntl.h>
#ifdef __ANDROID__
extern "C" { extern volatile long g_racacit_walker_tid; }   /* the FIFO walker thread, for racacit_lend_top_begin */
#endif
#include <unistd.h>
#include <sys/stat.h>
#include <sched.h>
#include <sys/resource.h>
#include <sys/prctl.h>
#include <pthread.h>
#include <linux/futex.h>
#if defined(__ANDROID__) && defined(__aarch64__)
extern "C" void racacit_fibers_dump(void);   /* spu_drain.c: the stall report's fiber and lane states */
#endif
#include <sys/syscall.h>
#include <dirent.h>
#include <errno.h>
extern "C" int rsx_ww_on(void);
#endif

#ifdef _WIN32
/* timeBeginPeriod: <windows.h> arrives with WIN32_LEAN_AND_MEAN set, which
 * excludes the multimedia timer API, so it is asked for by name -- and after
 * windows.h, since timeapi.h uses UINT. No shim exists off Windows because
 * there is nothing to shim: the 15.6 ms default timer granularity this works
 * around is a Windows problem. */
#include <timeapi.h>
#include <io.h>      /* _dup2 / _fileno */
#endif

/* ---------------------------------------------------------------------------
 * What the scaffold gives this file
 * -----------------------------------------------------------------------*/
extern "C" {
uint32_t ppu_load_elf(const char* path);      /* ELF -> guest RAM, returns entry OPD */
void     ppu_recomp_register(void);           /* generated: lifted table -> address map */
void     ppu_hle_init(void);                  /* firmware import NID -> HLE handler */
void     ppu_sysprx_register(void);           /* boot-critical CRT (sys_initialize_tls, ...) */
void     ppu_fs_register(void);               /* cellFs over the real game directory */
void     lv2_init_syscalls(void);             /* the lv2 syscall table */
int      ppu_run(uint32_t entry_opd, uint32_t stack_top);

/* This project's own: load whatever real system PRX modules the title needs
 * into guest RAM and register their exports. stubs.cpp has the empty version
 * a game with no lifted PRX wants. */
void     ps3_load_prx_modules(void);
void     racacit_register_language(void);     /* stubs.cpp: RACACIT_LANGUAGE */
void     racacit_register_trophies(void);     /* stubs.cpp: the bolt trophies' tiers */

extern const char* ppu_vfs_root;              /* host dir the PS3 mount points map into */

/* The status the guest handed to sys_process_exit. A title that exits that way
 * never returns through ppu_run, so this is only read on the path where the
 * entry function unwound instead. */
extern int     g_sys_process_exit_called;
extern int32_t g_sys_process_exit_code;

/* Host-provided symbols the runtime and the HLE libraries link against. */
uint8_t* vm_base = nullptr;
extern "C" void racacit_ring_dump(void);   /* spu_channels.c: RACACIT_LINE_RING */
extern "C" void racacit_collrec_stats(void);   /* stubs.cpp: collision record write-backs dropped */
extern "C" uint32_t ppu_hle_inject_base;   /* libs/video/cellGcmSys.c */
extern uint32_t ppu_vm_size;                  /* ppu_loader.cpp: the OOB guard */

/* At a stall: the state of the game's SPU job queues, always (owner logs have
 * no RACACIT_STALL_PEEK). The open "job posted, never taken" freeze (v40 level
 * 14, v91 Vorselon: main thread in job_wait func_00696614, every lane idle)
 * needs which queue the wait is on and what each side thinks it holds.
 *   igPm block B = [[0xD2FAA4] - 0x8000] (0x00FBE680): per queue q (0-4),
 *   done count B+0x9480+0x80q, posted count B+0x9700+0x80q (job_submit
 *   func_00695E84), kick-line pointer B+0x9994+4q, B+0x99A8+4q.
 *   Handle (func_00696614): bits 0-9 target count, 10-12 queue, 13 = the
 *   other path, 14+ frame; moby_prerender waits on [[0xD2F988] - 0x7FD8] + 0x3B0.
 *   Job manager lines 0x00F3CE00/E80/F00 (the v40 freeze's record and queue). */
static uint32_t stall_be32(uint32_t ea)
{
    if (!vm_base || ea < 0x10000u || ea + 4 > ppu_vm_size) return 0;
    const uint8_t* p = vm_base + ea;
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

static void stall_line(const char* what, uint32_t ea)
{
    ea &= ~127u;
    if (!vm_base || ea < 0x10000u || ea + 128 > ppu_vm_size) return;
    fprintf(stderr, "[stall-jobs] %s %08X:", what, ea);
    for (uint32_t o = 0; o < 128; o += 4) fprintf(stderr, " %08X", stall_be32(ea + o));
    fprintf(stderr, "%c", 10);
}

static void racacit_stall_jobs_dump(void)
{
    const uint32_t b = stall_be32(stall_be32(0xD2FAA4u) - 0x8000u);
    if (b) {
        fprintf(stderr, "[stall-jobs] igPm block 0x%08X, frame word %08X%c", b, stall_be32(b + 4), 10);
        for (uint32_t q = 0; q < 5; q++)
            fprintf(stderr, "[stall-jobs]   queue %u: done %08X posted %08X  99A8 %08X  kick line 0x%08X%c", q,
                    stall_be32(b + 0x9480 + 0x80 * q), stall_be32(b + 0x9700 + 0x80 * q),
                    stall_be32(b + 0x99A8 + 4 * q), stall_be32(b + 0x9994 + 4 * q), 10);
        for (uint32_t q = 0; q < 5; q++) {
            const uint32_t k = stall_be32(b + 0x9994 + 4 * q);
            if (k) stall_line("kick", k);
        }
    }
    const uint32_t m = stall_be32(stall_be32(0xD2F988u) - 0x7FD8u);
    if (m) {
        /* A doubleword, zeroed by job_wait once the job is done: non-zero = still waited on. */
        const uint32_t hi = stall_be32(m + 0x3B0), h = stall_be32(m + 0x3B4);
        fprintf(stderr, "[stall-jobs] moby_prerender handle %08X%08X: queue %u target %u%s frame %u%c",
                hi, h, (h >> 10) & 7, h & 0x3FF, (h & 0x2000) ? " (other path)" : "", h >> 14, 10);
    }
    stall_line("jobmgr", 0x00F3CE00u);
    stall_line("jobmgr", 0x00F3CE80u);
    stall_line("jobmgr", 0x00F3CF00u);
}

/* Guest-callback dispatch. g_ps3_guest_caller is the hook the HLE runtime
 * calls back into recompiled code through -- cellSysutil events and the GCM
 * vblank/flip handlers. ppu_guest_call does the OPD -> dispatch. */
typedef void (*ps3_guest_caller_fn)(uint32_t, uint64_t, uint64_t, uint64_t, uint64_t,
                                    uint64_t, uint64_t, uint64_t, uint64_t);
extern ps3_guest_caller_fn g_ps3_guest_caller;
uint64_t ppu_guest_call(uint32_t, uint64_t, uint64_t, uint64_t, uint64_t,
                        uint64_t, uint64_t, uint64_t, uint64_t);

/* The GCM half of the frame clock. */
void     cellGcmTickVBlank(void);
void     cellGcmTickFlip(void);
int      cellGcm_take_flip_pending(void);
void     cellGcm_rsx_process_fifo(void);      /* drain get -> put */
unsigned cellGcm_flip_request_count(void);
extern void (*g_gcm_wake_walker)(void);       /* wake frame_clock for a drain now */
}

/* ---------------------------------------------------------------------------
 * RSX present backend
 * -----------------------------------------------------------------------*/
/* D3D12 on Windows, Metal on Apple, and the null backend's headless software
 * path anywhere else -- the same selection runtime/host/host_posix.c and the
 * toolkit's boot harness make. All three expose the same three entry points,
 * so the frame clock below is backend-agnostic. */
#if defined(_WIN32) && !defined(RACACIT_HAVE_VULKAN)
extern "C" int  rsx_d3d12_backend_init(uint32_t w, uint32_t h, const char* title);
extern "C" void rsx_d3d12_backend_present(void);
extern "C" int  rsx_d3d12_backend_pump_messages(void);
#  define rsx_backend_init    rsx_d3d12_backend_init
#  define rsx_backend_present rsx_d3d12_backend_present
#  define rsx_backend_pump    rsx_d3d12_backend_pump_messages
#  define RSX_BACKEND_NAME    "D3D12"
#elif defined(__APPLE__) && !defined(RACACIT_HAVE_VULKAN)
extern "C" int  rsx_metal_backend_init(uint32_t w, uint32_t h, const char* title);
extern "C" void rsx_metal_backend_present(void);
extern "C" int  rsx_metal_backend_pump_messages(void);
#  define rsx_backend_init    rsx_metal_backend_init
#  define rsx_backend_present rsx_metal_backend_present
#  define rsx_backend_pump    rsx_metal_backend_pump_messages
#  define RSX_BACKEND_NAME    "Metal"
#else
/* Linux / Android / Windows: Vulkan when the toolkit was built with it (offscreen, frames
 * read back; PS3RECOMP_VK_DUMP / PS3RECOMP_VK_WINDOW to see them), else -- or
 * with RACACIT_RSX=null, or when no Vulkan device is present -- the headless
 * software null backend. Chosen once at init; both expose the same calls. */
#  if defined(_WIN32)
/* Windows has no headless software fallback: Vulkan or a clear error. */
static void rsx_none_present(void) {}
static int  rsx_none_pump(void) { return 0; }
#    define rsx_null_backend_present        rsx_none_present
#    define rsx_null_backend_pump_messages  rsx_none_pump
static int rsx_null_backend_init(uint32_t, uint32_t, const char*)
{
    MessageBoxA(NULL, "Vulkan could not be started.\n\nUpdate your graphics driver "
                "(NVIDIA, AMD or Intel) -- the game needs Vulkan 1.0 or newer.\n\n"
                "Details are in log.txt next to RaCACiT.exe.",
                "Ratchet & Clank: A Crack in Time", MB_OK | MB_ICONERROR);
    fflush(stdout); fflush(stderr);
    TerminateProcess(GetCurrentProcess(), 3);
    return -1;
}
#  else
extern "C" int  rsx_null_backend_init(uint32_t w, uint32_t h, const char* title);
extern "C" void rsx_null_backend_present(void);
extern "C" int  rsx_null_backend_pump_messages(void);
#  endif
#  if defined(RACACIT_HAVE_VULKAN)
extern "C" int  rsx_vulkan_backend_init(uint32_t w, uint32_t h, const char* title);
extern "C" void rsx_vulkan_backend_present(void);
extern "C" int  rsx_host_ui_modal(void);
extern "C" unsigned long long g_rt_drain_total_us, g_rt_present_total_us;
extern "C" int  rsx_vulkan_backend_pump_messages(void);
extern "C" int  rsx_vulkan_prepare_shaders(void);
#  endif
static void (*g_rsx_present)(void) = rsx_null_backend_present;
static int  (*g_rsx_pump)(void)    = rsx_null_backend_pump_messages;
static const char* g_rsx_name      = "null (headless software)";
static int rsx_backend_init_linux(uint32_t w, uint32_t h, const char* title)
{
#  if defined(RACACIT_HAVE_VULKAN)
    const char* sel = getenv("RACACIT_RSX");
    if (!sel || strcmp(sel, "null") != 0) {
        if (rsx_vulkan_backend_init(w, h, title) == 0) {
            g_rsx_present = rsx_vulkan_backend_present;
            g_rsx_pump    = rsx_vulkan_backend_pump_messages;
            g_rsx_name    = "Vulkan";
            return 0;
        }
#    if defined(__ANDROID__)
        fprintf(stderr, "[rsx] Vulkan initialization failed; Android requires a working Vulkan renderer\n");
        return -1;
#    else
        fprintf(stderr, "[rsx] Vulkan init failed -- falling back to the null backend\n");
#    endif
    }
#  endif
#  if defined(__ANDROID__)
    return -1;
#  else
    return rsx_null_backend_init(w, h, title);
#  endif
}
#  define rsx_backend_init    rsx_backend_init_linux
#  define rsx_backend_present g_rsx_present
#  define rsx_backend_pump    g_rsx_pump
#  define RSX_BACKEND_NAME    g_rsx_name
#endif

/* ---------------------------------------------------------------------------
 * Configuration
 * -----------------------------------------------------------------------*/
/* The flat VM must span every region the PS3 memory map uses, not just the
 * game image: sys_ppu_thread_create puts thread stacks at 0xD0000000, so a
 * smaller arena makes every spawned thread's stack access out of bounds. */
#define VM_SIZE       0x100010000ull   /* the full 32-bit guest space + a 64K guard */
#define STACK_TOP     0x0FF00000u      /* main-thread stack, below the 0x10000000 segment */
#define WINDOW_WIDTH  1280
#define WINDOW_HEIGHT 720

/* ---------------------------------------------------------------------------
 * Frame clock
 * -----------------------------------------------------------------------*/
/* On real hardware the RSX raises a vblank interrupt about 60 times a second
 * and that is what drives the game's frame loop. With no RSX it is synthesized
 * here: a host thread calls cellGcmTickVBlank/TickFlip, which invoke the
 * handlers the guest registered, and drains the GCM FIFO. Without this the
 * title initialises, registers its handlers and then waits forever.
 *
 * The ticks are driven off real elapsed time rather than off how long present()
 * took. A hidden or occluded window makes present block hard, and pacing the
 * ticks behind it paces the whole game behind it. */
static volatile LONG g_frames_presented = 0;

/* Frames handed to the backend at a guest FLIP boundary -- one per frame the
 * guest actually finished. The presents made before the guest's first flip, so
 * a fresh window is not left blank through a long boot, carry no guest frame
 * and are deliberately not counted. */
extern "C" unsigned ppu_boot_frames_presented(void)
{
    return (unsigned)g_frames_presented;
}

static unsigned long long rt_now_us(void);
static unsigned long long s_rt_present_us;
extern "C" void ps3_tl_at(unsigned long long t, char ev, uint32_t a, uint32_t b);
extern "C" void ps3_tl(char ev, uint32_t a, uint32_t b);
extern "C" unsigned long long ps3_qpc_us(void);
static void present_guest_frame(void)
{
    unsigned long long _t = rt_now_us();
    ps3_tl('P', 0, 0);
    rsx_backend_present();
    s_rt_present_us += rt_now_us() - _t;
    g_rt_present_total_us += rt_now_us() - _t;
    /* This thread increments and guest threads read, so interlocked rather
     * than a volatile ++, which on arm64 is neither atomic nor a fence. */
    InterlockedIncrement(&g_frames_presented);
}

/* The FIFO walker runs on frame_clock. A guest thread that has just moved
 * `put` and needs it consumed now (cellGcm_fifo_wake / cellGcm_fifo_sync)
 * cuts the loop's 4 ms sleep short through this event. Android has it too
 * (win32_compat's events): without it the walker slept a fixed 4 ms a pass,
 * and every RSX label the guest waited on cost up to 4 ms -- 52 fps in an
 * idle menu, ~40 in play, with every thread mostly waiting. */
#if defined(_WIN32) || defined(__ANDROID__)
extern "C" int racacit_few_cpus(void);   /* sys_timer.c */
static HANDLE s_walker_wake;
static void walker_wake(void) { if (s_walker_wake) SetEvent(s_walker_wake); }
#endif

/* RSX_THREAD_STATS=1: where the frame-clock thread's time goes, every 5 s --
 * draining the FIFO (which issues every draw), presenting, and idle. */
static unsigned long long s_rt_drain_us, s_rt_t0;
static unsigned long long rt_now_us(void)
{
    LARGE_INTEGER f, c; QueryPerformanceFrequency(&f); QueryPerformanceCounter(&c);
    return (unsigned long long)(c.QuadPart / (f.QuadPart / 1000000));
}
static void timed_drain(void)
{
    unsigned long long t = rt_now_us();
    const unsigned long long q = ps3_qpc_us();
    cellGcm_rsx_process_fifo();
    const unsigned long long d = rt_now_us() - t;
    s_rt_drain_us += d;
    g_rt_drain_total_us += d;
    if (d >= 100) ps3_tl_at(q, 'D', (uint32_t)d, 0);   /* PS3_TIMELINE: drains that did work */
}
static void rt_report(void)
{
    static int on = -1;
    if (on < 0) on = getenv("RSX_THREAD_STATS") ? 1 : 0;
    if (!on) return;
    unsigned long long now = rt_now_us();
    if (!s_rt_t0) { s_rt_t0 = now; return; }
    if (now - s_rt_t0 < 5000000ull) return;
    const double span = (double)(now - s_rt_t0);
    fprintf(stderr, "[rsx-thread] drain %.1f%%  present %.1f%%  other/idle %.1f%%%c",
            100.0 * s_rt_drain_us / span, 100.0 * s_rt_present_us / span,
            100.0 - 100.0 * (s_rt_drain_us + s_rt_present_us) / span, 10);
    s_rt_drain_us = s_rt_present_us = 0; s_rt_t0 = now;
}

/* Hardware write watch (dev, via peek.txt "w ea_hex"): DR0 on every thread
 * traps writes to 4 bytes of guest memory; the handler logs the host
 * instruction address (exe RVA -> scripts/dev/symbolize.py names the lifted
 * function) and code addresses found on the stack, for SPU DMA copies whose
 * own frame is in the CRT. */
#ifdef _WIN32
#include <tlhelp32.h>
static volatile uint64_t s_hw_addr[4];      /* host addresses; 0 = slot unused */
static volatile int s_hw_rw[4];              /* 1: break on reads too ("r" lines), every hit logged */
static float s_hw_lo[4], s_hw_hi[4];          /* optional float range (lo < hi) */
static volatile LONG s_hw_hits;
static LONG CALLBACK hw_watch_handler(EXCEPTION_POINTERS* ep)
{
    if (ep->ExceptionRecord->ExceptionCode != EXCEPTION_SINGLE_STEP) return EXCEPTION_CONTINUE_SEARCH;
    CONTEXT* c = ep->ContextRecord;
    const unsigned slot = (c->Dr6 & 1) ? 0 : (c->Dr6 & 2) ? 1 : (c->Dr6 & 4) ? 2 : (c->Dr6 & 8) ? 3 : 4;
    if (slot > 3 || !s_hw_addr[slot]) return EXCEPTION_CONTINUE_SEARCH;
    c->Dr6 = 0;
    uint32_t v; memcpy(&v, (const void*)s_hw_addr[slot], 4);
    const uint32_t vb = _byteswap_ulong(v);
    float fv; memcpy(&fv, &vb, 4);
    { static uint32_t last[4] = { 0x7FC00001u, 0x7FC00001u, 0x7FC00001u, 0x7FC00001u };   /* value changes only */
      if (!s_hw_rw[slot] && last[slot] == vb) return EXCEPTION_CONTINUE_EXECUTION;
      last[slot] = vb; }
    /* With a range: only values in it. Without: the first 20 writes, then
     * only those leaving a NaN or Inf (exponent 0xFF). At most 400 lines. */
    if (s_hw_lo[slot] < s_hw_hi[slot]) {
        if (!(fv >= s_hw_lo[slot] && fv <= s_hw_hi[slot])) return EXCEPTION_CONTINUE_EXECUTION;
        if (InterlockedIncrement(&s_hw_hits) > 400) return EXCEPTION_CONTINUE_EXECUTION;
    } else {
        /* PS3_HWWATCH_ALL=1: every change (still 400 lines at most). */
        static int all = -1;
        if (all < 0) all = getenv("PS3_HWWATCH_ALL") ? 1 : 0;
        const LONG hit = InterlockedIncrement(&s_hw_hits);
        if (hit > 400 || (!all && hit > 20 && ((vb >> 23) & 0xFF) != 0xFF)) {
            if (hit > 20) InterlockedDecrement(&s_hw_hits);
            return EXCEPTION_CONTINUE_EXECUTION;
        }
    }
    const uintptr_t base = (uintptr_t)GetModuleHandleA(NULL);
    char buf[600]; int n = snprintf(buf, sizeof buf, "[hwwatch] t=%llu slot %u tid=%lu value=%08X (%g) rip=%p rva=0x%llX stack:",
        rt_now_us(), slot, GetCurrentThreadId(), vb, fv, (void*)c->Rip, (unsigned long long)(c->Rip - base));
    const uintptr_t* sp = (const uintptr_t*)c->Rsp;
    ULONG_PTR st_lo = 0, st_hi = 0;
    GetCurrentThreadStackLimits(&st_lo, &st_hi);   /* never read past the top of this thread's stack */
    const int st_n = st_hi > (ULONG_PTR)sp ? (int)((st_hi - (ULONG_PTR)sp) / sizeof(uintptr_t)) : 0;
    for (int i = 0, k = 0; i < 256 && i < st_n && k < 8 && n < 560; i++) {
        uintptr_t a = sp[i];
        if (a > base + 0x1000 && a < base + 0x8000000) { n += snprintf(buf + n, sizeof buf - n, " 0x%llX", (unsigned long long)(a - base)); k++; }
    }
    fprintf(stderr, "%s%c", buf, 10);
    return EXCEPTION_CONTINUE_EXECUTION;
}
static void hw_watch_apply(void)
{
    if (!s_hw_addr[0] && !s_hw_addr[1] && !s_hw_addr[2] && !s_hw_addr[3]) return;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snap == INVALID_HANDLE_VALUE) return;
    THREADENTRY32 te; te.dwSize = sizeof te;
    const DWORD me = GetCurrentThreadId(), pid = GetCurrentProcessId();
    for (BOOL ok = Thread32First(snap, &te); ok; ok = Thread32Next(snap, &te)) {
        if (te.th32OwnerProcessID != pid || te.th32ThreadID == me) continue;
        HANDLE t = OpenThread(THREAD_GET_CONTEXT | THREAD_SET_CONTEXT | THREAD_SUSPEND_RESUME, FALSE, te.th32ThreadID);
        if (!t) continue;
        if (SuspendThread(t) != (DWORD)-1) {
            CONTEXT c; memset(&c, 0, sizeof c); c.ContextFlags = CONTEXT_DEBUG_REGISTERS;
            if (GetThreadContext(t, &c) && (c.Dr0 != s_hw_addr[0] || c.Dr1 != s_hw_addr[1] ||
                                            c.Dr2 != s_hw_addr[2] || c.Dr3 != s_hw_addr[3])) {
                c.Dr0 = s_hw_addr[0]; c.Dr1 = s_hw_addr[1]; c.Dr2 = s_hw_addr[2]; c.Dr3 = s_hw_addr[3];
                uint64_t d7 = 0;
                for (unsigned i = 0; i < 4; i++)
                    if (s_hw_addr[i]) d7 |= (1ull << (2 * i)) | ((s_hw_rw[i] ? 3ull : 1ull) << (16 + 4 * i)) | (3ull << (18 + 4 * i));   /* Li, write (or rw), 4 bytes */
                c.Dr7 = d7;
                SetThreadContext(t, &c);
            }
            ResumeThread(t);
        }
        CloseHandle(t);
    }
    CloseHandle(snap);
}

/* PS3_PEEK=1 (dev): every 100 ms, if peek.txt exists in the working directory,
 * dump each "ea_hex len_hex out_path" line's guest memory to out_path, then
 * delete peek.txt -- memory snapshots at moments a test script picks. A line
 * "w <slot> <ea_hex> [<lo> <hi>]" arms the hardware write watch above instead. */
static DWORD WINAPI peek_thread(LPVOID)
{
    for (;;) {
        Sleep(100);
        hw_watch_apply();   /* threads created since the last pass */
        FILE* f = fopen("peek.txt", "r");
        if (!f) continue;
        char line[600];
        while (fgets(line, sizeof line, f)) {
            /* "w <slot 0-3> <ea_hex> [<lo> <hi>]": watch writes there (float range filter). */
            unsigned wslot = 0, wea = 0; float wlo = 0, whi = 0;
            int wn = sscanf(line, "w %u %x %f %f", &wslot, &wea, &wlo, &whi);
            int rw = 0;
            if (wn < 2 && sscanf(line, "r %u %x", &wslot, &wea) == 2) { wn = 2; rw = 1; }   /* "r": reads and writes */
            if (wn >= 2 && wslot < 4) {
                static int vh = 0;
                if (!vh) { vh = 1; AddVectoredExceptionHandler(1, hw_watch_handler); }
                s_hw_rw[wslot] = rw; s_hw_lo[wslot] = wn == 4 ? wlo : 0; s_hw_hi[wslot] = wn == 4 ? whi : 0;
                s_hw_addr[wslot] = (uint64_t)(uintptr_t)(vm_base + (wea & ~3u));
                fprintf(stderr, "[hwwatch] slot %u armed on 0x%08X%c", wslot, wea & ~3u, 10);
                continue;
            }
            /* "u <value_hex> <start_hex> <len_hex>": every aligned big-endian
             * u32 equal to value (committed pages only), with the 32 bytes
             * around it -- where a structure holding that word lives. */
            { unsigned uv = 0, us = 0, ul = 0;
              if (sscanf(line, "u %x %x %x", &uv, &us, &ul) == 3) {
                  unsigned hits = 0;
                  for (unsigned a = us & ~3u; a < us + ul && hits < 64; ) {
                      MEMORY_BASIC_INFORMATION mbi;
                      if (!VirtualQuery(vm_base + a, &mbi, sizeof mbi)) break;
                      const unsigned rend = (unsigned)((uint8_t*)mbi.BaseAddress + mbi.RegionSize - vm_base);
                      if (mbi.State == MEM_COMMIT && !(mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD))) {
                          for (unsigned p = a; p + 4 <= rend && p < us + ul && hits < 64; p += 4) {
                              uint32_t b; memcpy(&b, vm_base + p, 4);
                              if (_byteswap_ulong(b) != uv) continue;
                              char hx[200]; int k = 0;
                              for (unsigned q = (p >= 16 ? p - 16 : 0); q < p + 16 && k < 190; q++) k += snprintf(hx + k, sizeof hx - k, "%02X%s", vm_base[q], q + 1 == p ? "|" : (q + 1 == p + 4 ? "|" : ""));
                              fprintf(stderr, "[peek-u] 0x%08X = %08X  around: %s%c", p, uv, hx, 10); hits++;
                          }
                      }
                      a = rend > a ? rend : a + 0x1000;
                  }
                  fprintf(stderr, "[peek-u] %08X: %u hit(s)%c", uv, hits, 10);
                  continue;
              } }
            /* "f <value> <tolerance> <start_hex> <len_hex>": every aligned
             * big-endian float within tolerance, committed pages only. */
            { float fv = 0, tol = 0; unsigned fs = 0, fl = 0;
              if (sscanf(line, "f %f %f %x %x", &fv, &tol, &fs, &fl) == 4) {
                  unsigned hits = 0;
                  for (unsigned a = fs & ~3u; a < fs + fl && hits < 64; ) {
                      MEMORY_BASIC_INFORMATION mbi;
                      if (!VirtualQuery(vm_base + a, &mbi, sizeof mbi)) break;
                      const unsigned rend = (unsigned)((uint8_t*)mbi.BaseAddress + mbi.RegionSize - vm_base);
                      if (mbi.State == MEM_COMMIT && !(mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD))) {
                          for (unsigned p = a; p + 4 <= rend && p < fs + fl && hits < 64; p += 4) {
                              uint32_t b; memcpy(&b, vm_base + p, 4); b = _byteswap_ulong(b);
                              float x; memcpy(&x, &b, 4);
                              if (x == x && fabsf(x - fv) <= tol) { fprintf(stderr, "[peek-f] 0x%08X = %g%c", p, x, 10); hits++; }
                          }
                      }
                      a = rend > a ? rend : a + 0x1000;
                  }
                  fprintf(stderr, "[peek-f] %g +- %g: %u hit(s)%c", fv, tol, hits, 10);
                  continue;
              } }
            unsigned ea = 0, len = 0; char out[512];
            if (sscanf(line, "%x %x %511s", &ea, &len, out) != 3 || !len || len > 0x1000000u) continue;
            FILE* o = fopen(out, "wb");
            if (o) { fwrite(vm_base + ea, 1, len, o); fclose(o); }
            fprintf(stderr, "[peek] 0x%08X+0x%X -> %s%c", ea, len, out, 10);
        }
        fclose(f);
        remove("peek.txt");
    }
    return 0;
}
#else
/* Android: no debug-register write watch; peek.txt is not offered. */
static DWORD WINAPI peek_thread(LPVOID) { return 0; }
#endif

#if defined(_WIN32) && !defined(__ANDROID__)
/* [mem], once a minute: what a long session holds -- the process's private
 * bytes, working set and OS handles (a host leak shows as a climb), the lines
 * SPU reservations have marked for the PPU's locked store path (spu_coherency.c
 * never unmarks them) with the rate of writes into them, and the Vulkan image
 * pool's live bytes against its blocks (fragmentation). The owner's next log
 * says whether any of these grow with play time, without a soak. */
#ifndef PSAPI_VERSION
#define PSAPI_VERSION 2
#endif
#include <psapi.h>
extern "C" unsigned long g_spu_coh_lines, g_spu_coh_writes, g_ppu_coh_stores;
extern "C" void rsx_vk_pool_stats(unsigned long long* live, unsigned long long* cap, unsigned* blocks);
static void racacit_memlog(unsigned long long now_us)
{
    static unsigned long long t0, last; static unsigned long w_last, p_last;
    if (!t0) { t0 = last = now_us; w_last = g_spu_coh_writes; p_last = g_ppu_coh_stores; return; }
    if (now_us - last < 60000000ull) return;
    const double secs = (double)(now_us - last) / 1e6;
    last = now_us;
    PROCESS_MEMORY_COUNTERS_EX pm; memset(&pm, 0, sizeof pm); pm.cb = sizeof pm;
    K32GetProcessMemoryInfo(GetCurrentProcess(), (PROCESS_MEMORY_COUNTERS*)&pm, sizeof pm);
    DWORD handles = 0; GetProcessHandleCount(GetCurrentProcess(), &handles);
    unsigned long long live = 0, cap = 0; unsigned blocks = 0;
    rsx_vk_pool_stats(&live, &cap, &blocks);
    const unsigned long w = g_spu_coh_writes, pw = g_ppu_coh_stores;
    fprintf(stderr, "[mem] %llu min: private %llu MB, working set %llu MB, %lu handles; SPU-reserved lines %lu "
                    "(PPU stores into them %.0f/s, SPU atomic commits %.0f/s); image pool %llu of %llu MB live in %u blocks\n",
            (now_us - t0) / 60000000ull, (unsigned long long)pm.PrivateUsage >> 20,
            (unsigned long long)pm.WorkingSetSize >> 20, (unsigned long)handles, g_spu_coh_lines,
            (double)(pw - p_last) / secs, (double)((w - w_last) - (pw - p_last)) / secs, live >> 20, cap >> 20, blocks);
    w_last = w; p_last = pw;
}
#endif

extern "C" int cellGcm_fifo_pending(void);
#ifdef __ANDROID__
static void android_drain_startup_log();
#endif

static DWORD WINAPI frame_clock(LPVOID)
{
    const char* title = getenv("PS3_TITLE");
    if (!title || !*title) title = "ps3recomp";
#if defined(_WIN32) || defined(__ANDROID__)
    s_walker_wake = CreateEventA(NULL, FALSE, FALSE, NULL);
    g_gcm_wake_walker = walker_wake;
#endif
#ifdef _WIN32
    /* PS3_SAMPLE_RSX=1: the sampler profiles this thread only. */
    { extern volatile unsigned long g_samp_only_tid;
      if (getenv("PS3_SAMPLE_RSX")) g_samp_only_tid = GetCurrentThreadId(); }
#endif

#ifdef __ANDROID__
    /* Named here, before the window opens: SDL attaches this thread to Java under its name
     * then, and a new thread starts with its creator's ("SDLThread"), which stuck when the
     * attach came before the log pump's rename (Thor, v113-android.2: top said SDLThread). */
    prctl(PR_SET_NAME, "rsx frame clock", 0, 0, 0);
    g_racacit_walker_tid = (long)gettid();   /* racacit_lend_top_begin */
#endif
    { char _hb[160]; snprintf(_hb, sizeof _hb, "[hosttid] %lu rsx frame clock / FIFO walker%c", (unsigned long)GetCurrentThreadId(), 10); fputs(_hb, stderr); }
    int rsx_ok = (rsx_backend_init(WINDOW_WIDTH, WINDOW_HEIGHT, title) == 0);
    fprintf(stderr, "[rsx] %s backend init %s\n", RSX_BACKEND_NAME,
            rsx_ok ? "OK -- window open" : "FAILED");
#ifdef __ANDROID__
    if (!rsx_ok) {
        android_drain_startup_log();
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "RACiT graphics unavailable",
            "The Vulkan renderer could not start on this device.\n\n"
            "Open Diagnostics > Last game log in the launcher to see the Vulkan error. "
            "If the lines are missing, turn on Logging and try again, then share log.txt.", nullptr);
        exit(EXIT_FAILURE);
    }
#endif

    unsigned  last_flip = 0;
    /* The vblank beat, on the microsecond clock: GetTickCount64 only moves
     * every ~15.6 ms (timeBeginPeriod does not change that), so a 16 ms step
     * on it gave ticks 15, 16 or 31 ms apart -- now and then two at once --
     * and a vsync-paced title lost a frame each time: ~58 fps where it should
     * hold 60. RACACIT_VBLANK_HZ=<hz> (default 60; a PS3's NTSC rate is 59.94). */
    unsigned long long tick_us = 16667;
    { const char* e = getenv("RACACIT_VBLANK_HZ"); const double hz = e ? atof(e) : 60.0;
      if (hz >= 10.0 && hz <= 1000.0) tick_us = (unsigned long long)(1000000.0 / hz + 0.5); }
    unsigned long long next_tick = rt_now_us();

    /* RSX_WALKER_POLL_MS: how long the walker sleeps when nothing woke it.
     * Most of a title's FIFO kicks do not pass through the wake hook, so this
     * is the latency between the guest kicking work and the RSX starting it --
     * and the guest waits on RSX labels every frame. */
    static DWORD s_poll_ms = 0;
    if (!s_poll_ms) { const char* e = getenv("RSX_WALKER_POLL_MS"); s_poll_ms = e ? (DWORD)atoi(e) : 1; if (!s_poll_ms) s_poll_ms = 1; }
    for (;;) {
#if defined(_WIN32) && !defined(__ANDROID__)
        racacit_memlog(rt_now_us());
#endif
#if defined(_WIN32) || defined(__ANDROID__)
        /* Spin (yielding) up to ~1 ms for new FIFO work before sleeping: the
         * guest waits on RSX labels every frame, so every ms between its kick
         * and the walker starting is a ms of its frame time. */
        if (!cellGcm_fifo_pending()) {
            /* RACACIT_WALKER_SPIN_US: how long (us) to spin before sleeping. */
            static LONGLONG s_spin_us = -1;
            if (s_spin_us < 0) { const char* e = getenv("RACACIT_WALKER_SPIN_US"); s_spin_us = e ? atoll(e) : (racacit_few_cpus() ? 50 : 1000); }
            LARGE_INTEGER f_, a_, n_; QueryPerformanceFrequency(&f_); QueryPerformanceCounter(&a_);
            const LONGLONG lim_ = f_.QuadPart * s_spin_us / 1000000;
            int got_ = 0;
#if defined(__ANDROID__)
            /* v113-android.48: the window is polled every 10 us asleep, not with sched_yield. ~10,000
             * yields a second here (3.0% of this thread's samples in the owner's .47 profile, the
             * Fongoid fight); on the full mid cores one that finds a lane runnable gives it the core
             * to the next 4 ms tick, and the guest waits on this thread's RSX labels every frame. A
             * short sleep frees the core just the same and wakes ahead of the lane. The render thread
             * has no timer slack (android_setup). RACACIT_WALKER_YIELD=1: as before. */
            static int s_walk_yield = -1;
            if (s_walk_yield < 0) { const char* e = getenv("RACACIT_WALKER_YIELD"); s_walk_yield = e && *e == '1'; }
            if (!s_walk_yield) {
                do { const struct timespec d_ = { 0, 10000L }; nanosleep(&d_, NULL);
                     if (cellGcm_fifo_pending()) { got_ = 1; break; }
                     QueryPerformanceCounter(&n_); } while (n_.QuadPart - a_.QuadPart < lim_);
            } else
#endif
            do { SwitchToThread(); if (cellGcm_fifo_pending()) { got_ = 1; break; }
                 QueryPerformanceCounter(&n_); } while (n_.QuadPart - a_.QuadPart < lim_);
            if (!got_) {
                if (s_walker_wake) WaitForSingleObject(s_walker_wake, s_poll_ms);
                else               Sleep(s_poll_ms);
            }
        }
#else
        Sleep(4);
#endif
        ULONGLONG now = GetTickCount64();
        const unsigned long long now_us = rt_now_us();

        /* Stall watch: no guest flip request for 5 s once the game is up
         * (300 flips) logs every guest thread's stack -- where the title is
         * stuck, from the owner's own log. Seen: the idle title screen
         * sometimes stops before its attract movie (HANDOFF, v35). At most
         * 3 dumps a run, one per stall. RACACIT_STALL_DUMP=0 = off. */
        { static int s_on = -1; static ULONGLONG s_t; static unsigned s_last, s_dumps; static ULONGLONG s_since;
          if (s_on < 0) { const char* e = getenv("RACACIT_STALL_DUMP"); s_on = !(e && *e == '0'); }
          if (s_on && now - s_t >= 1000) {
              s_t = now;
              const unsigned f = cellGcm_flip_request_count();
              if (f != s_last) { s_last = f; s_since = now; }
              else if (f >= 300 && s_since && now - s_since >= 5000 && s_dumps < 3) {
                  s_dumps++; s_since = 0;   /* once per stall: re-armed by the next flip */
                  fprintf(stderr, "[stall] no guest flip for 5 s at flip %u: guest thread stacks follow\n", f);
                  ppu_threads_dump_stacks(g_racacit_main_ctx);
#if defined(__ANDROID__) && defined(__aarch64__)
                  racacit_fibers_dump();
#endif
                  /* RACACIT_STALL_PEEK=<hex>[,<hex>...]: those guest 128-byte
                   * lines too -- the record and queue a stuck wait is on. */
                  { const char* pk = getenv("RACACIT_STALL_PEEK");
                    while (pk && *pk) {
                        char* end = nullptr; const uint32_t ea = (uint32_t)strtoul(pk, &end, 16) & ~127u;
                        if (end == pk) break;
                        for (uint32_t o = 0; o < 128; o += 16) {
                            const uint8_t* q = vm_base + ea + o;
                            fprintf(stderr, "[stall-peek] %08X: %02X%02X%02X%02X %02X%02X%02X%02X %02X%02X%02X%02X %02X%02X%02X%02X%c",
                                    ea + o, q[0],q[1],q[2],q[3], q[4],q[5],q[6],q[7], q[8],q[9],q[10],q[11], q[12],q[13],q[14],q[15], 10);
                        }
                        pk = (*end == ',') ? end + 1 : end;
                    } }
                  racacit_stall_jobs_dump();
                  racacit_collrec_stats();
                  racacit_ring_dump();   /* RACACIT_LINE_RING: the line's recent history */
                  fflush(stderr);
              }
          } }

        /* RACACIT_RAMDUMP=<dir> (test-only): when <dir>\req appears, guest
         * memory (the committed parts of 0x00010000-0x40000000: image, heap,
         * user memory) goes to
         * <dir>\ram_<n>.bin as records <ea u32><len u32><bytes> -- for diffing
         * game state (which weapon, its ammo) between two moments. */
#ifdef _WIN32
        { static const char* s_dir = (const char*)1; static ULONGLONG s_t; static unsigned s_n;
          if (s_dir == (const char*)1) s_dir = getenv("RACACIT_RAMDUMP");
          if (s_dir && now - s_t >= 250) {
              s_t = now;
              char req[512]; snprintf(req, sizeof req, "%s\\req", s_dir);
              if (GetFileAttributesA(req) != INVALID_FILE_ATTRIBUTES) {
                  DeleteFileA(req);
                  char out[512]; snprintf(out, sizeof out, "%s\\ram_%u.bin", s_dir, s_n++);
                  FILE* f = fopen(out, "wb"); unsigned long long tot = 0;
                  for (uint32_t a = 0x10000u; f && a < 0x40000000u; ) {
                      MEMORY_BASIC_INFORMATION mbi;
                      if (!VirtualQuery(vm_base + a, &mbi, sizeof mbi)) break;
                      uint32_t end = (uint32_t)((uint8_t*)mbi.BaseAddress + mbi.RegionSize - vm_base);
                      if (end > 0x40000000u || end <= a) end = 0x40000000u;
                      if (mbi.State == MEM_COMMIT && !(mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD))) {
                          const uint32_t len = end - a;
                          fwrite(&a, 4, 1, f); fwrite(&len, 4, 1, f); fwrite(vm_base + a, 1, len, f); tot += len;
                      }
                      a = end;
                  }
                  if (f) fclose(f);
                  fprintf(stderr, "[ramdump] %s: %llu MB at flip %u%c", out, tot >> 20, cellGcm_flip_request_count(), 10);
              }
          } }
#endif

        int fired = 0;
        while ((long long)(now_us - next_tick) >= 0 && fired < 240) {
            cellGcmTickVBlank();
            cellGcmTickFlip();
            /* Present a pending flip BEFORE draining any further. The flip
             * fires at a get==put frame boundary on the guest thread, so the
             * batch held right now is exactly the completed frame; presenting
             * after the drain races the guest's next-frame writes and shows a
             * mixed one. */
            if (rsx_ok && cellGcm_take_flip_pending()) {
                present_guest_frame();
                last_flip = cellGcm_flip_request_count();
            }
            /* Drain the FIFO every tick. This is what writes the RSX sync-fence
             * labels the game's per-frame logic blocks on, so it has to keep
             * advancing at 60 Hz even while present() throttles. */
            if (rsx_ok) timed_drain();
            next_tick += tick_us;
            fired++;
        }
        if (fired >= 240) next_tick = now_us;   /* fell too far behind -- resync */

        /* Drain at the outer cadence too, not only on the 16 ms tick: titles
         * fence every render pass on a label the drain writes, and one wave of
         * those at 16 ms apiece paces the guest into single-figure frame rates.
         * The real RSX writes them in microseconds. */
        if (rsx_ok) {
            if (cellGcm_take_flip_pending()) {
                present_guest_frame();
                last_flip = cellGcm_flip_request_count();
            }
            timed_drain();
            rt_report();

            /* Window events every 4 ms, not every pass: SDL_PollEvent also
             * polls every input device (SDL_SensorUpdate and the rest), and
             * once per pass it was a quarter of this thread's busy time. */
            static unsigned long long s_pump_us;
            const unsigned long long pnow = rt_now_us();
            const int pump = pnow - s_pump_us >= 4000;
            if (pump) s_pump_us = pnow;
            if (pump && rsx_backend_pump() != 0) {
                /* Window closed: quit. Guest threads are still running and
                 * an orderly exit() would wait on them, so leave at once. */
                fprintf(stderr, "[rsx] window closed -- exiting\n");
                fflush(stdout); fflush(stderr);
#ifdef _WIN32
                TerminateProcess(GetCurrentProcess(), 0);
#else
                _exit(0);
#endif
            }
            /* Present on a guest flip. A present on a fixed clock can catch the
             * drain mid-frame and flash a partial one. Before the first flip
             * present freely, so the window is not blank during boot. */
            /* One present per flip. The drain that reached the flip set both
             * the pending flag and the count; presenting here without taking
             * the flag presented the frame again on the next pass -- or, when
             * the drain had meanwhile reached the NEXT flip (a heavy scene:
             * ship landings), presented that one early. The pacing then took
             * the repeat for a frame and held the real one a slot: the
             * landing animation hitched on every landing. */
            unsigned fc = cellGcm_flip_request_count();
            if (fc != last_flip) {
                (void)cellGcm_take_flip_pending();
                present_guest_frame();
                last_flip = fc;
            } else if (rsx_host_ui_modal()) {
                /* The save screen is up and the game may have stopped
                 * flipping (its save thread waits on the player): keep the
                 * last frame and the screen on the window at ~30 Hz. */
                static ULONGLONG s_last_modal = 0;
                if (now - s_last_modal >= 33) { s_last_modal = now; rsx_backend_present(); }
            } else if (fc == 0) {
                /* Pre-flip presents only keep a fresh window from staying
                 * blank. Every 4 ms is free on the null backend but a full
                 * readback on Vulkan, which starves the guest -- twice a
                 * second is plenty. */
                static ULONGLONG s_last_free = 0;
                if (now - s_last_free >= 500) { s_last_free = now; rsx_backend_present(); }
            }
        }
    }
    return 0;
}

/* ---------------------------------------------------------------------------
 * VFS root
 * -----------------------------------------------------------------------*/
/* Derive the directory holding PS3_GAME from the EBOOT path
 * <root>/PS3_GAME/USRDIR/EBOOT.elf -> <root>. $PS3_VFS_ROOT overrides. */
static char s_vfs_root[1024];
extern bool racit_menu_archive_init();

static void derive_vfs_root(const char* eboot)
{
    const char* env = getenv("PS3_VFS_ROOT");
    if (env && *env) { ppu_vfs_root = env; return; }

    strncpy(s_vfs_root, eboot, sizeof s_vfs_root - 1);
    for (char* p = s_vfs_root; *p; p++) if (*p == '\\') *p = '/';
    /* strip EBOOT.elf, USRDIR and PS3_GAME */
    for (int i = 0; i < 3; i++) { char* s = strrchr(s_vfs_root, '/'); if (s) *s = 0; }
    if (!s_vfs_root[0]) strcpy(s_vfs_root, ".");
    ppu_vfs_root = s_vfs_root;
    /* The raw sys_fs layer (Bink's IO thread opens movies through it) reads
     * the root from the environment; without this it resolved
     * ./USRDIR/built/movies/... against the working directory and every
     * movie failed to open. */
#ifdef _WIN32
    _putenv_s("PS3_VFS_ROOT", s_vfs_root);
#else
    setenv("PS3_VFS_ROOT", s_vfs_root, 1);
#endif
}

/* cellFs's own entry points (Rename, Mkdir, Unlink...) translate through a
 * separate table that defaults to ./gamedata/<device>/, so the install step's
 * renames on /dev_hdd0 missed the tree every open uses. Point that table at
 * the same directories ppu_fs/sys_fs resolve to. */
extern "C" void cellfs_add_path_mapping(const char* ps3_prefix, const char* host_path);
extern "C" void vm_diag_refresh(void);   /* ppu_loader.cpp: guest-memory fast path gate */
extern "C" int ppu_inl_clip_hole;          /* ppu_loader.cpp: window ends below the GCM page */
static void map_cellfs_devices(void)
{
#ifdef _WIN32
    char abs[MAX_PATH * 2], m[MAX_PATH * 2 + 2];
    if (_fullpath(abs, ppu_vfs_root, sizeof abs)) {
        snprintf(m, sizeof m, "%s/", abs);
        cellfs_add_path_mapping("/dev_bdvd/", m);
        cellfs_add_path_mapping("/dev_hdd1/", m);
    }
    const char* hdd1 = getenv("PS3_HDD1_ROOT");
    if (hdd1 && *hdd1 && _fullpath(abs, hdd1, sizeof abs)) {
        snprintf(m, sizeof m, "%s/", abs);
        cellfs_add_path_mapping("/dev_hdd1/", m);
    }
    const char* hdd0 = getenv("PS3_HDD0_ROOT");
    if (hdd0 && *hdd0 && _fullpath(abs, hdd0, sizeof abs)) {
        snprintf(m, sizeof m, "%s/", abs);
        cellfs_add_path_mapping("/dev_hdd0/", m);
    }
#else
    char abs[PATH_MAX], m[PATH_MAX + 2];
    if (realpath(ppu_vfs_root, abs)) {
        snprintf(m, sizeof m, "%s/", abs);
        cellfs_add_path_mapping("/dev_bdvd/", m);
        cellfs_add_path_mapping("/dev_hdd1/", m);
    }
    const char* hdd0 = getenv("PS3_HDD0_ROOT");
    if (hdd0 && *hdd0 && realpath(hdd0, abs)) {
        snprintf(m, sizeof m, "%s/", abs);
        cellfs_add_path_mapping("/dev_hdd0/", m);
    }
#endif
}

/* ---------------------------------------------------------------------------
 * Guest VM
 * -----------------------------------------------------------------------*/
#ifdef _WIN32
/* Demand-paging for the flat VM: reserve the whole 4 GB guest space, which
 * costs no commit, and commit each 64 KB page on first access. Every 32-bit
 * guest offset is then valid, so a garbage guest pointer reads zero instead of
 * killing the process. Faults outside the arena fall through untouched. */
static LONG WINAPI vm_commit_veh(EXCEPTION_POINTERS* ep)
{
    if (ep->ExceptionRecord->ExceptionCode == EXCEPTION_ACCESS_VIOLATION) {
        ULONG_PTR fault = ep->ExceptionRecord->ExceptionInformation[1];
        uintptr_t base  = (uintptr_t)vm_base;
        if (vm_base && fault >= base && fault < base + VM_SIZE) {
            void* page = (void*)(fault & ~(uintptr_t)0xFFFF);
            if (VirtualAlloc(page, 0x10000, MEM_COMMIT, PAGE_READWRITE))
                return EXCEPTION_CONTINUE_EXECUTION;
        }
    }
    return EXCEPTION_CONTINUE_SEARCH;
}
#endif

static bool alloc_guest_vm(void)
{
#ifdef _WIN32
    AddVectoredExceptionHandler(1, vm_commit_veh);
    /* MEM_WRITE_WATCH: the OS records which guest pages get written, by
     * anyone, so the renderer's caches recheck only those
     * (libs/video/rsx_writewatch.h). RACACIT_NO_WRITE_WATCH=1 drops it. */
    const char* nww = getenv("RACACIT_NO_WRITE_WATCH");
    const DWORD ww = (nww && *nww && *nww != '0') ? 0u : (DWORD)MEM_WRITE_WATCH;
    /* 4 GB-aligned like the POSIX arena: probe an 8 GB window, release it and
     * reserve its aligned half (retry if another thread took the hole). */
    for (int tries = 0; tries < 16 && !vm_base; tries++) {
        uint8_t* probe = (uint8_t*)VirtualAlloc(NULL, 0x200000000ull, MEM_RESERVE, PAGE_NOACCESS);
        if (!probe) break;
        uintptr_t a = ((uintptr_t)probe + 0xFFFFFFFFull) & ~(uintptr_t)0xFFFFFFFFull;
        VirtualFree(probe, 0, MEM_RELEASE);
        vm_base = (uint8_t*)VirtualAlloc((void*)a, VM_SIZE, MEM_RESERVE | ww, PAGE_READWRITE);
    }
    if (!vm_base)
        vm_base = (uint8_t*)VirtualAlloc(NULL, VM_SIZE, MEM_RESERVE | ww, PAGE_READWRITE);
    ppu_vm_size = 0;              /* the whole space is backed; no OOB guard needed */
#else
    /* No vectored exception handlers off Windows, so the arena is committed up
     * front and lazily backed by the OS: only pages the title touches cost
     * anything. It stops below the 0xE0000000 mark, so ppu_vm_size arms the
     * loader's bounds check for what is left. */
    /* PORT FIX (racacit): the template used calloc(0xE0000000) here. calloc's
     * pointer is never page-aligned (glibc returns mmap+0x10), so every
     * vm_commit()/vm_protect() -> mprotect() failed with EINVAL: each
     * sys_memory_allocate leaked 1 MB of bump space and returned ENOMEM, and
     * the title's malloc starved. Reserve the full 4 GB guest space instead,
     * 4 GB-aligned (as runtime/memory/vm.h documents), lazily backed. */
    {
        const uint64_t span = 0x100000000ull;
        /* Reserve the 8 GB window inaccessible, then open up only the aligned
         * 4 GB: a writable 8 GB request is refused outright by heuristic
         * overcommit on phones with less RAM than that. */
        void* p = mmap(nullptr, (size_t)(span * 2), PROT_NONE,
                       MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
        if (p == MAP_FAILED) return false;
        uintptr_t lo = (uintptr_t)p, a = (lo + (span - 1)) & ~(uintptr_t)(span - 1);
        if (a > lo) munmap(p, a - lo);
        uintptr_t tail = a + span, end = lo + (uintptr_t)(span * 2);
        if (end > tail) munmap((void*)tail, end - tail);
        if (mprotect((void*)a, (size_t)span, PROT_READ | PROT_WRITE) != 0) return false;
        vm_base = (uint8_t*)a;
        ppu_vm_size = 0;   /* full 32-bit space backed -> OOB guard unnecessary */
    }
#endif
    return vm_base != nullptr;
}

/* ---------------------------------------------------------------------------
 * Main
 * -----------------------------------------------------------------------*/
static void harness_guest_caller(uint32_t opd, uint64_t a0, uint64_t a1,
                                 uint64_t a2, uint64_t a3, uint64_t a4,
                                 uint64_t a5, uint64_t a6, uint64_t a7)
{
    ppu_guest_call(opd, a0, a1, a2, a3, a4, a5, a6, a7);
}

extern "C" void cellGame_init_from_paramsfo(const char* sfo_path);
extern "C" void racacit_register_elf_overlays(void);   /* game/spu/spu_jobs.c */
extern "C" void racacit_register_extra_overlays(void); /* game/spu/spu_extra.c */

#ifdef __ANDROID__
/* ---------------------------------------------------------------------------
 * Android launch
 *
 * Everything lives in the app's external files directory, which needs no
 * storage permission and is reachable over adb / USB file transfer:
 *
 *   <files>/PS3_GAME/PARAM.SFO
 *   <files>/PS3_GAME/USRDIR/EBOOT.elf      (the decrypted EBOOT.ELF)
 *   <files>/PS3_GAME/USRDIR/packed/...     (and built/..., as on the disc)
 *   <files>/dev_hdd0/                      (created by the game's install step)
 *   <files>/env.txt                        optional KEY=VALUE overrides
 *   <files>/log.txt                        written only if <files>/debug exists
 * -----------------------------------------------------------------------*/
static char s_android_eboot[1024];
static FILE* s_android_log;   /* log.txt while the launcher's Logging is on, else null (logcat) */
static pthread_t s_android_log_thread;
static bool s_android_log_started;

/* A line from the log pump itself: written past the pipe it drains, which a
 * write to stderr could fill and block on. */
static void android_log_direct(const char* line)
{
    if (s_android_log) { fputs(line, s_android_log); fputc('\n', s_android_log); }
    else __android_log_write(ANDROID_LOG_INFO, "racacit", line);
}

/* Host threads named after what the runtime says they are, so a profiler,
 * `top -H` and the companion's status page show "PPU main thread" or
 * "spu w2.0" rather than the process name sixty times over. Fed the log
 * lines that announce them: "[hosttid] <tid> <role>" and the pair
 * "[SYS] sys_ppu_thread_create tid=<n> name="<guest name>"" ...
 * "[THREAD <n>] host thread started, ... hosttid=<tid>". */
extern "C" void racacit_perf_thread(unsigned long tid, const char* name);   /* android_companion.cpp */
static cpu_set_t s_cpus_others, s_cpus_fast, s_cpus_little;
static int s_cpus_others_on, s_top_cpu, s_swap_ok;
/* v113-android.49 (RACACIT_COLL_LITTLE=1): two small cores kept for the collision lanes (s_coll_cpu, the
 * two highest-numbered), and the small cores left for everything else placed there (s_cpus_spare: the
 * file streaming threads, helpers, the audio threads, the main thread's core-swap waits). Without it
 * s_cpus_spare is all the small cores. */
static cpu_set_t s_cpus_spare;
static int s_coll_little_on, s_coll_cpu[2] = { -1, -1 };
/* v121-android.63b (RACACIT_SPU_SPILL=1, off by default): the SPU lanes' home set -- the mid cores,
 * or with the switch the mid cores and the small ones (s_cpus_spare). In the owner's 2026-11-17 Agorian
 * Arena fights the five lanes waited 1.2-2.1 s a second for one of the four mid cores (one ran ~50 ms
 * a second and waited ~500) while the three small cores held only the file threads; CollNarrow, the
 * frame's critical job, went from ~11 ms to 21-35 ms a frame and the game to 22-43 fps. A lane running
 * a collision job is held to the mid cores for it (racacit_coll_place): .49 measured that job on a
 * small core as a loss (30.4 -> 17.9 fps). The other jobs may run on a small core at ~0.4x instead of
 * waiting for a mid one. s_cpus_lane = s_cpus_others when off. */
static cpu_set_t s_cpus_lane;
static int s_spill_on;
/* The threads "[sched]" reports (racacit_sched_thread): the main thread, the SPU lanes, the render
 * threads and the first SPURS kernel (sound). Filled as they are named, before it reads them. */
static struct { volatile long tid; char name[16]; } s_sched_th[16];
static volatile int s_sched_n;
extern "C" void racacit_profiler_start(const char* files_dir);             /* android_profiler.cpp */
static void android_name_thread(const char* line)
{
    static char s_guest[256][16];   /* guest thread id -> its name */
    unsigned long tid = 0; char name[16] = "";
    const char* p;
    if (!strncmp(line, "[hosttid] ", 10)) {
        char* end; tid = strtoul(line + 10, &end, 10);
        while (*end == ' ') end++;
        unsigned w, s;
        if (sscanf(end, "spurs lane wid=%u spu=%u", &w, &s) == 2) snprintf(name, sizeof name, "spu w%u.%u", w, s);
        else snprintf(name, sizeof name, "%s", end);
    } else if (!strncmp(line, "[SYS] sys_ppu_thread_create tid=", 32)) {
        unsigned g = (unsigned)strtoul(line + 32, NULL, 10);
        if ((p = strstr(line, "name=\"")) && g < 256) {
            p += 6; size_t i = 0;
            while (p[i] && p[i] != '"' && i < 15) { s_guest[g][i] = p[i]; i++; }
            s_guest[g][i] = 0;
        }
        return;
    } else if (!strncmp(line, "[THREAD ", 8) && (p = strstr(line, "hosttid="))) {
        unsigned g = (unsigned)strtoul(line + 8, NULL, 10);
        tid = strtoul(p + 8, NULL, 10);
        if (g < 256 && s_guest[g][0]) snprintf(name, sizeof name, "%s", s_guest[g]);
        else snprintf(name, sizeof name, "ppu thread %u", g);
    }
    if (!tid || !name[0]) return;
    char path[64]; snprintf(path, sizeof path, "/proc/self/task/%lu/comm", tid);
    if (FILE* f = fopen(path, "w")) { fputs(name, f); fclose(f); }
    racacit_perf_thread(tid, name);
    {   /* for "[sched]": the main thread, the lanes, the render threads, the sound kernel */
        static int s_kern_seen;
        const int want = !strncmp(name, "PPU main", 8) || !strncmp(name, "spu w", 5) || !strncmp(name, "rsx", 3) ||
                         (!strcmp(name, "spurs kernel") && !s_kern_seen++);
        const int k = s_sched_n;
        if (want && k < 16) {
            snprintf(s_sched_th[k].name, sizeof s_sched_th[k].name, "%s", name);
            __atomic_store_n(&s_sched_th[k].tid, (long)tid, __ATOMIC_RELEASE);
            __atomic_store_n(&s_sched_n, k + 1, __ATOMIC_RELEASE);
        }
    }
    /* The first SPURS kernel is the sound synthesizer's. On the shared cores it saturates at loud
     * moments and the sound stretches, with no dropouts; on the top core it keeps up, but where
     * the main thread is the limit that costs far more (the moon: 37 fps shared, 33 and 27 with
     * modes 1 and 3; Great Clock D gains 1-2). */
    static int s_kernels;
    const int sound = !strcmp(name, "spurs kernel") && s_kernels++ == 0;
    /* RACACIT_SOUND_CPU: 0 the shared cores, 1 those and the top core, 2 the shared cores at a
     * raised priority, 3 the top core alone. */
    static const int s_sound_cpu = [] { const char* e = getenv("RACACIT_SOUND_CPU"); return e ? atoi(e) : 0; }();
    /* The game's file-streaming threads (FIOS: "fios mediathread" x7, "fios scheduler" x3) go to
     * the small cores, which no other game thread uses. They sleep in timed lwcond waits and wake
     * at every timeout: in the .35 [sched] they ran 33 ms a second between them and sat runnable
     * 1.1-1.6 s a second on the four shared cores (oversubscribed about 2x in play), every wake a
     * chance to preempt the render thread or an SPU lane (nice -5, above both). On the idle small
     * cores they run as soon as they wake. RACACIT_FIOS_LITTLE=0: the shared cores, as before.
     * v113-android.37. */
    static const int s_fios_little = [] { const char* e = getenv("RACACIT_FIOS_LITTLE"); return !(e && *e == '0'); }();
    if (s_fios_little && s_cpus_others_on && CPU_COUNT(&s_cpus_spare) > 0 && !strncmp(name, "fios", 4)) {
        sched_setaffinity((pid_t)tid, sizeof s_cpus_spare, &s_cpus_spare);
    } else if (s_cpus_others_on && sound && s_sound_cpu == 3 && s_top_cpu >= 0) {
        cpu_set_t top; CPU_ZERO(&top); CPU_SET(s_top_cpu, &top);
        sched_setaffinity((pid_t)tid, sizeof top, &top);
    } else if (s_cpus_others_on && (!sound || s_sound_cpu == 0 || s_sound_cpu == 2) && strncmp(name, "PPU main", 8)) {
        const cpu_set_t* set = !strncmp(name, "spu w", 5) ? &s_cpus_lane : &s_cpus_others;
        sched_setaffinity((pid_t)tid, sizeof *set, set);
    }
    if (sound && s_sound_cpu == 2) setpriority(PRIO_PROCESS, (id_t)tid, -10);
    /* The timer slack each busy thread really has (android_setup sets 1 ns
     * before they start), so a log shows that it took. */
    static int s_slack_lines;
    if (s_slack_lines < 12 && (!strncmp(name, "PPU main", 8) || !strncmp(name, "rsx", 3) ||
                               !strncmp(name, "spu w", 5) || !strncmp(name, "spurs", 5))) {
        snprintf(path, sizeof path, "/proc/self/task/%lu/timerslack_ns", tid);
        unsigned long long ns = 0;
        if (FILE* f = fopen(path, "r")) { if (fscanf(f, "%llu", &ns) != 1) ns = 0; fclose(f); }
        char msg[96];
        snprintf(msg, sizeof msg, "[android] %s: timer slack %llu ns", name, ns);
        android_log_direct(msg);
        s_slack_lines++;
    }
}

static bool android_log_pump_start(int fd, FILE* file)
{
    /* stdout/stderr -> logcat (tag "racacit"), or into log.txt when one was
     * asked for. Line-buffered through a pipe drained by one thread, a block
     * per read (a byte per read was a syscall per character, and a writer
     * blocks once the pipe is full). Both ways the lines that announce host
     * threads name them: with log.txt written directly, as before 0.14, no
     * thread was named and the performance hint session never started. */
    s_android_log = file;
    const int err = pthread_create(&s_android_log_thread, NULL, [](void* arg) -> void* {
        const int read_fd = static_cast<int>(reinterpret_cast<intptr_t>(arg));
        char line[1024]; size_t n = 0; char chunk[4096]; ssize_t got;
        for (;;) {
            got = read(read_fd, chunk, sizeof chunk);
            if (got < 0 && errno == EINTR) continue;
            if (got <= 0) break;
            for (ssize_t i = 0; i < got; i++) {
                const char c = chunk[i];
                if (c != '\n' && n < sizeof line - 1) { line[n++] = c; continue; }
                line[n] = 0;
                /* Another thread's print can run into the line: a thread missed here is left
                 * unnamed, and free to take the core kept for the main thread. */
                if (const char* ann = strstr(line, "[hosttid] ")) android_name_thread(ann);
                else if (const char* th = strstr(line, "[THREAD ")) android_name_thread(th);
                else if (line[0] == '[') android_name_thread(line);
                if (s_android_log || !strstr(line, "sys_memory] allocate")) android_log_direct(line);
                n = 0;
                if (c != '\n') line[n++] = c;
            }
        }
        if (n) {
            line[n] = 0;
            android_log_direct(line);
        }
        close(read_fd);
        return NULL;
    }, reinterpret_cast<void*>(static_cast<intptr_t>(fd)));
    s_android_log_started = err == 0;
    if (!s_android_log_started) s_android_log = nullptr;
    return s_android_log_started;
}

static void android_drain_startup_log()
{
    fflush(stdout);
    fflush(stderr);
    if (!s_android_log_started) return;

    /* Stop both writers before joining. Other game threads can still write
     * during startup, so keep their descriptors valid until the process exits. */
    const int sink = open("/dev/null", O_WRONLY | O_CLOEXEC);
    if (sink < 0) return;
    const bool stdout_closed = dup2(sink, STDOUT_FILENO) >= 0;
    const bool stderr_closed = dup2(sink, STDERR_FILENO) >= 0;
    close(sink);
    if (!stdout_closed || !stderr_closed) return;
    pthread_join(s_android_log_thread, nullptr);
    s_android_log_started = false;
    if (s_android_log) fflush(s_android_log);
}

#ifdef RACIT_PGO_GEN
/* Profile-guided build, first stage: the counters to a file every 20 s, since Android
 * kills the process rather than letting it exit. */
extern "C" int __llvm_profile_write_file(void);
extern "C" void __llvm_profile_set_filename(const char*);
extern "C" void __llvm_profile_reset_counters(void);
static void* android_pgo_thread(void*)
{
    for (;;) {
        sleep(20);
        const int r = __llvm_profile_write_file();
        __llvm_profile_reset_counters();
        char msg[64]; snprintf(msg, sizeof msg, "[pgo] counters written: %d", r);
        android_log_direct(msg);
    }
    return NULL;
}
static void android_pgo_start(const char* base)
{
    static char path[600];
    snprintf(path, sizeof path, "%s/pgo_%%m.profraw", base);
    __llvm_profile_set_filename(path);
    pthread_t t;
    pthread_create(&t, NULL, android_pgo_thread, NULL);
}
#endif

/* The game on the device's fast cores. Left to itself the scheduler gave its threads the small
 * ones for about a tenth of their time: a third of the speed, and the main thread waits on the
 * slowest job of the frame. Fast = at least half the top core's capacity. RACACIT_CPUS=0: all
 * cores; 2: the top core only for the main thread as well, when four others remain.
 * With under four fast cores (2 big + 6 small is common in midrange phones) the game's three to
 * four cores of work does not fit on them: only the main thread stays there, the rest may use
 * every core. RACACIT_TEST_FAST=<n> counts only the n strongest as fast, to test that. */
static void android_cpu_policy(void)
{
    const char* e = getenv("RACACIT_CPUS");
    const int mode = e && *e ? atoi(e) : 2;
    if (mode <= 0) return;
    long cap[CPU_SETSIZE]; long top = 0; int top_cpu = -1, n = 0;
    for (int c = 0; c < CPU_SETSIZE && c < 32; c++) {
        char path[96]; long v = 0;
        snprintf(path, sizeof path, "/sys/devices/system/cpu/cpu%d/cpu_capacity", c);
        FILE* f = fopen(path, "r");
        if (!f) break;
        if (fscanf(f, "%ld", &v) != 1) v = 0;
        fclose(f);
        cap[c] = v; n = c + 1;
        if (v >= top) { top = v; top_cpu = c; }
    }
    if (!top) return;
    if (const char* tf = getenv("RACACIT_TEST_FAST")) {
        long keep = atol(tf), seen = 0;
        for (int c = n - 1; c >= 0; c--) { if (seen >= keep) cap[c] = 0; else if (cap[c]) seen++; }
    }
    cpu_set_t fast; CPU_ZERO(&fast); CPU_ZERO(&s_cpus_others); CPU_ZERO(&s_cpus_little);
    int nfast = 0, nothers = 0, nlittle = 0;
    for (int c = 0; c < n; c++) {
        if (cap[c] * 2 < top) { if (cap[c]) { CPU_SET(c, &s_cpus_little); nlittle++; } continue; }
        CPU_SET(c, &fast); nfast++;
        if (c != top_cpu) { CPU_SET(c, &s_cpus_others); nothers++; }
    }
    if (nfast >= 1 && nfast < n) sched_setaffinity(0, sizeof fast, &fast);
    s_cpus_fast = fast;
    s_cpus_others_on = mode == 2 && nothers >= 4;
    s_top_cpu = top_cpu;
    { extern int g_racacit_main_core_own; g_racacit_main_core_own = s_cpus_others_on; }
    /* The core swap (below) needs the top core kept for the main thread, and small cores that
     * no game thread uses for the main thread to wait on meanwhile. */
    s_swap_ok = s_cpus_others_on && nlittle > 0 && top_cpu >= 0;
    const int spread = nfast < 4 && nfast < n;
    if (spread) {
        CPU_ZERO(&s_cpus_others);
        for (int c = 0; c < n; c++) CPU_SET(c, &s_cpus_others);
        s_cpus_others_on = 1;
    }
    char msg[96];
    snprintf(msg, sizeof msg, "[android] cpus: %d fast of %d, mode %d, %s", nfast, n, mode,
             spread ? "main thread on the fast ones, the rest anywhere"
                    : s_cpus_others_on ? "top core kept for the main thread" : "top core shared");
    android_log_direct(msg);
    /* RACACIT_COLL_LITTLE=1 (v113-android.49, a test): the lanes running the two collision jobs move to
     * two small cores of their own for the job (racacit_coll_place, from spu_drain.c). In a fight the
     * four mid cores are full: the .48 A/B had the collision lanes running ~300 ms a second and waiting
     * 273-412 more for a core, the main thread waiting on their records, while the three small cores
     * idled. A small core is ~2.5x slower; nothing else runs there. Needs three small cores or more
     * (one left for the rest) and the top core kept for the main thread. */
    s_cpus_spare = s_cpus_little;
    { const char* cl = getenv("RACACIT_COLL_LITTLE");
      if (cl && *cl == '1' && s_cpus_others_on && !spread && nlittle >= 3) {
          int k = 0;
          for (int c = n - 1; c >= 0 && k < 2; c--) if (CPU_ISSET(c, &s_cpus_little)) { s_coll_cpu[k++] = c; CPU_CLR(c, &s_cpus_spare); }
          s_coll_little_on = k == 2;
          snprintf(msg, sizeof msg, "[android] collision lanes: small cores %d and %d (RACACIT_COLL_LITTLE)", s_coll_cpu[0], s_coll_cpu[1]);
          android_log_direct(msg);
      } }
    s_cpus_lane = s_cpus_others;
    { const char* sp = getenv("RACACIT_SPU_SPILL");
      if (sp && *sp == '1' && s_cpus_others_on && !spread && !s_coll_little_on && CPU_COUNT(&s_cpus_spare) > 0) {
          CPU_OR(&s_cpus_lane, &s_cpus_others, &s_cpus_spare);
          s_spill_on = 1;
          android_log_direct("[android] SPU lanes: mid and small cores; collision jobs on the mid cores (RACACIT_SPU_SPILL)");
      } }
}

/* The collision lane's own small core while it runs CollNarrow (43593402) or CollBroad (4248d602), the
 * mid cores again after (on = 0). Called by the lane itself. */
extern "C" void racacit_coll_place(unsigned guid, int on)
{
    if (!s_coll_little_on) {
        if (s_spill_on) { const cpu_set_t* set = on ? &s_cpus_others : &s_cpus_lane; sched_setaffinity(0, sizeof *set, set); }
        return;
    }
    if (!on) { sched_setaffinity(0, sizeof s_cpus_others, &s_cpus_others); return; }
    cpu_set_t one; CPU_ZERO(&one); CPU_SET(guid == 0x4248d602u ? s_coll_cpu[1] : s_coll_cpu[0], &one);
    sched_setaffinity(0, sizeof one, &one);
}
extern "C" int racacit_coll_little(void) { return s_coll_little_on; }

/* ---------------------------------------------------------------------------
 * Core swap (RACACIT_CORE_SWAP=1; RACACIT_AB=<s> alternates it off and on)
 *
 * In a heavy scene the main thread spends about two thirds of each frame waiting on SPU jobs
 * (racacit_wait_poll), and those jobs run one after another on the mid cores while the top core,
 * the fastest by far, holds the main thread's wait. With the swap, a wait that has lasted
 * RACACIT_SWAP_AFTER_US (150) moves the main thread to a small core to go on waiting there, and
 * the lane most likely running the job (awake, and woken last: spu_lanes.h) to the top core. When
 * the wait ends both go back, the lane first. A lane that goes to sleep meanwhile hands the top
 * core to the next one woken, at most four times a wait. Only the main thread swaps, and only
 * where the top core is kept for it and small cores exist (s_swap_ok).
 * ---------------------------------------------------------------------------*/
#include "spu_lanes.h"
#include "../tools/ps3recomp/runtime/platform/racacit_diag.h"
extern "C" { volatile int g_racacit_core_swap; long g_racacit_main_tid; }
static unsigned long long s_swap_after_tb = 150ull * 80u;   /* timebase ticks (79.8 MHz) */
static unsigned long s_swap_n[4];   /* swapped waits, picks, lanes moved back (relaxed atomics: the A/B thread reads them) */
#define SWAP_COUNT(k) __atomic_fetch_add(&s_swap_n[k], 1ul, __ATOMIC_RELAXED)
static thread_local int t_swap_lane = -1, t_swap_main, t_swap_picks, t_is_main = -1;
static void swap_aff(long tid, const cpu_set_t* set) { sched_setaffinity((pid_t)tid, sizeof *set, set); }

/* Collision jobs on the main thread's core (RACACIT_COLOC, v113-android.17 and .23-.26) were taken
 * out in .36: measured worse both times -- .17 Space Sector 1 at 11-12 fps (the co-located lanes
 * got ~5% of the top core), .26 the title screen and menus 60 -> 43-45 fps. The collision fibers
 * (spu_drain.c, RACACIT_COLL_FIBER=1) are the form of that idea still being worked on. */
extern "C" { extern volatile int g_racacit_fibers_live; extern volatile unsigned long g_racacit_fiber_n[4]; }   /* spu_drain.c */
extern "C" { extern volatile unsigned long g_racacit_fiber_k[3][7]; }   /* spu_drain.c: the same by job (v113-android.44) */
extern "C" { void racacit_ib2_report(char* out, int cap); extern volatile unsigned long g_racacit_ib2[20]; }

/* The top core to the FIFO walker while the main thread waits for it (sys_timer.c, RACACIT_LEND_TOP):
 * the game ends each frame polling for the RSX to reach a marker, and the walker that gets there
 * runs on a middle core while the top one spins. One affinity call each way, about twice a frame. */
extern "C" { volatile int g_racacit_top_lent; volatile long g_racacit_walker_tid; volatile unsigned long g_racacit_lend_n; }
extern "C" void racacit_lend_top_begin(void)
{
    const long w = g_racacit_walker_tid;
    if (!s_swap_ok || !w || s_top_cpu < 0) return;
    cpu_set_t top; CPU_ZERO(&top); CPU_SET(s_top_cpu, &top);
    if (sched_setaffinity((pid_t)w, sizeof top, &top) != 0) return;
    g_racacit_top_lent = 1;
    if (__atomic_fetch_add(&g_racacit_lend_n, 1ul, __ATOMIC_RELAXED) == 0)
        fprintf(stderr, "[android] top core lent to the FIFO walker while the main thread waits for it (RACACIT_LEND_TOP)%c", 10);
}
extern "C" void racacit_lend_top_end(void)
{
    if (!g_racacit_top_lent) return;
    const long w = g_racacit_walker_tid;
    if (w) sched_setaffinity((pid_t)w, sizeof s_cpus_others, &s_cpus_others);
    g_racacit_top_lent = 0;
}

/* v121-android.65 (RACACIT_SWAP_FIBERS, on unless 0): the swap also while collision fibers are live.
 * It was off then because the main thread runs the fibers. The owner's 2026-11-17 Agorian Arena fights
 * (.63/.64 profile_raw) say what that cost: the main thread's samples were 33% racacit_fiber_ready polls
 * and 20% usleep spins and almost no fiber code, while it waited 300-525 ms a second on SPU jobs (each
 * 3-5x its calm length: 4244aa02 0.29 -> 1.69 ms) and the lanes queued 0.7-2.1 s a second for the mid
 * cores -- the top core spinning, the jobs it waits for crowded. With fibers live the collision lane is
 * parked on the main thread's records, so it is never the pick; the lane whose job started last is. */
static int swap_with_fibers(void)
{
    static int on = -1;
    if (on < 0) { const char* e = getenv("RACACIT_SWAP_FIBERS"); on = !(e && *e == '0'); }
    return on;
}
extern "C" void racacit_swap_poll(unsigned long long waited_tb)
{
    if (!g_racacit_core_swap || !s_swap_ok || waited_tb < s_swap_after_tb) return;
    const int fibers = __atomic_load_n(&g_racacit_fibers_live, __ATOMIC_ACQUIRE) > 0;
    if (fibers && !swap_with_fibers()) return;   /* the main thread runs the collision fibers */
    if (t_is_main < 0) t_is_main = g_racacit_main_tid && (long)gettid() == g_racacit_main_tid;
    if (!t_is_main) return;
    if (t_swap_lane >= 0 && !__atomic_load_n(&g_racacit_lanes[t_swap_lane].asleep, __ATOMIC_ACQUIRE)) return;
    if (t_swap_picks >= 4) return;
    const int n = g_racacit_nlanes < RACACIT_LANES ? g_racacit_nlanes : RACACIT_LANES;
    /* A lane awake in a collision job the main thread hands records to (spu_lanes.h): most of the
     * main thread's waits are on its records. Else the lane whose job started last (spu_drain.c
     * spu_job_time_begin): the main thread submits a job and then waits on it. Else the awake
     * lane woken last. */
    int best = -1; unsigned long long bw = 0;
    /* (RACACIT_COLL_LITTLE: the collision lanes keep their own small cores; never picked.) */
    if (!s_coll_little_on && !fibers)
    for (int i = 0; i < n; i++) {
        const racacit_lane& L = g_racacit_lanes[i];
        if (L.tid && L.critical && !__atomic_load_n(&L.asleep, __ATOMIC_ACQUIRE)) { best = i; break; }
    }
    if (best < 0)
        for (int i = 0; i < n; i++) {
            const racacit_lane& L = g_racacit_lanes[i];
            const unsigned long long j = L.job;
            if ((s_coll_little_on || fibers) && L.critical) continue;
            if (L.tid && j && (best < 0 || j > bw)) { best = i; bw = j; }
        }
    if (best < 0)
        for (int i = 0; i < n; i++) {
            const racacit_lane& L = g_racacit_lanes[i];
            if ((s_coll_little_on || fibers) && L.critical) continue;
            if (!L.tid || __atomic_load_n(&L.asleep, __ATOMIC_ACQUIRE)) continue;
            if (best < 0 || L.woke > bw) { best = i; bw = L.woke; }
        }
    if (best == t_swap_lane) return;
    if (t_swap_lane >= 0) {
        swap_aff(g_racacit_lanes[t_swap_lane].tid, &s_cpus_lane);
        SWAP_COUNT(2); t_swap_lane = -1;
    }
    if (best < 0) return;
    if (!t_swap_main) {
        swap_aff(0, &s_cpus_spare); t_swap_main = 1;   /* off the top core first */
        SWAP_COUNT(0);
    }
    cpu_set_t top; CPU_ZERO(&top); CPU_SET(s_top_cpu, &top);
    swap_aff(g_racacit_lanes[best].tid, &top);
    t_swap_lane = best; t_swap_picks++; SWAP_COUNT(1);
}

/* racacit_pin_main (v113-android.14, removed in .15): the main thread pinned to the top core alone,
 * so a swap's return took one affinity call instead of two. Threads inherit their creator's
 * affinity, and the ones the main thread creates that are not placed by name (the cellAudio mixer
 * among them) were held to the top core beside it: the game's sound ran at 62% speed. */

extern "C" void racacit_swap_end(void)
{
    t_swap_picks = 0;
    if (t_swap_lane >= 0) {
        swap_aff(g_racacit_lanes[t_swap_lane].tid, &s_cpus_lane);
        t_swap_lane = -1;
    }
    if (t_swap_main) {
        /* Straight to the top core (a move into a wider set may land on any core of it), then
         * the set the main thread had. (Its home stays the fast set: see racacit_pin_main.) */
        cpu_set_t top; CPU_ZERO(&top); CPU_SET(s_top_cpu, &top);
        swap_aff(0, &top);
        swap_aff(0, &s_cpus_fast);
        t_swap_main = 0;
    }
}

/* For a helper thread whose timing does not matter (button_prompts.cpp's memory scans, the
 * A/B clock): the small cores, which no game thread uses, at a low priority, and a name for the
 * profiler. Unplaced, a helper inherits the fast cores from the thread that made it: the prompt
 * scans (100-220 ms of one core, five times in the first minute of each level; owner's .7 log,
 * Zolar) ran beside the SPU lanes, which the main thread waits on. */
/* v113-android.48: a light thread that wakes often, at its own (raised) priority, to the small cores:
 * the cellAudio mixer (~1,000 wakes a second at nice -16) and AAudio's callback thread. Made by game
 * threads, they inherited the fast set -- the top core included, which is the main thread's -- and
 * each wake took a core from the main thread or an SPU lane. 0 where there are no small cores kept
 * apart (then nothing changes). */
extern "C" int racacit_little_thread(void)
{
    if (!s_cpus_others_on || CPU_COUNT(&s_cpus_spare) == 0) return 0;
    return sched_setaffinity(0, sizeof s_cpus_spare, &s_cpus_spare) == 0;
}

extern "C" void racacit_background_thread(const char* name)
{
    if (name) prctl(PR_SET_NAME, name, 0, 0, 0);
    if (CPU_COUNT(&s_cpus_spare) > 0) sched_setaffinity(0, sizeof s_cpus_spare, &s_cpus_spare);
    setpriority(PRIO_PROCESS, (id_t)gettid(), 10);
}

/* A helper whose work the frame does not wait on directly but that should not lag either: the
 * Vulkan backend's pipeline builds (rsx_vulkan_backend.c, bursts of 30-370 ms each on a first
 * visit; 256 in the first minutes of the owner's .7 Zolar run). Made by the render thread, they
 * inherited its mid cores and took them from the SPU lanes; now the small cores too, never the top
 * one. background = 1: as racacit_background_thread. */
extern "C" void racacit_place_helper(const char* name, int background)
{
    if (background) { racacit_background_thread(name); return; }
    if (name) prctl(PR_SET_NAME, name, 0, 0, 0);
    if (!s_cpus_others_on || CPU_COUNT(&s_cpus_spare) == 0) return;
    cpu_set_t set; CPU_OR(&set, &s_cpus_others, &s_cpus_spare);
    sched_setaffinity(0, sizeof set, &set);
}

/* RACACIT_AB=<seconds>: the core swap off and on in turn, that long each, and the frame rate and
 * the main thread's SPU-job waits of each phase in the log ("[ab]"), with running totals; the
 * companion's Status page shows the totals (g_racacit_ab). A phase under 5 fps (a load, a menu,
 * a movie) is left out of the totals. */
extern "C" { volatile long g_racacit_ab[6]; }   /* on, current mode, phases off, fps off x100, phases on, fps on x100 */
extern "C" unsigned cellGcm_flip_request_count(void);
extern "C" unsigned long long g_racacit_main_wait_ticks;   /* stubs.cpp racacit_wait_poll */
static void* racacit_ab_thread(void* arg)
{
    const long secs = (long)(intptr_t)arg;
    racacit_background_thread("ab clock");
    double fr[2] = {0, 0}, sec[2] = {0, 0}, wt[2] = {0, 0}; long ph[2] = {0, 0};
    int mode = 0; g_racacit_core_swap = 0;
    g_racacit_ab[0] = 1;
    for (;;) {
        struct timespec a; clock_gettime(CLOCK_MONOTONIC, &a);
        const unsigned f0 = cellGcm_flip_request_count();
        const unsigned long long w0 = __atomic_load_n(&g_racacit_main_wait_ticks, __ATOMIC_RELAXED);
        const unsigned long n0[2] = { __atomic_load_n(&s_swap_n[0], __ATOMIC_RELAXED), __atomic_load_n(&s_swap_n[1], __ATOMIC_RELAXED) };
        sleep((unsigned)secs);
        struct timespec b; clock_gettime(CLOCK_MONOTONIC, &b);
        const double dt = (double)(b.tv_sec - a.tv_sec) + (double)(b.tv_nsec - a.tv_nsec) / 1e9;
        const unsigned df = cellGcm_flip_request_count() - f0;
        const double wait_ms = (double)(__atomic_load_n(&g_racacit_main_wait_ticks, __ATOMIC_RELAXED) - w0) / 79800.0;
        const double fps = dt > 0 ? df / dt : 0;
        if (fps >= 5) { fr[mode] += df; sec[mode] += dt; wt[mode] += wait_ms; ph[mode]++; }
        char msg[320];
        snprintf(msg, sizeof msg, "[ab] core swap %s: %.1f fps, %.1f ms a frame waiting on SPU jobs, %lu waits swapped, "
                 "%lu lane picks%s | totals: off %.2f fps (%ld phases, %.1f ms wait), on %.2f fps (%ld phases, %.1f ms wait)",
                 mode ? "on " : "off", fps, df ? wait_ms / df : 0.0,
                 __atomic_load_n(&s_swap_n[0], __ATOMIC_RELAXED) - n0[0], __atomic_load_n(&s_swap_n[1], __ATOMIC_RELAXED) - n0[1],
                 fps >= 5 ? "" : " (not counted)",
                 sec[0] > 0 ? fr[0] / sec[0] : 0.0, ph[0], fr[0] > 0 ? wt[0] / fr[0] : 0.0,
                 sec[1] > 0 ? fr[1] / sec[1] : 0.0, ph[1], fr[1] > 0 ? wt[1] / fr[1] : 0.0);
        android_log_direct(msg);
        g_racacit_ab[2] = ph[0]; g_racacit_ab[3] = sec[0] > 0 ? (long)(fr[0] / sec[0] * 100) : 0;
        g_racacit_ab[4] = ph[1]; g_racacit_ab[5] = sec[1] > 0 ? (long)(fr[1] / sec[1] * 100) : 0;
        mode ^= 1; g_racacit_core_swap = mode; g_racacit_ab[1] = mode;
    }
    return NULL;
}

/* "[sched]" every 10 s (RACACIT_SCHED_STATS=0 off): for each key thread, ms a second on a core and
 * ms a second runnable but waiting for one (/proc/self/task/<tid>/schedstat). The four mid cores
 * carry the SPU lanes, the render threads and the sound kernel together; this says whether the
 * ones the main thread waits on are kept waiting for a core. */
static int s_sched_lines;   /* "[sched]" asked for (the thread may run for "[ib2]" alone) */
static void* racacit_sched_thread(void*)
{
    racacit_background_thread("sched stats");
    unsigned long long run0[16] = {0}, wait0[16] = {0};
    long tid0[16] = {0};
    /* The threads the table above does not hold, by name, all threads of a name summed: the
     * sound feeders (.16 raised them), the cellAudio clock, the archive inflaters. */
    static const char* const k_more[] = { "cellAudio mix", "syn_tick_timer_", "snd_stream_thre", "fios mediathrea", "fios scheduler", "Bink Audio" };
    enum { NMORE = sizeof k_more / sizeof k_more[0], NTID = 96 };
    struct { long tid; int k; unsigned long long run, wait; } prev[NTID]; int nprev = 0;
    struct timespec a; clock_gettime(CLOCK_MONOTONIC, &a);
    for (;;) {
        sleep(10);
        struct timespec b; clock_gettime(CLOCK_MONOTONIC, &b);
        const double secs = (double)(b.tv_sec - a.tv_sec) + (double)(b.tv_nsec - a.tv_nsec) / 1e9;
        a = b;
        char msg[1800]; int o = snprintf(msg, sizeof msg, "[sched] %.0f s, ms a second on a core / waiting for one (nice):", secs);
        const int n = __atomic_load_n(&s_sched_n, __ATOMIC_ACQUIRE);
        int shown = 0;
        for (int i = 0; i < n && i < 16 && o < (int)sizeof msg - 64; i++) {
            const long tid = __atomic_load_n(&s_sched_th[i].tid, __ATOMIC_ACQUIRE);
            char path[64]; snprintf(path, sizeof path, "/proc/self/task/%ld/schedstat", tid);
            unsigned long long run = 0, wait = 0;
            FILE* f = fopen(path, "r");
            if (!f) continue;
            const int ok = fscanf(f, "%llu %llu", &run, &wait) == 2;
            fclose(f);
            if (!ok) continue;
            errno = 0; const int nice = getpriority(PRIO_PROCESS, (id_t)tid);
            if (tid0[i] == tid && secs > 0)
                o += snprintf(msg + o, sizeof msg - o, " %s %.0f/%.0f (%d);", s_sched_th[i].name,
                              (double)(run - run0[i]) / 1e6 / secs, (double)(wait - wait0[i]) / 1e6 / secs, errno ? 99 : nice), shown++;
            tid0[i] = tid; run0[i] = run; wait0[i] = wait;
        }
        /* The named extras: scan the process's threads. */
        double mrun[NMORE] = {0}, mwait[NMORE] = {0}; int mn[NMORE] = {0}, mnice[NMORE];
        for (int k = 0; k < NMORE; k++) mnice[k] = 99;
        if (DIR* d = opendir("/proc/self/task")) {
            while (struct dirent* e = readdir(d)) {
                const long tid = atol(e->d_name);
                if (tid <= 0) continue;
                char path[64], comm[32] = "";
                snprintf(path, sizeof path, "/proc/self/task/%ld/comm", tid);
                if (FILE* f = fopen(path, "r")) { if (!fgets(comm, sizeof comm, f)) comm[0] = 0; fclose(f); }
                comm[strcspn(comm, "\n")] = 0;
                int k = 0;
                while (k < NMORE && strncmp(comm, k_more[k], strlen(k_more[k]))) k++;
                if (k == NMORE) continue;
                snprintf(path, sizeof path, "/proc/self/task/%ld/schedstat", tid);
                unsigned long long run = 0, wait = 0;
                FILE* f = fopen(path, "r");
                if (!f) continue;
                const int ok = fscanf(f, "%llu %llu", &run, &wait) == 2;
                fclose(f);
                if (!ok) continue;
                int j = 0;
                while (j < nprev && prev[j].tid != tid) j++;
                if (j < nprev && secs > 0) {
                    mrun[k] += (double)(run - prev[j].run) / 1e6 / secs;
                    mwait[k] += (double)(wait - prev[j].wait) / 1e6 / secs;
                }
                if (j == nprev && nprev < NTID) { prev[j].tid = tid; nprev++; }
                if (j < nprev) { prev[j].k = k; prev[j].run = run; prev[j].wait = wait; }
                mn[k]++;
                errno = 0; const int nice = getpriority(PRIO_PROCESS, (id_t)tid);
                if (!errno) mnice[k] = nice;
            }
            closedir(d);
        }
        for (int k = 0; k < NMORE && o < (int)sizeof msg - 80; k++)
            if (mn[k]) o += snprintf(msg + o, sizeof msg - o, " %s x%d %.0f/%.0f (%d);", k_more[k], mn[k], mrun[k], mwait[k], mnice[k]);
        /* Collision jobs as fibers (spu_drain.c): jobs, runs on the main thread, lane fallbacks, no slot. */
        { static unsigned long fn0[4]; unsigned long fn[4];
          for (int k = 0; k < 4; k++) fn[k] = __atomic_load_n(&g_racacit_fiber_n[k], __ATOMIC_RELAXED);
          if (fn[0] != fn0[0] && secs > 0 && o < (int)sizeof msg - 160)
              o += snprintf(msg + o, sizeof msg - o, " | collision fibers %.0f/s, run by the main thread %.0f/s, by their lane %.0f/s, no slot %lu",
                            (fn[0] - fn0[0]) / secs, (fn[1] - fn0[1]) / secs, (fn[2] - fn0[2]) / secs, fn[3] - fn0[3]);
          for (int k = 0; k < 4; k++) fn0[k] = fn[k]; }
        /* ...and by job: which of the two the main thread really runs, how each parks, and what its
         * lane does meanwhile (v113-android.44). */
        { static unsigned long fk0[3][7]; static const char* const k_job[3] = { "narrow", "broad", "other" };
          for (int j = 0; j < 3; j++) {
              unsigned long v[7]; int any = 0;
              for (int i = 0; i < 7; i++) { v[i] = __atomic_load_n(&g_racacit_fiber_k[j][i], __ATOMIC_RELAXED); any |= v[i] != fk0[j][i]; }
              if (any && secs > 0 && o < (int)sizeof msg - 200)
                  o += snprintf(msg + o, sizeof msg - o, " | %s: %.0f jobs/s, main %.0f/s, lane %.0f/s, parks %.0f/s on a line + %.0f/s polling (%.0f/s lane-only), lane sleeps %.0f/s",
                                k_job[j], (v[0] - fk0[j][0]) / secs, (v[1] - fk0[j][1]) / secs, (v[2] - fk0[j][2]) / secs, (v[3] - fk0[j][3]) / secs,
                                (v[4] - fk0[j][4]) / secs, (v[5] - fk0[j][5]) / secs, (v[6] - fk0[j][6]) / secs);
              for (int i = 0; i < 7; i++) fk0[j][i] = v[i];
          } }
        /* v121-android.65: the core swap -- waits that moved the main thread off the top core, lanes given it. */
        { static unsigned long sw0[3]; unsigned long sw[3];
          for (int k = 0; k < 3; k++) sw[k] = __atomic_load_n(&s_swap_n[k], __ATOMIC_RELAXED);
          if (secs > 0 && o < (int)sizeof msg - 80)
              o += snprintf(msg + o, sizeof msg - o, " | core swap: %.0f waits/s, %.0f lanes/s to the top core",
                            (sw[0] - sw0[0]) / secs, (sw[1] - sw0[1]) / secs);
          for (int k = 0; k < 3; k++) sw0[k] = sw[k]; }
        if (shown && s_sched_lines) android_log_direct(msg);
        /* The sound module's computed branches (spu_channels.c "[ib2]"): fast memo or slow walk, and why. */
        { if (g_racacit_ib2[0]) { char ib[900]; racacit_ib2_report(ib, (int)sizeof ib); android_log_direct(ib); } }
    }
    return NULL;
}

static void android_swap_setup(void)
{
    /* "[sched]" off unless asked for (RACACIT_SCHED_STATS=1 or RACACIT_PROFILE=1: racacit_diag.h);
     * the same thread prints "[ib2]" when RACACIT_IB_STATS=1 counts it. */
    {   s_sched_lines = racacit_diag_switch("RACACIT_SCHED_STATS");
        const char* ib = getenv("RACACIT_IB_STATS");
        if (s_sched_lines || (ib && *ib == '1')) { pthread_t t; if (!pthread_create(&t, NULL, racacit_sched_thread, NULL)) pthread_detach(t); } }
    if (const char* e = getenv("RACACIT_SWAP_AFTER_US")) s_swap_after_tb = (unsigned long long)atol(e) * 80u;
    const char* ab = getenv("RACACIT_AB");
    const char* cs = getenv("RACACIT_CORE_SWAP");
    /* On unless set to 0. Measured with the A/B of .9-.11 (owner's .10 Zolar and .11 Space
     * Sector logs): on won 6 of the 7 neighbouring gameplay phases, 2-4 ms a frame less waiting on
     * SPU jobs. "ab", which those builds left in settings.txt, now means on: the A/B is only
     * RACACIT_AB=<seconds>. */
    const long secs = ab ? atol(ab) : 0;
    char msg[160];
    if (!s_swap_ok) {
        snprintf(msg, sizeof msg, "[android] core swap: not available here (needs the top core kept for the main thread and small cores)");
    } else if (secs > 0) {
        pthread_t t;
        pthread_create(&t, NULL, racacit_ab_thread, (void*)(intptr_t)(secs < 5 ? 5 : secs));
        pthread_detach(t);
        snprintf(msg, sizeof msg, "[android] core swap: A/B, %ld s off then %ld s on, in turn", secs < 5 ? 5 : secs, secs < 5 ? 5 : secs);
    } else {
        g_racacit_core_swap = !(cs && *cs == '0');
        snprintf(msg, sizeof msg, "[android] core swap: %s", g_racacit_core_swap ? "on" : "off");
    }
    android_log_direct(msg);
}

static void android_setup(int* argc, char*** argv)
{
    /* Timer slack: Linux lets every timed sleep or wait run late by the
     * thread's slack, 50 us for a foreground app. An idle SPU worker sleeps
     * 20-160 us (spu_channels.c; Windows uses a high-resolution timer there),
     * and the main thread meets the SPUs many times a frame, so each hand-off
     * from a resting worker cost the slack again; guest usleep and timed
     * event waits too. Set before any game thread exists: a new thread takes
     * its creator's value, so every thread the game starts from here on
     * sleeps as long as asked. */
    prctl(PR_SET_TIMERSLACK, 1UL, 0, 0, 0);
    /* Bring SDL up on the SDL_main thread; the frame clock's window and the
     * pad/audio backends then only add references. */
    SDL_SetHint(SDL_HINT_ORIENTATIONS, "LandscapeLeft LandscapeRight");
    /* Phones: SDL would add the accelerometer as joystick 0, and cellPad maps
     * joystick index to port -- a Bluetooth controller then became port 1,
     * which the game never reads. And the back button/gesture (an edge swipe
     * while playing on the touch controls) would close the game: trap it. */
    SDL_SetHint(SDL_HINT_ACCELEROMETER_AS_JOYSTICK, "0");
    SDL_SetHint(SDL_HINT_ANDROID_TRAP_BACK_BUTTON, "1");
    /* DualSense / DualShock 4: SDL's full HID driver, so rumble, the light
     * bar, the player LEDs, the motion sensors and (DualSense) the adaptive
     * triggers work over USB and Bluetooth alike. Must precede SDL_Init. */
    SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI_PS5_RUMBLE, "1");
    SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI_PS4_RUMBLE, "1");
    SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI_PS5_PLAYER_LED, "1");
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_GAMECONTROLLER | SDL_INIT_EVENTS) != 0) {
        __android_log_print(ANDROID_LOG_ERROR, "RACiT", "SDL initialization failed: %s", SDL_GetError());
        exit(EXIT_FAILURE);
    }
    const char* base = SDL_AndroidGetExternalStoragePath();
    if (!base || !*base) base = SDL_AndroidGetInternalStoragePath();
    if (!base || !*base) {
        __android_log_print(ANDROID_LOG_ERROR, "RACiT", "App storage is unavailable");
        exit(EXIT_FAILURE);
    }
    char path[1100];
    /* Relative paths resolve in the files folder, as they do beside the exe on
     * Windows. An app starts in "/", the read-only system partition: cellGame's
     * free-space query (content_free_kb from "./gamedata/...") then reads 0 KB
     * free there, and a fresh install stops on "Insufficient hard disk space". */
    if (chdir(base) != 0) {
        __android_log_print(ANDROID_LOG_ERROR, "RACiT", "Cannot enter app storage: %s", base);
        exit(EXIT_FAILURE);
    }

    /* Logging: a file only on request (the runtime is chatty, and a log on
     * flash costs frame time); otherwise logcat. */
    FILE* log_file = nullptr;
    snprintf(path, sizeof path, "%s/debug", base);
    if (access(path, F_OK) == 0) {
        snprintf(path, sizeof path, "%s/log.txt", base);
        if ((log_file = fopen(path, "w"))) setvbuf(log_file, NULL, _IOLBF, 0);
    }
    {
        int p[2];
        if (pipe(p) == 0) {
            if (android_log_pump_start(p[0], log_file)) {
                dup2(p[1], STDOUT_FILENO);
                dup2(p[1], STDERR_FILENO);
                setvbuf(stdout, NULL, _IOLBF, 0);
                setvbuf(stderr, NULL, _IOLBF, 0);
            } else {
                close(p[0]);
            }
            close(p[1]);
        }
        if (!s_android_log_started && log_file) { /* no pump: write directly */
            fclose(log_file);
            freopen(path, "w", stdout);
            freopen(path, "a", stderr);
            setvbuf(stdout, NULL, _IOLBF, 0);
        }
    }

    /* Defaults for this title; env.txt or the real environment win. */
    snprintf(path, sizeof path, "%s/dev_hdd0", base);
    mkdir(path, 0755);
    setenv("PS3_HDD0_ROOT", path, 0);
    /* The in-game Settings menu (rsx_host_ui) saves into the launcher's file,
     * so a change made in either place is there in the other. */
    snprintf(path, sizeof path, "%s/settings", base);
    mkdir(path, 0755);
    snprintf(path, sizeof path, "%s/settings/settings.txt", base);
    setenv("RACACIT_SETTINGS_FILE", path, 0);
    setenv("RACACIT_HLE_SPURS", "1", 0);
    setenv("PS3RECOMP_VK_GUEST_PROGRAMS", "1", 0);
    setenv("PS3RECOMP_VK_WINDOW", "1", 0);
    setenv("PS3RECOMP_VK_FULLSCREEN", "1", 0);
    setenv("PS3_TITLE", "Ratchet & Clank: A Crack in Time", 0);
    /* Translated shaders and driver pipelines, kept across runs: the first
     * sight of new scenery otherwise stalls the render thread for seconds. */
    snprintf(path, sizeof path, "%s/shadercache.bin", base);
    setenv("RSX_SHADER_CACHE", path, 0);
    snprintf(path, sizeof path, "%s/pipelinecache.bin", base);
    setenv("RSX_PIPELINE_CACHE", path, 0);
    snprintf(path, sizeof path, "%s/pipelinelist.bin", base);
    setenv("RSX_PIPELINE_LIST", path, 0);
    /* What each pipeline's shader text came from, so the launcher's Prepare
     * shaders can regenerate it after a translator change. */
    snprintf(path, sizeof path, "%s/programlist.bin", base);
    setenv("RSX_PROGRAM_LIST", path, 0);

    /* KEY=VALUE files, later ones winning: env.txt (old builds), the
     * launcher's settings/settings.txt (the player's settings, same names as
     * the Windows settings.txt) and launch.env (what the launcher sets up for
     * this run: enabled mods, the game folder when it is used in place). */
    const char* const env_files[] = { "env.txt", "settings/settings.txt", "launch.env" };
    for (const char* name : env_files) {
        snprintf(path, sizeof path, "%s/%s", base, name);
        FILE* f = fopen(path, "r");
        if (!f) continue;
        char line[2048];
        while (fgets(line, sizeof line, f)) {
            char* nl = strpbrk(line, "\r\n"); if (nl) *nl = 0;
            char* eq = strchr(line, '=');
            if (line[0] == '#' || !eq) continue;
            *eq = 0; setenv(line, eq + 1, 1);
        }
        fclose(f);
    }

    android_cpu_policy();
    android_swap_setup();
#ifdef RACIT_PGO_GEN
    android_pgo_start(base);
#endif

    /* The built-in sampling profiler, when the launcher turned it on
     * (RACACIT_PROFILE=1; android_profiler.cpp). */
    racacit_profiler_start(base);

    /* The game: GAME=<folder holding PS3_GAME> (used in place), else imported
     * into <files>/PS3_GAME. */
    const char* game = getenv("GAME");
    const char* root = game && *game ? game : base;
    const char* program = getenv("RACACIT_EBOOT");
    int program_len;
    if (program && *program) {
        program_len = snprintf(s_android_eboot, sizeof s_android_eboot, "%s", program);
        /* The independently imported program need not live in the disc dump. */
        setenv("PS3_VFS_ROOT", root, 1);
    } else {
        program_len = snprintf(s_android_eboot, sizeof s_android_eboot, "%s/PS3_GAME/USRDIR/EBOOT.elf", root);
        if (access(s_android_eboot, R_OK) != 0)
            program_len = snprintf(s_android_eboot, sizeof s_android_eboot, "%s/PS3_GAME/USRDIR/EBOOT.ELF", root);
    }
    if (program_len < 0 || (size_t)program_len >= sizeof s_android_eboot) {
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "RACiT game path", "The selected game program path is too long.", nullptr);
        exit(EXIT_FAILURE);
    }
    static char* s_argv[3] = { (char*)"racacit", s_android_eboot, NULL };
    *argc = 2; *argv = s_argv;
    printf("[android] files dir: %s\n[android] EBOOT: %s\n", base, s_android_eboot);

    if (access(s_android_eboot, R_OK) != 0) {
        char msg[1400];
        snprintf(msg, sizeof msg,
                 "Game data not found.\n\nCopy your decrypted dump so that this file exists:\n%s\n\n"
                 "(PS3_GAME/PARAM.SFO and PS3_GAME/USRDIR/packed, built must be there too.)",
                 s_android_eboot);
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Ratchet & Clank recomp", msg, NULL);
    }
}
#endif

#if defined(_WIN32)
/* ---------------------------------------------------------------------------
 * Windows launch
 *
 * RaCACiT.exe [path\to\EBOOT.elf]. Without an argument the game is found from
 * settings.txt beside the exe (GAME=<folder that holds PS3_GAME>, plus any
 * NAME=value environment overrides), else from a PS3_GAME folder beside the
 * exe. Saves/installed data go to dev_hdd0\ beside the exe; output goes to
 * log.txt there unless RACACIT_CONSOLE=1.
 * -----------------------------------------------------------------------*/
static char s_win_eboot[MAX_PATH * 2];
static char* s_win_argv[3];

static void win_msg(const char* text)
{
    MessageBoxA(NULL, text, "Ratchet & Clank: A Crack in Time", MB_OK | MB_ICONERROR);
}

static int win_file_exists(const char* p)
{
    DWORD a = GetFileAttributesA(p);
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

/* Crash reporting: a vectored handler (registered after the guest-VM commit
 * handler, so demand-paged guest faults never reach it) logs the first few
 * hard faults -- code, faulting module + offset, and the exe-relative return
 * addresses found on the stack -- to log.txt, then lets the fault continue
 * as before. Offsets resolve against RaCACiT.map from the build. Static
 * buffers and a raw WriteFile: it must work on an overflowed stack too. */
static char s_crash_buf[4096];
static volatile LONG s_crash_n = 0;
static void crash_write(const char* s)
{
    DWORD w; HANDLE h = (HANDLE)_get_osfhandle(2);
    if (h && h != INVALID_HANDLE_VALUE) WriteFile(h, s, (DWORD)strlen(s), &w, NULL);
}
static void crash_describe(const char* tag, EXCEPTION_POINTERS* ep)
{
    const DWORD code = ep->ExceptionRecord->ExceptionCode;
    const uintptr_t rip = (uintptr_t)ep->ContextRecord->Rip;
    HMODULE exe = GetModuleHandleA(NULL), mod = NULL;
    char modname[MAX_PATH] = "?";
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           (LPCSTR)rip, &mod) && mod) {
        GetModuleFileNameA(mod, modname, sizeof modname);
    }
    const char* base = strrchr(modname, '\\');
    int n = snprintf(s_crash_buf, sizeof s_crash_buf,
        "\n[CRASH] %s code=0x%08lX tid=%lu at %s+0x%llX (rip=0x%llX)",
        tag, (unsigned long)code, (unsigned long)GetCurrentThreadId(), base ? base + 1 : modname,
        (unsigned long long)(rip - (uintptr_t)mod), (unsigned long long)rip);
    if (code == EXCEPTION_ACCESS_VIOLATION && ep->ExceptionRecord->NumberParameters >= 2)
        n += snprintf(s_crash_buf + n, sizeof s_crash_buf - n, " %s 0x%llX (guest base 0x%llX)",
                      ep->ExceptionRecord->ExceptionInformation[0] ? "writing" : "reading",
                      (unsigned long long)ep->ExceptionRecord->ExceptionInformation[1],
                      (unsigned long long)(uintptr_t)vm_base);
    n += snprintf(s_crash_buf + n, sizeof s_crash_buf - n, "\n[CRASH] exe stack:");
    /* Return addresses into the exe found on the stack (a heuristic walk). */
    IMAGE_DOS_HEADER* dos = (IMAGE_DOS_HEADER*)exe;
    IMAGE_NT_HEADERS* nt = (IMAGE_NT_HEADERS*)((char*)exe + dos->e_lfanew);
    const uintptr_t lo = (uintptr_t)exe, hi = lo + nt->OptionalHeader.SizeOfImage;
    const uintptr_t* sp = (const uintptr_t*)ep->ContextRecord->Rsp;
    MEMORY_BASIC_INFORMATION mbi;
    int found = 0;
    if (VirtualQuery(sp, &mbi, sizeof mbi)) {
        const uintptr_t* end = (const uintptr_t*)((char*)mbi.BaseAddress + mbi.RegionSize);
        for (const uintptr_t* q = sp; q < end && found < 24 && n < (int)sizeof s_crash_buf - 24; q++)
            if (*q >= lo && *q < hi) { n += snprintf(s_crash_buf + n, sizeof s_crash_buf - n, " 0x%llX",
                                                     (unsigned long long)(*q - lo)); found++; }
        /* Any-module view of the top of the stack: system DLL frames too. */
        n += snprintf(s_crash_buf + n, sizeof s_crash_buf - n, "\n[CRASH] stack modules:");
        int shown = 0;
        for (const uintptr_t* q = sp; q < end && shown < 16 && n < (int)sizeof s_crash_buf - 64; q++) {
            HMODULE m2 = NULL;
            if (*q < 0x10000 || *q >= lo && *q < hi) continue;
            if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                    GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)*q, &m2) || !m2)
                continue;
            char mn[MAX_PATH]; GetModuleFileNameA(m2, mn, sizeof mn);
            const char* b2 = strrchr(mn, '\\');
            n += snprintf(s_crash_buf + n, sizeof s_crash_buf - n, " [sp+0x%llX]%s+0x%llX",
                          (unsigned long long)((const char*)q - (const char*)sp), b2 ? b2 + 1 : mn,
                          (unsigned long long)(*q - (uintptr_t)m2));
            shown++;
        }
        n += snprintf(s_crash_buf + n, sizeof s_crash_buf - n, "\n[CRASH] rsp=0x%llX rbp=0x%llX rcx=0x%llX rdx=0x%llX",
                      (unsigned long long)ep->ContextRecord->Rsp, (unsigned long long)ep->ContextRecord->Rbp,
                      (unsigned long long)ep->ContextRecord->Rcx, (unsigned long long)ep->ContextRecord->Rdx);
    }
    snprintf(s_crash_buf + n, sizeof s_crash_buf - n, "\n");
    crash_write(s_crash_buf);
}
static LONG WINAPI crash_veh(EXCEPTION_POINTERS* ep)
{
    const DWORD code = ep->ExceptionRecord->ExceptionCode;
    if (code == EXCEPTION_ACCESS_VIOLATION || code == EXCEPTION_ILLEGAL_INSTRUCTION ||
        code == EXCEPTION_STACK_OVERFLOW || code == EXCEPTION_INT_DIVIDE_BY_ZERO ||
        code == EXCEPTION_PRIV_INSTRUCTION || code == EXCEPTION_IN_PAGE_ERROR ||
        code == 0xC0000409 /* fast-fail / stack buffer overrun */) {
        if (InterlockedIncrement(&s_crash_n) <= 6) crash_describe("first-chance", ep);
    }
    return EXCEPTION_CONTINUE_SEARCH;
}
static LONG WINAPI crash_unhandled(EXCEPTION_POINTERS* ep)
{
    crash_describe("UNHANDLED (process ending)", ep);
    return EXCEPTION_CONTINUE_SEARCH;
}

static void windows_setup(int* argc, char*** argv)
{
    char dir[MAX_PATH * 2];
    GetModuleFileNameA(NULL, dir, sizeof dir);
    char* slash = strrchr(dir, '\\');
    if (slash) *slash = 0;

    char path[MAX_PATH * 3];
    char game[MAX_PATH * 2] = "";
    /* RACACIT_SETTINGS_FILE (set by the Windows launcher: its data\settings.txt)
     * names the settings file; else settings.txt beside the exe. */
    if (const char* sf = getenv("RACACIT_SETTINGS_FILE"); sf && *sf) snprintf(path, sizeof path, "%s", sf);
    else snprintf(path, sizeof path, "%s\\settings.txt", dir);
    if (FILE* f = fopen(path, "r")) {
        char line[2048];
        while (fgets(line, sizeof line, f)) {
            char* e = line + strlen(line);
            while (e > line && (e[-1] == '\n' || e[-1] == '\r' || e[-1] == ' ')) *--e = 0;
            if (!line[0] || line[0] == '#') continue;
            char* eq = strchr(line, '=');
            if (!eq) continue;
            *eq = 0;
            const char* val = eq + 1;
            while (*val == ' ' || *val == '"') val++;
            char v[2048]; snprintf(v, sizeof v, "%s", val);
            size_t n = strlen(v); while (n && v[n - 1] == '"') v[--n] = 0;
            if (!_stricmp(line, "GAME")) snprintf(game, sizeof game, "%s", v);
            else _putenv_s(line, v);
        }
        fclose(f);
    }
    /* The in-game Settings menu writes its choices back here. */
    if (!getenv("RACACIT_SETTINGS_FILE")) _putenv_s("RACACIT_SETTINGS_FILE", path);

    if (getenv("RACACIT_CONSOLE")) {
        AllocConsole();
        freopen("CONOUT$", "w", stdout);
        freopen("CONOUT$", "w", stderr);
    } else {
        /* One file, one shared offset: stdout reopened on log.txt and fd 2
         * duplicated onto it, so the two streams interleave instead of
         * overwriting each other. RACACIT_LOG_FILE (the launcher) moves it. */
        if (const char* lf = getenv("RACACIT_LOG_FILE"); lf && *lf) snprintf(path, sizeof path, "%s", lf);
        else snprintf(path, sizeof path, "%s\\log.txt", dir);
        if (freopen(path, "w", stdout)) {
            setvbuf(stdout, NULL, _IONBF, 0);
            /* A GUI-subsystem process starts with stderr detached (no fd),
             * and pointing fd 2 at the file does not reattach the stream:
             * every fprintf(stderr) was silently dropped on real Windows
             * (Wine hands GUI apps std handles, so it never showed). Give
             * the stream an fd first, then aim that fd at log.txt. */
            if (_fileno(stderr) < 0) freopen("NUL", "w", stderr);
            _dup2(_fileno(stdout), _fileno(stderr) >= 0 ? _fileno(stderr) : 2);
            _dup2(_fileno(stdout), 2);
            setvbuf(stderr, NULL, _IONBF, 0);
        }
    }

    AddVectoredExceptionHandler(0, crash_veh);
    SetUnhandledExceptionFilter(crash_unhandled);

    /* No IME or Text Services Framework for this process. SDL turns text
     * input on for every window, which attaches TSF (msctf /
     * textinputframework) to the window's thread -- our frame clock, busy
     * for long stretches between event pumps -- and TSF's CoreMessaging
     * dispatcher fail-fasts there (0xE0464645 in CoreMessaging.dll, minutes
     * into play: the process vanishes with nothing in log.txt). A game has
     * no text to compose. Must run before any window exists. */
    if (HMODULE imm = LoadLibraryA("imm32.dll")) {
        typedef BOOL (WINAPI *imm_fn)(DWORD);
        if (imm_fn f = (imm_fn)GetProcAddress(imm, "ImmDisableTextFrameService")) f((DWORD)-1);
        if (imm_fn f = (imm_fn)GetProcAddress(imm, "ImmDisableIME")) f((DWORD)-1);
    }

    if (!getenv("PS3_HDD0_ROOT")) {
        snprintf(path, sizeof path, "%s\\dev_hdd0", dir);
        CreateDirectoryA(path, NULL);
        _putenv_s("PS3_HDD0_ROOT", path);
    }
    if (!getenv("RACACIT_HLE_SPURS")) _putenv_s("RACACIT_HLE_SPURS", "1");
    if (!getenv("PS3RECOMP_VK_GUEST_PROGRAMS")) _putenv_s("PS3RECOMP_VK_GUEST_PROGRAMS", "1");
    if (!getenv("PS3RECOMP_VK_WINDOW")) _putenv_s("PS3RECOMP_VK_WINDOW", "1");
    if (!getenv("PS3_TITLE")) _putenv_s("PS3_TITLE", "Ratchet & Clank: A Crack in Time");
    /* Translated shaders and driver pipelines, kept across runs: the first
     * sight of new scenery otherwise stalls the render thread for seconds. */
    snprintf(path, sizeof path, "%s\\shadercache.bin", dir);
    if (!getenv("RSX_SHADER_CACHE")) _putenv_s("RSX_SHADER_CACHE", path);
    snprintf(path, sizeof path, "%s\\pipelinecache.bin", dir);
    if (!getenv("RSX_PIPELINE_CACHE")) _putenv_s("RSX_PIPELINE_CACHE", path);
    /* The pipelines seen so far, rebuilt in the background when a driver
     * update made pipelinecache.bin useless (rsx_vulkan_backend.c). */
    snprintf(path, sizeof path, "%s\\pipelinelist.bin", dir);
    if (!getenv("RSX_PIPELINE_LIST")) _putenv_s("RSX_PIPELINE_LIST", path);

    if (*argc >= 2) return;

    const char* cands[4] = { NULL, NULL, NULL, NULL };
    char c0[MAX_PATH * 3], c1[MAX_PATH * 3], c2[MAX_PATH * 3];
    if (game[0]) {
        /* GAME may name the folder holding PS3_GAME, PS3_GAME itself, or USRDIR. */
        snprintf(c0, sizeof c0, "%s\\PS3_GAME\\USRDIR\\EBOOT.elf", game);
        snprintf(c1, sizeof c1, "%s\\USRDIR\\EBOOT.elf", game);
        snprintf(c2, sizeof c2, "%s\\EBOOT.elf", game);
        cands[0] = c0; cands[1] = c1; cands[2] = c2;
    } else {
        snprintf(c0, sizeof c0, "%s\\PS3_GAME\\USRDIR\\EBOOT.elf", dir);
        cands[0] = c0;
    }
    for (int i = 0; cands[i]; i++)
        if (win_file_exists(cands[i])) {
            /* Normalise to <root>\PS3_GAME\USRDIR\EBOOT.elf form for derive_vfs_root. */
            _fullpath(s_win_eboot, cands[i], sizeof s_win_eboot);
            s_win_argv[0] = (*argv)[0];
            s_win_argv[1] = s_win_eboot;
            s_win_argv[2] = NULL;
            *argc = 2; *argv = s_win_argv;
            printf("[windows] EBOOT: %s\n[windows] HDD: %s\n", s_win_eboot, getenv("PS3_HDD0_ROOT"));
            return;
        }
    char msg[4096];
    snprintf(msg, sizeof msg,
             "Could not find the game.\n\nEdit settings.txt next to RaCACiT.exe so the GAME= line "
             "points at the folder that contains PS3_GAME (your disc dump), e.g.\n\n"
             "GAME=C:\\Games\\Ratchet & Clank Future - A Crack in Time\n\n"
             "PS3_GAME\\USRDIR must contain the decrypted EBOOT.elf.%s%s",
             game[0] ? "\n\nCurrently: " : "", game);
    win_msg(msg);
    exit(2);
}
#endif

#ifdef __ANDROID__
/* SDLActivity calls SDL_main on a Java thread with a ~1 MB stack, and the
 * primary guest thread runs on whatever thread calls ppu_run(). Lifted code
 * recurses as deep as the title does, so give it the same 256 MB the runtime
 * gives every other guest thread (Linux's main thread has 8 MB). SDL attaches
 * the new thread to the JVM on first use and detaches it on exit. */
static int racacit_main(int argc, char** argv);
struct racacit_big_stack { int argc; char** argv; int rc; };
int main(int argc, char** argv)        /* SDL.h renames this to SDL_main */
{
    racacit_big_stack a = { argc, argv, 0 };
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 256u << 20);
    pthread_t t;
    int err = pthread_create(&t, &attr, [](void* p) -> void* {
        racacit_big_stack* b = (racacit_big_stack*)p;
        b->rc = racacit_main(b->argc, b->argv);
        return nullptr;
    }, &a);
    pthread_attr_destroy(&attr);
    if (err != 0) return racacit_main(argc, argv);
    pthread_join(t, nullptr);
    return a.rc;
}
#undef main
#define main racacit_main
#endif

#if defined(_WIN32) && (defined(__x86_64__) || defined(_M_X64))
/* The lifted PPU and SPU code is built for AVX2 + FMA + BMI2 (game/CMakeLists.txt:
 * hardware fused multiply-add for PPC fmadd, wider SPU vector ops), and so is
 * the runtime; RaCACiT_compat.exe for SSE4.2 (RACACIT_CPU_COMPAT). Say so plainly on an older CPU rather than dying on an illegal
 * instruction. This file is built for plain x86-64. */
static int racacit_cpu_ok(void)
{
    unsigned c_a, c_b, c_c, c_d;
#if defined(RACACIT_CPU_COMPAT)
    /* RaCACiT_compat.exe (game/CMakeLists.txt RACACIT_CPU_COMPAT): x86-64-v2,
     * SSE3, SSSE3, SSE4.1, SSE4.2 and POPCNT. */
    int c_ok = __get_cpuid(1, &c_a, &c_b, &c_c, &c_d) && (c_c & (1u << 0)) && (c_c & (1u << 9)) &&
               (c_c & (1u << 19)) && (c_c & (1u << 20)) && (c_c & (1u << 23));
    /* RACACIT_TEST_NO_SSE42 (environment): the refusal on any CPU, to test it. */
    if (GetEnvironmentVariableA("RACACIT_TEST_NO_SSE42", NULL, 0)) c_ok = 0;
    return c_ok;
#else
    int c_ok = __get_cpuid(1, &c_a, &c_b, &c_c, &c_d) && (c_c & (1u << 12)) && (c_c & (1u << 27)) &&
               (c_c & (1u << 28));                                   /* FMA, OSXSAVE, AVX */
    if (c_ok) {
        unsigned x_lo, x_hi;
        __asm__ volatile("xgetbv" : "=a"(x_lo), "=d"(x_hi) : "c"(0));
        c_ok = (x_lo & 6u) == 6u;                                    /* the OS saves YMM state */
    }
    if (c_ok) c_ok = __get_cpuid_count(7, 0, &c_a, &c_b, &c_c, &c_d) &&
                     (c_b & (1u << 5)) && (c_b & (1u << 8));         /* AVX2, BMI2 */
    /* RACACIT_TEST_NO_AVX2 (in the environment, not settings.txt): take the
     * refusal path on any CPU, to test it. */
    if (GetEnvironmentVariableA("RACACIT_TEST_NO_AVX2", NULL, 0)) c_ok = 0;
    return c_ok;
#endif
}
static void racacit_cpu_refuse(void)
{
#if defined(RACACIT_CPU_COMPAT)
    MessageBoxA(NULL, "This build needs a CPU with SSE4.2 (Intel Core i from 2008 or newer, AMD FX or "
                      "newer). This computer's processor does not have it, so the game cannot run on it.",
#else
    MessageBoxA(NULL, "This build needs a CPU with AVX2, FMA and BMI2 (Intel Haswell from 2013 or newer, "
                      "AMD Ryzen or newer). This computer's processor does not have them.\n\n"
                      "For an older processor, start the game from the launcher with Settings > "
                      "Performance > Game build set to Automatic or Older CPUs.",
#endif
                "Ratchet & Clank: A Crack in Time", MB_OK | MB_ICONERROR);
}
#if !defined(_MSC_VER)
/* The process entry (game/CMakeLists.txt: -e racacit_entry), ahead of the C
 * runtime's start-up: the runtime library's and the lifted code's global
 * constructors are AVX2/BMI2 code too, and they run before main(). With the
 * check only in main(), an i7-3770 (Ivy Bridge: AVX, no AVX2) died at start
 * with 0xC000001D and never saw the message (user report, v95). */
extern "C" int mainCRTStartup(void);
extern "C" int racacit_entry(void)
{
    if (!racacit_cpu_ok()) { racacit_cpu_refuse(); ExitProcess(1); }
    return mainCRTStartup();
}
#endif
#endif

int main(int argc, char** argv)
{
#if defined(__APPLE__) && defined(RACACIT_HAVE_VULKAN)
    /* Preserve IEEE shader guards before SDL or the Vulkan loader starts.
     * Pending asynchronous pipelines currently skip draws in this backend. */
    setenv("MVK_CONFIG_FAST_MATH_ENABLED", "0", 0);
    setenv("RACACIT_ASYNC_PIPELINES", "0", 0);
    setenv("RACACIT_HLE_SPURS", "1", 0);
    setenv("PS3RECOMP_VK_WINDOW", "1", 0);
#endif
#ifdef __ANDROID__
    android_setup(&argc, &argv);
#endif
#if defined(_WIN32) && (defined(__x86_64__) || defined(_M_X64))
    if (!racacit_cpu_ok()) { racacit_cpu_refuse(); return 1; }
#endif
#if defined(_WIN32)
    windows_setup(&argc, &argv);
#endif
    if (argc < 2) {
        printf("usage: %s <PPU ELF>\n", argv[0]);
        return 2;
    }

#ifdef _WIN32
#pragma comment(lib, "winmm.lib")
    /* 1 ms timer resolution. The default granularity is about 15.6 ms, which
     * inflates every shorter wait the title makes and throttles the whole
     * thing. POSIX timers are already fine-grained. */
    timeBeginPeriod(1);
    setvbuf(stdout, NULL, _IONBF, 0);   /* unbuffered: do not lose prints on a kill */
#endif

    printf("=== ps3recomp game runner ===\n");
#if defined(_WIN32) && (defined(__x86_64__) || defined(_M_X64))
    /* Which exe and which processor: a player's log said neither, and the
     * older-CPU build's slow paths were found only by matching its stall
     * stacks against each build's symbols. */
    {   unsigned r[12] = { 0 }; char brand[49] = { 0 };
        for (unsigned i = 0; i < 3; i++) __get_cpuid(0x80000002u + i, &r[i * 4], &r[i * 4 + 1], &r[i * 4 + 2], &r[i * 4 + 3]);
        memcpy(brand, r, 48);
        const char* b = brand; while (*b == ' ') b++;
        for (size_t n = strlen(brand); n && brand[n - 1] == ' '; n--) brand[n - 1] = 0;
        SYSTEM_INFO si; GetSystemInfo(&si);
#if defined(RACACIT_CPU_COMPAT)
        const char* build = "older-CPU build (RaCACiT_compat.exe, no AVX2)";
#else
        const char* build = "standard build (RaCACiT.exe, AVX2)";
#endif
        printf("[racacit] %s; CPU: %s, %lu logical processors\n", build, *b ? b : "unknown",
               (unsigned long)si.dwNumberOfProcessors); }
#endif

    if (!alloc_guest_vm()) {
        fprintf(stderr, "ERROR: could not allocate the guest address space\n");
        return 1;
    }

    /* A Crack in Time maps its own GCM IO memory at 0x20000000 (ioSize
     * 0xBE00000) and its command buffer starts there, so the runtime's default
     * GCM window (control put/get/ref, labels, offset tables at 0x20000000+)
     * sat inside the title's command buffer: command writes clobbered put/get
     * and the boot hung on a GCM wait at random. Move the window above the
     * PPU stack region, which nothing in the title maps. */
    {
        ppu_hle_inject_base = 0xE0100000u;
        /* The lifted code is built with PPU_INL_NO_HOLE (game/CMakeLists.txt):
         * ppu_run ends its inline memory window below that page instead. */
        ppu_inl_clip_hole = 1;
    }

#if defined(__ANDROID__) && defined(RACACIT_HAVE_VULKAN)
    /* RACACIT_PREPARE=1 (the launcher's "Prepare shaders"): Vulkan only, no
     * game -- every pipeline the list holds built into the cache on all cores
     * (rsx_vulkan_prepare_shaders), the game activity drawing the progress. */
    if (getenv("RACACIT_PREPARE") && getenv("RACACIT_PREPARE")[0] == '1') {
        const char* t = getenv("PS3_TITLE");
        if (rsx_backend_init(WINDOW_WIDTH, WINDOW_HEIGHT, t ? t : "RACiT") != 0) {
            fprintf(stderr, "[prepare] Vulkan init failed%c", 10);
            android_drain_startup_log();
            _exit(2);
        }
        const int r = rsx_vulkan_prepare_shaders();
        fprintf(stderr, "[prepare] finished (%d)%c", r, 10);
        fflush(stdout); fflush(stderr);
        SDL_Delay(2500);          /* the bar shows "done" before the activity closes */
        _exit(0);
    }
#endif
    if (!racit_menu_archive_init()) return 1;
    uint32_t entry = ppu_load_elf(argv[1]);
    if (!entry) {
        fprintf(stderr, "ERROR: could not load %s\n", argv[1]);
        return 1;
    }

    vm_diag_refresh();   /* settings.txt may have enabled a memory diagnostic */
    derive_vfs_root(argv[1]);
    printf("[boot] VFS root: %s\n", ppu_vfs_root);
    map_cellfs_devices();

    /* Real title id (BCUS98124) from PARAM.SFO; without this cellGame,
     * save data and trophies all fall back to the BLES00000 placeholder. */
    {
        char sfo[1100];
        snprintf(sfo, sizeof sfo, "%s/PS3_GAME/PARAM.SFO", ppu_vfs_root);
        cellGame_init_from_paramsfo(sfo);
    }

    ppu_recomp_register();   /* lifted function table -> address map */
    /* SPU job ELFs the WWS job manager stages at LS 0x4000 (SCREAM synth,
     * reverb): sealed from the loaded EBOOT so they dispatch lifted. */
    racacit_register_elf_overlays();
    racacit_register_extra_overlays();
    /* PS3_SAMPLE=<ms> (Windows): sampling profiler -- per-thread split and the
     * hottest guest functions / host modules, reported into the log. */
#ifdef _WIN32
    { extern void ps3_sampler_start(void); ps3_sampler_start(); }
#endif
    ps3_load_prx_modules();  /* this game's lifted system PRX, if it has any */
    ppu_hle_init();          /* firmware import NID -> HLE handlers */
    racacit_register_language();   /* RACACIT_LANGUAGE: over the runtime's system-language answer */
    racacit_register_trophies();   /* a higher bolt trophy brings the lower ones */
    ppu_sysprx_register();   /* boot-critical CRT */
    ppu_fs_register();       /* cellFs over the game directory */
    lv2_init_syscalls();     /* the lv2 syscall table */
#ifdef __ANDROID__
    /* Fix the write-watch mode before renderer/guest threads begin. Kernel
     * write brackets must never straddle lazy initialization of protection. */
    (void)rsx_ww_on();
#endif

    /* Install the guest-callback hook, then start the frame clock. It no-ops
     * until the title registers its vblank and flip handlers during init. */
    g_ps3_guest_caller = harness_guest_caller;
#if defined(__APPLE__) && defined(RACACIT_HAVE_VULKAN)
    /* Cocoa creates and pumps windows on the process main thread. */
    const char* window = getenv("PS3RECOMP_VK_WINDOW");
    if (window && strcmp(window, "1") == 0) {
        static uint32_t window_entry;
        window_entry = entry;
        HANDLE guest = CreateThread(NULL, 16u * 1024 * 1024,
            [](LPVOID argument) -> DWORD {
                int result = ppu_run(*(uint32_t*)argument, STACK_TOP);
                fprintf(stderr, "[boot] windowed guest returned %d\n", result);
                fflush(stdout); fflush(stderr);
                _exit(g_sys_process_exit_called ? (int)g_sys_process_exit_code : result);
                return 0;
            }, &window_entry, 0, NULL);
        if (!guest) return 1;
        frame_clock(NULL);
        return 0;
    }
#endif
    CreateThread(NULL, 4u * 1024 * 1024, frame_clock, NULL, 0, NULL);
    if (getenv("PS3_PEEK")) CreateThread(NULL, 0, peek_thread, NULL, 0, NULL);

    printf("\n[boot] dispatching entry OPD 0x%08X (stack top 0x%08X)\n\n",
           entry, STACK_TOP);

#ifdef __ANDROID__
    /* This thread runs the guest's main thread: named for the log pump (android_name_thread),
     * so the profiler, the companion's Status page and the performance hint session know it.
     * Unannounced, it had no name until SDL first attached it to Java, and then "Thread-N". */
    prctl(PR_SET_NAME, "PPU main thread", 0, 0, 0);
    g_racacit_main_tid = (long)gettid();   /* the core swap's (racacit_swap_poll) */
    /* Not pinned to the top core (v113-android.14 did, racacit_pin_main): threads inherit their
     * creator's affinity, and those the main thread makes that are not placed by name -- the
     * cellAudio mixer among them -- were then held to the top core with the main thread, and the
     * game's sound ran at 62% speed. */
    fprintf(stderr, "[hosttid] %lu PPU main thread\n", (unsigned long)GetCurrentThreadId());
#endif
    int rc = ppu_run(entry, STACK_TOP);
    printf("\n[boot] ppu_run returned %d (entry function unwound)\n", rc);

    /* A title that called sys_process_exit never reaches this line: that path
     * ends in the host exit(). Getting here means the entry function returned
     * instead, so hand back the status the guest published if it published
     * one. Returning a hardcoded 0 would report success for a run that never
     * got anywhere. */
    return g_sys_process_exit_called ? (int)g_sys_process_exit_code : 0;
}
