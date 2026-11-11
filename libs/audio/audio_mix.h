/*
 * ps3recomp - one cellAudio port mixed into the output, by channel layout
 *
 * A port holds 1, 2 or 8 channels of big-endian floats, the 8 in the PS3's order: front left, front right,
 * centre, LFE, surround left, surround right, rear left, rear right (the SDK's order; RPCS3 #11066).
 *
 * The output is 2, 6 or 8 host floats in Windows' order, that of the speaker bits: front left and right,
 * centre, LFE, then for 5.1 the surround pair, for 7.1 the back pair and then the side pair
 * (KSAUDIO_SPEAKER_7POINT1_SURROUND). So an 8-channel port goes:
 *   - to 7.1 with its last two pairs swapped: the rears to the back speakers, the surrounds to the sides;
 *   - to 5.1 with the rears folded into the surrounds at -3 dB;
 *   - to stereo as it always has, sample for sample.
 * A 2-channel or mono port goes to the front pair alone.
 *
 * Header-only, so tests/test_audio_mix.c runs it alone.
 */
#ifndef PS3RECOMP_AUDIO_MIX_H
#define PS3RECOMP_AUDIO_MIX_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>

static inline float audio_be_f32(const float* p)
{
    uint32_t v;
    memcpy(&v, p, 4);
    v = (v >> 24) | ((v >> 8) & 0xFF00u) | ((v << 8) & 0xFF0000u) | (v << 24);
    float f;
    memcpy(&f, &v, 4);
    return f;
}

/* Add n frames of a port of nch channels into out, of out_ch channels, each sample times level. */
static inline void audio_mix_port(float* out, uint32_t out_ch, const float* src, uint32_t nch, uint32_t n,
                                  float level)
{
    for (uint32_t s = 0; s < n; s++) {
        float* o = out + (size_t)s * out_ch;
        const float* in = src + (size_t)s * nch;
        if (nch < 2) {
            const float m = audio_be_f32(in) * level;
            o[0] += m;
            o[1] += m;
            continue;
        }
        const float l = audio_be_f32(in + 0) * level, r = audio_be_f32(in + 1) * level;
        if (nch < 8) {
            o[0] += l;
            o[1] += r;
            continue;
        }
        const float c = audio_be_f32(in + 2) * level, lfe = audio_be_f32(in + 3) * level;
        const float sl = audio_be_f32(in + 4) * level, sr = audio_be_f32(in + 5) * level;   /* the surrounds */
        const float bl = audio_be_f32(in + 6) * level, br = audio_be_f32(in + 7) * level;   /* the rears */
        if (out_ch >= 8) {
            o[0] += l; o[1] += r; o[2] += c; o[3] += lfe;
            o[4] += bl; o[5] += br; o[6] += sl; o[7] += sr;
        } else if (out_ch >= 6) {
            o[0] += l; o[1] += r; o[2] += c; o[3] += lfe;
            o[4] += sl + bl * 0.707f;
            o[5] += sr + br * 0.707f;
        } else {
            /* The centre at -3 dB, the LFE and the surrounds at half, the rears at 0.3, added in the order the
             * stereo mix always added them, so its output stays the same to the bit. Each product is its own
             * statement: within one expression the compiler may fuse a multiply and an add (-ffp-contract=on). */
            const float lc = c * 0.707f, lf = lfe * 0.5f;
            const float ls = sl * 0.5f, rs = sr * 0.5f, lb = bl * 0.3f, rb = br * 0.3f;
            const float lsum = ((lc + lf) + ls) + lb, rsum = ((lc + lf) + rs) + rb;
            o[0] += l + lsum;
            o[1] += r + rsum;
        }
    }
}

#endif /* PS3RECOMP_AUDIO_MIX_H */
