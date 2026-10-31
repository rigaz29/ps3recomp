#ifndef SPU_OVERLAY_RESIDENCY_H
#define SPU_OVERLAY_RESIDENCY_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Exact code bytes a lifted streamed overlay was generated from, at the
 * local-store addresses the title loads it to. */
typedef struct spu_overlay_code_segment {
    uint32_t lsa, size;
    const uint8_t* bytes;
} spu_overlay_code_segment;

/* Register a streamed code overlay by its DMA source EA, like
 * spu_overlay_register_region, plus the code bytes its lifted functions were
 * generated from. A GET from content_ea into local store at load_lsa marks the
 * overlay resident but unverified; the first dispatch into its span compares
 * the segments once and then dispatches by residency until a DMA writes code
 * bytes in the span that differ from the lift (a GET of the same bytes -- the
 * module loaded again -- or of data beside the code leaves it verified). A
 * mismatch leaves the span interpreted until the next load.
 * Prefer a span covering the lifted code only, not the whole loaded image:
 * code that DMAs its own data into the image above the code would otherwise
 * clear residency with every such GET, sending later dispatch to the
 * interpreter. The segments are copied. Returns 1 on success, 0 on invalid input or a full
 * registry. */
int spu_overlay_register_verified_region(uint32_t content_ea, uint32_t load_lsa,
    uint32_t span, int image_id, const spu_overlay_code_segment* code,
    unsigned count);

/* The same for position-independent code, a piece a job streams to wherever
 * it has room: lifted once at lift_lsa, it is resident wherever a GET from
 * content_ea lands it. The segments are compared at that placement, and
 * dispatch into it runs the function registered at the same offset from
 * lift_lsa with ctx->pic_base set to the difference. A piece that arrives
 * inside a GET from anywhere else is found by its bytes: where dispatch finds
 * nothing and no resident span decides, 16 bytes that start one of the piece's
 * lifted functions place the segments, and if they all match there that
 * placement is resident (verified, with no source EA, so the next GET into it
 * ends it). Give the instructions as the segments, not a constant pool the
 * piece stores into. */
int spu_overlay_register_pic_region(uint32_t content_ea, uint32_t lift_lsa,
    uint32_t span, int image_id, const spu_overlay_code_segment* code,
    unsigned count);

#ifdef __cplusplus
}
#endif

#endif /* SPU_OVERLAY_RESIDENCY_H */
