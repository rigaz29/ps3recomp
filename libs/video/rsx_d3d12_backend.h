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

/* The first of the four vertex constants that hold the title's world-to-clip matrix, which
 * frame generation needs. The title's draws of what moves: prev_reg, last frame's matrix, the
 * same way, read only by them, and prev_attr, the attribute with a vertex's last position; -1
 * for none. Before rsx_d3d12_backend_init. */
void rsx_d3d12_backend_camera_register(int reg, int prev_reg, int prev_attr);

/* Force a present (useful for debugging). */
void rsx_d3d12_backend_present(void);

#ifdef __cplusplus
}
#endif
#endif
