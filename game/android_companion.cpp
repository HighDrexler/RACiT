/*
 * Android only: the game's links to the Android side.
 *   - the dual-screen companion: the Java side (NativeBridge.java) queues
 *     presses; cellPad.c (racacit_companion_step) merges them into port 0
 *     using bounded, cancellable guest-frame transactions
 *   - the host UI's font, drawn by Android's text engine (HostFont.java)
 *   - performance hints (ADPF): the game's busiest threads in one hint
 *     session, told each frame how long it took, so the CPU governor clocks
 *     for 60 fps instead of guessing from bursts
 */
#ifdef __ANDROID__
#include <jni.h>
#include <dlfcn.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <time.h>
#include <math.h>
#include <pthread.h>
#include <sys/uio.h>
#include <unistd.h>
#include <SDL.h>
#include "android_companion_input.h"

extern "C" uint8_t* vm_base;
extern "C" unsigned cellGcm_flip_request_count(void);
extern "C" unsigned ppu_boot_frames_presented(void);
extern "C" volatile unsigned g_racacit_level_changes, g_racacit_movie_frames;
extern "C" unsigned long long g_racacit_main_wait_ticks;   /* stubs.cpp racacit_wait_poll */
extern "C" volatile long g_racacit_ab[6];                   /* main.cpp racacit_ab_thread */

namespace {

/* All sections are bounded copies/state updates. No spinning, allocation,
 * JNI calls, guest reads or Vulkan work while this mutex is held. */
pthread_mutex_t s_input_mutex = PTHREAD_MUTEX_INITIALIZER;
struct InputGuard { InputGuard() { pthread_mutex_lock(&s_input_mutex); }
                    ~InputGuard() { pthread_mutex_unlock(&s_input_mutex); } };
CompanionInput s_input;
unsigned s_presented;
int s_weapon=-1;
uint64_t s_movie_at;
unsigned s_movie_frames;

/* The quick select table: 3 rings of 8 weapon ids, -1 empty (func_0016BC80
 * fills it, the pause menu copies it to Flash as quickselectItems). */
const uint32_t QS_TABLE=0x00E24CA0u;
const int QS_SLOTS=24;
const int WPN_LAST=22;              /* func_0016BC80 refuses ids above this */
int s_slots[QS_SLOTS];

/* A weapon tapped on the second screen, until a Lua thread takes it. Past the
 * limit it is dropped: a tap must never equip seconds later, mid-scene. */
const uint64_t EQUIP_LIFE_US=1000000;
int s_equip=-1;
uint64_t s_equip_at;

/* Where the player stands, for the second screen's map: the words of x, y (up),
 * z and the facing's x and z, or s_hero_ok false. */
uint32_t s_hero[5];
bool s_hero_ok;

uint32_t guest_word(uint32_t address)
{
    // Only called from the guest's cellPad path after guest presentation began.
    // These are existing v97 globals, not guessed object/weapon offsets.
    const uint8_t* p=vm_base+address;
    return (uint32_t)p[0]<<24 | (uint32_t)p[1]<<16 | (uint32_t)p[2]<<8 | p[3];
}

/* Guest memory a pointer read from the game leads to: user memory has unmapped
 * holes, and a stale pointer between levels must not fault the game. The kernel
 * copies, and says EFAULT instead of a signal. */
bool guest_read(uint32_t address, void* out, size_t n)
{
    iovec local={out,n}, remote={vm_base+address,n};
    return process_vm_readv(getpid(),&local,1,&remote,1,0)==(ssize_t)n;
}
uint32_t be32(const uint8_t* p) { return (uint32_t)p[0]<<24 | (uint32_t)p[1]<<16 | (uint32_t)p[2]<<8 | p[3]; }

/* The player's entity, as get_hero (func_0021B3A0 -> func_0016ACA4 -> func_001E1964)
 * finds it: the players array at [0xEE7500]; player 0's handle at +8+0x20+slot*8
 * (slot at +8+0x4C), its low 16 bits indexing the entity table at [0xF5CD00]
 * (0x180 bytes each, count at +0x18), the entity's +0x44 the handle's low word
 * again. The entity starts with its 4x4 world matrix: forward at +0x20, position
 * at +0x30, y up. Every pointer is checked: between levels they are stale. */
bool hero_read(uint32_t out[5])
{
    uint8_t b[0x48];
    const uint32_t players=guest_word(0x00EE7500u);
    if (!players || players>=0x40000000u-0x100 || !guest_read(players+8+0x4C,b,4)) return false;
    const uint32_t slot=be32(b);
    if (slot>=16 || !guest_read(players+8+0x20+slot*8+4,b,4)) return false;
    const uint32_t handle=be32(b);
    const uint32_t table=guest_word(0x00F5CD00u), count=guest_word(0x00F5CD00u+0x18);
    if (!table || table>=0x40000000u || count>0x10000 || (handle&0xFFFF)>=count) return false;
    const uint32_t entity=table+(handle&0xFFFF)*0x180;
    if (entity>=0x40000000u-0x180 || !guest_read(entity,b,0x48) || be32(b+0x44)!=handle) return false;
    out[0]=be32(b+0x30); out[1]=be32(b+0x34); out[2]=be32(b+0x38);   /* position */
    out[3]=be32(b+0x20); out[4]=be32(b+0x28);                        /* forward, x and z */
    return true;
}

unsigned long long now_us()
{
    timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return (unsigned long long)ts.tv_sec * 1000000ull + (unsigned long long)ts.tv_nsec / 1000ull;
}

}  // namespace

/* ---- performance hints (ADPF, Android 13+) --------------------------------
 * main.cpp's log pump names the host threads as they announce themselves and
 * passes the busy ones here. Once the game has run a while, they go into one
 * APerformanceHint session with a 16.7 ms target, and each frame (a new guest
 * flip, seen from the pad poll) reports how long that frame took. Loaded with
 * dlsym: API 29-32 devices can run this APK without these newer APIs. */
namespace {
struct APerformanceHintManager; struct APerformanceHintSession;
typedef APerformanceHintManager* (*hint_mgr_fn)(void);
typedef APerformanceHintSession* (*hint_create_fn)(APerformanceHintManager*, const int32_t*, size_t, int64_t);
typedef int (*hint_report_fn)(APerformanceHintSession*, int64_t);
int32_t s_hint_tids[16]; int s_hint_n;   /* filled by the log pump before the session exists */
int s_main_tid;                          /* the PPU main thread's, once announced */
pthread_mutex_t s_metadata_mutex=PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t s_hint_tick_mutex=PTHREAD_MUTEX_INITIALIZER;
struct MetadataGuard { MetadataGuard() { pthread_mutex_lock(&s_metadata_mutex); }
                       ~MetadataGuard() { pthread_mutex_unlock(&s_metadata_mutex); } };
struct HintTickGuard { bool locked; HintTickGuard() : locked(pthread_mutex_trylock(&s_hint_tick_mutex)==0) {}
                       ~HintTickGuard() { if (locked) pthread_mutex_unlock(&s_hint_tick_mutex); } };
bool s_hint_frozen;
APerformanceHintSession* s_hint;
hint_report_fn s_hint_report;
int s_hint_failed;

void hint_tick()
{
    HintTickGuard guard;
    if (!guard.locked) return; // another pad thread is already reporting this frame
    static unsigned s_flips; static unsigned long long s_at; static unsigned s_frames;
    if (s_hint_failed) return;
    const unsigned f = cellGcm_flip_request_count();
    if (f == s_flips) return;
    const unsigned long long t = now_us();
    const unsigned long long dt = s_at ? t - s_at : 0;
    s_flips = f; s_at = t; s_frames++;
    if (!s_hint) {
        if (s_frames < 600) return;   /* ~10 s in: the threads are up */
        int32_t tids[16]; int count;
        {
            MetadataGuard metadata;
            count=s_hint_n;
            if (count<2) return;
            memcpy(tids,s_hint_tids,(size_t)count*sizeof(tids[0]));
            s_hint_frozen=true;
        }
        const char* e = getenv("RACACIT_PERF_HINTS");
        if (e && e[0] == '0') { s_hint_failed = 1; return; }
        void* lib = dlopen("libandroid.so", RTLD_NOW);
        hint_mgr_fn mgr = lib ? (hint_mgr_fn)dlsym(lib, "APerformanceHint_getManager") : nullptr;
        hint_create_fn create = lib ? (hint_create_fn)dlsym(lib, "APerformanceHint_createSession") : nullptr;
        s_hint_report = lib ? (hint_report_fn)dlsym(lib, "APerformanceHint_reportActualWorkDuration") : nullptr;
        APerformanceHintManager* m = mgr ? mgr() : nullptr;
        s_hint = (m && create && s_hint_report) ? create(m, tids, (size_t)count, 16666667) : nullptr;
        fprintf(stderr, "[android] performance hints: %s (%d threads)%c",
                s_hint ? "on" : "not available", count, 10);
        if (!s_hint) { s_hint_failed = 1; return; }
    }
    if (dt > 0 && dt < 1000000) s_hint_report(s_hint, (int64_t)dt * 1000);
}
}  // namespace

/* Every named host thread's name, kept here too: Android renames a thread
 * "Thread-N" when it first attaches to Java (SDL does, on the PPU main and
 * RSX threads), so the profiler asks here first (racacit_thread_label). */
namespace { struct Label { int tid; char name[16]; }; Label s_labels[256]; int s_nlabels; }

extern "C" const char* racacit_thread_label(int tid)
{
    const int count=__atomic_load_n(&s_nlabels,__ATOMIC_ACQUIRE);
    for (int i=count-1;i>=0;i--) if (s_labels[i].tid==tid) return s_labels[i].name;
    return nullptr;
}

/* main.cpp, as host threads are named: kept, and the busy ones go into the hint session. */
extern "C" void racacit_perf_thread(unsigned long tid, const char* name)
{
    if (!tid || !name) return;
    MetadataGuard metadata;
    const int count=__atomic_load_n(&s_nlabels,__ATOMIC_RELAXED);
    bool duplicate=false;
    for (int i=0;i<count;i++) if (s_labels[i].tid==(int)tid && !strncmp(s_labels[i].name,name,15)) duplicate=true;
    if (!duplicate && count<256) {
        Label& l=s_labels[count];
        l.tid=(int)tid; snprintf(l.name,sizeof l.name,"%s",name);
        // Entries are immutable once published; profiler lookup never locks.
        __atomic_store_n(&s_nlabels,count+1,__ATOMIC_RELEASE);
    }
    if (!strncmp(name, "PPU main", 8)) __atomic_store_n(&s_main_tid, (int)tid, __ATOMIC_RELAXED);
    if (s_hint_frozen || s_hint_n>=16) return;
    for (int i=0;i<s_hint_n;i++) if (s_hint_tids[i]==(int32_t)tid) return;
    if (!strncmp(name, "PPU main", 8) || !strncmp(name, "rsx", 3) || !strncmp(name, "spu w", 5) ||
        !strncmp(name, "spurs kernel", 12))
        s_hint_tids[s_hint_n++] = (int32_t)tid;
}

/* ---- the host UI's font --------------------------------------------------
 * rsx_host_ui.c (font_build_android) asks for the 95 printable ASCII glyphs at
 * `px`: HostFont.build draws them into an alpha atlas and fills meta with the
 * atlas size, line height, ascent and each glyph's x, y, w, h, xoff, advance. */
extern "C" int racacit_android_font(int px, int* aw, int* ah, int* line_h, int* ascent,
                                    short glyph[95][6], unsigned char** pixels)
{
    JNIEnv* env = (JNIEnv*)SDL_AndroidGetJNIEnv();
    if (!env) return -1;
    static jclass s_cls;
    if (!s_cls) {
        jobject act = (jobject)SDL_AndroidGetActivity();
        if (!act) return -1;
        jclass ac = env->GetObjectClass(act);
        jobject loader = env->CallObjectMethod(act, env->GetMethodID(ac, "getClassLoader", "()Ljava/lang/ClassLoader;"));
        jclass lc = env->FindClass("java/lang/ClassLoader");
        jstring name = env->NewStringUTF("com.racacit.recomp.HostFont");
        jobject cls = loader ? env->CallObjectMethod(loader, env->GetMethodID(lc, "loadClass", "(Ljava/lang/String;)Ljava/lang/Class;"), name) : nullptr;
        if (env->ExceptionCheck()) { env->ExceptionClear(); cls = nullptr; }
        if (cls) s_cls = (jclass)env->NewGlobalRef(cls);
        env->DeleteLocalRef(name); env->DeleteLocalRef(lc); env->DeleteLocalRef(ac); env->DeleteLocalRef(act);
        if (loader) env->DeleteLocalRef(loader);
        if (cls) env->DeleteLocalRef(cls);
        if (!s_cls) return -1;
    }
    jmethodID build = env->GetStaticMethodID(s_cls, "build", "(I[I)[B");
    if (!build) { env->ExceptionClear(); return -1; }
    jintArray meta = env->NewIntArray(4 + 95 * 6);
    jbyteArray atlas = (jbyteArray)env->CallStaticObjectMethod(s_cls, build, (jint)px, meta);
    if (env->ExceptionCheck() || !atlas) { env->ExceptionClear(); env->DeleteLocalRef(meta); return -1; }
    jint m[4 + 95 * 6];
    env->GetIntArrayRegion(meta, 0, 4 + 95 * 6, m);
    const jsize n = env->GetArrayLength(atlas);
    int rc = -1;
    if (m[0] > 0 && m[1] > 0 && n == m[0] * m[1]) {
        *pixels = (unsigned char*)malloc((size_t)n);
        if (*pixels) {
            env->GetByteArrayRegion(atlas, 0, n, (jbyte*)*pixels);
            *aw = m[0]; *ah = m[1]; *line_h = m[2]; *ascent = m[3];
            for (int c = 0; c < 95; c++) for (int k = 0; k < 6; k++) glyph[c][k] = (short)m[4 + c * 6 + k];
            rc = 0;
        }
    }
    env->DeleteLocalRef(atlas); env->DeleteLocalRef(meta);
    return rc;
}

/* stubs.cpp, on a Lua thread: the weapon to equip now, or -1. Read on every
 * script call, so the empty case takes no lock. */
extern "C" int racacit_companion_take_equip(void)
{
    if (__atomic_load_n(&s_equip,__ATOMIC_RELAXED)<0) return -1;
    InputGuard guard;
    const int weapon=s_equip; s_equip=-1;
    return now_us()-s_equip_at<=EQUIP_LIFE_US ? weapon : -1;
}

/* Guest pad thread owns all guest reads; Java sees a copied snapshot only. */
/* v121-android.61: the level whose archive was opened last (ppu_fs.cpp), by its folder: the ids
 * of the word at 0x00EF7C90 (LevelMap.java FOLDERS). The owner's .58 session: back on Nefarious
 * Station from Space Sector 4, the wheel stayed empty until the next level -- the word may lag a
 * space-to-planet trip. The folder wins when it names a level; 0 = front end or unknown. */
static int s_file_level;
static bool space_word(int level) { return level==3 || level==6 || level==10 || level==15 || level==18; }
extern "C" void racacit_level_file_opened(const char* path)
{
    static const char* const kFolders[] = { "", "great_clock_a", "zolar_forest", "space_sector_1", "galacton_ship",
        "great_clock_b", "space_sector_2", "molonoth", "axiom_city", "great_clock_c", "space_sector_3", "krell_canyon",
        "agorian_arena", "tombli", "great_clock_d", "space_sector_4", "valkyrie_fleet", "nefarious_station",
        "space_sector_5", "gimlick_valley", "great_clock_e", "insomniac_museum" };
    const char* p = strstr(path, "/packed/levels/");
    if (!p) return;
    p += 15;
    const char* e = strchr(p, '/');
    if (!e) return;
    const size_t n = (size_t)(e - p);
    int id = 0;
    for (int i = 1; i < (int)(sizeof kFolders / sizeof kFolders[0]); i++)
        if (strlen(kFolders[i]) == n && !strncmp(kFolders[i], p, n)) { id = i; break; }
    __atomic_store_n(&s_file_level, id, __ATOMIC_RELAXED);
}

extern "C" int racacit_companion_step(unsigned* mask, int* sticks, unsigned char ax[4],
                                      unsigned physical_mask, const unsigned char physical_ax[4], int modal)
{
    hint_tick();
    const unsigned presented=ppu_boot_frames_presented();
    const bool ready=vm_base && presented>0;
    int level=-1, weapon=-1, slots[QS_SLOTS];
    memset(slots,0xFF,sizeof slots);
    uint32_t hero[5]={}; bool hero_ok=false;
    if (ready) {
        const uint32_t l=guest_word(0x00EF7C90u); // discord_rpc.cpp/current_level; func_0029A4F4
        if (l>0 && l<=22) level=(int)l;
        const int fl=__atomic_load_n(&s_file_level, __ATOMIC_RELAXED);
        { static int said_w=-2, said_f=-2;   /* every change of either, for the next play test's log */
          if ((int)l!=said_w || fl!=said_f) {
              said_w=(int)l; said_f=fl;
              fprintf(stderr,"[companion] level word %d, level files %d%s%c",(int)l,fl,
                      space_word(level) && CompanionInput::ratchet_level(fl) ? " (files win)" : "",10);
          } }
        /* The game opens the NEXT level's archive ahead of time (Vorselon's ship opened Space
         * Sector 1's while Ratchet stood on the ship), so the files only win where the word
         * says space and the files name a Ratchet level -- the stale case of the owner's trip. */
        if (space_word(level) && CompanionInput::ratchet_level(fl)) level=fl;
        if (level>0 && level<22) hero_ok=hero_read(hero);
        if (CompanionInput::ratchet_level(level)) {
            const uint32_t w=guest_word(0x00E24E78u); // stubs.cpp/racacit_dsw_event
            if (w<256u) weapon=(int)w;
            for (int i=0;i<QS_SLOTS;i++) {
                const uint32_t id=guest_word(QS_TABLE+(uint32_t)i*4);
                if (id<=(uint32_t)WPN_LAST) slots[i]=(int)id;
            }
        }
    }
    bool physical=physical_mask!=0;
    for (unsigned i=0;i<4;i++) if (physical_ax[i]<104 || physical_ax[i]>152) physical=true;
    InputGuard guard;
    const uint64_t now=now_us(); // timestamp after acquiring ownership; cannot predate a newer UI command
    const unsigned movies=g_racacit_movie_frames;
    if (movies!=s_movie_frames) { s_movie_frames=movies; s_movie_at=now; }
    CompanionContext context = {cellGcm_flip_request_count(),g_racacit_level_changes,now,level,
        ready,modal!=0,physical,s_movie_at && now-s_movie_at<500000};
    const CompanionOutput out=s_input.tick(context);
    s_presented=presented; s_weapon=weapon;
    memcpy(s_hero,hero,sizeof hero); s_hero_ok=hero_ok;
    if (memcmp(s_slots,slots,sizeof slots)) {
        memcpy(s_slots,slots,sizeof slots);
        char line[QS_SLOTS*4+32]; int n=snprintf(line,sizeof line,"[companion] quick select:");
        for (int i=0;i<QS_SLOTS;i++) n+=snprintf(line+n,sizeof line-(size_t)n," %d",slots[i]);
        fprintf(stderr,"%s%c",line,10);
    }
    *mask=out.mask; *sticks=out.sticks; memcpy(ax,out.ax,4);
    return out.mask || out.sticks;
}

extern "C" void rsx_vulkan_prepare_progress(int out[5]);

extern "C" {

/* The shader prepare's progress (RACACIT_PREPARE): state (0 starting, 1 reading,
 * 2 building, 3 done, 4 failed), handled, total, failed, without programs. */
JNIEXPORT jintArray JNICALL Java_com_racacit_recomp_NativeBridge_prepareProgress(JNIEnv* env, jclass)
{
    int v[5];
    rsx_vulkan_prepare_progress(v);
    jintArray a = env->NewIntArray(5);
    if (a) env->SetIntArrayRegion(a, 0, 5, (const jint*)v);
    return a;
}

JNIEXPORT jint JNICALL Java_com_racacit_recomp_NativeBridge_press(JNIEnv*, jclass, jint mask, jint frames)
{
    InputGuard guard; const uint64_t now=now_us();
    return s_input.press((unsigned)mask,(unsigned)frames,now);
}

/* A gameplay button (d-pad, R2) down or up, held with the controller (CompanionInput::hold). */
JNIEXPORT jint JNICALL Java_com_racacit_recomp_NativeBridge_hold(JNIEnv*, jclass, jint mask, jboolean down)
{
    InputGuard guard; return s_input.hold((unsigned)mask,down!=0,now_us());
}

/* Quick select slot 0-7, clockwise from the top: TRIANGLE opens the ring,
 * the left stick points at the slot, letting go equips it. */
JNIEXPORT jint JNICALL Java_com_racacit_recomp_NativeBridge_quickSelect(JNIEnv*, jclass, jint slot)
{
    InputGuard guard; return s_input.select(slot,now_us());
}
JNIEXPORT jint JNICALL Java_com_racacit_recomp_NativeBridge_beginQuickSelect(JNIEnv*, jclass)
{
    InputGuard guard; return s_input.begin(now_us());
}
JNIEXPORT jint JNICALL Java_com_racacit_recomp_NativeBridge_updateQuickSelect(JNIEnv*, jclass, jfloat x, jfloat y)
{
    if (!isfinite(x) || !isfinite(y) || x < -1 || x > 1 || y < -1 || y > 1) return CompanionInput::INVALID;
    int sector=-1;
    if (x*x+y*y>=0.0625f) {
        const double angle=atan2((double)x,-(double)y);
        sector=((int)floor(angle*4.0/3.14159265358979323846+0.5)+8)%8;
    }
    InputGuard guard; return s_input.update_direction(sector,now_us());
}
JNIEXPORT jint JNICALL Java_com_racacit_recomp_NativeBridge_endQuickSelect(JNIEnv*, jclass, jboolean commit)
{
    InputGuard guard; return s_input.end(commit!=0,now_us());
}
JNIEXPORT jint JNICALL Java_com_racacit_recomp_NativeBridge_pageQuickSelect(JNIEnv*, jclass, jint direction)
{
    InputGuard guard; return s_input.change_page(direction,now_us());
}
/* Equip a weapon that is on the quick select: the game's own hero_equip, run by
 * stubs.cpp on a Lua thread. Not refused while the controller is held, as the
 * wheel is: tapping with a thumb on the stick is the normal way to use this. */
JNIEXPORT jint JNICALL Java_com_racacit_recomp_NativeBridge_equip(JNIEnv*, jclass, jint weapon)
{
    InputGuard guard; const uint64_t now=now_us();
    if (!s_input.enabled || !s_input.sampled || !s_input.context.ready) return CompanionInput::NOT_READY;
    if (!s_input.fresh(now)) return CompanionInput::STALLED;
    const CompanionContext& c=s_input.context;
    if (c.modal || c.movie || !CompanionInput::ratchet_level(c.level)) return CompanionInput::CONTEXT;
    for (int i=0;i<QS_SLOTS;i++) if (weapon>=0 && s_slots[i]==weapon) {
        s_equip_at=now;
        __atomic_store_n(&s_equip,(int)weapon,__ATOMIC_RELAXED);
        return CompanionInput::OK;
    }
    return CompanionInput::INVALID;
}
JNIEXPORT void JNICALL Java_com_racacit_recomp_NativeBridge_cancel(JNIEnv*, jclass)
{
    InputGuard guard; s_input.cancel(now_us());
}
JNIEXPORT void JNICALL Java_com_racacit_recomp_NativeBridge_setActive(JNIEnv*, jclass, jboolean active)
{
    InputGuard guard; s_input.set_active(active!=0);
}

/* Open or close the in-game Settings menu (rsx_host_ui), as F1 does. */
JNIEXPORT void JNICALL Java_com_racacit_recomp_NativeBridge_openMenu(JNIEnv*, jclass)
{
    { InputGuard guard; s_input.finish(CompanionInput::CANCELLED); }
    SDL_Event ev; memset(&ev, 0, sizeof ev);
    ev.type = SDL_KEYDOWN; ev.key.state = SDL_PRESSED; ev.key.keysym.sym = SDLK_F1; ev.key.keysym.scancode = SDL_SCANCODE_F1;
    SDL_PushEvent(&ev);
    ev.type = SDL_KEYUP; ev.key.state = SDL_RELEASED;
    SDL_PushEvent(&ev);
}

/* Snapshot contract lives in NativeBridge.java. Unknown inventory stays unknown. */
JNIEXPORT jlongArray JNICALL Java_com_racacit_recomp_NativeBridge_stats(JNIEnv* env, jclass)
{
    /* then QUICK_SLOTS, then HERO: 1 when known, and the five float words of s_hero */
    /* then MAIN_WAIT: the PPU main thread's time in its SPU job waits (us, running total),
     * and its thread id (0 until announced) */
    /* then AB: the core swap A/B (main.cpp g_racacit_ab): on, current mode, phases and fps x100
     * with it off, then with it on */
    const int HERO=13+QS_SLOTS, MAIN_WAIT=HERO+6, AB=MAIN_WAIT+2, N=AB+6;
    jlong v[N]={0,0,0,-1,0,-1,0,0,0,-1,0,-1,-1};
    for (int i=0;i<QS_SLOTS;i++) v[13+i]=-1;
    {
        InputGuard guard;
        const uint64_t now=now_us(); v[2]=(jlong)now;
        const CompanionContext& c=s_input.context;
        const bool fresh=s_input.fresh(now);
        v[0]=c.frame; v[1]=s_presented; v[3]=fresh?s_weapon:-1;
        v[4]=(s_input.enabled?1:0)|(fresh?2:0)|(c.modal?4:0)|
            (CompanionInput::ratchet_level(c.level)?8:0)|(c.physical?16:0)|(c.movie?32:0)|(c.ready?64:0);
        v[5]=fresh?c.level:-1; v[6]=s_input.phase; v[7]=s_input.status; v[8]=s_input.generation;
        v[9]=s_input.sampled?(jlong)(now-c.now_us):-1;
        v[10]=s_input.phase!=CompanionInput::IDLE?(uint32_t)(c.frame-s_input.start_frame):0;
        if (fresh) for (int i=0;i<QS_SLOTS;i++) v[13+i]=s_slots[i];
        v[HERO]=fresh && s_hero_ok;
        for (int i=0;i<5;i++) v[HERO+1+i]=s_hero[i];
        v[MAIN_WAIT]=(jlong)(__atomic_load_n(&g_racacit_main_wait_ticks, __ATOMIC_RELAXED) * 10u / 798u);
        v[MAIN_WAIT+1]=__atomic_load_n(&s_main_tid, __ATOMIC_RELAXED);
        for (int i=0;i<6;i++) v[AB+i]=g_racacit_ab[i];
    }
    jlongArray out = env->NewLongArray(N);
    if (out) env->SetLongArrayRegion(out, 0, N, v);
    return out;
}

}  // extern "C"
#endif
