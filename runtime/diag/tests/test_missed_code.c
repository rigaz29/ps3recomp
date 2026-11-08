/*
 * The missed-code recorder on its own: the hooks are what spu_interp.c, spu_channels.c and
 * ppu_loader.cpp would hold. tools/test_missed_code.py runs this twice on one file and reads it.
 */
#include "ps3emu/missed_code.h"

#include <stdint.h>
#include <stdio.h>

volatile long long* (*g_spu_missed_entry)(const void* ctx, uint32_t pc, int owner, const uint32_t sig[4]);
void (*g_spu_missed_get)(const void* ctx, uint32_t ea, uint32_t lsa, uint32_t size);
void (*g_ppu_missed_hook)(uint32_t addr, uint32_t lr, uint32_t word);

int main(int argc, char** argv)
{
    if (argc != 2) return 2;
    if (g_spu_missed_entry || g_spu_missed_get || g_ppu_missed_hook) return 3;   /* nothing until asked */
    ps3_missed_code_to(argv[1]);
    if (!g_spu_missed_entry || !g_spu_missed_get || !g_ppu_missed_hook) return 4;

    int ctx_a, ctx_b;
    static const uint32_t in_piece[4] = {0x11111111u, 0x22222222u, 0x33333333u, 0x44444444u};
    static const uint32_t no_get[4]   = {0x55555555u, 0x66666666u, 0x77777777u, 0x88888888u};
    static const uint32_t no_steps[4] = {0x99999999u, 0xAAAAAAAAu, 0xBBBBBBBBu, 0xCCCCCCCCu};

    g_spu_missed_get(&ctx_a, 0x00755080u, 0x11000u, 2816u);
    /* Out of the heap, too small, and another context's: none of them is the GET that brought the code. */
    g_spu_missed_get(&ctx_a, 0x20000000u, 0x11000u, 64u);
    g_spu_missed_get(&ctx_a, 0x00600000u, 0x11000u, 8u);
    g_spu_missed_get(&ctx_b, 0x00700000u, 0x11000u, 64u);

    volatile long long* steps = g_spu_missed_entry(&ctx_a, 0x11018u, 2, in_piece);
    if (!steps) return 5;
    *steps += 100;
    steps = g_spu_missed_entry(&ctx_a, 0x20000u, -1, no_get);
    if (!steps) return 6;
    *steps += 7;
    if (!g_spu_missed_entry(&ctx_a, 0x30000u, 2, no_steps)) return 7;   /* given straight back */

    g_ppu_missed_hook(0x00123450u, 0x00123000u, 0x60000000u);
    ps3_missed_code_flush();
    return 0;
}
