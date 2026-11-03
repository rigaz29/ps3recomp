/*
 * The four-lane redo of the RZ float forms (spu__x8_fa_redo, spu__x8_fm_redo, spu__x8_fma_redo:
 * double precision under the host's rounding toward zero) against the exact lane-by-lane forms
 * (spu__fa_any, spu__fm_any, spu__fma_any), on random singles weighted toward the cases the redo
 * exists for: biased exponent 0 and 255, the smallest and largest exponents, the all-ones words
 * titles use as masks, sums that cancel.
 *
 *     test_spu_float_redo [vectors]
 */
#include "spu_context.h"
#include "spu_helpers.h"
#include <immintrin.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

static uint64_t s_rng = 0x9E3779B97F4A7C15ull;
static uint32_t rnd(void)
{
    s_rng ^= s_rng << 13; s_rng ^= s_rng >> 7; s_rng ^= s_rng << 17;
    return (uint32_t)(s_rng >> 16);
}
static uint32_t pick(void)
{
    const uint32_t s = rnd() & 0x80000000u, f = rnd() & 0x7FFFFFu;
    switch (rnd() % 10) {
    case 0: return s | f;                                     /* zero, "denormal" */
    case 1: return s | (255u << 23) | f;                      /* extended range */
    case 2: return s | ((248u + rnd() % 8) << 23) | f;        /* near the top */
    case 3: return s | ((1u + rnd() % 8) << 23) | f;          /* near the bottom */
    case 4: return s | ((120u + rnd() % 16) << 23) | f;
    case 5: return (rnd() & 1) ? 0xFFFFFFFFu : 0x7FFFFFFFu;    /* masks, the largest */
    case 6: return s | ((127u + rnd() % 4) << 23) | (rnd() & 1 ? 0 : f);
    default: return s | ((rnd() % 256u) << 23) | f;
    }
}

int main(int argc, char** argv)
{
    const long n = argc > 1 ? atol(argv[1]) : 2000000;
    _mm_setcsr(0xE040u | (_mm_getcsr() & 0x1F80u));            /* RZ, FTZ, DAZ; masks as they are */
    long bad[3] = { 0, 0, 0 };
    for (long i = 0; i < n; i++) {
        uint32_t x[4], y[4], z[4], ra[4], rm[4], rf[4];
        for (int k = 0; k < 4; k++) {
            x[k] = pick(); y[k] = pick(); z[k] = pick();
            if (rnd() % 8 == 0) y[k] = x[k] ^ 0x80000000u;            /* x + -x */
        }
        const __m128i X = _mm_loadu_si128((const __m128i*)x), Y = _mm_loadu_si128((const __m128i*)y);
        const __m128i Z = _mm_loadu_si128((const __m128i*)z);
        _mm_storeu_si128((__m128i*)ra, spu__x8_fa_redo(X, Y));
        _mm_storeu_si128((__m128i*)rm, spu__x8_fm_redo(X, Y));
        _mm_storeu_si128((__m128i*)rf, spu__x8_fma_redo(X, Y, Z));
        for (int k = 0; k < 4; k++) {
            const uint32_t wa = spu__fa_any(x[k], y[k]), wm = spu__fm_any(x[k], y[k]);
            const uint32_t wf = spu__fma_any(x[k], y[k], z[k], 0, 0);
            if (ra[k] != wa && bad[0]++ < 8)
                printf("fa  %08X + %08X: %08X, exact %08X\n", x[k], y[k], ra[k], wa);
            if (rm[k] != wm && bad[1]++ < 8)
                printf("fm  %08X * %08X: %08X, exact %08X\n", x[k], y[k], rm[k], wm);
            if (rf[k] != wf && bad[2]++ < 8)
                printf("fma %08X * %08X + %08X: %08X, exact %08X\n", x[k], y[k], z[k], rf[k], wf);
        }
    }
    printf("redo: %ld vectors, fa %ld differ, fm %ld differ, fma %ld differ\n", n, bad[0], bad[1], bad[2]);
    return (bad[0] || bad[1] || bad[2]) ? 1 : 0;
}
