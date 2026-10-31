/* The vector forms of runtime/ppu/ppu_vmx.h (FMA3 hosts) against the scalar
 * reference forms, byte for byte:
 *   - vaddfp, vsubfp, vmaddfp, vnmsubfp, vmaxfp, vminfp, the estimates and the
 *     roundings: random lanes drawn from ordinary values, zeros, denormals,
 *     infinities, quiet and signalling NaNs with payloads and values near
 *     overflow, with VSCR[NJ] set and clear, and the destination aliasing a
 *     source;
 *   - the word compares and their CR6 nibbles, from the same lanes and from
 *     random words;
 *   - vcfsx and vctsxs (with the SAT bit) for every scale;
 *   - vperm with random controls, lvlx/lvrx for every byte offset, lvsl/lvsr. */
#include "ppu_vmx.h"
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#if defined(__unix__) || defined(__APPLE__)
#include <sys/mman.h>
#include <unistd.h>
#endif

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

static void report_bytes(const char* op, const uint8_t* want, const uint8_t* got, uint32_t arg)
{
    if (s_bad++ >= 8) return;
    printf("FAIL %s (%u)\n  want", op, arg);
    for (int i = 0; i < 16; i++) printf(" %02X", want[i]);
    printf("\n  got ");
    for (int i = 0; i < 16; i++) printf(" %02X", got[i]);
    printf("\n");
}

/* the moves, compares, estimates and conversions; returns the operations checked */
static long check_more(long n)
{
    long checked = 0;
    for (long it = 0; it < n; it++) {
        const uint32_t vscr = (it & 1) ? VMX_NJ : 0;
        uint8_t a[16], b[16], c[16], want[16], got[16];
        fill(a); fill(b);
        for (int i = 0; i < 16; i++) c[i] = (uint8_t)rnd();
        if (rnd() % 4 == 0)   /* +0 against -0 in some lanes: max/min's equal case */
            for (int i = 0; i < 4; i++) if (rnd() & 1) {
                const uint32_t z = (rnd() & 1) ? 0x80000000u : 0u;
                vmx_sw(a, i, z); vmx_sw(b, i, z ^ ((rnd() & 1) ? 0x80000000u : 0u));
            }

        vmx_vperm_ref(want, a, b, c); vmx_vperm(got, a, b, c);
        if (memcmp(want, got, 16)) report_bytes("vperm", want, got, 0);
        { uint8_t al[16]; memcpy(al, c, 16); vmx_vperm(al, a, b, al);
          if (memcmp(want, al, 16)) report_bytes("vperm d=c", want, al, 0); }
        {   /* an aligned quadword, every offset */
            _Alignas(16) uint8_t m[16];
            for (int i = 0; i < 16; i++) m[i] = (uint8_t)rnd();
            for (uint32_t sh = 0; sh < 16; sh++) {
                vmx_lvlx_ref(want, m, sh); vmx_lvlx(got, m, sh);
                if (memcmp(want, got, 16)) report_bytes("lvlx", want, got, sh);
                vmx_lvrx_ref(want, m, sh); vmx_lvrx(got, m, sh);
                if (memcmp(want, got, 16)) report_bytes("lvrx", want, got, sh);
                vmx_lvs_ref(want, sh); vmx_lvs(got, sh);
                if (memcmp(want, got, 16)) report_bytes("lvsl", want, got, sh);
                vmx_lvs_ref(want, 16u - sh); vmx_lvs(got, 16u - sh);
                if (memcmp(want, got, 16)) report_bytes("lvsr", want, got, sh);
                checked += 4;
            }
        }

        /* compares: the float lanes above, and words with equal and opposite-sign pairs */
        uint8_t wa[16], wb[16];
        for (int i = 0; i < 4; i++) {
            const uint32_t x = rnd() ^ (rnd() << 16), k = rnd() % 4;
            vmx_sw(wa, i, x);
            vmx_sw(wb, i, k == 0 ? x : k == 1 ? x ^ 0x80000000u : rnd() ^ (rnd() << 16));
        }
#define CMP(fn, x, y) do { const uint32_t cw = fn##_ref(want, x, y), cg = fn(got, x, y); \
        if (memcmp(want, got, 16) || cw != cg) report_bytes(#fn, want, got, cw << 4 | cg); checked++; } while (0)
        CMP(vmx_vcmpequw, a, b); CMP(vmx_vcmpgtsw, a, b); CMP(vmx_vcmpgtuw, a, b);
        CMP(vmx_vcmpeqfp, a, b); CMP(vmx_vcmpgefp, a, b); CMP(vmx_vcmpgtfp, a, b);
        CMP(vmx_vcmpequw, wa, wb); CMP(vmx_vcmpgtsw, wa, wb); CMP(vmx_vcmpgtuw, wa, wb);
        CMP(vmx_vcmpeqfp, a, a); CMP(vmx_vcmpgefp, a, a); CMP(vmx_vcmpgtfp, b, b);
#undef CMP

#define BIN(fn) do { fn##_ref(want, a, b, vscr); fn(got, a, b, vscr); \
        if (memcmp(want, got, 16)) report("" #fn, vscr, a, b, NULL, want, got); checked++; } while (0)
        BIN(vmx_vmaxfp); BIN(vmx_vminfp);
#undef BIN
#define UN(fn) do { fn##_ref(want, b, vscr); fn(got, b, vscr); \
        if (memcmp(want, got, 16)) report("" #fn, vscr, b, b, NULL, want, got); checked++; } while (0)
        UN(vmx_vrefp); UN(vmx_vrsqrtefp); UN(vmx_vrfin); UN(vmx_vrfiz); UN(vmx_vrfip); UN(vmx_vrfim);
#undef UN

        const unsigned uimm = (unsigned)(it % 32);
        vmx_vcfsx_ref(want, wa, uimm); vmx_vcfsx(got, wa, uimm);
        if (memcmp(want, got, 16)) report_bytes("vcfsx", want, got, uimm);
        {   uint32_t sw = 0, sg = 0;
            vmx_vctsxs_ref(want, a, uimm, &sw); vmx_vctsxs(got, a, uimm, &sg);
            if (memcmp(want, got, 16) || sw != sg) report_bytes("vctsxs", want, got, uimm << 8 | sw << 4 | sg);
            sw = sg = 0;
            vmx_vctsxs_ref(want, wa, uimm, &sw); vmx_vctsxs(got, wa, uimm, &sg);
            if (memcmp(want, got, 16) || sw != sg) report_bytes("vctsxs words", want, got, uimm << 8 | sw << 4 | sg); }
        checked += 3;
    }
    return checked;
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
    checked += check_more(n / 4);
#if defined(__unix__) || defined(__APPLE__)
    {   /* lvrx at offset 0 reads no byte: its quadword may start an unmapped page */
        const long pg = sysconf(_SC_PAGESIZE);
        uint8_t* two = (uint8_t*)mmap(NULL, 2 * pg, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (two != MAP_FAILED && mprotect(two + pg, pg, PROT_NONE) == 0) {
            uint8_t z[16];
            vmx_lvrx(z, two + pg, 0);
            for (int i = 0; i < 16; i++) if (z[i]) { s_bad++; printf("FAIL lvrx sh 0 not zero\n"); break; }
            memset(two + pg - 16, 0xA5, 16);
            vmx_lvlx(z, two + pg - 16, 15);   /* the last byte before the hole */
            if (z[0] != 0xA5 || z[1]) { s_bad++; printf("FAIL lvlx sh 15 at a page end\n"); }
            checked += 2;
        }
    }
#endif
#if defined(__FMA__) && (defined(__x86_64__) || defined(_M_X64)) && !defined(VMX_SCALAR_FLOAT)
    const char* form = "vector";
#else
    const char* form = "scalar";
#endif
    printf("VMX vector forms (%s): %ld operations, %d differ\n", form, checked, s_bad);
    return s_bad != 0;
}
