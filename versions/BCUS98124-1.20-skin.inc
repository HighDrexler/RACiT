extern "C" void racacit_skin_armor_selected(ppu_context* ctx)
{
    s_skin_rows = (uint32_t)ctx->gpr[23];
    const int look = racacit_armor_look_now();
    if (!look) return;
    const uint32_t G = vm_read32((uint32_t)ctx->gpr[30] - 0x7FD8u);   /* the player record */
    if (!G || vm_read32(G + 0x5F0u) != 0) return;   /* a skin is worn: it shows, and the list marks it */
    const double sel = (double)(s_skin_rows + (uint32_t)look - 1u);
    uint64_t bits; memcpy(&bits, &sel, 8);
    const uint32_t sp = (uint32_t)ctx->gpr[1];
    vm_write32(sp + 0x80u, 3u);   /* a number */
    vm_write64(sp + 0x88u, bits);
}

/* After the list's last array: r27 = the movie, r30 = the menu's pool. The
 * entries sit below the guest stack pointer during the calls, and every
 * register is put back after them. */
extern "C" void racacit_skin_armor_rows(ppu_context* ctx)
{
    const uint32_t pool = (uint32_t)ctx->gpr[30], movie = (uint32_t)ctx->gpr[27], first = s_skin_rows;
    if (!movie || first > 64u) return;
    uint64_t gpr[32], lr = ctx->lr, ctr = ctx->ctr; double fpr[32]; u128 vr[32];
    const uint32_t cr = ctx->cr, xer = ctx->xer, fpscr = ctx->fpscr, vscr = ctx->vscr;
    memcpy(gpr, ctx->gpr, sizeof gpr); memcpy(fpr, ctx->fpr, sizeof fpr); memcpy(vr, ctx->vr, sizeof vr);
    const uint32_t sp = (uint32_t)ctx->gpr[1], nsp = (sp - 0x400u) & ~15u, data = nsp + 0x100u;
    vm_write32(nsp, 0); vm_write32(nsp + 4u, sp);   /* back chain */

    const uint32_t table = vm_read32(pool - 0x7F18u);   /* 0xDF07E0 */
    uint32_t name[4];
    for (uint32_t i = 0; i < 4; i++) {
        uint32_t tag = table ? vm_read32(table + 0x320u + (i + 1u) * 0x14u + 4u) : 0u;
        if (!tag) tag = 0xBAu + i;   /* "Holoflux Armor" .. "Hyperflux Armor" */
        ctx->gpr[1] = nsp; ctx->gpr[3] = tag; ctx->gpr[4] = 0; ctx->lr = 0x004A5F70u;
        func_00937880(ctx); racacit_guest_drain(ctx);
        const uint32_t t = (uint32_t)ctx->gpr[3];
        name[i] = t ? vm_read32(t + 8u) : vm_read32(pool - 0x7F14u);
    }
    /* skinNames, skinDescs, skinGB, skinUnlocked, skinImages, skinValues */
    static const uint32_t var[6] = { 0x7DF0u, 0x7DECu, 0x7DE8u, 0x7DE4u, 0x7DE0u, 0x7DDCu };
    for (uint32_t a = 0; a < 6; a++) {
        const uint32_t arr = data + a * 0x40u;
        for (uint32_t i = 0; i < 4; i++) {
            const uint32_t v = arr + i * 16u;   /* GFx value: +0 type, +8 the value */
            for (uint32_t k = 0; k < 16u; k += 4u) vm_write32(v + k, 0);
            if (a <= 1) { vm_write32(v, 4u); vm_write32(v + 8u, name[i]); continue; }   /* a string */
            if (a == 3) { vm_write32(v, 2u); vm_write8(v + 8u, 1); continue; }         /* true */
            static const uint32_t picture[4] = { 71u, 79u, 81u, 80u };   /* Holoflux .. Hyperflux */
            const double num = a == 4 ? (double)picture[i] : a == 5 ? (double)(101u + i) : 0.0;
            uint64_t bits; memcpy(&bits, &num, 8);
            vm_write32(v, 3u); vm_write64(v + 8u, bits);   /* a number */
        }
        ctx->gpr[1] = nsp; ctx->gpr[3] = movie; ctx->gpr[4] = vm_read32(pool - var[a]); ctx->gpr[5] = first;
        ctx->gpr[6] = arr; ctx->gpr[7] = 4; ctx->gpr[8] = 1; ctx->lr = 0x004A5F70u;
        func_0095D0FC(ctx); racacit_guest_drain(ctx);
    }

    memcpy(ctx->gpr, gpr, sizeof gpr); memcpy(ctx->fpr, fpr, sizeof fpr); memcpy(ctx->vr, vr, sizeof vr);
    ctx->cr = cr; ctx->lr = lr; ctx->ctr = ctr; ctx->xer = xer; ctx->fpscr = fpscr; ctx->vscr = vscr;
}
