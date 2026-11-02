/* rsx_index_copy_be (SSE4.1) against rsx_index_copy_be_scalar: random 16- and
 * 32-bit big-endian index arrays of every length 0..80 and some long ones, at
 * unaligned source and destination offsets. Same indices, same range. */
#include "rsx_index_copy.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint32_t rng = 0x12345678u;
static uint32_t rnd(void)
{
    rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5;
    return rng;
}

int main(void)
{
#if !RSX_INDEX_COPY_SIMD
    printf("SKIP: no SSE4.1 form on this target\n");
    return 0;
#else
    static uint8_t src[4 * 5000 + 64];
    static uint32_t a[5000 + 16], b[5000 + 16];
    unsigned cases = 0, bad = 0;
    for (int sixteen = 0; sixteen <= 1; sixteen++) {
        for (uint32_t trial = 0; trial < 4000; trial++) {
            const uint32_t count = trial < 81 * 4 ? trial / 4 : 81 + rnd() % 4900;
            const uint32_t so = rnd() % 16, dofs = rnd() % 4;
            /* Narrow ranges too, so lo/hi are not always the type's extremes. */
            const uint32_t span = (rnd() & 1) ? 0xFFFFFFFFu : 1u + rnd() % 1000;
            const uint32_t base = rnd();
            for (uint32_t k = 0; k < count; k++) {
                uint32_t v = span == 0xFFFFFFFFu ? rnd() : base + rnd() % span;
                if (sixteen) { v &= 0xFFFFu; src[so + k * 2] = (uint8_t)(v >> 8); src[so + k * 2 + 1] = (uint8_t)v; }
                else { src[so + k * 4] = (uint8_t)(v >> 24); src[so + k * 4 + 1] = (uint8_t)(v >> 16);
                       src[so + k * 4 + 2] = (uint8_t)(v >> 8); src[so + k * 4 + 3] = (uint8_t)v; }
            }
            memset(a, 0xAB, sizeof a); memset(b, 0xAB, sizeof b);
            uint32_t lo1, hi1, lo2, hi2;
            rsx_index_copy_be_scalar(src + so, count, sixteen, a + dofs, &lo1, &hi1);
            rsx_index_copy_be_sse41(src + so, count, sixteen, b + dofs, &lo2, &hi2);
            cases++;
            if (memcmp(a, b, sizeof a) || lo1 != lo2 || hi1 != hi2) {
                if (bad++ < 8)
                    printf("[index-copy] differ: sixteen=%d count=%u so=%u dofs=%u lo %08X/%08X hi %08X/%08X\n",
                           sixteen, count, so, dofs, lo1, lo2, hi1, hi2);
            }
        }
    }
    printf("[index-copy] %u cases, %u differ\n", cases, bad);
    return bad != 0;
#endif
}
