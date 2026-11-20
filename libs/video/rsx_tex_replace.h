/*
 * ps3recomp - pictures in place of the guest's textures
 *
 * A port's texture mods. The backend names each 2D texture it uploads from
 * guest memory by an id: a 64-bit hash of its level 0's bytes as the guest has
 * them, mixed with its width, height and format byte. An id is the same from
 * run to run and wherever the guest puts the texture. For a texture of a
 * format that decodes to RGBA (A8R8G8B8, D8R8G8B8, the packed 16-bit colour
 * formats, DXT1/3/5), and for B8 and G8B8 (fonts), the backend asks the port's
 * hooks whether it has a picture for that id, and uploads the picture in its
 * place, of any size, its mip chain made here: RGBA8, or for B8 and G8B8 the
 * picture's R (and G) in the texture's own channels. The texture is sampled as
 * before, through the same channel mapping: the decoded image these formats
 * upload is RGBA in that order too.
 *
 * The decode and the hash are RSX semantics and live here, beside
 * rsx_texture_layout.c, so a backend other than D3D12's can take them up.
 */
#ifndef PS3RECOMP_RSX_TEX_REPLACE_H
#define PS3RECOMP_RSX_TEX_REPLACE_H

#include "rsx_texture_layout.h"

#ifdef __cplusplus
extern "C" {
#endif

/* A picture: RGBA8, rows top down and tightly packed. key: the same for the same texels (w, h and rgba alike),
 * whichever id asked; the backend then uploads it once and every texture it stands in for draws from that one
 * upload. 0: no key, uploaded for each texture (a picture made over the guest's own, which differs with it). */
typedef struct {
    u32 w, h;
    const u8* rgba;
    u64 key;
} rsx_tex_picture;

typedef struct {
    /* What the port has for this id: 0 nothing, 1 a picture to take as it is,
     * 2 one to make over the guest's own (picture() is handed its level 0). */
    int (*has)(u64 id);
    /* The picture for id. base: the guest's level 0 as RGBA8, w x h, when
     * has() said 2, else NULL. The picture stays the port's until the next
     * call. 1 = given. */
    int (*picture)(u64 id, u32 w, u32 h, const u8* base, rsx_tex_picture* out);
    /* Every texture the backend can name, once a run for each id, with its
     * level 0 as RGBA8: for those who make the pictures. NULL: none. */
    void (*dump)(u64 id, u32 w, u32 h, u32 fmt, const u8* base);
} rsx_tex_hooks;

/* The port's hooks; NULL takes them away. Set before the first frame. */
void rsx_tex_set_hooks(const rsx_tex_hooks* hooks);
/* The hooks while they are set, else NULL. */
const rsx_tex_hooks* rsx_tex_get_hooks(void);

/* Whether textures of this format byte can be named and replaced. */
int rsx_tex_replaceable(u32 rsx_fmt);

/* What the upload of such a texture holds, and so how a picture goes in: RGBA8 for those decoded to RGBA; R8 for B8,
 * the picture's R; R8G8 for G8B8, its R and G; R16 for X16, its R times 257 (the view's mapping stays the same).
 * Their level 0 as RGBA is grey for R8 and R16 (R16's high byte), R and G for R8G8, with an opaque alpha. 0: not
 * named. */
enum { RSX_TEXREP_RGBA8 = 4, RSX_TEXREP_R8 = 1, RSX_TEXREP_R8G8 = 2, RSX_TEXREP_R16 = 16 };
u32 rsx_tex_channels(u32 rsx_fmt);
/* A picture's texels in such a texture's upload format: n texels of RGBA8 to n of kind. */
void rsx_tex_pack(u8* dst, const u8* rgba, size_t n, u32 kind);

/* The id of a texture: level 0 at src (as the layout says it is laid out). It is XXH64 (below) of the level's
 * bytes as the guest has them, a linear texture's rows without what pads them to its pitch, with the seed
 * format << 48 | width << 24 | height (the format byte as bound, each size in 24 bits). */
u64 rsx_tex_id(const u8* src, u32 w, u32 h, u32 rsx_fmt, const rsx_tex_layout* tl);

/* XXH64, as its specification has it: what ids are made with, for tools that work them out too. */
u64 rsx_tex_xxh64(const void* p, size_t len, u64 seed);

/* Level 0 decoded to RGBA8 into out (w * h * 4 bytes). 0 when the format is not
 * one rsx_tex_replaceable takes. */
int rsx_tex_base_rgba(u8* out, const u8* src, u32 w, u32 h, const rsx_tex_layout* tl);

/* The levels of a full mip chain for w x h, and level k's size. */
u32 rsx_tex_chain_levels(u32 w, u32 h);
/* The next level of an RGBA8 image, (w >> 1) x (h >> 1) at least 1 x 1, each
 * texel the mean of the two by two (or fewer) above it. */
void rsx_tex_mip_down(const u8* src, u32 w, u32 h, u8* dst);

#ifdef __cplusplus
}
#endif

#endif
