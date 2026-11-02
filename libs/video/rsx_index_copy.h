/*
 * The RSX's index array as host u32 indices, with their range.
 *
 * A draw's indices are big-endian 16- or 32-bit words in guest memory; the
 * D3D12 backend writes them into its index ring as u32 and needs the lowest
 * and highest one to know which vertices to copy (upload_tris_vp_raw). God of
 * War: Ascension's first level takes ~1.2 million indices a frame that way,
 * and a byte at a time that was ~2 ms of the frame clock thread's frame. The
 * SSE4.1 form converts eight 16-bit or four 32-bit indices an operation. The
 * port's SPU code needs SSE4.1 already (CMakeLists.txt, -msse4.1).
 *
 * rsx_index_copy_be(src, count, sixteen, dst, &lo, &hi): src[0..count) to
 * dst[0..count); lo/hi are 0xFFFFFFFF/0 for count 0. rsx_index_copy_be_scalar
 * is the same, a word at a time (the reference the unit test compares with).
 */
#ifndef RSX_INDEX_COPY_H
#define RSX_INDEX_COPY_H

#include <stdint.h>

static inline void rsx_index_copy_be_scalar(const uint8_t* src, uint32_t count, int sixteen,
                                            uint32_t* dst, uint32_t* plo, uint32_t* phi)
{
    uint32_t lo = 0xFFFFFFFFu, hi = 0;
    for (uint32_t k = 0; k < count; k++) {
        const uint32_t ix = sixteen
            ? ((uint32_t)src[k * 2] << 8) | src[k * 2 + 1]
            : ((uint32_t)src[k * 4] << 24) | ((uint32_t)src[k * 4 + 1] << 16) |
              ((uint32_t)src[k * 4 + 2] << 8) | src[k * 4 + 3];
        dst[k] = ix;
        if (ix < lo) lo = ix;
        if (ix > hi) hi = ix;
    }
    *plo = lo; *phi = hi;
}

#if defined(__x86_64__) || defined(_M_X64)
#include <immintrin.h>
#define RSX_INDEX_COPY_SIMD 1

__attribute__((target("sse4.1")))
static inline void rsx_index_copy_be_sse41(const uint8_t* src, uint32_t count, int sixteen,
                                           uint32_t* dst, uint32_t* plo, uint32_t* phi)
{
    __m128i vlo = _mm_set1_epi32(-1), vhi = _mm_setzero_si128();
    uint32_t k = 0;
    if (sixteen) {
        const __m128i sw = _mm_setr_epi8(1, 0, 3, 2, 5, 4, 7, 6, 9, 8, 11, 10, 13, 12, 15, 14);
        for (; k + 8 <= count; k += 8) {
            const __m128i v = _mm_shuffle_epi8(_mm_loadu_si128((const __m128i*)(src + k * 2)), sw);
            const __m128i a = _mm_cvtepu16_epi32(v);
            const __m128i b = _mm_cvtepu16_epi32(_mm_srli_si128(v, 8));
            _mm_storeu_si128((__m128i*)(dst + k), a);
            _mm_storeu_si128((__m128i*)(dst + k + 4), b);
            vlo = _mm_min_epu32(vlo, _mm_min_epu32(a, b));
            vhi = _mm_max_epu32(vhi, _mm_max_epu32(a, b));
        }
    } else {
        const __m128i sw = _mm_setr_epi8(3, 2, 1, 0, 7, 6, 5, 4, 11, 10, 9, 8, 15, 14, 13, 12);
        for (; k + 4 <= count; k += 4) {
            const __m128i a = _mm_shuffle_epi8(_mm_loadu_si128((const __m128i*)(src + k * 4)), sw);
            _mm_storeu_si128((__m128i*)(dst + k), a);
            vlo = _mm_min_epu32(vlo, a);
            vhi = _mm_max_epu32(vhi, a);
        }
    }
    vlo = _mm_min_epu32(vlo, _mm_shuffle_epi32(vlo, _MM_SHUFFLE(1, 0, 3, 2)));
    vlo = _mm_min_epu32(vlo, _mm_shuffle_epi32(vlo, _MM_SHUFFLE(2, 3, 0, 1)));
    vhi = _mm_max_epu32(vhi, _mm_shuffle_epi32(vhi, _MM_SHUFFLE(1, 0, 3, 2)));
    vhi = _mm_max_epu32(vhi, _mm_shuffle_epi32(vhi, _MM_SHUFFLE(2, 3, 0, 1)));
    uint32_t lo = (uint32_t)_mm_cvtsi128_si32(vlo), hi = (uint32_t)_mm_cvtsi128_si32(vhi);
    if (k < count) {
        uint32_t tlo, thi;
        rsx_index_copy_be_scalar(src + k * (sixteen ? 2u : 4u), count - k, sixteen, dst + k, &tlo, &thi);
        if (tlo < lo) lo = tlo;
        if (thi > hi) hi = thi;
    }
    *plo = lo; *phi = hi;
}
#endif

static inline void rsx_index_copy_be(const uint8_t* src, uint32_t count, int sixteen,
                                     uint32_t* dst, uint32_t* plo, uint32_t* phi)
{
#if RSX_INDEX_COPY_SIMD
    rsx_index_copy_be_sse41(src, count, sixteen, dst, plo, phi);
#else
    rsx_index_copy_be_scalar(src, count, sixteen, dst, plo, phi);
#endif
}

#endif
