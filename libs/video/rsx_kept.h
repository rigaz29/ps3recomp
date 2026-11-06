/*
 * A vertex ring's block whose bytes stay out of the ring.
 *
 * A port that knows a block's list and vertexes from an earlier frame (God of War: Ascension's kept
 * EDGE results) can leave them out of the ring and publish them here for the block's IO offset. The
 * D3D12 backend puts them in a resident part of its vertex and index buffers the first time and draws
 * from there: nothing is copied per frame. Any other path through the backend gets them written to
 * the ring first (kept_fill), so the ring's bytes stay a valid fallback. When the resident part fills
 * up it starts over, and blocks still drawn go in again.
 *
 * The block keeps the vertexes' first 16 bytes in the ring: a block is taken only while guest memory
 * holds them, as with the vertex pass's records.
 */
#ifndef RSX_KEPT_H
#define RSX_KEPT_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct rsx_kept {
    const void* list;       /* the indexes as the ring would hold them: big-endian u16; list_bytes
                               can run past what a job puts, with junk after the job's count */
    const void* verts;      /* nverts records, the first rec bytes of each of the ring's vertexes */
    uint32_t list_bytes, nverts, rec, stride;
    uint32_t put_verts;     /* the vertexes a job puts in the ring */
    /* A skinned block: verts are inputs of rec bytes, contiguous, that the vertex pass makes the
     * vertexes from with the job's record; fill makes them on the CPU instead, first rec bytes
     * of each vertex at stride, from the skin part of the record. */
    uint32_t skin;
    void (*fill)(const struct rsx_kept* k, const void* skin, uint8_t* ring, uint32_t n);
    uint8_t  tag[16];       /* the first 16 bytes of the ring's vertexes */
    /* the backend's, from its drawing thread */
    uint32_t res_vb, res_ib;    /* byte and index offsets in the resident parts */
    uint32_t res_lo, res_hi, res_n;     /* the lowest and highest of the n indexes drawn so far */
    uint32_t res_state, res_gen;    /* 0 not tried, 1 resident, 2 no room; under which filling */
} rsx_kept;

/* From any thread: the block at io is k's; NULL clears it. Before the hole that draws it. */
void rsx_d3d12_kept_publish(uint32_t io_offset, rsx_kept* k);

#ifdef __cplusplus
}
#endif

#endif /* RSX_KEPT_H */
