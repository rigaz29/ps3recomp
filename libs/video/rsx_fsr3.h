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

/* Every presented frame, before the command list closes. */
void rsx_fsr3_fg_frame(void* cmd_list, float frame_ms);

#ifdef __cplusplus
}
#endif
#endif /* _WIN32 */
#endif /* RSX_FSR3_H */
