/*
 * ps3recomp - AMD FidelityFX FSR 1 for the D3D12 presentation pass (rsx_fsr1.c)
 */
#ifndef PS3RECOMP_RSX_FSR1_H
#define PS3RECOMP_RSX_FSR1_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* EASU's sixteen constants for an image of in_w x in_h scaled to out_w x out_h. */
void rsx_fsr1_easu_con(uint32_t con[16], float in_w, float in_h, float out_w, float out_h);

/* RCAS's four constants; stops is AMD's scale: 0 is the sharpest, each stop
 * halves it. */
void rsx_fsr1_rcas_con(uint32_t con[4], float stops);

/* The pixel shader of a pass (0 = EASU, 1 = RCAS), compiled for ps_5_0: an
 * ID3DBlob*, or NULL with the compiler's message printed. Its input is the
 * presentation pass's vertex output (SV_POSITION, TEXCOORD0); it reads t0 with
 * s0 and takes its constants as the root constants at b0. */
void* rsx_fsr1_compile(int pass);

#ifdef __cplusplus
}
#endif

#endif /* PS3RECOMP_RSX_FSR1_H */
