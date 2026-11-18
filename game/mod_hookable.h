/* Functions code mods hook (docs/MODDING.md): never folded into a caller, so a
 * hook on one sees every call. Force-included into the lifted code (CMakeLists.txt).
 *
 * Add a line to make a function fully hookable in the next build:
 *     RACIT_HOOKABLE(0050A83C)   // what it is, who asked
 * Any other function can be hooked too; only the calls the compiler folded
 * into their caller (small functions, at the call sites it chose) skip it. */
#pragma once
#include "ppu_recomp.h"
#define RACIT_HOOKABLE(a) void func_##a(ppu_context* ctx) __attribute__((noinline));


/* Into the Nexus: none yet (RACiT's were A Crack in Time addresses). */
