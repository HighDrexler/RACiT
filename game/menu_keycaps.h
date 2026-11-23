/* Include after ppu_recomp.h. Publish to the current screen's movie before
 * its normal frame update, outside Scaleform's ActionScript callbacks.
 * Only while a prepared menu archive is in use (game/menu_archive.cpp): the
 * original movies have no ActionScript that reads these fields.
 * The keyboard/controller decision is cellPad.c's racit_keyboard_prompts_active.
 */
#ifndef RACIT_MENU_KEYCAPS_H
#define RACIT_MENU_KEYCAPS_H
#include <stdint.h>
#include <atomic>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX   /* keep std::min / std::max usable in the lifted code that includes this */
#endif
#include <windows.h>
#endif
#include "ppu_stack_read.h"

extern "C" int racit_keyboard_prompts_active(void);   /* cellPad.c */
extern "C" int racit_menu_archive_active(void);       /* game/menu_archive.cpp */
extern "C" void ps3_indirect_call(ppu_context*);
extern "C" PPU_THREAD_LOCAL void (*g_trampoline_fn)(void*);

static bool racit_menu_word(uint32_t address, uint32_t* value)
{
    uint32_t raw;
    if (!racit_stack_read(address, &raw)) return false;
    *value = __builtin_bswap32(raw);
    return true;
}

static void racit_menu_key_label(const char* binding, const char* fallback,
                                 char label[16])
{
    char name[48];
    snprintf(name, sizeof name, "RACACIT_BIND_%s", binding);
    const char* value = getenv(name);
    if (!value || !*value) value = fallback;
    struct Alias { const char* name; const char* label; };
    static const Alias aliases[] = {
        {"Mouse1", "LMB"}, {"Mouse2", "MMB"}, {"Mouse3", "RMB"},
        {"Mouse4", "M4"}, {"Mouse5", "M5"}, {"Space", "SPC"},
        {"Return", "ENT"}, {"Escape", "ESC"}, {"Tab", "TAB"},
        {"Left Ctrl", "CTRL"}, {"Right Ctrl", "CTRL"},
        {"Left Shift", "SHIFT"}, {"Right Shift", "SHIFT"},
    };
    /* The comma key: "Comma", or (settings saved before that) "," on its own
     * as the first entry, since "," also separates the entries. */
    if ((!strncmp(value, "Comma", 5) && (!value[5] || value[5] == ',')) ||
        (value[0] == ',' && (!value[1] || value[1] == ','))) {
        snprintf(label, 16, ",");
        return;
    }
    const size_t length = strcspn(value, ",");
    for (const Alias& alias : aliases) {
        if (strlen(alias.name) == length && !memcmp(value, alias.name, length)) {
            snprintf(label, 16, "%s", alias.label);
            return;
        }
    }
    const size_t count = length < 15 ? length : 15;
    for (size_t i = 0; i < count; ++i) {
        const unsigned char ch = (unsigned char)value[i];
        label[i] = ch >= 'a' && ch <= 'z' ? (char)(ch - 'a' + 'A') : (char)ch;
    }
    label[count] = 0;
}

static bool racit_menu_set_string(ppu_context* ctx, uint32_t movie,
                                  uint32_t procedure, uint32_t toc,
                                  const char* name, const char* text)
{
    const ppu_context saved = *ctx;
    auto continuation = g_trampoline_fn;
    const uint32_t sp = (uint32_t)saved.gpr[1];
    const uint32_t frame = (sp - 0x400u) & ~15u;
    const uint32_t path = frame + 0x100, value = frame + 0x180, string = frame + 0x1c0;
    memcpy(vm_base + path, name, strlen(name) + 1);
    memcpy(vm_base + string, text, strlen(text) + 1);
    for (unsigned i = 0; i < 0x18; i += 4) vm_write32(value + i, 0);
    vm_write32(value, 4); // Existing GFxValue string layout used by the card hook.
    vm_write32(value + 8, string);
    vm_write32(frame, 0);
    vm_write32(frame + 4, sp);
    ctx->gpr[1] = frame;
    ctx->gpr[2] = toc;
    ctx->gpr[3] = movie;
    ctx->gpr[4] = path;
    ctx->gpr[5] = value;
    ctx->gpr[6] = 1; // Sticky assignment, as in the existing front-end card hook.
    ctx->ctr = procedure;
    ctx->lr = saved.lr;
    g_trampoline_fn = nullptr;
    ps3_indirect_call(ctx);
    while (g_trampoline_fn) {
        auto next = g_trampoline_fn;
        g_trampoline_fn = nullptr;
        next(ctx);
    }
    const bool accepted = (uint8_t)ctx->gpr[3] != 0;
    *ctx = saved;
    g_trampoline_fn = continuation;
    return accepted;
}

static void racit_publish_menu_keyboard(ppu_context* ctx, uint32_t movie_offset = 4)
{
    if (!racit_menu_archive_active()) return;
    const uint32_t owner = (uint32_t)ctx->gpr[3];
    uint32_t movie = 0;
    if (!owner || owner > UINT32_MAX - movie_offset ||
        !racit_menu_word(owner + movie_offset, &movie) || !movie) return;
    static std::atomic<unsigned> reports{0};
    const bool report = reports.fetch_add(1, std::memory_order_relaxed) == 0;
    uint32_t table = 0, descriptor = 0, procedure = 0, toc = 0;
    if (!racit_menu_word(movie, &table) || table > UINT32_MAX - 0x30u ||
        !racit_menu_word(table + 0x30u, &descriptor) || descriptor > UINT32_MAX - 4u ||
        !racit_menu_word(descriptor, &procedure) || !racit_menu_word(descriptor + 4u, &toc)) {
        if (report) fprintf(stderr, "[menu-keycaps] frame movie=%08X table=%08X descriptor=%08X procedure=%08X: receiver read failed\n", movie, table, descriptor, procedure);
        return;
    }
    const char* enabled = getenv("RACACIT_PROMPT_SWAP");
    const bool keyboard = !(enabled && *enabled == '0') && racit_keyboard_prompts_active() != 0;
    /* Fallbacks: cellPad.c's default bindings (k_kb), first key shown. */
    struct Binding { const char* field; const char* binding; const char* fallback; };
    static const Binding bindings[] = {
        {"cross", "CROSS", "Space"}, {"circle", "CIRCLE", "E"},
        {"square", "SQUARE", "F"}, {"triangle", "TRIANGLE", "Q"},
        {"select", "SELECT", "Tab"}, {"start", "START", "Escape"},
        {"l1", "L1", "Left Ctrl"}, {"l2", "L2", "Mouse3"},   /* cellPad.c defaults: right click = L2 (PR #34) */
        {"r1", "R1", "Mouse1"}, {"r2", "R2", "Left Shift"},
        {"l3", "L3", "V"}, {"r3", "R3", "Mouse2"},
        {"lstick", "LS_UP", "WASD"}, {"rstick", "RS_UP", "IJKL"},
    };
    bool accepted = true;
    unsigned accepted_mask = 0, index = 0;
    for (const Binding& binding : bindings) {
        char name[48], label[16];
        snprintf(name, sizeof name, "_root.__rccit_%s", binding.field);
        racit_menu_key_label(binding.binding, binding.fallback, label);
        const bool field_accepted = racit_menu_set_string(ctx, movie, procedure, toc, name, label);
        if (field_accepted) accepted_mask |= 1u << index;
        ++index;
        accepted = field_accepted && accepted;
    }
    const bool mode_accepted = racit_menu_set_string(ctx, movie, procedure, toc, "_root.__rccit_mode",
                                                    accepted && keyboard ? "keyboard" : "controller");
    if (report) fprintf(stderr, "[menu-keycaps] frame movie=%08X table=%08X descriptor=%08X procedure=%08X toc=%08X keyboard=%d fields=%04X mode-accepted=%d\n", movie, table, descriptor, procedure, toc, keyboard, accepted_mask, mode_accepted);
    /* RACACIT_PROMPT_CHECK=1 (tests): a line for each menu movie and each change of
     * mode, so a headless run can tell every menu it opened got the keys. */
    static int check = -1;
    if (check < 0) { const char* e = getenv("RACACIT_PROMPT_CHECK"); check = e && *e == '1'; }
    static std::atomic<uint64_t> seen[64];
    const uint64_t now = (uint64_t)movie << 32 | (uint64_t)accepted_mask << 2 | (mode_accepted ? 2u : 0u) | (accepted && keyboard ? 1u : 0u);
    bool fresh = false;
    if (check) {
        std::atomic<uint64_t>* slot = nullptr;
        for (auto& s : seen) {   /* this movie's last state, or a free slot */
            const uint64_t v = s.load(std::memory_order_relaxed);
            if ((v >> 32) == movie || !v) { slot = &s; break; }
        }
        fresh = !slot || slot->exchange(now, std::memory_order_relaxed) != now;
    }
    if (fresh)
        fprintf(stderr, "[menu-check] movie=%08X mode=%s fields=%04X/%04X mode-accepted=%d\n", movie,
                accepted && keyboard ? "keyboard" : "controller", accepted_mask, (1u << index) - 1u, mode_accepted);
}
#endif
