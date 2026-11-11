/* libs/audio/audio_mix.h: a cellAudio port into a stereo, 5.1 or 7.1 output. The 7.1 order, the 5.1 fold, and the
 * stereo mix sample for sample as cellAudio.c computed it before the layouts. */
#include "audio_mix.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

static int fails = 0;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

static float be(float f)
{
    uint32_t v;
    memcpy(&v, &f, 4);
    v = (v >> 24) | ((v >> 8) & 0xFF00u) | ((v << 8) & 0xFF0000u) | (v << 24);
    memcpy(&f, &v, 4);
    return f;
}

/* The stereo mix of an 8-channel port as cellAudio.c had it, statement for statement. */
static void old_stereo(float* out, const float* src, uint32_t n, float level)
{
    for (uint32_t s = 0; s < n; s++) {
        float left = audio_be_f32(&src[s * 8 + 0]) * level;
        float right = audio_be_f32(&src[s * 8 + 1]) * level;
        float center = audio_be_f32(&src[s * 8 + 2]) * level * 0.707f;
        float lfe = audio_be_f32(&src[s * 8 + 3]) * level * 0.5f;
        float rl = audio_be_f32(&src[s * 8 + 4]) * level * 0.5f;
        float rr = audio_be_f32(&src[s * 8 + 5]) * level * 0.5f;
        float sl = audio_be_f32(&src[s * 8 + 6]) * level * 0.3f;
        float sr = audio_be_f32(&src[s * 8 + 7]) * level * 0.3f;
        left += center + lfe + rl + sl;
        right += center + lfe + rr + sr;
        out[s * 2 + 0] += left;
        out[s * 2 + 1] += right;
    }
}

int main(void)
{
    /* One frame: the channels 0.01 to 0.08, so each one can be told apart where it lands. */
    float port[8];
    for (int i = 0; i < 8; i++) port[i] = be(0.01f * (float)(i + 1));

#define CH(i) (0.01f * (float)((i) + 1))   /* port channel i's sample */
    float o8[8] = { 0 };
    audio_mix_port(o8, 8, port, 8, 1, 1.0f);
    const int from8[8] = { 0, 1, 2, 3, 6, 7, 4, 5 };   /* the rears to the back pair, the surrounds to the sides */
    for (int i = 0; i < 8; i++) CHECK(o8[i] == CH(from8[i]));

    float o6[6] = { 0 };
    audio_mix_port(o6, 6, port, 8, 1, 1.0f);
    CHECK(o6[0] == CH(0) && o6[1] == CH(1) && o6[2] == CH(2) && o6[3] == CH(3));
    CHECK(fabsf(o6[4] - (CH(4) + CH(6) * 0.707f)) < 1e-6f);
    CHECK(fabsf(o6[5] - (CH(5) + CH(7) * 0.707f)) < 1e-6f);

    /* Stereo: the same bits as before, over many frames and a level below one, added to what is there. */
    enum { N = 4096 };
    static float many[N * 8], a[N * 2], b[N * 2];
    uint32_t x = 12345;
    for (int i = 0; i < N * 8; i++) {
        x = x * 1664525u + 1013904223u;
        many[i] = be(((float)(x >> 8) / 16777216.0f) * 2.0f - 1.0f);
    }
    for (int i = 0; i < N * 2; i++) a[i] = b[i] = 0.001f * (float)(i % 7);
    audio_mix_port(a, 2, many, 8, N, 0.8f);
    old_stereo(b, many, N, 0.8f);
    CHECK(memcmp(a, b, sizeof a) == 0);

    /* A stereo port: the front pair alone, whatever the layout. */
    float st[2] = { be(0.25f), be(-0.5f) };
    float s8[8] = { 0 }, s6[6] = { 0 }, s2[2] = { 0 };
    audio_mix_port(s8, 8, st, 2, 1, 1.0f);
    audio_mix_port(s6, 6, st, 2, 1, 1.0f);
    audio_mix_port(s2, 2, st, 2, 1, 1.0f);
    CHECK(s8[0] == 0.25f && s8[1] == -0.5f && s8[2] == 0 && s8[4] == 0 && s8[7] == 0);
    CHECK(s6[0] == 0.25f && s6[1] == -0.5f && s6[5] == 0);
    CHECK(s2[0] == 0.25f && s2[1] == -0.5f);

    /* Mono: both front channels. */
    float mono[1] = { be(0.5f) };
    float m8[8] = { 0 };
    audio_mix_port(m8, 8, mono, 1, 1, 0.5f);
    CHECK(m8[0] == 0.25f && m8[1] == 0.25f && m8[2] == 0 && m8[6] == 0);

    printf(fails ? "audio_mix: %d failed\n" : "audio_mix: ok\n", fails);
    return fails != 0;
}
