/*
 * ps3recomp - NVIDIA Reflex through NVIDIA Streamline (graphics.reflex), D3D12 on Windows.
 *
 * Streamline is loaded at run time from sl.interposer.dll beside the exe, NVIDIA-signed, with manual
 * hooking: nothing of the backend's D3D12 or DXGI goes through it except the swap chain, which is made
 * through Streamline's factory so its present runs on each frame. The headers are
 * third_party/streamline's (MIT).
 *
 * The frame, as the PS3 title has it: the title builds frame N+1 once flip N completes, so the Reflex
 * sleep for N+1 goes just before that completion (cellGcmTickFlip), with the simulation's start just
 * after it; the simulation ends when the drain reaches the title's next flip, and frame N's render
 * submission and present are the backend's present of it.
 */
#ifndef PS3RECOMP_RSX_SL_H
#define PS3RECOMP_RSX_SL_H

#ifdef __cplusplus
extern "C" {
#endif

/* The markers of a frame the backend sets: its render submission and its present. */
enum { RSX_SL_SUBMIT_START, RSX_SL_SUBMIT_END, RSX_SL_PRESENT_START, RSX_SL_PRESENT_END };

/* Streamline loaded and Reflex set up on this device (an ID3D12Device*), whose adapter has this LUID (a
 * LUID*) and PCI vendor id, with graphics.reflex's mode: "off", "on" or "boost". Before the swap chain is
 * made: it is made through rsx_sl_factory's factory when this returns 1. 0: Streamline is not used (off,
 * not an NVIDIA GPU, no DLL beside the exe, not NVIDIA's, Reflex not supported); why goes to the log. */
int rsx_sl_start(void* device, const void* luid, unsigned vendor, const char* mode);
/* Streamline's factory for this DXGI factory (an IDXGIFactory*, given its own reference), or NULL. */
void* rsx_sl_factory(void* factory);
/* 1 once rsx_sl_start took. */
int rsx_sl_on(void);
/* 1 when this swap chain (an IDXGISwapChain*) is Streamline's, so its presents go through Streamline. */
int rsx_sl_presents_through(void* swap_chain);
/* The flip that releases the title's next frame: after = 0 just before it completes (the sleep), 1 just
 * after (the next frame's simulation starts). */
void rsx_sl_flip_release(int after);
/* The drain reached the title's flip: the frame's simulation has ended. */
void rsx_sl_flip_reached(void);
/* One of RSX_SL_* for the frame being presented. */
void rsx_sl_marker(int which);
/* A window message: PCL's latency ping comes as one. */
void rsx_sl_window_message(unsigned msg);
/* For the stats panel: "Reflex On", "Reflex On + Boost", "Reflex: latency stats only" (no low latency on this GPU),
 * or "" (Streamline not in use). */
const char* rsx_sl_label(void);
void rsx_sl_shutdown(void);

#ifdef __cplusplus
}
#endif

#endif /* PS3RECOMP_RSX_SL_H */
