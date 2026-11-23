/*
 * ps3recomp - NVIDIA DLSS Super Resolution and DLAA through NGX (Windows, PS3RECOMP_DLSS; see rsx_dlss.cpp)
 */
#ifndef RSX_DLSS_H
#define RSX_DLSS_H
#ifdef _WIN32
#include <wchar.h>
#ifdef __cplusplus
extern "C" {
#endif

/* NGX on the device, and whether DLSS can run there: 1 if it can. On 0, why has the reason for the log ("the NVIDIA
 * driver is too old for DLSS: 520.0 or newer", ...) and nothing is left up. dll_dir: a folder to look for
 * nvngx_dlss.dll in besides the exe's own, or NULL. data_dir: a folder NGX may write to. Once, until
 * rsx_dlss_shutdown. */
int rsx_dlss_init(void* device, const wchar_t* dll_dir, const wchar_t* data_dir, char* why, unsigned why_size);

/* graphics.dlss_preset: 'J', 'K', 'L' or 'M' asks every mode for that preset; anything else leaves each mode its
 * DLL's own. Before rsx_dlss_up_create. */
void rsx_dlss_preset(char letter);

/* The DLSS feature from render_w x render_h to display_w x display_h, set up on cmd_list (an
 * ID3D12GraphicsCommandList*, which DLSS records its first work on). The mode is the one whose range takes the render
 * size and whose own scale is nearest: DLAA, Quality, Balanced, Performance or Ultra Performance. 1 if it is up; on
 * 0, why has the reason. */
int rsx_dlss_up_create(void* cmd_list, unsigned display_w, unsigned display_h, unsigned render_w, unsigned render_h,
                       char* why, unsigned why_size);

/* Releases the feature, for another rsx_dlss_up_create after a resize. The GPU must be done with it. */
void rsx_dlss_up_destroy(void);

/* The jitter of the frame-th frame in render pixels, for the projection: Halton (2, 3) over 8 * scale^2 phases, as
 * NVIDIA's guide has it. 0, 0 while there is no feature. */
void rsx_dlss_up_jitter(unsigned frame, float* x, float* y);

/* One frame into output (ID3D12Resource*, the display's size, UNORDERED_ACCESS) from color (the render's size,
 * PIXEL_SHADER_RESOURCE), depth (R32_FLOAT, 0 at the near plane) and motion (R16G16_FLOAT, UV units, from a pixel to
 * where it was, without the jitter; both in PIXEL_SHADER_RESOURCE | NON_PIXEL_SHADER_RESOURCE). Each is left in the
 * state it came in. jitter_x, jitter_y: the frame's from rsx_dlss_up_jitter. reset: the history means nothing (a cut).
 * Afterwards the command list's descriptor heaps, root signature and pipeline state are NGX's. 1 if it ran. */
int rsx_dlss_up_frame(void* cmd_list, void* color, void* depth, void* motion, void* output, float jitter_x,
                      float jitter_y, int reset, float frame_ms);

/* What is in use, for the log and the stats panel: "DLSS 310.9.16 Quality", "DLSS" before the feature is up. */
const char* rsx_dlss_name(void);

/* Releases the feature and NGX. The GPU must be done with them. */
void rsx_dlss_shutdown(void);

#ifdef __cplusplus
}
#endif
#endif /* _WIN32 */
#endif /* RSX_DLSS_H */
