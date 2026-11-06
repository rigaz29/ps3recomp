/*
 * ps3recomp - AMD FSR 3.1 through the FSR API (Windows only, see rsx_fsr3.cpp)
 */
#ifndef RSX_FSR3_H
#define RSX_FSR3_H

#ifdef _WIN32

#ifdef __cplusplus
extern "C" {
#endif

/* 1 if the FSR API's loader is beside the exe and loads. */
int rsx_fsr3_available(void);

/* Puts the swap chain behind frame generation's proxy, which stands in for it
 * from then on. *swap_chain is an IDXGISwapChain3*, and the caller's only
 * reference to it: the proxy makes a swap chain of its own for the window, and
 * a window has one. On failure the swap chain is left as it was. Before the
 * first GetBuffer. */
int rsx_fsr3_wrap(void** swap_chain, void* queue);

/* Frame generation at the display's size, from a render at render_w x render_h.
 * generate 0 keeps the proxy and shows only rendered frames. */
int rsx_fsr3_fg_create(void* device, void* swap_chain, unsigned display_w, unsigned display_h,
                       unsigned render_w, unsigned render_h, int generate);

/* Destroys it, for another rsx_fsr3_fg_create after a resize. */
void rsx_fsr3_fg_destroy(void);

/* What a frame gives frame generation. With scene 0 (a menu, a movie) the frame
 * is shown as it is and the rest is not read. */
typedef struct rsx_fsr3_frame {
    int   scene;
    void* depth;            /* ID3D12Resource*: the scene's D24S8 as R24G8_TYPELESS, in
                             * NON_PIXEL_SHADER_RESOURCE, at the render's size */
    void* hudless;          /* ID3D12Resource*: the picture without its HUD, at the display's
                             * size, readable by pixel and compute shaders */
    float cam[16];          /* world to clip, four columns: x, y, z and w's */
    float z_scale, z_offset;/* clip z to the depth range: depth = ndc * scale + offset */
} rsx_fsr3_frame;

/* Every presented frame, before the command list closes. */
void rsx_fsr3_fg_frame(void* cmd_list, const rsx_fsr3_frame* frame, float frame_ms);

#ifdef __cplusplus
}
#endif
#endif /* _WIN32 */
#endif /* RSX_FSR3_H */
