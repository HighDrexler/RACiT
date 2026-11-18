/* DualSense Plus for Into the Nexus: which game sounds the controller's speaker
 * and haptics play, and the weapon events for the adaptive triggers.
 *
 * Ported from RACiT (A Crack in Time) game/stubs.cpp, 2026-11-11. The player
 * itself (tools/ps3recomp/libs/audio/ds_player.c), the dialogue lines
 * (runtime/ppu/ppu_fs.cpp), the triggers, light bar and motion
 * (libs/input/cellPad.c) and the on/off switch (RACACIT_DUALSENSE_PLUS=1,
 * rsx_host_ui.c) are the shared runtime's and came over unchanged; this file is
 * the game-specific part, on Into the Nexus's own hook points
 * (scripts/patch_lift_nexus.py "ds-*"):
 *
 *   ds-start   the engine's only call to SCREAM's play-a-bank-sound
 *              (func_00A81FD0, ACiT func_00975E60), at 0x006C707C inside the
 *              sound manager's update (func_006C2410 and the three other lifted
 *              copies of that code): bank r3, sound r4 before the call, the
 *              SCREAM handle in r3 after it. ACiT had a per-instance start
 *              (func_006DDC90) with the name at [[instance+0xC]]; Nexus's
 *              engine inlined it, and every sound is a bank of its own
 *              (/built/sound/bank/xx/<id>/resident_sound.dat, IGHW: section
 *              0x21200 = the name at +0x100, the SCREAM bank "klBS" at +0x230),
 *              loaded whole -- so the name is at bank - 0x130 on every path
 *              (bank_name). r26 holds it too, but only on the path that just
 *              looked the sections up (runs 87-89: many starts came with r26
 *              0 or 0xEC).
 *   ds-stop    SCREAM's stop (func_00A81C00, ACiT func_00974DF0) and release
 *              (func_00A81988, ACiT func_00973EB0), r3 = the handle.
 *   ds-pause   SCREAM's stream pause callback (func_00A9F5D8, ACiT
 *              func_0099B4E0): r3 the stream, r4 paused.
 * Found with scripts/dev/fn_match.py (ACiT code matched in the Nexus EBOOT)
 * and one probe run (run 84). SCREAM's globals: the word at TOC - 0x792C
 * (ACiT TOC - 0x48D8), from its handle check func_00A8C0A8 (ACiT
 * func_00982858, a 100% match), which ds_player.c's scream_alive copies.
 *
 * Every sound is keyed by its SCREAM handle (ACiT keyed by the instance).
 *
 * Dialogue lines are decided here (racacit_ds_line_decide) and played by
 * ppu_fs.cpp from the line's own bank. The Fire-on-R2 prompts are in
 * game/button_prompts.cpp.
 *
 * Not ported (need Into the Nexus addresses, or are A Crack in Time's own):
 * the equipped weapon id (ACiT 0x00E24E78; here a weapon's trigger profile
 * follows the family of the last shot fired), RACiT's level-change and Lua
 * stop_vo hooks (a line here follows the game's reads of its file and ends
 * with them), the hoverboots' L2, the Groovitron's music.
 */
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>

extern "C" uint8_t* vm_base;
extern "C" unsigned long long ps3_qpc_us(void);

extern "C" int  ds_player_enabled(void);
extern "C" void ds_player_scream(uint32_t bank_ea, uint32_t sound, float spk, float felt, uint32_t owner, const char* name);
extern "C" void ds_player_stop(uint32_t owner);
extern "C" void ds_player_release(uint32_t owner);
extern "C" void ds_player_bind(uint32_t owner, uint32_t handle, uint32_t snd_globals);
extern "C" void ds_player_stream_pause(uint32_t stream, int on);
extern "C" void racacit_ds_stream_expect(uint32_t owner, float level, const char* name);   /* ppu_fs.cpp */
extern "C" volatile unsigned long long g_racacit_fire_ms;   /* cellPad.c: fire (R1) last pressed (ms) */
extern "C" volatile int g_racacit_fire_held;                /* ...and whether it is down now */
extern "C" volatile int g_racacit_ds_pad;                   /* cellPad.c: a DualSense with weapon triggers */
extern "C" void racacit_ds_weapon_profile(int p);
extern "C" void racacit_ds_weapon_shot(void);
extern "C" void racacit_ds_weapon_empty(void);

namespace {

const uint32_t kToc = 0x00D8EE08u;              /* Into the Nexus's TOC (r2) */
const uint32_t kScreamGlobals = kToc - 0x792Cu; /* SCREAM's globals pointer */

uint32_t rd32(uint32_t a)
{
    if (a < 0x10000u || a >= 0x40000000u) return 0;
    const uint8_t* p = vm_base + a;
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

/* The sound's name: a C string in guest memory, printable, 1..63 chars. */
int read_name(uint32_t ea, char* out, int cap)
{
    out[0] = 0;
    if (ea < 0x10000u || ea >= 0x40000000u) return 0;
    int n = 0;
    while (n < cap - 1) {
        const uint8_t c = vm_base[ea + n];
        if (!c) break;
        if (c < 0x20 || c > 0x7E) { out[0] = 0; return 0; }
        out[n++] = (char)c;
    }
    out[n] = 0;
    return n > 0 && n < cap - 1;
}

/* The name of the sound a SCREAM bank belongs to: the bank sits at +0x230 of
 * its resident_sound.dat in memory (IGHW; sections 0x21300 at +0x80, 0x21200 =
 * the 64-byte name at +0x100, 0x21000 at +0x180 in every bank looked at), so
 * the IGHW header is looked for behind the bank and its 0x21200 section read. */
int bank_name(uint32_t bank, char* out, int cap)
{
    out[0] = 0;
    for (uint32_t back = 0x230u; back <= 0x800u; back += 0x10u) {
        const uint32_t base = bank - back;
        if (base < 0x10000u || rd32(base) != 0x49474857u) continue;   /* "IGHW" */
        const uint32_t n = rd32(base + 8);
        if (!n || n > 16u) continue;
        for (uint32_t i = 0; i < n; i++)
            if (rd32(base + 0x20u + i * 16u) == 0x21200u) {
                const uint32_t off = rd32(base + 0x24u + i * 16u);
                return off < back ? read_name(base + off, out, cap) : 0;
            }
        return 0;
    }
    return 0;
}

/* Comma list of substrings; ^ anchors one to the start. */
int match(const char* name, const char* list)
{
    char tok[64];
    for (const char* p = list; *p; ) {
        const char* q = strchr(p, ',');
        size_t n = q ? (size_t)(q - p) : strlen(p);
        if (n && n < sizeof tok) {
            memcpy(tok, p, n); tok[n] = 0;
            if (tok[0] == '^' ? !strncmp(name, tok + 1, n - 1) : strstr(name, tok) != nullptr) return 1;
        }
        if (!q) break;
        p = q + 1;
    }
    return 0;
}

/* Which sounds the controller's speaker carries (RACACIT_DS_ROUTE /
 * RACACIT_DS_SKIP). The defaults are RACiT's (the owner's choices for A Crack
 * in Time: the player's weapons and their projectiles, Mr. Zurkon, the Zoni,
 * the ship's guns, the weapon level-up popup; not enemies' weapons, idle hums,
 * the wrench) -- Insomniac's naming is shared, so they mostly hold
 * (wep_omniblaster_fire01, wep_blaster_projectile_hitsurface01,
 * wep_buzzblades_equip_dko on the prison ship); "whizby" is Nexus's "flyby".
 * The names Into the Nexus really plays are in the log with RACACIT_DS_NAMES=1. */
int classify(const char* name)
{
    static const char* route = nullptr; static const char* skip = nullptr;
    if (!route) { const char* e = getenv("RACACIT_DS_ROUTE");
                  route = e ? e : "^wep_,^mag_net_,^sub_new_,^proj_pistol_,^proj_ryno_,^proj_rift_,^proj_dynamo_,"
                                  "^proj_buzz_blade_,^proj_xbox_,zoni,^veh_aphelion_fire,^veh_aphelion_missilefire,"
                                  "^veh_aphelion_kinetic,^veh_aphelion_swarm,^veh_aphelion_plasma,helpdesk,"
                                  "^ui_upgrade_"; }
    if (!skip) { const char* e = getenv("RACACIT_DS_SKIP");
                 /* zurkon_: the three Zurkons' guns and gadget noises (wep_mrzurkon_*,
                  * wep_momzurkon_*, wep_jrzurkon_*) stay on the TV -- owner 2026-11-17,
                  * as in A Crack in Time; their VOICES go to the controller (line_route). */
                 skip = e ? e : "wep_turret_,wep_krell,wep_pirateturret_,wep_vis_,wep_nef_,wep_thrown_,mrzurkon_hover,zurkon_,"
                                 "handloop,activeloop,lavaloop,beeploop,motorloop,windloop,alarm_clock,"
                                 "wep_wrench_,wep_clank_,damage,death,idle,flyby,whizby,ricochet,debris,"
                                 "proximity_beep,proximitylatch,zoom_amb,_amb_,tracking,connector,"
                                 "supsoakalive,force_field"; }
    return match(name, route) && !match(name, skip) ? 1 : 0;
}

/* A routed sound's speaker level: one ceiling per kind (RACiT v69; ds_player.c
 * holds each sample to a fixed loudness first). RACACIT_DS_LEVEL_SCALE. */
float level(const char* name)
{
    static float scale = -1.0f;
    if (scale < 0.0f) { const char* e = getenv("RACACIT_DS_LEVEL_SCALE"); scale = e ? (float)atof(e) : 1.0f; }
    static const struct { const char* k; float lv; } t[] = {
        { "zurk", 0.70f }, { "ryno", 0.30f },
        { "beep", 0.15f }, { "latch", 0.15f }, { "equip", 0.15f }, { "_arm", 0.15f }, { "click", 0.15f },
        { "reload", 0.15f }, { "emptied", 0.15f }, { "^wep_gen_", 0.15f }, { "helpdesk", 0.25f },
        { "zoni", 0.60f }, { "^ui_upgrade_", 0.60f },
        { "explo", 0.45f }, { "impact", 0.45f }, { "blast", 0.45f }, { "shockwave", 0.45f }, { "_hit", 0.45f },
    };
    for (unsigned i = 0; i < sizeof t / sizeof t[0]; i++)
        if (match(name, t[i].k)) return t[i].lv * scale;
    return 0.55f * scale;                                        /* a gun's shot */
}

/* What the actuators play (RACiT v57/v61): the player's guns and projectiles,
 * impacts, explosions, landings and hits -- never characters' or enemy
 * projectiles' sounds. RACACIT_DS_HAPTIC_ROUTE / RACACIT_DS_HAPTIC_SKIP. */
int haptic(const char* name)
{
    static const char* route = nullptr; static const char* skip = nullptr;
    if (!route) { const char* e = getenv("RACACIT_DS_HAPTIC_ROUTE");
                  route = e ? e : "^wep_,^mag_net_,^sub_new_,^proj_,^veh_aphelion_fire,^veh_aphelion_missile,^veh_aphelion_kinetic,"
                                  "^veh_aphelion_swarm,^veh_aphelion_plasma,explo,impact,_hit,hit_,land,crash,smash,break"; }
    if (!skip) { const char* e = getenv("RACACIT_DS_HAPTIC_SKIP");
                 skip = e ? e : "wep_turret_,wep_krell,wep_pirateturret_,wep_vis_,wep_nef_,wep_thrown_,mrzurkon_hover,"
                                 "mrzurkon_fire,jrzurkon_fire,momzurkon_fire,"   /* the Zurkons shoot on their own */
                                 "loop,idle,flyby,whizby,ricochet,equip,emptied,wep_gen_,wep_wrench_whoosh,_vox,vocal,ui_,^cha_,^hca_,"
                                 "zoom_amb,_amb_,tracking,connector,proximity_beep,proximitylatch,supsoakalive"; }
    return match(name, route) && !match(name, skip) ? 1 : 0;
}

/* Weapon-aware triggers (cellPad.c). ACiT kept a family per equipped weapon id; Into the
 * Nexus's id is not found yet, so the profile follows the weapon's own sounds: its equip
 * sound when the player switches to it (most Nexus weapons have one), else its first shot.
 * The table is Into the Nexus's arsenal first (its sound names, 2026-11-17: 2,120 banks
 * listed), then A Crack in Time's names that Nexus still uses; first match wins. */
enum { DSW_MEDIUM, DSW_LIGHT, DSW_HEAVY, DSW_THROW, DSW_BEAM, DSW_DEPLOY };
int weapon_family(const char* n)
{
    static const struct { const char* k; int f; } t[] = {
        /* Into the Nexus */
        { "omniblaster", DSW_LIGHT },          /* the starting blaster: crisp click */
        { "netherblades", DSW_HEAVY },         /* Netherblaster: a shotgun */
        { "holidayicethrower", DSW_BEAM },     /* Winterizer: a stream (not a throw) */
        { "frostray", DSW_BEAM }, { "flamethrower", DSW_BEAM }, { "teslarod", DSW_BEAM },
        { "critterstrike", DSW_BEAM }, { "gravitywhip", DSW_BEAM },
        { "netherbeast", DSW_DEPLOY }, { "nightmarebox", DSW_DEPLOY }, { "thunderstrike", DSW_DEPLOY },
        { "ratachetdecoy", DSW_DEPLOY }, { "jrzurkon", DSW_DEPLOY }, { "momzurkon", DSW_DEPLOY },
        { "rocklaunch", DSW_HEAVY },           /* Warmonger */
        { "xbow", DSW_HEAVY },                 /* the scoped rifle: one hard snap */
        { "repulsor", DSW_MEDIUM }, { "buzzblades", DSW_MEDIUM }, { "blitzer", DSW_HEAVY },
        { "fusiongrenade", DSW_THROW }, { "grav_bomb", DSW_THROW }, { "tesla", DSW_THROW },
        /* A Crack in Time's */
        { "zurkon", DSW_DEPLOY }, { "groovitron", DSW_DEPLOY }, { "chimp", DSW_DEPLOY }, { "dynamo", DSW_DEPLOY }, { "turret", DSW_DEPLOY },
        { "spiral", DSW_BEAM }, { "beam", DSW_BEAM }, { "torch", DSW_BEAM }, { "flame", DSW_BEAM }, { "cryo", DSW_BEAM },
        { "bomb", DSW_THROW }, { "grenade", DSW_THROW }, { "mine", DSW_THROW }, { "spike", DSW_THROW }, { "throw", DSW_THROW },
        { "shotgun", DSW_HEAVY }, { "eruptor", DSW_HEAVY }, { "sonic", DSW_HEAVY }, { "sub_new", DSW_HEAVY }, { "ryno", DSW_HEAVY }, { "launch", DSW_HEAVY },
        { "cannon", DSW_HEAVY }, { "rift", DSW_HEAVY }, { "magnet", DSW_HEAVY }, { "mag_", DSW_HEAVY },
        { "pistol", DSW_LIGHT }, { "blaster", DSW_LIGHT }, { "combust", DSW_LIGHT }, { "rifle", DSW_LIGHT }, { "striker", DSW_LIGHT },
        { "plasma", DSW_LIGHT }, { "laser", DSW_LIGHT }, { "gun", DSW_LIGHT } };
    for (unsigned i = 0; i < sizeof t / sizeof t[0]; i++) if (strstr(n, t[i].k)) return t[i].f;
    return -1;
}

void weapon_event(const char* name)
{
    static int cur = -1;
    if (!strncmp(name, "veh_aphelion_fire", 17)) {               /* the ship: blasters on the fire trigger */
        if (cur != DSW_LIGHT) { cur = DSW_LIGHT; racacit_ds_weapon_profile(DSW_LIGHT); }
        racacit_ds_weapon_shot(); return; }
    if (!strncmp(name, "veh_aphelion_missile", 20)) return;
    if (strncmp(name, "wep_", 4) && strncmp(name, "mag_net_", 8) && strncmp(name, "sub_new_", 8)) return;
    if (strstr(name, "gen_emptied") || strstr(name, "outofammo") || strstr(name, "out_of_ammo")) { racacit_ds_weapon_empty(); return; }
    if (!strncmp(name, "wep_gen_", 8)) return;                    /* lock-on, ammo and the like */
    /* A weapon switch: its equip sound sets the trigger at once (enemies do not equip). */
    if (strstr(name, "_equip")) {
        const int f = weapon_family(name);
        if (f >= 0 && f != cur) { cur = f; racacit_ds_weapon_profile(f);
            fprintf(stderr, "[ds-trigger] %s: profile %d (weapon switch)%c", name, f, 10); }
        return;
    }
    static const char* const not_shot[] = { "explode", "impact", "_hit", "hit_", "damage", "death", "_vox", "vocal",
                                            "flyby", "whizby", "whizoff", "bombwhiz", "ricochet", "debris", "reload", "pickup",
                                            "idle", "upgrade", "flying", "_fly_", "projectile", "hover", "aliveloop", "alive_",
                                            "frozen", "freeze", "icebreak", "steadyloop", "trailing", "beeping", "zoom",
                                            "overload", "timerloop", "roar", "bounce", "connector", "morph", "unfold",
                                            "_away", "shrapnel", "charged", "charging" };
    for (unsigned i = 0; i < sizeof not_shot / sizeof not_shot[0]; i++) if (strstr(name, not_shot[i])) return;
    /* The player's shots only: Mr. Zurkon and enemies fire wep_* too. A shot
     * counts while fire is held or within 250 ms of a press. */
    static const char* const not_mine[] = { "wep_mrzurkon_fire", "wep_jrzurkon_fire", "wep_momzurkon_fire", "wep_turret_", "wep_krell",
                                            "wep_pirateturret_", "wep_vis_", "wep_nef_", "wep_thrown_" };
    for (unsigned i = 0; i < sizeof not_mine / sizeof not_mine[0]; i++) if (!strncmp(name, not_mine[i], strlen(not_mine[i]))) return;
    { const unsigned long long fp = g_racacit_fire_ms, now = ps3_qpc_us() / 1000ull;
      if (!g_racacit_fire_held && !(fp && now - fp < 250ull)) return; }
    const int f = weapon_family(name);
    if (f >= 0 && f != cur) { cur = f; racacit_ds_weapon_profile(f);
        fprintf(stderr, "[ds-trigger] %s: profile %d%c", name, f, 10); }
    racacit_ds_weapon_shot();
}

/* Dialogue lines. Into the Nexus has no resident_dialogue tables (ppu_fs.cpp's
 * route for A Crack in Time); each line is a SCREAM stream named
 * "<level>_<speaker>_<rest>.<lang>" -- pri_clank_001.us, pri_ratch_001.us on
 * the prison ship -- in a bank of its own whose resident_sound.<lang>.dat is
 * the line table ppu_fs.cpp knows from A Crack in Time. ppu_fs.cpp names the
 * line as the game reads its VAG header and asks racacit_ds_line_decide; a
 * controller line is played from the file, following the game's reads, the
 * TV's copy zeroed.
 * Which speakers the controller carries follows RACiT's rules: Mr. Zurkon (a
 * weapon) and calls recorded through the radio filter (a stem ending in "_r");
 * RACACIT_DS_RADIO adds speaker codes (comma list, the 2nd token), RACACIT_DS_
 * LINE_SKIP keeps name prefixes on the TV, RACACIT_DS_DIALOGUE=off sends none.
 * Into the Nexus's codes beyond the prison ship are not known yet: the log
 * names each line once ([ds-line]). Returns 0 not a line, 1 a line for the TV,
 * 2 a line for the controller. */
int line_route(const char* name, const char** why)
{
    const size_t L = strlen(name);
    const char* dot = strrchr(name, '.');
    if (!dot || (size_t)(name + L - dot) != 3 || dot[1] < 'a' || dot[1] > 'z' || dot[2] < 'a' || dot[2] > 'z') return 0;
    const char* u1 = strchr(name, '_');
    const char* u2 = u1 ? strchr(u1 + 1, '_') : nullptr;
    if (!u1 || !u2 || u2 > dot) return 0;
    static int mode = -1;
    if (mode < 0) { const char* e = getenv("RACACIT_DS_DIALOGUE"); mode = e && (*e == 'o' || *e == 'O' || *e == '0') ? 0 : 1; }
    if (!mode) { *why = "RACACIT_DS_DIALOGUE=off"; return 1; }
    { const char* e = getenv("RACACIT_DS_LINE_SKIP");
      if (e && *e && match(name, e)) { *why = "kept on the TV (RACACIT_DS_LINE_SKIP)"; return 1; } }
    char spk[24]; size_t n = (size_t)(u2 - u1 - 1); if (n >= sizeof spk) n = sizeof spk - 1;
    memcpy(spk, u1 + 1, n); spk[n] = 0;
    static char radio[256];
    /* Into the Nexus's Zurkon family (the game's gen_ line tables): zurk = Mr.
     * (69 lines), mzrk = Mrs. (26), lzrk = Jr. (31). Only zurk was listed until
     * 2026-11-17, so Mrs. and Jr. stayed on the TV (owner). */
    if (!radio[0]) { const char* e = getenv("RACACIT_DS_RADIO"); snprintf(radio, sizeof radio, "zurk,zurkon,mzrk,lzrk%s%s", e && *e ? "," : "", e && *e ? e : ""); }
    char tok[24]; snprintf(tok, sizeof tok, "%s", spk);
    for (const char* p = radio; *p; ) {
        const char* q = strchr(p, ','); const size_t k = q ? (size_t)(q - p) : strlen(p);
        if (k == strlen(tok) && !strncmp(p, tok, k)) { *why = (strstr(tok, "zurk") || strstr(tok, "zrk")) ? "a Zurkon, a weapon" : "radio (RACACIT_DS_RADIO)"; return 2; }
        if (!q) break;
        p = q + 1;
    }
    if (dot - name > 2 && dot[-2] == '_' && dot[-1] == 'r') { *why = "radio call (_r)"; return 2; }
    *why = "not a radio contact";
    return 1;
}

float line_level(const char* name)
{
    static float lv = -1.0f;
    if (lv < 0.0f) { const char* e = getenv("RACACIT_DS_LINE_LEVEL"); lv = e ? (float)atof(e) : 0.63f; }
    return (strstr(name, "_zurk") || strstr(name, "_mzrk") || strstr(name, "_lzrk")) ? lv * 1.15f : lv;   /* the Zurkons: just above the guns (RACiT v69) */
}

/* Each name once in the log (a small table under a spin lock). */
int first_time(uint32_t* seen, unsigned cap, unsigned* n, volatile long* mu, const char* s, uint32_t salt)
{
    uint32_t h = 2166136261u ^ salt;
    for (const char* q = s; *q; q++) h = (h ^ (uint8_t)*q) * 16777619u;
    while (__sync_lock_test_and_set(mu, 1)) { }
    int dup = 0;
    for (unsigned k = 0; k < *n; k++) if (seen[k] == h) { dup = 1; break; }
    if (!dup && *n < cap) seen[(*n)++] = h;
    __sync_lock_release(mu);
    return !dup;
}

}  // namespace

/* ds-start: the engine just asked SCREAM to play `sound` of `bank` and got
 * `handle` (0 or negative: it did not start); `name_ea` is the sound's name. */
extern "C" void nexus_ds_snd_started(uint32_t bank, uint32_t sound, const uint64_t* gpr)
{
    const int ds_on = ds_player_enabled();
    if (!ds_on && !g_racacit_ds_pad) return;
    const uint32_t handle = (uint32_t)gpr[3], name_ea = (uint32_t)gpr[26];
    char name[64];
    if (!bank_name(bank, name, sizeof name) && !read_name(name_ea, name, sizeof name)) {
        /* RACACIT_DS_DEBUG=1: a start with no name behind its bank nor in r26:
         * which register or field holds one. */
        static int dbg = -1; if (dbg < 0) { const char* e = getenv("RACACIT_DS_DEBUG"); dbg = e && *e == '1'; }
        static int n; if (dbg && n < 30) { n++;
            fprintf(stderr, "[ds-noname] bank %08X sound %u handle %08X r26 %08X%c", bank, sound, handle, name_ea, 10);
            char s[64];
            for (int i = 14; i < 32; i++) {
                const uint32_t r = (uint32_t)gpr[i];
                if (read_name(r, s, sizeof s) && strlen(s) >= 4) fprintf(stderr, "[ds-noname]    r%d -> \"%s\"%c", i, s, 10);
                for (uint32_t o = 0; o < 0x300 && r >= 0x10000u && r < 0x40000000u; o += 4) {
                    const uint32_t q = rd32(r + o);
                    if (read_name(q, s, sizeof s) && strlen(s) >= 6 && strchr(s, '_')) fprintf(stderr, "[ds-noname]    [r%d+0x%X] -> \"%s\"%c", i, o, s, 10);
                }
            } }
        return;
    }
    { const char* why = ""; if (line_route(name, &why)) return; }   /* a dialogue line: racacit_ds_line_decide's, at its file read */
    static int names = -1;
    if (names < 0) { const char* e = getenv("RACACIT_DS_NAMES"); names = e && *e == '1'; }
    const int cat = ds_on ? classify(name) : 0, hcat = ds_on ? haptic(name) : 0;
    if (names) {                                   /* every distinct sound once (choosing routes) */
        static uint32_t seen[4096]; static unsigned ns; static volatile long mu;
        if (first_time(seen, 4096, &ns, &mu, name, 0))
            fprintf(stderr, "[ds-name] %s%s%s%c", name, cat ? "  (speaker)" : "", hcat ? "  (haptics)" : "", 10);
    }
    {   /* What each gun plays: every sound (but characters' cha_*) that starts
         * within 60 ms of a fire press, once: a gun whose shots never reach the
         * controller shows up as "stays on the TV". */
        const unsigned long long f = g_racacit_fire_ms, now = ps3_qpc_us() / 1000ull;
        if (f && now - f < 60ull && strncmp(name, "amb_", 4) && strncmp(name, "ui_", 3) && strncmp(name, "cha_", 4)) {
            static uint32_t seen[512]; static unsigned ns; static volatile long mu;
            if (first_time(seen, 512, &ns, &mu, name, 0x9E37u))
                fprintf(stderr, "[ds-gun] %s %llu ms after fire -> %s%c", name, now - f, cat ? "controller" : "stays on the TV", 10);
        }
    }
    if (g_racacit_ds_pad) weapon_event(name);
    {   /* RACACIT_DS_DEBUG=1: the first routed starts with their handles */
        static int dbg = -1; if (dbg < 0) { const char* e = getenv("RACACIT_DS_DEBUG"); dbg = e && *e == '1'; }
        static int n; if (dbg && (cat || hcat) && n < 40) { n++;
            fprintf(stderr, "[ds-start] %s bank %08X sound %u handle %08X globals %08X%c", name, bank, sound, handle, rd32(kScreamGlobals), 10); } }
    /* A handle is type << 24 | slot << 16 | serial (ds_player.c scream_alive
     * reads the type from bits 24-28); 0 = the sound did not start. */
    if (!ds_on || (!cat && !hcat) || !handle) return;
    const float lv = cat ? level(name) : 0.0f;
    const size_t L = strlen(name);
    if (cat && L > 7 && !strcmp(name + L - 7, "_stream"))   /* a streamed sound: ppu_fs.cpp follows its file reads */
        racacit_ds_stream_expect(handle, lv, name);
    ds_player_scream(bank, sound, lv, hcat ? 0.5f : 0.0f, handle, name);
    ds_player_bind(handle, handle, rd32(kScreamGlobals));
    if (cat) {
        static uint32_t seen[256]; static unsigned ns; static volatile long mu;
        if (first_time(seen, 256, &ns, &mu, name, 0x51u))
            fprintf(stderr, "[ds-route] %s -> controller speaker%c", name, 10);
    }
}

/* ppu_fs.cpp, as the game reads the VAG header of a dialogue line `name`: 1 =
 * the controller plays it, at *level. Each line once in the log. */
extern "C" int racacit_ds_line_decide(const char* name, float* level)
{
    const char* why = "";
    const int ln = line_route(name, &why);
    static uint32_t seen[2048]; static unsigned ns; static volatile long mu;
    if (first_time(seen, 2048, &ns, &mu, name, 0x11u))
        fprintf(stderr, "[ds-line] %s -> %s (%s)%c", name, ln == 2 ? "controller speaker" : "TV", ln ? why : "not a line name", 10);
    if (ln != 2) return 0;
    *level = line_level(name);
    return 1;
}

/* ds-stop: SCREAM stops a sound; the controller's copy ends too. */
extern "C" void nexus_ds_snd_stop(uint32_t handle)
{
    if (handle && ds_player_enabled()) ds_player_stop(handle);
}

/* ds-release: SCREAM lets go of a sound's handle. The engine does it to every one-shot
 * right after starting it, so the controller's copy plays on (ds_player_release); until
 * 2026-11-17 this stopped it, and every gun was a 30 ms click on the speaker. */
extern "C" void nexus_ds_snd_release(uint32_t handle)
{
    if (handle && ds_player_enabled()) ds_player_release(handle);
}

/* ds-pause: SCREAM's stream pause callback. It fires for every stream at every
 * start too, paused then resumed: only a stream that stays paused means
 * anything (ds_player.c). RACACIT_DS_DEBUG=1 logs them. */
extern "C" void nexus_ds_stream_pause(uint32_t stream, uint32_t paused)
{
    if (!ds_player_enabled()) return;
    const int on = paused != 0;
    { static int dbg = -1; if (dbg < 0) { const char* e = getenv("RACACIT_DS_DEBUG"); dbg = e && *e == '1'; }
      static int n; if (dbg && n < 2000) { n++; fprintf(stderr, "[ds-pause] %llu us stream %08X: %s%c", ps3_qpc_us(), stream, on ? "paused" : "resumed", 10); } }
    ds_player_stream_pause(stream, on);
}
