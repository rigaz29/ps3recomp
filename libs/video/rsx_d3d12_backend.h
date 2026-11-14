/*
 * ps3recomp - D3D12 RSX Backend
 *
 * Translates RSX GPU commands to Direct3D 12 rendering.
 * Implements the rsx_backend callback interface.
 *
 * Architecture:
 *   rsx_commands.c (state tracking) → rsx_backend callbacks → this file
 *                                                               ↓
 *                                                         D3D12 device
 *                                                         swap chain
 *                                                         command lists
 *                                                         pipeline states
 *
 * Current scope (Phase 1):
 *   - Device/swap chain creation
 *   - Clear to RSX clear color
 *   - Present with vsync
 *   - Basic triangle rendering (vertex position + color)
 *
 * Future phases:
 *   - Texture upload and sampling
 *   - RSX vertex/fragment program → HLSL translation
 *   - Framebuffer resolve and display buffer management
 *   - Depth/stencil buffer creation
 */

#ifndef PS3RECOMP_RSX_D3D12_BACKEND_H
#define PS3RECOMP_RSX_D3D12_BACKEND_H

#include "rsx_commands.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Initialize the D3D12 backend.
 * Creates a Win32 window, D3D12 device, swap chain, and registers
 * as the active rsx_backend.
 * Returns 0 on success, -1 on failure. */
int rsx_d3d12_backend_init(u32 width, u32 height, const char* title);

/* Shut down the D3D12 backend and release all resources. */
void rsx_d3d12_backend_shutdown(void);

/* Process Win32 messages. Returns 0 normally, -1 if window closed. */
int rsx_d3d12_backend_pump_messages(void);

/* 1 if frame generation is on, past rsx_d3d12_backend_init: the title then has to run at
 * half the display's rate, and the frame clock caps it. */
int rsx_d3d12_backend_generating(void);

/* 1 if the GPU's queue has stopped: a wait for it ran out, and its fence has not moved since. */
int rsx_d3d12_backend_gpu_stuck(void);

/* The first of the four vertex constants that hold the title's world-to-clip matrix, which
 * frame generation needs. The title's draws of what moves: prev_reg, last frame's matrix, the
 * same way, read only by them, and prev_attr, the attribute with a vertex's last position; -1
 * for none. Before rsx_d3d12_backend_init. */
void rsx_d3d12_backend_camera_register(int reg, int prev_reg, int prev_attr);

/* A line of the game's own at the foot of the stats panel: fn writes it and returns its length, 0 for none. */
void rsx_d3d12_backend_overlay_line(int (*fn)(char* out, int max));

/* How much wider than the title's 16:9 the picture is to be: 1 unless display.ultrawide is on and the screen is wider. */
float rsx_d3d12_backend_wide_ratio(void);

/* The refresh rate of the display the window opened on, 0 if Windows cannot say. */
double rsx_d3d12_backend_refresh_hz(void);

/* Before rsx_d3d12_backend_init: the title cannot give FSR 3 what it needs this run, and why. */
void rsx_d3d12_backend_fsr3_refuse(const char* why);

/* Force a present (useful for debugging). */
void rsx_d3d12_backend_present(void);

/* A precompile's start: a device for the compiler's choice, and the shader cache
 * opened. -1 with why if it cannot be: before a port reads the title's files.
 * rsx_d3d12_precompile_vertex_programs calls it too. */
int rsx_d3d12_precompile_open(char* why, size_t why_len);

/* Vertex programs into the shader cache ahead of play, without a window (a port's
 * precompile). ucode[i] is a program as the title's files hold it: bytes[i] bytes,
 * big-endian words, ending at its last instruction. Each one's text and its
 * raw-fetch text are compiled as a draw would compile them, on `threads` threads,
 * past those the cache has; the cache is packed at the end. progress(done, total)
 * is called on this thread as it goes. stats: compiled, had already, failed.
 * Returns 0, or -1 with why: no shader cache in the settings, no device, or the
 * game holding the cache. Not with the backend running. */
int rsx_d3d12_precompile_vertex_programs(const uint8_t* const* ucode, const uint32_t* bytes, uint32_t n,
                                         int threads, void (*progress)(uint32_t done, uint32_t total),
                                         uint32_t stats[3], char* why, size_t why_len);

/* The vertex pass's program (rsx_d3d12_vertex_pass_set's text) into the shader cache ahead of play, under the key
 * the game looks it up by: it is then read at the first lit draw, not compiled there. *compiled: 1 built now, 0 the
 * cache had it. Returns 0, or -1 with why. Safe beside rsx_d3d12_precompile_vertex_programs, on another thread. */
int rsx_d3d12_precompile_vertex_pass(const char* hlsl, int* compiled, char* why, size_t why_len);

#ifdef __cplusplus
}
#endif
#endif
