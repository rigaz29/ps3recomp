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
    void* objects;          /* ID3D12Resource*, or NULL: R16G16_FLOAT at the render's size, readable,
                             * the motion in UV units of what moves, over 30000 where nothing does */
    float cam[16];          /* world to clip, four columns: x, y, z and w's */
    float z_scale, z_offset;/* clip z to the depth range: depth = ndc * scale + offset */
    float jitter_x, jitter_y;   /* of this frame, in pixels */
} rsx_fsr3_frame;

/* The frame's camera, and depth and motion into the form the API takes. 1 if the frame has a
 * camera to go by. rsx_fsr3_fg_frame does this itself if it was not done. */
int rsx_fsr3_prepare(void* cmd_list, const rsx_fsr3_frame* frame);

/* Every presented frame, before the command list closes. */
void rsx_fsr3_fg_frame(void* cmd_list, const rsx_fsr3_frame* frame, float frame_ms);

/* The upscaler, from render_w x render_h to the display's size. sharpness 0 to 1, below 0 none. */
int rsx_fsr3_up_create(void* device, unsigned display_w, unsigned display_h,
                       unsigned render_w, unsigned render_h, float sharpness);

/* Destroys it, for another rsx_fsr3_up_create after a resize. */
void rsx_fsr3_up_destroy(void);

/* The jitter of the frame-th frame, in pixels. */
void rsx_fsr3_up_jitter(unsigned frame, float* x, float* y);

/* After rsx_fsr3_prepare: color, in PIXEL_SHADER_RESOURCE, at the render's size, into output, in
 * UNORDERED_ACCESS, at the display's. */
void rsx_fsr3_up_frame(void* cmd_list, const rsx_fsr3_frame* frame, void* color, void* output, float frame_ms);

/* Frames a second under which frames are shown as they are, until it recovers. 0: never. */
void rsx_fsr3_fg_floor(double fps);

/* ANR FG (experimental, PS3RECOMP_ANR): its DLL makes the generated frames in place of FSR's, and
 * the same swap chain presents and paces them. After rsx_fsr3_fg_create; 1 if it is used. */
int rsx_fsr3_fg_anr(const char* dll);

#ifdef __cplusplus
}
#endif
#endif /* _WIN32 */
#endif /* RSX_FSR3_H */
