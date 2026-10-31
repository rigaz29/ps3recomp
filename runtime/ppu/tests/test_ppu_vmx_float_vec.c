/* The vector forms of vaddfp, vsubfp, vmaddfp and vnmsubfp (runtime/ppu/ppu_vmx.h,
 * FMA3 hosts) against the scalar reference forms, byte for byte: random lanes
 * drawn from ordinary values, zeros, denormals, infinities, quiet and signalling
 * NaNs with payloads and values near overflow, with VSCR[NJ] set and clear, and
 * with the destination aliasing a source. */
#include "ppu_vmx.h"
#include <stdio.h>
#include <string.h>
#include <stdint.h>

static uint64_t s_rng = 0x9E3779B97F4A7C15ull;
static uint32_t rnd(void)
{
    s_rng ^= s_rng << 13; s_rng ^= s_rng >> 7; s_rng ^= s_rng << 17;
    return (uint32_t)(s_rng >> 16);
}

static uint32_t lane(void)
{
    static const uint32_t special[] = {
        0x00000000u, 0x80000000u, 0x00000001u, 0x807FFFFFu, 0x00400000u, 0x00800000u, 0x80800000u,
        0x7F7FFFFFu, 0xFF7FFFFFu, 0x7F800000u, 0xFF800000u, 0x7FC00000u, 0xFFC00000u, 0x7F800001u,
        0x7FBFFFFFu, 0xFFC12345u, 0x3F800000u, 0xBF800000u, 0x3F7FFFFFu, 0x7E800000u, 0x01000000u,
    };
    const uint32_t k = rnd() % 8;
    if (k == 0) return special[rnd() % (sizeof special / sizeof special[0])];
    if (k == 1) return rnd();                                   /* any bits */
    if (k == 2) return (rnd() & 0x807FFFFFu);                   /* denormal or zero */
    /* an ordinary value, exponents spread so products and sums can over- and underflow */
    return (rnd() & 0x807FFFFFu) | ((rnd() % 254 + 1) << 23);
}

static void fill(uint8_t v[16])
{
    for (int i = 0; i < 4; i++) {
        const uint32_t x = lane();
        v[4 * i] = (uint8_t)(x >> 24); v[4 * i + 1] = (uint8_t)(x >> 16);
        v[4 * i + 2] = (uint8_t)(x >> 8); v[4 * i + 3] = (uint8_t)x;
    }
    if (rnd() % 4 == 0)   /* some lanes equal across operands: exact cancellations */
        memcpy(v + 4 * (rnd() % 4), v + 4 * (rnd() % 4), 4);
}

static int s_bad;
static void report(const char* op, uint32_t vscr, const uint8_t* a, const uint8_t* b, const uint8_t* c,
                   const uint8_t* want, const uint8_t* got)
{
    if (s_bad++ >= 8) return;
    printf("FAIL %s vscr=%08X\n", op, vscr);
    for (int i = 0; i < 4; i++)
        printf("  lane %d: a %08X b %08X c %08X want %08X got %08X\n", i,
               vmx_w(a, i), vmx_w(b, i), c ? vmx_w(c, i) : 0, vmx_w(want, i), vmx_w(got, i));
}

int main(void)
{
    const long n = 2000000;
    long checked = 0;
    for (long it = 0; it < n; it++) {
        const uint32_t vscr = (it & 1) ? VMX_NJ : 0;
        uint8_t a[16], b[16], c[16], want[16], got[16], al[16];
        fill(a); fill(b); fill(c);
        if (rnd() % 8 == 0) memcpy(c, a, 16);   /* a*a + b, the dot-product square */

        vmx_vaddfp_ref(want, a, b, vscr); vmx_vaddfp(got, a, b, vscr);
        if (memcmp(want, got, 16)) report("vaddfp", vscr, a, b, NULL, want, got);
        vmx_vsubfp_ref(want, a, b, vscr); vmx_vsubfp(got, a, b, vscr);
        if (memcmp(want, got, 16)) report("vsubfp", vscr, a, b, NULL, want, got);
        vmx_vmaddfp_ref(want, a, b, c, vscr); vmx_vmaddfp(got, a, b, c, vscr);
        if (memcmp(want, got, 16)) report("vmaddfp", vscr, a, b, c, want, got);
        vmx_vnmsubfp_ref(want, a, b, c, vscr); vmx_vnmsubfp(got, a, b, c, vscr);
        if (memcmp(want, got, 16)) report("vnmsubfp", vscr, a, b, c, want, got);

        /* the destination is one of the sources */
        vmx_vmaddfp_ref(want, a, b, c, vscr);
        memcpy(al, a, 16); vmx_vmaddfp(al, al, b, c, vscr);
        if (memcmp(want, al, 16)) report("vmaddfp d=a", vscr, a, b, c, want, al);
        vmx_vaddfp_ref(want, a, b, vscr);
        memcpy(al, b, 16); vmx_vaddfp(al, a, al, vscr);
        if (memcmp(want, al, 16)) report("vaddfp d=b", vscr, a, b, NULL, want, al);
        checked += 6;
    }
#if defined(__FMA__) && (defined(__x86_64__) || defined(_M_X64)) && !defined(VMX_SCALAR_FLOAT)
    const char* form = "vector";
#else
    const char* form = "scalar";
#endif
    printf("VMX float forms (%s): %ld operations, %d differ\n", form, checked, s_bad);
    return s_bad != 0;
}
