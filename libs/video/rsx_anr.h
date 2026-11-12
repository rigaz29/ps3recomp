/*
 * ps3recomp - ANR FG, Apex Neural Rendering's frame generation (experimental, PS3RECOMP_ANR)
 *
 * The player's own nvngx_dlssg.dll from an ANR FG release: a DLSS-G provider with NGX's D3D12
 * entry points of its own, driven here the way a DLSS-G host drives it. FSR 3.1's frame
 * generation swap chain still presents and paces the frames; its callback asks this for the
 * generated one (rsx_fsr3.cpp). ANR comes with no license, so nothing of it is kept or shipped
 * here: it is loaded only from where a player points.
 */
#ifndef RSX_ANR_H
#define RSX_ANR_H

#ifdef _WIN32

#include <d3d12.h>

/* What one generated frame is made from. */
struct rsx_anr_frame {
    ID3D12Resource* backbuffer;             /* the real frame shown after it, HUD and all */
    D3D12_RESOURCE_STATES backbuffer_state;
    ID3D12Resource* output;                 /* the generated frame, at the display's size */
    D3D12_RESOURCE_STATES output_state;
    ID3D12Resource* hudless;                /* or NULL: the picture without its HUD, at the display's size */
    ID3D12Resource* depth;                  /* R32_FLOAT at the render's size, 0 near */
    ID3D12Resource* motion;                 /* R16G16_FLOAT at the render's size, UV units, a pixel to where it was */
    unsigned display_w, display_h, render_w, render_h;
    float jitter_x, jitter_y;               /* pixels */
    float znear, zfar, fov;                 /* fov: vertical, radians */
    float pos[3], up[3], right[3], fwd[3];
    int reset;                              /* nothing to go by from the frame before */
};
/* hudless, depth and motion come and go in PIXEL_SHADER_RESOURCE | NON_PIXEL_SHADER_RESOURCE. */

/* Loads the DLL (a path, or one relative to the exe's folder) and starts it on the device. 1 if it can be used. */
int rsx_anr_open(ID3D12Device* device, const char* dll);

/* Records the generated frame into output. Where there is none to show (a reset, a failure),
 * output gets the real frame instead. 1 if ANR made it. */
int rsx_anr_generate(ID3D12GraphicsCommandList* cl, const rsx_anr_frame* f);

/* 0 once ANR cannot make the frames: its feature could not be made, or ten evaluations in a row
 * failed. FSR's own frame generation takes over then (rsx_fsr3.cpp). */
int rsx_anr_usable(void);

/* Releases the feature, for a resize; the next rsx_anr_generate makes it again. */
void rsx_anr_release(void);

#endif /* _WIN32 */
#endif /* RSX_ANR_H */
