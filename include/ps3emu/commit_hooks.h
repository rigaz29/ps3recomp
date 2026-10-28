/* Hooks on a committed atomic write to a 128-byte line, for diagnostics.
 *
 * Defined here, in the header (selectany / weak), so that whatever sets a hook
 * and whatever calls it need not link against each other: the PPU loader's
 * fixtures build ppu_loader.cpp on its own, and the SPURS fixtures cellSpurs.c. */
#ifndef PS3EMU_COMMIT_HOOKS_H
#define PS3EMU_COMMIT_HOOKS_H

#include <stdint.h>

#if defined(_MSC_VER)
#  define PS3_HOOK_SHARED __declspec(selectany)
#else
#  define PS3_HOOK_SHARED __attribute__((weak))
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* A PPU stwcx./stdcx. committed to `line` (SPURS_KICK_PROF sets it). */
PS3_HOOK_SHARED void (*g_ppu_line_commit_hook)(uint32_t line) = 0;

/* A PPU atomic commit to `line` by guest thread `who`, for PS3_FT_LINE
 * (spu_channels.c sets it once a word is watched). */
PS3_HOOK_SHARED void (*g_ps3_ft_line_hook)(uint32_t line, uint32_t who) = 0;

#ifdef __cplusplus
}
#endif

#endif
