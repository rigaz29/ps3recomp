/* libs/video/rsx_tex_replace.c: the ids textures are named by, their level 0 as RGBA, the mip chain, the hooks. */
#include "rsx_tex_replace.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int fails = 0;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

static int px(const u8* img, u32 w, u32 x, u32 y, u8 r, u8 g, u8 b, u8 a)
{
    const u8* p = img + ((size_t)y * w + x) * 4;
    if (p[0] == r && p[1] == g && p[2] == b && p[3] == a) return 1;
    printf("  texel %u,%u is %u,%u,%u,%u, not %u,%u,%u,%u\n", x, y, p[0], p[1], p[2], p[3], r, g, b, a);
    return 0;
}

static int has_none(u64 id) { (void)id; return 0; }
static int no_picture(u64 id, u32 w, u32 h, const u8* base, rsx_tex_picture* out)
{
    (void)id; (void)w; (void)h; (void)base; (void)out;
    return 0;
}

int main(void)
{
    /* XXH64 as its specification has it (and a Python version of the specification, for the 768 bytes). */
    u8 seq[768];
    for (int i = 0; i < 768; i++) seq[i] = (u8)i;
    CHECK(rsx_tex_xxh64("", 0, 0) == 0xEF46DB3751D8E999ULL);
    CHECK(rsx_tex_xxh64("abc", 3, 0) == 0x44BC2CF5AD770999ULL);
    CHECK(rsx_tex_xxh64("Nobody inspects the spammish repetition", 39, 0) == 0xFBCEA83C8A378BF1ULL);
    CHECK(rsx_tex_xxh64("xxhash", 6, 20141025) == 0xB559B98D844E0635ULL);
    CHECK(rsx_tex_xxh64(seq, sizeof seq, 0) == 0x8E03C838C596036FULL);

    /* An A8R8G8B8 texture, 4 x 2 and linear (0x85 | 0x20) at a pitch of 32: its id is of the rows without the
     * padding, and the padding does not change it. */
    {
        rsx_tex_layout tl;
        rsx_texture_layout_pitched(0xA5, 4, 2, 32, &tl);
        u8 img[64], img2[64];
        for (int i = 0; i < 64; i++) { img[i] = (u8)(i * 7); img2[i] = img[i]; }
        for (int i = 16; i < 32; i++) img2[i] = 0xEE;   /* the first row's padding */
        u8 rows[32];
        memcpy(rows, img, 16);
        memcpy(rows + 16, img + 32, 16);
        const u64 seed = (u64)0xA5 << 48 | (u64)4 << 24 | 2;
        CHECK(rsx_tex_id(img, 4, 2, 0xA5, &tl) == rsx_tex_xxh64(rows, 32, seed));
        CHECK(rsx_tex_id(img2, 4, 2, 0xA5, &tl) == rsx_tex_id(img, 4, 2, 0xA5, &tl));
        /* Another size or format byte: another id, the bytes the same. */
        rsx_tex_layout tl2;
        rsx_texture_layout_pitched(0xA5, 2, 4, 32, &tl2);
        CHECK(rsx_tex_id(img, 2, 4, 0xA5, &tl2) != rsx_tex_id(img, 4, 2, 0xA5, &tl));
        /* Its level 0 as R, G, B, A from the guest's A, R, G, B. */
        u8 out[32];
        CHECK(rsx_tex_base_rgba(out, img, 4, 2, &tl) == 1);
        CHECK(px(out, 4, 0, 0, img[1], img[2], img[3], img[0]));
        CHECK(px(out, 4, 3, 1, img[32 + 13], img[32 + 14], img[32 + 15], img[32 + 12]));
    }

    /* DXT1 (0x86): one 4 x 4 block, red and blue end points; texels 0-3 take each of the four colours. */
    {
        rsx_tex_layout tl;
        rsx_texture_layout(0x86, 4, 4, &tl);
        const u8 blk[8] = { 0x00, 0xF8, 0x1F, 0x00, 0xE4, 0x00, 0x00, 0x00 };
        u8 out[64];
        CHECK(rsx_tex_replaceable(0x86) && rsx_tex_base_rgba(out, blk, 4, 4, &tl) == 1);
        CHECK(px(out, 4, 0, 0, 255, 0, 0, 255));
        CHECK(px(out, 4, 1, 0, 0, 0, 255, 255));
        CHECK(px(out, 4, 2, 0, 170, 0, 85, 255));
        CHECK(px(out, 4, 3, 0, 85, 0, 170, 255));
        CHECK(px(out, 4, 0, 3, 255, 0, 0, 255));
        /* c0 <= c1: three colours, and the fourth clear. */
        const u8 blk3[8] = { 0x1F, 0x00, 0x00, 0xF8, 0xC0, 0x00, 0x00, 0x00 };
        CHECK(rsx_tex_base_rgba(out, blk3, 4, 4, &tl) == 1);
        CHECK(px(out, 4, 3, 0, 0, 0, 0, 0));
        CHECK(px(out, 4, 0, 0, 0, 0, 255, 255));
    }

    /* DXT5 (0x88): alpha end points 255 and 0 over a white block; texel 0 takes a0, texel 1 a1, texel 2 the first
     * of the six between them. */
    {
        rsx_tex_layout tl;
        rsx_texture_layout(0x88, 4, 4, &tl);
        u8 blk[16] = { 255, 0, 0x88, 0x00, 0, 0, 0, 0,  0xFF, 0xFF, 0xFF, 0xFF, 0, 0, 0, 0 };   /* indices 0, 1, 2 */
        u8 out[64];
        CHECK(rsx_tex_base_rgba(out, blk, 4, 4, &tl) == 1);
        CHECK(px(out, 4, 0, 0, 255, 255, 255, 255));
        CHECK(px(out, 4, 1, 0, 255, 255, 255, 0));
        CHECK(px(out, 4, 2, 0, 255, 255, 255, 219));
    }

    /* One and two channels: B8 grey, X16 its high byte grey, G8B8 in R and G; and a picture back into them. */
    {
        rsx_tex_layout tl;
        rsx_texture_layout(0xA1, 2, 1, &tl);   /* B8, linear */
        const u8 b8[2] = { 10, 200 };
        u8 out[8];
        CHECK(rsx_tex_channels(0x81) == RSX_TEXREP_R8 && rsx_tex_base_rgba(out, b8, 2, 1, &tl) == 1);
        CHECK(px(out, 2, 1, 0, 200, 200, 200, 255));
        rsx_texture_layout(0xB4, 2, 1, &tl);   /* X16, linear: big-endian half-words */
        const u8 x16[4] = { 0x12, 0x34, 0xAB, 0xCD };
        CHECK(rsx_tex_channels(0x94) == RSX_TEXREP_R16 && rsx_tex_base_rgba(out, x16, 2, 1, &tl) == 1);
        CHECK(px(out, 2, 0, 0, 0x12, 0x12, 0x12, 255) && px(out, 2, 1, 0, 0xAB, 0xAB, 0xAB, 255));
        rsx_texture_layout(0xAB, 1, 1, &tl);   /* G8B8 */
        const u8 g8b8[2] = { 7, 9 };
        CHECK(rsx_tex_channels(0x8B) == RSX_TEXREP_R8G8 && rsx_tex_base_rgba(out, g8b8, 1, 1, &tl) == 1);
        CHECK(px(out, 1, 0, 0, 7, 9, 0, 255));
        const u8 pic[8] = { 1, 2, 3, 4,  250, 6, 7, 8 };
        u8 packed[8];
        rsx_tex_pack(packed, pic, 2, RSX_TEXREP_R8);
        CHECK(packed[0] == 1 && packed[1] == 250);
        rsx_tex_pack(packed, pic, 2, RSX_TEXREP_R8G8);
        CHECK(packed[0] == 1 && packed[1] == 2 && packed[2] == 250 && packed[3] == 6);
        rsx_tex_pack(packed, pic, 2, RSX_TEXREP_R16);   /* R times 257, little-endian */
        CHECK(packed[0] == 1 && packed[1] == 1 && packed[2] == 250 && packed[3] == 250);
    }

    /* A format the decode does not take to RGBA nor to one or two channels it names: not named. */
    CHECK(!rsx_tex_replaceable(0x9A) && !rsx_tex_replaceable(0x90) && rsx_tex_replaceable(0x81));

    /* The chain: levels, and a level made from the one above, its colour weighted by alpha. */
    CHECK(rsx_tex_chain_levels(1, 1) == 1 && rsx_tex_chain_levels(256, 128) == 9 && rsx_tex_chain_levels(300, 7) == 9);
    {
        const u8 two[16] = { 255, 0, 0, 255,  0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0 };
        u8 one[4];
        rsx_tex_mip_down(two, 2, 2, one);
        CHECK(px(one, 1, 0, 0, 255, 0, 0, 64));
        const u8 row[12] = { 10, 20, 30, 255,  30, 40, 50, 255,  200, 200, 200, 255 };
        rsx_tex_mip_down(row, 3, 1, one);   /* 3 x 1 to 1 x 1: the first two */
        CHECK(px(one, 1, 0, 0, 20, 30, 40, 255));
        /* An AG normal map's level: its alpha is the normal's x, not coverage; each channel its own plain mean. */
        const u8 ag[16] = { 255, 100, 0, 40,  255, 200, 0, 240,  255, 100, 0, 40,  255, 200, 0, 240 };
        rsx_tex_mip_down_plain(ag, 2, 2, one);
        CHECK(px(one, 1, 0, 0, 255, 150, 0, 140));
        rsx_tex_mip_down(ag, 2, 2, one);   /* weighted, the green leans to the texels with more alpha */
        CHECK(one[1] > 150);
    }

    /* BC pictures: a level's bytes in 4x4 blocks (8 for BC1, 16 for the others), and a chain's. */
    CHECK(rsx_tex_bc_level_bytes(RSX_TEXPIC_BC1, 8, 8) == 32 && rsx_tex_bc_level_bytes(RSX_TEXPIC_BC7, 8, 8) == 64);
    CHECK(rsx_tex_bc_level_bytes(RSX_TEXPIC_BC3, 2, 1) == 16 && rsx_tex_bc_level_bytes(RSX_TEXPIC_BC1, 6, 4) == 16);
    CHECK(rsx_tex_bc_chain_bytes(RSX_TEXPIC_BC1, 8, 8, 4) == 32 + 8 + 8 + 8);
    CHECK(rsx_tex_bc_chain_bytes(RSX_TEXPIC_BC7, 16, 4, 5) == 64 + 32 + 16 + 16 + 16);

    /* Pictures read later: the count the backend watches moves on with each one ready; the size of the pictures. */
    {
        const u32 g = rsx_tex_pictures_gen();
        rsx_tex_pictures_ready();
        rsx_tex_pictures_ready();
        CHECK(rsx_tex_pictures_gen() == g + 2);
        CHECK(rsx_tex_pictures_bytes() == 0);
        rsx_tex_set_pictures_bytes(123u << 20);
        CHECK(rsx_tex_pictures_bytes() == (123ull << 20));
        rsx_tex_set_pictures_bytes(0);
    }

    /* The hooks: set, then taken away. */
    {
        rsx_tex_hooks hk = { has_none, no_picture, NULL };
        CHECK(!rsx_tex_get_hooks());
        rsx_tex_set_hooks(&hk);
        CHECK(rsx_tex_get_hooks() && rsx_tex_get_hooks()->has == has_none);
        rsx_tex_set_hooks(NULL);
        CHECK(!rsx_tex_get_hooks());
    }

    printf(fails ? "rsx_tex_replace: %d failed\n" : "rsx_tex_replace: ok\n", fails);
    return fails != 0;
}
