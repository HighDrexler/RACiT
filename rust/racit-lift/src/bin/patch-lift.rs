//! Post-lift source patches for A Crack in Time (re-run after every lift). It replaced
//! scripts/patch_lift.py, scripts/update120_hooks.py and scripts/fix_vmx_alias.py, and
//! writes the same bytes they did.
//!
//!     patch-lift [RECOMP] [--elf EBOOT.ELF] [--profile PROFILE] [--root RACIT]
//!     patch-lift --check-profile EBOOT.ELF [PROFILE|auto]
//!     patch-lift --identify-profile EBOOT.ELF [PROFILE|auto]
//!     patch-lift --apply-update-hooks RECOMP EBOOT.ELF
//!     patch-lift --fix-vmx RECOMP [--check]
//!
//! 1. Guest setjmp/longjmp (Lua error handling: luaD_rawrunprotected at 0x50A6BC calls
//!    setjmp 0x90B138; luaD_throw at 0x50A83C calls longjmp 0x90B25C). A static lift turns
//!    longjmp into "restore guest regs, return to caller", which falls through with r3=1
//!    and dereferences garbage. Here the protected frame does a real host setjmp in its own
//!    C frame (registered on a per-thread stack keyed by the guest jmp_buf), and the lifted
//!    longjmp, after restoring guest registers exactly as before, host-longjmps to it.
//! 2. GPU-idle wait in func_005CB084 (0x5CB1AC): the title reads ctrl->get once and spins
//!    until ctrl->put equals it. Re-read get inside the loop, which is what the source meant.
//! 3. End-of-frame blit in func_005F3E50: tell the runtime at both put writes so the FIFO
//!    walker catches up first (game/stubs.cpp racacit_gcm_put_moved has the full account).
//! 11. Vector instructions whose destination is also a source (src/vmx.rs).
//!
//! The numbered steps below keep scripts/patch_lift.py's numbers and comments.
//!
//! Guest addresses are written as scripts/retarget.py rewrites them for another disc:
//! func_XXXXXXXX, loc_XXXXXXXX and 0x00XXXXXX (update 1.20's in lower case, which it leaves).
//!
//! Speed: each chunk's lifted functions are listed once, and a step whose function is not
//! in that chunk is not looked for again.

use racit_lift::{
    Result, base, contains, count, find, find_from, insert_after_include, lifted_sources, profile,
    replace_all, replace_first, rfind, splice, update120, vmx,
};
use regex::bytes::{Captures, Regex};
use std::collections::{BTreeMap, BTreeSet, HashSet};
use std::path::{Path, PathBuf};
use std::process::ExitCode;
use std::sync::LazyLock;

// ---- helpers -----------------------------------------------------------------------------

/// The function NAME's span: (start of its head, start of its body, its closing "\n}\n").
fn func_span(src: &[u8], name: &str) -> Result<Option<(usize, usize, usize)>> {
    let head = format!("void {name}(ppu_context* ctx) {{\n");
    let Some(start) = find(src, head.as_bytes()) else {
        return Ok(None);
    };
    let body = start + head.len();
    let end = find_from(src, b"\n}\n", body)
        .ok_or_else(|| format!("{name}: substring not found (no closing brace)"))?;
    Ok(Some((start, body, end)))
}

/// The lifted function names in a chunk ("void func_XXXXXXXX(ppu_context* ctx) {\n"
/// anywhere, as func_span would find them).
fn function_names(src: &[u8]) -> HashSet<Vec<u8>> {
    const TAIL: &[u8] = b"(ppu_context* ctx) {\n";
    let mut names = HashSet::new();
    for i in memchr::memmem::find_iter(src, b"void func_") {
        let name = i + 5;
        let close = name + 13;
        if src.len() >= close + TAIL.len() && &src[close..close + TAIL.len()] == TAIL {
            names.insert(src[name..close].to_vec());
        }
    }
    names
}

fn assert_that(cond: bool, msg: impl Into<String>) -> Result<()> {
    if cond {
        Ok(())
    } else {
        Err(format!("AssertionError: {}", msg.into()))
    }
}

/// The 1.00 disc's hooks in one chunk.
struct Chunk<'a> {
    src: Vec<u8>,
    names: HashSet<Vec<u8>>,
    changed: bool,
    build_title: &'a str,
}

impl Chunk<'_> {
    fn span(&self, name: &str) -> Result<Option<(usize, usize, usize)>> {
        if !self.names.contains(name.as_bytes()) {
            return Ok(None);
        }
        func_span(&self.src, name)
    }

    fn fn_contains(&self, sp: (usize, usize, usize), needle: &str) -> bool {
        contains(&self.src[sp.0..sp.2], needle.as_bytes())
    }

    fn has(&self, needle: &str) -> bool {
        contains(&self.src, needle.as_bytes())
    }

    fn set_fn(&mut self, sp: (usize, usize, usize), fun: &[u8]) {
        self.src = splice(&self.src, sp.0, sp.2, fun);
    }

    /// The declaration after the first #include, unless `test` is in the chunk already.
    fn declare(&mut self, test: &str, decl: &str) -> Result<()> {
        if !self.has(test) {
            self.src = insert_after_include(&self.src, decl.as_bytes())?;
        }
        Ok(())
    }

    /// Insert `line` after a function's head (replace head with head + line).
    fn after_head(fun: &[u8], name: &str, line: &str) -> Vec<u8> {
        let head = format!("void {name}(ppu_context* ctx) {{\n");
        replace_first(fun, head.as_bytes(), format!("{head}{line}").as_bytes())
    }

    fn uses_usa(&self) -> bool {
        self.build_title == "BCUS98124"
    }
}

const DECLS: &str = "
/* racacit: guest setjmp/longjmp support (scripts/patch_lift.py) */
#include \"ps3_setjmp.h\"   /* non-unwinding on Windows (tools/ps3recomp/runtime/platform) */
typedef struct RacJbFrame { jmp_buf env; uint32_t guest_buf; struct RacJbFrame* prev; } RacJbFrame;
extern \"C\" void racacit_jb_push(RacJbFrame* f, uint32_t guest_buf);
extern \"C\" void racacit_jb_pop(RacJbFrame* f);
extern \"C\" void racacit_longjmp(uint32_t guest_buf);
";

/// Script (Lua C) functions traced by RACACIT_LUA_CTRACE; addresses from the title's
/// registration table of (name, function descriptor) pairs at ~0xC8D000.
const LUA_CTRACE: &[(&str, &str)] = &[
    ("func_00279EA8", "actor_speak"),
    ("func_00279E18", "actor_is_speaking"),
    ("func_00279FD4", "make_actor"),
    ("func_00279828", "actor_move"),
    ("func_00290804", "queue_scene"),
    ("func_00290A2C", "queue_movie"),
    ("func_00290998", "queue_cinematic"),
    ("func_00290E8C", "on_next_cine_end"),
    ("func_002918C8", "on_elapsed"),
    ("func_00290BD8", "unset_event"),
    ("func_00288928", "set_follow_cam"),
    ("func_002882B0", "activate_cam"),
    ("func_00281774", "hero_lockout_control"),
    ("func_00282DA0", "hero_give_weapon"),
    ("func_00292E84", "has_valid_checkpoint"),
    ("func_00292EC8", "checkpoint"),
    ("func_0028B398", "warp"),
    ("func_00279350", "scene_show_subtitle"),
    ("func_002792EC", "scene_hide_subtitle"),
    ("func_00507EC4", "print"),
    ("func_0051C048", "stop_all_vo"),
    ("func_00521804", "run_script"),
    ("func_00292B5C", "add_mission"),
    ("func_00292D44", "request_auto_save"),
    ("func_0026DC64", "qwark_door_enable_break"),
    ("func_0052194C", "screen_out"),
    ("func_0007DE04", "screen_out2"),
    // space: what makes a planet / moon landable, and the ship radio
    ("func_0026C71C", "space_enable_planet"),
    ("func_0026C7EC", "space_enable_moon"),
    ("func_0026C964", "space_land_ship"),
    ("func_0026C0A0", "space_unlock_level"),
    ("func_0026C5D4", "space_set_planet_tag"),
    ("func_0026C678", "space_set_moon_tag"),
    ("func_0026C58C", "space_enable_takeoff"),
    ("func_0026C268", "space_set_objective"),
    ("func_0026B5E4", "space_show_radio"),
    ("func_0026BA24", "space_set_radio_channel"),
    // the radio's chain (global space_radio.lua): each track's end starts the next
    ("func_00285B54", "play_music"),
    ("func_00285ADC", "stop_music"),
    ("func_00291444", "on_audio_end"),
    ("func_0051CF70", "play_explicit_dialog_spec"),
    ("func_0051D078", "play_explicit_sound_spec"),
    ("func_0051C4D8", "stop_vo"),
];

fn patch_chunk(c: &mut Chunk) -> Result<()> {
    // Each half is tested on its own function: the lifter may put the setjmp caller and
    // longjmp in different chunks.
    if let Some(sp) = c.span("func_0050A6BC")?
        && !c.fn_contains(sp, "racacit_jb_push")
    {
        let mut fun = c.src[sp.0..sp.2].to_vec();
        let call = b"ctx->lr = 0x0050A6F8; func_0090B138(ctx); DRAIN_TRAMPOLINE(ctx);";
        assert_that(
            contains(&fun, call),
            "rawrunprotected setjmp call not found",
        )?;
        fun = Chunk::after_head(&fun, "func_0050A6BC", "        RacJbFrame _jbf;\n");
        fun = replace_first(
                &fun,
                call,
                b"ctx->lr = 0x0050A6F8;\n        racacit_jb_push(&_jbf, (uint32_t)ctx->gpr[3]);\n        if (setjmp(_jbf.env) == 0) { func_0090B138(ctx); DRAIN_TRAMPOLINE(ctx); }\n        else { g_trampoline_fn = 0; /* longjmp landed: guest regs restored, r3 = val */ }",
            );
        // pop the frame on the (only) normal exit
        let tail = rfind(&fun, b"        return;")
            .ok_or("substring not found: return in func_0050A6BC")?;
        fun = splice(&fun, tail, tail, b"        racacit_jb_pop(&_jbf);\n");
        c.set_fn(sp, &fun);
        c.changed = true;
    }

    if let Some(sp) = c.span("func_0090B25C")?
        && !c.fn_contains(sp, "racacit_longjmp")
    {
        let mut fun = c.src[sp.0..sp.2].to_vec();
        fun = Chunk::after_head(
            &fun,
            "func_0090B25C",
            "        uint32_t _jb = (uint32_t)ctx->gpr[3];\n",
        );
        let first_ret =
            find(&fun, b"        return;").ok_or("substring not found: return in func_0090B25C")?;
        fun = splice(
            &fun,
            first_ret,
            first_ret,
            b"        racacit_longjmp(_jb);\n",
        );
        c.set_fn(sp, &fun);
        c.changed = true;
    }

    if let Some(sp) = c.span("func_005CB084")?
        && !c.has("/* racacit: get re-read */")
    {
        let fun = c.src[sp.0..sp.2].to_vec();
        let old = b"        ctx->gpr[29] = vm_read32(ctx->gpr[12] + 0x4);\nloc_005CB1BC:\n";
        assert_that(
            contains(&fun, old),
            "GPU idle wait not found in func_005CB084",
        )?;
        let fun = replace_first(
                &fun,
                old,
                b"loc_005CB1BC:\n        ctx->gpr[29] = vm_read32(ctx->gpr[12] + 0x4);   /* racacit: get re-read */\n",
            );
        c.set_fn(sp, &fun);
        c.changed = true;
    }

    if let Some(sp) = c.span("func_0050A83C")?
        && !c.has("racacit_lua_throw_hook")
    {
        let fun = Chunk::after_head(
            &c.src[sp.0..sp.2],
            "func_0050A83C",
            "        racacit_lua_throw_hook(ctx);   /* racacit: log Lua errors */\n",
        );
        c.set_fn(sp, &fun);
        c.declare(
            "void racacit_lua_throw_hook",
            "extern \"C\" void racacit_lua_throw_hook(ppu_context* ctx);\n",
        )?;
        c.changed = true;
    }

    if let Some(sp) = c.span("func_0005069C")?
        && !c.has("racacit_lua_exec_hook")
    {
        let fun = Chunk::after_head(
            &c.src[sp.0..sp.2],
            "func_0005069C",
            "        racacit_lua_exec_hook(ctx);   /* racacit: log Lua execute() strings */\n",
        );
        c.set_fn(sp, &fun);
        c.src = insert_after_include(
            &c.src,
            b"extern \"C\" void racacit_lua_exec_hook(ppu_context* ctx);\n",
        )?;
        c.changed = true;
    }

    // 12. Sound starts (game/stubs.cpp racacit_snd_play_hook): func_00577ED8 is the
    //     engine's play-a-sound-spec entry; func_0057AEC0 is the virtual it ends in. Their
    //     callers and arguments say what is playing -- the DualSense speaker routing keys on them.
    for (fname, tag) in [
        ("func_00577ED8", 0),
        ("func_0057AEC0", 1),
        ("func_006DDC90", 2),
        ("func_00979CF0", 3),
    ] {
        if let Some(sp) = c.span(fname)?
            && !c.fn_contains(sp, "racacit_snd_play_hook")
        {
            let fun = c.src[sp.0..sp.2].to_vec();
            let head = format!("void {fname}(ppu_context* ctx) {{\n");
            assert_that(
                count(&fun, head.as_bytes()) == 1,
                format!("{fname} head not found"),
            )?;
            let fun = Chunk::after_head(
                &fun,
                fname,
                &format!(
                    "        racacit_snd_play_hook(ctx, {tag});   /* racacit: sound starts (DualSense routing) */\n"
                ),
            );
            c.set_fn(sp, &fun);
            c.declare(
                "void racacit_snd_play_hook",
                "extern \"C\" void racacit_snd_play_hook(ppu_context* ctx, int tag);\n",
            )?;
            c.changed = true;
        }
    }

    //     func_006DDC90 starts one Insomniac sound instance through SCREAM's func_00979CF0;
    //     the instance's stops (racacit_snd_stop_hook, r3 = the instance): func_006DE478 sets
    //     its stopping flag, func_006DDC58 stops it at once.
    for (fname, tag) in [("func_006DE478", 0), ("func_006DDC58", 1)] {
        if let Some(sp) = c.span(fname)?
            && !c.fn_contains(sp, "racacit_snd_stop_hook")
        {
            let fun = c.src[sp.0..sp.2].to_vec();
            let head = format!("void {fname}(ppu_context* ctx) {{\n");
            assert_that(
                count(&fun, head.as_bytes()) == 1,
                format!("{fname} head not found"),
            )?;
            let fun = Chunk::after_head(
                &fun,
                fname,
                &format!(
                    "        racacit_snd_stop_hook(ctx, {tag});   /* racacit: sound stops (DualSense speaker) */\n"
                ),
            );
            c.set_fn(sp, &fun);
            c.declare(
                "void racacit_snd_stop_hook",
                "extern \"C\" void racacit_snd_stop_hook(ppu_context* ctx, int tag);\n",
            )?;
            c.changed = true;
        }
    }
    if let Some(sp) = c.span("func_006DDC90")?
        && !c.fn_contains(sp, "racacit_snd_start_end")
    {
        let mut fun = c.src[sp.0..sp.2].to_vec();
        assert_that(
            count(&fun, b"        return;") == 1,
            "func_006DDC90: expected exactly one return",
        )?;
        let tail = rfind(&fun, b"        return;").unwrap();
        fun = splice(&fun, tail, tail, b"        racacit_snd_start_end();\n");
        c.set_fn(sp, &fun);
        c.declare(
            "void racacit_snd_start_end",
            "extern \"C\" void racacit_snd_start_end(void);\n",
        )?;
        c.changed = true;
    }

    // 4. Start skips movies (game/stubs.cpp): the movie-slot update's "still playing" result
    //    and every BinkOpen go through hooks.
    const MOVIE_DECLS: &str = "extern \"C\" uint32_t racacit_movie_update_hook(uint32_t slot, uint32_t playing);\nextern \"C\" void racacit_bink_opened(uint32_t bink, uint32_t path_ea);\n";
    if let Some(sp) = c.span("func_006511BC")?
        && !c.fn_contains(sp, "racacit_movie_update_hook")
    {
        let mut fun = c.src[sp.0..sp.2].to_vec();
        let anchor = b"        ctx->gpr[28] = ctx->gpr[3] + ctx->gpr[0];\n";
        assert_that(
            contains(&fun, anchor),
            "movie slot computation not found in func_006511BC",
        )?;
        fun = Chunk::after_head(&fun, "func_006511BC", "        uint32_t _rac_slot = 0;\n");
        fun = replace_first(
            &fun,
            anchor,
            &[
                &anchor[..],
                b"        _rac_slot = (uint32_t)ctx->gpr[28];   /* racacit: movie slot */\n",
            ]
            .concat(),
        );
        let tail = rfind(&fun, b"        return;")
            .ok_or("substring not found: return in func_006511BC")?;
        fun = splice(
            &fun,
            tail,
            tail,
            b"        ctx->gpr[3] = racacit_movie_update_hook(_rac_slot, (uint32_t)ctx->gpr[3]);\n",
        );
        c.set_fn(sp, &fun);
        c.declare("uint32_t racacit_movie_update_hook", MOVIE_DECLS)?;
        c.changed = true;
    }

    if let Some(sp) = c.span("func_0065282C")?
        && !c.fn_contains(sp, "racacit_bink_opened")
    {
        let mut fun = c.src[sp.0..sp.2].to_vec();
        for lr in ["0x0065298C", "0x006529E4"] {
            let call = format!("ctx->lr = {lr}; func_009A8798(ctx); DRAIN_TRAMPOLINE(ctx);");
            assert_that(
                contains(&fun, call.as_bytes()),
                "BinkOpen call not found in func_0065282C",
            )?;
            fun = replace_first(
                    &fun,
                    call.as_bytes(),
                    format!(
                        "{{ uint32_t _rac_path = (uint32_t)ctx->gpr[3]; {call} racacit_bink_opened((uint32_t)ctx->gpr[3], _rac_path); }}"
                    )
                    .as_bytes(),
                );
        }
        c.set_fn(sp, &fun);
        c.declare("void racacit_bink_opened", MOVIE_DECLS)?;
        c.changed = true;
    }

    // 5. Every cutscene skippable (game/stubs.cpp racacit_force_abort).
    for (fname, kind) in [
        ("func_0004E7B8", 0),
        ("func_0004E598", 1),
        ("func_0004E6F0", 2),
    ] {
        if let Some(sp) = c.span(fname)?
            && !c.fn_contains(sp, "racacit_force_abort")
        {
            let fun = Chunk::after_head(
                &c.src[sp.0..sp.2],
                fname,
                &format!(
                    "        racacit_force_abort((uint32_t)ctx->gpr[5], {kind});   /* racacit */\n"
                ),
            );
            c.set_fn(sp, &fun);
            c.declare(
                "void racacit_force_abort",
                "extern \"C\" void racacit_force_abort(uint32_t opts, int kind);\n",
            )?;
            c.changed = true;
        }
    }

    // 6. End-of-frame blit (game/stubs.cpp racacit_gcm_put_moved): the two ctrl->put writes
    //    at the tail of func_005F3E50.
    if let Some(sp) = c.span("func_005F3E50")?
        && !c.fn_contains(sp, "racacit_gcm_put_moved")
    {
        let mut fun = c.src[sp.0..sp.2].to_vec();
        // put = end of the scratch packet; the title then waits for idle itself
        let l1 = "        vm_write32(ctx->gpr[28] + 0x0, ctx->gpr[7]);\n";
        let l2 = "        ctx->lr = 0x005F4324; func_0066117C(ctx); DRAIN_TRAMPOLINE(ctx);\n";
        // put = the other main buffer (the return JUMP's target); no wait follows
        let s1 = "        vm_write32(ctx->gpr[24] + 0x0, ctx->gpr[25]);\n";
        let s2 = "        vm_write32(ctx->gpr[23] + 0x0, ctx->gpr[21]);\n";
        let (first, second) = (format!("{l1}{l2}"), format!("{s1}{s2}"));
        assert_that(
            count(&fun, first.as_bytes()) == 1,
            "first put write not found in func_005F3E50",
        )?;
        assert_that(
            count(&fun, second.as_bytes()) == 1,
            "second put write not found in func_005F3E50",
        )?;
        fun = replace_first(
            &fun,
            first.as_bytes(),
            format!(
                "{l1}        racacit_gcm_put_moved(0);   /* racacit: wake the FIFO walker */\n{l2}"
            )
            .as_bytes(),
        );
        fun = replace_first(
                &fun,
                second.as_bytes(),
                format!("{s1}        racacit_gcm_put_moved(1);   /* racacit: RSX takes the return JUMP now */\n{s2}").as_bytes(),
            );
        c.set_fn(sp, &fun);
        c.declare(
            "void racacit_gcm_put_moved",
            "extern \"C\" void racacit_gcm_put_moved(int wait);\n",
        )?;
        c.changed = true;
    }

    // 7. Test hook RACACIT_START_LEVEL (game/stubs.cpp racacit_level_change): the level id
    //    argument of the level change func_0029A4F4.
    if let Some(sp) = c.span("func_0029A4F4")?
        && !c.fn_contains(sp, "racacit_level_change")
    {
        let fun = Chunk::after_head(
            &c.src[sp.0..sp.2],
            "func_0029A4F4",
            "        ctx->gpr[3] = racacit_level_change((uint32_t)ctx->gpr[3]);   /* racacit */\n",
        );
        c.set_fn(sp, &fun);
        c.declare(
            "uint32_t racacit_level_change",
            "extern \"C\" uint32_t racacit_level_change(uint32_t level);\n",
        )?;
        c.changed = true;
    }

    // 13. The game's GPU watchdog (func_005C50AC, its wait for a kick's end label): 2.5 s to
    //     20 s, as a real hang still has to get out (0x5F2EDC00 ticks at 79.8 MHz).
    if let Some(sp) = c.span("func_005C50AC")?
        && c.fn_contains(sp, "0xBE4 << 16")
    {
        let fun = replace_first(
            &c.src[sp.0..sp.2],
            b"(uint32_t)0xBE4 << 16",
            b"(uint32_t)0x5F2E << 16",
        );
        let fun = replace_first(&fun, b"ctx->gpr[8] | 0x20E0", b"ctx->gpr[8] | 0xDC00");
        c.set_fn(sp, &fun);
        c.changed = true;
    }

    // 14. SCREAM's stream pause callback (game/stubs.cpp racacit_stream_pause_hook).
    if let Some(sp) = c.span("func_0099B4E0")?
        && !c.fn_contains(sp, "racacit_stream_pause_hook")
    {
        let fun = Chunk::after_head(
            &c.src[sp.0..sp.2],
            "func_0099B4E0",
            "        racacit_stream_pause_hook(ctx);   /* racacit */\n",
        );
        c.set_fn(sp, &fun);
        c.declare(
            "void racacit_stream_pause_hook",
            "extern \"C\" void racacit_stream_pause_hook(ppu_context* ctx);\n",
        )?;
        c.changed = true;
    }

    // 8. Diagnostic RACACIT_LUA_CTRACE (game/stubs.cpp racacit_luac_trace): the entry of the
    //    script functions that drive scenes, speech and control.
    for (fname, lname) in LUA_CTRACE {
        if let Some(sp) = c.span(fname)?
            && !c.fn_contains(sp, "racacit_luac_trace")
        {
            let fun = Chunk::after_head(
                &c.src[sp.0..sp.2],
                fname,
                &format!("        racacit_luac_trace(ctx, \"{lname}\");   /* racacit */\n"),
            );
            c.set_fn(sp, &fun);
            c.declare(
                "void racacit_luac_trace",
                "extern \"C\" void racacit_luac_trace(ppu_context* ctx, const char* name);\n",
            )?;
            c.changed = true;
        }
    }

    // 9. Per-object shadow maps (game/stubs.cpp racacit_caster_box): refresh a caster's
    //    stale camera-space box (moby in r17) before func_005EDEE8 fits its shadow map to
    //    it, at loc_005EE1E8.
    if let Some(sp) = c.span("func_005EDEE8")?
        && !c.fn_contains(sp, "racacit_caster_box")
    {
        let fun = c.src[sp.0..sp.2].to_vec();
        let lab = b"loc_005EE1E8:\n";
        assert_that(
            count(&fun, lab) == 1,
            "loc_005EE1E8 not found in func_005EDEE8",
        )?;
        let fun = replace_first(
            &fun,
            lab,
            &[
                &lab[..],
                b"        racacit_caster_box((uint32_t)ctx->gpr[17]);   /* racacit */\n",
            ]
            .concat(),
        );
        c.set_fn(sp, &fun);
        c.declare(
            "void racacit_caster_box",
            "extern \"C\" void racacit_caster_box(uint32_t moby);\n",
        )?;
        c.changed = true;
    }

    // 10. SPU kick on wait (runtime/spu/spu_coherency.c racacit_spu_kick): the job handle
    //     wait func_00696614, the batch-counter wait func_00660924 and the async object's
    //     ready-bit spin func_00555EAC wake the parked SPU lanes as the wait begins.
    for fname in ["func_00696614", "func_00660924", "func_00555EAC"] {
        if let Some(sp) = c.span(fname)?
            && !c.fn_contains(sp, "racacit_wait_begin")
        {
            let mut fun = c.src[sp.0..sp.2].to_vec();
            let head = format!("void {fname}(ppu_context* ctx) {{\n");
            assert_that(
                count(&fun, head.as_bytes()) == 1,
                format!("{fname} head not found"),
            )?;
            assert_that(
                count(&fun, b"        return;") == 1,
                format!("{fname}: expected exactly one return"),
            )?;
            fun = Chunk::after_head(
                &fun,
                fname,
                "        racacit_wait_begin(ctx);   /* racacit: wake parked SPUs, time the wait */\n",
            );
            let tail = rfind(&fun, b"        return;").unwrap();
            fun = splice(&fun, tail, tail, b"        racacit_wait_end();\n");
            c.set_fn(sp, &fun);
            c.declare(
                    "void racacit_wait_begin",
                    "extern \"C\" void racacit_wait_begin(ppu_context* ctx);\nextern \"C\" void racacit_wait_end(void);\n",
                )?;
            c.changed = true;
        }
    }

    // 15. The game's per-title language list (func_0066C518): a BCUS disc allows only
    //     English, French and Spanish though it holds the full German, Italian and Dutch text
    //     and speech (RACACIT_LANGUAGE, game/stubs.cpp).
    if let Some(sp) = c.span("func_0066C518")?
        && !c.fn_contains(sp, "racacit: on the disc")
    {
        let read = "ctx->gpr[10] = vm_read8(ctx->gpr[9] + 0x70);\n";
        let fun = replace_first(
                &c.src[sp.0..sp.2],
                read.as_bytes(),
                format!("{read}        if ((uint32_t)ctx->gpr[11] - 4 <= 2) ctx->gpr[10] = 1;   /* racacit: on the disc */\n")
                    .as_bytes(),
            );
        c.set_fn(sp, &fun);
        c.changed = true;
    }

    // 16. Ratchet's armor look (game/stubs.cpp racacit_armor_look): func_001709B4's one
    //     read of the armor level goes through the setting.
    if let Some(sp) = c.span("func_001709B4")?
        && !c.fn_contains(sp, "racacit_armor_look")
    {
        let read = b"ctx->gpr[8] = vm_read32(ctx->gpr[12] + 0x5BC);\n";
        if count(&c.src[sp.0..sp.2], read) == 1 {
            let fun = replace_first(
                    &c.src[sp.0..sp.2],
                    read,
                    b"ctx->gpr[8] = racacit_armor_look(vm_read32(ctx->gpr[12] + 0x5BC));   /* racacit */\n",
                );
            c.set_fn(sp, &fun);
            c.declare(
                "uint32_t racacit_armor_look",
                "extern \"C\" uint32_t racacit_armor_look(uint32_t armor);\n",
            )?;
            c.changed = true;
        }
    }

    // 17. The armor looks in the game's Extras > Skins menu (game/stubs.cpp): the skin list
    //     builder func_004A3DB4 and the EquipSkin command func_004A27F8.
    if let Some(sp) = c.span("func_004A3DB4")?
        && !c.fn_contains(sp, "racacit_skin_armor_rows")
    {
        let lab = "\nloc_004A4DB4:\n";
        let last = "        ctx->lr = 0x004A4EE8; func_0095D30C(ctx); DRAIN_TRAMPOLINE(ctx);\n";
        let fun = c.src[sp.0..sp.2].to_vec();
        if count(&fun, lab.as_bytes()) == 1 && count(&fun, last.as_bytes()) == 1 {
            let fun = replace_first(
                &fun,
                lab.as_bytes(),
                format!("{lab}        racacit_skin_armor_selected(ctx);   /* racacit */\n")
                    .as_bytes(),
            );
            let fun = replace_first(
                &fun,
                last.as_bytes(),
                format!("{last}        racacit_skin_armor_rows(ctx);   /* racacit */\n").as_bytes(),
            );
            c.set_fn(sp, &fun);
            c.declare(
                    "void racacit_skin_armor_rows",
                    "extern \"C\" void racacit_skin_armor_selected(ppu_context* ctx);\nextern \"C\" void racacit_skin_armor_rows(ppu_context* ctx);\n",
                )?;
            c.changed = true;
        }
    }
    if let Some(sp) = c.span("func_004A27F8")?
        && !c.fn_contains(sp, "racacit_equip_skin")
    {
        let parse = "        ctx->lr = 0x004A2B54; func_00906828(ctx); DRAIN_TRAMPOLINE(ctx);\n";
        if count(&c.src[sp.0..sp.2], parse.as_bytes()) == 1 {
            let fun = replace_first(
                    &c.src[sp.0..sp.2],
                    parse.as_bytes(),
                    format!("{parse}        ctx->gpr[3] = racacit_equip_skin((uint32_t)ctx->gpr[3]);   /* racacit */\n").as_bytes(),
                );
            c.set_fn(sp, &fun);
            c.declare(
                "uint32_t racacit_equip_skin",
                "extern \"C\" uint32_t racacit_equip_skin(uint32_t skin);\n",
            )?;
            c.changed = true;
        }
    }

    // 18. Field of view and camera distance (Settings > General > Camera; game/stubs.cpp),
    //     both on the follow camera, so cutscenes and other cameras keep the game's own.
    let hooks18: [(&str, &str, &str, &str); 5] = [
        (
            "func_0003E874",
            "racacit_follow_fov",
            "        { uint32_t tmp = vm_read32(ctx->gpr[3] + 0x1D4); float ftmp; memcpy(&ftmp, &tmp, 4); ctx->fpr[13] = ftmp; }\n",
            "        ctx->fpr[13] = racacit_follow_fov(ctx->fpr[13]);   /* racacit */\n",
        ),
        (
            "func_0022CDC0",
            "racacit_follow_fov_unscale",
            "        ctx->lr = 0x0022CE60; func_0003E874(ctx); DRAIN_TRAMPOLINE(ctx);\n",
            "        ctx->fpr[1] = racacit_follow_fov_unscale(ctx->fpr[1]);   /* racacit */\n",
        ),
        (
            "func_00042558",
            "racacit_follow_dist",
            "        { uint32_t tmp = vm_read32(ctx->gpr[31] + 0x1D0); float ftmp; memcpy(&ftmp, &tmp, 4); ctx->fpr[2] = ftmp; }\n        { uint32_t tmp = vm_read32(ctx->gpr[31] + 0x1CC); float ftmp; memcpy(&ftmp, &tmp, 4); ctx->fpr[1] = ftmp; }\n",
            "        ctx->fpr[2] = racacit_follow_dist(ctx->fpr[2]);   /* racacit */\n",
        ),
        (
            "func_0003E9F0",
            "racacit_follow_dist",
            "        { uint32_t tmp = vm_read32(ctx->gpr[3] + 0x1D0); float ftmp; memcpy(&ftmp, &tmp, 4); ctx->fpr[2] = ftmp; }\n",
            "        ctx->fpr[2] = racacit_follow_dist(ctx->fpr[2]);   /* racacit */\n",
        ),
        (
            "func_0022D058",
            "racacit_follow_dist_unscale",
            "        { uint32_t tmp = vm_read32(ctx->gpr[3] + 0x1CC); float ftmp; memcpy(&ftmp, &tmp, 4); ctx->fpr[1] = ftmp; }\n",
            "        ctx->fpr[1] = racacit_follow_dist_unscale(ctx->fpr[1]);   /* racacit */\n",
        ),
    ];
    for (name, hook, anchor, add) in hooks18 {
        let Some(sp) = c.span(name)? else { continue };
        if c.fn_contains(sp, &format!("{hook}(")) {
            continue;
        }
        let n = count(&c.src[sp.0..sp.2], anchor.as_bytes());
        if n != 1 {
            println!("patch_lift #18: {name}: anchor not found once ({n})");
            continue;
        }
        let fun = replace_first(
            &c.src[sp.0..sp.2],
            anchor.as_bytes(),
            format!("{anchor}{add}").as_bytes(),
        );
        c.set_fn(sp, &fun);
        c.declare(
            &format!("double {hook}("),
            &format!("extern \"C\" double {hook}(double d);\n"),
        )?;
        c.changed = true;
    }

    // 19. The game's owner bonuses (game/stubs.cpp racacit_bonus_owned / racacit_bonus_grant).
    if let Some(sp) = c.span("func_0029AFCC")?
        && !c.fn_contains(sp, "racacit_bonus_owned(")
    {
        let anchor = "        vm_write8(ctx->gpr[11] + 0x6, ctx->gpr[0]);\n";
        let n = count(&c.src[sp.0..sp.2], anchor.as_bytes());
        if n != 1 {
            println!("patch_lift #19: func_0029AFCC: anchor not found once ({n})");
        } else {
            let fun = replace_first(
                &c.src[sp.0..sp.2],
                anchor.as_bytes(),
                format!(
                    "{anchor}        racacit_bonus_owned((uint32_t)ctx->gpr[11]);   /* racacit */\n"
                )
                .as_bytes(),
            );
            c.set_fn(sp, &fun);
            c.declare(
                "void racacit_bonus_owned(",
                "extern \"C\" void racacit_bonus_owned(uint32_t flags);\n",
            )?;
            c.changed = true;
        }
    }
    if let Some(sp) = c.span("func_0029B1D8")?
        && !c.fn_contains(sp, "racacit_bonus_grant(")
    {
        let anchor = "        ctx->gpr[0] = vm_read64(ctx->gpr[1] + 0xA0);\n";
        let n = count(&c.src[sp.0..sp.2], anchor.as_bytes());
        if n != 1 {
            println!("patch_lift #19: func_0029B1D8: anchor not found once ({n})");
        } else {
            let fun = replace_first(
                    &c.src[sp.0..sp.2],
                    anchor.as_bytes(),
                    format!("        racacit_bonus_grant(vm_read32(ctx->gpr[30] + -0x7FC4), (uint32_t)ctx->gpr[30]);   /* racacit */\n{anchor}").as_bytes(),
                );
            c.set_fn(sp, &fun);
            c.declare(
                "void racacit_bonus_grant(",
                "extern \"C\" void racacit_bonus_grant(uint32_t player, uint32_t pool);\n",
            )?;
            c.changed = true;
        }
    }

    // 20. The cheat menu's lockout (game/stubs.cpp racacit_skillpoint_blocked).
    if let Some(sp) = c.span("func_0021EF30")?
        && !c.fn_contains(sp, "racacit_skillpoint_blocked(")
    {
        let fun = Chunk::after_head(
            &c.src[sp.0..sp.2],
            "func_0021EF30",
            "        if (racacit_skillpoint_blocked((uint32_t)ctx->gpr[3])) return;   /* racacit */\n",
        );
        c.set_fn(sp, &fun);
        c.declare(
            "int racacit_skillpoint_blocked(",
            "extern \"C\" int racacit_skillpoint_blocked(uint32_t id);\n",
        )?;
        c.changed = true;
    }

    // 21. Boot straight to the main menu (game/stubs.cpp racacit_front_end_cards and
    //     racacit_front_end_menu).
    if let Some(sp) = c.span("func_004C8898")?
        && !c.fn_contains(sp, "racacit_front_end_cards")
    {
        // The call's TOC restore holds the disc's own TOC, so the anchor matches any value.
        static ANCHOR: LazyLock<Regex> = LazyLock::new(|| {
            Regex::new(r"        ctx->ctr = \(uint32_t\)ctx->lr; ps3_indirect_call\(ctx\); DRAIN_TRAMPOLINE\(ctx\);\n        ctx->gpr\[2\] = 0x[0-9A-F]+ULL; /\*TOCFIX ld r2,N\(r1\)\*/\n").unwrap()
        });
        const NEXT: &[u8] = b"        ctx->gpr[6] = vm_read32(ctx->gpr[30] + -0x7F24);\n";
        let fun = c.src[sp.0..sp.2].to_vec();
        // (?=...) after each match: the next line must follow.
        let hits: Vec<(usize, usize)> = ANCHOR
            .find_iter(&fun)
            .filter(|m| fun[m.end()..].starts_with(NEXT))
            .map(|m| (m.start(), m.end()))
            .collect();
        let name_load = b"        ctx->gpr[4] = vm_read32(ctx->gpr[30] + -0x7F28);\n";
        if hits.len() == 1 && contains(&fun[..hits[0].0], name_load) {
            let cut = hits[0].1;
            let fun = splice(
                &fun,
                cut,
                cut,
                b"        racacit_front_end_cards(ctx);   /* racacit */\n",
            );
            c.set_fn(sp, &fun);
            c.declare(
                "void racacit_front_end_cards",
                "extern \"C\" void racacit_front_end_cards(ppu_context* ctx);\n",
            )?;
            c.changed = true;
        } else {
            println!(
                "patch_lift #21: func_004C8898: startMode SetVariable not found once ({})",
                hits.len()
            );
        }
    }
    if let Some(sp) = c.span("func_004C8898")?
        && !c.fn_contains(sp, "racacit_front_end_menu")
    {
        // The Invoke of "Init" is the last call before the epilogue.
        static INIT: LazyLock<Regex> = LazyLock::new(|| {
            Regex::new(r"        ctx->lr = 0x[0-9A-F]+; func_[0-9A-F]{8}\(ctx\); DRAIN_TRAMPOLINE\(ctx\);\n        /\* nop \*/;\n").unwrap()
        });
        static NEXT: LazyLock<Regex> = LazyLock::new(|| {
            Regex::new(r"\Aloc_[0-9A-F]{8}:\n        ctx->gpr\[3\] = vm_read64\(ctx->gpr\[1\] \+ 0x140\);\n").unwrap()
        });
        let fun = c.src[sp.0..sp.2].to_vec();
        let hits: Vec<usize> = INIT
            .find_iter(&fun)
            .filter(|m| NEXT.is_match(&fun[m.end()..]))
            .map(|m| m.end())
            .collect();
        if hits.len() == 1 {
            let cut = hits[0];
            let fun = splice(
                &fun,
                cut,
                cut,
                b"        racacit_front_end_menu(ctx);   /* racacit */\n",
            );
            c.set_fn(sp, &fun);
            c.declare(
                "void racacit_front_end_menu",
                "extern \"C\" void racacit_front_end_menu(ppu_context* ctx);\n",
            )?;
            c.changed = true;
        } else {
            println!(
                "patch_lift #21: func_004C8898: Init call not found once ({})",
                hits.len()
            );
        }
    }

    // 23. Keyboard keycaps in the menus (game/menu_keycaps.h, game/menu_archive.cpp): before
    //     each frame of the menu movies' two frame paths, the bound key labels and the input
    //     mode go to the movie through its own string setter.
    for (entry, offsets) in [("func_004C6F6C", &[4][..]), ("func_004ADB3C", &[8, 4][..])] {
        let Some(sp) = c.span(entry)? else { continue };
        if c.fn_contains(sp, "racit_publish_menu_keyboard(") {
            continue;
        }
        let marker = format!("void {entry}(ppu_context* ctx) {{\n");
        if count(&c.src[sp.0..sp.2], marker.as_bytes()) != 1 {
            println!("patch_lift #23: {entry}: function start not found once");
            continue;
        }
        let calls: String = offsets
            .iter()
            .map(|o| format!("        racit_publish_menu_keyboard(ctx, {o});   /* racacit */\n"))
            .collect();
        let fun = Chunk::after_head(&c.src[sp.0..sp.2], entry, &calls);
        c.set_fn(sp, &fun);
        c.declare(
            "#include \"menu_keycaps.h\"",
            "#include \"menu_keycaps.h\"\n",
        )?;
        c.changed = true;
    }

    const OTHER_DISC: &str = "The function is in this lift but not the code update 1.20's port expects (another disc?): stopping rather than building without the fix.";

    // 24. Update 1.20's hero part masks in the disc build (game/stubs.cpp
    //     racacit_u120_part_masks). USA disc only.
    let sp = if c.uses_usa() {
        c.span("func_004C3C44")?
    } else {
        None
    };
    if let Some(sp) = sp
        && !c.fn_contains(sp, "racacit: 1.20 default")
    {
        let fun = c.src[sp.0..sp.2].to_vec();
        let li = b"        ctx->gpr[5] = (int64_t)(int32_t)(0);\n";
        let st = b"        vm_write32(ctx->gpr[1] + 0x78, ctx->gpr[5]);\n";
        let ok = count(&fun, li) == 1
            && count(&fun, st) == 1
            && count(&fun, b"vm_write32(ctx->gpr[1] + 0x78, ") == 3
            && find(&fun, li) < find(&fun, st)
            && !contains(
                &fun[find(&fun, li).unwrap() + li.len()..find(&fun, st).unwrap()],
                b"ctx->gpr[5] =",
            );
        if !ok {
            return Err(format!(
                "patch_lift #24: func_004C3C44: group 1 row 3 default not found once. {OTHER_DISC}"
            ));
        }
        let fun = replace_first(
            &fun,
            li,
            b"        ctx->gpr[5] = (int64_t)(int32_t)(3);   /* racacit: 1.20 default (was 0) */\n",
        );
        c.set_fn(sp, &fun);
        c.changed = true;
    }
    let sp = if c.uses_usa() {
        c.span("func_004C46FC")?
    } else {
        None
    };
    if let Some(sp) = sp
        && !c.fn_contains(sp, "racacit_u120_part_masks")
    {
        let fun = c.src[sp.0..sp.2].to_vec();
        let bound: &[u8] = b"        ctx->gpr[28] = vm_read32(ctx->gpr[9] + 0x0);\n        { uint64_t a = (uint32_t)ctx->gpr[28]; uint64_t b = (uint64_t)0x3;";
        let join: &[u8] = b"\nloc_004C4C68:\n";
        if !(count(&fun, bound) == 1
            && count(&fun, join) == 1
            && find(&fun, bound) < find(&fun, join))
        {
            return Err(format!(
                "patch_lift #24: func_004C46FC: row bound or mask join not found once. {OTHER_DISC}"
            ));
        }
        let new_bound = replace_first(
            bound,
            b"(uint64_t)0x3;",
            b"(uint64_t)0x4; /* racacit: 1.20 bound (was 3) */",
        );
        let fun = replace_first(&fun, bound, &new_bound);
        let fun = replace_first(
            &fun,
            join,
            &[
                join,
                b"        racacit_u120_part_masks(ctx);   /* racacit */\n",
            ]
            .concat(),
        );
        c.set_fn(sp, &fun);
        c.declare(
            "void racacit_u120_part_masks",
            "extern \"C\" void racacit_u120_part_masks(ppu_context* ctx);\n",
        )?;
        c.changed = true;
    }

    // 25. Update 1.20's level heap budget in the disc build (game/stubs.cpp
    //     racacit_u120_level_budget). USA disc only.
    let sp = if c.uses_usa() {
        c.span("func_0032C7A4")?
    } else {
        None
    };
    if let Some(sp) = sp
        && !c.fn_contains(sp, "racacit_u120_level_budget")
    {
        let fun = c.src[sp.0..sp.2].to_vec();
        let two: &[u8] = b"        ctx->gpr[0] = (int64_t)(int32_t)((uint32_t)0x74 << 16);\n        ctx->gpr[19] = (int64_t)(int32_t)((uint32_t)0x74 << 16);\n";
        if !(count(&fun, two) == 1 && count(&fun, b"0x74 << 16") == 2) {
            return Err(format!(
                "patch_lift #25: func_0032C7A4: level budget loads not found once. {OTHER_DISC}"
            ));
        }
        let fun = replace_first(
                &fun,
                two,
                b"        { const uint32_t b_ = racacit_u120_level_budget(ctx);   /* racacit: 1.20 level budget (was 0x740000) */\n          ctx->gpr[0] = (int64_t)(int32_t)b_; ctx->gpr[19] = (int64_t)(int32_t)b_; }\n",
            );
        c.set_fn(sp, &fun);
        c.declare(
            "uint32_t racacit_u120_level_budget",
            "extern \"C\" uint32_t racacit_u120_level_budget(ppu_context* ctx);\n",
        )?;
        c.changed = true;
    }

    // 26. Update 1.20's space-mission bits resync in the disc build (game/stubs.cpp
    //     racacit_u120_space_bits), at the disc function's only return. USA disc only.
    let sp = if c.uses_usa() {
        c.span("func_0047B7E8")?
    } else {
        None
    };
    if let Some(sp) = sp
        && !c.fn_contains(sp, "racacit_u120_space_bits")
    {
        let fun = c.src[sp.0..sp.2].to_vec();
        let tail: &[u8] = b"        ctx->gpr[1] = ctx->gpr[1] + (int64_t)(0x90);\n        return;";
        if !(count(&fun, tail) == 1
            && count(&fun, b"return;") == 1
            && contains(&fun, b"vm_write8(ctx->gpr[29] + 0x1B8, "))
        {
            return Err(format!(
                "patch_lift #26: func_0047B7E8: epilogue not found once. {OTHER_DISC}"
            ));
        }
        let fun = replace_first(
                &fun,
                tail,
                b"        ctx->gpr[1] = ctx->gpr[1] + (int64_t)(0x90);\n        racacit_u120_space_bits(ctx);   /* racacit: 1.20 mission bits resync */\n        return;",
            );
        c.set_fn(sp, &fun);
        c.declare(
            "void racacit_u120_space_bits",
            "extern \"C\" void racacit_u120_space_bits(ppu_context* ctx);\n",
        )?;
        c.changed = true;
    }

    // 27. Frame-rate unlock, the game clock (game/stubs.cpp racacit_clock_tick): at the head
    //     of func_0066A424 (Clock_Tick). USA disc only.
    let sp = if c.uses_usa() {
        c.span("func_0066A424")?
    } else {
        None
    };
    if let Some(sp) = sp
        && !c.fn_contains(sp, "racacit_clock_tick")
    {
        if !contains(&c.src[sp.1..sp.2], b"ppu_timebase_now()") {
            return Err("patch_lift #27: func_0066A424 does not read the timebase: not the game clock on this disc. \
                            Stopping rather than hooking the wrong function."
                    .into());
        }
        c.src = splice(
            &c.src,
            sp.1,
            sp.1,
            b"        racacit_clock_tick(ctx);   /* racacit: frame-rate unlock */\n",
        );
        c.declare(
            "void racacit_clock_tick",
            "extern \"C\" void racacit_clock_tick(ppu_context* ctx);\n",
        )?;
        c.changed = true;
    }

    // 28. Frame-rate unlock: code that counts frames as time (game/stubs.cpp
    //     racacit_frame60_at / racacit_new60). Counts are the disc's; another lift whose
    //     functions differ stops the build. USA disc only.
    if c.uses_usa() {
        frame60(c)?;
    }

    // 29. Frame-rate unlock, the CPU frame limiter (game/stubs.cpp racacit_frame_limit): at
    //     the head of func_005C4C00. USA disc only.
    let sp = if c.uses_usa() {
        c.span("func_005C4C00")?
    } else {
        None
    };
    if let Some(sp) = sp
        && !c.fn_contains(sp, "racacit_frame_limit")
    {
        let fun = &c.src[sp.1..sp.2];
        if !contains(fun, b"ctx->gpr[2] + -0x5120")
            || !contains(fun, b"ctx->gpr[30] + -0x7FF8")
            || !contains(fun, b"+ 0x2D8)")
        {
            return Err("patch_lift #29: func_005C4C00 does not read the display deadline: not the frame limiter \
                            on this disc. Stopping rather than hooking the wrong function."
                    .into());
        }
        c.src = splice(
            &c.src,
            sp.1,
            sp.1,
            b"        if (racacit_frame_limit(ctx)) return;   /* racacit: frame-rate unlock */\n",
        );
        c.declare(
            "int racacit_frame_limit",
            "extern \"C\" int racacit_frame_limit(ppu_context* ctx);\n",
        )?;
        c.changed = true;
    }

    // 30. Loading screens skip most of the frame limiter's wait (game/stubs.cpp
    //     racacit_loading_frame): in the level loader's screen frame func_0032DCC0, just
    //     before its one call of the frame end func_0052BFE0 (which runs the limiter), so an
    //     early return never leaves the mark for a later frame. USA disc only.
    let sp = if c.uses_usa() {
        c.span("func_0032DCC0")?
    } else {
        None
    };
    if let Some(sp) = sp
        && !c.fn_contains(sp, "racacit_loading_frame")
    {
        let fun = &c.src[sp.1..sp.2];
        let call = memchr::memmem::find_iter(fun, b"func_0052BFE0(ctx)").collect::<Vec<_>>();
        if call.len() != 1
            || !contains(fun, b"ctx->gpr[29] + 0x29")
            || !contains(fun, b"ppu_timebase_now()")
        {
            return Err(
                "patch_lift #30: func_0032DCC0 is not the loading screen frame on this disc. \
                            Stopping rather than hooking the wrong function."
                    .into(),
            );
        }
        let line = sp.1
            + fun[..call[0]]
                .iter()
                .rposition(|&b| b == b'\n')
                .map_or(0, |i| i + 1);
        c.src = splice(
            &c.src,
            line,
            line,
            b"        racacit_loading_frame();   /* racacit: fast loading screens */\n",
        );
        c.declare(
            "void racacit_loading_frame",
            "extern \"C\" void racacit_loading_frame(void);\n",
        )?;
        c.changed = true;
    }

    if (c.has("racacit_jb_push(") || c.has("racacit_longjmp(")) && !c.has("racacit: guest setjmp") {
        // declarations after the first include
        c.src = insert_after_include(&c.src, DECLS.as_bytes())?;
        c.changed = true;
    }
    Ok(())
}

const F60_LOADS: &[(&str, usize)] = &[
    ("func_000106FC", 1),
    ("func_000119A8", 1),
    ("func_00012F40", 2),
    ("func_00016B6C", 2),
    ("func_00016F14", 1),
    ("func_00018E54", 1),
    ("func_000190C0", 1),
    ("func_00019160", 3),
    ("func_0001BFD8", 15),
    ("func_0001F694", 1),
    ("func_0001FD68", 1),
    ("func_0001FE0C", 1),
    ("func_0001FF64", 2),
    ("func_00020DCC", 2),
    ("func_00022798", 1),
    ("func_00023DE4", 2),
];
const G60_GATES: &[(&str, usize)] = &[
    ("func_00023DE4", 2),
    ("func_00022798", 1),
    ("func_00012F40", 2),
    ("func_00019160", 2),
    ("func_00018E54", 1),
    ("func_00016B6C", 2),
    ("func_00016F14", 1),
    ("func_0001BFD8", 1),
    ("func_000BC018", 1),
    ("func_00148DA4", 1),
    ("func_0010DDB8", 1),
    ("func_00169EDC", 1),
    ("func_002B7828", 1),
    ("func_002B9B88", 1),
    ("func_002BF4F0", 1),
    ("func_00384858", 1),
    ("func_00385E78", 1),
    ("func_004735E8", 1),
    ("func_0046DB90", 1),
];

static F60_LOAD: LazyLock<Regex> =
    LazyLock::new(|| Regex::new(r"vm_read32\(ctx->gpr\[(\d+)\] \+ 0xD0\)").unwrap());
static F60_ANY: LazyLock<Regex> = LazyLock::new(|| {
    Regex::new(r"(?:vm_read32\(ctx->gpr\[\d+\] \+ 0xD0\)|racacit_frame60_at\(\(uint32_t\)ctx->gpr\[\d+\]\))").unwrap()
});
static F60_BR: LazyLock<Regex> = LazyLock::new(|| {
    Regex::new(r"^\s*if \(\(!\(\(ctx->cr >> \d+\) & [12]\)\)\) goto loc_[0-9A-F]+;$").unwrap()
});
static F60_MASK: LazyLock<Regex> =
    LazyLock::new(|| Regex::new(r"& 0x[0-9A-F]+\b|ppc_rlwinm|ppc_rlw64").unwrap());

fn frame60_sub(line: &[u8]) -> Vec<u8> {
    F60_LOAD
        .replace_all(line, |m: &Captures| {
            [&b"racacit_frame60_at((uint32_t)ctx->gpr["[..], &m[1], b"])"].concat()
        })
        .into_owned()
}

fn frame60(c: &mut Chunk) -> Result<()> {
    let names: BTreeSet<&str> = F60_LOADS.iter().chain(G60_GATES).map(|(n, _)| *n).collect();
    for fname in names {
        let Some(sp) = c.span(fname)? else { continue };
        if c.fn_contains(sp, "racacit_new60") || c.fn_contains(sp, "racacit_frame60_at") {
            continue;
        }
        let mut lines: Vec<Vec<u8>> = c.src[sp.0..sp.2]
            .split(|&b| b == b'\n')
            .map(<[u8]>::to_vec)
            .collect();
        if let Some(&(_, want)) = G60_GATES.iter().find(|(n, _)| *n == fname) {
            let mut gates = 0;
            for k in 0..lines.len() {
                if !F60_ANY.is_match(&lines[k]) {
                    continue;
                }
                for j in k + 1..(k + 9).min(lines.len()) {
                    if !F60_BR.is_match(&lines[j]) {
                        continue;
                    }
                    let between = lines[k..j].join(&b'\n');
                    if contains(&between, b"/_b") || F60_MASK.is_match(&between) {
                        lines[j] =
                            replace_first(&lines[j], b") goto ", b" || !racacit_new60()) goto ");
                        lines[k] = frame60_sub(&lines[k]);
                        gates += 1;
                    }
                    break;
                }
            }
            if gates != want {
                return Err(format!(
                    "patch_lift #28: {fname}: {gates} frame gates, expected {want} -- not the disc's function"
                ));
            }
        }
        let mut fun = lines.join(&b'\n');
        if let Some(&(_, want)) = F60_LOADS.iter().find(|(n, _)| *n == fname) {
            let n = F60_LOAD.find_iter(&fun).count() + count(&fun, b"racacit_frame60_at(");
            if n != want {
                return Err(format!(
                    "patch_lift #28: {fname}: {n} frame-counter loads, expected {want} -- not the disc's function"
                ));
            }
            fun = frame60_sub(&fun);
        }
        c.set_fn(sp, &fun);
        for decl in [
            "extern \"C\" uint32_t racacit_frame60_at(uint32_t base);",
            "extern \"C\" int racacit_new60(void);",
        ] {
            if !c.has(decl) {
                c.src = insert_after_include(&c.src, format!("{decl}\n").as_bytes())?;
            }
        }
        c.changed = true;
    }
    Ok(())
}

// ---- 22. the mouse cameras -----------------------------------------------------------------

// 22. The mouse turns every camera the right stick turns, directly (game/desktop_mouse.cpp;
//     public PRs #34 and #66, petralithic). Each camera's update takes the mouse motion since
//     the last camera update (radians, used once) and adds it to that camera's own yaw and
//     pitch step, after the stick's: a controller keeps working with the mouse captured.
//
//     MOUSE_CAMERAS is the recipe per camera, with places relative to the function's start so
//     one recipe serves every executable; mouse_camera_funcs says where each camera's update
//     is in each profile. An edit is Before(offset, code): before the call returning to
//     start+offset; After(offset, code): after it; Text(line, code): before that line. Signs
//     follow "the mouse is the right stick with the game's invert settings off". The build
//     stops if any camera of the profile is missing a hook.
#[derive(Clone, Copy)]
enum Edit {
    Before(u32, &'static str),
    After(u32, &'static str),
    Text(&'static str, &'static str),
}
use Edit::{After, Before, Text};

const PEEK: &str = "peek";

const MOUSE_CAMERAS: &[(&str, &[Edit])] = &[
    // Follow camera (the default third-person one). f1 is the yaw step into the rotation;
    // f31 the pitch target, kept within the game's own limits ([r30-0x7F24], [r30-0x7F1C]).
    (
        "follow",
        &[
            Before(0x398, "ctx->fpr[1] -= mouse_angles[0];"),
            Text(
                "ctx->fpr[3] = ppu_fp_single(ppu_fsub(ctx->fpr[31], ctx->fpr[1]));",
                "{ uint32_t lb = vm_read32(ctx->gpr[30] - 0x7F24), hb = vm_read32(ctx->gpr[30] - 0x7F1C);\n          float lo, hi; memcpy(&lo, &lb, 4); memcpy(&hi, &hb, 4);\n          const double target = ctx->fpr[31] + mouse_angles[1] - ctx->fpr[1];\n          ctx->fpr[31] = fmin(fmax(target, lo), hi) + ctx->fpr[1]; }",
            ),
        ],
    ),
    // Two free-look cameras: yaw then pitch step, each into its own rotation.
    (
        "look",
        &[
            Before(0x264, "ctx->fpr[1] -= mouse_angles[0];"),
            Before(0x280, "ctx->fpr[1] += mouse_angles[1];"),
        ],
    ),
    (
        "look2",
        &[
            Before(0x270, "ctx->fpr[1] -= mouse_angles[0];"),
            Before(0x294, "ctx->fpr[1] += mouse_angles[1];"),
        ],
    ),
    // An orbit camera: the step returned for the yaw and the pitch angle.
    (
        "orbit",
        &[
            After(0x270, "ctx->fpr[1] -= mouse_angles[0];"),
            After(0x2D4, "ctx->fpr[1] -= mouse_angles[1];"),
        ],
    ),
    // A camera that reads the stick with its axes swapped: pitch first, then yaw.
    (
        "free",
        &[
            Before(0x3D8, "ctx->fpr[1] -= mouse_angles[1];"),
            Before(0x3F8, "ctx->fpr[1] += mouse_angles[0];"),
        ],
    ),
    // Strafe (L2, over the shoulder): yaw and pitch steps, which the game then limits.
    (
        "strafe",
        &[
            After(0x300, "ctx->fpr[1] -= mouse_angles[0];"),
            After(0x3A4, "ctx->fpr[1] += mouse_angles[1];"),
        ],
    ),
    // The lock camera (class 0xC239BC): the yaw step, and its tilt, which follows the stick's
    // vertical axis and springs back; the mouse's pitch goes into that input right before the
    // game's own gate picks it.
    (
        "lock",
        &[
            Text(
                "ctx->fpr[12] = (ctx->fpr[1] >= 0.0) ? ctx->fpr[13] : ctx->fpr[0];",
                "ctx->fpr[13] = (float)fmin(fmax(ctx->fpr[13] - mouse_angles[1] * 20.0, -1.0), 1.0);",
            ),
            Before(0x4C8, "ctx->fpr[1] -= mouse_angles[0];"),
        ],
    ),
    // First-person look (hero state): the yaw step, and the pitch before the game clamps it.
    (
        "hero_look",
        &[
            Before(0x228, "ctx->fpr[1] -= mouse_angles[0];"),
            Text(
                "ctx->fpr[6] = ppu_fp_single(ppu_fadd(ctx->fpr[31], ctx->fpr[8]));",
                "ctx->fpr[31] += mouse_angles[1];",
            ),
        ],
    ),
    // Turrets: yaw and pitch steps into the turret's own limited turns.
    (
        "turret",
        &[
            Before(0x110, "ctx->fpr[1] -= mouse_angles[0];"),
            Before(0x11C, "ctx->fpr[1] += mouse_angles[1];"),
        ],
    ),
    // The camera that hands back to the default one when the stick moves: a mouse move
    // counts as that push. It only looks; the camera it hands to takes the motion.
    ("handback", &[After(0x9C, PEEK)]),
];

const DISC_CAMERAS: &[(&str, &str)] = &[
    ("follow", "func_000459D4"),
    ("look", "func_0003DD98"),
    ("look2", "func_00044050"),
    ("orbit", "func_00047104"),
    ("strafe", "func_0004833C"),
    ("free", "func_00048B38"),
    ("lock", "func_00041E34"),
    ("hero_look", "func_001AFF7C"),
    ("turret", "func_0037F18C"),
    ("handback", "func_00049450"),
];
// Update 1.20 (tools/racit_update120_fndiff.py): written as numbers so retarget leaves them.
const UPDATE120_CAMERAS: &[(&str, u32)] = &[
    ("follow", 0x459d4),
    ("look", 0x3dd98),
    ("look2", 0x44050),
    ("orbit", 0x47104),
    ("strafe", 0x4833c),
    ("free", 0x48b38),
    ("lock", 0x41e34),
    ("hero_look", 0x1aff80),
    ("turret", 0x37f4c4),
    ("handback", 0x49450),
];

fn mouse_camera_funcs(profile_id: &str) -> Option<BTreeMap<&'static str, String>> {
    match profile_id {
        "BCUS98124-1.00" | "BCES00511-1.00" => Some(
            DISC_CAMERAS
                .iter()
                .map(|(n, f)| (*n, f.to_string()))
                .collect(),
        ),
        "BCUS98124-1.20" => Some(
            UPDATE120_CAMERAS
                .iter()
                .map(|(n, a)| (*n, format!("func_{a:08X}")))
                .collect(),
        ),
        _ => None,
    }
}

fn camera_names() -> BTreeSet<&'static str> {
    MOUSE_CAMERAS.iter().map(|(n, _)| *n).collect()
}

const MOUSE_DECLS: &str = "extern \"C\" int racit_camera_turn(const char*, double*, unsigned long long);\nextern \"C\" int racit_camera_peek(unsigned long long);\nextern \"C\" unsigned long long SDL_GetPerformanceCounter(void);\nextern \"C\" unsigned long long SDL_GetPerformanceFrequency(void);\nstatic inline unsigned long long racit_mouse_now(void) { return (unsigned long long)((double)SDL_GetPerformanceCounter() * 1000000000.0 / (double)SDL_GetPerformanceFrequency()); }\n";

// What v1.0.0 put into the follow camera (func_000459D4) before this recipe: patch_lift step
// 22 on the disc, the update 1.20 recipe on 1.20. A lift patched then still has it, and the
// recipe would declare mouse_angles a second time, so it comes out first.
const OLD_FOLLOW_MOUSE: &[&str] = &[
    // The disc's step 22.
    "        double mouse_angles[2] = {};   /* racacit */\n        const int mouse_direct = racit_camera_take(mouse_angles, (uint64_t)((double)SDL_GetPerformanceCounter() * 1000000000.0 / (double)SDL_GetPerformanceFrequency()));\n",
    "        if (mouse_direct) ctx->fpr[1] -= mouse_angles[0];   /* racacit: added to the stick turn (PR #66) */\n",
    "        if (mouse_direct) {   /* racacit */\n            uint32_t lb = vm_read32(ctx->gpr[30] - 0x7F24), hb = vm_read32(ctx->gpr[30] - 0x7F1C);\n            float lo, hi; memcpy(&lo, &lb, 4); memcpy(&hi, &hb, 4);\n            const double target = ctx->fpr[31] + mouse_angles[1] - ctx->fpr[1];\n            ctx->fpr[31] = fmin(fmax(target, lo), hi) + ctx->fpr[1];\n        }\n",
    // Update 1.20's recipe (versions/BCUS98124-1.20-hooks.json in v1.0.0).
    "        double mouse_angles[2] = {};\n        const int mouse_direct = racit_camera_take(mouse_angles,(uint64_t)((double)SDL_GetPerformanceCounter()*1000000000.0/(double)SDL_GetPerformanceFrequency()));\n",
    "        if (mouse_direct) {\n            uint32_t lb=vm_read32(ctx->gpr[30]-0x7F24), hb=vm_read32(ctx->gpr[30]-0x7F1C);\n            float lo,hi; memcpy(&lo,&lb,4); memcpy(&hi,&hb,4);\n            /* Clamp before the original angle wrapping so a fast vertical\n             * swipe cannot jump across the pole or wrap to the other limit. */\n            const double target=mouse_angles[1]-ctx->fpr[1];\n            ctx->fpr[31]=fmin(fmax(target,lo),hi)+ctx->fpr[1];\n        }\n",
    "        if (mouse_direct) ctx->fpr[1] = -mouse_angles[0];\n",
    // 494cfa6 and 3efe71f (before 1.0.0): what the shipped v1.0.0 USA Windows exes were built from.
    "        double mouse_angles[2] = {};\n        const int mouse_direct = racit_camera_take(mouse_angles, SDL_GetPerformanceCounter() * (1000000000.0 / SDL_GetPerformanceFrequency()));\n",
    "        if (mouse_direct) {\n            uint32_t lb=vm_read32(ctx->gpr[30]-0x7F24), hb=vm_read32(ctx->gpr[30]-0x7F1C);\n            float lo,hi; memcpy(&lo,&lb,4); memcpy(&hi,&hb,4);\n            const double target=mouse_angles[1]-ctx->fpr[1];\n            ctx->fpr[31]=fmin(fmax(target,lo),hi)+ctx->fpr[1];\n        }\n",
];
const OLD_MOUSE_DECL: &str = "extern \"C\" int racit_camera_take(double*, unsigned long long);\n";
// 494cfa6 also declared SDL's counter beside it.
const OLD_SDL_DECL: &str = "extern \"C\" unsigned long long SDL_GetPerformanceCounter(void);\nextern \"C\" unsigned long long SDL_GetPerformanceFrequency(void);\n";

const MOUSE_HEAD: &str = "        double mouse_angles[2] = {0, 0}; int mouse_taken = 0, mouse_direct = 0;   /* racacit */\n";

/// fn without v1.0.0's follow camera mouse patch (None when it has none).
fn strip_old_follow_mouse(fun: &[u8], func: &str) -> Result<Option<Vec<u8>>> {
    if !contains(fun, b"racit_camera_take") {
        return Ok(None);
    }
    let mut f = fun.to_vec();
    for block in OLD_FOLLOW_MOUSE {
        f = replace_first(&f, block.as_bytes(), b"");
    }
    if contains(&f, b"racit_camera_take")
        || contains(&f, b"mouse_direct")
        || contains(&f, b"mouse_angles")
    {
        return Err(format!(
            "patch_lift #22: {func} holds a mouse patch this script does not know; regenerate the lift (scripts/pipeline.sh)"
        ));
    }
    println!("patch_lift #22: {func}: the v1.0.0 follow camera patch taken out");
    Ok(Some(f))
}

/// fn with one edit of camera name applied, or None when its place is not there once.
fn mouse_camera_edit(fun: &[u8], start: u32, name: &str, edit: Edit) -> Option<Vec<u8>> {
    let code_of = |code: &str| {
        if code == PEEK {
            format!(
                "if (racit_camera_peek(racit_mouse_now())) ctx->fpr[1] = 1.0;   /* racacit mouse: {name} */\n"
            )
        } else {
            format!(
                "if (!mouse_taken) {{ mouse_taken = 1; mouse_direct = racit_camera_turn(\"{name}\", mouse_angles, racit_mouse_now()); }}\n        if (mouse_direct) {code}   /* racacit mouse: {name} */\n"
            )
        }
    };
    let (places, code): (Vec<usize>, String) = match edit {
        Text(line, code) => {
            let re = Regex::new(&format!(r"(?m)^ *{}\n", regex::escape(line))).unwrap();
            (
                re.find_iter(fun).map(|m| m.start()).collect(),
                code_of(code),
            )
        }
        Before(off, code) | After(off, code) => {
            let re = Regex::new(&format!(
                r"(?m)^ *ctx->lr = 0x{:08X}; func_[0-9A-F]{{8}}\(ctx\); DRAIN_TRAMPOLINE\(ctx\);\n",
                start + off
            ))
            .unwrap();
            let before = matches!(edit, Before(..));
            (
                re.find_iter(fun)
                    .map(|m| if before { m.start() } else { m.end() })
                    .collect(),
                code_of(code),
            )
        }
    };
    if places.len() != 1 {
        return None;
    }
    Some(splice(
        fun,
        places[0],
        places[0],
        format!("        {code}").as_bytes(),
    ))
}

/// fn without this script's own edits for camera name (and its mouse_angles line).
fn strip_camera_marks(fun: &[u8], name: &str) -> Vec<u8> {
    let mark = regex::escape(&format!("/* racacit mouse: {name} */"));
    let a = Regex::new(&format!(
        r"(?ms)^        if \(!mouse_taken\) \{{[^\n]*\n        if \(mouse_direct\) .*?{mark}\n"
    ))
    .unwrap();
    let b = Regex::new(&format!(
        r"(?m)^        if \(racit_camera_peek\([^\n]*{mark}\n"
    ))
    .unwrap();
    let f = a.replace_all(fun, &b""[..]).into_owned();
    let mut f = b.replace_all(&f, &b""[..]).into_owned();
    if !contains(&f, b"racacit mouse:") {
        f = replace_first(&f, MOUSE_HEAD.as_bytes(), b"");
    }
    f
}

fn recipe(name: &str) -> &'static [Edit] {
    MOUSE_CAMERAS
        .iter()
        .find(|(n, _)| *n == name)
        .map(|(_, e)| *e)
        .unwrap()
}

/// fn with camera name's recipe put in, or None when a place for it is missing.
fn add_camera_edits(fun: &[u8], func: &str, name: &str) -> Option<Vec<u8>> {
    let start = u32::from_str_radix(&func[5..], 16).ok()?;
    let mut f = fun.to_vec();
    for edit in recipe(name) {
        f = mouse_camera_edit(&f, start, name, *edit)?;
    }
    let looks = recipe(name)
        .iter()
        .any(|e| !matches!(e, Before(_, PEEK) | After(_, PEEK) | Text(_, PEEK)));
    if looks && !contains(&f, b"double mouse_angles[2]") {
        let head = format!("void {func}(ppu_context* ctx) {{\n");
        f = replace_first(
            &f,
            head.as_bytes(),
            format!("{head}{MOUSE_HEAD}").as_bytes(),
        );
    }
    Some(f)
}

/// Puts every camera's mouse hook into the lift; stops the build if one is missing.
fn apply_mouse_cameras(recomp: &Path, profile_id: &str) -> Result<()> {
    let funcs = mouse_camera_funcs(profile_id)
        .filter(|f| f.keys().copied().collect::<BTreeSet<_>>() == camera_names())
        .ok_or_else(|| {
            format!("patch_lift #22: no complete mouse camera table for {profile_id}")
        })?;
    let mut done: BTreeSet<&str> = BTreeSet::new();
    let mut failed: Vec<String> = Vec::new();
    for path in lifted_sources(recomp) {
        let mut src = racit_lift::read(&path)?;
        let names = function_names(&src);
        if !funcs.values().any(|f| names.contains(f.as_bytes())) {
            continue;
        }
        let mut changed = false;
        for (&name, func) in &funcs {
            let Some(sp) = func_span(&src, func)? else {
                continue;
            };
            let fun = src[sp.0..sp.2].to_vec();
            let mark = format!("/* racacit mouse: {name} */");
            if contains(&fun, mark.as_bytes()) {
                // Patched before: up to date when taking our edits out and putting this
                // recipe's in gives the same function; otherwise an earlier recipe is replaced.
                let bare = strip_camera_marks(&fun, name);
                if contains(&bare, mark.as_bytes()) {
                    failed.push(format!(
                        "{name} ({func}: an earlier patch this script cannot take out)"
                    ));
                    continue;
                }
                match add_camera_edits(&bare, func, name) {
                    Some(fresh) if fresh == fun => {
                        done.insert(name);
                    }
                    None => failed.push(format!("{name} ({func})")),
                    Some(fresh) => {
                        src = splice(&src, sp.0, sp.2, &fresh);
                        println!(
                            "patch_lift #22: {func}: the earlier {name} camera patch replaced"
                        );
                        done.insert(name);
                        changed = true;
                    }
                }
                continue;
            }
            let (mut sp, mut fun) = (sp, fun);
            if let Some(stripped) = strip_old_follow_mouse(&fun, func)? {
                src = splice(&src, sp.0, sp.2, &stripped);
                changed = true;
                sp = func_span(&src, func)?.unwrap();
                fun = src[sp.0..sp.2].to_vec();
            }
            match add_camera_edits(&fun, func, name) {
                None => failed.push(format!("{name} ({func})")),
                Some(f) => {
                    src = splice(&src, sp.0, sp.2, &f);
                    done.insert(name);
                    changed = true;
                }
            }
        }
        if changed {
            src = replace_all(
                &src,
                format!("{OLD_MOUSE_DECL}{OLD_SDL_DECL}").as_bytes(),
                b"",
            );
            src = replace_all(&src, OLD_MOUSE_DECL.as_bytes(), b"");
            if !contains(&src, b"racit_mouse_now(void)") {
                src = insert_after_include(&src, MOUSE_DECLS.as_bytes())?;
            }
            racit_lift::write(&path, &src)?;
        }
    }
    let missing: Vec<&str> = camera_names()
        .into_iter()
        .filter(|n| !done.contains(n))
        .collect();
    if !failed.is_empty() || !missing.is_empty() {
        let mut all = failed.clone();
        for m in &missing {
            if !failed.iter().any(|f| f.starts_with(&format!("{m} "))) {
                all.push(format!("{m} ({} not found)", funcs[m]));
            }
        }
        return Err(format!(
            "patch_lift #22: mouse camera hooks missing for {profile_id}: {}",
            all.join(", ")
        ));
    }
    println!(
        "patch_lift #22: mouse hooks in all {} cameras ({profile_id})",
        done.len()
    );
    Ok(())
}

/// The recipe as tests/test_mouse_repatch.py reads it: MOUSE_CAMERAS ([kind, where, code]
/// per edit), _DISC_CAMERAS, UPDATE120_CAMERAS, OLD_FOLLOW_MOUSE and OLD_MOUSE_DECL.
fn mouse_recipe_json() -> String {
    use serde_json::{Map, Value, json};
    let mut cameras = Map::new();
    for (name, edits) in MOUSE_CAMERAS {
        let rows: Vec<Value> = edits
            .iter()
            .map(|e| match *e {
                Before(o, c) => json!(["before", o, c]),
                After(o, c) => json!(["after", o, c]),
                Text(w, c) => json!(["text", w, c]),
            })
            .collect();
        cameras.insert(name.to_string(), Value::Array(rows));
    }
    let table = |id: &str| -> Value {
        Value::Object(
            mouse_camera_funcs(id)
                .unwrap()
                .into_iter()
                .map(|(k, v)| (k.to_string(), Value::String(v)))
                .collect(),
        )
    };
    json!({
        "MOUSE_CAMERAS": cameras,
        "_DISC_CAMERAS": table("BCUS98124-1.00"),
        "UPDATE120_CAMERAS": table("BCUS98124-1.20"),
        "OLD_FOLLOW_MOUSE": OLD_FOLLOW_MOUSE,
        "OLD_MOUSE_DECL": OLD_MOUSE_DECL,
    })
    .to_string()
}

/// Every known profile (versions/profiles.json) has a place for every mouse camera.
fn check_mouse_camera_tables(root: &Path) -> Result<()> {
    let bad: Vec<String> = profile::load(root)?
        .into_iter()
        .filter(|p| {
            mouse_camera_funcs(&p.id).map(|f| f.keys().copied().collect::<BTreeSet<_>>())
                != Some(camera_names())
        })
        .map(|p| p.id)
        .collect();
    if !bad.is_empty() {
        return Err(format!(
            "patch_lift #22: mouse camera table incomplete for {}",
            bad.join(", ")
        ));
    }
    Ok(())
}

// ---- 13. an older lift's SCREAM synth hooks out ----------------------------------------------

/// v42-v62 hooked the SCREAM synth to copy the controller's sounds out of its mix. The
/// controller has its own player now: an older patched lift has those hook calls removed.
fn unpatch_spu(spu_dir: &Path) -> Result<()> {
    static HOOK: LazyLock<Regex> =
        LazyLock::new(|| Regex::new(r"\bracacit_syn_(voice|mix|mix4)_hook\b").unwrap());
    for lift in ["sub_BFD480_04000", "spu_0001_at_00BED400"] {
        let p = spu_dir.join(lift).join("spu_recomp.c");
        if !p.exists() {
            continue;
        }
        let src = racit_lift::read(&p)?;
        let lines: Vec<&[u8]> = src.split(|&b| b == b'\n').collect();
        let out: Vec<&[u8]> = lines
            .iter()
            .copied()
            .filter(|l| !HOOK.is_match(l))
            .collect();
        if out.len() != lines.len() {
            racit_lift::write(&p, &out.join(&b'\n'))?;
            println!("unpatched {lift}");
        }
    }
    Ok(())
}

// ---- the command line ------------------------------------------------------------------------

fn check_game_profile(
    root: &Path,
    elf: &Path,
    selected: &str,
    require_buildable: bool,
    quiet: bool,
) -> Result<profile::Profile> {
    let actual = profile::identify(root, elf, selected, require_buildable)
        .map_err(|e| format!("{e}. Generation stopped."))?;
    if !quiet {
        println!(
            "Game profile: {} (SHA-256 {})",
            actual.id, actual.elf_sha256
        );
    }
    Ok(actual)
}

fn patch_disc(root: &Path, recomp: &Path, profile_id: &str) -> Result<()> {
    // The disc this tree builds: scripts/retarget.py marks its copy's profile buildable.
    let profiles = profile::load(root)?;
    let build_title = profiles
        .iter()
        .find(|p| p.buildable)
        .map(|p| p.title_id.clone())
        .unwrap_or_else(|| "BCUS98124".into());
    let profile_id = if profile_id.is_empty() {
        format!("{build_title}-1.00")
    } else {
        profile_id.to_string()
    };

    let sources = lifted_sources(recomp);
    // The chunks are independent: patch them in parallel, report in order.
    let results: Vec<Result<bool>> = racit_lift::parallel_map(&sources, 0, |path| {
        let src = racit_lift::read(path)?;
        let names = function_names(&src);
        let mut c = Chunk {
            src,
            names,
            changed: false,
            build_title: &build_title,
        };
        patch_chunk(&mut c).map_err(|e| format!("{}: {e}", base(path)))?;
        if c.changed {
            racit_lift::write(path, &c.src)?;
        }
        Ok(c.changed)
    });
    let mut n = 0;
    for (path, r) in sources.iter().zip(results) {
        if r? {
            println!("patched {}", base(path));
            n += 1;
        }
    }
    println!("{n} file(s) patched");

    check_mouse_camera_tables(root)?;
    apply_mouse_cameras(recomp, &profile_id)?;
    let spu = std::env::var_os("RACACIT_SPU_DIR")
        .map(PathBuf::from)
        .unwrap_or_else(|| root.join("game/spu"));
    unpatch_spu(&spu)?;
    // 11. Vector instructions (src/vmx.rs). Its errors did not stop a disc build before.
    if let Err(e) = vmx::run(recomp, false) {
        eprintln!("{e}");
    }
    required_hooks(root, recomp)
}

/// scripts/retarget.py's copy for another disc lists every hook the USA lift gets in
/// versions/required-hooks.txt: on that disc a wrong address must stop the build.
fn required_hooks(root: &Path, recomp: &Path) -> Result<()> {
    let list = root.join("versions/required-hooks.txt");
    let Ok(text) = std::fs::read_to_string(&list) else {
        return Ok(());
    };
    let required: Vec<&str> = text
        .lines()
        .map(|l| l.split('#').next().unwrap_or("").trim())
        .filter(|l| !l.is_empty())
        .collect();
    let mut found = vec![false; required.len()];
    for path in lifted_sources(recomp) {
        let src = racit_lift::read(&path)?;
        for (i, r) in required.iter().enumerate() {
            found[i] |= contains(&src, r.as_bytes());
        }
    }
    let missing: Vec<&str> = required
        .iter()
        .zip(&found)
        .filter(|(_, f)| !**f)
        .map(|(r, _)| *r)
        .collect();
    if !missing.is_empty() {
        return Err(format!(
            "patch_lift: hooks not applied: {}",
            missing.join(", ")
        ));
    }
    println!("patch_lift: all {} hooks applied", required.len());
    Ok(())
}

fn usage() -> String {
    "usage: patch-lift [RECOMP] [--elf EBOOT.ELF] [--profile PROFILE] [--root RACIT]".into()
}

fn run() -> Result<()> {
    let mut args: Vec<String> = std::env::args().skip(1).collect();
    let mut root = racit_lift::default_root();
    if let Some(i) = args.iter().position(|a| a == "--root") {
        let v = args.get(i + 1).ok_or("--root needs a value")?.clone();
        root = PathBuf::from(v);
        args.drain(i..i + 2);
    }
    match args.first().map(String::as_str) {
        Some("--check-profile") | Some("--identify-profile") => {
            let check = args[0] == "--check-profile";
            if !(2..=3).contains(&args.len()) {
                return Err(format!(
                    "usage: patch-lift {} EBOOT.ELF [PROFILE_ID|auto]",
                    args[0]
                ));
            }
            let selected = args.get(2).map(String::as_str).unwrap_or("auto");
            let p = check_game_profile(&root, Path::new(&args[1]), selected, check, !check)?;
            if !check {
                println!("{}", p.id);
            }
            return Ok(());
        }
        Some("--apply-update-hooks") => {
            if args.len() != 3 {
                return Err("usage: patch-lift --apply-update-hooks RECOMP EBOOT.ELF".into());
            }
            return update120::apply(&root, Path::new(&args[1]), Path::new(&args[2]));
        }
        // For tests/test_mouse_repatch.py: the mouse recipe, and the camera pass on its own.
        Some("--mouse-recipe") => {
            println!("{}", mouse_recipe_json());
            return Ok(());
        }
        Some("--mouse-cameras") => {
            if args.len() != 3 {
                return Err("usage: patch-lift --mouse-cameras RECOMP PROFILE".into());
            }
            return apply_mouse_cameras(Path::new(&args[1]), &args[2]);
        }
        Some("--fix-vmx") => {
            let recomp = args
                .get(1)
                .filter(|a| !a.starts_with("--"))
                .ok_or("usage: patch-lift --fix-vmx RECOMP [--check]")?;
            return vmx::run(Path::new(recomp), args.iter().any(|a| a == "--check"));
        }
        _ => {}
    }

    let (mut recomp, mut elf) = (None, None);
    let mut selected = std::env::var("RACACIT_GAME_PROFILE").unwrap_or_else(|_| "auto".into());
    let mut it = args.into_iter();
    while let Some(a) = it.next() {
        match a.as_str() {
            "--elf" => elf = Some(PathBuf::from(it.next().ok_or("--elf needs a value")?)),
            "--profile" => selected = it.next().ok_or("--profile needs a value")?,
            "-h" | "--help" => return Err(usage()),
            _ if a.starts_with("--") => {
                return Err(format!(
                    "{}\npatch-lift: error: unrecognized arguments: {a}",
                    usage()
                ));
            }
            _ if recomp.is_none() => recomp = Some(PathBuf::from(a)),
            _ => {
                return Err(format!(
                    "{}\npatch-lift: error: unrecognized arguments: {a}",
                    usage()
                ));
            }
        }
    }
    let recomp = recomp.unwrap_or_else(|| root.join("game/src/recomp"));
    let Some(elf) = elf else {
        if selected != "auto" {
            return Err(format!(
                "{}\npatch-lift: error: --profile requires --elf to verify the selected executable",
                usage()
            ));
        }
        return patch_disc(&root, &recomp, "");
    };
    let p = check_game_profile(&root, &elf, &selected, false, false)?;
    if p.id == "BCUS98124-1.20" {
        update120::apply(&root, &recomp, &elf)?;
        check_mouse_camera_tables(&root)?;
        apply_mouse_cameras(&recomp, &p.id)?;
        return vmx::run(&recomp, false);
    }
    // A disc 1.00: the USA one here, the European one in scripts/retarget.py's copy.
    if p.app_version != "1.00" || !p.buildable {
        return Err(format!("No PPU hook implementation for {}", p.id));
    }
    patch_disc(&root, &recomp, &p.id)
}

fn main() -> ExitCode {
    match run() {
        Ok(()) => ExitCode::SUCCESS,
        Err(msg) => {
            eprintln!("{msg}");
            ExitCode::from(1)
        }
    }
}
