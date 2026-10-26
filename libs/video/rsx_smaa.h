/*
 * ps3recomp - SMAA 1x for the D3D12 presentation pass (rsx_smaa.c)
 */
#ifndef PS3RECOMP_RSX_SMAA_H
#define PS3RECOMP_RSX_SMAA_H

#ifdef __cplusplus
extern "C" {
#endif

/* A shader of a pass (0 = edge detection, 1 = blending weights, 2 =
 * neighbourhood blending), vertex (vs = 1) or pixel, compiled for *_5_0: an
 * ID3DBlob*, or NULL with the compiler's message printed. The passes read
 * t0-t2 with s0 (linear) and s1 (point), take the frame's metrics
 * (1/w, 1/h, w, h) as the first root constants at b0, and draw one triangle
 * over the whole target. */
void* rsx_smaa_compile(int pass, int vs);

/* The lookup textures: AreaTex (R8G8, 160 x 560) and SearchTex (R8, 64 x 16),
 * rows packed. */
const unsigned char* rsx_smaa_area(unsigned* w, unsigned* h, unsigned* bytes_per_pixel);
const unsigned char* rsx_smaa_search(unsigned* w, unsigned* h, unsigned* bytes_per_pixel);

#ifdef __cplusplus
}
#endif

#endif /* PS3RECOMP_RSX_SMAA_H */
