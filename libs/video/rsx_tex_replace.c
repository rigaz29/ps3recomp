/*
 * ps3recomp - pictures in place of the guest's textures (rsx_tex_replace.h)
 */
#include "rsx_tex_replace.h"

#include <stdlib.h>
#include <string.h>

static rsx_tex_hooks s_hooks;
static int s_hooks_set;

void rsx_tex_set_hooks(const rsx_tex_hooks* hooks)
{
    if (hooks && hooks->has && hooks->picture) {
        s_hooks = *hooks;
        s_hooks_set = 1;
    } else if (hooks && hooks->dump) {   /* a dump alone */
        memset(&s_hooks, 0, sizeof s_hooks);
        s_hooks.dump = hooks->dump;
        s_hooks_set = 1;
    } else {
        memset(&s_hooks, 0, sizeof s_hooks);
        s_hooks_set = 0;
    }
}

const rsx_tex_hooks* rsx_tex_get_hooks(void) { return s_hooks_set ? &s_hooks : NULL; }

int rsx_tex_replaceable(u32 rsx_fmt)
{
    return rsx_tex_channels(rsx_fmt) != 0;
}

u32 rsx_tex_channels(u32 rsx_fmt)
{
    switch (rsx_fmt & 0x9Fu) {
    case 0x82: case 0x83: case 0x84: case 0x85: case 0x97: case 0x9D: case 0x9E:   /* decoded to RGBA8 */
    case 0x86: case 0x87: case 0x88:                                               /* DXT1, DXT2/3, DXT4/5 */
        return RSX_TEXREP_RGBA8;
    case 0x81: return RSX_TEXREP_R8;    /* B8: video planes, masks */
    case 0x8B: return RSX_TEXREP_R8G8;  /* G8B8 */
    case 0x94: return RSX_TEXREP_R16;   /* X16: God of War: Ascension's font atlas */
    default:   return 0;
    }
}

/* --- XXH64 (Yann Collet's, as its specification has it), one shot, little-endian reads ------------------------ */

#define XP1 11400714785074694791ULL
#define XP2 14029467366897019727ULL
#define XP3 1609587929392839161ULL
#define XP4 9650029242287828579ULL
#define XP5 2870177450012600261ULL

static u64 xrotl(u64 x, int r) { return (x << r) | (x >> (64 - r)); }
static u64 xrd64(const u8* p)
{
    return (u64)p[0] | (u64)p[1] << 8 | (u64)p[2] << 16 | (u64)p[3] << 24 | (u64)p[4] << 32 | (u64)p[5] << 40 |
           (u64)p[6] << 48 | (u64)p[7] << 56;
}
static u64 xrd32(const u8* p) { return (u64)p[0] | (u64)p[1] << 8 | (u64)p[2] << 16 | (u64)p[3] << 24; }
static u64 xround(u64 acc, u64 in) { acc += in * XP2; acc = xrotl(acc, 31); return acc * XP1; }
static u64 xmerge(u64 acc, u64 v) { acc ^= xround(0, v); return acc * XP1 + XP4; }

static u64 xxh64(const u8* p, size_t len, u64 seed)
{
    const u8* const end = p + len;
    u64 h;
    if (len >= 32) {
        const u8* const limit = end - 32;
        u64 v1 = seed + XP1 + XP2, v2 = seed + XP2, v3 = seed, v4 = seed - XP1;
        do {
            v1 = xround(v1, xrd64(p)); p += 8;
            v2 = xround(v2, xrd64(p)); p += 8;
            v3 = xround(v3, xrd64(p)); p += 8;
            v4 = xround(v4, xrd64(p)); p += 8;
        } while (p <= limit);
        h = xrotl(v1, 1) + xrotl(v2, 7) + xrotl(v3, 12) + xrotl(v4, 18);
        h = xmerge(h, v1); h = xmerge(h, v2); h = xmerge(h, v3); h = xmerge(h, v4);
    } else {
        h = seed + XP5;
    }
    h += (u64)len;
    while (p + 8 <= end) { h ^= xround(0, xrd64(p)); h = xrotl(h, 27) * XP1 + XP4; p += 8; }
    if (p + 4 <= end) { h ^= xrd32(p) * XP1; h = xrotl(h, 23) * XP2 + XP3; p += 4; }
    while (p < end) { h ^= (u64)*p * XP5; h = xrotl(h, 11) * XP1; p++; }
    h ^= h >> 33; h *= XP2;
    h ^= h >> 29; h *= XP3;
    h ^= h >> 32;
    return h;
}

u64 rsx_tex_xxh64(const void* p, size_t len, u64 seed) { return xxh64((const u8*)p, len, seed); }

u64 rsx_tex_id(const u8* src, u32 w, u32 h, u32 rsx_fmt, const rsx_tex_layout* tl)
{
    /* The seed carries the size and the format byte; the bytes are level 0's as the guest has them, a linear
     * texture's rows without what pads them to its pitch. */
    const u64 seed = (u64)(rsx_fmt & 0xFFu) << 48 | (u64)(w & 0xFFFFFFu) << 24 | (u64)(h & 0xFFFFFFu);
    if (tl->compressed) return xxh64(src, (size_t)tl->row_bytes * tl->rows, seed);
    const size_t row = (size_t)w * tl->bytes_per_texel;
    if (tl->swizzled || tl->row_bytes == row) return xxh64(src, row * h, seed);
    u8* rows = (u8*)malloc(row * h);
    if (!rows) return 0;
    for (u32 y = 0; y < h; y++) memcpy(rows + y * row, src + (size_t)y * tl->row_bytes, row);
    const u64 id = xxh64(rows, row * h, seed);
    free(rows);
    return id;
}

/* --- DXT1/3/5 blocks to RGBA8 ---------------------------------------------------------------------------------- */

static void bc_colours(const u8* b, u8 c[4][4], int one_bit_alpha)
{
    const u32 c0 = (u32)b[0] | (u32)b[1] << 8, c1 = (u32)b[2] | (u32)b[3] << 8;
    for (int i = 0; i < 2; i++) {
        const u32 v = i ? c1 : c0;
        c[i][0] = (u8)(((v >> 11) & 31u) * 255u / 31u);
        c[i][1] = (u8)(((v >> 5) & 63u) * 255u / 63u);
        c[i][2] = (u8)((v & 31u) * 255u / 31u);
        c[i][3] = 255;
    }
    if (c0 > c1 || !one_bit_alpha) {
        for (int k = 0; k < 3; k++) {
            c[2][k] = (u8)((2u * c[0][k] + c[1][k] + 1u) / 3u);
            c[3][k] = (u8)((c[0][k] + 2u * c[1][k] + 1u) / 3u);
        }
        c[2][3] = c[3][3] = 255;
    } else {
        for (int k = 0; k < 3; k++) c[2][k] = (u8)((c[0][k] + c[1][k]) / 2u);
        c[2][3] = 255;
        c[3][0] = c[3][1] = c[3][2] = c[3][3] = 0;
    }
}

static void bc_block(const u8* b, int kind, u8 out[16][4])
{
    const u8* col = kind == 1 ? b : b + 8;
    u8 c[4][4];
    bc_colours(col, c, kind == 1);
    const u32 idx = (u32)col[4] | (u32)col[5] << 8 | (u32)col[6] << 16 | (u32)col[7] << 24;
    for (int i = 0; i < 16; i++) memcpy(out[i], c[(idx >> (2 * i)) & 3u], 4);
    if (kind == 2) {   /* four bits of alpha each */
        for (int i = 0; i < 16; i++) {
            const u32 a = (b[i >> 1] >> ((i & 1) * 4)) & 15u;
            out[i][3] = (u8)(a * 17u);
        }
    } else if (kind == 3) {   /* two end points and three bits each */
        u8 a[8];
        a[0] = b[0]; a[1] = b[1];
        if (a[0] > a[1]) {
            for (int k = 1; k < 7; k++) a[k + 1] = (u8)(((7 - k) * a[0] + k * a[1] + 3) / 7);
        } else {
            for (int k = 1; k < 5; k++) a[k + 1] = (u8)(((5 - k) * a[0] + k * a[1] + 2) / 5);
            a[6] = 0; a[7] = 255;
        }
        const u64 bits = (u64)b[2] | (u64)b[3] << 8 | (u64)b[4] << 16 | (u64)b[5] << 24 | (u64)b[6] << 32 |
                         (u64)b[7] << 40;
        for (int i = 0; i < 16; i++) out[i][3] = a[(bits >> (3 * i)) & 7u];
    }
}

int rsx_tex_base_rgba(u8* out, const u8* src, u32 w, u32 h, const rsx_tex_layout* tl)
{
    const u32 kind = rsx_tex_channels(tl->base_format);
    if (!kind) return 0;
    if (kind != RSX_TEXREP_RGBA8) {   /* one or two channels, as the upload has them */
        const u32 bpt = kind == RSX_TEXREP_R8 ? 1u : 2u;
        u8* b = (u8*)malloc((size_t)w * h * bpt);
        if (!b) return 0;
        rsx_texture_decode(b, w * bpt, src, w, h, tl, 0);
        for (size_t i = 0; i < (size_t)w * h; i++) {
            u8* o = out + i * 4;
            if (kind == RSX_TEXREP_R8G8) { o[0] = b[i * 2]; o[1] = b[i * 2 + 1]; o[2] = 0; }
            else o[0] = o[1] = o[2] = kind == RSX_TEXREP_R8 ? b[i] : b[i * 2 + 1];   /* R16: its high byte */
            o[3] = 255;
        }
        free(b);
        return 1;
    }
    if (!tl->compressed) {
        rsx_texture_decode(out, w * 4u, src, w, h, tl, 0);   /* natural R,G,B,A, as the upload has it */
        return 1;
    }
    const int bc = tl->fmt == RSX_TEXFMT_BC1 ? 1 : tl->fmt == RSX_TEXFMT_BC2 ? 2 : 3;
    u8* blocks = (u8*)malloc((size_t)tl->row_bytes * tl->rows);
    if (!blocks) return 0;
    rsx_texture_decode(blocks, tl->row_bytes, src, w, h, tl, 0);   /* the blocks as the upload has them */
    const u32 bw = (w + 3) / 4;
    for (u32 by = 0; by < tl->rows; by++)
        for (u32 bx = 0; bx < bw; bx++) {
            u8 t[16][4];
            bc_block(blocks + (size_t)by * tl->row_bytes + (size_t)bx * tl->block_bytes, bc, t);
            for (u32 i = 0; i < 16; i++) {
                const u32 x = bx * 4 + (i & 3), y = by * 4 + (i >> 2);
                if (x < w && y < h) memcpy(out + ((size_t)y * w + x) * 4, t[i], 4);
            }
        }
    free(blocks);
    return 1;
}

void rsx_tex_pack(u8* dst, const u8* rgba, size_t n, u32 kind)
{
    switch (kind) {
    case RSX_TEXREP_R8:
        for (size_t i = 0; i < n; i++) dst[i] = rgba[i * 4];
        break;
    case RSX_TEXREP_R8G8:
        for (size_t i = 0; i < n; i++) { dst[i * 2] = rgba[i * 4]; dst[i * 2 + 1] = rgba[i * 4 + 1]; }
        break;
    case RSX_TEXREP_R16:   /* little-endian, as the decode leaves an R16 */
        for (size_t i = 0; i < n; i++) { dst[i * 2] = rgba[i * 4]; dst[i * 2 + 1] = rgba[i * 4]; }
        break;
    default:
        memcpy(dst, rgba, n * 4);
        break;
    }
}

/* --- the mip chain ------------------------------------------------------------------------------------------- */

u32 rsx_tex_chain_levels(u32 w, u32 h)
{
    u32 n = 1, m = w > h ? w : h;
    while (m > 1) { m >>= 1; n++; }
    return n;
}

void rsx_tex_mip_down(const u8* src, u32 w, u32 h, u8* dst)
{
    const u32 nw = w > 1 ? w >> 1 : 1, nh = h > 1 ? h >> 1 : 1;
    for (u32 y = 0; y < nh; y++)
        for (u32 x = 0; x < nw; x++) {
            /* The colour weighted by alpha, so a clear texel beside a drawn one leaves no dark rim. */
            u32 rgb[3] = { 0, 0, 0 }, rgb_plain[3] = { 0, 0, 0 }, a = 0, n = 0;
            for (u32 dy = 0; dy < 2; dy++)
                for (u32 dx = 0; dx < 2; dx++) {
                    const u32 sx = x * 2 + dx, sy = y * 2 + dy;
                    if (sx >= w || sy >= h) continue;
                    const u8* p = src + ((size_t)sy * w + sx) * 4;
                    for (int c = 0; c < 3; c++) { rgb[c] += (u32)p[c] * p[3]; rgb_plain[c] += p[c]; }
                    a += p[3];
                    n++;
                }
            u8* d = dst + ((size_t)y * nw + x) * 4;
            for (int c = 0; c < 3; c++) d[c] = (u8)(a ? (rgb[c] + a / 2) / a : (rgb_plain[c] + n / 2) / n);
            d[3] = (u8)((a + n / 2) / n);
        }
}
