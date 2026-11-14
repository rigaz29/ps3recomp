/*
 * ps3recomp - D3D12 RSX Backend
 *
 * Translates RSX GPU state to D3D12 rendering commands.
 *
 * Phase 1 implementation:
 *   - Win32 window + D3D12 device + swap chain
 *   - Clear render target to RSX clear color
 *   - Present with vsync
 *   - Basic vertex-colored triangle rendering
 *
 * This file is C with COM calls (D3D12 is a COM API).
 * We use the C interface (__uuidof not available in C, so we
 * define GUIDs manually).
 */

#ifdef _WIN32

#include <time.h>
#include <stdarg.h>
#include "ps3emu/log_first.h"
#include "ps3emu/ps3_settings.h"
#include "rsx_kept.h"
#include "rsx_fsr1.h"
#include "rsx_smaa.h"
#include "ps3emu/thread_name.h"
#include "rsx_d3d12_backend.h"
#include "rsx_present_policy.h"
#include "rsx_primitives.h"
#include "rsx_vertex_fetch.h"
#include "rsx_texture_layout.h"
#include "rsx_index_copy.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <tlhelp32.h>
#include <psapi.h>     /* K32GetProcessMemoryInfo: what the pipeline cache's build costs */
#include <intrin.h>    /* __cpuid: the CPU's name in the stats panel */
#include <io.h>        /* _chsize_s, _get_osfhandle: the shader cache's journal */
#include <errno.h>
#include "zstd.h"      /* third_party/zstd: the shader cache's pack */

/* We need these GUIDs — define them here to avoid uuid.lib dependency.
 * initguid.h only turns DEFINE_GUID into a definition for headers included
 * after it, so it has to come first. */
#include <initguid.h>

/* D3D12 headers */
#include <d3d12.h>
#include <d3d12sdklayers.h>   /* ID3D12Debug / ID3D12InfoQueue */
#include <dxgi1_4.h>
#include <dxgi1_6.h>
#include <d3dcompiler.h>
#include "rsx_dxc.h"
#include "rsx_fsr3.h"

/* Link libraries */
#pragma comment(lib, "d3d12.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "d3dcompiler.lib")

/* ---------------------------------------------------------------------------
 * Constants
 * -----------------------------------------------------------------------*/

#define FRAME_COUNT         2   /* double buffering */
#define MAX_VERTICES    393216  /* per-frame vertex buffer (dbgfont submits
                                 * ~7.5k verts/frame; leave generous headroom) */
/* Per-frame index buffer of indexed VP draws, x2 parity. God of War: Ascension's gate takes
 * ~1.5 M a frame with EDGE culling its triangles on the SPUs and ~2.5 M without; past the end a
 * batch falls to the CPU's conversion (ib_full_note), which at that rate cost two thirds of a
 * core and dropped most of the frame's geometry (2 M, the old size, went black). */
#define VP_MAX_INDICES  (1u << 22)
#define MAX_DRAWS         8192  /* per-frame draw records. 2048 was sized
                                 * from God of War HD's worst case (~1841):
                                 * God of War: Ascension's first level needs
                                 * up to 3226, and two thirds of its frames
                                 * lost the draws past 2048 -- the HUD and
                                 * effects drawn last among them. A full batch
                                 * still drops, and says so once (rsx_draw_room).
                                 * The per-draw constant buffer is
                                 * VP_CB_STRIDE * MAX_DRAWS * 2 bytes, so 16384
                                 * asked for a 264 MB upload allocation. */
/* Per-draw VP constant-buffer slot: vp_c[512] + posscale + posoffset
 * (514 vec4 = 8224 B), then the raw-fetch attribute table (vp_fa[16] and
 * vp_fk[16], 32 vec4, at VP_CB_RAW_OFF), rounded up to D3D12's 256-byte CBV
 * alignment.
 * Constants are snapshotted at RECORD time -- wave's passes each set their
 * own texScale/offset uniforms, so one per-frame snapshot ran every pass
 * with the LAST pass's constants. */
/* Fragment texture units a draw carries: all sixteen. This backend bound
 * units 0-3 only, and a program that samples a higher one failed its pipeline
 * (the root signature lacked the register, E_INVALIDARG) and its draws were
 * dropped: God of War: Ascension's menu and level have many such programs. */
#define VP_TEX_UNITS      16
#define VP_CB_STRIDE      8960
#define VP_CB_RAW_OFF     8224
/* b1, a draw's fragment program constants: the PSConstants of
 * rsx_fp_decompile_buffered_*, rearranged by vp_get_fp_pso so the fixed members
 * come first -- fp_tcscale[16] (c0-c15), fp_alpha (c16), fp_constants[] (c17
 * on). Each draw's slot is as long as its program needs (vp_record_cb), taken
 * in turn from its frame's half of vp_fpcb; up to FP_CB_MAX_K constants, a
 * program with more is compiled with them as literals. The guard after the
 * second half keeps a slot read past its end inside the buffer. */
#define FPCB_HALF         ((u32)MAX_DRAWS * 2048u)
#define FPCB_GUARD        (64u * 1024u)
#define FPCB_ALPHA        16
#define FPCB_CONSTS       17
#define FP_CB_MAX_K       fp_cb_max_k()
/* RSX_FP_CB_MAXK=<n>: fewer constants read from b1 (1-1024, default 1024). */
static u32 fp_cb_max_k(void)
{
    static u32 k;
    if (!k) {
        const char* e = getenv("RSX_FP_CB_MAXK");
        const int v = e ? atoi(e) : 1024;
        k = (u32)(v < 1 ? 1 : v > 1024 ? 1024 : v);
    }
    return k;
}
/* How far a fragment program is read looking for its end: instructions and
 * their inline constants, 16 bytes each. It was 4 KB, and God of War:
 * Ascension's depth of field pass (249 inline constants among its
 * instructions) runs past that: decompiled without its last instructions it
 * exported zero, and with no end found its pipeline was keyed on its first 64
 * bytes, whose animated focus constant made a new program every frame. */
#define FP_MAX_BYTES      (16u * 1024u)
#define VERTEX_STRIDE       36  /* bytes per host vertex: pos3 + col4 + uv2 */

typedef struct {
    u32 vb_byte_offset; /* offset into vb_mapped where this draw's verts live */
    u32 vertex_count;
    u32 topology;       /* rsx_topology (NOT a D3D value; see topo_to_d3d) */
    int textured;       /* 1 = sample the bound font/atlas texture (dbgfont) */
    int is_vp;          /* 1 = real VP path: vb_byte_offset indexes vp_vb (float4) */
    /* VP path per-draw shader/texture state, captured at draw_arrays time. */
    u32 fp_addr;        /* SET_SHADER_PROGRAM value (guest FP ucode location)   */
    u32 fp_ctrl;        /* SET_SHADER_CONTROL export bits at draw time (FP_CTRL_BITS) */
    u32 l2_inval;       /* texture cache invalidates the title had sent by this draw */
    u32 wpos_win;       /* SET_SHADER_WINDOW when it moves WPOS off the host's
                         * top-left, half-pixel position; else 0 (rec_wpos_win) */
    u32 alpha_ctl;      /* alpha test: enable<<16 | (func&0xFF)<<8 | ref */
    u32 begin_epoch;    /* SET_BEGIN_END generation, for batch concatenation */
    u32 cull;           /* packed face culling: bit0 enable, bit1 cull FRONT
                         * (else BACK), bit2 front face is CCW. RSX culls back
                         * faces on most solid geometry; rendering everything
                         * double-sided lets a shell's interior faces show
                         * through and shade unlit (black). */
    u64 zs_key;         /* packed guest depth/stencil state (rsx_zs_key), PSO key */
    u64 poly;           /* polygon offset, rsx_poly_key (PSO key) */
    u32 blend_color, blend_color2;   /* constant blend colour (dynamic state) */
    u8  blend_const;    /* 1 a colour factor is CONSTANT_COLOR, 2 one is CONSTANT_ALPHA */
    u32 blend_mrt, cmask_mrt;        /* SET_BLEND_ENABLE_MRT, SET_COLOR_MASK_MRT */
    u32 dbt, dbt_min, dbt_max;       /* depth bounds: enable, min/max float bits */
    u32 clip_min, clip_max;          /* depth range, float bits */
    u32 zmm, aa;                     /* SET_ZMIN_MAX_CONTROL, SET_ANTI_ALIASING_CONTROL */
    u32 stencil_ref;    /* guest stencil reference at draw time (dynamic state) */
    u32 cmask;          /* D3D write mask from SET_COLOR_MASK at draw time
                         * (wave's sim passes write single lanes of the height
                         * maps; ignoring the mask stomped persistent state) */
    /* Per-unit textures (t0-t15). Captured at draw time. */
    struct {
        u32 off;        /* resolved vm offset (guest upload source), 0 = none */
        u32 raw;        /* raw RSX offset (offscreen-RT matching) */
        u32 w, h, fmt;  /* dims + RSX base format */
        u32 ctrl1;      /* NV4097 TEXTURE_CONTROL1: component remap crossbar */
        u32 mips;       /* SET_TEXTURE_FORMAT bits 16..31: mipmap level count.
                         * Cube faces sit one whole mip pyramid apart, so this
                         * is what sets the face stride. */
        int cube;       /* SET_TEXTURE_FORMAT bit 2: a cube texture. Sampled
                         * with a 3-component direction, not a 2D uv -- treating
                         * one as 2D is what makes an environment-mapped chrome
                         * surface come out with black patches and banding. */
        u32 depth;      /* a volume's depth (SET_TEXTURE_FORMAT dimension 3,
                         * CONTROL3 bits 20..31); 1 for anything else */
        int set;
        u32 addr, filter, ctrl0, border;   /* TEXTURE_ADDRESS/FILTER/CONTROL0/BORDER_COLOR */
    } tex[VP_TEX_UNITS];
    int tex_rt[VP_TEX_UNITS];   /* pre-pass: OffRT index sampled by unit, -1 = none */
    int tex_slot;       /* legacy single-slot path (atlas); -1 = none */
    int vs_idx;         /* VPVSEntry slot for this draw's vertex program (-1 = primary) */
    int blend;          /* guest blend enable at draw time */
    u32 blend_key;      /* packed guest blend state (factors+equation), PSO key */
    /* Render-to-texture: which colour surface this op targets. 0 = a display
     * buffer (the swap-chain backbuffer); else the resolved vm offset of an
     * offscreen surface (demosaic chains its effect passes through local-
     * memory buffers and composites from them). */
    u32 rt_off;
    u32 surf_a;         /* the colour surface's raw offset, display buffers
                         * included (rt_off folds those into 0) -- for RSX_CAP */
    /* Which per-draw constant slot this record's constants were written to.
     * vp_record_cb writes them keyed by the RECORD index, so any pass that
     * reorders or compacts s_d3d.draws (DRAW_KEEP_TEX, DRAW_LAST_TEX,
     * DRAW_LIMIT) would otherwise make every surviving draw read some other
     * object's MVP -- silently transforming it somewhere else entirely. */
    u32 cb_slot;
    const u8* fp_snap;  /* its fragment program as recorded (fp_snapshot), or NULL */
    u32 fp_snap_len;
    u64 fp_shash;       /* with fp_snap: the program's structural hash, */
    u32 fp_shash_nk;    /* its inline constant count by that walk, */
    u32 fp_units;       /* the texture units it samples, */
    const u8* fp_consts;/* and its constants as rsx_fp_collect_constants gives them */
    int fp_nconsts;
    u32 fpcb_off;       /* its b1 slot in this frame's half of vp_fpcb */
    u32 rt_mrt[3];      /* colour targets B,C,D (MRT1/2/3), 0 = none. Deferred
                         * shading G-buffers write 3-4 targets in one pass. */
    u32 rt_w, rt_h;     /* surface clip dims at record time (offscreen RT size) */
    u32 rt_fmt;         /* RSX surface colour format (SET_SURFACE_FORMAT [4:0]) */
    u32 zeta_off;       /* depth surface (SET_SURFACE_ZETA_OFFSET) when the surface has one */
    int no_color;       /* colour target NONE with a depth surface: a depth-only pass */
    int tex_cv[VP_TEX_UNITS];   /* pre-pass: OffRT holding unit u's RGBA8 target read as Y16_X16_FLOAT, -1 = none */
    int tex_ds[VP_TEX_UNITS];   /* pre-pass: OffDS read through unit u (its colour copy is tex_rt), -1 = none */
    int tex_dz[VP_TEX_UNITS];   /* pre-pass: OffDS whose depth plane unit u samples as a depth texture, -1 = none */
    u32 srv_win;        /* pre-pass: the draw whose t0-t15 window this one binds (itself, or an
                         * earlier draw of the batch with the same 16 views: srvq_commit) */
    /* Guest viewport rect at draw time (target pixels). Sub-viewport layouts
     * (wave's debug tiles) position quads with this, not with constants --
     * forcing full-target viewports drew every tile window-sized. */
    u32 vp_x, vp_y, vp_w, vp_h;
    /* Guest scissor rect at record time (SET_SCISSOR_HORIZONTAL/VERTICAL,
     * target pixels): the RSX clips draws and clears to it. */
    u32 sc_x, sc_y, sc_w, sc_h;
    /* Ordered clear op (offscreen surfaces only; display clears stay the
     * frame-start backbuffer clear). is_clear records also set is_vp so the
     * legacy replay pass skips them. */
    int   is_clear;
    float cc[4];
    u32   clear_mask;   /* CLEAR_SURFACE bits: 0x01 depth, 0x02 stencil, 0xF0 colour */
    float clear_z;
    u8    clear_s;
    /* Indexed VP draw (upload_tris_vp_ix): ib_first/index_count are u32
     * indices into vp_ib, relative to the vertex at vb_byte_offset. */
    int   indexed;
    u32   ib_first, index_count;
    /* Raw-fetch draw (upload_tris_vp_raw), also `indexed`: the vertex shader
     * reads the guest's own vertex bytes. Drawn as seg_n segments of s_seg,
     * each with its own index range and attribute bases. */
    int   raw;
    u32   seg_first, seg_n;
    /* Blit snapshot op (also is_clear, so the draw passes leave it alone): copy
     * the display surface into s_bsnap[snap_slot], keyed by snap_ea, the
     * address the blit's destination starts at. See "Blit snapshots". */
    u32   snap_ea;
    int   snap_slot;
    u32   snap_src;     /* RSX_DISPLAY_RT: the source display buffer's target key */
    /* Local-to-local copy op (also is_clear): w x h pixels from the byte
     * offset blit_src (pitch blit_sp) to blit_dst (pitch blit_dp), replayed
     * between the targets that hold them (blit_rt_copy). */
    int   is_blit;
    /* Flip op (also is_clear): RSX_DISPLAY_RT's key of the display buffer a
     * flip named at this point of the batch (d3d12_flip_observed). */
    u32   flip_key;
    /* ZPASS query op (also is_clear): 1 = CLEAR_REPORT_VALUE, 2 = GET_REPORT,
     * whose count goes into the value word at zq_ea (report number zq_seq),
     * 3 = SET_ZPASS_PIXEL_COUNT_ENABLE, on or off in zq_ea (d3d12_zpass_observed). */
    u32   zq_op, zq_ea, zq_seq;
    u16   tex_zok;      /* prepass: units whose offset a depth surface wrote
                         * last (or nothing seen yet), so may be read as depth */
    u32   blit_src, blit_sp, blit_dst, blit_dp, blit_w, blit_h;
    /* A colour target in main memory (rec_mwb): its offset (0 = none), pitch,
     * clip and SET_SURFACE_FORMAT, for the write-back at the batch's end. */
    u32   mwb_off, mwb_pitch, mwb_w, mwb_h, mwb_fmt;
    u8    wide_cam;     /* display.ultrawide: draws the scene through the title's camera */
} D3D12DrawRecord;
/* An indexed run (upload_tris_vp_ix) may take the next batch; cleared with the records. */
static int s_ix_run_valid;
/* Raw-fetch segments, one per batch that needed its own copy; reset with the
 * records. hdr is the byte offset in vp_vb of the batch's 16 attribute bases;
 * with lit set, the segment's draws read the vertex pass's copy, whose header
 * is at lhdr in vp_lit (see "The vertex pass"). */
#define VP_MAX_SEGS 16384
typedef struct { u32 ib_first, count, hdr, lo, hi, lhdr; int lit; } VPRawSeg;
static VPRawSeg s_seg[VP_MAX_SEGS];
static u32 s_seg_n;
static int s_raw_run_valid;
/* The resident parts of vp_vb and vp_ib (rsx_kept.h): sizes in bytes and indexes, what is used. */
static u32 s_res_vb_size, s_res_ib_size, s_res_vb_used, s_res_ib_used;
static u32 s_res_gen;           /* what a block was put there under; a new one starts it over */
static int s_res_full;          /* a block found no room: start over after this batch */
/* Starting over: no draw takes the resident part (they go through the ring) until the fence reaches s_res_retire, the
 * value signalled after the last batch that read it; then it is written again from the start. No wait for the GPU. */
static int s_res_retiring;
static u64 s_res_retire;
static u32 s_res_retire_frames;

/* The vertex pass (phase 2b.1 of the God of War: Ascension port): see "The vertex pass" below. */
#define VPT_SEGS 8192u                      /* a batch's lit segments: 32 bytes each in its table */
#define VPT_SEGB (VPT_SEGS * 32u)
#define VPT_HALF (VPT_SEGB + (4u << 20))   /* then the records */
static struct {
    const char* hlsl;
    u32 stride, stride2, max_rec;           /* stride2: a second layout's, or 0 */
    void (*cpu)(const void* rec, const u8* src, u8* dst, u32 n);
    volatile LONG set;
    int state;                              /* 0 not built yet (or its program not compiled yet), 1 ready, -1 failed */
    double cpu_since;                       /* the first draw lit on the CPU while its program was compiled */
    ID3D12RootSignature* rs;
    ID3D12PipelineState* pso;
    ID3D12Resource* lit;                    /* default heap: two halves of lit_half, by vp_parity */
    ID3D12Resource* tab;                    /* upload: two halves of VPT_HALF */
    u8* tab_mapped;
    ID3D12CommandAllocator* alloc[FRAME_COUNT];
    ID3D12GraphicsCommandList* cl;
    u32 lit_half;
    u8* rec_tmp;                            /* a record copied out for the CPU's pass */
    /* this batch */
    u32 nseg, lit_used, rec_used;
    u32 last_key, last_rec;
    LONG last_seq;
    int last_ok;
} s_vpass;
static void vpass_submit(u32 fi);
static void vpass_reset(void);
static void vpass_check_read(void);

/* Offscreen render target (render-to-texture). Persistent across frames --
 * pass N's output is sampled by pass N+1 and possibly by later frames. */
typedef struct {
    ID3D12Resource*       res;
    ID3D12Resource*       up;          /* init-upload staging (kept alive) */
    u32                   off, w, h;   /* raw RSX offset + dims */
    u32                   pw, ph;      /* the resource's size: w, h unless it is drawn larger */
    float                 sx, sy;      /* pw / w, ph / h */
    u32                   dxgi;        /* DXGI_FORMAT of the resource */
    D3D12_RESOURCE_STATES st;          /* tracked resource state */
    int                   used;        /* referenced this frame */
    u64                   last;        /* frame_count it was last bound or sampled */
    u32                   grow_w, grow_h;   /* a size a batch that had bound it asked for: the next one makes it */
} OffRT;
/* demosaic double-buffers its 6-surface pass chain. God of War: Ascension
 * holds 29 targets by the gate after its first fight when started from that
 * checkpoint (its post chain, three display buffers, a Y16 copy for each unit
 * and size that reads one), and a longer session holds what the scenes before
 * made too. A full table evicts: the target loses what it held, and the
 * eviction waits for the GPU. With 28 slots its display buffers evicted each
 * other every frame for ~25 s there. RSX_RT_SLOTS=<n> uses fewer slots. */
#define MAX_OFF_RTS  64

/* RSX_DISPLAY_RT=1: each display buffer is an offscreen target of its own,
 * keyed DISP_RT_TAG | offset, and the present copies the one the guest flipped
 * to the backbuffer. Without it every display buffer is the one backbuffer: a
 * title that draws into the next buffer while another is on screen (God of
 * War: Ascension tonemaps into one while its video plays in another, and its
 * post chain composites into a third) overwrites the frame being shown. */
#define DISP_RT_TAG  0x40000000u
#define Y16_RT_TAG   0x20000000u   /* the Y16_X16_FLOAT copy of a target */

/* The key of a Y16_X16_FLOAT copy at the texture's own size: per unit and
 * size, so the passes that read one target at different sizes each keep their
 * copy (it is refreshed before every draw that reads it, y16_convert). Bit 28
 * is never set in a guest offset. */
static u32 y16_copy_key(int unit, u32 w, u32 h)
{
    return Y16_RT_TAG | 0x10000000u | ((u32)unit << 24) | ((w - 1u) << 12) | (h - 1u);
}

/* RSX_Y16_EXACT (default 1): a target read as Y16_X16_FLOAT goes through a
 * copy the size of the texture the program reads, and the program's
 * coordinates reach it unscaled. 0: the copy is the target's size and the
 * coordinates are scaled by texture size / target size (before 2026-11-02). */
static int y16_exact_on(void)
{
    static int on = -1;
    if (on < 0) { const char* e = getenv("RSX_Y16_EXACT"); on = e ? atoi(e) : 1; }
    return on;
}
static int display_rt_on(void)
{
    static int on = -1;
    if (on < 0) { const char* e = getenv("RSX_DISPLAY_RT"); on = e ? atoi(e) : 0; }
    return on;
}

/* The target key of a texture at local offset `raw` (effective address `ea`):
 * a display buffer read as a texture is that buffer's own target, keyed as its
 * surface is. God of War: Ascension's depth-of-field pass reads the display it
 * is composing; looked up by the bare offset it found nothing (offset 0 is no
 * key at all) and read the previous final frame instead. */
static u32 tex_rt_key(u32 raw, u32 ea)
{
    extern int cellGcmOffsetIsDisplay(u32 offset);
    if (display_rt_on() && ea >= 0xC0000000u && ea < 0xD0000000u && cellGcmOffsetIsDisplay(raw))
        return DISP_RT_TAG | raw;
    return raw;
}

/* Depth surface, one per guest zeta offset. The pass chain keeps several
 * (God of War HD: the scene's 2x-AA depth at 0x17F0000 and the resolved depth
 * at 0x2240000) and reads one back as a texture, so they cannot share the one
 * depth buffer that is reset whenever a pass changes targets. */
typedef struct {
    ID3D12Resource*       res;         /* R24G8_TYPELESS: a D24S8 DSV and two SRVs */
    u32                   off, w, h;
    u32                   pw, ph;      /* as OffRT's */
    float                 sx, sy;
    D3D12_RESOURCE_STATES st;
    int                   cleared;     /* initialised to 1.0/0 on first use */
    u32                   gen;         /* bumped by every draw or clear that writes it */
    u32                   conv_gen;    /* gen its colour copy was made from */
    u64                   last_frame;  /* frame_count of the last pass that bound it */
    u32                   grow_w, grow_h;   /* as OffRT's */
} OffDS;

/* Internal resolution (graphics.render_scale, D3D12_RENDER_SCALE): a number from 1 to 4, or native,
 * the view's size. Targets the size of the display are drawn that much larger; the smaller ones
 * (bloom, blur, SSAO) and shadow maps stay as the title made them. A target's w and h stay the
 * guest's, and what goes by the guest's size goes on using them: which surface is which, texture
 * coordinates, the guest's viewport and scissor. Only what talks to D3D12 in pixels takes pw, ph
 * or sx, sy, and a fragment program divides WPOS back to guest pixels (fp_alpha.w). The guest's
 * own view stays at its size: a write-back to main memory is point-sampled down and occlusion
 * counts are divided. At 1 every path and every program is what it was. */
static float s_rs = 1.0f;
/* display.ultrawide: the picture as much wider than the title's 16:9 as the screen is. Targets the size of the display are
 * drawn that much wider while the guest goes on seeing its own size, and the port widens the title's cameras to match. Its
 * 2D comes through those cameras too, and its movies take the aspect from the same setting, so they keep their shape in
 * the 16:9 in the middle by themselves. A frame with no 3D scene has black bars beside that (present_to_backbuffer). */
static float s_wide = 1.0f;
static int s_wide_3d;                   /* recording: this frame has drawn through the title's camera */
static int s_wide_scene;                /* replay: the same */
#define RS_ON (s_rs != 1.0f || s_wide != 1.0f)
/* A target a batch has bound or sampled grows only when the next batch first takes it. The prepass
 * runs in parts as record-ahead goes, so a later part can ask for more of a target the draws of an
 * earlier one already have: made anew there, those draws sample the old one through the views they
 * copied while the barriers move the new one, a depth surface read in DEPTH_WRITE. God of War:
 * Ascension's shadow atlas grows like that as the level starts, 48x66 to 1280x1408 over ten frames,
 * three times within one batch, and the GPU hung there in half the runs. Until then the batch draws
 * into what the target has. RT_GROW_NOW=1: made anew at once, as before. */
static int grow_later(void)
{
    static int v = -1;
    if (v < 0) v = getenv("RT_GROW_NOW") ? 0 : 1;
    return v;
}
/* Room to spare: God of War: Ascension's first level keeps six in use (the
 * scene's, a 1408x1408 one, the half-size passes'). */
#define MAX_OFF_DS   12
#define RT_SRV_BASE  5   /* SRV heap slots 5.. hold the offscreen-RT SRVs, one per slot */
#define DS_SRV_BASE  (RT_SRV_BASE + MAX_OFF_RTS)   /* then depth + stencil SRVs, 2 per OffDS */
/* Per-draw SRV windows: each VP draw gets VP_TEX_UNITS consecutive
 * descriptors (t0-t15) so multi-unit fragment programs see all their
 * textures. */
#define DRAW_SRV_BASE ((DS_SRV_BASE + 2 * MAX_OFF_DS + 31) & ~31)
#define SRV_HEAP_SIZE (DRAW_SRV_BASE + MAX_DRAWS * VP_TEX_UNITS)

#define TEX_CSUM_PARTS 8u   /* see tex_changed */

/* VP texture slot: a guest texture held for the VP draws, uploaded again when
 * its source bytes change (gcm/cube's plasma animates in guest memory). Each
 * draw that samples it writes its own SRV into its window at DRAW_SRV_BASE. */
typedef struct {
    ID3D12Resource* res;
    ID3D12Resource* up;
    u32 off, w, h, fmt; /* current contents (resource reused when dims match) */
    u32 csum[TEX_CSUM_PARTS];   /* sparse checksum of the source bytes last uploaded, taken before
                                 * they were read */
    u32 csum_bytes;     /* the bytes it covers: every level, face and slice the upload read */
    u8  csum_next;      /* the part to compare next */
    u8  again;          /* uploaded again at its next bind, whatever the checksum: its bytes
                         * changed while they were read, or F9 dropped the cache */
    int cube;           /* its shape (tex_shape_*): 0 a 2D image, 1 a 6-face cube
                         * sampled as TextureCube, else a volume (Texture3D)
                         * of the depth above bit 8 */
    u32 key;            /* the ORIGINAL bound offset -- the cache lookup key.
                         * off is the RESOLVED source after TEX_OFF_BIAS/TEX_REMAP
                         * shift it, so keying on off never matches the raw offset
                         * a later draw arrives with, and every draw re-uploads. */
    int used;           /* referenced this frame */
    u32 last_frame;     /* frame_count when last referenced: the slot least
                         * recently used is the one given to a new texture */
    u32 bytes;          /* res's size, counted against vp_tex_budget() */
    int next;           /* the next slot + 1 in this one's lookup chain, 0 = end */
    int linked;         /* in the lookup table */
    u32 levels;         /* mip levels of res */
    int heap;           /* the texture heap res is placed in + 1, 0 = its own allocation */
    u64 hoff, hsize;    /* its block in that heap */
    /* Views of res made in the view cache (srvc_tex), up to SRVC_VARIANTS
     * format/swizzle/cube combinations; cleared when res goes (vp_tex_drop). */
    u32 srv_fmt[4], srv_map[4];
    u8 srv_cube[4], srv_n, srv_next;
} VPTexSlot;
/* Textures held at once. Four was enough for a UI/atlas-style frame but not for
 * a scene: every draw past the fourth got slot -1 from vp_upload_tex_slot and
 * drew UNTEXTURED, which reads as a correctly-lit but solid black object. 192
 * was not enough for God of War: Ascension, whose levels bind more distinct
 * textures than that in one frame: each frame evicted 20-50 that the next one
 * needed again (25-55 ms of uploads on the frame clock thread) and refused
 * 400-2,200 binds. The cache is bounded by its size in bytes now; the slots
 * only have to outnumber what that holds. Found through a hash table. */
#define VP_TEX_SLOTS 4096
#define VP_TEX_BUCKET_BITS 13
static u32 s_vram_mb;   /* the adapter's dedicated VRAM; 0 = not known */
/* Guest texture uploads are staged in one persistently mapped upload buffer,
 * a part per frame in flight, reset when that frame's command allocator is
 * (the GPU has finished its copies by then). A committed upload buffer per
 * texture cost about two thirds of a texture burst in God of War HD: 50 new
 * textures in one report interval spent 41 ms creating their upload buffers.
 * A texture that does not fit in what is left of the part uses its own buffer,
 * released once the batch has run (vp_tex_release_staging). */
#define TEX_RING_PART (32u << 20)

/* Decompiled-VS cache: one entry per distinct vertex-program ucode seen at
 * draw time (hashed). Apps switch VPs between draws (gcm/cube: its MVP cube VP
 * vs dbgfont-gcm's text VP); compiling only the first left later draws
 * transformed by the wrong program (text offscreen). */
struct ShaderJob;
typedef struct {
    u32 hash;               /* FNV-1a of the ucode */
    u64 last_use;           /* frame_count of the last draw recorded with it */
    ID3DBlob* vs;
    int uses_c03;
    int uses_cam;           /* reads the title's camera (rsx_d3d12_backend_camera_register) */
    int uses_prev;          /* and last frame's: it draws what moves */
    char* hlsl;             /* the decompiled source, for vp_vs_raw */
    ID3DBlob* vs_raw;       /* raw-fetch variant, compiled on first use */
    int raw_failed;
    struct ShaderJob* job;     /* vs still building (D3D12_ASYNC_SHADERS) */
    struct ShaderJob* raw_job; /* vs_raw still building */
} VPVSEntry;
#define VP_VS_CACHE 1024 /* wave uses 5+ distinct VPs; at 4 the cache
                          * thrashed every frame and eviction shifted slots
                          * under recorded vs_idx values -- the display mesh
                          * drew with the WRONG vertex program (sub-rect +
                          * edge slivers instead of fullscreen). A fight in
                          * SoulCalibur II HD uses more than 16, the first
                          * minute of God of War: Ascension's first level
                          * 106. A full cache replaces the least recently
                          * used entry where it is (vp_get_vs): no other
                          * slot moves. */

/* Not a SHADER_CONTROL bit: the draw's colour outputs are encoded to sRGB
 * (SET_SHADER_PACKER on an integer colour target). It rides in the recorded
 * control word so the pipeline key separates the two compiles. */
#define FP_CTRL_SRGB_OUT 0x00800000u

/* Compiled guest-FP pipeline cache: decompiled VS (by cache slot) + decompiled
 * PS (fragment ucode at fp_addr). */
typedef struct {
    u32 fp_addr;
    int vs_idx;             /* VPVSEntry slot this PSO's VS came from */
    u32 vs_hash;            /* validates the slot hasn't been evicted */
    u32 gen;
    u32 blend;              /* packed guest blend key (enable+factors+eq, PSO key) */
    int nrt;                /* bound colour target count (PSO key)       */
    u32 rtfmt;              /* DXGI format of the colour targets (PSO key) */
    u32 ctrl;               /* SHADER_CONTROL export bits (PSO key)       */
    u32 ucode_hash;         /* FNV-1a of the FP ucode: apps re-patch inline
                             * constants per frame (wave's stamp position/
                             * amplitude) -- address-only keying served the
                             * stale compile forever. */
    u32 cmask;              /* colour write mask (PSO key) */
    u32 cull;               /* packed face culling (PSO key) */
    u32 cube_mask;          /* which units are cube textures (PSO key): the HLSL
                             * declares those samplers as TextureCube, so a cube
                             * and a 2D variant of the same program are different
                             * pipelines and must not share a cache entry. */
    int raw;                /* raw-fetch VS, no input layout (PSO key) */
    u64 zs;                 /* depth/stencil state, rsx_zs_key (PSO key) */
    u64 poly;               /* polygon offset, rsx_poly_key (PSO key) */
    u32 ext;                /* 1 depth bounds test, 2 depth clip (PSO key) */
    u32 aa;                 /* the surface's sample grid, x | y << 4, 0 for 1x1 (PSO key) */
    u32 mrt;                /* MRT 1-3 masks and blend enables, rsx_pso_mrt (PSO key) */
    u64 gamma;              /* texture gamma lanes, 4 bits per unit (PSO key) */
    u64 unnorm;             /* texel-coordinate units and their sizes (PSO key) */
    u64 shadow;             /* depth-compared units, dr_shadow_descs (PSO key) */
    u64 expand;             /* sign-expanding units, a hash of their lane masks (PSO key) */
    u32 wpos;               /* rec_wpos_win of the draw, literal programs (PSO key) */
    u32 atest;              /* alpha test compiled in: CELL_GCM function, and for a
                             * literal program the reference above bit 16 (PSO key) */
    u64 last_use;           /* frame_count of the last draw that asked for it */
    ID3D12PipelineState* pso;
    struct ShaderJob* job;  /* pso still building (D3D12_ASYNC_SHADERS) */
} VPFPEntry;
/* Guest-FP PSO cache. 16 entries thrashed badly: this title needs far more
 * distinct (fp, vs, blend, rt, cmask, ucode-hash) combinations than that in a
 * single frame, so the FIFO evicted entries that were needed again immediately
 * and ~17 shaders were recompiled EVERY frame -- about half the frame's CPU
 * time. Entries are small (a PSO pointer plus key fields). */
/* Room for a level's working set once every program builds (God of War:
 * Ascension's level selects several hundred), looked up through a hash of the
 * ucode rather than a scan. */
#define VP_FP_CACHE 8192
#define VP_FP_HASH  4096

/* ---------------------------------------------------------------------------
 * Internal state
 * -----------------------------------------------------------------------*/

typedef struct {
    /* Window */
    HWND hwnd;
    HWND view;   /* the child the swap chain presents to (see create_window) */
    u32  width;
    u32  height;
    u32  pw, ph;        /* the display targets' size: width, height times the render scale */
    int  window_closed;

    /* D3D12 core */
    ID3D12Device*              device;
    ID3D12CommandQueue*        cmd_queue;
    IDXGISwapChain3*           swap_chain;
    int                        tearing;     /* D3D12_VSYNC=0 and the swap chain allows tearing */
    ID3D12DescriptorHeap*      rtv_heap;
    u32                        rtv_descriptor_size;
    /* The title's display targets: what a frame drawn to the display buffer
     * lands in. The swap chain's own buffers only receive them, at present
     * (present_to_backbuffer), so the picture can be scaled or filtered on
     * the way, and every read of the display (screen copies, dumps, CPU
     * readbacks) reads the title's picture, not the window's. A display
     * target rests in COMMON, which is the same state as PRESENT. */
    ID3D12Resource*            render_targets[FRAME_COUNT];
    ID3D12Resource*            backbuffers[FRAME_COUNT];
    u32                        bb_w, bb_h;         /* the swap chain's size: the view's */
    ID3D12DescriptorHeap*      bb_rtv_heap;        /* RTVs of backbuffers[] */
    /* The pass that scales a display target into the swap chain's buffer */
    ID3D12RootSignature*       pp_root;
    ID3D12PipelineState*       pp_pso;             /* bilinear and sharp bilinear */
    ID3D12PipelineState*       pp_pso_easu;        /* FSR 1 upscale */
    ID3D12PipelineState*       pp_pso_rcas;        /* FSR 1 sharpening */
    ID3D12Resource*            pp_tmp;             /* EASU's output when RCAS follows, the back buffers' size */
    ID3D12DescriptorHeap*      pp_srv_heap;        /* SRVs: render_targets[], then pp_tmp; shader-visible */
    int                        pp_mode;            /* graphics.scaling: 0 bilinear, 1 sharp, 2 fsr1 */
    float                      pp_stops;           /* graphics.sharpening for RCAS, in AMD's stops; < 0 off */
    /* The overlay: text over the scaled picture (ov_draw) */
    ID3D12PipelineState*       ov_pso;
    ID3D12Resource*            ov_tex;             /* OV_W x OV_H, premultiplied RGBA */
    ID3D12Resource*            ov_up[FRAME_COUNT]; /* its uploads, one per frame in flight, mapped */
    unsigned char*             ov_up_ptr[FRAME_COUNT];
    int                        ov_tex_ready;       /* ov_tex has left COMMON for PIXEL_SHADER_RESOURCE */
    /* SMAA 1x on the display target before it is scaled (graphics.antialiasing) */
    int                        aa_on;
    ID3D12RootSignature*       aa_root;            /* an SRV table of three, 16 constants */
    ID3D12PipelineState*       aa_pso[3];          /* edges, blending weights, neighbourhood blending */
    ID3D12Resource*            aa_edges;           /* R8G8, the display's size */
    ID3D12Resource*            aa_blend;           /* RGBA8 */
    ID3D12Resource*            aa_out;             /* RGBA8: what the scaling pass reads */
    ID3D12Resource*            aa_area;            /* the lookup textures, uploaded on first use */
    ID3D12Resource*            aa_search;
    ID3D12Resource*            aa_up;
    ID3D12DescriptorHeap*      aa_rtv_heap;        /* edges, blend, out */
    int                        aa_luts_ready;
    /* F12 with display.screenshot = output: the swap chain's buffer read back */
    ID3D12Resource*            shot_buf;
    u32                        shot_w, shot_h, shot_pitch;
    ID3D12CommandAllocator*    cmd_allocators[FRAME_COUNT];
    ID3D12GraphicsCommandList* cmd_list;

    /* Synchronization */
    ID3D12Fence* fence;
    HANDLE       fence_event;
    u64          fence_values[FRAME_COUNT];
    u32          frame_index;

    /* Pipeline */
    ID3D12RootSignature*  root_signature;
    ID3D12PipelineState*  pipeline_state;         /* triangle class — default */
    ID3D12PipelineState*  pipeline_state_lines;   /* line class */
    ID3D12PipelineState*  pipeline_state_points;  /* point class */

    /* Depth/stencil */
    ID3D12DescriptorHeap* dsv_heap;
    ID3D12Resource*       depth_buffer;

    /* Dynamic vertex buffer (upload heap) */
    ID3D12Resource*       vertex_buffer;
    D3D12_VERTEX_BUFFER_VIEW vb_view;
    void*                 vb_mapped;      /* persistently mapped */
    u32                   vb_offset;      /* current write position */

    int                   pipeline_ready; /* 1 if root sig + PSO created */

    /* Debug: copy the presented backbuffer to disk as BMP for the first N
     * frames (enabled by env CELLMARK_DUMP). Lets us verify what actually
     * rendered without racing the window/process lifetime. */
    ID3D12Resource*       readback_buf;
    u32                   readback_pitch;
    int                   dump_frames_left;
    int                   dump_skip_left;   /* CELLMARK_DUMP_SKIP: presents to ignore first */

    /* Textured pipeline (dbgfont / 2D atlas quads). The font atlas is an 8-bit
     * coverage texture uploaded as R8_UNORM and sampled in the pixel shader. */
    ID3D12PipelineState*  pipeline_state_tex;   /* triangle + texture + blend */
    ID3D12DescriptorHeap* srv_heap;             /* shader-visible, 1 SRV slot  */
    ID3D12Resource*       tex_resource;         /* current atlas texture       */
    ID3D12Resource*       tex_upload;           /* staging buffer for uploads  */
    u32                   tex_w, tex_h;         /* dims of tex_resource         */
    int                   tex_ready;            /* 1 once an atlas is uploaded  */
    u32                   tex_src_offset;       /* guest RSX offset of atlas    */
    int                   tex_bound;            /* a texture is bound for draws */
    int                   tex_dirty;            /* re-upload needed this frame  */

    /* Real RSX vertex-program path: the captured VP is decompiled to HLSL and
     * used as the vertex shader, fed the raw float4 attrib0 + the vp_c[]
     * constant bank. This produces exact position + texcoord (vs. the frac
     * approximation). Used for the 2D/dbgfont quad draws. */
    ID3D12RootSignature*  vp_root_sig;          /* CBV(b0) + SRV table + sampler */
    ID3D12PipelineState*  pipeline_state_vp;    /* decompiled VS + atlas PS      */
    ID3D12PipelineState*  pipeline_state_vp_color; /* decompiled VS + colour PS (untextured 3D) */
    ID3D12Resource*       vp_vb;                /* raw float4 attrib0, per-frame */
    void*                 vp_vb_mapped;
    u32                   vp_vb_offset;
    ID3D12Resource*       vp_ib;                /* u32 indices of indexed VP draws, per-frame */
    void*                 vp_ib_mapped;
    u32                   vp_ib_offset;         /* in indices */
    ID3D12Resource*       vp_cb;                /* per-draw VP constant slots    */
    void*                 vp_cb_mapped;
    ID3D12Resource*       vp_fpcb;              /* per-draw FP texscale (b1)     */
    void*                 vp_fpcb_mapped;
    int                   vp_ready;             /* VS+PSO compiled ok            */
    u32                   vp_compiled_bytes;    /* ucode size when last compiled */
    ID3DBlob*             vp_vs_blob;           /* kept for guest-FP PSO builds  */
    u32                   vp_gen;               /* bumped per VP recompile       */
    int                   vp_uses_c03;          /* VS references vp_c[0..3]      */
    VPTexSlot             vp_tex[VP_TEX_SLOTS]; /* the VP texture cache          */
    ID3D12Resource*       tex_ring;             /* texture upload ring, FRAME_COUNT parts */
    u8*                   tex_ring_ptr;         /* persistently mapped           */
    u64                   tex_ring_used[FRAME_COUNT];
    VPVSEntry             vp_vs[VP_VS_CACHE];   /* per-draw decompiled VS cache  */
    int                   vp_vs_n;
    VPFPEntry             vp_fp[VP_FP_CACHE];   /* guest-FP PSO cache            */
    int                   vp_fp_n;
    u32                   srv_inc;              /* CBV_SRV_UAV descriptor size   */
    /* VP path: latest texture bound per unit (t0-t15). */
    struct { u32 off, raw, w, h, fmt, ctrl1, mips; int cube; int set;
             u32 addr, filter, ctrl0, border, depth; } cur_texs[VP_TEX_UNITS];
    /* Guest sampler states, one shader-visible descriptor each (samp_key[i]
     * describes descriptor i; 0 and 1 are the defaults for units with none). */
    ID3D12DescriptorHeap* samp_heap;
    u32                   samp_inc, samp_n;
    u32                   samp_key[2048][4];

    /* Render-to-texture: offscreen RT pool + their RTV heap. */
    OffRT                 off_rt[MAX_OFF_RTS];
    OffDS                 off_ds[MAX_OFF_DS];
    ID3D12DescriptorHeap* ds_heap;              /* one DSV per OffDS              */
    ID3D12RootSignature*  dsconv_rs;            /* depth -> guest colour bytes    */
    ID3D12PipelineState*  dsconv_pso;
    /* DRAW_ARRAYS batch merging (see d3d12_draw_arrays): the vertex index one
     * past the last batch, and whether the current BEGIN/END may still be
     * extended. */
    u32                   merge_first_end;
    int                   merge_prev_draw;
    ID3D12DescriptorHeap* rt_rtv_heap;          /* MAX_OFF_RTS RTVs (CPU only)   */

    /* Frame-parity double buffering for the per-draw upload streams (vp_vb
     * vertices, vp_cb constants, vp_fpcb texscales): records for frame N+1
     * are written while frame N's GPU work may still be reading -- writes go
     * to the other half. Toggled at the end of render_frame. */
    int                   vp_parity;

    /* Per-frame draw recording */
    int                   frame_recording; /* 1 if cmd list is open for recording */
    u32                   draw_count;      /* draws this frame */
    D3D12DrawRecord       draws[MAX_DRAWS];

    /* Pointer to current RSX state (set before draw calls) */
    const rsx_state*      current_rsx_state;

    /* Current frame state */
    float clear_color[4];  /* RGBA float */

    /* Stats */
    u64 frame_count;
    u64 last_fps_time;
    u32 fps;

    int initialized;
} D3D12State;

static D3D12State s_d3d;
char g_rsx_title_base[128] = "ps3recomp";
/* graphics.render_thread (RSX_RENDER_THREAD): see "The render thread". 0 off;
 * 1 the walk goes on from render_frame's reset; 2 (on, the default) from the
 * end of the frame, its present included. */
static int s_rt_mode;
static void rt_start(void);
static void rt_stop(void);
static void rt_publish(void);
static void rt_released(void);
static void rt_render(int present, int full);
static void rt_excl_begin(void);
static void rt_excl_end(void);
static u32 s_dbg_last_draws = 0;
static u64 s_req_verts = 0, s_req_draws = 0, s_drop_draws = 0;
/* Raw bind offset of the duck's texture, identified by CONTENT (duck.tga is
 * overwhelmingly yellow, avg 243,191,23). VRAM offsets move between runs, so a
 * hard-coded offset in a filter silently matches nothing and the run looks like
 * "renders nothing" -- which cost real time. DRAW_KEEP_TEX=duck resolves here. */
/* Copy of the last completed frame, bound wherever a draw samples a
 * DISPLAY-SIZED texture. On RSX this title renders its reflection into a corner
 * of the render surface and then samples that surface as a texture; our backend
 * renders into a D3D backbuffer, so the guest memory behind that sampler is
 * never written and it reads empty -- which is why the water had no reflection.
 * Feeding it the previous frame costs one frame of latency, which for a water
 * reflection is not visible. */
static ID3D12Resource* s_screen_copy = NULL;
/* Sub-viewport render-to-texture. This title renders its reflection/refraction
 * pre-pass into a REGION of the same surface (vp 0,208 512x512 and 0,208
 * 1024x512, cmask=F) and then samples a texture of exactly that size. On
 * hardware the pass lands in the buffer that texture points at; our backend
 * renders into D3D resources, so that guest buffer is never written and every
 * fluid sampler reads zero. Capture each region into a matching resource and
 * bind it for samplers whose guest source is empty and whose size matches.
 *
 * SUBVP_RTT=1 to enable. OFF by default because it cannot be validated on this
 * title yet: the fluid draws that would consume these captures never rasterize
 * (see below), so binding them changes nothing observable. The capture and
 * binding themselves are confirmed working -- SUBVP_DBG shows the 512x512 and
 * 256x256 regions captured and handed to fp 0x5DB81/0x5EA81/0x5EF81/0x5E381. */
#define SUBVP_SLOTS 4
static struct { ID3D12Resource* res; u32 x, y, w, h; } s_subvp[SUBVP_SLOTS];
static int s_subvp_n = 0;

/* Once, when a call finds the device gone: why, and with D3D12_DRED=1 what the GPU was
 * doing and the address it faulted on. */
static void device_lost_report(const char* where)
{
    static volatile LONG said = 0;
    if (InterlockedExchange(&said, 1) || !s_d3d.device) return;
    fprintf(stderr, "[D3D12] device lost, seen %s: reason 0x%08lX, frame %llu%c", where,
            (unsigned long)s_d3d.device->lpVtbl->GetDeviceRemovedReason(s_d3d.device),
            (unsigned long long)s_d3d.frame_count, 10);
    ID3D12DeviceRemovedExtendedData* dr = NULL;
    if (FAILED(s_d3d.device->lpVtbl->QueryInterface(s_d3d.device, &IID_ID3D12DeviceRemovedExtendedData, (void**)&dr)) || !dr) {
        fflush(stderr);
        return;
    }
    D3D12_DRED_AUTO_BREADCRUMBS_OUTPUT bc;
    memset(&bc, 0, sizeof bc);
    if (SUCCEEDED(dr->lpVtbl->GetAutoBreadcrumbsOutput(dr, &bc))) {
        for (const D3D12_AUTO_BREADCRUMB_NODE* n = bc.pHeadAutoBreadcrumbNode; n; n = n->pNext) {
            const UINT32 last = n->pLastBreadcrumbValue ? *n->pLastBreadcrumbValue : 0;
            if (last >= n->BreadcrumbCount) continue;   /* the GPU got through this list */
            fprintf(stderr, "[dred] list %p '%s' queue '%s': %u of %u ops done, around the stop:", (void*)n->pCommandList,
                    n->pCommandListDebugNameA ? n->pCommandListDebugNameA : "", n->pCommandQueueDebugNameA ? n->pCommandQueueDebugNameA : "",
                    last, n->BreadcrumbCount);
            const UINT32 from = last > 12 ? last - 12 : 0, to = last + 12 < n->BreadcrumbCount ? last + 12 : n->BreadcrumbCount;
            for (UINT32 i = from; i < to; i++)
                fprintf(stderr, "%s%u", i == last ? " >" : " ", (unsigned)n->pCommandHistory[i]);
            fprintf(stderr, "%c", 10);
        }
    }
    D3D12_DRED_PAGE_FAULT_OUTPUT pf;
    memset(&pf, 0, sizeof pf);
    if (SUCCEEDED(dr->lpVtbl->GetPageFaultAllocationOutput(dr, &pf))) {
        fprintf(stderr, "[dred] page fault at GPU VA 0x%llX%c", (unsigned long long)pf.PageFaultVA, 10);
        int k = 0;
        for (const D3D12_DRED_ALLOCATION_NODE* a = pf.pHeadExistingAllocationNode; a && k < 12; a = a->pNext, k++)
            fprintf(stderr, "[dred]   there now: type %d '%s'%c", (int)a->AllocationType, a->ObjectNameA ? a->ObjectNameA : "", 10);
        k = 0;
        for (const D3D12_DRED_ALLOCATION_NODE* a = pf.pHeadRecentFreedAllocationNode; a && k < 12; a = a->pNext, k++)
            fprintf(stderr, "[dred]   freed there: type %d '%s'%c", (int)a->AllocationType, a->ObjectNameA ? a->ObjectNameA : "", 10);
    }
    dr->lpVtbl->Release(dr);
    fflush(stderr);
}

static void subvp_note(u32 x, u32 y, u32 w, u32 h)
{
    if (!w || !h || w > s_d3d.width || h > s_d3d.height) return;
    if (w == s_d3d.width && h == s_d3d.height) return;      /* full-screen pass */
    if (RS_ON) return;                                       /* its copies are in guest pixels */
    for (int i = 0; i < s_subvp_n; i++)
        if (s_subvp[i].w == w && s_subvp[i].h == h) { s_subvp[i].x = x; s_subvp[i].y = y; return; }
    if (s_subvp_n >= SUBVP_SLOTS) return;
    s_subvp[s_subvp_n].x = x; s_subvp[s_subvp_n].y = y;
    s_subvp[s_subvp_n].w = w; s_subvp[s_subvp_n].h = h;
    s_subvp[s_subvp_n].res = NULL;
    s_subvp_n++;
}

/* Copy each noted region out of the backbuffer. Called at the same moment as
 * screen_copy_capture -- after the reduced-viewport passes and before the
 * full-screen scene overwrites them. */
static void subvp_capture(u32 fi)
{
    static int en = -1;
    if (en < 0) { const char* e = getenv("SUBVP_RTT"); en = e ? atoi(e) : 0; }
    if (!en || !s_d3d.device || !s_subvp_n) return;
    for (int i = 0; i < s_subvp_n; i++) {
        if (!s_subvp[i].res) {
            D3D12_HEAP_PROPERTIES hp = {0}; hp.Type = D3D12_HEAP_TYPE_DEFAULT;
            D3D12_RESOURCE_DESC td = {0};
            td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
            td.Width = s_subvp[i].w; td.Height = s_subvp[i].h;
            td.DepthOrArraySize = 1; td.MipLevels = 1;
            td.Format = DXGI_FORMAT_R8G8B8A8_UNORM; td.SampleDesc.Count = 1;
            td.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
            if (FAILED(s_d3d.device->lpVtbl->CreateCommittedResource(
                    s_d3d.device, &hp, D3D12_HEAP_FLAG_NONE, &td,
                    D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, NULL,
                    &IID_ID3D12Resource, (void**)&s_subvp[i].res)))
                { s_subvp[i].res = NULL; continue; }
        }
        D3D12_RESOURCE_BARRIER bb[2] = {0};
        bb[0].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        bb[0].Transition.pResource   = s_d3d.render_targets[fi];
        bb[0].Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
        bb[0].Transition.StateAfter  = D3D12_RESOURCE_STATE_COPY_SOURCE;
        bb[0].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        bb[1] = bb[0];
        bb[1].Transition.pResource   = s_subvp[i].res;
        bb[1].Transition.StateBefore = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        bb[1].Transition.StateAfter  = D3D12_RESOURCE_STATE_COPY_DEST;
        s_d3d.cmd_list->lpVtbl->ResourceBarrier(s_d3d.cmd_list, 2, bb);

        D3D12_TEXTURE_COPY_LOCATION dstl = {0}, srcl = {0};
        dstl.pResource = s_subvp[i].res;
        dstl.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX; dstl.SubresourceIndex = 0;
        srcl.pResource = s_d3d.render_targets[fi];
        srcl.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX; srcl.SubresourceIndex = 0;
        D3D12_BOX box; box.left = s_subvp[i].x; box.top = s_subvp[i].y;
        box.front = 0; box.right = s_subvp[i].x + s_subvp[i].w;
        box.bottom = s_subvp[i].y + s_subvp[i].h; box.back = 1;
        if (box.right > s_d3d.width)  box.right  = s_d3d.width;
        if (box.bottom > s_d3d.height) box.bottom = s_d3d.height;
        s_d3d.cmd_list->lpVtbl->CopyTextureRegion(s_d3d.cmd_list, &dstl, 0, 0, 0, &srcl, &box);

        bb[0].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
        bb[0].Transition.StateAfter  = D3D12_RESOURCE_STATE_RENDER_TARGET;
        bb[1].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
        bb[1].Transition.StateAfter  = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        s_d3d.cmd_list->lpVtbl->ResourceBarrier(s_d3d.cmd_list, 2, bb);
        { static int _n = 0; if (getenv("SUBVP_DBG") && PS3_LOG_FIRST(_n, 8))
            fprintf(stderr, "[SUBVP] captured %ux%u from (%u,%u)%c",
                    s_subvp[i].w, s_subvp[i].h, s_subvp[i].x, s_subvp[i].y, 10); }
    }
}
static void screen_copy_capture(u32 fi);   /* fwd */
static int  s_sc_dump_pending = 0;
static u32 s_duck_raw = 0;   /* as BOUND (what draw records carry) */
static u32 s_duck_off = 0;   /* as RESOLVED (what the uploader sees)   */
/* PERF=1: where a frame's CPU time actually goes. Guessing at this is how you
 * spend an afternoon optimising the wrong loop. */
static double s_perf_tex = 0.0, s_perf_frame = 0.0, s_perf_vtx = 0.0, s_perf_rf = 0.0;
static double s_perf_gpu = 0.0, s_perf_pre = 0.0, s_perf_srv = 0.0, s_perf_pso = 0.0;
static int s_perf_pso_calls = 0, s_perf_pso_miss = 0, s_perf_pso_hashbytes = 0;
static u64    s_perf_nverts = 0;
static u64    s_perf_texbytes = 0;
static int    s_perf_ntex = 0;
static double perf_now(void)
{
    LARGE_INTEGER f, c;
    QueryPerformanceFrequency(&f); QueryPerformanceCounter(&c);
    return (double)c.QuadPart / (double)f.QuadPart;
}
/* RSX_CENSUS's hitch log: what the backend spent between two presents --
 * textures uploaded new or again and their time, waits for shader builds,
 * draws skipped for one, builds queued, waits for the GPU. */
static double s_hl_tex_s, s_hl_sjwait_s, s_hl_gpuwait_s;
/* What texture uploads spent creating resources, releasing evicted ones and
 * decoding into the upload buffer, summed since start: TEX_STATS and the hitch
 * log report what each grew by. */
static double s_tex_create_s, s_tex_evict_s, s_tex_decode_s;
/* PERF: vertex programs replaced in the VS cache, pipelines dropped from the
 * fragment pipeline cache, since the last report. */
static unsigned s_perf_vs_replaced, s_perf_fp_dropped;
static unsigned s_hl_tex_n, s_hl_skipped, s_hl_jobs;
/* More of what a long frame did: fragment pipelines made on this thread on a
 * miss (decompile, edits, queueing), upload buffers made because the texture
 * ring was full, offscreen targets made, and every wait for the GPU. */
static unsigned s_hl_miss_n, s_hl_upbuf_n, s_hl_rt_n, s_hl_waits_n;
/* The frame before on the GPU (RSX_CENSUS timestamps): its copies (to the end
 * of the prepass), its draws, the rest, and all of it, in ms. */
static double s_hl_gpu_ms[4];
static double s_hl_miss_s, s_hl_upbuf_s, s_hl_rt_s, s_hl_waits_s;
static IDXGIAdapter3* s_adapter3;   /* local memory use and budget, for the hitch log */
static char s_ovs_gpu_name[64];     /* the adapter's name, for the stats panel */
static int perf_on(void)
{
    static int v = -1;
    if (v < 0) { const char* e = getenv("PERF"); v = e ? atoi(e) : 0; }
    return v;
}

/* RENDERDOC_DLL=<path to renderdoc.dll>: load RenderDoc's capture layer before
 * the device exists, and after each CELLMARK_DUMP capture the next frame that
 * carries at least CELLMARK_DUMP_MINDRAWS draws, up to RENDERDOC_MAX captures
 * (default 2), as RENDERDOC_DIR/gcm_*.rdc (default directory "rdoc"). The
 * in-application API is a table of function pointers (renderdoc_app.h,
 * RENDERDOC_API_1_6_0); only the slots used here are named. */
typedef int (__cdecl* rdoc_get_api_fn)(int version, void** out);
static void** s_rdoc;
static int    s_rdoc_pending, s_rdoc_active, s_rdoc_done, s_rdoc_max;
enum { RDOC_SET_CAPTURE_PATH = 11, RDOC_START_FRAME = 19, RDOC_END_FRAME = 21 };

static void rdoc_load(void)
{
    const char* dll = getenv("RENDERDOC_DLL");
    if (!dll || !*dll) return;
    HMODULE m = LoadLibraryA(dll);
    rdoc_get_api_fn get = m ? (rdoc_get_api_fn)(void*)GetProcAddress(m, "RENDERDOC_GetAPI") : NULL;
    void* api = NULL;
    if (!get || !get(10600, &api) || !api) {
        printf("[rdoc] cannot load RenderDoc from %s%c", dll, 10);
        return;
    }
    s_rdoc = (void**)api;
    const char* dir = getenv("RENDERDOC_DIR");
    char tmpl[512];
    snprintf(tmpl, sizeof tmpl, "%s/gcm", dir && *dir ? dir : "rdoc");
    ((void (__cdecl*)(const char*))s_rdoc[RDOC_SET_CAPTURE_PATH])(tmpl);
    const char* mx = getenv("RENDERDOC_MAX");
    s_rdoc_max = mx ? atoi(mx) : 2;
    printf("[rdoc] RenderDoc loaded; up to %d captures as %s_*.rdc%c", s_rdoc_max, tmpl, 10);
}

static void rdoc_frame_begin(u32 draws)
{
    static int mind = -1;
    if (mind < 0) { const char* e = getenv("CELLMARK_DUMP_MINDRAWS"); mind = e ? atoi(e) : 0; }
    if (!s_rdoc || !s_rdoc_pending || draws < (u32)mind) return;
    ((void (__cdecl*)(void*, void*))s_rdoc[RDOC_START_FRAME])(NULL, NULL);
    s_rdoc_pending = 0;
    s_rdoc_active = 1;
}

static void rdoc_frame_end(void)
{
    if (!s_rdoc_active) return;
    const u32 ok = ((u32 (__cdecl*)(void*, void*))s_rdoc[RDOC_END_FRAME])(NULL, NULL);
    s_rdoc_active = 0;
    s_rdoc_done++;
    printf("[rdoc] capture %d %s%c", s_rdoc_done, ok ? "written" : "FAILED", 10);
}

/* While RenderDoc is loaded, name a D3D12 object after the guest data it holds
 * (texture offset and format, render-target offset, fragment program), so a
 * capture reads in guest terms. */
static void rdoc_name(ID3D12Object* o, const char* fmt, ...)
{
    if (!s_rdoc || !o) return;
    char s[160];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(s, sizeof s, fmt, ap);
    va_end(ap);
    WCHAR w[160];
    MultiByteToWideChar(CP_ACP, 0, s, -1, w, 160);
    o->lpVtbl->SetName(o, w);
}

static void vp_sampler_defaults(void);

static void rdoc_request(void)
{
    if (s_rdoc && !s_rdoc_pending && !s_rdoc_active && s_rdoc_done < s_rdoc_max) s_rdoc_pending = 1;
}

/* graphics.frame_generation (D3D12_FRAME_GENERATION): off, on, or passthrough, which
 * is the proxy swapchain and its threads with nothing generated, to measure them.
 * The swap chain goes behind the proxy before anything asks it for a buffer; the
 * context waits for the first frame, when the window's size is known. */
#define FG_OBJ_MAX   1024      /* draws of what moves in a frame */
#define FG_OBJ_PSOS  16        /* programs that draw them */
#define FG_OBJ_NONE  60000.0f  /* in the target, where nothing moves */
#define FG_OBJ_BIAS  (-64)     /* their depth against the scene's, from another program's arithmetic */
static int s_fg_mode;       /* 0 off, 1 passthrough, 2 on */
/* graphics.scaling = fsr3: FSR's upscaler on scene frames, FSR 1 on the rest. */
static struct {
    int want;                   /* asked for, and the DLLs are there */
    int ready;                  /* the context is up: frames are jittered */
    u32 frame;
    float jx, jy;               /* the frame's jitter, in pixels, */
    float cx, cy;               /* in clip space */
    ID3D12RootSignature* root;
    ID3D12PipelineState* pso;
} s_up;
#define FG_INPUTS() (s_fg_mode == 2 || s_up.want)
static int s_fg_ready;
static double s_fg_last;
/* What it takes from a frame, as the batch is recorded and replayed. */
static struct {
    int cam_reg;                /* the vertex constant the world-to-clip matrix starts at, or -1 */
    int prev_reg, prev_attr;    /* last frame's, and the attribute of a vertex's last position */
    int cam_ok;                 /* this frame has a 3D scene: a camera, and */
    float cam[16], z_scale, z_offset;
    u32 depth_off;              /* the depth surface it draws with */
    int marked;                 /* the HUD's clear was reached: see fg_marker */
    int copied;                 /* and the frame's depth and picture are in depth and hud_src */
    int depth_ok;               /* the frame's depth is in depth */
    int ready;                  /* the frame's inputs are all there */
    ID3D12Resource* depth;      /* the scene's depth */
    ID3D12Resource* hud_src;    /* the picture without its HUD, at the display target's size */
    ID3D12Resource* hud_out[2]; /* that at the window's size, a frame about */
    u32 hud_i;
    u32 obj[FG_OBJ_MAX];        /* this frame's records of what moves */
    u32 obj_n;
    int obj_drawn;
    ID3D12Resource* obj_rt;     /* their motion, R16G16_FLOAT */
    ID3D12DescriptorHeap* obj_rtv;
    ID3D12DescriptorHeap* obj_dsv;   /* depth, read only */
    ID3DBlob* obj_ps;
    struct { u32 hash; struct ShaderJob* job; ID3D12PipelineState* pso; int tried; } obj_pso[FG_OBJ_PSOS];
} s_fg = { -1, -1, -1 };

/* The title's world-to-clip matrix: four vertex constants from reg, read by the
 * programs that draw its scene. Without it frames are shown as they are. */
static const char* s_fsr3_refused;

void rsx_d3d12_backend_fsr3_refuse(const char* why)
{
    s_fsr3_refused = why;
}

void rsx_d3d12_backend_camera_register(int reg, int prev_reg, int prev_attr)
{
    s_fg.cam_reg = reg;
    s_fg.prev_reg = prev_reg;
    s_fg.prev_attr = prev_attr;
}

static void fg_start(void)
{
    const char* m = ps3_settings_str("graphics", "frame_generation", "D3D12_FRAME_GENERATION", "off");
    const int mode = ps3_settings_ieq(m, "on") ? 2 : ps3_settings_ieq(m, "passthrough") ? 1 : 0;
    if (!ps3_settings_bool("graphics", "frame_generation_objects", "D3D12_FG_OBJECTS", 1)) s_fg.prev_reg = -1;
    if (rsx_d3d12_backend_wide_ratio() != 1.0f) s_fg.prev_reg = -1;   /* 21:9: it left noise on Kratos in close-ups; cause not found */
    if (!mode) return;
    if (s_fsr3_refused) {
        printf("[D3D12] frame generation: off, %s%c", s_fsr3_refused, 10);
        return;
    }
    if (!rsx_fsr3_available()) {
        printf("[D3D12] frame generation: no amd_fidelityfx_loader_dx12.dll beside the exe%c", 10);
        return;
    }
    void* sc = s_d3d.swap_chain;
    if (rsx_fsr3_wrap(&sc, s_d3d.cmd_queue)) s_fg_mode = mode;
    s_d3d.swap_chain = (IDXGISwapChain3*)sc;
    rsx_fsr3_fg_floor(atof(ps3_settings_str("graphics", "frame_generation_floor", "D3D12_FG_FLOOR", "25")));
}

int rsx_d3d12_backend_generating(void) { return s_fg_mode == 2; }

/* From the display paths, which have 59.94 where EnumDisplaySettings has 59. */
double rsx_d3d12_backend_refresh_hz(void)
{
    MONITORINFOEXW mi;
    memset(&mi, 0, sizeof mi);
    mi.cbSize = sizeof mi;
    if (!GetMonitorInfoW(MonitorFromWindow(s_d3d.hwnd, MONITOR_DEFAULTTOPRIMARY), (MONITORINFO*)&mi)) return 0.0;
    double hz = 0.0;
    UINT32 np = 0, nm = 0;
    if (GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &np, &nm) == ERROR_SUCCESS) {
        DISPLAYCONFIG_PATH_INFO* paths = (DISPLAYCONFIG_PATH_INFO*)calloc(np + 1, sizeof *paths);
        DISPLAYCONFIG_MODE_INFO* modes = (DISPLAYCONFIG_MODE_INFO*)calloc(nm + 1, sizeof *modes);
        if (paths && modes && QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &np, paths, &nm, modes, NULL) == ERROR_SUCCESS)
            for (UINT32 i = 0; i < np && hz <= 0.0; i++) {
                DISPLAYCONFIG_SOURCE_DEVICE_NAME src;
                memset(&src, 0, sizeof src);
                src.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
                src.header.size = sizeof src;
                src.header.adapterId = paths[i].sourceInfo.adapterId;
                src.header.id = paths[i].sourceInfo.id;
                if (DisplayConfigGetDeviceInfo(&src.header) != ERROR_SUCCESS || wcscmp(src.viewGdiDeviceName, mi.szDevice))
                    continue;
                const DISPLAYCONFIG_RATIONAL r = paths[i].targetInfo.refreshRate;
                if (r.Denominator) hz = (double)r.Numerator / (double)r.Denominator;
            }
        free(paths);
        free(modes);
    }
    if (hz <= 1.0) {
        DEVMODEW dm;
        memset(&dm, 0, sizeof dm);
        dm.dmSize = sizeof dm;
        hz = EnumDisplaySettingsW(mi.szDevice, ENUM_CURRENT_SETTINGS, &dm) && dm.dmDisplayFrequency > 1
             ? (double)dm.dmDisplayFrequency : 0.0;
    }
    return hz;
}

static float fg_ms(double now)
{
    return s_fg_last > 0.0 ? (float)((now - s_fg_last) * 1000.0) : 33.3f;
}

static void fg_desc(rsx_fsr3_frame* f)
{
    memset(f, 0, sizeof *f);
    f->scene = s_fg.depth_ok;
    f->depth = s_fg.depth;
    f->hudless = s_fg.ready ? s_fg.hud_out[s_fg.hud_i] : NULL;
    f->objects = s_fg.obj_drawn ? s_fg.obj_rt : NULL;
    memcpy(f->cam, s_fg.cam, sizeof f->cam);
    f->z_scale = s_fg.z_scale;
    f->z_offset = s_fg.z_offset;
    if (s_up.ready) { f->jitter_x = s_up.jx; f->jitter_y = s_up.jy; }
}

/* Every presented frame, before the list closes. */
static void fg_frame(void)
{
    if (!s_fg_mode && !s_up.want) return;
    if (s_fg_mode && !s_fg_ready) {
        s_fg_ready = 1;
        if (!rsx_fsr3_fg_create(s_d3d.device, s_d3d.swap_chain, s_d3d.bb_w, s_d3d.bb_h,
                                s_d3d.pw, s_d3d.ph, s_fg_mode == 2)) {
            s_fg_mode = 0;
            return;
        }
        printf("[D3D12] frame generation %s: %ux%u from %ux%u%c", s_fg_mode == 2 ? "on" : "passthrough",
               s_d3d.bb_w, s_d3d.bb_h, s_d3d.pw, s_d3d.ph, 10);
    }
    const double now = perf_now();
    if (s_fg_mode) {
        rsx_fsr3_frame f;
        fg_desc(&f);
        f.scene = s_fg.ready;
        rsx_fsr3_fg_frame(s_d3d.cmd_list, &f, fg_ms(now));
    }
    s_fg_last = now;
    s_fg.cam_ok = s_fg.marked = s_fg.copied = s_fg.depth_ok = s_fg.ready = 0;
    s_fg.obj_n = 0;
    s_fg.obj_drawn = 0;
    if (s_up.ready) {
        rsx_fsr3_up_jitter(++s_up.frame, &s_up.jx, &s_up.jy);
        s_up.cx = 2.0f * s_up.jx / (float)s_d3d.pw;
        s_up.cy = -2.0f * s_up.jy / (float)s_d3d.ph;
    }
}

/* ---------------------------------------------------------------------------
 * Compiled guest shaders on disk (graphics.shader_cache, D3D12_SHADER_CACHE)
 *
 * Guest vertex and fragment programs are decompiled to HLSL and compiled when
 * a draw first needs them, tens of milliseconds each, so a scene that brings
 * in new programs stops the drain for as long as they take. With a cache file
 * every compiled program is kept under a hash of its source and target, and
 * the next run skips the compiler for every program it has seen. The guest
 * pipelines made from them are kept with them (D3D12_PSO_CACHE, below).
 *
 * Programs come from DXC, or from D3DCompile (see shc_pick_compiler). D3D12
 * refuses a pipeline with one of each, so every program of a guest pipeline
 * comes from the same one, and DXC's are kept apart, in <shader cache>.dxil.
 *
 * The store keeps each program once, under a hash of its bytes; the sources
 * that compile to it and the pipelines made from it name it by that hash.
 * On disk:
 *  - <file>, the pack: the store as the last fold saw it, in one zstd frame
 *    (level 19, long-distance matching, a window as long as the store). The
 *    programs share most of their bytes: God of War: Ascension's 2,287
 *    (28.4 MB) pack to 5.8 MB, where each alone compresses to two thirds.
 *  - <file>.journal: what came after, one record appended for each program,
 *    source or pipeline as it is made.
 * Records, little-endian: a tag, the length of the rest (padded to 4), then
 *    PROG  id, length, bytecode
 *    SRCK  source key, source length, program id
 *    PIPE  pipeline key, vertex and pixel program ids, the session that last
 *          used it (its start, in seconds), its description's length and
 *          the description (psoc_state_bytes)
 *    USED  a session, the number of keys, the keys of the pipelines it used
 *          (the journal only: a fold puts the session in their PIPE records)
 * A fold, on a thread of its own at the lowest priority, packs the store
 * again when the journal has grown past an eighth of the pack (4 MB at
 * least) or the pack cannot be read, and the journal then starts over with
 * what was added while it ran. A record cut short ends the read, and the
 * journal is cut back to the records before it.
 *
 * Before the store, programs were kept whole in <file> (PS3RSHC2), and again,
 * both of each pipeline's, in <file>.pso (GWPSOC01), which stopped taking
 * pipelines at 6,143: the player's cache had grown to 83 MB, the fight
 * bench's to 129 MB. Both are read once into the store; the fold replaces the
 * first and deletes the second.
 * -----------------------------------------------------------------------*/
typedef struct ShProg { u64 id; u32 len; u32 pad; const u8* p; } ShProg;
typedef struct { u64 key; u32 srclen; ShProg* prog; } ShaderCacheEntry;
static ShaderCacheEntry* s_shc;     /* source -> program: open addressing, s_shc_cap slots (a power of two) */
static u32    s_shc_cap, s_shc_n;
static ShProg** s_shp;              /* id -> program: the same, s_shp_cap slots */
static u32    s_shp_cap, s_shp_n;
static ShProg** s_shp_list;         /* the programs in the order they came, s_shp_n of them */
static FILE*  s_shc_file;           /* the journal, appended to */
static u64    s_shc_jid;            /* its id; 0 without one */
static char   s_shc_path[1024];     /* the pack */
static int    s_shc_ro;             /* another process keeps the store on disk: this one only reads it */
static int    s_shc_fold_late;      /* the precompile packs the store once, at its end */
static int    s_shc_old_pso;        /* <pack>.pso was read in (and goes with the next fold) */
static const char* s_shc_why;       /* why shc_open wanted a fold; NULL: it did not */
static int    s_shc_on = -1;
static int    s_shc_dxc;            /* guest programs come from DXC, settled by shc_open */
static SRWLOCK s_shc_lock = SRWLOCK_INIT;
static u32    s_shc_stat_compiled, s_shc_stat_hits, s_shc_stat_pso;   /* PERF=1 report */
static double s_shc_stat_compile_s, s_shc_stat_pso_s;
/* 2: fragment programs read their constants from b1 again, so every one
 * compiled before is text no draw asks for; the file started over.
 * 3: the store's pack. */
static const char s_shc_magic[8]   = { 'P', 'S', '3', 'R', 'S', 'H', 'C', '3' };
static const char s_shc_magic2[8]  = { 'P', 'S', '3', 'R', 'S', 'H', 'C', '2' };
static const char s_shj_magic[8]   = { 'P', 'S', '3', 'R', 'S', 'H', 'J', '1' };
static const char s_psoc_magic1[8] = { 'G', 'W', 'P', 'S', 'O', 'C', '0', '1' };
/* The pack's header: magic, its size and 0 (u32), the store's size, the frame's size,
 * the id of the journal it has the start of and how much of it (u64). */
#define SHC_HDR   48u
#define SHR_PROG  0x474F5250u       /* "PROG" */
#define SHR_SRCK  0x4B435253u       /* "SRCK" */
#define SHR_PIPE  0x45504950u       /* "PIPE" */
#define SHR_USED  0x44455355u       /* "USED" */
#define PSOC_STATE_MAX 2048u        /* a pipeline's description at most (psoc_state_bytes) */
/* The pipelines the store had at the start, in order: built by psoc_thread. */
typedef struct { u64 key, vs, ps; u32 used, stn; const u8* st; } PsocRec;
static PsocRec* s_psoc_recs;
static u32 s_psoc_nrecs, s_psoc_rcap;

/* D3D12_SHADER_OPT=0|1|2|3|skip: the D3DCompile optimisation level for guest
 * programs (default 1, D3DCompile's own default). */
static UINT shc_flags(void)
{
    static int v = -2;
    if (v == -2) {
        const char* e = getenv("D3D12_SHADER_OPT");
        v = !e ? 1 : !strcmp(e, "skip") ? -1 : atoi(e);
    }
    switch (v) {
    case -1: return D3DCOMPILE_SKIP_OPTIMIZATION;
    case 0:  return D3DCOMPILE_OPTIMIZATION_LEVEL0;
    case 2:  return D3DCOMPILE_OPTIMIZATION_LEVEL2;
    case 3:  return D3DCOMPILE_OPTIMIZATION_LEVEL3;
    default: return D3DCOMPILE_OPTIMIZATION_LEVEL1;
    }
}

/* The same level for DXC. */
static const wchar_t* shc_dxc_level(UINT flags)
{
    switch (flags) {
    case D3DCOMPILE_SKIP_OPTIMIZATION:   return L"-Od";
    case D3DCOMPILE_OPTIMIZATION_LEVEL0: return L"-O0";
    case D3DCOMPILE_OPTIMIZATION_LEVEL2: return L"-O2";
    case D3DCOMPILE_OPTIMIZATION_LEVEL3: return L"-O3";
    default:                             return L"-O1";
    }
}

static u64 shc_key(const char* src, size_t len, const char* target, UINT flags)
{
    u64 h = 1469598103934665603ull;   /* FNV-1a 64 */
    for (size_t i = 0; i < len; i++) { h ^= (u8)src[i]; h *= 1099511628211ull; }
    for (const char* t = target; *t; t++) { h ^= (u8)*t; h *= 1099511628211ull; }
    h ^= flags; h *= 1099511628211ull;
    return h;
}

static ShaderCacheEntry* shc_slot(u64 key, u32 srclen)
{
    u32 i = (u32)(key ^ (key >> 32)) & (s_shc_cap - 1);
    while (s_shc[i].prog && !(s_shc[i].key == key && s_shc[i].srclen == srclen))
        i = (i + 1) & (s_shc_cap - 1);
    return &s_shc[i];
}

/* The source's program; 1 if the source was new. s_shc_lock held. */
static int shc_put(u64 key, u32 srclen, ShProg* prog)
{
    if ((s_shc_n + 1) * 2 > s_shc_cap) {          /* keep the table under half full */
        const u32 old_cap = s_shc_cap;
        ShaderCacheEntry* old = s_shc;
        ShaderCacheEntry* t = (ShaderCacheEntry*)calloc(old_cap ? old_cap * 2 : 1024, sizeof *t);
        if (!t) return 0;
        s_shc = t; s_shc_cap = old_cap ? old_cap * 2 : 1024;
        for (u32 i = 0; i < old_cap; i++)
            if (old[i].prog) *shc_slot(old[i].key, old[i].srclen) = old[i];
        free(old);
    }
    ShaderCacheEntry* e = shc_slot(key, srclen);
    if (e->prog) return 0;
    e->key = key; e->srclen = srclen; e->prog = prog; s_shc_n++;
    return 1;
}

/* A program's id: FNV-1a 64 of its bytes, then of their length; never 0. */
static u64 shp_hash(const void* p, size_t n)
{
    const u8* b = (const u8*)p;
    u64 h = 1469598103934665603ull;
    for (size_t i = 0; i < n; i++) { h ^= b[i]; h *= 1099511628211ull; }
    h ^= (u64)n; h *= 1099511628211ull;
    return h ? h : 1;
}

static ShProg** shp_slot(u64 id)
{
    u32 i = (u32)(id ^ (id >> 32)) & (s_shp_cap - 1);
    while (s_shp[i] && s_shp[i]->id != id) i = (i + 1) & (s_shp_cap - 1);
    return &s_shp[i];
}

/* The program with this id, or NULL. Programs never move or go, so one found
 * stays good after the lock is let go. s_shc_lock held (shared will do). */
static ShProg* shp_get(u64 id)
{
    return s_shp_cap ? *shp_slot(id) : NULL;
}

/* The store's program with these bytes, added if it has none (*added): the
 * bytes copied (own), or used where they are, in a buffer kept for the
 * session. id: their hash if the caller has it, else 0. NULL when out of
 * memory, or when the store has other bytes under that hash: never one
 * program for another. s_shc_lock held. */
static ShProg* shp_put(const void* p, u32 n, u64 id, int own, int* added)
{
    *added = 0;
    if (!n) return NULL;
    if (!id) id = shp_hash(p, n);
    if ((s_shp_n + 1) * 2 > s_shp_cap) {          /* under half full; the list as long as half */
        const u32 old_cap = s_shp_cap, cap = old_cap ? old_cap * 2 : 4096;
        ShProg** t = (ShProg**)calloc(cap, sizeof *t);
        ShProg** l = t ? (ShProg**)realloc(s_shp_list, (size_t)(cap / 2) * sizeof *l) : NULL;
        if (!l) { free(t); return NULL; }
        ShProg** old = s_shp;
        s_shp = t; s_shp_cap = cap; s_shp_list = l;
        for (u32 i = 0; i < old_cap; i++)
            if (old[i]) *shp_slot(old[i]->id) = old[i];
        free(old);
    }
    ShProg** s = shp_slot(id);
    if (*s) return (*s)->len == n && !memcmp((*s)->p, p, n) ? *s : NULL;
    ShProg* g = (ShProg*)malloc(sizeof *g + (own ? n : 0));
    if (!g) return NULL;
    g->id = id; g->len = n; g->pad = 0;
    g->p = own ? (const u8*)memcpy(g + 1, p, n) : (const u8*)p;
    *s = g;
    s_shp_list[s_shp_n++] = g;
    *added = 1;
    return g;
}

/* A program's bytes in a blob for the caller to own: the store hands out no
 * blob of its own. d3dcompiler_47 counts a blob's references without atomics
 * (ID3DBlob::Release is a plain load, decrement and store), and one blob the
 * cache shared with the shader workers and the drain thread lost counts:
 * freed while the cache still held it, the next pipeline made from it was
 * refused (E_FAIL, its bytecode gone) and its release freed the block a
 * second time. God of War: Ascension's first level crashed with a corrupted
 * heap in half the runs, a few seconds in. */
static ID3DBlob* shp_blob(const ShProg* g)
{
    ID3DBlob* c = NULL;
    if (FAILED(D3DCreateBlob(g->len, &c)) || !c) return NULL;
    memcpy(c->lpVtbl->GetBufferPointer(c), g->p, g->len);
    return c;
}

/* A record's size, and the record written to o: the tag, the length of the
 * rest (a and b, padded to 4), a, b. */
static u32 shr_size(u32 an, u32 bn) { return 8u + ((an + bn + 3u) & ~3u); }
static u32 shr_fill(u8* o, u32 tag, const void* a, u32 an, const void* b, u32 bn)
{
    const u32 rest = (an + bn + 3u) & ~3u;
    memcpy(o, &tag, 4); memcpy(o + 4, &rest, 4);
    memcpy(o + 8, a, an);
    if (bn) memcpy(o + 8 + an, b, bn);
    memset(o + 8 + an + bn, 0, rest - an - bn);
    return 8u + rest;
}
static void shr_prog(u32 h[4], const ShProg* g)
{
    h[0] = (u32)g->id; h[1] = (u32)(g->id >> 32); h[2] = g->len; h[3] = 0;
}
static void shr_srck(u32 h[6], u64 key, u32 srclen, u64 id)
{
    h[0] = (u32)key; h[1] = (u32)(key >> 32); h[2] = srclen; h[3] = 0;
    h[4] = (u32)id; h[5] = (u32)(id >> 32);
}
static void shr_pipe(u32 h[8], const PsocRec* r)
{
    h[0] = (u32)r->key; h[1] = (u32)(r->key >> 32); h[2] = (u32)r->vs; h[3] = (u32)(r->vs >> 32);
    h[4] = (u32)r->ps; h[5] = (u32)(r->ps >> 32); h[6] = r->used; h[7] = r->stn;
}

/* One record onto the journal, in one write. A write that fails ends the
 * journal for the session; the next one cuts it back. s_shc_lock held. */
static void shj_put(u32 tag, const void* a, u32 an, const void* b, u32 bn)
{
    if (!s_shc_file) return;
    u8 buf[1024];
    const u32 n = shr_size(an, bn);
    u8* o = n <= sizeof buf ? buf : (u8*)malloc(n);
    if (!o) return;
    shr_fill(o, tag, a, an, b, bn);
    if (fwrite(o, 1, n, s_shc_file) != n || fflush(s_shc_file) != 0) {
        fclose(s_shc_file);
        s_shc_file = NULL;
    }
    if (o != buf) free(o);
}

static void shj_prog(const ShProg* g)
{
    u32 h[4];
    shr_prog(h, g);
    shj_put(SHR_PROG, h, 16, g->p, g->len);
}

/* A compiled program into the store under its source, and onto the journal.
 * s_shc_lock held. */
static void shc_keep(u64 key, u32 srclen, ID3DBlob* b)
{
    int added = 0;
    ShProg* g = shp_put(b->lpVtbl->GetBufferPointer(b), (u32)b->lpVtbl->GetBufferSize(b), 0, 1, &added);
    if (!g) return;
    if (added) shj_prog(g);
    if (shc_put(key, srclen, g)) {
        u32 h[6];
        shr_srck(h, key, srclen, g->id);
        shj_put(SHR_SRCK, h, 24, NULL, 0);
    }
}

/* While the store is read: pipeline key -> its record + 1, to keep each once
 * and to find it for a USED record. */
static u32* s_psr_map;
static u32  s_psr_map_cap;
typedef struct { u32 progs, srcs, pipes, bad; } ShrLoad;

static u32* psr_slot(u64 key)
{
    u32 i = (u32)(key ^ (key >> 29)) & (s_psr_map_cap - 1);
    while (s_psr_map[i] && s_psoc_recs[s_psr_map[i] - 1].key != key) i = (i + 1) & (s_psr_map_cap - 1);
    return &s_psr_map[i];
}

/* A pipeline read into s_psoc_recs, unless one with its key was. Its
 * description copied (own), or used where it is, in a buffer kept for the
 * session. */
static int psr_add(u64 key, u64 vs, u64 ps, u32 used, const u8* st, u32 stn, int own)
{
    if (!key) return 0;
    if ((s_psoc_nrecs + 1) * 2 > s_psr_map_cap) {
        const u32 cap = s_psr_map_cap ? s_psr_map_cap * 2 : 8192;
        u32* t = (u32*)calloc(cap, sizeof *t);
        if (!t) return 0;
        free(s_psr_map);
        s_psr_map = t; s_psr_map_cap = cap;
        for (u32 r = 0; r < s_psoc_nrecs; r++) *psr_slot(s_psoc_recs[r].key) = r + 1;
    }
    u32* m = psr_slot(key);
    if (*m) return 0;
    if (s_psoc_nrecs == s_psoc_rcap) {
        const u32 cap = s_psoc_rcap ? s_psoc_rcap * 2 : 1024;
        PsocRec* g = (PsocRec*)realloc(s_psoc_recs, (size_t)cap * sizeof *g);
        if (!g) return 0;
        s_psoc_recs = g; s_psoc_rcap = cap;
    }
    const u8* d = st;
    if (own) {
        u8* c = (u8*)malloc(stn);
        if (!c) return 0;
        d = (const u8*)memcpy(c, st, stn);
    }
    *m = s_psoc_nrecs + 1;
    PsocRec* r = &s_psoc_recs[s_psoc_nrecs++];
    r->key = key; r->vs = vs; r->ps = ps; r->used = used; r->stn = stn; r->st = d;
    return 1;
}

/* The records of b[0, n) into the store; their bytes stay in b, kept for the
 * session. check: a program's bytes must give its id (the journal has no
 * checksum; the pack's frame has). Returns how far the records were whole. */
static size_t shr_read(const u8* b, size_t n, int check, ShrLoad* L)
{
    size_t o = 0;
    while (n - o >= 8) {
        u32 tag, rest;
        memcpy(&tag, b + o, 4); memcpy(&rest, b + o + 4, 4);
        if ((rest & 3u) || rest > n - o - 8) break;
        const u8* r = b + o + 8;
        if (tag == SHR_PROG) {
            u64 id; u32 len; int added = 0;
            if (rest < 16) break;
            memcpy(&id, r, 8); memcpy(&len, r + 8, 4);
            if (!len || ((len + 3u) & ~3u) != rest - 16) break;
            if (check && shp_hash(r + 16, len) != id) L->bad++;
            else if (shp_put(r + 16, len, id, 0, &added)) L->progs += (u32)added;
            else L->bad++;
        } else if (tag == SHR_SRCK) {
            u64 key, id; u32 srclen;
            if (rest != 24) break;
            memcpy(&key, r, 8); memcpy(&srclen, r + 8, 4); memcpy(&id, r + 16, 8);
            ShProg* g = shp_get(id);
            if (g) L->srcs += (u32)shc_put(key, srclen, g);
            else L->bad++;
        } else if (tag == SHR_PIPE) {
            u64 h[3]; u32 used, stn;
            if (rest < 32) break;
            memcpy(h, r, 24); memcpy(&used, r + 24, 4); memcpy(&stn, r + 28, 4);
            if (!stn || stn > PSOC_STATE_MAX || ((stn + 3u) & ~3u) != rest - 32) break;
            if (shp_get(h[1]) && shp_get(h[2])) L->pipes += (u32)psr_add(h[0], h[1], h[2], used, r + 32, stn, 0);
            else L->bad++;
        } else if (tag == SHR_USED) {
            u32 when, k;
            if (rest < 8) break;
            memcpy(&when, r, 4); memcpy(&k, r + 4, 4);
            if ((u64)k * 8u != rest - 8u) break;
            for (u32 j = 0; j < k && s_psr_map_cap; j++) {
                u64 key;
                memcpy(&key, r + 8 + (size_t)j * 8u, 8);
                const u32 m = *psr_slot(key);
                if (m && s_psoc_recs[m - 1].used < when) s_psoc_recs[m - 1].used = when;
            }
        }   /* a kind this build does not know: passed over */
        o += 8u + rest;
    }
    return o;
}

/* PS3RSHC2, the programs before the store: key, source length, length, bytecode. */
static void shl_programs(const u8* b, size_t n, ShrLoad* L)
{
    size_t o = 8;
    while (n - o >= 16) {
        u64 key; u32 h[2]; int added = 0;
        memcpy(&key, b + o, 8); memcpy(h, b + o + 8, 8);
        if (!h[1] || h[1] > (16u << 20) || h[1] > n - o - 16) break;
        ShProg* g = shp_put(b + o + 16, h[1], 0, 1, &added);
        if (g) { L->progs += (u32)added; L->srcs += (u32)shc_put(key, h[0], g); }
        o += 16u + h[1];
    }
}

/* GWPSOC01, the pipelines before the store: key, description, then each
 * program as its length and bytecode. */
static void shl_pipelines(const u8* b, size_t n, ShrLoad* L)
{
    size_t o = 8;
    for (;;) {
        u64 key; u32 stn, vsn, psn; int a1 = 0, a2 = 0;
        if (n - o < 12) break;
        memcpy(&key, b + o, 8); memcpy(&stn, b + o + 8, 4); o += 12;
        if (!stn || stn > PSOC_STATE_MAX || stn > n - o) break;
        const u8* st = b + o; o += stn;
        if (n - o < 4) break;
        memcpy(&vsn, b + o, 4); o += 4;
        if (!vsn || vsn > (4u << 20) || vsn > n - o) break;
        const u8* vs = b + o; o += vsn;
        if (n - o < 4) break;
        memcpy(&psn, b + o, 4); o += 4;
        if (!psn || psn > (4u << 20) || psn > n - o) break;
        const u8* ps = b + o; o += psn;
        ShProg* v = shp_put(vs, vsn, 0, 1, &a1);
        ShProg* p = shp_put(ps, psn, 0, 1, &a2);
        L->progs += (u32)(a1 + a2);
        if (v && p) L->pipes += (u32)psr_add(key, v->id, p->id, 0, st, stn, 1);
        else L->bad++;
    }
}

/* A whole file into *out (NULL when it is empty): 1. 0 when it is not there,
 * -1 when it is but cannot be read (open elsewhere, out of memory): the store
 * then neither folds nor journals, as either could lose what is in it. */
static int shf_read(const char* path, u8** out, size_t* n)
{
    *out = NULL;
    *n = 0;
    FILE* f = fopen(path, "rb");
    if (!f) return errno == ENOENT ? 0 : -1;
    int r = -1;
    if (_fseeki64(f, 0, SEEK_END) == 0) {
        const long long sz = _ftelli64(f);
        if (sz == 0) {
            r = 1;
        } else if (sz > 0 && sz < (1ll << 31) && _fseeki64(f, 0, SEEK_SET) == 0) {
            u8* b = (u8*)malloc((size_t)sz);
            if (b && fread(b, 1, (size_t)sz, f) == (size_t)sz) { *out = b; *n = (size_t)sz; r = 1; }
            else free(b);
        }
    }
    fclose(f);
    return r;
}

static u64 shj_new_id(void)
{
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    u64 x = (u64)c.QuadPart ^ ((u64)time(NULL) << 32) ^ ((u64)GetCurrentProcessId() << 12);
    x ^= x >> 33; x *= 0xff51afd7ed558ccdull; x ^= x >> 33; x *= 0xc4ceb9fe1a85ec53ull; x ^= x >> 33;
    return x ? x : 1;
}

/* A new journal at path, open to append (s_shc_file, s_shc_jid). */
static void shj_create(const char* path)
{
    const u64 id = shj_new_id();
    s_shc_file = fopen(path, "wb");
    if (s_shc_file && (fwrite(s_shj_magic, 1, 8, s_shc_file) != 8 || fwrite(&id, 8, 1, s_shc_file) != 1 ||
                       fflush(s_shc_file) != 0)) {
        fclose(s_shc_file);
        s_shc_file = NULL;
    }
    s_shc_jid = s_shc_file ? id : 0;
}

/* After a fold: the journal again, under a new id, with only what was
 * appended to it after the fold's snapshot (its first jlen bytes are in the
 * pack now). If that fails the old one stays, and the next read starts after
 * what the pack has of it. s_shc_lock held. */
static void shj_restart(u64 jid, u64 jlen)
{
    if (!s_shc_file || s_shc_jid != jid || jlen < 16) return;
    char jp[1100], tmp[1120];
    snprintf(jp, sizeof jp, "%s.journal", s_shc_path);
    snprintf(tmp, sizeof tmp, "%s.tmp", jp);
    fclose(s_shc_file);
    s_shc_file = NULL;
    size_t n = 0;
    u8* b = NULL;
    const u64 id = shj_new_id();
    int ok = 0;
    if (shf_read(jp, &b, &n) == 1 && b && n >= jlen) {
        FILE* f = fopen(tmp, "wb");
        ok = f && fwrite(s_shj_magic, 1, 8, f) == 8 && fwrite(&id, 8, 1, f) == 1 &&
             fwrite(b + jlen, 1, n - (size_t)jlen, f) == n - (size_t)jlen && fflush(f) == 0;
        if (f) fclose(f);
        ok = ok && MoveFileExA(tmp, jp, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
        if (!ok) DeleteFileA(tmp);
    }
    free(b);
    if (ok) s_shc_jid = id;
    s_shc_file = fopen(jp, "ab");
    if (!s_shc_file) s_shc_jid = 0;
}

/* A fold: the store as it was when it began, packed. */
typedef struct {
    ShProg** progs; u32 nprog;
    ShaderCacheEntry* srcs; u32 nsrc;
    u32 npipe;                      /* the first npipe of s_psoc_recs, which no one changes */
    u64 jid, jlen;                  /* the journal, and how long it was */
    int old_pso;                    /* <pack>.pso was read and goes */
    int inline_;                    /* on the caller's thread (the precompile), not one of its own */
    char why[48];
} ShFold;

static DWORD WINAPI shc_fold(LPVOID arg)
{
    ShFold* F = (ShFold*)arg;
    if (!F->inline_) {
        ps3_thread_name("shader cache pack");
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_IDLE);
    }
    const double t0 = perf_now();
    size_t raw = 0, cz = 0;
    for (u32 i = 0; i < F->nprog; i++) raw += shr_size(16, F->progs[i]->len);
    raw += (size_t)F->nsrc * shr_size(24, 0);
    for (u32 i = 0; i < F->npipe; i++) raw += shr_size(32, s_psoc_recs[i].stn);
    const char* fail = NULL;
    u8* pay = (u8*)malloc(raw ? raw : 1);
    const size_t cap = ZSTD_compressBound(raw);
    u8* out = pay ? (u8*)malloc(SHC_HDR + cap) : NULL;
    if (!out) {
        fail = "out of memory";
    } else {
        u8* o = pay;
        for (u32 i = 0; i < F->nprog; i++) {
            u32 h[4];
            shr_prog(h, F->progs[i]);
            o += shr_fill(o, SHR_PROG, h, 16, F->progs[i]->p, F->progs[i]->len);
        }
        for (u32 i = 0; i < F->nsrc; i++) {
            u32 h[6];
            shr_srck(h, F->srcs[i].key, F->srcs[i].srclen, F->srcs[i].prog->id);
            o += shr_fill(o, SHR_SRCK, h, 24, NULL, 0);
        }
        for (u32 i = 0; i < F->npipe; i++) {
            u32 h[8];
            shr_pipe(h, &s_psoc_recs[i]);
            o += shr_fill(o, SHR_PIPE, h, 32, s_psoc_recs[i].st, s_psoc_recs[i].stn);
        }
        ZSTD_CCtx* cc = ZSTD_createCCtx();
        if (!cc) {
            fail = "out of memory";
        } else {
            ZSTD_CCtx_setParameter(cc, ZSTD_c_compressionLevel, 19);
            ZSTD_CCtx_setParameter(cc, ZSTD_c_windowLog, 27);   /* cut to the store's size by zstd */
            ZSTD_CCtx_setParameter(cc, ZSTD_c_enableLongDistanceMatching, 1);
            ZSTD_CCtx_setParameter(cc, ZSTD_c_checksumFlag, 1);
            cz = ZSTD_compress2(cc, out + SHC_HDR, cap, pay, raw);
            if (ZSTD_isError(cz)) fail = ZSTD_getErrorName(cz);
            ZSTD_freeCCtx(cc);
        }
    }
    free(pay);
    if (!fail) {
        char tmp[1100];
        snprintf(tmp, sizeof tmp, "%s.tmp", s_shc_path);
        const u32 hs[2] = { SHC_HDR, 0 };
        const u64 hd[4] = { (u64)raw, (u64)cz, F->jid, F->jlen };
        memcpy(out, s_shc_magic, 8); memcpy(out + 8, hs, 8); memcpy(out + 16, hd, 32);
        FILE* f = fopen(tmp, "wb");
        int ok = f && fwrite(out, 1, SHC_HDR + cz, f) == SHC_HDR + cz && fflush(f) == 0 &&
                 FlushFileBuffers((HANDLE)_get_osfhandle(_fileno(f)));
        if (f) fclose(f);
        if (!ok) fail = "cannot write it";
        else if (!MoveFileExA(tmp, s_shc_path, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
            fail = "cannot put it in place";
        if (fail) DeleteFileA(tmp);
    }
    free(out);
    if (!fail) {
        AcquireSRWLockExclusive(&s_shc_lock);
        shj_restart(F->jid, F->jlen);
        ReleaseSRWLockExclusive(&s_shc_lock);
        if (F->old_pso) {
            char p[1100];
            snprintf(p, sizeof p, "%s.pso", s_shc_path);
            DeleteFileA(p);
        }
        fprintf(stderr, "[d3d12] shader cache packed (%s): %u programs, %u sources, %u pipelines, "
                        "%.1f MB in %.1f MB, %.1f s%c", F->why, F->nprog, F->nsrc, F->npipe,
                raw / 1048576.0, (SHC_HDR + cz) / 1048576.0, perf_now() - t0, 10);
    } else {
        fprintf(stderr, "[d3d12] shader cache not packed (%s): %s%c", F->why, fail, 10);
    }
    free(F->progs); free(F->srcs); free(F);
    return 0;
}

/* A fold of the store as it is now: on its own thread, or (wait) on this one,
 * with s_shc_lock let go while it packs. s_shc_lock held. */
static void shc_fold_start(const char* why, int old_pso, u64 jlen, int wait)
{
    ShFold* F = (ShFold*)calloc(1, sizeof *F);
    if (!F) return;
    F->progs = (ShProg**)malloc((size_t)(s_shp_n + 1) * sizeof *F->progs);
    F->srcs = (ShaderCacheEntry*)malloc((size_t)(s_shc_n + 1) * sizeof *F->srcs);
    if (F->progs && F->srcs) {
        if (s_shp_n) memcpy(F->progs, s_shp_list, (size_t)s_shp_n * sizeof *F->progs);
        F->nprog = s_shp_n;
        for (u32 i = 0; i < s_shc_cap; i++)
            if (s_shc[i].prog) F->srcs[F->nsrc++] = s_shc[i];
        F->npipe = s_psoc_nrecs;
        F->jid = s_shc_file ? s_shc_jid : 0;
        F->jlen = s_shc_file ? jlen : 0;
        F->old_pso = old_pso;
        F->inline_ = wait;
        snprintf(F->why, sizeof F->why, "%s", why);
        if (wait) {
            ReleaseSRWLockExclusive(&s_shc_lock);   /* shc_fold takes it to start the journal over */
            shc_fold(F);
            AcquireSRWLockExclusive(&s_shc_lock);
            return;
        }
        HANDLE t = CreateThread(NULL, 0, shc_fold, F, 0, NULL);
        if (t) { CloseHandle(t); return; }
    }
    free(F->progs); free(F->srcs); free(F);
}

/* graphics.shader_compiler (D3D12_SHADER_COMPILER): dxc, or fxc for D3DCompile.
 * DXC needs its DLLs beside the exe and shader model 6 from the driver; without
 * either the programs come from D3DCompile, as with fxc.
 * The port's own programs (rsx_compile_own) come from D3DCompile, but under Wine,
 * where D3DCompile is vkd3d-shader's, from DXC when the guest programs do (issue 12
 * of Ascension-R). D3D12_OWN_SHADER_COMPILER=dxc or fxc chooses. Once, when the
 * device is made, before the first of them. */
static int s_dxc_usable;   /* DXC's DLLs and shader model 6 */
static void shc_pick_compiler(void)
{
    static int picked;
    if (picked) return;
    picked = 1;
    const char* c = ps3_settings_str("graphics", "shader_compiler", "D3D12_SHADER_COMPILER", "dxc");
    const char* own = getenv("D3D12_OWN_SHADER_COMPILER");
    D3D12_FEATURE_DATA_SHADER_MODEL sm = { D3D_SHADER_MODEL_6_0 };
    const int sm6 = SUCCEEDED(s_d3d.device->lpVtbl->CheckFeatureSupport(s_d3d.device, D3D12_FEATURE_SHADER_MODEL,
                                                                         &sm, sizeof sm)) &&
                    sm.HighestShaderModel >= D3D_SHADER_MODEL_6_0;
    /* its DLLs are loaded only where something may use them */
    const int fxc = ps3_settings_ieq(c, "fxc");
    s_dxc_usable = sm6 && (!fxc || own || getenv("VTX_LIT_COMPILER")) && rsx_dxc_available();
    const char* why = fxc ? "the setting" : !sm6 ? "no shader model 6 here"
                    : !s_dxc_usable ? "no dxcompiler.dll and dxil.dll beside the exe" : NULL;
    s_shc_dxc = !why;
    if (why) printf("[d3d12] guest shaders from D3DCompile: %s%c", why, 10);
    else     printf("[d3d12] guest shaders from DXC%c", 10);

    const char* wine = rsx_wine_version();
    const int own_dxc = s_dxc_usable && (own ? ps3_settings_ieq(own, "dxc") : wine && s_shc_dxc);
    rsx_own_dxc_set(own_dxc);
    if (own || wine)
        printf("[d3d12] the port's own shaders from %s%s%s%s%c", own_dxc ? "DXC" : "D3DCompile",
               own ? " (D3D12_OWN_SHADER_COMPILER)" : "", wine ? ", on Wine " : "", wine ? wine : "", 10);
}

/* graphics.shader_cache (D3D12_SHADER_CACHE): the cache file, or off. The
 * store is read from the pack, the files from before it, and the journal
 * from where the pack's part of it ends. s_shc_lock held. */
static void shc_open(void)
{
    shc_pick_compiler();
    const char* path = ps3_settings_str("graphics", "shader_cache", "D3D12_SHADER_CACHE", NULL);
    s_shc_on = path && *path && strcmp(path, "0") != 0 && !ps3_settings_ieq(path, "off");
    if (!s_shc_on) return;
    snprintf(s_shc_path, sizeof s_shc_path, "%s%s", path, s_shc_dxc ? ".dxil" : "");
    char jp[1100], pp[1100];
    snprintf(jp, sizeof jp, "%s.journal", s_shc_path);
    snprintf(pp, sizeof pp, "%s.pso", s_shc_path);
    /* One process at a time keeps the store on disk (the game, or the launcher's
     * precompile): a named mutex per store, held for the process's life. Another
     * process (a second game, the precompile while the game runs) reads it and
     * keeps what it adds in memory: two writers would interleave the journal. */
    {
        char full[1100];
        if (!GetFullPathNameA(s_shc_path, sizeof full, full, NULL)) snprintf(full, sizeof full, "%s", s_shc_path);
        u64 h = 1469598103934665603ull;
        for (const char* c = full; *c; c++) {
            const u8 ch = (u8)(*c == '/' ? '\\' : (*c >= 'A' && *c <= 'Z') ? *c + 32 : *c);
            h ^= ch; h *= 1099511628211ull;
        }
        char name[64];
        snprintf(name, sizeof name, "Local\\ps3recomp-shader-cache-%016llx", (unsigned long long)h);
        HANDLE m = CreateMutexA(NULL, FALSE, name);
        if (!m || GetLastError() == ERROR_ALREADY_EXISTS) {
            if (m) CloseHandle(m);   /* not ours: the store is free again once its holder exits */
            s_shc_ro = 1;
        }
    }
    const double t0 = perf_now();
    ShrLoad L;
    memset(&L, 0, sizeof L);
    const char* why = NULL;           /* why the store is packed again; NULL: it is not */
    int stuck = 0;                    /* a file of it is there but cannot be read: no fold */
    u64 ajid = 0, ajlen = 0;          /* the journal the pack has the start of, and how much */
    size_t raw = 0, packed = 0, n = 0;
    u8* b = NULL;
    int got = shf_read(s_shc_path, &b, &n);
    stuck |= got < 0;
    if (b && n >= SHC_HDR && !memcmp(b, s_shc_magic, 8)) {
        u32 hs; u64 hd[4];
        memcpy(&hs, b + 8, 4); memcpy(hd, b + 16, 32);
        if (hs == SHC_HDR && hd[1] == n - SHC_HDR && hd[0] && hd[0] < (1ull << 31)) {
            u8* pay = (u8*)malloc((size_t)hd[0]);
            const size_t r = pay ? ZSTD_decompress(pay, (size_t)hd[0], b + SHC_HDR, (size_t)hd[1]) : 0;
            if (!pay) {
                stuck = 1;
            } else if (!ZSTD_isError(r) && r == hd[0]) {
                raw = (size_t)hd[0]; packed = n;
                ajid = hd[2]; ajlen = hd[3];
                if (shr_read(pay, raw, 0, &L) != raw) why = "a pack cut short";
                /* pay stays: the programs and descriptions are in it */
            } else {
                free(pay);
                why = "a damaged pack";
            }
        } else {
            why = "a damaged pack";
        }
    } else if (b && n >= 8 && !memcmp(b, s_shc_magic2, 8)) {
        shl_programs(b, n, &L);
        why = "programs kept before the store";
    } else if (got > 0) {
        why = "a pack of another kind";
    }
    free(b);
    got = shf_read(pp, &b, &n);
    stuck |= got < 0;
    const int old_pso = b && n >= 8 && !memcmp(b, s_psoc_magic1, 8);
    if (old_pso) {
        shl_pipelines(b, n, &L);
        if (!why) why = "pipelines kept before the store";
    }
    free(b);
    /* The journal, from the end of what the pack has of it, cut back to its last whole record. */
    got = shf_read(jp, &b, &n);
    size_t jfrom = 16, jend = 16;
    if (got < 0) {
        stuck = 1;                    /* left alone, and nothing kept on disk this session */
    } else if (b && n >= 16 && !memcmp(b, s_shj_magic, 8)) {
        u64 id;
        memcpy(&id, b + 8, 8);
        if (id && id == ajid && ajlen >= 16 && ajlen <= n) jfrom = (size_t)ajlen;
        jend = jfrom + shr_read(b + jfrom, n - jfrom, 1, &L);
        if (jend == jfrom) free(b);   /* else it stays: the records are in it */
        if (!s_shc_ro) {
            if (jend < n) {
                FILE* f = fopen(jp, "r+b");
                if (f) { _chsize_s(_fileno(f), (long long)jend); fclose(f); }
            }
            s_shc_file = fopen(jp, "ab");
            s_shc_jid = s_shc_file ? id : 0;
        }
    } else {
        free(b);
        if (!s_shc_ro) shj_create(jp);
    }
    free(s_psr_map);
    s_psr_map = NULL; s_psr_map_cap = 0;
    const size_t jlive = jend - jfrom;
    if (!why && jlive >= (4u << 20) && jlive >= raw / 8) why = "the journal has grown";
    printf("[d3d12] shader cache %s: %u programs for %u sources, %u pipelines; %.1f MB packed, %.1f MB of "
           "journal, read in %.0f ms (%u records dropped)%s%s%c", s_shc_path, s_shp_n, s_shc_n, s_psoc_nrecs,
           packed / 1048576.0, jlive / 1048576.0, (perf_now() - t0) * 1000.0, L.bad,
           s_shc_ro ? " (another process keeps it: read only)" : s_shc_file ? "" : " (cannot write it)",
           stuck ? " (a file of it cannot be read: not packed)" : "", 10);
    s_shc_old_pso = old_pso;
    s_shc_why = stuck ? NULL : why;
    if (why && !stuck && !s_shc_ro && !s_shc_fold_late) shc_fold_start(why, old_pso, (u64)jend, 0);
}

/* A guest program (entry "main") through the disk cache. */
static HRESULT d3d_compile_cached(const char* src, size_t len, const char* name,
                                  const char* target, ID3DBlob** out, ID3DBlob** err)
{
    const UINT flags = shc_flags();
    u64 key = 0;
    AcquireSRWLockExclusive(&s_shc_lock);
    if (s_shc_on < 0) shc_open();
    if (s_shc_on) {
        key = shc_key(src, len, target, flags);
        ShaderCacheEntry* e = s_shc_cap ? shc_slot(key, (u32)len) : NULL;
        ID3DBlob* c = e && e->prog ? shp_blob(e->prog) : NULL;
        if (c) {
            *out = c;
            if (err) *err = NULL;
            s_shc_stat_hits++;
            ReleaseSRWLockExclusive(&s_shc_lock);
            return S_OK;
        }
    }
    ReleaseSRWLockExclusive(&s_shc_lock);
    const double t0 = perf_now();
    HRESULT hr = s_shc_dxc ? rsx_dxc_compile(src, len, target, shc_dxc_level(flags), out, err)
                           : D3DCompile(src, len, name, NULL, NULL, "main", target, flags, 0, out, err);
    const double dt = perf_now() - t0;
    if (dt >= 0.1) {
        static int n = 0;
        if (PS3_LOG_FIRST(n, 24))
            printf("[d3d12] slow shader compile: %s (%s, %u bytes of HLSL) took %.0f ms%c",
                   name, target, (unsigned)len, dt * 1000.0, 10);
    }
    /* D3D12_SHADER_MISS_DUMP=<existing dir>: the source of each of the first
     * 400 programs compiled (not in the disk cache), as <n>_<name>.hlsl: what
     * a warm cache still has to compile. */
    { static const char* dir = (const char*)1;
      static volatile LONG nd;
      if (dir == (const char*)1) dir = getenv("D3D12_SHADER_MISS_DUMP");
      if (dir) {
          const LONG i = InterlockedIncrement(&nd) - 1;
          if (i < 400) {
              char p[600];
              snprintf(p, sizeof p, "%s/%04ld_%s.hlsl", dir, (long)i, name);
              FILE* f = fopen(p, "wb");
              if (f) { fwrite(src, 1, len, f); fclose(f); }
          }
      } }
    AcquireSRWLockExclusive(&s_shc_lock);
    s_shc_stat_compiled++;
    s_shc_stat_compile_s += dt;
    if (s_shc_on && SUCCEEDED(hr) && *out)
        shc_keep(key, (u32)len, *out);
    ReleaseSRWLockExclusive(&s_shc_lock);
    return hr;
}

/* A copy of the disk cache's program for this source, or NULL: no compile. */
static ID3DBlob* shc_find(const char* src, size_t len, const char* target)
{
    ID3DBlob* b = NULL;
    AcquireSRWLockExclusive(&s_shc_lock);
    if (s_shc_on < 0) shc_open();
    if (s_shc_on && s_shc_cap) {
        ShaderCacheEntry* e = shc_slot(shc_key(src, len, target, shc_flags()), (u32)len);
        if (e->prog && (b = shp_blob(e->prog)) != NULL)
            s_shc_stat_hits++;
    }
    ReleaseSRWLockExclusive(&s_shc_lock);
    return b;
}

/* ---------------------------------------------------------------------------
 * Guest shaders built off the drain thread (D3D12_ASYNC_SHADERS=<workers>)
 *
 * A guest program the drain has not seen before used to be compiled, and a
 * fragment program's pipeline created, on the drain thread when a draw first
 * needed it, and nothing drew until that was done. God of War HD's raw-fetch
 * vertex programs take 0.4-0.8 s in D3DCompile: the first fight's aftermath
 * stopped for 0.9 s on two of them. Now the drain still decompiles (cheap),
 * then hands the source to a worker, and the draws that need it are skipped
 * until it is built: a new effect turns up a few frames late instead of the
 * picture stopping. A build that finishes inside the wait budget is used at
 * once: the drain may wait D3D12_ASYNC_WAIT_MS (default 3) in every 16 ms.
 * Default 2 workers; 0 builds everything on the drain thread, as before. A
 * program already in the disk cache needs no compile and is used at once
 * (its pipeline is still created by a worker).
 * -----------------------------------------------------------------------*/
typedef struct ShaderJob {
    struct ShaderJob* next;         /* queue link */
    volatile LONG refs;             /* the owner's and the worker's; the last release frees it */
    volatile LONG done;             /* results below are final */
    char* src; size_t len;          /* HLSL, owned */
    const char* name; const char* target;
    void* vs;                       /* pipeline jobs: the vertex shader's bytecode, a copy
                                     * (no blob is shared between threads: shp_blob) */
    D3D12_GRAPHICS_PIPELINE_STATE_DESC pd;   /* pipeline jobs: all but the pixel shader */
    HRESULT hr;
    ID3DBlob* blob;                 /* the compiled program */
    ID3D12PipelineState* pso;       /* pipeline jobs: the pipeline */
    int dbt;                        /* pipeline jobs: with the depth bounds test */
    char err[160];                  /* the start of a compile error */
} ShaderJob;

static int s_sj_workers = -1;
static SRWLOCK s_sj_lock = SRWLOCK_INIT;
static CONDITION_VARIABLE s_sj_work = CONDITION_VARIABLE_INIT;   /* a job queued */
static CONDITION_VARIABLE s_sj_done = CONDITION_VARIABLE_INIT;   /* a job finished */
static ShaderJob *s_sj_head, *s_sj_tail;
static int s_sj_quit;
static HANDLE s_sj_thread[8];
static int s_sj_nowait;             /* set while render_frame only queues builds */
static int s_pso_pending;           /* vp_get_fp_pso returned NULL: still building */
static u32 s_sj_stat_jobs, s_sj_stat_skipped;   /* PERF=1 report */

static void sj_release(ShaderJob* j)
{
    if (!j || InterlockedDecrement(&j->refs)) return;
    if (j->blob) j->blob->lpVtbl->Release(j->blob);
    if (j->pso)  j->pso->lpVtbl->Release(j->pso);
    free(j->vs);
    free(j->src);
    free(j);
}

/* 2.3, the depth bounds test: God of War: Ascension enables it on ~300,000
 * draws a minute in its first level (bounds ~0.97-1.0: effects meant for the
 * far background only). It needs ID3D12GraphicsCommandList1::OMSetDepthBounds
 * and a pipeline with D3D12_DEPTH_STENCIL_DESC1.DepthBoundsTestEnable, which
 * only a pipeline state stream (ID3D12Device2) can describe; the pipelines of
 * the draws that use it are made that way, the rest as before. Off where the
 * adapter lacks it, or with RSX_DEPTH_BOUNDS=0. */
static struct { int init, ok; ID3D12Device2* dev2; ID3D12GraphicsCommandList1* cl1; } s_dbt;
static int dbt_ready(void)
{
    if (s_dbt.init) return s_dbt.ok;
    s_dbt.init = 1;
    const char* e = getenv("RSX_DEPTH_BOUNDS");
    if (e && !atoi(e)) return 0;
    D3D12_FEATURE_DATA_D3D12_OPTIONS2 o2;
    memset(&o2, 0, sizeof o2);
    if (s_d3d.device && s_d3d.cmd_list &&
        SUCCEEDED(s_d3d.device->lpVtbl->CheckFeatureSupport(s_d3d.device, D3D12_FEATURE_D3D12_OPTIONS2, &o2, sizeof o2)) &&
        o2.DepthBoundsTestSupported &&
        SUCCEEDED(s_d3d.device->lpVtbl->QueryInterface(s_d3d.device, &IID_ID3D12Device2, (void**)&s_dbt.dev2)) &&
        SUCCEEDED(s_d3d.cmd_list->lpVtbl->QueryInterface(s_d3d.cmd_list, &IID_ID3D12GraphicsCommandList1, (void**)&s_dbt.cl1)))
        s_dbt.ok = 1;
    printf("[D3D12] depth bounds test %s\n", s_dbt.ok ? "on" : "unavailable");
    return s_dbt.ok;
}
/* Polygon offset set per draw (ID3D12GraphicsCommandList9::RSSetDepthBias)
 * rather than built into the pipeline. The offset was the key field a new
 * pipeline most often differed in from the closest one cached (PSO_MISS_WHY):
 * a minute of God of War: Ascension's first level made 2,000-3,000 pipelines,
 * and 22-57 with the offset set per draw. Off where the adapter lacks it, or
 * with RSX_DYN_DEPTH_BIAS=0. */
static struct { int init, ok; ID3D12GraphicsCommandList9* cl9; } s_dyb;
static int dyb_ready(void)
{
    if (s_dyb.init) return s_dyb.ok;
    s_dyb.init = 1;
    const char* e = getenv("RSX_DYN_DEPTH_BIAS");
    if (e && !atoi(e)) return 0;
    D3D12_FEATURE_DATA_D3D12_OPTIONS16 o16;
    memset(&o16, 0, sizeof o16);
    if (s_d3d.device && s_d3d.cmd_list &&
        SUCCEEDED(s_d3d.device->lpVtbl->CheckFeatureSupport(s_d3d.device, D3D12_FEATURE_D3D12_OPTIONS16, &o16, sizeof o16)) &&
        o16.DynamicDepthBiasSupported &&
        SUCCEEDED(s_d3d.cmd_list->lpVtbl->QueryInterface(s_d3d.cmd_list, &IID_ID3D12GraphicsCommandList9, (void**)&s_dyb.cl9)))
        s_dyb.ok = 1;
    printf("[D3D12] dynamic depth bias %s\n", s_dyb.ok ? "on" : "unavailable");
    return s_dyb.ok;
}
typedef struct { _Alignas(void*) D3D12_PIPELINE_STATE_SUBOBJECT_TYPE t; ID3D12RootSignature* v; } PssRootSig;
typedef struct { _Alignas(void*) D3D12_PIPELINE_STATE_SUBOBJECT_TYPE t; D3D12_SHADER_BYTECODE v; } PssShader;
typedef struct { _Alignas(void*) D3D12_PIPELINE_STATE_SUBOBJECT_TYPE t; D3D12_BLEND_DESC v; } PssBlend;
typedef struct { _Alignas(void*) D3D12_PIPELINE_STATE_SUBOBJECT_TYPE t; UINT v; } PssUint;
typedef struct { _Alignas(void*) D3D12_PIPELINE_STATE_SUBOBJECT_TYPE t; D3D12_RASTERIZER_DESC v; } PssRaster;
typedef struct { _Alignas(void*) D3D12_PIPELINE_STATE_SUBOBJECT_TYPE t; D3D12_DEPTH_STENCIL_DESC1 v; } PssDepth1;
typedef struct { _Alignas(void*) D3D12_PIPELINE_STATE_SUBOBJECT_TYPE t; D3D12_INPUT_LAYOUT_DESC v; } PssLayout;
typedef struct { _Alignas(void*) D3D12_PIPELINE_STATE_SUBOBJECT_TYPE t; D3D12_PRIMITIVE_TOPOLOGY_TYPE v; } PssTopology;
typedef struct { _Alignas(void*) D3D12_PIPELINE_STATE_SUBOBJECT_TYPE t; struct D3D12_RT_FORMAT_ARRAY v; } PssRtFormats;
typedef struct { _Alignas(void*) D3D12_PIPELINE_STATE_SUBOBJECT_TYPE t; DXGI_FORMAT v; } PssFormat;
typedef struct { _Alignas(void*) D3D12_PIPELINE_STATE_SUBOBJECT_TYPE t; DXGI_SAMPLE_DESC v; } PssSample;
/* A guest pipeline: from its description, or as a stream with the depth
 * bounds test on (`dbt`). */
static HRESULT guest_pso_create(const D3D12_GRAPHICS_PIPELINE_STATE_DESC* pd, int dbt,
                                ID3D12PipelineState** out)
{
    if (!dbt || !s_dbt.dev2)
        return s_d3d.device->lpVtbl->CreateGraphicsPipelineState(s_d3d.device, pd, &IID_ID3D12PipelineState,
                                                                 (void**)out);
    struct {
        PssRootSig rs; PssShader vs, ps; PssBlend blend; PssUint mask; PssRaster raster; PssDepth1 depth;
        PssLayout layout; PssTopology topo; PssRtFormats rtf; PssFormat dsf; PssSample sample; PssUint flags;
    } st;
    memset(&st, 0, sizeof st);
    st.rs.t = D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_ROOT_SIGNATURE;      st.rs.v = pd->pRootSignature;
    st.vs.t = D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_VS;                  st.vs.v = pd->VS;
    st.ps.t = D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PS;                  st.ps.v = pd->PS;
    st.blend.t = D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_BLEND;            st.blend.v = pd->BlendState;
    st.mask.t = D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_SAMPLE_MASK;       st.mask.v = pd->SampleMask;
    st.raster.t = D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RASTERIZER;      st.raster.v = pd->RasterizerState;
    st.depth.t = D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL1;
    st.depth.v.DepthEnable      = pd->DepthStencilState.DepthEnable;
    st.depth.v.DepthWriteMask   = pd->DepthStencilState.DepthWriteMask;
    st.depth.v.DepthFunc        = pd->DepthStencilState.DepthFunc;
    st.depth.v.StencilEnable    = pd->DepthStencilState.StencilEnable;
    st.depth.v.StencilReadMask  = pd->DepthStencilState.StencilReadMask;
    st.depth.v.StencilWriteMask = pd->DepthStencilState.StencilWriteMask;
    st.depth.v.FrontFace        = pd->DepthStencilState.FrontFace;
    st.depth.v.BackFace         = pd->DepthStencilState.BackFace;
    st.depth.v.DepthBoundsTestEnable = TRUE;
    st.layout.t = D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_INPUT_LAYOUT;    st.layout.v = pd->InputLayout;
    st.topo.t = D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PRIMITIVE_TOPOLOGY; st.topo.v = pd->PrimitiveTopologyType;
    st.rtf.t = D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RENDER_TARGET_FORMATS;
    st.rtf.v.NumRenderTargets = pd->NumRenderTargets;
    memcpy(st.rtf.v.RTFormats, pd->RTVFormats, sizeof st.rtf.v.RTFormats);
    st.dsf.t = D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL_FORMAT; st.dsf.v = pd->DSVFormat;
    st.sample.t = D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_SAMPLE_DESC;     st.sample.v = pd->SampleDesc;
    st.flags.t = D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_FLAGS;            st.flags.v = (UINT)pd->Flags;
    D3D12_PIPELINE_STATE_STREAM_DESC sd;
    sd.SizeInBytes = sizeof st; sd.pPipelineStateSubobjectStream = &st;
    return s_dbt.dev2->lpVtbl->CreatePipelineState(s_dbt.dev2, &sd, &IID_ID3D12PipelineState, (void**)out);
}

/* ---------------------------------------------------------------------------
 * Guest pipelines kept between sessions (D3D12_PSO_CACHE)
 *
 * A guest pipeline is built when a draw first needs it, and with the async
 * workers the draws that need it are skipped until it is built. God of War:
 * Ascension's first fight built 409 of them, ~1.15 ms each, in bursts of 10-30
 * when something appeared for the first time, and skipped up to 1,562 of a
 * frame's 3,678 draws; every session built them all again. Now each pipeline
 * built goes into the shader cache's store (a PIPE record): its key (a hash of
 * its pixel shader's HLSL, its vertex shader and the rest of its description),
 * that description, and its two programs by their ids. The next session
 * builds them all on a thread of its own from the title's first pipeline on,
 * long before a fight, and a draw finds its pipeline by the key, built. The
 * description is written field by field, so struct padding never reaches the
 * key. D3D12_PSO_CACHE=0: off. It needs graphics.shader_cache.
 *
 * The store keeps every pipeline; the table it used to be read into stopped
 * taking them at 6,143. Built, they cost memory: the fight bench's 6,143 took
 * 373 MB more while they were built, and the fight peaked 281 MB above a run
 * that built one. So the table holds at most 8,192 built (D3D12_PSO_CACHE_MAX);
 * past that a draw's build is kept only by the draw's own pipeline cache, as
 * before. At the start the most recently used are built first: the first time
 * a session uses a pipeline it is noted, and the notes go onto the journal as
 * USED records every 10 s.
 *
 * The start's build runs on several threads, and the title waits for it
 * before its first picture (graphics.pipeline_wait, on by default), so no
 * pipeline the store has is built while the game is played. With the driver's
 * own cache warm the fight bench's 6,143 take 1.02 s on one thread and 0.32 s
 * on six, and the title starts 0.32 s later. Without the wait the build was
 * over 1.1 s into the boot, long before the title, and a whole session
 * (boot, menus, 140 s of the fight) queued one build, on the first logo; the
 * wait is for when the driver compiles them all again, after an update.
 * -----------------------------------------------------------------------*/
#define PSOC_VERSION   1u
static struct PsocSlot { u64 key; ID3D12PipelineState* pso; u32 seen, pad; } *s_psoc;   /* open addressing, s_psoc_cap slots */
static u32 s_psoc_cap, s_psoc_n;
static u32 s_psoc_held, s_psoc_max = 8192;   /* built pipelines held, and the most it holds */
static u32 s_psoc_now;                        /* this session: its start, as PIPE and USED records have it */
static u64* s_psoc_used;                      /* keys first used this session, for the next USED record */
static u32 s_psoc_nused, s_psoc_used_cap;
static SRWLOCK s_psoc_lock = SRWLOCK_INIT;
static int s_psoc_mode = -1;             /* -1 not started, 0 off, 1 on */
static volatile LONG s_psoc_hits, s_psoc_added, s_psoc_built, s_psoc_failed;

static u64 psoc_fnv(u64 h, const void* p, size_t n)
{
    const u8* b = (const u8*)p;
    for (size_t i = 0; i < n; i++) { h ^= b[i]; h *= 1099511628211ull; }
    return h;
}
static void psoc_u32(u8* b, u32* n, u32 v)
{
    if (*n + 4u <= PSOC_STATE_MAX) memcpy(b + *n, &v, 4);
    *n += 4u;
}
static u32 psoc_f32(float f) { u32 v; memcpy(&v, &f, 4); return v; }

/* The description of a guest pipeline but its shaders and root signature, as
 * bytes; 0 for one the cache does not keep. */
static u32 psoc_state_bytes(const D3D12_GRAPHICS_PIPELINE_STATE_DESC* pd, int dbt, u8* b)
{
    u32 n = 0;
    if (pd->pRootSignature != s_d3d.vp_root_sig || pd->InputLayout.NumElements > 16 ||
        pd->StreamOutput.NumEntries || pd->CachedPSO.CachedBlobSizeInBytes)
        return 0;
    psoc_u32(b, &n, PSOC_VERSION); psoc_u32(b, &n, (u32)dbt);
    const D3D12_BLEND_DESC* bl = &pd->BlendState;
    psoc_u32(b, &n, (u32)bl->AlphaToCoverageEnable); psoc_u32(b, &n, (u32)bl->IndependentBlendEnable);
    for (int r = 0; r < 8; r++) {
        const D3D12_RENDER_TARGET_BLEND_DESC* t = &bl->RenderTarget[r];
        psoc_u32(b, &n, (u32)t->BlendEnable); psoc_u32(b, &n, (u32)t->LogicOpEnable);
        psoc_u32(b, &n, (u32)t->SrcBlend); psoc_u32(b, &n, (u32)t->DestBlend); psoc_u32(b, &n, (u32)t->BlendOp);
        psoc_u32(b, &n, (u32)t->SrcBlendAlpha); psoc_u32(b, &n, (u32)t->DestBlendAlpha);
        psoc_u32(b, &n, (u32)t->BlendOpAlpha); psoc_u32(b, &n, (u32)t->LogicOp);
        psoc_u32(b, &n, (u32)t->RenderTargetWriteMask);
    }
    psoc_u32(b, &n, pd->SampleMask);
    const D3D12_RASTERIZER_DESC* ra = &pd->RasterizerState;
    psoc_u32(b, &n, (u32)ra->FillMode); psoc_u32(b, &n, (u32)ra->CullMode);
    psoc_u32(b, &n, (u32)ra->FrontCounterClockwise); psoc_u32(b, &n, (u32)ra->DepthBias);
    psoc_u32(b, &n, psoc_f32(ra->DepthBiasClamp)); psoc_u32(b, &n, psoc_f32(ra->SlopeScaledDepthBias));
    psoc_u32(b, &n, (u32)ra->DepthClipEnable); psoc_u32(b, &n, (u32)ra->MultisampleEnable);
    psoc_u32(b, &n, (u32)ra->AntialiasedLineEnable); psoc_u32(b, &n, ra->ForcedSampleCount);
    psoc_u32(b, &n, (u32)ra->ConservativeRaster);
    const D3D12_DEPTH_STENCIL_DESC* ds = &pd->DepthStencilState;
    psoc_u32(b, &n, (u32)ds->DepthEnable); psoc_u32(b, &n, (u32)ds->DepthWriteMask);
    psoc_u32(b, &n, (u32)ds->DepthFunc); psoc_u32(b, &n, (u32)ds->StencilEnable);
    psoc_u32(b, &n, ds->StencilReadMask); psoc_u32(b, &n, ds->StencilWriteMask);
    const D3D12_DEPTH_STENCILOP_DESC* f[2] = { &ds->FrontFace, &ds->BackFace };
    for (int i = 0; i < 2; i++) {
        psoc_u32(b, &n, (u32)f[i]->StencilFailOp); psoc_u32(b, &n, (u32)f[i]->StencilDepthFailOp);
        psoc_u32(b, &n, (u32)f[i]->StencilPassOp); psoc_u32(b, &n, (u32)f[i]->StencilFunc);
    }
    psoc_u32(b, &n, pd->InputLayout.NumElements);
    for (u32 i = 0; i < pd->InputLayout.NumElements; i++) {
        const D3D12_INPUT_ELEMENT_DESC* e = &pd->InputLayout.pInputElementDescs[i];
        char name[16];
        if (!e->SemanticName || strlen(e->SemanticName) >= sizeof name) return 0;
        memset(name, 0, sizeof name);
        strcpy(name, e->SemanticName);
        for (int k = 0; k < 16; k += 4) { u32 w; memcpy(&w, name + k, 4); psoc_u32(b, &n, w); }
        psoc_u32(b, &n, e->SemanticIndex); psoc_u32(b, &n, (u32)e->Format); psoc_u32(b, &n, e->InputSlot);
        psoc_u32(b, &n, e->AlignedByteOffset); psoc_u32(b, &n, (u32)e->InputSlotClass);
        psoc_u32(b, &n, e->InstanceDataStepRate);
    }
    psoc_u32(b, &n, (u32)pd->IBStripCutValue); psoc_u32(b, &n, (u32)pd->PrimitiveTopologyType);
    psoc_u32(b, &n, pd->NumRenderTargets);
    for (int r = 0; r < 8; r++) psoc_u32(b, &n, (u32)pd->RTVFormats[r]);
    psoc_u32(b, &n, (u32)pd->DSVFormat);
    psoc_u32(b, &n, pd->SampleDesc.Count); psoc_u32(b, &n, pd->SampleDesc.Quality);
    psoc_u32(b, &n, pd->NodeMask); psoc_u32(b, &n, (u32)pd->Flags);
    return n <= PSOC_STATE_MAX ? n : 0;
}

/* psoc_state_bytes read back into pd; il holds its input layout and names
 * (16 bytes each, at least 16 elements). The dbt flag in *dbt. */
static int psoc_state_read(const u8* b, u32 n, D3D12_GRAPHICS_PIPELINE_STATE_DESC* pd,
                           D3D12_INPUT_ELEMENT_DESC* il, char (*names)[16], int* dbt)
{
    u32 o = 0;
#define PSOC_RD() (o + 4u <= n ? (o += 4u, *(const u32*)(b + o - 4u)) : (o = n + 1u, 0u))
    memset(pd, 0, sizeof *pd);
    if (PSOC_RD() != PSOC_VERSION) return 0;
    *dbt = (int)PSOC_RD();
    D3D12_BLEND_DESC* bl = &pd->BlendState;
    bl->AlphaToCoverageEnable = (BOOL)PSOC_RD(); bl->IndependentBlendEnable = (BOOL)PSOC_RD();
    for (int r = 0; r < 8; r++) {
        D3D12_RENDER_TARGET_BLEND_DESC* t = &bl->RenderTarget[r];
        t->BlendEnable = (BOOL)PSOC_RD(); t->LogicOpEnable = (BOOL)PSOC_RD();
        t->SrcBlend = (D3D12_BLEND)PSOC_RD(); t->DestBlend = (D3D12_BLEND)PSOC_RD();
        t->BlendOp = (D3D12_BLEND_OP)PSOC_RD(); t->SrcBlendAlpha = (D3D12_BLEND)PSOC_RD();
        t->DestBlendAlpha = (D3D12_BLEND)PSOC_RD(); t->BlendOpAlpha = (D3D12_BLEND_OP)PSOC_RD();
        t->LogicOp = (D3D12_LOGIC_OP)PSOC_RD(); t->RenderTargetWriteMask = (UINT8)PSOC_RD();
    }
    pd->SampleMask = PSOC_RD();
    D3D12_RASTERIZER_DESC* ra = &pd->RasterizerState;
    ra->FillMode = (D3D12_FILL_MODE)PSOC_RD(); ra->CullMode = (D3D12_CULL_MODE)PSOC_RD();
    ra->FrontCounterClockwise = (BOOL)PSOC_RD(); ra->DepthBias = (INT)PSOC_RD();
    { u32 v = PSOC_RD(); memcpy(&ra->DepthBiasClamp, &v, 4); }
    { u32 v = PSOC_RD(); memcpy(&ra->SlopeScaledDepthBias, &v, 4); }
    ra->DepthClipEnable = (BOOL)PSOC_RD(); ra->MultisampleEnable = (BOOL)PSOC_RD();
    ra->AntialiasedLineEnable = (BOOL)PSOC_RD(); ra->ForcedSampleCount = PSOC_RD();
    ra->ConservativeRaster = (D3D12_CONSERVATIVE_RASTERIZATION_MODE)PSOC_RD();
    D3D12_DEPTH_STENCIL_DESC* ds = &pd->DepthStencilState;
    ds->DepthEnable = (BOOL)PSOC_RD(); ds->DepthWriteMask = (D3D12_DEPTH_WRITE_MASK)PSOC_RD();
    ds->DepthFunc = (D3D12_COMPARISON_FUNC)PSOC_RD(); ds->StencilEnable = (BOOL)PSOC_RD();
    ds->StencilReadMask = (UINT8)PSOC_RD(); ds->StencilWriteMask = (UINT8)PSOC_RD();
    D3D12_DEPTH_STENCILOP_DESC* f[2] = { &ds->FrontFace, &ds->BackFace };
    for (int i = 0; i < 2; i++) {
        f[i]->StencilFailOp = (D3D12_STENCIL_OP)PSOC_RD(); f[i]->StencilDepthFailOp = (D3D12_STENCIL_OP)PSOC_RD();
        f[i]->StencilPassOp = (D3D12_STENCIL_OP)PSOC_RD(); f[i]->StencilFunc = (D3D12_COMPARISON_FUNC)PSOC_RD();
    }
    const u32 ne = PSOC_RD();
    if (ne > 16) return 0;
    for (u32 i = 0; i < ne; i++) {
        for (int k = 0; k < 16; k += 4) { u32 w = PSOC_RD(); memcpy(names[i] + k, &w, 4); }
        names[i][15] = 0;
        il[i].SemanticName = names[i];
        il[i].SemanticIndex = PSOC_RD(); il[i].Format = (DXGI_FORMAT)PSOC_RD(); il[i].InputSlot = PSOC_RD();
        il[i].AlignedByteOffset = PSOC_RD(); il[i].InputSlotClass = (D3D12_INPUT_CLASSIFICATION)PSOC_RD();
        il[i].InstanceDataStepRate = PSOC_RD();
    }
    pd->InputLayout.NumElements = ne;
    pd->InputLayout.pInputElementDescs = ne ? il : NULL;
    pd->IBStripCutValue = (D3D12_INDEX_BUFFER_STRIP_CUT_VALUE)PSOC_RD();
    pd->PrimitiveTopologyType = (D3D12_PRIMITIVE_TOPOLOGY_TYPE)PSOC_RD();
    pd->NumRenderTargets = PSOC_RD();
    for (int r = 0; r < 8; r++) pd->RTVFormats[r] = (DXGI_FORMAT)PSOC_RD();
    pd->DSVFormat = (DXGI_FORMAT)PSOC_RD();
    pd->SampleDesc.Count = PSOC_RD(); pd->SampleDesc.Quality = PSOC_RD();
    pd->NodeMask = PSOC_RD(); pd->Flags = (D3D12_PIPELINE_STATE_FLAGS)PSOC_RD();
#undef PSOC_RD
    pd->pRootSignature = s_d3d.vp_root_sig;
    return o == n;
}

static u64 psoc_key(const char* hlsl, size_t hn, const void* vs, size_t vn, const u8* st, u32 stn)
{
    u64 h = psoc_fnv(1469598103934665603ull, hlsl, hn);
    h = psoc_fnv(h, vs, vn);
    return psoc_fnv(h, st, stn);
}

/* The slot of key, or of the empty place it would go; s_psoc_lock held, s_psoc_cap > 0. */
static u32 psoc_slot(u64 key)
{
    u32 i = (u32)(key ^ (key >> 29)) & (s_psoc_cap - 1u);
    while (s_psoc[i].key && s_psoc[i].key != key) i = (i + 1u) & (s_psoc_cap - 1u);
    return i;
}

/* Room for one more key, the table at most three quarters full; 0 when out of
 * memory. s_psoc_lock held exclusive. */
static int psoc_room(void)
{
    if ((s_psoc_n + 1u) * 4u <= s_psoc_cap * 3u) return 1;
    const u32 old_cap = s_psoc_cap, cap = old_cap ? old_cap * 2u : 8192u;
    struct PsocSlot* t = (struct PsocSlot*)calloc(cap, sizeof *t);
    if (!t) return 0;
    struct PsocSlot* old = s_psoc;
    s_psoc = t; s_psoc_cap = cap;
    for (u32 i = 0; i < old_cap; i++)
        if (old[i].key) s_psoc[psoc_slot(old[i].key)] = old[i];
    free(old);
    return 1;
}

/* The slot's first use this session, noted for a USED record. s_psoc_lock held exclusive. */
static void psoc_note(u32 i)
{
    if (s_psoc[i].seen) return;
    s_psoc[i].seen = 1;
    if (s_psoc_nused == s_psoc_used_cap) {
        const u32 cap = s_psoc_used_cap ? s_psoc_used_cap * 2 : 1024;
        u64* t = (u64*)realloc(s_psoc_used, (size_t)cap * sizeof *t);
        if (!t) return;
        s_psoc_used = t; s_psoc_used_cap = cap;
    }
    s_psoc_used[s_psoc_nused++] = s_psoc[i].key;
}

/* The built pipeline of key, with a reference for the caller; NULL when none
 * is built (yet). *known: the key is in the store or was added this session.
 * use: a draw asks for it (not the cache's own build). */
static ID3D12PipelineState* psoc_find(u64 key, int* known, int use)
{
    ID3D12PipelineState* p = NULL;
    int note = 0;
    *known = 0;
    AcquireSRWLockShared(&s_psoc_lock);
    if (s_psoc_cap) {
        const u32 i = psoc_slot(key);
        *known = s_psoc[i].key == key;
        if (*known && s_psoc[i].pso) {
            p = s_psoc[i].pso; p->lpVtbl->AddRef(p);
            note = use && !s_psoc[i].seen;
        }
    }
    ReleaseSRWLockShared(&s_psoc_lock);
    if (note) {
        AcquireSRWLockExclusive(&s_psoc_lock);
        psoc_note(psoc_slot(key));
        ReleaseSRWLockExclusive(&s_psoc_lock);
    }
    if (p) InterlockedIncrement(&s_psoc_hits);
    return p;
}

/* A pipeline built for a draw: held (one reference) while the table holds
 * fewer than s_psoc_max, and into the store unless it has it: its programs
 * (which it has unless they came from elsewhere) and a PIPE record. The two
 * locks are never held together. */
static void psoc_add(u64 key, ID3D12PipelineState* pso, const u8* st, u32 stn,
                     const void* vs, u32 vsn, const void* ps, u32 psn)
{
    if (s_psoc_mode != 1 || !pso || !key) return;
    int fresh = 0;
    AcquireSRWLockExclusive(&s_psoc_lock);
    if (psoc_room()) {
        const u32 i = psoc_slot(key);
        if (s_psoc[i].key != key) {
            s_psoc[i].key = key; s_psoc[i].seen = 1; s_psoc_n++;   /* its PIPE record has this session */
            fresh = 1;
        } else {
            psoc_note(i);
        }
        if (!s_psoc[i].pso && s_psoc_held < s_psoc_max) {
            pso->lpVtbl->AddRef(pso); s_psoc[i].pso = pso; s_psoc_held++;
        }
    }
    ReleaseSRWLockExclusive(&s_psoc_lock);
    if (!fresh) return;
    AcquireSRWLockExclusive(&s_shc_lock);
    int a1 = 0, a2 = 0;
    ShProg* v = shp_put(vs, vsn, 0, 1, &a1);
    ShProg* p = v ? shp_put(ps, psn, 0, 1, &a2) : NULL;
    if (a1) shj_prog(v);
    if (a2) shj_prog(p);
    if (v && p) {
        PsocRec r;
        u32 h[8];
        r.key = key; r.vs = v->id; r.ps = p->id; r.used = s_psoc_now; r.stn = stn; r.st = st;
        shr_pipe(h, &r);
        shj_put(SHR_PIPE, h, 32, st, stn);
    }
    ReleaseSRWLockExclusive(&s_shc_lock);
    InterlockedIncrement(&s_psoc_added);
}

/* The uses noted since the last call onto the journal, as one USED record. */
static void psoc_flush_used(void)
{
    AcquireSRWLockExclusive(&s_psoc_lock);
    u64* keys = s_psoc_used;
    const u32 n = s_psoc_nused;
    s_psoc_used = NULL; s_psoc_nused = s_psoc_used_cap = 0;
    ReleaseSRWLockExclusive(&s_psoc_lock);
    if (n) {
        const u32 h[2] = { s_psoc_now, n };
        AcquireSRWLockExclusive(&s_shc_lock);
        shj_put(SHR_USED, h, 8, keys, n * 8u);
        ReleaseSRWLockExclusive(&s_shc_lock);
    }
    free(keys);
}

/* The order of the start's build: the session that used it last, the latest
 * first; then the order the store has them in. */
static int psoc_order(const void* a, const void* b)
{
    const u32 x = *(const u32*)a, y = *(const u32*)b;
    const u32 ux = s_psoc_recs[x].used, uy = s_psoc_recs[y].used;
    if (ux != uy) return ux > uy ? -1 : 1;
    return x < y ? -1 : x > y;
}

/* The process's private bytes, and the GPU's local and shared memory in use, in MB. */
static void psoc_mem(double* priv, double* local, double* shared)
{
    PROCESS_MEMORY_COUNTERS_EX m;
    DXGI_QUERY_VIDEO_MEMORY_INFO vl, vs;
    memset(&m, 0, sizeof m); memset(&vl, 0, sizeof vl); memset(&vs, 0, sizeof vs);
    K32GetProcessMemoryInfo(GetCurrentProcess(), (PROCESS_MEMORY_COUNTERS*)&m, sizeof m);
    if (s_adapter3) {
        s_adapter3->lpVtbl->QueryVideoMemoryInfo(s_adapter3, 0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &vl);
        s_adapter3->lpVtbl->QueryVideoMemoryInfo(s_adapter3, 0, DXGI_MEMORY_SEGMENT_GROUP_NON_LOCAL, &vs);
    }
    *priv = m.PrivateUsage / 1048576.0;
    *local = vl.CurrentUsage / 1048576.0;
    *shared = vs.CurrentUsage / 1048576.0;
}

/* The shader build indicator's sources (ovi_*, with the overlay): guest shader jobs
 * queued or being built, and the pipeline cache's build at the start (done of total;
 * total 0 when it is not running). */
static volatile LONG s_sj_inflight;
static volatile LONG s_psoc_done, s_psoc_total;

/* graphics.pipeline_wait (D3D12_PSO_CACHE_WAIT), on by default: the title
 * waits, before its first picture, until the start's build is over. Read once. */
static int psoc_wait_on(void)
{
    static int v = -1;
    if (v < 0) v = ps3_settings_bool("graphics", "pipeline_wait", "D3D12_PSO_CACHE_WAIT", 1);
    return v;
}

/* The start's build: threads at once (D3D12_PSO_CACHE_THREADS, half the
 * logical processors, at most 8), each taking the next pipeline in
 * psoc_order's order until none is left or the table holds s_psoc_max. */
static u32* s_psoc_order;
static volatile LONG s_psoc_next;
static HANDLE s_psoc_built_ev;          /* set when it is over */

static int psoc_nthreads(void)
{
    const char* e = getenv("D3D12_PSO_CACHE_THREADS");
    int n = e ? atoi(e) : 0;
    if (n <= 0) {
        SYSTEM_INFO si;
        GetSystemInfo(&si);
        n = (int)si.dwNumberOfProcessors / 2;
    }
    return n < 1 ? 1 : n > 8 ? 8 : n;
}

static DWORD WINAPI psoc_builder(LPVOID arg)
{
    const int k = (int)(intptr_t)arg;
    if (k) {
        ps3_thread_name("pipeline cache %d", k);
        SetThreadPriority(GetCurrentThread(), psoc_wait_on() ? THREAD_PRIORITY_NORMAL : THREAD_PRIORITY_BELOW_NORMAL);
    }
    D3D12_INPUT_ELEMENT_DESC il[16];
    char names[16][16];
    for (;;) {
        const LONG n = InterlockedIncrement(&s_psoc_next) - 1;
        if ((u32)n >= s_psoc_nrecs || s_psoc_held >= s_psoc_max) break;
        InterlockedIncrement(&s_psoc_done);
        const PsocRec* rec = &s_psoc_recs[s_psoc_order ? s_psoc_order[n] : (u32)n];
        D3D12_GRAPHICS_PIPELINE_STATE_DESC pd;
        int dbt = 0, known = 0;
        ID3D12PipelineState* have = psoc_find(rec->key, &known, 0);
        if (have) { have->lpVtbl->Release(have); continue; }   /* a draw's build got there first */
        AcquireSRWLockShared(&s_shc_lock);
        const ShProg* vs = shp_get(rec->vs);
        const ShProg* ps = shp_get(rec->ps);
        ReleaseSRWLockShared(&s_shc_lock);
        if (!vs || !ps || !psoc_state_read(rec->st, rec->stn, &pd, il, names, &dbt)) {
            InterlockedIncrement(&s_psoc_failed);
            continue;
        }
        pd.VS.pShaderBytecode = vs->p; pd.VS.BytecodeLength = vs->len;
        pd.PS.pShaderBytecode = ps->p; pd.PS.BytecodeLength = ps->len;
        ID3D12PipelineState* pso = NULL;
        if (SUCCEEDED(guest_pso_create(&pd, dbt, &pso)) && pso) {
            AcquireSRWLockExclusive(&s_psoc_lock);
            const u32 i = psoc_slot(rec->key);
            if (s_psoc[i].key == rec->key && !s_psoc[i].pso && s_psoc_held < s_psoc_max) {
                s_psoc[i].pso = pso; pso = NULL; s_psoc_held++;
            }
            ReleaseSRWLockExclusive(&s_psoc_lock);
            if (pso) pso->lpVtbl->Release(pso);
            InterlockedIncrement(&s_psoc_built);
        } else {
            InterlockedIncrement(&s_psoc_failed);
        }
    }
    return 0;
}

/* The start's build (arg: its threads, this one among them), then the
 * session's uses onto the journal every 10 s, for the next start's order. */
static DWORD WINAPI psoc_thread(LPVOID arg)
{
    const int nt = (int)(intptr_t)arg;
    ps3_thread_name("pipeline cache");
    SetThreadPriority(GetCurrentThread(), psoc_wait_on() ? THREAD_PRIORITY_NORMAL : THREAD_PRIORITY_BELOW_NORMAL);
    const double t0 = perf_now();
    double m0[3], m1[3];
    psoc_mem(&m0[0], &m0[1], &m0[2]);
    s_psoc_order = (u32*)malloc((size_t)s_psoc_nrecs * sizeof *s_psoc_order);
    if (s_psoc_order) {
        for (u32 r = 0; r < s_psoc_nrecs; r++) s_psoc_order[r] = r;
        qsort(s_psoc_order, s_psoc_nrecs, sizeof *s_psoc_order, psoc_order);
    }
    InterlockedExchange(&s_psoc_done, 0);
    InterlockedExchange(&s_psoc_total, (LONG)(s_psoc_nrecs < s_psoc_max ? s_psoc_nrecs : s_psoc_max));
    HANDLE h[8];
    int nh = 0;
    for (int k = 1; k < nt && nh < 8; k++) {
        HANDLE t = CreateThread(NULL, 0, psoc_builder, (LPVOID)(intptr_t)k, 0, NULL);
        if (t) h[nh++] = t;
    }
    psoc_builder(NULL);
    if (nh) WaitForMultipleObjects((DWORD)nh, h, TRUE, INFINITE);
    for (int k = 0; k < nh; k++) CloseHandle(h[k]);
    free(s_psoc_order);
    s_psoc_order = NULL;
    InterlockedExchange(&s_psoc_total, 0);
    psoc_mem(&m1[0], &m1[1], &m1[2]);
    fprintf(stderr, "[d3d12] pipeline cache: %ld of %u pipelines built in %.0f ms by %d threads (%ld failed, %u "
                    "held at most); memory while it ran: private %+.0f MB, GPU local %+.0f MB, shared %+.0f MB%c",
            (long)s_psoc_built, s_psoc_nrecs, (perf_now() - t0) * 1000.0, nh + 1, (long)s_psoc_failed,
            s_psoc_max, m1[0] - m0[0], m1[1] - m0[1], m1[2] - m0[2], 10);
    SetEvent(s_psoc_built_ev);
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
    for (;;) {
        Sleep(10000);
        psoc_flush_used();
    }
}

static void rt_pump(void);
static DWORD rt_wake_mask(void);

/* The title, before its first picture, until the start's build is over: at
 * most 30 s, the window's messages seen to meanwhile (without the render
 * thread this thread owns the window). */
static void psoc_hold(void)
{
    const double t0 = perf_now();
    int over = 0;
    while (!over && !s_d3d.window_closed && perf_now() - t0 < 30.0) {
        const DWORD r = MsgWaitForMultipleObjects(1, &s_psoc_built_ev, FALSE, 50, rt_wake_mask());
        if (r == WAIT_OBJECT_0) over = 1;
        else if (r == WAIT_OBJECT_0 + 1) rt_pump();
    }
    fprintf(stderr, "[d3d12] pipeline cache: the title waited %.2f s for it%s%c", perf_now() - t0,
            over ? "" : ", and went on without it", 10);
}

/* Where the title's first guest pipeline is asked for (the render thread, or
 * the walk without it): the store's pipelines into the table, and their build. */
static void psoc_start(void)
{
    if (s_psoc_mode >= 0) return;
    s_psoc_mode = 0;
    const char* e = getenv("D3D12_PSO_CACHE");
    if ((e && !atoi(e)) || !s_d3d.vp_root_sig) return;
    AcquireSRWLockExclusive(&s_shc_lock);
    if (s_shc_on < 0) shc_open();
    ReleaseSRWLockExclusive(&s_shc_lock);
    if (!s_shc_on) return;
    dbt_ready();   /* a depth-bounds pipeline needs it, and it is made on this thread */
    const char* mx = getenv("D3D12_PSO_CACHE_MAX");
    if (mx && atoi(mx) > 0) s_psoc_max = (u32)atoi(mx);
    s_psoc_now = (u32)time(NULL);
    AcquireSRWLockExclusive(&s_psoc_lock);
    for (u32 r = 0; r < s_psoc_nrecs && psoc_room(); r++) {
        const u32 i = psoc_slot(s_psoc_recs[r].key);
        if (!s_psoc[i].key) { s_psoc[i].key = s_psoc_recs[r].key; s_psoc_n++; }
    }
    ReleaseSRWLockExclusive(&s_psoc_lock);
    s_psoc_mode = 1;
    const int nt = psoc_nthreads();
    fprintf(stderr, "[d3d12] pipeline cache: %u pipelines to build on %d threads%s%c", s_psoc_nrecs, nt,
            s_psoc_nrecs && psoc_wait_on() ? ", the title waits for them" : "", 10);
    if (s_psoc_nrecs) {
        s_psoc_built_ev = CreateEventA(NULL, TRUE, FALSE, NULL);
        HANDLE t = s_psoc_built_ev ? CreateThread(NULL, 0, psoc_thread, (LPVOID)(intptr_t)nt, 0, NULL) : NULL;
        if (t) {
            CloseHandle(t);
            if (psoc_wait_on()) psoc_hold();
        }
    }
}

static void sj_build(ShaderJob* j)
{
    ID3DBlob* err = NULL;
    j->hr = d3d_compile_cached(j->src, j->len, j->name, j->target, &j->blob, &err);
    if (err) {
        snprintf(j->err, sizeof j->err, "%s", (const char*)err->lpVtbl->GetBufferPointer(err));
        err->lpVtbl->Release(err);
    }
    if (FAILED(j->hr) || !j->blob || !j->vs) return;
    j->pd.PS.pShaderBytecode = j->blob->lpVtbl->GetBufferPointer(j->blob);
    j->pd.PS.BytecodeLength  = j->blob->lpVtbl->GetBufferSize(j->blob);
    const double t0 = perf_now();
    j->hr = guest_pso_create(&j->pd, j->dbt, &j->pso);
    const double dt = perf_now() - t0;
    if (SUCCEEDED(j->hr) && j->pso && s_psoc_mode == 1) {
        u8 st[PSOC_STATE_MAX];
        const u32 stn = psoc_state_bytes(&j->pd, j->dbt, st);
        if (stn)
            psoc_add(psoc_key(j->src, j->len, j->pd.VS.pShaderBytecode, j->pd.VS.BytecodeLength, st, stn),
                     j->pso, st, stn, j->pd.VS.pShaderBytecode, (u32)j->pd.VS.BytecodeLength,
                     j->pd.PS.pShaderBytecode, (u32)j->pd.PS.BytecodeLength);
    }
    if (FAILED(j->hr)) {
        if (j->hr == DXGI_ERROR_DEVICE_REMOVED) device_lost_report("building a pipeline");
        /* The driver refused it: what it was given. */
        const char* cb = strstr(j->src, "fp_constants[");
        char k[40];
        if (cb) snprintf(k, sizeof k, "fp_constants[%lu]", strtoul(cb + 13, NULL, 10));
        else snprintf(k, sizeof k, "constants compiled in");
        fprintf(stderr, "[FP] pipeline refused (0x%08lX): %u bytes of HLSL, %u of code, %s, dbt %d, %u targets%c",
                (unsigned long)j->hr, (unsigned)j->len, (unsigned)j->pd.PS.BytecodeLength,
                k, j->dbt, j->pd.NumRenderTargets, 10);
    }
    AcquireSRWLockExclusive(&s_shc_lock);
    s_shc_stat_pso++;
    s_shc_stat_pso_s += dt;
    ReleaseSRWLockExclusive(&s_shc_lock);
}

static DWORD WINAPI sj_worker(LPVOID arg)
{
    ps3_thread_name("shader build %d", (int)(intptr_t)arg);
    /* Below the game's threads, as the pipeline cache's builder: on a six-core laptop the
     * game's threads fill the cores, and builds at their priority (a new area's first
     * minute: ~0.7 of a core) cost it ~10% of its frames and left the audio blocks late. */
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
    for (;;) {
        AcquireSRWLockExclusive(&s_sj_lock);
        while (!s_sj_head && !s_sj_quit)
            SleepConditionVariableSRW(&s_sj_work, &s_sj_lock, INFINITE, 0);
        if (s_sj_quit) { ReleaseSRWLockExclusive(&s_sj_lock); return 0; }
        ShaderJob* j = s_sj_head;
        s_sj_head = j->next;
        if (!s_sj_head) s_sj_tail = NULL;
        ReleaseSRWLockExclusive(&s_sj_lock);
        if (j->refs > 1) sj_build(j);   /* else its entry was evicted while it waited */
        AcquireSRWLockExclusive(&s_sj_lock);
        j->done = 1;
        WakeAllConditionVariable(&s_sj_done);
        ReleaseSRWLockExclusive(&s_sj_lock);
        InterlockedDecrement(&s_sj_inflight);
        sj_release(j);
    }
}

/* Workers in use, started on first call; 0 = build on the drain thread.
 * graphics.async_shaders (D3D12_ASYNC_SHADERS), 2 by default. */
static int sj_workers(void)
{
    if (s_sj_workers >= 0) return s_sj_workers;
    int n = (int)ps3_settings_int("graphics", "async_shaders", "D3D12_ASYNC_SHADERS", 2);
    if (n < 0) n = 0;
    if (n > (int)(sizeof s_sj_thread / sizeof s_sj_thread[0])) n = (int)(sizeof s_sj_thread / sizeof s_sj_thread[0]);
    int started = 0;
    for (int i = 0; i < n; i++) {
        s_sj_thread[started] = CreateThread(NULL, 0, sj_worker, (LPVOID)(intptr_t)i, 0, NULL);
        if (s_sj_thread[started]) started++;
    }
    s_sj_workers = started;
    printf("[d3d12] guest shaders built by %d worker thread%s%c", started, started == 1 ? "" : "s", 10);
    return s_sj_workers;
}

/* Queue a compile of src; with vs and pd, also the pipeline for that vertex
 * shader and the compiled program as pixel shader. The caller owns one
 * reference (sj_release). */
static ShaderJob* sj_submit(const char* src, size_t len, const char* name, const char* target,
                            ID3DBlob* vs, const D3D12_GRAPHICS_PIPELINE_STATE_DESC* pd, int dbt)
{
    ShaderJob* j = (ShaderJob*)calloc(1, sizeof *j);
    if (!j) return NULL;
    j->src = (char*)malloc(len + 1);
    if (!j->src) { free(j); return NULL; }
    memcpy(j->src, src, len);
    j->src[len] = 0;
    j->len = len; j->name = name; j->target = target;
    j->refs = 2;
    j->dbt = dbt;
    if (vs && pd) {
        const SIZE_T vn = vs->lpVtbl->GetBufferSize(vs);
        j->vs = malloc(vn);
        if (!j->vs) { free(j->src); free(j); return NULL; }
        memcpy(j->vs, vs->lpVtbl->GetBufferPointer(vs), vn);
        j->pd = *pd;
        j->pd.VS.pShaderBytecode = j->vs;
        j->pd.VS.BytecodeLength  = vn;
    }
    AcquireSRWLockExclusive(&s_sj_lock);
    if (s_sj_tail) s_sj_tail->next = j; else s_sj_head = j;
    s_sj_tail = j;
    InterlockedIncrement(&s_sj_inflight);   /* before a worker can take it */
    WakeConditionVariable(&s_sj_work);
    ReleaseSRWLockExclusive(&s_sj_lock);
    s_sj_stat_jobs++;
    s_hl_jobs++;
    return j;
}

/* j->done with a full barrier: its results are read after it. */
static int sj_is_done(ShaderJob* j)
{
    return InterlockedCompareExchange(&j->done, 0, 0) != 0;
}

/* 1 once j is built, waiting for it out of this 16 ms window's budget. */
static int sj_wait(ShaderJob* j)
{
    if (sj_is_done(j)) return 1;
    if (s_sj_nowait) return 0;
    static double budget = -1.0, win0, used;
    if (budget < 0.0) {
        const char* e = getenv("D3D12_ASYNC_WAIT_MS");
        budget = (e ? atof(e) : 3.0) / 1000.0;
    }
    const double t0 = perf_now();
    if (t0 - win0 > 0.016) { win0 = t0; used = 0.0; }
    if (used >= budget) return 0;
    AcquireSRWLockExclusive(&s_sj_lock);
    for (;;) {
        if (j->done) break;
        const double left = budget - used - (perf_now() - t0);
        if (left <= 0.0) break;
        SleepConditionVariableSRW(&s_sj_done, &s_sj_lock, (DWORD)(left * 1000.0) + 1, 0);
    }
    ReleaseSRWLockExclusive(&s_sj_lock);
    used += perf_now() - t0;
    s_hl_sjwait_s += perf_now() - t0;
    return sj_is_done(j);
}

/* At shutdown: let the workers finish what they are building, and stop. */
static void sj_stop(void)
{
    if (s_sj_workers <= 0) return;
    AcquireSRWLockExclusive(&s_sj_lock);
    s_sj_quit = 1;
    WakeAllConditionVariable(&s_sj_work);
    ReleaseSRWLockExclusive(&s_sj_lock);
    WaitForMultipleObjects((DWORD)s_sj_workers, s_sj_thread, TRUE, 5000);
    for (int i = 0; i < s_sj_workers; i++) CloseHandle(s_sj_thread[i]);
    s_sj_workers = 0;
}
/* Debug switches read on every draw, quad or bind go through this: getenv
 * takes the CRT environment lock and scans every variable, and a dozen of
 * them per draw was a quarter of God of War HD's drain-thread time (ucrtbase
 * in PS3_SAMPLE_DRAIN). Read once, like the switches that already cache. */
static int env_on(const char* name, int* cache)
{
    if (*cache < 0) *cache = getenv(name) ? 1 : 0;
    return *cache;
}
static int s_env_noalphatest = -1, s_env_vp_nofixproj = -1, s_env_vp_fixproj = -1,
           s_env_vp_attrs = -1, s_env_vtx_dump = -1, s_env_movie_bind = -1,
           s_env_texfmtdbg = -1;
/* The same for switches that carry a value; the cache starts at (char*)1. */
static const char* env_str(const char* name, const char** cache)
{
    if (*cache == (const char*)1) *cache = getenv(name);
    return *cache;
}
static const char *s_env_rtt_dump = (const char*)1, *s_env_rtt_pass = (const char*)1,
                  *s_env_rtt_savert = (const char*)1, *s_env_mempeek = (const char*)1,
                  *s_env_rsx_cap = (const char*)1;
/* DBG_LOCK: running clip-space centroid of the tracked mesh (RSX_DBG_VTX's
 * texture), so the debug camera can follow it. The ducks are driven by the
 * physics sim and drift every frame, so a fixed DBG_CENTER loses them as soon
 * as the magnification is high enough to make one recognisable. */
static float s_lock_x = 0.0f, s_lock_y = 0.0f;
static int   s_lock_valid = 0;
/* Accumulated over one frame, published at the frame boundary. Averaging as the
 * frame is recorded makes the aim wander while it is being used, which at high
 * magnification walks the target off-screen entirely. */
static double s_lock_sx = 0.0, s_lock_sy = 0.0;
static u32    s_lock_n  = 0;

/* ---------------------------------------------------------------------------
 * Win32 window
 * -----------------------------------------------------------------------*/

/* A top-level window holds a child, the view, which the swap chain presents
 * to. The swap chain's buffers stay at the title's display size and DXGI
 * stretches them to the view; the view is kept at the largest rectangle of
 * the display's shape centred in the window, so a window or a monitor of
 * another shape gets black bars instead of a stretched picture.
 *
 * display.mode = windowed | borderless (the whole monitor, no frame); Alt+Enter
 * and F11 switch between them. display.window_size = WxH is the windowed client
 * size, the display size by default. Esc closes the game only while
 * system.esc_quits is on, as it always did before the setting. */
static int  s_borderless;
static RECT s_windowed_rect;   /* the frame's rectangle before going borderless */
static volatile LONG s_shot_pending;   /* F12: write the next presented frame (shot_png) */
static volatile LONG s_tex_drop;       /* F9: every held texture uploaded again (vp_upload_tex_slot) */
static int s_shot_cap;                 /* RSX_CAP_SHOT: this frame's shot also captures it */
static void ov_toast(const char* msg);
/* The view's client size, set by the window's thread; the presenting thread
 * resizes the swap chain to it after its next Present (bb_resize). */
static volatile LONG s_view_w, s_view_h;

/* How much wider than the title's 16:9 the picture is to be: 1 unless display.ultrawide is on and the screen is wider. */
float rsx_d3d12_backend_wide_ratio(void)
{
    static float r = -1.0f;
    if (r < 0.0f) {
        float v = 1.0f;
        if (ps3_settings_bool("display", "ultrawide", "D3D12_ULTRAWIDE", 0)) {
            int w = GetSystemMetrics(SM_CXSCREEN), h = GetSystemMetrics(SM_CYSCREEN), ww = 0, wh = 0;
            const char* ws = ps3_settings_str("display", "window_size", NULL, NULL);
            if (!ps3_settings_ieq(ps3_settings_str("display", "mode", NULL, "windowed"), "borderless") &&
                ws && sscanf(ws, "%dx%d", &ww, &wh) == 2 && ww > 0 && wh > 0) {
                w = ww;
                h = wh;
            }
            if (w > 0 && h > 0 && (float)w / (float)h / (16.0f / 9.0f) > 1.02f) v = (float)w / (float)h / (16.0f / 9.0f);
        }
        r = v;
    }
    return r;
}

static void d3d12_place_view(HWND hwnd)
{
    RECT rc;
    if (!s_d3d.view || !s_d3d.width || !s_d3d.height || !GetClientRect(hwnd, &rc)) return;
    const LONG cw = rc.right - rc.left, ch = rc.bottom - rc.top;
    if (cw <= 0 || ch <= 0) return;
    LONG w = cw, h = (LONG)((long long)cw * s_d3d.height / s_d3d.width);
    if (h > ch) { h = ch; w = (LONG)((long long)ch * s_d3d.width / s_d3d.height); }
    const float wide = rsx_d3d12_backend_wide_ratio();
    if (wide != 1.0f) {
        w = cw;
        h = (LONG)((double)cw * s_d3d.height / ((double)s_d3d.width * wide) + 0.5);
        if (h > ch) { h = ch; w = (LONG)((double)ch * s_d3d.width * wide / s_d3d.height + 0.5); }
    }
    MoveWindow(s_d3d.view, (cw - w) / 2, (ch - h) / 2, w, h, TRUE);
    InterlockedExchange(&s_view_w, w);
    InterlockedExchange(&s_view_h, h);
}

static void d3d12_set_borderless(HWND hwnd, int on)
{
    if (on == s_borderless) return;
    if (on) {
        MONITORINFO mi;
        mi.cbSize = sizeof mi;
        if (!GetMonitorInfoA(MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST), &mi)) return;
        GetWindowRect(hwnd, &s_windowed_rect);
        SetWindowLongA(hwnd, GWL_STYLE, WS_POPUP | WS_VISIBLE | WS_CLIPCHILDREN);
        SetWindowPos(hwnd, HWND_TOP, mi.rcMonitor.left, mi.rcMonitor.top,
                     mi.rcMonitor.right - mi.rcMonitor.left, mi.rcMonitor.bottom - mi.rcMonitor.top,
                     SWP_FRAMECHANGED | SWP_NOOWNERZORDER);
    } else {
        SetWindowLongA(hwnd, GWL_STYLE, WS_OVERLAPPEDWINDOW | WS_VISIBLE | WS_CLIPCHILDREN);
        SetWindowPos(hwnd, HWND_NOTOPMOST, s_windowed_rect.left, s_windowed_rect.top,
                     s_windowed_rect.right - s_windowed_rect.left,
                     s_windowed_rect.bottom - s_windowed_rect.top,
                     SWP_FRAMECHANGED | SWP_NOOWNERZORDER);
    }
    s_borderless = on;
    d3d12_place_view(hwnd);
    printf("[D3D12] window: %s\n", on ? "borderless" : "windowed");
}

static int d3d12_esc_quits(void)
{
    static int v = -1;
    if (v < 0) v = ps3_settings_bool("system", "esc_quits", NULL, 1);
    return v;
}

static LRESULT CALLBACK d3d12_wndproc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_CLOSE:
        /* With the render thread on, the window stays until the process ends:
         * that thread may be presenting into it. The main loop ends the
         * process on window_closed either way. */
        s_d3d.window_closed = 1;
        if (s_rt_mode <= 0) DestroyWindow(hwnd);
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    case WM_SIZE:
        d3d12_place_view(hwnd);
        return 0;
    case WM_SETCURSOR:   /* no pointer over the picture when borderless */
        if (s_borderless && LOWORD(lp) == HTCLIENT) { SetCursor(NULL); return TRUE; }
        break;
    case WM_SYSCOMMAND:
        /* Alt alone opens the window menu in a modal loop on this thread,
         * which is the one that presents; the game would stand still. */
        if ((wp & 0xFFF0) == SC_KEYMENU) return 0;
        break;
    case WM_SYSKEYDOWN:
        if (wp == VK_RETURN && (lp & (1 << 29))) {   /* Alt+Enter */
            if (!(lp & (1 << 30))) {
                d3d12_set_borderless(hwnd, !s_borderless);
                ov_toast(s_borderless ? "Borderless fullscreen" : "Windowed");
            }
            return 0;
        }
        break;   /* Alt+F4 and the rest: the default */
    case WM_KEYDOWN:
        if (wp == VK_F11) {
            if (!(lp & (1 << 30))) {
                d3d12_set_borderless(hwnd, !s_borderless);
                ov_toast(s_borderless ? "Borderless fullscreen" : "Windowed");
            }
        } else if (wp == VK_F12) {
            if (!(lp & (1 << 30))) InterlockedExchange(&s_shot_pending, 1);
        } else if (wp == VK_F9) {
            /* Every texture read again from the game's memory: if that mends a broken picture,
             * what broke was the copy held here, not the game's bytes (issue #9). */
            if (!(lp & (1 << 30))) {
                InterlockedExchange(&s_tex_drop, 1);
                ov_toast("Textures uploaded again");
            }
        } else if (wp == VK_ESCAPE && d3d12_esc_quits()) {
            s_d3d.window_closed = 1;
            if (s_rt_mode <= 0) DestroyWindow(hwnd);   /* as WM_CLOSE */
        }
        return 0;
    }
    return DefWindowProcA(hwnd, msg, wp, lp);
}

static LRESULT CALLBACK d3d12_view_wndproc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    if (msg == WM_ERASEBKGND) return 1;   /* the swap chain covers it */
    return DefWindowProcA(hwnd, msg, wp, lp);
}

static HWND create_window(u32 width, u32 height, const char* title)
{
    WNDCLASSEXA wc = {0};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = d3d12_wndproc;
    wc.hInstance = GetModuleHandle(NULL);
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);   /* the bars */
    /* The program's own icon, resource 1, when it has one (a port's resource script); Windows' own otherwise. */
    wc.hIcon = LoadIconA(wc.hInstance, MAKEINTRESOURCEA(1));
    wc.hIconSm = (HICON)LoadImageA(wc.hInstance, MAKEINTRESOURCEA(1), IMAGE_ICON, GetSystemMetrics(SM_CXSMICON),
                                   GetSystemMetrics(SM_CYSMICON), 0);
    wc.lpszClassName = "ps3recomp_d3d12";
    RegisterClassExA(&wc);
    wc.lpfnWndProc = d3d12_view_wndproc;
    wc.hbrBackground = NULL;
    wc.lpszClassName = "ps3recomp_d3d12_view";
    RegisterClassExA(&wc);

    /* Physical pixels. Without this, Windows scales the whole window by the
     * display's scale factor (125% on many laptops) after DXGI has already
     * stretched the picture once, and both blur it. A window of the default
     * size keeps the size it had: the display size times that factor. */
    UINT dpi = 96;
    {
        typedef BOOL (WINAPI *set_ctx_fn)(HANDLE);
        typedef UINT (WINAPI *get_dpi_fn)(void);
        HMODULE user = GetModuleHandleA("user32.dll");
        set_ctx_fn set_ctx = user ? (set_ctx_fn)(void*)GetProcAddress(user, "SetProcessDpiAwarenessContext") : NULL;
        get_dpi_fn get_dpi = user ? (get_dpi_fn)(void*)GetProcAddress(user, "GetDpiForSystem") : NULL;
        if (set_ctx) set_ctx((HANDLE)(intptr_t)-4);   /* DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2 */
        if (get_dpi) dpi = get_dpi();
        if (dpi < 96) dpi = 96;
    }
    u32 cw = width * dpi / 96, ch = height * dpi / 96;
    const char* ws = ps3_settings_str("display", "window_size", NULL, NULL);
    if (ws && *ws) {
        unsigned a = 0, c = 0;
        char x = 0;
        if (sscanf(ws, "%u%c%u", &a, &x, &c) == 3 && (x == 'x' || x == 'X') &&
            a >= 320 && c >= 180 && a <= 16384 && c <= 16384) { cw = a; ch = c; }
        else printf("[D3D12] display.window_size = %s is not WxH; using %ux%u\n", ws, cw, ch);
    }
    RECT wr = {0, 0, (LONG)cw, (LONG)ch};
    AdjustWindowRect(&wr, WS_OVERLAPPEDWINDOW, FALSE);

    /* Keep the display awake (see rsx_null_backend.c). */
    SetThreadExecutionState(ES_CONTINUOUS | ES_DISPLAY_REQUIRED | ES_SYSTEM_REQUIRED);
    HWND hwnd = CreateWindowExA(
        0, "ps3recomp_d3d12",
        title ? title : "ps3recomp (D3D12)",
        WS_OVERLAPPEDWINDOW | WS_VISIBLE | WS_CLIPCHILDREN,
        CW_USEDEFAULT, CW_USEDEFAULT,
        wr.right - wr.left, wr.bottom - wr.top,
        NULL, NULL, GetModuleHandle(NULL), NULL);
    if (!hwnd) return NULL;
    s_d3d.view = CreateWindowExA(0, "ps3recomp_d3d12_view", NULL, WS_CHILD | WS_VISIBLE,
                                 0, 0, (int)cw, (int)ch, hwnd, NULL, GetModuleHandle(NULL), NULL);
    if (!s_d3d.view) { DestroyWindow(hwnd); return NULL; }
    d3d12_place_view(hwnd);

    const char* mode = ps3_settings_str("display", "mode", NULL, "windowed");
    if (ps3_settings_ieq(mode, "borderless")) d3d12_set_borderless(hwnd, 1);
    else if (!ps3_settings_ieq(mode, "windowed"))
        printf("[D3D12] display.mode = %s is not windowed or borderless; windowed\n", mode);
    return hwnd;
}

/* ---------------------------------------------------------------------------
 * D3D12 initialization
 * -----------------------------------------------------------------------*/

/* display.vsync off (D3D12_VSYNC=0): present without waiting for the vblank,
 * for measuring a title past the display's rate. Read once. */
static int d3d12_vsync_off(void)
{
    static int off = -1;
    if (off < 0) off = !ps3_settings_bool("display", "vsync", "D3D12_VSYNC", 1);
    return off;
}

static int pp_init(void);
static void ov_init(D3D12_GRAPHICS_PIPELINE_STATE_DESC pd);
static int aa_init(D3D12_GRAPHICS_PIPELINE_STATE_DESC pd);

/* The device (s_d3d.device), 0 on success. On a dual-GPU laptop a NULL adapter
 * usually lands on the integrated GPU; explicitly pick the high-performance
 * (discrete) adapter via IDXGIFactory6. graphics.gpu = integrated (D3D12_GPU)
 * takes the low-power one instead, as CELLMARK_IGPU does (for A/B testing a
 * suspected iGPU-driver stall). A launcher that shows the choice asks Windows
 * the same way. The game's start and the precompile (which must pick the same
 * compiler) both come here. */
static int d3d_device_create(IDXGIFactory4* factory)
{
    IDXGIFactory6* factory6 = NULL;
    if (SUCCEEDED(factory->lpVtbl->QueryInterface(
            factory, &IID_IDXGIFactory6, (void**)&factory6))) {
        DXGI_GPU_PREFERENCE pref = getenv("CELLMARK_IGPU") ||
                                   ps3_settings_ieq(ps3_settings_str("graphics", "gpu", "D3D12_GPU", "discrete"), "integrated")
            ? DXGI_GPU_PREFERENCE_MINIMUM_POWER
            : DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE;
        IDXGIAdapter1* adapter = NULL;
        for (UINT ai = 0; factory6->lpVtbl->EnumAdapterByGpuPreference(
                 factory6, ai, pref, &IID_IDXGIAdapter1, (void**)&adapter)
                 != DXGI_ERROR_NOT_FOUND; ai++) {
            DXGI_ADAPTER_DESC1 ad;
            adapter->lpVtbl->GetDesc1(adapter, &ad);
            if (!(ad.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) &&
                SUCCEEDED(D3D12CreateDevice((IUnknown*)adapter,
                    D3D_FEATURE_LEVEL_11_0, &IID_ID3D12Device,
                    (void**)&s_d3d.device))) {
                printf("[D3D12] adapter: %ls (VRAM %llu MB)\n", ad.Description,
                       (unsigned long long)(ad.DedicatedVideoMemory >> 20));
                s_vram_mb = (u32)(ad.DedicatedVideoMemory >> 20);
                for (int ni = 0; ni < (int)sizeof s_ovs_gpu_name - 1 && ad.Description[ni]; ni++)
                    s_ovs_gpu_name[ni] = (char)(ad.Description[ni] < 128 ? ad.Description[ni] : '?');
                /* An integrated GPU's textures are in the PC's memory: the little it
                 * has of its own says nothing about what fits. */
                { D3D12_FEATURE_DATA_ARCHITECTURE arch;
                  memset(&arch, 0, sizeof arch);
                  if (SUCCEEDED(s_d3d.device->lpVtbl->CheckFeatureSupport(s_d3d.device, D3D12_FEATURE_ARCHITECTURE, &arch, sizeof arch)) && arch.UMA)
                      s_vram_mb = (u32)((ad.DedicatedVideoMemory + ad.SharedSystemMemory) >> 20); }
                if (FAILED(adapter->lpVtbl->QueryInterface(adapter, &IID_IDXGIAdapter3, (void**)&s_adapter3)))
                    s_adapter3 = NULL;
                adapter->lpVtbl->Release(adapter);
                break;
            }
            adapter->lpVtbl->Release(adapter);
            adapter = NULL;
        }
        factory6->lpVtbl->Release(factory6);
    }
    if (!s_d3d.device) {
        const HRESULT hr = D3D12CreateDevice(NULL, D3D_FEATURE_LEVEL_11_0,
                                             &IID_ID3D12Device, (void**)&s_d3d.device);
        if (FAILED(hr)) {
            printf("[D3D12] ERROR: D3D12CreateDevice failed (0x%08lX)\n", hr);
            return -1;
        }
        printf("[D3D12] Device created on default adapter (feature level 11.0)\n");
    }
    return 0;
}

/* Once the view's size is known. A temporal upscaler and frame generation take no render larger
 * than the display. */
static void rs_init(void)
{
    const char* v = ps3_settings_str("graphics", "render_scale", "D3D12_RENDER_SCALE", "1");
    const char* cb = getenv("FP_CONSTBUF");
    if (cb && cb[0] == '0') {   /* WPOS is divided back through the constant buffer */
        if (!ps3_settings_ieq(v, "1")) printf("[D3D12] render scale: 1, FP_CONSTBUF=0 has no way to divide WPOS%c", 10);
        return;
    }
    s_wide = rsx_d3d12_backend_wide_ratio();
    const float fit_w = (float)s_d3d.bb_w / ((float)s_d3d.width * s_wide), fit_h = (float)s_d3d.bb_h / (float)s_d3d.height;
    const float fit = fit_w < fit_h ? fit_w : fit_h;
    float s = ps3_settings_ieq(v, "native") ? fit : (float)atof(v);
    if (!(s >= 1.0f)) s = 1.0f;
    if (s > 4.0f) s = 4.0f;
    if (s > fit && (s_fg_mode || (ps3_settings_ieq(ps3_settings_str("graphics", "scaling", NULL, "bilinear"), "fsr3") &&
                                  !s_fsr3_refused && rsx_fsr3_available()))) {
        printf("[D3D12] render scale %.3g: held to the display's %.3g for FSR 3%c", s, fit < 1.0f ? 1.0f : fit, 10);
        s = fit < 1.0f ? 1.0f : fit;
    }
    s_rs = s;
    s_d3d.pw = (u32)((float)s_d3d.width * s * s_wide + 0.5f);
    s_d3d.ph = (u32)((float)s_d3d.height * s + 0.5f);
    if (RS_ON) printf("[D3D12] internal resolution: %ux%u (%.3gx %ux%u)%c", s_d3d.pw, s_d3d.ph, s, s_d3d.width, s_d3d.height, 10);
}

/* Whether a surface of this guest size is drawn larger. */
static int rs_class(u32 w, u32 h)
{
    return RS_ON && w == s_d3d.width && h == s_d3d.height;
}

/* A guest coordinate in the pixels of a target drawn f times larger. */
static u32 rs_px(u32 v, float f)
{
    return (u32)((float)v * f + 0.5f);
}

/* A guest rectangle in the pixels of a target drawn sx, sy times larger. The factors are not
 * exact: the slack keeps the target's own edge from rounding one pixel outward. */
static void rs_rect(D3D12_RECT* r, float sx, float sy)
{
    if (sx == 1.0f && sy == 1.0f) return;
    r->left = (LONG)floorf((float)r->left * sx + 0.001f);
    r->top = (LONG)floorf((float)r->top * sy + 0.001f);
    r->right = (LONG)ceilf((float)r->right * sx - 0.001f);
    r->bottom = (LONG)ceilf((float)r->bottom * sy - 0.001f);
}

/* The factors of what a pass draws into: its depth surface when it has no colour, else its colour
 * target, else the display target. */
static void rs_target(int rt, int z, int depth_only, float* sx, float* sy)
{
    if (depth_only && z >= 0) { *sx = s_d3d.off_ds[z].sx; *sy = s_d3d.off_ds[z].sy; }
    else if (rt >= 0)         { *sx = s_d3d.off_rt[rt].sx; *sy = s_d3d.off_rt[rt].sy; }
    else { *sx = (float)s_d3d.pw / (float)s_d3d.width; *sy = (float)s_d3d.ph / (float)s_d3d.height; }
}

static int init_d3d12(u32 width, u32 height)
{
    HRESULT hr;

    if (getenv("D3D12_DRED")) {
        ID3D12DeviceRemovedExtendedDataSettings* dred = NULL;
        if (SUCCEEDED(D3D12GetDebugInterface(&IID_ID3D12DeviceRemovedExtendedDataSettings, (void**)&dred)) && dred) {
            dred->lpVtbl->SetAutoBreadcrumbsEnablement(dred, D3D12_DRED_ENABLEMENT_FORCED_ON);
            dred->lpVtbl->SetPageFaultEnablement(dred, D3D12_DRED_ENABLEMENT_FORCED_ON);
            dred->lpVtbl->Release(dred);
            printf("[D3D12] DRED on%c", 10);
        }
    }

    /* Enable debug layer in debug builds */
    /* Debug layer: on in debug builds, or in any build when D3D12_DBG is set
     * (so a Release run can capture exact PSO/validation errors). */
    if (
#ifdef NDEBUG
        getenv("D3D12_DBG")
#else
        1
#endif
    ) {
        ID3D12Debug* debug_controller = NULL;
        hr = D3D12GetDebugInterface(&IID_ID3D12Debug, (void**)&debug_controller);
        if (SUCCEEDED(hr) && debug_controller) {
            debug_controller->lpVtbl->EnableDebugLayer(debug_controller);
            debug_controller->lpVtbl->Release(debug_controller);
            printf("[D3D12] Debug layer enabled\n");
        } else {
            printf("[D3D12] Debug layer requested but unavailable (0x%08lX) -- "
                   "install 'Graphics Tools' optional feature\n", hr);
        }
    }

    rdoc_load();   /* before the factory and device: RenderDoc hooks their creation */

    /* Create DXGI factory */
    IDXGIFactory4* factory = NULL;
    hr = CreateDXGIFactory1(&IID_IDXGIFactory4, (void**)&factory);
    if (FAILED(hr)) {
        printf("[D3D12] ERROR: CreateDXGIFactory1 failed (0x%08lX)\n", hr);
        return -1;
    }

    if (d3d_device_create(factory) != 0) {
        factory->lpVtbl->Release(factory);
        return -1;
    }
    shc_pick_compiler();   /* which compiler makes the programs, before the first of them */

    /* Create command queue */
    D3D12_COMMAND_QUEUE_DESC queue_desc = {0};
    queue_desc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    hr = s_d3d.device->lpVtbl->CreateCommandQueue(
        s_d3d.device, &queue_desc, &IID_ID3D12CommandQueue, (void**)&s_d3d.cmd_queue);
    if (FAILED(hr)) {
        printf("[D3D12] ERROR: CreateCommandQueue failed\n");
        factory->lpVtbl->Release(factory);
        return -1;
    }

    /* Create swap chain, at the view's size: the display targets are scaled
     * into it (present_to_backbuffer), not stretched by DXGI. */
    {
        RECT vr;
        s_d3d.bb_w = width;
        s_d3d.bb_h = height;
        if (GetClientRect(s_d3d.view, &vr) && vr.right > 0 && vr.bottom > 0) {
            s_d3d.bb_w = (u32)vr.right;
            s_d3d.bb_h = (u32)vr.bottom;
        }
    }
    DXGI_SWAP_CHAIN_DESC1 sc_desc = {0};
    sc_desc.Width = s_d3d.bb_w;
    sc_desc.Height = s_d3d.bb_h;
    sc_desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sc_desc.SampleDesc.Count = 1;
    sc_desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sc_desc.BufferCount = FRAME_COUNT;
    sc_desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    /* D3D12_VSYNC=0 presents with a sync interval of 0, but a windowed
     * flip-model swap chain still waits for the compositor, which runs at the
     * display's rate, unless it allows tearing. So allow it where DXGI can. */
    if (d3d12_vsync_off()) {
        IDXGIFactory5* factory5 = NULL;
        BOOL tear = FALSE;
        if (SUCCEEDED(factory->lpVtbl->QueryInterface(
                factory, &IID_IDXGIFactory5, (void**)&factory5))) {
            if (FAILED(factory5->lpVtbl->CheckFeatureSupport(
                    factory5, DXGI_FEATURE_PRESENT_ALLOW_TEARING, &tear, sizeof tear)))
                tear = FALSE;
            factory5->lpVtbl->Release(factory5);
        }
        s_d3d.tearing = tear ? 1 : 0;
        if (tear) sc_desc.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING;
        printf("[D3D12] D3D12_VSYNC=0: tearing %s\n", tear ? "allowed" : "not supported");
    }

    IDXGISwapChain1* swap_chain1 = NULL;
    hr = factory->lpVtbl->CreateSwapChainForHwnd(
        factory, (IUnknown*)s_d3d.cmd_queue,
        s_d3d.view, &sc_desc, NULL, NULL, &swap_chain1);
    if (FAILED(hr)) {
        printf("[D3D12] ERROR: CreateSwapChainForHwnd failed (0x%08lX)\n", hr);
        factory->lpVtbl->Release(factory);
        return -1;
    }

    /* Disable Alt+Enter fullscreen toggle */
    factory->lpVtbl->MakeWindowAssociation(factory, s_d3d.view, DXGI_MWA_NO_ALT_ENTER);
    factory->lpVtbl->Release(factory);

    /* Query SwapChain3 interface */
    hr = swap_chain1->lpVtbl->QueryInterface(
        swap_chain1, &IID_IDXGISwapChain3, (void**)&s_d3d.swap_chain);
    swap_chain1->lpVtbl->Release(swap_chain1);
    if (FAILED(hr)) {
        printf("[D3D12] ERROR: QueryInterface for SwapChain3 failed\n");
        return -1;
    }

    fg_start();
    rs_init();
    s_d3d.frame_index = s_d3d.swap_chain->lpVtbl->GetCurrentBackBufferIndex(s_d3d.swap_chain);

    /* Create RTV descriptor heap */
    D3D12_DESCRIPTOR_HEAP_DESC rtv_heap_desc = {0};
    rtv_heap_desc.NumDescriptors = FRAME_COUNT;
    rtv_heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    hr = s_d3d.device->lpVtbl->CreateDescriptorHeap(
        s_d3d.device, &rtv_heap_desc, &IID_ID3D12DescriptorHeap, (void**)&s_d3d.rtv_heap);
    if (FAILED(hr)) return -1;

    s_d3d.rtv_descriptor_size = s_d3d.device->lpVtbl->GetDescriptorHandleIncrementSize(
        s_d3d.device, D3D12_DESCRIPTOR_HEAP_TYPE_RTV);

    /* Create RTVs for each frame */
    D3D12_CPU_DESCRIPTOR_HANDLE rtv_handle;
    s_d3d.rtv_heap->lpVtbl->GetCPUDescriptorHandleForHeapStart(s_d3d.rtv_heap, &rtv_handle);

    for (u32 i = 0; i < FRAME_COUNT; i++) {
        hr = s_d3d.swap_chain->lpVtbl->GetBuffer(
            s_d3d.swap_chain, i, &IID_ID3D12Resource, (void**)&s_d3d.backbuffers[i]);
        if (FAILED(hr)) return -1;

        D3D12_HEAP_PROPERTIES hp = {0};
        hp.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC rd = {0};
        rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        rd.Width = s_d3d.pw;
        rd.Height = s_d3d.ph;
        rd.DepthOrArraySize = 1;
        rd.MipLevels = 1;
        rd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;   /* the swap chain's: copied 1:1 */
        rd.SampleDesc.Count = 1;
        rd.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
        D3D12_CLEAR_VALUE cv = {0};
        cv.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        cv.Color[3] = 1.0f;   /* opaque black, what titles clear the display to */
        hr = s_d3d.device->lpVtbl->CreateCommittedResource(
            s_d3d.device, &hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_COMMON, &cv,
            &IID_ID3D12Resource, (void**)&s_d3d.render_targets[i]);
        if (FAILED(hr)) {
            printf("[D3D12] ERROR: display target %u (0x%08lX)\n", i, hr);
            return -1;
        }

        s_d3d.device->lpVtbl->CreateRenderTargetView(
            s_d3d.device, s_d3d.render_targets[i], NULL, rtv_handle);
        rtv_handle.ptr += s_d3d.rtv_descriptor_size;
    }
    if (pp_init() != 0) return -1;

    /* ---------------------------------------------------------------
     * Depth/stencil buffer
     * 24-bit depth + 8-bit stencil (DXGI_FORMAT_D24_UNORM_S8_UINT).
     * One shared depth texture across both frames — RSX games on PS3
     * typically use a single zeta surface.
     * ---------------------------------------------------------------*/
    {
        D3D12_DESCRIPTOR_HEAP_DESC dsv_heap_desc = {0};
        dsv_heap_desc.NumDescriptors = 1;
        dsv_heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
        hr = s_d3d.device->lpVtbl->CreateDescriptorHeap(
            s_d3d.device, &dsv_heap_desc, &IID_ID3D12DescriptorHeap,
            (void**)&s_d3d.dsv_heap);
        if (FAILED(hr)) {
            printf("[D3D12] DSV heap creation failed\n");
            return -1;
        }

        D3D12_HEAP_PROPERTIES heap_props = {0};
        heap_props.Type = D3D12_HEAP_TYPE_DEFAULT;

        D3D12_RESOURCE_DESC depth_desc = {0};
        depth_desc.Dimension          = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        /* Sized to cover the LARGEST render target, not just the window:
         * D3D12 clips rasterization to the smallest bound attachment, and
         * this DSV is shared by every pass -- with a window-sized depth
         * buffer, draws into larger offscreen RTs (wave's 1920x1080 input
         * image) were silently cropped to the window rect. */
        depth_desc.Width              = s_d3d.pw > 2048 ? s_d3d.pw : 2048;
        depth_desc.Height             = s_d3d.ph > 2048 ? s_d3d.ph : 2048;
        depth_desc.DepthOrArraySize   = 1;
        depth_desc.MipLevels          = 1;
        depth_desc.Format             = DXGI_FORMAT_D24_UNORM_S8_UINT;
        depth_desc.SampleDesc.Count   = 1;
        depth_desc.Layout             = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        depth_desc.Flags              = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;

        D3D12_CLEAR_VALUE depth_clear = {0};
        depth_clear.Format                       = DXGI_FORMAT_D24_UNORM_S8_UINT;
        depth_clear.DepthStencil.Depth           = 1.0f;
        depth_clear.DepthStencil.Stencil         = 0;

        hr = s_d3d.device->lpVtbl->CreateCommittedResource(
            s_d3d.device, &heap_props, D3D12_HEAP_FLAG_NONE,
            &depth_desc, D3D12_RESOURCE_STATE_DEPTH_WRITE, &depth_clear,
            &IID_ID3D12Resource, (void**)&s_d3d.depth_buffer);
        if (FAILED(hr)) {
            printf("[D3D12] Depth buffer creation failed (0x%08lX)\n", hr);
            return -1;
        }

        D3D12_DEPTH_STENCIL_VIEW_DESC dsv_desc = {0};
        dsv_desc.Format         = DXGI_FORMAT_D24_UNORM_S8_UINT;
        dsv_desc.ViewDimension  = D3D12_DSV_DIMENSION_TEXTURE2D;

        D3D12_CPU_DESCRIPTOR_HANDLE dsv_handle;
        s_d3d.dsv_heap->lpVtbl->GetCPUDescriptorHandleForHeapStart(s_d3d.dsv_heap, &dsv_handle);
        s_d3d.device->lpVtbl->CreateDepthStencilView(
            s_d3d.device, s_d3d.depth_buffer, &dsv_desc, dsv_handle);

        printf("[D3D12] Depth buffer created (%ux%u D24S8)\n", width, height);
    }

    /* Create command allocators and command list */
    for (u32 i = 0; i < FRAME_COUNT; i++) {
        hr = s_d3d.device->lpVtbl->CreateCommandAllocator(
            s_d3d.device, D3D12_COMMAND_LIST_TYPE_DIRECT,
            &IID_ID3D12CommandAllocator, (void**)&s_d3d.cmd_allocators[i]);
        if (FAILED(hr)) return -1;
    }

    hr = s_d3d.device->lpVtbl->CreateCommandList(
        s_d3d.device, 0, D3D12_COMMAND_LIST_TYPE_DIRECT,
        s_d3d.cmd_allocators[0], NULL,
        &IID_ID3D12GraphicsCommandList, (void**)&s_d3d.cmd_list);
    if (FAILED(hr)) return -1;

    /* Close the command list (it starts in recording state) */
    s_d3d.cmd_list->lpVtbl->Close(s_d3d.cmd_list);

    /* Create fence */
    hr = s_d3d.device->lpVtbl->CreateFence(
        s_d3d.device, 0, D3D12_FENCE_FLAG_NONE,
        &IID_ID3D12Fence, (void**)&s_d3d.fence);
    if (FAILED(hr)) return -1;

    s_d3d.fence_event = CreateEvent(NULL, FALSE, FALSE, NULL);
    memset(s_d3d.fence_values, 0, sizeof(s_d3d.fence_values));

    /* ---------------------------------------------------------------
     * Create root signature with 16 root constants (one mat4 MVP at b0).
     * Visible only to the vertex shader — pixel shader doesn't need it.
     * ---------------------------------------------------------------*/
    {
        /* param 0: mat4 MVP as 16 root constants at b0 (vertex shader).
         * param 1: one SRV (t0) descriptor table for the atlas texture (pixel).
         * static sampler s0: linear clamp. The plain (untextured) PSO simply
         * never references t0/s0, so it ignores them. */
        D3D12_DESCRIPTOR_RANGE srv_range = {0};
        srv_range.RangeType          = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        srv_range.NumDescriptors     = 1;
        srv_range.BaseShaderRegister = 0;   /* t0 */
        srv_range.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

        D3D12_ROOT_PARAMETER root_params[2] = {0};
        root_params[0].ParameterType            = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        root_params[0].Constants.ShaderRegister = 0;   /* b0 */
        root_params[0].Constants.RegisterSpace  = 0;
        root_params[0].Constants.Num32BitValues = 16;  /* mat4 */
        root_params[0].ShaderVisibility         = D3D12_SHADER_VISIBILITY_VERTEX;
        root_params[1].ParameterType            = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        root_params[1].DescriptorTable.NumDescriptorRanges = 1;
        root_params[1].DescriptorTable.pDescriptorRanges   = &srv_range;
        root_params[1].ShaderVisibility         = D3D12_SHADER_VISIBILITY_PIXEL;

        D3D12_STATIC_SAMPLER_DESC samp = {0};
        samp.Filter           = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
        samp.AddressU         = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        samp.AddressV         = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        samp.AddressW         = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        samp.ShaderRegister   = 0;   /* s0 */
        samp.MaxLOD           = D3D12_FLOAT32_MAX;
        samp.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

        D3D12_ROOT_SIGNATURE_DESC rs_desc = {0};
        rs_desc.NumParameters     = 2;
        rs_desc.pParameters       = root_params;
        rs_desc.NumStaticSamplers = 1;
        rs_desc.pStaticSamplers   = &samp;
        rs_desc.Flags         = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT
                              | D3D12_ROOT_SIGNATURE_FLAG_DENY_HULL_SHADER_ROOT_ACCESS
                              | D3D12_ROOT_SIGNATURE_FLAG_DENY_DOMAIN_SHADER_ROOT_ACCESS
                              | D3D12_ROOT_SIGNATURE_FLAG_DENY_GEOMETRY_SHADER_ROOT_ACCESS;

        ID3DBlob* signature_blob = NULL;
        ID3DBlob* error_blob = NULL;
        hr = D3D12SerializeRootSignature(&rs_desc, D3D_ROOT_SIGNATURE_VERSION_1,
                                          &signature_blob, &error_blob);
        if (FAILED(hr)) {
            printf("[D3D12] Root signature serialization failed: %s\n",
                   error_blob ? (const char*)error_blob->lpVtbl->GetBufferPointer(error_blob) : "?");
            if (error_blob) error_blob->lpVtbl->Release(error_blob);
            return -1;
        }

        hr = s_d3d.device->lpVtbl->CreateRootSignature(
            s_d3d.device, 0,
            signature_blob->lpVtbl->GetBufferPointer(signature_blob),
            signature_blob->lpVtbl->GetBufferSize(signature_blob),
            &IID_ID3D12RootSignature, (void**)&s_d3d.root_signature);
        signature_blob->lpVtbl->Release(signature_blob);
        if (FAILED(hr)) {
            printf("[D3D12] Root signature creation failed\n");
            return -1;
        }
    }

    /* ---------------------------------------------------------------
     * Compile shaders and create PSO
     * ---------------------------------------------------------------*/
    {
        /* Basic vertex-colored shader.
         * The MVP matrix arrives via root constants as 4 vec4 columns
         * (PS3/OpenGL column-major convention). We multiply explicitly so
         * we don't depend on HLSL's matrix packing — matches PS3 semantics
         * `gl_Position = MVP * vec4(pos, 1.0)`. */
        static const char vs_hlsl[] =
            "cbuffer cb0 : register(b0) {\n"
            "    float4 mvp_col0;\n"
            "    float4 mvp_col1;\n"
            "    float4 mvp_col2;\n"
            "    float4 mvp_col3;\n"
            "};\n"
            "struct VSInput  { float3 pos : POSITION; float4 col : COLOR; };\n"
            "struct VSOutput { float4 pos : SV_POSITION; float4 col : COLOR; };\n"
            "VSOutput main(VSInput i) {\n"
            "    VSOutput o;\n"
            "    float4 p = float4(i.pos, 1.0);\n"
            "    o.pos = mvp_col0 * p.x + mvp_col1 * p.y + mvp_col2 * p.z + mvp_col3 * p.w;\n"
            "    o.col = i.col;\n"
            "    return o;\n"
            "}\n";
        static const char ps_hlsl[] =
            "struct PSInput { float4 pos : SV_POSITION; float4 col : COLOR; };\n"
            "float4 main(PSInput i) : SV_TARGET { return i.col; }\n";

        ID3DBlob* vs_blob = NULL;
        ID3DBlob* ps_blob = NULL;
        ID3DBlob* err = NULL;

        hr = rsx_compile_own(vs_hlsl, sizeof(vs_hlsl) - 1, "vs_basic", NULL, NULL,
                             "main", "vs_5_0", 0, 0, &vs_blob, &err);
        if (FAILED(hr)) {
            printf("[D3D12] VS compile failed: %s\n",
                   err ? (const char*)err->lpVtbl->GetBufferPointer(err) : "unknown");
            if (err) err->lpVtbl->Release(err);
        }

        hr = rsx_compile_own(ps_hlsl, sizeof(ps_hlsl) - 1, "ps_basic", NULL, NULL,
                             "main", "ps_5_0", 0, 0, &ps_blob, &err);
        if (FAILED(hr)) {
            printf("[D3D12] PS compile failed: %s\n",
                   err ? (const char*)err->lpVtbl->GetBufferPointer(err) : "unknown");
            if (err) err->lpVtbl->Release(err);
        }

        if (vs_blob && ps_blob) {
            D3D12_INPUT_ELEMENT_DESC input_layout[] = {
                {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0,
                 D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
                {"COLOR", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 12,
                 D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
                {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 28,
                 D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
            };

            D3D12_GRAPHICS_PIPELINE_STATE_DESC pso_desc = {0};
            pso_desc.pRootSignature = s_d3d.root_signature;
            pso_desc.VS.pShaderBytecode = vs_blob->lpVtbl->GetBufferPointer(vs_blob);
            pso_desc.VS.BytecodeLength = vs_blob->lpVtbl->GetBufferSize(vs_blob);
            pso_desc.PS.pShaderBytecode = ps_blob->lpVtbl->GetBufferPointer(ps_blob);
            pso_desc.PS.BytecodeLength = ps_blob->lpVtbl->GetBufferSize(ps_blob);
            pso_desc.InputLayout.pInputElementDescs = input_layout;
            pso_desc.InputLayout.NumElements = 3;
            pso_desc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
            pso_desc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
            pso_desc.BlendState.RenderTarget[0].RenderTargetWriteMask =
                D3D12_COLOR_WRITE_ENABLE_ALL;
            pso_desc.SampleMask = UINT_MAX;
            pso_desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
            pso_desc.NumRenderTargets = 1;
            pso_desc.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
            pso_desc.DSVFormat = DXGI_FORMAT_D24_UNORM_S8_UINT;
            /* Depth test enabled, write enabled, LESS func.
             * Games with depth_test_enable=0 in RSX state can still render —
             * LESS just means new-z must be less than existing — but future
             * work should mirror RSX depth state into a PSO cache. */
            pso_desc.DepthStencilState.DepthEnable    = TRUE;
            pso_desc.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
            pso_desc.DepthStencilState.DepthFunc      = D3D12_COMPARISON_FUNC_LESS_EQUAL;
            pso_desc.DepthStencilState.StencilEnable  = FALSE;
            pso_desc.SampleDesc.Count = 1;

            hr = s_d3d.device->lpVtbl->CreateGraphicsPipelineState(
                s_d3d.device, &pso_desc,
                &IID_ID3D12PipelineState, (void**)&s_d3d.pipeline_state);
            if (SUCCEEDED(hr)) {
                s_d3d.pipeline_ready = 1;
                printf("[D3D12] Pipeline state created (triangle class)\n");
            } else {
                printf("[D3D12] PSO TRIANGLE creation failed (0x%08lX)\n", hr);
            }

            /* Line-class PSO — same shader, LINE topology type. */
            pso_desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_LINE;
            hr = s_d3d.device->lpVtbl->CreateGraphicsPipelineState(
                s_d3d.device, &pso_desc,
                &IID_ID3D12PipelineState, (void**)&s_d3d.pipeline_state_lines);
            if (SUCCEEDED(hr)) printf("[D3D12] Pipeline state created (line class)\n");
            else printf("[D3D12] PSO LINE creation failed (0x%08lX)\n", hr);

            /* Point-class PSO. */
            pso_desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_POINT;
            hr = s_d3d.device->lpVtbl->CreateGraphicsPipelineState(
                s_d3d.device, &pso_desc,
                &IID_ID3D12PipelineState, (void**)&s_d3d.pipeline_state_points);
            if (SUCCEEDED(hr)) printf("[D3D12] Pipeline state created (point class)\n");
            else printf("[D3D12] PSO POINT creation failed (0x%08lX)\n", hr);

            vs_blob->lpVtbl->Release(vs_blob);
            ps_blob->lpVtbl->Release(ps_blob);
        }

        /* -----------------------------------------------------------------
         * Textured triangle PSO (dbgfont / 2D atlas quads). Same MVP VS but
         * carrying UV; the PS samples the single-channel atlas as coverage
         * and modulates the vertex color, with straight alpha blending so
         * glyph edges composite over the framebuffer. Depth off (2D overlay).
         * ----------------------------------------------------------------*/
        {
            static const char vs_tex[] =
                "cbuffer cb0 : register(b0){float4 c0;float4 c1;float4 c2;float4 c3;};\n"
                "struct VSIn { float3 pos:POSITION; float4 col:COLOR; float2 uv:TEXCOORD; };\n"
                "struct VSOut{ float4 pos:SV_POSITION; float4 col:COLOR; float2 uv:TEXCOORD; };\n"
                "VSOut main(VSIn i){ VSOut o; float4 p=float4(i.pos,1.0);\n"
                "  o.pos=c0*p.x+c1*p.y+c2*p.z+c3*p.w; o.col=i.col; o.uv=i.uv; return o; }\n";
            static const char ps_tex[] =
                "Texture2D tex : register(t0);\n"
                "SamplerState smp : register(s0);\n"
                "struct PSIn{ float4 pos:SV_POSITION; float4 col:COLOR; float2 uv:TEXCOORD; };\n"
                "float4 main(PSIn i):SV_TARGET{\n"
                /* dbgfont texcoords are compressed ~10x (U) / ~8x (V) vs. the
                 * atlas; scale to recover glyph cells. Exact per-glyph offset
                 * still being calibrated. */
                "  float2 uv2 = float2(i.uv.x*10.0, i.uv.y*8.0 + 0.59);\n"
                "  float cov = tex.Sample(smp, uv2).r;\n"
                "  return float4(i.col.rgb, i.col.a * cov); }\n";

            ID3DBlob *vtb = NULL, *ptb = NULL, *e2 = NULL;
            hr = rsx_compile_own(vs_tex, sizeof(vs_tex) - 1, "vs_tex", NULL, NULL, "main", "vs_5_0", 0, 0, &vtb, &e2);
            if (FAILED(hr)) printf("[D3D12] VS(tex) compile failed: %s\n", e2 ? (const char*)e2->lpVtbl->GetBufferPointer(e2) : "?");
            hr = rsx_compile_own(ps_tex, sizeof(ps_tex) - 1, "ps_tex", NULL, NULL, "main", "ps_5_0", 0, 0, &ptb, &e2);
            if (FAILED(hr)) printf("[D3D12] PS(tex) compile failed: %s\n", e2 ? (const char*)e2->lpVtbl->GetBufferPointer(e2) : "?");

            if (vtb && ptb) {
                D3D12_INPUT_ELEMENT_DESC il[] = {
                    {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0,  D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
                    {"COLOR",    0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
                    {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 28, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
                };
                D3D12_GRAPHICS_PIPELINE_STATE_DESC pd = {0};
                pd.pRootSignature = s_d3d.root_signature;
                pd.VS.pShaderBytecode = vtb->lpVtbl->GetBufferPointer(vtb);
                pd.VS.BytecodeLength  = vtb->lpVtbl->GetBufferSize(vtb);
                pd.PS.pShaderBytecode = ptb->lpVtbl->GetBufferPointer(ptb);
                pd.PS.BytecodeLength  = ptb->lpVtbl->GetBufferSize(ptb);
                pd.InputLayout.pInputElementDescs = il;
                pd.InputLayout.NumElements = 3;
                pd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
                pd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
                pd.BlendState.RenderTarget[0].BlendEnable    = TRUE;
                pd.BlendState.RenderTarget[0].SrcBlend       = D3D12_BLEND_SRC_ALPHA;
                pd.BlendState.RenderTarget[0].DestBlend      = D3D12_BLEND_INV_SRC_ALPHA;
                pd.BlendState.RenderTarget[0].BlendOp        = D3D12_BLEND_OP_ADD;
                pd.BlendState.RenderTarget[0].SrcBlendAlpha  = D3D12_BLEND_ONE;
                pd.BlendState.RenderTarget[0].DestBlendAlpha = D3D12_BLEND_INV_SRC_ALPHA;
                pd.BlendState.RenderTarget[0].BlendOpAlpha   = D3D12_BLEND_OP_ADD;
                pd.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
                pd.SampleMask = UINT_MAX;
                pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
                pd.NumRenderTargets = 1;
                pd.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
                pd.DSVFormat = DXGI_FORMAT_D24_UNORM_S8_UINT;
                pd.DepthStencilState.DepthEnable = FALSE;
                pd.DepthStencilState.StencilEnable = FALSE;
                pd.SampleDesc.Count = 1;
                hr = s_d3d.device->lpVtbl->CreateGraphicsPipelineState(
                    s_d3d.device, &pd, &IID_ID3D12PipelineState, (void**)&s_d3d.pipeline_state_tex);
                if (SUCCEEDED(hr)) printf("[D3D12] Pipeline state created (textured class)\n");
                else printf("[D3D12] PSO TEX creation failed (0x%08lX)\n", hr);
            }
            if (vtb) vtb->lpVtbl->Release(vtb);
            if (ptb) ptb->lpVtbl->Release(ptb);
        }

        /* SRV descriptor heap (shader-visible). Layout: slot 0 = legacy atlas
         * (dbgfont / textured 2D path), slots 1-4 = per-draw VP textures,
         * 5-20 = offscreen render targets (render-to-texture), rest spare.
         * All slots start as null SRVs so any 4-wide table window (the
         * root signature binds t0-t3) is valid on tier-1 hardware. */
        {
            D3D12_DESCRIPTOR_HEAP_DESC hd = {0};
            {
                D3D12_DESCRIPTOR_HEAP_DESC sh = {0};
                sh.Type = D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER;
                sh.NumDescriptors = 2048;
                sh.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
                if (FAILED(s_d3d.device->lpVtbl->CreateDescriptorHeap(
                        s_d3d.device, &sh, &IID_ID3D12DescriptorHeap, (void**)&s_d3d.samp_heap)))
                    printf("[D3D12] sampler heap creation failed%c", 10);
                else {
                    s_d3d.samp_inc = s_d3d.device->lpVtbl->GetDescriptorHandleIncrementSize(
                        s_d3d.device, D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER);
                    vp_sampler_defaults();
                }
            }
            hd.Type  = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
            hd.NumDescriptors = SRV_HEAP_SIZE;
            hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
            hr = s_d3d.device->lpVtbl->CreateDescriptorHeap(
                s_d3d.device, &hd, &IID_ID3D12DescriptorHeap, (void**)&s_d3d.srv_heap);
            if (FAILED(hr)) printf("[D3D12] SRV heap creation failed (0x%08lX)\n", hr);
            else {
                s_d3d.srv_inc = s_d3d.device->lpVtbl->GetDescriptorHandleIncrementSize(
                    s_d3d.device, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
                D3D12_SHADER_RESOURCE_VIEW_DESC nv = {0};
                nv.Format = DXGI_FORMAT_R8_UNORM;
                nv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
                nv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
                nv.Texture2D.MipLevels = 1;
                D3D12_CPU_DESCRIPTOR_HANDLE hh;
                s_d3d.srv_heap->lpVtbl->GetCPUDescriptorHandleForHeapStart(s_d3d.srv_heap, &hh);
                for (int _i = 0; _i < SRV_HEAP_SIZE; _i++) {
                    s_d3d.device->lpVtbl->CreateShaderResourceView(s_d3d.device, NULL, &nv, hh);
                    hh.ptr += s_d3d.srv_inc;
                }
            }
        }

        /* RTV heap for offscreen render targets (CPU-visible only). */
        {
            D3D12_DESCRIPTOR_HEAP_DESC hd = {0};
            hd.Type  = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
            hd.NumDescriptors = MAX_OFF_RTS;
            hr = s_d3d.device->lpVtbl->CreateDescriptorHeap(
                s_d3d.device, &hd, &IID_ID3D12DescriptorHeap, (void**)&s_d3d.rt_rtv_heap);
            if (FAILED(hr)) printf("[D3D12] offscreen RTV heap creation failed (0x%08lX)\n", hr);
        }
    }

    /* ---------------------------------------------------------------
     * Create dynamic vertex buffer (upload heap, 4MB)
     * ---------------------------------------------------------------*/
    {
        D3D12_HEAP_PROPERTIES heap_props = {0};
        heap_props.Type = D3D12_HEAP_TYPE_UPLOAD;

        D3D12_RESOURCE_DESC buf_desc = {0};
        buf_desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        buf_desc.Width = MAX_VERTICES * VERTEX_STRIDE;
        buf_desc.Height = 1;
        buf_desc.DepthOrArraySize = 1;
        buf_desc.MipLevels = 1;
        buf_desc.SampleDesc.Count = 1;
        buf_desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

        hr = s_d3d.device->lpVtbl->CreateCommittedResource(
            s_d3d.device, &heap_props, D3D12_HEAP_FLAG_NONE,
            &buf_desc, D3D12_RESOURCE_STATE_GENERIC_READ, NULL,
            &IID_ID3D12Resource, (void**)&s_d3d.vertex_buffer);
        if (SUCCEEDED(hr)) {
            D3D12_RANGE read_range = {0, 0};
            s_d3d.vertex_buffer->lpVtbl->Map(
                s_d3d.vertex_buffer, 0, &read_range, &s_d3d.vb_mapped);
            s_d3d.vb_view.BufferLocation =
                s_d3d.vertex_buffer->lpVtbl->GetGPUVirtualAddress(s_d3d.vertex_buffer);
            s_d3d.vb_view.StrideInBytes = VERTEX_STRIDE;
            s_d3d.vb_view.SizeInBytes = MAX_VERTICES * VERTEX_STRIDE;
            printf("[D3D12] Vertex buffer created (%u KB)\n",
                   (MAX_VERTICES * VERTEX_STRIDE) / 1024);
        }
    }

    /* ---------------------------------------------------------------
     * Real vertex-program path resources: root signature (CBV b0 for the
     * vp_c[] bank + SRV t0 + static sampler s0), a raw-float4 vertex buffer,
     * and the constant-bank buffer. The PSO itself is built lazily once the
     * game uploads its VP microcode (render_frame).
     * ---------------------------------------------------------------*/
    {
        /* SRV table t0-t15, one register per RSX texture unit; the hardcoded
         * atlas/colour PSs use only t0 and are unaffected. */
        D3D12_DESCRIPTOR_RANGE srv_range = {0};
        srv_range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        srv_range.NumDescriptors = VP_TEX_UNITS;
        srv_range.BaseShaderRegister = 0;
        srv_range.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

        D3D12_ROOT_PARAMETER rp[5 + VP_TEX_UNITS] = {0};
        rp[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;   /* b0 = vp_c bank */
        rp[0].Descriptor.ShaderRegister = 0;
        rp[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;
        rp[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        rp[1].DescriptorTable.NumDescriptorRanges = 1;
        rp[1].DescriptorTable.pDescriptorRanges = &srv_range;
        rp[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        rp[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;   /* b1 = FP texscale */
        rp[2].Descriptor.ShaderRegister = 1;
        rp[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        rp[3].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;   /* t8 = raw vertex bytes (VS) */
        rp[3].Descriptor.ShaderRegister = 8;
        rp[3].Descriptor.RegisterSpace = 0;
        rp[3].ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;
        rp[4].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;   /* b2 = segment header */
        rp[4].Constants.ShaderRegister = 2;
        rp[4].Constants.Num32BitValues = 1;
        rp[4].ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;

        /* s0-s15: one sampler table per unit, filled per draw from the guest's
         * TEXTURE_ADDRESS/FILTER/CONTROL0 (vp_sampler). These were four static
         * samplers -- s0 point/clamp for the dbgfont atlas, s1-s3 linear/wrap --
         * so every unit-0 texture was point-sampled and clamped whatever the
         * title asked for: God of War HD's soldiers wrap their texture atlas
         * (v up to ~2) and drew transparent-black from its clamped bottom row.
         * Units without guest state keep those two defaults. */
        D3D12_DESCRIPTOR_RANGE samp_range[VP_TEX_UNITS] = {0};
        for (int _s = 0; _s < VP_TEX_UNITS; _s++) {
            samp_range[_s].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER;
            samp_range[_s].NumDescriptors = 1;
            samp_range[_s].BaseShaderRegister = (UINT)_s;
            rp[5 + _s].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
            rp[5 + _s].DescriptorTable.NumDescriptorRanges = 1;
            rp[5 + _s].DescriptorTable.pDescriptorRanges = &samp_range[_s];
            rp[5 + _s].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        }

        D3D12_ROOT_SIGNATURE_DESC rd = {0};
        rd.NumParameters = 5 + VP_TEX_UNITS; rd.pParameters = rp;
        rd.NumStaticSamplers = 0; rd.pStaticSamplers = NULL;
        rd.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

        ID3DBlob* sig = NULL; ID3DBlob* err = NULL;
        hr = D3D12SerializeRootSignature(&rd, D3D_ROOT_SIGNATURE_VERSION_1, &sig, &err);
        if (SUCCEEDED(hr)) {
            s_d3d.device->lpVtbl->CreateRootSignature(s_d3d.device, 0,
                sig->lpVtbl->GetBufferPointer(sig), sig->lpVtbl->GetBufferSize(sig),
                &IID_ID3D12RootSignature, (void**)&s_d3d.vp_root_sig);
            sig->lpVtbl->Release(sig);
        } else if (err) { printf("[D3D12] VP root sig: %s\n", (const char*)err->lpVtbl->GetBufferPointer(err)); err->lpVtbl->Release(err); }

        /* raw float4 vertex buffer + constant bank (both UPLOAD, persistently mapped) */
        D3D12_HEAP_PROPERTIES hp = {0}; hp.Type = D3D12_HEAP_TYPE_UPLOAD;
        D3D12_RESOURCE_DESC bd = {0};
        bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; bd.Height = 1;
        bd.DepthOrArraySize = 1; bd.MipLevels = 1; bd.SampleDesc.Count = 1;
        bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        D3D12_RANGE nr = {0,0};

        /* VTX_RESIDENT_MB: a resident part after the two halves (rsx_kept.h), and an eighth of it
         * in indexes after vp_ib's: half as many bytes as the vertexes. With a sixteenth the indexes
         * ran out first, at the temple with 128-192 of the 256 MB of vertexes used (issue #9). */
        { const char* e = getenv("VTX_RESIDENT_MB");
          s_res_vb_size = e ? (u32)atoi(e) << 20 : 0;
          s_res_ib_size = s_res_vb_size / 8u; }
        bd.Width = (u64)MAX_VERTICES * 256 * 2 + s_res_vb_size;  /* generic VP vertex: 16 float4 slots,
                                          * DOUBLE-buffered by frame parity so a
                                          * new frame's upload never overwrites
                                          * vertices the in-flight GPU frame is
                                          * still reading (was: torn/missing
                                          * triangles mixing stale+new verts) */
        /* Failure here leaves vp_vb_mapped NULL and every VP draw silently
         * declines to upload -- the scene just does not render, with no error
         * anywhere. Report the size too: this buffer is
         * MAX_VERTICES * 256 * 2 bytes and grows fast. */
        fprintf(stderr, "[VPVB] per-frame vertex buffer: %llu MB (%u verts, x2 parity)%c",
                (unsigned long long)(bd.Width >> 20), (unsigned)MAX_VERTICES, 10);
        if (SUCCEEDED(s_d3d.device->lpVtbl->CreateCommittedResource(s_d3d.device, &hp,
                D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_GENERIC_READ, NULL,
                &IID_ID3D12Resource, (void**)&s_d3d.vp_vb)))
            s_d3d.vp_vb->lpVtbl->Map(s_d3d.vp_vb, 0, &nr, &s_d3d.vp_vb_mapped);
        else
            fprintf(stderr, "[VPVB] ALLOCATION FAILED -- no VP geometry will render%c", 10);

        bd.Width = (u64)VP_MAX_INDICES * 4 * 2 + (u64)s_res_ib_size * 4;   /* u32 indices, x2 parity */
        if (SUCCEEDED(s_d3d.device->lpVtbl->CreateCommittedResource(s_d3d.device, &hp,
                D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_GENERIC_READ, NULL,
                &IID_ID3D12Resource, (void**)&s_d3d.vp_ib)))
            s_d3d.vp_ib->lpVtbl->Map(s_d3d.vp_ib, 0, &nr, &s_d3d.vp_ib_mapped);
        else
            fprintf(stderr, "[VPIB] allocation failed -- indexed draws are expanded to lists%c", 10);

        bd.Width = (u64)VP_CB_STRIDE * MAX_DRAWS * 2;   /* per-draw constant slots, x2 parity */
        /* A failure here is silent and catastrophic: vp_record_cb returns early,
         * every draw loses its constants, and the whole scene transforms by
         * zeros -- indistinguishable from a broken vertex program. Say so. */
        fprintf(stderr, "[VPCB] per-draw constant buffer: %llu MB (%d draws)%c",
                (unsigned long long)(bd.Width >> 20), MAX_DRAWS, 10);
        if (SUCCEEDED(s_d3d.device->lpVtbl->CreateCommittedResource(s_d3d.device, &hp,
                D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_GENERIC_READ, NULL,
                &IID_ID3D12Resource, (void**)&s_d3d.vp_cb)))
            s_d3d.vp_cb->lpVtbl->Map(s_d3d.vp_cb, 0, &nr, &s_d3d.vp_cb_mapped);

        bd.Width = (u64)FPCB_HALF * 2 + FPCB_GUARD;  /* per-draw FP slots (b1), x2 parity */
        if (SUCCEEDED(s_d3d.device->lpVtbl->CreateCommittedResource(s_d3d.device, &hp,
                D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_GENERIC_READ, NULL,
                &IID_ID3D12Resource, (void**)&s_d3d.vp_fpcb)))
            s_d3d.vp_fpcb->lpVtbl->Map(s_d3d.vp_fpcb, 0, &nr, &s_d3d.vp_fpcb_mapped);

        printf("[D3D12] VP path resources: rootsig=%p vb=%p cb=%p\n",
               (void*)s_d3d.vp_root_sig, (void*)s_d3d.vp_vb, (void*)s_d3d.vp_cb);
    }

    printf("[D3D12] Initialization complete (%ux%u, %u buffers, pipeline=%s)\n",
           width, height, FRAME_COUNT,
           s_d3d.pipeline_ready ? "ready" : "NOT ready");
    return 0;
}

/* ---------------------------------------------------------------------------
 * Frame sync helpers
 * -----------------------------------------------------------------------*/

static void wait_for_gpu_once(void);
/* PERF=1: the frame clock thread's waits for the GPU, by the line that asks. */
static double s_perf_gpuwait;
static struct { int line; u32 n; double s; } s_perf_gw[16];
static void wait_for_gpu_at(int line)
{
    const double w0 = perf_now();
    wait_for_gpu_once();
    const double w = perf_now() - w0;
    s_hl_waits_n++; s_hl_waits_s += w;
    s_perf_gpuwait += w;
    for (int i = 0; i < 16; i++)
        if (s_perf_gw[i].line == line || !s_perf_gw[i].line) {
            s_perf_gw[i].line = line; s_perf_gw[i].n++; s_perf_gw[i].s += w;
            break;
        }
}
#define wait_for_gpu() wait_for_gpu_at(__LINE__)

static u64 s_fence_last;   /* the last value signalled on the queue */
/* The fence's completed value + 1 when a wait for the GPU last ran out, 0 for never: the freeze watch tells a GPU that
 * stopped from a title that stopped (rsx_d3d12_backend_gpu_stuck). */
static volatile u64 s_stuck_fence;

int rsx_d3d12_backend_gpu_stuck(void)
{
    const u64 at = s_stuck_fence;
    return at && s_d3d.fence && s_d3d.fence->lpVtbl->GetCompletedValue(s_d3d.fence) + 1 <= at;
}
/* fence_values[fi] is the next value to signal for the frame index: a wait signals it, and leaves the
 * one after it for the batch it sits in, which move_to_next_frame signals after the batch. The wait
 * used to take the one after and leave it there: the batch then signalled the wait's own value
 * again, already complete, and nothing waited for that batch. The next rf_open could reset its
 * allocator and its texture ring under it, and its ZPASS counts and main-memory surfaces were read
 * as done. D3D12_FENCE_REUSE=1: as before. */
static int fence_reuse(void)
{
    static int v = -1;
    if (v < 0) v = getenv("D3D12_FENCE_REUSE") ? 1 : 0;
    return v;
}
static void wait_for_gpu_once(void)
{
    u32 fi = s_d3d.frame_index;
    const u64 v = fence_reuse() ? ++s_d3d.fence_values[fi] : s_d3d.fence_values[fi]++;
    s_d3d.cmd_queue->lpVtbl->Signal(s_d3d.cmd_queue, s_d3d.fence, v);
    s_fence_last = v;

    if (s_d3d.fence->lpVtbl->GetCompletedValue(s_d3d.fence) < v) {
        s_d3d.fence->lpVtbl->SetEventOnCompletion(s_d3d.fence, v, s_d3d.fence_event);
        /* NEVER wait unbounded here: this runs on the vblank ticker, which also
         * drives the guest's vblank handlers, the FIFO drain, and the fence
         * publication. A device removal (TDR) leaves the D3D12 fence unsignaled
         * forever and an INFINITE wait silently froze the ENTIRE emulation --
         * observed on LBP as a boot that died the moment the 2048x2048
         * offscreen RT was created (last log line), every guest thread parked.
         * Degrade loudly instead: bounded waits + the removal reason, then
         * carry on (subsequent D3D calls fail visibly but the game keeps
         * running headless). */
        for (int tries = 0; tries < 3; tries++) {
            if (WaitForSingleObject(s_d3d.fence_event, 2000) != WAIT_TIMEOUT)
                return;
            HRESULT rr = s_d3d.device->lpVtbl->GetDeviceRemovedReason(s_d3d.device);
            const u64 got = s_d3d.fence->lpVtbl->GetCompletedValue(s_d3d.fence);
            s_stuck_fence = got + 1;
            printf("[D3D12] wait_for_gpu STUCK %ds: want %llu got %llu removed=0x%08lX\n",
                   2 * (tries + 1),
                   (unsigned long long)v,
                   (unsigned long long)got,
                   (long)rr);
            fflush(stdout);
            if (rr != 0 /* S_OK: device still alive, keep waiting a bit */)
                break;
        }
    }
}

/* ---------------------------------------------------------------------------
 * The presentation pass: a display target scaled into the swap chain's buffer
 * -----------------------------------------------------------------------*/
/* pp_srv_heap: the display targets at 0..FRAME_COUNT-1, then pp_tmp, ov_tex
 * and aa_out; then SMAA's tables of three, one per pass (and per display
 * target for the passes that read it). */
#define PP_SLOT_AA_OUT   (FRAME_COUNT + 2)
#define PP_SLOT_AA_E(fi) (8u + 3u * (fi))
#define PP_SLOT_AA_W     16u
#define PP_SLOT_AA_N(fi) (20u + 3u * (fi))
#define PP_SLOT_HUD      32u   /* frame generation's picture without its HUD, */
#define PP_SLOT_HUD_E    33u   /* and SMAA's tables for it */
#define PP_SLOT_HUD_N    36u
#define PP_SLOT_UP(out, fi) (40u + 3u * ((out) * 2u + (fi)))   /* the upscaler's output, the frame, the picture */
#define PP_SRV_SLOTS     52u

/* RTVs for the swap chain's buffers, made again after each resize. */
static void bb_views(void)
{
    D3D12_CPU_DESCRIPTOR_HANDLE h;
    s_d3d.bb_rtv_heap->lpVtbl->GetCPUDescriptorHandleForHeapStart(s_d3d.bb_rtv_heap, &h);
    for (u32 i = 0; i < FRAME_COUNT; i++) {
        s_d3d.device->lpVtbl->CreateRenderTargetView(s_d3d.device, s_d3d.backbuffers[i], NULL, h);
        h.ptr += s_d3d.rtv_descriptor_size;
    }
}

/* A root signature of the presentation's passes: a table of nsrv SRVs
 * (t0..), sixteen root constants at b0, and two static samplers, s0 linear and
 * s1 point, both clamped. */
static int pp_root_create(UINT nsrv, ID3D12RootSignature** out)
{
    HRESULT hr;
    D3D12_DESCRIPTOR_RANGE range = {0};
    range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    range.NumDescriptors = nsrv;
    range.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;
    D3D12_ROOT_PARAMETER rp[2];
    memset(rp, 0, sizeof rp);
    rp[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    rp[0].DescriptorTable.NumDescriptorRanges = 1;
    rp[0].DescriptorTable.pDescriptorRanges = &range;
    rp[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    rp[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    rp[1].Constants.Num32BitValues = 16;
    rp[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    D3D12_STATIC_SAMPLER_DESC ss[2];
    memset(ss, 0, sizeof ss);
    for (int i = 0; i < 2; i++) {
        ss[i].Filter = i ? D3D12_FILTER_MIN_MAG_MIP_POINT : D3D12_FILTER_MIN_MAG_MIP_LINEAR;
        ss[i].AddressU = ss[i].AddressV = ss[i].AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        ss[i].ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
        ss[i].MaxLOD = D3D12_FLOAT32_MAX;
        ss[i].ShaderRegister = (UINT)i;
        ss[i].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    }
    D3D12_ROOT_SIGNATURE_DESC rd = {0};
    rd.NumParameters = 2;
    rd.pParameters = rp;
    rd.NumStaticSamplers = 2;
    rd.pStaticSamplers = ss;
    ID3DBlob *sig = NULL, *err = NULL;
    hr = D3D12SerializeRootSignature(&rd, D3D_ROOT_SIGNATURE_VERSION_1, &sig, &err);
    if (SUCCEEDED(hr))
        hr = s_d3d.device->lpVtbl->CreateRootSignature(s_d3d.device, 0, sig->lpVtbl->GetBufferPointer(sig),
                                                       sig->lpVtbl->GetBufferSize(sig), &IID_ID3D12RootSignature,
                                                       (void**)out);
    if (sig) sig->lpVtbl->Release(sig);
    if (err) err->lpVtbl->Release(err);
    if (FAILED(hr)) { printf("[D3D12] ERROR: presentation root signature (0x%08lX)\n", hr); return -1; }
    return 0;
}

/* EASU's output, at the back buffers' size, for RCAS to read: its RTV and SRV
 * take the slot after the back buffers' and the display targets'. Made with
 * the GPU idle (at init, or in bb_resize). */
static void pp_tmp_create(void)
{
    if (s_d3d.pp_tmp) { s_d3d.pp_tmp->lpVtbl->Release(s_d3d.pp_tmp); s_d3d.pp_tmp = NULL; }
    D3D12_HEAP_PROPERTIES hp = {0};
    hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC rd = {0};
    rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    rd.Width = s_d3d.bb_w;
    rd.Height = s_d3d.bb_h;
    rd.DepthOrArraySize = 1;
    rd.MipLevels = 1;
    rd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    rd.SampleDesc.Count = 1;
    rd.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    if (FAILED(s_d3d.device->lpVtbl->CreateCommittedResource(
            s_d3d.device, &hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_COMMON, NULL,
            &IID_ID3D12Resource, (void**)&s_d3d.pp_tmp))) {
        printf("[D3D12] FSR 1: no %ux%u target; bilinear\n", s_d3d.bb_w, s_d3d.bb_h);
        s_d3d.pp_tmp = NULL;
        s_d3d.pp_mode = 0;
        return;
    }
    D3D12_CPU_DESCRIPTOR_HANDLE h;
    s_d3d.bb_rtv_heap->lpVtbl->GetCPUDescriptorHandleForHeapStart(s_d3d.bb_rtv_heap, &h);
    h.ptr += FRAME_COUNT * s_d3d.rtv_descriptor_size;
    s_d3d.device->lpVtbl->CreateRenderTargetView(s_d3d.device, s_d3d.pp_tmp, NULL, h);
    s_d3d.pp_srv_heap->lpVtbl->GetCPUDescriptorHandleForHeapStart(s_d3d.pp_srv_heap, &h);
    h.ptr += FRAME_COUNT * s_d3d.device->lpVtbl->GetDescriptorHandleIncrementSize(
        s_d3d.device, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    s_d3d.device->lpVtbl->CreateShaderResourceView(s_d3d.device, s_d3d.pp_tmp, NULL, h);
}

/* One triangle over the whole target; the pixel shader samples the display
 * target, or for FSR 1, EASU into pp_tmp and RCAS from it (rsx_fsr1.c). */
static int up_init(D3D12_GRAPHICS_PIPELINE_STATE_DESC pd);

static int pp_init(void)
{
    HRESULT hr;
    D3D12_DESCRIPTOR_HEAP_DESC hd = {0};
    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    hd.NumDescriptors = FRAME_COUNT + 3;   /* the back buffers, pp_tmp, then s_fg.hud_out */
    hr = s_d3d.device->lpVtbl->CreateDescriptorHeap(s_d3d.device, &hd, &IID_ID3D12DescriptorHeap,
                                                   (void**)&s_d3d.bb_rtv_heap);
    if (FAILED(hr)) { printf("[D3D12] ERROR: back buffer RTV heap (0x%08lX)\n", hr); return -1; }
    bb_views();

    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    hd.NumDescriptors = PP_SRV_SLOTS;
    hr = s_d3d.device->lpVtbl->CreateDescriptorHeap(s_d3d.device, &hd, &IID_ID3D12DescriptorHeap,
                                                   (void**)&s_d3d.pp_srv_heap);
    if (FAILED(hr)) { printf("[D3D12] ERROR: presentation SRV heap (0x%08lX)\n", hr); return -1; }
    {
        const UINT inc = s_d3d.device->lpVtbl->GetDescriptorHandleIncrementSize(
            s_d3d.device, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        D3D12_CPU_DESCRIPTOR_HANDLE h;
        s_d3d.pp_srv_heap->lpVtbl->GetCPUDescriptorHandleForHeapStart(s_d3d.pp_srv_heap, &h);
        for (u32 i = 0; i < FRAME_COUNT; i++) {
            s_d3d.device->lpVtbl->CreateShaderResourceView(s_d3d.device, s_d3d.render_targets[i], NULL, h);
            h.ptr += inc;
        }
    }

    if (pp_root_create(1, &s_d3d.pp_root) != 0) return -1;
    ID3DBlob* err = NULL;

    static const char hlsl[] =
        "Texture2D src : register(t0);\n"
        "SamplerState lin : register(s0);\n"
        "SamplerState pnt : register(s1);\n"
        "cbuffer pc : register(b0) { float4 p0; float4 p1; float4 p2; float4 p3; };\n"
        "struct V { float4 pos : SV_POSITION; float2 uv : TEXCOORD0; };\n"
        "V vs(uint id : SV_VertexID) {\n"
        "  V o; float2 t = float2((id << 1) & 2, id & 2);\n"
        "  o.uv = t; o.pos = float4(t * float2(2, -2) + float2(-1, 1), 0, 1); return o; }\n"
        /* p0 = display size, back buffer size; p1.x = 1 for sharp bilinear:
         * flat across most of each display pixel, blended only near its edge,
         * over the width a scaled pixel covers. */
        "float4 ps(V i) : SV_TARGET {\n"
        "  float2 uv = i.uv;\n"
        "  if (p1.x > 0.5) {\n"
        "    float2 t = uv * p0.xy, s = max(p0.zw / p0.xy, 1.0), f = frac(t) - 0.5, r = 0.5 - 0.5 / s;\n"
        "    uv = (floor(t) + (f - clamp(f, -r, r)) * s + 0.5) / p0.xy;\n"
        "  }\n"
        "  return float4(src.SampleLevel(lin, uv, 0).rgb, 1); }\n";
    ID3DBlob *vsb = NULL, *psb = NULL;
    hr = rsx_compile_own(hlsl, sizeof hlsl - 1, "present_vs", NULL, NULL, "vs", "vs_5_0", 0, 0, &vsb, &err);
    if (SUCCEEDED(hr)) {
        if (err) { err->lpVtbl->Release(err); err = NULL; }
        hr = rsx_compile_own(hlsl, sizeof hlsl - 1, "present_ps", NULL, NULL, "ps", "ps_5_0", 0, 0, &psb, &err);
    }
    if (FAILED(hr)) {
        printf("[D3D12] ERROR: presentation shader: %s\n", err ? (const char*)err->lpVtbl->GetBufferPointer(err) : "?");
        if (err) err->lpVtbl->Release(err);
        if (vsb) vsb->lpVtbl->Release(vsb);
        return -1;
    }
    if (err) err->lpVtbl->Release(err);
    D3D12_GRAPHICS_PIPELINE_STATE_DESC pd = {0};
    pd.pRootSignature = s_d3d.pp_root;
    pd.VS.pShaderBytecode = vsb->lpVtbl->GetBufferPointer(vsb);
    pd.VS.BytecodeLength  = vsb->lpVtbl->GetBufferSize(vsb);
    pd.PS.pShaderBytecode = psb->lpVtbl->GetBufferPointer(psb);
    pd.PS.BytecodeLength  = psb->lpVtbl->GetBufferSize(psb);
    pd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    pd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    pd.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    pd.SampleMask = UINT_MAX;
    pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pd.NumRenderTargets = 1;
    pd.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
    pd.SampleDesc.Count = 1;
    hr = s_d3d.device->lpVtbl->CreateGraphicsPipelineState(s_d3d.device, &pd, &IID_ID3D12PipelineState,
                                                           (void**)&s_d3d.pp_pso);
    psb->lpVtbl->Release(psb);
    if (FAILED(hr)) {
        vsb->lpVtbl->Release(vsb);
        printf("[D3D12] ERROR: presentation pipeline (0x%08lX)\n", hr);
        return -1;
    }

    /* graphics.scaling = bilinear | sharp | fsr1, graphics.sharpening =
     * off | low | medium | high | max (FSR 1's RCAS; high is AMD's 0.2). */
    {
        const char* sc = ps3_settings_str("graphics", "scaling", NULL, "bilinear");
        s_d3d.pp_mode = ps3_settings_ieq(sc, "fsr1") || ps3_settings_ieq(sc, "fsr3") ? 2 : ps3_settings_ieq(sc, "sharp") ? 1 : 0;
        if (!s_d3d.pp_mode && !ps3_settings_ieq(sc, "bilinear"))
            printf("[D3D12] graphics.scaling = %s is not bilinear, sharp, fsr1 or fsr3; bilinear\n", sc);
        if (ps3_settings_ieq(sc, "fsr3")) {
            s_up.want = !s_fsr3_refused && rsx_fsr3_available();
            if (!s_up.want)
                printf("[D3D12] graphics.scaling = fsr3: %s; FSR 1%c",
                       s_fsr3_refused ? s_fsr3_refused : "no amd_fidelityfx_loader_dx12.dll beside the exe", 10);
        }
        static const struct { const char* name; float stops; } k[] = {
            { "off", -1.0f }, { "low", 1.0f }, { "medium", 0.5f }, { "high", 0.2f }, { "max", 0.0f } };
        const char* sh = ps3_settings_str("graphics", "sharpening", NULL, "high");
        s_d3d.pp_stops = 0.2f;
        unsigned i = 0;
        for (; i < sizeof k / sizeof k[0]; i++)
            if (ps3_settings_ieq(sh, k[i].name)) { s_d3d.pp_stops = k[i].stops; break; }
        if (i == sizeof k / sizeof k[0])
            printf("[D3D12] graphics.sharpening = %s is not off, low, medium, high or max; high\n", sh);
    }
    if (s_d3d.pp_mode == 2) {
        for (int pass = 0; pass < 2; pass++) {
            if (pass == 1 && s_d3d.pp_stops < 0.0f) break;
            ID3DBlob* fb = (ID3DBlob*)rsx_fsr1_compile(pass);
            if (!fb) break;
            pd.PS.pShaderBytecode = fb->lpVtbl->GetBufferPointer(fb);
            pd.PS.BytecodeLength  = fb->lpVtbl->GetBufferSize(fb);
            hr = s_d3d.device->lpVtbl->CreateGraphicsPipelineState(
                s_d3d.device, &pd, &IID_ID3D12PipelineState,
                (void**)(pass ? &s_d3d.pp_pso_rcas : &s_d3d.pp_pso_easu));
            fb->lpVtbl->Release(fb);
            if (FAILED(hr)) { printf("[D3D12] FSR 1 pipeline %d (0x%08lX)\n", pass, hr); break; }
        }
        if (!s_d3d.pp_pso_easu || (s_d3d.pp_stops >= 0.0f && !s_d3d.pp_pso_rcas)) {
            printf("[D3D12] FSR 1 unavailable; bilinear\n");
            s_d3d.pp_mode = 0;
        } else if (s_d3d.pp_stops >= 0.0f) {
            pp_tmp_create();
        }
    }
    if (s_up.want && up_init(pd) != 0) {
        printf("[D3D12] the upscaler's last pass failed; FSR 1%c", 10);
        s_up.want = 0;
    }
    vsb->lpVtbl->Release(vsb);
    ov_init(pd);
    /* graphics.antialiasing = off (the default) | smaa */
    {
        const char* aa = ps3_settings_str("graphics", "antialiasing", NULL, "off");
        if (ps3_settings_ieq(aa, "smaa")) {
            s_d3d.aa_on = aa_init(pd) == 0;
            if (!s_d3d.aa_on) printf("[D3D12] SMAA unavailable; off\n");
        } else if (!ps3_settings_ieq(aa, "off")) {
            printf("[D3D12] graphics.antialiasing = %s is not off or smaa; off\n", aa);
        }
    }
    static const char* const mode_name[] = { "bilinear", "sharp bilinear", "FSR 1" };
    printf("[D3D12] presentation pass: %ux%u -> %ux%u, %s%s\n", s_d3d.pw, s_d3d.ph,
           s_d3d.bb_w, s_d3d.bb_h, mode_name[s_d3d.pp_mode], s_d3d.aa_on ? ", SMAA 1x" : "");
    return 0;
}

/* The swap chain to the view's size, once its window has changed. Called
 * after a Present, so no batch of the frame is cut in two. */
static void bb_resize(void)
{
    u32 w = (u32)s_view_w, h = (u32)s_view_h;
    if (!w || !h) return;
    /* FSR 3 takes no render larger than the display: a view made smaller than the render
     * keeps a swap chain that big, and the flip stretches it down. */
    if (RS_ON && (s_fg_mode || s_up.want)) {
        if (w < s_d3d.pw) w = s_d3d.pw;
        if (h < s_d3d.ph) h = s_d3d.ph;
    }
    if (w == s_d3d.bb_w && h == s_d3d.bb_h) return;
    wait_for_gpu();
    for (u32 i = 0; i < FRAME_COUNT; i++)
        if (s_d3d.backbuffers[i]) { s_d3d.backbuffers[i]->lpVtbl->Release(s_d3d.backbuffers[i]); s_d3d.backbuffers[i] = NULL; }
    if (s_up.ready) {
        rsx_fsr3_up_destroy();
        s_up.ready = 0;
        s_up.jx = s_up.jy = s_up.cx = s_up.cy = 0.0f;
    }
    if (s_fg_mode || s_up.want) {        /* its size is the window's: made again at the next frame */
        if (s_fg_mode) rsx_fsr3_fg_destroy();
        s_fg_ready = 0;
        for (int i = 0; i < 2; i++)
            if (s_fg.hud_out[i]) { s_fg.hud_out[i]->lpVtbl->Release(s_fg.hud_out[i]); s_fg.hud_out[i] = NULL; }
    }
    HRESULT hr = s_d3d.swap_chain->lpVtbl->ResizeBuffers(s_d3d.swap_chain, FRAME_COUNT, w, h,
                                                         DXGI_FORMAT_R8G8B8A8_UNORM,
                                                         s_d3d.tearing ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0);
    if (SUCCEEDED(hr)) { s_d3d.bb_w = w; s_d3d.bb_h = h; }
    else printf("[D3D12] swap chain resize to %ux%u failed (0x%08lX)\n", w, h, hr);
    if (s_d3d.pp_tmp) pp_tmp_create();
    for (u32 i = 0; i < FRAME_COUNT; i++)
        s_d3d.swap_chain->lpVtbl->GetBuffer(s_d3d.swap_chain, i, &IID_ID3D12Resource, (void**)&s_d3d.backbuffers[i]);
    bb_views();
    /* The buffer index may have moved. The GPU is idle: every fence value is
     * complete, and the new frame signals one past the highest. */
    const u64 done = s_d3d.fence_values[s_d3d.frame_index];
    s_d3d.frame_index = s_d3d.swap_chain->lpVtbl->GetCurrentBackBufferIndex(s_d3d.swap_chain);
    for (u32 i = 0; i < FRAME_COUNT; i++) s_d3d.fence_values[i] = done;
    s_d3d.fence_values[s_d3d.frame_index] = done + 1;
    if (SUCCEEDED(hr)) printf("[D3D12] swap chain %ux%u\n", w, h);
}

/* One pass of the presentation: src (an SRV slot of pp_srv_heap) drawn with
 * pso into rtv, full size, with the given root constants. */
static void pp_draw(ID3D12PipelineState* pso, u32 src, D3D12_CPU_DESCRIPTOR_HANDLE rtv, const u32 c[16])
{
    ID3D12GraphicsCommandList* cl = s_d3d.cmd_list;
    D3D12_GPU_DESCRIPTOR_HANDLE srv;
    s_d3d.pp_srv_heap->lpVtbl->GetGPUDescriptorHandleForHeapStart(s_d3d.pp_srv_heap, &srv);
    srv.ptr += src * s_d3d.device->lpVtbl->GetDescriptorHandleIncrementSize(
        s_d3d.device, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    D3D12_VIEWPORT vp = { 0.0f, 0.0f, (float)s_d3d.bb_w, (float)s_d3d.bb_h, 0.0f, 1.0f };
    D3D12_RECT sc = { 0, 0, (LONG)s_d3d.bb_w, (LONG)s_d3d.bb_h };
    cl->lpVtbl->SetGraphicsRootSignature(cl, s_d3d.pp_root);
    cl->lpVtbl->SetDescriptorHeaps(cl, 1, &s_d3d.pp_srv_heap);
    cl->lpVtbl->SetGraphicsRootDescriptorTable(cl, 0, srv);
    cl->lpVtbl->SetGraphicsRoot32BitConstants(cl, 1, 16, c, 0);
    cl->lpVtbl->SetPipelineState(cl, pso);
    cl->lpVtbl->RSSetViewports(cl, 1, &vp);
    cl->lpVtbl->RSSetScissorRects(cl, 1, &sc);
    cl->lpVtbl->OMSetRenderTargets(cl, 1, &rtv, FALSE, NULL);
    cl->lpVtbl->IASetPrimitiveTopology(cl, D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    cl->lpVtbl->DrawInstanced(cl, 3, 1, 0, 0);
}

static void pp_barrier(ID3D12Resource* r, D3D12_RESOURCE_STATES from, D3D12_RESOURCE_STATES to)
{
    D3D12_RESOURCE_BARRIER bar;
    memset(&bar, 0, sizeof bar);
    bar.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    bar.Transition.pResource = r;
    bar.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    bar.Transition.StateBefore = from;
    bar.Transition.StateAfter = to;
    s_d3d.cmd_list->lpVtbl->ResourceBarrier(s_d3d.cmd_list, 1, &bar);
}

/* ---------------------------------------------------------------------------
 * The overlay: the frame rate (display.show_fps = overlay) and short messages
 * ("Borderless fullscreen", "Screenshot saved"), drawn over the scaled picture
 * at the window's own resolution. The text is drawn by GDI into a small
 * bitmap, uploaded only when it changes (once a second for the frame rate),
 * and each item is one quad of it, white on a dark box.
 * -----------------------------------------------------------------------*/
#define OV_W   640
#define OV_ROWS_H 96   /* the two text rows: the frame rate (unused with the panel), the message */
#define OV_ROW 48      /* one item a row */
#define OVS_W  640     /* the stats panel's region, below the rows */
#define OVS_H  736
#define OVI_H  96      /* the shader build indicator's band, below the panel's region */
#define OV_H   (OV_ROWS_H + OVS_H + OVI_H)

static const char* volatile s_toast_msg;   /* a string literal, or NULL */
static volatile ULONGLONG   s_toast_until;
static char s_ov_fps[32];                  /* the presenting thread's */

/* A message for two seconds; any thread. */
static void ov_toast(const char* msg)
{
    s_toast_until = GetTickCount64() + 2000;
    InterlockedExchangePointer((void* volatile*)&s_toast_msg, (void*)msg);
}

/* display.show_fps: 0 off, 1 the title bar (the default), 2 overlay. Read once. */
static int d3d12_show_fps(void)
{
    static int v = -1;
    if (v < 0) {
        const char* s = ps3_settings_str("display", "show_fps", NULL, "title");
        v = ps3_settings_ieq(s, "off") ? 0 : ps3_settings_ieq(s, "overlay") ? 2 : 1;
    }
    return v;
}

/* The stats panel (display.show_fps = overlay), top right: how busy the GPU, the CPU and the guest's
 * processors are, video memory and RAM, the frame rate and frame times, a graph of the last frames'
 * times, the GPU and CPU, how the picture is made, and a line of the game's own.
 * - CPU: the whole machine.
 * - PPU, SPU, RSX: the host threads the runtime names "ppu main"; "spurs ..."
 *   and "spu ..." (six of them make 100%, as the PS3's six SPUs); "frame
 *   clock" (the FIFO walk, the recording and the present). A thread of its own
 *   samples their CPU times every half second.
 * - GPU: the frames' GPU time (their timestamps) over their wall time.
 * Text and graph are drawn by GDI a tenth of a second at most, so the
 * presenting thread spends about 2 ms a second on it. */
#define OVS_HIST 96
static float s_ovs_ft[OVS_HIST];        /* the last presents' frame times, ms, a ring */
static int s_ovs_ft_i;                  /* its next slot */
static volatile LONG s_ovs_pct[4];      /* CPU, PPU, SPU, RSX: percent x10, from the sampler */
static double s_ovs_fps, s_ovs_ms, s_ovs_gpu;   /* the presenting thread's, every half second */
static double s_ovs_gpu_ms;             /* the GPU time of the frames read since */
static volatile LONG s_ovs_vram_mb, s_ovs_ram_mb, s_ovs_ram_total_mb;   /* from the sampler */
static int (*s_ovs_line)(char* out, int max);   /* the game's line at the panel's foot */

void rsx_d3d12_backend_overlay_line(int (*fn)(char* out, int max)) { s_ovs_line = fn; }
static int ovs_on(void) { return d3d12_show_fps() == 2; }

static ULONGLONG ovs_ft64(FILETIME f) { return ((ULONGLONG)f.dwHighDateTime << 32) | f.dwLowDateTime; }

/* Which part a thread's name says it does: 1 PPU, 2 SPU, 3 RSX, 0 the rest. */
static int ovs_group(HANDLE h)
{
    typedef HRESULT (WINAPI *get_desc_fn)(HANDLE, PWSTR*);
    static get_desc_fn get_desc = (get_desc_fn)(void*)1;
    if (get_desc == (get_desc_fn)(void*)1)
        get_desc = (get_desc_fn)(void*)GetProcAddress(GetModuleHandleA("kernel32.dll"), "GetThreadDescription");
    PWSTR w = NULL;
    char n[24] = { 0 };
    if (get_desc && SUCCEEDED(get_desc(h, &w)) && w)
        for (int i = 0; w[i] && i < 23; i++) n[i] = (char)(w[i] < 128 ? w[i] : '?');
    if (w) LocalFree(w);
    if (!strcmp(n, "ppu main")) return 1;
    if (!strncmp(n, "spurs", 5) || !strncmp(n, "spu ", 4)) return 2;
    if (!strcmp(n, "frame clock")) return 3;
    if (!strcmp(n, "rsx render")) return 4;
    return 0;
}

/* The PPU, SPU and RSX threads, found by a snapshot of the system's threads every
 * OVS_FIND ticks: on a desktop with a browser open that snapshot holds thousands of
 * threads, and taken every half second it cost ~16% of a core. Their handles stay open
 * and each half second reads their times; the machine's total is GetSystemTimes. */
#define OVS_FIND 10
static DWORD WINAPI ovs_sampler(LPVOID arg)
{
    (void)arg;
    enum { MAXT = 128 };
    static DWORD th_tid[MAXT];
    static HANDLE th_h[MAXT];
    static ULONGLONG th_cpu[MAXT];
    static signed char th_grp[MAXT];
    int n = 0, tick = 0;
    const DWORD pid = GetCurrentProcessId();
    FILETIME c, e, k, u;
    ULONGLONG wall0 = GetTickCount64();
    FILETIME si0, sk0, su0;
    GetSystemTimes(&si0, &sk0, &su0);
    for (;; tick++) {
        Sleep(500);
        ULONGLONG d[5] = { 0, 0, 0, 0, 0 };   /* by ovs_group: 4 is the render thread */
        /* the threads already known: their time since the last tick; one gone is dropped */
        for (int j = 0; j < n; ) {
            if (GetThreadTimes(th_h[j], &c, &e, &k, &u) &&
                WaitForSingleObject(th_h[j], 0) == WAIT_TIMEOUT) {
                const ULONGLONG t = ovs_ft64(k) + ovs_ft64(u);
                d[th_grp[j]] += t - th_cpu[j];
                th_cpu[j] = t;
                j++;
            } else {
                CloseHandle(th_h[j]);
                n--;
                th_tid[j] = th_tid[n]; th_h[j] = th_h[n]; th_cpu[j] = th_cpu[n]; th_grp[j] = th_grp[n];
            }
        }
        /* new ones, now and then: kept if their name says PPU main, SPU or RSX */
        if (tick % OVS_FIND == 0) {
            HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
            if (snap != INVALID_HANDLE_VALUE) {
                THREADENTRY32 te;
                te.dwSize = sizeof te;
                for (BOOL ok = Thread32First(snap, &te); ok && n < MAXT; ok = Thread32Next(snap, &te)) {
                    if (te.th32OwnerProcessID != pid) continue;
                    int j = 0;
                    while (j < n && th_tid[j] != te.th32ThreadID) j++;
                    if (j < n) continue;
                    HANDLE h = OpenThread(THREAD_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, FALSE, te.th32ThreadID);
                    if (!h) continue;
                    const int g = ovs_group(h);
                    if (g && GetThreadTimes(h, &c, &e, &k, &u)) {
                        th_tid[n] = te.th32ThreadID; th_h[n] = h; th_grp[n] = (signed char)g;
                        th_cpu[n] = ovs_ft64(k) + ovs_ft64(u);   /* counted from the next tick */
                        n++;
                    } else {
                        CloseHandle(h);
                    }
                }
                CloseHandle(snap);
            }
        }
        FILETIME si1, sk1, su1;
        double machine = 0.0;
        if (GetSystemTimes(&si1, &sk1, &su1)) {
            const ULONGLONG idle = ovs_ft64(si1) - ovs_ft64(si0);
            const ULONGLONG all = ovs_ft64(sk1) - ovs_ft64(sk0) + ovs_ft64(su1) - ovs_ft64(su0);   /* kernel time holds the idle */
            if (all > idle) machine = (double)(all - idle) * 100.0 / (double)all;
            si0 = si1; sk0 = sk1; su0 = su1;
        }
        const ULONGLONG wall = GetTickCount64();
        const double win = (double)(wall - wall0) * 1e4;   /* 100 ns units */
        if (win > 0) {
            const double pct[4] = { machine, (double)d[1] * 100.0 / win, (double)d[2] * 100.0 / (win * 6.0),
                                    (double)(d[3] > d[4] ? d[3] : d[4]) * 100.0 / win };   /* the busier RSX thread */
            for (int i = 0; i < 4; i++)
                InterlockedExchange(&s_ovs_pct[i], (LONG)((pct[i] < 0 ? 0 : pct[i] > 100 ? 100 : pct[i]) * 10.0 + 0.5));
        }
        wall0 = wall;
        MEMORYSTATUSEX ms;
        ms.dwLength = sizeof ms;
        if (GlobalMemoryStatusEx(&ms)) {
            InterlockedExchange(&s_ovs_ram_mb, (LONG)((ms.ullTotalPhys - ms.ullAvailPhys) >> 20));
            InterlockedExchange(&s_ovs_ram_total_mb, (LONG)(ms.ullTotalPhys >> 20));
        }
        if (s_adapter3) {
            DXGI_QUERY_VIDEO_MEMORY_INFO vm;
            if (SUCCEEDED(s_adapter3->lpVtbl->QueryVideoMemoryInfo(s_adapter3, 0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &vm)))
                InterlockedExchange(&s_ovs_vram_mb, (LONG)(vm.CurrentUsage >> 20));
        }
    }
    return 0;
}

/* A present: its frame time into the graph, and every half second the frame
 * rate, the mean frame time and the GPU's share. */
static void ovs_present(void)
{
    static double t_last, t_win;
    static int frames;
    const double now = perf_now();
    if (t_last > 0.0) {
        s_ovs_ft[s_ovs_ft_i] = (float)((now - t_last) * 1000.0);
        s_ovs_ft_i = (s_ovs_ft_i + 1) % OVS_HIST;
        frames++;
    } else {
        t_win = now;
    }
    t_last = now;
    const double win_ms = (now - t_win) * 1000.0;
    if (win_ms >= 500.0 && frames) {
        s_ovs_fps = frames * 1000.0 / win_ms;
        s_ovs_ms = win_ms / frames;
        s_ovs_gpu = s_ovs_gpu_ms * 100.0 / win_ms;
        if (s_ovs_gpu > 100.0) s_ovs_gpu = 100.0;
        s_ovs_gpu_ms = 0.0;
        frames = 0;
        t_win = now;
    }
}

/* The two items into a premultiplied RGBA image, OV_W wide, OV_H tall, at
 * dst: white text with the font's antialiasing over a box of 55% black. The
 * widths drawn come back in w[]. */
static void ov_render(unsigned char* dst, const char* const item[2], u32 font_px, u32 w[2])
{
    static HDC dc;
    static u32* bits;
    static HFONT font;
    static u32 font_size;
    if (!dc) {
        BITMAPINFO bi;
        memset(&bi, 0, sizeof bi);
        bi.bmiHeader.biSize = sizeof bi.bmiHeader;
        bi.bmiHeader.biWidth = OV_W;
        bi.bmiHeader.biHeight = -OV_ROWS_H;   /* top-down */
        bi.bmiHeader.biPlanes = 1;
        bi.bmiHeader.biBitCount = 32;
        dc = CreateCompatibleDC(NULL);
        HBITMAP bmp = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, (void**)&bits, NULL, 0);
        if (!dc || !bmp || !bits) { w[0] = w[1] = 0; return; }
        SelectObject(dc, bmp);
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, RGB(255, 255, 255));
    }
    if (font_size != font_px) {
        if (font) DeleteObject(font);
        font = CreateFontA(-(int)font_px, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                           OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY,
                           DEFAULT_PITCH | FF_SWISS, "Segoe UI");
        SelectObject(dc, font);
        font_size = font_px;
    }
    memset(bits, 0, (size_t)OV_W * OV_ROWS_H * 4);
    const int pad = (int)font_px / 2;
    for (int r = 0; r < 2; r++) {
        w[r] = 0;
        if (!item[r] || !item[r][0]) continue;
        SIZE sz;
        const int len = (int)strlen(item[r]);
        GetTextExtentPoint32A(dc, item[r], len, &sz);
        w[r] = (u32)(sz.cx + 2 * pad) < OV_W ? (u32)(sz.cx + 2 * pad) : OV_W;
        TextOutA(dc, pad, r * OV_ROW + (OV_ROW - sz.cy) / 2, item[r], len);
    }
    GdiFlush();
    for (u32 y = 0; y < OV_ROWS_H; y++) {
        const u32 r = y / OV_ROW;
        for (u32 x = 0; x < OV_W; x++) {
            unsigned char* o = dst + ((size_t)y * OV_W + x) * 4;
            if (x >= w[r]) { o[0] = o[1] = o[2] = o[3] = 0; continue; }
            const u32 p = bits[(size_t)y * OV_W + x];
            u32 cov = p & 0xFF;
            if (((p >> 8) & 0xFF) > cov) cov = (p >> 8) & 0xFF;
            if (((p >> 16) & 0xFF) > cov) cov = (p >> 16) & 0xFF;
            o[0] = o[1] = o[2] = (unsigned char)cov;
            o[3] = (unsigned char)(cov + (255 - cov) * 140 / 255);
        }
    }
}

/* The stats panel into premultiplied RGBA at dst (rows OV_W * 4 apart), at most OVS_W x OVS_H; its size comes back in
 * *pw, *ph. GDI draws the text white at twice the size, for its coverage, which keeps the letters evenly spaced at small
 * sizes; each piece is then coloured by the box it is in, over a rounded box of dark glass that is worked out once. The
 * text changes four times a second and the graph every call. A thread of its own paints it, into two images in turn; the
 * presenting thread only copies the last one finished. Painted there it took 3 ms a call, up to 11. */
typedef struct { int x0, y0, x1, y1; u32 rgb; } OvsBox;
static CRITICAL_SECTION s_ovs_lock;     /* over the next three */
static const u32* s_ovs_front;          /* the last finished image, OVS_W wide */
static int s_ovs_fw, s_ovs_fh;          /* its size */
static volatile LONG s_ovs_font;        /* the size of text the presenting thread wants */
#define OVS_ROWS 6
#define OVS_LINES 5

static float ovs_rbox(float px, float py, float x0, float y0, float x1, float y1, float rad)
{
    const float cx = px < x0 + rad ? x0 + rad : (px > x1 - rad ? x1 - rad : px);
    const float cy = py < y0 + rad ? y0 + rad : (py > y1 - rad ? y1 - rad : py);
    const float a = rad + 0.5f - sqrtf((px - cx) * (px - cx) + (py - cy) * (py - cy));
    return a < 0.0f ? 0.0f : a > 1.0f ? 1.0f : a;
}

/* One string in the double-size bitmap; x, y, h and max_x are the panel's own pixels. */
static void ovs_put(HDC dc, int x, int y, int h, int max_x, const char* s)
{
    const int n = (int)strlen(s);
    SIZE z;
    RECT rc;
    if (!n) return;
    GetTextExtentPoint32A(dc, s, n, &z);
    rc.left = x * 2; rc.top = y * 2; rc.right = max_x * 2; rc.bottom = (y + h) * 2;
    ExtTextOutA(dc, x * 2, y * 2 + (h * 2 - z.cy) / 2, ETO_CLIPPED, &rc, s, n, NULL);
}

/* How wide a string is in the panel's own pixels. */
static int ovs_width(HDC dc, const char* s)
{
    SIZE z;
    GetTextExtentPoint32A(dc, s, (int)strlen(s), &z);
    return z.cx / 2 + 1;
}

/* A name from the hardware without its trademarks and what follows the model. */
static void ovs_tidy(char* s)
{
    static const char* const marks[] = { "(R)", "(TM)", "(r)", "(tm)" };
    for (int i = 0; i < 4; i++) {
        const size_t n = strlen(marks[i]);
        for (char* p; (p = strstr(s, marks[i])) != NULL; ) memmove(p, p + n, strlen(p + n) + 1);
    }
    char* p = strstr(s, " with ");
    if (p) *p = 0;
    p = strstr(s, " CPU @");
    if (p) *p = 0;
    p = strstr(s, "-Core Processor");
    if (p) {
        while (p > s && p[-1] != ' ') p--;
        *p = 0;
    }
    for (size_t n = strlen(s); n && s[n - 1] == ' '; n--) s[n - 1] = 0;
}

static const char* ovs_cpu_name(void)
{
    static char name[49];
    if (!name[0]) {
        int r[4];
        __cpuid(r, (int)0x80000000);
        if ((unsigned)r[0] >= 0x80000004u) {
            for (int i = 0; i < 3; i++) {
                __cpuid(r, (int)(0x80000002u + (unsigned)i));
                memcpy(name + i * 16, r, 16);
            }
            name[48] = 0;
            const char* p = name;
            while (*p == ' ') p++;
            memmove(name, p, strlen(p) + 1);
            ovs_tidy(name);
        }
    }
    return name;
}

/* The title's own MLAA runs unless its patch turns it off. */
static int ovs_mlaa(void)
{
    static int v = -1;
    if (v < 0) v = !ps3_settings_bool("patches", "disable_mlaa", NULL, 1);
    return v;
}

/* The panel's lines under the graph, but for the game's own. */
static void ovs_lines(char line[OVS_LINES][96])
{
    snprintf(line[0], sizeof line[0], "%s", s_ovs_gpu_name);
    ovs_tidy(line[0]);
    snprintf(line[1], sizeof line[1], "%s", ovs_cpu_name());
    snprintf(line[2], sizeof line[2], "Upscaler: %s%s | Anti-aliasing: %s",
             s_up.want ? "FSR 3.1" : s_d3d.pp_mode == 2 ? "FSR 1" : s_d3d.pp_mode == 1 ? "Sharp" : "Off",
             s_fg_mode == 2 ? " + FG" : "", s_up.want ? "FSR" : s_d3d.aa_on ? "SMAA" : ovs_mlaa() ? "MLAA" : "Off");
    snprintf(line[3], sizeof line[3], "%u x %u -> %u x %u", s_d3d.pw, s_d3d.ph, s_d3d.bb_w, s_d3d.bb_h);
}

static void ov_render_panel(u32 font_px)
{
    static HDC dc;
    static u32* bits, *bg, *pair[2];   /* GDI's text, double size; the empty panel; the panel's two images (RGBA, premultiplied) */
    static int back;
    static unsigned char* cov, *gcov;  /* the text's coverage; the graph's, over its own box */
    static HFONT font;
    static u32 f_size;
    static int W, H, pad, top, rh, lh, ly, gx0, gy0, gx1, gy1, x1, x2, x3, nlines;
    static ULONGLONG text_at;
    static OvsBox box[OVS_ROWS + OVS_LINES];
    static int nb;
    if (!dc) {
        BITMAPINFO bi;
        memset(&bi, 0, sizeof bi);
        bi.bmiHeader.biSize = sizeof bi.bmiHeader;
        bi.bmiHeader.biWidth = OVS_W * 2;
        bi.bmiHeader.biHeight = -(OVS_H * 2);   /* top-down */
        bi.bmiHeader.biPlanes = 1;
        bi.bmiHeader.biBitCount = 32;
        dc = CreateCompatibleDC(NULL);
        HBITMAP bmp = dc ? CreateDIBSection(dc, &bi, DIB_RGB_COLORS, (void**)&bits, NULL, 0) : NULL;
        bg = (u32*)malloc((size_t)OVS_W * OVS_H * 4);
        pair[0] = (u32*)malloc((size_t)OVS_W * OVS_H * 4);
        pair[1] = (u32*)malloc((size_t)OVS_W * OVS_H * 4);
        cov = (unsigned char*)malloc((size_t)OVS_W * OVS_H);
        gcov = (unsigned char*)malloc((size_t)OVS_W * OVS_H);
        if (!bmp || !bits || !bg || !pair[0] || !pair[1] || !cov || !gcov) { if (dc) DeleteDC(dc); dc = NULL; return; }
        SelectObject(dc, bmp);
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, RGB(255, 255, 255));
    }
    const int F = (int)font_px;
    if (f_size != font_px) {
        SelectObject(dc, GetStockObject(SYSTEM_FONT));
        if (font) DeleteObject(font);
        font = CreateFontA(-F * 2, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                           CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY, DEFAULT_PITCH | FF_SWISS, "Segoe UI");
        SelectObject(dc, font);
        nlines = s_ovs_line ? OVS_LINES : OVS_LINES - 1;
        pad = F * 3 / 2;
        top = F * 4 / 3;
        rh = F * 3 / 2;
        lh = F * 2;
        x1 = pad + F * 33 / 10;
        x2 = x1 + F * 39 / 10;
        x3 = x2 + F * 5;
        {
            /* as wide as what it holds: the longest line, or the row with three values */
            char line[OVS_LINES][96];
            ovs_lines(line);
            snprintf(line[4], sizeof line[4], "X -9999.9   Y -9999.9   Z -9999.9");
            int inner = x3 - pad + ovs_width(dc, "RSX 100%");
            for (int l = 0; l < nlines; l++)
                if (ovs_width(dc, line[l]) > inner) inner = ovs_width(dc, line[l]);
            W = pad + inner + pad;
        }
        gx0 = pad;
        gx1 = W - pad;
        gy0 = top + OVS_ROWS * rh + F / 2;
        gy1 = gy0 + F * 5 / 2;
        ly = gy1 + F * 3 / 4;
        H = ly + nlines * lh + F * 2 / 3;
        if (W > OVS_W) W = OVS_W;
        if (H > OVS_H) H = OVS_H;
        const float glass[3] = { 18.0f, 24.0f, 24.0f }, well[3] = { 14.0f, 22.0f, 16.0f };
        const float rad = (float)F * 0.8f, wrad = (float)F * 0.4f;
        for (int y = 0; y < H; y++) {
            for (int x = 0; x < W; x++) {
                const float px = (float)x + 0.5f, py = (float)y + 0.5f;
                const float a0 = ovs_rbox(px, py, 0.0f, 0.0f, (float)W, (float)H, rad) * 0.72f;
                const float a1 = ovs_rbox(px, py, (float)gx0, (float)gy0, (float)gx1, (float)gy1, wrad) * 0.62f;
                const float a = a1 + a0 * (1.0f - a1);
                u32 p = (u32)(a * 255.0f + 0.5f) << 24;
                for (int ch = 0; ch < 3; ch++)
                    p |= (u32)(well[ch] * a1 + glass[ch] * a0 * (1.0f - a1) + 0.5f) << (8 * ch);
                bg[(size_t)y * OVS_W + (size_t)x] = p;
            }
        }
        f_size = font_px;
        text_at = 0;
    }

    const ULONGLONG now = GetTickCount64();
    const int full = now - text_at >= 250;
    if (full) {
        text_at = now;
        memset(bits, 0, (size_t)OVS_W * 2 * H * 2 * 4);
        static const char* const lab[OVS_ROWS] = { "GPU", "VRAM", "CPU", "RAM", "PPU", "FPS" };
        static const u32 col[OVS_ROWS] = { 0xA4D65A, 0xA4D65A, 0x42AEE6, 0xD877C4, 0xEBAA3C, 0xE6EAE6 };
        char v[OVS_ROWS][3][24];
        memset(v, 0, sizeof v);
        snprintf(v[0][0], sizeof v[0][0], "%.0f%%", s_ovs_gpu);
        snprintf(v[1][0], sizeof v[1][0], "%.2f GB", (double)s_ovs_vram_mb / 1024.0);
        if (s_vram_mb) snprintf(v[1][1], sizeof v[1][1], "/ %.2f GB", (double)s_vram_mb / 1024.0);
        snprintf(v[2][0], sizeof v[2][0], "%ld%%", (long)((s_ovs_pct[0] + 5) / 10));
        snprintf(v[3][0], sizeof v[3][0], "%.1f GB", (double)s_ovs_ram_mb / 1024.0);
        snprintf(v[3][1], sizeof v[3][1], "/ %.1f GB", (double)s_ovs_ram_total_mb / 1024.0);
        snprintf(v[4][0], sizeof v[4][0], "%ld%%", (long)((s_ovs_pct[1] + 5) / 10));
        snprintf(v[4][1], sizeof v[4][1], "SPU %ld%%", (long)((s_ovs_pct[2] + 5) / 10));
        snprintf(v[4][2], sizeof v[4][2], "RSX %ld%%", (long)((s_ovs_pct[3] + 5) / 10));
        snprintf(v[5][0], sizeof v[5][0], "%.0f", s_ovs_fps);
        snprintf(v[5][1], sizeof v[5][1], "%.1f ms", s_ovs_ms);
        nb = 0;
        for (int r = 0; r < OVS_ROWS; r++) {
            const int y = top + r * rh;
            ovs_put(dc, pad, y, rh, x1, lab[r]);
            box[nb].x0 = pad; box[nb].y0 = y; box[nb].x1 = x1; box[nb].y1 = y + rh; box[nb].rgb = col[r]; nb++;
            ovs_put(dc, x1, y, rh, x2, v[r][0]);
            ovs_put(dc, x2, y, rh, x3, v[r][1]);
            ovs_put(dc, x3, y, rh, W - pad, v[r][2]);
        }
        char line[OVS_LINES][96];
        ovs_lines(line);
        line[4][0] = 0;
        if (s_ovs_line && s_ovs_line(line[4], (int)sizeof line[4]) <= 0) line[4][0] = 0;
        for (int l = 0; l < nlines; l++) {
            const int y = ly + l * lh;
            ovs_put(dc, pad, y, lh, W - pad, line[l]);
            if (l < 4) { box[nb].x0 = pad; box[nb].y0 = y; box[nb].x1 = W - pad; box[nb].y1 = y + lh; box[nb].rgb = 0x98A29C; nb++; }
        }
        GdiFlush();
        for (int y = 0; y < H; y++) {
            const u32* a = bits + (size_t)y * 2 * OVS_W * 2, *b = a + OVS_W * 2;
            unsigned char* o = cov + (size_t)y * OVS_W;
            for (int x = 0; x < W; x++)
                o[x] = (unsigned char)(((a[x * 2] & 0xFF) + (a[x * 2 + 1] & 0xFF) + (b[x * 2] & 0xFF) + (b[x * 2 + 1] & 0xFF) + 2) >> 2);
        }
    }
    /* The graph: the last OVS_HIST frame times, 0 to 66.7 ms (15 frames a second) bottom to top, oldest left, a line
     * F / 12 thick, each pixel covered by what of the line's span crosses it. */
    const int gw = gx1 - gx0, gh = gy1 - gy0, in = F / 3;
    memset(gcov, 0, (size_t)gw * gh);
    {
        float v[OVS_HIST];
        for (int i = 0; i < OVS_HIST; i++) {
            float ft = s_ovs_ft[(s_ovs_ft_i + i) % OVS_HIST] / 66.7f;
            if (ft > 1.0f) ft = 1.0f;
            if (ft < 0.0f) ft = 0.0f;
            v[i] = (float)(gh - in) - ft * (float)(gh - 2 * in);
        }
        const int ax = in, bx = gw - in;
        const float hw = (float)F / 24.0f;
        float y0 = v[0];
        for (int x = ax; x < bx; x++) {
            const float t = (float)(x + 1 - ax) * (float)(OVS_HIST - 1) / (float)(bx - ax);
            const int i = t >= (float)(OVS_HIST - 1) ? OVS_HIST - 2 : (int)t;
            const float y1 = v[i] + (v[i + 1] - v[i]) * (t - (float)i);
            const float lo = (y0 < y1 ? y0 : y1) - hw, hi = (y0 > y1 ? y0 : y1) + hw;
            int a = (int)floorf(lo), b = (int)floorf(hi);
            if (a < 0) a = 0;
            if (b >= gh) b = gh - 1;
            for (int y = a; y <= b; y++) {
                const float top_ = (float)y > lo ? (float)y : lo, bot_ = (float)(y + 1) < hi ? (float)(y + 1) : hi;
                const float c = bot_ - top_;
                if (c > 0.0f) gcov[(size_t)y * gw + (size_t)x] = (unsigned char)((c > 1.0f ? 1.0f : c) * 255.0f + 0.5f);
            }
            y0 = y1;
        }
    }

    u32* const img = pair[back];
    for (int y = 0; y < H; y++) {
        const unsigned char* s = cov + (size_t)y * OVS_W;
        const u32* b = bg + (size_t)y * OVS_W;
        u32* o = img + (size_t)y * OVS_W;
        const int graph = y >= gy0 && y < gy1;
        for (int x = 0; x < W; x++) {
            const u32 cv = graph && x >= gx0 && x < gx1 ? gcov[(size_t)(y - gy0) * gw + (size_t)(x - gx0)] : s[x];
            if (!cv) { o[x] = b[x]; continue; }
            u32 rgb = 0xE6EAE6;
            for (int i = 0; i < nb; i++)
                if (x >= box[i].x0 && x < box[i].x1 && y >= box[i].y0 && y < box[i].y1) { rgb = box[i].rgb; break; }
            const u32 inv = 255 - cv, p = b[x];
            const u32 cr = (((rgb >> 16) & 0xFF) * cv + (p & 0xFF) * inv + 127) / 255;
            const u32 cg = (((rgb >> 8) & 0xFF) * cv + ((p >> 8) & 0xFF) * inv + 127) / 255;
            const u32 cb = ((rgb & 0xFF) * cv + ((p >> 16) & 0xFF) * inv + 127) / 255;
            const u32 ca = cv + ((p >> 24) * inv + 127) / 255;
            o[x] = cr | (cg << 8) | (cb << 16) | (ca << 24);
        }
    }
    EnterCriticalSection(&s_ovs_lock);
    s_ovs_front = img;
    s_ovs_fw = W;
    s_ovs_fh = H;
    LeaveCriticalSection(&s_ovs_lock);
    back ^= 1;
}

/* The presenting thread's part: the last finished image into dst (rows OV_W * 4 apart). */
static void ovs_copy(unsigned char* dst, u32* pw, u32* ph)
{
    *pw = *ph = 0;
    EnterCriticalSection(&s_ovs_lock);
    if (s_ovs_front) {
        for (int y = 0; y < s_ovs_fh; y++)
            memcpy(dst + (size_t)y * OV_W * 4, s_ovs_front + (size_t)y * OVS_W, (size_t)s_ovs_fw * 4);
        *pw = (u32)s_ovs_fw;
        *ph = (u32)s_ovs_fh;
    }
    LeaveCriticalSection(&s_ovs_lock);
}

static DWORD WINAPI ovs_painter(LPVOID arg)
{
    (void)arg;
    for (;;) {
        Sleep(100);
        const LONG font = InterlockedCompareExchange(&s_ovs_font, 0, 0);
        if (font) ov_render_panel((u32)font);
    }
}

/* The overlay's items over the back buffer at rtv, after the scaling pass. */
/* display.shader_hint (on by default): the shader build indicator. Read once. */
static int ovi_on(void)
{
    static int v = -1;
    if (v < 0) v = ps3_settings_bool("display", "shader_hint", "D3D12_SHADER_HINT", 1);
    return v;
}

static float ovi_clamp01(float v) { return v < 0.0f ? 0.0f : v > 1.0f ? 1.0f : v; }

/* The indicator into premultiplied RGBA at dst (rows OV_W * 4 apart), at most OV_W x
 * OVI_H; its size comes back in *pw, *ph. A rounded box of 85% near-black; in it a ring
 * -- frac < 0: an arc that turns (phase, turns), its tail fading; else the arc from the
 * top to frac of the way round -- and the text. The ring is computed per pixel with a
 * pixel of antialiasing; GDI draws the text, for its coverage. */
static void ovi_render(unsigned char* dst, u32 font_px, const char* text, float frac, float phase,
                       u32* pw, u32* ph)
{
    static HDC dc;
    static u32* bits;
    static HFONT font;
    static u32 font_size;
    static char drawn[64];
    static int tw;
    if (!dc) {
        BITMAPINFO bi;
        memset(&bi, 0, sizeof bi);
        bi.bmiHeader.biSize = sizeof bi.bmiHeader;
        bi.bmiHeader.biWidth = OV_W;
        bi.bmiHeader.biHeight = -OVI_H;   /* top-down */
        bi.bmiHeader.biPlanes = 1;
        bi.bmiHeader.biBitCount = 32;
        dc = CreateCompatibleDC(NULL);
        HBITMAP bmp = dc ? CreateDIBSection(dc, &bi, DIB_RGB_COLORS, (void**)&bits, NULL, 0) : NULL;
        if (!bmp || !bits) { *pw = *ph = 0; return; }
        SelectObject(dc, bmp);
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, RGB(255, 255, 255));
    }
    const int pad = (int)(font_px * 6 / 10), ring = (int)(font_px * 5 / 4), gap = (int)(font_px / 2);
    const int bh = ring + 2 * pad;
    if (font_size != font_px || strcmp(drawn, text)) {
        if (font_size != font_px) {
            if (font) DeleteObject(font);
            font = CreateFontA(-(int)font_px, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                               OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY,
                               DEFAULT_PITCH | FF_SWISS, "Segoe UI");
            SelectObject(dc, font);
            font_size = font_px;
        }
        memset(bits, 0, (size_t)OV_W * OVI_H * 4);
        SIZE sz;
        const int len = (int)strlen(text);
        GetTextExtentPoint32A(dc, text, len, &sz);
        tw = sz.cx;
        TextOutA(dc, pad + ring + gap, (bh - sz.cy) / 2, text, len);
        GdiFlush();
        snprintf(drawn, sizeof drawn, "%s", text);
    }
    int bw = pad + ring + gap + tw + pad;
    if (bw > OV_W) bw = OV_W;
    if (bh > OVI_H) { *pw = *ph = 0; return; }
    const float rad = (float)bh * 0.3f;                       /* the box's corners */
    const float cx = (float)pad + (float)ring * 0.5f, cy = (float)bh * 0.5f;
    const float r_out = (float)ring * 0.5f - 0.5f, r_in = r_out - (float)ring * 0.16f;
    for (int y = 0; y < bh; y++) {
        for (int x = 0; x < OV_W; x++) {
            unsigned char* o = dst + ((size_t)y * OV_W + x) * 4;
            if (x >= bw) { o[0] = o[1] = o[2] = o[3] = 0; continue; }
            /* the box, its corners rounded */
            const float px = (float)x + 0.5f, py = (float)y + 0.5f;
            const float qx = px < rad ? rad - px : px > (float)bw - rad ? px - ((float)bw - rad) : 0.0f;
            const float qy = py < rad ? rad - py : py > (float)bh - rad ? py - ((float)bh - rad) : 0.0f;
            const float box = ovi_clamp01(rad - sqrtf(qx * qx + qy * qy) + 0.5f);
            float a = 0.85f * box, r = 12.0f / 255.0f * a, g = 14.0f / 255.0f * a, b = 20.0f / 255.0f * a;
            /* the text, white */
            const u32 p = bits[(size_t)y * OV_W + x];
            u32 cov = p & 0xFF;
            if (((p >> 8) & 0xFF) > cov) cov = (p >> 8) & 0xFF;
            if (((p >> 16) & 0xFF) > cov) cov = (p >> 16) & 0xFF;
            const float t = (float)cov / 255.0f * box;
            r = t + r * (1.0f - t); g = t + g * (1.0f - t); b = t + b * (1.0f - t); a = t + a * (1.0f - t);
            /* the ring: a faint track, the arc over it */
            const float dx = px - cx, dy = py - cy, d = sqrtf(dx * dx + dy * dy);
            const float band = ovi_clamp01(fminf(d - r_in, r_out - d) + 0.5f);
            if (band > 0.0f) {
                float turn = atan2f(dx, -dy) / 6.2831853f;     /* 0 at the top, clockwise */
                if (turn < 0.0f) turn += 1.0f;
                float arc;
                if (frac < 0.0f) {
                    float behind = phase - turn;                /* how far behind the head */
                    behind -= floorf(behind);
                    arc = behind < 0.75f ? powf(1.0f - behind / 0.75f, 1.5f) : 0.0f;
                } else {
                    arc = turn <= frac ? 1.0f : 0.0f;
                }
                const float c = band * (0.18f + 0.82f * arc);
                r = c + r * (1.0f - c); g = c + g * (1.0f - c); b = c + b * (1.0f - c); a = c + a * (1.0f - c);
            }
            o[0] = (unsigned char)(r * 255.0f + 0.5f);
            o[1] = (unsigned char)(g * 255.0f + 0.5f);
            o[2] = (unsigned char)(b * 255.0f + 0.5f);
            o[3] = (unsigned char)(a * 255.0f + 0.5f);
        }
    }
    *pw = (u32)bw;
    *ph = (u32)bh;
}

static void ov_draw(u32 fi, D3D12_CPU_DESCRIPTOR_HANDLE rtv)
{
    if (!s_d3d.ov_pso) return;
    const char* msg = (const char*)s_toast_msg;
    if (msg && GetTickCount64() > s_toast_until) msg = NULL;
    const int panel = ovs_on();
    const char* item[2] = { NULL, msg };   /* the frame rate is the panel's */
    /* The shader build indicator: while jobs are in flight or the pipeline cache is
     * building, and 0.6 s after, so a short burst does not flicker. */
    static ULONGLONG ovi_until;
    static char ovi_text[64];
    static float ovi_frac = -1.0f;
    {
        const LONG jobs = InterlockedCompareExchange(&s_sj_inflight, 0, 0);
        const LONG ptot = InterlockedCompareExchange(&s_psoc_total, 0, 0);
        if (ovi_on() && (jobs > 0 || ptot > 0)) {
            ovi_until = GetTickCount64() + 600;
            if (ptot > 0) {
                LONG pdone = InterlockedCompareExchange(&s_psoc_done, 0, 0);
                if (pdone > ptot) pdone = ptot;   /* the builders take a few past the end */
                snprintf(ovi_text, sizeof ovi_text, "Building pipelines %ld / %ld", (long)pdone, (long)ptot);
                ovi_frac = (float)pdone / (float)ptot;
            } else {
                snprintf(ovi_text, sizeof ovi_text, "Compiling shaders");
                ovi_frac = -1.0f;
            }
        }
    }
    const int ind = ovi_on() && GetTickCount64() < ovi_until && ovi_text[0];
    if (!panel && !item[1] && !ind) return;

    /* Text the window's height can read: the message 1/40 of it (14 to 40
     * pixels), the panel 1/60 (12 to 28). */
    u32 font_px = s_d3d.bb_h / 40;
    if (font_px < 14) font_px = 14;
    if (font_px > 40) font_px = 40;
    u32 pfont = s_d3d.bb_h / 60;
    if (pfont < 12) pfont = 12;
    if (pfont > 28) pfont = 28;
    if (panel) InterlockedExchange(&s_ovs_font, (LONG)pfont);
    static char shown[2][64];
    static u32 shown_px, w[2], pw, ph;
    static ULONGLONG panel_at;
    const ULONGLONG now = GetTickCount64();
    const int panel_due = panel && now - panel_at >= 100;   /* the panel ten times a second */
    ID3D12GraphicsCommandList* cl = s_d3d.cmd_list;
    static u32 iw, ih;
    if (ind)   /* its band every frame it shows: the ring turns */
        ovi_render(s_d3d.ov_up_ptr[fi] + (size_t)(OV_ROWS_H + OVS_H) * OV_W * 4, pfont, ovi_text, ovi_frac,
                   (float)(now % 100000u) / 1000.0f * 1.25f, &iw, &ih);
    int whole = 0;
    if (panel_due || font_px != shown_px || strcmp(shown[0], item[0] ? item[0] : "") ||
        strcmp(shown[1], item[1] ? item[1] : "")) {
        whole = 1;
        ov_render(s_d3d.ov_up_ptr[fi], item, font_px, w);
        if (panel) {
            ovs_copy(s_d3d.ov_up_ptr[fi] + (size_t)OV_ROWS_H * OV_W * 4, &pw, &ph);
            panel_at = now;
        }
        snprintf(shown[0], sizeof shown[0], "%s", item[0] ? item[0] : "");
        snprintf(shown[1], sizeof shown[1], "%s", item[1] ? item[1] : "");
        shown_px = font_px;
        pp_barrier(s_d3d.ov_tex, s_d3d.ov_tex_ready ? D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE
                                                    : D3D12_RESOURCE_STATE_COMMON,
                   D3D12_RESOURCE_STATE_COPY_DEST);
        D3D12_TEXTURE_COPY_LOCATION dl = {0}, sl = {0};
        dl.pResource = s_d3d.ov_tex;
        dl.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        sl.pResource = s_d3d.ov_up[fi];
        sl.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        sl.PlacedFootprint.Footprint.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        sl.PlacedFootprint.Footprint.Width = OV_W;
        sl.PlacedFootprint.Footprint.Height = OV_H;
        sl.PlacedFootprint.Footprint.Depth = 1;
        sl.PlacedFootprint.Footprint.RowPitch = OV_W * 4;
        cl->lpVtbl->CopyTextureRegion(cl, &dl, 0, 0, 0, &sl, NULL);
        pp_barrier(s_d3d.ov_tex, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        s_d3d.ov_tex_ready = 1;
    }
    if (ind && !whole && ih) {
        /* only the indicator's band */
        pp_barrier(s_d3d.ov_tex, s_d3d.ov_tex_ready ? D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE
                                                    : D3D12_RESOURCE_STATE_COMMON,
                   D3D12_RESOURCE_STATE_COPY_DEST);
        D3D12_TEXTURE_COPY_LOCATION dl = {0}, sl = {0};
        dl.pResource = s_d3d.ov_tex;
        dl.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        sl.pResource = s_d3d.ov_up[fi];
        sl.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        sl.PlacedFootprint.Footprint.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        sl.PlacedFootprint.Footprint.Width = OV_W;
        sl.PlacedFootprint.Footprint.Height = OV_H;
        sl.PlacedFootprint.Footprint.Depth = 1;
        sl.PlacedFootprint.Footprint.RowPitch = OV_W * 4;
        const D3D12_BOX band = { 0, OV_ROWS_H + OVS_H, 0, OV_W, OV_ROWS_H + OVS_H + OVI_H, 1 };
        cl->lpVtbl->CopyTextureRegion(cl, &dl, 0, OV_ROWS_H + OVS_H, 0, &sl, &band);
        pp_barrier(s_d3d.ov_tex, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        s_d3d.ov_tex_ready = 1;
    }

    D3D12_GPU_DESCRIPTOR_HANDLE srv;
    s_d3d.pp_srv_heap->lpVtbl->GetGPUDescriptorHandleForHeapStart(s_d3d.pp_srv_heap, &srv);
    srv.ptr += (FRAME_COUNT + 1) * s_d3d.device->lpVtbl->GetDescriptorHandleIncrementSize(
        s_d3d.device, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    cl->lpVtbl->SetGraphicsRootSignature(cl, s_d3d.pp_root);
    cl->lpVtbl->SetDescriptorHeaps(cl, 1, &s_d3d.pp_srv_heap);
    cl->lpVtbl->SetGraphicsRootDescriptorTable(cl, 0, srv);
    cl->lpVtbl->SetPipelineState(cl, s_d3d.ov_pso);
    cl->lpVtbl->OMSetRenderTargets(cl, 1, &rtv, FALSE, NULL);
    cl->lpVtbl->IASetPrimitiveTopology(cl, D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
    const float bw = (float)s_d3d.bb_w, bh = (float)s_d3d.bb_h;
    for (int r = 0; r < 2; r++) {
        if (!item[r] || !w[r]) continue;
        /* a message centred near the top */
        const float x = (bw - (float)w[r]) * 0.5f, y = bh * 0.08f;
        const float c[16] = {
            x / bw * 2.0f - 1.0f, 1.0f - y / bh * 2.0f,
            (x + (float)w[r]) / bw * 2.0f - 1.0f, 1.0f - (y + (float)OV_ROW) / bh * 2.0f,
            0.0f, (float)(r * OV_ROW) / (float)OV_H, (float)w[r] / (float)OV_W, (float)((r + 1) * OV_ROW) / (float)OV_H };
        cl->lpVtbl->SetGraphicsRoot32BitConstants(cl, 1, 16, c, 0);
        cl->lpVtbl->DrawInstanced(cl, 4, 1, 0, 0);
    }
    if (ind && iw && ih) {
        /* the indicator in the bottom left corner, a texel to a pixel */
        const float x = (float)pfont, y = bh - (float)pfont - (float)ih;
        const float y0 = (float)(OV_ROWS_H + OVS_H);
        const float c[16] = {
            x / bw * 2.0f - 1.0f, 1.0f - y / bh * 2.0f,
            (x + (float)iw) / bw * 2.0f - 1.0f, 1.0f - (y + (float)ih) / bh * 2.0f,
            0.0f, y0 / (float)OV_H, (float)iw / (float)OV_W, (y0 + (float)ih) / (float)OV_H };
        cl->lpVtbl->SetGraphicsRoot32BitConstants(cl, 1, 16, c, 0);
        cl->lpVtbl->DrawInstanced(cl, 4, 1, 0, 0);
    }
    if (panel && pw && ph) {
        /* the panel in the top right corner, a texel to a pixel */
        const float x = bw - (float)pfont - (float)pw, y = (float)pfont;
        const float c[16] = {
            x / bw * 2.0f - 1.0f, 1.0f - y / bh * 2.0f,
            (x + (float)pw) / bw * 2.0f - 1.0f, 1.0f - (y + (float)ph) / bh * 2.0f,
            0.0f, (float)OV_ROWS_H / (float)OV_H, (float)pw / (float)OV_W, (float)(OV_ROWS_H + ph) / (float)OV_H };
        cl->lpVtbl->SetGraphicsRoot32BitConstants(cl, 1, 16, c, 0);
        cl->lpVtbl->DrawInstanced(cl, 4, 1, 0, 0);
    }
}

/* The overlay's texture, uploads and pipeline; part of pp_init. Without them
 * the game runs as before, with no overlay. */
static void ov_init(D3D12_GRAPHICS_PIPELINE_STATE_DESC pd)
{
    D3D12_HEAP_PROPERTIES hp = {0};
    hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC rd = {0};
    rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    rd.Width = OV_W;
    rd.Height = OV_H;
    rd.DepthOrArraySize = 1;
    rd.MipLevels = 1;
    rd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    rd.SampleDesc.Count = 1;
    if (FAILED(s_d3d.device->lpVtbl->CreateCommittedResource(s_d3d.device, &hp, D3D12_HEAP_FLAG_NONE, &rd,
            D3D12_RESOURCE_STATE_COMMON, NULL, &IID_ID3D12Resource, (void**)&s_d3d.ov_tex)))
        return;
    hp.Type = D3D12_HEAP_TYPE_UPLOAD;
    D3D12_RESOURCE_DESC ub = {0};
    ub.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    ub.Width = (u64)OV_W * 4 * OV_H;
    ub.Height = 1; ub.DepthOrArraySize = 1; ub.MipLevels = 1; ub.SampleDesc.Count = 1;
    ub.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    for (u32 i = 0; i < FRAME_COUNT; i++) {
        D3D12_RANGE none = {0, 0};
        if (FAILED(s_d3d.device->lpVtbl->CreateCommittedResource(s_d3d.device, &hp, D3D12_HEAP_FLAG_NONE, &ub,
                D3D12_RESOURCE_STATE_GENERIC_READ, NULL, &IID_ID3D12Resource, (void**)&s_d3d.ov_up[i])) ||
            FAILED(s_d3d.ov_up[i]->lpVtbl->Map(s_d3d.ov_up[i], 0, &none, (void**)&s_d3d.ov_up_ptr[i])))
            return;
    }
    D3D12_CPU_DESCRIPTOR_HANDLE h;
    s_d3d.pp_srv_heap->lpVtbl->GetCPUDescriptorHandleForHeapStart(s_d3d.pp_srv_heap, &h);
    h.ptr += (FRAME_COUNT + 1) * s_d3d.device->lpVtbl->GetDescriptorHandleIncrementSize(
        s_d3d.device, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    s_d3d.device->lpVtbl->CreateShaderResourceView(s_d3d.device, s_d3d.ov_tex, NULL, h);

    static const char hlsl[] =
        "cbuffer pc : register(b0) { float4 dst; float4 src_uv; float4 p2; float4 p3; };\n"
        "Texture2D tex : register(t0);\n"
        "SamplerState pnt : register(s1);\n"
        "struct V { float4 pos : SV_POSITION; float2 uv : TEXCOORD0; };\n"
        "V vs(uint id : SV_VertexID) {\n"
        "  float2 t = float2(id & 1, id >> 1); V o;\n"
        "  o.pos = float4(lerp(dst.xy, dst.zw, t), 0, 1); o.uv = lerp(src_uv.xy, src_uv.zw, t); return o; }\n"
        "float4 ps(V i) : SV_TARGET { return tex.SampleLevel(pnt, i.uv, 0); }\n";
    ID3DBlob *vsb = NULL, *psb = NULL, *err = NULL;
    HRESULT hr = rsx_compile_own(hlsl, sizeof hlsl - 1, "overlay_vs", NULL, NULL, "vs", "vs_5_0", 0, 0, &vsb, &err);
    if (err) { err->lpVtbl->Release(err); err = NULL; }
    if (SUCCEEDED(hr)) hr = rsx_compile_own(hlsl, sizeof hlsl - 1, "overlay_ps", NULL, NULL, "ps", "ps_5_0", 0, 0, &psb, &err);
    if (err) err->lpVtbl->Release(err);
    if (SUCCEEDED(hr)) {
        pd.VS.pShaderBytecode = vsb->lpVtbl->GetBufferPointer(vsb);
        pd.VS.BytecodeLength  = vsb->lpVtbl->GetBufferSize(vsb);
        pd.PS.pShaderBytecode = psb->lpVtbl->GetBufferPointer(psb);
        pd.PS.BytecodeLength  = psb->lpVtbl->GetBufferSize(psb);
        D3D12_RENDER_TARGET_BLEND_DESC* bl = &pd.BlendState.RenderTarget[0];
        bl->BlendEnable = TRUE;   /* premultiplied */
        bl->SrcBlend = D3D12_BLEND_ONE;       bl->DestBlend = D3D12_BLEND_INV_SRC_ALPHA;      bl->BlendOp = D3D12_BLEND_OP_ADD;
        bl->SrcBlendAlpha = D3D12_BLEND_ONE;  bl->DestBlendAlpha = D3D12_BLEND_INV_SRC_ALPHA; bl->BlendOpAlpha = D3D12_BLEND_OP_ADD;
        hr = s_d3d.device->lpVtbl->CreateGraphicsPipelineState(s_d3d.device, &pd, &IID_ID3D12PipelineState,
                                                               (void**)&s_d3d.ov_pso);
    }
    if (vsb) vsb->lpVtbl->Release(vsb);
    if (psb) psb->lpVtbl->Release(psb);
    if (FAILED(hr)) { printf("[D3D12] overlay unavailable (0x%08lX)\n", hr); s_d3d.ov_pso = NULL; }
    static int started;
    if (s_d3d.ov_pso && ovs_on() && !started) {
        started = 1;
        InitializeCriticalSection(&s_ovs_lock);
        typedef HRESULT (WINAPI *set_desc_fn)(HANDLE, PCWSTR);
        const set_desc_fn set_desc = (set_desc_fn)(void*)GetProcAddress(GetModuleHandleA("kernel32.dll"),
                                                                        "SetThreadDescription");
        const struct { LPTHREAD_START_ROUTINE fn; const wchar_t* name; } th[2] = {
            { ovs_sampler, L"overlay stats" }, { ovs_painter, L"overlay panel" } };
        for (int i = 0; i < 2; i++) {
            HANDLE h = CreateThread(NULL, 0, th[i].fn, NULL, 0, NULL);
            if (!h) continue;
            if (set_desc) set_desc(h, th[i].name);
            SetThreadPriority(h, THREAD_PRIORITY_BELOW_NORMAL);
            CloseHandle(h);
        }
    }
}

/* SMAA 1x (graphics.antialiasing = smaa; rsx_smaa.c): its targets, the
 * display's size, its lookup textures, its tables in pp_srv_heap and its
 * three pipelines. Anything missing turns it off and the game runs as before. */
static ID3D12Resource* aa_texture(DXGI_FORMAT fmt, u32 w, u32 h, int rt)
{
    D3D12_HEAP_PROPERTIES hp = {0};
    hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC rd = {0};
    rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    rd.Width = w; rd.Height = h; rd.DepthOrArraySize = 1; rd.MipLevels = 1;
    rd.Format = fmt;
    rd.SampleDesc.Count = 1;
    rd.Flags = rt ? D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET : D3D12_RESOURCE_FLAG_NONE;
    ID3D12Resource* r = NULL;
    if (FAILED(s_d3d.device->lpVtbl->CreateCommittedResource(s_d3d.device, &hp, D3D12_HEAP_FLAG_NONE, &rd,
            D3D12_RESOURCE_STATE_COMMON, NULL, &IID_ID3D12Resource, (void**)&r)))
        return NULL;
    return r;
}

static int aa_init(D3D12_GRAPHICS_PIPELINE_STATE_DESC pd)
{
    const u32 w = s_d3d.pw, h = s_d3d.ph;
    if (pp_root_create(3, &s_d3d.aa_root) != 0) return -1;
    s_d3d.aa_edges  = aa_texture(DXGI_FORMAT_R8G8_UNORM, w, h, 1);
    s_d3d.aa_blend  = aa_texture(DXGI_FORMAT_R8G8B8A8_UNORM, w, h, 1);
    s_d3d.aa_out    = aa_texture(DXGI_FORMAT_R8G8B8A8_UNORM, w, h, 1);
    unsigned aw, ah, ab, sw, sh, sb;
    const unsigned char* area = rsx_smaa_area(&aw, &ah, &ab);
    const unsigned char* search = rsx_smaa_search(&sw, &sh, &sb);
    s_d3d.aa_area   = aa_texture(DXGI_FORMAT_R8G8_UNORM, aw, ah, 0);
    s_d3d.aa_search = aa_texture(DXGI_FORMAT_R8_UNORM, sw, sh, 0);
    if (!s_d3d.aa_edges || !s_d3d.aa_blend || !s_d3d.aa_out || !s_d3d.aa_area || !s_d3d.aa_search) return -1;

    /* The lookup textures' upload, rows at the 256-byte pitch copies need:
     * AreaTex, then SearchTex at the next 512-byte boundary. */
    const u32 ap = (aw * ab + 255u) & ~255u, sp = (sw * sb + 255u) & ~255u;
    const u32 soff = (ap * ah + 511u) & ~511u;
    D3D12_HEAP_PROPERTIES up = {0};
    up.Type = D3D12_HEAP_TYPE_UPLOAD;
    D3D12_RESOURCE_DESC ub = {0};
    ub.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    ub.Width = soff + (u64)sp * sh;
    ub.Height = 1; ub.DepthOrArraySize = 1; ub.MipLevels = 1; ub.SampleDesc.Count = 1;
    ub.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    unsigned char* m = NULL;
    D3D12_RANGE none = {0, 0};
    if (FAILED(s_d3d.device->lpVtbl->CreateCommittedResource(s_d3d.device, &up, D3D12_HEAP_FLAG_NONE, &ub,
            D3D12_RESOURCE_STATE_GENERIC_READ, NULL, &IID_ID3D12Resource, (void**)&s_d3d.aa_up)) ||
        FAILED(s_d3d.aa_up->lpVtbl->Map(s_d3d.aa_up, 0, &none, (void**)&m)) || !m)
        return -1;
    for (u32 y = 0; y < ah; y++) memcpy(m + (size_t)y * ap, area + (size_t)y * aw * ab, aw * ab);
    for (u32 y = 0; y < sh; y++) memcpy(m + soff + (size_t)y * sp, search + (size_t)y * sw * sb, sw * sb);
    s_d3d.aa_up->lpVtbl->Unmap(s_d3d.aa_up, 0, NULL);

    D3D12_DESCRIPTOR_HEAP_DESC hd = {0};
    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    hd.NumDescriptors = 3;
    if (FAILED(s_d3d.device->lpVtbl->CreateDescriptorHeap(s_d3d.device, &hd, &IID_ID3D12DescriptorHeap,
                                                          (void**)&s_d3d.aa_rtv_heap)))
        return -1;
    D3D12_CPU_DESCRIPTOR_HANDLE r;
    s_d3d.aa_rtv_heap->lpVtbl->GetCPUDescriptorHandleForHeapStart(s_d3d.aa_rtv_heap, &r);
    ID3D12Resource* const rts[3] = { s_d3d.aa_edges, s_d3d.aa_blend, s_d3d.aa_out };
    for (int i = 0; i < 3; i++) {
        s_d3d.device->lpVtbl->CreateRenderTargetView(s_d3d.device, rts[i], NULL, r);
        r.ptr += s_d3d.rtv_descriptor_size;
    }

    /* The tables: an unused place holds a null view. */
    const UINT inc = s_d3d.device->lpVtbl->GetDescriptorHandleIncrementSize(
        s_d3d.device, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    D3D12_CPU_DESCRIPTOR_HANDLE base;
    s_d3d.pp_srv_heap->lpVtbl->GetCPUDescriptorHandleForHeapStart(s_d3d.pp_srv_heap, &base);
    D3D12_SHADER_RESOURCE_VIEW_DESC nd;
    memset(&nd, 0, sizeof nd);
    nd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    nd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    nd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    nd.Texture2D.MipLevels = 1;
    struct { u32 slot; ID3D12Resource* res; } views[] = {
        { PP_SLOT_AA_OUT, s_d3d.aa_out },
        { PP_SLOT_AA_W, s_d3d.aa_edges }, { PP_SLOT_AA_W + 1, s_d3d.aa_area }, { PP_SLOT_AA_W + 2, s_d3d.aa_search },
    };
    for (unsigned i = 0; i < sizeof views / sizeof views[0]; i++) {
        D3D12_CPU_DESCRIPTOR_HANDLE hh = base;
        hh.ptr += views[i].slot * inc;
        s_d3d.device->lpVtbl->CreateShaderResourceView(s_d3d.device, views[i].res, NULL, hh);
    }
    for (u32 fi = 0; fi < FRAME_COUNT; fi++) {
        ID3D12Resource* const e[3] = { s_d3d.render_targets[fi], NULL, NULL };
        ID3D12Resource* const n[3] = { s_d3d.render_targets[fi], s_d3d.aa_blend, NULL };
        for (u32 k = 0; k < 3; k++) {
            D3D12_CPU_DESCRIPTOR_HANDLE he = base, hn = base;
            he.ptr += (PP_SLOT_AA_E(fi) + k) * inc;
            hn.ptr += (PP_SLOT_AA_N(fi) + k) * inc;
            s_d3d.device->lpVtbl->CreateShaderResourceView(s_d3d.device, e[k], e[k] ? NULL : &nd, he);
            s_d3d.device->lpVtbl->CreateShaderResourceView(s_d3d.device, n[k], n[k] ? NULL : &nd, hn);
        }
    }

    static const DXGI_FORMAT fmt[3] = { DXGI_FORMAT_R8G8_UNORM, DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_R8G8B8A8_UNORM };
    pd.pRootSignature = s_d3d.aa_root;
    for (int p = 0; p < 3; p++) {
        ID3DBlob* vs = (ID3DBlob*)rsx_smaa_compile(p, 1);
        ID3DBlob* ps = vs ? (ID3DBlob*)rsx_smaa_compile(p, 0) : NULL;
        HRESULT hr = E_FAIL;
        if (vs && ps) {
            pd.VS.pShaderBytecode = vs->lpVtbl->GetBufferPointer(vs);
            pd.VS.BytecodeLength  = vs->lpVtbl->GetBufferSize(vs);
            pd.PS.pShaderBytecode = ps->lpVtbl->GetBufferPointer(ps);
            pd.PS.BytecodeLength  = ps->lpVtbl->GetBufferSize(ps);
            pd.RTVFormats[0] = fmt[p];
            hr = s_d3d.device->lpVtbl->CreateGraphicsPipelineState(s_d3d.device, &pd, &IID_ID3D12PipelineState,
                                                                   (void**)&s_d3d.aa_pso[p]);
        }
        if (vs) vs->lpVtbl->Release(vs);
        if (ps) ps->lpVtbl->Release(ps);
        if (FAILED(hr)) { s_d3d.aa_pso[p] = NULL; return -1; }
    }
    return 0;
}

/* One SMAA pass at the display's size: the table at slot src into rtv. */
static void aa_draw(ID3D12PipelineState* pso, u32 src, D3D12_CPU_DESCRIPTOR_HANDLE rtv)
{
    ID3D12GraphicsCommandList* cl = s_d3d.cmd_list;
    D3D12_GPU_DESCRIPTOR_HANDLE srv;
    s_d3d.pp_srv_heap->lpVtbl->GetGPUDescriptorHandleForHeapStart(s_d3d.pp_srv_heap, &srv);
    srv.ptr += src * s_d3d.device->lpVtbl->GetDescriptorHandleIncrementSize(
        s_d3d.device, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    const float w = (float)s_d3d.pw, h = (float)s_d3d.ph;
    const float c[16] = { 1.0f / w, 1.0f / h, w, h };
    D3D12_VIEWPORT vp = { 0.0f, 0.0f, w, h, 0.0f, 1.0f };
    D3D12_RECT sc = { 0, 0, (LONG)s_d3d.pw, (LONG)s_d3d.ph };
    cl->lpVtbl->SetGraphicsRootSignature(cl, s_d3d.aa_root);
    cl->lpVtbl->SetDescriptorHeaps(cl, 1, &s_d3d.pp_srv_heap);
    cl->lpVtbl->SetGraphicsRootDescriptorTable(cl, 0, srv);
    cl->lpVtbl->SetGraphicsRoot32BitConstants(cl, 1, 16, c, 0);
    cl->lpVtbl->SetPipelineState(cl, pso);
    cl->lpVtbl->RSSetViewports(cl, 1, &vp);
    cl->lpVtbl->RSSetScissorRects(cl, 1, &sc);
    cl->lpVtbl->OMSetRenderTargets(cl, 1, &rtv, FALSE, NULL);
    cl->lpVtbl->IASetPrimitiveTopology(cl, D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    cl->lpVtbl->DrawInstanced(cl, 3, 1, 0, 0);
}

/* SMAA 1x from display target fi into aa_out (left in PIXEL_SHADER_RESOURCE
 * for the scaling pass; aa_finish puts the targets back in COMMON). The
 * display target is already in PIXEL_SHADER_RESOURCE. */
static void aa_run(u32 table_e, u32 table_n)
{
    ID3D12GraphicsCommandList* cl = s_d3d.cmd_list;
    if (!s_d3d.aa_luts_ready) {
        unsigned aw, ah, ab, sw, sh, sb;
        rsx_smaa_area(&aw, &ah, &ab);
        rsx_smaa_search(&sw, &sh, &sb);
        const u32 ap = (aw * ab + 255u) & ~255u, sp = (sw * sb + 255u) & ~255u, soff = (ap * ah + 511u) & ~511u;
        ID3D12Resource* const dst[2] = { s_d3d.aa_area, s_d3d.aa_search };
        const DXGI_FORMAT fmt[2] = { DXGI_FORMAT_R8G8_UNORM, DXGI_FORMAT_R8_UNORM };
        const u32 dims[2][4] = { { aw, ah, ap, 0 }, { sw, sh, sp, soff } };
        for (int i = 0; i < 2; i++) {
            pp_barrier(dst[i], D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST);
            D3D12_TEXTURE_COPY_LOCATION dl = {0}, sl = {0};
            dl.pResource = dst[i];
            dl.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            sl.pResource = s_d3d.aa_up;
            sl.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
            sl.PlacedFootprint.Offset = dims[i][3];
            sl.PlacedFootprint.Footprint.Format = fmt[i];
            sl.PlacedFootprint.Footprint.Width = dims[i][0];
            sl.PlacedFootprint.Footprint.Height = dims[i][1];
            sl.PlacedFootprint.Footprint.Depth = 1;
            sl.PlacedFootprint.Footprint.RowPitch = dims[i][2];
            cl->lpVtbl->CopyTextureRegion(cl, &dl, 0, 0, 0, &sl, NULL);
            pp_barrier(dst[i], D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        }
        s_d3d.aa_luts_ready = 1;
    }
    D3D12_CPU_DESCRIPTOR_HANDLE rtv[3];
    s_d3d.aa_rtv_heap->lpVtbl->GetCPUDescriptorHandleForHeapStart(s_d3d.aa_rtv_heap, &rtv[0]);
    rtv[1] = rtv[0]; rtv[1].ptr += s_d3d.rtv_descriptor_size;
    rtv[2] = rtv[1]; rtv[2].ptr += s_d3d.rtv_descriptor_size;
    static const float zero[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
    /* Edges; the pass discards where there are none, so it starts cleared. */
    pp_barrier(s_d3d.aa_edges, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_RENDER_TARGET);
    cl->lpVtbl->ClearRenderTargetView(cl, rtv[0], zero, 0, NULL);
    aa_draw(s_d3d.aa_pso[0], table_e, rtv[0]);
    pp_barrier(s_d3d.aa_edges, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    /* Blending weights */
    pp_barrier(s_d3d.aa_blend, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_RENDER_TARGET);
    aa_draw(s_d3d.aa_pso[1], PP_SLOT_AA_W, rtv[1]);
    pp_barrier(s_d3d.aa_blend, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    /* Neighbourhood blending */
    pp_barrier(s_d3d.aa_out, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_RENDER_TARGET);
    aa_draw(s_d3d.aa_pso[2], table_n, rtv[2]);
    pp_barrier(s_d3d.aa_out, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
}

static void aa_finish(void)
{
    pp_barrier(s_d3d.aa_edges, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON);
    pp_barrier(s_d3d.aa_blend, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON);
    pp_barrier(s_d3d.aa_out, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON);
}

/* The display target into the swap chain's buffer, for a frame that is
 * presented, through the presentation pass: bilinear or sharp bilinear in one
 * draw, or FSR 1, EASU then RCAS through pp_tmp (EASU alone with sharpening
 * off). FSR 1 only scales up; a back buffer no larger than the display takes
 * bilinear. Everything ends where the next frame expects it: the display
 * target and pp_tmp in COMMON, the swap chain's buffer in PRESENT. */
/* The upscaler's output into the back buffer. Where the frame has a HUD, what the HUD changed in
 * the 720p picture goes on top: t1 is the frame with it, t2 the picture without. What it added
 * (text, icons, glow, a light tint) goes on as a difference. Where it darkened the picture a lot
 * (the death screen's black, a fade, the pause menu's dimming) the frame is taken as it is: the
 * difference took the scene back out of the upscaled scene, and left what the upscaler and its
 * sharpening put in, the scene's edges, on black (issue #9: the death screen showed the fall's
 * outlines with FSR 3.1). The luminance ratio, frame to picture: 0.9 and up the difference, 0.6
 * and down the frame, between them a blend. */
static int up_init(D3D12_GRAPHICS_PIPELINE_STATE_DESC pd)
{
    static const char hlsl[] =
        "Texture2D up : register(t0);\n"
        "Texture2D frame : register(t1);\n"
        "Texture2D scene : register(t2);\n"
        "SamplerState lin : register(s0);\n"
        "SamplerState pnt : register(s1);\n"
        "cbuffer pc : register(b0) { float4 p0; float4 p1; float4 p2; float4 p3; };\n"
        "struct V { float4 pos : SV_POSITION; float2 uv : TEXCOORD0; };\n"
        "float4 ps(V i) : SV_TARGET {\n"
        "  float3 c = up.SampleLevel(pnt, i.uv, 0).rgb;\n"
        "  if (p0.x > 0.5) {\n"
        "    const float3 f = frame.SampleLevel(lin, i.uv, 0).rgb, s = scene.SampleLevel(lin, i.uv, 0).rgb;\n"
        "    const float3 lw = float3(0.299, 0.587, 0.114);\n"
        "    const float r = (dot(f, lw) + 0.01) / (dot(s, lw) + 0.01);\n"
        "    c = lerp(c + f - s, f, saturate((0.9 - r) / 0.3));\n"
        "  }\n"
        "  return float4(saturate(c), 1); }\n";
    if (pp_root_create(3, &s_up.root) != 0) return -1;
    ID3DBlob *ps = NULL, *err = NULL;
    HRESULT hr = rsx_compile_own(hlsl, sizeof hlsl - 1, "up_ps", NULL, NULL, "ps", "ps_5_0", 0, 0, &ps, &err);
    if (err) { if (FAILED(hr)) printf("[D3D12] %s%c", (const char*)err->lpVtbl->GetBufferPointer(err), 10); err->lpVtbl->Release(err); }
    if (FAILED(hr)) return -1;
    pd.pRootSignature = s_up.root;
    pd.PS.pShaderBytecode = ps->lpVtbl->GetBufferPointer(ps);
    pd.PS.BytecodeLength = ps->lpVtbl->GetBufferSize(ps);
    hr = s_d3d.device->lpVtbl->CreateGraphicsPipelineState(s_d3d.device, &pd, &IID_ID3D12PipelineState, (void**)&s_up.pso);
    ps->lpVtbl->Release(ps);
    return FAILED(hr) ? -1 : 0;
}

static void up_draw(u32 table, int hud, D3D12_CPU_DESCRIPTOR_HANDLE rtv)
{
    ID3D12GraphicsCommandList* cl = s_d3d.cmd_list;
    D3D12_GPU_DESCRIPTOR_HANDLE srv;
    s_d3d.pp_srv_heap->lpVtbl->GetGPUDescriptorHandleForHeapStart(s_d3d.pp_srv_heap, &srv);
    srv.ptr += table * s_d3d.device->lpVtbl->GetDescriptorHandleIncrementSize(
        s_d3d.device, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    const float c[16] = { hud ? 1.0f : 0.0f };
    D3D12_VIEWPORT vp = { 0.0f, 0.0f, (float)s_d3d.bb_w, (float)s_d3d.bb_h, 0.0f, 1.0f };
    D3D12_RECT sc = { 0, 0, (LONG)s_d3d.bb_w, (LONG)s_d3d.bb_h };
    cl->lpVtbl->SetGraphicsRootSignature(cl, s_up.root);
    cl->lpVtbl->SetDescriptorHeaps(cl, 1, &s_d3d.pp_srv_heap);
    cl->lpVtbl->SetGraphicsRootDescriptorTable(cl, 0, srv);
    cl->lpVtbl->SetGraphicsRoot32BitConstants(cl, 1, 16, c, 0);
    cl->lpVtbl->SetPipelineState(cl, s_up.pso);
    cl->lpVtbl->RSSetViewports(cl, 1, &vp);
    cl->lpVtbl->RSSetScissorRects(cl, 1, &sc);
    cl->lpVtbl->OMSetRenderTargets(cl, 1, &rtv, FALSE, NULL);
    cl->lpVtbl->IASetPrimitiveTopology(cl, D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    cl->lpVtbl->DrawInstanced(cl, 3, 1, 0, 0);
}

/* The frame through the upscaler and into the back buffer, left as a render target. 0 if it
 * cannot take this one: not a scene, or the context not there. */
static int up_run(u32 fi, ID3D12Resource* disp, ID3D12Resource* bb, D3D12_CPU_DESCRIPTOR_HANDLE bb_rtv)
{
    if (!s_up.want || !s_fg.depth_ok || !s_fg.hud_out[0] || (s_fg.marked && !s_fg.copied) ||
        s_d3d.bb_w < s_d3d.pw || s_d3d.bb_h < s_d3d.ph)
        return 0;
    if (!s_up.ready) {
        /* graphics.sharpening on FSR's own scale: high is the 0.8 of AMD's sample. */
        const float sharp = s_d3d.pp_stops < 0.0f ? -1.0f : 1.0f - s_d3d.pp_stops;
        if (!rsx_fsr3_up_create(s_d3d.device, s_d3d.bb_w, s_d3d.bb_h, s_d3d.pw, s_d3d.ph, sharp)) {
            s_up.want = 0;
            return 0;
        }
        s_up.ready = 1;
        printf("[D3D12] upscaler on: %ux%u from %ux%u%c", s_d3d.bb_w, s_d3d.bb_h, s_d3d.pw, s_d3d.ph, 10);
    }
    rsx_fsr3_frame f;
    fg_desc(&f);
    if (!rsx_fsr3_prepare(s_d3d.cmd_list, &f)) return 0;
    const D3D12_RESOURCE_STATES read = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    s_fg.hud_i ^= 1;
    ID3D12Resource* out = s_fg.hud_out[s_fg.hud_i];
    pp_barrier(out, read, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    rsx_fsr3_up_frame(s_d3d.cmd_list, &f, s_fg.copied ? s_fg.hud_src : disp, out, fg_ms(perf_now()));
    pp_barrier(out, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, read);
    pp_barrier(bb, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET);
    up_draw(PP_SLOT_UP(s_fg.hud_i, fi), s_fg.copied, bb_rtv);
    s_fg.ready = 1;
    return 1;
}

/* src (an SRV slot) through SMAA, when it is on, and the scaling into out, left as a
 * render target. SMAA's targets stay as they are until aa_finish. */
static void pp_chain(u32 src, u32 table_e, u32 table_n, D3D12_CPU_DESCRIPTOR_HANDLE out_rtv,
                     D3D12_CPU_DESCRIPTOR_HANDLE tmp_rtv, ID3D12Resource* out, D3D12_RESOURCE_STATES before)
{
    const int up = s_d3d.bb_w > s_d3d.pw && s_d3d.bb_h > s_d3d.ph;
    u32 c[16];
    memset(c, 0, sizeof c);
    if (s_d3d.aa_on) {
        aa_run(table_e, table_n);
        src = PP_SLOT_AA_OUT;
    }
    if (s_d3d.pp_mode == 2 && up) {
        rsx_fsr1_easu_con(c, (float)s_d3d.pw, (float)s_d3d.ph, (float)s_d3d.bb_w, (float)s_d3d.bb_h);
        if (s_d3d.pp_tmp) {
            pp_barrier(s_d3d.pp_tmp, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_RENDER_TARGET);
            pp_draw(s_d3d.pp_pso_easu, src, tmp_rtv, c);
            pp_barrier(s_d3d.pp_tmp, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
            pp_barrier(out, before, D3D12_RESOURCE_STATE_RENDER_TARGET);
            memset(c, 0, sizeof c);
            rsx_fsr1_rcas_con(c, s_d3d.pp_stops);
            pp_draw(s_d3d.pp_pso_rcas, FRAME_COUNT, out_rtv, c);
            pp_barrier(s_d3d.pp_tmp, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON);
        } else {
            pp_barrier(out, before, D3D12_RESOURCE_STATE_RENDER_TARGET);
            pp_draw(s_d3d.pp_pso_easu, src, out_rtv, c);
        }
    } else {
        const float f[5] = { (float)s_d3d.pw, (float)s_d3d.ph, (float)s_d3d.bb_w, (float)s_d3d.bb_h,
                             s_d3d.pp_mode == 1 ? 1.0f : 0.0f };
        memcpy(c, f, sizeof f);
        pp_barrier(out, before, D3D12_RESOURCE_STATE_RENDER_TARGET);
        pp_draw(s_d3d.pp_pso, src, out_rtv, c);
    }
}

/* The picture without its HUD through the same pass as the frame, so that what differs
 * between the two is the HUD and nothing else: frame generation goes by that. */
static void fg_hudless(D3D12_CPU_DESCRIPTOR_HANDLE tmp_rtv)
{
    if (!s_fg.copied || !s_fg.hud_out[0]) return;
    s_fg.hud_i ^= 1;
    ID3D12Resource* out = s_fg.hud_out[s_fg.hud_i];
    D3D12_CPU_DESCRIPTOR_HANDLE rtv = tmp_rtv;
    rtv.ptr += (1 + s_fg.hud_i) * s_d3d.rtv_descriptor_size;
    const D3D12_RESOURCE_STATES read = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    pp_chain(PP_SLOT_HUD, PP_SLOT_HUD_E, PP_SLOT_HUD_N, rtv, tmp_rtv, out, read);
    if (s_d3d.aa_on) aa_finish();
    pp_barrier(out, D3D12_RESOURCE_STATE_RENDER_TARGET, read);
    s_fg.ready = 1;
}

static void fg_unmarked(ID3D12Resource* bb);

static void present_to_backbuffer(u32 fi)
{
    D3D12_CPU_DESCRIPTOR_HANDLE bb_rtv, tmp_rtv;
    s_d3d.bb_rtv_heap->lpVtbl->GetCPUDescriptorHandleForHeapStart(s_d3d.bb_rtv_heap, &bb_rtv);
    tmp_rtv = bb_rtv;
    bb_rtv.ptr += fi * s_d3d.rtv_descriptor_size;
    tmp_rtv.ptr += FRAME_COUNT * s_d3d.rtv_descriptor_size;
    ID3D12Resource* disp = s_d3d.render_targets[fi];
    ID3D12Resource* bb = s_d3d.backbuffers[fi];

    if (s_wide != 1.0f && !s_wide_scene) {
        const LONG side = (LONG)(((float)s_d3d.pw - (float)s_d3d.pw / s_wide) * 0.5f);
        const D3D12_RECT bars[2] = { { 0, 0, side, (LONG)s_d3d.ph },
                                     { (LONG)s_d3d.pw - side, 0, (LONG)s_d3d.pw, (LONG)s_d3d.ph } };
        const float black[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
        D3D12_CPU_DESCRIPTOR_HANDLE rtv;
        s_d3d.rtv_heap->lpVtbl->GetCPUDescriptorHandleForHeapStart(s_d3d.rtv_heap, &rtv);
        rtv.ptr += fi * s_d3d.rtv_descriptor_size;
        pp_barrier(disp, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_RENDER_TARGET);
        s_d3d.cmd_list->lpVtbl->ClearRenderTargetView(s_d3d.cmd_list, rtv, black, 2, bars);
        pp_barrier(disp, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COMMON);
    }
    s_wide_scene = 0;
    pp_barrier(disp, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    const int up = up_run(fi, disp, bb, bb_rtv);
    if (!up) {
        fg_hudless(tmp_rtv);
        pp_chain(fi, PP_SLOT_AA_E(fi), PP_SLOT_AA_N(fi), bb_rtv, tmp_rtv, bb, D3D12_RESOURCE_STATE_PRESENT);
        fg_unmarked(bb);
    }
    ov_draw(fi, bb_rtv);
    if (s_d3d.aa_on && !up) aa_finish();
    pp_barrier(bb, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PRESENT);
    pp_barrier(disp, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON);
}

static double s_rf_last_submit;   /* RF_WAITLOG: when the last batch was submitted */
static void zq_submitted(u32 fi, u64 fence);   /* the ZPASS block, below */
static void mwb_submitted(u32 fi, u64 fence);  /* main-memory surfaces, below it */
static void move_to_next_frame(void)
{
    s_rf_last_submit = perf_now();
    u64 current_fence = s_d3d.fence_values[s_d3d.frame_index];
    s_d3d.cmd_queue->lpVtbl->Signal(s_d3d.cmd_queue, s_d3d.fence, current_fence);
    s_fence_last = current_fence;
    zq_submitted(s_d3d.frame_index, current_fence);
    mwb_submitted(s_d3d.frame_index, current_fence);

    s_d3d.frame_index = s_d3d.swap_chain->lpVtbl->GetCurrentBackBufferIndex(s_d3d.swap_chain);

    if (s_d3d.fence->lpVtbl->GetCompletedValue(s_d3d.fence) < s_d3d.fence_values[s_d3d.frame_index]) {
        s_d3d.fence->lpVtbl->SetEventOnCompletion(
            s_d3d.fence, s_d3d.fence_values[s_d3d.frame_index], s_d3d.fence_event);
        if (WaitForSingleObject(s_d3d.fence_event, 2000) == WAIT_TIMEOUT) {
            HRESULT rr = s_d3d.device->lpVtbl->GetDeviceRemovedReason(s_d3d.device);
            const u64 got = s_d3d.fence->lpVtbl->GetCompletedValue(s_d3d.fence);
            s_stuck_fence = got + 1;
            printf("[D3D12] FENCE STUCK 2s: want %llu got %llu removed=0x%08lX\n",
                   (unsigned long long)s_d3d.fence_values[s_d3d.frame_index],
                   (unsigned long long)got,
                   (long)rr);
        }
    }

    s_d3d.fence_values[s_d3d.frame_index] = current_fence + 1;
}

/* ---------------------------------------------------------------------------
 * Render a frame (clear + present)
 * -----------------------------------------------------------------------*/

/* Write the mapped readback buffer (R8G8B8A8, row pitch = readback_pitch) out
 * as a 24-bit bottom-up BMP. Debug-only. */
/* Name prefix for the next readback dump: lets the same writer emit the
 * backbuffer and the screen-copy texture under different filenames. */
static const char* s_dump_name = NULL;

static void dump_backbuffer_bmp(void)
{
    if (!s_d3d.readback_buf) return;
    void* mapped = NULL;
    D3D12_RANGE rr = {0, (SIZE_T)s_d3d.readback_pitch * s_d3d.ph};
    if (FAILED(s_d3d.readback_buf->lpVtbl->Map(s_d3d.readback_buf, 0, &rr, &mapped)) || !mapped)
        return;

    static int idx = 0;
    char path[512];
    const char* dir = getenv("CELLMARK_DUMP_DIR");   /* default: current dir */
    snprintf(path, sizeof(path), "%s%s%s_%03d.bmp",
             dir ? dir : "", dir ? "/" : "", s_dump_name ? s_dump_name : "frame", idx++);
    FILE* f = fopen(path, "wb");
    if (f) {
        u32 w = s_d3d.pw, h = s_d3d.ph;
        u32 padded = (w * 3 + 3) & ~3u;
        u32 imgsz  = padded * h;
        u32 filesz = 54 + imgsz;
        unsigned char hdr[54] = {0};
        hdr[0] = 'B'; hdr[1] = 'M';
        hdr[2]=filesz&0xFF; hdr[3]=(filesz>>8)&0xFF; hdr[4]=(filesz>>16)&0xFF; hdr[5]=(filesz>>24)&0xFF;
        hdr[10]=54; hdr[14]=40;
        hdr[18]=w&0xFF; hdr[19]=(w>>8)&0xFF; hdr[20]=(w>>16)&0xFF; hdr[21]=(w>>24)&0xFF;
        hdr[22]=h&0xFF; hdr[23]=(h>>8)&0xFF; hdr[24]=(h>>16)&0xFF; hdr[25]=(h>>24)&0xFF;
        hdr[26]=1; hdr[28]=24;
        hdr[34]=imgsz&0xFF; hdr[35]=(imgsz>>8)&0xFF; hdr[36]=(imgsz>>16)&0xFF; hdr[37]=(imgsz>>24)&0xFF;
        fwrite(hdr, 1, 54, f);
        unsigned char* row = (unsigned char*)malloc(padded);
        if (row) {
            memset(row, 0, padded);
            for (int y = (int)h - 1; y >= 0; y--) {   /* BMP is bottom-up */
                unsigned char* src = (unsigned char*)mapped + (u32)y * s_d3d.readback_pitch;
                for (u32 x = 0; x < w; x++) {
                    row[x*3+0] = src[x*4+2]; /* B */
                    row[x*3+1] = src[x*4+1]; /* G */
                    row[x*3+2] = src[x*4+0]; /* R */
                }
                fwrite(row, 1, padded, f);
            }
            free(row);
        }
        fclose(f);
        printf("[D3D12] dumped %s (%ux%u)\n", path, s_d3d.pw, s_d3d.ph);
    }
    D3D12_RANGE wr = {0, 0};
    s_d3d.readback_buf->lpVtbl->Unmap(s_d3d.readback_buf, 0, &wr);
}

/* F12: the frame in the readback buffer as a PNG in screenshots/ next to the
 * executable, named by the title and the time. The pixels go in uncompressed
 * (deflate's stored blocks): a 1280x720 shot is 2.7 MB, written in a few
 * milliseconds on the presenting thread, with no image library. */
static u32 png_crc(u32 c, const unsigned char* p, size_t n)
{
    static u32 t[256];
    if (!t[1])
        for (u32 i = 0; i < 256; i++) {
            u32 v = i;
            for (int k = 0; k < 8; k++) v = (v & 1) ? 0xEDB88320u ^ (v >> 1) : v >> 1;
            t[i] = v;
        }
    c = ~c;
    while (n--) c = t[(c ^ *p++) & 0xFF] ^ (c >> 8);
    return ~c;
}

static void png_u32(unsigned char* p, u32 v) { p[0] = (unsigned char)(v >> 24); p[1] = (unsigned char)(v >> 16); p[2] = (unsigned char)(v >> 8); p[3] = (unsigned char)v; }

static void png_chunk(FILE* f, const char* type, const unsigned char* data, u32 n)
{
    unsigned char h[8];
    png_u32(h, n);
    memcpy(h + 4, type, 4);
    fwrite(h, 1, 8, f);
    if (n) fwrite(data, 1, n, f);
    u32 c = png_crc(0, h + 4, 4);
    c = png_crc(c, data, n);
    png_u32(h, c);
    fwrite(h, 1, 4, f);
}

/* The back buffer into the readback buffer, for a screenshot on a frame that
 * is not being dumped (a dump makes the same copy itself). */
static void shot_copy(u32 fi)
{
    D3D12_RESOURCE_BARRIER bar = {0};
    bar.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    bar.Transition.pResource = s_d3d.render_targets[fi];
    bar.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    bar.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
    bar.Transition.StateAfter  = D3D12_RESOURCE_STATE_COPY_SOURCE;
    s_d3d.cmd_list->lpVtbl->ResourceBarrier(s_d3d.cmd_list, 1, &bar);
    D3D12_TEXTURE_COPY_LOCATION dst = {0}, src = {0};
    dst.pResource = s_d3d.readback_buf;
    dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    dst.PlacedFootprint.Footprint.Format   = DXGI_FORMAT_R8G8B8A8_UNORM;
    dst.PlacedFootprint.Footprint.Width    = s_d3d.pw;
    dst.PlacedFootprint.Footprint.Height   = s_d3d.ph;
    dst.PlacedFootprint.Footprint.Depth    = 1;
    dst.PlacedFootprint.Footprint.RowPitch = s_d3d.readback_pitch;
    src.pResource = s_d3d.render_targets[fi];
    src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    s_d3d.cmd_list->lpVtbl->CopyTextureRegion(s_d3d.cmd_list, &dst, 0, 0, 0, &src, NULL);
    bar.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
    bar.Transition.StateAfter  = D3D12_RESOURCE_STATE_PRESENT;
    s_d3d.cmd_list->lpVtbl->ResourceBarrier(s_d3d.cmd_list, 1, &bar);
}

/* display.screenshot = output (the default: what the window shows, at its
 * size, after the scaling and the overlay) or game (the title's own frame).
 * Read once. */
static int d3d12_shot_game(void)
{
    static int v = -1;
    if (v < 0) v = ps3_settings_ieq(ps3_settings_str("display", "screenshot", NULL, "output"), "game");
    return v;
}

/* The swap chain's buffer into shot_buf, after the presentation pass: made
 * again when the window's size has changed (the last shot has been written,
 * so nothing still uses the old one). */
static void shot_copy_bb(u32 fi)
{
    const u32 pitch = (s_d3d.bb_w * 4 + 255) & ~255u;
    if (!s_d3d.shot_buf || s_d3d.shot_w != s_d3d.bb_w || s_d3d.shot_h != s_d3d.bb_h) {
        if (s_d3d.shot_buf) { s_d3d.shot_buf->lpVtbl->Release(s_d3d.shot_buf); s_d3d.shot_buf = NULL; }
        D3D12_HEAP_PROPERTIES hp = {0};
        hp.Type = D3D12_HEAP_TYPE_READBACK;
        D3D12_RESOURCE_DESC rd = {0};
        rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        rd.Width = (u64)pitch * s_d3d.bb_h;
        rd.Height = 1; rd.DepthOrArraySize = 1; rd.MipLevels = 1; rd.SampleDesc.Count = 1;
        rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        if (FAILED(s_d3d.device->lpVtbl->CreateCommittedResource(s_d3d.device, &hp, D3D12_HEAP_FLAG_NONE, &rd,
                D3D12_RESOURCE_STATE_COPY_DEST, NULL, &IID_ID3D12Resource, (void**)&s_d3d.shot_buf))) {
            s_d3d.shot_buf = NULL;
            return;
        }
        s_d3d.shot_w = s_d3d.bb_w; s_d3d.shot_h = s_d3d.bb_h; s_d3d.shot_pitch = pitch;
    }
    D3D12_RESOURCE_BARRIER bar;
    memset(&bar, 0, sizeof bar);
    bar.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    bar.Transition.pResource = s_d3d.backbuffers[fi];
    bar.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    bar.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
    bar.Transition.StateAfter  = D3D12_RESOURCE_STATE_COPY_SOURCE;
    s_d3d.cmd_list->lpVtbl->ResourceBarrier(s_d3d.cmd_list, 1, &bar);
    D3D12_TEXTURE_COPY_LOCATION dst = {0}, src = {0};
    dst.pResource = s_d3d.shot_buf;
    dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    dst.PlacedFootprint.Footprint.Format   = DXGI_FORMAT_R8G8B8A8_UNORM;
    dst.PlacedFootprint.Footprint.Width    = s_d3d.bb_w;
    dst.PlacedFootprint.Footprint.Height   = s_d3d.bb_h;
    dst.PlacedFootprint.Footprint.Depth    = 1;
    dst.PlacedFootprint.Footprint.RowPitch = pitch;
    src.pResource = s_d3d.backbuffers[fi];
    src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    s_d3d.cmd_list->lpVtbl->CopyTextureRegion(s_d3d.cmd_list, &dst, 0, 0, 0, &src, NULL);
    bar.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
    bar.Transition.StateAfter  = D3D12_RESOURCE_STATE_PRESENT;
    s_d3d.cmd_list->lpVtbl->ResourceBarrier(s_d3d.cmd_list, 1, &bar);
}

/* A w x h RGBA image read back into buf, rows pitch bytes apart, as a PNG. */
static int shot_png(ID3D12Resource* buf, u32 w, u32 h, u32 pitch)
{
    if (!buf) return 0;
    const u32 row = 1 + w * 3;
    const size_t raw = (size_t)row * h, blocks = (raw + 65534) / 65535;
    const size_t idat = 2 + raw + blocks * 5 + 4;
    unsigned char* z = (unsigned char*)malloc(idat);
    unsigned char* line = (unsigned char*)malloc(row);
    void* mapped = NULL;
    D3D12_RANGE rr = {0, (SIZE_T)pitch * h};
    if (!z || !line || FAILED(buf->lpVtbl->Map(buf, 0, &rr, &mapped)) || !mapped) {
        free(z); free(line);
        return 0;
    }
    /* zlib: header, stored blocks of the filtered rows, Adler-32 */
    size_t o = 0, left = raw, blk = 0;
    u32 a = 1, s = 0;
    z[o++] = 0x78; z[o++] = 0x01;
    for (u32 y = 0; y < h; y++) {
        const unsigned char* src = (const unsigned char*)mapped + (size_t)y * pitch;
        line[0] = 0;   /* filter: none */
        for (u32 x = 0; x < w; x++) memcpy(line + 1 + x * 3, src + x * 4, 3);
        for (u32 i = 0; i < row; i++) {
            if (!blk) {
                blk = left < 65535 ? left : 65535;
                z[o++] = left == blk;   /* BFINAL on the last block, BTYPE 00 */
                z[o++] = (unsigned char)blk; z[o++] = (unsigned char)(blk >> 8);
                z[o++] = (unsigned char)~blk; z[o++] = (unsigned char)(~blk >> 8);
            }
            z[o++] = line[i];
            a = (a + line[i]) % 65521u;
            s = (s + a) % 65521u;
            blk--; left--;
        }
    }
    png_u32(z + o, (s << 16) | a);
    o += 4;
    D3D12_RANGE wr = {0, 0};
    buf->lpVtbl->Unmap(buf, 0, &wr);

    /* Beside the settings, where replacing the program's folder does not reach them. */
    char dir[MAX_PATH], path[MAX_PATH + 96];
    const char* settings = ps3_settings_path();
    char* slash = NULL;
    if (settings && *settings) {
        snprintf(dir, sizeof dir, "%s", settings);
        slash = strrchr(dir, '\\');
        if (!slash) slash = strrchr(dir, '/');
    }
    if (!slash) {
        DWORD n = GetModuleFileNameA(NULL, dir, sizeof dir);
        slash = n && n < sizeof dir ? strrchr(dir, '\\') : NULL;
    }
    if (slash) slash[1] = 0; else dir[0] = 0;
    strncat(dir, "screenshots", sizeof dir - strlen(dir) - 1);
    CreateDirectoryA(dir, NULL);
    SYSTEMTIME t;
    GetLocalTime(&t);
    extern char g_rsx_title_base[128];
    snprintf(path, sizeof path, "%s\\%s_%04u%02u%02u_%02u%02u%02u_%03u.png", dir,
             g_rsx_title_base[0] ? g_rsx_title_base : "ps3recomp", t.wYear, t.wMonth, t.wDay,
             t.wHour, t.wMinute, t.wSecond, t.wMilliseconds);
    FILE* f = fopen(path, "wb");
    if (f) {
        static const unsigned char sig[8] = { 0x89, 'P', 'N', 'G', 13, 10, 26, 10 };
        unsigned char ihdr[13];
        png_u32(ihdr, w); png_u32(ihdr + 4, h);
        ihdr[8] = 8; ihdr[9] = 2; ihdr[10] = ihdr[11] = ihdr[12] = 0;   /* 8-bit RGB */
        fwrite(sig, 1, 8, f);
        png_chunk(f, "IHDR", ihdr, 13);
        png_chunk(f, "IDAT", z, (u32)o);
        png_chunk(f, "IEND", NULL, 0);
        fclose(f);
        printf("[D3D12] screenshot %s\n", path);
    } else {
        printf("[D3D12] screenshot: could not write %s\n", path);
    }
    free(z); free(line);
    return f != NULL;
}

/* ---------------------------------------------------------------------------
 * RSX frame capture ("rsxcap") -- a purpose-built GPU debugger. RSX_CAP=start
 * [:count[:stride]] snapshots whole frames to <RSX_CAP_DIR>/frame_NN/: a text
 * manifest of every op (in submission order, with full surface/shader/texture
 * state) plus a BMP of the backbuffer and every offscreen colour RT used that
 * frame. One frame, captured atomically -- no cross-frame guessing.
 * -----------------------------------------------------------------------*/

/* RSX_CAP schedule: the next capture is taken at frame_count >= cap_target(). */
static int s_cap_parsed = 0;
static u32 s_cap_start = 0, s_cap_count = 1, s_cap_stride = 1, s_cap_done = 0;
/* RSX_CAP=t<s>[:count[:stride]]: the first capture on the first frame that
 * starts s seconds after the first frame rendered, the others stride frames
 * apart -- frame counts move with every change to the renderer. Armed at a
 * frame's end (cap_arm), so a batch is due or not from its first record. */
static int s_cap_timed = 0, s_cap_armed = 0;
static ULONGLONG s_cap_t0 = 0;
static u64 s_cap_f0 = 0;
static const char* cap_env(void)
{
    static const char* e = (const char*)1;
    if (e == (const char*)1) e = getenv("RSX_CAP");
    if (e && !s_cap_parsed) {
        s_cap_parsed = 1;
        const char* q = e;
        if (*q == 't') { s_cap_timed = 1; q++; s_cap_t0 = GetTickCount64(); }
        u32 a = 0, b = 1, c = 1;
        int n = sscanf(q, "%u:%u:%u", &a, &b, &c);
        s_cap_start  = (n >= 1 && a) ? a : 200;
        s_cap_count  = (n >= 2 && b) ? b : 1;
        s_cap_stride = (n >= 3 && c) ? c : 1;
    }
    return e;
}
static int cap_due(void)
{
    if (!cap_env() || s_cap_done >= s_cap_count) return 0;
    if (s_cap_timed)
        return s_cap_armed && s_d3d.frame_count >= s_cap_f0 + (u64)s_cap_done * s_cap_stride;
    return s_d3d.frame_count >= s_cap_start + s_cap_done * s_cap_stride;
}
static void cap_arm(void)
{
    if (s_cap_timed && !s_cap_armed &&
        GetTickCount64() - s_cap_t0 >= (ULONGLONG)s_cap_start * 1000ull) {
        s_cap_armed = 1;
        s_cap_f0 = s_d3d.frame_count + 1;
    }
}

/* RSX_CAP_STOP=<op>[,<op>...]: a frame about to be captured replays its ops
 * only up to (not including) that one -- the k-th capture takes the k-th value,
 * the last one repeating -- so the targets it saves show the frame at that
 * point instead of at its end, where a later clear may have wiped them. A
 * value fp<hex> stops before the first op drawn with that fragment program
 * (op numbers move from frame to frame). */
static u32 cap_stop(u32 n)
{
    static const char* e = (const char*)1;
    if (e == (const char*)1) e = getenv("RSX_CAP_STOP");
    if (!e || !cap_due()) return n;
    u32 v = n; const char* q = e;
    for (u32 k = 0; ; k++) {
        char* end;
        if (q[0] == 'f' && q[1] == 'p') {
            const u32 fp = (u32)strtoul(q + 2, &end, 16);
            if (end == q + 2) break;
            v = n;
            for (u32 i = 0; i < n && i < MAX_DRAWS; i++)
                if (s_d3d.draws[i].is_vp && !s_d3d.draws[i].is_clear && s_d3d.draws[i].fp_addr == fp) { v = i; break; }
        } else {
            const unsigned long x = strtoul(q, &end, 10);
            if (end == q) break;
            v = (u32)x;
        }
        if (k >= s_cap_done || *end != ',') break;
        q = end + 1;
    }
    return v < n ? v : n;
}

/* Shared 24-bit BMP writer for readbacks. R8G8B8A8_UNORM straight; the half/
 * float RT formats are |v|-tonemapped so signed/HDR data is visible. */
static void cap_write_bmp(const char* path, const void* mapped, u32 w, u32 h,
                          u32 pitch, u32 dxgi)
{
    FILE* f = fopen(path, "wb");
    if (!f) { fprintf(stderr, "[RSXCAP] cannot write %s\n", path); return; }
    u32 rowb = (w * 3 + 3) & ~3u, datasz = rowb * h;
    u8 hdr[54] = {0};
    hdr[0] = 'B'; hdr[1] = 'M';
    *(u32*)(hdr+2) = 54 + datasz; *(u32*)(hdr+10) = 54; *(u32*)(hdr+14) = 40;
    *(int*)(hdr+18) = (int)w; *(int*)(hdr+22) = (int)h;
    *(u16*)(hdr+26) = 1; *(u16*)(hdr+28) = 24; *(u32*)(hdr+34) = datasz;
    fwrite(hdr, 1, 54, f);
    u8* line = (u8*)malloc(rowb);
    if (line) for (int y = (int)h - 1; y >= 0; y--) {   /* BMP is bottom-up */
        const u8* srow = (const u8*)mapped + (u64)y * pitch;
        memset(line, 0, rowb);
        for (u32 x = 0; x < w; x++) {
            float rv, gv, bv;
            if (dxgi == DXGI_FORMAT_R16G16B16A16_FLOAT) {
                const u16* hp = (const u16*)(srow + (u64)x * 8);
                float v[3];
                for (int c = 0; c < 3; c++) {
                    u16 hv = hp[c]; u32 s = (hv>>15)&1, e = (hv>>10)&0x1F, m = hv&0x3FF; float fv;
                    if (e == 0) fv = (float)m / 16777216.0f;
                    else { u32 fb = (s<<31)|((e-15+127)<<23)|(m<<13); memcpy(&fv,&fb,4); }
                    v[c] = fv;
                }
                rv = v[0]; gv = v[1]; bv = v[2];
            } else if (dxgi == DXGI_FORMAT_R32G32B32A32_FLOAT) {
                const float* fp = (const float*)(srow + (u64)x * 16);
                rv = fp[0]; gv = fp[1]; bv = fp[2];
            } else {
                const u8* p = srow + (u64)x * 4;
                rv = p[0]/255.0f; gv = p[1]/255.0f; bv = p[2]/255.0f;
            }
            float ar = rv<0?-rv:rv, ag = gv<0?-gv:gv, ab = bv<0?-bv:bv;
            if (ar>1) ar=1; if (ag>1) ag=1; if (ab>1) ab=1;
            line[x*3+0] = (u8)(ab*255.0f);
            line[x*3+1] = (u8)(ag*255.0f);
            line[x*3+2] = (u8)(ar*255.0f);
        }
        fwrite(line, 1, rowb, f);
    }
    free(line);
    fclose(f);
}

/* Copy one GPU resource into a fresh readback buffer and write it as a BMP.
 * Self-contained: waits, resets the frame command list, copies, executes,
 * waits, maps. `prior` is the resource's current tracked state (restored after
 * the copy so the caller's state tracking stays valid). */
/* Open while a capture is written: the raw bytes of each target's first
 * pixels (an exposure kept in a 1x1 corner of a larger surface, packed halves
 * a BMP cannot show). */
static FILE* s_cap_px;

/* RSX_CAP_RAW=1: each captured target's exact bytes as well (<name>.raw, rows of
 * width x bytes per pixel, no padding): the BMPs keep three bytes a pixel, which
 * loses a float target's top byte. Depth surfaces too (cap_ds_write). */
static int cap_raw_on(void)
{
    static int v = -1;
    if (v < 0) { const char* e = getenv("RSX_CAP_RAW"); v = e ? atoi(e) != 0 : 0; }
    return v;
}
static void cap_write_raw(const char* path, const char* ext, const u8* p, u32 row_bytes, u32 h, u32 pitch)
{
    char rp[600];
    snprintf(rp, sizeof rp, "%s%s", path, ext);
    char* dot = strrchr(rp, '.');
    /* rt_X.bmp.raw -> rt_X.raw */
    { char* b = strstr(rp, ".bmp"); if (b && dot && b < dot) memmove(b, dot, strlen(dot) + 1); }
    FILE* f = fopen(rp, "wb");
    if (!f) return;
    for (u32 y = 0; y < h; y++) fwrite(p + (u64)y * pitch, 1, row_bytes, f);
    fclose(f);
}

static void cap_readback_write(ID3D12Resource* res, u32 w, u32 h, u32 dxgi,
                               D3D12_RESOURCE_STATES prior, u32 fi, const char* path)
{
    if (!res || !w || !h) return;
    u32 bpp = (dxgi == DXGI_FORMAT_R16G16B16A16_FLOAT) ? 8 :
              (dxgi == DXGI_FORMAT_R32G32B32A32_FLOAT) ? 16 : 4;
    u32 pitch = (w * bpp + 255) & ~255u;

    ID3D12Resource* rb = NULL;
    D3D12_HEAP_PROPERTIES hp = {0}; hp.Type = D3D12_HEAP_TYPE_READBACK;
    D3D12_RESOURCE_DESC rd = {0};
    rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    rd.Width = (u64)pitch * h; rd.Height = 1; rd.DepthOrArraySize = 1;
    rd.MipLevels = 1; rd.SampleDesc.Count = 1;
    rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    HRESULT chr = s_d3d.device->lpVtbl->CreateCommittedResource(
            s_d3d.device, &hp, D3D12_HEAP_FLAG_NONE, &rd,
            D3D12_RESOURCE_STATE_COPY_DEST, NULL,
            &IID_ID3D12Resource, (void**)&rb);
    if (FAILED(chr) || !rb) {
        fprintf(stderr, "[RSXCAP] readback buffer failed hr=0x%08lX removed=0x%08lX (%s)\n",
                (unsigned long)chr,
                (unsigned long)s_d3d.device->lpVtbl->GetDeviceRemovedReason(s_d3d.device), path);
        return;
    }

    wait_for_gpu();
    s_d3d.cmd_allocators[fi]->lpVtbl->Reset(s_d3d.cmd_allocators[fi]);
    s_d3d.cmd_list->lpVtbl->Reset(s_d3d.cmd_list, s_d3d.cmd_allocators[fi], NULL);

    D3D12_RESOURCE_BARRIER b = {0};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = res;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    if (prior != D3D12_RESOURCE_STATE_COPY_SOURCE) {
        b.Transition.StateBefore = prior;
        b.Transition.StateAfter  = D3D12_RESOURCE_STATE_COPY_SOURCE;
        s_d3d.cmd_list->lpVtbl->ResourceBarrier(s_d3d.cmd_list, 1, &b);
    }

    D3D12_TEXTURE_COPY_LOCATION dst = {0}, src = {0};
    dst.pResource = rb; dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    dst.PlacedFootprint.Footprint.Format = (DXGI_FORMAT)dxgi;
    dst.PlacedFootprint.Footprint.Width = w; dst.PlacedFootprint.Footprint.Height = h;
    dst.PlacedFootprint.Footprint.Depth = 1; dst.PlacedFootprint.Footprint.RowPitch = pitch;
    src.pResource = res; src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    src.SubresourceIndex = 0;
    s_d3d.cmd_list->lpVtbl->CopyTextureRegion(s_d3d.cmd_list, &dst, 0, 0, 0, &src, NULL);
        /* TEXUP_DBG=1: texture uploads per second. A cache that misses every
         * draw re-uploads megabytes per frame, which is the kind of GPU work
         * that trips a 2-second TDR while looking like nothing in a draw count. */
        { static int td = -1; if (td < 0) td = getenv("TEXUP_DBG") ? 1 : 0;
          if (td) { static long long n = 0, bytes = 0; static clock_t t0 = 0;
              n++; bytes += (long long)pitch * h;
              const clock_t now = clock();
              if (!t0) t0 = now;
              if (now - t0 >= CLOCKS_PER_SEC) {
                  fprintf(stderr, "[TEXUP] %lld uploads/s, %.1f MB/s\n",
                          n, bytes / 1048576.0);
                  n = 0; bytes = 0; t0 = now; } } }

    if (prior != D3D12_RESOURCE_STATE_COPY_SOURCE) {
        b.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
        b.Transition.StateAfter  = prior;
        s_d3d.cmd_list->lpVtbl->ResourceBarrier(s_d3d.cmd_list, 1, &b);
    }

    s_d3d.cmd_list->lpVtbl->Close(s_d3d.cmd_list);
    ID3D12CommandList* cl[] = { (ID3D12CommandList*)s_d3d.cmd_list };
    s_d3d.cmd_queue->lpVtbl->ExecuteCommandLists(s_d3d.cmd_queue, 1, cl);
    wait_for_gpu();

    void* mp = NULL; D3D12_RANGE rr = {0, (SIZE_T)pitch * h};
    const HRESULT mhr = rb->lpVtbl->Map(rb, 0, &rr, &mp);
    if (SUCCEEDED(mhr) && mp) {
        cap_write_bmp(path, mp, w, h, pitch, dxgi);
        if (cap_raw_on()) cap_write_raw(path, ".raw", (const u8*)mp, w * bpp, h, pitch);
        if (s_cap_px) {
            const char* bn = strrchr(path, '/');
            fprintf(s_cap_px, "%s", bn ? bn + 1 : path);
            for (u32 y = 0; y < 2 && y < h; y++) {
                fprintf(s_cap_px, " |");
                for (u32 x = 0; x < 4 && x < w; x++) {
                    fputc(' ', s_cap_px);
                    for (u32 k = 0; k < bpp; k++)
                        fprintf(s_cap_px, "%02X", ((const u8*)mp)[(u64)y * pitch + x * bpp + k]);
                }
            }
            fputc(10, s_cap_px);
        }
        D3D12_RANGE wr = {0, 0};
        rb->lpVtbl->Unmap(rb, 0, &wr);
    } else {
        fprintf(stderr, "[RSXCAP] map failed hr=0x%08lX (%s)\n", (unsigned long)mhr, path);
    }
    rb->lpVtbl->Release(rb);
}

static void cap_dump_snaps(u32 fi, const char* dir);   /* with the blit snapshots */
static int s_present_this_frame;

/* RSX_CAP_YELLOW=<n>: capture (as frame_1NN) up to n batches that leave a
 * display buffer they drew in saturated yellow along row 40 -- the rare flash
 * a fixed schedule misses (it is presented a batch later, so the batch that
 * made it is the one to keep). Reads those rows back every batch, so the GPU
 * is waited on each time. */
static int cap_res_yellow(ID3D12Resource* res, D3D12_RESOURCE_STATES st, u32 w, u32 h, u32 fi)
{
    if (!res || w < 64 || h < 64 || w > 4096) return 0;
    const u32 pitch = (w * 4 + 255) & ~255u;
    static ID3D12Resource* rb = NULL;
    if (!rb) {
        D3D12_HEAP_PROPERTIES hp = {0}; hp.Type = D3D12_HEAP_TYPE_READBACK;
        D3D12_RESOURCE_DESC rd = {0};
        rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        rd.Width = pitch; rd.Height = 1; rd.DepthOrArraySize = 1;
        rd.MipLevels = 1; rd.SampleDesc.Count = 1; rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        if (FAILED(s_d3d.device->lpVtbl->CreateCommittedResource(s_d3d.device, &hp, D3D12_HEAP_FLAG_NONE,
                   &rd, D3D12_RESOURCE_STATE_COPY_DEST, NULL, &IID_ID3D12Resource, (void**)&rb)))
            return 0;
    }
    wait_for_gpu();
    s_d3d.cmd_allocators[fi]->lpVtbl->Reset(s_d3d.cmd_allocators[fi]);
    s_d3d.cmd_list->lpVtbl->Reset(s_d3d.cmd_list, s_d3d.cmd_allocators[fi], NULL);
    D3D12_RESOURCE_BARRIER b = {0};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = res;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = st;
    b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
    if (st != D3D12_RESOURCE_STATE_COPY_SOURCE) s_d3d.cmd_list->lpVtbl->ResourceBarrier(s_d3d.cmd_list, 1, &b);
    D3D12_TEXTURE_COPY_LOCATION dst = {0}, src = {0};
    dst.pResource = rb; dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    dst.PlacedFootprint.Footprint.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    dst.PlacedFootprint.Footprint.Width = w; dst.PlacedFootprint.Footprint.Height = 1;
    dst.PlacedFootprint.Footprint.Depth = 1; dst.PlacedFootprint.Footprint.RowPitch = pitch;
    src.pResource = res; src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    D3D12_BOX box = { 0, 40, 0, w, 41, 1 };
    s_d3d.cmd_list->lpVtbl->CopyTextureRegion(s_d3d.cmd_list, &dst, 0, 0, 0, &src, &box);
    b.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
    b.Transition.StateAfter = st;
    if (st != D3D12_RESOURCE_STATE_COPY_SOURCE) s_d3d.cmd_list->lpVtbl->ResourceBarrier(s_d3d.cmd_list, 1, &b);
    s_d3d.cmd_list->lpVtbl->Close(s_d3d.cmd_list);
    ID3D12CommandList* cl[] = { (ID3D12CommandList*)s_d3d.cmd_list };
    s_d3d.cmd_queue->lpVtbl->ExecuteCommandLists(s_d3d.cmd_queue, 1, cl);
    wait_for_gpu();
    void* mp = NULL; D3D12_RANGE rr = {0, pitch};
    u32 yel = 0;
    if (SUCCEEDED(rb->lpVtbl->Map(rb, 0, &rr, &mp)) && mp) {
        const u8* px = (const u8*)mp;
        for (u32 x = 0; x < w; x++)
            if (px[x*4] > 240 && px[x*4+1] > 240 && px[x*4+2] < 40) yel++;
        D3D12_RANGE wr = {0, 0};
        rb->lpVtbl->Unmap(rb, 0, &wr);
    }
    return yel * 10 > w * 8;
}

static int off_rt_find(u32 off);
static int cap_batch_yellow(u32 fi)
{
    if (!display_rt_on())
        return s_present_this_frame &&
               cap_res_yellow(s_d3d.render_targets[fi], D3D12_RESOURCE_STATE_PRESENT,
                              s_d3d.pw, s_d3d.ph, fi);
    u32 seen = 0;
    for (u32 i = 0; i < s_dbg_last_draws && i < MAX_DRAWS; i++) {   /* draw_count is reset by now */
        const D3D12DrawRecord* r = &s_d3d.draws[i];
        if (r->is_clear || !(r->rt_off & DISP_RT_TAG)) continue;
        const int k = off_rt_find(r->rt_off);
        if (k < 0 || k >= 32 || (seen >> k) & 1u) continue;
        seen |= 1u << k;
        const OffRT* o = &s_d3d.off_rt[k];
        const int y = cap_res_yellow(o->res, o->st, o->pw, o->ph, fi);
        if (getenv("RSX_CAP_YELLOW_DBG"))
            fprintf(stderr, "[yellow-dbg] f=%llu slot %d key 0x%X st=0x%X -> %d%c",
                    (unsigned long long)s_d3d.frame_count, k, o->off, (unsigned)o->st, y, 10);
        if (y) return 1;
    }
    /* Shown yellow although nothing this batch drew is: 2. */
    if (s_present_this_frame &&
        cap_res_yellow(s_d3d.render_targets[fi], D3D12_RESOURCE_STATE_PRESENT,
                       s_d3d.pw, s_d3d.ph, fi))
        return 2;
    return 0;
}

/* A depth surface's planes (depth, stencil) as raw rows: <path>.depth.raw and
 * <path>.stencil.raw, in the formats GetCopyableFootprints gives for them. */
static void cap_ds_write(OffDS* ds, u32 fi, const char* path)
{
    if (!ds->res) return;
    D3D12_RESOURCE_DESC desc;
    ds->res->lpVtbl->GetDesc(ds->res, &desc);
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp[2];
    UINT rows[2]; UINT64 rowb[2], total = 0;
    s_d3d.device->lpVtbl->GetCopyableFootprints(s_d3d.device, &desc, 0, 2, 0, fp, rows, rowb, &total);
    if (!total) return;
    ID3D12Resource* rb = NULL;
    D3D12_HEAP_PROPERTIES hp = {0}; hp.Type = D3D12_HEAP_TYPE_READBACK;
    D3D12_RESOURCE_DESC rd = {0};
    rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    rd.Width = total; rd.Height = 1; rd.DepthOrArraySize = 1;
    rd.MipLevels = 1; rd.SampleDesc.Count = 1;
    rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    if (FAILED(s_d3d.device->lpVtbl->CreateCommittedResource(s_d3d.device, &hp, D3D12_HEAP_FLAG_NONE, &rd,
            D3D12_RESOURCE_STATE_COPY_DEST, NULL, &IID_ID3D12Resource, (void**)&rb)) || !rb)
        return;
    wait_for_gpu();
    s_d3d.cmd_allocators[fi]->lpVtbl->Reset(s_d3d.cmd_allocators[fi]);
    s_d3d.cmd_list->lpVtbl->Reset(s_d3d.cmd_list, s_d3d.cmd_allocators[fi], NULL);
    D3D12_RESOURCE_BARRIER b = {0};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = ds->res;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    if (ds->st != D3D12_RESOURCE_STATE_COPY_SOURCE) {
        b.Transition.StateBefore = ds->st;
        b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
        s_d3d.cmd_list->lpVtbl->ResourceBarrier(s_d3d.cmd_list, 1, &b);
    }
    for (UINT pl = 0; pl < 2; pl++) {
        D3D12_TEXTURE_COPY_LOCATION dst = {0}, src = {0};
        dst.pResource = rb; dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        dst.PlacedFootprint = fp[pl];
        src.pResource = ds->res; src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        src.SubresourceIndex = pl;
        s_d3d.cmd_list->lpVtbl->CopyTextureRegion(s_d3d.cmd_list, &dst, 0, 0, 0, &src, NULL);
    }
    if (ds->st != D3D12_RESOURCE_STATE_COPY_SOURCE) {
        b.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
        b.Transition.StateAfter = ds->st;
        s_d3d.cmd_list->lpVtbl->ResourceBarrier(s_d3d.cmd_list, 1, &b);
    }
    s_d3d.cmd_list->lpVtbl->Close(s_d3d.cmd_list);
    ID3D12CommandList* cl[] = { (ID3D12CommandList*)s_d3d.cmd_list };
    s_d3d.cmd_queue->lpVtbl->ExecuteCommandLists(s_d3d.cmd_queue, 1, cl);
    wait_for_gpu();
    void* mp = NULL; D3D12_RANGE rr = {0, (SIZE_T)total};
    if (SUCCEEDED(rb->lpVtbl->Map(rb, 0, &rr, &mp)) && mp) {
        for (UINT pl = 0; pl < 2; pl++)
            cap_write_raw(path, pl ? ".stencil.raw" : ".depth.raw", (const u8*)mp + fp[pl].Offset,
                          (u32)rowb[pl], rows[pl], fp[pl].Footprint.RowPitch);
        fprintf(stderr, "[RSXCAP] %s: depth %ux%u fmt %u row %llu, stencil fmt %u row %llu%c", path,
                fp[0].Footprint.Width, fp[0].Footprint.Height, (unsigned)fp[0].Footprint.Format,
                (unsigned long long)rowb[0], (unsigned)fp[1].Footprint.Format,
                (unsigned long long)rowb[1], 10);
        D3D12_RANGE wr = {0, 0};
        rb->lpVtbl->Unmap(rb, 0, &wr);
    }
    rb->lpVtbl->Release(rb);
}

static void rsx_capture_frame(u32 fi, u32 ndraws, u32 capidx)
{
    char dir[300]; const char* base = getenv("RSX_CAP_DIR");
    if (!base || !*base) base = "rsxcap";
    CreateDirectoryA(base, NULL);
    snprintf(dir, sizeof dir, "%s/frame_%02u", base, capidx);
    CreateDirectoryA(dir, NULL);
    char path[512];

    /* Manifest: every op in submission order with full state. */
    snprintf(path, sizeof path, "%s/manifest.txt", dir);
    FILE* mf = fopen(path, "w");
    if (mf) {
        fprintf(mf, "frame_count=%llu draws=%u backbuffer=%ux%u\n",
                (unsigned long long)s_d3d.frame_count, ndraws, s_d3d.width, s_d3d.height);
        for (int i = 0; i < MAX_OFF_RTS; i++)
            if (s_d3d.off_rt[i].res && s_d3d.off_rt[i].used)
                fprintf(mf, "offrt[%d] off=0x%08X %ux%u dxgi=%u\n", i, s_d3d.off_rt[i].off,
                        s_d3d.off_rt[i].w, s_d3d.off_rt[i].h, s_d3d.off_rt[i].dxgi);
        for (int i = 0; i < MAX_OFF_DS; i++)
            if (s_d3d.off_ds[i].res)
                fprintf(mf, "offds[%d] off=0x%08X %ux%u last=%llu\n", i, s_d3d.off_ds[i].off,
                        s_d3d.off_ds[i].w, s_d3d.off_ds[i].h, (unsigned long long)s_d3d.off_ds[i].last_frame);
        fprintf(mf, "--- ops ---\n");
        for (u32 d = 0; d < ndraws && d < MAX_DRAWS; d++) {
            const D3D12DrawRecord* r = &s_d3d.draws[d];
            if (r->snap_ea)
                fprintf(mf, "op%03u SNAP  ea=0x%08X slot=%d src=0x%X\n", d, r->snap_ea, r->snap_slot, r->snap_src);
            else if (r->flip_key)
                fprintf(mf, "op%03u FLIP  0x%08X\n", d, r->flip_key);
            else if (r->zq_op)
                fprintf(mf, "op%03u ZPASS %s 0x%08X\n", d,
                        r->zq_op == 1 ? "clear" : r->zq_op == 2 ? "report" : "count", r->zq_ea);
            else if (r->is_blit)
                fprintf(mf, "op%03u BLIT  0x%X/%u -> 0x%X/%u %ux%u\n", d, r->blit_src, r->blit_sp,
                        r->blit_dst, r->blit_dp, r->blit_w, r->blit_h);
            else if (r->is_clear)
                fprintf(mf, "op%03u CLEAR rt=0x%08X mrt=0x%X/0x%X/0x%X cc=(%.3f,%.3f,%.3f,%.3f) mask=0x%X surf=0x%X sc=%u,%u,%ux%u\n",
                        d, r->rt_off, r->rt_mrt[0], r->rt_mrt[1], r->rt_mrt[2],
                        r->cc[0], r->cc[1], r->cc[2], r->cc[3], r->clear_mask, r->surf_a,
                        r->sc_x, r->sc_y, r->sc_w, r->sc_h);
            else {
                fprintf(mf, "op%03u DRAW  rt=0x%08X mrt=0x%X/0x%X/0x%X vp=%u,%u,%ux%u fp=0x%08X vs=%d "
                            "n=%u cmask=%X blend=%d srgb=%d bk=0x%X t0=0x%08X t1=0x%08X t2=0x%08X t3=0x%08X surf=0x%X sc=%u,%u,%ux%u z=0x%X zs=0x%llX\n",
                        d, r->rt_off, r->rt_mrt[0], r->rt_mrt[1], r->rt_mrt[2],
                        r->vp_x, r->vp_y, r->vp_w, r->vp_h,
                        r->fp_addr, r->vs_idx, r->vertex_count, r->cmask, r->blend,
                        (r->fp_ctrl & FP_CTRL_SRGB_OUT) ? 1 : 0, r->blend_key,
                        r->tex[0].raw, r->tex[1].raw, r->tex[2].raw, r->tex[3].raw, r->surf_a,
                        r->sc_x, r->sc_y, r->sc_w, r->sc_h, r->zeta_off, (unsigned long long)r->zs_key);
                if (r->dbt) {
                    float lo, hi;
                    memcpy(&lo, &r->dbt_min, sizeof lo); memcpy(&hi, &r->dbt_max, sizeof hi);
                    fprintf(mf, "      dbt %.6f..%.6f z=0x%X sref=0x%02X\n", lo, hi, r->zeta_off,
                            (unsigned)r->stencil_ref);
                }
                /* Only the units the program samples: the others carry
                 * whatever earlier draws left bound. */
                u32 fpm = 0xFFFFFFFFu;
                { extern uint8_t* vm_base;
                  extern u32 cellGcmResolveLocated(int local, u32 offset);
                  extern u32 rsx_fp_texture_units(const u8* ucode, u32 max_bytes);
                  if (r->fp_addr && vm_base) {
                      const u32 fo = cellGcmResolveLocated((r->fp_addr & 3u) == 1, r->fp_addr & ~3u);
                      if (fo != 0xFFFFFFFFu) fpm = rsx_fp_texture_units(vm_base + fo, FP_MAX_BYTES);
                  } }
                for (int u = 0; u < VP_TEX_UNITS; u++)
                    if (r->tex[u].set && ((fpm >> u) & 1u))
                        fprintf(mf, "      t%d off=0x%08X fmt=0x%02X (%s%s) %ux%u remap=0x%04X addr=0x%X filter=0x%X border=0x%08X\n", u,
                                r->tex[u].off, r->tex[u].fmt,
                                (r->tex[u].fmt & 0x20) ? "LN" : "SZ",
                                (r->tex[u].fmt & 0x40) ? ",NR" : "",
                                r->tex[u].w, r->tex[u].h, r->tex[u].ctrl1 & 0xFFFF,
                                r->tex[u].addr, r->tex[u].filter, r->tex[u].border);
            }
        }
        /* Per-draw VP constant banks for the first few offscreen-RT geometry
         * draws: c[0..3] is the projection row-major for most CG VPs; also scan
         * for the largest non-zero slot so a mis-placed MVP is obvious. Records
         * live at slot==draw-index; parity is post-^1 here (capture is after the
         * frame's vp_parity flip), so read the other half. */
        if (s_d3d.vp_cb_mapped) {
            u32 par = (u32)(s_d3d.vp_parity ^ 1);
            fprintf(mf, "--- vp constants (first offscreen geometry draws) ---\n");
            int shown = 0;
            for (u32 d = 0; d < ndraws && d < MAX_DRAWS && shown < 4; d++) {
                const D3D12DrawRecord* r = &s_d3d.draws[d];
                if (r->is_clear || !r->rt_off) continue;
                const float* c = (const float*)((const char*)s_d3d.vp_cb_mapped
                    + ((u64)par * MAX_DRAWS + r->cb_slot) * VP_CB_STRIDE);
                int last_nz = -1; float maxabs = 0.0f;
                for (int i = 0; i < RSX_MAX_VERTEX_CONSTANTS * 4; i++) {
                    float a = c[i] < 0 ? -c[i] : c[i];
                    if (a > 1e-9f) last_nz = i;
                    if (a > maxabs) maxabs = a;
                }
                fprintf(mf, "op%03u vs=%d lastNZslot=%d maxabs=%.3f\n",
                        d, r->vs_idx, last_nz / 4, maxabs);
                if (shown == 0) {   /* full non-zero slot dump for the 1st draw */
                    for (int s = 0; s <= last_nz / 4; s++) {
                        const float* v = c + s * 4;
                        if ((v[0]||v[1]||v[2]||v[3]))
                            fprintf(mf, "   c[%3d] = %9.4f %9.4f %9.4f %9.4f\n",
                                    s, v[0], v[1], v[2], v[3]);
                    }
                }
                shown++;
            }
            /* The draws with a depth bounds test (God of War: Ascension's screen
             * mask passes): the first one's whole bank, then for each of the next
             * the slots that differ from it. */
            fprintf(mf, "--- vp constants of depth-bounds draws (slots differing from the first) ---\n");
            const float* c0 = NULL;
            int nshown = 0;
            for (u32 d = 0; d < ndraws && d < MAX_DRAWS && nshown < 48; d++) {
                const D3D12DrawRecord* r = &s_d3d.draws[d];
                if (r->is_clear || !r->dbt) continue;
                const float* c = (const float*)((const char*)s_d3d.vp_cb_mapped
                    + ((u64)par * MAX_DRAWS + r->cb_slot) * VP_CB_STRIDE);
                fprintf(mf, "op%03u vs=%d n=%u sref=0x%02X\n", d, r->vs_idx, r->vertex_count,
                        (unsigned)r->stencil_ref);
                for (int sl = 0; sl < RSX_MAX_VERTEX_CONSTANTS; sl++) {
                    const float* v = c + sl * 4;
                    const int nz = v[0] || v[1] || v[2] || v[3];
                    if (!c0 ? nz : memcmp(v, c0 + sl * 4, 16) != 0)
                        fprintf(mf, "   c[%3d] = %9.4f %9.4f %9.4f %9.4f\n", sl, v[0], v[1], v[2], v[3]);
                }
                if (!c0) c0 = c;
                nshown++;
            }
        }
        fclose(mf);
    }

    /* Backbuffer (in PRESENT state at capture time) + every used offscreen RT. */
    snprintf(path, sizeof path, "%s/px.txt", dir);
    s_cap_px = fopen(path, "w");
    snprintf(path, sizeof path, "%s/backbuffer.bmp", dir);
    cap_readback_write(s_d3d.render_targets[fi], s_d3d.pw, s_d3d.ph,
                       DXGI_FORMAT_R8G8B8A8_UNORM, D3D12_RESOURCE_STATE_PRESENT, fi, path);
    for (int i = 0; i < MAX_OFF_RTS; i++) {
        OffRT* r = &s_d3d.off_rt[i];
        if (!r->res || !r->used) continue;
        snprintf(path, sizeof path, "%s/rt_%08X.bmp", dir, r->off);
        cap_readback_write(r->res, r->pw, r->ph, r->dxgi, r->st, fi, path);
    }
    /* RSX_CAP_MEM=<ea>:<bytes>[,...]: those guest memory ranges as they are
     * (mem_<ea>.bin), e.g. the CPU-written tables a frame samples. */
    { const char* e = getenv("RSX_CAP_MEM");
      extern uint8_t* vm_base;
      while (e && *e && vm_base) {
          char* end = NULL;
          const unsigned long ea = strtoul(e, &end, 16);
          if (end == e || *end != ':') break;
          const unsigned long len = strtoul(end + 1, &end, 0);
          if (len && len <= (64ul << 20)) {
              snprintf(path, sizeof path, "%s/mem_%08lX.bin", dir, ea);
              FILE* f = fopen(path, "wb");
              if (f) { fwrite(vm_base + ea, 1, len, f); fclose(f); }
          }
          e = (*end == ',') ? end + 1 : NULL;
      } }
    if (cap_raw_on())
        for (int i = 0; i < MAX_OFF_DS; i++) {
            if (!s_d3d.off_ds[i].res) continue;
            snprintf(path, sizeof path, "%s/ds_%08X", dir, s_d3d.off_ds[i].off);
            cap_ds_write(&s_d3d.off_ds[i], fi, path);
        }
    cap_dump_snaps(fi, dir);
    if (s_cap_px) { fclose(s_cap_px); s_cap_px = NULL; }
    printf("[RSXCAP] frame %llu -> %s (%u ops)\n",
           (unsigned long long)s_d3d.frame_count, dir, ndraws);
}

/* Decompile the captured RSX vertex program to HLSL and build the VP PSO
 * (decompiled VS + atlas alpha-test PS). One-shot per program. */
static void compile_vp(void)
{
    extern int rsx_vp_decompile(const uint8_t*, u32, char*, u32);
    const rsx_state* st = s_d3d.current_rsx_state;
    if (!st || st->vp_ucode_bytes < 16 || !s_d3d.vp_root_sig) return;

    static char hlsl[64 * 1024];
    extern void rsx_vp_set_branch_base(u32);
    rsx_vp_set_branch_base(0);      /* from slot 0, as below from the program's start */
    int ni = rsx_vp_decompile(st->vp_ucode, st->vp_ucode_bytes, hlsl, sizeof hlsl);
    /* Does this VP read vp_c[0..3]? Gates the garbage-projection fallback:
     * programs keeping their MVP elsewhere (gcm/cube: c[256..259]) must not
     * have c[0..3] stomped nor the viewport z-lane overridden. */
    s_d3d.vp_uses_c03 = (strstr(hlsl, "vp_c[0]") || strstr(hlsl, "vp_c[1]") ||
                         strstr(hlsl, "vp_c[2]") || strstr(hlsl, "vp_c[3]")) ? 1 : 0;
    if (ni <= 0) { printf("[VP] decompile failed (%d)\n", ni); s_d3d.vp_compiled_bytes = st->vp_ucode_bytes; return; }
    if (getenv("VP_DUMP")) {
        FILE* vf = fopen("vp_dump.hlsl", "w");
        if (vf) { fwrite(hlsl, 1, strlen(hlsl), vf); fclose(vf);
                  printf("[VP] dumped vp_dump.hlsl (%d instrs)\n", ni); }
        printf("[VPRAW] first 3 instrs (%u ucode bytes):\n", st->vp_ucode_bytes);
        for (u32 _q = 0; _q < 48 && _q < st->vp_ucode_bytes; _q += 16)
            printf("[VPRAW]  d0=%02X%02X%02X%02X d1=%02X%02X%02X%02X d2=%02X%02X%02X%02X d3=%02X%02X%02X%02X\n",
                st->vp_ucode[_q+0],st->vp_ucode[_q+1],st->vp_ucode[_q+2],st->vp_ucode[_q+3],
                st->vp_ucode[_q+4],st->vp_ucode[_q+5],st->vp_ucode[_q+6],st->vp_ucode[_q+7],
                st->vp_ucode[_q+8],st->vp_ucode[_q+9],st->vp_ucode[_q+10],st->vp_ucode[_q+11],
                st->vp_ucode[_q+12],st->vp_ucode[_q+13],st->vp_ucode[_q+14],st->vp_ucode[_q+15]);
    }

    /* Pixel shader mirrors dbgfont's FP: sample the atlas coverage at TEXCOORD0
     * (VP output o[7]), alpha-test at 0.5, output the vertex color (o[1]). */
    static const char ps[] =
        "Texture2D tex : register(t0); SamplerState smp : register(s0);\n"
        "struct PSIn{ float4 pos:SV_Position; float4 col0:COLOR0; float4 col1:COLOR1; float4 fog:FOG;\n"
        "  float4 t0:TEXCOORD0; float4 t1:TEXCOORD1; float4 t2:TEXCOORD2; float4 t3:TEXCOORD3;\n"
        "  float4 t4:TEXCOORD4; float4 t5:TEXCOORD5; float4 t6:TEXCOORD6; float4 t7:TEXCOORD7; };\n"
        "float4 main(PSIn i):SV_TARGET{ float cov = tex.Sample(smp, i.t0.xy).r;\n"
        "  if (cov <= 0.5) discard; return float4(i.col0.rgb, 1); }\n";

    ID3DBlob *vb=NULL,*pb=NULL,*e=NULL;
    /* Through the guest programs' compiler: this vertex shader goes on to pair
     * with them (vp_vs_blob), and the two pixel shaders here with it. */
    HRESULT hr = d3d_compile_cached(hlsl, strlen(hlsl), "vp", "vs_5_0", &vb, &e);
    if (FAILED(hr)) { printf("[VP] VS compile FAIL: %s\n", e?(const char*)e->lpVtbl->GetBufferPointer(e):"?"); if(e)e->lpVtbl->Release(e); s_d3d.vp_compiled_bytes=st->vp_ucode_bytes; return; }
    hr = d3d_compile_cached(ps, sizeof(ps)-1, "vpps", "ps_5_0", &pb, &e);
    if (FAILED(hr)) { printf("[VP] PS compile FAIL: %s\n", e?(const char*)e->lpVtbl->GetBufferPointer(e):"?"); if(e)e->lpVtbl->Release(e); if(vb)vb->lpVtbl->Release(vb); s_d3d.vp_compiled_bytes=st->vp_ucode_bytes; return; }

    /* The decompiled VS declares inputs a0:ATTR0 .. a15:ATTR15 and the HLSL
     * compiler keeps whichever the program body reads. D3D12 requires EVERY VS
     * input to have a matching input-layout element (by semantic name+index),
     * so declare all 16 ATTR slots. (The old single "POSITION" element matched
     * NO VS input once the decompiler switched to ATTR semantics -> PSO failed
     * with E_INVALIDARG for every VP, blanking cellmark's text and vkcube.)
     * The VP path uploads only attrib0 to vp_vb (one float4/vertex), so every
     * slot reads that same float4 at offset 0; attributes other than position
     * therefore alias attrib0 for now (colours/uv wrong until the VP path
     * uploads multiple attributes), but geometry is correct and the PSO is
     * valid. */
    D3D12_INPUT_ELEMENT_DESC il[16];
    for (int _e = 0; _e < 16; _e++) {
        il[_e].SemanticName = "ATTR";
        il[_e].SemanticIndex = _e;
        il[_e].Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
        il[_e].InputSlot = 0;
        /* 256-byte generic VP vertex: every ATTRi is a float4 slot at i*16
         * (read_vp_vertex converts each enabled RSX attrib by type; disabled
         * slots hold (0,0,0,1)). Covers tiny3d (a0/a3/a8), SDK gcm samples
         * (a0/a1/a2), dbgfont -- no aliasing. */
        il[_e].AlignedByteOffset = (UINT)(_e * 16);
        il[_e].InputSlotClass = D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA;
        il[_e].InstanceDataStepRate = 0;
    }
    D3D12_GRAPHICS_PIPELINE_STATE_DESC pd = {0};
    pd.pRootSignature = s_d3d.vp_root_sig;
    pd.VS.pShaderBytecode = vb->lpVtbl->GetBufferPointer(vb); pd.VS.BytecodeLength = vb->lpVtbl->GetBufferSize(vb);
    pd.PS.pShaderBytecode = pb->lpVtbl->GetBufferPointer(pb); pd.PS.BytecodeLength = pb->lpVtbl->GetBufferSize(pb);
    pd.InputLayout.pInputElementDescs = il; pd.InputLayout.NumElements = 16;
    pd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    pd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    pd.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    pd.SampleMask = UINT_MAX;
    pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pd.NumRenderTargets = 1; pd.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
    pd.DSVFormat = DXGI_FORMAT_D24_UNORM_S8_UINT;
    /* Depth test ON (LESS_EQUAL, matching the guest's rsxtiny_DepthTestFunc 515 /
     * CELL_GCM_LEQUAL). Without it the cube's faces drew in submission order and
     * back faces overwrote the front -> you saw through the near face to the
     * darker interior. The depth buffer is bound + cleared to 1.0 each frame. */
    pd.DepthStencilState.DepthEnable = TRUE;
    pd.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
    pd.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;
    pd.DepthStencilState.StencilEnable = FALSE;
    pd.SampleDesc.Count = 1;
    hr = s_d3d.device->lpVtbl->CreateGraphicsPipelineState(s_d3d.device, &pd, &IID_ID3D12PipelineState, (void**)&s_d3d.pipeline_state_vp);

    /* Second PSO: same VS but a COLOUR-only PS (no texture sample, no alpha
     * discard) for untextured 3D geometry (vkcube's cube). The atlas PS above
     * samples a texture and discards where coverage<=0.5, which blanks any draw
     * with no atlas bound. */
    {
        static const char ps_col[] =
            "struct PSIn{ float4 pos:SV_Position; float4 col0:COLOR0; float4 col1:COLOR1; float4 fog:FOG;\n"
            "  float4 t0:TEXCOORD0; float4 t1:TEXCOORD1; float4 t2:TEXCOORD2; float4 t3:TEXCOORD3;\n"
            "  float4 t4:TEXCOORD4; float4 t5:TEXCOORD5; float4 t6:TEXCOORD6; float4 t7:TEXCOORD7; };\n"
            "float4 main(PSIn i):SV_TARGET{ return float4(i.col0.rgb, 1); }\n";
        ID3DBlob *pcb=NULL,*ec=NULL;
        if (SUCCEEDED(d3d_compile_cached(ps_col, sizeof(ps_col)-1, "vppsc", "ps_5_0", &pcb, &ec)) && pcb) {
            pd.PS.pShaderBytecode = pcb->lpVtbl->GetBufferPointer(pcb);
            pd.PS.BytecodeLength  = pcb->lpVtbl->GetBufferSize(pcb);
            HRESULT hr2 = s_d3d.device->lpVtbl->CreateGraphicsPipelineState(
                s_d3d.device, &pd, &IID_ID3D12PipelineState,
                (void**)&s_d3d.pipeline_state_vp_color);
            printf(SUCCEEDED(hr2) ? "[VP] colour pipeline ready\n"
                                  : "[VP] colour PSO FAIL (0x%08lX)\n", hr2);
            pcb->lpVtbl->Release(pcb);
        }
        if (ec) ec->lpVtbl->Release(ec);
    }
    /* Keep the VS bytecode for guest-FP PSO builds (vp_get_fp_pso); bump the
     * generation so cached FP PSOs built against the old VS are rebuilt. */
    if (s_d3d.vp_vs_blob) s_d3d.vp_vs_blob->lpVtbl->Release(s_d3d.vp_vs_blob);
    s_d3d.vp_vs_blob = vb;
    s_d3d.vp_gen++;
    pb->lpVtbl->Release(pb);
    s_d3d.vp_compiled_bytes = st->vp_ucode_bytes;
    if (SUCCEEDED(hr)) { s_d3d.vp_ready = 1; printf("[VP] pipeline ready (%d instrs)\n", ni); }
    else {
        printf("[VP] PSO creation FAIL (0x%08lX)\n", hr);
        /* Drain the debug layer's message queue to get the EXACT validation
         * reason (E_INVALIDARG is otherwise opaque). One-shot. */
        ID3D12InfoQueue* iq = NULL;
        if (SUCCEEDED(s_d3d.device->lpVtbl->QueryInterface(
                s_d3d.device, &IID_ID3D12InfoQueue, (void**)&iq)) && iq) {
            UINT64 n = iq->lpVtbl->GetNumStoredMessages(iq);
            for (UINT64 mi = 0; mi < n; mi++) {
                SIZE_T len = 0;
                iq->lpVtbl->GetMessage(iq, mi, NULL, &len);
                D3D12_MESSAGE* m = (D3D12_MESSAGE*)malloc(len);
                if (m && SUCCEEDED(iq->lpVtbl->GetMessage(iq, mi, m, &len)))
                    printf("[VP][DBG] %s\n", m->pDescription);
                free(m);
            }
            iq->lpVtbl->Release(iq);
        }
    }
}

/* Build (or fetch) a PSO pairing the current decompiled VS with the guest's
 * FRAGMENT program at fp_addr: read the FP ucode from guest memory, decompile
 * to HLSL (rsx_fp_decompiler), compile, and cache. This replaces the two
 * hardcoded pixel shaders for draws whose FP we can translate -- e.g.
 * gcm/cube's plasma FP `c = tex2D(t0, uv).x; out = (c, 0, c, 1)`. */
#include "rsx_fp_decompiler.h"

/* Hash + compile the CURRENT rsx_state vertex program into the VS cache;
 * returns the cache slot (or -1). Called at draw-record time so each draw
 * carries the VP that was loaded when it was submitted. */
static u32 vp_hash_ucode(const u8* p, u32 n)
{
    u32 h = 2166136261u;
    for (u32 i = 0; i < n; i++) { h ^= p[i]; h *= 16777619u; }
    return h ? h : 1u;
}

static int vp_get_vs(const rsx_state* st)
{
    extern int rsx_vp_decompile(const uint8_t*, u32, char*, u32);
    if (!st || st->vp_ucode_bytes < 16) return -1;
    /* The program changes only when the title loads microcode or moves the
     * start slot, and both set vp_dirty; until then the last lookup stands,
     * rather than hashing the program (kilobytes, a byte at a time) again for
     * every draw record. The raw-fetch path looks it up once more per record. */
    static const rsx_state* memo_st;
    static int memo_idx = -1;
    static u32 memo_hash;
    if (st == memo_st && !st->vp_dirty && memo_idx >= 0 && memo_idx < s_d3d.vp_vs_n &&
        s_d3d.vp_vs[memo_idx].hash == memo_hash) {
        s_d3d.vp_vs[memo_idx].last_use = s_d3d.frame_count;
        return memo_idx;
    }
    /* Start at the instruction SET_TRANSFORM_PROGRAM_START selects, not at 0.
     * The microcode store holds every resident program -- this title keeps its
     * scene, fluid, caustics and droplet programs in it at once -- so
     * decompiling from 0 ran the scene's transform for every draw. That is why
     * the fluid's 114300 vertices rasterized nothing: they were being pushed
     * through the wrong program.
     * VP_START_OFF=1 restores the old always-from-zero behaviour. */
    u32 vstart = 0;
    { static int off = -1; if (off < 0) off = getenv("VP_START_OFF") ? 1 : 0;
      if (!off) vstart = st->transform_program_start * 16u; }
    if (vstart >= st->vp_ucode_bytes) vstart = 0;
    const u8* vuc = st->vp_ucode + vstart;
    u32 vlen = st->vp_ucode_bytes - vstart;
    /* Hash the program's own bytes, up to its end bit, not everything behind it in the store (as the Metal
     * backend does). A title that keeps several programs resident and uploads another one behind the program it
     * draws with changed that hash on every upload, so nearly every draw missed the cache and ran D3DCompile. */
    { extern u32 rsx_vp_program_size_instrs(const u8*, u32);
      const u32 instrs = rsx_vp_program_size_instrs(vuc, vlen);
      if (instrs) vlen = instrs * 16u; }
    u32 hash = vp_hash_ucode(vuc, vlen);
    ((rsx_state*)st)->vp_dirty = 0;
    memo_st = st; memo_hash = hash; memo_idx = -1;
    for (int i = 0; i < s_d3d.vp_vs_n; i++)
        if (s_d3d.vp_vs[i].hash == hash) { memo_idx = i; s_d3d.vp_vs[i].last_use = s_d3d.frame_count; return i; }

    static char hlsl[262144];
    /* A branch's target is a slot of the whole store, the program's blocks count from its start: the
     * decompiler takes that start as its branch base. Left at 0, a branching program loaded past slot
     * 0 jumped to blocks it does not have, and its loop ran its 16384 rounds for every vertex.
     * VP_BRANCH_BASE0=1: the base at 0, as before. */
    { extern void rsx_vp_set_branch_base(u32);
      static int base0 = -1;
      if (base0 < 0) base0 = getenv("VP_BRANCH_BASE0") ? 1 : 0;
      rsx_vp_set_branch_base(base0 ? 0u : vstart / 16u); }
    int ni = rsx_vp_decompile(vuc, vlen, hlsl, sizeof hlsl);
    if (ni <= 0) return -1;
    if (getenv("VP_DUMP")) { static int _d=0; if (PS3_LOG_FIRST(_d, 4)) {
        FILE* f = fopen("vp2_dump.hlsl", _d==1 ? "w" : "a");
        if (f) { fprintf(f, "/* per-draw VS hash pending, %d instrs */%s%s", ni, hlsl, "\n"); fclose(f); } } }
    /* RSX_DBG_VP=<hex tex0 offset>: write the vertex program belonging to the draws
     * that bind that texture. vp2_dump.hlsl holds whichever four programs were
     * compiled first, which need not include the one under investigation -- and
     * reading the wrong program sends you chasing the wrong constants. */
    { static const char* dp = (const char*)1; static u32 wantp = 0; static int done = 0;
      if (dp == (const char*)1) { dp = getenv("RSX_DBG_VP");
                                  wantp = dp ? (u32)strtoul(dp, NULL, 16) : 0; }
      if (wantp && !done && s_d3d.cur_texs[0].raw == wantp) {
          done = 1;
          FILE* f = fopen("duck_vp.hlsl", "w");
          if (f) { fprintf(f, "/* duck VP, %d instrs, tex0=0x%X */%s", ni, wantp, hlsl);
                   fclose(f); }
          fprintf(stderr, "[DUCKVP] wrote duck_vp.hlsl (%d instrs)%c", ni, 10);
      } }
    /* VP_BYPASS=1: replace the guest transform with a direct map of attribute 0
     * into clip space. If geometry appears with this on, everything from the
     * input layout through raster/depth/present is sound and the fault is the
     * transform (constants or the decompiled program); if it stays blank, the
     * fault is upstream of the shader -- the attribute binding itself. Patched
     * in place, space-padded to the original length so offsets stay valid. */
    /* VP_W_ONE=1: keep the guest transform but force the clip-space w to 1.
     * Different from VP_BYPASS, which replaces the whole position: this tests
     * only the hypothesis that w is wrong. This title's VP ends
     *   { float4 _v = (float4)((vp_c[467]).xxxx); o[0].w = _v.w; }
     * with c467 = (-1, 1, 0, 0), so w = -1 and the clipper discards every
     * vertex. Patched in place, space-padded so offsets stay valid. */
    if (getenv("VP_W_ONE")) {
        static const char anchor[] = "o[0].w = _v.w;";
        static const char repl[]   = "o[0].w = 1.0;";
        char* at = hlsl;
        while ((at = strstr(at, anchor)) != NULL) {
            size_t n = sizeof(anchor) - 1, m = sizeof(repl) - 1;
            memcpy(at, repl, m);
            memset(at + m, ' ', n - m);
            at += n;
        }
    }
    if (getenv("VP_BYPASS")) {
        static const char anchor[] =
            "Out.pos = float4(_p.xyz * vp_posscale.xyz + _p.w * vp_posoffset.xyz, _p.w);";
        static const char repl[] = "Out.pos = float4(v[0].xy * 0.5, 0.5, 1.0);";
        char* at = hlsl;
        while ((at = strstr(at, anchor)) != NULL) {
            size_t n = sizeof(anchor) - 1, m = sizeof(repl) - 1;
            memcpy(at, repl, m);
            memset(at + m, ' ', n - m);
            at += n;
        }
    }
    /* display.ultrawide, the screen's edges: the title keeps its 2D inside the 16:9 in the middle, so a fade, a backdrop
     * or a cutscene's bars would stop there with the scene going on beside them. A flat vertex at or past that edge goes
     * as far past the screen's when the draw asks for it (vp_posoffset.w, vp_record_cb); inside the 16:9 nothing moves. */
    if (s_wide != 1.0f) {
        static const char anchor[] = "Out.pos = float4(_p.xyz * vp_posscale.xyz + _p.w * vp_posoffset.xyz, _p.w);";
        char add[256];
        const int n = snprintf(add, sizeof add,
                               " if (vp_posoffset.w != 0.0 && abs(_p.w - 1.0) < 0.001 && abs(Out.pos.x) >= %f && abs(Out.pos.x) < 0.999)"
                               " Out.pos.x *= %f;", 1.0f / s_wide - 0.002f, s_wide);
        char* at = strstr(hlsl, anchor);
        if (at && n > 0 && n < (int)sizeof add && strlen(hlsl) + (size_t)n < sizeof hlsl) {
            at += sizeof anchor - 1;
            memmove(at + n, at, strlen(at) + 1);
            memcpy(at, add, (size_t)n);
        }
    }
    /* A worker builds it unless the disk cache has it (D3D12_ASYNC_SHADERS);
     * the draws that use it wait for it in vp_get_fp_pso. */
    ID3DBlob* vb = NULL;
    ShaderJob* vjob = NULL;
    if (sj_workers() > 0) {
        vb = shc_find(hlsl, strlen(hlsl), "vs_5_0");
        if (!vb) vjob = sj_submit(hlsl, strlen(hlsl), "guest_vp2", "vs_5_0", NULL, NULL, 0);
    }
    if (!vb && !vjob) {
        ID3DBlob* e = NULL;
        HRESULT hr = d3d_compile_cached(hlsl, strlen(hlsl), "guest_vp2", "vs_5_0", &vb, &e);
        if (e) e->lpVtbl->Release(e);
        if (FAILED(hr) || !vb) {
            static int _e=0; if (PS3_LOG_FIRST(_e, 4)) printf("[VP] per-draw VS compile FAIL (hash=0x%08X)\n", hash);
            return -1;
        }
    }
    int slot;
    if (s_d3d.vp_vs_n < VP_VS_CACHE) slot = s_d3d.vp_vs_n++;
    else {
        /* Replace the least recently used program in its own slot. Not one a
         * draw of this frame or the last was recorded with: that draw replays
         * with whatever its vs_idx then holds. */
        const u64 fc = s_d3d.frame_count;
        slot = -1;
        for (int i = 0; i < VP_VS_CACHE; i++)
            if (fc - s_d3d.vp_vs[i].last_use >= 2 &&
                (slot < 0 || s_d3d.vp_vs[i].last_use < s_d3d.vp_vs[slot].last_use)) slot = i;
        if (slot < 0) {
            static int _f = 0;
            if (PS3_LOG_FIRST(_f, 4))
                fprintf(stderr, "[VP] all %d cached vertex programs drawn with in two frames; not cached%c",
                        VP_VS_CACHE, 10);
            if (vb) vb->lpVtbl->Release(vb);
            sj_release(vjob);
            return -1;
        }
        VPVSEntry* o = &s_d3d.vp_vs[slot];
        if (o->vs) o->vs->lpVtbl->Release(o->vs);
        if (o->vs_raw) o->vs_raw->lpVtbl->Release(o->vs_raw);
        sj_release(o->job);
        sj_release(o->raw_job);
        free(o->hlsl);
        s_perf_vs_replaced++;
    }
    s_d3d.vp_vs[slot].hash = hash;
    s_d3d.vp_vs[slot].last_use = s_d3d.frame_count;
    s_d3d.vp_vs[slot].vs   = vb;
    s_d3d.vp_vs[slot].job  = vjob;
    s_d3d.vp_vs[slot].raw_job = NULL;
    { const size_t hn = strlen(hlsl) + 1;
      s_d3d.vp_vs[slot].hlsl = (char*)malloc(hn);
      if (s_d3d.vp_vs[slot].hlsl) memcpy(s_d3d.vp_vs[slot].hlsl, hlsl, hn); }
    s_d3d.vp_vs[slot].vs_raw = NULL;
    s_d3d.vp_vs[slot].raw_failed = 0;
    s_d3d.vp_vs[slot].uses_c03 =
        (strstr(hlsl, "vp_c[0]") || strstr(hlsl, "vp_c[1]") ||
         strstr(hlsl, "vp_c[2]") || strstr(hlsl, "vp_c[3]")) ? 1 : 0;
    { char k[24];
      snprintf(k, sizeof k, "vp_c[%d]", s_fg.cam_reg);
      s_d3d.vp_vs[slot].uses_cam = s_fg.cam_reg >= 0 && strstr(hlsl, k) != NULL;
      snprintf(k, sizeof k, "vp_c[%d]", s_fg.prev_reg);
      s_d3d.vp_vs[slot].uses_prev = s_fg.prev_reg >= 0 && strstr(hlsl, k) != NULL; }
    { static int _n=0; if (PS3_LOG_FIRST(_n, 6)) printf("[VP] per-draw VS cached (hash=0x%08X, %d instrs, slot %d)\n", hash, ni, slot); }
    memo_idx = slot;
    /* Build the base VP pipeline here if it does not exist yet. render_frame's
     * trigger reads s_d3d.current_rsx_state, which by frame end no longer has
     * the microcode -- so for a title that only ever compiles per-draw VS the
     * base pipeline was never built, s_d3d.vp_ready stayed 0, and the VP draw
     * pass it gates dropped every recorded is_vp draw. Here we are at RECORD
     * time with a live state that definitely has microcode. */
    if (!s_d3d.vp_ready && st && st->vp_ucode_bytes >= 16) {
        const rsx_state* prev = s_d3d.current_rsx_state;
        s_d3d.current_rsx_state = st;
        rt_excl_begin();   /* RSX_RENDER_THREAD: not while the render thread records */
        compile_vp();
        rt_excl_end();
        s_d3d.current_rsx_state = prev;
    }
    return slot;
}

/* Raw-fetch variant of a cached vertex program (see upload_tris_vp_raw). The
 * decompiled VS takes its 16 attributes as float4 inputs; this variant takes
 * SV_VertexID, reads the guest's big-endian vertex bytes from vp_raw at the
 * bases in the batch's segment header (b2) plus index * stride, and converts
 * them as rsx_vertex_fetch.c does. vp_fa[i] = (0, stride, type | comps << 4,
 * enabled) and vp_fk[i] = the attribute's constant register, which also fills
 * the components past comps. Same text otherwise, so the two variants run the
 * same program. */
static const char s_vp_raw_hlsl[] =
    "ByteAddressBuffer vp_raw : register(t8);\n"
    "cbuffer VPRawSeg : register(b2) { uint vp_seg; };\n"
    "uint vp_ld(uint a) {\n"
    "    uint b = a & ~3u; uint s = (a & 3u) * 8u; uint lo = vp_raw.Load(b);\n"
    "    if (s == 0u) return lo;\n"
    "    return (lo >> s) | (vp_raw.Load(b + 4u) << (32u - s));\n"
    "}\n"
    "uint vp_be32(uint a) { uint x = vp_ld(a);\n"
    "    return (x << 24) | ((x << 8) & 0xFF0000u) | ((x >> 8) & 0xFF00u) | (x >> 24); }\n"
    "uint vp_be16(uint a) { uint x = vp_ld(a); return ((x & 0xFFu) << 8) | ((x >> 8) & 0xFFu); }\n"
    "int vp_s16(uint a) { return ((int)(vp_be16(a) << 16)) >> 16; }\n"
    "float vp_half(uint h) {\n"
    "    uint s = (h >> 15) << 31; uint e = (h >> 10) & 31u; uint m = h & 1023u;\n"
    "    uint f = (e == 0u) ? s : ((e == 31u) ? (s | 0x7F800000u | (m << 13))\n"
    "                                         : (s | ((e + 112u) << 23) | (m << 13)));\n"
    "    return asfloat(f);\n"
    "}\n"
    /* vp_vs_raw calls this from one loop over the inputs the program reads.
     * Inlined once per input, with the format switch unrolled over four
     * components, the shader was ~54 KB of bytecode and FXC took 0.7-0.9 s
     * over it, a stall on the drain thread at the program's first draw; from
     * the loop, ~22-25 KB and 0.4-0.45 s. The components stay unrolled: a
     * second loop over them, writing a local array, compiled faster again
     * but drew some programs wrong (God of War HD's HUD shrank to a few
     * pixels), two nested loops over indexable temporaries. */
    "float4 vp_fetch(uint i, uint vid) {\n"
    "    float4 r = vp_fk[i];\n"
    "    uint4 d = vp_fa[i];\n"
    "    if (d.w == 0u) return r;\n"
    "    uint a = vp_raw.Load(vp_seg + i * 4u) + vid * d.y;\n"
    "    uint t = d.z & 15u; uint c = d.z >> 4;\n"
    "    if (t == 6u) {\n"
    "        uint w = vp_be32(a);\n"
    "        return float4((float)(((int)(w << 21)) >> 21) / 1023.0,\n"
    "                      (float)(((int)(w << 10)) >> 21) / 1023.0,\n"
    "                      (float)(((int)w) >> 22) / 511.0, 1.0);\n"
    "    }\n"
    "    [unroll] for (uint k = 0u; k < 4u; k++) {\n"
    "        if (k < c) {\n"
    "            float f;\n"
    "            if (t == 3u)      f = vp_half(vp_be16(a + k * 2u));\n"
    "            else if (t == 4u) f = (float)(vp_ld(a + k) & 0xFFu) / 255.0;\n"
    "            else if (t == 7u) f = (float)(vp_ld(a + k) & 0xFFu);\n"
    "            else if (t == 1u) f = (float)vp_s16(a + k * 2u) / 32767.0;\n"
    "            else if (t == 5u) f = (float)vp_s16(a + k * 2u);\n"
    "            else              f = asfloat(vp_be32(a + k * 4u));\n"
    "            r[k] = f;\n"
    "        }\n"
    "    }\n"
    "    return r;\n"
    "}\n";

/* The entry's vertex shader, taking over a finished worker build: NULL while
 * it builds (within the wait budget) or if it failed. */
static ID3DBlob* vp_vs_ready(VPVSEntry* e)
{
    if (!e->job) return e->vs;
    if (!sj_wait(e->job)) return NULL;
    ShaderJob* j = e->job;
    e->job = NULL;
    if (SUCCEEDED(j->hr) && j->blob) { e->vs = j->blob; j->blob = NULL; }
    else { static int _e = 0;
           if (PS3_LOG_FIRST(_e, 4))
               printf("[VP] per-draw VS compile FAIL (hash=0x%08X): %s\n", e->hash, j->err); }
    sj_release(j);
    return e->vs;
}

/* The raw-fetch variant's source, malloc'ed, or NULL. */
static char* vp_raw_text(const char* src, size_t* len)
{
    static const char cb_end[] = "    float4 vp_posoffset;\n};\n";
    static const char v_last[] = "v[15]=input.a15;\n";
    static const char cb_raw[] = "    float4 vp_posoffset;\n    uint4 vp_fa[16];\n    float4 vp_fk[16];\n};\n";
    static const char in_raw[] = "struct VSInput { uint vid:SV_VertexID; };\n";
    const char* in0 = strstr(src, "struct VSInput {");
    const char* in1 = in0 ? strstr(in0, "};\n") : NULL;
    const char* cb  = strstr(src, cb_end);
    const char* v0  = strstr(src, "    v[0]=input.a0;");
    const char* v1  = v0 ? strstr(v0, v_last) : NULL;
    if (!in1 || !cb || !v1 || in1 > cb || cb > v0) return NULL;
    char* out = (char*)malloc(strlen(src) + sizeof s_vp_raw_hlsl + 2048);
    if (!out) return NULL;
    size_t o = 0;
#define VP_RAW_PUT(s, n) do { memcpy(out + o, (s), (n)); o += (n); } while (0)
    VP_RAW_PUT(src, (size_t)(in0 - src));
    VP_RAW_PUT(in_raw, sizeof in_raw - 1);
    in1 += 3;
    VP_RAW_PUT(in1, (size_t)(cb - in1));
    VP_RAW_PUT(cb_raw, sizeof cb_raw - 1);
    VP_RAW_PUT(s_vp_raw_hlsl, sizeof s_vp_raw_hlsl - 1);
    cb += sizeof cb_end - 1;
    VP_RAW_PUT(cb, (size_t)(v0 - cb));
    v1 += sizeof v_last - 1;
    /* Fetch the inputs the program reads (the decompiler names them with
     * constant indexes, v[3]), in one loop, so FXC sees vp_fetch once. */
    {
        char list[128] = "", line[256];
        int nused = 0;
        for (int i = 0; i < 16; i++) {
            char tok[8];
            snprintf(tok, sizeof tok, "v[%d]", i);
            int used = 0;
            for (const char* p = strstr(v1, tok); p && !used; p = strstr(p + 1, tok)) {
                const char c = p == v1 ? ' ' : p[-1];   /* not the tail of another name */
                used = !((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_');
            }
            if (used) {
                const size_t l = strlen(list);
                snprintf(list + l, sizeof list - l, "%s%du", nused ? ", " : "", i);
                nused++;
            }
        }
        static const char init[] = "    [unroll] for (uint _q = 0u; _q < 16u; _q++) v[_q] = vp_fk[_q];\n";
        VP_RAW_PUT(init, sizeof init - 1);
        if (nused) {
            const int n = snprintf(line, sizeof line,
                "    { static const uint vp_used[%d] = { %s };\n"
                "      [loop] for (uint _u = 0u; _u < %du; _u++) { const uint _i = vp_used[_u];"
                " v[_i] = vp_fetch(_i, input.vid); } }\n", nused, list, nused);
            VP_RAW_PUT(line, (size_t)n);
        }
    }
    VP_RAW_PUT(v1, strlen(v1));
#undef VP_RAW_PUT
    out[o] = 0;
    *len = o;
    return out;
}

/* ---------------------------------------------------------------------------
 * Vertex programs compiled ahead of play (rsx_d3d12_precompile_vertex_programs)
 *
 * A vertex program's text depends on its code alone: where in the store the
 * title loads it changed the text of none of the 102 that a session of God of
 * War: Ascension's first level drew with, and its raw-fetch text is made from
 * that one. So a port can find the title's vertex programs in its files and
 * have both texts compiled into the shader cache before the title runs; none is
 * then compiled in play, where a new one holds back the draws that need it (a
 * raw-fetch text took 127-149 ms in DXC here). A fragment program's text also
 * depends on the textures and state of its draws, which the files do not say.
 * -----------------------------------------------------------------------*/
typedef struct { char* src; size_t len; const char* name; } PcJob;
static PcJob* s_pc_jobs;
static LONG s_pc_n;
static volatile LONG s_pc_next, s_pc_done, s_pc_failed;

static DWORD WINAPI pc_worker(LPVOID arg)
{
    (void)arg;
    ps3_thread_name("precompile");
    for (;;) {
        const LONG i = InterlockedIncrement(&s_pc_next) - 1;
        if (i >= s_pc_n) break;
        ID3DBlob* b = NULL;
        ID3DBlob* e = NULL;
        const HRESULT hr = d3d_compile_cached(s_pc_jobs[i].src, s_pc_jobs[i].len, s_pc_jobs[i].name, "vs_5_0", &b, &e);
        if (FAILED(hr) || !b) InterlockedIncrement(&s_pc_failed);
        if (b) b->lpVtbl->Release(b);
        if (e) e->lpVtbl->Release(e);
        InterlockedIncrement(&s_pc_done);
    }
    return 0;
}

int rsx_d3d12_precompile_open(char* why, size_t why_len)
{
    if (!s_d3d.device) {
        IDXGIFactory4* f = NULL;
        const int ok = SUCCEEDED(CreateDXGIFactory1(&IID_IDXGIFactory4, (void**)&f)) && d3d_device_create(f) == 0;
        if (f) f->lpVtbl->Release(f);
        if (!ok) { snprintf(why, why_len, "no Direct3D 12 device"); return -1; }
    }
    AcquireSRWLockExclusive(&s_shc_lock);
    s_shc_fold_late = 1;
    if (s_shc_on < 0) shc_open();
    ReleaseSRWLockExclusive(&s_shc_lock);
    if (!s_shc_on) { snprintf(why, why_len, "no shader cache in the settings (graphics.shader_cache)"); return -1; }
    if (s_shc_ro) { snprintf(why, why_len, "the game has the shader cache open"); return -1; }
    return 0;
}

int rsx_d3d12_precompile_vertex_programs(const uint8_t* const* ucode, const uint32_t* bytes, uint32_t n,
                                         int threads, void (*progress)(uint32_t done, uint32_t total),
                                         uint32_t stats[3], char* why, size_t why_len)
{
    extern int rsx_vp_decompile(const uint8_t*, u32, char*, u32);
    extern void rsx_vp_set_branch_base(u32);
    stats[0] = stats[1] = stats[2] = 0;
    if (rsx_d3d12_precompile_open(why, why_len) != 0) return -1;

    /* The texts, on this thread (the decompiler's branch base is one for all):
     * each program's, at slot 0 as the title's draws had them, and its raw-fetch
     * one, those the cache has and repeats passed over. */
    const UINT flags = shc_flags();
    static char hlsl[262144];
    u32 maxb = 0;
    for (u32 i = 0; i < n; i++) if (bytes[i] > maxb) maxb = bytes[i];
    u32* words = (u32*)malloc((size_t)maxb + 16);
    s_pc_jobs = (PcJob*)calloc((size_t)n * 2 + 1, sizeof *s_pc_jobs);
    u32 kcap = 1024;
    while (kcap < n * 4 + 16) kcap *= 2;
    u64* keys = (u64*)calloc(kcap, sizeof *keys);   /* the batch's keys, for its repeats */
    if (!words || !s_pc_jobs || !keys) {
        free(words); free(s_pc_jobs); free(keys); s_pc_jobs = NULL;
        snprintf(why, why_len, "out of memory");
        return -1;
    }
    s_pc_n = 0;
    for (u32 i = 0; i < n; i++) {
        const u32 nw = bytes[i] / 4;
        for (u32 w = 0; w < nw; w++) {
            const uint8_t* p = ucode[i] + 4 * w;
            words[w] = (u32)p[0] << 24 | (u32)p[1] << 16 | (u32)p[2] << 8 | (u32)p[3];
        }
        rsx_vp_set_branch_base(0);
        if (rsx_vp_decompile((const u8*)words, nw * 4, hlsl, sizeof hlsl) <= 0) { stats[2]++; continue; }
        size_t rl = 0;
        char* raw = vp_raw_text(hlsl, &rl);
        for (int k = 0; k < 2; k++) {
            const char* s = k ? raw : hlsl;
            const size_t l = k ? rl : strlen(hlsl);
            if (!s) continue;
            const u64 key = shc_key(s, l, "vs_5_0", flags) ^ (u64)l * 0x9E3779B97F4A7C15ull;
            u32 slot = (u32)(key ^ (key >> 32)) & (kcap - 1);
            while (keys[slot] && keys[slot] != key) slot = (slot + 1) & (kcap - 1);
            if (keys[slot]) continue;   /* another program of the batch has this text */
            keys[slot] = key ? key : 1;
            AcquireSRWLockShared(&s_shc_lock);
            const int have = s_shc_cap && shc_slot(shc_key(s, l, "vs_5_0", flags), (u32)l)->prog;
            ReleaseSRWLockShared(&s_shc_lock);
            if (have) { stats[1]++; continue; }
            PcJob* j = &s_pc_jobs[s_pc_n++];
            j->len = l;
            j->name = k ? "guest_vp_raw" : "guest_vp2";
            j->src = (char*)malloc(l + 1);
            if (j->src) memcpy(j->src, s, l + 1);
            else s_pc_n--;
        }
        free(raw);
    }
    free(words);
    free(keys);

    /* Compiled as a draw's would be (d3d_compile_cached), on the threads. */
    const int nt = threads < 1 ? 1 : threads > 32 ? 32 : threads;
    HANDLE th[32];
    int nh = 0;
    s_pc_next = s_pc_done = s_pc_failed = 0;
    for (int t = 0; t < nt && s_pc_n; t++) {
        HANDLE h = CreateThread(NULL, 0, pc_worker, NULL, 0, NULL);
        if (h) th[nh++] = h;
    }
    if (nh) {
        while (WaitForMultipleObjects((DWORD)nh, th, TRUE, 250) == WAIT_TIMEOUT)
            if (progress) progress((u32)s_pc_done, (u32)s_pc_n);
        for (int t = 0; t < nh; t++) CloseHandle(th[t]);
    } else if (s_pc_n) {
        pc_worker(NULL);
    }
    if (progress) progress((u32)s_pc_n, (u32)s_pc_n);
    stats[0] = (u32)(s_pc_n - s_pc_failed);
    stats[2] += (u32)s_pc_failed;
    for (LONG i = 0; i < s_pc_n; i++) free(s_pc_jobs[i].src);
    free(s_pc_jobs);
    s_pc_jobs = NULL;

    /* Packed, on this thread: what was compiled, and whatever the cache was
     * waiting to pack (the files from before the store, a grown journal). */
    AcquireSRWLockExclusive(&s_shc_lock);
    if (stats[0] || s_shc_why) {
        u64 jl = 0;
        if (s_shc_file && fflush(s_shc_file) == 0 && _fseeki64(s_shc_file, 0, SEEK_END) == 0)
            jl = (u64)_ftelli64(s_shc_file);
        shc_fold_start(stats[0] ? "precompiled" : s_shc_why, s_shc_old_pso, jl, 1);
    }
    ReleaseSRWLockExclusive(&s_shc_lock);
    return 0;
}

static ID3DBlob* vp_vs_raw_locked(VPVSEntry* e)
{
    if (e->vs_raw || e->raw_failed || !e->hlsl) return e->vs_raw;
    /* A worker is building it: until then the draw takes the indexed path,
     * which needs no raw variant. */
    if (e->raw_job) {
        if (!sj_is_done(e->raw_job)) return NULL;
        ShaderJob* j = e->raw_job;
        e->raw_job = NULL;
        if (SUCCEEDED(j->hr) && j->blob) { e->vs_raw = j->blob; j->blob = NULL; }
        else { e->raw_failed = 1;
               static int _e = 0;
               if (PS3_LOG_FIRST(_e, 4))
                   printf("[VP] raw-fetch VS compile FAIL (hash=0x%08X): %s\n", e->hash, j->err); }
        sj_release(j);
        return e->vs_raw;
    }
    e->raw_failed = 1;                      /* until it compiles */
    size_t o = 0;
    char* out = vp_raw_text(e->hlsl, &o);
    if (!out) return NULL;
    ID3DBlob* vb = NULL; ID3DBlob* err = NULL;
    if (sj_workers() > 0) {
        vb = shc_find(out, o, "vs_5_0");
        if (vb) {
            free(out);
            e->vs_raw = vb;
            e->raw_failed = 0;
            return vb;
        }
        e->raw_job = sj_submit(out, o, "guest_vp_raw", "vs_5_0", NULL, NULL, 0);
        if (e->raw_job) {
            free(out);
            e->raw_failed = 0;
            return NULL;
        }
    }
    HRESULT hr = d3d_compile_cached(out, o, "guest_vp_raw", "vs_5_0", &vb, &err);
    if (FAILED(hr) || !vb) {
        static int _e = 0;
        if (PS3_LOG_FIRST(_e, 4))
            printf("[VP] raw-fetch VS compile FAIL (hash=0x%08X): %s\n", e->hash,
                   err ? (const char*)err->lpVtbl->GetBufferPointer(err) : "?");
        if (err) err->lpVtbl->Release(err);
        free(out);
        return NULL;
    }
    if (err) err->lpVtbl->Release(err);
    free(out);
    e->vs_raw = vb;
    e->raw_failed = 0;
    return vb;
}

/* The walk asks for an entry's raw-fetch variant when it records a draw, the
 * render thread (RSX_RENDER_THREAD) when it builds the draw's pipeline: the
 * variant's build is one at a time. Once built it stays until the entry is
 * replaced, which no draw of this frame or the last can see (vp_get_vs). */
static SRWLOCK s_vs_lock = SRWLOCK_INIT;
static ID3DBlob* vp_vs_raw(int vs_idx)
{
    if (vs_idx < 0 || vs_idx >= s_d3d.vp_vs_n) return NULL;
    VPVSEntry* e = &s_d3d.vp_vs[vs_idx];
    ID3DBlob* r = e->vs_raw;
    if (r) return r;
    AcquireSRWLockExclusive(&s_vs_lock);
    r = vp_vs_raw_locked(e);
    ReleaseSRWLockExclusive(&s_vs_lock);
    return r;
}

/* --- Guest blend state -> packed PSO key ----------------------------------
 * RSX blend factors/equation carry GL enums; each SFACTOR/DFACTOR/EQUATION
 * word packs (alpha<<16)|colour. The key packs D3D12 enums so the PSO cache
 * distinguishes real blend modes: bit0 enable, [5:1] srcC, [10:6] dstC,
 * [15:11] srcA, [20:16] dstA, [23:21] opC, [26:24] opA. DeferredShading's
 * light accumulation is additive (ONE,ONE) -- the old hardcoded straight-
 * alpha PSO turned each light pass into a screen-blanking flash. */
static u8 gl_blend_factor_d3d(u32 f, int is_alpha)
{
    switch (f & 0xFFFF) {
    case 0x0000: return D3D12_BLEND_ZERO;
    case 0x0001: return D3D12_BLEND_ONE;
    case 0x0300: return is_alpha ? D3D12_BLEND_SRC_ALPHA     : D3D12_BLEND_SRC_COLOR;
    case 0x0301: return is_alpha ? D3D12_BLEND_INV_SRC_ALPHA : D3D12_BLEND_INV_SRC_COLOR;
    case 0x0302: return D3D12_BLEND_SRC_ALPHA;
    case 0x0303: return D3D12_BLEND_INV_SRC_ALPHA;
    case 0x0304: return D3D12_BLEND_DEST_ALPHA;
    case 0x0305: return D3D12_BLEND_INV_DEST_ALPHA;
    case 0x0306: return is_alpha ? D3D12_BLEND_DEST_ALPHA     : D3D12_BLEND_DEST_COLOR;
    case 0x0307: return is_alpha ? D3D12_BLEND_INV_DEST_ALPHA : D3D12_BLEND_INV_DEST_COLOR;
    case 0x0308: return D3D12_BLEND_SRC_ALPHA_SAT;
    case 0x8001: return D3D12_BLEND_BLEND_FACTOR;       /* CONSTANT_COLOR   */
    case 0x8002: return D3D12_BLEND_INV_BLEND_FACTOR;
    case 0x8003: return D3D12_BLEND_BLEND_FACTOR;       /* CONSTANT_ALPHA ~ */
    case 0x8004: return D3D12_BLEND_INV_BLEND_FACTOR;
    default:     return D3D12_BLEND_ONE;
    }
}
static u8 gl_blend_op_d3d(u32 e)
{
    switch (e & 0xFFFF) {
    case 0x8007: return D3D12_BLEND_OP_MIN;
    case 0x8008: return D3D12_BLEND_OP_MAX;
    case 0x800A: return D3D12_BLEND_OP_SUBTRACT;
    case 0x800B: return D3D12_BLEND_OP_REV_SUBTRACT;
    default:     return D3D12_BLEND_OP_ADD;   /* 0x8006 FUNC_ADD / unset */
    }
}
/* Pack the guest's face-culling state for the PSO key.
 * bit0 = cull enabled, bit1 = cull FRONT (else BACK), bit2 = front face is CCW.
 * RSX/GL enums: CULL_FACE FRONT=0x0404 BACK=0x0405 FRONT_AND_BACK=0x0408;
 * FRONT_FACE CW=0x0900 CCW=0x0901. rsx_commands seeds cull_face/front_face with
 * plain 1/0 before any register arrives, so treat those defaults as BACK/CW. */
static u32 rsx_cull_key(const rsx_state* st)
{
    if (!st) return 0;
    /* The front face counts with culling off too: two-sided stencil picks its
     * front and back state by it (shadow volumes are drawn unculled). */
    u32 k = st->front_face == 0x0901u ? 4u : 0u;                        /* CCW */
    if (!st->cull_face_enable) return k;
    k |= 1u;
    if (st->cull_face == 0x0404u || st->cull_face == 0x0408u) k |= 2u;  /* FRONT */
    return k;
}

/* NV4097 texture wrap mode -> D3D12. */
static D3D12_TEXTURE_ADDRESS_MODE rsx_wrap(u32 w)
{
    switch (w & 0xFu) {
    case 2: return D3D12_TEXTURE_ADDRESS_MODE_MIRROR;
    case 3: case 5: return D3D12_TEXTURE_ADDRESS_MODE_CLAMP;    /* CLAMP_TO_EDGE, CLAMP */
    case 4: return D3D12_TEXTURE_ADDRESS_MODE_BORDER;
    case 6: case 7: case 8: return D3D12_TEXTURE_ADDRESS_MODE_MIRROR_ONCE;
    default: return D3D12_TEXTURE_ADDRESS_MODE_WRAP;            /* 1 = WRAP */
    }
}

/* Sampler from the guest's TEXTURE_ADDRESS (wrap S [3:0], T [11:8], R [19:16]),
 * TEXTURE_FILTER (LOD bias [12:0] signed x/256, min [18:16], mag [26:24]) and
 * TEXTURE_CONTROL0 (max anisotropy [6:4]) and BORDER_COLOR (ARGB). */
/* graphics.anisotropy = game (the title's own, the default) or 2, 4, 8, 16:
 * a sampler that filters linearly both ways becomes anisotropic at that level,
 * or at the title's when it asks for more. Point-sampled lookups stay as they
 * are; they are often data, not pictures. Read once. */
static int rsx_aniso_setting(void)
{
    static int v = -1;
    if (v >= 0) return v;
    const char* s = ps3_settings_str("graphics", "anisotropy", NULL, "game");
    const long n = ps3_settings_ieq(s, "game") ? 0 : strtol(s, NULL, 10);
    v = (n == 2 || n == 4 || n == 8 || n == 16) ? (int)n : 0;
    if (!v && !ps3_settings_ieq(s, "game"))
        printf("[D3D12] graphics.anisotropy = %s is not game, 2, 4, 8 or 16; the title's\n", s);
    return v;
}

/* RSX_CENSUS: the samplers made, by filter: [0] point or linear, [1] anisotropic
 * at the title's level, [2] anisotropic raised to graphics.anisotropy. */
static volatile long s_samp_made[3];

static void rsx_sampler_desc(const u32 k[4], D3D12_SAMPLER_DESC* s)
{
    const u32 addr = k[0], filter = k[1], ctrl0 = k[2], border = k[3];
    const u32 minf = (filter >> 16) & 7u, magf = (filter >> 24) & 7u;
    const int min_lin = minf == 2 || minf == 4 || minf == 6 || minf == 7;
    const int mip_lin = minf == 5 || minf == 6 || minf == 7;
    const int mag_lin = magf == 2 || magf == 4;
    static const u8 aniso[8] = { 1, 2, 4, 6, 8, 10, 12, 16 };
    u32 level = aniso[(ctrl0 >> 4) & 7u];
    const u32 own = level;
    if (min_lin && mag_lin && level < (u32)rsx_aniso_setting()) level = (u32)rsx_aniso_setting();
    s_samp_made[!(level > 1 && min_lin && mag_lin) ? 0 : level > own ? 2 : 1]++;
    memset(s, 0, sizeof *s);
    if (level > 1 && min_lin && mag_lin) {
        s->Filter = D3D12_FILTER_ANISOTROPIC;
        s->MaxAnisotropy = level;
    } else {
        s->Filter = (D3D12_FILTER)((min_lin << 4) | (mag_lin << 2) | mip_lin);
        s->MaxAnisotropy = 1;
    }
    s->AddressU = rsx_wrap(addr);
    s->AddressV = rsx_wrap(addr >> 8);
    s->AddressW = rsx_wrap(addr >> 16);
    int bias = (int)(filter & 0x1FFFu);
    if (bias & 0x1000) bias -= 0x2000;
    s->MipLODBias = (float)bias / 256.0f;
    /* What the upscaler is given to work with: AMD's formula. Not on what is blended, marked by
     * vp_sampler: those are drawn at their sample grid, a level finer than the title chose, and
     * a level finer again is noise. */
    if (s_up.want && s_d3d.bb_w && !(filter >> 31))
        s->MipLODBias += log2f((float)s_d3d.pw / (float)s_d3d.bb_w) - 1.0f;
    s->ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
    s->BorderColor[0] = (float)((border >> 16) & 0xFFu) / 255.0f;
    s->BorderColor[1] = (float)((border >> 8) & 0xFFu) / 255.0f;
    s->BorderColor[2] = (float)(border & 0xFFu) / 255.0f;
    s->BorderColor[3] = (float)(border >> 24) / 255.0f;
    /* Which levels may be sampled. A min filter without mipmapping (1 NEAREST,
     * 2 LINEAR; 7 the convolution filter) reads level 0 only -- D3D12 has no
     * such filter, a MIP_POINT one with MaxLOD 0 is the same. The mipmapped
     * ones (3-6) keep to TEXTURE_CONTROL0's minimum [30:19] and maximum
     * [18:7] level of detail, 4.8 fixed point. While only level 0 was
     * uploaded neither mattered; with the chains uploaded, the cut-out
     * textures a title samples unmipmapped came out as blocky lower levels. */
    float lo = 0.0f, hi = 0.0f;
    if (minf >= 3 && minf <= 6) {
        lo = (float)((ctrl0 >> 19) & 0xFFFu) / 256.0f;
        hi = (float)((ctrl0 >> 7) & 0xFFFu) / 256.0f;
        if (hi < lo) hi = lo;
    }
    s->MinLOD = lo;
    s->MaxLOD = hi;
}

static void vp_sampler_write(u32 i, const D3D12_SAMPLER_DESC* s)
{
    D3D12_CPU_DESCRIPTOR_HANDLE h;
    s_d3d.samp_heap->lpVtbl->GetCPUDescriptorHandleForHeapStart(s_d3d.samp_heap, &h);
    h.ptr += (SIZE_T)i * s_d3d.samp_inc;
    s_d3d.device->lpVtbl->CreateSampler(s_d3d.device, s, h);
}

/* Descriptors 0 and 1: what the four static samplers were, for units the
 * guest gave no state (s0 point/clamp for the dbgfont atlas, else linear/wrap). */
static void vp_sampler_defaults(void)
{
    D3D12_SAMPLER_DESC s = {0};
    s.Filter = D3D12_FILTER_MIN_MAG_MIP_POINT;
    s.AddressU = s.AddressV = s.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    s.MaxAnisotropy = 1; s.MaxLOD = D3D12_FLOAT32_MAX;
    vp_sampler_write(0, &s);
    s.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    s.AddressU = s.AddressV = s.AddressW = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    vp_sampler_write(1, &s);
    s_d3d.samp_n = 2;
}

/* The descriptor index for a draw's unit: one per distinct guest state, made
 * on first use. A full heap falls back to the defaults. */
static int vp_sampler(const D3D12DrawRecord* dr, int u)
{
    if (!dr->tex[u].set) return u == 0 ? 0 : 1;
    const u32 k[4] = { dr->tex[u].addr & 0x000F0F0Fu, (dr->tex[u].filter & 0x07071FFFu) | (s_up.want && dr->blend ? 1u << 31 : 0u),
                       dr->tex[u].ctrl0 & 0x70u, dr->tex[u].border };
    for (u32 i = 2; i < s_d3d.samp_n; i++)
        if (!memcmp(s_d3d.samp_key[i], k, sizeof k)) return (int)i;
    if (s_d3d.samp_n >= 2048) return u == 0 ? 0 : 1;
    const u32 i = s_d3d.samp_n++;
    memcpy(s_d3d.samp_key[i], k, sizeof k);
    D3D12_SAMPLER_DESC s;
    rsx_sampler_desc(k, &s);
    vp_sampler_write(i, &s);
    return (int)i;
}

/* NV4097 stencil op (GL enum) -> D3D12_STENCIL_OP - 1. */
static u32 rsx_stencil_op(u32 op)
{
    switch (op) {
    case 0x0000: return 1;   /* ZERO      */
    case 0x1E01: return 2;   /* REPLACE   */
    case 0x1E02: return 3;   /* INCR (saturating) */
    case 0x1E03: return 4;   /* DECR (saturating) */
    case 0x150A: return 5;   /* INVERT    */
    case 0x8507: return 6;   /* INCR_WRAP */
    case 0x8508: return 7;   /* DECR_WRAP */
    default:     return 0;   /* KEEP      */
    }
}

/* Guest depth/stencil state, packed as a PSO key. Comparison functions are
 * GL enums 0x200 NEVER .. 0x207 ALWAYS, so the low three bits are D3D12's
 * D3D12_COMPARISON_FUNC - 1. Fields that cannot matter are left zero (no
 * depth state with the test off, no stencil state with the test off), so the
 * same pipeline is not built twice.
 *   bit 0 depth test, 1 depth write, 2-4 depth func,
 *   5 stencil test, 6-8 front func, 9-17 front fail/zfail/zpass,
 *   18-20 back func, 21-29 back fail/zfail/zpass, 32-39 read mask, 40-47 write mask */
static u64 rsx_zs_key(const rsx_state* st)
{
    if (!st) return 0;
    u64 k = 0;
    if (st->depth_test_enable) {
        k |= 1u;
        if (st->depth_mask) k |= 2u;
        k |= (u64)(st->depth_func & 7u) << 2;
    }
    if (st->stencil_test_enable) {
        const int two = st->two_sided_stencil;
        k |= 1u << 5;
        k |= (u64)(st->stencil_func & 7u) << 6;
        k |= (u64)rsx_stencil_op(st->stencil_op_fail) << 9;
        k |= (u64)rsx_stencil_op(st->stencil_op_zfail) << 12;
        k |= (u64)rsx_stencil_op(st->stencil_op_zpass) << 15;
        k |= (u64)((two ? st->back_stencil_func : st->stencil_func) & 7u) << 18;
        k |= (u64)rsx_stencil_op(two ? st->back_stencil_op_fail : st->stencil_op_fail) << 21;
        k |= (u64)rsx_stencil_op(two ? st->back_stencil_op_zfail : st->stencil_op_zfail) << 24;
        k |= (u64)rsx_stencil_op(two ? st->back_stencil_op_zpass : st->stencil_op_zpass) << 27;
        k |= (u64)(st->stencil_mask & 0xFFu) << 32;
        k |= (u64)(st->stencil_write_mask & 0xFFu) << 40;
    }
    return k;
}

/* Polygon offset of a filled primitive as a PSO key: 0 when off, else the
 * factor's float bits in the high word and the bias's in the low. The RSX
 * bias counts steps of the depth format's resolution, as D3D12's DepthBias
 * does; every depth buffer here is D24, and a Z16 surface's step is 256 of
 * those. God of War: Ascension draws with factor 2 and biases of ~240,000. */
static u64 rsx_poly_key(const rsx_state* st)
{
    if (!st || !st->poly_offset_fill) return 0;
    u32 bias = st->poly_offset_bias;
    if (((st->surface_format >> 5) & 7u) == 1u) {   /* Z16 */
        float b; memcpy(&b, &bias, sizeof b); b *= 256.0f; memcpy(&bias, &b, sizeof bias);
    }
    if (!(st->poly_offset_factor & 0x7FFFFFFFu) && !(bias & 0x7FFFFFFFu)) return 0;
    return ((u64)st->poly_offset_factor << 32) | bias;
}

/* The state step 2 of the rendering accuracy plan records per draw, and the
 * census of it (RSX_CENSUS): which of it the title uses with values that
 * matter. A value table keeps the eight most frequent. */
typedef struct { u32 v[8]; unsigned long long n[8]; unsigned long long other; } CenTop;
static void cen_top(CenTop* t, u32 v)
{
    for (int i = 0; i < 8; i++) if (t->n[i] && t->v[i] == v) { t->n[i]++; return; }
    for (int i = 0; i < 8; i++) if (!t->n[i]) { t->v[i] = v; t->n[i] = 1; return; }
    t->other++;
}
static void cen_top_print(const char* name, CenTop* t)
{
    fprintf(stderr, " %s:", name);
    for (int i = 0; i < 8; i++) if (t->n[i]) fprintf(stderr, " %08X=%llu", t->v[i], t->n[i]);
    if (t->other) fprintf(stderr, " other=%llu", t->other);
    memset(t, 0, sizeof *t);
}
static unsigned long long s_c2_draws, s_c2_const, s_c2_mrt, s_c2_dbt, s_c2_clip, s_c2_a2c;
static CenTop s_c2_bcol, s_c2_bfmt, s_c2_bmrt, s_c2_cmrt, s_c2_dbtmm, s_c2_zmm;
static void rsx_census_step2_print(void)
{
    fprintf(stderr, "[census] step2: draws %llu; constant blend %llu, MRT %llu, depth bounds on %llu, "
                    "depth range not 0..1 %llu, alpha to coverage %llu;",
            s_c2_draws, s_c2_const, s_c2_mrt, s_c2_dbt, s_c2_clip, s_c2_a2c);
    cen_top_print("blend colour", &s_c2_bcol); cen_top_print("its target fmt", &s_c2_bfmt);
    cen_top_print("MRT blend", &s_c2_bmrt); cen_top_print("MRT mask", &s_c2_cmrt);
    cen_top_print("bounds min", &s_c2_dbtmm); cen_top_print("zmm", &s_c2_zmm);
    fprintf(stderr, "\n");
    s_c2_draws = s_c2_const = s_c2_mrt = s_c2_dbt = s_c2_clip = s_c2_a2c = 0;
}
static int rsx_census_secs(void);
static void rec_step2_state(D3D12DrawRecord* dr, const rsx_state* st)
{
    if (!st) return;
    dr->blend_color = st->blend_color; dr->blend_color2 = st->blend_color2;
    dr->blend_const = 0;
    if (st->blend_enable) {
        const u32 f[2] = { st->blend_sfactor & 0xFFFFu, st->blend_dfactor & 0xFFFFu };
        for (int i = 0; i < 2; i++) {
            if (f[i] == 0x8001u || f[i] == 0x8002u) dr->blend_const |= 1u;
            if (f[i] == 0x8003u || f[i] == 0x8004u) dr->blend_const |= 2u;
        }
        const u32 fa[2] = { st->blend_sfactor >> 16, st->blend_dfactor >> 16 };
        for (int i = 0; i < 2; i++)
            if (fa[i] >= 0x8001u && fa[i] <= 0x8004u) dr->blend_const |= 4u;
    }
    dr->blend_mrt = st->blend_enable_mrt; dr->cmask_mrt = st->color_mask_mrt;
    dr->dbt = (u32)st->depth_bounds_enable; dr->dbt_min = st->depth_bounds_min; dr->dbt_max = st->depth_bounds_max;
    memcpy(&dr->clip_min, &st->clip_min, sizeof dr->clip_min);
    memcpy(&dr->clip_max, &st->clip_max, sizeof dr->clip_max);
    dr->zmm = st->zmin_max_control; dr->aa = st->aa_control;
    if (!rsx_census_secs()) return;
    s_c2_draws++;
    if (dr->blend_const) { s_c2_const++; cen_top(&s_c2_bcol, dr->blend_color); cen_top(&s_c2_bfmt, st->surface_format & 0x1Fu); }
    if (st->color_target & 0x10u) { s_c2_mrt++; cen_top(&s_c2_bmrt, (st->blend_enable ? 1u : 0u) | (dr->blend_mrt & 0xEu)); cen_top(&s_c2_cmrt, dr->cmask_mrt); }
    if (dr->dbt) { s_c2_dbt++; cen_top(&s_c2_dbtmm, dr->dbt_min); }
    if (st->clip_min != 0.0f || (st->clip_max != 1.0f && st->clip_max != 0.0f)) s_c2_clip++;
    if (dr->aa & 0x10u) s_c2_a2c++;
    cen_top(&s_c2_zmm, dr->zmm);
}

/* Room for one more draw record. A full batch drops the draw: said once, so
 * a scene that outgrows MAX_DRAWS shows in the log, not as missing objects. */
static int rsx_draw_room(void)
{
    if (s_d3d.draw_count < MAX_DRAWS) return 1;
    static int said = 0;
    if (!said) {
        said = 1;
        fprintf(stderr, "[D3D12] a frame has more than %d draws; the rest of it is dropped (MAX_DRAWS)\n",
                MAX_DRAWS);
    }
    return 0;
}

/* The per-draw pipeline key bits of step 2: the depth bounds test (bit 0) and
 * depth clipping (bit 1); and MRT 1-3's colour masks and blend enables. The
 * switches RSX_DEPTH_CLIP=0 and RSX_MRT_MASK=0 keep the old behaviour. */
static int dbt_ready(void);
static u32 rsx_pso_ext(const D3D12DrawRecord* dr)
{
    static int clip = -1;
    if (clip < 0) { const char* e = getenv("RSX_DEPTH_CLIP"); clip = e ? (atoi(e) != 0) : 1; }
    if (!dr) return 0;
    u32 e = 0;
    if (dr->dbt && dbt_ready()) e |= 1u;
    if (clip && (dr->zmm & 1u)) e |= 2u;
    return e;
}
static u32 rsx_pso_mrt(const D3D12DrawRecord* dr)
{
    static int on = -1;
    if (on < 0) { const char* e = getenv("RSX_MRT_MASK"); on = e ? (atoi(e) != 0) : 1; }
    if (!dr || !on) return 0;
    return 1u | (dr->cmask_mrt & 0xFFF0u) | ((dr->blend_mrt & 0xEu) << 16);
}

/* 2.1: the constant blend colour of a draw that blends with it. SET_BLEND_COLOR
 * is A8R8G8B8 for an 8-bit target; for an fp16 or fp32 one it holds R and G as
 * halves, and SET_BLEND_COLOR2 B and A. CONSTANT_ALPHA has no D3D12 factor of
 * its own here, so a draw blending with it (and not with CONSTANT_COLOR) gets
 * the alpha in all four lanes. The factor was never set before: such draws
 * blended with D3D12's default. RSX_BLEND_COLOR=0 leaves it so. */
static int rsx_blend_color_on(void)
{
    static int v = -1;
    if (v < 0) { const char* e = getenv("RSX_BLEND_COLOR"); v = e ? (atoi(e) != 0) : 1; }
    return v;
}
static float rsx_half(u32 h)
{
    const u32 s = (h >> 15) & 1u, e = (h >> 10) & 31u, m = h & 1023u;
    float v = e ? ldexpf((float)(m | 1024u), (int)e - 25) : ldexpf((float)m, -24);
    if (e == 31u) v = m ? 0.0f : 65504.0f;
    return s ? -v : v;
}
static void rsx_blend_factor(const D3D12DrawRecord* dr, float bf[4])
{
    const u32 c = dr->blend_color, c2 = dr->blend_color2;
    if ((dr->rt_fmt & 0x1Fu) == 0x0Bu || (dr->rt_fmt & 0x1Fu) == 0x0Cu) {
        bf[0] = rsx_half(c & 0xFFFFu); bf[1] = rsx_half(c >> 16);
        bf[2] = rsx_half(c2 & 0xFFFFu); bf[3] = rsx_half(c2 >> 16);
    } else {
        bf[0] = (float)((c >> 16) & 0xFFu) / 255.0f; bf[1] = (float)((c >> 8) & 0xFFu) / 255.0f;
        bf[2] = (float)(c & 0xFFu) / 255.0f;         bf[3] = (float)(c >> 24) / 255.0f;
    }
    if ((dr->blend_const & 3u) == 2u) bf[0] = bf[1] = bf[2] = bf[3];
}

static u32 rsx_blend_key(const rsx_state* st, int enable)
{
    { static int _bd = -1; if (_bd < 0) _bd = getenv("BLENDDBG") ? 1 : 0;
      if (_bd) { static u32 seen[32]; static int ns=0;
        u32 sk = (enable?0x80000000u:0u) | (st ? (st->blend_sfactor & 0xFFFFu) : 0u);
        int f=0; for (int k2=0;k2<ns;k2++) if (seen[k2]==sk) f=1;
        if (!f && ns<32) { seen[ns++]=sk;
            fprintf(stderr, "[BLEND] enable=%d sfactor=0x%X dfactor=0x%X%c", enable,
                    st?st->blend_sfactor:0, st?st->blend_dfactor:0, 10); } } }
    if (!enable) return 0;
    /* Factors never programmed: keep the legacy straight-alpha behaviour
     * (dbgfont-style text enables blending without setting factors). */
    if (!st || (st->blend_sfactor == 0 && st->blend_dfactor == 0))
        return 1u
             | ((u32)D3D12_BLEND_SRC_ALPHA     << 1)
             | ((u32)D3D12_BLEND_INV_SRC_ALPHA << 6)
             | ((u32)D3D12_BLEND_ONE           << 11)
             | ((u32)D3D12_BLEND_INV_SRC_ALPHA << 16)
             | ((u32)D3D12_BLEND_OP_ADD << 21) | ((u32)D3D12_BLEND_OP_ADD << 24);
    return 1u
         | ((u32)gl_blend_factor_d3d(st->blend_sfactor,       0) << 1)
         | ((u32)gl_blend_factor_d3d(st->blend_dfactor,       0) << 6)
         | ((u32)gl_blend_factor_d3d(st->blend_sfactor >> 16, 1) << 11)
         | ((u32)gl_blend_factor_d3d(st->blend_dfactor >> 16, 1) << 16)
         | ((u32)gl_blend_op_d3d(st->blend_equation)       << 21)
         | ((u32)gl_blend_op_d3d(st->blend_equation >> 16) << 24);
}

/* Bound colour target count for a draw record: 1 (target A) + the contiguous
 * prefix of MRT B/C/D offsets. */
static int dr_num_rts(const D3D12DrawRecord* dr)
{
    if (dr->no_color) return 0;
    int n = 1;
    while (n < 4 && dr->rt_mrt[n - 1]) n++;
    return n;
}

/* Replace every occurrence of `find` with `repl` inside a NUL-terminated buffer,
 * shifting the tail. Used to retarget generated HLSL (2D sampler -> cube). */
static void hlsl_replace_all(char* buf, size_t cap, const char* find, const char* repl)
{
    size_t fl = strlen(find), rl = strlen(repl);
    if (!fl) return;
    char* at = buf;
    while ((at = strstr(at, find)) != NULL) {
        size_t used = strlen(buf) + 1;
        if (used + rl - fl > cap) return;
        memmove(at + rl, at + fl, used - (size_t)(at - buf) - fl);
        memcpy(at, repl, rl);
        at += rl;
    }
}

/* Which of a draw's texture units are cube textures, as a bit mask. */
/* The RGBA lanes each unit's fetch decodes from sRGB (TEXTURE_ADDRESS gamma
 * bits, after the remap), 4 bits per unit. A title that encodes its output to
 * sRGB keeps its colour textures sRGB-encoded too and has the fetch decode
 * them; without the decode they come out too light once the output encodes.
 * RSX_NO_SRGB=1 turns this off with the output side. */
static u32 dr_gamma_mask(const D3D12DrawRecord* dr, int u)
{
    static int no_srgb = -1;
    if (no_srgb < 0) no_srgb = getenv("RSX_NO_SRGB") ? 1 : 0;
    if (no_srgb || !dr->tex[u].set) return 0;
    return rsx_texture_gamma_mask(dr->tex[u].fmt, dr->tex[u].ctrl1, dr->tex[u].filter,
                                  dr->tex[u].addr) & 15u;
}

/* A program compiled with the masks in (FP_CONSTBUF=0) is keyed on them; a
 * buffered one reads them from b1 (fp_tcscale[u].z, written at replay). */
static u64 dr_gamma_key(const D3D12DrawRecord* dr)
{
    u64 k = 0;
    for (int u = 0; u < VP_TEX_UNITS; u++) k |= (u64)dr_gamma_mask(dr, u) << (4 * u);
    return k;
}

static void rsx_aa_grid(u32 surface_format, u32* sx, u32* sy);   /* the surface's sample grid */

/* A depth texture (DEPTH24_D8, DEPTH16 and their float forms) on unit u. */
static int dr_tex_depth_fmt(const D3D12DrawRecord* dr, int u)
{
    const u32 b = dr->tex[u].fmt & 0x9Fu;
    return dr->tex[u].set && b >= 0x90u && b <= 0x93u;
}

/* The units the RSX samples with a depth comparison: a depth texture whose
 * SET_TEXTURE_ADDRESS zfunc is 1-6 (rsx_fp_shadow_desc_from_texture). God of
 * War: Ascension builds its screen-space shadow mask this way, its shadow map
 * on unit 5 with GEQUAL and bilinear filtering. Their descriptors for the
 * decompiler and a key over them, 0 with none. A layout the decompiler does
 * not translate (float depth, mips, cube) is left as it was. */
static u64 dr_shadow_descs(const D3D12DrawRecord* dr, rsx_fp_shadow_desc sd[16])
{
    memset(sd, 0, 16 * sizeof *sd);
    u64 k = 0;
    int any = 0;
    for (int u = 0; u < VP_TEX_UNITS && u < 16; u++) {
        if (!dr_tex_depth_fmt(dr, u)) continue;
        rsx_fp_shadow_desc_from_texture(&sd[u], dr->tex[u].fmt, 2u, dr->tex[u].cube, dr->tex[u].mips,
                                        dr->tex[u].filter, dr->tex[u].addr, dr->tex[u].ctrl0);
        if (sd[u].enabled != 1) { memset(&sd[u], 0, sizeof sd[u]); continue; }
        any = 1;
        k = (k ^ (u64)(u + 1)) * 1099511628211ull;
        k = (k ^ ((u64)sd[u].func | (u64)sd[u].linear << 4 | (u64)sd[u].unnormalized << 5)) * 1099511628211ull;
    }
    return any ? (k ? k : 1u) : 0u;
}

/* A normalized depth texture is a region of the depth surface its view
 * covers (the title's shadow maps of each light, 578x796 and the like in a
 * 1408x1406 surface): its coordinate takes the scale to that region from b1
 * (fp_tcscale[u].xy, dr_coord_scales), as a 2D fetch's does in
 * fp_apply_coord_scale. A texel-coordinate one divides by the view's size in
 * the helper already. */
static void fp_shadow_scale(char* hlsl, size_t cap, const rsx_fp_shadow_desc sd[16])
{
    static const char from[] = "    float2 uv = coordinate.xy;\n";
    for (u32 u = 0; u < 16; u++) {
        if (sd[u].enabled != 1 || sd[u].unnormalized) continue;
        char head[64], to[80];
        snprintf(head, sizeof head, "float4 _rsx_shadow%u(float3 coordinate) {", u);
        char* hp = strstr(hlsl, head);
        char* at = hp ? strstr(hp, from) : NULL;
        if (!at) continue;
        snprintf(to, sizeof to, "    float2 uv = coordinate.xy * fp_tcscale[%u].xy;\n", u);
        const size_t fl = sizeof from - 1, tl = strlen(to), used = strlen(hlsl);
        if (used + tl - fl + 1 > cap) return;
        memmove(at + tl, at + fl, used - (size_t)(at + fl - hlsl) + 1);
        memcpy(at, to, tl);
    }
}

/* Per unit, the factor the program's texture coordinate is multiplied by, and
 * the units that need one:
 *  - a texel-coordinate unit (SET_TEXTURE_FORMAT bit 6): 1/size of what is
 *    bound -- a target's own size when the texture is a target. This backend
 *    sampled them with the texel coordinate as is: God of War: Ascension binds
 *    its 1280x720 surface at 0x1D90000 this way;
 *  - a normalized unit reading the top-left corner of a larger target (a
 *    target re-bound smaller, RT_REUSE): texture size / target size.
 * Cube units are left alone. tex_rt is set by the prepass. */
static u32 dr_coord_scales(const D3D12DrawRecord* dr, float sc[VP_TEX_UNITS][2])
{
    u32 mask = 0;
    for (int u = 0; u < VP_TEX_UNITS; u++) {
        sc[u][0] = sc[u][1] = 1.0f;
        if (!dr->tex[u].set || dr->tex[u].cube || !dr->tex[u].w || !dr->tex[u].h) continue;
        u32 rw = dr->tex[u].w, rh = dr->tex[u].h;
        const int rt = dr->tex_rt[u];
        if (rt >= 0 && rt < MAX_OFF_RTS && s_d3d.off_rt[rt].res) {
            rw = s_d3d.off_rt[rt].w; rh = s_d3d.off_rt[rt].h;
        }
        const int cv = dr->tex_cv[u];   /* read through its Y16_X16 copy: the copy's size */
        if (cv >= 0 && cv < MAX_OFF_RTS && s_d3d.off_rt[cv].res) {
            rw = s_d3d.off_rt[cv].w; rh = s_d3d.off_rt[cv].h;
        }
        const int dz = dr->tex_dz[u];
        if (dz >= 0 && dz < MAX_OFF_DS && s_d3d.off_ds[dz].res) {
            rw = s_d3d.off_ds[dz].w; rh = s_d3d.off_ds[dz].h;
        }
        if (dr->tex[u].fmt & 0x40) {
            sc[u][0] = 1.0f / (float)rw; sc[u][1] = 1.0f / (float)rh; mask |= 1u << u;
        } else if (rw != dr->tex[u].w || rh != dr->tex[u].h) {
            sc[u][0] = (float)dr->tex[u].w / (float)rw;
            sc[u][1] = (float)dr->tex[u].h / (float)rh;
            mask |= 1u << u;
        }
    }
    return mask;
}

/* The scales as a pipeline key: a hash above the 16-bit unit mask. */
static u64 dr_unnorm_key(const D3D12DrawRecord* dr)
{
    float sc[VP_TEX_UNITS][2];
    const u32 mask = dr_coord_scales(dr, sc);
    if (!mask) return 0;
    u32 h = 2166136261u;
    for (int u = 0; u < VP_TEX_UNITS; u++)
        if ((mask >> u) & 1u) {
            u32 bits;
            memcpy(&bits, &sc[u][0], 4); h = (h ^ bits) * 16777619u;
            memcpy(&bits, &sc[u][1], 4); h = (h ^ bits) * 16777619u;
        }
    return ((u64)h << 16) | mask;
}

/* Multiply the coordinate of every 2D Sample of the masked units by its scale,
 * the way rsx_fp_apply_unnorm_scale does with 1/size: the literal one in sc, or
 * with sc NULL the draw's own from b1 (fp_tcscale[], written at replay). */
static void fp_apply_coord_scale(char* hlsl, size_t cap, u32 mask, const float sc[VP_TEX_UNITS][2])
{
    for (u32 u = 0; u < VP_TEX_UNITS; u++) {
        if (!((mask >> u) & 1u)) continue;
        char needles[2][40];
        snprintf(needles[0], sizeof needles[0], "rsx_tex[%u].Sample(rsx_samp[%u],", u, u);
        snprintf(needles[1], sizeof needles[1], "rsx_tex%u.Sample(rsx_samp[%u],", u, u);
        for (int ni = 0; ni < 2; ni++) {
            const size_t nlen = strlen(needles[ni]);
            for (;;) {
                char* at = strstr(hlsl, needles[ni]);
                if (!at) break;
                char* open = at + nlen - 1;
                while (open > at && *open != '(') open--;
                int depth = 0;
                char* q = open;
                for (; *q; q++) {
                    if (*q == '(') depth++;
                    else if (*q == ')') { depth--; if (!depth) break; }
                }
                if (*q != ')') break;
                char scale[80];
                if (sc) snprintf(scale, sizeof scale, " * float2(%.9g, %.9g)", sc[u][0], sc[u][1]);
                else    snprintf(scale, sizeof scale, " * fp_tcscale[%u].xy", u);
                const size_t sl = strlen(scale), used = strlen(hlsl);
                if (used + sl + 1 > cap) return;
                memmove(q + sl, q, used - (size_t)(q - hlsl) + 1);
                memcpy(q, scale, sl);
                memcpy(at, "rsx_TEX", 7);   /* done; restored below */
            }
        }
        for (char* q = hlsl; (q = strstr(q, "rsx_TEX")) != NULL; ) memcpy(q, "rsx_tex", 7);
    }
}

static int dr_cube_tex_on(void);
static u32 dr_cube_mask(const D3D12DrawRecord* dr)
{
    /* ON by default (RSX_CUBE_TEX=0 to disable). This was off while the cube path
     * cost 2262 PSO misses per 20 frames and 0.38 fps -- both caused by inline
     * fragment-program constants baking into the shader source. With those
     * constants hoisted into b1 the cube path costs 0 PSO misses, and with the
     * per-face stride fixed (each face is a whole mip pyramid, not one mip-0
     * image) all six faces decode as clean environment art. */
    if (!dr_cube_tex_on()) return 0;
    u32 m = 0;
    for (int u = 0; u < VP_TEX_UNITS; u++) if (dr->tex[u].set && dr->tex[u].cube) m |= 1u << u;
    return m;
}

/* RSX_CUBE_TEX=0 turns cube textures off (dr_cube_mask). */
static int dr_cube_tex_on(void)
{
    static int en = -1;
    if (en < 0) { const char* e = getenv("RSX_CUBE_TEX"); en = e ? atoi(e) : 1; }
    return en;
}

/* Cube and volume units declared on their own t-register (TextureCube,
 * Texture3D) in buffered programs too. Without it a buffered program declares
 * every unit in the Texture2D array: a cube's TEXTURECUBE view was sampled as a
 * 2D image and a volume as its first slice -- God of War: Ascension's fog
 * volume (64x32x16, its first slice empty) then added nothing, and its menu
 * came out darker than RPCS3's, the shadowed cloth by almost a third.
 * RSX_TEX_SEP=0 keeps the array, as before. */
static int tex_sep_on(void)
{
    static int en = -1;
    if (en < 0) { const char* e = getenv("RSX_TEX_SEP"); en = e ? (atoi(e) != 0) : 1; }
    return en;
}

/* RSX_TEX_EXPAND=0: programs decompiled without texture sign expansion. */
static int tex_expand_on(void)
{
    static int en = -1;
    if (en < 0) { const char* e = getenv("RSX_TEX_EXPAND"); en = e ? (atoi(e) != 0) : 1; }
    return en;
}

/* A unit bound to a volume the upload makes as one: uncompressed, deeper than
 * one slice. RSX_TEX3D=0: its first slice, in 2D, as before. */
static int dr_unit_vol(const D3D12DrawRecord* dr, int u)
{
    static int en = -1;
    if (en < 0) { const char* e = getenv("RSX_TEX3D"); en = e ? (atoi(e) != 0) : 1; }
    if (!en || !tex_sep_on() || !dr->tex[u].set || dr->tex[u].cube || dr->tex[u].depth <= 1)
        return 0;
    rsx_tex_layout tl;
    rsx_texture_layout(dr->tex[u].fmt, dr->tex[u].w, dr->tex[u].h, &tl);
    return !tl.compressed && tl.bytes_per_texel;
}

static u32 dr_vol_mask(const D3D12DrawRecord* dr)
{
    u32 m = 0;
    for (int u = 0; u < VP_TEX_UNITS; u++) if (dr_unit_vol(dr, u)) m |= 1u << u;
    return m;
}

/* The view a unit's program declares: 1 TextureCube, 2 Texture3D, 0 Texture2D. */
static int dr_unit_view(const D3D12DrawRecord* dr, int u)
{
    if (!tex_sep_on() || !dr->tex[u].set) return 0;
    if (dr->tex[u].cube) return dr_cube_tex_on() ? 1 : 0;
    return dr_unit_vol(dr, u) ? 2 : 0;
}

/* A texture's shape as the cache keys it (VPTexSlot.cube). */
static int tex_shape_vol(u32 depth) { return (int)((depth << 8) | 2u); }
static u32 tex_shape_depth(int shape) { return (shape & 2) ? ((u32)shape >> 8) : 1u; }

/* The entry's pipeline, taking over a finished worker build. NULL with
 * s_pso_pending set while it builds (within the wait budget); NULL alone if
 * the build failed. */
static ID3D12PipelineState* vp_fp_ready(VPFPEntry* q)
{
    if (!q->job) return q->pso;
    if (!sj_wait(q->job)) { s_pso_pending = 1; return NULL; }
    ShaderJob* j = q->job;
    q->job = NULL;
    if (SUCCEEDED(j->hr) && j->pso) {
        q->pso = j->pso;
        j->pso = NULL;
        { static int _ok=0; if (PS3_LOG_FIRST(_ok, 32)) printf("[FP] guest FP pipeline ready (fp=0x%08X)\n", q->fp_addr); }
        rdoc_name((ID3D12Object*)q->pso, "pso fp 0x%08X ucode %08X vs %d", q->fp_addr, q->ucode_hash, q->vs_idx);
    } else {
        static int _e = 0;
        if (PS3_LOG_FIRST(_e, 16))
            printf("[FP] %s FAIL (fp=0x%08X, 0x%08lX): %s\n", j->blob ? "PSO" : "PS compile",
                   q->fp_addr, (unsigned long)j->hr, j->err);
    }
    sj_release(j);
    return q->pso;
}

/* NULL with s_pso_pending set: a shader or pipeline this draw needs is still
 * being built by a worker (D3D12_ASYNC_SHADERS), and the draw should be
 * skipped rather than drawn with another program. */
static void rf_release(void* p);

/* The cache's index: per ucode-hash bucket, a chain of entry indices + 1. */
static int s_fp_head[VP_FP_HASH];
static int s_fp_next[VP_FP_CACHE];

/* PSO_MISS_WHY=1 (PERF report): misses of a program no pipeline was cached
 * for, and for the rest how often each key field differed from the cached
 * pipeline of the same program that differs in the fewest. */
enum { PW_VS_IDX, PW_VS_HASH, PW_GEN, PW_BLEND, PW_NRT, PW_RTFMT, PW_CTRL, PW_CMASK, PW_CULL,
       PW_CUBE, PW_RAW, PW_GAMMA, PW_UNNORM, PW_WPOS, PW_ATEST, PW_POLY, PW_EXT, PW_MRT, PW_ZS, PW_N };
static const char* const s_pw_name[PW_N] = { "vs_idx", "vs_hash", "gen", "blend", "nrt", "rtfmt", "ctrl",
    "cmask", "cull", "cube_mask", "raw", "gamma", "unnorm", "wpos", "atest", "poly", "ext", "mrt", "zs" };
static unsigned s_pw_new, s_pw_n[PW_N];
static u32 s_pw_ctrl;           /* control bits that differed */
static int s_env_psowhy = -1;

static void vp_fp_index_rebuild(void)
{
    memset(s_fp_head, 0, sizeof s_fp_head);
    for (int i = 0; i < s_d3d.vp_fp_n; i++) {
        const u32 h = s_d3d.vp_fp[i].ucode_hash & (VP_FP_HASH - 1);
        s_fp_next[i] = s_fp_head[h];
        s_fp_head[h] = i + 1;
    }
}

static ID3D12PipelineState* vp_get_fp_pso(int vs_idx, u32 fp_addr, u32 blend, int nrt,
                                          DXGI_FORMAT rtfmt, u32 ctrl, u32 cmask, u32 cull,
                                          u32 cube_mask, int raw, u64 zs, u64 gamma,
                                          u64 unnorm, const D3D12DrawRecord* udr)
{
    s_pso_pending = 0;
    if (nrt < 0) nrt = 0; if (nrt > 4) nrt = 4;   /* 0: a depth-only pass */
    const u32 ext = rsx_pso_ext(udr);
    const u32 mrt = nrt > 1 ? rsx_pso_mrt(udr) : 0u;
    /* The polygon offset as the key holds it: with dynamic depth bias only
     * whether the draw has one, the values being set per draw. */
    const u64 pkey = (udr && udr->poly) ? (dyb_ready() ? 1u : udr->poly) : 0u;
    if (rtfmt == 0) rtfmt = DXGI_FORMAT_R8G8B8A8_UNORM;
    extern uint8_t* vm_base;
    extern u32 cellGcmResolveOffset(u32);
    if (!fp_addr || !vm_base) return NULL;
    { static int _off=-1; if(_off<0)_off=getenv("FP_OFF")?1:0; if(_off) return NULL; }
    /* Resolve the VS: the draw's cached per-VP blob, else the primary. */
    ID3DBlob* vsb = NULL; u32 vs_hash = 0;
    if (vs_idx >= 0 && vs_idx < s_d3d.vp_vs_n) {
        VPVSEntry* ve = &s_d3d.vp_vs[vs_idx];
        vs_hash = ve->hash;
        if (!raw) {
            vsb = vp_vs_ready(ve);
            /* Still building: wait for this program, not draw with the primary one. */
            if (ve->job) { s_pso_pending = 1; return NULL; }
        }
    }
    if (raw) { vsb = vp_vs_raw(vs_idx); if (!vsb) return NULL; }
    if (!vsb) { vsb = s_d3d.vp_vs_blob; vs_idx = -1; }
    if (!vsb) return NULL;

    /* SET_SHADER_PROGRAM low bits = location+1 (same as textures): 1 = LOCAL
     * (VRAM), 2 = MAIN (gcm/cube emits 0x00B90001 for its VRAM-resident FP). */
    extern u32 cellGcmResolveLocated(int local, u32 offset);
    u32 off = cellGcmResolveLocated((fp_addr & 0x3u) == 1, fp_addr & ~0x3u);
    if (off == 0xFFFFFFFFu) return NULL;
    /* The program as it was when the draw was recorded (fp_snapshot). */
    const u8* prog = (udr && udr->fp_snap && udr->fp_addr == fp_addr) ? udr->fp_snap : vm_base + off;
    const u32 prog_max = prog != vm_base + off ? udr->fp_snap_len : FP_MAX_BYTES;

    /* The key's program part. Buffered (the default): the shader reads the
     * program's inline constants from b1 (fp_constants[], refilled for every
     * draw by vp_record_cb), so it is keyed on the program without them
     * (rsx_fp_structural_hash) and a title that re-patches them per material
     * or per frame reuses one pipeline. FP_CONSTBUF=0, or more constants than
     * a b1 slot holds: they are compiled in as literals, and keyed on with
     * every other byte of the program. */
    u32 uhash = 0;
    int buffered = 0;
    {
        static int cb_on = -1;
        if (cb_on < 0) { const char* e = getenv("FP_CONSTBUF"); cb_on = !(e && e[0] == '0'); }
        u32 nk = 0;
        u64 sh = 0;
        if (cb_on && prog != vm_base + off) { sh = udr->fp_shash; nk = udr->fp_shash_nk; }
        else if (cb_on) sh = rsx_fp_structural_hash_n(prog, prog_max, 1469598103934665603ull, &nk);
        if (sh && nk <= FP_CB_MAX_K) {
            buffered = 1;
            uhash = (u32)(sh ^ (sh >> 32));
        } else {
            u32 usz = rsx_fp_program_size(prog, prog_max);
            if (cb_on) {
                static int _w = 0;
                if (PS3_LOG_FIRST(_w, 8))
                    fprintf(stderr, sh ? "[FP] fp=0x%08X: %u inline constants, more than b1 holds; compiled in%c"
                                       : "[FP] fp=0x%08X: no end within %u bytes; constants compiled in%c",
                            fp_addr, sh ? nk : FP_MAX_BYTES, 10);
            }
            if (usz == 0) usz = prog_max;
            s_perf_pso_hashbytes += (int)usz;
            uhash = 2166136261u;
            const u8* up = prog;
            for (u32 i = 0; i < usz; i++) { uhash ^= up[i]; uhash *= 16777619u; }
        }
    }
    /* A buffered program takes its coordinate scales and WPOS window from b1
     * too; a literal one has them compiled in. The alpha test's function is
     * compiled in (its reference is fp_alpha.x, or a literal). NO_ALPHATEST=1
     * leaves it out. */
    /* Compiled in, the program is keyed on its units' gamma decode, cubes and
     * scales; the callers pass 0 for them, and they are worked out here only
     * then (each walks the 16 units, and both lookups of every draw paid it). */
    if (!buffered && udr) {
        gamma = dr_gamma_key(udr);
        unnorm = dr_unnorm_key(udr);
    }
    /* The units declared on their own: cubes in the low 16 bits, volumes in the
     * high 16, of the units the program samples (a stale binding on another
     * unit would make a variant of its own). A buffered program has them
     * declared too unless RSX_TEX_SEP=0. */
    if (udr && (!buffered || tex_sep_on())) {
        const u32 su = (udr->fp_snap && udr->fp_addr == fp_addr) ? udr->fp_units : 0xFFFFu;
        cube_mask = ((dr_cube_mask(udr) | dr_vol_mask(udr) << 16) & (su | su << 16));
    }
    const u32 cube_units = cube_mask & 0xFFFFu, vol_units = cube_mask >> 16;
    /* Texture sign expansion (SET_TEXTURE_ADDRESS UNSIGNED_REMAP_BIASED): the
     * lanes each sampled unit re-centres to -1..1 (rsx_texture_expand_mask),
     * compiled in and keyed on, as the other backends do. No program of God
     * of War: Ascension's menu, opening cinematic or first level uses it.
     * RSX_TEX_EXPAND=0 leaves them out, as before. */
    u8 exp_masks[16] = {0};
    u64 expand = 0;
    if (udr && tex_expand_on()) {
        const u32 su = (udr->fp_snap && udr->fp_addr == fp_addr) ? udr->fp_units : 0xFFFFu;
        for (int u = 0; u < VP_TEX_UNITS; u++) {
            if (!((su >> u) & 1u) || !udr->tex[u].set) continue;
            exp_masks[u] = rsx_texture_expand_mask(udr->tex[u].fmt, udr->tex[u].ctrl1,
                                                   udr->tex[u].filter, udr->tex[u].addr);
            if (exp_masks[u]) expand = (expand ^ ((u64)exp_masks[u] << 8 | (u64)u)) * 1099511628211ull + 1u;
        }
    }
    /* Depth textures the RSX compares (shadow maps): the comparison is compiled
     * in (_rsx_shadowN) and keyed on, and with it the gamma decode of the
     * program's other units (the shadow entry point takes the masks rather
     * than choosing them at run time). */
    rsx_fp_shadow_desc shd[16];
    const u64 shadow = (buffered && udr) ? dr_shadow_descs(udr, shd) : 0u;
    if (shadow) gamma = dr_gamma_key(udr);
    /* An antialiased surface is drawn at its sample grid; WPOS reads the
     * pixel (rsx_fp_apply_wpos_grid), so the grid is part of the key. */
    u32 aax = 1, aay = 1;
    if (udr) rsx_aa_grid(udr->rt_fmt, &aax, &aay);
    const u32 aa = (aax > 1 || aay > 1) ? (aax | aay << 4) : 0u;
    /* Nor, buffered, on the units' gamma decode or on which are cubes. The
     * decode is chosen when the program runs (_rsx_gamma_dyn, its lanes in
     * fp_tcscale[u].z), and every unit's coordinate takes its scale from b1,
     * a cube's being 1 (dr_coord_scales); the buffered decompile declares the
     * 2D array, which the cube rewrite below does not change. Compiled in,
     * each combination of a program's bindings was a program of its own: God
     * of War: Ascension with its MLAA off added 250-700 to the shader cache a
     * run and skipped ~80K draws waiting for them. */
    const u32 wpos_key = (!buffered && udr) ? udr->wpos_win : 0u;
    u32 atest = 0;
    if (udr && ((udr->alpha_ctl >> 16) & 1u) && ((udr->alpha_ctl >> 8) & 0xFFu) < 7u &&
        !env_on("NO_ALPHATEST", &s_env_noalphatest)) {
        atest = 0x200u | ((udr->alpha_ctl >> 8) & 0xFFu);
        if (!buffered) atest |= (udr->alpha_ctl & 0xFFu) << 16;
    }

    s_perf_pso_calls++;
    /* Keyed on the code, not on where it lives. The compiled shader reads its
     * inline constants from the draw's own fp_addr (fp_k[], refilled per draw),
     * so a program's pipeline is the same wherever the title puts it. God of
     * War HD uploads its fragment programs into a ring every frame: keyed on the
     * address, every draw of every frame missed and went through a full
     * decompile + D3DCompile (179 draws -> seconds per frame on its menu). */
    for (int i = s_fp_head[uhash & (VP_FP_HASH - 1)] - 1; i >= 0; i = s_fp_next[i] - 1)
        if (s_d3d.vp_fp[i].ucode_hash != uhash) continue;   /* cheap reject first */
        else if (s_d3d.vp_fp[i].vs_idx == vs_idx &&
            s_d3d.vp_fp[i].vs_hash == vs_hash && s_d3d.vp_fp[i].gen == s_d3d.vp_gen &&
            s_d3d.vp_fp[i].blend == blend && s_d3d.vp_fp[i].nrt == nrt &&
            s_d3d.vp_fp[i].rtfmt == (u32)rtfmt && s_d3d.vp_fp[i].ctrl == ctrl &&
            s_d3d.vp_fp[i].ucode_hash == uhash && s_d3d.vp_fp[i].cmask == cmask &&
            s_d3d.vp_fp[i].cull == cull &&
            s_d3d.vp_fp[i].cube_mask == cube_mask && s_d3d.vp_fp[i].raw == raw &&
            s_d3d.vp_fp[i].expand == expand &&
            s_d3d.vp_fp[i].gamma == gamma && s_d3d.vp_fp[i].unnorm == unnorm &&
            s_d3d.vp_fp[i].shadow == shadow && s_d3d.vp_fp[i].aa == aa &&
            s_d3d.vp_fp[i].wpos == wpos_key && s_d3d.vp_fp[i].atest == atest &&
            s_d3d.vp_fp[i].poly == pkey &&
            s_d3d.vp_fp[i].ext == ext && s_d3d.vp_fp[i].mrt == mrt &&
            s_d3d.vp_fp[i].zs == zs) {
            s_d3d.vp_fp[i].last_use = s_d3d.frame_count;
            return vp_fp_ready(&s_d3d.vp_fp[i]);
        }
    s_perf_pso_miss++;      /* falls through to a full decompile + D3DCompile */
    if (s_env_psowhy < 0) { const char* e = getenv("PSO_MISS_WHY"); s_env_psowhy = e ? atoi(e) : 0; }
    if (s_env_psowhy) {
        u32 best = 0; int bestn = PW_N + 1, bi = -1;
        const u64 poly = pkey;
        for (int i = s_fp_head[uhash & (VP_FP_HASH - 1)] - 1; i >= 0; i = s_fp_next[i] - 1) {
            const VPFPEntry* q = &s_d3d.vp_fp[i];
            if (q->ucode_hash != uhash) continue;
            const u32 m = (u32)(q->vs_idx != vs_idx) << PW_VS_IDX | (u32)(q->vs_hash != vs_hash) << PW_VS_HASH |
                (u32)(q->gen != s_d3d.vp_gen) << PW_GEN | (u32)(q->blend != blend) << PW_BLEND |
                (u32)(q->nrt != nrt) << PW_NRT | (u32)(q->rtfmt != (u32)rtfmt) << PW_RTFMT |
                (u32)(q->ctrl != ctrl) << PW_CTRL | (u32)(q->cmask != cmask) << PW_CMASK |
                (u32)(q->cull != cull) << PW_CULL | (u32)(q->cube_mask != cube_mask) << PW_CUBE |
                (u32)(q->raw != raw) << PW_RAW | (u32)(q->gamma != gamma) << PW_GAMMA |
                (u32)(q->unnorm != unnorm) << PW_UNNORM | (u32)(q->wpos != wpos_key) << PW_WPOS |
                (u32)(q->atest != atest) << PW_ATEST | (u32)(q->poly != poly) << PW_POLY |
                (u32)(q->ext != ext) << PW_EXT | (u32)(q->mrt != mrt) << PW_MRT | (u32)(q->zs != zs) << PW_ZS;
            int n = 0;
            for (u32 x = m; x; x &= x - 1) n++;
            if (n < bestn) { bestn = n; best = m; bi = i; }
        }
        if (bestn > PW_N) s_pw_new++;
        else for (int b = 0; b < PW_N; b++) if ((best >> b) & 1u) s_pw_n[b]++;
        if (bi >= 0 && ((best >> PW_CTRL) & 1u)) s_pw_ctrl |= s_d3d.vp_fp[bi].ctrl ^ ctrl;
    }
    /* PSOMISSDBG=1: on a miss, name the key field that differs from an existing
     * entry for the same program. "the cache misses" is not actionable; "it
     * misses on ucode_hash" is. */
    /* HASHDBG=1: how many DISTINCT ucode hashes each program produces. Inline
     * constants are patched into the shader bytes, so a program whose constants
     * animate yields a new hash -- and a new pipeline -- every time. */
    { static int hd = -1;
      if (hd < 0) { const char* e = getenv("HASHDBG"); hd = e ? atoi(e) : 0; }
      if (hd) { static u32 fps[32]; static u32 cnt[32]; static int nf = 0;
        int idx = -1;
        for (int i = 0; i < nf; i++) if (fps[i] == fp_addr) idx = i;
        if (idx < 0 && nf < 32) { idx = nf++; fps[idx] = fp_addr; cnt[idx] = 0; }
        if (idx >= 0) { cnt[idx]++;
          if ((cnt[idx] % 200) == 0)
            fprintf(stderr, "[HASHDBG] fp=0x%X has produced %u distinct compiles%c",
                    fp_addr, cnt[idx], 10); } } }
    { static int md = -1;
      if (md < 0) { const char* e = getenv("PSOMISSDBG"); md = e ? atoi(e) : 0; }
      if (md) { static int n = 0;
        for (int i = 0; i < s_d3d.vp_fp_n && n < 12; i++) {
            VPFPEntry* q = &s_d3d.vp_fp[i];
            if (q->fp_addr != fp_addr) continue;
            n++;
            fprintf(stderr, "[PSOMISS] fp=0x%X differs:%s%s%s%s%s%s%s%s%c", fp_addr,
                    q->vs_idx != vs_idx        ? " vs_idx"     : "",
                    q->vs_hash != vs_hash      ? " vs_hash"    : "",
                    q->gen != s_d3d.vp_gen     ? " gen"        : "",
                    q->blend != blend          ? " blend"      : "",
                    q->nrt != nrt              ? " nrt"        : "",
                    q->rtfmt != (u32)rtfmt     ? " rtfmt"      : "",
                    q->ucode_hash != uhash     ? " ucode_hash" : "",
                    q->cube_mask != cube_mask  ? " cube_mask"  : "", 10);
            break;
        } } }
    /* CUBEKEYDBG=1: which (program, cube mask) pairs are being compiled. If one
     * program shows several masks, the mask is unstable and is the reason the
     * pipeline cache thrashes with cube support on. */
    { static int ck = -1;
      if (ck < 0) { const char* e = getenv("CUBEKEYDBG"); ck = e ? atoi(e) : 0; }
      if (ck) { static u32 seen[64][2]; static int ns = 0; int known = 0;
        for (int i = 0; i < ns; i++)
            if (seen[i][0] == fp_addr && seen[i][1] == cube_mask) known = 1;
        if (!known && ns < 64) { seen[ns][0] = fp_addr; seen[ns][1] = cube_mask; ns++;
            fprintf(stderr, "[CUBEKEY] fp=0x%X cube_mask=0x%X (distinct pairs=%d)%c",
                    fp_addr, cube_mask, ns, 10); } } }
    static char hlsl[RSX_FP_HLSL_BUFFER_SIZE];
    /* The third parameter is the SET_SHADER_CONTROL word; its 0x40 bit picks
     * the export registers (r0/r2/r3/r4 for 32-bit, h0/h4/h6/h8 for 16-bit).
     * Each draw records that bit (fp_ctrl, also part of the pipeline key), so
     * pass it rather than letting the decompiler guess from "was r0 written".
     * The guess fails on a program that uses r0 as a temp and exports through
     * h0: God of War HD's 2x-AA resolve computes (B-A)/2 in r0 and the average
     * in h0, and with r0 exported every 3D frame came out black. The depth
     * export bits come with it: the same resolve writes the scene depth it
     * reads from the AA depth surface into the resolved one, and without them
     * it wrote the quad's own depth there, so every depth-tested pass after it
     * (shadows, fog, rain, glows) was rejected.
     * RSX_FP_CTRL_AUTO=1 restores the guess. */
    static int ctrl_auto = -1;
    if (ctrl_auto < 0) ctrl_auto = getenv("RSX_FP_CTRL_AUTO") ? 1 : 0;
    u32 fp_ctrl = ctrl_auto ? RSX_FP_CTRL_AUTO : (ctrl & ~FP_CTRL_SRGB_OUT);
    u8 gamma_masks[16] = {0};
    for (int u = 0; u < VP_TEX_UNITS; u++) gamma_masks[u] = (u8)((gamma >> (4 * u)) & 15u);
    u32 nk = 0;
    int n = (buffered && shadow)
        ? rsx_fp_decompile_buffered_shadow_color_expand_vol_ex(prog, prog_max, fp_ctrl,
                                                    cube_units, vol_units, shd, gamma_masks,
                                                    expand ? exp_masks : NULL,
                                                    hlsl, sizeof(hlsl), &nk)
        : buffered
        ? rsx_fp_decompile_buffered_color_dyn_vol_ex(prog, prog_max, fp_ctrl, cube_units, vol_units,
                                                     expand ? exp_masks : NULL,
                                                     hlsl, sizeof(hlsl), &nk)
        : rsx_fp_decompile_color_vol_ex(prog, prog_max, fp_ctrl, cube_units, vol_units,
                                        gamma ? gamma_masks : NULL, expand ? exp_masks : NULL,
                                        hlsl, sizeof(hlsl));
    if (expand) {
        static int _n = 0;
        if (PS3_LOG_FIRST(_n, 8)) {
            fprintf(stderr, "[EXPAND] fp=0x%X lanes", fp_addr);
            for (int u = 0; u < VP_TEX_UNITS; u++)
                if (exp_masks[u]) fprintf(stderr, " t%d=0x%02X", u, exp_masks[u]);
            fprintf(stderr, "%c", 10);
        }
    }
    /* SET_SHADER_PACKER: encode the colour outputs to sRGB, as the RSX does
     * before they reach an integer target. */
    if (n > 0 && !ctrl_auto && (ctrl & FP_CTRL_SRGB_OUT) &&
        rsx_fp_apply_srgb_output(hlsl, sizeof(hlsl)) < 0) {
        static int _w = 0;
        if (PS3_LOG_FIRST(_w, 8))
            fprintf(stderr, "[FP] fp=0x%08X: sRGB output not applied (shape)%c", fp_addr, 10);
    }
    if (n <= 0) { static int _e=0; if(PS3_LOG_FIRST(_e, 16)) printf("[FP] decompile fail (fp=0x%08X)\n", fp_addr); return NULL; }
    if (buffered) {
        /* The decompiler declares fp_constants[] before fp_alpha, which then
         * moves with the program's constant count. Put the fixed members
         * first, with the coordinate scales, so that every program's slot
         * starts the same way (vp_record_cb). */
        char from[192], to[224];
        const u32 nd = nk ? nk : 1u;
        snprintf(from, sizeof from, "cbuffer PSConstants : register(b1) {\n    float4 fp_constants[%u];\n"
                                    "    float4 fp_alpha;\n};\n", nd);
        snprintf(to, sizeof to, "cbuffer PSConstants : register(b1) {\n    float4 fp_tcscale[16];\n"
                                "    float4 fp_alpha;\n    float4 fp_constants[%u];\n};\n", nd);
        if (!strstr(hlsl, from)) {
            static int _e = 0;
            if (PS3_LOG_FIRST(_e, 8))
                fprintf(stderr, "[FP] fp=0x%08X: PSConstants not in the expected form, not drawn%c", fp_addr, 10);
            return NULL;
        }
        hlsl_replace_all(hlsl, sizeof hlsl, from, to);
        hlsl_replace_all(hlsl, sizeof hlsl, "#define _rsx_gamma_mask(u) 0u\n",
                         "#define _rsx_gamma_mask(u) ((uint)fp_tcscale[u].z)\n");
        fp_apply_coord_scale(hlsl, sizeof(hlsl), 0xFFFFu & ~(cube_units | vol_units), NULL);
        if (shadow) fp_shadow_scale(hlsl, sizeof(hlsl), shd);
    } else if (unnorm && udr) {
        float sc[VP_TEX_UNITS][2];
        const u32 mask = dr_coord_scales(udr, sc);
        fp_apply_coord_scale(hlsl, sizeof(hlsl), mask & ~(cube_units | vol_units), (const float (*)[2])sc);
    }
    if (aa && rsx_fp_apply_wpos_grid(hlsl, sizeof(hlsl), aax, aay) < 0) {
        static int _w = 0;
        if (PS3_LOG_FIRST(_w, 8))
            fprintf(stderr, "[FP] fp=0x%08X: WPOS sample grid not applied (shape)%c", fp_addr, 10);
    }
    /* WPOS in the draw's window convention: y counted up from the bottom edge
     * of a window that tall, and/or integer pixel centres (the draw engine's
     * fp_alpha.y/.z, here compiled in). */
    if (!buffered && udr && udr->wpos_win) {
        const u32 wv = udr->wpos_win;
        const char* c = ((wv >> 16) & 0xFu) ? " - 0.5" : "";
        char ydef[48], def[160];
        if ((wv >> 12) & 0xFu) snprintf(ydef, sizeof ydef, "%u.0 - (p).y%s", wv & 0xFFFu, c);
        else                   snprintf(ydef, sizeof ydef, "(p).y%s", c);
        snprintf(def, sizeof def, "#define _rsx_wpos(p) float4((p).x%s, %s, (p).z, (p).w)\n", c, ydef);
        hlsl_replace_all(hlsl, sizeof hlsl, "#define _rsx_wpos(p) (p)\n", def);
    }
    /* Internal resolution: WPOS back in guest pixels by the target's own factor, which replay
     * puts in fp_alpha.w. Only when it is on, so that at 1 every program is the text it was. */
    if (buffered && s_wide != 1.0f) {
        /* A target drawn wider has -1/sy in fp_alpha.w; x is then that much further. */
        char wp[256];
        snprintf(wp, sizeof wp, "float4((p).x * abs(fp_alpha.w) * (fp_alpha.w < 0.0 ? %.7f : 1.0) + fp_alpha.z, (fp_alpha.y != 0.0 ? "
                 "fp_alpha.y - (p).y * abs(fp_alpha.w) : (p).y * abs(fp_alpha.w)) + fp_alpha.z,", 1.0f / s_wide);
        hlsl_replace_all(hlsl, sizeof hlsl,
                         "float4((p).x + fp_alpha.z, (fp_alpha.y != 0.0 ? fp_alpha.y - (p).y : (p).y) + fp_alpha.z,", wp);
    } else if (buffered && s_rs != 1.0f)
        hlsl_replace_all(hlsl, sizeof hlsl,
                         "float4((p).x + fp_alpha.z, (fp_alpha.y != 0.0 ? fp_alpha.y - (p).y : (p).y) + fp_alpha.z,",
                         "float4((p).x * fp_alpha.w + fp_alpha.z, (fp_alpha.y != 0.0 ? fp_alpha.y - (p).y * fp_alpha.w : "
                         "(p).y * fp_alpha.w) + fp_alpha.z,");
    /* The guest alpha test: D3D12 has none of its own, so the program
     * discards what fails it. */
    if (atest) {
        const int r = buffered
            ? rsx_fp_apply_alpha_test_buffered(hlsl, sizeof hlsl, atest & 0xFFFFu)
            : rsx_fp_apply_alpha_test(hlsl, sizeof hlsl, atest & 0xFFFFu, (float)(atest >> 16) / 255.0f);
        if (r < 0) {
            static int _w = 0;
            if (PS3_LOG_FIRST(_w, 8))
                fprintf(stderr, "[FP] fp=0x%08X: alpha test 0x%X not applied (shape)%c", fp_addr, atest & 0xFFFFu, 10);
        }
    }
    /* FP_LIST=1: every program the title actually compiles, with its size. A
     * fragment program that hangs the GPU shows up here as an implausible
     * instruction count long before it shows up as a TDR. */
    { static int fl = -1; if (fl < 0) fl = getenv("FP_LIST") ? 1 : 0;
      if (fl) fprintf(stderr, "[FPLIST] fp=0x%08X instrs=%d hlsl=%u bytes%c",
                      fp_addr, n, (unsigned)strlen(hlsl), 10); }
    /* FP_DUMP=<n>: the first n compiled programs (1 = the old default of 4). */
    if (getenv("FP_DUMP")) { static int _d=0, _cap=-1;
      if (_cap < 0) { _cap = atoi(getenv("FP_DUMP")); if (_cap <= 1) _cap = 4; }
      if (PS3_LOG_FIRST(_d, _cap)) {
        FILE* f = fopen("fp_dump.hlsl", _d==1 ? "w" : "a");
        if (f) {
            const u8* uc = prog;
            fprintf(f, "/* fp_addr=0x%08X vmoff=0x%08X raw:", fp_addr, off);
            for (int _b = 0; _b < 32; _b++) fprintf(f, " %02X", uc[_b]);
            fprintf(f, " */\n%s\n", hlsl); fclose(f);
        } } }

    /* FP_PICK=<hex fp_addr>: write just that program, whatever order it compiles
     * in. fp_dump.hlsl keeps only the first four, same trap as vp2_dump.hlsl.
     * FP_PICK=u<hex>: the program with that key hash (the "ucode" in a RenderDoc
     * capture's pipeline names), wherever the title put it. */
    { static const char* fpk = (const char*)1; static u32 wantf = 0; static int done = 0, by_hash = 0;
      if (fpk == (const char*)1) { fpk = getenv("FP_PICK");
                                   by_hash = fpk && (fpk[0] == 'u' || fpk[0] == 'U');
                                   wantf = fpk ? (u32)strtoul(fpk + by_hash, NULL, 16) : 0; }
      if (wantf && !done && (by_hash ? uhash == wantf : fp_addr == wantf)) { done = 1;
          FILE* f = fopen("duck_fp.hlsl", "w");
          if (f) { fprintf(f, "/* fp_addr=0x%08X, %d instrs */%s", fp_addr, n, hlsl);
                   fclose(f); }
          /* and its ucode as the guest has it (duck_fp.bin) */
          { const u32 sz = rsx_fp_program_size(prog, prog_max);
            FILE* fb = sz ? fopen("duck_fp.bin", "wb") : NULL;
            if (fb) { fwrite(prog, 1, sz, fb); fclose(fb); } }
          fprintf(stderr, "[DUCKFP] wrote duck_fp.hlsl (fp=0x%08X, %d instrs)%c",
                  fp_addr, n, 10);
      } }

    /* Debug FP_ONE=<hex fp_addr>: force that program's colour output to
     * all-ones (e.g. wave's colour-detect -> full mask -> island borders
     * everywhere -> the water sim must visibly radiate if it works). */
    { const char* f1 = getenv("FP_ONE");
      if (f1 && (u32)strtoul(f1, NULL, 16) == fp_addr) {
          /* Match the assignment, not one exact spelling of it: the final
           * register is r[N] for some programs and h[N] for others, and the old
           * fixed "= r[0];" anchor silently did nothing for every h[] shader --
           * so this switch appeared to have no effect and was useless for
           * identifying which program draws what. Rewrite the expression with a
           * proper length-changing splice. */
          char* rp = strstr(hlsl, "_po.c0 = ");
          if (rp) {
              char* semi = strchr(rp, ';');
              if (semi) {
                  static const char rep[] = "_po.c0 = (1.0).xxxx";
                  size_t oldlen = (size_t)(semi - rp), newlen = sizeof(rep) - 1;
                  size_t tail = strlen(semi) + 1;
                  if ((rp - hlsl) + newlen + tail < sizeof(hlsl)) {
                      memmove(rp + newlen, semi, tail);
                      memcpy(rp, rep, newlen);
                  }
                  (void)oldlen;
              }
          }
      } }
    /* FP_TEX=1: make every program output its unit-0 texture sample verbatim.
     * Separates "the texture is wrong" from "the shading tints it" in one run,
     * without having to identify which program draws which surface first. */
    /* FP_SHOW=<hlsl expr>: replace a program's colour output with an arbitrary
     * expression (e.g. FP_SHOW=h[1]) so an intermediate register can be looked
     * at directly. Restrict with FP_TEX_FP=<hex>. Reading a 69-instruction
     * shader to guess which term carries a bad value is slower and less certain
     * than displaying the terms. */
    /* FP_KILL=<hex fp_addr>: make that program discard every fragment, so what
     * it covers becomes visible. Distinguishes "this surface shades black" from
     * "this surface is correct and something else is missing behind it". */
    { const char* kf = getenv("FP_KILL");
      if (kf && (u32)strtoul(kf, NULL, 16) == fp_addr) {
          char* rp = strstr(hlsl, "PSOut _po;");
          if (rp) {
              const char* ins = "discard; ";
              size_t nl = strlen(ins), tail = strlen(rp) + 1;
              if ((size_t)(rp - hlsl) + nl + tail < sizeof(hlsl)) {
                  memmove(rp + nl, rp, tail);
                  memcpy(rp, ins, nl);
                  fprintf(stderr, "[FPKILL] fp=0x%X discards%c", fp_addr, 10);
              }
          }
      } }
    { const char* sh = getenv("FP_SHOW");
      const char* only = getenv("FP_TEX_FP");
      if (sh && (!only || (u32)strtoul(only, NULL, 16) == fp_addr)) {
          char* rp = strstr(hlsl, "_po.c0 = ");
          if (rp) { char* semi = strchr(rp, ';');
            if (semi) {
                char rep[256];
                snprintf(rep, sizeof rep, "_po.c0 = float4((%s).xyz, 1.0)", sh);
                size_t newlen = strlen(rep), tail = strlen(semi) + 1;
                if ((size_t)(rp - hlsl) + newlen + tail < sizeof(hlsl)) {
                    memmove(rp + newlen, semi, tail);
                    memcpy(rp, rep, newlen);
                    fprintf(stderr, "[FPSHOW] fp=0x%X output replaced with %s%c",
                            fp_addr, sh, 10);
                }
            } }
      } }
    /* Cube and volume units are declared by the decompiler (TextureCube,
     * Texture3D on their own t-register, sampled with three coordinates). */
    if (cube_mask) {
        static int _n = 0;
        if (PS3_LOG_FIRST(_n, 8))
            fprintf(stderr, "[TEXSEP] fp=0x%X cube units 0x%X, volume units 0x%X%c",
                    fp_addr, cube_units, vol_units, 10);
    }
    /* FP_IDCOLOR=1: give every fragment program a distinct flat colour derived
     * from its address. One frame then shows which program paints which surface,
     * instead of one run per candidate to test them by elimination. */
    if (getenv("FP_IDCOLOR")) {
        char* rp = strstr(hlsl, "_po.c0 = ");
        if (rp) {
            char* semi = strchr(rp, ';');
            if (semi) {
                /* Mix properly: every fp address in a title tends to share low
                 * bits (they all end 0x01/0x81 here), so a single multiply left
                 * one channel constant across all programs and made distinct
                 * shaders look like the same colour. */
                u32 hsh = fp_addr * 2654435761u;
                hsh ^= hsh >> 13; hsh *= 0x5BD1E995u; hsh ^= hsh >> 15;
                /* Force the top bit on in each channel: a hash that happens to
                 * land near black is indistinguishable from "this surface was
                 * never painted", which is the exact question the mode exists
                 * to answer. */
                float cr = (float)(((hsh >> 16) & 0x7F) | 0x80) / 255.0f;
                float cg = (float)(((hsh >>  8) & 0x7F) | 0x80) / 255.0f;
                float cb = (float)(((hsh      ) & 0x7F) | 0x80) / 255.0f;
                char rep[128];
                snprintf(rep, sizeof rep, "_po.c0 = float4(%.3f,%.3f,%.3f,1.0)", cr, cg, cb);
                size_t newlen = strlen(rep), tail = strlen(semi) + 1;
                if ((size_t)(rp - hlsl) + newlen + tail < sizeof(hlsl)) {
                    memmove(rp + newlen, semi, tail);
                    memcpy(rp, rep, newlen);
                    fprintf(stderr, "[FPID] fp=0x%X -> rgb(%.0f,%.0f,%.0f)%c",
                            fp_addr, cr*255, cg*255, cb*255, 10);
                }
            }
        }
    }
    /* FP_TEX=<unit>: output that unit's sample verbatim. FP_TEX_FP=<hex> limits
     * it to one program, so one surface can be inspected without repainting the
     * whole scene. FP_TEX_UV=1 samples with the same texcoord the program uses
     * for that unit rather than tc0. */
    if (getenv("FP_TEX") && (!getenv("FP_TEX_FP") ||
        (u32)strtoul(getenv("FP_TEX_FP"), NULL, 16) == fp_addr)) {
        int _un = atoi(getenv("FP_TEX"));
        if (_un < 0 || _un > 3) _un = 0;
        char* rp = strstr(hlsl, "_po.c0 = ");
        if (rp) {
            char* semi = strchr(rp, ';');
            if (semi) {
                char rep[160];
                /* FP_TEX_UV=1: sample at a fixed centre UV instead of the
                 * interpolated texcoord. If the texture is populated but the
                 * texcoords never arrive, the normal path samples texel (0,0)
                 * -- usually black -- and looks exactly like an empty texture.
                 * This tells the two apart. */
                if (getenv("FP_TEX_UV"))
                    snprintf(rep, sizeof rep,
                             "_po.c0 = rsx_tex%d.Sample(rsx_samp%d, float2(0.5, 0.5))",
                             _un, _un);
                else
                snprintf(rep, sizeof rep,
                         "_po.c0 = rsx_tex%d.Sample(rsx_samp%d, input.tc0.xy * rsx_texscale[%d].xy)",
                         _un, _un, _un);
                size_t newlen = strlen(rep), tail = strlen(semi) + 1;
                fprintf(stderr, "[FP_TEX] program 0x%X rewritten to sample unit %d%c",
                        fp_addr, _un, 10);
                if ((size_t)(rp - hlsl) + newlen + tail < sizeof(hlsl)) {
                    memmove(rp + newlen, semi, tail);
                    memcpy(rp, rep, newlen);
                }
            }
        }
    }
    /* FP_FORCE=1: replace the translated body with solid magenta -- isolates
     * geometry/transform problems from texture/blend problems. */
    if (getenv("FP_FORCE")) {
        char* rp = strstr(hlsl, "return r[0];");
        if (rp) memcpy(rp, "return float4(1,0,1,1)", 23);
    }
    /* With workers, the program is compiled and the pipeline created by one
     * (sj_build), from the description below. */
    const int async = sj_workers() > 0;
    ID3DBlob* pb = NULL; ID3DBlob* e = NULL;
    HRESULT hr = S_OK;
    if (!async) {
        hr = d3d_compile_cached(hlsl, strlen(hlsl), "guest_fp", "ps_5_0", &pb, &e);
        if (FAILED(hr) || !pb) {
            static int _e2=0; if (PS3_LOG_FIRST(_e2, 16))
                printf("[FP] PS compile FAIL (fp=0x%08X): %s\n", fp_addr,
                       e ? (const char*)e->lpVtbl->GetBufferPointer(e) : "?");
            if (e) e->lpVtbl->Release(e);
            return NULL;
        }
        if (e) e->lpVtbl->Release(e);
    }

    /* Static: a worker may create the pipeline after this returns. */
    static D3D12_INPUT_ELEMENT_DESC il[16];
    if (!il[0].SemanticName)
        for (int _i = 0; _i < 16; _i++) {
            il[_i].SemanticName = "ATTR"; il[_i].SemanticIndex = _i;
            il[_i].Format = DXGI_FORMAT_R32G32B32A32_FLOAT; il[_i].InputSlot = 0;
            il[_i].AlignedByteOffset = (UINT)(_i * 16);
            il[_i].InputSlotClass = D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA;
            il[_i].InstanceDataStepRate = 0;
        }
    D3D12_GRAPHICS_PIPELINE_STATE_DESC pd = {0};
    pd.pRootSignature = s_d3d.vp_root_sig;
    pd.VS.pShaderBytecode = vsb->lpVtbl->GetBufferPointer(vsb);
    pd.VS.BytecodeLength  = vsb->lpVtbl->GetBufferSize(vsb);
    if (pb) {
        pd.PS.pShaderBytecode = pb->lpVtbl->GetBufferPointer(pb);
        pd.PS.BytecodeLength  = pb->lpVtbl->GetBufferSize(pb);
    }
    pd.InputLayout.pInputElementDescs = raw ? NULL : il; pd.InputLayout.NumElements = raw ? 0 : 16;
    pd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    /* Guest face culling. CULL_OFF=1 forces double-sided, the old
     * unconditional behaviour -- geometry that vanishes under culling is a
     * winding/front-face problem, not a missing draw. */
    { static int _co = -1; if (_co < 0) _co = getenv("CULL_OFF") ? 1 : 0;
      pd.RasterizerState.CullMode = (!_co && (cull & 1u))
          ? ((cull & 2u) ? D3D12_CULL_MODE_FRONT : D3D12_CULL_MODE_BACK)
          : D3D12_CULL_MODE_NONE;
      /* CULL_FLIP=1: invert the front-face convention. Our VP path may hand
       * D3D the opposite winding from the guest's, in which case honouring
       * CULL_FACE culls exactly the faces it should keep. */
      static int _cfl = -1; if (_cfl < 0) _cfl = getenv("CULL_FLIP") ? 1 : 0;
      pd.RasterizerState.FrontCounterClockwise =
          (((cull & 4u) ? 1 : 0) ^ _cfl) ? TRUE : FALSE; }
    /* 2.4: SET_ZMIN_MAX_CONTROL bit 0 culls what falls outside the depth range
     * (D3D12's depth clip); clear (0x110 here: clamp instead) it does not. The
     * guest pipelines had DepthClipEnable left at FALSE, a clamp for all. */
    pd.RasterizerState.DepthClipEnable = (ext & 2u) ? TRUE : FALSE;
    /* Polygon offset: offset = factor * max depth slope + bias * r, the same
     * formula on both sides (rsx_poly_key). */
    { const u64 po = udr ? udr->poly : 0u;
      if (po && dyb_ready()) {
          pd.Flags |= D3D12_PIPELINE_STATE_FLAG_DYNAMIC_DEPTH_BIAS;   /* RSSetDepthBias per draw */
      } else if (po) {
          const u32 fb = (u32)(po >> 32), bb = (u32)po;
          float f, b; memcpy(&f, &fb, sizeof f); memcpy(&b, &bb, sizeof b);
          pd.RasterizerState.DepthBias = (INT)b;
          pd.RasterizerState.SlopeScaledDepthBias = f;
          pd.RasterizerState.DepthBiasClamp = 0.0f;
      } }
    /* Blend per the guest's packed key (see rsx_blend_key): dbgfont text needs
     * straight alpha; demosaic's effect passes blend OFF; DeferredShading's
     * light accumulation is additive ONE,ONE. */
    pd.BlendState.RenderTarget[0].BlendEnable    = (blend & 1) ? TRUE : FALSE;
    pd.BlendState.RenderTarget[0].SrcBlend       = (blend & 1) ? (D3D12_BLEND)((blend >> 1)  & 0x1F) : D3D12_BLEND_ONE;
    pd.BlendState.RenderTarget[0].DestBlend      = (blend & 1) ? (D3D12_BLEND)((blend >> 6)  & 0x1F) : D3D12_BLEND_ZERO;
    pd.BlendState.RenderTarget[0].BlendOp        = (blend & 1) ? (D3D12_BLEND_OP)((blend >> 21) & 0x7) : D3D12_BLEND_OP_ADD;
    pd.BlendState.RenderTarget[0].SrcBlendAlpha  = (blend & 1) ? (D3D12_BLEND)((blend >> 11) & 0x1F) : D3D12_BLEND_ONE;
    pd.BlendState.RenderTarget[0].DestBlendAlpha = (blend & 1) ? (D3D12_BLEND)((blend >> 16) & 0x1F) : D3D12_BLEND_ZERO;
    pd.BlendState.RenderTarget[0].BlendOpAlpha   = (blend & 1) ? (D3D12_BLEND_OP)((blend >> 24) & 0x7) : D3D12_BLEND_OP_ADD;
    pd.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    pd.SampleMask = UINT_MAX;
    pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pd.NumRenderTargets = (UINT)nrt;
    for (int _r = 0; _r < nrt; _r++) {
        pd.RTVFormats[_r] = rtfmt;
        /* Zero-init leaves RenderTargetWriteMask 0 on secondary targets --
         * every MRT-B write would be masked off. Mirror RT0's blend state. */
        pd.BlendState.RenderTarget[_r] = pd.BlendState.RenderTarget[0];
    }
    /* Guest colour write mask (RGBA nibble, already D3D-ordered). cmask is 0xF
     * by default (unset colour_mask register decodes to all-on); a literal 0
     * therefore means the guest EXPLICITLY masked every channel -- a depth-only
     * pass (e.g. DeferredShading's shadow-map generation). Honour it: forcing 0
     * back to 0xF splatters the depth pass's fragment colour onto the target. */
    /* CMASK_FORCE=1: ignore the guest colour mask (write RGBA on every draw).
     * A mis-decoded mask makes geometry rasterize correctly and write nothing,
     * which is indistinguishable from "the draw never happened". */
    { static int _cf = -1; if (_cf < 0) _cf = getenv("CMASK_FORCE") ? 1 : 0;
      if (_cf) cmask = 0xF; }
    for (int _r = 0; _r < nrt; _r++)
        pd.BlendState.RenderTarget[_r].RenderTargetWriteMask = (UINT8)(cmask & 0xF);
    /* 2.2: targets B-D (MRT 1-3) take their own nibble of SET_COLOR_MASK_MRT
     * (A, R, G, B at bits 0-3) and bit of SET_BLEND_ENABLE_MRT, target A the
     * main colour mask and blend enable -- the rule the Vulkan and Metal
     * backends keep. With A's state on every target, a pass that leaves the
     * G-buffer's alpha alone (0xEEE0, a few thousand draws a minute here)
     * wrote over it. */
    if (mrt) {
        static int cf2 = -1;
        if (cf2 < 0) cf2 = getenv("CMASK_FORCE") ? 1 : 0;
        pd.BlendState.IndependentBlendEnable = TRUE;
        for (int _r = 1; _r < nrt; _r++) {
            const u32 nib = (mrt >> (_r * 4)) & 0xFu;
            UINT8 wm = 0;
            if (nib & 1u) wm |= D3D12_COLOR_WRITE_ENABLE_ALPHA;
            if (nib & 2u) wm |= D3D12_COLOR_WRITE_ENABLE_RED;
            if (nib & 4u) wm |= D3D12_COLOR_WRITE_ENABLE_GREEN;
            if (nib & 8u) wm |= D3D12_COLOR_WRITE_ENABLE_BLUE;
            pd.BlendState.RenderTarget[_r].RenderTargetWriteMask = cf2 ? (UINT8)0xF : wm;
            pd.BlendState.RenderTarget[_r].BlendEnable =
                pd.BlendState.RenderTarget[0].BlendEnable && ((mrt >> (16 + _r)) & 1u);
        }
    }
    pd.DSVFormat = DXGI_FORMAT_D24_UNORM_S8_UINT;
    /* Guest depth/stencil state (rsx_zs_key). This was a fixed LESS_EQUAL
     * depth test with writes and no stencil, whatever the title set: God of
     * War HD's stencil shadow volumes then marked nothing and its shadow pass,
     * a full-screen reverse-subtract quad meant only for the marked pixels,
     * darkened the whole frame. */
    {
        D3D12_DEPTH_STENCIL_DESC* ds = &pd.DepthStencilState;
        ds->DepthEnable    = (zs & 1u) ? TRUE : FALSE;
        ds->DepthWriteMask = (zs & 2u) ? D3D12_DEPTH_WRITE_MASK_ALL : D3D12_DEPTH_WRITE_MASK_ZERO;
        ds->DepthFunc      = (D3D12_COMPARISON_FUNC)(((zs >> 2) & 7u) + 1u);
        ds->StencilEnable  = (zs & (1u << 5)) ? TRUE : FALSE;
        ds->StencilReadMask  = (UINT8)(zs >> 32);
        ds->StencilWriteMask = (UINT8)(zs >> 40);
        ds->FrontFace.StencilFunc        = (D3D12_COMPARISON_FUNC)(((zs >> 6) & 7u) + 1u);
        ds->FrontFace.StencilFailOp      = (D3D12_STENCIL_OP)(((zs >> 9) & 7u) + 1u);
        ds->FrontFace.StencilDepthFailOp = (D3D12_STENCIL_OP)(((zs >> 12) & 7u) + 1u);
        ds->FrontFace.StencilPassOp      = (D3D12_STENCIL_OP)(((zs >> 15) & 7u) + 1u);
        ds->BackFace.StencilFunc         = (D3D12_COMPARISON_FUNC)(((zs >> 18) & 7u) + 1u);
        ds->BackFace.StencilFailOp       = (D3D12_STENCIL_OP)(((zs >> 21) & 7u) + 1u);
        ds->BackFace.StencilDepthFailOp  = (D3D12_STENCIL_OP)(((zs >> 24) & 7u) + 1u);
        ds->BackFace.StencilPassOp       = (D3D12_STENCIL_OP)(((zs >> 27) & 7u) + 1u);
    }
    /* DEPTH_OFF=1: drop the depth test for guest-FP draws, so submission order
     * alone decides what is on top. Paired with DRAW_LAST_TEX this puts one
     * object in front of everything and answers "is it merely occluded?" --
     * which reordering alone cannot, because a relocated draw still fails the
     * depth test against whatever is already in the buffer. Diagnostic. */
    { static int doff = -1;
      if (doff < 0) { const char* e = getenv("DEPTH_OFF"); doff = e ? atoi(e) : 0; }
      if (doff) { pd.DepthStencilState.DepthEnable = FALSE;
                  pd.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO; } }
    pd.SampleDesc.Count = 1;

    ID3D12PipelineState* pso = NULL;
    ShaderJob* job = NULL;
    /* D3D12_PSO_CACHE: built at the start from an earlier session's file. */
    psoc_start();
    u8 pst[PSOC_STATE_MAX];
    u32 pstn = 0;
    u64 psokey = 0;
    if (s_psoc_mode == 1 && (pstn = psoc_state_bytes(&pd, (int)(ext & 1u), pst)) != 0) {
        int known = 0;
        psokey = psoc_key(hlsl, strlen(hlsl), pd.VS.pShaderBytecode, pd.VS.BytecodeLength, pst, pstn);
        pso = psoc_find(psokey, &known, 1);
    }
    if (pso) {
        if (pb) pb->lpVtbl->Release(pb);
    } else if (async) {
        job = sj_submit(hlsl, strlen(hlsl), "guest_fp", "ps_5_0", vsb, &pd, (int)(ext & 1u));
        if (!job) return NULL;
    } else {
        { const double t0 = perf_now();
          hr = guest_pso_create(&pd, (int)(ext & 1u), &pso);
          s_shc_stat_pso++; s_shc_stat_pso_s += perf_now() - t0; }
        if (SUCCEEDED(hr) && pso && pstn)
            psoc_add(psokey, pso, pst, pstn, pd.VS.pShaderBytecode, (u32)pd.VS.BytecodeLength,
                     pb->lpVtbl->GetBufferPointer(pb), (u32)pb->lpVtbl->GetBufferSize(pb));
        pb->lpVtbl->Release(pb);
        if (FAILED(hr)) {
            if (hr == DXGI_ERROR_DEVICE_REMOVED) device_lost_report("building a pipeline");
            static int _e3=0; if (PS3_LOG_FIRST(_e3, 16)) printf("[FP] PSO FAIL (fp=0x%08X, 0x%08lX)\n", fp_addr, hr);
            return NULL;
        }
        { static int _ok=0; if (PS3_LOG_FIRST(_ok, 32)) printf("[FP] guest FP pipeline ready (fp=0x%08X)\n", fp_addr); }
        rdoc_name((ID3D12Object*)pso, "pso fp 0x%08X ucode %08X vs %d", fp_addr, uhash, vs_idx);
    }

    /* Full: drop the pipelines no draw has asked for for longest, an eighth of
     * the cache or more at once behind one wait for the GPU. It used to drop
     * the oldest one, waiting for the GPU, for every pipeline added once the
     * cache was full -- God of War: Ascension's first level fills it. Never
     * one asked for this frame or the last: the open command list may bind it. */
    if (s_d3d.vp_fp_n >= VP_FP_CACHE) {
        const u64 fc = s_d3d.frame_count;
        u64 age = 1024;
        int k = 0;
        for (;;) {
            k = 0;
            for (int i = 0; i < s_d3d.vp_fp_n; i++) k += fc - s_d3d.vp_fp[i].last_use >= age;
            if (k >= VP_FP_CACHE / 8 || age <= 2) break;
            age /= 2;
        }
        if (!k) {
            static int _f = 0;
            if (PS3_LOG_FIRST(_f, 4))
                fprintf(stderr, "[FP] all %d cached pipelines asked for in two frames; not cached%c", VP_FP_CACHE, 10);
            if (pso) rf_release(pso);
            if (job) { s_pso_pending = 1; sj_release(job); }
            return NULL;
        }
        wait_for_gpu();
        int w = 0;
        for (int i = 0; i < s_d3d.vp_fp_n; i++) {
            VPFPEntry* q = &s_d3d.vp_fp[i];
            if (fc - q->last_use >= age) {
                if (q->pso) q->pso->lpVtbl->Release(q->pso);
                sj_release(q->job);
                s_perf_fp_dropped++;
                continue;
            }
            if (w != i) s_d3d.vp_fp[w] = *q;
            w++;
        }
        s_d3d.vp_fp_n = w;
        vp_fp_index_rebuild();
    }
    s_d3d.vp_fp[s_d3d.vp_fp_n].fp_addr = fp_addr;
    s_d3d.vp_fp[s_d3d.vp_fp_n].vs_idx  = vs_idx;
    s_d3d.vp_fp[s_d3d.vp_fp_n].vs_hash = vs_hash;
    s_d3d.vp_fp[s_d3d.vp_fp_n].gen     = s_d3d.vp_gen;
    s_d3d.vp_fp[s_d3d.vp_fp_n].blend   = blend;
    s_d3d.vp_fp[s_d3d.vp_fp_n].nrt     = nrt;
    s_d3d.vp_fp[s_d3d.vp_fp_n].rtfmt   = (u32)rtfmt;
    s_d3d.vp_fp[s_d3d.vp_fp_n].ctrl    = ctrl;
    s_d3d.vp_fp[s_d3d.vp_fp_n].ucode_hash = uhash;
    s_d3d.vp_fp[s_d3d.vp_fp_n].cmask   = cmask;
    s_d3d.vp_fp[s_d3d.vp_fp_n].cull    = cull;
    s_d3d.vp_fp[s_d3d.vp_fp_n].cube_mask = cube_mask;
    s_d3d.vp_fp[s_d3d.vp_fp_n].gamma     = gamma;
    s_d3d.vp_fp[s_d3d.vp_fp_n].unnorm    = unnorm;
    s_d3d.vp_fp[s_d3d.vp_fp_n].shadow    = shadow;
    s_d3d.vp_fp[s_d3d.vp_fp_n].expand    = expand;
    s_d3d.vp_fp[s_d3d.vp_fp_n].wpos      = wpos_key;
    s_d3d.vp_fp[s_d3d.vp_fp_n].atest     = atest;
    s_d3d.vp_fp[s_d3d.vp_fp_n].last_use  = s_d3d.frame_count;
    s_d3d.vp_fp[s_d3d.vp_fp_n].raw     = raw;
    s_d3d.vp_fp[s_d3d.vp_fp_n].zs      = zs;
    s_d3d.vp_fp[s_d3d.vp_fp_n].poly    = pkey;
    s_d3d.vp_fp[s_d3d.vp_fp_n].ext     = ext;
    s_d3d.vp_fp[s_d3d.vp_fp_n].aa      = aa;
    s_d3d.vp_fp[s_d3d.vp_fp_n].mrt     = mrt;
    s_d3d.vp_fp[s_d3d.vp_fp_n].pso     = pso;
    s_d3d.vp_fp[s_d3d.vp_fp_n].job     = job;
    { const int n = s_d3d.vp_fp_n;
      const u32 h = uhash & (VP_FP_HASH - 1);
      s_fp_next[n] = s_fp_head[h];
      s_fp_head[h] = n + 1; }
    s_d3d.vp_fp_n++;
    return job ? vp_fp_ready(&s_d3d.vp_fp[s_d3d.vp_fp_n - 1]) : pso;
}

/* Upload the guest texture for a VP draw into a per-frame texture slot
 * (re-uploaded every frame: gcm/cube's plasma animates in guest memory).
 * Returns the slot index or -1. Must run while the command list is open,
 * before the draw passes. */
/* NV4097 TEXTURE_CONTROL1 component remap -> D3D12 Shader4ComponentMapping.
 * The crossbar decode itself is RSX semantics and lives in
 * rsx_texture_layout.c; all that is left here is packing the four selectors
 * into D3D12's word. Its force-zero/force-one encodings are 4 and 5, which is
 * what RSX_REMAP_ZERO/ONE already are, so the selectors pass straight through.
 *
 * out[] arrives in the crossbar's field order A,R,G,B; D3D12 wants
 * destR | destG<<3 | destB<<6 | destA<<9 | valid<<12. */
static u32 rsx_remap_to_d3d(u32 c1, u32 basef)
{
    u8 out[4];
    rsx_texture_component_remap(c1, basef, out);
    /* TEX_REMAP_ID=<n>: override the derived crossbar with a fixed mapping, to
     * test channel order directly. The resource holds the guest's A8R8G8B8
     * bytes straight through, so its components are (R=A, G=R, B=G, A=B) and
     * the shader needs destR=1, destG=2, destB=3, destA=0 to see real RGBA.
     *   1 = identity   2 = rotate (the ARGB fix)   3 = BGRA swap */
    { static int fixed = -1;
      if (fixed < 0) { const char* e = getenv("TEX_REMAP_ID"); fixed = e ? atoi(e) : 0; }
      if (fixed == 1) return 0u | (1u<<3) | (2u<<6) | (3u<<9) | (1u<<12);
      if (fixed == 2) return 1u | (2u<<3) | (3u<<6) | (0u<<9) | (1u<<12);
      if (fixed == 3) return 2u | (1u<<3) | (0u<<6) | (3u<<9) | (1u<<12); }
    /* D3D12 mapping: destR | destG<<3 | destB<<6 | destA<<9 | valid bit */
    { static int _rd = -1; if (_rd < 0) _rd = getenv("REMAPDBG") ? 1 : 0;
      if (_rd) { static u32 seen[32]; static int ns=0;
        u32 k = (basef << 16) | (c1 & 0xFFFFu); int f=0;
        for (int i2=0;i2<ns;i2++) if (seen[i2]==k) f=1;
        if (!f && ns<32) { seen[ns++]=k;
            fprintf(stderr, "[REMAP] basef=0x%02X c1=0x%04X -> destR=%u destG=%u destB=%u destA=%u%c",
                    basef, c1 & 0xFFFFu, out[1], out[2], out[3], out[0], 10); } } }
    return out[1] | (out[2] << 3) | (out[3] << 6) | (out[0] << 9) | (1u << 12);
}

/* Morton/Z-order texel offset for RSX swizzled textures (LN bit clear).
 * Interleaves x (even bit positions) and y (odd) until the smaller dimension's
 * bits run out, then the larger dimension's remaining bits ride above -- the
 * NV40 layout (matches RPCS3 convert_linear_swizzle). POT dims only, which is
 * what the hardware requires for swizzled textures. */

/* Sparse FNV-1a over a texture's source bytes, enough to notice an animated
 * surface changing: every eighth of up to 1,024 samples, from sample `part`. */
static u32 tex_csum_part(const u8* base, u32 nbytes, u32 part)
{
    u32 h = 2166136261u;
    const u32 step = nbytes > 4096u ? nbytes / 1024u : 4u;
    for (u32 i = part * step; i + 3 < nbytes; i += step * TEX_CSUM_PARTS) {
        u32 w32; memcpy(&w32, base + i, sizeof w32);
        h ^= w32; h *= 16777619u;
    }
    return h;
}

/* Whether a held texture's source bytes differ from what was uploaded, at its
 * first bind of a frame. All the samples of every texture every frame was 11%
 * of the RSX thread, nearly all of it waiting on memory, a line a sample, for
 * textures that hardly ever change. So a check reads one part, in turn: a new
 * image shows in any part at once, and a change in a few samples within eight
 * checks. 4 KB or less is read whole, as those are the textures it sees to the
 * word. TEX_CHECK_ALL=1: every part at every check. */
/* An upload whose source bytes changed while they were read: the first eight said, then every
 * power of two. A report with these lines had a texture the title wrote while it was read. */
static volatile LONG s_tex_torn;
static void tex_torn_note(u32 off, u32 w, u32 h, u32 fmt)
{
    const LONG n = InterlockedIncrement(&s_tex_torn);
    if (n <= 8 || !(n & (n - 1)))
        fprintf(stderr, "[tex] 0x%08X %ux%u format 0x%02X changed while it was read (%ld so far): read again "
                        "at its next bind%c", off, w, h, fmt, (long)n, 10);
}

static int tex_changed(VPTexSlot* c, const u8* base)
{
    static int all = -1;
    if (all < 0) all = getenv("TEX_CHECK_ALL") ? 1 : 0;
    if (c->again) return 1;
    for (u32 n = all || c->csum_bytes <= 4096u ? TEX_CSUM_PARTS : 1u; n; n--) {
        const u32 p = c->csum_next++ % TEX_CSUM_PARTS;
        if (tex_csum_part(base, c->csum_bytes, p) != c->csum[p]) return 1;
    }
    return 0;
}

/* The DXGI format a guest texture is uploaded as -- the DECODED image's, which
 * is not always the guest's: the packed 16-bit colour formats arrive unpacked
 * to RGBA8, and DEPTH24_D8 as a float. rsx_texture_layout.h says which class
 * each format byte lands in. The SRV over the upload must name the same one. */
static DXGI_FORMAT vp_tex_dxgi_of(u32 fmt)
{
    rsx_tex_layout tl;
    rsx_texture_layout(fmt, 4, 4, &tl);
    switch (tl.fmt) {
    case RSX_TEXFMT_R8G8B8A8:      return DXGI_FORMAT_R8G8B8A8_UNORM;
    case RSX_TEXFMT_R8G8:          return DXGI_FORMAT_R8G8_UNORM;
    case RSX_TEXFMT_BC1:           return DXGI_FORMAT_BC1_UNORM;
    case RSX_TEXFMT_BC2:           return DXGI_FORMAT_BC2_UNORM;
    case RSX_TEXFMT_BC3:           return DXGI_FORMAT_BC3_UNORM;
    case RSX_TEXFMT_R16:           return DXGI_FORMAT_R16_UNORM;
    case RSX_TEXFMT_R16G16:        return DXGI_FORMAT_R16G16_UNORM;
    case RSX_TEXFMT_R16G16F:       return DXGI_FORMAT_R16G16_FLOAT;
    case RSX_TEXFMT_R16G16B16A16F: return DXGI_FORMAT_R16G16B16A16_FLOAT;
    case RSX_TEXFMT_R32F:          return DXGI_FORMAT_R32_FLOAT;
    case RSX_TEXFMT_R32G32B32A32F: return DXGI_FORMAT_R32G32B32A32_FLOAT;
    default:                       return DXGI_FORMAT_R8_UNORM;
    }
}

/* Asked for every texture unit of every draw, and only these bits of the format
 * byte choose the answer (rsx_texture_layout_pitched's base format). */
static DXGI_FORMAT vp_tex_dxgi(u32 fmt)
{
    static DXGI_FORMAT memo[0x9F + 1];
    DXGI_FORMAT* m = &memo[fmt & 0x9Fu];
    if (*m == DXGI_FORMAT_UNKNOWN) *m = vp_tex_dxgi_of(fmt);
    return *m;
}

/* TEX_STATS=<frames>: the texture binds of the prepass by outcome, every so many
 * frames: per frame, how many and how long (1 bound already this frame; 2 held
 * from an earlier frame, checksum unchanged; 3 checksum changed, uploaded again;
 * 4 not held, uploaded into a free or evicted slot; 5 refused), textures
 * evicted, resources created, and what the cache holds. */
enum { TS_HIT = 1, TS_SAME, TS_CHANGED, TS_NEW, TS_FAIL, TS_N };
static int s_ts_out;
static u64 s_ts_n[TS_N], s_ts_evict, s_ts_create, s_ts_reuse;
static double s_ts_t[TS_N];

/* The texture cache: a hash table over the slots' keys, and what their
 * resources take against vp_tex_budget(). Slots are handed out in order; once
 * all have been, vp_tex_alloc takes an empty one or evicts. */
static int s_tex_head[1 << VP_TEX_BUCKET_BITS];   /* slot + 1, 0 = empty chain */
static int s_tex_hw;                              /* slots handed out so far */
static u64 s_tex_bytes;                           /* held by the slots' resources */
/* Upload buffers of textures that did not fit in the ring: released by the
 * next rf_open, after its wait for the GPU; the batch's copies read them. */
static ID3D12Resource* s_tex_staging[1024];
static int s_tex_staging_n;

/* RSX_CENSUS: the level-0 texture decode by format (base format, swizzled,
 * compressed): textures, texels and time since the last report. */
static struct { u64 n, texels; double s; } s_tdp[256][2][2];
static void tex_decode_note(const rsx_tex_layout* tl, u32 w, u32 h, double secs)
{
    if (!rsx_census_secs()) return;
    const u32 f = tl->base_format & 0xFFu, sw = tl->swizzled ? 1u : 0u, cp = tl->compressed ? 1u : 0u;
    s_tdp[f][sw][cp].n++;
    s_tdp[f][sw][cp].texels += (u64)w * h;
    s_tdp[f][sw][cp].s += secs;
}
static void tex_decode_print(void)
{
    fprintf(stderr, "[census] texture decode (format, swizzled, compressed: textures, Mtexels, ms):");
    for (int f = 0; f < 256; f++)
        for (int sw = 0; sw < 2; sw++)
            for (int cp = 0; cp < 2; cp++)
                if (s_tdp[f][sw][cp].n)
                    fprintf(stderr, " %02X%s%s: %llu, %.2f, %.1f", f, sw ? " swz" : "", cp ? " dxt" : "",
                            (unsigned long long)s_tdp[f][sw][cp].n, s_tdp[f][sw][cp].texels / 1e6,
                            s_tdp[f][sw][cp].s * 1000.0);
    fprintf(stderr, "%c", 10);
    memset(s_tdp, 0, sizeof s_tdp);
}

/* Texture uploads that do not fit in the frame's part of the ring go to spill
 * chunks of TEX_RING_PART, made when a frame first needs them and kept for the
 * frames after it; one unused for TEX_SPILL_IDLE frames is released. They got
 * an upload buffer each before: 43-110 on entering God of War: Ascension's
 * first level, 25-98 ms on the frame clock thread. */
#define TEX_SPILL_MAX  8
#define TEX_SPILL_IDLE 600u
typedef struct { ID3D12Resource* res; u8* ptr; u64 used, last; } TexSpill;
static TexSpill s_tex_spill[FRAME_COUNT][TEX_SPILL_MAX];

/* `bytes` from frame index fi's spill chunks: the chunk and its offset in *at,
 * or NULL. */
static TexSpill* tex_spill_take(u32 fi, u64 bytes, u64* at)
{
    if (bytes > TEX_RING_PART) return NULL;
    for (int i = 0; i < TEX_SPILL_MAX; i++) {
        TexSpill* c = &s_tex_spill[fi][i];
        if (!c->res) {
            const double c0 = perf_now();
            D3D12_HEAP_PROPERTIES hu = {0}; hu.Type = D3D12_HEAP_TYPE_UPLOAD;
            D3D12_RESOURCE_DESC bd = {0};
            bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
            bd.Width = TEX_RING_PART;
            bd.Height = 1; bd.DepthOrArraySize = 1;
            bd.MipLevels = 1; bd.SampleDesc.Count = 1; bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
            void* p = NULL; D3D12_RANGE nr = {0, 0};
            if (FAILED(s_d3d.device->lpVtbl->CreateCommittedResource(
                    s_d3d.device, &hu, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_GENERIC_READ, NULL,
                    &IID_ID3D12Resource, (void**)&c->res)))
                return NULL;
            if (FAILED(c->res->lpVtbl->Map(c->res, 0, &nr, &p)) || !p) {
                c->res->lpVtbl->Release(c->res); c->res = NULL;
                return NULL;
            }
            c->ptr = (u8*)p; c->used = 0;
            s_hl_upbuf_n++; s_hl_upbuf_s += perf_now() - c0;
        }
        const u64 a = (c->used + 511u) & ~(u64)511u;   /* placement alignment */
        if (a + bytes <= TEX_RING_PART) {
            c->used = a + bytes; c->last = s_d3d.frame_count;
            *at = a;
            return c;
        }
    }
    return NULL;
}

/* A new frame on index fi: its spill chunks are free again (the batch that
 * used them has run), and one long unused is released. */
static void tex_spill_reset(u32 fi)
{
    for (int i = 0; i < TEX_SPILL_MAX; i++) {
        TexSpill* c = &s_tex_spill[fi][i];
        if (!c->res) continue;
        c->used = 0;
        /* The first chunk stays: made again for the next burst, 32 MB of
         * upload heap cost 35 ms on the frame clock thread (God of War:
         * Ascension's first fight, the first of Megaera's effects). */
        if (i > 0 && s_d3d.frame_count - c->last > TEX_SPILL_IDLE) {
            c->res->lpVtbl->Release(c->res);
            memset(c, 0, sizeof *c);
        }
    }
}

static void vp_tex_release_staging(void)
{
    for (int i = 0; i < s_tex_staging_n; i++)
        s_tex_staging[i]->lpVtbl->Release(s_tex_staging[i]);
    s_tex_staging_n = 0;
}

static void vp_tex_cache_reset(void)
{
    memset(s_tex_head, 0, sizeof s_tex_head);
    s_tex_hw = 0; s_tex_bytes = 0; s_tex_staging_n = 0;
}

static u32 vp_tex_hash(u32 key, u32 w, u32 h, u32 fmt, int cube)
{
    u32 x = (key * 0x9E3779B1u) ^ (w * 0x85EBCA77u) ^ (h * 0xC2B2AE3Du) ^ (fmt << 16) ^ (u32)cube;
    x ^= x >> 15; x *= 0x2C1B3C6Du; x ^= x >> 12;
    return x & ((1u << VP_TEX_BUCKET_BITS) - 1u);
}

static int vp_tex_find(u32 key, u32 w, u32 h, u32 fmt, int cube)
{
    for (int i = s_tex_head[vp_tex_hash(key, w, h, fmt, cube)]; i; i = s_d3d.vp_tex[i - 1].next) {
        const VPTexSlot* c = &s_d3d.vp_tex[i - 1];
        if (c->key == key && c->w == w && c->h == h && c->fmt == fmt && c->cube == cube)
            return i - 1;
    }
    return -1;
}

static void vp_tex_link(int slot)
{
    VPTexSlot* t = &s_d3d.vp_tex[slot];
    int* head = &s_tex_head[vp_tex_hash(t->key, t->w, t->h, t->fmt, t->cube)];
    t->next = *head; *head = slot + 1; t->linked = 1;
}

static void vp_tex_unlink(int slot)
{
    VPTexSlot* t = &s_d3d.vp_tex[slot];
    if (!t->linked) return;
    int* p = &s_tex_head[vp_tex_hash(t->key, t->w, t->h, t->fmt, t->cube)];
    while (*p && *p != slot + 1) p = &s_d3d.vp_tex[*p - 1].next;
    if (*p) *p = t->next;
    t->next = 0; t->linked = 0;
}

/* Texture memory: a guest texture is placed in a heap of TEX_HEAP_BYTES that
 * it shares with others, instead of an allocation of its own. A committed
 * texture took ~0.6 ms to create on the frame clock thread, and on entering
 * God of War: Ascension's first level 150-174 are created in one frame: 95-107
 * ms of it, placed 5-14 ms. Heaps
 * are made as textures need them, up to what the cache may hold and a little
 * over, and are not zeroed (an upload writes every texel first). A heap's free
 * space is a list of blocks by offset, joined again as textures leave. A
 * texture no heap has room for, or every one with RSX_TEX_PLACED=0, gets an
 * allocation of its own as before. */
#define TEX_HEAP_BYTES (64ull << 20)
#define TEX_HEAPS      48
static u64 vp_tex_budget(void);
typedef struct { u64 off, size; } TexFree;
typedef struct {
    ID3D12Heap* heap;
    TexFree* fr;                /* free blocks, by offset */
    int nfr, capfr;
} TexHeap;
static TexHeap s_theap[TEX_HEAPS];
static int s_theap_n;
static unsigned s_hl_heap_n;    /* hitch log: heaps made */
static double s_hl_heap_s;

static int tex_placed_on(void)
{
    static int v = -1;
    if (v < 0) { const char* e = getenv("RSX_TEX_PLACED"); v = e ? (atoi(e) != 0) : 1; }
    return v;
}

static int tex_heap_room(TexHeap* th)
{
    if (th->nfr < th->capfr) return 1;
    const int cap = th->capfr ? th->capfr * 2 : 64;
    TexFree* n = (TexFree*)realloc(th->fr, (size_t)cap * sizeof *n);
    if (!n) return 0;
    th->fr = n; th->capfr = cap;
    return 1;
}

/* `size` bytes at `align` from the heap's free blocks: the offset, or ~0. */
static u64 tex_heap_take(TexHeap* th, u64 size, u64 align)
{
    for (int i = 0; i < th->nfr; i++) {
        const u64 b0 = th->fr[i].off, b1 = b0 + th->fr[i].size;
        const u64 at = (b0 + align - 1) & ~(align - 1);
        if (at + size > b1) continue;
        if (at > b0 && at + size < b1) {          /* a block either side */
            if (!tex_heap_room(th)) return ~0ull;
            memmove(&th->fr[i + 2], &th->fr[i + 1], (size_t)(th->nfr - i - 1) * sizeof th->fr[0]);
            th->nfr++;
            th->fr[i].size = at - b0;
            th->fr[i + 1].off = at + size; th->fr[i + 1].size = b1 - (at + size);
        } else if (at > b0) {
            th->fr[i].size = at - b0;
        } else if (at + size < b1) {
            th->fr[i].off = at + size; th->fr[i].size = b1 - (at + size);
        } else {
            memmove(&th->fr[i], &th->fr[i + 1], (size_t)(th->nfr - i - 1) * sizeof th->fr[0]);
            th->nfr--;
        }
        return at;
    }
    return ~0ull;
}

/* A block back to the heap, joined to its neighbours. */
static void tex_heap_give(TexHeap* th, u64 off, u64 size)
{
    int i = 0;
    while (i < th->nfr && th->fr[i].off < off) i++;
    const int prev = i > 0 && th->fr[i - 1].off + th->fr[i - 1].size == off;
    const int next = i < th->nfr && off + size == th->fr[i].off;
    if (prev && next) {
        th->fr[i - 1].size += size + th->fr[i].size;
        memmove(&th->fr[i], &th->fr[i + 1], (size_t)(th->nfr - i - 1) * sizeof th->fr[0]);
        th->nfr--;
    } else if (prev) {
        th->fr[i - 1].size += size;
    } else if (next) {
        th->fr[i].off = off; th->fr[i].size += size;
    } else if (tex_heap_room(th)) {
        memmove(&th->fr[i + 1], &th->fr[i], (size_t)(th->nfr - i) * sizeof th->fr[0]);
        th->fr[i].off = off; th->fr[i].size = size;
        th->nfr++;
    }
}

/* The texture `td` placed in a heap, its heap + 1 and block in *heap, *off and
 * *size; NULL when no heap has room and no other may be made. */
static ID3D12Resource* tex_place(const D3D12_RESOURCE_DESC* td, int* heap, u64* off, u64* size)
{
    D3D12_RESOURCE_ALLOCATION_INFO ai;
    s_d3d.device->lpVtbl->GetResourceAllocationInfo(s_d3d.device, &ai, 0, 1, td);
    if (!ai.SizeInBytes || ai.SizeInBytes > TEX_HEAP_BYTES) return NULL;
    const u64 align = ai.Alignment ? ai.Alignment : D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT;
    const u64 sz = (ai.SizeInBytes + align - 1) & ~(align - 1);
    int h = 0;
    u64 at = ~0ull;
    for (; h < s_theap_n; h++)
        if ((at = tex_heap_take(&s_theap[h], sz, align)) != ~0ull) break;
    if (at == ~0ull) {
        if (s_theap_n == TEX_HEAPS || (u64)s_theap_n * TEX_HEAP_BYTES >= vp_tex_budget() + 4 * TEX_HEAP_BYTES)
            return NULL;
        const double h0 = perf_now();
        D3D12_HEAP_DESC hd;
        memset(&hd, 0, sizeof hd);
        hd.SizeInBytes = TEX_HEAP_BYTES;
        hd.Properties.Type = D3D12_HEAP_TYPE_DEFAULT;
        hd.Alignment = D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT;
        hd.Flags = D3D12_HEAP_FLAG_ALLOW_ONLY_NON_RT_DS_TEXTURES | D3D12_HEAP_FLAG_CREATE_NOT_ZEROED;
        ID3D12Heap* hp = NULL;
        if (FAILED(s_d3d.device->lpVtbl->CreateHeap(s_d3d.device, &hd, &IID_ID3D12Heap, (void**)&hp))) {
            hd.Flags = D3D12_HEAP_FLAG_ALLOW_ONLY_NON_RT_DS_TEXTURES;   /* a runtime without CREATE_NOT_ZEROED */
            if (FAILED(s_d3d.device->lpVtbl->CreateHeap(s_d3d.device, &hd, &IID_ID3D12Heap, (void**)&hp)))
                return NULL;
        }
        h = s_theap_n++;
        s_theap[h].heap = hp;
        tex_heap_give(&s_theap[h], 0, TEX_HEAP_BYTES);
        s_hl_heap_n++; s_hl_heap_s += perf_now() - h0;
        if ((at = tex_heap_take(&s_theap[h], sz, align)) == ~0ull) return NULL;
    }
    ID3D12Resource* r = NULL;
    if (FAILED(s_d3d.device->lpVtbl->CreatePlacedResource(s_d3d.device, s_theap[h].heap, at, td,
            D3D12_RESOURCE_STATE_COPY_DEST, NULL, &IID_ID3D12Resource, (void**)&r))) {
        tex_heap_give(&s_theap[h], at, sz);
        return NULL;
    }
    *heap = h + 1; *off = at; *size = sz;
    return r;
}

/* Release a slot's resources. Only ever a slot this batch has not bound: the
 * batches before it have run (rf_open waits for the GPU), so nothing in flight
 * reads them. */
static u32 s_srvq_gen = 1;
static void srvq_forget(ID3D12Resource* res);
static void vp_tex_drop(VPTexSlot* t)
{
    t->srv_n = 0;
    s_srvq_gen++;
    if (t->res) srvq_forget(t->res);
    if (t->res) { t->res->lpVtbl->Release(t->res); t->res = NULL; }
    if (t->heap) { tex_heap_give(&s_theap[t->heap - 1], t->hoff, t->hsize); t->heap = 0; }
    if (t->up) { t->up->lpVtbl->Release(t->up); t->up = NULL; }
    s_tex_bytes -= t->bytes; t->bytes = 0;
}

/* TEX_CACHE_MB=<n>: what the cached textures may take. Default: a third of the
 * adapter's VRAM (an integrated GPU's: with the memory it shares) within
 * 256-2048 MB, 512 MB when that is not known. */
static u64 vp_tex_budget(void)
{
    static u64 b;
    if (!b) {
        const char* e = getenv("TEX_CACHE_MB");
        const int v = e ? atoi(e) : 0;
        u64 mb = v > 0 ? (u64)v : 0;
        if (!mb) {
            mb = s_vram_mb ? s_vram_mb / 3u : 512u;
            if (mb < 256) mb = 256;
            if (mb > 2048) mb = 2048;
        }
        b = mb << 20;
    }
    return b;
}

/* The least recently used slot holding a texture this batch has not bound; -1
 * when there is none. */
static int vp_tex_lru(void)
{
    int best = -1;
    for (int i = 0; i < s_tex_hw; i++) {
        const VPTexSlot* c = &s_d3d.vp_tex[i];
        if (c->res && !c->used &&
            (best < 0 || c->last_frame < s_d3d.vp_tex[best].last_frame)) best = i;
    }
    return best;
}

/* A slot for a texture the cache does not hold, `need` bytes: over the budget,
 * the least recently used textures go first. Then a slot not handed out yet,
 * an empty one, or the least recently used, whose resource vp_upload_tex_slot
 * reuses when the size and format match. -1 when this batch binds them all. */
/* Frames a texture has gone unbound before a new one of the same size, format
 * and level count may take over its resource. */
#define VP_TEX_REUSE_AGE 60u

static int vp_tex_alloc(u64 need, u32 w, u32 h, u32 fmt, int cube, u32 levels)
{
    /* When the cache is full, a texture of the same size, format and levels
     * not bound for VP_TEX_REUSE_AGE frames hands its slot and resource over
     * as they are: creating a texture resource costs ~1-1.5 ms on the frame
     * clock thread and releasing one ~0.5 ms, and on entering a new area God
     * of War: Ascension uploads 90-150 new textures in a frame (113-236 ms).
     * RSX_TEX_TAKEOVER=0 always releases and creates. */
    static int takeover = -1;
    if (takeover < 0) { const char* e = getenv("RSX_TEX_TAKEOVER"); takeover = e ? (atoi(e) != 0) : 1; }
    if (takeover && (s_tex_bytes + need > vp_tex_budget() || s_tex_hw == VP_TEX_SLOTS)) {
        const u32 now = (u32)s_d3d.frame_count;
        int best = -1;
        for (int i = 0; i < s_tex_hw; i++) {
            const VPTexSlot* c = &s_d3d.vp_tex[i];
            if (c->res && !c->used && c->w == w && c->h == h && c->fmt == fmt && c->cube == cube &&
                c->levels == levels && now - c->last_frame >= VP_TEX_REUSE_AGE &&
                (best < 0 || c->last_frame < s_d3d.vp_tex[best].last_frame)) best = i;
        }
        if (best >= 0) { vp_tex_unlink(best); s_ts_evict++; s_ts_reuse++; return best; }
    }
    const double ev0 = perf_now();
    while (s_tex_bytes + need > vp_tex_budget()) {
        const int v = vp_tex_lru();
        if (v < 0) break;
        vp_tex_unlink(v);
        vp_tex_drop(&s_d3d.vp_tex[v]);
        s_ts_evict++;
    }
    s_tex_evict_s += perf_now() - ev0;
    if (s_tex_hw < VP_TEX_SLOTS) return s_tex_hw++;
    for (int i = 0; i < VP_TEX_SLOTS; i++)
        if (!s_d3d.vp_tex[i].res && !s_d3d.vp_tex[i].linked) return i;
    const int v = vp_tex_lru();
    if (v >= 0) { vp_tex_unlink(v); s_ts_evict++; }
    return v;
}
static int tex_stats_frames(void)
{
    static int v = -1;
    if (v < 0) { const char* e = getenv("TEX_STATS"); v = e ? atoi(e) : 0; }
    return v;
}
static void tex_stats_note(double dt)
{
    static u64 f0 = ~0ull;
    const u64 fc = (u64)s_d3d.frame_count;
    if (f0 == ~0ull) f0 = fc;
    if (s_ts_out > 0 && s_ts_out < TS_N) { s_ts_n[s_ts_out]++; s_ts_t[s_ts_out] += dt; }
    if (fc - f0 >= (u64)tex_stats_frames()) {
        const double nf = (double)(fc - f0);
        int live = 0;
        for (int i = 0; i < s_tex_hw; i++) live += s_d3d.vp_tex[i].res != NULL;
        static const char* const name[TS_N] = { "", "bound this frame", "same checksum", "changed", "new", "refused" };
        fprintf(stderr, "[tex-stats] per frame:");
        for (int k = 1; k < TS_N; k++)
            fprintf(stderr, " %s %.0f (%.2f ms)", name[k], s_ts_n[k] / nf, s_ts_t[k] * 1000.0 / nf);
        static double cr_prev, ev_prev, dc_prev;
        fprintf(stderr, "; evicted %.1f (%.2f ms), created %.1f (%.2f ms), taken over %.1f; decode %.2f ms; "
                        "%d textures held, %.0f of %.0f MB, %d heaps\n",
                s_ts_evict / nf, (s_tex_evict_s - ev_prev) * 1000.0 / nf,
                s_ts_create / nf, (s_tex_create_s - cr_prev) * 1000.0 / nf, s_ts_reuse / nf,
                (s_tex_decode_s - dc_prev) * 1000.0 / nf, live,
                s_tex_bytes / 1048576.0, vp_tex_budget() / 1048576.0, s_theap_n);
        cr_prev = s_tex_create_s; ev_prev = s_tex_evict_s; dc_prev = s_tex_decode_s;
        fflush(stderr);
        for (int k = 0; k < TS_N; k++) { s_ts_n[k] = 0; s_ts_t[k] = 0; }
        s_ts_evict = s_ts_create = s_ts_reuse = 0; f0 = fc;
    }
}

/* TEX_MIPCHECK=<n>: for the first n textures uploaded with a mip chain, the
 * mean colour of each level as read from guest memory -- decoded for the
 * uncompressed formats, from the block end-point colours for DXT. A level
 * that is a downscale of the one above keeps its mean; one read from the
 * wrong place does not. Checks rsx_texture_mip_chain against the title. */
static void tex_mipcheck(u32 off, u32 w, u32 h, u32 fmt, u32 nlev, const rsx_tex_level* lv)
{
    static int cap = -1, n = 0;
    if (cap < 0) { const char* e = getenv("TEX_MIPCHECK"); cap = e ? atoi(e) : 0; }
    if (!cap || n >= cap || nlev < 2) return;
    n++;
    extern uint8_t* vm_base;
    fprintf(stderr, "[mipcheck] off=0x%08X fmt=0x%02X %ux%u levels %u:", off, fmt, w, h, nlev);
    for (u32 l = 0; l < nlev; l++) {
        const u8* src = vm_base + off + lv[l].offset;
        double sum[4] = { 0, 0, 0, 0 }; u64 cnt = 0;
        if (lv[l].tl.compressed) {
            const u32 bsz = lv[l].tl.fmt == RSX_TEXFMT_BC1 ? 8u : 16u;
            const u32 nb = lv[l].tl.face_bytes / bsz;
            for (u32 b = 0; b < nb; b++) {
                const u8* blk = src + (u64)b * bsz + (bsz == 16u ? 8u : 0u);
                for (int k = 0; k < 2; k++) {
                    const u16 c = (u16)(blk[k * 2] | (blk[k * 2 + 1] << 8));
                    sum[0] += ((c >> 11) & 31) * 255.0 / 31.0;
                    sum[1] += ((c >> 5) & 63) * 255.0 / 63.0;
                    sum[2] += (c & 31) * 255.0 / 31.0;
                    cnt++;
                }
            }
        } else {
            const u32 rb = lv[l].tl.dst_row_bytes, rows = lv[l].tl.rows;
            u8* buf = (u8*)malloc((size_t)rb * rows);
            if (!buf) continue;
            rsx_texture_decode(buf, rb, src, lv[l].w, lv[l].h, &lv[l].tl, rsx_texture_argb_is_rgba());
            const u32 bpp = lv[l].tl.dst_row_bytes / (lv[l].w ? lv[l].w : 1u);
            if (bpp == 4) {
                for (u32 i = 0; i + 3 < rb * rows; i += 4) {
                    sum[0] += buf[i]; sum[1] += buf[i + 1]; sum[2] += buf[i + 2]; sum[3] += buf[i + 3]; cnt++;
                }
            } else {
                for (u32 i = 0; i < rb * rows; i++) { sum[0] += buf[i]; cnt++; }
            }
            free(buf);
        }
        if (!cnt) cnt = 1;
        fprintf(stderr, " L%u %ux%u=(%.0f,%.0f,%.0f,%.0f)", l, lv[l].w, lv[l].h,
                sum[0] / cnt, sum[1] / cnt, sum[2] / cnt, sum[3] / cnt);
    }
    fprintf(stderr, "\n");
}

/* A volume's levels decoded into their footprints. Each level of the guest's
 * image is its slices one after another, face_bytes apart (linear), or one
 * Morton block over x, y and z (swizzled); a footprint holds a slice every
 * RowPitch * Height bytes. */
static void tex_decode_volume(u8* mapped, const D3D12_PLACED_SUBRESOURCE_FOOTPRINT* lay,
                              const u8* src, const rsx_tex_level* lv, u32 nlev, u32 depth)
{
    static u8* lin;
    static size_t lin_cap;
    u32 d = depth;
    for (u32 l = 0; l < nlev; l++) {
        const rsx_tex_layout* tl = &lv[l].tl;
        const u32 rp = (u32)lay[l].Footprint.RowPitch;
        const u64 slice = (u64)rp * lay[l].Footprint.Height;
        const u32 nz = d < lay[l].Footprint.Depth ? d : lay[l].Footprint.Depth;
        for (u32 z = 0; z < nz; z++) {
            u8* dst = mapped + lay[l].Offset + z * slice;
            if (tl->swizzled) {
                const u32 bpp = tl->bytes_per_texel;
                const size_t need = (size_t)lv[l].w * lv[l].h * bpp;
                if (need > lin_cap) {
                    u8* p = (u8*)realloc(lin, need);
                    if (!p) return;
                    lin = p; lin_cap = need;
                }
                u32 lw = 0, lh = 0, ld = 0;
                while ((1u << lw) < lv[l].w) lw++;
                while ((1u << lh) < lv[l].h) lh++;
                while ((1u << ld) < d) ld++;
                for (u32 y = 0; y < lv[l].h; y++)
                    for (u32 x = 0; x < lv[l].w; x++)
                        memcpy(lin + ((size_t)y * lv[l].w + x) * bpp,
                               src + (size_t)rsx_swizzle_offset_3d(x, y, z, lw, lh, ld) * bpp, bpp);
                rsx_tex_layout ltl = *tl;
                ltl.swizzled = 0;
                rsx_texture_decode(dst, rp, lin, lv[l].w, lv[l].h, &ltl, rsx_texture_argb_is_rgba());
            } else {
                rsx_texture_decode(dst, rp, src + (size_t)z * tl->face_bytes, lv[l].w, lv[l].h, tl,
                                   rsx_texture_argb_is_rgba());
            }
        }
        src += (size_t)tl->face_bytes * d;
        if (d > 1) d >>= 1;
    }
}

static int vp_upload_tex_slot(u32 off, u32 w, u32 h, u32 fmt, int cube, u32 mips)
{
    /* cube is the texture's shape as the cache keys it (tex_shape_*): a 2D
     * image, a six-face cube, or a volume of some depth. */
    const int is_cube = (cube == 1);
    const u32 vdepth = tex_shape_depth(cube);
    const int vol = vdepth > 1;
    s_ts_out = TS_FAIL;
    /* TEX_BUDGET=<n>: cap texture uploads per frame. Off by default -- it was
     * tried against the TDR and does not help, because PERF shows the upload
     * path costing 0.01s for 775 calls. Kept because it is the right lever if a
     * title ever does hit an upload-bound frame. */
    { static int budget = -1;
      if (budget < 0) { const char* e = getenv("TEX_BUDGET"); budget = e ? atoi(e) : 0; }
      if (budget > 0) {
          static u64 last_frame = ~0ull; static int used = 0;
          if (last_frame != (u64)s_d3d.frame_count) { last_frame = (u64)s_d3d.frame_count; used = 0; }
          if (++used > budget) return -1;   /* try again next frame */
      } }
    extern uint8_t* vm_base;

    if (!off || !w || !h || !vm_base || !s_d3d.srv_heap) return -1;
    /* F9 (s_tex_drop): every held texture is read again at its next bind. Here, on the thread that
     * holds the slots. */
    if (s_tex_drop && InterlockedExchange(&s_tex_drop, 0)) {
        int n = 0;
        for (int i = 0; i < VP_TEX_SLOTS; i++)
            if (s_d3d.vp_tex[i].res) { s_d3d.vp_tex[i].again = 1; n++; }
        fprintf(stderr, "[tex] F9: the %d textures held are read again at their next bind%c", n, 10);
    }
    const u32 key_off = off;         /* lookup key: the offset as bound */
    /* Before any of the format's arithmetic: nearly all of a frame's calls end here. */
    int slot = vp_tex_find(key_off, w, h, fmt, cube);
    if (slot >= 0) {
        VPTexSlot* c = &s_d3d.vp_tex[slot];
        c->last_frame = s_d3d.frame_count;
        if (c->used) { s_ts_out = TS_HIT; return slot; }   /* already bound this frame */
        static int nocache = -1;                 /* TEX_NOCACHE=1: always re-upload */
        if (nocache < 0) { const char* e = getenv("TEX_NOCACHE"); nocache = e ? atoi(e) : 0; }
        /* Held from an earlier frame: re-upload only if the guest bytes
         * changed. Most of this scene's textures are static, and converting
         * every one of them every frame was ~70% of the frame's CPU time. */
        if (!nocache && !tex_changed(c, vm_base + c->off)) {
            c->used = 1;
            s_ts_out = TS_SAME;
            return slot;
        }
        /* stale: uploaded again below, in its own slot */
    }
    /* Format classes (base = fmt & 0x9F). The LBP loading screen uses:
     * 0x85 A8R8G8B8 (swizzled UI art), 0x8B G8B8 (the 1024x2048 linear FONT
     * atlas -- without it no text renders at all), 0x86/87/88 DXT1/23/45
     * (512x512 detail/LUT layers bound at t1/t3 on every draw). */
    /* Format class, row sizes and swizzling are RSX semantics: rsx_texture_
     * layout.c owns them so the Metal backend can read a guest texture without
     * re-deriving any of it. Only the DXGI mapping below is ours. */
    rsx_tex_layout tl;
    rsx_texture_layout(fmt, w, h, &tl);
    int argb = (tl.fmt == RSX_TEXFMT_R8G8B8A8);
    int dxt  = tl.compressed;
    /* Compressed rows are counted in blocks, so keep bpp at 1 for the byte
     * arithmetic the diagnostics below do. */
    u32 bpp = tl.compressed ? 1u : tl.bytes_per_texel;
    /* The DECODED image's format, which is not always the guest's: the packed
     * 16-bit colour formats arrive here unpacked to RGBA8, and DEPTH24_D8 as a
     * float. rsx_texture_layout.h says which class each format byte lands in. */
    DXGI_FORMAT dxfmt = vp_tex_dxgi(fmt);
    /* DXT data is stored as linear 4x4 block rows (compressed formats are
     * never Morton-swizzled on RSX). Row of blocks = (w/4)*blocksize. */
    u32 blkrow  = dxt ? tl.row_bytes : 0;
    u32 blkrows = dxt ? tl.rows      : 0;
    /* TEX_OFF_BIAS=1: sample the image one whole level-0 BELOW the bound offset.
     * Measured on the Rubber Ducky demo: at the bind offset every texture reads
     * as all-zero, while [off - w*h*bpp, off) holds the image (3800-4160 of 4298
     * sampled bytes non-zero, for four different textures at three sizes). So
     * the offset the guest programs points PAST level 0 rather than at it. */
    /* TEX_OFF_BIAS: 1 = one level-0 below the bound offset, 2 = one full MIP
     * CHAIN above it. The measured upload-to-bind deltas on the Rubber Ducky
     * demo are 0x2AAB00 for a 2MB level 0 and 0x555580 for a 4MB one -- both
     * exactly the mipmap pyramid total (L0 * 4/3), which is what the guest
     * reserves per texture. */
    { static int bias = -1;
      if (bias < 0) { const char* e = getenv("TEX_OFF_BIAS"); bias = e ? atoi(e) : 0; }
      if (bias == 1) { u32 sz = w * h * bpp; if (off > sz) off -= sz; }
      else if (bias >= 2) {
          u32 chain = 0;
          for (u32 mw = w, mh = h; mw && mh; mw >>= 1, mh >>= 1) {
              chain += mw * mh * bpp;
              if (mw == 1 && mh == 1) break;
          }
          /* bias 3: only shift when the bound offset really is empty, so a
           * texture that IS bound correctly keeps its own data. Applying the
           * chain unconditionally fixes the mis-bound majority but breaks any
           * correctly-bound minority, which shows up as objects going black. */
          int shift = 1;
          if (bias >= 3) {
              u32 nz = 0, sz = w * h * bpp;
              for (u32 i = 0; i < sz && i < 0x20000u; i += 97) if (vm_base[off + i]) nz++;
              shift = (nz == 0);
          }
          if (shift) off += chain;
      } }
    /* TEX_REMAP=1: if the bound offset holds nothing, look the image up by size
     * in the VRAM upload registry (cellGcmSys). For a title whose upload address
     * and SET_TEXTURE_OFFSET disagree this puts the real bytes under the sampler
     * without guessing a delta. */
    { static int remap = -1;
      if (remap < 0) { const char* e = getenv("TEX_REMAP"); remap = e ? atoi(e) : 0; }
      if (remap) {
        u32 nz = 0, sz = w * h * bpp;
        for (u32 i = 0; i < sz && i < 0x20000u; i += 97) if (vm_base[off + i]) nz++;
        if (!nz) {
            extern u32 rsx_find_vram_upload(u32);
            u32 alt = rsx_find_vram_upload(sz);
            if (alt) {
                static int _n = 0;
                if (PS3_LOG_FIRST(_n, 8)) fprintf(stderr, "[TEXREMAP] 0x%08X (%ux%u, %u bytes) -> 0x%08X%c",
                                      off, w, h, sz, alt, 10);
                off = alt;
            }
        } } }
    /* TEX_SRCDBG=<N>: is the guest memory this slot uploads FROM actually
     * populated? An empty source and a broken sampler both render flat. */
    { static int cap = -1, n = 0;
      if (cap < 0) { const char* e = getenv("TEX_SRCDBG"); cap = e ? atoi(e) : 0; }
      if (cap && n < cap) { n++;
        u32 nz = 0, tot = 0;
        for (u32 i = 0; i < w * h * bpp && i < 0x40000u; i += 61) { tot++; if (vm_base[off + i]) nz++; }
        /* Several of this title's textures are bound at EXACTLY the end address
         * of an upload, so also sample the block immediately BELOW the bind
         * offset: if that holds the image, the bind is one whole texture high. */
        u32 nzb = 0, totb = 0;
        { u32 sz = w * h * bpp; u32 base = (off > sz) ? off - sz : 0;
          for (u32 i = 0; i < sz && i < 0x40000u; i += 61) { totb++; if (vm_base[base + i]) nzb++; } }
        long nearest = 0; u32 found = 0;
        if (!nz) {   /* empty: where IS the data? scan out from the bind offset */
            for (long d = 0x1000; d <= 0x1000000 && !found; d += 0x1000) {
                for (int s = 0; s < 2 && !found; s++) {
                    long a = (long)off + (s ? -d : d);
                    if (a < 0x1000) continue;
                    u32 c = 0;
                    for (u32 i = 0; i < 0x1000u; i += 61) if (vm_base[a + i]) c++;
                    if (c > 8) { nearest = (s ? -d : d); found = c; }
                }
            }
        }
        fprintf(stderr, "[TEXSRC] off=0x%08X %ux%u fmt=0x%02X bpp=%u nonzero=%u/%u"
                        "%s%+ld (page nonzero=%u)\n",
                off, w, h, fmt, bpp, nz, tot,
                nz ? "" : "  nearest data at ", nz ? 0L : nearest, found);
        fprintf(stderr, "[TEXSRC]    block BELOW bind (off-%u): nonzero=%u/%u%c",
                w * h * bpp, nzb, totb, 10);
        /* Walk DOWN in 4KB steps to the first populated page whose predecessor
         * is empty: that is where this texture's data actually begins, and the
         * delta from the bound offset is the rule we are missing. */
        { u32 step = 0x1000, prev_pop = 0; long start_delta = 0;
          for (long d = 0; d >= -(long)(w * h * bpp) * 2; d -= step) {
              u32 a = (u32)((long)off + d); u32 pop = 0;
              if (a < 0x1000) break;
              for (u32 i = 0; i < step; i += 53) if (vm_base[a + i]) pop++;
              if (!pop && prev_pop) { start_delta = d + step; break; }
              prev_pop = pop;
          }
          fprintf(stderr, "[TEXSRC]    data starts at off%+ld (level0=%u bytes)%c",
                  start_delta, w * h * bpp, 10); } } }
    /* The texture's whole mip chain is uploaded: 60% of God of War: Ascension's
     * binds carry one, and minified without it its distant surfaces shimmered.
     * rsx_texture_mip_chain() has the RSX layout (each level packed after the
     * one above, swizzled levels swizzled within themselves); D3D12 says where
     * each face's levels go in the staging buffer (GetCopyableFootprints:
     * subresource = level + face * levels). RSX_TEX_MIPS=0 uploads level 0
     * only, as before. */
    static rsx_tex_level s_lv[RSX_MAX_TEXTURE_LEVELS];
    static D3D12_PLACED_SUBRESOURCE_FOOTPRINT s_lay[6 * RSX_MAX_TEXTURE_LEVELS];
    static int mips_on = -1;
    if (mips_on < 0) { const char* e = getenv("RSX_TEX_MIPS"); mips_on = e ? (atoi(e) != 0) : 1; }
    u32 nlev = mips_on ? rsx_texture_mip_chain(fmt, w, h, mips, 0, s_lv) : 1u;
    if (!nlev) nlev = 1;
    /* A LINEAR texture's levels keep level 0's row pitch: God of War:
     * Ascension's linear A8R8G8B8 chains read at their own width came out half
     * padding (each level's mean colour half of level 0's, TEX_MIPCHECK), and
     * every level after drifted. rsx_texture_mip_chain() packs them; DXT and
     * swizzled chains are packed, and read right. */
    if (nlev > 1 && !s_lv[0].tl.swizzled && !s_lv[0].tl.compressed) {
        const u32 pitch0 = s_lv[0].tl.row_bytes;
        u32 o = 0;
        for (u32 l = 0; l < nlev; l++) {
            rsx_texture_layout_pitched(fmt, s_lv[l].w, s_lv[l].h, pitch0, &s_lv[l].tl);
            s_lv[l].offset = o;
            o += s_lv[l].tl.face_bytes;
        }
    }
    const u32 nsub = (is_cube ? 6u : 1u) * nlev;   /* a volume: one per level */
    D3D12_RESOURCE_DESC fd = {0};
    fd.Dimension = vol ? D3D12_RESOURCE_DIMENSION_TEXTURE3D : D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    fd.Width = w; fd.Height = h;
    fd.DepthOrArraySize = vol ? (UINT16)vdepth : is_cube ? 6 : 1;   /* cube = 6 array slices */
    fd.MipLevels = (UINT16)nlev;
    fd.Format = dxfmt; fd.SampleDesc.Count = 1;
    fd.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    UINT64 lay_total = 0;
    s_d3d.device->lpVtbl->GetCopyableFootprints(s_d3d.device, &fd, 0, nsub, 0, s_lay, NULL, NULL, &lay_total);
    if (!lay_total) return -1;
    if (nlev > 1) tex_mipcheck(off, w, h, fmt, nlev, s_lv);
    /* A texture not held gets a slot from vp_tex_alloc; its size there is the
     * staged chain's, in the 64 KB units a committed resource takes. */
    const int ts_new = slot < 0;
    const u64 need = ((u64)lay_total + 0xFFFFu) & ~(u64)0xFFFFu;
    if (slot < 0) slot = vp_tex_alloc(need, w, h, fmt, cube, nlev);
    if (slot < 0) return -1;
    VPTexSlot* t = &s_d3d.vp_tex[slot];
    t->last_frame = s_d3d.frame_count;
    /* The upload buffer's rows hold the DECODED image, so they are sized from
     * dst_row_bytes -- w * bpp is the source row, which is narrower for any
     * format rsx_texture_decode unpacks. */
    u32 pitch = (tl.dst_row_bytes + 255) & ~255u;
    int fresh = 0;

    /* Also recreate when the CUBE-ness changes: a slot holding a 2D texture of
     * the same dimensions would otherwise be reused with 1 array slice while the
     * cube path copies 6 subresources and writes a 6x upload buffer -- which
     * crashes. */
    if (t->res && (t->w != w || t->h != h || t->fmt != fmt || t->cube != cube || t->levels != nlev))
        vp_tex_drop(t);
    if (!t->res) {
        s_ts_create++;
        const double cr0 = perf_now();
        D3D12_HEAP_PROPERTIES hp = {0}; hp.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC td = {0};
        td.Dimension = vol ? D3D12_RESOURCE_DIMENSION_TEXTURE3D : D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        td.Width = w; td.Height = h;
        td.DepthOrArraySize = vol ? (UINT16)vdepth : is_cube ? 6 : 1;   /* cube = 6 array slices */
        td.MipLevels = (UINT16)nlev;
        td.Format = dxfmt; td.SampleDesc.Count = 1;
        td.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        t->heap = 0;
        if (tex_placed_on()) t->res = tex_place(&td, &t->heap, &t->hoff, &t->hsize);
        if (t->res) {
            /* Its memory may have held another texture: this one's use of it
             * begins here. */
            D3D12_RESOURCE_BARRIER ab;
            memset(&ab, 0, sizeof ab);
            ab.Type = D3D12_RESOURCE_BARRIER_TYPE_ALIASING;
            ab.Aliasing.pResourceAfter = t->res;
            s_d3d.cmd_list->lpVtbl->ResourceBarrier(s_d3d.cmd_list, 1, &ab);
        } else if (FAILED(s_d3d.device->lpVtbl->CreateCommittedResource(
                s_d3d.device, &hp, D3D12_HEAP_FLAG_NONE, &td,
                D3D12_RESOURCE_STATE_COPY_DEST, NULL,
                &IID_ID3D12Resource, (void**)&t->res)))
            return -1;
        s_tex_create_s += perf_now() - cr0;
        t->bytes = (u32)need; s_tex_bytes += need;
        fresh = 1;
    }
    /* Stage the decoded image in the ring, or in the slot's own upload
     * buffer when the ring is full or could not be made. */
    pitch = (u32)s_lay[0].Footprint.RowPitch;   /* level 0: the same 256-aligned row */
    const u64 up_bytes = (u64)lay_total;
    const u32 fi = s_d3d.frame_index;
    if (!s_d3d.tex_ring) {
        D3D12_HEAP_PROPERTIES hu = {0}; hu.Type = D3D12_HEAP_TYPE_UPLOAD;
        D3D12_RESOURCE_DESC bd = {0};
        bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        bd.Width = (u64)TEX_RING_PART * FRAME_COUNT;
        bd.Height = 1; bd.DepthOrArraySize = 1;
        bd.MipLevels = 1; bd.SampleDesc.Count = 1; bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        void* rp = NULL; D3D12_RANGE nr0 = {0, 0};
        if (SUCCEEDED(s_d3d.device->lpVtbl->CreateCommittedResource(
                s_d3d.device, &hu, D3D12_HEAP_FLAG_NONE, &bd,
                D3D12_RESOURCE_STATE_GENERIC_READ, NULL,
                &IID_ID3D12Resource, (void**)&s_d3d.tex_ring))) {
            if (SUCCEEDED(s_d3d.tex_ring->lpVtbl->Map(s_d3d.tex_ring, 0, &nr0, &rp)) && rp)
                s_d3d.tex_ring_ptr = (u8*)rp;
            else { s_d3d.tex_ring->lpVtbl->Release(s_d3d.tex_ring); s_d3d.tex_ring = NULL; }
        }
        /* Each frame index's first spill chunk with the ring, at the first
         * texture (boot), not at the first burst that outgrows the ring. */
        for (u32 _i = 0; _i < FRAME_COUNT && s_d3d.tex_ring; _i++) {
            u64 _at = 0;
            TexSpill* _c = tex_spill_take(_i, 0, &_at);
            if (_c) _c->used = 0;
        }
    }
    ID3D12Resource* up_res = NULL;
    u64 up_base = 0;
    TexSpill* spill = NULL;
    const u64 ring_at = (s_d3d.tex_ring_used[fi] + 511u) & ~(u64)511u;   /* placement alignment */
    if (s_d3d.tex_ring_ptr && ring_at + up_bytes <= TEX_RING_PART) {
        up_res = s_d3d.tex_ring;
        up_base = (u64)fi * TEX_RING_PART + ring_at;
        s_d3d.tex_ring_used[fi] = ring_at + up_bytes;
    } else if ((spill = tex_spill_take(fi, up_bytes, &up_base)) != NULL) {
        up_res = spill->res;
    } else {
        if (!t->up) {
            const double ub0 = perf_now();
            D3D12_HEAP_PROPERTIES hu = {0}; hu.Type = D3D12_HEAP_TYPE_UPLOAD;
            D3D12_RESOURCE_DESC bd = {0};
            bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
            bd.Width = up_bytes;
            bd.Height = 1; bd.DepthOrArraySize = 1;
            bd.MipLevels = 1; bd.SampleDesc.Count = 1; bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
            if (FAILED(s_d3d.device->lpVtbl->CreateCommittedResource(
                    s_d3d.device, &hu, D3D12_HEAP_FLAG_NONE, &bd,
                    D3D12_RESOURCE_STATE_GENERIC_READ, NULL,
                    &IID_ID3D12Resource, (void**)&t->up)))
                return -1;
            s_hl_upbuf_n++; s_hl_upbuf_s += perf_now() - ub0;
        }
        up_res = t->up;
    }
    rdoc_name((ID3D12Object*)t->res, "gtex 0x%08X fmt %02X %ux%u%s", off, fmt, w, h,
              is_cube ? " cube" : vol ? " volume" : "");

    void* mapped = NULL; D3D12_RANGE nr = {0,0};
    if (up_res == s_d3d.tex_ring) mapped = s_d3d.tex_ring_ptr + up_base;
    else if (spill) mapped = spill->ptr + up_base;
    else if (FAILED(t->up->lpVtbl->Map(t->up, 0, &nr, &mapped)) || !mapped) return -1;
    { static int _tp=0; if (env_str("RTT_DUMP", &s_env_rtt_dump) && PS3_LOG_FIRST(_tp, 6)) {
        const u8* sp = vm_base + off;
        fprintf(stderr, "[TEXUP] off=0x%X fmt=0x%X row0:", off, fmt);
        for (int _b=0;_b<8;_b++) fprintf(stderr, " %02X", sp[_b]);
        fprintf(stderr, "  row240:");
        for (int _b=0;_b<8;_b++) fprintf(stderr, " %02X", sp[240*w*bpp+_b]);
        fprintf(stderr, "  mid:");
        for (int _b=0;_b<8;_b++) fprintf(stderr, " %02X", sp[240*w*bpp+320*bpp+_b]);
        fprintf(stderr, "%s", "\n");
    } }
    /* Swizzled (LN bit 0x20 clear): texels live in Morton/Z-order, not rows.
     * Uploading them as linear rows produced the diagonal-stripe garbage on
     * LBP's loading screen (every texture there is 0x85 swizzled). POT dims
     * only -- the hardware requires that for swizzled textures anyway. */
    int swz = tl.swizzled;
    /* Cube textures store their 6 faces consecutively. Convert each into its own
     * slice of the upload buffer; the conversion below is unchanged and simply
     * runs once per face with off/mapped rebased. */
    const u32 _face_rows  = dxt ? blkrows : h;
    /* The face stride is NOT one mip-0 image: RSX stores each cube face as its
     * own complete mip pyramid, so face f starts a whole pyramid (128-byte
     * aligned) in. Assuming mip-0-sized strides made face 1 land inside face 0's
     * mip chain -- which is exactly what the dumps showed, every face after the
     * first a progressively smaller copy of the first. */
    /* rsx_texture_cube_face_stride has the pyramid's size: here it was summed
     * with 8 bytes a block whatever the format (bpp is 1 for any compressed
     * one), so a DXT3/DXT5 cube read faces 1-5 from half their offsets -- God
     * of War: Ascension's menu rocks reflected noise once its cubes were
     * sampled as cubes. */
    u32 _face_bytes = dxt ? (blkrow * blkrows) : (w * h * bpp);
    if (is_cube) _face_bytes = rsx_texture_cube_face_stride(fmt, w, h, mips ? mips : 1u, 0);
    /* A volume is decoded below, slice by slice (tex_decode_volume). */
    const u32 _nfaces     = vol ? 0u : is_cube ? 6u : 1u;
    /* What the decode reads -- every level, face and slice -- and its checksum before it is read.
     * Taken after the read, it was the checksum of what a title had written meanwhile: God of War:
     * Ascension's streaming refills a block once the RSX has said it read it, sometimes while this
     * still reads it (issue #9). The slot held part of each texture, its checksum matched the new
     * bytes at every later bind, and the texture stayed broken. The levels below 0 were not in it. */
    u32 src_bytes = (dxt ? (blkrow * blkrows) : (w * h * bpp)) * vdepth;   /* level 0 */
    if (is_cube) {
        src_bytes = 6u * _face_bytes;   /* six whole pyramids */
    } else if (vol && nlev > 1) {
        src_bytes = 0;
        for (u32 l = 0, d = vdepth; l < nlev; l++, d = d > 1 ? d >> 1 : 1u)
            src_bytes += s_lv[l].tl.face_bytes * d;
    } else if (nlev > 1) {
        src_bytes = s_lv[nlev - 1].offset + s_lv[nlev - 1].tl.face_bytes;
    }
    u32 csum_pre[TEX_CSUM_PARTS];
    for (u32 i = 0; i < TEX_CSUM_PARTS; i++)
        csum_pre[i] = tex_csum_part(vm_base + off, src_bytes, i);
    const u32 _off0 = off; u8* const _map0 = (u8*)mapped;
    for (u32 _f = 0; _f < _nfaces; _f++) {
    off    = _off0 + _f * _face_bytes;
    if (is_cube && getenv("CUBEDBG")) { u32 _s=0; const u8* _p = vm_base + off;
        for (u32 _i = 0; _i < _face_bytes; _i += 97) _s = _s*131 + _p[_i];
        fprintf(stderr, "[CUBEFACE] f=%u off=0x%X bytes=%u bpp=%u swz=%d csum=%08X%c",
                _f, off, _face_bytes, bpp, swz, _s, 10); }
    mapped = _map0 + s_lay[_f * nlev].Offset;
    /* Pixel conversion -- deswizzle, channel order, block copy -- is RSX
     * semantics and lives in rsx_texture_layout.c, so a second backend gets
     * it by calling this rather than porting the loops again. */
    { const double dc0 = perf_now();
      rsx_texture_decode(mapped, pitch, vm_base + off, w, h, &tl,
                         rsx_texture_argb_is_rgba());
      const double ddc = perf_now() - dc0;
      s_tex_decode_s += ddc;
      tex_decode_note(&tl, w, h, ddc); }

    /* The diagnostics below only apply to the linear 8-bit case (Bink video
     * planes); they used to sit inside that branch of the conversion. */
    if (!tl.compressed && tl.bytes_per_texel == 1 && !tl.swizzled) {
        /* MOVIE_FIND=1: when a 640x360 movie plane uploads ZERO, scan a wide
         * guest window for the REAL frame (a 640-wide region with content) so
         * we learn where the video decode actually wrote vs where the texture
         * points -- ends the frame-buffer-address guessing. */
        if (getenv("MOVIE_FIND") && w == 640 && h == 360) {
            extern uint8_t* vm_base;
            const u8* here = vm_base + off;
            u32 hz = 0; for (u32 i=0;i<w*h;i+=137) if (here[i]) { hz=1; break; }
            static int _mf = 0;
            if (!hz && PS3_LOG_FIRST(_mf, 3)) {
                fprintf(stderr, "[movie-find] bound plane ea=0x%X is ZERO; full scan...\n", off);
                /* Scan FULL VRAM + main heap in 0x8000 steps for a 640x360
                 * content block -- NO early cap (the previous found<6 stopped in
                 * the low-VRAM display buffers before ever reaching the movie
                 * region ~0x40E80000). Report the near-plane hits explicitly so
                 * ring-desync (frame present at a DIFFERENT slot) is separable
                 * from decode-never-ran (region entirely zero). */
                struct { u32 lo, hi; const char* tag; } rng[] = {
                    {0x40000000u, 0x41000000u, "VRAM"}, {0x00100000u, 0x10000000u, "MAIN"},
                    {0x48000000u, 0x4A000000u, "BINK"} };  /* Bink working set (handle ~0x4849E760, SPU I/O 0x4847/0x4945) */
                u32 plane_lo = (off > 0x400000u) ? off - 0x400000u : 0;
                u32 plane_hi = off + 0x400000u;
                int found = 0, nearc = 0;
                for (int r = 0; r < (int)(sizeof(rng)/sizeof(rng[0])); r++) {
                    u32 best_a = 0, best_nz = 0; u32 best_span = 0;
                    for (u32 a = rng[r].lo; a + w*h < rng[r].hi; a += 0x8000) {
                        const u8* p = vm_base + a;
                        u32 nz = 0; u8 mn2 = 255, mx2 = 0;
                        for (u32 i = 0; i < w*h; i += 257) { u8 v = p[i]; if (v) nz++; if (v<mn2) mn2=v; if (v>mx2) mx2=v; }
                        u32 span = mx2 - mn2;
                        if (nz > best_nz) { best_nz = nz; best_a = a; best_span = span; }
                        if (nz > 400 && span > 60) {  /* looks like image content */
                            int isnear = (a >= plane_lo && a <= plane_hi);
                            if (isnear) nearc++;
                            if (found < 40)
                                fprintf(stderr, "[movie-find]   CONTENT @0x%08X (%s) nz=%u span=%u%s\n",
                                        a, rng[r].tag, nz, span, isnear ? " <== NEAR bound plane" : "");
                            found++;
                        }
                    }
                    /* Always report the densest block in this range even below the
                     * content threshold -- the MM intro fades in from black, so the
                     * real first frames are low-span and would otherwise be invisible. */
                    fprintf(stderr, "[movie-find]   %s best @0x%08X nz=%u span=%u%s\n",
                            rng[r].tag, best_a, best_nz, best_span,
                            (best_a >= plane_lo && best_a <= plane_hi) ? " <== NEAR bound plane" : "");
                    /* Dump the densest block as a 640x360 grayscale BMP so the
                     * "is this the decoded movie frame" question is answered by
                     * eye. One dump per range per process. */
                    if (best_nz > 400 && best_span > 40) {
                        static int _bd[4] = {0,0,0,0};
                        if (r < 4 && !_bd[r]) { _bd[r] = 1;
                            const u8* sp = vm_base + best_a;
                            char pn[128];
                            snprintf(pn, sizeof(pn), "binkscan_%s_%08X.bmp", rng[r].tag, best_a);
                            FILE* f = fopen(pn, "wb");
                            if (f) {
                                u32 rowb = w*3, rowp = (rowb+3)&~3u, fsz = 54 + rowp*h;
                                u8 hd[54] = {'B','M'};
                                hd[2]=(u8)fsz; hd[3]=(u8)(fsz>>8); hd[4]=(u8)(fsz>>16); hd[5]=(u8)(fsz>>24);
                                hd[10]=54; hd[14]=40;
                                hd[18]=(u8)w; hd[19]=(u8)(w>>8); hd[22]=(u8)h; hd[23]=(u8)(h>>8);
                                hd[26]=1; hd[28]=24;
                                fwrite(hd,1,54,f);
                                for (int y=(int)h-1; y>=0; y--) {
                                    const u8* srow = sp + (u64)y*w;
                                    for (u32 x=0;x<w;x++){ u8 px[3]; px[0]=px[1]=px[2]=srow[x]; fwrite(px,1,3,f); }
                                    { u8 z[3]={0,0,0}; fwrite(z,1,rowp-rowb,f); }
                                }
                                fclose(f);
                                fprintf(stderr, "[movie-find]   dumped %s\n", pn);
                            }
                        }
                    }
                }
                fprintf(stderr, "[movie-find]   total=%d near-plane=%d (window 0x%X..0x%X)\n",
                        found, nearc, plane_lo, plane_hi);
                if (!found) fprintf(stderr, "[movie-find]   no 640x360 content block (>thresh) found anywhere\n");
            }
        }
        /* TEX_SAVE=1: also dump wide B8 uploads (Bink video planes are B8 --
         * the Y plane IS the movie frame in grayscale) + a content stat, so
         * "is the decoder producing pixels" is answerable by looking at a BMP. */
        if (getenv("TEX_SAVE") && w >= 256) {
            static int _bs = 0;
            const u8* sp = vm_base + off;
            u32 nz = 0; u8 mn = 255, mx = 0;
            for (u32 i = 0; i < w * h; i += 17) {
                u8 v = sp[i]; if (v) nz++; if (v < mn) mn = v; if (v > mx) mx = v;
            }
            fprintf(stderr, "[TEXB8] off=0x%X %ux%u nz=%u/%u min=%u max=%u\n",
                    off, w, h, nz, (w * h) / 17, mn, mx);
            if (_bs < 8 && mx > mn) { _bs++;
                char pn[128];
                snprintf(pn, sizeof(pn), "texb8_%08X_%ux%u_%d.bmp", off, w, h, _bs);
                FILE* f = fopen(pn, "wb");
                if (f) {
                    u32 rowb = w * 3, rowp = (rowb + 3) & ~3u;
                    u32 fsz = 54 + rowp * h;
                    u8 hd[54] = {'B','M'};
                    hd[2]=(u8)fsz; hd[3]=(u8)(fsz>>8); hd[4]=(u8)(fsz>>16); hd[5]=(u8)(fsz>>24);
                    hd[10]=54; hd[14]=40;
                    hd[18]=(u8)w; hd[19]=(u8)(w>>8); hd[22]=(u8)h; hd[23]=(u8)(h>>8);
                    hd[26]=1; hd[28]=24;
                    fwrite(hd, 1, 54, f);
                    for (int y = (int)h - 1; y >= 0; y--) {
                        const u8* srow = sp + (u64)y * w;
                        for (u32 x = 0; x < w; x++) {
                            u8 px[3]; px[0]=px[1]=px[2]=srow[x];
                            fwrite(px, 1, 3, f);
                        }
                        { u8 z[3] = {0,0,0}; fwrite(z, 1, rowp - rowb, f); }
                    }
                    fclose(f);
                    fprintf(stderr, "[TEX_SAVE] %s\n", pn);
                }
            }
        }
    }
    /* TEX_SAVE=1: dump the first few converted ARGB uploads as BMPs (rgb +
     * alpha channel separately) -- ground-truth for "is the guest texture
     * wrong or is the sampling wrong" questions (wave's hue palette). */
    if (argb && _f + 1 == _nfaces && getenv("TEX_SAVE")) { static int _ts = 0; if (_ts < 8) { _ts++;
        /* For a cube, write every face: the point of the dump is to check the
         * assumed 6-face layout against the game's cubemap art. */
        for (u32 _face = 0; _face < _nfaces; _face++)
        for (int pass = 0; pass < 2; pass++) {
            char pn[128];
            if (_nfaces > 1)
                snprintf(pn, sizeof(pn), "cube_%08X_%ux%u_f%u_%s.bmp", _off0, w, h,
                         _face, pass ? "a" : "rgb");
            else
                snprintf(pn, sizeof(pn), "tex_%08X_%ux%u_%s.bmp", off, w, h,
                     pass ? "a" : "rgb");
            FILE* f = fopen(pn, "wb");
            if (f) {
                u32 rowb = w * 3, rowp = (rowb + 3) & ~3u;
                u32 fsz = 54 + rowp * h;
                u8 hd[54] = {'B','M'};
                hd[2]=(u8)fsz; hd[3]=(u8)(fsz>>8); hd[4]=(u8)(fsz>>16); hd[5]=(u8)(fsz>>24);
                hd[10]=54; hd[14]=40;
                hd[18]=(u8)w; hd[19]=(u8)(w>>8); hd[22]=(u8)h; hd[23]=(u8)(h>>8);
                hd[26]=1; hd[28]=24;
                fwrite(hd, 1, 54, f);
                for (int y = (int)h - 1; y >= 0; y--) {
                    const u8* srow = _map0 + s_lay[_face * nlev].Offset
                                     + (u64)y * pitch;
                    for (u32 x = 0; x < w; x++) {
                        u8 px[3];
                        if (pass) { px[0]=px[1]=px[2]=srow[x*4+3]; }
                        else { px[0]=srow[x*4+2]; px[1]=srow[x*4+1]; px[2]=srow[x*4+0]; }
                        fwrite(px, 1, 3, f);
                    }
                    { u8 z[3] = {0,0,0}; fwrite(z, 1, rowp - rowb, f); }
                }
                fclose(f);
                fprintf(stderr, "[TEX_SAVE] %s\n", pn);
            }
        }
    } }
    /* TEX_AVGDBG=1: one line per distinct bound texture offset with its average
     * source colour. duck.tga is overwhelmingly yellow (avg ~243,191,23), so this
     * says whether the duck's image is bound at all -- and if it is, the duck is
     * being drawn somewhere and the question is where, not whether. */
    { static int avgd = -1;
      if (avgd < 0) { const char* e = getenv("TEX_AVGDBG"); avgd = e ? atoi(e) : 0; }
      if (avgd && !dxt && bpp == 4) {
        static u32 seen[64]; static int nseen = 0;
        int known = 0;
        for (int i2 = 0; i2 < nseen; i2++) if (seen[i2] == off) { known = 1; break; }
        if (!known && nseen < 64) {
            seen[nseen++] = off;
            /* Report the source as R,G,B,A -- the layout TEX_RGBA uploads (PSGL
             * writes GL_RGBA/UNSIGNED_INT_8_8_8_8, so bytes land R,G,B,A). Alpha
             * matters as much as colour here: a blended surface whose alpha is a
             * flat 255 renders opaque no matter how correct the blend state is. */
            u64 sr = 0, sg = 0, sb = 0, sa = 0; u32 cnt = 0, amin = 255, amax = 0;
            for (u32 i2 = 0; i2 + 3 < w * h * 4u; i2 += 4 * 37) {
                const u8* q = vm_base + off + i2;
                sr += q[0]; sg += q[1]; sb += q[2]; sa += q[3]; cnt++;
                if (q[3] < amin) amin = q[3];
                if (q[3] > amax) amax = q[3];
            }
            if (cnt) fprintf(stderr, "[TEXAVG] raw=0x%08X %4ux%-4u fmt=0x%02X"
                                     " avgRGBA=(%3u,%3u,%3u,%3u) alpha[%u..%u]%c",
                             key_off, w, h, fmt, (u32)(sr/cnt), (u32)(sg/cnt),
                             (u32)(sb/cnt), (u32)(sa/cnt), amin, amax, 10);
        }
      } }
    /* Content-identify the duck's texture so filters can say "duck". */
    if (!dxt && bpp == 4 && w == 512 && h == 512 && s_duck_raw != key_off) {
        u64 ar = 0, ag = 0, ab = 0; u32 n = 0;
        for (u32 i2 = 0; i2 + 3 < w * h * 4u; i2 += 4 * 211) {
            const u8* q = vm_base + off + i2;
            ar += q[0]; ag += q[1]; ab += q[2]; n++;
        }
        if (n && ar/n > 200 && ag/n > 140 && ab/n < 90) {
            s_duck_off = key_off;
            { static u32 last = 0; if (last != key_off) { last = key_off;
                fprintf(stderr, "[DUCKTEX] resolved=0x%08X identified by content%c", key_off, 10); } }
        }
    }
    /* TEX_HILITE=1: paint the duck's texture pure red. duck.tga is overwhelmingly
     * yellow (avg 243,191,23); with the R,G,B,A source order that is bytes
     * s[0] high, s[1] mid, s[2] low. Flooding it with an unmistakable colour
     * answers "where on screen is the duck drawn" directly, which neither the
     * vertex data nor a pixel count can. Diagnostic only. */
    { static int hil = -1;
      if (hil < 0) { const char* e = getenv("TEX_HILITE"); hil = e ? atoi(e) : 0; }
      if (hil && !dxt && bpp == 4 && w == 512 && h == 512) {
        u64 sr = 0, sg = 0, sb = 0; u32 cnt = 0;
        for (u32 i2 = 0; i2 + 3 < w * h * 4u; i2 += 4 * 37) {
            const u8* q = vm_base + off + i2;
            sr += q[0]; sg += q[1]; sb += q[2]; cnt++;
        }
        if (cnt) {
            u32 ar = (u32)(sr/cnt), ag = (u32)(sg/cnt), ab = (u32)(sb/cnt);
            if (ar > 200 && ag > 140 && ab < 90) {
                for (u32 y = 0; y < h; y++) {
                    u8* drow = (u8*)mapped + (u64)y * pitch;
                    for (u32 x = 0; x < w; x++) {
                        drow[x*4+0] = 0xFF; drow[x*4+1] = 0x00;
                        drow[x*4+2] = 0x00; drow[x*4+3] = 0xFF;
                    }
                }
                { static int _n = 0; if (PS3_LOG_FIRST(_n, 4))
                    fprintf(stderr, "[TEXHILITE] duck texture off=0x%08X avg=(%u,%u,%u) -> red%c",
                            off, ar, ag, ab, 10); }
            }
        }
      } }
    /* TEX_MARKEMPTY=1: paint any texture whose guest source is entirely zero a
     * flat magenta instead of uploading the empty bytes. An unresolved texture
     * and a legitimately black surface are indistinguishable on screen, so this
     * says WHICH geometry is missing its image -- diagnostic only. */
    { static int mark = -1;
      if (mark < 0) { const char* e = getenv("TEX_MARKEMPTY"); mark = e ? atoi(e) : 0; }
      if (mark && !dxt) {
        u32 nz = 0, sz = w * h * bpp;
        for (u32 i2 = 0; i2 < sz && i2 < 0x40000u; i2 += 61) if (vm_base[off + i2]) nz++;
        if (!nz) {
            for (u32 y = 0; y < h; y++) {
                u8* drow = (u8*)mapped + (u64)y * pitch;
                for (u32 x = 0; x < w; x++) {
                    /* Pure RED. Magenta collided with this title's wall tint,
                     * so the marker was indistinguishable from real geometry. */
                    drow[x*4+0] = 0xFF; drow[x*4+1] = 0x00;
                    drow[x*4+2] = 0x00; drow[x*4+3] = 0xFF;
                }
            }
            { static int _n = 0; if (PS3_LOG_FIRST(_n, 8))
                fprintf(stderr, "[TEXEMPTY] off=0x%08X %ux%u fmt=0x%02X -> magenta%c",
                        off, w, h, fmt, 10); }
        }
      } }
    }   /* end per-face conversion */
    off = _off0; mapped = _map0;
    if (vol) {
        const double dv0 = perf_now();
        tex_decode_volume((u8*)mapped, s_lay, vm_base + off, s_lv, nlev, vdepth);
        s_tex_decode_s += perf_now() - dv0;
    }
    /* The levels below 0, each face's at its footprint. */
    const double dcl0 = perf_now();
    for (u32 _f = 0; _f < _nfaces && nlev > 1; _f++)
        for (u32 _l = 1; _l < nlev; _l++) {
            const D3D12_PLACED_SUBRESOURCE_FOOTPRINT* fp = &s_lay[_f * nlev + _l];
            rsx_texture_decode((u8*)mapped + fp->Offset, (u32)fp->Footprint.RowPitch,
                               vm_base + _off0 + _f * _face_bytes + s_lv[_l].offset,
                               s_lv[_l].w, s_lv[_l].h, &s_lv[_l].tl, rsx_texture_argb_is_rgba());
        }
    s_tex_decode_s += perf_now() - dcl0;
    if (up_res != s_d3d.tex_ring && !spill) t->up->lpVtbl->Unmap(t->up, 0, NULL);

    if (!fresh) {   /* reused resource: PSR -> COPY_DEST first */
        D3D12_RESOURCE_BARRIER b = {0};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition.pResource = t->res;
        b.Transition.StateBefore = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        b.Transition.StateAfter  = D3D12_RESOURCE_STATE_COPY_DEST;
        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        s_d3d.cmd_list->lpVtbl->ResourceBarrier(s_d3d.cmd_list, 1, &b);
    }
    D3D12_TEXTURE_COPY_LOCATION dst = {0}, src = {0};
    dst.pResource = t->res; dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    src.pResource = up_res; src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    for (u32 _s = 0; _s < nsub; _s++) {   /* level + face * levels */
        dst.SubresourceIndex = _s;
        src.PlacedFootprint = s_lay[_s];
        src.PlacedFootprint.Offset += up_base;
        s_d3d.cmd_list->lpVtbl->CopyTextureRegion(s_d3d.cmd_list, &dst, 0, 0, 0, &src, NULL);
    }
    {
        D3D12_RESOURCE_BARRIER b = {0};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition.pResource = t->res;
        b.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
        b.Transition.StateAfter  = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        s_d3d.cmd_list->lpVtbl->ResourceBarrier(s_d3d.cmd_list, 1, &b);
    }

    t->off = off; t->key = key_off; t->w = w; t->h = h; t->fmt = fmt;
    t->cube = cube; t->used = 1; t->levels = nlev;
    if (!t->linked) vp_tex_link(slot);
    if (t->up && s_tex_staging_n < (int)(sizeof s_tex_staging / sizeof s_tex_staging[0])) {
        s_tex_staging[s_tex_staging_n++] = t->up;   /* the copy above reads it */
        t->up = NULL;
    }
    /* The checksum of before the read is kept. If the bytes changed while they were read, the next
     * bind's check would see it only in the part it compares; `again` uploads them then anyway. */
    t->csum_bytes = src_bytes;
    t->again = 0;
    for (u32 i = 0; i < TEX_CSUM_PARTS; i++) {
        t->csum[i] = csum_pre[i];
        if (tex_csum_part(vm_base + off, src_bytes, i) != csum_pre[i]) t->again = 1;
    }
    if (t->again) tex_torn_note(off, w, h, fmt);
    s_ts_out = ts_new ? TS_NEW : TS_CHANGED;
    return slot;
}

/* ---------------------------------------------------------------------------
 * Render-to-texture: offscreen RT pool.
 *
 * Draws/clears targeting a non-display surface (SET_SURFACE_COLOR_xOFFSET
 * not registered via cellGcmSetDisplayBuffer) render into a pooled RGBA8
 * texture keyed by the surface's resolved vm offset; a later draw binding a
 * texture at that offset samples the RT directly instead of guest memory.
 * RGBA8 for every RT (even half-float guest surfaces) keeps the existing
 * PSO/RTV formats -- values clamp to [0,1], which demosaic's RGB data fits.
 * -----------------------------------------------------------------------*/

/* Snapshot the VP constant bank + viewport epilogue for one draw into its
 * vp_cb slot. Runs at record time so every draw keeps the constants that
 * were live when the guest issued it. */
/* Draw a raw-fetch record: vp_vb as raw bytes once per pass, then each
 * segment with its header offset as the b2 root constant. The indices are the
 * guest's own, so no base vertex. */
static void vp_draw_raw(const D3D12DrawRecord* dr, int* ib_bound, int* raw_bound)
{
    ID3D12GraphicsCommandList* cl = s_d3d.cmd_list;
    if (!*ib_bound) {
        D3D12_INDEX_BUFFER_VIEW ibv;
        ibv.BufferLocation = s_d3d.vp_ib->lpVtbl->GetGPUVirtualAddress(s_d3d.vp_ib)
                           + (u64)s_d3d.vp_parity * VP_MAX_INDICES * 4;
        ibv.SizeInBytes = (2u - s_d3d.vp_parity) * VP_MAX_INDICES * 4 + s_res_ib_size * 4;   /* and the resident part */
        ibv.Format = DXGI_FORMAT_R32_UINT;
        cl->lpVtbl->IASetIndexBuffer(cl, &ibv);
        *ib_bound = 1;
    }
    /* *raw_bound: 1 vp_vb is bound as the raw bytes (t8), 2 vp_lit (the vertex pass's copies) */
    for (u32 s = 0; s < dr->seg_n; s++) {
        const VPRawSeg* sg = &s_seg[dr->seg_first + s];
        if (!sg->count) continue;
        const int want = sg->lit ? 2 : 1;
        if (*raw_bound != want) {
            cl->lpVtbl->SetGraphicsRootShaderResourceView(cl, 3, want == 2
                ? s_vpass.lit->lpVtbl->GetGPUVirtualAddress(s_vpass.lit) + (u64)s_d3d.vp_parity * s_vpass.lit_half
                : s_d3d.vp_vb->lpVtbl->GetGPUVirtualAddress(s_d3d.vp_vb) + (u64)s_d3d.vp_parity * MAX_VERTICES * 256);
            *raw_bound = want;
        }
        cl->lpVtbl->SetGraphicsRoot32BitConstant(cl, 4, sg->lit ? sg->lhdr : sg->hdr, 0);
        cl->lpVtbl->DrawIndexedInstanced(cl, sg->count, 1, sg->ib_first, 0, 0);
    }
}

/* A draw's vp_cb slot (the vertex constants, the viewport epilogue, the raw fetch's attribute
 * table) is made first in s_cbimg; vp_cb_commit writes it to the draw's slot, or gives the draw the
 * slot the batch's last written one took when the two are the same. A draw used to write its whole
 * constant bank, 8 KB of upload heap: ~20 MB a frame at God of War: Ascension's gate, where runs
 * of draws (EDGE's jobs of one object, its pieces' records) share their constants. VP_CB_SHARE=0
 * writes every slot. Only the recording thread.
 *
 * Five records in six have the bank of the one before. rsx_state's generation says so without
 * touching it: s_cbimg's constants are copied only when it has moved, and compared with the last
 * written ones only when the two generations differ (a bank rewritten with other words). Each was
 * 8 KB a record at 34,000 records a second. VP_CB_GEN=0 copies and compares every time. */
#define VP_CB_IMG       VP_CB_RAW_OFF          /* the constants and the epilogue */
#define VP_CB_RAW_BYTES 512u                   /* vp_fa[16], vp_fk[16] */
static u8 s_cbimg[VP_CB_STRIDE], s_cblast[VP_CB_STRIDE];
static int s_cbimg_ok, s_cbimg_raw, s_cblast_raw;
static u32 s_cblast_slot, s_cblast_gen = ~0u, s_cb_gen;
static u32 s_cbimg_bank, s_cblast_bank;     /* the bank's generation in each, 0 for none */
static unsigned long long s_cb_written, s_cb_shared, s_cb_same_bank;

/* The raw-fetch attribute table for this draw's slot: the shader's vp_fa/vp_fk
 * (see s_vp_raw_hlsl), from the state the draw was recorded with. */
static void vp_record_raw_cb(u32 slot, const rsx_state* st)
{
    if (!s_d3d.vp_cb_mapped || !st || slot >= MAX_DRAWS) return;
    u32 fa[64];
    float fk[64];
    for (int i = 0; i < 16; i++) {
        const rsx_vertex_attrib* a = &st->vertex_attribs[i];
        const u32 on = (a->enabled && a->stride) ? 1u : 0u;
        u32 c = a->size ? a->size : 4; if (c > 4) c = 4;
        fa[i * 4 + 0] = 0;
        fa[i * 4 + 1] = on ? a->stride : 0;
        fa[i * 4 + 2] = (a->type & 15u) | (c << 4);
        fa[i * 4 + 3] = on;
        /* What an array leaves out is 0, 0, 0, 1 as the input assembler's is, not the attribute's
         * constant: God of War: Ascension's last positions have three components, and their w
         * came out 0 here, which sent its motion blur's vertexes off the screen. */
        if (on) { fk[i * 4] = fk[i * 4 + 1] = fk[i * 4 + 2] = 0.0f; fk[i * 4 + 3] = 1.0f; }
        else memcpy(&fk[i * 4], st->vertex_data4f[i], 16);
    }
    if (!s_cbimg_ok) return;                     /* written to the slot by vp_cb_commit */
    u8* dst = s_cbimg + VP_CB_RAW_OFF;
    memcpy(dst, fa, sizeof fa);
    memcpy(dst + sizeof fa, fk, sizeof fk);
    s_cbimg_raw = 1;
}

/* b1 slots taken from the half of vp_fpcb this frame's records use. */
static u32 s_fpcb_used;

/* The fragment programs as they were when their draws were recorded. The
 * pipeline is built at the batch's end (vp_get_fp_pso), and by then a title
 * may have written its next programs over them, once the walk has passed the
 * label that frees the memory: God of War: Ascension with its MLAA off does,
 * in 0.2% of the lookups (bursts of ~2,000 in a second). Built from those
 * bytes a pipeline was a mix of two programs -- a wrong draw, and a shader
 * never seen before every time (300-400 compiled a run, never settling). So a
 * draw keeps its program's bytes: copied once a batch for each program
 * address, again only if they changed, into a half of s_fpsnap taken by
 * vp_parity as b1's slots are. FP_SNAPSHOT=0 reads them at the build, as
 * before. */
#define FP_SNAP_HALF  (4u << 20)
#define FP_SNAP_SLOTS 1024u
static u8* s_fpsnap;
static u32 s_fpsnap_used, s_fpsnap_gen = 1;
static struct { u32 addr, off, len, gen; u64 shash; u32 shash_nk, units, koff; int nconsts; }
    s_fpsnap_idx[FP_SNAP_SLOTS];

static void fp_snapshot_fill(D3D12DrawRecord* dr, const u8* half, u32 slot)
{
    dr->fp_snap = half + s_fpsnap_idx[slot].off;
    dr->fp_snap_len = s_fpsnap_idx[slot].len;
    dr->fp_shash = s_fpsnap_idx[slot].shash;
    dr->fp_shash_nk = s_fpsnap_idx[slot].shash_nk;
    dr->fp_units = s_fpsnap_idx[slot].units;
    dr->fp_consts = half + s_fpsnap_idx[slot].koff;
    dr->fp_nconsts = s_fpsnap_idx[slot].nconsts;
}

static void fp_snapshot(D3D12DrawRecord* dr, const u8* prog)
{
    static int on = -1;
    dr->fp_snap = NULL;
    dr->fp_snap_len = 0;
    if (on < 0) { const char* e = getenv("FP_SNAPSHOT"); on = !(e && e[0] == '0'); }
    if (!on) return;
    if (!s_fpsnap && !(s_fpsnap = (u8*)malloc(2u * (size_t)FP_SNAP_HALF))) { on = 0; return; }
    u8* half = s_fpsnap + (size_t)s_d3d.vp_parity * FP_SNAP_HALF;
    const u32 slot = (dr->fp_addr * 2654435761u) >> 22;            /* FP_SNAP_SLOTS */
    if (s_fpsnap_idx[slot].gen == s_fpsnap_gen && s_fpsnap_idx[slot].addr == dr->fp_addr &&
        !memcmp(half + s_fpsnap_idx[slot].off, prog, s_fpsnap_idx[slot].len)) {
        fp_snapshot_fill(dr, half, slot);
        return;
    }
    const u32 len = rsx_fp_program_size(prog, FP_MAX_BYTES);
    if (!len) return;                     /* no end: read at the build, as before */
    /* The program, then its constants: what each draw of it used to work out
     * twice or three times (the structural hash at both lookups, the texture
     * units at the prepass, the constants at the record), once a batch. */
    static rsx_fp_constant_block kb;      /* only the recording thread */
    int nconsts = rsx_fp_collect_constants(prog, len, &kb);
    if (nconsts < 0) nconsts = 0;
    if (nconsts > (int)FP_CB_MAX_K) nconsts = (int)FP_CB_MAX_K;
    const u32 klen = (u32)nconsts * 16u;
    if (s_fpsnap_used + ((len + 15u) & ~15u) + klen > FP_SNAP_HALF) {
        static int _w = 0;
        if (PS3_LOG_FIRST(_w, 4))
            fprintf(stderr, "[D3D12] fragment program snapshots full this batch (%u bytes); the rest read at the build%c",
                    s_fpsnap_used, 10);
        return;
    }
    memcpy(half + s_fpsnap_used, prog, len);
    const u32 koff = s_fpsnap_used + ((len + 15u) & ~15u);
    if (klen) memcpy(half + koff, kb.values, klen);
    u32 shash_nk = 0;
    s_fpsnap_idx[slot].addr = dr->fp_addr;
    s_fpsnap_idx[slot].off = s_fpsnap_used;
    s_fpsnap_idx[slot].len = len;
    s_fpsnap_idx[slot].gen = s_fpsnap_gen;
    s_fpsnap_idx[slot].shash = rsx_fp_structural_hash_n(half + s_fpsnap_used, len, 1469598103934665603ull,
                                                        &shash_nk);
    s_fpsnap_idx[slot].shash_nk = shash_nk;
    s_fpsnap_idx[slot].units = rsx_fp_texture_units(half + s_fpsnap_used, len);
    s_fpsnap_idx[slot].koff = koff;
    s_fpsnap_idx[slot].nconsts = nconsts;
    s_fpsnap_used = koff + klen;
    fp_snapshot_fill(dr, half, slot);
}

static void vp_record_cb(u32 slot, int vs_idx, D3D12DrawRecord* dr)
{
    const rsx_state* st = s_d3d.current_rsx_state;
    s_cbimg_ok = 0;
    s_cbimg_raw = 0;
    if (!s_d3d.vp_cb_mapped || !st || slot >= MAX_DRAWS) return;
    /* b1 as of this draw: fp_alpha, and the program's inline constants as
     * the guest has patched them for it, in a slot as long as they need.
     * fp_tcscale[] waits for the replay, which knows the target each unit
     * reads (dr_coord_scales). */
    if (s_d3d.vp_fpcb_mapped && dr) {
        extern uint8_t* vm_base;
        extern u32 cellGcmResolveLocated(int, u32);
        static rsx_fp_constant_block kb;   /* only the recording thread */
        int nk = 0;
        const void* kv = kb.values;
        if (dr->fp_addr && vm_base) {
            const u32 foff = cellGcmResolveLocated((dr->fp_addr & 0x3u) == 1, dr->fp_addr & ~0x3u);
            if (foff != 0xFFFFFFFFu) {
                fp_snapshot(dr, vm_base + foff);
                if (dr->fp_snap) { nk = dr->fp_nconsts; kv = dr->fp_consts; }
                else nk = rsx_fp_collect_constants(vm_base + foff, FP_MAX_BYTES, &kb);
            }
            if (nk < 0) nk = 0;
            if (nk > (int)FP_CB_MAX_K) nk = (int)FP_CB_MAX_K;
        }
        const u32 bytes = ((FPCB_CONSTS + (nk ? (u32)nk : 1u)) * 16u + 255u) & ~255u;
        if (s_fpcb_used + bytes > FPCB_HALF) {
            static int _f = 0;
            if (PS3_LOG_FIRST(_f, 4))
                fprintf(stderr, "[D3D12] b1 half full (%u bytes this frame); the draw shares slot 0%c",
                        s_fpcb_used, 10);
            dr->fpcb_off = 0;
        } else {
            dr->fpcb_off = s_fpcb_used;
            s_fpcb_used += bytes;
        }
        float* ts = (float*)((char*)s_d3d.vp_fpcb_mapped + (u64)s_d3d.vp_parity * FPCB_HALF + dr->fpcb_off);
        if (nk > 0) memcpy(ts + FPCB_CONSTS * 4, kv, (size_t)nk * 16u);
        /* fp_alpha: x the alpha test's reference, y the WPOS window height
         * when its origin is the bottom edge (0 otherwise), z the pixel-centre
         * bias (-0.5 for integer centres): what the buffered decompile
         * compiles the alpha test and _rsx_wpos against. */
        { float* fa = ts + FPCB_ALPHA * 4;
          const u32 wv = dr ? dr->wpos_win : 0u;
          fa[0] = rsx_fp_alpha_ref(st->alpha_ref, st->surface_format & 0x1Fu);
          fa[1] = ((wv >> 12) & 0xFu) ? (float)(wv & 0xFFFu) : 0.0f;
          fa[2] = ((wv >> 16) & 0xFu) ? -0.5f : 0.0f;
          fa[3] = 0.0f; }
        if (dr->fp_addr) {
            /* FPK_PICK=<hex fp>[:<every>]: that program's inline constants as its
             * draws get them, every <every>th draw (30), 200 times. A diagnostic. */
            { static int init = 0; static u32 want = 0, every = 30, seen = 0, shown = 0;
              if (!init) { init = 1; const char* e = getenv("FPK_PICK");
                  if (e) { char* end = NULL; want = (u32)strtoul(e, &end, 16);
                           if (end && *end == ':') every = (u32)strtoul(end + 1, NULL, 10); if (!every) every = 1; } }
              if (want && dr->fp_addr == want && shown < 200 && (seen++ % every) == 0) {
                  shown++;
                  extern unsigned cellGcm_flip_request_count(void);
                  fprintf(stderr, "[FPKPICK] flip=%u nk=%u", cellGcm_flip_request_count(), nk);
                  for (u32 i = 0; i < nk && i < 32; i++)
                      fprintf(stderr, " c%u=(%g %g %g %g)", i, ts[(FPCB_CONSTS + i) * 4 + 0], ts[(FPCB_CONSTS + i) * 4 + 1],
                              ts[(FPCB_CONSTS + i) * 4 + 2], ts[(FPCB_CONSTS + i) * 4 + 3]);
                  fprintf(stderr, "%c", 10);
              } }
            /* FPK_DBG=1: the fragment constants as the shader will see them.
             * A program whose only output is fp_k[0].xxxx renders black if
             * those constants never arrive, which looks identical from the
             * outside to geometry that never rasterised. */
            { static int fk = -1; if (fk < 0) fk = getenv("FPK_DBG") ? 1 : 0;
              static int n = 0;
              if (fk && n < 12 && dr->fp_addr != 0x00EC2B02u) { n++;
                  fprintf(stderr, "[FPK] tex0=0x%08X fp=0x%08X k0=(%g %g %g %g)"
                                  " k1=(%g %g %g %g)%c",
                          dr->tex[0].raw, dr->fp_addr,
                          ts[FPCB_CONSTS * 4 + 0], ts[FPCB_CONSTS * 4 + 1], ts[FPCB_CONSTS * 4 + 2],
                          ts[FPCB_CONSTS * 4 + 3], ts[FPCB_CONSTS * 4 + 4], ts[FPCB_CONSTS * 4 + 5],
                          ts[FPCB_CONSTS * 4 + 6], ts[FPCB_CONSTS * 4 + 7], 10);
                  const u32 raw = dr->fp_addr & ~0x3u;
                  const u32 ea_main  = cellGcmResolveLocated(0, raw);
                  const u32 ea_local = cellGcmResolveLocated(1, raw);
                  fprintf(stderr, "[FPK]   ucode main@0x%08X:", ea_main);
                  for (int b = 0; b < 48; b++) fprintf(stderr, "%s%02X", (b%16)?" ":"  |", vm_base[ea_main + b]);
                  fputc(10, stderr); }
              /* distinct fragment programs seen -- if every draw shares one,
               * SET_SHADER_PROGRAM is not being tracked per draw. */
              if (fk) { static u32 seen[16]; static int ns = 0; int f = 0;
                  for (int q = 0; q < ns; q++) if (seen[q] == dr->fp_addr) f = 1;
                  if (!f && ns < 16) { seen[ns++] = dr->fp_addr;
                      fprintf(stderr, "[FPK] distinct fp #%d = 0x%08X%c", ns, dr->fp_addr, 10); } } }
        }
    }
    char* dst = (char*)s_cbimg;             /* written to the slot by vp_cb_commit */
    { static int gen = -1;
      if (gen < 0) { const char* e = getenv("VP_CB_GEN"); gen = !(e && e[0] == '0'); }
      if (!gen || !s_cbimg_bank || s_cbimg_bank != st->vertex_constants_gen) {
          memcpy(dst, st->vertex_constants, RSX_MAX_VERTEX_CONSTANTS * 16);
          s_cbimg_bank = gen ? st->vertex_constants_gen : 0;
      } }
    /* VP_MVP=<N>: the constant bank as the shader will see it, for the first N
     * draws. A snapshot taken from a stale rsx_state looks exactly like a broken
     * vertex program from the outside -- both give zero fragments. */
    { static int cap = -1, seen = 0;
      if (cap < 0) { const char* e = getenv("VP_MVP"); cap = e ? atoi(e) : 0; }
      if (cap && seen < cap) { seen++;
        const float* c = (const float*)dst;
        int last_nz = -1;
        for (int i = 0; i < RSX_MAX_VERTEX_CONSTANTS * 4; i++)
            if (c[i] > 1e-9f || c[i] < -1e-9f) last_nz = i / 4;
        fprintf(stderr, "[VPMVP] slot=%u lastNZ=c%d  c260=(%g %g %g %g) c467=(%g %g %g %g)\n",
                slot, last_nz,
                c[260*4+0], c[260*4+1], c[260*4+2], c[260*4+3],
                c[467*4+0], c[467*4+1], c[467*4+2], c[467*4+3]); } }
    /* RSX_DBG_CB=<hex tex0 offset>: the constants as they land in the per-draw
     * constant buffer for that texture's draws -- i.e. exactly what the shader
     * samples, not what the CPU-side rsx_state holds. Those two agreeing is an
     * assumption worth checking directly when a draw transforms to nothing. */
    const float* vpx_dbg = (const float*)(dst + RSX_MAX_VERTEX_CONSTANTS * 16);
    { static const char* dc = (const char*)1; static u32 wantc = 0; static int n = 0;
      if (dc == (const char*)1) { dc = getenv("RSX_DBG_CB");
                                  wantc = dc ? (u32)strtoul(dc, NULL, 16) : 0; }
      if (wantc && dr && dr->tex[0].raw == wantc && n < 4) { n++;
          const float* c = (const float*)dst;
          fprintf(stderr, "[DUCKCB] slot=%u vs_idx=%d posscale=(%g %g %g) posoffset=(%g %g %g)%c",
                  slot, vs_idx, vpx_dbg[0], vpx_dbg[1], vpx_dbg[2],
                  vpx_dbg[4], vpx_dbg[5], vpx_dbg[6], 10);
          for (int r = 256; r <= 259; r++)
              fprintf(stderr, "[DUCKCB]   c%d=(%g %g %g %g)%c", r,
                      c[r*4+0], c[r*4+1], c[r*4+2], c[r*4+3], 10);
      } }

    /* Viewport epilogue (see the render_frame notes this logic came from):
     * x/y identity, z lane remaps GL clip z when the guest programs one. */
    float* vpx = (float*)(dst + RSX_MAX_VERTEX_CONSTANTS * 16);
    const float* vs_ = st->viewport_scale;
    const float* vo_ = st->viewport_offset;
    vpx[0] = vpx[1] = vpx[3] = 1.0f;
    vpx[4] = vpx[5] = vpx[7] = 0.0f;
    if (vs_[2] != 0.0f) { vpx[2] = vs_[2]; vpx[6] = vo_[2]; }
    else                { vpx[2] = 1.0f;   vpx[6] = 0.0f;   }
    /* DBG_ZOOM=<k> with DBG_CENTER="cx,cy": a debug camera applied in CLIP space,
     * not a framebuffer crop -- the geometry is re-rasterized at full resolution.
     * The shader epilogue already computes
     *     pos.xyz = _p.xyz * vp_posscale + _p.w * vp_posoffset
     * so recentring NDC point c and magnifying by k is exactly
     *     pos.xy = _p.xy * k + _p.w * (-c * k).
     * Rubber Ducky frames its ducks near the bottom edge of the viewport and its
     * camera only moves from the analog sticks, so without this there is no way
     * to look at the subject of the demo. It changes the view, nothing else:
     * same draws, same transform, same textures. */
    { static int z = -1; static float k = 1.0f, cx = 0.0f, cy = 0.0f;
      if (z < 0) { const char* e = getenv("DBG_ZOOM");
                   k = e ? (float)atof(e) : 1.0f;
                   z = (e && k > 0.0f) ? 1 : 0;
                   const char* ce = getenv("DBG_CENTER");
                   if (ce) { double a = 0, b = 0; sscanf(ce, "%lf,%lf", &a, &b);
                             cx = (float)a; cy = (float)b; } }
      if (z) { float ux = cx, uy = cy;
               static int lock = -1;
               if (lock < 0) { const char* le = getenv("DBG_LOCK"); lock = le ? atoi(le) : 0; }
               if (lock && s_lock_valid) { ux = s_lock_x; uy = s_lock_y; }
               vpx[0] = k; vpx[1] = k; vpx[4] = -ux * k; vpx[5] = -uy * k; } }

    /* Garbage-projection fallback (vkcube; see the original comment). */
    int uses_c03 = (vs_idx >= 0 && vs_idx < s_d3d.vp_vs_n)
                       ? s_d3d.vp_vs[vs_idx].uses_c03 : s_d3d.vp_uses_c03;
    float* c = (float*)dst;
    float c00 = c[0];
    int garbage = !(c00 > 0.0f && c00 < 8.0f);
    if (env_on("VP_NOFIXPROJ", &s_env_vp_nofixproj)) garbage = 0;
    if ((garbage && uses_c03) || env_on("VP_FIXPROJ", &s_env_vp_fixproj)) {
        { static int _fb = 0; if (PS3_LOG_FIRST(_fb, 6))
            printf("[VPFB] fallback proj on draw slot %u (c00=%g vs_idx=%d)\n",
                   slot, c00, vs_idx); }
        s_cbimg_bank = 0;                   /* no longer the state's bank */
        for (int _i = 0; _i < 16; _i++) c[_i] = 0.0f;
        c[0]=1.358f; c[5]=2.414f; c[11]=1.0f;
        c[10]=1.0f/99.0f; c[14]=-1.0f/99.0f;
        vpx[2] = 1.0f; vpx[6] = 0.0f;
    }
    /* The frame's first draw at the render's size through a perspective has its camera: a
     * shadow pass goes through the same constants at another size, a screen of 2D through
     * a flat matrix. */
    if (s_up.ready && dr && dr->rt_w * 9u == dr->rt_h * 16u && vs_idx >= 0 && vs_idx < s_d3d.vp_vs_n &&
        s_d3d.vp_vs[vs_idx].uses_cam) {
        /* The upscaler's jitter, on what draws the scene at any of its sizes: a shadow atlas
         * is not 16:9. */
        vpx[4] += s_up.cx;
        vpx[5] += s_up.cy;
    }
    if (s_wide != 1.0f && dr) {
        if (dr->zeta_off && dr->rt_w == s_d3d.width && dr->rt_h == s_d3d.height &&
            vs_idx >= 0 && vs_idx < s_d3d.vp_vs_n && s_d3d.vp_vs[vs_idx].uses_cam) {
            const float* m = c + s_fg.cam_reg * 4;
            if (m[3] != 0.0f || m[7] != 0.0f || m[11] != 0.0f) { s_wide_3d = 1; dr->wide_cam = 1; }
        }
        if (s_wide_3d && (dr->rt_off & DISP_RT_TAG)) vpx[7] = 1.0f;   /* over a scene: see "the screen's edges" */
    }
    if (FG_INPUTS() && !s_fg.cam_ok && dr && dr->zeta_off && dr->rt_w == s_d3d.width && dr->rt_h == s_d3d.height &&
        vs_idx >= 0 && vs_idx < s_d3d.vp_vs_n && s_d3d.vp_vs[vs_idx].uses_cam) {
        const float* m = c + s_fg.cam_reg * 4;
        if (m[3] != 0.0f || m[7] != 0.0f || m[11] != 0.0f) {
            memcpy(s_fg.cam, m, sizeof s_fg.cam);
            s_fg.z_scale = vpx[2];
            s_fg.z_offset = vpx[6];
            s_fg.depth_off = dr->zeta_off;
            s_fg.cam_ok = 1;
        }
    }
    if (FG_INPUTS() && dr && s_fg.obj_n < FG_OBJ_MAX && vs_idx >= 0 && vs_idx < s_d3d.vp_vs_n &&
        s_d3d.vp_vs[vs_idx].uses_prev)
        s_fg.obj[s_fg.obj_n++] = slot;
    s_cbimg_ok = 1;
}

/* The slot made by vp_record_cb (and vp_record_raw_cb for a raw draw) to the draw: its own, or the
 * last written one of this batch when the same. */
static void vp_cb_commit(D3D12DrawRecord* dr, u32 slot)
{
    static int share = -1;
    if (share < 0) { const char* e = getenv("VP_CB_SHARE"); share = e ? atoi(e) != 0 : 1; }
    if (!s_cbimg_ok || !dr || slot >= MAX_DRAWS) return;
    s_cbimg_ok = 0;
    const u32 bank_bytes = RSX_MAX_VERTEX_CONSTANTS * 16;
    const int same_bank = s_cbimg_bank && s_cbimg_bank == s_cblast_bank;
    if (share && s_cblast_gen == s_cb_gen && s_cblast_raw == s_cbimg_raw &&
        !memcmp(s_cbimg + (same_bank ? bank_bytes : 0), s_cblast + (same_bank ? bank_bytes : 0),
                VP_CB_IMG - (same_bank ? bank_bytes : 0)) &&
        (!s_cbimg_raw || !memcmp(s_cbimg + VP_CB_RAW_OFF, s_cblast + VP_CB_RAW_OFF, VP_CB_RAW_BYTES))) {
        dr->cb_slot = s_cblast_slot;
        s_cb_shared++;
        s_cb_same_bank += (unsigned)same_bank;
        s_cblast_bank = s_cbimg_bank;       /* the same words */
        return;
    }
    char* dst = (char*)s_d3d.vp_cb_mapped + ((u64)s_d3d.vp_parity * MAX_DRAWS + slot) * VP_CB_STRIDE;
    memcpy(dst, s_cbimg, VP_CB_IMG);
    if (s_cbimg_raw) memcpy(dst + VP_CB_RAW_OFF, s_cbimg + VP_CB_RAW_OFF, VP_CB_RAW_BYTES);
    memcpy(s_cblast, s_cbimg, VP_CB_IMG);
    if (s_cbimg_raw) memcpy(s_cblast + VP_CB_RAW_OFF, s_cbimg + VP_CB_RAW_OFF, VP_CB_RAW_BYTES);
    s_cblast_raw = s_cbimg_raw;
    s_cblast_bank = s_cbimg_bank;
    s_cblast_slot = slot;
    s_cblast_gen = s_cb_gen;
    dr->cb_slot = slot;
    s_cb_written++;
}

/* The SET_SHADER_CONTROL bits a guest FP's translation depends on. */
#define FP_CTRL_BITS (CELL_GCM_SHADER_CONTROL_32_BITS_EXPORTS | CELL_GCM_SHADER_CONTROL_DEPTH_EXPORT)
static u32 rec_fp_ctrl(void)
{
    const rsx_state* st = s_d3d.current_rsx_state;
    if (!st) return CELL_GCM_SHADER_CONTROL_32_BITS_EXPORTS;
    u32 c = st->shader_control & FP_CTRL_BITS;
    /* RSX_NO_SRGB=1: ignore the packer (outputs stay linear, as before). */
    static int no_srgb = -1;
    if (no_srgb < 0) no_srgb = getenv("RSX_NO_SRGB") ? 1 : 0;
    if (!no_srgb && rsx_fp_srgb_output_enabled(st->shader_packer, st->shader_control,
                                               st->surface_format & 0x1Fu))
        c |= FP_CTRL_SRGB_OUT;
    return c;
}

static u32 rec_l2_inval(void)
{
    return s_d3d.current_rsx_state ? s_d3d.current_rsx_state->l2_invalidates : 0u;
}

static u32 rec_surface_a(void)
{
    const rsx_state* st = s_d3d.current_rsx_state;
    if (!st) return 0;
    return st->surface_color_offset[st->color_target == 2 ? 1 : 0];
}

/* SET_SHADER_WINDOW, when it differs from the host's window position (origin
 * top left, pixel centres at .5). A pass that addresses a screen-sized buffer
 * by WPOS with the origin at the bottom read it upside down: God of War:
 * Ascension's menu refracted its own bottom-of-screen text near the top. 0
 * keeps every other draw on its existing pipeline. RSX_WPOS_RAW=1: always 0. */
/* A draw record is filled field by field over whatever the slot held in an
 * earlier batch: the copy ops' fields must be cleared, or a draw that lands in
 * a slot a snapshot or RT copy used is replayed as that stale copy instead. */
static void rec_not_copy(D3D12DrawRecord* dr)
{
    dr->snap_ea = 0; dr->snap_slot = -1; dr->snap_src = 0;
    dr->is_blit = 0; dr->flip_key = 0; dr->zq_op = 0;
}

static u32 rec_wpos_win(void)
{
    const rsx_state* st = s_d3d.current_rsx_state;
    static int raw = -1;
    if (raw < 0) raw = getenv("RSX_WPOS_RAW") ? 1 : 0;
    if (!st || raw) return 0;
    const u32 w = st->shader_window & 0xFFFFFu;
    return (((w >> 12) & 0xFu) || ((w >> 16) & 0xFu)) ? w : 0;
}

static void rec_scissor(D3D12DrawRecord* dr)
{
    const rsx_state* st = s_d3d.current_rsx_state;
    dr->sc_x = st ? st->scissor_x : 0;
    dr->sc_y = st ? st->scissor_y : 0;
    dr->sc_w = st ? st->scissor_w : 4096;
    dr->sc_h = st ? st->scissor_h : 4096;
}

/* RSX_SCISSOR=0: draws clip to their viewport rect and clears cover the whole
 * surface, whatever the guest scissor. */
static int rsx_scissor_on(void)
{
    static int on = -1;
    if (on < 0) { const char* e = getenv("RSX_SCISSOR"); on = e ? atoi(e) : 1; }
    return on;
}

/* The part of a tw x th target a record's scissor leaves, in `r`; 0 when that
 * is the whole target (or the scissor is off), so the caller can keep its
 * full-target path. An empty rect (right <= left) clips everything. */
/* SET_SURFACE_FORMAT's antialias field (bits 12-15) as a sample grid. An
 * antialiased surface is held at its sample resolution, as it lies in guest
 * memory: DIAGONAL_CENTERED_2 (3) is 2x1 samples a pixel, SQUARE_CENTERED_4
 * (4) and SQUARE_ROTATED_4 (5) are 2x2 (rsx_dispatch.c, the shared engine).
 * God of War: Ascension draws its transparent effects into a 640x360 4x
 * surface over its 1280x720 targets and depth (the same memory, as 1280x720
 * samples); drawn as a 640x360 viewport they landed shrunk in the top-left
 * quarter. The viewport and scissor registers stay in pixels and are scaled
 * by the grid; WPOS gets the pixel back (rsx_fp_apply_wpos_grid).
 * RSX_AA_GRID=0: off. */
static void rsx_aa_grid(u32 surface_format, u32* sx, u32* sy)
{
    static int off = -1;
    if (off < 0) { const char* e = getenv("RSX_AA_GRID"); off = (e && atoi(e) == 0) ? 1 : 0; }
    const u32 aa = (surface_format >> 12) & 0xFu;
    *sx = (!off && aa >= 3 && aa <= 5) ? 2u : 1u;
    *sy = (!off && (aa == 4 || aa == 5)) ? 2u : 1u;
}

static int rec_scissor_rect(const D3D12DrawRecord* dr, LONG tw, LONG th, D3D12_RECT* r)
{
    if (!rsx_scissor_on()) return 0;
    u32 ax, ay;
    rsx_aa_grid(dr->rt_fmt, &ax, &ay);
    LONG l = (LONG)(dr->sc_x * ax), t = (LONG)(dr->sc_y * ay);
    LONG rr = l + (LONG)(dr->sc_w * ax), b = t + (LONG)(dr->sc_h * ay);
    if (rr > tw) rr = tw;
    if (b > th) b = th;
    if (l > rr) l = rr;
    if (t > b) t = b;
    if (l <= 0 && t <= 0 && rr >= tw && b >= th) return 0;
    r->left = l; r->top = t; r->right = rr; r->bottom = b;
    return 1;
}

/* The draw's depth surface (none when the surface format has no depth), and
 * whether it is a depth-only pass: colour target NONE with a depth surface. */
static void rec_zeta(D3D12DrawRecord* dr)
{
    const rsx_state* st = s_d3d.current_rsx_state;
    dr->zeta_off = 0;
    dr->no_color = 0;
    if (!st || !((st->surface_format >> 5) & 7u)) return;
    dr->zeta_off = st->surface_zeta_offset;
    dr->no_color = st->color_target == 0 && dr->zeta_off != 0;
}

/* Colour surfaces in main memory, written back.
 *
 * A colour surface whose context DMA is the IO-mapped host buffer (0xFEED0001)
 * is drawn into main memory, and that is where the CPU reads it. God of War:
 * Ascension renders the scene's average luminance into a 1x1 A8R8G8B8 surface
 * there, and the PPU takes the exposure from it (a half float in its first two
 * bytes). The GPU's copy never reached guest memory, so the PPU read 0 and the
 * first level's exposure stayed at its maximum (1.4, where RPCS3 had 0.52): the
 * fog and the sky washed out to white. A draw or clear into such a surface is
 * recorded here; at the batch's end the surface is copied to a readback buffer
 * (mwb_record), and once the batch has run its pixels go to guest memory in the
 * RSX's byte order (mwb_poll). RSX_MAIN_WB=0: off. */
extern u32 rsx_color_dma[2];   /* SET_CONTEXT_DMA_COLOR_A/B as the stream set them (rsx_commands.c) */
static int mwb_on(void)
{
    static int v = -1;
    if (v < 0) { const char* e = getenv("RSX_MAIN_WB"); v = !(e && atoi(e) == 0); }
    return v;
}
static void rec_mwb(D3D12DrawRecord* dr)
{
    const rsx_state* st = s_d3d.current_rsx_state;
    dr->mwb_off = 0;
    /* One target, A or B; an MRT set in main memory is left as it was. */
    if (!st || !mwb_on() || st->color_target == 0 || st->color_target >= 0x13) return;
    const int sel = st->color_target == 2 ? 1 : 0;
    if (rsx_color_dma[sel] != CELL_GCM_CONTEXT_DMA_MEMORY_HOST_BUFFER) return;
    dr->mwb_off = st->surface_color_offset[sel];
    dr->mwb_pitch = st->surface_color_pitch[sel];
    dr->mwb_w = st->surface_clip_w;
    dr->mwb_h = st->surface_clip_h;
    dr->mwb_fmt = st->surface_format;
}

/* Which offscreen surface (if any) does the current RSX state render to?
 * Returns 0 for a display buffer (backbuffer), else the surface's RAW RSX
 * offset. RTs are keyed by raw offset -- a texture bound at the same raw
 * offset is the same buffer (surface and texture registers share the offset
 * space; location bits differ but one title doesn't alias local vs main at
 * one offset), which sidesteps guessing the surface's context DMA location. */
static u32 current_rt_off(u32* out_w, u32* out_h, u32 out_mrt[3])
{
    extern int cellGcmOffsetIsDisplay(u32 offset);
    const rsx_state* st = s_d3d.current_rsx_state;
    if (out_w) *out_w = 0;
    if (out_h) *out_h = 0;
    if (out_mrt) out_mrt[0] = out_mrt[1] = out_mrt[2] = 0;
    if (!st) return 0;
    /* SET_SURFACE_COLOR_TARGET: 1 = A, 2 = B, 0x13 = MRT1 (A+B),
     * 0x17 = MRT2 (A+B+C), 0x1F = MRT3 (A+B+C+D). */
    int sel = (st->color_target == 2) ? 1 : 0;
    u32 raw = st->surface_color_offset[sel];
    if (out_mrt) {
        if (st->color_target >= 0x13) out_mrt[0] = st->surface_color_offset[1];
        if (st->color_target >= 0x17) out_mrt[1] = st->surface_color_offset[2];
        if (st->color_target >= 0x1F) out_mrt[2] = st->surface_color_offset[3];
    }
    { static int _rs = -1; if (_rs < 0) _rs = getenv("RT_SEQDBG") ? 1 : 0;
      static u32 _last = 0; static int _n2 = 0;
      if (_rs && st->surface_color_offset[0] != _last && _n2 < 400000) {
          _last = st->surface_color_offset[0]; _n2++;
          fprintf(stderr, "[RTSEQ] #%d -> 0x%X%c", _n2, _last, 10); } }
    { static int _ro = -1; if (_ro < 0) _ro = getenv("RT_OFFDBG") ? 1 : 0;
      static u32 _seen[32]; static int _ns = 0;
      if (_ro) { u32 _k = st->surface_color_offset[0] ^ (st->color_target << 24)
                        ^ (st->surface_clip_w << 8) ^ st->surface_format ^ (rsx_color_dma[0] << 20);
        int _f = 0; for (int _i = 0; _i < _ns; _i++) if (_seen[_i] == _k) _f = 1;
        if (!_f && _ns < 32) { _seen[_ns++] = _k;
        fprintf(stderr, "[RTOFF] fmt=0x%X tgt=0x%X off[0]=0x%X off[1]=0x%X zeta=0x%X clip=%ux%u disp=%d dma=0x%X/0x%X -> 0x%X\n",
                st->surface_format, st->color_target, st->surface_color_offset[0], st->surface_color_offset[1],
                st->surface_zeta_offset, st->surface_clip_w, st->surface_clip_h,
                cellGcmOffsetIsDisplay(raw), rsx_color_dma[0], rsx_color_dma[1],
                cellGcmOffsetIsDisplay(raw) ? 0 : raw); } } }
    if (cellGcmOffsetIsDisplay(raw)) {
        if (!display_rt_on()) return 0;
        if (out_w) *out_w = st->surface_clip_w;
        if (out_h) *out_h = st->surface_clip_h;
        return DISP_RT_TAG | raw;
    }
    /* RT_DISPLAY_BY_SIZE=1: also treat a single-target surface whose clip
     * exactly matches the display resolution as the backbuffer.
     *
     * cellGcmSetDisplayBuffer only registers the buffers the FLIP may point at;
     * a guest is free to render into a different surface of the same size and
     * flip to it later (PSGL does). Matching offsets alone then classifies every
     * draw as offscreen, has_display stays 0, and render_frame() -- which is
     * what draws ALL recorded geometry -- is never called. The backbuffer shows
     * only the clear, which looks exactly like "nothing rasterizes". */
    { static int _bs = -1; if (_bs < 0) _bs = getenv("RT_DISPLAY_BY_SIZE") ? 1 : 0;
      if (_bs && st->color_target < 0x13 &&
          st->surface_clip_w == s_d3d.width && st->surface_clip_h == s_d3d.height)
          return 0; }
    /* Surface clip dims when sane; else the window size. Any size works --
     * passes draw normalized full-surface quads -- this only picks resolution. */
    u32 w = st->surface_clip_w, h = st->surface_clip_h;
    { u32 ax, ay; rsx_aa_grid(st->surface_format, &ax, &ay); w *= ax; h *= ay; }
    if (w < 16 || w > 2048 || h < 16 || h > 2048) { w = 0; h = 0; }
    if (out_w) *out_w = w;
    if (out_h) *out_h = h;
    return raw;
}

/* RSX surface colour format (SET_SURFACE_FORMAT bits [4:0]) -> DXGI. Float
 * targets matter: wave's water height maps store SIGNED values (F_W16..),
 * demosaic's differential planes likewise -- RGBA8 clamps them to zero. */
static DXGI_FORMAT rsx_surface_dxgi(u32 fmt)
{
    switch (fmt & 0x1F) {
    case 0x0B: return DXGI_FORMAT_R16G16B16A16_FLOAT; /* F_W16Z16Y16X16 */
    case 0x0C: return DXGI_FORMAT_R32G32B32A32_FLOAT; /* F_W32Z32Y32X32 */
    case 0x0D: return DXGI_FORMAT_R32_FLOAT;          /* F_X32          */
    default:   return DXGI_FORMAT_R8G8B8A8_UNORM;
    }
}

/* RSX_RT_SLOTS=<n>: use n of the MAX_OFF_RTS slots (8 at least), to see what a
 * full table does. The table's traffic, for RSX_CENSUS. */
static unsigned long long s_ort_made, s_ort_remade, s_ort_evicted, s_ort_refused;
static int ort_cap(void)
{
    static int v = -1;
    if (v < 0) {
        const char* e = getenv("RSX_RT_SLOTS");
        v = e ? atoi(e) : MAX_OFF_RTS;
        if (v < 8) v = 8;
        if (v > MAX_OFF_RTS) v = MAX_OFF_RTS;
    }
    return v;
}

/* A bit for an offset, of 1024: most of the textures a draw binds were never a target, and a clear
 * bit says so without walking the 64 slots (or the 64 aliases) for it. A set bit can be stale, so
 * the walk still decides. */
static inline u32 off_bit(u32 off) { return (off * 0x9E3779B1u) >> 22; }
static inline int off_bit_get(const u64* s, u32 off) { const u32 b = off_bit(off); return (int)((s[b >> 6] >> (b & 63)) & 1u); }
static inline void off_bit_set(u64* s, u32 off) { const u32 b = off_bit(off); s[b >> 6] |= 1ull << (b & 63); }
static u64 s_ort_bits[16];      /* the offsets of off_rt's slots; set as one is made, rebuilt each batch */
static void ort_bits_rebuild(void)
{
    /* Made aside and copied a word at a time: a lookup from another thread (the walk's
     * DISP_RT_DBG print, with the render thread on) sees each word old or new, and both
     * hold every live slot's offset. */
    u64 b[16];
    memset(b, 0, sizeof b);
    for (int i = 0; i < MAX_OFF_RTS; i++)
        if (s_d3d.off_rt[i].off) off_bit_set(b, s_d3d.off_rt[i].off);
    for (int w = 0; w < 16; w++) ((volatile u64*)s_ort_bits)[w] = b[w];
}

static int off_rt_find(u32 off)
{
    if (!off) return -1;
    if (!off_bit_get(s_ort_bits, off)) return -1;
    for (int i = 0; i < MAX_OFF_RTS; i++)
        if (s_d3d.off_rt[i].res && s_d3d.off_rt[i].off == off) {
            s_d3d.off_rt[i].last = s_d3d.frame_count;   /* sampled: recent for eviction */
            return i;
        }
    return -1;
}

/* Ensure an RT resource exists for this surface; (re)creates the RTV at
 * rt_rtv_heap[i] and the SRV at srv_heap[RT_SRV_BASE+i]. */
/* render_frame()'s state between its stages (see "render_frame() in
 * stages"): the frame index the batch records for, the display handles, and
 * the VP pass's bindings, which recording ahead carries from one call to the
 * next. */
static struct {
    int  open;                     /* recording ahead: the list holds part of the batch */
    u32  fi, pre_done, done;       /* prepass and recording progress */
    D3D12_CPU_DESCRIPTOR_HANDLE rtv_handle, dsv_handle, cur_dsv;
    int  vp_on;
    ID3D12PipelineState* vpso;
    D3D12_GPU_DESCRIPTOR_HANDLE gh_base;
    int  samp_bound[VP_TEX_UNITS], cur_rt, ib_bound, raw_bound, cur_m[3], cur_z, cur_nc, force_rebind;
    int  seen_small_vp, captured;
    float vp_last_w, vp_last_h, dvp_x, dvp_y, dvp_w, dvp_h;
} s_rf;

/* A resource the open command list may still reference is released only after
 * that list has run: off_rt_get and off_ds_get replace surfaces from the
 * prepass, which recording ahead runs after earlier draws are recorded. */
static IUnknown* s_rf_dead[256];
static int s_rf_dead_n;
static void rf_release(void* p)
{
    IUnknown* u = (IUnknown*)p;
    if (!u) return;
    if (s_rf.open) {
        if (s_rf_dead_n < (int)(sizeof s_rf_dead / sizeof s_rf_dead[0])) { s_rf_dead[s_rf_dead_n++] = u; return; }
        static int _n = 0;
        if (PS3_LOG_FIRST(_n, 2)) fprintf(stderr, "[D3D12] deferred-release list full; leaking%c", 10);
        return;
    }
    u->lpVtbl->Release(u);
}
static void rf_release_dead(void)
{
    for (int i = 0; i < s_rf_dead_n; i++) s_rf_dead[i]->lpVtbl->Release(s_rf_dead[i]);
    s_rf_dead_n = 0;
}

static int off_rt_get(u32 off, u32 w, u32 h, u32 rsx_fmt)
{
    if (!off || !s_d3d.rt_rtv_heap || !s_d3d.srv_heap) return -1;
    if (!w) w = s_d3d.width;
    if (!h) h = s_d3d.height;
    DXGI_FORMAT want_fmt = rsx_surface_dxgi(rsx_fmt);
    /* RT_FORCE_RGBA8=1: allocate every offscreen target as R8G8B8A8 rather than
     * its native format. A title whose final composite reads a half-float
     * surface cannot be shown with a plain CopyTextureRegion, and giving it an
     * 8-bit target costs HDR precision but makes the composite a same-format
     * copy. A stopgap for looking at the frame, not a rendering fix. */
    { static int f8 = -1;
      if (f8 < 0) { const char* e = getenv("RT_FORCE_RGBA8"); f8 = e ? atoi(e) : 0; }
      if (f8) want_fmt = DXGI_FORMAT_R8G8B8A8_UNORM; }
    int slot = off_rt_find(off);
    if (slot >= 0) {
        OffRT* r = &s_d3d.off_rt[slot];
        const int in_batch = r->last == s_d3d.frame_count;   /* bound or sampled by this batch */
        if (!in_batch) {   /* the size a batch before asked for (grow_later) */
            if (r->dxgi == (u32)want_fmt && r->grow_w > w) w = r->grow_w;
            if (r->dxgi == (u32)want_fmt && r->grow_h > h) h = r->grow_h;
            r->grow_w = r->grow_h = 0;
        }
        if (r->w == w && r->h == h && r->dxgi == (u32)want_fmt) { r->used = 1; r->last = s_d3d.frame_count; return slot; }
        /* RT_REUSE (default 1): a re-bind of the same format no larger than
         * the surface keeps it -- the pass draws into its top-left corner
         * through its viewport -- and a larger one grows it. God of War:
         * Ascension binds 0x24A0000 at 640x360 and then 40x45, and its
         * luminance chain halves one surface down to 8x9; each recreation lost
         * what the pass before had drawn there. RT_REUSE=0: recreate. */
        { static int reuse = -1;
          if (reuse < 0) { const char* e = getenv("RT_REUSE"); reuse = e ? atoi(e) : 1; }
          if (reuse && r->dxgi == (u32)want_fmt) {
              if (r->w >= w && r->h >= h) { r->used = 1; r->last = s_d3d.frame_count; return slot; }
              if (r->w > w) w = r->w;
              if (r->h > h) h = r->h;
              if (in_batch && grow_later()) {
                  if (w > r->grow_w) r->grow_w = w;
                  if (h > r->grow_h) r->grow_h = h;
                  r->used = 1;
                  return slot;
              }
          } }
        /* The frame in flight still references this resource -- releasing it here
         * is "was deleted prior to closing the command list" (D3D12 message 921),
         * and every draw recorded against it silently lands nowhere while the
         * replacement reads back empty. Let the GPU finish first. Recreations are
         * rare (a surface changing size), so the stall costs nothing. */
        s_ort_remade++;
        wait_for_gpu();
        rf_release(r->res); r->res = NULL;   /* dims/format changed */
        if (r->up) { rf_release(r->up); r->up = NULL; }
    } else {
        const int cap = ort_cap();
        for (int i = 0; i < cap; i++)
            if (!s_d3d.off_rt[i].res) { slot = i; break; }
        if (slot < 0) {
            /* Evict the entry bound or sampled longest ago among those not
             * bound this frame. The first one not yet bound this frame was
             * usually one the frame binds later: early in a frame that is
             * every slot, so a full table evicted the frame's own targets.
             * RSX_RT_EVICT=first: that old choice, to compare. */
            static int first = -1;
            if (first < 0) { const char* e = getenv("RSX_RT_EVICT"); first = (e && strcmp(e, "first") == 0) ? 1 : 0; }
            for (int i = 0; i < cap; i++) {
                if (s_d3d.off_rt[i].used) continue;
                if (first) { slot = i; break; }
                if (slot < 0 || s_d3d.off_rt[i].last < s_d3d.off_rt[slot].last) slot = i;
            }
            if (slot < 0) {
                s_ort_refused++;
                static int _n = 0;
                if (PS3_LOG_FIRST(_n, 8))
                    fprintf(stderr, "[D3D12] all %d render-target slots in use this frame;"
                                    " 0x%08X %ux%u gets none%c", cap, off, w, h, 10);
                return -1;
            }
            s_ort_evicted++;
            if (s_ort_evicted <= 24 || (s_ort_evicted & 255u) == 0) {
                const OffRT* e = &s_d3d.off_rt[slot];
                fprintf(stderr, "[D3D12] render-target table full (%d slots): 0x%08X %ux%u, last used at frame %llu,"
                                " evicted for 0x%08X %ux%u at frame %llu (eviction %llu)%c",
                        cap, e->off, e->w, e->h, (unsigned long long)e->last, off, w, h,
                        (unsigned long long)s_d3d.frame_count, s_ort_evicted, 10);
            }
            wait_for_gpu();   /* same hazard as above: the frame may still use it */
            rf_release(s_d3d.off_rt[slot].res);
            s_d3d.off_rt[slot].res = NULL;
            if (s_d3d.off_rt[slot].up) {
                rf_release(s_d3d.off_rt[slot].up);
                s_d3d.off_rt[slot].up = NULL;
            }
        }
    }
    const double rt0 = perf_now();
    OffRT* r = &s_d3d.off_rt[slot];
    D3D12_HEAP_PROPERTIES hp = {0}; hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC td = {0};
    td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    const u32 pw = rs_class(w, h) ? s_d3d.pw : w, ph = rs_class(w, h) ? s_d3d.ph : h;
    td.Width = pw; td.Height = ph; td.DepthOrArraySize = 1; td.MipLevels = 1;
    td.Format = want_fmt; td.SampleDesc.Count = 1;
    td.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    td.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    D3D12_CLEAR_VALUE cv = {0};
    cv.Format = td.Format;
    if (FAILED(s_d3d.device->lpVtbl->CreateCommittedResource(
            s_d3d.device, &hp, D3D12_HEAP_FLAG_NONE, &td,
            D3D12_RESOURCE_STATE_COPY_DEST, &cv,
            &IID_ID3D12Resource, (void**)&r->res)))
        return -1;
    r->off = off; r->w = w; r->h = h; r->dxgi = (u32)want_fmt;
    r->pw = pw; r->ph = ph; r->sx = (float)pw / (float)w; r->sy = (float)ph / (float)h;
    off_bit_set(s_ort_bits, off);
    r->grow_w = r->grow_h = 0;
    r->st = D3D12_RESOURCE_STATE_COPY_DEST;
    r->used = 1;
    r->last = s_d3d.frame_count;
    s_ort_made++;
    rdoc_name((ID3D12Object*)r->res, "rt 0x%08X %ux%u rsxfmt %02X", off, w, h, rsx_fmt);

    /* Populate the new RT from guest memory: titles CPU-initialise their
     * render-target buffers (wave fills the height fields with texels of
     * (0,0,0, 1.0f) -- the .w is 'inverse of mass'; from an all-zero GPU
     * resource the water simulation can never boot). Guest data is
     * big-endian and tightly packed. RSX_RT_READ=0 leaves it empty instead
     * (RPCS3's Read Color Buffers off): God of War: Ascension composites its
     * scene target before drawing a scene into it on its first loading
     * frames, and the old data in that memory showed as a band of noise. */
    {
        extern uint8_t* vm_base;
        extern u32 cellGcmResolveOffset(u32);
        static int rt_read = -1;
        if (rt_read < 0) { const char* e = getenv("RSX_RT_READ"); rt_read = (e && atoi(e) == 0) ? 0 : 1; }
        u32 bpp = (want_fmt == DXGI_FORMAT_R16G16B16A16_FLOAT) ? 8u :
                  (want_fmt == DXGI_FORMAT_R32G32B32A32_FLOAT) ? 16u :
                  (want_fmt == DXGI_FORMAT_R32_FLOAT) ? 4u : 4u;
        u32 pitch = (w * bpp + 255) & ~255u;
        u32 src = cellGcmResolveOffset(off & ~DISP_RT_TAG);
        if (rt_read && vm_base && src != 0xFFFFFFFFu && pw == w && ph == h) {
            D3D12_HEAP_PROPERTIES hu = {0}; hu.Type = D3D12_HEAP_TYPE_UPLOAD;
            D3D12_RESOURCE_DESC bd = {0};
            bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
            bd.Width = (u64)pitch * h; bd.Height = 1; bd.DepthOrArraySize = 1;
            bd.MipLevels = 1; bd.SampleDesc.Count = 1;
            bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
            if (SUCCEEDED(s_d3d.device->lpVtbl->CreateCommittedResource(
                    s_d3d.device, &hu, D3D12_HEAP_FLAG_NONE, &bd,
                    D3D12_RESOURCE_STATE_GENERIC_READ, NULL,
                    &IID_ID3D12Resource, (void**)&r->up))) {
                void* mp = NULL; D3D12_RANGE nr = {0,0};
                if (SUCCEEDED(r->up->lpVtbl->Map(r->up, 0, &nr, &mp)) && mp) {
                    const u8* sp = vm_base + src;
                    for (u32 y = 0; y < h; y++) {
                        const u8* srow = sp + (u64)y * w * bpp;
                        u8* drow = (u8*)mp + (u64)y * pitch;
                        if (want_fmt == DXGI_FORMAT_R8G8B8A8_UNORM) {
                            /* guest A8R8G8B8 (bytes A,R,G,B) -> R,G,B,A */
                            for (u32 x = 0; x < w; x++) {
                                drow[x*4+0] = srow[x*4+1];
                                drow[x*4+1] = srow[x*4+2];
                                drow[x*4+2] = srow[x*4+3];
                                drow[x*4+3] = srow[x*4+0];
                            }
                        } else if (want_fmt == DXGI_FORMAT_R16G16B16A16_FLOAT) {
                            for (u32 x = 0; x < w * 4; x++) {   /* u16 halves, BE */
                                drow[x*2+0] = srow[x*2+1];
                                drow[x*2+1] = srow[x*2+0];
                            }
                        } else {                                 /* u32 floats, BE */
                            u32 nw = (w * bpp) / 4;
                            for (u32 x = 0; x < nw; x++) {
                                drow[x*4+0] = srow[x*4+3];
                                drow[x*4+1] = srow[x*4+2];
                                drow[x*4+2] = srow[x*4+1];
                                drow[x*4+3] = srow[x*4+0];
                            }
                        }
                    }
                    r->up->lpVtbl->Unmap(r->up, 0, NULL);
                    D3D12_TEXTURE_COPY_LOCATION cdst = {0}, csrc = {0};
                    cdst.pResource = r->res;
                    cdst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
                    csrc.pResource = r->up;
                    csrc.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
                    csrc.PlacedFootprint.Footprint.Format   = want_fmt;
                    csrc.PlacedFootprint.Footprint.Width    = w;
                    csrc.PlacedFootprint.Footprint.Height   = h;
                    csrc.PlacedFootprint.Footprint.Depth    = 1;
                    csrc.PlacedFootprint.Footprint.RowPitch = pitch;
                    s_d3d.cmd_list->lpVtbl->CopyTextureRegion(s_d3d.cmd_list, &cdst, 0, 0, 0, &csrc, NULL);
                }
            }
        }
        {
            D3D12_RESOURCE_BARRIER b = {0};
            b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            b.Transition.pResource   = r->res;
            b.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
            b.Transition.StateAfter  = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
            b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            s_d3d.cmd_list->lpVtbl->ResourceBarrier(s_d3d.cmd_list, 1, &b);
            r->st = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        }
    }

    D3D12_CPU_DESCRIPTOR_HANDLE rh;
    s_d3d.rt_rtv_heap->lpVtbl->GetCPUDescriptorHandleForHeapStart(s_d3d.rt_rtv_heap, &rh);
    rh.ptr += (u64)slot * s_d3d.device->lpVtbl->GetDescriptorHandleIncrementSize(
        s_d3d.device, D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    s_d3d.device->lpVtbl->CreateRenderTargetView(s_d3d.device, r->res, NULL, rh);

    D3D12_SHADER_RESOURCE_VIEW_DESC sv = {0};
    sv.Format = td.Format;
    sv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    sv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    sv.Texture2D.MipLevels = 1;
    D3D12_CPU_DESCRIPTOR_HANDLE sh;
    s_d3d.srv_heap->lpVtbl->GetCPUDescriptorHandleForHeapStart(s_d3d.srv_heap, &sh);
    sh.ptr += (u64)(RT_SRV_BASE + slot) * s_d3d.srv_inc;
    s_d3d.device->lpVtbl->CreateShaderResourceView(s_d3d.device, r->res, &sv, sh);

    s_hl_rt_n++; s_hl_rt_s += perf_now() - rt0;
    static int _log = 0;
    if (PS3_LOG_FIRST(_log, 8))
        printf("[D3D12] offscreen RT %d: off=0x%X %ux%u (render-to-texture)\n",
               slot, off, w, h);
    return slot;
}

static D3D12_CPU_DESCRIPTOR_HANDLE off_rt_rtv(int slot)
{
    D3D12_CPU_DESCRIPTOR_HANDLE rh;
    s_d3d.rt_rtv_heap->lpVtbl->GetCPUDescriptorHandleForHeapStart(s_d3d.rt_rtv_heap, &rh);
    rh.ptr += (u64)slot * s_d3d.device->lpVtbl->GetDescriptorHandleIncrementSize(
        s_d3d.device, D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    return rh;
}


/* RSX_CENSUS=<secs> (see cellGcmSys.c): texture binds by format base and how
 * many end up a null SRV (0 no offset, 1 format the texture path does not
 * take, 2 taken but no slot), binds with a mip chain (uploaded as level 0
 * only), and present-to-present intervals. */
static int rsx_census_secs(void)
{
    static int v = -1;
    if (v < 0) { const char* e = getenv("RSX_CENSUS"); v = e ? atoi(e) : 0; }
    return v;
}
static unsigned long long s_cen_bind[256], s_cen_null[256][3], s_cen_mips, s_cen_binds;
static unsigned long long s_cen_hist[10];
static double s_cen_last_present, s_cen_t0, s_cen_max_ms;
static void rsx_census_bind(u32 fmt, u32 mips)
{
    s_cen_bind[fmt & 0xFFu]++; s_cen_binds++;
    if (mips > 1u) s_cen_mips++;
}
static void rsx_census_null(u32 fmt, u32 off)
{
    const u32 bf = fmt & 0x9Fu;
    const int taken = bf == 0x81 || bf == 0x85 || bf == 0x8B || (bf >= 0x86 && bf <= 0x88) ||
                      bf == 0x9A || bf == 0x94 || bf == 0x95 || bf == 0x9C || bf == 0x9F;
    s_cen_null[fmt & 0xFFu][!off ? 0 : (!taken ? 1 : 2)]++;
}
extern void rsx_fp_census_print(void);
static void rsx_census_present(void)
{
    const double now = perf_now();
    if (s_cen_last_present > 0.0) {
        const double ms = (now - s_cen_last_present) * 1000.0;
        static const double edge[9] = { 20, 40, 60, 80, 100, 150, 250, 500, 1000 };
        int b = 0; while (b < 9 && ms >= edge[b]) b++;
        s_cen_hist[b]++;
        if (ms > s_cen_max_ms) s_cen_max_ms = ms;
    }
    { static double pre_prev, rf_prev, cr_prev, ev_prev, dc_prev;
      const double dcr = s_tex_create_s - cr_prev, dev = s_tex_evict_s - ev_prev, ddc = s_tex_decode_s - dc_prev;
      cr_prev = s_tex_create_s; ev_prev = s_tex_evict_s; dc_prev = s_tex_decode_s;
      double dpre = s_perf_pre - pre_prev, drf = s_perf_rf - rf_prev;
      if (dpre < 0.0) dpre = s_perf_pre;   /* PERF reset its sums in between */
      if (drf < 0.0) drf = s_perf_rf;
      pre_prev = s_perf_pre; rf_prev = s_perf_rf;
      /* RSX_HITCH_MS=<ms>: a line for every present at least that long after the one
       * before (150 without it), and with it set also for every present that skipped a
       * draw or queued a build, each with its time since the process started and the
       * frame's draws: stutter and its causes, frame by frame. */
      static double hitch_ms = -1.0, t_proc0;
      static int hitch_every;
      if (hitch_ms < 0.0) {
          const char* e = getenv("RSX_HITCH_MS");
          hitch_ms = e ? atof(e) : 150.0;
          hitch_every = e != NULL;
          FILETIME cr, ex, kt, ut, nf;
          ULARGE_INTEGER a, b;
          GetProcessTimes(GetCurrentProcess(), &cr, &ex, &kt, &ut);
          GetSystemTimePreciseAsFileTime(&nf);
          a.LowPart = cr.dwLowDateTime; a.HighPart = cr.dwHighDateTime;
          b.LowPart = nf.dwLowDateTime; b.HighPart = nf.dwHighDateTime;
          t_proc0 = now - (double)(b.QuadPart - a.QuadPart) / 1e7;
      }
      const double gap_ms = (now - s_cen_last_present) * 1000.0;
      if (s_cen_last_present > 0.0 &&
          (gap_ms >= hitch_ms || (hitch_every && (s_hl_skipped || s_hl_jobs)))) {
          DXGI_QUERY_VIDEO_MEMORY_INFO vm;
          memset(&vm, 0, sizeof vm);
          if (s_adapter3)
              s_adapter3->lpVtbl->QueryVideoMemoryInfo(s_adapter3, 0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &vm);
          fprintf(stderr, "[hitch] %.0f ms at %.2f s, %u draws: "
                          "textures uploaded %u (%.1f ms: create %.1f, evict %.1f, decode %.1f), "
                          "shader waits %.1f ms, draws skipped %u, builds queued %u, GPU wait %.1f ms, prepass %.1f ms, "
                          "render_frame %.1f ms; pipelines made here %u (%.1f ms), upload buffers %u (%.1f ms), "
                          "targets made %u (%.1f ms), heaps made %u (%.1f ms), all GPU waits %u (%.1f ms), "
                          "VRAM %llu of %llu MB; the frame before on the GPU %.1f ms (copies %.1f, draws %.1f, "
                          "the rest %.1f)\n",
                  gap_ms, now - t_proc0, s_dbg_last_draws, s_hl_tex_n, s_hl_tex_s * 1000.0,
                  dcr * 1000.0, dev * 1000.0, ddc * 1000.0, s_hl_sjwait_s * 1000.0,
                  s_hl_skipped, s_hl_jobs, s_hl_gpuwait_s * 1000.0, dpre * 1000.0, drf * 1000.0,
                  s_hl_miss_n, s_hl_miss_s * 1000.0, s_hl_upbuf_n, s_hl_upbuf_s * 1000.0,
                  s_hl_rt_n, s_hl_rt_s * 1000.0, s_hl_heap_n, s_hl_heap_s * 1000.0,
                  s_hl_waits_n, s_hl_waits_s * 1000.0,
                  (unsigned long long)(vm.CurrentUsage >> 20), (unsigned long long)(vm.Budget >> 20),
                  s_hl_gpu_ms[3], s_hl_gpu_ms[0], s_hl_gpu_ms[1], s_hl_gpu_ms[2]);
      } }
    s_hl_tex_n = s_hl_skipped = s_hl_jobs = 0;
    s_hl_tex_s = s_hl_sjwait_s = s_hl_gpuwait_s = 0.0;
    s_hl_miss_n = s_hl_upbuf_n = s_hl_rt_n = s_hl_waits_n = s_hl_heap_n = 0;
    s_hl_miss_s = s_hl_upbuf_s = s_hl_rt_s = s_hl_waits_s = s_hl_heap_s = 0.0;
    s_cen_last_present = now;
    if (s_cen_t0 <= 0.0) s_cen_t0 = now;
    if (now - s_cen_t0 < (double)rsx_census_secs()) return;
    s_cen_t0 = now;
    fprintf(stderr, "[census] presents by interval (ms) <20:%llu 20-40:%llu 40-60:%llu 60-80:%llu "
                    "80-100:%llu 100-150:%llu 150-250:%llu 250-500:%llu 500-1000:%llu >1000:%llu, max %.0f\n",
            s_cen_hist[0], s_cen_hist[1], s_cen_hist[2], s_cen_hist[3], s_cen_hist[4], s_cen_hist[5],
            s_cen_hist[6], s_cen_hist[7], s_cen_hist[8], s_cen_hist[9], s_cen_max_ms);
    if (s_psoc_mode == 1)
        fprintf(stderr, "[census] pipeline cache: %ld built from the file, %ld found built by a draw, %ld new%c",
                (long)s_psoc_built, (long)s_psoc_hits, (long)s_psoc_added, 10);
    tex_decode_print();
    { int held = 0;
      for (int i = 0; i < MAX_OFF_RTS; i++) held += s_d3d.off_rt[i].res != NULL;
      fprintf(stderr, "[census] render targets: %d held of %d slots; %llu made, %llu remade at a new size or"
                      " format, %llu evicted, %llu refused%c", held, ort_cap(), s_ort_made, s_ort_remade,
              s_ort_evicted, s_ort_refused, 10); }
    fprintf(stderr, "[census] samplers made: %ld point or linear, %ld anisotropic at the title's level, "
                    "%ld raised to graphics.anisotropy (%d; 0 = the title's)%c",
            s_samp_made[0], s_samp_made[1], s_samp_made[2], rsx_aniso_setting(), 10);
    fprintf(stderr, "[census] texture binds %llu, with a mip chain %llu; by format (binds / null: "
                    "no offset, format not taken, no slot):", s_cen_binds, s_cen_mips);
    for (int f = 0; f < 256; f++)
        if (s_cen_bind[f] || s_cen_null[f][0] || s_cen_null[f][1] || s_cen_null[f][2])
            fprintf(stderr, " %02X:%llu/%llu,%llu,%llu", f, s_cen_bind[f],
                    s_cen_null[f][0], s_cen_null[f][1], s_cen_null[f][2]);
    fprintf(stderr, "\n");
    rsx_fp_census_print();
    rsx_census_step2_print();
    fflush(stderr);
    memset(s_cen_hist, 0, sizeof s_cen_hist); s_cen_max_ms = 0.0;
    memset(s_cen_bind, 0, sizeof s_cen_bind); memset(s_cen_null, 0, sizeof s_cen_null);
    s_cen_mips = s_cen_binds = 0;
}

/* A draw's t0-t15 views, collected by the prepass instead of written one by
 * one: a draw whose 16 views equal an earlier draw's in the same batch binds
 * that draw's window, and only a new set is written, copied from the view
 * cache below. Creating the views was 14 ms of the frame clock thread's
 * ~55 ms a frame in God of War: Ascension's first level (~1,800 windows a
 * frame, 16 views each, CreateShaderResourceView every time). Within a batch
 * nothing a view names is released (releases wait for the next batch's GPU
 * wait), so equal pointers are the same resources -- except a texture slot
 * dropped to be made again at other dimensions, whose drop starts a new map
 * (vp_tex_drop). A draw that samples an offscreen target neither shares nor lends its
 * window: the draw pass may put a copy of its own target in it
 * (RT_FEEDBACK). SRV_DEDUPE=0 writes every window as before. */
typedef struct { ID3D12Resource* res; u32 fmt, mapping, cube; int ts; } SrvKey;   /* no padding: hashed and compared whole */
#define SRVQ_MAP (2 * MAX_DRAWS)   /* a power of two, at most half full */
typedef char srvq_map_pow2[(SRVQ_MAP & (SRVQ_MAP - 1)) == 0 ? 1 : -1];
static SrvKey s_srvk[MAX_DRAWS][VP_TEX_UNITS];   /* each window's views, for the compare */
static struct { u32 gen, hash, win; } s_srvq_map[SRVQ_MAP];
/* s_srvq_gen (declared with vp_tex_drop): bumped at each batch and at each texture
 * slot's release, so the map's entries are this batch's, and name only live views. */
static int s_srvq_on;          /* collecting the window at s_srvq_base */
static u32 s_srvq_base;
static SrvKey s_srvq_keys[VP_TEX_UNITS];
static u32 s_srvq_written, s_srvq_reused, s_srvq_copied, s_srvq_made;
static void srv_write_ex(u32 heap_slot, ID3D12Resource* res, DXGI_FORMAT fmt,
                         UINT mapping, int cube);
static void srv_create_at(D3D12_CPU_DESCRIPTOR_HANDLE h, ID3D12Resource* res, DXGI_FORMAT fmt,
                          UINT mapping, int cube);

/* The view cache: a CPU-only heap holding the null view and each texture
 * slot's views, copied into a draw's window (CopyDescriptors) instead of
 * creating them there. A slot's views go with its resource (vp_tex_drop). */
#define SRVC_VARIANTS 4
static ID3D12DescriptorHeap* s_srvc_heap;
static D3D12_CPU_DESCRIPTOR_HANDLE s_srvc_base;
static int srvc_ready(void)
{
    static int failed;
    if (s_srvc_heap) return 1;
    if (failed || !s_d3d.device) return 0;
    D3D12_DESCRIPTOR_HEAP_DESC hd;
    memset(&hd, 0, sizeof hd);
    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    hd.NumDescriptors = 1 + VP_TEX_SLOTS * SRVC_VARIANTS;
    hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
    if (FAILED(s_d3d.device->lpVtbl->CreateDescriptorHeap(s_d3d.device, &hd, &IID_ID3D12DescriptorHeap,
                                                          (void**)&s_srvc_heap))) {
        s_srvc_heap = NULL;
        failed = 1;
        fprintf(stderr, "[D3D12] view cache heap not made: views are created per draw%c", 10);
        return 0;
    }
    s_srvc_heap->lpVtbl->GetCPUDescriptorHandleForHeapStart(s_srvc_heap, &s_srvc_base);
    srv_create_at(s_srvc_base, NULL, DXGI_FORMAT_R8G8B8A8_UNORM, D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING, 0);
    return 1;
}
static D3D12_CPU_DESCRIPTOR_HANDLE srvc_at(u32 i)
{
    D3D12_CPU_DESCRIPTOR_HANDLE h = s_srvc_base;
    h.ptr += (SIZE_T)i * s_d3d.srv_inc;
    return h;
}
static D3D12_CPU_DESCRIPTOR_HANDLE srvc_tex(int ts, const SrvKey* k)
{
    VPTexSlot* t = &s_d3d.vp_tex[ts];
    for (int v = 0; v < t->srv_n; v++)
        if (t->srv_fmt[v] == k->fmt && t->srv_map[v] == k->mapping && t->srv_cube[v] == k->cube)
            return srvc_at(1u + (u32)ts * SRVC_VARIANTS + (u32)v);
    const int v = t->srv_n < SRVC_VARIANTS ? t->srv_n++ : (t->srv_next++ % SRVC_VARIANTS);
    t->srv_fmt[v] = k->fmt; t->srv_map[v] = k->mapping; t->srv_cube[v] = (u8)k->cube;
    const D3D12_CPU_DESCRIPTOR_HANDLE h = srvc_at(1u + (u32)ts * SRVC_VARIANTS + (u32)v);
    srv_create_at(h, t->res, (DXGI_FORMAT)k->fmt, k->mapping, (int)k->cube);
    s_srvq_made++;
    return h;
}
static int srvq_dedupe_on(void)
{
    static int v = -1;
    if (v < 0) { const char* e = getenv("SRV_DEDUPE"); v = e ? atoi(e) != 0 : 1; }
    return v;
}
static void srvq_begin(u32 d)
{
    if (!srvq_dedupe_on()) return;
    for (int u = 0; u < VP_TEX_UNITS; u++) {
        s_srvq_keys[u].res = NULL;
        s_srvq_keys[u].fmt = (u32)DXGI_FORMAT_R8G8B8A8_UNORM;
        s_srvq_keys[u].mapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        s_srvq_keys[u].cube = 0;
        s_srvq_keys[u].ts = -1;
    }
    s_srvq_base = DRAW_SRV_BASE + d * VP_TEX_UNITS;
    s_srvq_on = 1;
}
/* A texture slot's resource released while a draw's views are being
 * collected (the same texture bound again at other dimensions): what the
 * draw named from it becomes the null view, not a view of freed memory. */
static void srvq_forget(ID3D12Resource* res)
{
    if (!s_srvq_on) return;
    for (int u = 0; u < VP_TEX_UNITS; u++)
        if (s_srvq_keys[u].res == res) {
            s_srvq_keys[u].res = NULL;
            s_srvq_keys[u].fmt = (u32)DXGI_FORMAT_R8G8B8A8_UNORM;
            s_srvq_keys[u].mapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            s_srvq_keys[u].cube = 0;
            s_srvq_keys[u].ts = -1;
        }
}
static void srvq_commit(D3D12DrawRecord* dr, u32 d)
{
    if (!s_srvq_on) return;
    s_srvq_on = 0;
    int share = 1;
    for (int u = 0; u < VP_TEX_UNITS; u++)
        if (dr->tex_rt[u] >= 0 || dr->tex_cv[u] >= 0 || dr->tex_ds[u] >= 0 || dr->tex_dz[u] >= 0) share = 0;
    u32 h = 2166136261u;
    if (share) {
        const u32* w = (const u32*)s_srvq_keys;
        for (size_t i = 0; i < sizeof s_srvq_keys / 4; i++) h = (h ^ w[i]) * 16777619u;
        const u32 mask = SRVQ_MAP - 1;
        for (u32 i = (h * 2654435761u) & mask;; i = (i + 1) & mask) {
            if (s_srvq_map[i].gen != s_srvq_gen) break;
            if (s_srvq_map[i].hash == h &&
                !memcmp(s_srvk[s_srvq_map[i].win], s_srvq_keys, sizeof s_srvq_keys)) {
                dr->srv_win = s_srvq_map[i].win;
                s_srvq_reused++;
                return;
            }
        }
    }
    memcpy(s_srvk[d], s_srvq_keys, sizeof s_srvq_keys);
    /* The null view and texture slots' views are copied from the cache; the
     * rest (targets, snapshots, copies) are made in place. */
    D3D12_CPU_DESCRIPTOR_HANDLE dst[VP_TEX_UNITS], src[VP_TEX_UNITS];
    UINT ones[VP_TEX_UNITS];
    UINT n = 0;
    const int cache = srvc_ready();
    D3D12_CPU_DESCRIPTOR_HANDLE wbase;
    s_d3d.srv_heap->lpVtbl->GetCPUDescriptorHandleForHeapStart(s_d3d.srv_heap, &wbase);
    for (int u = 0; u < VP_TEX_UNITS; u++) {
        const SrvKey* k = &s_srvq_keys[u];
        const u32 slot = DRAW_SRV_BASE + d * VP_TEX_UNITS + (u32)u;
        const int null_view = !k->res && k->fmt == (u32)DXGI_FORMAT_R8G8B8A8_UNORM &&
                              k->mapping == D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING && !k->cube;
        if (cache && (null_view || (k->ts >= 0 && k->res && s_d3d.vp_tex[k->ts].res == k->res))) {
            dst[n] = wbase; dst[n].ptr += (SIZE_T)slot * s_d3d.srv_inc;
            src[n] = null_view ? srvc_at(0) : srvc_tex(k->ts, k);
            ones[n] = 1;
            n++;
        } else {
            srv_write_ex(slot, k->res, (DXGI_FORMAT)k->fmt, k->mapping, (int)k->cube);
        }
    }
    if (n) {
        s_d3d.device->lpVtbl->CopyDescriptors(s_d3d.device, n, dst, ones, n, src, ones,
                                              D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        s_srvq_copied += n;
    }
    dr->srv_win = d;
    s_srvq_written++;
    if (share) {
        const u32 mask = SRVQ_MAP - 1;
        u32 i = (h * 2654435761u) & mask;
        while (s_srvq_map[i].gen == s_srvq_gen) i = (i + 1) & mask;
        s_srvq_map[i].gen = s_srvq_gen; s_srvq_map[i].hash = h; s_srvq_map[i].win = d;
    }
}

/* Write a texture SRV (or a null SRV when res == NULL) at an absolute SRV
 * heap slot. Used to fill per-draw t0-t15 descriptor windows. */
static void srv_write_ex(u32 heap_slot, ID3D12Resource* res, DXGI_FORMAT fmt,
                         UINT mapping, int cube)
{
    if (s_srvq_on && heap_slot - s_srvq_base < VP_TEX_UNITS) {
        SrvKey* k = &s_srvq_keys[heap_slot - s_srvq_base];
        k->res = res; k->fmt = (u32)fmt; k->mapping = mapping; k->cube = (u32)cube;
        k->ts = -1;
        return;
    }
    D3D12_CPU_DESCRIPTOR_HANDLE h;
    s_d3d.srv_heap->lpVtbl->GetCPUDescriptorHandleForHeapStart(s_d3d.srv_heap, &h);
    h.ptr += (u64)heap_slot * s_d3d.srv_inc;
    srv_create_at(h, res, fmt, mapping, cube);
}

/* A texture slot's view: through the view cache when the window is collected. */
static void srv_write_tex(u32 heap_slot, int ts, DXGI_FORMAT fmt, UINT mapping, int cube)
{
    if (s_srvq_on && heap_slot - s_srvq_base < VP_TEX_UNITS) {
        srv_write_ex(heap_slot, s_d3d.vp_tex[ts].res, fmt, mapping, cube);
        s_srvq_keys[heap_slot - s_srvq_base].ts = ts;
        return;
    }
    srv_write_ex(heap_slot, s_d3d.vp_tex[ts].res, fmt, mapping, cube);
}

static void srv_create_at(D3D12_CPU_DESCRIPTOR_HANDLE h, ID3D12Resource* res, DXGI_FORMAT fmt,
                          UINT mapping, int cube)
{
    D3D12_SHADER_RESOURCE_VIEW_DESC sv = {0};
    sv.Format = fmt;
    sv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    sv.Shader4ComponentMapping = mapping;
    sv.Texture2D.MipLevels = (UINT)-1;   /* every level the resource has */
    if (cube == 2) {   /* a volume: must match the Texture3D declaration */
        sv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE3D;
        sv.Texture3D.MipLevels = (UINT)-1;
        sv.Texture3D.MostDetailedMip = 0;
        sv.Texture3D.ResourceMinLODClamp = 0.0f;
    } else if (cube) {   /* must match the TextureCube declaration in the HLSL */
        sv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURECUBE;
        sv.TextureCube.MipLevels = (UINT)-1;
        sv.TextureCube.MostDetailedMip = 0;
        sv.TextureCube.ResourceMinLODClamp = 0.0f;
    }
    s_d3d.device->lpVtbl->CreateShaderResourceView(s_d3d.device, res, &sv, h);
}

static void srv_write(u32 heap_slot, ID3D12Resource* res, DXGI_FORMAT fmt, UINT mapping)
{
    srv_write_ex(heap_slot, res, fmt, mapping, 0);
}

static void off_rt_transition(int slot, D3D12_RESOURCE_STATES to)
{
    OffRT* r = &s_d3d.off_rt[slot];
    if (r->st == to) return;
    D3D12_RESOURCE_BARRIER b = {0};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource   = r->res;
    b.Transition.StateBefore = r->st;
    b.Transition.StateAfter  = to;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    s_d3d.cmd_list->lpVtbl->ResourceBarrier(s_d3d.cmd_list, 1, &b);
    r->st = to;
}

/* A draw may sample the target it draws into -- God of War: Ascension's menu
 * blends each frame into its own 640x360 history target that way. D3D12
 * cannot read a resource bound as a render target: such a unit reads a copy.
 * The RSX reads a target through its texture cache, which the title
 * invalidates (NV4097_INVALIDATE_L2) when it wants what was drawn since, so
 * the copy is made at the first such draw after an invalidate and read until
 * the next. A copy before every draw cost the first level's opening a 65-170
 * ms frame on the GPU: runs of up to 128 particles each copied the 1280x720
 * target, with no invalidate between them. RT_FEEDBACK=1: a copy before
 * every draw; 0: none, the target itself is read. */
static struct { ID3D12Resource* res; u32 w, h, dxgi; D3D12_RESOURCE_STATES st; u32 inval; u64 frame; } s_fb[MAX_OFF_RTS];

static ID3D12Resource* fb_copy(int slot)
{
    OffRT* r = &s_d3d.off_rt[slot];
    if (!r->res) return NULL;
    if (!s_fb[slot].res || s_fb[slot].w != r->pw || s_fb[slot].h != r->ph || s_fb[slot].dxgi != r->dxgi) {
        if (s_fb[slot].res) { rf_release(s_fb[slot].res); s_fb[slot].res = NULL; }
        D3D12_HEAP_PROPERTIES hp = {0}; hp.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC td = {0};
        td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        td.Width = r->pw; td.Height = r->ph; td.DepthOrArraySize = 1; td.MipLevels = 1;
        td.Format = (DXGI_FORMAT)r->dxgi; td.SampleDesc.Count = 1;
        td.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        if (FAILED(s_d3d.device->lpVtbl->CreateCommittedResource(s_d3d.device, &hp, D3D12_HEAP_FLAG_NONE,
                   &td, D3D12_RESOURCE_STATE_COPY_DEST, NULL, &IID_ID3D12Resource,
                   (void**)&s_fb[slot].res)))
            return NULL;
        s_fb[slot].w = r->pw; s_fb[slot].h = r->ph; s_fb[slot].dxgi = r->dxgi;
        s_fb[slot].st = D3D12_RESOURCE_STATE_COPY_DEST;
    }
    D3D12_RESOURCE_BARRIER b = {0};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = s_fb[slot].res;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    if (s_fb[slot].st != D3D12_RESOURCE_STATE_COPY_DEST) {
        b.Transition.StateBefore = s_fb[slot].st;
        b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
        s_d3d.cmd_list->lpVtbl->ResourceBarrier(s_d3d.cmd_list, 1, &b);
    }
    const D3D12_RESOURCE_STATES was = r->st;
    off_rt_transition(slot, D3D12_RESOURCE_STATE_COPY_SOURCE);
    s_d3d.cmd_list->lpVtbl->CopyResource(s_d3d.cmd_list, s_fb[slot].res, r->res);
    off_rt_transition(slot, was);
    b.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
    b.Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    s_d3d.cmd_list->lpVtbl->ResourceBarrier(s_d3d.cmd_list, 1, &b);
    s_fb[slot].st = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    return s_fb[slot].res;
}

/* Whether a bound texture may be a depth surface read as one: not a display
 * buffer with its own target. God of War: Ascension's intro uses only two
 * display buffers and puts a depth surface where the third one lies; the menu
 * later reads that third buffer as a texture, and the stale depth surface at
 * the same offset handed it the depth (far plane: R and G full, B zero) -- the
 * rare all-yellow frame. */
static int tex_reads_depth(const D3D12DrawRecord* dr, int u)
{
    return dr->tex[u].set && ((dr->tex_zok >> u) & 1u) &&
           tex_rt_key(dr->tex[u].raw, dr->tex[u].off) == dr->tex[u].raw;
}

/* The kind of surface that last wrote each offset, in op order across
 * batches: 1 colour, 2 depth/stencil. God of War: Ascension reuses memory
 * within a frame -- 0xA8C000 is a depth surface in one pass and the colour
 * target of the composite and bloom in the next. Read as a depth texture there,
 * its depth-to-colour copy (which shares the colour target's slot) overwrote
 * the composite with the far plane: an all-yellow frame. */
#define ALIAS_N 64
static struct { u32 off; u8 kind; } s_alias[ALIAS_N];
static int s_alias_n, s_alias_next;
static u64 s_alias_bits[16];    /* the offsets in s_alias (off_bit), rebuilt as the ring comes round */
static int alias_kind(u32 off)
{
    if (!off_bit_get(s_alias_bits, off)) return 0;
    for (int i = 0; i < s_alias_n; i++) if (s_alias[i].off == off) return s_alias[i].kind;
    return 0;
}
static void alias_set(u32 off, int kind)
{
    if (!off) return;
    for (int i = 0; i < s_alias_n; i++) if (s_alias[i].off == off) { s_alias[i].kind = (u8)kind; return; }
    int i = s_alias_n < ALIAS_N ? s_alias_n++ : (s_alias_next++ % ALIAS_N);
    s_alias[i].off = off; s_alias[i].kind = (u8)kind;
    if (s_alias_n == ALIAS_N && s_alias_next % ALIAS_N == 0) {   /* the old offsets' bits go */
        memset(s_alias_bits, 0, sizeof s_alias_bits);
        for (int k = 0; k < s_alias_n; k++) off_bit_set(s_alias_bits, s_alias[k].off);
    } else {
        off_bit_set(s_alias_bits, off);
    }
}
/* Record dr's reads (tex_zok), then its writes. */
static void alias_note(D3D12DrawRecord* dr)
{
    dr->tex_zok = 0;
    if (!dr->is_vp) return;
    for (int u = 0; u < VP_TEX_UNITS; u++)
        if (dr->tex[u].set && alias_kind(dr->tex[u].raw) != 1) dr->tex_zok |= (u16)(1u << u);
    if (dr->snap_ea || dr->is_blit) return;
    const int colour = dr->is_clear ? (dr->clear_mask & 0xF0u) != 0 : dr->cmask != 0;
    if (colour && !dr->no_color) {
        if (dr->rt_off && !(dr->rt_off & (DISP_RT_TAG | Y16_RT_TAG))) alias_set(dr->rt_off, 1);
        for (int m = 0; m < 3; m++) if (dr->rt_mrt[m]) alias_set(dr->rt_mrt[m], 1);
    }
    const int zw = dr->is_clear ? (dr->clear_mask & 3u) != 0
                                : ((dr->zs_key & 3u) == 3u || ((dr->zs_key >> 5) & 1u));
    if (dr->zeta_off && zw) alias_set(dr->zeta_off, 2);
}

static int off_ds_find(u32 off)
{
    if (!off) return -1;
    for (int i = 0; i < MAX_OFF_DS; i++)
        if (s_d3d.off_ds[i].res && s_d3d.off_ds[i].off == off) return i;
    return -1;
}

static D3D12_CPU_DESCRIPTOR_HANDLE off_ds_dsv(int i)
{
    D3D12_CPU_DESCRIPTOR_HANDLE h;
    s_d3d.ds_heap->lpVtbl->GetCPUDescriptorHandleForHeapStart(s_d3d.ds_heap, &h);
    h.ptr += (u64)i * s_d3d.device->lpVtbl->GetDescriptorHandleIncrementSize(
        s_d3d.device, D3D12_DESCRIPTOR_HEAP_TYPE_DSV);
    return h;
}

static void off_ds_transition(int i, D3D12_RESOURCE_STATES to)
{
    OffDS* z = &s_d3d.off_ds[i];
    if (z->st == to) return;
    D3D12_RESOURCE_BARRIER br = {0};
    br.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    br.Transition.pResource   = z->res;
    br.Transition.StateBefore = z->st;
    br.Transition.StateAfter  = to;
    br.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    s_d3d.cmd_list->lpVtbl->ResourceBarrier(s_d3d.cmd_list, 1, &br);
    z->st = to;
}

/* What the marker copies into, made on first use and, the window's size, after a resize. */
static int fg_targets(void)
{
    ID3D12Device* dev = s_d3d.device;
    const D3D12_RESOURCE_STATES read = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    D3D12_HEAP_PROPERTIES hp = {0};
    hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC rd = {0};
    rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    rd.DepthOrArraySize = 1;
    rd.MipLevels = 1;
    rd.SampleDesc.Count = 1;
    if (!s_fg.depth) {
        rd.Width = s_d3d.pw; rd.Height = s_d3d.ph;
        rd.Format = DXGI_FORMAT_R24G8_TYPELESS;
        rd.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
        if (FAILED(dev->lpVtbl->CreateCommittedResource(dev, &hp, D3D12_HEAP_FLAG_NONE, &rd,
                D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, NULL, &IID_ID3D12Resource, (void**)&s_fg.depth)))
            return 0;
    }
    if (!s_fg.obj_rt && s_fg.prev_reg >= 0) {
        rd.Width = s_d3d.pw; rd.Height = s_d3d.ph;
        rd.Format = DXGI_FORMAT_R16G16_FLOAT;
        rd.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
        D3D12_DESCRIPTOR_HEAP_DESC hd = {0};
        hd.NumDescriptors = 1;
        hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        if (FAILED(dev->lpVtbl->CreateCommittedResource(dev, &hp, D3D12_HEAP_FLAG_NONE, &rd, read, NULL,
                &IID_ID3D12Resource, (void**)&s_fg.obj_rt)) ||
            FAILED(dev->lpVtbl->CreateDescriptorHeap(dev, &hd, &IID_ID3D12DescriptorHeap, (void**)&s_fg.obj_rtv)))
            return 0;
        hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
        if (FAILED(dev->lpVtbl->CreateDescriptorHeap(dev, &hd, &IID_ID3D12DescriptorHeap, (void**)&s_fg.obj_dsv)))
            return 0;
        D3D12_CPU_DESCRIPTOR_HANDLE h;
        s_fg.obj_rtv->lpVtbl->GetCPUDescriptorHandleForHeapStart(s_fg.obj_rtv, &h);
        dev->lpVtbl->CreateRenderTargetView(dev, s_fg.obj_rt, NULL, h);
        s_fg.obj_dsv->lpVtbl->GetCPUDescriptorHandleForHeapStart(s_fg.obj_dsv, &h);
        D3D12_DEPTH_STENCIL_VIEW_DESC dv;
        memset(&dv, 0, sizeof dv);
        dv.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
        dv.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
        dv.Flags = D3D12_DSV_FLAG_READ_ONLY_DEPTH | D3D12_DSV_FLAG_READ_ONLY_STENCIL;
        dev->lpVtbl->CreateDepthStencilView(dev, s_fg.depth, &dv, h);
    }
    const UINT inc = dev->lpVtbl->GetDescriptorHandleIncrementSize(dev, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    D3D12_CPU_DESCRIPTOR_HANDLE base;
    s_d3d.pp_srv_heap->lpVtbl->GetCPUDescriptorHandleForHeapStart(s_d3d.pp_srv_heap, &base);
    if (!s_fg.hud_src) {
        rd.Width = s_d3d.pw; rd.Height = s_d3d.ph;
        rd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        rd.Flags = D3D12_RESOURCE_FLAG_NONE;
        if (FAILED(dev->lpVtbl->CreateCommittedResource(dev, &hp, D3D12_HEAP_FLAG_NONE, &rd,
                D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, NULL, &IID_ID3D12Resource, (void**)&s_fg.hud_src)))
            return 0;
        D3D12_SHADER_RESOURCE_VIEW_DESC nd;
        memset(&nd, 0, sizeof nd);
        nd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        nd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        nd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        nd.Texture2D.MipLevels = 1;
        struct { u32 slot; ID3D12Resource* res; } v[] = {
            { PP_SLOT_HUD, s_fg.hud_src }, { PP_SLOT_HUD_E, s_fg.hud_src }, { PP_SLOT_HUD_E + 1, NULL }, { PP_SLOT_HUD_E + 2, NULL },
            { PP_SLOT_HUD_N, s_fg.hud_src }, { PP_SLOT_HUD_N + 1, s_d3d.aa_on ? s_d3d.aa_blend : NULL }, { PP_SLOT_HUD_N + 2, NULL },
        };
        for (unsigned i = 0; i < sizeof v / sizeof v[0]; i++) {
            D3D12_CPU_DESCRIPTOR_HANDLE h = base;
            h.ptr += v[i].slot * inc;
            dev->lpVtbl->CreateShaderResourceView(dev, v[i].res, v[i].res ? NULL : &nd, h);
        }
    }
    for (u32 i = 0; i < 2; i++) {
        if (s_fg.hud_out[i]) continue;
        rd.Width = s_d3d.bb_w; rd.Height = s_d3d.bb_h;
        rd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        rd.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
        if (s_up.want) rd.Flags |= D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        if (FAILED(dev->lpVtbl->CreateCommittedResource(dev, &hp, D3D12_HEAP_FLAG_NONE, &rd, read, NULL,
                &IID_ID3D12Resource, (void**)&s_fg.hud_out[i])))
            return 0;
        for (u32 fi = 0; s_up.want && fi < FRAME_COUNT; fi++) {
            ID3D12Resource* const t[3] = { s_fg.hud_out[i], s_d3d.render_targets[fi], s_fg.hud_src };
            for (u32 k = 0; k < 3; k++) {
                D3D12_CPU_DESCRIPTOR_HANDLE hv = base;
                hv.ptr += (PP_SLOT_UP(i, fi) + k) * inc;
                dev->lpVtbl->CreateShaderResourceView(dev, t[k], NULL, hv);
            }
        }
        D3D12_CPU_DESCRIPTOR_HANDLE h;
        s_d3d.bb_rtv_heap->lpVtbl->GetCPUDescriptorHandleForHeapStart(s_d3d.bb_rtv_heap, &h);
        h.ptr += (FRAME_COUNT + 1 + i) * s_d3d.rtv_descriptor_size;
        dev->lpVtbl->CreateRenderTargetView(dev, s_fg.hud_out[i], NULL, h);
    }
    return 1;
}

static void fg_copy_depth(int zi)
{
    ID3D12GraphicsCommandList* cl = s_d3d.cmd_list;
    const D3D12_RESOURCE_STATES zs = s_d3d.off_ds[zi].st;
    off_ds_transition(zi, D3D12_RESOURCE_STATE_COPY_SOURCE);
    pp_barrier(s_fg.depth, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
    cl->lpVtbl->CopyResource(cl, s_fg.depth, s_d3d.off_ds[zi].res);
    pp_barrier(s_fg.depth, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    off_ds_transition(zi, zs);
}

/* A clear of depth alone on a display surface, in the depth surface the scene drew with: the
 * picture is finished and the HUD comes next (God of War: Ascension, when it has a HUD to
 * draw), and the clear is about to wipe the scene's depth. Both are copied here. Nothing
 * else of the draw pass changes: the surfaces go back to the states they were in. */
static void fg_marker(const D3D12DrawRecord* dr)
{
    if (!FG_INPUTS() || !s_fg.cam_ok || dr->zeta_off != s_fg.depth_off) return;
    s_fg.marked = 1;
    const int ri = off_rt_find(dr->rt_off), zi = off_ds_find(dr->zeta_off);
    if (ri < 0 || zi < 0) return;
    OffRT* rt = &s_d3d.off_rt[ri];
    const OffDS* z = &s_d3d.off_ds[zi];
    if (rt->dxgi != DXGI_FORMAT_R8G8B8A8_UNORM || rt->w != s_d3d.width || rt->h != s_d3d.height ||
        z->w != s_d3d.width || z->h != s_d3d.height || !fg_targets())
        return;
    ID3D12GraphicsCommandList* cl = s_d3d.cmd_list;
    const D3D12_RESOURCE_STATES rs = rt->st;
    off_rt_transition(ri, D3D12_RESOURCE_STATE_COPY_SOURCE);
    pp_barrier(s_fg.hud_src, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
    cl->lpVtbl->CopyResource(cl, s_fg.hud_src, rt->res);
    pp_barrier(s_fg.hud_src, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    off_rt_transition(ri, rs);
    fg_copy_depth(zi);
    s_fg.copied = s_fg.depth_ok = 1;
}

/* A scene with no HUD to draw has no marker: its depth is whole at the frame's end (see
 * fg_objects), and the frame as it stands, before the overlay goes on, is the picture without
 * a HUD. */
static void fg_unmarked(ID3D12Resource* bb)
{
    if (!FG_INPUTS() || s_fg.marked || !s_fg.depth_ok) return;
    ID3D12GraphicsCommandList* cl = s_d3d.cmd_list;
    const D3D12_RESOURCE_STATES read = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    s_fg.hud_i ^= 1;
    ID3D12Resource* out = s_fg.hud_out[s_fg.hud_i];
    pp_barrier(bb, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
    pp_barrier(out, read, D3D12_RESOURCE_STATE_COPY_DEST);
    cl->lpVtbl->CopyResource(cl, out, bb);
    pp_barrier(out, D3D12_RESOURCE_STATE_COPY_DEST, read);
    pp_barrier(bb, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
    s_fg.ready = 1;
}

/* The depth surface at this zeta offset, made on first use at the size of the
 * pass that binds it. A larger pass on the same offset grows it (contents
 * lost); a smaller one uses it as it is, so passes of two sizes sharing one
 * depth surface do not recreate it every frame. With the pool full, an entry
 * no pass bound this frame makes way. */
static int off_ds_get(u32 off, u32 w, u32 h)
{
    if (!off || !w || !h) return off_ds_find(off);
    int i = off_ds_find(off);
    if (i >= 0) {
        OffDS* z = &s_d3d.off_ds[i];
        const int in_batch = z->last_frame == s_d3d.frame_count;   /* bound or sampled by this batch */
        if (!in_batch) {   /* the size a batch before asked for (grow_later) */
            if (z->grow_w > w) w = z->grow_w;
            if (z->grow_h > h) h = z->grow_h;
            z->grow_w = z->grow_h = 0;
        }
        z->last_frame = s_d3d.frame_count;
        if (w <= z->w && h <= z->h) return i;
        if (z->w > w) w = z->w;
        if (z->h > h) h = z->h;
        if (in_batch && grow_later()) {
            if (w > z->grow_w) z->grow_w = w;
            if (h > z->grow_h) z->grow_h = h;
            return i;
        }
    } else {
        for (i = 0; i < MAX_OFF_DS && s_d3d.off_ds[i].res; i++) {}
        if (i >= MAX_OFF_DS) {
            for (i = 0; i < MAX_OFF_DS && s_d3d.off_ds[i].last_frame == s_d3d.frame_count; i++) {}
            if (i >= MAX_OFF_DS) return -1;
        }
    }
    if (s_d3d.off_ds[i].res) {
        /* An earlier frame may still reference it (see off_rt_get). */
        wait_for_gpu();
        rf_release(s_d3d.off_ds[i].res);
        s_d3d.off_ds[i].res = NULL;
    }
    if (!s_d3d.ds_heap) {
        D3D12_DESCRIPTOR_HEAP_DESC hd = {0};
        hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
        hd.NumDescriptors = MAX_OFF_DS;
        if (FAILED(s_d3d.device->lpVtbl->CreateDescriptorHeap(
                s_d3d.device, &hd, &IID_ID3D12DescriptorHeap, (void**)&s_d3d.ds_heap)))
            return -1;
    }
    D3D12_HEAP_PROPERTIES hp = {0}; hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC td = {0};
    td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    const u32 pw = rs_class(w, h) ? s_d3d.pw : w, ph = rs_class(w, h) ? s_d3d.ph : h;
    td.Width = pw; td.Height = ph; td.DepthOrArraySize = 1; td.MipLevels = 1;
    td.Format = DXGI_FORMAT_R24G8_TYPELESS; td.SampleDesc.Count = 1;
    td.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
    D3D12_CLEAR_VALUE cv = {0};
    cv.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
    cv.DepthStencil.Depth = 1.0f;
    OffDS* z = &s_d3d.off_ds[i];
    if (FAILED(s_d3d.device->lpVtbl->CreateCommittedResource(
            s_d3d.device, &hp, D3D12_HEAP_FLAG_NONE, &td, D3D12_RESOURCE_STATE_DEPTH_WRITE, &cv,
            &IID_ID3D12Resource, (void**)&z->res)))
        return -1;
    z->off = off; z->w = w; z->h = h;
    z->pw = pw; z->ph = ph; z->sx = (float)pw / (float)w; z->sy = (float)ph / (float)h;
    z->grow_w = z->grow_h = 0;
    z->st = D3D12_RESOURCE_STATE_DEPTH_WRITE;
    z->cleared = 0; z->gen = 0; z->conv_gen = 0xFFFFFFFFu;
    z->last_frame = s_d3d.frame_count;
    D3D12_DEPTH_STENCIL_VIEW_DESC dv = {0};
    dv.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
    dv.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
    s_d3d.device->lpVtbl->CreateDepthStencilView(s_d3d.device, z->res, &dv, off_ds_dsv(i));
    D3D12_SHADER_RESOURCE_VIEW_DESC sv = {0};
    sv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    sv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    sv.Texture2D.MipLevels = 1;
    D3D12_CPU_DESCRIPTOR_HANDLE sh;
    s_d3d.srv_heap->lpVtbl->GetCPUDescriptorHandleForHeapStart(s_d3d.srv_heap, &sh);
    sh.ptr += (u64)(DS_SRV_BASE + i * 2) * s_d3d.srv_inc;
    sv.Format = DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
    s_d3d.device->lpVtbl->CreateShaderResourceView(s_d3d.device, z->res, &sv, sh);
    sh.ptr += s_d3d.srv_inc;
    sv.Format = DXGI_FORMAT_X24_TYPELESS_G8_UINT;
    sv.Texture2D.PlaneSlice = 1;
    s_d3d.device->lpVtbl->CreateShaderResourceView(s_d3d.device, z->res, &sv, sh);
    rdoc_name((ID3D12Object*)z->res, "ds 0x%08X %ux%u", off, w, h);
    return i;
}

/* Pipeline for the colour copy of a depth surface: what a guest sampling the
 * surface as A8R8G8B8 reads. The D24S8 word sits big-endian in memory, depth in
 * the top 24 bits, so the bytes are A = depth[23:16], R = depth[15:8],
 * G = depth[7:0], B = stencil. */
static int dsconv_init(void)
{
    if (s_d3d.dsconv_pso) return 1;
    static int failed = 0;
    if (failed) return 0;
    failed = 1;
    static const char vs[] =
        "float4 main(uint id : SV_VertexID) : SV_Position {\n"
        "    float2 p = float2((id << 1) & 2, id & 2);\n"
        "    return float4(p * float2(2, -2) + float2(-1, 1), 0, 1);\n"
        "}\n";
    static const char ps[] =
        "Texture2D<float> dz : register(t0);\n"
        "Texture2D<uint2> dst : register(t1);\n"
        "float4 main(float4 pos : SV_Position) : SV_Target {\n"
        "    int3 p = int3(pos.xy, 0);\n"
        "    uint d = (uint)round(saturate(dz.Load(p)) * 16777215.0);\n"
        "    uint s = dst.Load(p).y;\n"
        "    return float4((d >> 8) & 255u, d & 255u, s & 255u, d >> 16) / 255.0;\n"
        "}\n";
    ID3DBlob *vb = NULL, *pb = NULL, *err = NULL;
    if (FAILED(rsx_compile_own(vs, sizeof vs - 1, "dsconv_vs", NULL, NULL, "main", "vs_5_0", 0, 0, &vb, &err)) ||
        FAILED(rsx_compile_own(ps, sizeof ps - 1, "dsconv_ps", NULL, NULL, "main", "ps_5_0", 0, 0, &pb, &err))) {
        printf("[D3D12] depth copy shader: %s%c", err ? (const char*)err->lpVtbl->GetBufferPointer(err) : "?", 10);
        return 0;
    }
    D3D12_DESCRIPTOR_RANGE rg = {0};
    rg.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    rg.NumDescriptors = 2;
    D3D12_ROOT_PARAMETER rp = {0};
    rp.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    rp.DescriptorTable.NumDescriptorRanges = 1;
    rp.DescriptorTable.pDescriptorRanges = &rg;
    rp.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    D3D12_ROOT_SIGNATURE_DESC rd = {0};
    rd.NumParameters = 1; rd.pParameters = &rp;
    ID3DBlob* sig = NULL;
    if (FAILED(D3D12SerializeRootSignature(&rd, D3D_ROOT_SIGNATURE_VERSION_1, &sig, &err)) ||
        FAILED(s_d3d.device->lpVtbl->CreateRootSignature(s_d3d.device, 0, sig->lpVtbl->GetBufferPointer(sig),
               sig->lpVtbl->GetBufferSize(sig), &IID_ID3D12RootSignature, (void**)&s_d3d.dsconv_rs)))
        return 0;
    D3D12_GRAPHICS_PIPELINE_STATE_DESC pd = {0};
    pd.pRootSignature = s_d3d.dsconv_rs;
    pd.VS.pShaderBytecode = vb->lpVtbl->GetBufferPointer(vb);
    pd.VS.BytecodeLength  = vb->lpVtbl->GetBufferSize(vb);
    pd.PS.pShaderBytecode = pb->lpVtbl->GetBufferPointer(pb);
    pd.PS.BytecodeLength  = pb->lpVtbl->GetBufferSize(pb);
    pd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    pd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    pd.RasterizerState.DepthClipEnable = TRUE;
    pd.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    pd.SampleMask = UINT_MAX;
    pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pd.NumRenderTargets = 1;
    pd.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
    pd.SampleDesc.Count = 1;
    if (FAILED(s_d3d.device->lpVtbl->CreateGraphicsPipelineState(
            s_d3d.device, &pd, &IID_ID3D12PipelineState, (void**)&s_d3d.dsconv_pso)))
        return 0;
    failed = 0;
    return 1;
}

/* Refresh the colour copy (offscreen RT `rt`) of depth surface `i`. Leaves
 * the guest path's root signature, heaps and targets unbound. */
static void ds_convert(int i, int rt)
{
    if (rt < 0 || !dsconv_init()) return;
    ID3D12GraphicsCommandList* cl = s_d3d.cmd_list;
    OffRT* r = &s_d3d.off_rt[rt];
    if (r->sx != s_d3d.off_ds[i].sx || r->sy != s_d3d.off_ds[i].sy) {   /* the copy goes pixel for pixel */
        static int _r = 0;
        if (PS3_LOG_FIRST(_r, 4)) fprintf(stderr, "[ds-convert] 0x%X: its colour copy is drawn at another scale, not refreshed%c", r->off, 10);
        return;
    }
    off_ds_transition(i, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    off_rt_transition(rt, D3D12_RESOURCE_STATE_RENDER_TARGET);
    D3D12_CPU_DESCRIPTOR_HANDLE rh = off_rt_rtv(rt);
    cl->lpVtbl->OMSetRenderTargets(cl, 1, &rh, FALSE, NULL);
    D3D12_VIEWPORT vp = {0, 0, (float)r->pw, (float)r->ph, 0.0f, 1.0f};
    D3D12_RECT sc = {0, 0, (LONG)r->pw, (LONG)r->ph};
    cl->lpVtbl->RSSetViewports(cl, 1, &vp);
    cl->lpVtbl->RSSetScissorRects(cl, 1, &sc);
    cl->lpVtbl->SetGraphicsRootSignature(cl, s_d3d.dsconv_rs);
    cl->lpVtbl->SetPipelineState(cl, s_d3d.dsconv_pso);
    ID3D12DescriptorHeap* hp[] = { s_d3d.srv_heap };
    cl->lpVtbl->SetDescriptorHeaps(cl, 1, hp);
    D3D12_GPU_DESCRIPTOR_HANDLE gh;
    s_d3d.srv_heap->lpVtbl->GetGPUDescriptorHandleForHeapStart(s_d3d.srv_heap, &gh);
    gh.ptr += (u64)(DS_SRV_BASE + i * 2) * s_d3d.srv_inc;
    cl->lpVtbl->SetGraphicsRootDescriptorTable(cl, 0, gh);
    cl->lpVtbl->IASetPrimitiveTopology(cl, D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    cl->lpVtbl->DrawInstanced(cl, 3, 1, 0, 0);
    off_ds_transition(i, D3D12_RESOURCE_STATE_DEPTH_WRITE);
    s_d3d.off_ds[i].conv_gen = s_d3d.off_ds[i].gen;
}

/* RGBA8 target -> its Y16_X16_FLOAT reading. A8R8G8B8 sits in memory as the
 * bytes A, R, G, B (the host target holds R, G, B, A); read as Y16_X16 they
 * are the big-endian halves A:R and G:B, which the guest-texture decode puts
 * in R and G. */
static ID3D12RootSignature* s_y16_rs;
static ID3D12PipelineState* s_y16_pso;
static int y16_init(void)
{
    if (s_y16_pso) return 1;
    static int failed = 0;
    if (failed) return 0;
    failed = 1;
    static const char vs[] =
        "float4 main(uint id : SV_VertexID) : SV_Position {\n"
        "    float2 p = float2((id << 1) & 2, id & 2);\n"
        "    return float4(p * float2(2, -2) + float2(-1, 1), 0, 1);\n"
        "}\n";
    static const char ps[] =
        "Texture2D<float4> src : register(t0);\n"
        "cbuffer tex : register(b0) { uint2 tsize; float2 ratio; };\n"
        "float4 main(float4 pos : SV_Position) : SV_Target {\n"
        "    int2 q = min(int2(pos.xy * ratio), int2(tsize) - 1);\n"
        "    uint4 b = (uint4)round(saturate(src.Load(int3(q, 0))) * 255.0);\n"
        "    return float4(f16tof32((b.a << 8) | b.r), f16tof32((b.g << 8) | b.b), 0, 1);\n"
        "}\n";
    ID3DBlob *vb = NULL, *pb = NULL, *err = NULL;
    if (FAILED(rsx_compile_own(vs, sizeof vs - 1, "y16_vs", NULL, NULL, "main", "vs_5_0", 0, 0, &vb, &err)) ||
        FAILED(rsx_compile_own(ps, sizeof ps - 1, "y16_ps", NULL, NULL, "main", "ps_5_0", 0, 0, &pb, &err))) {
        printf("[D3D12] Y16 copy shader: %s%c", err ? (const char*)err->lpVtbl->GetBufferPointer(err) : "?", 10);
        return 0;
    }
    D3D12_DESCRIPTOR_RANGE rg = {0};
    rg.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    rg.NumDescriptors = 1;
    D3D12_ROOT_PARAMETER rp[2] = {0};
    rp[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    rp[0].DescriptorTable.NumDescriptorRanges = 1;
    rp[0].DescriptorTable.pDescriptorRanges = &rg;
    rp[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    rp[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;   /* b0 = the texture's size, this copy's pixels to its */
    rp[1].Constants.ShaderRegister = 0;
    rp[1].Constants.Num32BitValues = 4;
    rp[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    D3D12_ROOT_SIGNATURE_DESC rd = {0};
    rd.NumParameters = 2; rd.pParameters = rp;
    ID3DBlob* sig = NULL;
    if (FAILED(D3D12SerializeRootSignature(&rd, D3D_ROOT_SIGNATURE_VERSION_1, &sig, &err)) ||
        FAILED(s_d3d.device->lpVtbl->CreateRootSignature(s_d3d.device, 0, sig->lpVtbl->GetBufferPointer(sig),
               sig->lpVtbl->GetBufferSize(sig), &IID_ID3D12RootSignature, (void**)&s_y16_rs)))
        return 0;
    D3D12_GRAPHICS_PIPELINE_STATE_DESC pd = {0};
    pd.pRootSignature = s_y16_rs;
    pd.VS.pShaderBytecode = vb->lpVtbl->GetBufferPointer(vb);
    pd.VS.BytecodeLength  = vb->lpVtbl->GetBufferSize(vb);
    pd.PS.pShaderBytecode = pb->lpVtbl->GetBufferPointer(pb);
    pd.PS.BytecodeLength  = pb->lpVtbl->GetBufferSize(pb);
    pd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    pd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    pd.RasterizerState.DepthClipEnable = TRUE;
    pd.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    pd.SampleMask = UINT_MAX;
    pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pd.NumRenderTargets = 1;
    pd.RTVFormats[0] = DXGI_FORMAT_R16G16B16A16_FLOAT;
    pd.SampleDesc.Count = 1;
    if (FAILED(s_d3d.device->lpVtbl->CreateGraphicsPipelineState(
            s_d3d.device, &pd, &IID_ID3D12PipelineState, (void**)&s_y16_pso)))
        return 0;
    failed = 0;
    return 1;
}

/* Refresh target `dst` (R16G16B16A16_FLOAT) from RGBA8 target `src`, read as a
 * tw x th texture: past that size the edge texel repeats, so a filtered fetch
 * at the edge of a texture that is the corner of a larger target clamps there
 * as the RSX does, instead of pulling in what lies beyond it (God of War:
 * Ascension's luminance chain shrinks one surface to 8x9; the bytes around it
 * read as NaN halves). Leaves the guest path's root signature, heaps and
 * targets unbound. */
static void y16_convert(int src, int dst, u32 tw, u32 th)
{
    if (src < 0 || dst < 0 || !y16_init()) return;
    ID3D12GraphicsCommandList* cl = s_d3d.cmd_list;
    OffRT* r = &s_d3d.off_rt[dst];
    off_rt_transition(src, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    off_rt_transition(dst, D3D12_RESOURCE_STATE_RENDER_TARGET);
    D3D12_CPU_DESCRIPTOR_HANDLE rh = off_rt_rtv(dst);
    cl->lpVtbl->OMSetRenderTargets(cl, 1, &rh, FALSE, NULL);
    D3D12_VIEWPORT vp = {0, 0, (float)r->pw, (float)r->ph, 0.0f, 1.0f};
    D3D12_RECT sc = {0, 0, (LONG)r->pw, (LONG)r->ph};
    cl->lpVtbl->RSSetViewports(cl, 1, &vp);
    cl->lpVtbl->RSSetScissorRects(cl, 1, &sc);
    cl->lpVtbl->SetGraphicsRootSignature(cl, s_y16_rs);
    cl->lpVtbl->SetPipelineState(cl, s_y16_pso);
    ID3D12DescriptorHeap* hp[] = { s_d3d.srv_heap };
    cl->lpVtbl->SetDescriptorHeaps(cl, 1, hp);
    D3D12_GPU_DESCRIPTOR_HANDLE gh;
    s_d3d.srv_heap->lpVtbl->GetGPUDescriptorHandleForHeapStart(s_d3d.srv_heap, &gh);
    gh.ptr += (u64)(RT_SRV_BASE + src) * s_d3d.srv_inc;
    cl->lpVtbl->SetGraphicsRootDescriptorTable(cl, 0, gh);
    const OffRT* sr = &s_d3d.off_rt[src];
    const struct { u32 w, h; float x, y; } ts = {
        (u32)((float)(tw ? tw : r->w) * sr->sx + 0.5f), (u32)((float)(th ? th : r->h) * sr->sy + 0.5f),
        sr->sx / r->sx, sr->sy / r->sy };
    cl->lpVtbl->SetGraphicsRoot32BitConstants(cl, 1, 4, &ts, 0);
    cl->lpVtbl->IASetPrimitiveTopology(cl, D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    cl->lpVtbl->DrawInstanced(cl, 3, 1, 0, 0);
}

/* Set by the present entry point: 1 = this batch ends with a swapchain
 * Present, 0 = execute the recorded draws only. An OFFSCREEN-only batch (a
 * render-to-texture pass) must still run -- it is what fills the texture a
 * later batch samples -- but presenting it would show a half-built frame. */
static int s_present_this_frame = 1;

/* Resolve a DRAW_*_TEX value: a hex raw offset, or the literal "duck" for the
 * content-identified duck texture. */
static u32 draw_filter_tex(const char* e)
{
    if (!e) return 0;
    if (e[0] == 'd' && e[1] == 'u') return s_duck_raw;
    return (u32)strtoul(e, NULL, 16);
}

/* Blit snapshots. A title can copy a surface it has rendered into memory with
 * the 2D engine and sample the copy: God of War: Ascension copies each frame's
 * display buffer to main memory (for its SPU anti-aliasing, in 1024- and
 * 256-pixel-wide pieces), and later draws of the same frame sample that copy.
 * The frame exists only on the GPU, so the guest-memory copy moves stale
 * bytes, and the display-sized sampler fell back to s_screen_copy, the
 * previous final frame. A blit out of a display buffer records a snapshot op
 * at its place in the batch; the replay copies that buffer's target there
 * (RSX_DISPLAY_RT; else the backbuffer) into a texture keyed by the
 * destination address, and draws that sample that address bind it. What the
 * SPUs do to the guest copy is not seen. BLIT_SNAP=0 turns it off. */
#define BLIT_SNAPS 4
typedef struct { ID3D12Resource* res; u32 ea, w, h; u64 last_frame; } BlitSnap;
static BlitSnap s_bsnap[BLIT_SNAPS];

/* RSX_CAP: each blit snapshot as snap_<ea>.bmp next to the targets. */
static void cap_dump_snaps(u32 fi, const char* dir)
{
    char path[512];
    for (int i = 0; i < BLIT_SNAPS; i++) {
        if (!s_bsnap[i].res) continue;
        snprintf(path, sizeof path, "%s/snap_%08X.bmp", dir, s_bsnap[i].ea);
        cap_readback_write(s_bsnap[i].res, s_d3d.pw, s_d3d.ph, DXGI_FORMAT_R8G8B8A8_UNORM,
                           D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, fi, path);
    }
}

static int blit_snap_on(void)
{
    static int on = -1;
    if (on < 0) { const char* e = getenv("BLIT_SNAP"); on = e ? atoi(e) : 1; }
    return on;
}

/* cellGcm_blit_observer: record the op. The pieces of one copy (this title
 * splits each row at 1024 pixels) have the same base, so one snapshot of the
 * whole surface serves them. */
static void d3d12_blit_observed(int src_display, u32 src_off, u32 sx, u32 sy, u32 src_pitch,
                                u32 dst_ea, u32 dst_pitch, u32 w, u32 h)
{
    (void)src_off; (void)src_pitch;
    /* BLITSNAP_DBG=<frame>[:<n>]: every observed copy from that frame on. */
    { static int on = -1; static u32 from = 0, left = 0;
      if (on < 0) { const char* e = getenv("BLITSNAP_DBG");
                    on = e ? 1 : 0; left = 200;
                    if (e) sscanf(e, "%u:%u", &from, &left); }
      if (on && left && s_d3d.frame_count >= from) {
          left--;
          fprintf(stderr, "[blit-snap-dbg] f=%llu op=%u display 0x%X +(%u,%u) %ux%u pitch %u -> 0x%08X/%u%c",
                  (unsigned long long)s_d3d.frame_count, s_d3d.draw_count, src_off, sx, sy, w, h,
                  src_pitch, dst_ea, dst_pitch, 10);
      } }
    if (!blit_snap_on() || !src_display || !s_d3d.initialized) return;
    /* A copy into local memory lands in a target the GPU holds: the RT copy op
     * (d3d12_blit_local_observed) carries it there. A snapshot keyed by such an
     * address would also stand in for that target wherever it is sampled. */
    if (dst_ea >= 0xC0000000u && dst_ea < 0xD0000000u) return;
    /* With one backbuffer for every display buffer: only when the GPU has
     * drawn the display in this batch, since the backbuffer is cleared at the
     * start of the replay. With RSX_DISPLAY_RT the source buffer's own target
     * is copied, if it exists by then (prepass). */
    if (!display_rt_on()) {
        int drawn = 0;
        for (u32 i = 0; i < s_d3d.draw_count && i < MAX_DRAWS && !drawn; i++)
            drawn = s_d3d.draws[i].is_vp && !s_d3d.draws[i].is_clear && s_d3d.draws[i].rt_off == 0;
        if (!drawn) return;
    }
    const u32 base = dst_ea - sy * dst_pitch - sx * 4u;
    if (s_d3d.draw_count && s_d3d.draws[s_d3d.draw_count - 1].snap_ea == base) return;
    if (s_d3d.draw_count >= MAX_DRAWS) return;
    D3D12DrawRecord* dr = &s_d3d.draws[s_d3d.draw_count++];
    memset(dr, 0, sizeof(*dr));
    dr->is_vp = 1; dr->is_clear = 1; dr->tex_slot = -1;
    dr->snap_ea = base; dr->snap_slot = -1;
    dr->snap_src = display_rt_on() ? (DISP_RT_TAG | src_off) : 0u;
    s_d3d.merge_prev_draw = 0;
    s_ix_run_valid = 0;
    s_raw_run_valid = 0;
    { static int _n = 0;
      if (PS3_LOG_FIRST(_n, 4))
          fprintf(stderr, "[blit-snap] display 0x%X +(%u,%u) %ux%u -> 0x%08X, snapshot keyed 0x%08X%c",
                  src_off, sx, sy, w, h, dst_ea, base, 10); }
}

/* cellGcm_flip_observer: a flip named a display buffer at this point of the
 * stream. Presenting the flipped buffer's target at the end of the batch shows
 * whatever the batch drew into it after the flip too: once God of War:
 * Ascension's SPU work stopped holding it back, the drain passed several
 * flips per present and the screen showed a buffer whose next frame was half
 * drawn -- its scene without the post chain or the menu. Recorded as an op,
 * the replay copies the buffer to the backbuffer right there (the last flip of
 * the batch wins). FLIP_AT_OP=0: present the buffer at the batch's end. */
/* GUEST_PEEK=<ea>:<bytes>[:<every>]: at every <every>th flip (default 30), log
 * that many bytes of guest memory at <ea> (e.g. a render target the CPU reads).
 * GUEST_POKE=<ea>:<hex bytes>: write those bytes there at every flip, a lever for
 * testing what a CPU reader does with a value. Both are diagnostics. */
static void guest_peek_poke(void)
{
    extern uint8_t* vm_base;
    extern u32 gcm_io_offset_to_ea(u32 io);
    static int init = 0, peek_n = 0, peek_every = 30, poke_n = 0, peek_io = 0, poke_io = 0;
    static unsigned long peek_ea = 0, poke_ea = 0;
    static u8 poke_b[32];
    static u32 flips = 0;
    if (!init) {
        init = 1;
        const char* e = getenv("GUEST_PEEK");
        if (e) {
            char* end = NULL;
            peek_io = (e[0] == 'i' && e[1] == 'o' && e[2] == ':');
            peek_ea = strtoul(e + (peek_io ? 3 : 0), &end, 16);
            if (end && *end == ':') {
                peek_n = (int)strtoul(end + 1, &end, 0);
                if (end && *end == ':') peek_every = (int)strtoul(end + 1, NULL, 0);
            }
            if (peek_n > 64) peek_n = 64;
            if (peek_every < 1) peek_every = 1;
        }
        e = getenv("GUEST_POKE");
        if (e) {
            char* end = NULL;
            poke_io = (e[0] == 'i' && e[1] == 'o' && e[2] == ':');
            poke_ea = strtoul(e + (poke_io ? 3 : 0), &end, 16);
            if (end && *end == ':') {
                const char* h = end + 1;
                while (poke_n < (int)sizeof poke_b) {
                    int hi = -1, lo = -1;
                    char c0 = h[0], c1 = c0 ? h[1] : 0;
                    hi = (c0 >= '0' && c0 <= '9') ? c0 - '0' : (c0 >= 'a' && c0 <= 'f') ? c0 - 'a' + 10 :
                         (c0 >= 'A' && c0 <= 'F') ? c0 - 'A' + 10 : -1;
                    lo = (c1 >= '0' && c1 <= '9') ? c1 - '0' : (c1 >= 'a' && c1 <= 'f') ? c1 - 'a' + 10 :
                         (c1 >= 'A' && c1 <= 'F') ? c1 - 'A' + 10 : -1;
                    if (hi < 0 || lo < 0) break;
                    poke_b[poke_n++] = (u8)(hi * 16 + lo);
                    h += 2;
                }
            }
        }
    }
    if (!vm_base) return;
    flips++;
    if (peek_n > 0 && (flips % (u32)peek_every) == 0) {
        const unsigned long ea = peek_io ? gcm_io_offset_to_ea((u32)peek_ea) : peek_ea;
        if (ea) {
            char line[64 * 3 + 1];
            int k = 0;
            for (int i = 0; i < peek_n; i++) k += snprintf(line + k, sizeof line - (size_t)k, "%02X ", vm_base[ea + (unsigned long)i]);
            fprintf(stderr, "[GUEST_PEEK] flip %u %08lX: %s%c", flips, ea, line, 10);
        }
    }
    if (poke_n > 0) {
        const unsigned long ea = poke_io ? gcm_io_offset_to_ea((u32)poke_ea) : poke_ea;
        if (ea) memcpy(vm_base + ea, poke_b, (size_t)poke_n);
    }
}

/* FLIP_OP_DBG=1: what each flip op did (flip_op_note), and every present whose batch did not
 * carry exactly one, with its time since the process started (flip_op_present_note). */
static int flip_op_dbg(void)
{
    static int on = -1;
    if (on < 0) { const char* e = getenv("FLIP_OP_DBG"); on = e ? atoi(e) : 0; }
    return on;
}
static double flip_op_secs(void)
{
    static double t0 = -1.0;
    if (t0 < 0.0) {
        FILETIME cr, ex, kt, ut, nf;
        ULARGE_INTEGER a, b;
        GetProcessTimes(GetCurrentProcess(), &cr, &ex, &kt, &ut);
        GetSystemTimePreciseAsFileTime(&nf);
        a.LowPart = cr.dwLowDateTime; a.HighPart = cr.dwHighDateTime;
        b.LowPart = nf.dwLowDateTime; b.HighPart = nf.dwHighDateTime;
        t0 = perf_now() - (double)(b.QuadPart - a.QuadPart) / 1e7;
    }
    return perf_now() - t0;
}
static unsigned s_fo_batch, s_fo_dropped, s_fo_failed;

static void d3d12_flip_observed(u32 buffer_id)
{
    static int on = -1;
    s_wide_3d = 0;
    guest_peek_poke();
    if (on < 0) { const char* e = getenv("FLIP_AT_OP"); on = e ? atoi(e) : 1; }
    extern u32 cellGcm_display_buffer_offset(u32 id);
    if (on && display_rt_on() && s_d3d.initialized && s_d3d.draw_count >= MAX_DRAWS) {
        s_fo_dropped++;
        if (flip_op_dbg())
            fprintf(stderr, "[flip-op] %.2f s: buffer %u dropped, the batch is full (%u records)%c",
                    flip_op_secs(), buffer_id, s_d3d.draw_count, 10);
    }
    if (!on || !display_rt_on() || !s_d3d.initialized || s_d3d.draw_count >= MAX_DRAWS) return;
    const u32 o = cellGcm_display_buffer_offset(buffer_id);
    if (o == 0xFFFFFFFFu) return;
    D3D12DrawRecord* dr = &s_d3d.draws[s_d3d.draw_count++];
    memset(dr, 0, sizeof(*dr));
    dr->is_vp = 1; dr->is_clear = 1; dr->tex_slot = -1; dr->snap_slot = -1;
    dr->flip_key = DISP_RT_TAG | o;
    { static int cap = -1, n = 0;
      if (cap < 0) { const char* e = getenv("FLIP_PATH_DBG"); cap = e ? atoi(e) : 0; }
      if (n < cap) { n++; fprintf(stderr, "[flip-op] buffer %u at op %u%c", buffer_id,
                                  s_d3d.draw_count - 1, 10); } }
    s_d3d.merge_prev_draw = 0;
    s_ix_run_valid = 0;
    s_raw_run_valid = 0;
}

/* cellGcm_blit_local_observer: a copy between two places in local memory. God
 * of War: Ascension's menu copies a rectangle of the previous frame's display
 * buffer into its scene target, shifted, every frame (its drifting ink); with
 * only the guest-memory copy the target never saw it. Recorded at its place
 * in the batch and replayed between the targets that hold the two places
 * (BLIT_RT=0: off). */
static void d3d12_blit_local_observed(u32 src_off, u32 src_pitch, u32 dst_off, u32 dst_pitch,
                                      u32 w, u32 h)
{
    static int on = -1;
    if (on < 0) { const char* e = getenv("BLIT_RT"); on = e ? atoi(e) : 1; }
    if (!on || !s_d3d.initialized || !w || !h || s_d3d.draw_count >= MAX_DRAWS) return;
    D3D12DrawRecord* dr = &s_d3d.draws[s_d3d.draw_count++];
    memset(dr, 0, sizeof(*dr));
    dr->is_vp = 1; dr->is_clear = 1; dr->tex_slot = -1; dr->snap_slot = -1;
    dr->is_blit = 1;
    dr->blit_src = src_off; dr->blit_sp = src_pitch;
    dr->blit_dst = dst_off; dr->blit_dp = dst_pitch;
    dr->blit_w = w; dr->blit_h = h;
    s_d3d.merge_prev_draw = 0;
    s_ix_run_valid = 0;
    s_raw_run_valid = 0;
}

/* The target holding local byte offset `off` of a surface with `pitch` bytes a
 * row (RGBA8), and the pixel it is: display buffers by their own targets
 * (RSX_DISPLAY_RT), others by the offscreen target that contains the offset. */
static int blit_rt_find(u32 off, u32 pitch, u32* x, u32* y)
{
    extern int cellGcmOffsetIsDisplay(u32 offset);
    int best = -1; u32 bo = 0;
    if (!pitch) return -1;
    for (int i = 0; i < MAX_OFF_RTS; i++) {
        const OffRT* r = &s_d3d.off_rt[i];
        if (!r->res || r->dxgi != (u32)DXGI_FORMAT_R8G8B8A8_UNORM) continue;
        if (r->off & (Y16_RT_TAG)) continue;
        if ((r->off & DISP_RT_TAG) == 0 && cellGcmOffsetIsDisplay(r->off) && display_rt_on()) continue;
        const u32 base = r->off & ~DISP_RT_TAG;
        if (off < base) continue;
        const u32 rel = off - base, ry = rel / pitch, rx = (rel % pitch) / 4u;
        if (rx >= r->w || ry >= r->h) continue;
        if (best < 0 || base > bo) { best = i; bo = base; }   /* the closest start */
    }
    if (best >= 0) {
        const u32 rel = off - bo;
        *y = rel / pitch; *x = (rel % pitch) / 4u;
    }
    return best;
}

/* The op, in the VP pass. */
static void blit_rt_copy(const D3D12DrawRecord* dr)
{
    u32 sx, sy, dx, dy;
    const int s = blit_rt_find(dr->blit_src, dr->blit_sp, &sx, &sy);
    const int d = blit_rt_find(dr->blit_dst, dr->blit_dp, &dx, &dy);
    if (s < 0 || d < 0 || s == d) return;
    OffRT* rs = &s_d3d.off_rt[s];
    OffRT* rd = &s_d3d.off_rt[d];
    u32 w = dr->blit_w, h = dr->blit_h;
    if (sx + w > rs->w) w = rs->w - sx;
    if (sy + h > rs->h) h = rs->h - sy;
    if (dx + w > rd->w) w = rd->w - dx;
    if (dy + h > rd->h) h = rd->h - dy;
    if (!w || !h || w > 0x8000u || h > 0x8000u) return;
    if (rs->sx != rd->sx || rs->sy != rd->sy) {   /* a copy cannot scale */
        static int _r = 0;
        if (PS3_LOG_FIRST(_r, 4)) fprintf(stderr, "[blit-rt] 0x%X -> 0x%X: drawn at different scales, not copied%c", rs->off, rd->off, 10);
        return;
    }
    if (rs->sx != 1.0f || rs->sy != 1.0f) {
        /* By the edges. A position and a size rounded apart reach one pixel past the
         * target, and a box past its edge loses the device (DXGI_ERROR_INVALID_CALL). */
        const float fx = rs->sx, fy = rs->sy;
        u32 x1 = rs_px(sx + w, fx), y1 = rs_px(sy + h, fy), x2 = rs_px(dx + w, fx), y2 = rs_px(dy + h, fy);
        sx = rs_px(sx, fx); sy = rs_px(sy, fy);
        dx = rs_px(dx, fx); dy = rs_px(dy, fy);
        if (x1 > rs->pw) x1 = rs->pw;
        if (y1 > rs->ph) y1 = rs->ph;
        if (x2 > rd->pw) x2 = rd->pw;
        if (y2 > rd->ph) y2 = rd->ph;
        w = x1 - sx < x2 - dx ? x1 - sx : x2 - dx;
        h = y1 - sy < y2 - dy ? y1 - sy : y2 - dy;
        if (!w || !h) return;
    }
    const D3D12_RESOURCE_STATES ss = rs->st, ds = rd->st;
    off_rt_transition(s, D3D12_RESOURCE_STATE_COPY_SOURCE);
    off_rt_transition(d, D3D12_RESOURCE_STATE_COPY_DEST);
    D3D12_TEXTURE_COPY_LOCATION dl = {0}, sl = {0};
    dl.pResource = rd->res; dl.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    sl.pResource = rs->res; sl.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    D3D12_BOX box = {0};
    box.left = sx; box.top = sy; box.front = 0;
    box.right = sx + w; box.bottom = sy + h; box.back = 1;
    s_d3d.cmd_list->lpVtbl->CopyTextureRegion(s_d3d.cmd_list, &dl, dx, dy, 0, &sl, &box);
    off_rt_transition(s, ss);
    off_rt_transition(d, ds);
    { static int _n = 0;
      if (PS3_LOG_FIRST(_n, 4))
          fprintf(stderr, "[blit-rt] 0x%X (%u,%u) -> 0x%X (%u,%u) %ux%u%c",
                  rs->off, sx, sy, rd->off, dx, dy, w, h, 10); }
}

/* The snapshot resource for ea, made in the prepass at the backbuffer's size. */
static int blit_snap_get(u32 ea)
{
    int slot = -1;
    for (int i = 0; i < BLIT_SNAPS; i++)
        if (s_bsnap[i].res && s_bsnap[i].ea == ea) { slot = i; break; }
    if (slot >= 0 && (s_bsnap[slot].w != s_d3d.width || s_bsnap[slot].h != s_d3d.height)) {
        wait_for_gpu();
        rf_release(s_bsnap[slot].res);
        s_bsnap[slot].res = NULL;
    }
    if (slot < 0) {
        for (int i = 0; i < BLIT_SNAPS; i++)
            if (!s_bsnap[i].res) { slot = i; break; }
        if (slot < 0) {   /* the least recently used */
            slot = 0;
            for (int i = 1; i < BLIT_SNAPS; i++)
                if (s_bsnap[i].last_frame < s_bsnap[slot].last_frame) slot = i;
            wait_for_gpu();
            rf_release(s_bsnap[slot].res);
            s_bsnap[slot].res = NULL;
        }
    }
    BlitSnap* b = &s_bsnap[slot];
    if (!b->res) {
        D3D12_HEAP_PROPERTIES hp = {0}; hp.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC td = {0};
        td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        td.Width = s_d3d.pw; td.Height = s_d3d.ph;
        td.DepthOrArraySize = 1; td.MipLevels = 1;
        td.Format = DXGI_FORMAT_R8G8B8A8_UNORM; td.SampleDesc.Count = 1;
        td.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        if (FAILED(s_d3d.device->lpVtbl->CreateCommittedResource(
                s_d3d.device, &hp, D3D12_HEAP_FLAG_NONE, &td,
                D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, NULL,
                &IID_ID3D12Resource, (void**)&b->res))) {
            b->res = NULL;
            return -1;
        }
        b->w = s_d3d.width; b->h = s_d3d.height;
    }
    b->ea = ea;
    b->last_frame = s_d3d.frame_count;
    return slot;
}

/* A texture at a snapshot's address and size, taken in this batch or the one
 * before: God of War: Ascension's menu copies the display at the end of the
 * batch that drew its scene and reads the copy at the start of the next one.
 * A frame that copies from a display the CPU filled, and has not done so for
 * longer than that, reads the guest copy instead. */
static int blit_snap_find(u32 ea, u32 w, u32 h)
{
    if (!ea) return -1;
    for (int i = 0; i < BLIT_SNAPS; i++)
        if (s_bsnap[i].res && s_bsnap[i].ea == ea && s_bsnap[i].w == w && s_bsnap[i].h == h &&
            s_bsnap[i].last_frame + 1 >= s_d3d.frame_count)
            return i;
    return -1;
}

/* The op, in the VP pass: the backbuffer as drawn so far into the snapshot. */
static void blit_snap_take(const D3D12DrawRecord* dr, u32 fi)
{
    const int i = dr->snap_slot;
    if (i < 0 || !s_bsnap[i].res) return;
    ID3D12Resource* src = s_d3d.render_targets[fi];
    D3D12_RESOURCE_STATES src_st = D3D12_RESOURCE_STATE_RENDER_TARGET;
    if (dr->snap_src) {
        const int r = off_rt_find(dr->snap_src);
        if (r < 0 || !s_d3d.off_rt[r].res || s_d3d.off_rt[r].w != s_bsnap[i].w ||
            s_d3d.off_rt[r].h != s_bsnap[i].h || s_d3d.off_rt[r].pw != s_d3d.pw ||
            s_d3d.off_rt[r].dxgi != (u32)DXGI_FORMAT_R8G8B8A8_UNORM)
            return;
        src = s_d3d.off_rt[r].res;
        src_st = s_d3d.off_rt[r].st;
    }
    D3D12_RESOURCE_BARRIER bb[2] = {0};
    bb[0].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    bb[0].Transition.pResource   = src;
    bb[0].Transition.StateBefore = src_st;
    bb[0].Transition.StateAfter  = D3D12_RESOURCE_STATE_COPY_SOURCE;
    bb[0].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    bb[1] = bb[0];
    bb[1].Transition.pResource   = s_bsnap[i].res;
    bb[1].Transition.StateBefore = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    bb[1].Transition.StateAfter  = D3D12_RESOURCE_STATE_COPY_DEST;
    s_d3d.cmd_list->lpVtbl->ResourceBarrier(s_d3d.cmd_list, 2, bb);
    s_d3d.cmd_list->lpVtbl->CopyResource(s_d3d.cmd_list, s_bsnap[i].res, src);
    bb[0].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
    bb[0].Transition.StateAfter  = src_st;
    bb[1].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
    bb[1].Transition.StateAfter  = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    s_d3d.cmd_list->lpVtbl->ResourceBarrier(s_d3d.cmd_list, 2, bb);
}

/* Copy the current backbuffer into s_screen_copy. Called mid-frame the moment
 * the reduced-viewport passes finish -- this title renders its reflection into
 * the TOP-LEFT REGION of the render target and the main scene then overwrites
 * it, so a snapshot taken at end of frame contains the scene, not the
 * reflection. Also called at end of frame as a fallback for frames with no
 * reduced-viewport pass. */
static void screen_copy_capture(u32 fi)
{
    static int en = -1;
    if (en < 0) { const char* e = getenv("SCREEN_AS_TEX"); en = e ? atoi(e) : 1; }
    if (!en || !s_d3d.device) return;
            if (!s_screen_copy) {
                D3D12_HEAP_PROPERTIES hp = {0}; hp.Type = D3D12_HEAP_TYPE_DEFAULT;
                D3D12_RESOURCE_DESC td = {0};
                td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
                td.Width = s_d3d.pw; td.Height = s_d3d.ph;
                td.DepthOrArraySize = 1; td.MipLevels = 1;
                td.Format = DXGI_FORMAT_R8G8B8A8_UNORM; td.SampleDesc.Count = 1;
                td.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
                if (FAILED(s_d3d.device->lpVtbl->CreateCommittedResource(
                        s_d3d.device, &hp, D3D12_HEAP_FLAG_NONE, &td,
                        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, NULL,
                        &IID_ID3D12Resource, (void**)&s_screen_copy)))
                    s_screen_copy = NULL;
            }
            if (s_screen_copy) {
                D3D12_RESOURCE_BARRIER bb[2] = {0};
                bb[0].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                bb[0].Transition.pResource   = s_d3d.render_targets[fi];
                bb[0].Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
                bb[0].Transition.StateAfter  = D3D12_RESOURCE_STATE_COPY_SOURCE;
                bb[0].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
                bb[1] = bb[0];
                bb[1].Transition.pResource   = s_screen_copy;
                bb[1].Transition.StateBefore = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
                bb[1].Transition.StateAfter  = D3D12_RESOURCE_STATE_COPY_DEST;
                s_d3d.cmd_list->lpVtbl->ResourceBarrier(s_d3d.cmd_list, 2, bb);

                s_d3d.cmd_list->lpVtbl->CopyResource(s_d3d.cmd_list,
                    s_screen_copy, s_d3d.render_targets[fi]);

                bb[0].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
                bb[0].Transition.StateAfter  = D3D12_RESOURCE_STATE_RENDER_TARGET;
                bb[1].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
                bb[1].Transition.StateAfter  = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
                s_d3d.cmd_list->lpVtbl->ResourceBarrier(s_d3d.cmd_list, 2, bb);

                /* SCREENCOPY_DUMP=1: stage the captured image into the readback
                 * buffer so it can be written out and LOOKED AT. Claiming a
                 * capture happened is not the same as showing what is in it. */
                { static int sd = -1;
                  if (sd < 0) { const char* e = getenv("SCREENCOPY_DUMP"); sd = e ? atoi(e) : 0; }
                  if (sd && s_d3d.readback_buf) {
                      D3D12_RESOURCE_BARRIER t = {0};
                      t.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                      t.Transition.pResource   = s_screen_copy;
                      t.Transition.StateBefore = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
                      t.Transition.StateAfter  = D3D12_RESOURCE_STATE_COPY_SOURCE;
                      t.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
                      s_d3d.cmd_list->lpVtbl->ResourceBarrier(s_d3d.cmd_list, 1, &t);

                      D3D12_TEXTURE_COPY_LOCATION cd = {0}, cs = {0};
                      cd.pResource = s_d3d.readback_buf;
                      cd.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
                      cd.PlacedFootprint.Footprint.Format   = DXGI_FORMAT_R8G8B8A8_UNORM;
                      cd.PlacedFootprint.Footprint.Width    = s_d3d.pw;
                      cd.PlacedFootprint.Footprint.Height   = s_d3d.ph;
                      cd.PlacedFootprint.Footprint.Depth    = 1;
                      cd.PlacedFootprint.Footprint.RowPitch = s_d3d.readback_pitch;
                      cs.pResource = s_screen_copy;
                      cs.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
                      s_d3d.cmd_list->lpVtbl->CopyTextureRegion(s_d3d.cmd_list, &cd, 0, 0, 0, &cs, NULL);

                      t.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
                      t.Transition.StateAfter  = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
                      s_d3d.cmd_list->lpVtbl->ResourceBarrier(s_d3d.cmd_list, 1, &t);
                      s_sc_dump_pending = 1;
                  } }
            }

}

/* rsx_topology -> D3D_PRIMITIVE_TOPOLOGY. Spelled out rather than relying on
 * the two enums happening to agree: the draw record carries the neutral value
 * so a second backend can read it, and this is the one place it becomes a D3D
 * one. */
static D3D12_PRIMITIVE_TOPOLOGY topo_to_d3d(u32 t)
{
    switch (t) {
    case RSX_TOPOLOGY_POINTS:         return D3D_PRIMITIVE_TOPOLOGY_POINTLIST;
    case RSX_TOPOLOGY_LINES:          return D3D_PRIMITIVE_TOPOLOGY_LINELIST;
    case RSX_TOPOLOGY_LINE_STRIP:     return D3D_PRIMITIVE_TOPOLOGY_LINESTRIP;
    case RSX_TOPOLOGY_TRIANGLE_STRIP: return D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP;
    case RSX_TOPOLOGY_TRIANGLES:      return D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
    default:                          return D3D_PRIMITIVE_TOPOLOGY_UNDEFINED;
    }
}

/* GCM_GUEST_FB=<off>,<w>,<h>,<pitch> -- present the guest display buffer.
 *
 * A title that composites its final image with the RSX 2D engine assembles it
 * in GUEST memory: nv3089_blit is a CPU copy and never touches D3D12, and the
 * draw pass below only runs when a recorded draw targets the display surface,
 * which for such a title never happens. The frame is finished, in the wrong
 * memory, and the swapchain shows only the clear. Copy it across.
 *
 * Off unless the offset and geometry are given, so nothing else changes. */
/* GPU-side composite of an offscreen RT onto the backbuffer.
 *
 * A title that finishes its frame with the RSX 2D engine blits a rendered
 * surface into the display buffer. That surface lives in guest RAM, which the
 * D3D12 path never writes -- the scene is on the GPU -- so the CPU copy in
 * nv3089_blit faithfully moves zeros. The backend already holds the same
 * surface as a texture, so do the composite here instead. Triggered from
 * cellGcmSys under GCM_COMPOSITE_RT. */
static u32 s_composite_src = 0;
static int s_last_drawn_slot = -1;
static u32 s_last_drawn_off  = 0;

void rsx_d3d12_composite_from_offscreen(u32 raw_off) { s_composite_src = raw_off; }

static int s_flip_shown;   /* a flip op already put this batch's frame on the backbuffer */
static void composite_copy(u32 fi, int i, u32 want);

static void composite_present(u32 fi)
{
    u32 want = s_composite_src;
    s_composite_src = 0;
    if (s_flip_shown) { s_flip_shown = 0; return; }
    if (!want) return;
    /* want == 2 means "whatever this frame's draws targeted" -- the one thing
     * guaranteed to hold geometry, and immune to the offset collision above. */
    int i;
    if (want == 2u) { i = s_last_drawn_slot; want = s_last_drawn_off; }
    else            { i = off_rt_find(want); }
    composite_copy(fi, i, want);
}

/* Copy offscreen target i (key want, for the log) to frame fi's backbuffer,
 * which is a render target in the frame's list. */
static void composite_copy(u32 fi, int i, u32 want)
{
    if (i < 0 || !s_d3d.off_rt[i].res) return;
    OffRT* r = &s_d3d.off_rt[i];
    /* Same-format copy only; a half-float source would need a converting draw. */
    if (r->dxgi != DXGI_FORMAT_R8G8B8A8_UNORM) {
        static int w1 = 0;
        if (PS3_LOG_FIRST(w1, 2))
            fprintf(stderr, "[COMPOSITE] RT 0x%08X is dxgi=%u, not R8G8B8A8 -- needs a draw%c",
                    want, r->dxgi, 10);
        return;
    }
    u32 cw = r->pw < s_d3d.pw ? r->pw : s_d3d.pw;
    u32 ch = r->ph < s_d3d.ph ? r->ph : s_d3d.ph;
    if (!cw || !ch) return;
    { static int n = 0;
      if (PS3_LOG_FIRST(n, 3)) fprintf(stderr, "[COMPOSITE] RT 0x%08X %ux%u -> backbuffer%c",
                           want, cw, ch, 10); }

    D3D12_RESOURCE_BARRIER b[2] = {0};
    b[0].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b[0].Transition.pResource   = r->res;
    b[0].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b[0].Transition.StateBefore = r->st;
    b[0].Transition.StateAfter  = D3D12_RESOURCE_STATE_COPY_SOURCE;
    b[1] = b[0];
    b[1].Transition.pResource   = s_d3d.render_targets[fi];
    b[1].Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
    b[1].Transition.StateAfter  = D3D12_RESOURCE_STATE_COPY_DEST;
    s_d3d.cmd_list->lpVtbl->ResourceBarrier(s_d3d.cmd_list, 2, b);

    D3D12_TEXTURE_COPY_LOCATION dl = {0}, sl = {0};
    dl.pResource = s_d3d.render_targets[fi];
    dl.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX; dl.SubresourceIndex = 0;
    sl.pResource = r->res;
    sl.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX; sl.SubresourceIndex = 0;
    D3D12_BOX box = {0};
    box.left = 0; box.top = 0; box.front = 0;
    box.right = cw; box.bottom = ch; box.back = 1;
    s_d3d.cmd_list->lpVtbl->CopyTextureRegion(s_d3d.cmd_list, &dl, 0, 0, 0, &sl, &box);

    b[0].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
    b[0].Transition.StateAfter  = r->st;
    b[1].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
    b[1].Transition.StateAfter  = D3D12_RESOURCE_STATE_RENDER_TARGET;
    s_d3d.cmd_list->lpVtbl->ResourceBarrier(s_d3d.cmd_list, 2, b);
}


static void guest_fb_present(u32 fi)
{
    static int inited = 0;
    static u32 off, gw, gh, gpitch, up_pitch;
    static ID3D12Resource* up = NULL;
    if (!inited) {
        inited = 1;
        const char* e = getenv("GCM_GUEST_FB");
        unsigned a = 0, b = 0, c = 0, d = 0;
        if (!e || sscanf(e, "%i,%u,%u,%u", &a, &b, &c, &d) != 4 || !b || !c) return;
        off = a; gw = b; gh = c; gpitch = d;
        up_pitch = (gw * 4u + 255u) & ~255u;      /* D3D12 wants 256-byte rows */
        D3D12_HEAP_PROPERTIES hp = {0}; hp.Type = D3D12_HEAP_TYPE_UPLOAD;
        D3D12_RESOURCE_DESC bd = {0};
        bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        bd.Width  = (u64)up_pitch * gh; bd.Height = 1;
        bd.DepthOrArraySize = 1; bd.MipLevels = 1;
        bd.Format = DXGI_FORMAT_UNKNOWN; bd.SampleDesc.Count = 1;
        bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        if (FAILED(s_d3d.device->lpVtbl->CreateCommittedResource(
                s_d3d.device, &hp, D3D12_HEAP_FLAG_NONE, &bd,
                D3D12_RESOURCE_STATE_GENERIC_READ, NULL,
                &IID_ID3D12Resource, (void**)&up)))
            up = NULL;
        fprintf(stderr, "[GUESTFB] presenting guest 0x%08X %ux%u pitch %u -> backbuffer%c",
                off, gw, gh, gpitch, 10);
    }
    if (!up) return;
    { extern u8* vm_base;
      if (!vm_base) return;
      /* RSX local memory is mapped at guest 0xC0000000. */
      const u8* srow = vm_base + 0xC0000000u + off;
      void* m = NULL; D3D12_RANGE nr = {0, 0};
      if (FAILED(up->lpVtbl->Map(up, 0, &nr, &m)) || !m) return;
      for (u32 y = 0; y < gh; y++) {
          const u8* s = srow + (size_t)y * gpitch;
          u8* dp = (u8*)m + (size_t)y * up_pitch;
          for (u32 x = 0; x < gw; x++) {   /* guest B,G,R,A -> backbuffer R,G,B,A */
              dp[x*4+0] = s[x*4+2]; dp[x*4+1] = s[x*4+1];
              dp[x*4+2] = s[x*4+0]; dp[x*4+3] = 0xFF;
          }
      }
      D3D12_RANGE wr = {0, (SIZE_T)up_pitch * gh};
      up->lpVtbl->Unmap(up, 0, &wr); }

    D3D12_RESOURCE_BARRIER gb = {0};
    gb.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    gb.Transition.pResource   = s_d3d.render_targets[fi];
    gb.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    gb.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
    gb.Transition.StateAfter  = D3D12_RESOURCE_STATE_COPY_DEST;
    s_d3d.cmd_list->lpVtbl->ResourceBarrier(s_d3d.cmd_list, 1, &gb);

    D3D12_TEXTURE_COPY_LOCATION dl = {0}, sl = {0};
    dl.pResource = s_d3d.render_targets[fi];
    dl.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX; dl.SubresourceIndex = 0;
    sl.pResource = up; sl.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    sl.PlacedFootprint.Offset = 0;
    sl.PlacedFootprint.Footprint.Format   = DXGI_FORMAT_R8G8B8A8_UNORM;
    sl.PlacedFootprint.Footprint.Width    = gw;
    sl.PlacedFootprint.Footprint.Height   = gh;
    sl.PlacedFootprint.Footprint.Depth    = 1;
    sl.PlacedFootprint.Footprint.RowPitch = up_pitch;
    s_d3d.cmd_list->lpVtbl->CopyTextureRegion(s_d3d.cmd_list, &dl, 0, 0, 0, &sl, NULL);

    gb.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
    gb.Transition.StateAfter  = D3D12_RESOURCE_STATE_RENDER_TARGET;
    s_d3d.cmd_list->lpVtbl->ResourceBarrier(s_d3d.cmd_list, 1, &gb);
}


/* ---------------------------------------------------------------------------
 * render_frame() in stages
 *
 * A batch is recorded into the frame's command list in one go at its end (a
 * display clear, or a flip): open the list, the RT and texture prepass over
 * every record, the display target, the legacy pass, then the VP pass. In God
 * of War HD that is about 480 draws and 2 ms, on the drain thread, while the
 * title waits for the drain to reach its flip -- and the same thread sat idle
 * for most of the frame before.
 *
 * D3D12_RECORD_AHEAD=1 records the batch as it arrives instead:
 * rsx_d3d12_backend_record_ahead(), called by the host between drain passes,
 * opens the list and records the records captured so far; the end of the
 * batch records the rest, executes and presents. Same thread, same order, so
 * the guest memory a texture upload reads is read earlier, never later. Not
 * the last record (a batch can still extend it), not a batch with legacy
 * (non-VP) records, and not while a debug filter that edits the batch is set.
 * -------------------------------------------------------------------------*/
/* RSX_CENSUS: GPU timestamps at a frame's start, after its prepass (the
 * texture copies), after its draws and at its end, read back after the next
 * frame's wait for the GPU: a long wait at a frame's start is the frame
 * before it on the GPU. */
static ID3D12QueryHeap* s_gts_heap;
static ID3D12Resource* s_gts_rb;
static u64 s_gts_freq;
static int s_gts_slot = -1, s_gts_marked;
/* And one at every render target change of the draw pass: a frame whose draws
 * took over 50 ms on the GPU prints its longest passes. */
#define GPS_MAX 256
typedef struct { u32 from, off, w, h, fp, fbc, inval; } GpsPass;
static u32 s_fb_copies;                 /* RT_FEEDBACK copies made, for the passes */
static ID3D12QueryHeap* s_gps_heap;
static ID3D12Resource* s_gps_rb;
static GpsPass s_gps[FRAME_COUNT][GPS_MAX];
static int s_gps_n[FRAME_COUNT];        /* timestamps of the frame on index fi */
static int s_gps_rd_slot = -1, s_gps_rd_n;
static int gts_on(void)
{
    static int v = -1;
    if (v >= 0) return v;
    v = 0;
    if (!(rsx_census_secs() || ovs_on()) || !s_d3d.device || !s_d3d.cmd_queue) return v;
    D3D12_QUERY_HEAP_DESC qd;
    memset(&qd, 0, sizeof qd);
    qd.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
    qd.Count = 4 * FRAME_COUNT;
    D3D12_HEAP_PROPERTIES hp = {0}; hp.Type = D3D12_HEAP_TYPE_READBACK;
    D3D12_RESOURCE_DESC bd = {0};
    bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; bd.Width = 32 * FRAME_COUNT;
    bd.Height = 1; bd.DepthOrArraySize = 1; bd.MipLevels = 1; bd.SampleDesc.Count = 1;
    bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    if (SUCCEEDED(s_d3d.device->lpVtbl->CreateQueryHeap(s_d3d.device, &qd, &IID_ID3D12QueryHeap, (void**)&s_gts_heap)) &&
        SUCCEEDED(s_d3d.device->lpVtbl->CreateCommittedResource(s_d3d.device, &hp, D3D12_HEAP_FLAG_NONE, &bd,
            D3D12_RESOURCE_STATE_COPY_DEST, NULL, &IID_ID3D12Resource, (void**)&s_gts_rb)) &&
        SUCCEEDED(s_d3d.cmd_queue->lpVtbl->GetTimestampFrequency(s_d3d.cmd_queue, &s_gts_freq)) && s_gts_freq)
        v = 1;
    if (v && rsx_census_secs()) {
        qd.Count = GPS_MAX * FRAME_COUNT;
        bd.Width = 8ull * GPS_MAX * FRAME_COUNT;
        if (FAILED(s_d3d.device->lpVtbl->CreateQueryHeap(s_d3d.device, &qd, &IID_ID3D12QueryHeap, (void**)&s_gps_heap)) ||
            FAILED(s_d3d.device->lpVtbl->CreateCommittedResource(s_d3d.device, &hp, D3D12_HEAP_FLAG_NONE, &bd,
                D3D12_RESOURCE_STATE_COPY_DEST, NULL, &IID_ID3D12Resource, (void**)&s_gps_rb)))
            s_gps_heap = NULL;
    }
    return v;
}

/* A pass begins at draw `from`, into the target at `off` (0: the display),
 * w x h, its first draw's fragment program fp. */
static void gps_pass(u32 from, u32 off, u32 w, u32 h, u32 fp)
{
    if (!gts_on() || !s_gps_heap) return;
    const u32 fi = s_rf.fi;
    if (s_gps_n[fi] >= GPS_MAX - 1) return;     /* the last one ends the passes */
    GpsPass* p = &s_gps[fi][s_gps_n[fi]];
    p->from = from; p->off = off; p->w = w; p->h = h; p->fp = fp;
    p->fbc = s_fb_copies;
    p->inval = 0;
    /* The pass before this one: how often the title invalidated the texture
     * cache between its VP draws (the records still hold this frame). */
    if (s_gps_n[fi] > 0) {
        GpsPass* q = &s_gps[fi][s_gps_n[fi] - 1];
        int have = 0; u32 last = 0;
        for (u32 i = q->from; i < from && i < s_d3d.draw_count && i < MAX_DRAWS; i++) {
            const D3D12DrawRecord* r = &s_d3d.draws[i];
            if (!r->is_vp || r->is_clear) continue;
            if (have && r->l2_inval != last) q->inval++;
            last = r->l2_inval; have = 1;
        }
    }
    s_d3d.cmd_list->lpVtbl->EndQuery(s_d3d.cmd_list, s_gps_heap, D3D12_QUERY_TYPE_TIMESTAMP,
                                     (UINT)(fi * GPS_MAX + s_gps_n[fi]));
    s_gps_n[fi]++;
}
static void gps_end(u32 draws)
{
    if (!gts_on() || !s_gps_heap || !s_gps_n[s_rf.fi]) return;
    gps_pass(draws, 0, 0, 0, 0);
}
static void gts_mark(int k)
{
    if (!gts_on()) return;
    s_d3d.cmd_list->lpVtbl->EndQuery(s_d3d.cmd_list, s_gts_heap, D3D12_QUERY_TYPE_TIMESTAMP, (UINT)(s_rf.fi * 4 + k));
    if (k == 0) { s_gts_marked = 1; s_gps_n[s_rf.fi] = 0; }
}
static void gts_resolve(void)
{
    if (!gts_on() || !s_gts_marked) return;
    gts_mark(3);
    const UINT base = (UINT)s_rf.fi * 4;
    s_d3d.cmd_list->lpVtbl->ResolveQueryData(s_d3d.cmd_list, s_gts_heap, D3D12_QUERY_TYPE_TIMESTAMP, base, 4,
                                             s_gts_rb, (UINT64)base * 8);
    s_gts_slot = (int)s_rf.fi;
    s_gts_marked = 0;
    s_gps_rd_slot = -1;
    if (s_gps_heap && s_gps_n[s_rf.fi] >= 2) {
        s_d3d.cmd_list->lpVtbl->ResolveQueryData(s_d3d.cmd_list, s_gps_heap, D3D12_QUERY_TYPE_TIMESTAMP,
                                                 (UINT)s_rf.fi * GPS_MAX, (UINT)s_gps_n[s_rf.fi], s_gps_rb,
                                                 8ull * GPS_MAX * s_rf.fi);
        s_gps_rd_slot = (int)s_rf.fi;
        s_gps_rd_n = s_gps_n[s_rf.fi];
    }
}
static void gts_read(void)
{
    if (!gts_on() || s_gts_slot < 0) return;
    void* p = NULL;
    D3D12_RANGE rr = { (SIZE_T)s_gts_slot * 32, (SIZE_T)s_gts_slot * 32 + 32 };
    if (FAILED(s_gts_rb->lpVtbl->Map(s_gts_rb, 0, &rr, &p)) || !p) return;
    u64 t[4];
    memcpy(t, (const u8*)p + s_gts_slot * 32, sizeof t);
    D3D12_RANGE wr = {0, 0};
    s_gts_rb->lpVtbl->Unmap(s_gts_rb, 0, &wr);
    s_gts_slot = -1;
    if (t[1] < t[0] || t[2] < t[1] || t[3] < t[2]) return;
    const double k = 1000.0 / (double)s_gts_freq;
    s_hl_gpu_ms[0] = (double)(t[1] - t[0]) * k;
    s_hl_gpu_ms[1] = (double)(t[2] - t[1]) * k;
    s_hl_gpu_ms[2] = (double)(t[3] - t[2]) * k;
    s_hl_gpu_ms[3] = (double)(t[3] - t[0]) * k;
    if (ovs_on()) s_ovs_gpu_ms += s_hl_gpu_ms[3];
    if (s_hl_gpu_ms[1] < 50.0 || s_gps_rd_slot < 0) return;
    const int fi = s_gps_rd_slot, n = s_gps_rd_n;
    D3D12_RANGE pr = { (SIZE_T)8 * GPS_MAX * fi, (SIZE_T)8 * GPS_MAX * fi + (SIZE_T)8 * n };
    if (FAILED(s_gps_rb->lpVtbl->Map(s_gps_rb, 0, &pr, &p)) || !p) return;
    static u64 ts[GPS_MAX];
    memcpy(ts, (const u8*)p + (size_t)8 * GPS_MAX * fi, (size_t)8 * n);
    s_gps_rb->lpVtbl->Unmap(s_gps_rb, 0, &wr);
    fprintf(stderr, "[gpu-passes] the frame's draws took %.1f ms on the GPU, %d passes; longest:", s_hl_gpu_ms[1], n - 1);
    static unsigned char done[GPS_MAX];
    memset(done, 0, sizeof done);
    for (int r = 0; r < 6; r++) {
        int best = -1;
        for (int i = 0; i + 1 < n; i++)
            if (!done[i] && ts[i + 1] >= ts[i] && (best < 0 || ts[i + 1] - ts[i] > ts[best + 1] - ts[best])) best = i;
        if (best < 0) break;
        done[best] = 1;
        const GpsPass* g = &s_gps[fi][best];
        fprintf(stderr, " | %.1f ms: target 0x%08X %ux%u, draws %u-%u, fp 0x%08X, own-target copies %u, "
                        "cache invalidates %u",
                (double)(ts[best + 1] - ts[best]) * k, g->off, g->w, g->h, g->from,
                s_gps[fi][best + 1].from, g->fp, s_gps[fi][best + 1].fbc - g->fbc, g->inval);
    }
    fprintf(stderr, "%c", 10);
}

/* ZPASS occlusion counts. God of War: Ascension issues ~400 a frame, one
 * bounding shape per object between CLEAR_REPORT_VALUE and GET_REPORT, and
 * its SPU jobs read every result: answered "visible" they culled nothing, and
 * answered 0 nearly the whole scene. The draws between a clear and its report
 * are counted by D3D12 occlusion queries, resolved at the batch's end; the
 * next batch, which starts by waiting for the GPU, hands each count to the
 * walker (cellGcm_zpass_result), which writes it into the report's value word
 * and keeps it for the next time the title reads that report.
 * RSX_ZPASS=0: off, every count "visible". */
#define ZQ_MAX     4096
#define ZQ_REP_MAX 2048
typedef struct { u32 ea, seq, q0, q1; } ZqRep; /* q0 = 0xFFFFFFFF: not counted */
static ID3D12QueryHeap* s_zq_heap;
static ID3D12Resource* s_zq_rb;
static ZqRep s_zq_rep[FRAME_COUNT][ZQ_REP_MAX];
static u32 s_zq_nrep[FRAME_COUNT], s_zq_n[FRAME_COUNT];
static int s_zq_resolved[FRAME_COUNT];
static u64 s_zq_fence[FRAME_COUNT];   /* the fence value its batch was submitted with, 0 = not yet */
static int s_zq_open = -1, s_zq_counting, s_zq_lost;
static int s_zq_enabled = 1;   /* SET_ZPASS_PIXEL_COUNT_ENABLE as the stream has it: on until told */
static unsigned long long s_zq_stat[4];      /* [1] visible [2] hidden [3] not counted */
static u32 s_zq_acc0;
static int zq_wanted(void)
{
    const char* e = getenv("RSX_ZPASS");
    return !(e && atoi(e) == 0);
}
static int zq_on(void)
{
    static int v = -1;
    if (v >= 0) return v;
    v = 0;
    if (!zq_wanted() || !s_d3d.device) return v;
    D3D12_QUERY_HEAP_DESC qd;
    memset(&qd, 0, sizeof qd);
    qd.Type = D3D12_QUERY_HEAP_TYPE_OCCLUSION;
    qd.Count = ZQ_MAX * FRAME_COUNT;
    D3D12_HEAP_PROPERTIES hp = {0}; hp.Type = D3D12_HEAP_TYPE_READBACK;
    D3D12_RESOURCE_DESC bd = {0};
    bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; bd.Width = 8ull * ZQ_MAX * FRAME_COUNT;
    bd.Height = 1; bd.DepthOrArraySize = 1; bd.MipLevels = 1; bd.SampleDesc.Count = 1;
    bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    if (SUCCEEDED(s_d3d.device->lpVtbl->CreateQueryHeap(s_d3d.device, &qd, &IID_ID3D12QueryHeap, (void**)&s_zq_heap)) &&
        SUCCEEDED(s_d3d.device->lpVtbl->CreateCommittedResource(s_d3d.device, &hp, D3D12_HEAP_FLAG_NONE, &bd,
            D3D12_RESOURCE_STATE_COPY_DEST, NULL, &IID_ID3D12Resource, (void**)&s_zq_rb)))
        v = 1;
    printf("[D3D12] ZPASS counts: %s\n", v ? "occlusion queries" : "could not create the query heap, reported visible");
    return v;
}
/* cellGcm_zpass_observer: the counter's clear (op 0), a report (op 1, its
 * value word at a, its number b) or the counter switched on or off (op 2, a),
 * recorded where it falls among the draws. */
static void d3d12_zpass_observed(int op, u32 a, u32 b)
{
    if (!s_d3d.initialized || s_d3d.draw_count >= MAX_DRAWS) return;
    D3D12DrawRecord* dr = &s_d3d.draws[s_d3d.draw_count++];
    memset(dr, 0, sizeof(*dr));
    dr->is_vp = 1; dr->is_clear = 1; dr->tex_slot = -1; dr->snap_slot = -1;
    dr->zq_op = (u32)op + 1u;
    dr->zq_ea = a;
    dr->zq_seq = b;
    s_d3d.merge_prev_draw = 0;
    s_ix_run_valid = 0;
    s_raw_run_valid = 0;
}
static void zq_close(void)
{
    if (s_zq_open < 0) return;
    s_d3d.cmd_list->lpVtbl->EndQuery(s_d3d.cmd_list, s_zq_heap, D3D12_QUERY_TYPE_OCCLUSION,
                                     (UINT)(s_rf.fi * ZQ_MAX) + (UINT)s_zq_open);
    s_zq_open = -1;
}
static void zq_op_replay(const D3D12DrawRecord* dr)
{
    if (!zq_on()) return;
    const u32 fi = s_rf.fi;
    zq_close();
    if (dr->zq_op == 1) { s_zq_counting = 1; s_zq_acc0 = s_zq_n[fi]; s_zq_lost = 0; return; }
    if (dr->zq_op == 3) { s_zq_enabled = dr->zq_ea != 0; return; }
    if (s_zq_nrep[fi] < ZQ_REP_MAX) {
        ZqRep* r = &s_zq_rep[fi][s_zq_nrep[fi]++];
        r->ea = dr->zq_ea;
        r->seq = dr->zq_seq;
        /* The count since the clear: every query opened after it. A report
         * with no clear before it in this batch, or whose draws ran out of
         * queries, keeps what the walker wrote ("visible"). */
        r->q0 = (s_zq_counting && !s_zq_lost) ? s_zq_acc0 : 0xFFFFFFFFu;
        r->q1 = s_zq_n[fi];
    }
    /* The counter goes on after a report: the next one, before a clear,
     * reports the total so far (God of War: Ascension writes one count into
     * eight reports). */
}
/* A draw that was to be counted is not drawn (its pipeline is still being
 * built): the count would read 0 and drop the object, so the reports up to
 * the next clear keep what the walker wrote ("visible"). */
static void zq_draw_lost(void)
{
    if (s_zq_counting && s_zq_enabled) s_zq_lost = 1;
}
static void zq_before_draw(void)
{
    if (!s_zq_counting || !s_zq_enabled || s_zq_open >= 0 || !zq_on()) return;
    const u32 fi = s_rf.fi;
    if (s_zq_n[fi] >= ZQ_MAX) { s_zq_lost = 1; return; }
    s_zq_open = (int)s_zq_n[fi]++;
    s_d3d.cmd_list->lpVtbl->BeginQuery(s_d3d.cmd_list, s_zq_heap, D3D12_QUERY_TYPE_OCCLUSION,
                                       (UINT)(fi * ZQ_MAX) + (UINT)s_zq_open);
}
static void zq_resolve(void)
{
    if (!zq_on()) return;
    const u32 fi = s_rf.fi;
    zq_close();
    if (s_zq_n[fi])
        s_d3d.cmd_list->lpVtbl->ResolveQueryData(s_d3d.cmd_list, s_zq_heap, D3D12_QUERY_TYPE_OCCLUSION,
                                                 (UINT)(fi * ZQ_MAX), s_zq_n[fi], s_zq_rb, 8ull * ZQ_MAX * fi);
    s_zq_resolved[fi] = 1;
}
/* Hand a submitted batch's counts to the reports (cellGcm_zpass_result). */
static void zq_deliver(u32 fi)
{
    extern void cellGcm_zpass_result(u32 value_ea, u32 count, u32 seq);
    {
        if (s_zq_nrep[fi]) {
            /* A report whose counter was off from its clear on counted nothing:
             * its range is empty and its answer 0, in a batch with no query too
             * (God of War: Ascension clears, switches the counter off at once
             * and reports, in two reports a frame). */
            void* p = NULL;
            const u64* q = NULL;
            if (s_zq_n[fi]) {
                D3D12_RANGE rr = { (SIZE_T)8 * ZQ_MAX * fi, (SIZE_T)8 * ZQ_MAX * fi + (SIZE_T)8 * s_zq_n[fi] };
                if (SUCCEEDED(s_zq_rb->lpVtbl->Map(s_zq_rb, 0, &rr, &p)) && p)
                    q = (const u64*)((const u8*)p + (size_t)8 * ZQ_MAX * fi);
            }
            for (u32 k = 0; k < s_zq_nrep[fi]; k++) {
                const ZqRep* r = &s_zq_rep[fi][k];
                if (r->q0 == 0xFFFFFFFFu || r->q1 > s_zq_n[fi] || (r->q1 > r->q0 && !q)) { s_zq_stat[3]++; continue; }
                u64 sum = 0;
                for (u32 i = r->q0; i < r->q1; i++) sum += q[i];
                /* A draw covers s*s the samples the title counts on: visible stays visible. */
                if (RS_ON && sum) { sum = (u64)((double)sum / (double)(s_rs * s_rs * s_wide)); if (!sum) sum = 1; }
                const u32 c = sum > 0xFFFFFFFFull ? 0xFFFFFFFFu : (u32)sum;
                cellGcm_zpass_result(r->ea, c, r->seq);
                s_zq_stat[c ? 1 : 2]++;
            }
            if (p) {
                D3D12_RANGE wr = {0, 0};
                s_zq_rb->lpVtbl->Unmap(s_zq_rb, 0, &wr);
            }
        }
        s_zq_resolved[fi] = 0; s_zq_nrep[fi] = 0; s_zq_n[fi] = 0; s_zq_fence[fi] = 0;
    }
}
/* The batch whose queries were resolved into frame fi went to the GPU with this fence. */
static void zq_submitted(u32 fi, u64 fence)
{
    if (fi < FRAME_COUNT && s_zq_resolved[fi]) s_zq_fence[fi] = fence;
}
/* Between walker passes: a batch the GPU has finished hands its counts on
 * now, not when the next batch starts. The title reads a frame's reports
 * during the next one and writes the same reports again a frame later, so
 * counts that waited for the next batch's start came after the reports had
 * been taken again (2,348 of them in 40,000 trace events of the menu). */
static void zq_poll(void)
{
    if (!zq_on() || !s_d3d.fence) return;
    const u64 done = s_d3d.fence->lpVtbl->GetCompletedValue(s_d3d.fence);
    for (u32 fi = 0; fi < FRAME_COUNT; fi++)
        if (s_zq_resolved[fi] && s_zq_fence[fi] && done >= s_zq_fence[fi])
            zq_deliver(fi);
}
/* After the wait for the GPU: every batch submitted has run. */
static void zq_read(void)
{
    if (!zq_on()) return;
    for (u32 fi = 0; fi < FRAME_COUNT; fi++) {
        /* A list reset without being submitted took its queries with it. */
        if (!s_zq_resolved[fi]) { s_zq_nrep[fi] = 0; s_zq_n[fi] = 0; s_zq_fence[fi] = 0; continue; }
        zq_deliver(fi);
    }
    s_zq_open = -1;   /* a new command list: nothing open in it */
    /* A clear in an earlier batch: its queries are in that batch's heap, so the
     * reports of this one cannot add them up; they keep the walker's value. */
    if (s_zq_counting) s_zq_lost = 1;
    /* With RSX_CENSUS: how the counts came out over each census window. */
    { static ULONGLONG t0;
      const unsigned secs = rsx_census_secs();
      const ULONGLONG now = GetTickCount64();
      if (!t0) t0 = now;
      if (secs && now - t0 >= (ULONGLONG)secs * 1000ull) {
          t0 = now;
          fprintf(stderr, "[zpass] reports counted: %llu visible, %llu hidden; %llu left as the walker wrote them%c",
                  s_zq_stat[1], s_zq_stat[2], s_zq_stat[3], 10);
          memset(s_zq_stat, 0, sizeof s_zq_stat);
      } }
}

/* Colour surfaces in main memory (rec_mwb): at the batch's end each one its
 * records drew is copied into this frame's readback buffer; when the batch's
 * fence has passed, the pixels are written to guest memory. */
#define MWB_MAX    8
#define MWB_BYTES  ((u64)(8u << 20) * (u32)ceilf(s_rs * s_rs * s_wide))
typedef struct { u32 off, pitch, w, h, fmt, row_pitch; u64 buf_off; u32 pw, ph; float sx, sy; } MwbEntry;
static ID3D12Resource* s_mwb_rb[FRAME_COUNT];
static MwbEntry s_mwb[FRAME_COUNT][MWB_MAX];
static u32 s_mwb_n[FRAME_COUNT];
static u64 s_mwb_used[FRAME_COUNT];    /* bytes of the readback buffer the copies fill */
static u64 s_mwb_fence[FRAME_COUNT];   /* the batch's fence value, 0 = not submitted yet */

/* The RSX's colour formats with 4 bytes a pixel (SET_SURFACE_FORMAT [4:0]):
 * X8R8G8B8 (Z: X = 0, O: X = 0xFF), A8R8G8B8 and their B8G8R8 twins. */
static int mwb_format_ok(u32 cf)
{
    return cf == 0x4 || cf == 0x5 || cf == 0x8 || cf == 0xE || cf == 0xF || cf == 0x10;
}

static void mwb_record(void)
{
    if (!mwb_on() || !s_d3d.device) return;
    const u32 fi = s_rf.fi;
    if (fi >= FRAME_COUNT) return;
    if (s_mwb_n[fi]) {   /* this frame's last batch not delivered: keep it */
        static int _n = 0;
        if (PS3_LOG_FIRST(_n, 4)) fprintf(stderr, "[MWB] frame %u's write-back still pending; skipped%c", fi, 10);
        return;
    }
    u32 n = 0;
    for (u32 i = 0; i < s_d3d.draw_count && i < MAX_DRAWS; i++) {
        const D3D12DrawRecord* dr = &s_d3d.draws[i];
        if (!dr->mwb_off) continue;
        u32 k = 0;
        while (k < n && s_mwb[fi][k].off != dr->mwb_off) k++;
        if (k == n) {
            if (n == MWB_MAX) continue;
            n++;
        }
        MwbEntry* e = &s_mwb[fi][k];
        e->off = dr->mwb_off; e->pitch = dr->mwb_pitch; e->w = dr->mwb_w; e->h = dr->mwb_h; e->fmt = dr->mwb_fmt;
    }
    if (!n) return;
    if (!s_mwb_rb[fi]) {
        D3D12_HEAP_PROPERTIES hp = {0}; hp.Type = D3D12_HEAP_TYPE_READBACK;
        D3D12_RESOURCE_DESC bd = {0};
        bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; bd.Width = MWB_BYTES;
        bd.Height = 1; bd.DepthOrArraySize = 1; bd.MipLevels = 1; bd.SampleDesc.Count = 1;
        bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        if (FAILED(s_d3d.device->lpVtbl->CreateCommittedResource(s_d3d.device, &hp, D3D12_HEAP_FLAG_NONE, &bd,
                D3D12_RESOURCE_STATE_COPY_DEST, NULL, &IID_ID3D12Resource, (void**)&s_mwb_rb[fi]))) {
            s_mwb_rb[fi] = NULL;
            static int _n = 0;
            if (PS3_LOG_FIRST(_n, 2)) fprintf(stderr, "[MWB] readback buffer failed%c", 10);
            return;
        }
    }
    u64 pos = 0;
    u32 kept = 0;
    for (u32 k = 0; k < n; k++) {
        MwbEntry e = s_mwb[fi][k];
        const u32 cf = e.fmt & 0x1Fu;
        const int slot = off_rt_find(e.off);
        u32 ax = 1, ay = 1;
        rsx_aa_grid(e.fmt, &ax, &ay);
        const char* why = NULL;
        if (slot < 0) why = "no target";
        else if (!mwb_format_ok(cf) || s_d3d.off_rt[slot].dxgi != DXGI_FORMAT_R8G8B8A8_UNORM) why = "format";
        else if (ax * ay != 1) why = "antialiased";
        else if (!e.w || !e.h) why = "empty clip";
        if (why) {
            static int _n = 0;
            if (PS3_LOG_FIRST(_n, 8))
                fprintf(stderr, "[MWB] main-memory surface 0x%X %ux%u format 0x%X not written back (%s)%c",
                        e.off, e.w, e.h, e.fmt, why, 10);
            continue;
        }
        OffRT* r = &s_d3d.off_rt[slot];
        if (e.w > r->w) e.w = r->w;
        if (e.h > r->h) e.h = r->h;
        e.sx = r->sx; e.sy = r->sy;
        e.pw = (u32)((float)e.w * e.sx + 0.5f); e.ph = (u32)((float)e.h * e.sy + 0.5f);
        if (e.pw > r->pw) e.pw = r->pw;
        if (e.ph > r->ph) e.ph = r->ph;
        e.row_pitch = (e.pw * 4u + 255u) & ~255u;
        pos = (pos + 511u) & ~(u64)511u;
        if (pos + (u64)e.row_pitch * e.ph > MWB_BYTES) {
            static int _n = 0;
            if (PS3_LOG_FIRST(_n, 4))
                fprintf(stderr, "[MWB] main-memory surface 0x%X %ux%u does not fit the readback buffer%c", e.off, e.w, e.h, 10);
            continue;
        }
        e.buf_off = pos;
        const D3D12_RESOURCE_STATES before = r->st;
        off_rt_transition(slot, D3D12_RESOURCE_STATE_COPY_SOURCE);
        D3D12_TEXTURE_COPY_LOCATION dst = {0}, src = {0};
        dst.pResource = s_mwb_rb[fi];
        dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        dst.PlacedFootprint.Offset = pos;
        dst.PlacedFootprint.Footprint.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        dst.PlacedFootprint.Footprint.Width = e.pw;
        dst.PlacedFootprint.Footprint.Height = e.ph;
        dst.PlacedFootprint.Footprint.Depth = 1;
        dst.PlacedFootprint.Footprint.RowPitch = e.row_pitch;
        src.pResource = r->res;
        src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        src.SubresourceIndex = 0;
        D3D12_BOX box = { 0, 0, 0, e.pw, e.ph, 1 };
        s_d3d.cmd_list->lpVtbl->CopyTextureRegion(s_d3d.cmd_list, &dst, 0, 0, 0, &src, &box);
        off_rt_transition(slot, before);
        pos += (u64)e.row_pitch * e.ph;
        s_mwb[fi][kept++] = e;
    }
    s_mwb_n[fi] = kept;
    s_mwb_used[fi] = pos;
    s_mwb_fence[fi] = 0;
}

static void mwb_submitted(u32 fi, u64 fence)
{
    if (fi < FRAME_COUNT && s_mwb_n[fi]) s_mwb_fence[fi] = fence;
}

/* The pixels of a batch that has run, into guest memory: the RSX keeps an
 * A8R8G8B8 pixel as the bytes A R G B (A8B8G8R8: A B G R), and the target
 * holds the shader's x y z w as R G B A. */
static void mwb_deliver(u32 fi)
{
    extern uint8_t* vm_base;
    extern u32 gcm_io_offset_to_ea(u32 io);
    void* p = NULL;
    D3D12_RANGE rr = { 0, (SIZE_T)s_mwb_used[fi] };
    if (vm_base && s_mwb_rb[fi] && SUCCEEDED(s_mwb_rb[fi]->lpVtbl->Map(s_mwb_rb[fi], 0, &rr, &p)) && p) {
        for (u32 k = 0; k < s_mwb_n[fi]; k++) {
            const MwbEntry* e = &s_mwb[fi][k];
            const u32 ea = gcm_io_offset_to_ea(e->off);
            if (!ea) continue;
            const u32 cf = e->fmt & 0x1Fu;
            const u32 pitch = e->pitch >= e->w * 4u ? e->pitch : e->w * 4u;
            for (u32 y = 0; y < e->h; y++) {
                /* The guest's pixel from the nearest of a target drawn larger. */
                u32 py = (u32)(((float)y + 0.5f) * e->sy);
                if (py >= e->ph) py = e->ph - 1;
                const u8* row = (const u8*)p + e->buf_off + (u64)py * e->row_pitch;
                u8* d = vm_base + ea + (u64)y * pitch;
                for (u32 x = 0; x < e->w; x++, d += 4) {
                    u32 px = (u32)(((float)x + 0.5f) * e->sx);
                    if (px >= e->pw) px = e->pw - 1;
                    const u8* s = row + (u64)px * 4u;
                    const u8 R = s[0], G = s[1], B = s[2], A = s[3];
                    const u8 X = (cf == 0x4 || cf == 0xE) ? 0x00 : (cf == 0x5 || cf == 0xF) ? 0xFF : A;
                    if (cf >= 0xE) { d[0] = X; d[1] = B; d[2] = G; d[3] = R; }
                    else           { d[0] = X; d[1] = R; d[2] = G; d[3] = B; }
                }
            }
            static int _n = 0;
            if (PS3_LOG_FIRST(_n, 3))
                fprintf(stderr, "[MWB] main-memory surface 0x%X (0x%08X) %ux%u format 0x%X written back: %02X %02X %02X %02X%c",
                        e->off, ea, e->w, e->h, e->fmt, vm_base[ea], vm_base[ea + 1], vm_base[ea + 2], vm_base[ea + 3], 10);
        }
        D3D12_RANGE wr = { 0, 0 };
        s_mwb_rb[fi]->lpVtbl->Unmap(s_mwb_rb[fi], 0, &wr);
    }
    s_mwb_n[fi] = 0;
    s_mwb_fence[fi] = 0;
}

static void mwb_poll(void)
{
    if (!s_d3d.fence) return;
    const u64 done = s_d3d.fence->lpVtbl->GetCompletedValue(s_d3d.fence);
    for (u32 fi = 0; fi < FRAME_COUNT; fi++)
        if (s_mwb_n[fi] && s_mwb_fence[fi] && done >= s_mwb_fence[fi])
            mwb_deliver(fi);
}

static int s_rf_open_ahead;   /* RF_WAITLOG: rf_open called by record-ahead */
static void rf_open(void)
{
    u32 fi = s_d3d.frame_index;
    s_rf.fi = fi;

    /* Drain the GPU before touching shared upload resources (vp_vb vertices,
     * vp_cb constants, per-frame texture staging): the previous frame's draws
     * may still be reading them, and overwriting mid-flight tears geometry
     * (gcm/cube: triangles mixing stale and new vertices -> missing/sliver
     * polygons). These workloads are a few draws/frame, so full serialisation
     * costs little. */
    /* When the last value signalled is done, so is everything the backend gave
     * the queue (a batch's list, its present, the signal after them), and the
     * wait is skipped. Its own signal took 3-17 ms there in God of War:
     * Ascension's first level (~10 ms a frame, with the drain stopped behind
     * it), though the batch before had finished. RF_IDLE_WAIT=1 waits anyway. */
    { const double t0 = perf_now();
      const u64 done0 = s_d3d.fence->lpVtbl->GetCompletedValue(s_d3d.fence);
      static int idle_wait = -1;
      if (idle_wait < 0) { const char* e = getenv("RF_IDLE_WAIT"); idle_wait = e ? atoi(e) != 0 : 0; }
      if (idle_wait || done0 < s_fence_last)
          wait_for_gpu();
      s_hl_gpuwait_s += perf_now() - t0;
      vpass_check_read();     /* the last batch has run: its lit copies against the SPU's */
      /* RF_WAITLOG=1: every 50th open -- who opened the batch, how long after
       * the last submit, what the GPU had done by then, and the wait. */
      { static int on = -1; static long n;
        if (on < 0) on = getenv("RF_WAITLOG") ? 1 : 0;
        if (on && (++n % 50) == 0)
            fprintf(stderr, "[rf-wait] #%ld by %s: %.1f ms after the last submit, fence done %llu, last signalled %llu, waited %.1f ms, %u records%c",
                    n, s_rf_open_ahead ? "record-ahead" : "render_frame",
                    (t0 - s_rf_last_submit) * 1000.0, (unsigned long long)done0,
                    (unsigned long long)s_fence_last, (perf_now() - t0) * 1000.0, s_d3d.draw_count, 10); } }
    gts_read();
    zq_read();
    mwb_poll();
    rf_release_dead();   /* the batch that last referenced them has run */
    ort_bits_rebuild();  /* the offsets of slots made anew since drop out of off_rt_find's bits */
    vp_tex_release_staging();
    rdoc_frame_begin(s_d3d.draw_count);

    /* Compile the real vertex program once its microcode is captured, and keep
     * the constant bank uploaded for the VS. */
    /* The base VP pipeline gates the ENTIRE vertex-program draw pass below
     * (`if (s_d3d.vp_ready ...)`). The old trigger also required the microcode
     * size to differ from the last compile, so a title whose first captured
     * program has the same byte count as vp_compiled_bytes never built the base
     * pipeline -- and then every is_vp draw record was silently dropped: draws
     * recorded, constants uploaded, per-draw VS compiled and cached, and not one
     * of them submitted. While !vp_ready there is nothing to be stale against,
     * so only the "have microcode" test belongs here. */
    if (s_d3d.current_rsx_state && !s_d3d.vp_ready &&
        s_d3d.current_rsx_state->vp_ucode_bytes >= 16)
        compile_vp();
    /* Per-draw VP constants are snapshotted at record time (vp_record_cb). */

    /* Lazily create the readback buffer the first time a dump is requested. */
    if ((s_d3d.dump_frames_left > 0 || (s_shot_pending && d3d12_shot_game())) && !s_d3d.readback_buf) {
        s_d3d.readback_pitch = (s_d3d.pw * 4 + 255) & ~255u;
        D3D12_HEAP_PROPERTIES hp = {0};
        hp.Type = D3D12_HEAP_TYPE_READBACK;
        D3D12_RESOURCE_DESC rd = {0};
        rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        rd.Width = (u64)s_d3d.readback_pitch * s_d3d.ph;
        rd.Height = 1; rd.DepthOrArraySize = 1; rd.MipLevels = 1;
        rd.SampleDesc.Count = 1;
        rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        s_d3d.device->lpVtbl->CreateCommittedResource(
            s_d3d.device, &hp, D3D12_HEAP_FLAG_NONE, &rd,
            D3D12_RESOURCE_STATE_COPY_DEST, NULL,
            &IID_ID3D12Resource, (void**)&s_d3d.readback_buf);
    }

    /* Reset command allocator and list */
    s_d3d.cmd_allocators[fi]->lpVtbl->Reset(s_d3d.cmd_allocators[fi]);
    s_d3d.cmd_list->lpVtbl->Reset(s_d3d.cmd_list, s_d3d.cmd_allocators[fi], NULL);
    s_d3d.tex_ring_used[fi] = 0;   /* this frame index's copies are done */
    tex_spill_reset(fi);
    gts_mark(0);

    /* Upload the bound font atlas into an R8_UNORM texture and create its SRV.
     * The atlas is a linear 8-bit coverage map, so a straight row copy (no
     * deswizzle) suffices. It is a DYNAMIC glyph cache -- the game rasterizes
     * new glyphs into it over time -- so re-upload whenever it is dirty (set on
     * every bind), not just once. Uploading only on first bind left every glyph
     * added after frame 1 sampling stale texels -> torn text. The backend is
     * synchronous (wait_for_gpu each frame), so recreating the resource per
     * dirty frame is safe. */
    /* The legacy atlas, for the legacy records only. Not when the render
     * thread opens a batch ahead (RSX_RENDER_THREAD): the walk is binding its
     * textures meanwhile, and a batch recorded ahead has no legacy records. */
    if (!(s_rt_mode > 0 && s_rf_open_ahead) &&
        s_d3d.tex_bound && (!s_d3d.tex_ready || s_d3d.tex_dirty) && s_d3d.srv_heap && s_d3d.pipeline_state_tex) {
        extern uint8_t* vm_base;
        u32 w = s_d3d.tex_w, h = s_d3d.tex_h;
        u32 pitch = (w + 255) & ~255u;   /* D3D12 requires 256-byte row pitch */

        if (s_d3d.tex_resource) { s_d3d.tex_resource->lpVtbl->Release(s_d3d.tex_resource); s_d3d.tex_resource = NULL; }
        if (s_d3d.tex_upload)   { s_d3d.tex_upload->lpVtbl->Release(s_d3d.tex_upload);     s_d3d.tex_upload = NULL; }

        D3D12_HEAP_PROPERTIES hp_def = {0}; hp_def.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC td = {0};
        td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        td.Width = w; td.Height = h; td.DepthOrArraySize = 1; td.MipLevels = 1;
        td.Format = DXGI_FORMAT_R8_UNORM; td.SampleDesc.Count = 1;
        td.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        HRESULT thr = s_d3d.device->lpVtbl->CreateCommittedResource(
            s_d3d.device, &hp_def, D3D12_HEAP_FLAG_NONE, &td,
            D3D12_RESOURCE_STATE_COPY_DEST, NULL,
            &IID_ID3D12Resource, (void**)&s_d3d.tex_resource);

        D3D12_HEAP_PROPERTIES hp_up = {0}; hp_up.Type = D3D12_HEAP_TYPE_UPLOAD;
        D3D12_RESOURCE_DESC ud = {0};
        ud.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        ud.Width = (u64)pitch * h; ud.Height = 1; ud.DepthOrArraySize = 1; ud.MipLevels = 1;
        ud.SampleDesc.Count = 1; ud.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        if (SUCCEEDED(thr))
            thr = s_d3d.device->lpVtbl->CreateCommittedResource(
                s_d3d.device, &hp_up, D3D12_HEAP_FLAG_NONE, &ud,
                D3D12_RESOURCE_STATE_GENERIC_READ, NULL,
                &IID_ID3D12Resource, (void**)&s_d3d.tex_upload);

        if (SUCCEEDED(thr) && vm_base) {
            void* mapped = NULL;
            D3D12_RANGE nr = {0, 0};
            if (SUCCEEDED(s_d3d.tex_upload->lpVtbl->Map(s_d3d.tex_upload, 0, &nr, &mapped)) && mapped) {
                const u8* srcbase = vm_base + s_d3d.tex_src_offset;
                for (u32 y = 0; y < h; y++)
                    memcpy((u8*)mapped + (u64)y * pitch, srcbase + (u64)y * w, w);
                s_d3d.tex_upload->lpVtbl->Unmap(s_d3d.tex_upload, 0, NULL);

                D3D12_TEXTURE_COPY_LOCATION dst = {0}, src = {0};
                dst.pResource = s_d3d.tex_resource;
                dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
                dst.SubresourceIndex = 0;
                src.pResource = s_d3d.tex_upload;
                src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
                src.PlacedFootprint.Offset = 0;
                src.PlacedFootprint.Footprint.Format   = DXGI_FORMAT_R8_UNORM;
                src.PlacedFootprint.Footprint.Width    = w;
                src.PlacedFootprint.Footprint.Height   = h;
                src.PlacedFootprint.Footprint.Depth    = 1;
                src.PlacedFootprint.Footprint.RowPitch = pitch;
                s_d3d.cmd_list->lpVtbl->CopyTextureRegion(s_d3d.cmd_list, &dst, 0, 0, 0, &src, NULL);

                D3D12_RESOURCE_BARRIER tb = {0};
                tb.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                tb.Transition.pResource   = s_d3d.tex_resource;
                tb.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
                tb.Transition.StateAfter  = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
                tb.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
                s_d3d.cmd_list->lpVtbl->ResourceBarrier(s_d3d.cmd_list, 1, &tb);

                D3D12_SHADER_RESOURCE_VIEW_DESC sv = {0};
                sv.Format = DXGI_FORMAT_R8_UNORM;
                sv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
                sv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
                sv.Texture2D.MipLevels = 1;
                D3D12_CPU_DESCRIPTOR_HANDLE sh;
                s_d3d.srv_heap->lpVtbl->GetCPUDescriptorHandleForHeapStart(s_d3d.srv_heap, &sh);
                s_d3d.device->lpVtbl->CreateShaderResourceView(s_d3d.device, s_d3d.tex_resource, &sv, sh);

                s_d3d.tex_ready = 1;
                s_d3d.tex_dirty = 0;   /* content now in sync with guest atlas */
                { static int _au = 0; if (PS3_LOG_FIRST(_au, 3))
                    printf("[D3D12] atlas uploaded (%ux%u R8) -> textured\n", w, h); }
            }
        }
    }

    /* Frames with no ops (init/boot presents) don't count against the cap --
     * PNG-decode-heavy titles burn hundreds of empty presents before the first
     * real draw. */
    if (env_str("RTT_DUMP", &s_env_rtt_dump) && s_d3d.draw_count > 0) { static int _f=0; int _cap = atoi(s_env_rtt_dump); if (_cap < 2) _cap = 14; if (PS3_LOG_FIRST(_f, _cap)) {
        fprintf(stderr, "[RTT] frame %d: %u ops\n", _f, s_d3d.draw_count);
        for (u32 _d = 0; _d < s_d3d.draw_count && _d < MAX_DRAWS; _d++) {
            D3D12DrawRecord* r = &s_d3d.draws[_d];
            fprintf(stderr, "[RTT]  op%02u %s rt=0x%X mrt=0x%X/0x%X/0x%X t0=0x%X fp=0x%X n=%u cmask=%X vp=%u,%u %ux%u blend=%d\n",
                _d, r->is_clear?"CLR ":(r->is_vp?"draw":"leg "), r->rt_off,
                r->rt_mrt[0], r->rt_mrt[1], r->rt_mrt[2],
                r->tex[0].raw, r->fp_addr, r->vertex_count,
                r->cmask, r->vp_x, r->vp_y, r->vp_w, r->vp_h, r->blend);
        }
    } }

    /* Debug: RTT_PASS=N shows pass N's output directly on screen (drops later
     * ops, retargets pass N to the backbuffer). */
    { const char* rp = env_str("RTT_PASS", &s_env_rtt_pass);
      if (rp) {
        int keep = atoi(rp), seen = 0; u32 cut = s_d3d.draw_count;
        int viewrt = getenv("RTT_VIEWRT") != NULL;
        for (u32 _d = 0; _d < s_d3d.draw_count && _d < MAX_DRAWS; _d++) {
            D3D12DrawRecord* r = &s_d3d.draws[_d];
            if (r->is_clear || !r->is_vp) continue;
            if (seen == keep) {
                /* Alone: retarget op N to the backbuffer. With RTT_VIEWRT:
                 * keep op N intact and pull the display composite forward so
                 * the chosen RT is shown as of this point in the chain. */
                if (!viewrt) { r->rt_off = 0; r->rt_mrt[0] = r->rt_mrt[1] = r->rt_mrt[2] = 0; }
                cut = _d + 1;
                break;
            }
            seen++;
        }
        if (viewrt) {
            for (u32 _j = cut; _j < s_d3d.draw_count && _j < MAX_DRAWS; _j++) {
                D3D12DrawRecord* r = &s_d3d.draws[_j];
                if (r->is_vp && !r->is_clear && r->rt_off == 0) {
                    if (cut < MAX_DRAWS) s_d3d.draws[cut++] = *r;
                    break;
                }
            }
        }
        s_d3d.draw_count = (cut < s_d3d.draw_count) ? cut : s_d3d.draw_count;
      } }

    /* DRAW_LIMIT=<N>: keep only the first N recorded ops this frame. The duck's
     * draws are ops 00..~03 of every frame, so a small limit shows the ducks
     * alone -- separating "the ducks never rasterize" from "later geometry is
     * painted over them". Diagnostic only. */
    { static int lim = -1;
      if (lim < 0) { const char* e = getenv("DRAW_LIMIT"); lim = e ? atoi(e) : 0; }
      if (lim > 0 && s_d3d.draw_count > (u32)lim) s_d3d.draw_count = (u32)lim; }

    /* DRAW_SKIP_TEX=<hex raw offset>: drop every op binding that texture on
     * unit 0. The ducks sit behind the water's slab geometry, so removing the
     * occluder is the way to see whether the duck itself renders. Diagnostic. */
    { static const char* sk = (const char*)1; static u32 want = 0;
      if (sk == (const char*)1) sk = getenv("DRAW_SKIP_TEX");
      want = draw_filter_tex(sk);
      if (want) {
        u32 keep = 0;
        for (u32 _d = 0; _d < s_d3d.draw_count && _d < MAX_DRAWS; _d++) {
            if (!s_d3d.draws[_d].is_clear && s_d3d.draws[_d].tex[0].raw == want) continue;
            if (keep != _d) s_d3d.draws[keep] = s_d3d.draws[_d];
            keep++;
        }
        s_d3d.draw_count = keep;
      } }

    /* DRAW_KEEP_TEX=<hex raw offset>: the inverse -- keep only the ops binding
     * that texture (plus clears). Isolates one object from everything drawn over
     * it, which is what finally shows the duck on its own. Diagnostic. */
    { static const char* kp = (const char*)1; static u32 want = 0;
      if (kp == (const char*)1) kp = getenv("DRAW_KEEP_TEX");
      want = draw_filter_tex(kp);
      /* DRAW_KEEP_NOCLEAR=1: also drop the clears. Keeping them preserves their
       * ORIGINAL position in the batch, so a clear that originally ran after the
       * kept draws still runs after them -- and wipes the very geometry being
       * isolated. That makes an object that renders fine look like it renders
       * nothing. */
      static int noclr = -1;
      if (noclr < 0) { const char* e = getenv("DRAW_KEEP_NOCLEAR"); noclr = e ? atoi(e) : 0; }
      /* DRAW_KEEP_BLEND=0|1: further restrict DRAW_KEEP_TEX to draws with that
       * blend flag. A mesh drawn twice -- once opaque, once blended -- is two
       * different passes (body vs reflection shell) that a texture filter alone
       * cannot separate. */
      static int kblend = -2;
      if (kblend == -2) { const char* e = getenv("DRAW_KEEP_BLEND");
                          kblend = e ? atoi(e) : -1; }
      if (want) {
        /* Emit the CLEARS FIRST, then the kept draws. Preserving the original
         * order means a clear that ran after them still runs after them and
         * wipes the isolated geometry; dropping the clears entirely (noclr)
         * leaves stale DEPTH that rejects most of its fragments. Neither shows
         * the object properly -- clear, then draw, does. */
        static D3D12DrawRecord kept[MAX_DRAWS];
        u32 nk = 0;
        if (!noclr)
            for (u32 _d = 0; _d < s_d3d.draw_count && _d < MAX_DRAWS; _d++)
                if (s_d3d.draws[_d].is_clear && nk < MAX_DRAWS) kept[nk++] = s_d3d.draws[_d];
        for (u32 _d = 0; _d < s_d3d.draw_count && _d < MAX_DRAWS; _d++)
            if (!s_d3d.draws[_d].is_clear && s_d3d.draws[_d].tex[0].raw == want
                && (kblend < 0 || s_d3d.draws[_d].blend == kblend)
                && nk < MAX_DRAWS)
                kept[nk++] = s_d3d.draws[_d];
        for (u32 _k = 0; _k < nk; _k++) s_d3d.draws[_k] = kept[_k];
        s_d3d.draw_count = nk;
      } }

    /* DRAW_LAST_TEX=<hex raw offset>: move the ops binding that texture to the
     * END of the batch so nothing is drawn over them. The duck's draws are ops
     * 00..03 of every frame and the tub wall is drawn afterwards across the same
     * screen area, so anything that defeats the depth sort hides the duck
     * completely. Reordering separates "occluded" from "not rendered". */
    { static const char* lt = (const char*)1; static u32 want = 0;
      if (lt == (const char*)1) lt = getenv("DRAW_LAST_TEX");
      want = draw_filter_tex(lt);
      if (want && s_d3d.draw_count > 1) {
        static D3D12DrawRecord moved[MAX_DRAWS];
        u32 nm = 0, keep = 0;
        for (u32 _d = 0; _d < s_d3d.draw_count && _d < MAX_DRAWS; _d++) {
            if (!s_d3d.draws[_d].is_clear && s_d3d.draws[_d].tex[0].raw == want) {
                if (nm < MAX_DRAWS) moved[nm++] = s_d3d.draws[_d];
                continue;
            }
            if (keep != _d) s_d3d.draws[keep] = s_d3d.draws[_d];
            keep++;
        }
        for (u32 _m = 0; _m < nm && keep < MAX_DRAWS; _m++)
            s_d3d.draws[keep++] = moved[_m];
        s_d3d.draw_count = keep;
      } }

    for (int _i = 0; _i < MAX_OFF_RTS; _i++) s_d3d.off_rt[_i].used = 0;
    for (int _i = 0; _i < s_tex_hw; _i++) s_d3d.vp_tex[_i].used = 0;
    s_rf.pre_done = s_rf.done = 0;
    s_srvq_gen++;
}

/* The RT and texture prepass for records [from, to). */
static unsigned long long s_pre_draws, s_pre_shared;   /* rf_prepass's draws, those whose units were shared */
/* Whether a draw with these units' targets reads one of the targets it draws into (the draw pass
 * gives such a unit RT_FEEDBACK's copy, in the draw's own window). */
static int rec_reads_own(const D3D12DrawRecord* dr, const int* tex_rt)
{
    if (dr->no_color) return 0;
    int own[4];
    own[0] = dr->rt_off ? off_rt_find(dr->rt_off) : -1;
    for (int m = 0; m < 3; m++) own[1 + m] = dr->rt_mrt[m] ? off_rt_find(dr->rt_mrt[m]) : -1;
    for (int u = 0; u < VP_TEX_UNITS; u++)
        if (tex_rt[u] >= 0)
            for (int k = 0; k < 4; k++) if (own[k] >= 0 && tex_rt[u] == own[k]) return 1;
    return 0;
}
/* A draw whose window another may bind: the draw pass rewrites none of it. */
static int pre_shareable(const D3D12DrawRecord* dr)
{
    for (int u = 0; u < VP_TEX_UNITS; u++) if (dr->tex_dz[u] >= 0) return 0;
    return !rec_reads_own(dr, dr->tex_rt);
}
static void rf_prepass(u32 from, u32 to)
{
    u32 fi = s_rf.fi; (void)fi;
    /* Render-to-texture pre-pass: make sure an offscreen RT resource exists for
     * every non-display surface targeted this frame (so draws binding it as a
     * texture can resolve to it below, whatever the op order). Depth surfaces
     * too, and the colour copy of each one a draw samples: both settle their
     * size here, before any draw's SRV window points at them. */
    for (u32 _d = from; _d < to && _d < MAX_DRAWS; _d++) {
        D3D12DrawRecord* dr = &s_d3d.draws[_d];
        if (dr->is_vp && dr->rt_off)
            off_rt_get(dr->rt_off, dr->rt_w, dr->rt_h, dr->rt_fmt);
        for (int _m = 0; _m < 3; _m++)
            if (dr->is_vp && dr->rt_mrt[_m])
                off_rt_get(dr->rt_mrt[_m], dr->rt_w, dr->rt_h, dr->rt_fmt);
        /* Only for a pass that uses its depth surface: a depth or stencil
         * test, the depth bounds test, or a depth clear. God of War:
         * Ascension names a zeta in every post pass that never touches it,
         * ~20 a frame, and those took the slots its real ones needed. */
        if (dr->is_vp && dr->zeta_off && (dr->rt_off || dr->no_color) &&
            (dr->is_clear ? (dr->clear_mask & 3u) != 0 : (dr->zs_key != 0 || dr->dbt)))
            off_ds_get(dr->zeta_off, dr->rt_w, dr->rt_h);
        if (dr->snap_ea)
            dr->snap_slot = (!dr->snap_src || off_rt_find(dr->snap_src) >= 0)
                          ? blit_snap_get(dr->snap_ea) : -1;
        alias_note(dr);
        for (int _u = 0; _u < VP_TEX_UNITS; _u++) dr->tex_ds[_u] = dr->tex_cv[_u] = dr->tex_dz[_u] = -1;   /* set below for draws */
        dr->srv_win = _d;
    }
    for (u32 _d = from; _d < to && _d < MAX_DRAWS; _d++) {
        const D3D12DrawRecord* dr = &s_d3d.draws[_d];
        if (!dr->is_vp || dr->is_clear) continue;
        /* tex_reads_depth needs the unit's tex_zok bit: only those units (alias_note set them above) */
        for (u32 zm = dr->tex_zok; zm; zm &= zm - 1) {
            const int _u = __builtin_ctz(zm);
            if (dr_tex_depth_fmt(dr, _u)) continue;   /* its depth plane, no colour copy */
            const int ds = tex_reads_depth(dr, _u) ? off_ds_find(dr->tex[_u].raw) : -1;
            if (ds < 0) continue;
            s_d3d.off_ds[ds].last_frame = s_d3d.frame_count;
            off_rt_get(dr->tex[_u].raw, s_d3d.off_ds[ds].w, s_d3d.off_ds[ds].h, 0x08u);
        }
    }

    /* Per-frame VP textures + guest-FP pipelines: for each VP draw, upload the
     * texture it had bound at submit time into a slot (plasma animates, so
     * contents re-upload every frame) and pre-build its FP PSO.
     * A texture whose offset matches an offscreen RT samples the RT directly
     * (tex_slot 1000+idx) -- no guest-memory upload. */
    double _pre0 = perf_on() ? perf_now() : 0.0;
    /* The record before, when this one may take its units: its texture state, depth-read mask and
     * program the same, every unit resolves the same within the pass, so the window and the
     * uploads already made serve both, and the targets, copies and conversions each draw refreshes
     * are named the same. Not a window the draw pass rewrites (pre_shareable): a depth plane's,
     * whose view becomes null while its surface is the draw's own, and one that reads the draw's
     * own target (RT_FEEDBACK's copy). God of War: Ascension's lit draws read targets, so this is
     * the rule that lets their runs share. PREPASS_SHARE=0 resolves every draw's. */
    static int pre_share = -1;
    if (pre_share < 0) { const char* e = getenv("PREPASS_SHARE"); pre_share = e ? atoi(e) != 0 : 1; }
    const D3D12DrawRecord* pp = NULL;
    for (u32 _d = from; _d < to && _d < MAX_DRAWS; _d++) {
        D3D12DrawRecord* dr = &s_d3d.draws[_d];
        if (!dr->is_vp || dr->is_clear) { pp = NULL; continue; }
        /* Debug: RTT_VIEWRT=<hex raw offset> makes display draws sample that
         * offscreen RT at t0 (the composite blit then shows it fullscreen). */
        { static const char* vr = (const char*)1;     /* hoisted: this runs per DRAW */
          if (vr == (const char*)1) vr = getenv("RTT_VIEWRT");
          if (vr && dr->rt_off == 0) {
              dr->tex[0].raw = (u32)strtoul(vr, NULL, 16);
              dr->tex[0].off = 0;
              dr->tex[0].set = 1;
          } }
        /* SCREENTEX=1: report draws that sample a DISPLAY-SIZED texture. On RSX
         * a title renders a reflection/refraction into a surface and then
         * samples it; our backend renders into a D3D backbuffer, so the guest
         * memory that sampler reads is never written and the texture comes back
         * empty. Finding which geometry does it is the first step to feeding it
         * the rendered image instead. */
        /* EMPTYTEX=1: which geometry binds a texture whose source is all zero.
         * An unresolved texture renders untextured, so this names the surfaces
         * affected instead of leaving it to guesswork. */
        { static int ed = -1;
          if (ed < 0) { const char* e = getenv("EMPTYTEX"); ed = e ? atoi(e) : 0; }
          extern uint8_t* vm_base;
          if (ed && vm_base) for (int _u = 0; _u < 4; _u++) {
              if (!dr->tex[_u].set || !dr->tex[_u].off) continue;
              u32 nz = 0, sz = dr->tex[_u].w * dr->tex[_u].h * 4u;
              for (u32 i2 = 0; i2 < sz && i2 < 0x20000u; i2 += 997)
                  if (vm_base[dr->tex[_u].off + i2]) { nz = 1; break; }
              if (!nz) {
                  static u32 seen[24]; static int ns = 0; int known = 0;
                  for (int k = 0; k < ns; k++) if (seen[k] == dr->fp_addr) known = 1;
                  if (!known && ns < 24) { seen[ns++] = dr->fp_addr;
                      /* Scan a window around the resolved address: if the pass
                       * that fills this surface ran but landed elsewhere, the
                       * data is nearby; if the whole window is zero it was never
                       * produced. */
                      /* Same offset through BOTH context DMAs: a texture the
                       * guest put in main memory but that we resolve as LOCAL
                       * reads as untouched VRAM, i.e. all zeros. */
                      { extern u32 cellGcmResolveLocated(int, u32);
                        u32 la = cellGcmResolveLocated(1, dr->tex[_u].raw);
                        u32 ma = cellGcmResolveLocated(0, dr->tex[_u].raw);
                        u32 ln = 0, mn = 0;
                        for (u32 i4 = 0; i4 < 0x20000u; i4 += 997) {
                            if (vm_base[la + i4]) ln++;
                            if (vm_base[ma + i4]) mn++;
                        }
                        fprintf(stderr, "[TEXLOC] raw=0x%08X LOCAL@0x%08X nz=%u  MAIN@0x%08X nz=%u%c",
                                dr->tex[_u].raw, la, ln, ma, mn, 10); }
                      { u32 base = dr->tex[_u].off & ~0xFFFFFu;
                        for (u32 w = 0; w < 0x400000u; w += 0x100000u) {
                            u32 nzc = 0;
                            for (u32 i3 = 0; i3 < 0x100000u; i3 += 1021)
                                if (vm_base[base + w + i3]) nzc++;
                            fprintf(stderr, "[EMPTYSCAN] 0x%08X..+1MB nonzero %u/1027%c",
                                    base + w, nzc, 10);
                        } }
                      fprintf(stderr, "[EMPTYTEX] fp=0x%X unit=%d raw=0x%08X res=0x%08X %ux%u"
                                      " fmt=0x%02X verts=%u vp=%u,%u %ux%u%c",
                              dr->fp_addr, _u, dr->tex[_u].raw, dr->tex[_u].off, dr->tex[_u].w,
                              dr->tex[_u].h, dr->tex[_u].fmt, dr->vertex_count,
                              dr->vp_x, dr->vp_y, dr->vp_w, dr->vp_h, 10); }
              }
          } }
        { static int sd = -1;
          if (sd < 0) { const char* e = getenv("SCREENTEX"); sd = e ? atoi(e) : 0; }
          if (sd) for (int _u = 0; _u < 4; _u++)
              if (dr->tex[_u].set && dr->tex[_u].w == s_d3d.width &&
                  dr->tex[_u].h == s_d3d.height) {
                  static u32 seen[16]; static int ns = 0; int known = 0;
                  for (int k = 0; k < ns; k++) if (seen[k] == dr->fp_addr) known = 1;
                  if (!known && ns < 16) { seen[ns++] = dr->fp_addr;
                      fprintf(stderr, "[SCREENTEX] unit=%d raw=0x%08X %ux%u fmt=0x%02X"
                                      " fp=0x%X verts=%u vp=%u,%u %ux%u%c",
                              _u, dr->tex[_u].raw, dr->tex[_u].w, dr->tex[_u].h,
                              dr->tex[_u].fmt, dr->fp_addr, dr->vertex_count,
                              dr->vp_x, dr->vp_y, dr->vp_w, dr->vp_h, 10); }
              } }
        int units_shared = 0;
        if (pre_share && pp && pp->fp_addr == dr->fp_addr && pp->fp_snap == dr->fp_snap &&
            pp->fp_units == dr->fp_units && pp->tex_zok == dr->tex_zok && !memcmp(pp->tex, dr->tex, sizeof dr->tex) &&
            !rec_reads_own(dr, pp->tex_rt)) {
            memcpy(dr->tex_rt, pp->tex_rt, sizeof dr->tex_rt);
            memcpy(dr->tex_ds, pp->tex_ds, sizeof dr->tex_ds);
            memcpy(dr->tex_cv, pp->tex_cv, sizeof dr->tex_cv);
            memcpy(dr->tex_dz, pp->tex_dz, sizeof dr->tex_dz);
            dr->srv_win = pp->srv_win;
            units_shared = 1;
            s_pre_shared++;
        }
        s_pre_draws++;
        if (!units_shared) {
        /* Fill this draw's t0-t15 SRV window (DRAW_SRV_BASE + d*16): each unit
         * resolves to an offscreen RT (sampled directly), an uploaded guest
         * texture, or a null SRV. */
        double _sv0 = perf_on() ? perf_now() : 0.0;
        /* Units 4 and up count only when the program samples them: state left
         * on them by earlier draws would otherwise cost an upload per draw. */
        u32 fp_units = 0xFFFFFFFFu;
        { int hi = 0;
          for (int _u = 4; _u < VP_TEX_UNITS; _u++) hi |= dr->tex[_u].set;
          extern uint8_t* vm_base;
          extern u32 cellGcmResolveLocated(int local, u32 offset);
          if (hi && dr->fp_addr && vm_base) {
              const u32 fo = cellGcmResolveLocated((dr->fp_addr & 3u) == 1, dr->fp_addr & ~3u);
              if (dr->fp_snap) fp_units = dr->fp_units;
              else if (fo != 0xFFFFFFFFu) fp_units = rsx_fp_texture_units(vm_base + fo, FP_MAX_BYTES);
          } }
        srvq_begin(_d);
        for (int _u = 0; _u < VP_TEX_UNITS; _u++) {
            u32 wslot = DRAW_SRV_BASE + _d * VP_TEX_UNITS + (u32)_u;
            dr->tex_rt[_u] = -1;
            if (_u >= 4 && !(fp_units & (1u << _u))) {
                srv_write(wslot, NULL, DXGI_FORMAT_R8G8B8A8_UNORM,
                          D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING);
                continue;
            }
            if (rsx_census_secs() && dr->tex[_u].set) rsx_census_bind(dr->tex[_u].fmt, dr->tex[_u].mips);
            /* A depth texture (a shadow map): the depth plane of the depth
             * surface at its offset, which the program compares
             * (_rsx_shadowN); with none there, a null view. */
            if (dr_tex_depth_fmt(dr, _u)) {
                const int ds = off_ds_find(dr->tex[_u].raw);
                if (ds >= 0) {
                    dr->tex_dz[_u] = ds;
                    s_d3d.off_ds[ds].last_frame = s_d3d.frame_count;
                    srv_write(wslot, s_d3d.off_ds[ds].res, DXGI_FORMAT_R24_UNORM_X8_TYPELESS,
                              D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING);
                } else {
                    srv_write(wslot, NULL, DXGI_FORMAT_R8G8B8A8_UNORM,
                              D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING);
                }
                continue;
            }
            /* A depth surface read as a texture: its colour copy (made in the
             * RT pre-pass), refreshed from the depth buffer just before this
             * draw (ds_convert). */
            if (tex_reads_depth(dr, _u)) {
                const int ds = off_ds_find(dr->tex[_u].raw);
                const int crt = ds >= 0 ? off_rt_find(dr->tex[_u].raw) : -1;
                if (crt >= 0) {
                    dr->tex_rt[_u] = crt;
                    dr->tex_ds[_u] = ds;
                    srv_write(wslot, s_d3d.off_rt[crt].res, (DXGI_FORMAT)s_d3d.off_rt[crt].dxgi,
                              D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING);
                    continue;
                }
            }
            /* What a blit snapshot took for this address. */
            if (dr->tex[_u].set) {
                const int bs = blit_snap_find(dr->tex[_u].off, dr->tex[_u].w, dr->tex[_u].h);
                if (bs >= 0) {
                    srv_write(wslot, s_bsnap[bs].res, DXGI_FORMAT_R8G8B8A8_UNORM,
                              D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING);
                    continue;
                }
            }
            /* Display-sized sampler source -> the rendered frame, when it is
             * not an offscreen target and the guest buffer behind it is empty
             * (never written by the CPU or a copy). God of War: Ascension
             * samples its 1280x720 targets (the scene, the depth copies) all
             * through its post chain, and each of them was handed the previous
             * final frame; and a copy of a display the CPU filled (its opening
             * video, copied to main memory) is read from guest memory. */
            int guest_nonzero = 0;
            if (dr->tex[_u].set && off_rt_find(tex_rt_key(dr->tex[_u].raw, dr->tex[_u].off)) >= 0) guest_nonzero = 1;
            else if (dr->tex[_u].set && s_screen_copy && dr->tex[_u].off &&
                dr->tex[_u].w == s_d3d.width && dr->tex[_u].h == s_d3d.height) {
                extern uint8_t* vm_base;
                const u32 sz = dr->tex[_u].w * dr->tex[_u].h * 4u;
                for (u32 i6 = 0; i6 < sz && vm_base; i6 += 4099)
                    if (vm_base[dr->tex[_u].off + i6]) { guest_nonzero = 1; break; }
            }
            if (dr->tex[_u].set && s_screen_copy && !guest_nonzero &&
                dr->tex[_u].w == s_d3d.width && dr->tex[_u].h == s_d3d.height) {
                static int en = -1;
                if (en < 0) { const char* e = getenv("SCREEN_AS_TEX"); en = e ? atoi(e) : 1; }
                if (en) {
                    { static int _n = 0; if (PS3_LOG_FIRST(_n, 3))
                        fprintf(stderr, "[SCREENTEX] bound frame copy at unit %d for"
                                        " fp=0x%X (%ux%u)%c", _u, dr->fp_addr,
                                dr->tex[_u].w, dr->tex[_u].h, 10); }
                    srv_write(wslot, s_screen_copy, DXGI_FORMAT_R8G8B8A8_UNORM,
                              D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING);
                    continue;
                }
            }
            /* A sampler whose guest buffer was never written, at the size of a
             * reduced-viewport pass we captured: that pass IS its producer. */
            if (dr->tex[_u].set && dr->tex[_u].off && s_subvp_n) {
                int hit = -1;
                for (int i = 0; i < s_subvp_n; i++)
                    if (s_subvp[i].res && s_subvp[i].w == dr->tex[_u].w &&
                        s_subvp[i].h == dr->tex[_u].h) { hit = i; break; }
                if (hit >= 0) {
                    extern uint8_t* vm_base;
                    u32 nz = 0, sz = dr->tex[_u].w * dr->tex[_u].h * 4u;
                    for (u32 i5 = 0; i5 < sz && i5 < 0x20000u; i5 += 997)
                        if (vm_base[dr->tex[_u].off + i5]) { nz = 1; break; }
                    if (!nz) {
                        { static int _n = 0; if (getenv("SUBVP_DBG") && PS3_LOG_FIRST(_n, 8))
                            fprintf(stderr, "[SUBVP] unit %d fp=0x%X <- captured %ux%u%c",
                                    _u, dr->fp_addr, dr->tex[_u].w, dr->tex[_u].h, 10); }
                        srv_write(wslot, s_subvp[hit].res, DXGI_FORMAT_R8G8B8A8_UNORM,
                                  D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING);
                        continue;
                    }
                }
            }
            if (dr->tex[_u].set) {
                int rt = off_rt_find(tex_rt_key(dr->tex[_u].raw, dr->tex[_u].off));
                if (rt >= 0) {
                    dr->tex_rt[_u] = rt;
                    /* An RGBA8 target read as Y16_X16_FLOAT: the bytes are two
                     * big-endian halves (God of War: Ascension's luminance
                     * chain). Sampled through a converted copy, refreshed
                     * before each draw that reads it (y16_convert), at the
                     * texture's size (y16_exact_on). The chain's last pass
                     * point-samples its 8x9 input on texel edges (u = 0.375);
                     * scaled by 8/320 onto a target-sized copy, those fell a
                     * hair short and took the texel to the left, columns 0
                     * and 2 for the RSX's 1 and 3, and the exposure the PPU
                     * takes from its result came out high. */
                    if ((dr->tex[_u].fmt & 0x9F) == 0x9F &&
                        s_d3d.off_rt[rt].dxgi == (u32)DXGI_FORMAT_R8G8B8A8_UNORM) {
                        s_d3d.off_rt[rt].used = 1;
                        const u32 tw = dr->tex[_u].w, th = dr->tex[_u].h;
                        const int cv = (y16_exact_on() && tw && th && tw <= 4096u && th <= 4096u)
                            ? off_rt_get(y16_copy_key(_u, tw, th), tw, th, 0x0Bu)
                            : off_rt_get(dr->tex[_u].raw | Y16_RT_TAG,
                                         s_d3d.off_rt[rt].w, s_d3d.off_rt[rt].h, 0x0Bu);
                        if (cv >= 0 && cv != rt) {
                            dr->tex_cv[_u] = cv;
                            srv_write(wslot, s_d3d.off_rt[cv].res, DXGI_FORMAT_R16G16B16A16_FLOAT,
                                      D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING);
                            continue;
                        }
                    }
                    srv_write(wslot, s_d3d.off_rt[rt].res, (DXGI_FORMAT)s_d3d.off_rt[rt].dxgi,
                              D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING);
                    continue;
                }
                u32 _bf = dr->tex[_u].fmt & 0x9F;
                /* The formats d3d12_bind_texture records. A half-float one
                 * that is not a render target found above is guest data too. */
                if (dr->tex[_u].off &&
                    (_bf == 0x81 || _bf == 0x85 || _bf == 0x8B ||
                     (_bf >= 0x86 && _bf <= 0x88) || _bf == 0x9A ||
                     _bf == 0x94 || _bf == 0x95 || _bf == 0x9C || _bf == 0x9F)) {
                    double _tt = perf_on() ? perf_now() : 0.0;
                    int _cube = dr->tex[_u].cube && dr_cube_tex_on();
                    const int _vol = !_cube && dr_unit_vol(dr, _u);
                    const double _ts0 = tex_stats_frames() ? perf_now() : 0.0;
                    const double _hl0 = rsx_census_secs() ? perf_now() : 0.0;
                    int ts = vp_upload_tex_slot(dr->tex[_u].off, dr->tex[_u].w,
                                                dr->tex[_u].h, dr->tex[_u].fmt,
                                                _cube ? 1 : _vol ? tex_shape_vol(dr->tex[_u].depth) : 0,
                                                dr->tex[_u].mips);
                    if (_ts0 > 0.0) tex_stats_note(perf_now() - _ts0);
                    if (_hl0 > 0.0 && (s_ts_out == TS_NEW || s_ts_out == TS_CHANGED)) {
                        s_hl_tex_n++; s_hl_tex_s += perf_now() - _hl0;
                    }
                    if (perf_on()) { s_perf_tex += perf_now() - _tt; s_perf_ntex++;
                        s_perf_texbytes += (u64)dr->tex[_u].w * dr->tex[_u].h * 4u; }
                    /* Both forms of the offset are in hand here; the filters
                     * compare the BOUND one. */
                    if (s_duck_off && dr->tex[_u].off == s_duck_off)
                        s_duck_raw = dr->tex[_u].raw;
                    if (ts >= 0) {
                        /* The upload's own format: an X16 font atlas bound
                         * as R8 over an R16 resource sampled nothing. */
                        DXGI_FORMAT sf = vp_tex_dxgi(dr->tex[_u].fmt);
                        srv_write_tex(wslot, ts, sf,
                                  (_bf == 0x81) ? 0x1000
                                                : rsx_remap_to_d3d(dr->tex[_u].ctrl1, _bf),
                                  _cube ? 1 : _vol ? 2 : 0);
                        continue;
                    }
                }
            }
            /* TEX_BINDDBG=<N>: a unit that ends up with a NULL SRV. The draw is
             * still recorded, lit and transformed -- it just samples nothing,
             * which renders as a solid black object. */
            { static int cap = -1, n = 0;
              if (cap < 0) { const char* e = getenv("TEX_BINDDBG"); cap = e ? atoi(e) : 0; }
              if (cap && n < cap && dr->tex[_u].set) { n++;
                fprintf(stderr, "[TEXBIND] null SRV unit=%d raw=0x%X off=0x%X %ux%u fmt=0x%02X%c",
                        _u, dr->tex[_u].raw, dr->tex[_u].off, dr->tex[_u].w,
                        dr->tex[_u].h, dr->tex[_u].fmt, 10); } }
            if (rsx_census_secs() && dr->tex[_u].set) rsx_census_null(dr->tex[_u].fmt, dr->tex[_u].off);
            /* Null, but of the kind the program declares for the unit. */
            srv_write_ex(wslot, NULL, DXGI_FORMAT_R8G8B8A8_UNORM,
                         D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING, dr_unit_view(dr, _u));
        }
        srvq_commit(dr, _d);
        if (perf_on()) s_perf_srv += perf_now() - _sv0;
        pp = pre_shareable(dr) ? dr : NULL;
        }
        double _ps0 = perf_on() ? perf_now() : 0.0;
        /* Queue every build the frame needs before waiting for any: the draw
         * pass below waits, within the budget, for the ones still running. */
        s_sj_nowait = 1;
        const int _m0 = s_perf_pso_miss;
        const double _mt0 = rsx_census_secs() ? perf_now() : 0.0;
        if (dr->fp_addr) vp_get_fp_pso(dr->vs_idx, dr->fp_addr, dr->blend_key,
                                       dr_num_rts(dr),
                                       dr->rt_off ? rsx_surface_dxgi(dr->rt_fmt)
                                                  : DXGI_FORMAT_R8G8B8A8_UNORM,
                                       dr->fp_ctrl, dr->cmask, dr->cull, 0u, dr->raw,
                                       dr->zs_key, 0u, 0u, dr);
        if (_mt0 > 0.0 && s_perf_pso_miss != _m0) { s_hl_miss_n++; s_hl_miss_s += perf_now() - _mt0; }
        s_sj_nowait = 0;
        if (perf_on()) s_perf_pso += perf_now() - _ps0;
    }

    if (perf_on()) s_perf_pre += perf_now() - _pre0;
    s_rf.pre_done = to;
}

/* The display target: to RENDER_TARGET, bound, cleared. */
static void rf_target(void)
{
    u32 fi = s_rf.fi;
    /* Transition render target to RENDER_TARGET state */
    D3D12_RESOURCE_BARRIER barrier = {0};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = s_d3d.render_targets[fi];
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    s_d3d.cmd_list->lpVtbl->ResourceBarrier(s_d3d.cmd_list, 1, &barrier);

    /* Get RTV handle for current frame */
    D3D12_CPU_DESCRIPTOR_HANDLE rtv_handle;
    s_d3d.rtv_heap->lpVtbl->GetCPUDescriptorHandleForHeapStart(s_d3d.rtv_heap, &rtv_handle);
    rtv_handle.ptr += fi * s_d3d.rtv_descriptor_size;

    /* Get DSV handle (single depth buffer shared across frames) */
    D3D12_CPU_DESCRIPTOR_HANDLE dsv_handle;
    s_d3d.dsv_heap->lpVtbl->GetCPUDescriptorHandleForHeapStart(s_d3d.dsv_heap, &dsv_handle);

    /* Set render target + depth */
    s_d3d.cmd_list->lpVtbl->OMSetRenderTargets(s_d3d.cmd_list, 1, &rtv_handle, FALSE, &dsv_handle);

    /* Clear color and depth */
    s_d3d.cmd_list->lpVtbl->ClearRenderTargetView(
        s_d3d.cmd_list, rtv_handle, s_d3d.clear_color, 0, NULL);
    s_d3d.cmd_list->lpVtbl->ClearDepthStencilView(
        s_d3d.cmd_list, dsv_handle,
        D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL,
        1.0f, 0, 0, NULL);

    /* Set viewport and scissor */
    D3D12_VIEWPORT viewport = {0, 0, (float)s_d3d.pw, (float)s_d3d.ph, 0.0f, 1.0f};
    D3D12_RECT scissor = {0, 0, (LONG)s_d3d.pw, (LONG)s_d3d.ph};
    s_d3d.cmd_list->lpVtbl->RSSetViewports(s_d3d.cmd_list, 1, &viewport);
    s_d3d.cmd_list->lpVtbl->RSSetScissorRects(s_d3d.cmd_list, 1, &scissor);

    s_rf.rtv_handle = rtv_handle;
    s_rf.dsv_handle = dsv_handle;
    (void)barrier;
}

/* The legacy pass: every record that is not a VP draw. */
static void rf_legacy(void)
{
    u32 fi = s_rf.fi; (void)fi;
    D3D12_CPU_DESCRIPTOR_HANDLE rtv_handle = s_rf.rtv_handle, dsv_handle = s_rf.dsv_handle;
    (void)rtv_handle; (void)dsv_handle;
    /* Bind pipeline state and push MVP if anything to draw */
    if (s_d3d.pipeline_ready && s_d3d.draw_count > 0) {
        s_d3d.cmd_list->lpVtbl->SetGraphicsRootSignature(s_d3d.cmd_list, s_d3d.root_signature);
        s_d3d.cmd_list->lpVtbl->IASetVertexBuffers(s_d3d.cmd_list, 0, 1, &s_d3d.vb_view);

        /* Make the atlas SRV heap current and bind its table (t0) for textured
         * draws. Safe to set even if no draw is textured. */
        if (s_d3d.tex_ready && s_d3d.srv_heap) {
            ID3D12DescriptorHeap* heaps[] = { s_d3d.srv_heap };
            s_d3d.cmd_list->lpVtbl->SetDescriptorHeaps(s_d3d.cmd_list, 1, heaps);
            D3D12_GPU_DESCRIPTOR_HANDLE gh;
            s_d3d.srv_heap->lpVtbl->GetGPUDescriptorHandleForHeapStart(s_d3d.srv_heap, &gh);
            s_d3d.cmd_list->lpVtbl->SetGraphicsRootDescriptorTable(s_d3d.cmd_list, 1, gh);
        }

        /* Push the MVP matrix from RSX vertex constants slots 0..3.
         * If the game hasn't written any constants (e.g. placeholder data
         * already in clip space), fall back to identity. */
        float mvp[16];
        const rsx_state* st = s_d3d.current_rsx_state;
        int have_mvp = 0;
        if (st) {
            for (u32 r = 0; r < 4; r++) {
                for (u32 c = 0; c < 4; c++) {
                    float v = st->vertex_constants[r][c];
                    mvp[r * 4 + c] = v;
                    if (v != 0.0f) have_mvp = 1;
                }
            }
        }
        if (!have_mvp) {
            memset(mvp, 0, sizeof(mvp));
            mvp[0] = mvp[5] = mvp[10] = mvp[15] = 1.0f; /* identity */
        }
        s_d3d.cmd_list->lpVtbl->SetGraphicsRoot32BitConstants(
            s_d3d.cmd_list, 0 /*root param 0*/, 16, mvp, 0);

        /* D3D12_IQ=1: dump exactly what the GPU is about to see -- the MVP root
         * constants and the first uploaded host vertices. The legacy path draws
         * with no validation errors yet produces zero pixels, so the geometry
         * must be degenerate/off-screen post-VS. */
        { static int _vd = -1;
          if (_vd < 0) { const char* e = getenv("D3D12_IQ"); _vd = e ? 1 : 0; }
          static int _n = 0;
          if (_vd && _n < 3) {
            _n++;
            fprintf(stderr, "[D3D12-DBG] have_mvp=%d draw_count=%u vb_offset=%u\n",
                    have_mvp, s_d3d.draw_count, s_d3d.vb_offset);
            fprintf(stderr, "[D3D12-DBG] mvp rows: [%.2f %.2f %.2f %.2f][%.2f %.2f %.2f %.2f]"
                            "[%.2f %.2f %.2f %.2f][%.2f %.2f %.2f %.2f]\n",
                    mvp[0],mvp[1],mvp[2],mvp[3], mvp[4],mvp[5],mvp[6],mvp[7],
                    mvp[8],mvp[9],mvp[10],mvp[11], mvp[12],mvp[13],mvp[14],mvp[15]);
            if (s_d3d.vb_mapped) {
                /* host vertex = 9 floats (pos3 + col4 + uv2), VERTEX_STRIDE=36 */
                const float* bv = (const float*)s_d3d.vb_mapped;
                for (int k = 0; k < 3; k++) {
                    const float* v = bv + k * 9;
                    fprintf(stderr, "[D3D12-DBG]  hostvert[%d] pos=(%.3f,%.3f,%.3f) col=(%.2f,%.2f,%.2f,%.2f)\n",
                            k, v[0], v[1], v[2], v[3], v[4], v[5], v[6]);
                }
            }
            for (u32 d = 0; d < s_d3d.draw_count && d < 3; d++) {
                const D3D12DrawRecord* dr = &s_d3d.draws[d];
                fprintf(stderr, "[D3D12-DBG]  draw[%u] is_vp=%d is_clear=%d topo=%u cnt=%u vbofs=%u startv=%u rt=%u\n",
                        d, dr->is_vp, dr->is_clear, dr->topology, dr->vertex_count,
                        dr->vb_byte_offset, dr->vb_byte_offset / VERTEX_STRIDE, dr->rt_off);
            }
            fflush(stderr);
          } }

        /* Replay each recorded draw with its own primitive topology and
         * the matching PSO class (triangle / line / point). The PSO class
         * must match the topology or D3D12 rejects the draw. */
        u32 last_topo = 0xFFFFFFFFu;
        ID3D12PipelineState* last_pso = NULL;
        u32 draws = s_d3d.draw_count;
        if (draws > MAX_DRAWS) draws = MAX_DRAWS;
        for (u32 d = 0; d < draws; d++) {
            const D3D12DrawRecord* dr = &s_d3d.draws[d];
            if (dr->is_vp) continue; /* drawn by the VP pass below */

            /* Select PSO: textured triangles (dbgfont) use the atlas PSO;
             * otherwise pick by topology class. */
            ID3D12PipelineState* target_pso = s_d3d.pipeline_state; /* default triangle */
            if (dr->textured && s_d3d.tex_ready && s_d3d.pipeline_state_tex) {
                target_pso = s_d3d.pipeline_state_tex;
            } else if (dr->topology == RSX_TOPOLOGY_POINTS) {
                target_pso = s_d3d.pipeline_state_points
                             ? s_d3d.pipeline_state_points : s_d3d.pipeline_state;
            } else if (dr->topology == RSX_TOPOLOGY_LINES ||
                       dr->topology == RSX_TOPOLOGY_LINE_STRIP) {
                target_pso = s_d3d.pipeline_state_lines
                             ? s_d3d.pipeline_state_lines : s_d3d.pipeline_state;
            }
            if (target_pso != last_pso) {
                s_d3d.cmd_list->lpVtbl->SetPipelineState(s_d3d.cmd_list, target_pso);
                last_pso = target_pso;
            }
            if (dr->topology != last_topo) {
                s_d3d.cmd_list->lpVtbl->IASetPrimitiveTopology(s_d3d.cmd_list, topo_to_d3d(dr->topology));
                last_topo = dr->topology;
            }
            u32 start_vert = dr->vb_byte_offset / VERTEX_STRIDE;
            s_d3d.cmd_list->lpVtbl->DrawInstanced(
                s_d3d.cmd_list, dr->vertex_count, 1, start_vert, 0);
        }
    }

}

/* The VP pass's bindings, and its recording state. */
static void rf_vp_begin(void)
{
    u32 fi = s_rf.fi; (void)fi;
    D3D12_CPU_DESCRIPTOR_HANDLE rtv_handle = s_rf.rtv_handle, dsv_handle = s_rf.dsv_handle;
    (void)rtv_handle; (void)dsv_handle;
    s_rf.vp_on = 0;
    /* VP pass: real decompiled vertex program + atlas alpha-test PS. Feeds raw
     * float4 attrib0 from vp_vb and the vp_c[] constant bank. */
    if (s_d3d.vp_ready && s_d3d.draw_count > 0) {
        int any = 0;
        for (u32 d = 0; d < s_d3d.draw_count && d < MAX_DRAWS; d++)
            if (s_d3d.draws[d].is_vp) { any = 1; break; }
        /* Textured geometry (dbgfont atlas) uses the sampling PS; untextured 3D
         * (vkcube) uses the colour-only PS. Fall back to whichever exists. */
        ID3D12PipelineState* vpso =
            (s_d3d.tex_ready && s_d3d.pipeline_state_vp) ? s_d3d.pipeline_state_vp
                                                         : s_d3d.pipeline_state_vp_color;
        if (!vpso) vpso = s_d3d.pipeline_state_vp;
        { static int cap = -1, n = 0;
          if (cap < 0) { const char* e = getenv("VP_SUBMIT"); cap = e ? atoi(e) : 0; }
          if (cap && n < cap) { n++;
            u32 nvp = 0, nclr = 0;
            for (u32 d2 = 0; d2 < s_d3d.draw_count && d2 < MAX_DRAWS; d2++) {
                if (s_d3d.draws[d2].is_vp)    nvp++;
                if (s_d3d.draws[d2].is_clear) nclr++;
            }
            fprintf(stderr, "[VPPASS] records=%u is_vp=%u clears=%u any=%d vpso=%p rootsig=%p\n",
                    s_d3d.draw_count, nvp, nclr, any,
                    (void*)vpso, (void*)s_d3d.vp_root_sig); } }
        if (any && vpso) {
            s_d3d.cmd_list->lpVtbl->SetGraphicsRootSignature(s_d3d.cmd_list, s_d3d.vp_root_sig);
            s_d3d.cmd_list->lpVtbl->SetPipelineState(s_d3d.cmd_list, vpso);
            s_d3d.cmd_list->lpVtbl->IASetPrimitiveTopology(s_d3d.cmd_list, D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            s_d3d.cmd_list->lpVtbl->SetGraphicsRootConstantBufferView(s_d3d.cmd_list, 0,
                s_d3d.vp_cb->lpVtbl->GetGPUVirtualAddress(s_d3d.vp_cb));
            ID3D12DescriptorHeap* heaps[] = { s_d3d.srv_heap, s_d3d.samp_heap };
            s_d3d.cmd_list->lpVtbl->SetDescriptorHeaps(s_d3d.cmd_list, s_d3d.samp_heap ? 2 : 1, heaps);
            D3D12_GPU_DESCRIPTOR_HANDLE gh;
            s_d3d.srv_heap->lpVtbl->GetGPUDescriptorHandleForHeapStart(s_d3d.srv_heap, &gh);
            s_d3d.cmd_list->lpVtbl->SetGraphicsRootDescriptorTable(s_d3d.cmd_list, 1, gh);
            int samp_bound[VP_TEX_UNITS];
            for (int _s = 0; _s < VP_TEX_UNITS; _s++) samp_bound[_s] = -1;
            D3D12_VERTEX_BUFFER_VIEW vbv;
            vbv.BufferLocation = s_d3d.vp_vb->lpVtbl->GetGPUVirtualAddress(s_d3d.vp_vb)
                               + (u64)s_d3d.vp_parity * MAX_VERTICES * 256;
            vbv.SizeInBytes    = MAX_VERTICES * 256;
            vbv.StrideInBytes  = 256;   /* 16 float4 attrib slots */
            s_d3d.cmd_list->lpVtbl->IASetVertexBuffers(s_d3d.cmd_list, 0, 1, &vbv);
            D3D12_GPU_DESCRIPTOR_HANDLE gh_base;
            s_d3d.srv_heap->lpVtbl->GetGPUDescriptorHandleForHeapStart(s_d3d.srv_heap, &gh_base);
            int cur_rt = -1;                       /* target A: -1 = backbuffer */
            int ib_bound = 0;                      /* vp_ib bound for indexed records */
            int raw_bound = 0;                     /* vp_vb bound as raw bytes (t8) */
            float vp_last_w = 0, vp_last_h = 0;    /* for DRAWARGS */
            float dvp_x = 0, dvp_y = 0, dvp_w = 0, dvp_h = 0;  /* per-draw, for DRAWARGS */
            int cur_m[3] = {-1, -1, -1};           /* MRT B/C/D: -1 = unbound   */
            int cur_z = -1, cur_nc = 0, force_rebind = 0;   /* OffDS bound (-1 = shared buffer) */
            D3D12_CPU_DESCRIPTOR_HANDLE cur_dsv = dsv_handle;
            int seen_small_vp = 0, captured = 0;
            s_rf.vp_on = 1;
            s_rf.vpso = vpso;
            s_rf.gh_base = gh_base;
            memcpy(s_rf.samp_bound, samp_bound, sizeof samp_bound);
            s_rf.cur_rt = cur_rt; s_rf.ib_bound = ib_bound; s_rf.raw_bound = raw_bound;
            s_rf.vp_last_w = vp_last_w; s_rf.vp_last_h = vp_last_h;
            s_rf.dvp_x = dvp_x; s_rf.dvp_y = dvp_y; s_rf.dvp_w = dvp_w; s_rf.dvp_h = dvp_h;
            memcpy(s_rf.cur_m, cur_m, sizeof cur_m);
            s_rf.cur_z = cur_z; s_rf.cur_nc = cur_nc; s_rf.force_rebind = force_rebind;
            s_rf.cur_dsv = cur_dsv;
            s_rf.seen_small_vp = seen_small_vp; s_rf.captured = captured;
        }
    }
}

/* Record VP draws [from, to); returns where it stopped (ahead: at a draw whose
 * pipeline is still building). */
static u32 rf_vp_draws(u32 from, u32 to, int ahead)
{
    if (!s_rf.vp_on) return to;
    u32 fi = s_rf.fi; (void)fi;
    D3D12_CPU_DESCRIPTOR_HANDLE rtv_handle = s_rf.rtv_handle, dsv_handle = s_rf.dsv_handle;
    (void)rtv_handle; (void)dsv_handle;
    ID3D12PipelineState* vpso = s_rf.vpso;
    D3D12_GPU_DESCRIPTOR_HANDLE gh_base = s_rf.gh_base;
    int samp_bound[VP_TEX_UNITS]; memcpy(samp_bound, s_rf.samp_bound, sizeof samp_bound);
    int cur_rt = s_rf.cur_rt, ib_bound = s_rf.ib_bound, raw_bound = s_rf.raw_bound;
    float vp_last_w = s_rf.vp_last_w, vp_last_h = s_rf.vp_last_h;
    float dvp_x = s_rf.dvp_x, dvp_y = s_rf.dvp_y, dvp_w = s_rf.dvp_w, dvp_h = s_rf.dvp_h;
    int cur_m[3]; memcpy(cur_m, s_rf.cur_m, sizeof cur_m);
    int cur_z = s_rf.cur_z, cur_nc = s_rf.cur_nc, force_rebind = s_rf.force_rebind;
    D3D12_CPU_DESCRIPTOR_HANDLE cur_dsv = s_rf.cur_dsv;
    int seen_small_vp = s_rf.seen_small_vp, captured = s_rf.captured;
    double _rec0 = perf_on() ? perf_now() : 0.0;
    u32 d;
            for (d = from; d < to && d < MAX_DRAWS; d++) {
                const D3D12DrawRecord* dr = &s_d3d.draws[d];
                if (!dr->is_vp) continue;
                if (dr->snap_ea) { blit_snap_take(dr, fi); continue; }
                if (dr->is_blit) { blit_rt_copy(dr); force_rebind = 1; continue; }
                if (dr->flip_key) {
                    const int fslot = off_rt_find(dr->flip_key);
                    s_fo_batch++;
                    if (fslot < 0 || s_d3d.off_rt[fslot].dxgi != DXGI_FORMAT_R8G8B8A8_UNORM) {
                        s_fo_failed++;
                        if (flip_op_dbg())
                            fprintf(stderr, "[flip-op] %.2f s: op %u key 0x%08X not copied: %s %d%c",
                                    flip_op_secs(), d, dr->flip_key,
                                    fslot < 0 ? "no target, slot" : "its target's format is dxgi",
                                    fslot < 0 ? fslot : (int)s_d3d.off_rt[fslot].dxgi, 10);
                    }
                    composite_copy(fi, fslot, dr->flip_key);
                    s_flip_shown = 1;
                    force_rebind = 1;
                    continue;
                }
                if (dr->zq_op) { zq_op_replay(dr); continue; }
                if (dr->wide_cam) s_wide_scene = 1;
                if (dr->is_clear && dr->clear_mask == 1u && (dr->rt_off & DISP_RT_TAG)) fg_marker(dr);
                if (!dr->is_clear) zq_before_draw();
                /* The targets and depth surface this draw binds (as the retarget
                 * below picks them): a copy of one of those is not refreshed here
                 * -- not of whatever the draw before left bound. */
                const int own_rt = (dr->rt_off && !dr->no_color) ? off_rt_find(dr->rt_off) : -1;
                int own_m[3];
                for (int _m = 0; _m < 3; _m++)
                    own_m[_m] = (dr->rt_mrt[_m] && !dr->no_color) ? off_rt_find(dr->rt_mrt[_m]) : -1;
                const int own_z = (dr->zeta_off && (own_rt >= 0 || dr->no_color)) ? off_ds_find(dr->zeta_off) : -1;
                for (int _u = 0; _u < VP_TEX_UNITS; _u++) {
                    if (dr->is_clear || dr->tex_cv[_u] < 0 || dr->tex_rt[_u] < 0 ||
                        dr->tex_rt[_u] == own_rt || dr->tex_rt[_u] == own_m[0] ||
                        dr->tex_rt[_u] == own_m[1] || dr->tex_rt[_u] == own_m[2])
                        continue;
                    y16_convert(dr->tex_rt[_u], dr->tex_cv[_u], dr->tex[_u].w, dr->tex[_u].h);
                    s_d3d.cmd_list->lpVtbl->SetGraphicsRootSignature(s_d3d.cmd_list, s_d3d.vp_root_sig);
                    { ID3D12DescriptorHeap* hp2[] = { s_d3d.srv_heap, s_d3d.samp_heap };
                      s_d3d.cmd_list->lpVtbl->SetDescriptorHeaps(s_d3d.cmd_list, s_d3d.samp_heap ? 2 : 1, hp2); }
                    for (int _s = 0; _s < VP_TEX_UNITS; _s++) samp_bound[_s] = -1;
                    raw_bound = 0;
                    force_rebind = 1;
                }
                for (int _u = 0; _u < VP_TEX_UNITS; _u++) {
                    const int ds = dr->tex_ds[_u];
                    if (ds < 0 || ds == own_z || s_d3d.off_ds[ds].conv_gen == s_d3d.off_ds[ds].gen)
                        continue;
                    ds_convert(ds, dr->tex_rt[_u]);
                    s_d3d.cmd_list->lpVtbl->SetGraphicsRootSignature(s_d3d.cmd_list, s_d3d.vp_root_sig);
                    { ID3D12DescriptorHeap* hp2[] = { s_d3d.srv_heap, s_d3d.samp_heap };
                      s_d3d.cmd_list->lpVtbl->SetDescriptorHeaps(s_d3d.cmd_list, s_d3d.samp_heap ? 2 : 1, hp2); }
                    for (int _s = 0; _s < VP_TEX_UNITS; _s++) samp_bound[_s] = -1;
                    raw_bound = 0;
                    force_rebind = 1;
                }
                /* The reduced-viewport passes (this title's reflection, drawn
                 * into a region of the same target) finish here -- snapshot
                 * before the full-screen scene overwrites them. */
                if (!dr->is_clear) {
                    if (dr->vp_w && dr->vp_w < s_d3d.width) {
                        seen_small_vp = 1;
                        subvp_note(dr->vp_x, dr->vp_y, dr->vp_w, dr->vp_h);
                    }
                    else if (seen_small_vp && !captured &&
                             dr->vp_w == s_d3d.width && dr->rt_off == 0) {
                        captured = 1;
                        screen_copy_capture(fi);
                        subvp_capture(fi);
                        s_d3d.cmd_list->lpVtbl->OMSetRenderTargets(
                            s_d3d.cmd_list, 1, &rtv_handle, FALSE, &dsv_handle);
                    }
                }
                /* Render-to-texture: retarget when this op's surfaces differ.
                 * A pass with a depth surface binds its OffDS; one without
                 * shares a single depth buffer, cleared per switch. */
                int want  = (dr->rt_off && !dr->no_color) ? off_rt_find(dr->rt_off) : -1;
                /* Remember where geometry actually went: off_rt_find() matches
                 * on offset alone and slots share offsets at different sizes,
                 * so the composite cannot just look the source up by address. */
                if (want >= 0) { s_last_drawn_slot = want; s_last_drawn_off = dr->rt_off; }
                int wantm[3];
                for (int _m = 0; _m < 3; _m++)
                    wantm[_m] = (dr->rt_mrt[_m] && !dr->no_color) ? off_rt_find(dr->rt_mrt[_m]) : -1;
                /* The draw's own depth surface, or the shared buffer for a pass
                 * without one (and for display targets, as before). */
                const int wantz = (dr->zeta_off && (want >= 0 || dr->no_color))
                                ? off_ds_find(dr->zeta_off) : -1;
                if (want != cur_rt || wantm[0] != cur_m[0] ||
                    wantm[1] != cur_m[1] || wantm[2] != cur_m[2] ||
                    wantz != cur_z || dr->no_color != cur_nc || force_rebind) {
                    D3D12_CPU_DESCRIPTOR_HANDLE rh[4];
                    UINT nrt = 1;
                    rh[0] = rtv_handle;
                    D3D12_VIEWPORT vp = {0, 0, (float)s_d3d.width, (float)s_d3d.height, 0.0f, 1.0f};
                    if (want >= 0) {
                        off_rt_transition(want, D3D12_RESOURCE_STATE_RENDER_TARGET);
                        rh[0] = off_rt_rtv(want);
                        vp.Width  = (float)s_d3d.off_rt[want].w;
                        vp.Height = (float)s_d3d.off_rt[want].h;
                    }
                    for (int _m = 0; _m < 3 && wantm[_m] >= 0; _m++) {
                        off_rt_transition(wantm[_m], D3D12_RESOURCE_STATE_RENDER_TARGET);
                        rh[nrt++] = off_rt_rtv(wantm[_m]);
                    }
                    cur_dsv = dsv_handle;
                    if (wantz >= 0) {
                        off_ds_transition(wantz, D3D12_RESOURCE_STATE_DEPTH_WRITE);
                        cur_dsv = off_ds_dsv(wantz);
                        if (dr->no_color) {
                            vp.Width  = (float)s_d3d.off_ds[wantz].w;
                            vp.Height = (float)s_d3d.off_ds[wantz].h;
                        }
                    }
                    vp_last_w = vp.Width; vp_last_h = vp.Height;
                    gps_pass(d, want >= 0 ? s_d3d.off_rt[want].off : 0, (u32)vp.Width, (u32)vp.Height, dr->fp_addr);
                    { float rx, ry;
                      rs_target(want, wantz, dr->no_color, &rx, &ry);
                      vp.Width *= rx; vp.Height *= ry; }
                    D3D12_RECT sc = {0, 0, (LONG)vp.Width, (LONG)vp.Height};
                    s_d3d.cmd_list->lpVtbl->OMSetRenderTargets(s_d3d.cmd_list, dr->no_color ? 0 : nrt,
                                                               dr->no_color ? NULL : rh, FALSE, &cur_dsv);
                    s_d3d.cmd_list->lpVtbl->RSSetViewports(s_d3d.cmd_list, 1, &vp);
                    s_d3d.cmd_list->lpVtbl->RSSetScissorRects(s_d3d.cmd_list, 1, &sc);
                    if (wantz >= 0) {
                        /* A depth surface keeps its contents across passes; the
                         * guest clears it when it wants it cleared. */
                        if (!s_d3d.off_ds[wantz].cleared) {
                            s_d3d.cmd_list->lpVtbl->ClearDepthStencilView(s_d3d.cmd_list, cur_dsv,
                                D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL, 1.0f, 0, 0, NULL);
                            s_d3d.off_ds[wantz].cleared = 1;
                            s_d3d.off_ds[wantz].gen++;
                        }
                    } else if (!force_rebind) {
                        s_d3d.cmd_list->lpVtbl->ClearDepthStencilView(s_d3d.cmd_list, dsv_handle,
                            D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL, 1.0f, 0, 0, NULL);
                    }
                    cur_z = wantz; cur_nc = dr->no_color; force_rebind = 0;
                    /* RT_CLEARDBG=2: magenta-clear the surface the DRAWS bind,
                     * at the moment they bind it. The guest's own clears may go
                     * to a different surface entirely (they do here: clears to
                     * 0xAB0000, draws to 0x1BE0000), so clearing on the clear
                     * path tests the wrong target. Anything not magenta
                     * afterwards was rasterised. */
                    { static int cd2 = -1;
                      if (cd2 < 0) { const char* e = getenv("RT_CLEARDBG"); cd2 = e ? atoi(e) : 0; }
                      if (cd2 >= 2 && want >= 0) {
                          const float mag[4] = {1.0f, 0.0f, 1.0f, 1.0f};
                          s_d3d.cmd_list->lpVtbl->ClearRenderTargetView(
                              s_d3d.cmd_list, off_rt_rtv(want), mag, 0, NULL);
                      } }
                    /* VP_TESTTRI=1: our own triangle, through the game's exact
                     * pipeline -- same vpso, same root signature, same vertex
                     * buffer, same RTV just bound. Written at vertex 1000 so it
                     * cannot collide with the guest's data. Pair with
                     * VP_BYPASS=1 so the shader is a trivial passthrough.
                     *
                     * If this renders, the GPU does read vp_vb and the fault is
                     * in the guest draws' own state. If it does not, nothing
                     * this backend draws reaches a render target at all. */
                    { static int tt = -1;
                      if (tt < 0) { const char* e = getenv("VP_TESTTRI"); tt = e ? atoi(e) : 0; }
                      if (tt && want >= 0 && s_d3d.vp_vb_mapped) {
                          float* vtx = (float*)((char*)s_d3d.vp_vb_mapped
                              + (u64)s_d3d.vp_parity * MAX_VERTICES * 256u
                              + (u64)1000 * 256u);
                          static const float tri[3][4] = {
                              {-2.0f, -2.0f, 0.0f, 1.0f},
                              { 2.0f, -2.0f, 0.0f, 1.0f},
                              { 0.0f,  2.0f, 0.0f, 1.0f} };
                          for (int _v = 0; _v < 3; _v++) {
                              float* dstv = vtx + (size_t)_v * (256u / 4u);
                              dstv[0] = tri[_v][0]; dstv[1] = tri[_v][1];
                              dstv[2] = tri[_v][2]; dstv[3] = tri[_v][3];
                          }
                          s_d3d.cmd_list->lpVtbl->DrawInstanced(s_d3d.cmd_list, 3, 1, 1000, 0);
                          { static int _n = 0;
                            if (PS3_LOG_FIRST(_n, 3)) fprintf(stderr, "[TESTTRI] issued on rt=%d off=0x%08X%c",
                                                  want, s_d3d.off_rt[want].off, 10); }
                      } }
                    cur_rt = want;
                    for (int _m = 0; _m < 3; _m++) cur_m[_m] = wantm[_m];
                }
                if (dr->is_clear) {
                    D3D12_CPU_DESCRIPTOR_HANDLE rh =
                        (cur_rt >= 0) ? off_rt_rtv(cur_rt) : rtv_handle;
                    /* RT_CLEARDBG=1: clear offscreen targets to magenta rather
                     * than the guest colour. "0.000% nonzero" cannot tell an
                     * untouched surface from one written black, and that
                     * ambiguity invalidates every read of an offscreen target.
                     * If the surface comes back magenta the clears land and the
                     * draws are writing black; if it stays black, nothing
                     * reaches it at all. */
                    /* Depth/stencil part: one depth buffer serves every
                     * target, and it is reset whenever the pass switches
                     * targets; a clear inside a pass has to land here. */
                    /* A clear covers the guest scissor's part of the surface
                     * only, as on the RSX. */
                    D3D12_RECT csr, zsr;
                    const int cpart = rec_scissor_rect(dr,
                        cur_rt >= 0 ? (LONG)s_d3d.off_rt[cur_rt].w : (LONG)s_d3d.width,
                        cur_rt >= 0 ? (LONG)s_d3d.off_rt[cur_rt].h : (LONG)s_d3d.height, &csr);
                    const int zpart = rec_scissor_rect(dr,
                        cur_z >= 0 ? (LONG)s_d3d.off_ds[cur_z].w : (LONG)s_d3d.width,
                        cur_z >= 0 ? (LONG)s_d3d.off_ds[cur_z].h : (LONG)s_d3d.height, &zsr);
                    const int zempty = zpart && (zsr.right <= zsr.left || zsr.bottom <= zsr.top);
                    const int cempty = cpart && (csr.right <= csr.left || csr.bottom <= csr.top);
                    { float rx, ry;
                      rs_target(cur_rt, cur_z, 0, &rx, &ry);
                      rs_rect(&csr, rx, ry);
                      rs_target(cur_rt, cur_z, cur_z >= 0, &rx, &ry);
                      rs_rect(&zsr, rx, ry); }
                    if ((dr->clear_mask & 0x3u) && !zempty) {
                        D3D12_CLEAR_FLAGS zf = (D3D12_CLEAR_FLAGS)
                            (((dr->clear_mask & 0x1u) ? D3D12_CLEAR_FLAG_DEPTH : 0) |
                             ((dr->clear_mask & 0x2u) ? D3D12_CLEAR_FLAG_STENCIL : 0));
                        s_d3d.cmd_list->lpVtbl->ClearDepthStencilView(s_d3d.cmd_list, cur_dsv,
                            zf, dr->clear_z, dr->clear_s, zpart ? 1 : 0, zpart ? &zsr : NULL);
                        if (cur_z >= 0) { s_d3d.off_ds[cur_z].cleared = 1; s_d3d.off_ds[cur_z].gen++; }
                    }
                    if (cempty) continue;
                    if (!(dr->clear_mask & 0xF0u) || dr->no_color)
                        continue;
                    { static int cd = -1;
                      if (cd < 0) { const char* e = getenv("RT_CLEARDBG"); cd = e ? atoi(e) : 0; }
                      if (cd && cur_rt >= 0) {
                          const float mag[4] = {1.0f, 0.0f, 1.0f, 1.0f};
                          s_d3d.cmd_list->lpVtbl->ClearRenderTargetView(s_d3d.cmd_list, rh, mag, 0, NULL);
                      } else
                          s_d3d.cmd_list->lpVtbl->ClearRenderTargetView(s_d3d.cmd_list, rh, dr->cc,
                                                                        cpart ? 1 : 0, cpart ? &csr : NULL); }
                    for (int _m = 0; _m < 3; _m++)
                        if (cur_m[_m] >= 0) {
                            D3D12_CPU_DESCRIPTOR_HANDLE rhm = off_rt_rtv(cur_m[_m]);
                            s_d3d.cmd_list->lpVtbl->ClearRenderTargetView(s_d3d.cmd_list, rhm, dr->cc,
                                                                          cpart ? 1 : 0, cpart ? &csr : NULL);
                        }
                    continue;
                }
                /* Per-draw pipeline: prefer the guest's own compiled FP; fall
                 * back to the hardcoded atlas/colour PS pair. */
                if (ahead) s_sj_nowait = 1;
                ID3D12PipelineState* dpso =
                    dr->fp_addr ? vp_get_fp_pso(dr->vs_idx, dr->fp_addr, dr->blend_key,
                                                dr_num_rts(dr),
                                                dr->rt_off ? rsx_surface_dxgi(dr->rt_fmt)
                                                           : DXGI_FORMAT_R8G8B8A8_UNORM,
                                                dr->fp_ctrl, dr->cmask, dr->cull,
                                                0u, dr->raw, dr->zs_key,
                                                0u, 0u, dr) : NULL;
                /* Its program is still being built: skip the draw this frame
                 * rather than draw it with another one. */
                s_sj_nowait = 0;
                if (dr->fp_addr && !dpso && s_pso_pending) {
                    if (ahead) break;   /* the final pass waits for it, as it always did */
                    zq_draw_lost();
                    s_sj_stat_skipped++; s_hl_skipped++; continue;
                }
                if (dr->raw && !dpso) { zq_draw_lost(); continue; }   /* the base pipeline reads the input layout */
                s_d3d.cmd_list->lpVtbl->SetPipelineState(s_d3d.cmd_list,
                                                         dpso ? dpso : vpso);
                if (dpso && dr->poly && dyb_ready()) {
                    const u32 fb = (u32)(dr->poly >> 32), bb = (u32)dr->poly;
                    float f, b;
                    memcpy(&f, &fb, sizeof f); memcpy(&b, &bb, sizeof b);
                    { float rx, ry;   /* the slope is per pixel: that much smaller on a target drawn larger */
                      rs_target(cur_rt, cur_z, cur_nc, &rx, &ry);
                      f *= rx > ry ? rx : ry; }
                    s_dyb.cl9->lpVtbl->RSSetDepthBias(s_dyb.cl9, (float)(INT)b, 0.0f, f);
                }
                s_d3d.cmd_list->lpVtbl->OMSetStencilRef(s_d3d.cmd_list, dr->stencil_ref & 0xFFu);
                if (dr->dbt && dbt_ready()) {
                    float lo, hi;
                    memcpy(&lo, &dr->dbt_min, sizeof lo); memcpy(&hi, &dr->dbt_max, sizeof hi);
                    if (!(lo >= 0.0f)) lo = 0.0f;
                    if (!(hi <= 1.0f)) hi = 1.0f;
                    if (hi < lo) hi = lo;
                    s_dbt.cl1->lpVtbl->OMSetDepthBounds(s_dbt.cl1, lo, hi);
                }
                if (dr->blend_const && rsx_blend_color_on()) {
                    float bf[4];
                    rsx_blend_factor(dr, bf);
                    s_d3d.cmd_list->lpVtbl->OMSetBlendFactor(s_d3d.cmd_list, bf);
                }
                if (cur_z >= 0 && (dr->zs_key & (2u | (1u << 5)))) s_d3d.off_ds[cur_z].gen++;
                /* Per-draw viewport: the guest rect when sane, else the
                 * full target. Scissor tracks the same rect. */
                {
                    float tw = (cur_rt >= 0) ? (float)s_d3d.off_rt[cur_rt].w : (float)s_d3d.width;
                    float th = (cur_rt >= 0) ? (float)s_d3d.off_rt[cur_rt].h : (float)s_d3d.height;
                    if (cur_nc && cur_z >= 0) {
                        tw = (float)s_d3d.off_ds[cur_z].w;
                        th = (float)s_d3d.off_ds[cur_z].h;
                    }
                    D3D12_VIEWPORT dvp = {0, 0, tw, th, 0.0f, 1.0f};
                    u32 ax, ay;
                    rsx_aa_grid(dr->rt_fmt, &ax, &ay);   /* an AA surface: samples */
                    /* A 1x1 viewport is sane: a luminance chain ends in one pixel. */
                    if (dr->vp_w >= 1 && dr->vp_h >= 1 &&
                        (float)((dr->vp_x + dr->vp_w) * ax) <= tw + 0.5f &&
                        (float)((dr->vp_y + dr->vp_h) * ay) <= th + 0.5f) {
                        dvp.TopLeftX = (float)(dr->vp_x * ax);
                        dvp.TopLeftY = (float)(dr->vp_y * ay);
                        dvp.Width    = (float)(dr->vp_w * ax);
                        dvp.Height   = (float)(dr->vp_h * ay);
                    }
                    D3D12_RECT dsc = {(LONG)dvp.TopLeftX, (LONG)dvp.TopLeftY,
                                      (LONG)(dvp.TopLeftX + dvp.Width),
                                      (LONG)(dvp.TopLeftY + dvp.Height)};
                    /* The guest scissor clips as well (God of War: Ascension's
                     * always matches its viewport; its shadow-map clears are
                     * where its scissor is smaller than the target). */
                    { D3D12_RECT sr;
                      if (rec_scissor_rect(dr, (LONG)tw, (LONG)th, &sr)) {
                          if (sr.left > dsc.left) dsc.left = sr.left;
                          if (sr.top > dsc.top) dsc.top = sr.top;
                          if (sr.right < dsc.right) dsc.right = sr.right;
                          if (sr.bottom < dsc.bottom) dsc.bottom = sr.bottom;
                          if (dsc.right < dsc.left) dsc.right = dsc.left;
                          if (dsc.bottom < dsc.top) dsc.bottom = dsc.top;
                      } }
                    dvp_x = dvp.TopLeftX; dvp_y = dvp.TopLeftY;
                    dvp_w = dvp.Width;    dvp_h = dvp.Height;
                    { float rx, ry;
                      rs_target(cur_rt, cur_z, cur_nc, &rx, &ry);
                      dvp.TopLeftX *= rx; dvp.TopLeftY *= ry; dvp.Width *= rx; dvp.Height *= ry;
                      rs_rect(&dsc, rx, ry); }
                    s_d3d.cmd_list->lpVtbl->RSSetViewports(s_d3d.cmd_list, 1, &dvp);
                    s_d3d.cmd_list->lpVtbl->RSSetScissorRects(s_d3d.cmd_list, 1, &dsc);
                }
                { static int fbon = -1;
                  if (fbon < 0) { const char* e = getenv("RT_FEEDBACK"); fbon = e ? atoi(e) : 2; }
                  int done_rt = -1;
                  for (int _u = 0; fbon && !dr->is_clear && _u < VP_TEX_UNITS; _u++) {
                      const int rt = dr->tex_rt[_u];
                      if (rt < 0 || dr->tex_cv[_u] >= 0 || dr->tex_ds[_u] >= 0) continue;
                      if (rt != cur_rt && rt != cur_m[0] && rt != cur_m[1] && rt != cur_m[2]) continue;
                      /* RT_FEEDBACK=2: the copy made at the title's last texture
                       * cache invalidate this frame, if there is one. */
                      const int reuse = done_rt == rt ||
                          (fbon == 2 && s_fb[rt].res && s_fb[rt].frame == s_d3d.frame_count &&
                           s_fb[rt].inval == dr->l2_inval);
                      ID3D12Resource* fr = reuse ? s_fb[rt].res : (s_fb_copies++, fb_copy(rt));
                      if (fr && !reuse) { s_fb[rt].inval = dr->l2_inval; s_fb[rt].frame = s_d3d.frame_count; }
                      if (!fr) continue;
                      done_rt = rt;
                      srv_write(DRAW_SRV_BASE + d * VP_TEX_UNITS + (u32)_u, fr,
                                (DXGI_FORMAT)s_fb[rt].dxgi, D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING);
                      { static int _n = 0;
                        if (PS3_LOG_FIRST(_n, 4))
                            fprintf(stderr, "[rt-feedback] op %u unit %d reads its own target 0x%X%c",
                                    d, _u, s_d3d.off_rt[rt].off, 10); }
                  } }
                /* Per-draw textures: bind this draw's t0-t15 SRV window.
                 * Any sampled offscreen RT transitions to PSR first (never
                 * one of the currently-bound colour targets). */
                for (int _u = 0; _u < VP_TEX_UNITS; _u++) {
                    int rt = dr->tex_rt[_u];
                    if (rt >= 0 && rt != cur_rt &&
                        rt != cur_m[0] && rt != cur_m[1] && rt != cur_m[2])
                        off_rt_transition(rt, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
                    if (dr->tex_cv[_u] >= 0)
                        off_rt_transition(dr->tex_cv[_u], D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
                    const int dz = dr->tex_dz[_u];
                    if (dz >= 0) {
                        if (dz != cur_z)
                            off_ds_transition(dz, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
                        else   /* the surface it draws into: not readable here */
                            srv_write(DRAW_SRV_BASE + dr->srv_win * VP_TEX_UNITS + (u32)_u, NULL,
                                      DXGI_FORMAT_R8G8B8A8_UNORM, D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING);
                    }
                }
                D3D12_GPU_DESCRIPTOR_HANDLE gh = gh_base;
                gh.ptr += (u64)(DRAW_SRV_BASE + dr->srv_win * VP_TEX_UNITS) * s_d3d.srv_inc;
                s_d3d.cmd_list->lpVtbl->SetGraphicsRootDescriptorTable(s_d3d.cmd_list, 1, gh);
                if (s_d3d.samp_heap) {
                    D3D12_GPU_DESCRIPTOR_HANDLE sb;
                    s_d3d.samp_heap->lpVtbl->GetGPUDescriptorHandleForHeapStart(s_d3d.samp_heap, &sb);
                    for (int _u = 0; _u < VP_TEX_UNITS; _u++) {
                        const int si = vp_sampler(dr, _u);
                        if (si == samp_bound[_u]) continue;
                        samp_bound[_u] = si;
                        D3D12_GPU_DESCRIPTOR_HANDLE sh = sb;
                        sh.ptr += (u64)si * s_d3d.samp_inc;
                        s_d3d.cmd_list->lpVtbl->SetGraphicsRootDescriptorTable(s_d3d.cmd_list, 5 + _u, sh);
                    }
                }
                /* Per-draw constants: this draw's vp_cb, and its b1 slot with
                 * the coordinate scale of each unit's texture as bound now, and
                 * in .z the lanes its fetches decode from sRGB (_rsx_gamma_dyn). */
                if (s_d3d.vp_fpcb_mapped && !dr->is_clear) {
                    float sc[VP_TEX_UNITS][2];
                    dr_coord_scales(dr, sc);
                    float* ts = (float*)((char*)s_d3d.vp_fpcb_mapped
                        + (u64)s_d3d.vp_parity * FPCB_HALF + dr->fpcb_off);
                    for (int _u = 0; _u < VP_TEX_UNITS; _u++) {
                        ts[_u * 4 + 0] = sc[_u][0]; ts[_u * 4 + 1] = sc[_u][1];
                        ts[_u * 4 + 2] = (float)dr_gamma_mask(dr, _u);
                        ts[_u * 4 + 3] = 0.0f;
                    }
                    if (RS_ON) {
                        float rx, ry;
                        rs_target(cur_rt, cur_z, cur_nc, &rx, &ry);
                        ts[FPCB_ALPHA * 4 + 3] = rx != ry ? -1.0f / ry : 1.0f / rx;
                    }
                }
                s_d3d.cmd_list->lpVtbl->SetGraphicsRootConstantBufferView(s_d3d.cmd_list, 0,
                    s_d3d.vp_cb->lpVtbl->GetGPUVirtualAddress(s_d3d.vp_cb)
                    + ((u64)s_d3d.vp_parity * MAX_DRAWS + dr->cb_slot) * VP_CB_STRIDE);
                if (s_d3d.vp_fpcb)
                    s_d3d.cmd_list->lpVtbl->SetGraphicsRootConstantBufferView(s_d3d.cmd_list, 2,
                        s_d3d.vp_fpcb->lpVtbl->GetGPUVirtualAddress(s_d3d.vp_fpcb)
                        + (u64)s_d3d.vp_parity * FPCB_HALF + dr->fpcb_off);
                /* DRAWARGS=<N>: the arguments actually handed to the GPU, plus
                 * the target they are aimed at. A zero vertex count, or a start
                 * vertex past the buffer, rasterises nothing while every other
                 * check upstream still says the draw is fine. */
                { static int dcap = -1, dn = 0;
                  if (dcap < 0) { const char* e = getenv("DRAWARGS"); dcap = e ? atoi(e) : 0; }
                  if (dcap && dn < dcap) { dn++;
                      D3D12_CPU_DESCRIPTOR_HANDLE _rh =
                          (cur_rt >= 0) ? off_rt_rtv(cur_rt) : rtv_handle;
                      fprintf(stderr, "[DRAWARGS] verts=%u start=%u rt=%d rec=0x%08X bound=0x%08X"
                              " tex0=0x%08X texrt=%d dvp=%.0f,%.0f %.0fx%.0f%c",
                              dr->vertex_count, dr->vb_byte_offset / 256, cur_rt,
                              dr->rt_off,
                              (cur_rt >= 0) ? s_d3d.off_rt[cur_rt].off : 0u,
                              dr->tex[0].raw, dr->tex_rt[0],
                              (double)dvp_x, (double)dvp_y,
                              (double)dvp_w, (double)dvp_h, 10); } }
                /* RT_REBIND=1: re-issue OMSetRenderTargets immediately before
                 * every draw instead of only when the target changes. A clear
                 * proves the RTV descriptor is good but not that the binding is
                 * still live at draw time -- ClearRenderTargetView writes
                 * through the descriptor and never consults the OM. If anything
                 * between the retarget and here drops the binding, this restores
                 * it. */
                { static int rb = -1;
                  if (rb < 0) { const char* e = getenv("RT_REBIND"); rb = e ? atoi(e) : 0; }
                  if (rb) {
                      D3D12_CPU_DESCRIPTOR_HANDLE brh[4];
                      UINT bn = 1;
                      brh[0] = (cur_rt >= 0) ? off_rt_rtv(cur_rt) : rtv_handle;
                      for (int _m = 0; _m < 3; _m++)
                          if (cur_m[_m] >= 0) brh[bn++] = off_rt_rtv(cur_m[_m]);
                      s_d3d.cmd_list->lpVtbl->OMSetRenderTargets(
                          s_d3d.cmd_list, bn, brh, FALSE, &cur_dsv);
                  } }
                if (dr->raw) {
                    vp_draw_raw(dr, &ib_bound, &raw_bound);
                } else if (dr->indexed) {
                    if (!ib_bound) {
                        D3D12_INDEX_BUFFER_VIEW ibv;
                        ibv.BufferLocation = s_d3d.vp_ib->lpVtbl->GetGPUVirtualAddress(s_d3d.vp_ib)
                                           + (u64)s_d3d.vp_parity * VP_MAX_INDICES * 4;
                        ibv.SizeInBytes = (2u - s_d3d.vp_parity) * VP_MAX_INDICES * 4 + s_res_ib_size * 4;
                        ibv.Format = DXGI_FORMAT_R32_UINT;
                        s_d3d.cmd_list->lpVtbl->IASetIndexBuffer(s_d3d.cmd_list, &ibv);
                        ib_bound = 1;
                    }
                    s_d3d.cmd_list->lpVtbl->DrawIndexedInstanced(s_d3d.cmd_list,
                        dr->index_count, 1, dr->ib_first, (INT)(dr->vb_byte_offset / 256), 0);
                } else
                s_d3d.cmd_list->lpVtbl->DrawInstanced(s_d3d.cmd_list,
                    dr->vertex_count, 1, dr->vb_byte_offset / 256, 0);
                /* VP_SUBMIT=<N>: prove the VP pass actually reaches the GPU.
                 * "records exist" and "draws were submitted" are different
                 * claims, and every blank-output investigation conflates them. */
                { static int cap = -1, n = 0;
                  if (cap < 0) { const char* e = getenv("VP_SUBMIT"); cap = e ? atoi(e) : 0; }
                  if (cap && n < cap) { n++;
                    fprintf(stderr, "[VPSUBMIT] draw[%u] verts=%u pso=%s vp=%ux%u tex0=0x%X(set=%d) tex1=0x%X slot=%d fp=0x%X%c",
                            d, dr->vertex_count, dpso ? "guest-fp" : "fallback",
                            dr->vp_w, dr->vp_h, dr->tex[0].raw, dr->tex[0].set,
                            dr->tex[1].raw, dr->tex_slot, dr->fp_addr, 10);
                    fprintf(stderr, "            vs_idx=%d cb_slot=%u vbofs=%u cull=%u blend=%d%c",
                            dr->vs_idx, dr->cb_slot, dr->vb_byte_offset / 256,
                            dr->cull, dr->blend, 10); } }
            }
    s_sj_nowait = 0;
    if (perf_on()) s_perf_gpu += perf_now() - _rec0;   /* reuse: record time */
    memcpy(s_rf.samp_bound, samp_bound, sizeof samp_bound);
    s_rf.cur_rt = cur_rt; s_rf.ib_bound = ib_bound; s_rf.raw_bound = raw_bound;
    s_rf.vp_last_w = vp_last_w; s_rf.vp_last_h = vp_last_h;
    s_rf.dvp_x = dvp_x; s_rf.dvp_y = dvp_y; s_rf.dvp_w = dvp_w; s_rf.dvp_h = dvp_h;
    memcpy(s_rf.cur_m, cur_m, sizeof cur_m);
    s_rf.cur_z = cur_z; s_rf.cur_nc = cur_nc; s_rf.force_rebind = force_rebind;
    s_rf.cur_dsv = cur_dsv;
    s_rf.seen_small_vp = seen_small_vp; s_rf.captured = captured;
    (void)vpso; (void)gh_base; (void)vp_last_w; (void)vp_last_h;
    return d < to ? d : to;
}

/* The title's draw of what moves, again: its stretch left out, last frame's position and this
 * one's going to the pixel. */
static const char s_fg_obj_ps[] =
    "struct PSInput {\n"
    "    float4 position : SV_POSITION; float4 col0 : COLOR0; float4 col1 : COLOR1;\n"
    "    float4 fog : FOG;\n"
    "    float4 tc0:TEXCOORD0; float4 tc1:TEXCOORD1; float4 tc2:TEXCOORD2; float4 tc3:TEXCOORD3;\n"
    "    float4 tc4:TEXCOORD4; float4 tc5:TEXCOORD5; float4 tc6:TEXCOORD6; float4 tc7:TEXCOORD7;\n"
    "};\n"
    "float2 main(PSInput i) : SV_Target {\n"
    "    if (i.tc1.w <= 0.0) return float2(0.0, 0.0);\n"
    "    float2 now = i.tc0.xy / i.tc0.w, was = i.tc1.xy / i.tc1.w;\n"
    "    return float2(was.x - now.x, now.y - was.y) * 0.5;\n"
    "}\n";

static ID3D12PipelineState* fg_obj_pso(int vs_idx)
{
    if (vs_idx < 0 || vs_idx >= s_d3d.vp_vs_n) return NULL;
    const VPVSEntry* e = &s_d3d.vp_vs[vs_idx];
    int i = 0;
    while (i < FG_OBJ_PSOS && s_fg.obj_pso[i].hash != e->hash && s_fg.obj_pso[i].tried) i++;
    if (i == FG_OBJ_PSOS) return NULL;
    if (s_fg.obj_pso[i].pso) return s_fg.obj_pso[i].pso;
    ID3DBlob* vs = NULL;
    if (!s_fg.obj_pso[i].tried) {
        s_fg.obj_pso[i].tried = 1;
        s_fg.obj_pso[i].hash = e->hash;
        size_t n = 0;
        char* raw = e->hlsl ? vp_raw_text(e->hlsl, &n) : NULL;
        const char* at = raw ? strstr(raw, "    VSOutput Out;\n") : NULL;
        if (!at) { free(raw); return NULL; }
        const int c = s_fg.cam_reg, p = s_fg.prev_reg, a = s_fg.prev_attr;
        char mid[512];
        const int m = snprintf(mid, sizeof mid,
            "    o[0] = vp_c[%d] * v[0].x + vp_c[%d] * v[0].y + vp_c[%d] * v[0].z + vp_c[%d] * v[0].w;\n"
            "    o[7] = o[0];\n"
            "    o[8] = vp_c[%d] * v[%d].x + vp_c[%d] * v[%d].y + vp_c[%d] * v[%d].z + vp_c[%d] * v[%d].w;\n",
            c, c + 1, c + 2, c + 3, p, a, p + 1, a, p + 2, a, p + 3, a);
        const size_t head = (size_t)(at - raw);
        char* src = (char*)malloc(n + (size_t)m + 1);
        if (!src) { free(raw); return NULL; }
        memcpy(src, raw, head);
        memcpy(src + head, mid, (size_t)m);
        memcpy(src + head + m, at, n - head + 1);
        free(raw);
        n += (size_t)m;
        if (sj_workers() > 0) {
            vs = shc_find(src, n, "vs_5_0");
            if (!vs) s_fg.obj_pso[i].job = sj_submit(src, n, "guest_vp_motion", "vs_5_0", NULL, NULL, 0);
        }
        if (!vs && !s_fg.obj_pso[i].job) {
            ID3DBlob* err = NULL;
            d3d_compile_cached(src, n, "guest_vp_motion", "vs_5_0", &vs, &err);
            if (err) err->lpVtbl->Release(err);
        }
        free(src);
    }
    if (s_fg.obj_pso[i].job) {
        ShaderJob* j = s_fg.obj_pso[i].job;
        if (!sj_is_done(j)) return NULL;
        s_fg.obj_pso[i].job = NULL;
        if (SUCCEEDED(j->hr) && j->blob) { vs = j->blob; j->blob = NULL; }
        sj_release(j);
    }
    if (!s_fg.obj_ps) {
        ID3DBlob* err = NULL;
        d3d_compile_cached(s_fg_obj_ps, sizeof s_fg_obj_ps - 1, "fg_obj_ps", "ps_5_0", &s_fg.obj_ps, &err);
        if (err) err->lpVtbl->Release(err);
    }
    if (!vs || !s_fg.obj_ps) {
        if (vs) vs->lpVtbl->Release(vs);
        static int said;
        if (!said++) fprintf(stderr, "[D3D12] frame generation: no program for what moves%c", 10);
        return NULL;
    }
    D3D12_GRAPHICS_PIPELINE_STATE_DESC pd;
    memset(&pd, 0, sizeof pd);
    pd.pRootSignature = s_d3d.vp_root_sig;
    pd.VS.pShaderBytecode = vs->lpVtbl->GetBufferPointer(vs);
    pd.VS.BytecodeLength = vs->lpVtbl->GetBufferSize(vs);
    pd.PS.pShaderBytecode = s_fg.obj_ps->lpVtbl->GetBufferPointer(s_fg.obj_ps);
    pd.PS.BytecodeLength = s_fg.obj_ps->lpVtbl->GetBufferSize(s_fg.obj_ps);
    pd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    pd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    pd.RasterizerState.DepthBias = FG_OBJ_BIAS;
    pd.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    pd.SampleMask = UINT32_MAX;
    pd.DepthStencilState.DepthEnable = TRUE;
    pd.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
    pd.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;
    pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pd.NumRenderTargets = 1;
    pd.RTVFormats[0] = DXGI_FORMAT_R16G16_FLOAT;
    pd.DSVFormat = DXGI_FORMAT_D24_UNORM_S8_UINT;
    pd.SampleDesc.Count = 1;
    const HRESULT hr = s_d3d.device->lpVtbl->CreateGraphicsPipelineState(s_d3d.device, &pd, &IID_ID3D12PipelineState,
                                                                         (void**)&s_fg.obj_pso[i].pso);
    vs->lpVtbl->Release(vs);
    if (FAILED(hr)) {
        s_fg.obj_pso[i].pso = NULL;
        fprintf(stderr, "[D3D12] frame generation: the pipeline for what moves failed (0x%08lX)%c", (unsigned long)hr, 10);
    }
    return s_fg.obj_pso[i].pso;
}

/* The frame's draws are in. Where no HUD's clear took the depth, it is taken now; and the title's
 * draws of what moves go again, into a target of their own at the render's size, tested against
 * that depth. The depth is the scene's at its end, so only what shows gets through. */
static void fg_objects(void)
{
    if (!FG_INPUTS() || !s_fg.cam_ok || !s_present_this_frame) { s_fg.obj_n = 0; return; }
    if (!s_fg.marked) {
        const int zi = off_ds_find(s_fg.depth_off);
        if (zi < 0 || s_d3d.off_ds[zi].w != s_d3d.width || s_d3d.off_ds[zi].h != s_d3d.height || !fg_targets()) return;
        fg_copy_depth(zi);
        s_fg.depth_ok = 1;
    }
    if (!s_fg.depth_ok || !s_fg.obj_n || !s_fg.obj_rt) return;
    ID3D12GraphicsCommandList* cl = s_d3d.cmd_list;
    const D3D12_RESOURCE_STATES read = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    pp_barrier(s_fg.obj_rt, read, D3D12_RESOURCE_STATE_RENDER_TARGET);
    pp_barrier(s_fg.depth, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_DEPTH_READ);
    D3D12_CPU_DESCRIPTOR_HANDLE rtv, dsv;
    s_fg.obj_rtv->lpVtbl->GetCPUDescriptorHandleForHeapStart(s_fg.obj_rtv, &rtv);
    s_fg.obj_dsv->lpVtbl->GetCPUDescriptorHandleForHeapStart(s_fg.obj_dsv, &dsv);
    const float none[4] = { FG_OBJ_NONE, FG_OBJ_NONE, 0.0f, 0.0f };
    cl->lpVtbl->ClearRenderTargetView(cl, rtv, none, 0, NULL);
    cl->lpVtbl->OMSetRenderTargets(cl, 1, &rtv, FALSE, &dsv);
    const D3D12_VIEWPORT vp = { 0.0f, 0.0f, (float)s_d3d.pw, (float)s_d3d.ph, 0.0f, 1.0f };
    const D3D12_RECT sc = { 0, 0, (LONG)s_d3d.pw, (LONG)s_d3d.ph };
    cl->lpVtbl->RSSetViewports(cl, 1, &vp);
    cl->lpVtbl->RSSetScissorRects(cl, 1, &sc);
    const u64 cb = s_d3d.vp_cb->lpVtbl->GetGPUVirtualAddress(s_d3d.vp_cb) + (u64)s_d3d.vp_parity * MAX_DRAWS * VP_CB_STRIDE;
    for (u32 i = 0; i < s_fg.obj_n; i++) {
        const D3D12DrawRecord* dr = &s_d3d.draws[s_fg.obj[i]];
        ID3D12PipelineState* pso = dr->raw ? fg_obj_pso(dr->vs_idx) : NULL;
        if (!pso) continue;
        cl->lpVtbl->SetPipelineState(cl, pso);
        cl->lpVtbl->SetGraphicsRootConstantBufferView(cl, 0, cb + (u64)dr->cb_slot * VP_CB_STRIDE);
        vp_draw_raw(dr, &s_rf.ib_bound, &s_rf.raw_bound);
        s_fg.obj_drawn = 1;
    }
    pp_barrier(s_fg.depth, D3D12_RESOURCE_STATE_DEPTH_READ, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    pp_barrier(s_fg.obj_rt, D3D12_RESOURCE_STATE_RENDER_TARGET, read);
    if (s_rf.cur_rt < 0) s_rf.cur_rt = 0;   /* rf_vp_end binds the display target again */
    s_rf.force_rebind = 1;
}

/* The end of the VP pass: the display target bound again. */
static void rf_vp_end(void)
{
    if (!s_rf.vp_on) return;
    u32 fi = s_rf.fi; (void)fi;
    D3D12_CPU_DESCRIPTOR_HANDLE rtv_handle = s_rf.rtv_handle, dsv_handle = s_rf.dsv_handle;
    (void)rtv_handle; (void)dsv_handle;
    int cur_rt = s_rf.cur_rt;
            /* Leave the backbuffer bound for the dump/present epilogue. */
            if (cur_rt >= 0) {
                s_d3d.cmd_list->lpVtbl->OMSetRenderTargets(s_d3d.cmd_list, 1, &rtv_handle, FALSE, &dsv_handle);
                D3D12_VIEWPORT vp = {0, 0, (float)s_d3d.pw, (float)s_d3d.ph, 0.0f, 1.0f};
                D3D12_RECT sc = {0, 0, (LONG)s_d3d.pw, (LONG)s_d3d.ph};
                s_d3d.cmd_list->lpVtbl->RSSetViewports(s_d3d.cmd_list, 1, &vp);
                s_d3d.cmd_list->lpVtbl->RSSetScissorRects(s_d3d.cmd_list, 1, &sc);
            }
    s_rf.vp_on = 0;
}

static void render_frame(void)
{
    double _rf0 = perf_on() ? perf_now() : 0.0;
    if (!s_rf.open) {
        rf_open();
        rf_prepass(0, s_d3d.draw_count);
        rf_target();
        rf_legacy();
        rf_vp_begin();
    } else {
        /* Recorded ahead: the rest of the batch. */
        rf_prepass(s_rf.pre_done, s_d3d.draw_count);
        int legacy = 0;
        for (u32 i = 0; i < s_d3d.draw_count && i < MAX_DRAWS; i++)
            if (!s_d3d.draws[i].is_vp) { legacy = 1; break; }
        if (legacy) {
            /* Legacy records turned up after recording began: they are drawn
             * after the VP draws already recorded, not before them. */
            static int _n = 0;
            if (PS3_LOG_FIRST(_n, 4))
                fprintf(stderr, "[D3D12] record-ahead batch with legacy records: drawn out of order%c", 10);
            rf_vp_end();
            rf_legacy();
            rf_vp_begin();
        }
    }
    gts_mark(1);
    rf_vp_draws(s_rf.done, cap_stop(s_d3d.draw_count), 0);
    gps_end(s_d3d.draw_count);
    fg_objects();
    rf_vp_end();
    mwb_record();
    gts_mark(2);
    s_rf.open = 0;
    u32 fi = s_rf.fi;
    D3D12_RESOURCE_BARRIER barrier = {0};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = s_d3d.render_targets[fi];
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;

    if (perf_on()) s_perf_rf += perf_now() - _rf0;
    /* MEMPEEK=<hex addr>:<n>: n big-endian floats at a guest address each frame,
     * with a min/max so a buffer of constants is distinguishable from real data. */
    { const char* mp = env_str("MEMPEEK", &s_env_mempeek);
      if (mp) { extern uint8_t* vm_base; static int _n = 0;
        u32 addr = 0; int cnt = 8; sscanf(mp, "%x:%d", &addr, &cnt);
        if (vm_base && addr && PS3_LOG_FIRST(_n, 6)) {
            float lo = 1e30f, hi = -1e30f; u32 nz = 0;
            for (int i = 0; i < cnt; i++) {
                const u8* q = vm_base + addr + i * 4;
                u32 bits = ((u32)q[0]<<24)|((u32)q[1]<<16)|((u32)q[2]<<8)|q[3];
                float f; memcpy(&f, &bits, 4);
                if (f == f && f < 1e30f && f > -1e30f) { if (f < lo) lo = f; if (f > hi) hi = f; }
                if (bits) nz++;
            }
            fprintf(stderr, "[MEMPEEK] 0x%08X n=%d nonzero=%u range[%g..%g]%c",
                    addr, cnt, nz, lo, hi, 10);
        } } }
    /* VRAMSCAN=1: which 64 KB blocks of guest memory change between frames.
     * "nothing writes the buffer we read" is only half an answer -- this says
     * where the producer IS writing, so the two can be matched up. */
    { static int vs = -1; if (vs < 0) vs = getenv("VRAMSCAN") ? 1 : 0;
      if (vs) { extern uint8_t* vm_base;
        static u32 prev[512]; static int have = 0; static int reported = 0;
        u32 base = 0xC6000000u; u32 nblk = 512;          /* 32 MB window */
        const char* bs = getenv("VRAMSCAN_BASE");
        if (bs) base = (u32)strtoul(bs, NULL, 0);
        if (vm_base && reported < 6) {
            u32 changed = 0; char list[512]; int ln = 0;
            for (u32 b = 0; b < nblk; b++) {
                const u8* p = vm_base + base + b * 0x10000u;
                u32 h = 2166136261u;
                for (u32 i = 0; i < 0x10000u; i += 61) h = (h ^ p[i]) * 16777619u;
                if (have && h != prev[b]) { changed++;
                    if (ln < 400) ln += snprintf(list + ln, sizeof(list) - ln,
                                                 " +0x%X", b * 0x10000u); }
                prev[b] = h;
            }
            if (have && changed) { reported++;
                fprintf(stderr, "[VRAMSCAN] base=0x%08X %u/512 blocks changed:%s%c",
                        base, changed, list, 10); }
            have = 1;
        } } }
    /* FRAME_BUDGET=1: geometry a frame asked for vs what the batch can hold.
     * Reported from render_frame so it works under RSX_ACCUM_FRAME too -- it
     * used to live in the clear-boundary present path, which accum bypasses, so
     * it silently produced nothing in exactly the configuration being used. */
    { static int fb = -1;
      if (fb < 0) { const char* e = getenv("FRAME_BUDGET"); fb = e ? atoi(e) : 0; }
      if (fb && s_req_draws) { static int n = 0; if (PS3_LOG_FIRST(n, 40))
        fprintf(stderr, "[BUDGET] frame: requested %llu verts / %llu draws, "
                        "buffer holds %u verts, %llu draws truncated%c",
                (unsigned long long)s_req_verts, (unsigned long long)s_req_draws,
                MAX_VERTICES, (unsigned long long)s_drop_draws, 10); }
      s_req_verts = 0; s_req_draws = 0; s_drop_draws = 0; }
    if (perf_on()) {
        static double s_t_prev = 0.0; static int s_n = 0;
        double now = perf_now();
        if (s_t_prev > 0.0) s_perf_frame += now - s_t_prev;
        s_t_prev = now;
        if (++s_n % 20 == 0) {
            double fr = s_perf_frame > 0 ? s_perf_frame : 1;
            fprintf(stderr, "[PERF] %.2f fps | tex %.2fs (%.0f%%, %d calls) | vtx %.2fs"
                            " (%.0f%%, %.0fk verts) | render_frame %.2fs (%.0f%%)"
                            " | prepass %.2fs (%.0f%%) [srv %.2fs %.0f%% | pso %.2fs %.0f%% %d calls %d MISS %dKB hashed] | guest %.2fs (%.0f%%)%c",
                    20.0 / fr,
                    s_perf_tex, 100.0 * s_perf_tex / fr, s_perf_ntex,
                    s_perf_vtx, 100.0 * s_perf_vtx / fr,
                    (double)s_perf_nverts / 1000.0,
                    s_perf_rf, 100.0 * s_perf_rf / fr,
                    s_perf_pre, 100.0 * s_perf_pre / fr,
                    s_perf_srv, 100.0 * s_perf_srv / fr,
                    s_perf_pso, 100.0 * s_perf_pso / fr,
                    s_perf_pso_calls, s_perf_pso_miss, s_perf_pso_hashbytes / 1024,
                    fr - s_perf_rf, 100.0 * (fr - s_perf_rf) / fr, 10);
            if (s_shc_stat_compiled || s_shc_stat_hits || s_shc_stat_pso) {
                fprintf(stderr, "[PERF] shaders: %u compiled (%.0f ms), %u from the disk cache,"
                                " %u guest pipelines (%.0f ms)%c",
                        s_shc_stat_compiled, s_shc_stat_compile_s * 1000.0, s_shc_stat_hits,
                        s_shc_stat_pso, s_shc_stat_pso_s * 1000.0, 10);
                s_shc_stat_compiled = s_shc_stat_hits = s_shc_stat_pso = 0;
                s_shc_stat_compile_s = s_shc_stat_pso_s = 0.0;
            }
            fprintf(stderr, "[PERF] texture view windows: %u written (%u views copied, %u made in the cache), %u shared%c",
                    s_srvq_written, s_srvq_copied, s_srvq_made, s_srvq_reused, 10);
            s_srvq_written = s_srvq_reused = s_srvq_copied = s_srvq_made = 0;
            fprintf(stderr, "[PERF] waits for the GPU: %.0f ms;", s_perf_gpuwait * 1000.0);
            for (int i = 0; i < 16 && s_perf_gw[i].line; i++)
                fprintf(stderr, " line %d: %u, %.1f ms;", s_perf_gw[i].line, s_perf_gw[i].n, s_perf_gw[i].s * 1000.0);
            fprintf(stderr, "%c", 10);
            s_perf_gpuwait = 0.0;
            memset(s_perf_gw, 0, sizeof s_perf_gw);
            fprintf(stderr, "[PERF] caches: %d vertex programs (%u replaced), %d fragment pipelines (%u dropped)%c",
                    s_d3d.vp_vs_n, s_perf_vs_replaced, s_d3d.vp_fp_n, s_perf_fp_dropped, 10);
            s_perf_vs_replaced = s_perf_fp_dropped = 0;
            if (s_env_psowhy > 0) {
                fprintf(stderr, "[PERF] pipeline misses: %u of programs not cached; differing from the closest:", s_pw_new);
                for (int b = 0; b < PW_N; b++)
                    if (s_pw_n[b]) fprintf(stderr, " %s %u", s_pw_name[b], s_pw_n[b]);
                if (s_pw_ctrl) fprintf(stderr, " (control bits 0x%08X)", s_pw_ctrl);
                fprintf(stderr, "%c", 10);
                s_pw_new = 0; s_pw_ctrl = 0;
                memset(s_pw_n, 0, sizeof s_pw_n);
            }
            if (s_sj_stat_jobs || s_sj_stat_skipped) {
                fprintf(stderr, "[PERF] shader builds: %u queued to workers, %u draws skipped"
                                " waiting for one%c", s_sj_stat_jobs, s_sj_stat_skipped, 10);
                s_sj_stat_jobs = s_sj_stat_skipped = 0;
            }
            s_perf_frame = 0.0; s_perf_tex = 0.0; s_perf_vtx = 0.0; s_perf_rf = 0.0;
            s_perf_gpu = 0.0; s_perf_pre = 0.0; s_perf_srv = 0.0; s_perf_pso = 0.0;
            s_perf_pso_calls = 0; s_perf_pso_miss = 0; s_perf_pso_hashbytes = 0;
            s_perf_ntex = 0; s_perf_texbytes = 0; s_perf_nverts = 0;
        }
    }
    { static int dt = -1;
      if (dt < 0) { const char* e = getenv("RSX_DBG_TRACK"); dt = e ? atoi(e) : 0; }
      if (dt) { static int fr = 0; fr++;
        if (s_lock_n)
            fprintf(stderr, "[RSX_DBG_TRACK] frame %d: %u duck draws, ndc=(%.3f,%.3f)%c",
                    fr, s_lock_n, (float)(s_lock_sx/s_lock_n), (float)(s_lock_sy/s_lock_n), 10);
        else if (fr % 20 == 0)
            fprintf(stderr, "[RSX_DBG_TRACK] frame %d: NO duck draws%c", fr, 10);
      } }
    if (s_lock_n) {                       /* publish this frame's centroid */
        s_lock_x = (float)(s_lock_sx / s_lock_n);
        s_lock_y = (float)(s_lock_sy / s_lock_n);
        s_lock_valid = 1;
        s_lock_sx = s_lock_sy = 0.0; s_lock_n = 0;
    }
    vpass_submit(fi);      /* the vertex pass, executed before the batch's list */
    s_dbg_last_draws = s_d3d.draw_count;
    s_d3d.vb_offset  = 0; /* reset for next frame */
    s_d3d.vp_vb_offset = 0;
    s_d3d.vp_ib_offset = 0;
    for (u32 _i = 0; _i < s_d3d.draw_count && _i < MAX_DRAWS; _i++) {
        s_d3d.draws[_i].indexed = 0;
        s_d3d.draws[_i].raw = 0;
    }
    s_ix_run_valid = 0;
    s_raw_run_valid = 0;
    s_seg_n = 0;
    s_d3d.draw_count = 0;
    s_d3d.vp_parity ^= 1;  /* next frame's records go to the other half */
    vpass_reset();
    s_fpcb_used = 0;
    s_fpsnap_used = 0;     /* and their program snapshots */
    s_fpsnap_gen++;
    if (s_rt_mode > 0) rt_released();   /* RSX_RENDER_THREAD: the walk may go on */

    /* Debug: RTT_SAVERT=<hex raw offset>[:frame] copies that offscreen RT
     * into a readback buffer this frame and writes rt_save.bmp (half-float
     * RTs are tonemapped |v| -> byte). */
    static ID3D12Resource* s_rtsave_buf = NULL;
    static u32 s_rtsave_state = 0;   /* 1 = copy queued this frame */
    static u32 s_rtsave_w, s_rtsave_h, s_rtsave_pitch, s_rtsave_dxgi;
    { const char* sv = env_str("RTT_SAVERT", &s_env_rtt_savert);
      static int _done = 0;
      static u32 _skip = 0xFFFFFFFFu;
      if (_skip == 0xFFFFFFFFu) { const char* sk = getenv("RTT_SAVERT_SKIP");
          _skip = sk ? (u32)strtoul(sk, NULL, 0) : 0; }   /* frame 60 is far too
          early for a title that takes a minute to reach its menu */
      /* RTT_SAVERT_AFTER=<seconds> triggers on wall time instead of a frame
       * count. This title runs its menu at a few frames a second, so a frame
       * number is not a usable way to say "once it has settled". */
      static u32 _after = 0xFFFFFFFFu;
      if (_after == 0xFFFFFFFFu) { const char* sa = getenv("RTT_SAVERT_AFTER");
          _after = sa ? (u32)strtoul(sa, NULL, 0) : 0; }
      static clock_t _t0 = 0;
      if (!_t0) _t0 = clock();
      const int _due = _after
          ? ((u32)((clock() - _t0) / CLOCKS_PER_SEC) >= _after)
          : (s_d3d.frame_count > 60 + _skip);
      { static int _p1 = 0;
        if (sv && _due && !_p1) { _p1 = 1;
            printf("[RTT_SAVERT] due: want=0x%lX found_rt=%d frame=%u\n",
                   strtoul(sv, NULL, 16), off_rt_find((u32)strtoul(sv, NULL, 16)),
                   (unsigned)s_d3d.frame_count); fflush(stdout); } }
      if (sv && !_done && _due) {
        int rt = off_rt_find((u32)strtoul(sv, NULL, 16));
        fprintf(stderr, "[RTT_SAVERT] rt=%d res=%p\n", rt, rt >= 0 ? (void*)s_d3d.off_rt[rt].res : NULL);
        if (rt >= 0 && s_d3d.off_rt[rt].res) {
            OffRT* r = &s_d3d.off_rt[rt];
            u32 bpp = (r->dxgi == DXGI_FORMAT_R16G16B16A16_FLOAT) ? 8 :
                      (r->dxgi == DXGI_FORMAT_R32G32B32A32_FLOAT) ? 16 : 4;
            u32 pitch = (r->pw * bpp + 255) & ~255u;
            fprintf(stderr, "[RTT_SAVERT] rt=%d w=%u h=%u bpp=%u pitch=%u dxgi=%d bytes=%llu\n",
                    rt, r->pw, r->ph, bpp, pitch, (int)r->dxgi,
                    (unsigned long long)((u64)pitch * r->ph));
            if (!s_rtsave_buf) {
                D3D12_HEAP_PROPERTIES hp = {0}; hp.Type = D3D12_HEAP_TYPE_READBACK;
                D3D12_RESOURCE_DESC rd = {0};
                rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
                rd.Width = (u64)pitch * r->ph; rd.Height = 1; rd.DepthOrArraySize = 1;
                rd.MipLevels = 1; rd.SampleDesc.Count = 1;
                rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
                s_d3d.device->lpVtbl->CreateCommittedResource(
                    s_d3d.device, &hp, D3D12_HEAP_FLAG_NONE, &rd,
                    D3D12_RESOURCE_STATE_COPY_DEST, NULL,
                    &IID_ID3D12Resource, (void**)&s_rtsave_buf);
            }
            if (!s_rtsave_buf) fprintf(stderr, "[RTT_SAVERT] readback buffer alloc FAILED\n");
            if (s_rtsave_buf) {
                off_rt_transition(rt, D3D12_RESOURCE_STATE_COPY_SOURCE);
                D3D12_TEXTURE_COPY_LOCATION cdst = {0}, csrc = {0};
                cdst.pResource = s_rtsave_buf;
                cdst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
                cdst.PlacedFootprint.Footprint.Format   = (DXGI_FORMAT)r->dxgi;
                cdst.PlacedFootprint.Footprint.Width    = r->pw;
                cdst.PlacedFootprint.Footprint.Height   = r->ph;
                cdst.PlacedFootprint.Footprint.Depth    = 1;
                cdst.PlacedFootprint.Footprint.RowPitch = pitch;
                csrc.pResource = r->res;
                csrc.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
                s_d3d.cmd_list->lpVtbl->CopyTextureRegion(s_d3d.cmd_list, &cdst, 0, 0, 0, &csrc, NULL);
                off_rt_transition(rt, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
                s_rtsave_state = 1;
                s_rtsave_w = r->pw; s_rtsave_h = r->ph;
                s_rtsave_pitch = pitch; s_rtsave_dxgi = r->dxgi;
                fprintf(stderr, "[RTT_SAVERT] copy queued: rt=%d %ux%u pitch=%u\n",
                        rt, r->w, r->h, pitch);
                _done = 1;
            }
        }
      } }

    /* CELLMARK_DUMP_MINDRAWS=<N>: only consider frames carrying at least N draw
     * records. Most render_frame calls come from the guest flip with a batch the
     * clear boundary already drained, so they present an empty backbuffer -- and
     * sampling those wastes every dump on a black image while the frames that do
     * hold the scene go uncaptured. */
    { static int mind = -1;
      if (mind < 0) { const char* e = getenv("CELLMARK_DUMP_MINDRAWS");
                      mind = e ? atoi(e) : 0; }
      /* draw_count is already reset by here; s_dbg_last_draws holds the
       * count this frame actually rendered. */
      if (mind > 0 && s_dbg_last_draws < (u32)mind) goto skip_dump_consider; }
    if (s_d3d.dump_skip_left > 0 && s_d3d.dump_frames_left > 0) s_d3d.dump_skip_left--;
skip_dump_consider: ;
    guest_fb_present(fi);
    composite_present(fi);
    int dumping = (s_d3d.dump_frames_left > 0 && s_d3d.readback_buf
                   && s_d3d.dump_skip_left == 0);
    /* A shot of the title's frame needs the readback buffer; one of the
     * window, a frame that is presented. */
    int shot = 0;
    if (s_shot_pending && (d3d12_shot_game() ? s_d3d.readback_buf != NULL : s_present_this_frame))
        shot = InterlockedExchange(&s_shot_pending, 0) != 0;
    s_shot_cap = shot;
    { static int mind2 = -1;
      if (mind2 < 0) { const char* e = getenv("CELLMARK_DUMP_MINDRAWS");
                       mind2 = e ? atoi(e) : 0; }
      if (mind2 > 0 && s_dbg_last_draws < (u32)mind2) dumping = 0; }
    /* CELLMARK_DUMP_EVERY=N: after each dump, skip N-1 frames -- samples the whole
     * run instead of one contiguous window, so a short burst of real content
     * can't fall between the dumped frames. */
    if (dumping) {
        static int s_every = -1;
        if (s_every < 0) { const char* e = getenv("CELLMARK_DUMP_EVERY");
                           s_every = e ? atoi(e) : 0; }
        if (s_every > 1) s_d3d.dump_skip_left = s_every - 1;
        rdoc_request();
    }
    if (dumping) {
        /* RT -> COPY_SOURCE, copy into the readback buffer, then -> PRESENT. */
        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
        barrier.Transition.StateAfter  = D3D12_RESOURCE_STATE_COPY_SOURCE;
        s_d3d.cmd_list->lpVtbl->ResourceBarrier(s_d3d.cmd_list, 1, &barrier);

        D3D12_TEXTURE_COPY_LOCATION dst = {0}, src = {0};
        dst.pResource = s_d3d.readback_buf;
        dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        dst.PlacedFootprint.Offset = 0;
        dst.PlacedFootprint.Footprint.Format   = DXGI_FORMAT_R8G8B8A8_UNORM;
        dst.PlacedFootprint.Footprint.Width    = s_d3d.pw;
        dst.PlacedFootprint.Footprint.Height   = s_d3d.ph;
        dst.PlacedFootprint.Footprint.Depth    = 1;
        dst.PlacedFootprint.Footprint.RowPitch = s_d3d.readback_pitch;
        src.pResource = s_d3d.render_targets[fi];
        src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        src.SubresourceIndex = 0;
        s_d3d.cmd_list->lpVtbl->CopyTextureRegion(s_d3d.cmd_list, &dst, 0, 0, 0, &src, NULL);

        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
        barrier.Transition.StateAfter  = D3D12_RESOURCE_STATE_PRESENT;
        s_d3d.cmd_list->lpVtbl->ResourceBarrier(s_d3d.cmd_list, 1, &barrier);
    } else {
        /* End-of-frame fallback snapshot: used when the frame never contained a
         * reduced-viewport pass to capture mid-frame. */
        screen_copy_capture(fi);
        if (shot && d3d12_shot_game()) {
            shot_copy(fi);   /* ends in PRESENT */
        } else {
            /* Transition render target to PRESENT state */
            barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
            barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
            s_d3d.cmd_list->lpVtbl->ResourceBarrier(s_d3d.cmd_list, 1, &barrier);
        }
    }

    if (s_present_this_frame && flip_op_dbg() && (s_fo_batch != 1 || s_fo_dropped || s_fo_failed))
        fprintf(stderr, "[flip-op] %.2f s: present with %u flip ops (%u dropped, %u not copied), %u records%c",
                flip_op_secs(), s_fo_batch, s_fo_dropped, s_fo_failed, s_d3d.draw_count, 10);
    if (s_present_this_frame) s_fo_batch = s_fo_dropped = s_fo_failed = 0;
    if (s_present_this_frame) present_to_backbuffer(fi);
    if (shot && !d3d12_shot_game()) shot_copy_bb(fi);
    if (s_present_this_frame) fg_frame();

    /* Close and execute */
    zq_resolve();
    gts_resolve();
    s_d3d.cmd_list->lpVtbl->Close(s_d3d.cmd_list);
    ID3D12CommandList* cmd_lists[] = {(ID3D12CommandList*)s_d3d.cmd_list};
    s_d3d.cmd_queue->lpVtbl->ExecuteCommandLists(s_d3d.cmd_queue, 1, cmd_lists);

    /* The resident part is full, of blocks nobody may draw any more among others: it starts over
     * once the batches that read it are done. It took a wait for the GPU here, 9-17 ms on a
     * GTX 1650 Ti, a frame of ~50 ms against ~25 every 11-17 s at the temple (issue #9). Now the
     * draws go through the ring meanwhile, a frame or two, and nothing waits: the value the
     * fence gets next comes after this frame's batches, whoever signals it (move_to_next_frame,
     * or a wait before it). Rare: it takes a level's worth of new results. */
    if (s_res_vb_size) {
        if (s_res_retiring) {
            if (s_d3d.fence->lpVtbl->GetCompletedValue(s_d3d.fence) >= s_res_retire) {
                s_res_vb_used = s_res_ib_used = 0;
                s_res_gen++;
                s_res_retiring = 0;
                fprintf(stderr, "[vtx-kept] resident part full: started over (%u), %u frames through the ring%c",
                        s_res_gen, s_res_retire_frames, 10);
            } else {
                s_res_retire_frames++;
            }
        } else if (s_res_full) {
            s_res_retire = s_d3d.fence_values[s_d3d.frame_index];
            s_res_retiring = 1;
            s_res_retire_frames = 1;
            s_res_full = 0;
        }
    }

    /* D3D12_IQ=1: drain the debug layer's message queue after submitting the
     * frame. The legacy (is_vp=0) DrawInstanced path silently produced ZERO
     * pixels in flOw's render injection while ClearRenderTargetView worked, and
     * every input (verts, offsets, layout, MVP, PSO, root sig, depth, cull) was
     * verified correct -- so the only remaining explanation is a validation
     * error the debug layer is swallowing. Off unless the env var is set. */
    { static int _iq = -1;
      if (_iq < 0) { const char* e = getenv("D3D12_IQ"); _iq = e ? (atoi(e) > 1 ? atoi(e) : 1) : 0; }
      if (_iq && s_d3d.device) {
        ID3D12InfoQueue* iq = NULL;
        if (SUCCEEDED(s_d3d.device->lpVtbl->QueryInterface(
                s_d3d.device, &IID_ID3D12InfoQueue, (void**)&iq)) && iq) {
            UINT64 n = iq->lpVtbl->GetNumStoredMessages(iq);
            static int _printed = 0;
            /* D3D12_IQ=2: errors and corruption only, each message ID once, so
             * the warnings a run starts with do not use up the budget. */
            static int s_seen_n = 0; static int s_seen[256];
            for (UINT64 mi = 0; mi < n && _printed < 80; mi++) {
                SIZE_T len = 0;
                iq->lpVtbl->GetMessage(iq, mi, NULL, &len);
                D3D12_MESSAGE* m = (D3D12_MESSAGE*)malloc(len);
                if (m && SUCCEEDED(iq->lpVtbl->GetMessage(iq, mi, m, &len))) {
                    int skip = 0;
                    if (_iq >= 2) {
                        skip = m->Severity > D3D12_MESSAGE_SEVERITY_ERROR;
                        for (int k = 0; k < s_seen_n && !skip; k++) skip = s_seen[k] == (int)m->ID;
                        if (!skip && s_seen_n < 256) s_seen[s_seen_n++] = (int)m->ID;
                    }
                    if (!skip) {
                        fprintf(stderr, "[D3D12-IQ][sev=%d id=%d] %s\n",
                                (int)m->Severity, (int)m->ID, m->pDescription);
                        _printed++;
                    }
                }
                free(m);
            }
            if (n) iq->lpVtbl->ClearStoredMessages(iq);
            iq->lpVtbl->Release(iq);
            fflush(stderr);
        }
      } }

    if (s_sc_dump_pending) {
        s_sc_dump_pending = 0;
        wait_for_gpu();
        s_dump_name = "screencopy";
        dump_backbuffer_bmp();
        s_dump_name = NULL;
    }
    if (shot) {
        wait_for_gpu();
        const int ok = d3d12_shot_game()
            ? shot_png(s_d3d.readback_buf, s_d3d.pw, s_d3d.ph, s_d3d.readback_pitch)
            : shot_png(s_d3d.shot_buf, s_d3d.shot_w, s_d3d.shot_h, s_d3d.shot_pitch);
        ov_toast(ok ? "Screenshot saved" : "Screenshot failed");
    }
    if (dumping) {
        wait_for_gpu();            /* ensure the copy finished before mapping */
        dump_backbuffer_bmp();
        s_d3d.dump_frames_left--;
    }

    if (s_rtsave_state) {
        fprintf(stderr, "[RTT_SAVERT] writing rt_save.bmp %ux%u\n", s_rtsave_w, s_rtsave_h);
        if (!dumping) { double _g = perf_on() ? perf_now() : 0.0;
                        wait_for_gpu();
                        if (perf_on()) s_perf_gpu += perf_now() - _g; }
        void* mp = NULL; D3D12_RANGE rr = {0, (SIZE_T)s_rtsave_pitch * s_rtsave_h};
        if (SUCCEEDED(s_rtsave_buf->lpVtbl->Map(s_rtsave_buf, 0, &rr, &mp)) && mp) {
            FILE* f = fopen("rt_save.bmp", "wb");
            if (f) {
                u32 w = s_rtsave_w, h = s_rtsave_h;
                u32 rowb = (w * 3 + 3) & ~3u;
                u32 datasz = rowb * h;
                u8 hdr[54] = {0};
                hdr[0]='B'; hdr[1]='M';
                *(u32*)(hdr+2) = 54 + datasz; *(u32*)(hdr+10) = 54;
                *(u32*)(hdr+14) = 40; *(int*)(hdr+18) = (int)w; *(int*)(hdr+22) = (int)h;
                *(u16*)(hdr+26) = 1; *(u16*)(hdr+28) = 24; *(u32*)(hdr+34) = datasz;
                fwrite(hdr, 1, 54, f);
                u8* line = (u8*)malloc(rowb);
                for (int y = (int)h - 1; y >= 0; y--) {
                    const u8* srow = (const u8*)mp + (u64)y * s_rtsave_pitch;
                    memset(line, 0, rowb);
                    for (u32 x = 0; x < w; x++) {
                        float rv, gv, bv;
                        if (s_rtsave_dxgi == DXGI_FORMAT_R16G16B16A16_FLOAT) {
                            const u16* hp16 = (const u16*)(srow + (u64)x * 8);
                            /* crude half->float: sign|exp|mant */
                            float v[3];
                            for (int c2 = 0; c2 < 3; c2++) {
                                u16 hv = hp16[c2];
                                u32 sign = (hv >> 15) & 1, exp = (hv >> 10) & 0x1F, man = hv & 0x3FF;
                                float fv;
                                if (exp == 0) fv = (float)man / 16777216.0f;
                                else { u32 fb = (sign << 31) | ((exp - 15 + 127) << 23) | (man << 13);
                                       memcpy(&fv, &fb, 4); }
                                v[c2] = fv;
                            }
                            rv = v[0]; gv = v[1]; bv = v[2];
                        } else if (s_rtsave_dxgi == DXGI_FORMAT_R32G32B32A32_FLOAT) {
                            const float* fp32 = (const float*)(srow + (u64)x * 16);
                            rv = fp32[0]; gv = fp32[1]; bv = fp32[2];
                        } else {
                            const u8* p8 = srow + (u64)x * 4;
                            rv = p8[0] / 255.0f; gv = p8[1] / 255.0f; bv = p8[2] / 255.0f;
                        }
                        /* RTT_SAVEA=1: show the alpha lane in RED (gates
                         * like wave's mask.w live there). */
                        if (getenv("RTT_SAVEA")) {
                            float av;
                            if (s_rtsave_dxgi == DXGI_FORMAT_R16G16B16A16_FLOAT) {
                                const u16* hp16 = (const u16*)(srow + (u64)x * 8);
                                u16 hv = hp16[3];
                                u32 sign = (hv >> 15) & 1, exp = (hv >> 10) & 0x1F, man = hv & 0x3FF;
                                if (exp == 0) av = (float)man / 16777216.0f;
                                else { u32 fb = (sign << 31) | ((exp - 15 + 127) << 23) | (man << 13);
                                       memcpy(&av, &fb, 4); }
                            } else if (s_rtsave_dxgi == DXGI_FORMAT_R32G32B32A32_FLOAT) {
                                av = ((const float*)(srow + (u64)x * 16))[3];
                            } else {
                                av = (srow + (u64)x * 4)[3] / 255.0f;
                            }
                            rv = av;
                        }
                        /* |v| tonemap so signed heights are visible */
                        float ar = rv < 0 ? -rv : rv, ag = gv < 0 ? -gv : gv, ab = bv < 0 ? -bv : bv;
                        if (ar > 1) ar = 1; if (ag > 1) ag = 1; if (ab > 1) ab = 1;
                        line[x*3+0] = (u8)(ab * 255.0f);
                        line[x*3+1] = (u8)(ag * 255.0f);
                        line[x*3+2] = (u8)(ar * 255.0f);
                    }
                    fwrite(line, 1, rowb, f);
                }
                free(line);
                fclose(f);
                printf("[D3D12] wrote rt_save.bmp (%ux%u dxgi=%u)\n", w, h, s_rtsave_dxgi);
            }
            s_rtsave_buf->lpVtbl->Unmap(s_rtsave_buf, 0, NULL);
        }
        s_rtsave_state = 0;
    }

    /* rsxcap: snapshot whole frames (backbuffer + every offscreen RT + a draw
     * manifest) at RSX_CAP=start[:count[:stride]] frame_counts. Runs before the
     * present transition would matter -- the backbuffer is in PRESENT here and
     * is restored to PRESENT after each readback. */
    if (cap_due() && s_dbg_last_draws > 0) {
        rsx_capture_frame(fi, s_dbg_last_draws, s_cap_done);
        s_cap_done++;
    }
    /* RSX_CAP_SHOT=1: a screenshot (F12) captures its frame too (captures
     * 400+), and the frame after it (500+): a script's picture of a moment
     * comes with that frame's targets. God of War: Ascension lights a frame
     * with the shadow mask the batch before made, so the pair is needed. */
    { static int on = -1, k = 0, next = -1;
      if (on < 0) { const char* e = getenv("RSX_CAP_SHOT"); on = e ? atoi(e) : 0; }
      if (on && next >= 0 && s_dbg_last_draws > 0) {
          rsx_capture_frame(fi, s_dbg_last_draws, 500 + (u32)next);
          next = -1;
      }
      if (s_shot_cap) {
          if (on && s_dbg_last_draws > 0) { rsx_capture_frame(fi, s_dbg_last_draws, 400 + (u32)k); next = k++; }
          s_shot_cap = 0;
      } }
    { static int yn = -1, yi = 0;
      if (yn < 0) { const char* e = getenv("RSX_CAP_YELLOW"); yn = e ? atoi(e) : 0; }
      int yk = 0;
      if (yi < yn && s_dbg_last_draws > 0 && (yk = cap_batch_yellow(fi)) != 0) {
          fprintf(stderr, "[RSXCAP] yellow frame %llu (%s)%c", (unsigned long long)s_d3d.frame_count,
                  yk == 1 ? "a display this batch drew" : "the backbuffer only", 10);
          rsx_capture_frame(fi, s_dbg_last_draws, 100 + yi);
          yi++;
      } }
    /* RSX_CAP_DIP=<n>: up to n frames that draw less than half of what the
     * frame before drew, each with the frame after it (captures 200+ and
     * 300+). In the menus 70 presents carried less than half the draws of
     * both neighbours, the suspects for the flicker reported there. */
    { static int dn = -1, di = 0, after = -1;
      static u32 prev;
      if (dn < 0) { const char* e = getenv("RSX_CAP_DIP"); dn = e ? atoi(e) : 0; }
      if (dn && s_dbg_last_draws > 0) {
          if (after >= 0) {
              rsx_capture_frame(fi, s_dbg_last_draws, 300 + (u32)after);
              fprintf(stderr, "[RSXCAP] after dip %d: frame %llu, %u draws%c", after,
                      (unsigned long long)s_d3d.frame_count, s_dbg_last_draws, 10);
              after = -1;
          } else if (di < dn && prev >= 60 && s_dbg_last_draws * 2 < prev) {
              fprintf(stderr, "[RSXCAP] dip %d: frame %llu, %u draws after %u%c", di,
                      (unsigned long long)s_d3d.frame_count, s_dbg_last_draws, prev, 10);
              rsx_capture_frame(fi, s_dbg_last_draws, 200 + (u32)di);
              after = di++;
          }
          prev = s_dbg_last_draws;
      } }
    if (cap_env()) cap_arm();

    /* Present, on the vblank unless D3D12_VSYNC=0: that one is for measuring a
     * title past the display's rate. Skipped for an offscreen-only batch. */
    if (s_present_this_frame) {
        HRESULT phr;
        if (rsx_census_secs()) rsx_census_present();
        if (!d3d12_vsync_off())
            phr = s_d3d.swap_chain->lpVtbl->Present(s_d3d.swap_chain, 1, 0);
        else
            phr = s_d3d.swap_chain->lpVtbl->Present(s_d3d.swap_chain, 0,
                                                    s_d3d.tearing ? DXGI_PRESENT_ALLOW_TEARING : 0);
        /* The frame the device was lost at: every later D3D call fails, so
         * the screen just stops changing while the title runs on. */
        { static int said = 0;
          if (FAILED(phr) && !said) {
              said = 1;
              device_lost_report("at Present");
              fprintf(stderr, "[D3D12] Present failed hr=0x%08lX removed=0x%08lX at frame %llu (%u ops)\n",
                      (unsigned long)phr,
                      (unsigned long)s_d3d.device->lpVtbl->GetDeviceRemovedReason(s_d3d.device),
                      (unsigned long long)s_d3d.frame_count, s_dbg_last_draws);
          } }
    }
    rdoc_frame_end();

    move_to_next_frame();
    if (s_present_this_frame) bb_resize();

    s_d3d.frame_count++;
    if (s_present_this_frame && ovs_on()) ovs_present();

    /* RPCS3-style titlebar stats, refreshed once a second: presented FPS,
     * draw count of the last frame, and the backbuffer size. */
    {
        static ULONGLONG s_tt0 = 0;
        static u64 s_tframes = 0;
        s_tframes++;
        ULONGLONG tnow = GetTickCount64();
        if (s_tt0 == 0) s_tt0 = tnow;
        if (tnow - s_tt0 >= 1000 && s_d3d.hwnd) {
            const double fps = s_tframes * 1000.0 / (double)(tnow - s_tt0);
            if (d3d12_show_fps() == 1) {
                extern char g_rsx_title_base[128];
                char tb[256];
                snprintf(tb, sizeof(tb), "%s | FPS: %.2f | draws: %u | %ux%u",
                         g_rsx_title_base, fps, s_dbg_last_draws, s_d3d.width, s_d3d.height);
                SetWindowTextA(s_d3d.hwnd, tb);
            } else if (d3d12_show_fps() == 2) {
                snprintf(s_ov_fps, sizeof s_ov_fps, "%.0f FPS", fps);
            }
            s_tframes = 0;
            s_tt0 = tnow;
        }
    }
}

/* ---------------------------------------------------------------------------
 * RSX backend callbacks
 * -----------------------------------------------------------------------*/

static int d3d12_init(void* ud, u32 width, u32 height)
{
    (void)ud;
    printf("[D3D12] Backend init(%ux%u)\n", width, height);
    return 0;
}

static void d3d12_shutdown(void* ud)
{
    (void)ud;
    printf("[D3D12] Backend shutdown\n");
}

static void d3d12_begin_frame(void* ud)
{
    (void)ud;
}

static void d3d12_end_frame(void* ud)
{
    (void)ud;
}

static u32 s_dbg_clears_since_present = 0;   /* CELLMARK_BLINKDBG */
static u32 s_clear_presents = 0;   /* presents issued at clear (frame boundary) */
static unsigned s_last_clear_boundary_flip = 0;
static unsigned s_last_clear_presented_flip = 0;

/* FRAME_BUDGET=1: how much geometry a guest frame actually asks for, versus
 * what the per-frame vertex buffer can hold. A frame that overflows is silently
 * truncated -- the tail of the scene never renders -- which reads as "the object
 * is missing" rather than "the batch is too small". */

static int blink_dbg(void)
{
    static int v = -1;
    if (v < 0) v = getenv("CELLMARK_BLINKDBG") ? 1 : 0;
    return v;
}

static void d3d12_present(void* ud, u32 buffer_id)
{
    (void)ud;
    (void)buffer_id;

    if (blink_dbg())
        printf("[PRESENT] draws=%u clears_since_last=%u\n",
               s_d3d.draw_count, s_dbg_clears_since_present);
    s_dbg_clears_since_present = 0;

    /* An accumulated batch with draws but NONE targeting a display buffer is
     * offscreen pass work only (demosaic flips once per effect pass): showing
     * it would strobe the bare backbuffer clear. Keep accumulating -- the
     * composite draw that targets the display presents the whole chain, in
     * order, in one command list. Empty batches still present (boot/idle). */
    int flip_has_display = (s_d3d.draw_count == 0);
    for (u32 _i = 0; _i < s_d3d.draw_count && _i < MAX_DRAWS; _i++)
        if (!s_d3d.draws[_i].is_clear && s_d3d.draws[_i].rt_off == 0) {
            flip_has_display = 1;
            break;
        }

    if (s_d3d.initialized && flip_has_display)
        render_frame();

    /* FPS tracking */
    ULONGLONG now = GetTickCount64();
    if (now - s_d3d.last_fps_time >= 1000) {
        s_d3d.fps = (u32)s_d3d.frame_count; /* rough estimate */
        s_d3d.last_fps_time = now;
        s_d3d.frame_count = 0;
    }
}

static void d3d12_clear(void* ud, u32 flags, u32 color, float depth, u8 stencil)
{
    (void)ud;

    /* Convert RSX ARGB u32 to float[4] RGBA */
    float cc[4];
    cc[0] = ((color >> 16) & 0xFF) / 255.0f; /* R */
    cc[1] = ((color >> 8) & 0xFF) / 255.0f;  /* G */
    cc[2] = (color & 0xFF) / 255.0f;          /* B */
    cc[3] = ((color >> 24) & 0xFF) / 255.0f;  /* A */
    /* CLEAR_RGB=r,g,b: override the guest clear colour. Black holes (geometry
     * that never rasterized) and black pixels a shader really wrote look
     * identical against a black clear; this separates them in one run. */
    { static int _cinit = 0; static float _co[3]; static int _con = 0;
      if (!_cinit) { _cinit = 1; const char* e = getenv("CLEAR_RGB");
          if (e && sscanf(e, "%f,%f,%f", &_co[0], &_co[1], &_co[2]) == 3) _con = 1; }
      if (_con) { cc[0] = _co[0]; cc[1] = _co[1]; cc[2] = _co[2]; } }

    u32 rt_w = 0, rt_h = 0, mrt[3] = {0, 0, 0};
    u32 rt = current_rt_off(&rt_w, &rt_h, mrt);

    /* An OFFSCREEN clear is just an ordered op in the current frame's pass
     * chain (demosaic clears each effect pass's surface) -- record it, don't
     * touch the frame boundary. So is a clear with no colour bits, wherever
     * it lands: God of War HD clears depth (mask 0x1/0x3) between compositing
     * its scene into the final surface and drawing the HUD over it, and
     * clearing colour there wiped the scene -- the menu came out black. It is
     * not a frame boundary either. */
    if (rt != 0 || !(flags & 0xF0u)) {
        if (s_d3d.draw_count < MAX_DRAWS) {
            D3D12DrawRecord* dr = &s_d3d.draws[s_d3d.draw_count++];
            memset(dr, 0, sizeof(*dr));
            dr->is_vp = 1; dr->is_clear = 1; dr->tex_slot = -1;
            dr->rt_off = rt; memcpy(dr->rt_mrt, mrt, sizeof(mrt)); dr->surf_a = rec_surface_a();
            rec_zeta(dr); rec_mwb(dr);
            dr->rt_w = rt_w; dr->rt_h = rt_h;
            dr->rt_fmt = s_d3d.current_rsx_state ? s_d3d.current_rsx_state->surface_format : 0;
            memcpy(dr->cc, cc, sizeof(cc));
            dr->clear_mask = flags;
            dr->clear_z = depth;
            dr->clear_s = stencil;
            rec_scissor(dr);
        }
        return;
    }

    memcpy(s_d3d.clear_color, cc, sizeof(cc));

    /* A DISPLAY clear marks the start of a new visible frame. If a completed
     * frame is still accumulated (the drain gulped across a frame boundary --
     * guaranteed at the FIFO ring wrap, where rest-of-frame-N + clear-N+1
     * arrive in one batch), PRESENT it now instead of discarding it. Only a
     * batch that actually contains DISPLAY draws is a completed frame: a
     * batch of offscreen pass work (render-to-texture) must keep accumulating
     * until its composite draw arrives, or the screen strobes intermediates. */
    int have_display_draws = 0;
    for (u32 i = 0; i < s_d3d.draw_count && i < MAX_DRAWS; i++)
        if (!s_d3d.draws[i].is_clear && s_d3d.draws[i].rt_off == 0) {
            have_display_draws = 1;
            break;
        }
    if (!have_display_draws)
        return;   /* keep accumulating the in-progress frame */

    /* A title that DOUBLE-BUFFERS the display (DeferredShading clears both
     * 0x0 and 0x440000 per frame, plus a HUD pass) issues several display
     * clears per real frame -- treating each as a boundary presented the
     * 1-2-draw intermediates, strobing black between the full 146-draw
     * frames. Anchor the boundary to the FLIP instead: only present once a
     * cellGcmSetFlip has landed since the last present. Titles that flip once
     * per frame (wave/cellmark/gcmcube) are unaffected -- their single
     * display clear still follows their single flip. Fall back to the old
     * clear-only heuristic for titles that never flip (fc stays 0). */
    static u32 s_frame_draws_max = 0;   /* running max frame size (typical full frame) */
    if (s_d3d.draw_count > s_frame_draws_max) s_frame_draws_max = s_d3d.draw_count;
    {
        extern unsigned cellGcm_flip_request_count(void);
        unsigned fc = cellGcm_flip_request_count();
        if (fc != 0) {
            if (fc == s_last_clear_boundary_flip)
                return;   /* no flip since last present -> not a real boundary */
            /* A double-buffered title issues several display clears per flip
             * (DeferredShading: clear back-buffer, HUD pass, ...). They can
             * arrive in either order, so a small batch here may be an
             * intermediate that precedes the full frame's clear. Don't present
             * (or consume the flip) until the batch is a substantial fraction
             * of a full frame -- keep accumulating so the complete frame lands
             * in one present. Gauged against the running MAX frame size (not
             * the last, which a leaked tiny batch would poison), so it
             * self-scales per title with no fixed threshold. */
            if (s_frame_draws_max > 16 && s_d3d.draw_count < s_frame_draws_max / 4) {
                /* A second flip with the batch still small, though, is not an
                 * intermediate: the title's frames have become small. God of
                 * War HD plays a movie inside the game as one quad a frame
                 * after hundreds of draws of game, and holding those back until
                 * a quarter of the game's size had piled up presented one frame
                 * every four seconds or so while the movie's sound played on.
                 * Take the new size as the title's frame size. */
                if (fc - s_last_clear_boundary_flip < 2u)
                    return;   /* intermediate: keep accumulating, flip stays pending */
                s_frame_draws_max = s_d3d.draw_count;
            }
            s_last_clear_boundary_flip = fc;
        }
    }

    /* RSX_ACCUM_FRAME=1: never present at a clear boundary, so every draw in a
     * guest frame accumulates into one image. A title that clears several times
     * per frame (render-to-texture passes) otherwise gets presented mid-scene,
     * and each captured frame holds only a slice of the geometry -- the floor in
     * one, a wall in the next -- which makes a single model impossible to see. */
    { static int accum = -1;
      if (accum < 0) { const char* e = getenv("RSX_ACCUM_FRAME"); accum = e ? atoi(e) : 0; }
      if (accum) return; }
    if (s_d3d.initialized) {
        if (blink_dbg())
            printf("[CLEAR] presenting %u accumulated draws at frame boundary\n",
                   s_d3d.draw_count);
        if (s_rt_mode > 0) rt_render(1, 1);   /* RSX_RENDER_THREAD: the walk resets the batch below */
        else render_frame();
        s_clear_presents++;
        s_last_clear_presented_flip = s_last_clear_boundary_flip;
    }
    s_dbg_clears_since_present++;
    for (u32 _i = 0; _i < s_d3d.draw_count && _i < MAX_DRAWS; _i++) {
        s_d3d.draws[_i].indexed = 0;
        s_d3d.draws[_i].raw = 0;
    }
    s_ix_run_valid = 0;
    s_raw_run_valid = 0;
    s_seg_n = 0;
    vpass_reset();
    s_d3d.draw_count   = 0;
    s_d3d.vb_offset    = 0;
    s_d3d.vp_vb_offset = 0;
    s_d3d.vp_ib_offset = 0;
}

static void d3d12_set_render_target(void* ud, const rsx_state* state)
{
    (void)ud;
    s_d3d.current_rsx_state = state;
    /* Log only the first few; set_render_target is called every frame and
     * floods the log otherwise. */
    static int s_count = 0;
    if (s_count < 5) {
        printf("[D3D12] set_render_target(%ux%u)\n",
               state->surface_clip_w, state->surface_clip_h);
        s_count++;
    }
}

static void d3d12_set_viewport(void* ud, const rsx_state* state)
{
    (void)ud;
    /* TODO: update D3D12 viewport from RSX state */
    (void)state;
}

/* Our host vertex layout for the fallback path: position (xyz) + color (rgba)
 * + texcoord (uv), 36 bytes. (The real VP path feeds raw float4 attrib0.) */
typedef struct { float x, y, z; float r, g, b, a; float u, v; } BasicVertex;


/* Read one RSX vertex (by absolute vertex index) from guest memory into our
 * host layout. Position is attrib 0 (float3+), color is attrib 3 (ubyte4 or
 * float4); missing attribs default to opaque white. RSX stores each 32-bit
 * component big-endian, so every lane is byte-swapped. */
static void read_rsx_vertex(const rsx_state* state, u32 vindex, BasicVertex* out)
{
    extern uint8_t* vm_base;
    extern u32 cellGcmResolveOffset(u32);

    out->x = out->y = out->z = 0.0f;
    out->r = out->g = out->b = out->a = 1.0f;
    out->u = out->v = 0.0f;
    if (!state || !vm_base) return;

    const rsx_vertex_attrib* pos = &state->vertex_attribs[0];
    if (pos->enabled && pos->type == 2 /* float */ && pos->size >= 2) {
        u8* src = vm_base + cellGcmResolveOffset(pos->offset + vindex * pos->stride);
        out->x = rsx_rd_bef(src);
        out->y = rsx_rd_bef(src + 4);
        if (pos->size >= 3) out->z = rsx_rd_bef(src + 8);

        /* dbgfont (and similar 2D overlays) store screen positions biased far
         * outside clip space; their vertex program folds them back to clip
         * space using transform constants we don't execute. When a position is
         * well outside NDC, recover the fractional part as a normalized [0,1]
         * screen coord and map it to NDC (screen Y-down -> clip Y-up). In this
         * layout the attribute is [posX, posY, U, V] (size 4), so the last two
         * components are the atlas texcoords, not depth.
         * TODO: execute the real vertex-program / RSX viewport transform so
         * this is not needed. */
        if (out->x > 2.0f || out->x < -2.0f || out->y > 2.0f || out->y < -2.0f) {
            float sx = out->x - floorf(out->x);
            float sy = out->y - floorf(out->y);
            out->x = sx * 2.0f - 1.0f;
            out->y = 1.0f - sy * 2.0f;
            out->z = 0.0f;
            if (pos->size >= 4) {
                out->u = rsx_rd_bef(src + 8);   /* U */
                out->v = rsx_rd_bef(src + 12);  /* V */
            }
        }
    }

    const rsx_vertex_attrib* col = &state->vertex_attribs[3];
    if (col->enabled && col->size >= 3) {
        u8* src = vm_base + cellGcmResolveOffset(col->offset + vindex * col->stride);
        if (col->type == 4 /* ubyte */) {
            out->r = src[0] / 255.0f;
            out->g = src[1] / 255.0f;
            out->b = src[2] / 255.0f;
            out->a = (col->size >= 4) ? src[3] / 255.0f : 1.0f;
        } else if (col->type == 2 /* float */) {
            u32 fr, fg, fb, fa;
            memcpy(&fr, src,     4); fr = ((fr>>24)&0xFF)|((fr>>8)&0xFF00)|((fr<<8)&0xFF0000)|((fr<<24)&0xFF000000);
            memcpy(&fg, src + 4, 4); fg = ((fg>>24)&0xFF)|((fg>>8)&0xFF00)|((fg<<8)&0xFF0000)|((fg<<24)&0xFF000000);
            memcpy(&fb, src + 8, 4); fb = ((fb>>24)&0xFF)|((fb>>8)&0xFF00)|((fb<<8)&0xFF0000)|((fb<<24)&0xFF000000);
            memcpy(&out->r, &fr, 4); memcpy(&out->g, &fg, 4); memcpy(&out->b, &fb, 4);
            if (col->size >= 4) {
                memcpy(&fa, src + 12, 4); fa = ((fa>>24)&0xFF)|((fa>>8)&0xFF00)|((fa<<8)&0xFF0000)|((fa<<24)&0xFF000000);
                memcpy(&out->a, &fa, 4);
            }
        }
    }
}

/* Upload `count` sequential vertices [first, first+count). Returns the count
 * actually written (clamped to the remaining per-frame buffer). */
static u32 upload_vertices_from_rsx(u32 first, u32 count)
{
    BasicVertex* verts = (BasicVertex*)((u8*)s_d3d.vb_mapped + s_d3d.vb_offset);
    u32 max_verts = (MAX_VERTICES * VERTEX_STRIDE - s_d3d.vb_offset) / sizeof(BasicVertex);
    if (count > max_verts) count = max_verts;
    const rsx_state* state = s_d3d.current_rsx_state;
    for (u32 i = 0; i < count; i++)
        read_rsx_vertex(state, first + i, &verts[i]);
    s_d3d.vb_offset += count * sizeof(BasicVertex);
    return count;
}

/* Upload RSX QUADS (prim 8) as a triangle list: each 4-vertex quad v0..v3
 * (perimeter winding) splits into triangles (v0,v1,v2) and (v0,v2,v3).
 * D3D12 has no quad topology, so this expansion is how quads render at all.
 * Returns the number of triangle-list vertices emitted (6 per quad). */
static u32 upload_quads_from_rsx(u32 first, u32 count)
{
    const rsx_state* state = s_d3d.current_rsx_state;
    u32 quads = count / 4;
    u32 max_verts = (MAX_VERTICES * VERTEX_STRIDE - s_d3d.vb_offset) / sizeof(BasicVertex);
    if (quads * 6 > max_verts) quads = max_verts / 6;
    BasicVertex* verts = (BasicVertex*)((u8*)s_d3d.vb_mapped + s_d3d.vb_offset);
    u32 o = 0;
    for (u32 q = 0; q < quads; q++) {
        BasicVertex c[4];
        for (u32 k = 0; k < 4; k++)
            read_rsx_vertex(state, first + q * 4 + k, &c[k]);
        verts[o++] = c[0]; verts[o++] = c[1]; verts[o++] = c[2];
        verts[o++] = c[0]; verts[o++] = c[2]; verts[o++] = c[3];
    }
    s_d3d.vb_offset += o * sizeof(BasicVertex);
    return o;
}

/* Upload RSX QUADS as raw float4 attrib0 (byte-swapped) into vp_vb, expanded
 * to a triangle list (6 verts/quad). The decompiled vertex shader does the
 * transform. Returns emitted vertex count. */
/* Generic VP vertex: all 16 RSX vertex attributes, each converted to a float4
 * slot -- 256 bytes/vertex, input layout ATTRi @ i*16. Apps place attributes at
 * arbitrary indices (tiny3d: pos=a0 colour=a3 tex=a8; SDK gcm samples: pos=a0
 * colour=a1 tex=a2; dbgfont: a0..a2), so a hardcoded pos+colour pair can't
 * cover them: every enabled attrib is fetched from guest memory and converted
 * by its RSX type; disabled slots read (0,0,0,1). */
typedef struct { float v[4]; } VPSlot;
#define VP_VERT_STRIDE (16 * sizeof(VPSlot))   /* 256 */


void rsx_vtx_pos_dbg(const rsx_state* state, const float* v, u32 n);

/* Fill all 16 attribute slots for one vertex. The per-attribute work lives in
 * rsx_vertex_fetch.c so the Metal and null backends read guest vertices the
 * same way this one does, rather than each porting the logic again. */
/* VTX_STATS=1: every 5 s, what the vertex path is given and what it does with
 * it -- draws by primitive, index references against distinct vertices, the
 * conversions actually run, vertices written, attribute formats, and the time
 * the indexed uploads take. */
static int s_vs_on = -1;
static struct {
    unsigned long long draws_idx[11], draws_arr[11], refs_idx, refs_arr, uniq, range, conv, out;
    unsigned long long attr[8][5], nattr[17], freq, wide;
    unsigned long long raw_segs, raw_ext, raw_bytes;
    long long t_up, t0;
    /* by kind ([1] the draws from a registered vertex ring): indexed draws, index references,
     * the path taken (raw fetch, ix, the CPU's conversions), raw bytes copied, upload time */
    unsigned long long c_draws[2], c_refs[2], c_raw[2], c_ix[2], c_cpu[2], c_raw_bytes[2];
    long long c_t[2];
    /* the vertex pass: segments lit on the GPU and their vertexes, segments of its stride in a ring
     * with no record, CPU-lit copies and guest blocks, segments the buffers had no room for */
    unsigned long long lit_segs, lit_verts, lit_miss, lit_cpu, lit_guest, lit_full, lit_lost, lit_stale;
    /* a vertex ring's indexed draws by the face culling they draw with (rsx_cull_key), attribute
     * 0's stride 54 (EDGE's lit layout) or another */
    unsigned long long c_cull54[8], c_cullo[8];
} s_vs;

/* ---- Vertex rings a port names (God of War: Ascension's EDGE output rings) ----
 * Main-memory IO offsets [io_offset, io_offset + size). Phase 2b's first step: with VTX_STATS=1
 * the indexed draws whose position array (attribute 0) lies in one are counted apart. */
#define VR_MAX 8
static u32 s_vr_off[VR_MAX], s_vr_size[VR_MAX];
static volatile LONG s_vr_n, s_vr_lock;
static int s_vr_cls;                  /* the indexed draw being uploaded: 1 a ring's */
/* per ring, the vertex pass's records: a word per 128 bytes (a ring's allocations are 128-byte
 * blocks, each with one vertex array), its sequence << 32 | its IO offset */
static volatile LONG64* s_vr_idx[VR_MAX];
/* and the kept blocks (rsx_kept.h), a slot per 128 bytes */
typedef struct { rsx_kept* volatile k; volatile u32 io, alt_on; u8 alt[16]; } vr_kept;
static vr_kept* s_vr_kept[VR_MAX];
static volatile LONG s_kept_seen;
void rsx_d3d12_register_vertex_ring(u32 io_offset, u32 size)
{
    if (!size) return;
    for (LONG i = 0, n = s_vr_n; i < n; i++)
        if (s_vr_off[i] == io_offset && s_vr_size[i] == size) return;
    while (InterlockedCompareExchange(&s_vr_lock, 1, 0) != 0) YieldProcessor();
    const LONG n = s_vr_n;
    int known = 0;
    for (LONG i = 0; i < n; i++)
        if (s_vr_off[i] == io_offset && s_vr_size[i] == size) known = 1;
    if (!known && n < VR_MAX) {
        s_vr_off[n] = io_offset;
        s_vr_size[n] = size;
        s_vr_idx[n] = (volatile LONG64*)calloc((size >> 7) + 1, sizeof(LONG64));
        s_vr_kept[n] = (vr_kept*)calloc((size >> 7) + 1, sizeof(vr_kept));
        MemoryBarrier();
        InterlockedExchange(&s_vr_n, n + 1);
        fprintf(stderr, "[vring] ring %ld: io 0x%08X, %u bytes\n", (long)n, io_offset, size);
    }
    InterlockedExchange(&s_vr_lock, 0);
}
static int vr_class(const rsx_state* st)
{
    const LONG n = s_vr_n;
    if (!n || !st || !st->vertex_attribs[0].enabled) return 0;
    const u32 raw = st->vertex_attribs[0].offset;
    if (!(raw & 0x80000000u)) return 0;           /* local memory */
    const u32 off = raw & 0x7FFFFFFFu;
    for (LONG i = 0; i < n; i++)
        if (off - s_vr_off[i] < s_vr_size[i]) return 1;
    return 0;
}

/* ---- The vertex pass (phase 2b.1 of the God of War: Ascension port) ----
 * A port registers a compute shader that rewrites a draw's raw copy on the GPU (EDGE's lighting:
 * ids 27-30 of its 54-byte vertexes, from the position, normal and tangent, and a record of the
 * job's lights), and publishes a record for the vertexes at an IO offset in a vertex ring. The
 * stride's high half is a second layout's (0 none): the record says which of the two it is.
 *  - A record carries the first 16 bytes the job put out at its key: it is taken only while guest
 *    memory holds them there (an older job's record at the same place in a ring is not). A publish
 *    without a record clears the key.
 *  - A raw segment whose attribute 0 lies at a published key (its copy whole vertexes of the
 *    pass's stride) gets a lit copy in vp_lit, a default-heap buffer with a half per vp_parity: an
 *    entry in the batch's table (the raw copy's header, the lit copy's header, the start of
 *    attribute 0's copy from the header, its vertex count with the copy's first vertex in the high
 *    half, the record's offset in the table, the segment's bytes from the header), and the
 *    segment's draws read the lit copy. The pass copies
 *    the whole segment, with the other arrays the draw has enabled (EDGE's draws keep some from
 *    earlier draws), and rewrites attribute 0's.
 *  - At the batch's end the pass runs, a group a segment, in a command list of its own executed
 *    before the batch's: no barrier among the draws. vp_lit is a UAV there, and back in COMMON
 *    (read by the draws through implicit promotion).
 *  - Fallbacks, the port's C version of the pass: a draw that does not take the raw fetch (its
 *    vertexes lit in guest memory, once a record), a full lit buffer or table, a shader that did
 *    not build (the raw copy lit as it is made).
 *  - A record marked applied (the port's check mode: the SPU lit the vertexes as well) is drawn as
 *    the guest wrote it. VTX_LIT_CHECK=N compares every Nth of those segments' lit copies with the
 *    guest's bytes, read back after the batch, for the vertexes the segment's first batch names
 *    (a copy spans the batch's index range: the vertexes of culled triangles in it are lit only
 *    here, the SPU's arrays keep what they held); VTX_LIT_DRAW=1 draws the lit copies instead.
 * VTX_LIT_MB: the size of a vp_lit half (default 40). */
#define VPR_POOL 8192u                 /* more than a ring set's jobs in flight */
typedef struct { volatile LONG seq; u32 key, n, applied, bytes, skin; volatile LONG done; u8 tag[16]; } vpr_head;
static u8* s_vpr_pool;                 /* VPR_POOL slots: a head, then the record */
static u32 s_vpr_slot;
static volatile LONG s_vpr_seq;

/* From any thread (rsx_kept.h). */
void rsx_d3d12_kept_publish(u32 io_offset, rsx_kept* k)
{
    const LONG nr = s_vr_n;
    for (LONG r = 0; r < nr; r++) {
        const u32 off = io_offset - s_vr_off[r];
        if (off >= s_vr_size[r]) continue;
        vr_kept* e = s_vr_kept[r] ? &s_vr_kept[r][off >> 7] : NULL;
        if (!e) return;
        if (!k && !e->k) return;
        e->io = io_offset;
        e->alt_on = 0;
        MemoryBarrier();
        e->k = k;
        if (k && !s_kept_seen) InterlockedExchange(&s_kept_seen, 1);
        return;
    }
}

static vr_kept* kept_slot(u32 io)
{
    const LONG nr = s_vr_n;
    for (LONG r = 0; r < nr; r++) {
        const u32 off = io - s_vr_off[r];
        if (off < s_vr_size[r]) return s_vr_kept[r] ? &s_vr_kept[r][off >> 7] : NULL;
    }
    return NULL;
}

/* The kept block of a draw's attribute 0, if guest memory still holds its first bytes. */
static rsx_kept* kept_find(const rsx_state* st, u32* io)
{
    extern uint8_t* vm_base;
    extern u32 cellGcmResolveLocated(int local, u32 offset);
    if (!s_kept_seen || !st || !st->vertex_attribs[0].enabled) return NULL;
    const u32 raw = st->vertex_attribs[0].offset;
    if (!(raw & 0x80000000u)) return NULL;
    const u32 a = raw & 0x7FFFFFFFu;
    const LONG nr = s_vr_n;
    for (LONG r = 0; r < nr; r++) {
        const u32 off = a - s_vr_off[r];
        if (off >= s_vr_size[r]) continue;
        const vr_kept* e = s_vr_kept[r] ? &s_vr_kept[r][off >> 7] : NULL;
        if (!e) return NULL;
        rsx_kept* k = e->k;
        if (!k || e->io != a || k->stride != st->vertex_attribs[0].stride) return NULL;
        const u8* g = vm_base + cellGcmResolveLocated(0, a);
        if (memcmp(g, k->tag, 16) != 0 && !(e->alt_on && memcmp(g, e->alt, 16) == 0)) return NULL;
        *io = a;
        return k;
    }
    return NULL;
}

static HANDLE s_vpass_cthread;              /* the pass's program being compiled (vpass_compile) */
static DWORD WINAPI vpass_compile(LPVOID arg);
void rsx_d3d12_vertex_pass_set(const char* hlsl, u32 stride, u32 max_record,
                               void (*cpu)(const void* rec, const u8* src, u8* dst, u32 n))
{
    if (s_vpass.set || !hlsl || !stride || !max_record || max_record > 65536u) return;
    s_vpr_slot = ((u32)sizeof(vpr_head) + max_record + 15u) & ~15u;
    s_vpr_pool = (u8*)calloc(VPR_POOL, s_vpr_slot);
    s_vpass.rec_tmp = (u8*)malloc(max_record);
    if (!s_vpr_pool || !s_vpass.rec_tmp) return;
    s_vpass.hlsl = hlsl;
    s_vpass.stride = stride & 0xFFFFu;
    s_vpass.stride2 = stride >> 16;
    s_vpass.max_rec = max_record;
    s_vpass.cpu = cpu;
    MemoryBarrier();
    InterlockedExchange(&s_vpass.set, 1);
    fprintf(stderr, "[vtx-lit] vertex pass set: stride %u (and %u), records up to %u bytes%c", s_vpass.stride,
            s_vpass.stride2, max_record, 10);
    s_vpass_cthread = CreateThread(NULL, 0, vpass_compile, NULL, 0, NULL);
    if (s_vpass_cthread) SetThreadPriority(s_vpass_cthread, THREAD_PRIORITY_BELOW_NORMAL);
}

/* From any thread (an SPU worker's job): the record for n vertexes at io; it replaces the one there
 * before (the ring has been handed back for it). Not in a registered ring: dropped. */
void rsx_d3d12_vertex_pass_publish(u32 io_offset, u32 n, const void* rec, u32 bytes, u32 applied,
                                   const void* tag16, const void* skin, u32 skin_bytes)
{
    if (!s_vpass.set || bytes + skin_bytes > s_vpass.max_rec) return;
    const LONG nr = s_vr_n;
    for (LONG r = 0; r < nr; r++) {
        const u32 off = io_offset - s_vr_off[r];
        if (off >= s_vr_size[r]) continue;
        if (!s_vr_idx[r]) return;
        if (!rec || !bytes || !tag16) {             /* no record: the key cleared */
            InterlockedExchange64(&s_vr_idx[r][off >> 7], 0);
            return;
        }
        LONG seq = InterlockedIncrement(&s_vpr_seq);
        if (seq == 0) seq = InterlockedIncrement(&s_vpr_seq);
        u8* sl = s_vpr_pool + (size_t)((u32)seq % VPR_POOL) * s_vpr_slot;
        vpr_head* h = (vpr_head*)sl;
        InterlockedExchange(&h->seq, 0);                /* being written */
        h->key = io_offset; h->n = n; h->applied = applied; h->bytes = bytes + skin_bytes;
        h->skin = skin_bytes ? bytes : 0;
        memcpy(h->tag, tag16, 16);
        InterlockedExchange(&h->done, 0);
        memcpy(sl + sizeof(vpr_head), rec, bytes);
        if (skin_bytes) memcpy(sl + sizeof(vpr_head) + bytes, skin, skin_bytes);
        MemoryBarrier();
        InterlockedExchange(&h->seq, seq);
        InterlockedExchange64(&s_vr_idx[r][off >> 7], ((LONG64)(u32)seq << 32) | io_offset);
        return;
    }
}

/* The record published for io (the frame clock thread): its slot and sequence, or NULL. A slot
 * may be written over VPR_POOL publishes later: copy it out, then see that seq still holds. */
static vpr_head* vpr_find(u32 io, LONG* seq)
{
    if (!s_vpr_pool) return NULL;
    const LONG nr = s_vr_n;
    for (LONG r = 0; r < nr; r++) {
        const u32 off = io - s_vr_off[r];
        if (off >= s_vr_size[r]) continue;
        if (!s_vr_idx[r]) return NULL;
        const LONG64 e = s_vr_idx[r][off >> 7];
        if ((u32)e != io) return NULL;
        const LONG sq = (LONG)(e >> 32);
        vpr_head* h = (vpr_head*)(s_vpr_pool + (size_t)((u32)sq % VPR_POOL) * s_vpr_slot);
        if (h->seq != sq) { if (s_vs_on > 0) s_vs.lit_lost++; return NULL; }
        {   /* the job's first bytes still there: not an older job's record */
            extern uint8_t* vm_base;
            extern u32 cellGcmResolveLocated(int local, u32 offset);
            if (!vm_base || memcmp(vm_base + cellGcmResolveLocated(0, io), h->tag, 16) != 0) {
                if (s_vs_on > 0) s_vs.lit_stale++;
                return NULL;
            }
        }
        *seq = sq;
        return h;
    }
    return NULL;
}
/* the record out to dst; 0 if it was written over meanwhile */
static int vpr_copy(const vpr_head* h, LONG seq, u8* dst)
{
    const u32 bytes = h->bytes;
    if (bytes > s_vpass.max_rec) return 0;
    memcpy(dst, (const u8*)h + sizeof(vpr_head), bytes);
    MemoryBarrier();
    return h->seq == seq;
}

static int vpass_check_n(void)
{
    static int n = -1;
    if (n < 0) { const char* e = getenv("VTX_LIT_CHECK"); n = e ? atoi(e) : 0; if (n < 0) n = 0; }
    return n;
}
static int vpass_draw_lit(void)
{
    static int d = -1;
    if (d < 0) { const char* e = getenv("VTX_LIT_DRAW"); d = e && e[0] == '1'; }
    return d;
}

/* The pass's program, compiled on a thread of its own from the moment the pass is set (a low
 * priority one): with God of War: Ascension's skinned position done the SPU's way in integer
 * operations, FXC takes ~5.6 s over it (1.5 s before). Nothing waits for it: until it is there,
 * vpass_build says no and the CPU lights the pass's draws (vpass_segment's vpass_cpu_copy). Under
 * Proton, D3DCompile is Wine's, on vkd3d-shader, and it was still at this program after a minute
 * while the frame clock thread waited for it (issue #12). */
static ID3DBlob* s_vpass_cs;
static HRESULT s_vpass_chr;
static char s_vpass_cerr[1024];
static double s_vpass_cdt;
static int s_vpass_cached;
static int s_vpass_dxc;                     /* the program is DXC's, at shader model 6.0 */
static volatile LONG s_vpass_cdone;         /* vpass_compile's thread has done its work */
/* Which compiler makes it: the one of the port's own programs (shc_pick_compiler). D3DCompile, which
 * the check (VTX_LIT_CHECK) finds the nearer to the SPU: at the temple 94.5% of the lit F16 values the
 * same, against DXC's 86%, both within 0.004. Under Wine, DXC: 0.7 s here. VTX_LIT_COMPILER=dxc or fxc
 * chooses for this one alone. */
static int vpass_dxc_wanted(void)
{
    const char* c = getenv("VTX_LIT_COMPILER");
    if (c && ps3_settings_ieq(c, "fxc")) return 0;
    if (c && ps3_settings_ieq(c, "dxc")) return s_dxc_usable;
    return rsx_own_dxc();
}
/* Through the shader cache file (graphics.shader_cache) too, keyed apart from the guest programs and
 * DXC's apart from D3DCompile's: compiled the first time, read back after. The precompile builds it
 * before the game starts (rsx_d3d12_precompile_vertex_pass), under the same key. */
static HRESULT vpass_program(const char* src, ID3DBlob** out, int* cached, int* dxc, char* err_text, size_t err_len)
{
    const size_t len = strlen(src);
    const UINT flags = D3DCOMPILE_OPTIMIZATION_LEVEL3;
    *out = NULL;
    *cached = 0;
    AcquireSRWLockExclusive(&s_shc_lock);
    if (s_shc_on < 0) shc_open();
    *dxc = vpass_dxc_wanted();
    const u64 key = shc_key(src, len, *dxc ? "cs_6_0" : "cs_5_0", flags | 0x80000000u);
    if (s_shc_on && s_shc_cap) {
        ShaderCacheEntry* e = shc_slot(key, (u32)len);
        if (e->prog) *out = shp_blob(e->prog);
    }
    ReleaseSRWLockExclusive(&s_shc_lock);
    if (*out) {
        *cached = 1;
        return S_OK;
    }
    ID3DBlob* err = NULL;
    const HRESULT hr = *dxc ? rsx_dxc_compile(src, len, "cs_5_0", shc_dxc_level(flags), out, &err)
                            : D3DCompile(src, len, "vertex_pass", NULL, NULL, "main", "cs_5_0", flags, 0, out, &err);
    if (err) {
        if (err_text) snprintf(err_text, err_len, "%s", (const char*)err->lpVtbl->GetBufferPointer(err));
        err->lpVtbl->Release(err);
    }
    if (SUCCEEDED(hr) && *out) {
        AcquireSRWLockExclusive(&s_shc_lock);
        if (s_shc_on) shc_keep(key, (u32)len, *out);
        ReleaseSRWLockExclusive(&s_shc_lock);
    }
    return hr;
}
static void vpass_compile_now(void)
{
    const double t0 = perf_now();
    int dxc = 0;
    s_vpass_chr = vpass_program(s_vpass.hlsl, &s_vpass_cs, &s_vpass_cached, &dxc, s_vpass_cerr, sizeof s_vpass_cerr);
    s_vpass_dxc = dxc;
    s_vpass_cdt = perf_now() - t0;
}

int rsx_d3d12_precompile_vertex_pass(const char* hlsl, int* compiled, char* why, size_t why_len)
{
    *compiled = 0;
    if (!hlsl || !*hlsl) { snprintf(why, why_len, "no vertex pass text"); return -1; }
    if (rsx_d3d12_precompile_open(why, why_len) != 0) return -1;
    ID3DBlob* b = NULL;
    int cached = 0, dxc = 0;
    char err[512] = "";
    const HRESULT hr = vpass_program(hlsl, &b, &cached, &dxc, err, sizeof err);
    if (b) b->lpVtbl->Release(b);
    if (FAILED(hr)) {
        snprintf(why, why_len, "the vertex pass did not compile (0x%08lX): %s", (unsigned long)hr, err);
        return -1;
    }
    *compiled = !cached;
    return 0;
}
static DWORD WINAPI vpass_compile(LPVOID arg)
{
    (void)arg;
    vpass_compile_now();
    InterlockedExchange(&s_vpass_cdone, 1);
    return 0;
}

/* The compute pipeline, the buffers and the command list, on first use (the frame clock thread).
 * While the program is still being compiled it says no, and is asked again at the next draw. */
static int vpass_build(void)
{
    if (s_vpass.state) return s_vpass.state > 0;
    if (s_vpass_cthread && !InterlockedCompareExchange(&s_vpass_cdone, 0, 0)) {
        if (!s_vpass.cpu_since) {
            s_vpass.cpu_since = perf_now();
            fprintf(stderr, "[vtx-lit] the vertex pass is not compiled yet: the CPU lights its draws until it is%c", 10);
        }
        return 0;
    }
    s_vpass.state = -1;
    if (!s_vpass.hlsl || !s_d3d.device) return 0;
    ID3D12Device* dev = s_d3d.device;
    ID3DBlob *cs = NULL, *err = NULL, *sig = NULL;
    HRESULT hr;
    if (s_vpass_cthread) {
        WaitForSingleObject(s_vpass_cthread, INFINITE);   /* it has done its work: s_vpass_cdone */
        CloseHandle(s_vpass_cthread);
        s_vpass_cthread = NULL;
    } else {
        vpass_compile_now();                               /* no thread for it: here, waited for */
    }
    hr = s_vpass_chr;
    cs = s_vpass_cs;
    s_vpass_cs = NULL;
    fprintf(stderr, "[vtx-lit] the vertex pass %s in %.1f s; the CPU lit its draws for %.1f s before it%c",
            s_vpass_cached ? "read from the shader cache" : s_vpass_dxc ? "compiled by DXC" : "compiled by D3DCompile",
            s_vpass_cdt, s_vpass.cpu_since > 0.0 ? perf_now() - s_vpass.cpu_since : 0.0, 10);
    if (FAILED(hr) || !cs) {
        fprintf(stderr, "[vtx-lit] the vertex pass did not compile (0x%08lX): %s%c", hr, s_vpass_cerr, 10);
        if (cs) cs->lpVtbl->Release(cs);
        return 0;
    }
    D3D12_ROOT_PARAMETER rp[4];
    memset(rp, 0, sizeof rp);
    rp[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV; rp[0].Descriptor.ShaderRegister = 0;
    rp[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV; rp[1].Descriptor.ShaderRegister = 1;
    rp[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV; rp[2].Descriptor.ShaderRegister = 0;
    rp[3].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    rp[3].Constants.ShaderRegister = 0; rp[3].Constants.Num32BitValues = 4;
    for (int i = 0; i < 4; i++) rp[i].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    D3D12_ROOT_SIGNATURE_DESC rd = {0};
    rd.NumParameters = 4;
    rd.pParameters = rp;
    hr = D3D12SerializeRootSignature(&rd, D3D_ROOT_SIGNATURE_VERSION_1, &sig, &err);
    if (SUCCEEDED(hr))
        hr = dev->lpVtbl->CreateRootSignature(dev, 0, sig->lpVtbl->GetBufferPointer(sig), sig->lpVtbl->GetBufferSize(sig),
                                              &IID_ID3D12RootSignature, (void**)&s_vpass.rs);
    if (sig) sig->lpVtbl->Release(sig);
    if (err) err->lpVtbl->Release(err);
    if (SUCCEEDED(hr)) {
        D3D12_COMPUTE_PIPELINE_STATE_DESC pd = {0};
        pd.pRootSignature = s_vpass.rs;
        pd.CS.pShaderBytecode = cs->lpVtbl->GetBufferPointer(cs);
        pd.CS.BytecodeLength = cs->lpVtbl->GetBufferSize(cs);
        hr = dev->lpVtbl->CreateComputePipelineState(dev, &pd, &IID_ID3D12PipelineState, (void**)&s_vpass.pso);
    }
    cs->lpVtbl->Release(cs);
    if (FAILED(hr)) { fprintf(stderr, "[vtx-lit] the vertex pass's pipeline failed (0x%08lX)%c", hr, 10); return 0; }
    { const char* e = getenv("VTX_LIT_MB"); u32 mb = e ? (u32)atoi(e) : 40u;
      if (mb < 4) mb = 4;
      if (mb > 512) mb = 512;
      s_vpass.lit_half = mb << 20; }
    D3D12_HEAP_PROPERTIES hp = {0};
    D3D12_RESOURCE_DESC bd = {0};
    bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    bd.Height = 1; bd.DepthOrArraySize = 1; bd.MipLevels = 1;
    bd.SampleDesc.Count = 1;
    bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    bd.Width = 2ull * s_vpass.lit_half;
    bd.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    hr = dev->lpVtbl->CreateCommittedResource(dev, &hp, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COMMON, NULL,
                                              &IID_ID3D12Resource, (void**)&s_vpass.lit);
    if (SUCCEEDED(hr)) {
        hp.Type = D3D12_HEAP_TYPE_UPLOAD;
        bd.Width = 2ull * VPT_HALF;
        bd.Flags = D3D12_RESOURCE_FLAG_NONE;
        hr = dev->lpVtbl->CreateCommittedResource(dev, &hp, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_GENERIC_READ,
                                                  NULL, &IID_ID3D12Resource, (void**)&s_vpass.tab);
    }
    if (SUCCEEDED(hr)) {
        D3D12_RANGE nr0 = {0, 0};
        hr = s_vpass.tab->lpVtbl->Map(s_vpass.tab, 0, &nr0, (void**)&s_vpass.tab_mapped);
    }
    for (int i = 0; SUCCEEDED(hr) && i < FRAME_COUNT; i++)
        hr = dev->lpVtbl->CreateCommandAllocator(dev, D3D12_COMMAND_LIST_TYPE_DIRECT, &IID_ID3D12CommandAllocator,
                                                 (void**)&s_vpass.alloc[i]);
    if (SUCCEEDED(hr))
        hr = dev->lpVtbl->CreateCommandList(dev, 0, D3D12_COMMAND_LIST_TYPE_DIRECT, s_vpass.alloc[0], NULL,
                                            &IID_ID3D12GraphicsCommandList, (void**)&s_vpass.cl);
    if (FAILED(hr)) { fprintf(stderr, "[vtx-lit] the vertex pass's buffers failed (0x%08lX)%c", hr, 10); return 0; }
    s_vpass.cl->lpVtbl->Close(s_vpass.cl);
    s_vpass.state = 1;
    fprintf(stderr, "[vtx-lit] vertex pass ready: stride %u, vp_lit 2 x %u MB%c", s_vpass.stride,
            s_vpass.lit_half >> 20, 10);
    return 1;
}

/* The check (VTX_LIT_CHECK=N): sampled lit copies of applied records, with the guest's bytes. */
#define VCK_MAX 64
#define VCK_BYTES (4u << 20)
static struct { u32 n, used, submitted; u32 off[VCK_MAX], len[VCK_MAX], at[VCK_MAX], stride[VCK_MAX]; u8* spu[VCK_MAX]; u8* used_v[VCK_MAX]; } s_vck[2];
static ID3D12Resource* s_vck_rb[2];
static u32 s_vck_tick;
static struct {
    unsigned long long verts, skipped, units[5], nan;
    unsigned long long v20, u20[4];     /* the second layout's vertexes, its attribute 20 bytes by units apart */
    double max27, maxh[3];
    double wrel, wspu, wgpu;            /* the largest |d| / max(|SPU's|, 1) of ids 28-30, its values */
    int wid;
    long long t0;
} s_vck_st;

/* a segment to check: its lit copy at lit_off, the guest's bytes at src, and the vertexes its
 * indexes name (ix: the guest's index array, n of them from first, 16 or 32 bits; lo the copy's
 * first vertex). The second layout (stride2, God of War: Ascension's 58 bytes) has attribute 20's
 * four U8N bytes at +24 and the rest four bytes on. */
static void vck_add(u32 lit_off, u32 len, u32 src, const u8* ix, int sixteen, u32 first, u32 n, u32 lo, u32 stride)
{
    extern uint8_t* vm_base;
    const u32 par = s_d3d.vp_parity;
    if (++s_vck_tick % (u32)vpass_check_n()) return;
    if (s_vck[par].n >= VCK_MAX || s_vck[par].used + len > VCK_BYTES || !ix || !stride) return;
    const u32 nv = len / stride;
    u8* cp = (u8*)malloc(len);
    u8* uv = (u8*)calloc(nv ? nv : 1, 1);
    if (!cp || !uv) { free(cp); free(uv); return; }
    memcpy(cp, vm_base + src, len);
    for (u32 k = first; k < first + n; k++) {
        const u8* q = ix + (size_t)k * (sixteen ? 2u : 4u);
        const u32 i = sixteen ? (((u32)q[0] << 8) | q[1])
                              : (((u32)q[0] << 24) | ((u32)q[1] << 16) | ((u32)q[2] << 8) | q[3]);
        if (i - lo < nv) uv[i - lo] = 1;
    }
    const u32 k = s_vck[par].n++;
    s_vck[par].off[k] = lit_off; s_vck[par].len[k] = len; s_vck[par].at[k] = s_vck[par].used; s_vck[par].stride[k] = stride;
    s_vck[par].spu[k] = cp;
    s_vck[par].used_v[k] = uv;
    s_vck[par].used += (len + 15u) & ~15u;
}
static void vck_free(u32 par)
{
    for (u32 k = 0; k < s_vck[par].n; k++) { free(s_vck[par].spu[k]); free(s_vck[par].used_v[k]); }
    s_vck[par].n = s_vck[par].used = s_vck[par].submitted = 0;
}
static float vck_half(u32 h)
{
    const u32 e = (h >> 10) & 31u, m = h & 1023u;
    const float v = e == 0 ? (float)m * (1.0f / 16777216.0f) : e == 31 ? 65536.0f * 4.0f
                  : ldexpf(1.0f + (float)m / 1024.0f, (int)e - 15);
    return (h & 0x8000u) ? -v : v;
}
static void vck_cmp(const u8* g, const u8* s, u32 len, const u8* used_v, u32 S)
{
    const int two = s_vpass.stride2 && S == s_vpass.stride2;
    const u32 o27 = two ? 28u : 24u;
    for (u32 v = 0; v + S <= len; v += S) {
        if (!used_v[v / S]) { s_vck_st.skipped++; continue; }
        s_vck_st.verts++;
        if (two) {
            s_vck_st.v20++;
            for (int k = 0; k < 4; k++) {
                const int du = g[v + 24 + k] > s[v + 24 + k] ? g[v + 24 + k] - s[v + 24 + k] : s[v + 24 + k] - g[v + 24 + k];
                s_vck_st.u20[du == 0 ? 0 : du == 1 ? 1 : du < 4 ? 2 : 3]++;
            }
        }
        for (int k = 0; k < 3; k++) {
            const u8* a = g + v + o27 + 4 * k; const u8* b = s + v + o27 + 4 * k;
            const u32 ua = ((u32)a[0] << 24) | ((u32)a[1] << 16) | ((u32)a[2] << 8) | a[3];
            const u32 ub = ((u32)b[0] << 24) | ((u32)b[1] << 16) | ((u32)b[2] << 8) | b[3];
            float fa, fb; memcpy(&fa, &ua, 4); memcpy(&fb, &ub, 4);
            const double d = fabs((double)fa - (double)fb);
            if (!(d == d)) s_vck_st.nan++;
            else if (d > s_vck_st.max27) s_vck_st.max27 = d;
        }
        for (int i = 0; i < 3; i++)
            for (int k = 0; k < 3; k++) {
                const u8* a = g + v + o27 + 12 + 6 * i + 2 * k; const u8* b = s + v + o27 + 12 + 6 * i + 2 * k;
                const u32 ha = ((u32)a[0] << 8) | a[1], hb = ((u32)b[0] << 8) | b[1];
                const u32 ma = ha & 0x7FFFu, mb = hb & 0x7FFFu;
                const u32 du = ((ha ^ hb) & 0x8000u) ? ma + mb : (ma > mb ? ma - mb : mb - ma);
                s_vck_st.units[du == 0 ? 0 : du == 1 ? 1 : du < 4 ? 2 : du < 16 ? 3 : 4]++;
                const double fa = vck_half(ha), fb = vck_half(hb), d = fabs(fa - fb);
                if (d > s_vck_st.maxh[i]) s_vck_st.maxh[i] = d;
                const double rel = d / (fabs(fb) > 1.0 ? fabs(fb) : 1.0);
                if (rel > s_vck_st.wrel) {
                    s_vck_st.wrel = rel; s_vck_st.wspu = fb; s_vck_st.wgpu = fa; s_vck_st.wid = 28 + i;
                }
            }
    }
}
static void vpass_check_read(void)
{
    const u32 par = s_d3d.vp_parity ^ 1u;    /* the batch that has just run */
    if (!s_vck[par].n) return;
    if (!s_vck[par].submitted || !s_vck_rb[par]) { vck_free(par); return; }
    u8* m = NULL;
    D3D12_RANGE rr = {0, s_vck[par].used};
    if (SUCCEEDED(s_vck_rb[par]->lpVtbl->Map(s_vck_rb[par], 0, &rr, (void**)&m)) && m) {
        for (u32 k = 0; k < s_vck[par].n; k++)
            vck_cmp(m + s_vck[par].at[k], s_vck[par].spu[k], s_vck[par].len[k], s_vck[par].used_v[k], s_vck[par].stride[k]);
        D3D12_RANGE wr = {0, 0};
        s_vck_rb[par]->lpVtbl->Unmap(s_vck_rb[par], 0, &wr);
    }
    vck_free(par);
    const double now = perf_now();
    if (!s_vck_st.t0) s_vck_st.t0 = (long long)now;
    if (now - (double)s_vck_st.t0 >= 5.0 && s_vck_st.verts) {
        const double n = (double)s_vck_st.verts * 9.0;
        fprintf(stderr, "[vtx-lit] check: %llu vertexes (%llu no index names); id 27 max |d| %.3g%s; ids 28-30 as F16:"
                        " %.2f%% the same, %.2f%% 1 unit, %.2f%% 2-3, %.3f%% 4-15, %.3f%% 16 or more; max |d| %.3g %.3g %.3g;"
                        " worst against max(|value|, 1): %.3g (id %d, SPU %.5g, GPU %.5g)%c",
                s_vck_st.verts, s_vck_st.skipped, s_vck_st.max27, s_vck_st.nan ? " (NaN seen)" : "",
                100.0 * s_vck_st.units[0] / n, 100.0 * s_vck_st.units[1] / n, 100.0 * s_vck_st.units[2] / n,
                100.0 * s_vck_st.units[3] / n, 100.0 * s_vck_st.units[4] / n,
                s_vck_st.maxh[0], s_vck_st.maxh[1], s_vck_st.maxh[2],
                s_vck_st.wrel, s_vck_st.wid, s_vck_st.wspu, s_vck_st.wgpu, 10);
        if (s_vck_st.v20) {
            const double m4 = (double)s_vck_st.v20 * 4.0;
            fprintf(stderr, "[vtx-lit] check, the second layout: %llu of those vertexes; attribute 20's U8N bytes %.3f%% the same,"
                            " %.3f%% 1 unit, %.4f%% 2-3, %.4f%% more%c", s_vck_st.v20, 100.0 * s_vck_st.u20[0] / m4,
                    100.0 * s_vck_st.u20[1] / m4, 100.0 * s_vck_st.u20[2] / m4, 100.0 * s_vck_st.u20[3] / m4, 10);
        }
        memset(&s_vck_st, 0, sizeof s_vck_st);
        s_vck_st.t0 = (long long)now;
    }
}

/* The CPU's pass over a raw copy as it is made (no lit copy to be had for it). */
static void vpass_cpu_copy(const vpr_head* h, LONG seq, u8* dst, u32 src, u32 nv)
{
    extern uint8_t* vm_base;
    if (!s_vpass.cpu || !vpr_copy(h, seq, s_vpass.rec_tmp)) return;
    s_vpass.cpu(s_vpass.rec_tmp, vm_base + src, dst, nv);
    if (s_vs_on > 0) s_vs.lit_cpu++;
}

/* one of the pass's layouts' strides */
static int vpass_stride_ok(u32 s)
{
    return s == s_vpass.stride || (s_vpass.stride2 && s == s_vpass.stride2);
}

/* A draw the vertex pass lights (a record not applied names its vertexes). */
static int vpass_wants(const rsx_state* st)
{
    if (!s_vpass.set || !st) return 0;
    const rsx_vertex_attrib* a0 = &st->vertex_attribs[0];
    if (!(a0->offset & 0x80000000u) || !vpass_stride_ok(a0->stride)) return 0;
    LONG seq = 0;
    const vpr_head* h = vpr_find(a0->offset & 0x7FFFFFFFu, &seq);
    return h && !h->applied;
}

/* upload_tris_vp_raw's new segment: a lit copy if a record names its vertexes. dst: attribute 0's
 * copy, its offset in the vp_vb half (base); len: its bytes; src: the guest's; total: the
 * segment's bytes from its header. */
static void vpass_segment(const rsx_state* st, VPRawSeg* sg, u32 dst, u8* base, u32 len, u32 src, u32 total,
                          const u8* ix, int sixteen, u32 first, u32 n)
{
    sg->lit = 0;
    sg->lhdr = 0;
    const rsx_vertex_attrib* a0 = &st->vertex_attribs[0];
    if (!(a0->offset & 0x80000000u) || !vpass_stride_ok(a0->stride)) return;
    LONG seq = 0;
    const vpr_head* h = vpr_find(a0->offset & 0x7FFFFFFFu, &seq);
    if (!h) { if (s_vs_on > 0 && s_vr_cls) s_vs.lit_miss++; return; }
    const u32 nv = sg->hi - sg->lo + 1;
    if (len != nv * a0->stride || dst - sg->hdr + len > total) return;
    const int applied = h->applied != 0;
    if (applied && !vpass_check_n() && !vpass_draw_lit()) return;
    /* the table has the count and the first vertex in one word */
    if (!vpass_build() || nv > 0xFFFFu || sg->lo > 0xFFFFu) { if (!applied) vpass_cpu_copy(h, seq, base + dst, src, nv); return; }
    const u32 par = s_d3d.vp_parity;
    u8* tab = s_vpass.tab_mapped + (size_t)par * VPT_HALF;
    u32 rec_off;
    if (s_vpass.last_ok && s_vpass.last_key == h->key && s_vpass.last_seq == seq) {
        rec_off = s_vpass.last_rec;
    } else {
        rec_off = VPT_SEGB + ((s_vpass.rec_used + 15u) & ~15u);
        if (rec_off + h->bytes > VPT_HALF || s_vpass.nseg >= VPT_SEGS) {
            if (s_vs_on > 0) s_vs.lit_full++;
            if (!applied) vpass_cpu_copy(h, seq, base + dst, src, nv);
            return;
        }
        if (!vpr_copy(h, seq, tab + rec_off)) return;
        s_vpass.rec_used = rec_off - VPT_SEGB + h->bytes;
        s_vpass.last_key = h->key; s_vpass.last_seq = seq; s_vpass.last_rec = rec_off; s_vpass.last_ok = 1;
    }
    /* the lit copy: the segment as it is laid out from its header, 64 bytes over for the last
     * pair's store and the last word */
    const u32 data = dst - sg->hdr;
    const u32 lhdr = (s_vpass.lit_used + 15u) & ~15u;
    const u32 need = total + 64u;
    if (s_vpass.nseg >= VPT_SEGS || (u64)lhdr + need > s_vpass.lit_half) {
        if (s_vs_on > 0) s_vs.lit_full++;
        if (!applied) vpass_cpu_copy(h, seq, base + dst, src, nv);
        return;
    }
    u32* e = (u32*)(tab + (size_t)s_vpass.nseg * 32u);
    e[0] = sg->hdr; e[1] = lhdr; e[2] = data; e[3] = nv | (sg->lo << 16); e[4] = rec_off; e[5] = total; e[6] = 0; e[7] = 0;
    s_vpass.nseg++;
    s_vpass.lit_used = lhdr + need;
    sg->lhdr = lhdr;
    sg->lit = !applied || vpass_draw_lit();
    if (s_vs_on > 0) { s_vs.lit_segs++; s_vs.lit_verts += nv; }
    if (applied && vpass_check_n()) vck_add(lhdr + data, len, src, ix, sixteen, first, n, sg->lo, a0->stride);
}

/* A draw that did not take the raw fetch: its record's vertexes lit in guest memory, once. */
static void vpass_guest(const rsx_state* st)
{
    extern uint8_t* vm_base;
    extern u32 cellGcmResolveLocated(int local, u32 offset);
    if (!st || !s_vpass.cpu) return;
    const rsx_vertex_attrib* a0 = &st->vertex_attribs[0];
    if (!a0->enabled || !(a0->offset & 0x80000000u) || !vpass_stride_ok(a0->stride)) return;
    LONG seq = 0;
    vpr_head* h = vpr_find(a0->offset & 0x7FFFFFFFu, &seq);
    if (!h || h->applied || h->done) return;
    const u32 key = h->key, n = h->n, len = n * a0->stride;
    if (!len) return;
    const u32 ea = cellGcmResolveLocated(0, key);
    if (cellGcmResolveLocated(0, key + len - 1) - ea != len - 1) return;
    if (!vpr_copy(h, seq, s_vpass.rec_tmp)) return;
    if (InterlockedCompareExchange(&h->done, 1, 0) != 0) return;
    s_vpass.cpu(s_vpass.rec_tmp, vm_base + ea, vm_base + ea, n);
    if (s_vs_on > 0) s_vs.lit_guest++;
}

static void vpass_barrier(ID3D12GraphicsCommandList* cl, D3D12_RESOURCE_STATES a, D3D12_RESOURCE_STATES b)
{
    D3D12_RESOURCE_BARRIER br = {0};
    br.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    br.Transition.pResource = s_vpass.lit;
    br.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    br.Transition.StateBefore = a;
    br.Transition.StateAfter = b;
    cl->lpVtbl->ResourceBarrier(cl, 1, &br);
}

/* The batch's pass, before its list is executed (render_frame; the batch's parity still set). */
static void vpass_submit(u32 fi)
{
    if (!s_vpass.nseg || s_vpass.state <= 0) return;
    const u32 par = s_d3d.vp_parity;
    ID3D12GraphicsCommandList* cl = s_vpass.cl;
    if (FAILED(s_vpass.alloc[fi]->lpVtbl->Reset(s_vpass.alloc[fi])) ||
        FAILED(cl->lpVtbl->Reset(cl, s_vpass.alloc[fi], s_vpass.pso))) {
        static int _n = 0;
        if (PS3_LOG_FIRST(_n, 4)) fprintf(stderr, "[vtx-lit] the pass's command list did not reset%c", 10);
        return;
    }
    cl->lpVtbl->SetComputeRootSignature(cl, s_vpass.rs);
    vpass_barrier(cl, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    cl->lpVtbl->SetComputeRootShaderResourceView(cl, 0,
        s_d3d.vp_vb->lpVtbl->GetGPUVirtualAddress(s_d3d.vp_vb) + (u64)par * MAX_VERTICES * 256);
    cl->lpVtbl->SetComputeRootShaderResourceView(cl, 1,
        s_vpass.tab->lpVtbl->GetGPUVirtualAddress(s_vpass.tab) + (u64)par * VPT_HALF);
    cl->lpVtbl->SetComputeRootUnorderedAccessView(cl, 2,
        s_vpass.lit->lpVtbl->GetGPUVirtualAddress(s_vpass.lit) + (u64)par * s_vpass.lit_half);
    for (u32 b0 = 0; b0 < s_vpass.nseg; b0 += 65535u) {
        cl->lpVtbl->SetComputeRoot32BitConstant(cl, 3, b0, 0);
        const u32 n = s_vpass.nseg - b0 < 65535u ? s_vpass.nseg - b0 : 65535u;
        cl->lpVtbl->Dispatch(cl, n, 1, 1);
    }
    if (s_vck[par].n) {
        if (!s_vck_rb[par]) {
            D3D12_HEAP_PROPERTIES hp = {0};
            hp.Type = D3D12_HEAP_TYPE_READBACK;
            D3D12_RESOURCE_DESC bd = {0};
            bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
            bd.Width = VCK_BYTES; bd.Height = 1; bd.DepthOrArraySize = 1; bd.MipLevels = 1;
            bd.SampleDesc.Count = 1; bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
            s_d3d.device->lpVtbl->CreateCommittedResource(s_d3d.device, &hp, D3D12_HEAP_FLAG_NONE, &bd,
                D3D12_RESOURCE_STATE_COPY_DEST, NULL, &IID_ID3D12Resource, (void**)&s_vck_rb[par]);
        }
        if (s_vck_rb[par]) {
            vpass_barrier(cl, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
            for (u32 k = 0; k < s_vck[par].n; k++)
                cl->lpVtbl->CopyBufferRegion(cl, s_vck_rb[par], s_vck[par].at[k], s_vpass.lit,
                                             (u64)par * s_vpass.lit_half + s_vck[par].off[k], s_vck[par].len[k]);
            vpass_barrier(cl, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COMMON);
            s_vck[par].submitted = 1;
        } else {
            vpass_barrier(cl, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COMMON);
        }
    } else {
        vpass_barrier(cl, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COMMON);
    }
    if (FAILED(cl->lpVtbl->Close(cl))) {
        static int _n = 0;
        if (PS3_LOG_FIRST(_n, 4)) fprintf(stderr, "[vtx-lit] the pass's command list did not close%c", 10);
        return;
    }
    ID3D12CommandList* lists[] = { (ID3D12CommandList*)cl };
    s_d3d.cmd_queue->lpVtbl->ExecuteCommandLists(s_d3d.cmd_queue, 1, lists);
}

/* With the segments: a new batch (after the parity flip), or a batch let go. */
static void vpass_reset(void)
{
    s_cb_gen++;                 /* the constant slots: none of the last batch's to share */
    s_vpass.nseg = 0;
    s_vpass.lit_used = 0;
    s_vpass.rec_used = 0;
    s_vpass.last_ok = 0;
    const u32 par = s_d3d.vp_parity;
    if (s_vck[par].n && !s_vck[par].submitted) vck_free(par);
}
static u32* s_vs_stamp;
static u32 s_vs_gen;
static int vs_on(void) { if (s_vs_on < 0) s_vs_on = getenv("VTX_STATS") ? 1 : 0; return s_vs_on; }
static long long vs_now(void) { LARGE_INTEGER t; QueryPerformanceCounter(&t); return t.QuadPart; }
static u32 read_guest_index(const rsx_state* st, u32 k);
static void vs_attrs(const rsx_state* st)
{
    int n = 0;
    for (int i = 0; i < 16; i++) {
        const rsx_vertex_attrib* a = &st->vertex_attribs[i];
        if (!a->enabled || !a->stride) continue;
        n++;
        u32 sz = a->size ? a->size : 4; if (sz > 4) sz = 4;
        s_vs.attr[a->type & 7][sz]++;
        if (a->frequency > 1) s_vs.freq++;
    }
    s_vs.nattr[n]++;
}
static void vs_report(void)
{
    LARGE_INTEGER f; QueryPerformanceFrequency(&f);
    const long long now = vs_now();
    if (!s_vs.t0) { s_vs.t0 = now; return; }
    if (now - s_vs.t0 < 5 * f.QuadPart) return;
    const double sec = (double)(now - s_vs.t0) / (double)f.QuadPart;
    fprintf(stderr, "[vtx-stats] %.1f s; indexed draws/s:", sec);
    for (int p = 0; p < 11; p++) if (s_vs.draws_idx[p]) fprintf(stderr, " prim%d=%.0f", p, s_vs.draws_idx[p] / sec);
    fprintf(stderr, "; array draws/s:");
    for (int p = 0; p < 11; p++) if (s_vs.draws_arr[p]) fprintf(stderr, " prim%d=%.0f", p, s_vs.draws_arr[p] / sec);
    fprintf(stderr, "%c", 10);
    fprintf(stderr, "[vtx-stats]   per s: index refs %.0f, distinct %.0f, index range %.0f, array verts %.0f,"
                    " conversions %.0f, vertices written %.0f; indexed upload %.1f ms/s%c",
            s_vs.refs_idx / sec, s_vs.uniq / sec, s_vs.range / sec, s_vs.refs_arr / sec,
            s_vs.conv / sec, s_vs.out / sec, 1000.0 * (double)s_vs.t_up / (double)f.QuadPart / sec, 10);
    fprintf(stderr, "[vtx-stats]   attributes (type x size, per s):");
    for (int t = 0; t < 8; t++) for (int z = 0; z < 5; z++)
        if (s_vs.attr[t][z]) fprintf(stderr, " t%dx%d=%.0f", t, z, s_vs.attr[t][z] / sec);
    fprintf(stderr, "; draws by enabled count:");
    for (int k = 0; k < 17; k++) if (s_vs.nattr[k]) fprintf(stderr, " %d:%.0f", k, s_vs.nattr[k] / sec);
    fprintf(stderr, "; freq-divided %.0f, indices >= 2^20 %.0f%c", s_vs.freq / sec, s_vs.wide / sec, 10);
    fprintf(stderr, "[vtx-stats]   raw fetch per s: segments %.0f, batches extending one %.0f, %.1f MB copied%c",
            s_vs.raw_segs / sec, s_vs.raw_ext / sec, s_vs.raw_bytes / sec / 1048576.0, 10);
    for (int k = 1; k >= 0; k--)
        fprintf(stderr, "[vtx-stats]   %s indexed draws per s: %.0f (raw %.0f, ix %.0f, CPU conversion %.0f),"
                        " index refs %.0f, raw %.1f MB, upload %.1f ms/s%c",
                k ? "vertex-ring" : "other", s_vs.c_draws[k] / sec, s_vs.c_raw[k] / sec, s_vs.c_ix[k] / sec,
                s_vs.c_cpu[k] / sec, s_vs.c_refs[k] / sec, s_vs.c_raw_bytes[k] / sec / 1048576.0,
                1000.0 * (double)s_vs.c_t[k] / (double)f.QuadPart / sec, 10);
    { static const char* const cn[8] = { "off/CW", "back/CW", "off/CCW?", "front/CW", "off/CCW", "back/CCW", "?", "front/CCW" };
      fprintf(stderr, "[vtx-stats]   vertex-ring draws per s by face culling (culled face / front face): stride 54:");
      for (int c = 0; c < 8; c++) if (s_vs.c_cull54[c]) fprintf(stderr, " %s %.0f", cn[c], s_vs.c_cull54[c] / sec);
      fprintf(stderr, "; other strides:");
      for (int c = 0; c < 8; c++) if (s_vs.c_cullo[c]) fprintf(stderr, " %s %.0f", cn[c], s_vs.c_cullo[c] / sec);
      fprintf(stderr, "%c", 10); }
    { static unsigned long long cw0, cs0, cb0, pd0, ps0;
      fprintf(stderr, "[vtx-stats]   records per s: constant slots written %.0f, shared %.0f (%.0f by generation);"
                      " prepass draws %.0f, their units shared %.0f%c", (s_cb_written - cw0) / sec, (s_cb_shared - cs0) / sec,
              (s_cb_same_bank - cb0) / sec, (s_pre_draws - pd0) / sec, (s_pre_shared - ps0) / sec, 10);
      cw0 = s_cb_written; cs0 = s_cb_shared; cb0 = s_cb_same_bank; pd0 = s_pre_draws; ps0 = s_pre_shared; }
    if (s_vpass.set)
        fprintf(stderr, "[vtx-stats]   vertex pass per s: %.0f segments (%.0f vertexes) on the GPU, %.0f"
                        " without a record (%.0f found written over, %.0f an older job's); on the CPU %.0f"
                        " copies, %.0f guest blocks; %.0f with no room%c",
                s_vs.lit_segs / sec, s_vs.lit_verts / sec, s_vs.lit_miss / sec, s_vs.lit_lost / sec,
                s_vs.lit_stale / sec, s_vs.lit_cpu / sec, s_vs.lit_guest / sec, s_vs.lit_full / sec, 10);
    memset(&s_vs, 0, sizeof s_vs);
    s_vs.t0 = now;
}
static long long vs_begin_indexed(const rsx_state* st, u32 prim, u32 first, u32 count)
{
    if (!vs_on() || !st) return 0;
    vs_report();
    if (prim < 11) s_vs.draws_idx[prim]++;
    s_vs.refs_idx += count;
    vs_attrs(st);
    if (!s_vs_stamp) s_vs_stamp = (u32*)calloc(1u << 20, sizeof(u32));
    if (!s_vs_stamp) return 0;
    if (++s_vs_gen == 0) { memset(s_vs_stamp, 0, (1u << 20) * sizeof(u32)); s_vs_gen = 1; }
    u32 lo = 0xFFFFFFFFu, hi = 0, uq = 0;
    for (u32 k = 0; k < count; k++) {
        const u32 ix = read_guest_index(st, first + k);
        if (ix < lo) lo = ix;
        if (ix > hi) hi = ix;
        if (ix < (1u << 20)) { if (s_vs_stamp[ix] != s_vs_gen) { s_vs_stamp[ix] = s_vs_gen; uq++; } }
        else s_vs.wide++;
    }
    s_vs.uniq += uq;
    if (count) s_vs.range += hi - lo + 1;
    return vs_now();
}
static void vs_end(long long t0, u32 emitted)
{
    if (!t0) return;
    s_vs.t_up += vs_now() - t0;
    s_vs.out += emitted;
}
static void vs_arrays(const rsx_state* st, u32 prim, u32 count)
{
    if (!vs_on() || !st) return;
    vs_report();
    if (prim < 11) s_vs.draws_arr[prim]++;
    s_vs.refs_arr += count;
    vs_attrs(st);
}

static void read_vp_vertex(const rsx_state* state, u32 vi, VPSlot* out16)
{
    if (s_vs_on > 0) s_vs.conv++;
    for (int i = 0; i < 16; i++) {
        rsx_fetch_attrib(state, i, vi, out16[i].v);
        if (i == 0) {
            const rsx_vertex_attrib* a = &state->vertex_attribs[0];
            u32 n = a->size ? a->size : 4; if (n > 4) n = 4;
            rsx_vtx_pos_dbg(state, out16[0].v, n);
        }
    }
}

/* Per-draw vertex cache for the paths that expand one guest vertex into
 * several output vertices: indexed lists (shared vertices), strips and fans
 * (each vertex in up to three triangles) and indexed quads. Fetching all 16
 * attributes of a vertex costs far more than copying its 256 converted bytes,
 * and God of War HD's gameplay frames expand ~370k vertices through these
 * paths; converting each one every time it was referenced was a third of the
 * frame. Direct-mapped on the low index bits (meshes reference nearby
 * indices), and dropped at every draw because attribute state and the guest
 * arrays may change between draws. VTX_NOCACHE=1 disables it. */
#define VC_BITS 13
#define VC_N (1u << VC_BITS)
static VPSlot (*s_vc)[16];
static u32 s_vc_tag[VC_N], s_vc_gen[VC_N], s_vc_cur;

static void vc_begin_draw(void)
{
    static int on = -1;
    if (on < 0) on = getenv("VTX_NOCACHE") ? 0 : 1;
    if (!on) return;
    if (!s_vc) s_vc = malloc(sizeof(*s_vc) * VC_N);
    if (++s_vc_cur == 0) { memset(s_vc_gen, 0, sizeof s_vc_gen); s_vc_cur = 1; }
}

static void read_vp_vertex_cached(const rsx_state* state, u32 vi, VPSlot* out16)
{
    if (!s_vc || !s_vc_cur) { read_vp_vertex(state, vi, out16); return; }
    u32 h = vi & (VC_N - 1);
    if (s_vc_gen[h] != s_vc_cur || s_vc_tag[h] != vi) {
        read_vp_vertex(state, vi, s_vc[h]);
        s_vc_gen[h] = s_vc_cur;
        s_vc_tag[h] = vi;
    }
    memcpy(out16, s_vc[h], sizeof(VPSlot) * 16);
    /* VTX_CACHE_CHECK=1: also refetch on every lookup and count differences
     * (validation for a new title; the refetch costs what the cache saves). */
    { static int chk = -1; if (chk < 0) chk = getenv("VTX_CACHE_CHECK") ? 1 : 0;
      if (chk) { static unsigned long long hits, bad; VPSlot fresh[16];
        read_vp_vertex(state, vi, fresh); hits++;
        if (memcmp(fresh, s_vc[h], sizeof fresh)) bad++;
        if ((hits & 0xFFFFF) == 0)
            fprintf(stderr, "[vtx-cache-check] %llu lookups, %llu mismatches\n", hits, bad); } }
}

/* VTX_POS=<N>: print the first fetched position of the first N draws. A guest
 * vertex array that resolves to the wrong memory reads as garbage/denormals, and
 * that is indistinguishable from "the shader is wrong" without seeing the input. */
void rsx_vtx_pos_dbg(const rsx_state* state, const float* v, u32 n)
{
    static int cap = -1, seen = 0;
    if (cap < 0) { const char* e = getenv("VTX_POS"); cap = e ? atoi(e) : 0; }
    if (!cap || seen >= cap) return;
    seen++;
    const rsx_vertex_attrib* a = &state->vertex_attribs[0];
    fprintf(stderr, "[VTXPOS] a0 off=0x%X stride=%u size=%u type=%u -> (%g, %g, %g, %g)\n",
            a->offset, a->stride, a->size, a->type,
            v[0], v[1], v[2], v[3]);   /* all four: masking by `size` hid whether w survived */
}

static void vp_attrs_dbg(const rsx_state* state)
{
    if (!env_on("VP_ATTRS", &s_env_vp_attrs)) return;
    /* VP_ATTRS_FP=<hex shader_program>: dump only the draws that use one
     * fragment program. Without it the first six draws are whatever the frame
     * happens to start with, which is never the mesh being investigated. */
    { static const char* e = (const char*)1; static u32 want = 0;
      if (e == (const char*)1) { e = getenv("VP_ATTRS_FP");
          want = e ? (u32)strtoul(e, NULL, 0) : 0; }
      if (want && (!state || state->shader_program != want)) return; }
    /* Dedupe by the set of enabled attributes rather than printing the first
     * few draws: one fragment program can be used by meshes with different
     * vertex layouts, and a mesh missing an attribute takes the constant
     * register instead -- which is exactly the case worth seeing. */
    { u32 mask = 0;
      for (int i = 0; i < 16; i++) if (state->vertex_attribs[i].enabled) mask |= 1u << i;
      static u32 seen[16]; static int ns = 0;
      for (int i = 0; i < ns; i++) if (seen[i] == mask) return;
      if (ns >= 16) return;
      seen[ns++] = mask; }
    fprintf(stderr, "[VPATTR] --- fp=0x%X ---%c", state->shader_program, 10);
    fprintf(stderr, "[VPATTR] divider_op=0x%08X\n", state->frequency_divider_op);
    for (int i = 0; i < 16; i++) {
        const rsx_vertex_attrib* a = &state->vertex_attribs[i];
        if (a->enabled) fprintf(stderr, "[VPATTR] a%d off=0x%X stride=%u size=%u type=%u freq=%u fmt=0x%08X\n",
                                i, a->offset, a->stride, a->size, a->type, a->frequency, a->format);
    }
    /* VP_ATTRS_DUMP=<attr>:<n>: read n entries of that attribute straight out of
     * guest memory. Answers "did the guest write these values, or did our fetch
     * mangle them" without inferring it from the rendered image. */
    { const char* e = getenv("VP_ATTRS_DUMP"); if (!e) return;
      int ai = 0, cnt = 8; sscanf(e, "%d:%d", &ai, &cnt);
      const rsx_vertex_attrib* a = &state->vertex_attribs[ai];
      if (!a->enabled || !a->stride) return;
      extern uint8_t* vm_base;
      extern u32 cellGcmResolveLocated(int, u32);
      u32 nbad = 0, nzero = 0;
      for (int v = 0; v < cnt; v++) {
          u32 aoff = (a->offset & 0x7FFFFFFFu) + (u32)v * a->stride;
          const u8* q = vm_base + ((a->offset & 0x80000000u)
                        ? cellGcmResolveLocated(0, aoff) : cellGcmResolveLocated(1, aoff));
          /* Also read the same offset through the OTHER context-DMA. An array
           * the guest put in main memory but whose offset we resolve as LOCAL
           * reads as untouched VRAM, i.e. all zeros -- indistinguishable from
           * "the producer never ran". */
          if (v == 0) {
              u32 la = cellGcmResolveLocated(1, aoff), ma = cellGcmResolveLocated(0, aoff);
              const u8* lp = vm_base + la; const u8* mp = vm_base + ma;
              const u8* rp = vm_base + (a->offset & 0x7FFFFFFFu);   /* raw, unresolved */
              fprintf(stderr, "[VPDUMP] a%d off=0x%X  LOCAL@0x%X=(%g,%g,%g)  MAIN@0x%X=(%g,%g,%g)"
                              "  RAW@0x%X=(%g,%g,%g)%c",
                      ai, aoff, la, rsx_rd_bef(lp), rsx_rd_bef(lp+4), rsx_rd_bef(lp+8),
                      ma, rsx_rd_bef(mp), rsx_rd_bef(mp+4), rsx_rd_bef(mp+8),
                      (a->offset & 0x7FFFFFFFu), rsx_rd_bef(rp), rsx_rd_bef(rp+4), rsx_rd_bef(rp+8), 10);
          }
          u32 nc = a->size ? a->size : 3; if (nc > 3) nc = 3;
          float f[3] = {0,0,0};
          for (u32 k = 0; k < nc; k++) f[k] = rsx_rd_bef(q + k * 4);
          if (v < 12)
              fprintf(stderr, "[VPDUMP] a%d[%d] = (%g, %g, %g)%c", ai, v, f[0], f[1], f[2], 10);
          int allz = 1; for (u32 k = 0; k < nc; k++) if (f[k] != 0.f) allz = 0;
          if (allz) nzero++;
          else if (f[2] < -0.9f && f[0] > -0.1f && f[0] < 0.1f) nbad++;
      }
      fprintf(stderr, "[VPDUMP] a%d over %d verts: %u zero, %u near (0,0,-1)%c",
              ai, cnt, nzero, nbad, 10);
      /* How many entries differ from the first? An all-identical position array
       * is a degenerate mesh: it rasterizes nothing, which looks the same as
       * "the draw never happened". */
      { float f0[3]; u32 o0 = (a->offset & 0x7FFFFFFFu);
        const u8* q0 = vm_base + ((a->offset & 0x80000000u)
                       ? cellGcmResolveLocated(0, o0) : cellGcmResolveLocated(1, o0));
        for (int k = 0; k < 3; k++) f0[k] = rsx_rd_bef(q0 + k * 4);
        u32 diff = 0;
        for (int v = 1; v < cnt; v++) {
            u32 ao = o0 + (u32)v * a->stride;
            const u8* q = vm_base + ((a->offset & 0x80000000u)
                          ? cellGcmResolveLocated(0, ao) : cellGcmResolveLocated(1, ao));
            for (int k = 0; k < 3; k++)
                if (rsx_rd_bef(q + k * 4) != f0[k]) { diff++; break; }
        }
        fprintf(stderr, "[VPDUMP] a%d: %u/%d entries differ from entry 0 (%g,%g,%g)%c",
                ai, diff, cnt, f0[0], f0[1], f0[2], 10);
        /* Extent: an isosurface with little fluid is a speck, and a speck that
         * rasterizes nothing is correct, not a bug. */
        { float lo[3] = {1e30f,1e30f,1e30f}, hi[3] = {-1e30f,-1e30f,-1e30f};
          for (int v = 0; v < cnt; v++) {
              u32 ao = o0 + (u32)v * a->stride;
              const u8* q = vm_base + ((a->offset & 0x80000000u)
                            ? cellGcmResolveLocated(0, ao) : cellGcmResolveLocated(1, ao));
              for (int k = 0; k < 3; k++) { float f = rsx_rd_bef(q + k * 4);
                  if (f < lo[k]) lo[k] = f; if (f > hi[k]) hi[k] = f; } }
          u32 nnan = 0, nfin = 0;
          for (int v = 0; v < cnt; v++) {
              u32 ao = o0 + (u32)v * a->stride;
              const u8* q = vm_base + ((a->offset & 0x80000000u)
                            ? cellGcmResolveLocated(0, ao) : cellGcmResolveLocated(1, ao));
              int bad = 0;
              for (int k = 0; k < 3; k++) { float f = rsx_rd_bef(q + k * 4);
                  if (!(f == f) || f > 1e30f || f < -1e30f) bad = 1; }
              if (bad) nnan++; else nfin++;
          }
          fprintf(stderr, "[VPDUMP] a%d extent x[%g..%g] y[%g..%g] z[%g..%g]"
                          "  finite=%u nan/inf=%u%c",
                  ai, lo[0], hi[0], lo[1], hi[1], lo[2], hi[2], nfin, nnan, 10); } } }
}

static u32 upload_quads_vp(const rsx_state* state, u32 first, u32 count)
{
    extern uint8_t* vm_base;
    if (!state || !vm_base || !s_d3d.vp_vb_mapped) return 0;
    if (!state->vertex_attribs[0].enabled) return 0;
    vp_attrs_dbg(state);
    u32 quads = count / 4;
    u32 maxv = (MAX_VERTICES * VP_VERT_STRIDE - s_d3d.vp_vb_offset) / VP_VERT_STRIDE;
    if (quads * 6 > maxv) quads = maxv / 6;
    VPSlot* out = (VPSlot*)((u8*)s_d3d.vp_vb_mapped
        + (u64)s_d3d.vp_parity * MAX_VERTICES * VP_VERT_STRIDE + s_d3d.vp_vb_offset);
    u32 o = 0;
    for (u32 q = 0; q < quads; q++) {
        VPSlot c[4][16];
        for (u32 k = 0; k < 4; k++)
            read_vp_vertex(state, first + q*4 + k, c[k]);
        /* quad -> two triangles (perimeter winding) */
        static const int idx[6] = {0,1,2, 0,2,3};
        for (int t = 0; t < 6; t++) { memcpy(&out[o*16], c[idx[t]], sizeof(c[0])); o++; }
        if (env_on("VTX_DUMP", &s_env_vtx_dump)) { static int _n=0; if (PS3_LOG_FIRST(_n, 4)) {
            FILE* f = fopen("vtx_dump.txt", _n==1 ? "w" : "a");
            if (f) { for (u32 k = 0; k < 4; k++)
                fprintf(f, "q%02d v%u a0=(%.3f,%.3f,%.3f,%.3f) a8=(%.3f,%.3f)\n", _n, k,
                    c[k][0].v[0],c[k][0].v[1],c[k][0].v[2],c[k][0].v[3],
                    c[k][8].v[0],c[k][8].v[1]);
              fclose(f); } } }
    }
    s_d3d.vp_vb_offset += o * VP_VERT_STRIDE;
    return o;
}

/* Straight triangle-list upload through the VP path (gcm/cube's cube draws
 * TRIANGLES, prim 5 -- no expansion needed). */
static u32 upload_tris_vp(const rsx_state* state, u32 first, u32 count)
{
    extern uint8_t* vm_base;
    s_req_verts += count; s_req_draws++;
    if (!state || !vm_base || !s_d3d.vp_vb_mapped) return 0;
    if (!state->vertex_attribs[0].enabled) return 0;
    vp_attrs_dbg(state);
    u32 maxv = (MAX_VERTICES * VP_VERT_STRIDE - s_d3d.vp_vb_offset) / VP_VERT_STRIDE;
    if (count > maxv) { s_drop_draws++;
        { static int _n = 0; if (getenv("VBFULL") && PS3_LOG_FIRST(_n, 12))
            fprintf(stderr, "[VBFULL] fp=0x%X wanted %u verts, room for %u%c",
                    state->shader_program, count, maxv, 10); }
        count = maxv - (maxv % 3); }
    VPSlot* out = (VPSlot*)((u8*)s_d3d.vp_vb_mapped
        + (u64)s_d3d.vp_parity * MAX_VERTICES * VP_VERT_STRIDE + s_d3d.vp_vb_offset);
    { double _tv = perf_on() ? perf_now() : 0.0;
      for (u32 k = 0; k < count; k++)
          read_vp_vertex(state, first + k, &out[k*16]);
      if (perf_on()) { s_perf_vtx += perf_now() - _tv; s_perf_nverts += count; } }
    /* IDXDBG=<hex shader_program>: vertex range for that program's draws. */
    { static const char* ie = (const char*)1; static u32 iw = 0;
      if (ie == (const char*)1) { ie = getenv("IDXDBG");
          iw = ie ? (u32)strtoul(ie, NULL, 16) : 0; }
      if (iw && state->shader_program == iw) { static int _n = 0; if (PS3_LOG_FIRST(_n, 12))
          fprintf(stderr, "[IDXDBG] tris first=%u count=%u a0off=0x%X stride=%u%c",
                  first, count, state->vertex_attribs[0].offset,
                  state->vertex_attribs[0].stride, 10); } }
    /* RSX_DBG_VTX=<hex tex0 offset>: dump attribute-0 positions for the draws that
     * bind that texture. The duck's texture resolves and its 9960 draws all
     * target the backbuffer, yet none of its texels reach the screen -- so the
     * question is whether its vertices are where they should be. */
    { static const char* dv = (const char*)1; static u32 want = 0; static int n = 0;
      if (dv == (const char*)1) { dv = getenv("RSX_DBG_VTX");
                                  want = dv ? (u32)strtoul(dv, NULL, 16) : 0; }
      if (want && s_d3d.cur_texs[0].raw == want && n < 6) { n++;
        fprintf(stderr, "[DUCKVTX] first=%u count=%u", first, count);
        for (u32 k = 0; k < count && k < 4; k++)
            fprintf(stderr, "  v%u=(%.2f,%.2f,%.2f)", k,
                    out[k*16].v[0], out[k*16].v[1], out[k*16].v[2]);
        { int lo = -1, hi = -1, nz = 0;
          for (int r = 0; r < RSX_MAX_VERTEX_CONSTANTS; r++) {
              const float* m = state->vertex_constants[r];
              if (m[0] || m[1] || m[2] || m[3]) { if (lo < 0) lo = r; hi = r; nz++; }
          }
          fprintf(stderr, "  | constants: %d non-zero, slots %d..%d", nz, lo, hi); }
        fprintf(stderr, "%c", 10);
      } }
    if (env_on("VTX_DUMP", &s_env_vtx_dump) && state->surface_color_offset[0] == 0xCC0000) {
        static int _n=0; if (PS3_LOG_FIRST(_n, 1)) {
        FILE* f = fopen("vtx_dump.txt", "w");
        if (f) { fprintf(f, "G-buffer draw surf0=0x%X count=%u first=%u\n",
                         state->surface_color_offset[0], count, first);
          for (u32 k = 0; k < count && k < 30; k++)
            fprintf(f, "v%02u a0=(%.3f,%.3f,%.3f) a2=(%.3f,%.3f,%.3f) a9=(%.4f,%.4f,%.4f,%.4f)\n", k,
                out[k*16].v[0],out[k*16].v[1],out[k*16].v[2],
                out[k*16+2].v[0],out[k*16+2].v[1],out[k*16+2].v[2],
                out[k*16+9].v[0],out[k*16+9].v[1],out[k*16+9].v[2],out[k*16+9].v[3]);
          fclose(f); } } }
    s_d3d.vp_vb_offset += count * VP_VERT_STRIDE;
    return count;
}

/* Non-indexed TRIANGLE_STRIP (fan=0) / TRIANGLE_FAN (fan=1) through the VP
 * path, expanded to a triangle list. LBP's Bink movie draws its YUV quad as a
 * non-indexed 4-vertex strip (prim 6); without this it fell to the fixed-
 * function fallback with is_vp=0 (no vertex-program transform -> the quad
 * collapsed to a line) and textured=0 (the Y/U/V planes never sampled). */
static u32 upload_strip_vp(const rsx_state* state, u32 first, u32 count, int fan)
{
    extern uint8_t* vm_base;
    if (!state || !vm_base || !s_d3d.vp_vb_mapped) return 0;
    if (!state->vertex_attribs[0].enabled) return 0;
    if (count < 3) return 0;
    u32 tris = count - 2;
    u32 maxv = (MAX_VERTICES * VP_VERT_STRIDE - s_d3d.vp_vb_offset) / VP_VERT_STRIDE;
    if (tris * 3 > maxv) tris = maxv / 3;
    VPSlot* out = (VPSlot*)((u8*)s_d3d.vp_vb_mapped
        + (u64)s_d3d.vp_parity * MAX_VERTICES * VP_VERT_STRIDE + s_d3d.vp_vb_offset);
    u32 o = 0;
    vc_begin_draw();
    for (u32 t = 0; t < tris; t++) {
        u32 i0 = fan ? 0 : t;
        /* strip winding alternates per triangle to keep facing consistent */
        u32 i1 = t + 1, i2 = t + 2;
        if (!fan && (t & 1)) { u32 tmp = i1; i1 = i2; i2 = tmp; }
        read_vp_vertex_cached(state, first + i0, &out[o*16]); o++;
        read_vp_vertex_cached(state, first + i1, &out[o*16]); o++;
        read_vp_vertex_cached(state, first + i2, &out[o*16]); o++;
    }
    s_d3d.vp_vb_offset += o * VP_VERT_STRIDE;
    return o;
}

/* Fetch index k from the guest index array (SET_INDEX_ARRAY_ADDRESS/_DMA:
 * dma [3:0] = location (0 local, 1 main), [7:4] = type (0 u32, 1 u16)).
 * Indices are big-endian in guest memory. */
static u32 read_guest_index(const rsx_state* st, u32 k)
{
    extern uint8_t* vm_base;
    extern u32 cellGcmResolveLocated(int local, u32 offset);
    int local = ((st->index_array_dma & 0xF) == 0);
    u32 base = cellGcmResolveLocated(local, st->index_array_offset);
    const u8* p = vm_base + base;
    if (((st->index_array_dma >> 4) & 0xF) == 1) {
        p += (u64)k * 2;
        return ((u32)p[0] << 8) | p[1];
    }
    p += (u64)k * 4;
    return ((u32)p[0] << 24) | ((u32)p[1] << 16) | ((u32)p[2] << 8) | p[3];
}

/* Indexed variants of the VP-path uploads. */
static u32 upload_quads_vp_indexed(const rsx_state* state, u32 first, u32 count)
{
    extern uint8_t* vm_base;
    if (!state || !vm_base || !s_d3d.vp_vb_mapped) return 0;
    if (!state->vertex_attribs[0].enabled) return 0;
    u32 quads = count / 4;
    u32 maxv = (MAX_VERTICES * VP_VERT_STRIDE - s_d3d.vp_vb_offset) / VP_VERT_STRIDE;
    if (quads * 6 > maxv) quads = maxv / 6;
    VPSlot* out = (VPSlot*)((u8*)s_d3d.vp_vb_mapped
        + (u64)s_d3d.vp_parity * MAX_VERTICES * VP_VERT_STRIDE + s_d3d.vp_vb_offset);
    u32 o = 0;
    vc_begin_draw();
    for (u32 q = 0; q < quads; q++) {
        VPSlot c[4][16];
        for (u32 k = 0; k < 4; k++)
            read_vp_vertex_cached(state, read_guest_index(state, first + q*4 + k), c[k]);
        static const int idx[6] = {0,1,2, 0,2,3};
        for (int t = 0; t < 6; t++) { memcpy(&out[o*16], c[idx[t]], sizeof(c[0])); o++; }
        if (env_on("VTX_DUMP", &s_env_vtx_dump)) { static int _n=0; if (PS3_LOG_FIRST(_n, 6)) {
            FILE* f = fopen("vtx_dump.txt", _n==1 ? "w" : "a");
            if (f) {
                const float* vs_ = state->viewport_scale;
                const float* vo_ = state->viewport_offset;
                fprintf(f, "iq%02d vp_scale=(%.1f,%.1f,%.3f) vp_off=(%.1f,%.1f,%.3f)\n", _n,
                        vs_[0], vs_[1], vs_[2], vo_[0], vo_[1], vo_[2]);
                for (int _c = 464; _c <= 467; _c++)
                    fprintf(f, "  c[%d]=(%.3f,%.3f,%.3f,%.3f)\n", _c,
                            state->vertex_constants[_c][0], state->vertex_constants[_c][1],
                            state->vertex_constants[_c][2], state->vertex_constants[_c][3]);
                for (u32 k = 0; k < 4; k++)
                    fprintf(f, "  v%u i%u a0=(%.3f,%.3f,%.3f,%.3f) a8=(%.3f,%.3f)\n", k,
                        read_guest_index(state, first + q*4 + k),
                        c[k][0].v[0],c[k][0].v[1],c[k][0].v[2],c[k][0].v[3],
                        c[k][8].v[0],c[k][8].v[1]);
                fclose(f);
            } } }
    }
    s_d3d.vp_vb_offset += o * VP_VERT_STRIDE;
    return o;
}

/* Per-draw fetch plan for the indexed path. read_vp_vertex calls
 * rsx_fetch_attrib for all 16 slots of every vertex, in another file: 16 calls,
 * an address resolve per enabled attribute, and the constant register copied
 * into each of the 11-12 slots a draw leaves disabled. The plan resolves each
 * enabled array's base once (local memory is base + offset), keeps the slots
 * that only carry the constant register as a template, and converts the 4-5
 * enabled attributes inline. Same conversions as rsx_vertex_fetch.c, bit for
 * bit: VTX_PLAN_CHECK=1 compares every vertex against read_vp_vertex.
 * VTX_PLAN=0 turns it off. Instanced arrays (frequency > 1) and VTX_POS keep
 * the generic fetch. */
typedef struct {
    int ok, n;
    struct { u32 slot, type, comps, stride, off; const u8* base; } a[16];
    VPSlot tmpl[16];
    u16 active;   /* bit per slot that carries an array */
} VPFetchPlan;

static inline float vf_bef(const u8* p)
{
    u32 w; memcpy(&w, p, 4);
    w = ((w >> 24) & 0xFFu) | ((w >> 8) & 0xFF00u) | ((w << 8) & 0xFF0000u) | ((w << 24) & 0xFF000000u);
    float f; memcpy(&f, &w, 4); return f;
}
static inline float vf_half(const u8* p)
{
    const u16 h = (u16)((p[0] << 8) | p[1]);
    const u32 sgn = (h >> 15) & 1u, ex = (h >> 10) & 0x1Fu, man = h & 0x3FFu;
    u32 f;
    if (ex == 0)        f = sgn << 31;
    else if (ex == 31)  f = (sgn << 31) | 0x7F800000u | (man << 13);
    else                f = (sgn << 31) | ((ex - 15u + 127u) << 23) | (man << 13);
    float o; memcpy(&o, &f, 4); return o;
}

static void vp_plan_build(const rsx_state* st, VPFetchPlan* pl)
{
    extern uint8_t* vm_base;
    extern u32 cellGcmResolveLocated(int local, u32 offset);
    static int off = -1, pos = -1;
    if (off < 0) { const char* e = getenv("VTX_PLAN"); off = (e && e[0] == '0') ? 1 : 0; }
    if (pos < 0) pos = getenv("VTX_POS") ? 1 : 0;
    pl->ok = 0; pl->n = 0; pl->active = 0;
    if (off || pos || !vm_base || g_rsx_inline) return;   /* inline: the generic fetch */
    for (int i = 0; i < 16; i++) {
        memcpy(pl->tmpl[i].v, st->vertex_data4f[i], sizeof pl->tmpl[i].v);
        const rsx_vertex_attrib* a = &st->vertex_attribs[i];
        if (!a->enabled || a->stride == 0) continue;
        if (a->frequency > 1) return;
        u32 c = a->size ? a->size : 4; if (c > 4) c = 4;
        const u32 o = a->offset & 0x7FFFFFFFu;
        pl->a[pl->n].slot = (u32)i;
        pl->a[pl->n].type = a->type;
        pl->a[pl->n].comps = c;
        pl->a[pl->n].stride = a->stride;
        pl->a[pl->n].off = o;
        /* Local memory resolves as localAddress + offset, so one base serves
         * every vertex. Main memory goes through the IO table per vertex. */
        pl->a[pl->n].base = (a->offset & 0x80000000u) ? NULL : vm_base + cellGcmResolveLocated(1, o);
        pl->active |= (u16)(1u << i);
        pl->n++;
    }
    pl->ok = 1;
}

static inline void vp_plan_attr(const VPFetchPlan* pl, int j, u32 vi, float* out)
{
    extern uint8_t* vm_base;
    extern u32 cellGcmResolveLocated(int local, u32 offset);
    const u32 stride = pl->a[j].stride, c = pl->a[j].comps;
    const u8* p = pl->a[j].base ? pl->a[j].base + (u64)vi * stride
                                : vm_base + cellGcmResolveLocated(0, pl->a[j].off + vi * stride);
    switch (pl->a[j].type) {
    case 2: for (u32 k = 0; k < c; k++) out[k] = vf_bef(p + k * 4); break;
    case 3: for (u32 k = 0; k < c; k++) out[k] = vf_half(p + k * 2); break;
    case 4: for (u32 k = 0; k < c; k++) out[k] = p[k] / 255.0f; break;
    case 1: for (u32 k = 0; k < c; k++) { s16 s = (s16)((p[k*2] << 8) | p[k*2+1]); out[k] = (float)s / 32767.0f; } break;
    case 5: for (u32 k = 0; k < c; k++) out[k] = (float)(s16)((p[k*2] << 8) | p[k*2+1]); break;
    case 7: for (u32 k = 0; k < c; k++) out[k] = (float)p[k]; break;
    case 6: {
        const u32 w = ((u32)p[0] << 24) | ((u32)p[1] << 16) | ((u32)p[2] << 8) | p[3];
        s32 x = (s32)(w & 0x7FF);          if (x & 0x400) x -= 0x800;
        s32 y = (s32)((w >> 11) & 0x7FF); if (y & 0x400) y -= 0x800;
        s32 z = (s32)((w >> 22) & 0x3FF); if (z & 0x200) z -= 0x400;
        out[0] = (float)x / 1023.0f; out[1] = (float)y / 1023.0f;
        out[2] = (float)z / 511.0f;  out[3] = 1.0f;
        break; }
    default: for (u32 k = 0; k < c; k++) out[k] = vf_bef(p + k * 4); break;
    }
}

/* One vertex, every slot written once and in order (the destination is the
 * write-combined upload heap). */
static inline void vp_plan_fetch(const VPFetchPlan* pl, u32 vi, VPSlot* out16)
{
    int j = 0;
    for (int s = 0; s < 16; s++) {
        if (pl->active & (1u << s)) {
            VPSlot t = pl->tmpl[s];
            vp_plan_attr(pl, j++, vi, t.v);
            memcpy(&out16[s], &t, sizeof t);
        } else {
            memcpy(&out16[s], &pl->tmpl[s], sizeof pl->tmpl[s]);
        }
    }
}

static void vp_fetch_vertex(const rsx_state* state, const VPFetchPlan* pl, u32 vi, VPSlot* out16)
{
    if (!pl->ok) { read_vp_vertex(state, vi, out16); return; }
    if (s_vs_on > 0) s_vs.conv++;
    vp_plan_fetch(pl, vi, out16);
    { static int chk = -1; if (chk < 0) chk = getenv("VTX_PLAN_CHECK") ? 1 : 0;
      if (chk) { static unsigned long long n, bad; VPSlot ref[16];
          read_vp_vertex(state, vi, ref); n++;
          if (memcmp(ref, out16, sizeof ref)) {
              if (bad < 8) {
                  for (int s = 0; s < 16; s++)
                      if (memcmp(&ref[s], &out16[s], sizeof ref[s]))
                          fprintf(stderr, "[vtx-plan-check] vi=%u slot %d: plan (%g %g %g %g) ref (%g %g %g %g)%c", vi, s,
                                  out16[s].v[0], out16[s].v[1], out16[s].v[2], out16[s].v[3],
                                  ref[s].v[0], ref[s].v[1], ref[s].v[2], ref[s].v[3], 10);
              }
              bad++;
          }
          if ((n & 0xFFFFF) == 0)
              fprintf(stderr, "[vtx-plan-check] %llu vertices, %llu mismatches%c", n, bad, 10); } }
}

/* Indexed TRIANGLES without the list expansion: each distinct vertex of the
 * draw is converted once, straight into the vertex buffer, and the draw keeps
 * its indices (DrawIndexedInstanced). The expansion wrote 256 bytes per index
 * reference; God of War HD gameplay makes 6.4M references a second to 2.0M
 * distinct vertices, and that copy was most of the drain. A run continues
 * across the BEGIN/END batches the list path merges (count need not be a
 * multiple of 3): the batches share the run's vertices and append indices.
 * VTX_LIST=1 goes back to the expansion. */
static u32* s_ix_stamp;   /* guest index -> generation that converted it */
static u32* s_ix_slot;    /* guest index -> vertex within the run        */
static u32  s_ix_gen, s_ix_vbase, s_ix_nverts;

static int ix_on(void)
{
    static int on = -1;
    if (on < 0) on = getenv("VTX_LIST") ? 0 : 1;
    return on;
}

/* May this batch extend the indexed run of the previous record? Mirrors the
 * list path's merge test, plus the run's own buffers lining up. */
static int ix_can_continue(u32 first)
{
    if (!s_ix_run_valid || !s_d3d.merge_prev_draw || s_d3d.draw_count == 0) return 0;
    const D3D12DrawRecord* pv = &s_d3d.draws[s_d3d.draw_count - 1];
    const rsx_state* st = s_d3d.current_rsx_state;
    return pv->indexed && s_d3d.merge_first_end == first &&
           pv->fp_addr == (st ? st->shader_program : 0) &&
           pv->begin_epoch == (st ? st->begin_epoch : 0) &&
           pv->ib_first + pv->index_count == s_d3d.vp_ib_offset &&
           pv->vb_byte_offset == s_ix_vbase * VP_VERT_STRIDE &&
           s_d3d.vp_vb_offset == (s_ix_vbase + s_ix_nverts) * VP_VERT_STRIDE;
}

/* Returns the number of indices written; 0 means "use the list path". */
/* The frame's index buffer is full: the batch goes to the CPU's conversion. Said the first times. */
static void ib_full_note(u32 count)
{
    static int n = 0;
    if (PS3_LOG_FIRST(n, 4))
        fprintf(stderr, "[D3D12] the frame's index buffer is full (%u of %u indices, %u more asked):"
                        " batches go to the CPU's conversion%c", s_d3d.vp_ib_offset, VP_MAX_INDICES, count, 10);
}

static u32 upload_tris_vp_ix(const rsx_state* state, u32 first, u32 count, int cont)
{
    extern uint8_t* vm_base;
    extern u32 cellGcmResolveLocated(int local, u32 offset);
    if (!state || !vm_base || !s_d3d.vp_vb_mapped || !s_d3d.vp_ib_mapped) return 0;
    if (!state->vertex_attribs[0].enabled || !count) return 0;
    if (s_d3d.vp_ib_offset + count > VP_MAX_INDICES) { ib_full_note(count); return 0; }
    if (!s_ix_stamp) {
        s_ix_stamp = (u32*)calloc(1u << 20, sizeof(u32));
        s_ix_slot  = (u32*)malloc((1u << 20) * sizeof(u32));
        if (!s_ix_stamp || !s_ix_slot) return 0;
    }
    if (!cont) {
        if (++s_ix_gen == 0) { memset(s_ix_stamp, 0, (1u << 20) * sizeof(u32)); s_ix_gen = 1; }
        s_ix_vbase  = s_d3d.vp_vb_offset / VP_VERT_STRIDE;
        s_ix_nverts = 0;
    }
    VPSlot* vb = (VPSlot*)((u8*)s_d3d.vp_vb_mapped + (u64)s_d3d.vp_parity * MAX_VERTICES * VP_VERT_STRIDE);
    u32* ib = (u32*)s_d3d.vp_ib_mapped + (u64)s_d3d.vp_parity * VP_MAX_INDICES + s_d3d.vp_ib_offset;
    const int local = ((state->index_array_dma & 0xF) == 0);
    const u8* ip = vm_base + cellGcmResolveLocated(local, state->index_array_offset);
    const int sixteen = (((state->index_array_dma >> 4) & 0xF) == 1);
    VPFetchPlan pl;
    vp_plan_build(state, &pl);
    u32 n = s_ix_nverts;
    for (u32 k = 0; k < count; k++) {
        const u32 kk = first + k;
        const u32 ix = sixteen ? (((u32)ip[kk * 2] << 8) | ip[kk * 2 + 1])
                               : (((u32)ip[kk * 4] << 24) | ((u32)ip[kk * 4 + 1] << 16) |
                                  ((u32)ip[kk * 4 + 2] << 8) | ip[kk * 4 + 3]);
        if (ix >= (1u << 20) || s_ix_vbase + n >= MAX_VERTICES) {
            /* Out of range for the tables, or the buffer is full: abandon the
             * run; nothing of it is published, the list path takes the draw. */
            s_ix_run_valid = 0;
            if (++s_ix_gen == 0) { memset(s_ix_stamp, 0, (1u << 20) * sizeof(u32)); s_ix_gen = 1; }
            return 0;
        }
        if (s_ix_stamp[ix] != s_ix_gen) {
            s_ix_stamp[ix] = s_ix_gen;
            s_ix_slot[ix] = n;
            vp_fetch_vertex(state, &pl, ix, &vb[(u64)(s_ix_vbase + n) * 16]);
            n++;
        }
        ib[k] = s_ix_slot[ix];
    }
    s_d3d.vp_vb_offset += (n - s_ix_nverts) * VP_VERT_STRIDE;
    s_ix_nverts = n;
    s_d3d.vp_ib_offset += count;
    s_ix_run_valid = 1;
    return count;
}

/* Raw fetch for indexed TRIANGLES. Converting each distinct vertex on the
 * drain thread still took ~7.6 ms of a God of War HD gameplay frame: 2.5M
 * vertices a second, 256 bytes each into the upload heap. Here the drain
 * copies the guest's own vertex bytes for the index range the batch uses, once
 * per stretch of memory the enabled arrays share (interleaved arrays are one
 * copy), and writes the guest indices as they are. The raw-fetch VS
 * (vp_vs_raw) reads those bytes by SV_VertexID and converts them on the GPU.
 *
 * Each batch that needs its own copy is a segment: a header of 16 attribute
 * bases in vp_vb, selected by a root constant, and its range of the index
 * buffer. A batch whose indices fall inside the previous segment's range
 * extends that segment. Batches of a run need not hold whole triangles, so a
 * new segment starts with the previous one's unfinished triangle, and its copy
 * covers those indices too.
 *
 * Instanced arrays, unknown attribute types, VTX_POS and draws without a
 * fragment program keep the CPU path, as does a first batch whose indices are
 * spread much wider than their count. VTX_RAW=0 turns it off. */
static int raw_on(void)
{
    static int on = -1;
    if (on < 0) { const char* e = getenv("VTX_RAW"); on = (e && e[0] == '0') ? 0 : 1; }
    return on;
}

static u32 vp_raw_elem_bytes(u32 type, u32 comps)
{
    switch (type) {
    case 1: case 3: case 5: return 2 * comps;   /* S1, SF, S32K */
    case 4: case 7:         return comps;       /* UB, UB256    */
    case 6:                 return 4;           /* CMP          */
    default:                return 4 * comps;   /* F            */
    }
}

typedef struct { u32 slot, off, loc, grp; u64 start, end; } VPRawArr;

/* May this batch extend the raw run of the previous record? */
static int raw_can_continue(u32 first)
{
    if (!s_raw_run_valid || !s_d3d.merge_prev_draw || s_d3d.draw_count == 0) return 0;
    const D3D12DrawRecord* pv = &s_d3d.draws[s_d3d.draw_count - 1];
    const rsx_state* st = s_d3d.current_rsx_state;
    return pv->raw && s_d3d.merge_first_end == first &&
           pv->fp_addr == (st ? st->shader_program : 0) &&
           pv->begin_epoch == (st ? st->begin_epoch : 0) &&
           pv->ib_first + pv->index_count == s_d3d.vp_ib_offset &&
           pv->seg_first + pv->seg_n == s_seg_n;
}

/* A kept block's bytes in the resident parts, the first time (rsx_kept.h). 0: no room. */
static unsigned long long s_kept_draws, s_kept_fills;
static int kept_resident(rsx_kept* k)
{
    if (s_res_retiring) return 0;   /* starting over: not even a block that is there (see render_frame) */
    if (k->res_state && k->res_gen == s_res_gen) return k->res_state == 1;
    const u32 rs = k->skin ? k->rec : k->stride;
    const u32 vb = (k->nverts * rs + 64u + 255u) & ~255u, nix = k->list_bytes / 2u;
    k->res_gen = s_res_gen;
    if (s_res_vb_used + vb > s_res_vb_size || s_res_ib_used + nix > s_res_ib_size) {
        k->res_state = 2;
        s_res_full = 1;
        return 0;
    }
    u8* d = (u8*)s_d3d.vp_vb_mapped + (u64)2 * MAX_VERTICES * VP_VERT_STRIDE + s_res_vb_used;
    const u8* v = (const u8*)k->verts;
    if (k->rec == rs) memcpy(d, v, (size_t)k->nverts * k->rec);
    else for (u32 i = 0; i < k->nverts; i++) memcpy(d + (size_t)i * rs, v + (size_t)i * k->rec, k->rec);
    u32* ib = (u32*)s_d3d.vp_ib_mapped + (u64)2 * VP_MAX_INDICES + s_res_ib_used;
    const u8* l = (const u8*)k->list;
    for (u32 i = 0; i < nix; i++) ib[i] = ((u32)l[2 * i] << 8) | l[2 * i + 1];
    if (!k->res_n) k->res_lo = 0xFFFFFFFFu;
    k->res_vb = s_res_vb_used;
    k->res_ib = s_res_ib_used;
    if ((s_res_vb_used + vb) >> 25 != s_res_vb_used >> 25)
        fprintf(stderr, "[vtx-kept] resident: %u MB of vertexes, %u MB of indexes%c",
                (s_res_vb_used + vb) >> 20, (s_res_ib_used + nix) >> 18, 10);
    s_res_vb_used += vb;
    s_res_ib_used += nix;
    k->res_state = 1;
    return 1;
}

/* A path that reads the ring: the block's bytes there, as the job would have put them, and no more.
 * The list runs on past what the job puts, and past that the ring holds the next job's block: only
 * the indexes up to the draw's last go there. */
static void kept_fill(const rsx_state* st, const rsx_kept* k, u32 io, u32 first, u32 count)
{
    extern uint8_t* vm_base;
    extern u32 cellGcmResolveLocated(int local, u32 offset);
    u8* d = vm_base + cellGcmResolveLocated(0, io);
    const u8* v = (const u8*)k->verts;
    const u32 n = k->put_verts < k->nverts ? k->put_verts : k->nverts;
    if (k->skin) {
        /* the vertexes of this frame, from the job's record. The block's first bytes change with
         * them: it stays known by those, until the next job there. */
        LONG seq = 0;
        vpr_head* h = vpr_find(io, &seq);
        vr_kept* e = kept_slot(io);
        if (h && h->skin && e && vpr_copy(h, seq, s_vpass.rec_tmp)) {
            k->fill(k, s_vpass.rec_tmp + h->skin, d, n);
            memcpy(h->tag, d, 16);
            memcpy(e->alt, d, 16);
            e->alt_on = 1;
        }
    } else if (k->rec == k->stride) {
        memcpy(d, v, (size_t)n * k->rec);
    } else {
        for (u32 i = 0; i < n; i++) memcpy(d + (size_t)i * k->stride, v + (size_t)i * k->rec, k->rec);
    }
    const int ilocal = ((st->index_array_dma & 0xF) == 0);
    const u32 lb = (first + count) * 2u;
    if (((st->index_array_dma >> 4) & 0xF) == 1 && lb <= k->list_bytes)
        memcpy(vm_base + cellGcmResolveLocated(ilocal, st->index_array_offset), k->list, lb);
    s_kept_fills++;
    if (!(s_kept_fills & (s_kept_fills - 1)))
        fprintf(stderr, "[vtx-kept] %llu fills, %llu draws from the resident part%c", s_kept_fills, s_kept_draws, 10);
}

/* A draw of a kept block, from the resident parts. What the draw has enabled outside the block (a
 * mesh's own arrays, set by the PPU's command buffer) is copied as upload_tris_vp_raw copies it. A lit
 * block's segment leaves room for the vertexes in the lit copy, and the pass reads them from the
 * resident part. 0: not this way. */
static u32 upload_tris_kept(const rsx_state* state, rsx_kept* k, u32 io, u32 first, u32 count)
{
    extern uint8_t* vm_base;
    extern u32 cellGcmResolveLocated(int local, u32 offset);
    const int sixteen = (((state->index_array_dma >> 4) & 0xF) == 1);
    if (!sixteen || (first + count) * 2u > k->list_bytes || count % 3u || first % 3u) return 0;
    if (s_seg_n >= VP_MAX_SEGS || !kept_resident(k)) return 0;
    /* the range of what has been drawn: past the job's count the list holds junk */
    if (first + count > k->res_n) {
        const u8* l = (const u8*)k->list;
        for (u32 i = k->res_n; i < first + count; i++) {
            const u32 x = ((u32)l[2 * i] << 8) | l[2 * i + 1];
            if (x < k->res_lo) k->res_lo = x;
            if (x > k->res_hi) k->res_hi = x;
        }
        k->res_n = first + count;
    }
    if (k->res_hi >= k->nverts) return 0;
    const u32 par = s_d3d.vp_parity, lo = k->res_lo, hi = k->res_hi;
    /* the resident bytes, from the half's start, as a segment's bases are */
    const u32 rv = (2u - par) * (u32)(MAX_VERTICES * VP_VERT_STRIDE) + k->res_vb;

    u32 bslot[16], bd[16];
    int nb = 0;
    VPRawArr ar[16];
    int na = 0;
    for (int i = 0; i < 16; i++) {
        const rsx_vertex_attrib* a = &state->vertex_attribs[i];
        if (!a->enabled || a->stride == 0) continue;
        if (a->frequency > 1 || a->type < 1 || a->type > 7) return 0;
        const u32 off = a->offset & 0x7FFFFFFFu;
        if ((a->offset & 0x80000000u) && off - io < k->stride) {
            bslot[nb] = (u32)i; bd[nb] = off - io; nb++;
            continue;
        }
        u32 c = a->size ? a->size : 4; if (c > 4) c = 4;
        ar[na].slot  = (u32)i;
        ar[na].off   = off;
        ar[na].loc   = (a->offset & 0x80000000u) ? 0u : 1u;
        ar[na].start = (u64)off + (u64)lo * a->stride;
        ar[na].end   = (u64)off + (u64)hi * a->stride + vp_raw_elem_bytes(a->type, c);
        if (ar[na].end > 0xFFFFFFFFull) return 0;
        na++;
    }
    for (int j = 1; j < na; j++)
        for (int m = j; m > 0 && (ar[m - 1].loc > ar[m].loc ||
             (ar[m - 1].loc == ar[m].loc && ar[m - 1].start > ar[m].start)); m--) {
            const VPRawArr t = ar[m]; ar[m] = ar[m - 1]; ar[m - 1] = t;
        }
    struct { u64 start, end, dst; u32 loc, src; } gr[16];
    int ng = 0;
    for (int j = 0; j < na; j++) {
        if (ng && gr[ng - 1].loc == ar[j].loc && ar[j].start <= gr[ng - 1].end + 64) {
            if (ar[j].end > gr[ng - 1].end) gr[ng - 1].end = ar[j].end;
        } else {
            gr[ng].start = ar[j].start; gr[ng].end = ar[j].end; gr[ng].loc = ar[j].loc; ng++;
        }
        ar[j].grp = (u32)(ng - 1);
    }
    const u64 hdr = ((u64)s_d3d.vp_vb_offset + 15) & ~15ull;
    u64 at = hdr + 64;
    for (int g = 0; g < ng; g++) {
        const u32 len = (u32)(gr[g].end - gr[g].start);
        gr[g].src = cellGcmResolveLocated((int)gr[g].loc, (u32)gr[g].start);
        if (!gr[g].loc && cellGcmResolveLocated(0, (u32)gr[g].end - 1) - gr[g].src != len - 1)
            return 0;                               /* main memory, not contiguous */
        at = (at + 15) & ~15ull;
        gr[g].dst = at;
        at += len;
    }
    at = (at + 15) & ~15ull;
    if (at + 256 > (u64)MAX_VERTICES * VP_VERT_STRIDE) return 0;
    const u32 data = (u32)(at - hdr);

    u32 lhdr = 0;
    int lit = 0;
    if (k->skin && !s_vpass.set) return 0;
    if (k->nverts > 0xFFFFu) return 0;          /* the pass's table: the count in a word's low half */
    if (s_vpass.set && (k->stride == s_vpass.stride || k->skin)) {
        LONG seq = 0;
        const vpr_head* h = vpr_find(io, &seq);
        if (!h || h->applied || !vpass_build() || (k->skin != 0) != (h->skin != 0)) return 0;
        u8* tab = s_vpass.tab_mapped + (size_t)par * VPT_HALF;
        u32 rec_off;
        if (s_vpass.last_ok && s_vpass.last_key == h->key && s_vpass.last_seq == seq) {
            rec_off = s_vpass.last_rec;
        } else {
            rec_off = VPT_SEGB + ((s_vpass.rec_used + 15u) & ~15u);
            if (rec_off + h->bytes > VPT_HALF || s_vpass.nseg >= VPT_SEGS) return 0;
            if (!vpr_copy(h, seq, tab + rec_off)) return 0;
            s_vpass.rec_used = rec_off - VPT_SEGB + h->bytes;
            s_vpass.last_key = h->key; s_vpass.last_seq = seq; s_vpass.last_rec = rec_off; s_vpass.last_ok = 1;
        }
        lhdr = (s_vpass.lit_used + 15u) & ~15u;
        const u32 need = data + k->nverts * k->stride + 64u;
        if (s_vpass.nseg >= VPT_SEGS || (u64)lhdr + need > s_vpass.lit_half) return 0;
        /* the pass copies the header and the other arrays, and lights from the resident bytes */
        u32* e = (u32*)(tab + (size_t)s_vpass.nseg * 32u);
        e[0] = (u32)hdr; e[1] = lhdr; e[2] = data; e[3] = k->nverts; e[4] = rec_off; e[5] = data; e[6] = rv;
        e[7] = k->skin ? (rec_off + h->skin) | (k->stride != s_vpass.stride ? 1u : 0u) : 0;
        s_vpass.nseg++;
        s_vpass.lit_used = lhdr + need;
        lit = 1;
        if (s_vs_on > 0) { s_vs.lit_segs++; s_vs.lit_verts += k->nverts; }
    }
    u8* base = (u8*)s_d3d.vp_vb_mapped + (u64)par * MAX_VERTICES * VP_VERT_STRIDE;
    for (int g = 0; g < ng; g++)
        memcpy(base + gr[g].dst, vm_base + gr[g].src, (size_t)(gr[g].end - gr[g].start));
    u32 hb[16] = {0};
    for (int j = 0; j < na; j++)
        hb[ar[j].slot] = (u32)gr[ar[j].grp].dst + ar[j].off - (u32)gr[ar[j].grp].start;
    for (int j = 0; j < nb; j++)
        hb[bslot[j]] = (lit ? (u32)at : rv) + bd[j];
    memcpy(base + hdr, hb, sizeof hb);
    s_d3d.vp_vb_offset = (u32)((at + 16 + VP_VERT_STRIDE - 1) / VP_VERT_STRIDE * VP_VERT_STRIDE);
    VPRawSeg* sg = &s_seg[s_seg_n++];
    sg->ib_first = (2u - par) * VP_MAX_INDICES + k->res_ib + first;
    sg->count    = count;
    sg->hdr      = (u32)hdr;
    sg->lo = lo; sg->hi = hi;
    sg->lit = lit; sg->lhdr = lhdr;
    s_raw_run_valid = 0;
    s_kept_draws++;
    return count;
}

/* Returns the number of indices taken; 0 means "use another path". */
static u32 upload_tris_vp_raw(const rsx_state* state, u32 first, u32 count, int cont)
{
    extern uint8_t* vm_base;
    extern u32 cellGcmResolveLocated(int local, u32 offset);
    if (!state || !vm_base || !s_d3d.vp_vb_mapped || !s_d3d.vp_ib_mapped || !count) return 0;
    if (!state->vertex_attribs[0].enabled) return 0;
    if (s_d3d.vp_ib_offset + count > VP_MAX_INDICES) { ib_full_note(count); return 0; }
    if (s_seg_n >= VP_MAX_SEGS) return 0;
    { static int pos = -1; if (pos < 0) pos = getenv("VTX_POS") ? 1 : 0; if (pos) return 0; }
    {   u32 kio = 0;
        rsx_kept* k = kept_find(state, &kio);
        if (k) {
            const u32 taken = upload_tris_kept(state, k, kio, first, count);
            if (taken) return taken;
            kept_fill(state, k, kio, first, count);
        }
    }

    const int ilocal = ((state->index_array_dma & 0xF) == 0);
    const u8* ip = vm_base + cellGcmResolveLocated(ilocal, state->index_array_offset);
    const int sixteen = (((state->index_array_dma >> 4) & 0xF) == 1);
#define RAW_IX(kk) (sixteen ? (((u32)ip[(kk) * 2] << 8) | ip[(kk) * 2 + 1]) \
                            : (((u32)ip[(kk) * 4] << 24) | ((u32)ip[(kk) * 4 + 1] << 16) | \
                               ((u32)ip[(kk) * 4 + 2] << 8) | ip[(kk) * 4 + 3]))
    u32* ib = (u32*)s_d3d.vp_ib_mapped + (u64)s_d3d.vp_parity * VP_MAX_INDICES + s_d3d.vp_ib_offset;
    u32 lo, hi;
    /* VTX_IX_SIMD=0: a word at a time, as before (rsx_index_copy.h). */
    { static int simd = -1;
      if (simd < 0) { const char* e = getenv("VTX_IX_SIMD"); simd = e ? atoi(e) : 1; }
      const u8* src = ip + (u64)first * (sixteen ? 2u : 4u);
      if (simd) rsx_index_copy_be(src, count, sixteen, ib, &lo, &hi);
      else      rsx_index_copy_be_scalar(src, count, sixteen, ib, &lo, &hi); }
    VPRawSeg* pv = (cont && s_seg_n) ? &s_seg[s_seg_n - 1] : NULL;
    if (pv && pv->lo <= lo && hi <= pv->hi) {      /* the previous copy covers it */
        if (s_vs_on > 0) s_vs.raw_ext++;
        pv->count += count;
        s_d3d.vp_ib_offset += count;
        return count;
    }
    /* The previous segment's unfinished triangle moves into this one. Its
     * indices are the ones just before `first`: a run's batches are
     * consecutive in the index array. */
    const u32 rem = pv ? pv->count % 3 : 0;
    for (u32 r = 1; r <= rem; r++) {
        const u32 ix = RAW_IX(first - r);
        if (ix < lo) lo = ix;
        if (ix > hi) hi = ix;
    }
#undef RAW_IX
    /* A first batch spread much wider than its count goes to the ix path, but for one the vertex
     * pass lights: there its block would be lit on the CPU. */
    if (!cont && hi - lo >= 4u * count + 256u && !vpass_wants(state)) return 0;

    /* The bytes each enabled array can be read at, [start, end). */
    VPRawArr ar[16];
    int na = 0;
    for (int i = 0; i < 16; i++) {
        const rsx_vertex_attrib* a = &state->vertex_attribs[i];
        if (!a->enabled || a->stride == 0) continue;
        if (a->frequency > 1 || a->type < 1 || a->type > 7) return 0;
        u32 c = a->size ? a->size : 4; if (c > 4) c = 4;
        ar[na].slot  = (u32)i;
        ar[na].off   = a->offset & 0x7FFFFFFFu;
        ar[na].loc   = (a->offset & 0x80000000u) ? 0u : 1u;   /* 1 = local memory */
        ar[na].start = (u64)ar[na].off + (u64)lo * a->stride;
        ar[na].end   = (u64)ar[na].off + (u64)hi * a->stride + vp_raw_elem_bytes(a->type, c);
        if (ar[na].end > 0xFFFFFFFFull) return 0;
        na++;
    }
    if (!na) return 0;
    for (int j = 1; j < na; j++)                   /* by (location, start) */
        for (int m = j; m > 0 && (ar[m - 1].loc > ar[m].loc ||
             (ar[m - 1].loc == ar[m].loc && ar[m - 1].start > ar[m].start)); m--) {
            const VPRawArr t = ar[m]; ar[m] = ar[m - 1]; ar[m - 1] = t;
        }
    /* Arrays whose bytes overlap or nearly touch share one copy. */
    struct { u64 start, end, dst; u32 loc, src; } gr[16];
    int ng = 0;
    for (int j = 0; j < na; j++) {
        if (ng && gr[ng - 1].loc == ar[j].loc && ar[j].start <= gr[ng - 1].end + 64) {
            if (ar[j].end > gr[ng - 1].end) gr[ng - 1].end = ar[j].end;
        } else {
            gr[ng].start = ar[j].start; gr[ng].end = ar[j].end; gr[ng].loc = ar[j].loc; ng++;
        }
        ar[j].grp = (u32)(ng - 1);
    }
    u64 need = 64 + 256;
    for (int g = 0; g < ng; g++) {
        const u32 len = (u32)(gr[g].end - gr[g].start);
        gr[g].src = cellGcmResolveLocated((int)gr[g].loc, (u32)gr[g].start);
        if (!gr[g].loc && cellGcmResolveLocated(0, (u32)gr[g].end - 1) - gr[g].src != len - 1)
            return 0;                               /* main memory, not contiguous */
        need += len + 16;
    }
    const u64 hdr = ((u64)s_d3d.vp_vb_offset + 15) & ~15ull;
    if (hdr + need > (u64)MAX_VERTICES * VP_VERT_STRIDE) return 0;

    u8* base = (u8*)s_d3d.vp_vb_mapped + (u64)s_d3d.vp_parity * MAX_VERTICES * VP_VERT_STRIDE;
    u64 at = hdr + 64;
    for (int g = 0; g < ng; g++) {
        at = (at + 15) & ~15ull;
        gr[g].dst = at;
        memcpy(base + at, vm_base + gr[g].src, (size_t)(gr[g].end - gr[g].start));
        at += gr[g].end - gr[g].start;
    }
    /* base + index * stride lands on the copy of that vertex; the subtraction
     * may wrap, and the shader's u32 arithmetic wraps back. */
    u32 h[16] = {0};
    for (int j = 0; j < na; j++)
        h[ar[j].slot] = (u32)gr[ar[j].grp].dst + ar[j].off - (u32)gr[ar[j].grp].start;
    memcpy(base + hdr, h, sizeof h);
    /* The other paths address vp_vb in whole 256-byte vertices. */
    s_d3d.vp_vb_offset = (u32)((at + 16 + VP_VERT_STRIDE - 1) / VP_VERT_STRIDE * VP_VERT_STRIDE);

    if (pv) pv->count -= rem;
    VPRawSeg* sg = &s_seg[s_seg_n++];
    sg->ib_first = s_d3d.vp_ib_offset - rem;
    sg->count    = count + rem;
    sg->hdr      = (u32)hdr;
    sg->lo = lo; sg->hi = hi;
    sg->lit = 0; sg->lhdr = 0;
    if (s_vpass.set) {          /* the vertex pass: a lit copy, if a record names these vertexes */
        int g0 = -1;
        for (int j = 0; j < na; j++) if (ar[j].slot == 0) g0 = (int)ar[j].grp;
        if (g0 >= 0)
            vpass_segment(state, sg, (u32)gr[g0].dst, base, (u32)(gr[g0].end - gr[g0].start), gr[g0].src,
                          (u32)(at - hdr), ip, sixteen, first - rem, count + rem);
    }
    s_d3d.vp_ib_offset += count;
    s_raw_run_valid = 1;
    if (s_vs_on > 0) { s_vs.raw_segs++; s_vs.raw_bytes += at - hdr; s_vs.c_raw_bytes[s_vr_cls] += at - hdr; }
    return count;
}

static u32 upload_tris_vp_indexed(const rsx_state* state, u32 first, u32 count)
{
    extern uint8_t* vm_base;
    s_req_verts += count; s_req_draws++;
    if (!state || !vm_base || !s_d3d.vp_vb_mapped) return 0;
    if (!state->vertex_attribs[0].enabled) return 0;
    vp_attrs_dbg(state);
    u32 maxv = (MAX_VERTICES * VP_VERT_STRIDE - s_d3d.vp_vb_offset) / VP_VERT_STRIDE;
    if (count > maxv) { s_drop_draws++;
        { static int _n = 0; if (getenv("VBFULL") && PS3_LOG_FIRST(_n, 12))
            fprintf(stderr, "[VBFULL] fp=0x%X wanted %u verts, room for %u%c",
                    state->shader_program, count, maxv, 10); }
        count = maxv - (maxv % 3); }
    VPSlot* out = (VPSlot*)((u8*)s_d3d.vp_vb_mapped
        + (u64)s_d3d.vp_parity * MAX_VERTICES * VP_VERT_STRIDE + s_d3d.vp_vb_offset);
    { double _tv = perf_on() ? perf_now() : 0.0;
      vc_begin_draw();
      for (u32 k = 0; k < count; k++)
          read_vp_vertex_cached(state, read_guest_index(state, first + k), &out[k*16]);
      if (perf_on()) { s_perf_vtx += perf_now() - _tv; s_perf_nverts += count; } }
    /* IDXDBG=<hex shader_program>: index range for that program's draws. An
     * index past the vertex array reads unmapped guest memory as zero, which
     * shows up as a spike triangle with a zero texcoord, not as a missing draw. */
    { static const char* ie = (const char*)1; static u32 iw = 0;
      if (ie == (const char*)1) { ie = getenv("IDXDBG");
          iw = ie ? (u32)strtoul(ie, NULL, 16) : 0; }
      if (iw && state->shader_program == iw) { static int _n = 0; if (PS3_LOG_FIRST(_n, 12)) {
          u32 lo = 0xFFFFFFFFu, hi = 0;
          for (u32 k = 0; k < count; k++) { u32 ix = read_guest_index(state, first + k);
              if (ix < lo) lo = ix; if (ix > hi) hi = ix; }
          fprintf(stderr, "[IDXDBG] first=%u count=%u idx=[%u..%u] dma=0x%X a0off=0x%X stride=%u%c",
                  first, count, lo, hi, state->index_array_dma,
                  state->vertex_attribs[0].offset, state->vertex_attribs[0].stride, 10); } } }
    /* DBG_LOCK tracking: projected centroid of this mesh, updated every draw. */
    { static const char* dl = (const char*)1; static u32 wantl = 0; static int mvpb = 256;
      if (dl == (const char*)1) { dl = getenv("RSX_DBG_VTX");
                                  wantl = dl ? (u32)strtoul(dl, NULL, 16) : 0;
                                  const char* mb = getenv("MVP_BASE");
                                  if (mb) mvpb = atoi(mb); }
      /* RSX_DBG_TRACK=1 tracks the content-identified duck without needing its
       * offset passed in (VRAM offsets move between runs). Re-read every call:
       * s_duck_raw is filled by the texture upload, which happens AFTER the
       * first draws, so caching it once left the tracker watching offset 0. */
      { static int dtrk = -1;
        if (dtrk < 0) { const char* e = getenv("RSX_DBG_TRACK"); dtrk = e ? atoi(e) : 0; }
        if (dtrk && s_duck_raw) wantl = s_duck_raw; }
      /* RSX_DBG_PICK=<first>: track ONLY the draw with that starting index. The
       * mesh is one big vertex buffer sliced into 256-index chunks, so averaging
       * across all of them converges on the centre of the whole field -- which is
       * where there is nothing in particular. A single slice is a stable target
       * and can be magnified without drifting off it. */
      static int pick = -1, pick_read = 0;
      if (!pick_read) { const char* e = getenv("RSX_DBG_PICK"); pick = e ? atoi(e) : -2; pick_read = 1; }
      if (wantl && s_d3d.cur_texs[0].raw == wantl && count
          && (pick == -2 || (int)first == pick)) {
          double sx = 0, sy = 0, sz = 0;
          for (u32 k = 0; k < count; k++) {
              sx += out[k*16].v[0]; sy += out[k*16].v[1]; sz += out[k*16].v[2];
          }
          float v[4] = { (float)(sx/count), (float)(sy/count), (float)(sz/count), 1.0f };
          float clip[4];
          for (int r = 0; r < 4; r++) {
              const float* m = state->vertex_constants[mvpb + r];
              clip[r] = m[0]*v[0] + m[1]*v[1] + m[2]*v[2] + m[3]*v[3];
          }
          if (clip[3] > 1e-6f) {
              float nx = clip[0]/clip[3], ny = clip[1]/clip[3];
              if (nx > -2.0f && nx < 2.0f && ny > -2.0f && ny < 2.0f) {
                  s_lock_sx += nx; s_lock_sy += ny; s_lock_n++;
                  if (!s_lock_valid) {        /* seed so frame 1 is usable */
                      s_lock_x = nx; s_lock_y = ny; s_lock_valid = 1;
                  }
              }
          }
      } }
    /* RSX_DBG_SCALE=<f>: multiply attribute-0 positions for the draws binding
     * RSX_DBG_VTX's texture. The duck's object-space bbox is ~0.2 units and its
     * draws rasterize only a few dozen pixels per frame -- sub-pixel. Growing it
     * about its own origin tests the rest of the chain (index buffer, texture,
     * per-draw transform): if a duck-shaped, duck-coloured object appears, only
     * the scale is wrong. Diagnostic. */
    { static const char* ds = (const char*)1; static float sc = 0.0f;
      if (ds == (const char*)1) { ds = getenv("RSX_DBG_SCALE");
                                  sc = ds ? (float)atof(ds) : 0.0f; }
      static const char* dv2 = (const char*)1; static u32 want2 = 0;
      if (dv2 == (const char*)1) { dv2 = getenv("RSX_DBG_VTX");
                                   want2 = dv2 ? (u32)strtoul(dv2, NULL, 16) : 0; }
      if (sc > 0.0f && want2 && s_d3d.cur_texs[0].raw == want2 && count) {
        /* Magnify about a FIXED point captured from the first tracked draw, not
         * about each chunk's own centre -- recentring per chunk would stack every
         * chunk of the mesh on top of the others. Combined with VP_BYPASS (which
         * maps attribute 0 straight to clip space) this renders the mesh's real
         * geometry and texture at a readable size. */
        /* RSX_DBG_RECENTER=1: recentre EVERY chunk on its own centroid, overlaying
         * them. If each chunk is one instance of the same mesh they align into a
         * single silhouette; if they are arbitrary slices of one big mesh they
         * smear. Either way the answer is visible at a glance. */
        static int recen = -1;
        if (recen < 0) { const char* e = getenv("RSX_DBG_RECENTER"); recen = e ? atoi(e) : 0; }
        static int have_c = 0; static float ox = 0, oy = 0, oz = 0;
        if (!have_c || recen) {
            double sx = 0, sy = 0, sz = 0;
            for (u32 k = 0; k < count; k++) {
                sx += out[k*16].v[0]; sy += out[k*16].v[1]; sz += out[k*16].v[2];
            }
            ox = (float)(sx/count); oy = (float)(sy/count); oz = (float)(sz/count);
            have_c = 1;
        }
        for (u32 k = 0; k < count; k++) {
            /* RSX_DBG_SHIFT="dx,dy": nudge after scaling. VP_BYPASS applies the
             * guest's posoffset, so a recentred mesh does not land at screen
             * centre -- without this the magnified geometry sits clipped against
             * the top edge. */
            static int shf = -1; static float dx = 0.0f, dy = 0.0f;
            if (shf < 0) { const char* e = getenv("RSX_DBG_SHIFT");
                           if (e) { double a=0,b=0; sscanf(e, "%lf,%lf", &a, &b);
                                    dx = (float)a; dy = (float)b; }
                           shf = 1; }
            out[k*16].v[0] = (out[k*16].v[0] - ox) * sc + dx;
            out[k*16].v[1] = (out[k*16].v[1] - oy) * sc + dy;
            /* Leave z alone: under VP_BYPASS the object z maps straight to clip
             * z, so scaling it walks the geometry out of the depth range and
             * everything clips away. */
            (void)oz;
        }
      } }
    /* RSX_DBG_VTX=<hex tex0 offset>: attribute-0 positions for the draws binding
     * that texture (see upload_tris_vp). The duck's meshes are indexed, so this
     * is the copy that actually fires for it. */
    { static const char* dv = (const char*)1; static u32 want = 0; static int n = 0;
      if (dv == (const char*)1) { dv = getenv("RSX_DBG_VTX");
                                  want = dv ? (u32)strtoul(dv, NULL, 16) : 0; }
      if (want && s_d3d.cur_texs[0].raw == want && n < 8) { n++;
        float mnx=1e30f,mny=1e30f,mnz=1e30f,mxx=-1e30f,mxy=-1e30f,mxz=-1e30f;
        for (u32 k = 0; k < count; k++) {
            float x=out[k*16].v[0], y=out[k*16].v[1], z=out[k*16].v[2];
            if (x<mnx)mnx=x; if (y<mny)mny=y; if (z<mnz)mnz=z;
            if (x>mxx)mxx=x; if (y>mxy)mxy=y; if (z>mxz)mxz=z;
        }
        const rsx_vertex_attrib* _a3 = &state->vertex_attribs[3];
        fprintf(stderr, "[DUCKVTX] a3(en=%d type=%u size=%u stride=%u off=0x%08X freq=%u)"
                        " -> col0 v0=(%.3f %.3f %.3f %.3f)%c",
                _a3->enabled, _a3->type, _a3->size, _a3->stride, _a3->offset,
                _a3->frequency,
                out[3].v[0], out[3].v[1], out[3].v[2], out[3].v[3], 10);
        const rsx_vertex_attrib* _a0 = &state->vertex_attribs[0];
        fprintf(stderr, "[DUCKVTX] first=%u count=%u a0(type=%u size=%u stride=%u off=0x%08X)"
                        " obj-bbox x[%.3f,%.3f] y[%.3f,%.3f] z[%.3f,%.3f]%c",
                first, count, _a0->type, _a0->size, _a0->stride, _a0->offset,
                mnx,mxx, mny,mxy, mnz,mxz, 10);
        /* Project the object-space bbox corners through the first four transform
         * constants (the conventional MVP rows) and report NDC. Sub-pixel output
         * with correct vertices means the placement transform is the problem, and
         * the NDC extent says by how much. */
        { float cx=(mnx+mxx)*0.5f, cy=(mny+mxy)*0.5f, cz=(mnz+mxz)*0.5f;
          /* This title's Cg programs keep their MVP at c[256..259], not c[0..3]
           * (see the vp_uses_c03 note); projecting through c0 reported zeros. */
          static int MVP = -1;
          if (MVP < 0) { const char* e = getenv("MVP_BASE"); MVP = e ? atoi(e) : 256; }
          float v[4] = { cx, cy, cz, 1.0f };
          float clip[4];
          for (int r = 0; r < 4; r++) {
              const float* m = state->vertex_constants[MVP + r];
              clip[r] = m[0]*v[0] + m[1]*v[1] + m[2]*v[2] + m[3]*v[3];
          }
          float ex[4];
          { float e[4] = { mxx-cx, mxy-cy, mxz-cz, 0.0f };
            for (int r = 0; r < 4; r++) {
                const float* m = state->vertex_constants[MVP + r];
                ex[r] = m[0]*e[0] + m[1]*e[1] + m[2]*e[2];
            } }
          { int lo = -1, hi = -1, nz = 0;
            for (int r = 0; r < RSX_MAX_VERTEX_CONSTANTS; r++) {
                const float* m = state->vertex_constants[r];
                if (m[0] || m[1] || m[2] || m[3]) { if (lo < 0) lo = r; hi = r; nz++; }
            }
            fprintf(stderr, "[DUCKVTX]   constants: %d non-zero, slots %d..%d;", nz, lo, hi);
            for (int r = (lo < 0 ? 0 : lo); r < (lo < 0 ? 0 : lo) + 4 && r < RSX_MAX_VERTEX_CONSTANTS; r++)
                fprintf(stderr, " c%d=(%.3f %.3f %.3f %.3f)", r,
                        state->vertex_constants[r][0], state->vertex_constants[r][1],
                        state->vertex_constants[r][2], state->vertex_constants[r][3]);
            fprintf(stderr, "%c", 10); }
          fprintf(stderr, "[DUCKVTX]   mvp0=(%.3f %.3f %.3f %.3f) clip=(%.3f %.3f %.3f w=%.3f)",
                  state->vertex_constants[MVP][0], state->vertex_constants[MVP][1],
                  state->vertex_constants[MVP][2], state->vertex_constants[MVP][3],
                  clip[0], clip[1], clip[2], clip[3]);
          if (clip[3] > 1e-6f)
              fprintf(stderr, " ndc=(%.4f %.4f) half-extent-px=(%.2f %.2f)",
                      clip[0]/clip[3], clip[1]/clip[3],
                      ex[0]/clip[3]*640.0f, ex[1]/clip[3]*360.0f);
          fprintf(stderr, "%c", 10); }
      } }
    /* IDX_DBG=<N>: the first indices and the position they resolve to. An index
     * buffer read with the wrong element size or base yields huge indices and
     * degenerate triangles -- an INDEXED mesh vanishes while non-indexed
     * geometry in the same scene renders fine. */
    { static int cap = -1, n = 0;
      if (cap < 0) { const char* e = getenv("IDX_DBG"); cap = e ? atoi(e) : 0; }
      if (cap && n < cap && count >= 3) { n++;
        extern u32 cellGcmResolveOffset(u32);
        const rsx_vertex_attrib* a0 = &state->vertex_attribs[0];
        u32 vbase = cellGcmResolveOffset(a0->offset & 0x7FFFFFFFu);
        u32 nz = 0; for (u32 q = 0; q < 0x1000u; q += 29) if (vm_base[vbase + q]) nz++;
        fprintf(stderr, "[IDX] idxoff=0x%X idx: %u %u %u | a0 off=0x%08X(->0x%08X nz=%u/142)"
                        " stride=%u size=%u type=%u  pos0=(%g %g %g)%c",
                state->index_array_offset,
                read_guest_index(state, first), read_guest_index(state, first+1),
                read_guest_index(state, first+2),
                a0->offset, vbase, nz, a0->stride, a0->size, a0->type,
                out[0].v[0], out[0].v[1], out[0].v[2], 10); } }
    s_d3d.vp_vb_offset += count * VP_VERT_STRIDE;
    return count;
}

/* Indexed TRIANGLE_STRIP / TRIANGLE_FAN, expanded CPU-side into a triangle
 * list (the whole VP path draws lists). DeferredShading's spline tube meshes
 * -- the octopus tentacles -- are indexed strips. Odd strip triangles swap
 * two vertices, as in upload_strip_vp: guest culling is honoured now, and
 * without the swap every second triangle comes out back-facing (God of War
 * HD's fullscreen 4-index quads lost their lower-right half). */
static u32 upload_strip_vp_indexed(const rsx_state* state, u32 first, u32 count, int fan)
{
    extern uint8_t* vm_base;
    if (!state || !vm_base || !s_d3d.vp_vb_mapped) return 0;
    if (!state->vertex_attribs[0].enabled) return 0;
    if (count < 3) return 0;
    u32 tris = count - 2;
    u32 maxv = (MAX_VERTICES * VP_VERT_STRIDE - s_d3d.vp_vb_offset) / VP_VERT_STRIDE;
    if (tris * 3 > maxv) tris = maxv / 3;
    VPSlot* out = (VPSlot*)((u8*)s_d3d.vp_vb_mapped
        + (u64)s_d3d.vp_parity * MAX_VERTICES * VP_VERT_STRIDE + s_d3d.vp_vb_offset);
    u32 o = 0;
    vc_begin_draw();
    for (u32 t = 0; t < tris; t++) {
        u32 i0 = fan ? 0 : t;
        u32 i1 = t + 1, i2 = t + 2;
        if (!fan && (t & 1)) { u32 tmp = i1; i1 = i2; i2 = tmp; }
        read_vp_vertex_cached(state, read_guest_index(state, first + i0), &out[o*16]); o++;
        read_vp_vertex_cached(state, read_guest_index(state, first + i1), &out[o*16]); o++;
        read_vp_vertex_cached(state, read_guest_index(state, first + i2), &out[o*16]); o++;
    }
    s_d3d.vp_vb_offset += o * VP_VERT_STRIDE;
    return o;
}

/* DBT_VTX=<n>: the first n draws with the depth bounds test on (God of War:
 * Ascension's screen mask passes): primitive, vertex range, viewport, scissor and
 * the first four values of attribute 0 as the guest wrote them. A diagnostic. */
static void dbt_vtx_dbg(const rsx_state* st, const char* kind, u32 primitive, u32 first, u32 count)
{
    static int cap = -1, n = 0;
    if (cap < 0) { const char* e = getenv("DBT_VTX"); cap = e ? atoi(e) : 0; }
    if (!cap || n >= cap || !st || !st->depth_bounds_enable) return;
    n++;
    float lo, hi;
    memcpy(&lo, &st->depth_bounds_min, 4); memcpy(&hi, &st->depth_bounds_max, 4);
    fprintf(stderr, "[DBTVTX] %s%s prim=%u first=%u count=%u dbt=%.6f..%.6f vp=%u,%u,%ux%u sc=%u,%u,%ux%u"
                    " vpscale=%g,%g off=%g,%g sref=0x%X",
            kind, g_rsx_inline ? " inline" : "", primitive, first, count, lo, hi, st->viewport_x, st->viewport_y, st->viewport_w,
            st->viewport_h, st->scissor_x, st->scissor_y, st->scissor_w, st->scissor_h,
            st->viewport_scale[0], st->viewport_scale[1], st->viewport_offset[0], st->viewport_offset[1],
            st->stencil_ref);
    for (int u = 0; u < 16; u++) {
        const rsx_texture_state* t = &st->textures[u];
        if (!(t->control0 & 0x80000000u)) continue;   /* enabled units */
        fprintf(stderr, " | t%d off=0x%X fmt=0x%X addr=0x%X ctl0=0x%X filt=0x%X rect=%ux%u", u, t->offset,
                t->format, t->address, t->control0, t->filter, t->image_rect >> 16, t->image_rect & 0xFFFF);
    }
    for (int ai = 0; ai < 16; ai++) {
        const rsx_vertex_attrib* a = &st->vertex_attribs[ai];
        if (!a->enabled) continue;
        fprintf(stderr, " | a%d off=0x%X str=%u sz=%u ty=%u:", ai, a->offset, a->stride, a->size, a->type);
        if (ai > 2) continue;
        /* As the backend fetches them (inline arrays included). */
        for (u32 v = 0; v < 6 && v < count; v++) {
            float f[4];
            rsx_fetch_attrib(st, ai, first + v, f);
            fprintf(stderr, " (%g,%g,%g,%g)", f[0], f[1], f[2], f[3]);
        }
    }
    fprintf(stderr, "%c", 10);
}

/* SURF_DBG=1: each distinct surface set-up a draw renders with (targets,
 * offsets, pitches, clip, viewport, zeta). A diagnostic. */
static void surf_dbg(const rsx_state* st)
{
    static int on = -1;
    if (on < 0) { const char* e = getenv("SURF_DBG"); on = e ? atoi(e) : 0; }
    if (!on || !st) return;
    static u32 seen[256]; static int ns = 0;
    u32 k = 2166136261u;
    const u32 v[] = { st->color_target, st->surface_color_offset[0], st->surface_color_offset[1],
                      st->surface_color_pitch[0], st->surface_color_pitch[1], st->surface_clip_w,
                      st->surface_clip_h, st->viewport_w, st->viewport_h, st->surface_zeta_offset,
                      st->surface_zeta_pitch, st->surface_format };
    for (unsigned i = 0; i < sizeof v / sizeof v[0]; i++) k = (k ^ v[i]) * 16777619u;
    for (int i = 0; i < ns; i++) if (seen[i] == k) return;
    if (ns >= 256) return;
    seen[ns++] = k;
    extern unsigned cellGcm_flip_request_count(void);
    fprintf(stderr, "[SURF] flip=%u tgt=0x%X a=0x%X/%u b=0x%X/%u clip=%ux%u vp=%u,%u,%ux%u zeta=0x%X/%u fmt=0x%X fp=0x%X%c",
            cellGcm_flip_request_count(), st->color_target, st->surface_color_offset[0], st->surface_color_pitch[0],
            st->surface_color_offset[1], st->surface_color_pitch[1], st->surface_clip_w, st->surface_clip_h,
            st->viewport_x, st->viewport_y, st->viewport_w, st->viewport_h, st->surface_zeta_offset,
            st->surface_zeta_pitch, st->surface_format, st->shader_program, 10);
}

static void d3d12_draw_arrays(void* ud, u32 primitive, u32 first, u32 count)
{
    if (s_rt_mode > 0) rt_publish();   /* RSX_RENDER_THREAD: the records before the last are done */
    /* RSX_LIVE_FEED_DBG=1: pair with the [feed] method counts so "the FIFO
     * carries no draw batches" can be checked against "the backend is told
     * to draw" in the same run. */
    { static int dbg = -1; if (dbg < 0) dbg = getenv("RSX_LIVE_FEED_DBG") ? 1 : 0;
      if (dbg) { static long long n = 0;
          if (((++n) % 20000) == 0)
              printf("[backend] arrays callbacks=%lld (prim=%u count=%u)\n", n, primitive, count), fflush(stdout); } }
    (void)ud;
    /* Log the first 20 calls in detail, then every 1000th to show liveness
     * without flooding. */
    static u64 s_total = 0;
    if (s_total < 20 || (s_total % 1000) == 0) {
        printf("[D3D12] draw_arrays #%llu prim=%u first=%u count=%u\n",
               (unsigned long long)s_total, primitive, first, count);
    }
    s_total++;

    dbt_vtx_dbg(s_d3d.current_rsx_state, "arrays", primitive, first, count);
    surf_dbg(s_d3d.current_rsx_state);
    if (!s_d3d.pipeline_ready || !s_d3d.vb_mapped) return;
    if (count == 0 || count > MAX_VERTICES) return;
    vs_arrays(s_d3d.current_rsx_state, primitive, count);

    /* QUADS (prim 8) have no D3D12 topology; expand to a triangle list.
     * dbgfont draws all text as quads. Prefer the real vertex-program path
     * (exact position + texcoord): upload raw float4 attrib0 into vp_vb and
     * mark the draw is_vp; render_frame compiles the VP and draws it. Fall
     * back to the frac-approximation textured path if VP resources are absent. */
    if (primitive == RSX_PRIMITIVE_QUADS) {
        /* Prefer the real vertex-program path whenever VP resources exist --
         * NOT only when a texture is bound. Requiring tex_bound routed vkcube's
         * UNtextured cube to the fixed-function fallback, which never applies the
         * VP's MVP transform, so the cube was drawn in object space and clipped
         * off-screen. Untextured VP draws now render via the colour PSO. */
        if (s_d3d.vp_vb_mapped && s_d3d.vp_root_sig) {
            u32 rec = s_d3d.vp_vb_offset;
            u32 emitted = upload_quads_vp(s_d3d.current_rsx_state, first, count);
            if (emitted && rsx_draw_room()) {
                D3D12DrawRecord* dr = &s_d3d.draws[s_d3d.draw_count];
                dr->vb_byte_offset = rec;
                dr->vertex_count   = emitted;
                dr->topology       = RSX_TOPOLOGY_TRIANGLES;
                dr->textured       = s_d3d.tex_bound;
                dr->is_vp          = 1;
                dr->fp_addr = s_d3d.current_rsx_state ? s_d3d.current_rsx_state->shader_program : 0;
                dr->fp_ctrl = rec_fp_ctrl(); dr->wpos_win = rec_wpos_win(); dr->l2_inval = rec_l2_inval();
                dr->cull = rsx_cull_key(s_d3d.current_rsx_state); dr->zs_key = rsx_zs_key(s_d3d.current_rsx_state); dr->poly = rsx_poly_key(s_d3d.current_rsx_state); rec_step2_state(dr, s_d3d.current_rsx_state); dr->stencil_ref = s_d3d.current_rsx_state ? s_d3d.current_rsx_state->stencil_ref : 0;
        dr->begin_epoch = s_d3d.current_rsx_state ? s_d3d.current_rsx_state->begin_epoch : 0;
            dr->begin_epoch = s_d3d.current_rsx_state ? s_d3d.current_rsx_state->begin_epoch : 0;
                dr->cmask = 0xF;
                if (s_d3d.current_rsx_state) {
                    u32 _cm = s_d3d.current_rsx_state->color_mask;
                    dr->cmask = ((_cm & 0x00010000) ? 1u : 0u)   /* R */
                              | ((_cm & 0x00000100) ? 2u : 0u)   /* G */
                              | ((_cm & 0x00000001) ? 4u : 0u)   /* B */
                              | ((_cm & 0x01000000) ? 8u : 0u);  /* A */
                }
                dr->alpha_ctl = 0;
                if (s_d3d.current_rsx_state)
                    dr->alpha_ctl = ((s_d3d.current_rsx_state->alpha_test_enable ? 1u : 0u) << 16)
                                  | ((s_d3d.current_rsx_state->alpha_func & 0xFFu) << 8)
                                  | (s_d3d.current_rsx_state->alpha_ref & 0xFFu);
                for (int _u = 0; _u < VP_TEX_UNITS; _u++) {
                    dr->tex[_u].off = s_d3d.cur_texs[_u].off;
                    dr->tex[_u].raw = s_d3d.cur_texs[_u].raw;
                    dr->tex[_u].w   = s_d3d.cur_texs[_u].w;
                    dr->tex[_u].h   = s_d3d.cur_texs[_u].h;
                    dr->tex[_u].fmt = s_d3d.cur_texs[_u].fmt;
                    dr->tex[_u].ctrl1 = s_d3d.cur_texs[_u].ctrl1;
                    dr->tex[_u].cube  = s_d3d.cur_texs[_u].cube;
                    dr->tex[_u].depth = s_d3d.cur_texs[_u].depth;
                    dr->tex[_u].mips  = s_d3d.cur_texs[_u].mips; dr->tex[_u].addr = s_d3d.cur_texs[_u].addr; dr->tex[_u].filter = s_d3d.cur_texs[_u].filter; dr->tex[_u].ctrl0 = s_d3d.cur_texs[_u].ctrl0; dr->tex[_u].border = s_d3d.cur_texs[_u].border;
                    dr->tex[_u].set = s_d3d.cur_texs[_u].set;
                    dr->tex_rt[_u]  = -1;
                }
                dr->tex_slot = -1;
                dr->vs_idx = vp_get_vs(s_d3d.current_rsx_state);
                dr->is_clear = 0; rec_not_copy(dr);
                dr->blend = s_d3d.current_rsx_state ? s_d3d.current_rsx_state->blend_enable : 1;
                dr->blend_key = rsx_blend_key(s_d3d.current_rsx_state, dr->blend);
                if (getenv("BLENDDBG")) { static u32 seen[32]; static int ns=0;
                    u32 k = (dr->blend ? 0x80000000u : 0u) | dr->blend_key;
                    int f=0; for (int i=0;i<ns;i++) if (seen[i]==k) f=1;
                    if (!f && ns<32) { seen[ns++]=k;
                        fprintf(stderr, "[BLEND] enable=%d key=0x%X%c",
                                dr->blend, dr->blend_key, 10); } }
                dr->rt_off = current_rt_off(&dr->rt_w, &dr->rt_h, dr->rt_mrt); rec_zeta(dr); rec_mwb(dr); dr->surf_a = rec_surface_a();
                dr->rt_fmt = s_d3d.current_rsx_state ? s_d3d.current_rsx_state->surface_format : 0;
                if (s_d3d.current_rsx_state) {
                    dr->vp_x = s_d3d.current_rsx_state->viewport_x;
                    dr->vp_y = s_d3d.current_rsx_state->viewport_y;
                    dr->vp_w = s_d3d.current_rsx_state->viewport_w;
                    dr->vp_h = s_d3d.current_rsx_state->viewport_h;
                } else { dr->vp_x = dr->vp_y = dr->vp_w = dr->vp_h = 0; }
                rec_scissor(dr);
                dr->cb_slot = s_d3d.draw_count;
                vp_record_cb(s_d3d.draw_count, dr->vs_idx, dr);
                vp_cb_commit(dr, s_d3d.draw_count);
                s_d3d.draw_count++;
            }
            return;
        }
        u32 record_offset = s_d3d.vb_offset;
        u32 emitted = upload_quads_from_rsx(first, count);
        if (emitted == 0) return;
        if (s_d3d.draw_count < MAX_DRAWS) {
            s_d3d.draws[s_d3d.draw_count].vb_byte_offset = record_offset;
            s_d3d.draws[s_d3d.draw_count].vertex_count   = emitted;
            s_d3d.draws[s_d3d.draw_count].topology       = RSX_TOPOLOGY_TRIANGLES;
            s_d3d.draws[s_d3d.draw_count].textured       = s_d3d.tex_bound;
            s_d3d.draws[s_d3d.draw_count].is_vp          = 0;
            s_d3d.draws[s_d3d.draw_count].is_clear       = 0;
            s_d3d.draws[s_d3d.draw_count].rt_off         = 0;
            s_d3d.draw_count++;
        }
        return;
    }

    /* TRIANGLES (prim 5): same VP-path preference as quads -- the guest's
     * vertex program does the MVP transform (gcm/cube draws its cube this way);
     * the fixed-function fallback below applies no transform, so 3D geometry
     * ends up in object space (invisible/garbage). */
    /* Route TRIANGLES/STRIP/FAN through the VP path (sagemono: LBP draws
     * non-indexed strips/fans), but ONLY when the guest ACTUALLY HAS a vertex
     * program (flOw). The VP path transforms via the guest's VP microcode; with
     * no microcode loaded it transforms nothing and the draw silently produces
     * zero pixels (it also uploads to vp_vb, leaving the fixed-function vb empty).
     * Geometry already in clip space with no VP -- flOw's injected scene -- must
     * go down the fixed-function passthrough below instead. */
    if ((primitive == RSX_PRIMITIVE_TRIANGLES || primitive == RSX_PRIMITIVE_TRIANGLE_STRIP
         || primitive == RSX_PRIMITIVE_TRIANGLE_FAN) &&
        s_d3d.vp_vb_mapped && s_d3d.vp_root_sig &&
        s_d3d.current_rsx_state && s_d3d.current_rsx_state->vp_ucode_bytes >= 16) {
        u32 rec = s_d3d.vp_vb_offset;
        u32 emitted = (primitive == RSX_PRIMITIVE_TRIANGLES)
            ? upload_tris_vp(s_d3d.current_rsx_state, first, count)
            : upload_strip_vp(s_d3d.current_rsx_state, first, count, primitive == RSX_PRIMITIVE_TRIANGLE_FAN);
        /* RSX splits one primitive stream across several DRAW_ARRAYS entries
         * inside a single BEGIN/END (each carries at most 256 vertices), and the
         * hardware concatenates them before assembling primitives. Recording a
         * draw per batch instead regroups the triangles: this title's tub mesh
         * arrives as 128 then 256,256,..., neither a multiple of 3, so every
         * batch after the first was assembled one vertex out of phase -- which
         * rendered as spike triangles with a zero texcoord along the tub rim.
         * The batches land contiguously in the vertex buffer, so extending the
         * previous record is all that is needed to restore the stream. */
        if (emitted && s_d3d.merge_prev_draw && s_d3d.draw_count > 0 &&
            s_d3d.merge_first_end == first) {
            D3D12DrawRecord* pv = &s_d3d.draws[s_d3d.draw_count - 1];
            u32 fpnow = s_d3d.current_rsx_state
                        ? s_d3d.current_rsx_state->shader_program : 0;
            if (pv->is_vp && !pv->indexed && pv->topology == RSX_TOPOLOGY_TRIANGLES &&
                pv->fp_addr == fpnow && primitive == RSX_PRIMITIVE_TRIANGLES &&
                pv->begin_epoch == (s_d3d.current_rsx_state
                                    ? s_d3d.current_rsx_state->begin_epoch : 0) &&
                pv->vb_byte_offset + pv->vertex_count * VP_VERT_STRIDE == rec) {
                pv->vertex_count += emitted;
                s_d3d.merge_first_end = first + count;
                return;
            }
        }
        if (emitted && rsx_draw_room()) {
            D3D12DrawRecord* dr = &s_d3d.draws[s_d3d.draw_count];
            dr->vb_byte_offset = rec;
            dr->vertex_count   = emitted;
            dr->topology       = RSX_TOPOLOGY_TRIANGLES;
            dr->textured       = s_d3d.tex_bound;
            dr->is_vp          = 1;
            dr->fp_addr = s_d3d.current_rsx_state ? s_d3d.current_rsx_state->shader_program : 0;
            dr->fp_ctrl = rec_fp_ctrl(); dr->wpos_win = rec_wpos_win(); dr->l2_inval = rec_l2_inval();
            dr->cull = rsx_cull_key(s_d3d.current_rsx_state); dr->zs_key = rsx_zs_key(s_d3d.current_rsx_state); dr->poly = rsx_poly_key(s_d3d.current_rsx_state); rec_step2_state(dr, s_d3d.current_rsx_state); dr->stencil_ref = s_d3d.current_rsx_state ? s_d3d.current_rsx_state->stencil_ref : 0;
        dr->begin_epoch = s_d3d.current_rsx_state ? s_d3d.current_rsx_state->begin_epoch : 0;
            dr->begin_epoch = s_d3d.current_rsx_state ? s_d3d.current_rsx_state->begin_epoch : 0;
            dr->cmask = 0xF;
            if (s_d3d.current_rsx_state) {
                u32 _cm = s_d3d.current_rsx_state->color_mask;
                dr->cmask = ((_cm & 0x00010000) ? 1u : 0u)   /* R */
                          | ((_cm & 0x00000100) ? 2u : 0u)   /* G */
                          | ((_cm & 0x00000001) ? 4u : 0u)   /* B */
                          | ((_cm & 0x01000000) ? 8u : 0u);  /* A */
            }
            dr->alpha_ctl = 0;
            if (s_d3d.current_rsx_state)
                dr->alpha_ctl = ((s_d3d.current_rsx_state->alpha_test_enable ? 1u : 0u) << 16)
                              | ((s_d3d.current_rsx_state->alpha_func & 0xFFu) << 8)
                              | (s_d3d.current_rsx_state->alpha_ref & 0xFFu);
            for (int _u = 0; _u < VP_TEX_UNITS; _u++) {
                dr->tex[_u].off = s_d3d.cur_texs[_u].off;
                dr->tex[_u].raw = s_d3d.cur_texs[_u].raw;
                dr->tex[_u].w   = s_d3d.cur_texs[_u].w;
                dr->tex[_u].h   = s_d3d.cur_texs[_u].h;
                dr->tex[_u].fmt = s_d3d.cur_texs[_u].fmt;
                    dr->tex[_u].ctrl1 = s_d3d.cur_texs[_u].ctrl1;
                    dr->tex[_u].cube  = s_d3d.cur_texs[_u].cube;
                    dr->tex[_u].depth = s_d3d.cur_texs[_u].depth;
                    dr->tex[_u].mips  = s_d3d.cur_texs[_u].mips; dr->tex[_u].addr = s_d3d.cur_texs[_u].addr; dr->tex[_u].filter = s_d3d.cur_texs[_u].filter; dr->tex[_u].ctrl0 = s_d3d.cur_texs[_u].ctrl0; dr->tex[_u].border = s_d3d.cur_texs[_u].border;
                dr->tex[_u].set = s_d3d.cur_texs[_u].set;
                dr->tex_rt[_u]  = -1;
            }
            dr->tex_slot = -1;
            dr->vs_idx = vp_get_vs(s_d3d.current_rsx_state);
            dr->is_clear = 0; rec_not_copy(dr);
            dr->blend = s_d3d.current_rsx_state ? s_d3d.current_rsx_state->blend_enable : 1;
            dr->blend_key = rsx_blend_key(s_d3d.current_rsx_state, dr->blend);
            dr->rt_off = current_rt_off(&dr->rt_w, &dr->rt_h, dr->rt_mrt); rec_zeta(dr); rec_mwb(dr); dr->surf_a = rec_surface_a();
            dr->rt_fmt = s_d3d.current_rsx_state ? s_d3d.current_rsx_state->surface_format : 0;
            if (s_d3d.current_rsx_state) {
                dr->vp_x = s_d3d.current_rsx_state->viewport_x;
                dr->vp_y = s_d3d.current_rsx_state->viewport_y;
                dr->vp_w = s_d3d.current_rsx_state->viewport_w;
                dr->vp_h = s_d3d.current_rsx_state->viewport_h;
            } else { dr->vp_x = dr->vp_y = dr->vp_w = dr->vp_h = 0; }
            rec_scissor(dr);
            dr->cb_slot = s_d3d.draw_count;
                vp_record_cb(s_d3d.draw_count, dr->vs_idx, dr);
            vp_cb_commit(dr, s_d3d.draw_count);
            s_d3d.draw_count++;
            /* Anchor for merging the next DRAW_ARRAYS batch of this stream. */
            s_d3d.merge_prev_draw  = (primitive == RSX_PRIMITIVE_TRIANGLES);
            s_d3d.merge_first_end  = first + count;
        }
        return;
    }
    s_d3d.merge_prev_draw = 0;   /* any other primitive breaks the stream */

    u32 topo = (u32)rsx_primitive_topology(primitive);
    if (topo == RSX_TOPOLOGY_UNSUPPORTED) {
        /* Other primitives still needing index-buffer conversion
         * (line loops, triangle fans) — skip rather than draw wrong. */
        static int s_skipped_nontri = 0;
        if (s_skipped_nontri < 3) {
            printf("[D3D12] draw_arrays: skipping prim=%u (needs index conversion)\n",
                   primitive);
            s_skipped_nontri++;
        }
        return;
    }

    u32 record_offset = s_d3d.vb_offset;
    u32 actual_count  = upload_vertices_from_rsx(first, count);
    if (actual_count == 0) return;

    if (s_d3d.draw_count < MAX_DRAWS) {
        s_d3d.draws[s_d3d.draw_count].vb_byte_offset = record_offset;
        s_d3d.draws[s_d3d.draw_count].vertex_count   = actual_count;
        s_d3d.draws[s_d3d.draw_count].topology       = topo;
        s_d3d.draws[s_d3d.draw_count].textured       = 0;
        s_d3d.draws[s_d3d.draw_count].is_vp          = 0;
        s_d3d.draws[s_d3d.draw_count].is_clear       = 0;
        s_d3d.draws[s_d3d.draw_count].rt_off         = 0;
        s_d3d.draw_count++;
    }
}

static void d3d12_draw_indexed(void* ud, u32 primitive, u32 first, u32 count)
{
    if (s_rt_mode > 0) rt_publish();   /* RSX_RENDER_THREAD: the records before the last are done */
    /* RSX_LIVE_FEED_DBG=1: pair with the [feed] method counts so "the FIFO
     * carries no draw batches" can be checked against "the backend is told
     * to draw" in the same run. */
    { static int dbg = -1; if (dbg < 0) dbg = getenv("RSX_LIVE_FEED_DBG") ? 1 : 0;
      if (dbg) { static long long n = 0;
          if (((++n) % 20000) == 0)
              printf("[backend] indexed callbacks=%lld (prim=%u count=%u)\n", n, primitive, count), fflush(stdout); } }
    (void)ud;
    static int log_count = 0;
    if (log_count < 8) {
        printf("[D3D12] draw_indexed(prim=%u, first=%u, count=%u)\n",
               primitive, first, count);
        log_count++;
    }
    dbt_vtx_dbg(s_d3d.current_rsx_state, "indexed", primitive, first, count);
    surf_dbg(s_d3d.current_rsx_state);
    if (!s_d3d.pipeline_ready) return;
    if (count == 0 || count > MAX_VERTICES) return;
    if (!s_d3d.vp_vb_mapped || !s_d3d.vp_root_sig) return;

    /* Expand through the VP path (indices resolved CPU-side): QUADS -> two
     * triangles per quad, TRIANGLES straight through, STRIP/FAN -> triangle
     * list. Other primitives are skipped rather than drawn wrong. */
    u32 emitted = 0;
    u32 rec = s_d3d.vp_vb_offset;
    const long long vs_t0 = vs_begin_indexed(s_d3d.current_rsx_state, primitive, first, count);
    s_vr_cls = vs_t0 ? vr_class(s_d3d.current_rsx_state) : 0;
    int ix_used = 0, ix_cont = 0, raw_used = 0, raw_cont = 0;
    const u32 ib0 = s_d3d.vp_ib_offset, seg0 = s_seg_n;
    /* A batch in the middle of another path's stream stays on that path: the
     * stream need not have ended on a whole triangle. */
    const D3D12DrawRecord* last = s_d3d.draw_count ? &s_d3d.draws[s_d3d.draw_count - 1] : NULL;
    const int mid_stream = last && s_d3d.merge_prev_draw && s_d3d.merge_first_end == first &&
        last->begin_epoch == (s_d3d.current_rsx_state ? s_d3d.current_rsx_state->begin_epoch : 0);
    if (primitive == RSX_PRIMITIVE_TRIANGLES && raw_on() && s_d3d.current_rsx_state &&
        s_d3d.current_rsx_state->shader_program && (!mid_stream || last->raw)) {
        raw_cont = raw_can_continue(first);
        const int vsi = raw_cont ? s_d3d.draws[s_d3d.draw_count - 1].vs_idx
                                 : vp_get_vs(s_d3d.current_rsx_state);
        if (vsi >= 0 && vp_vs_raw(vsi)) {
            emitted = upload_tris_vp_raw(s_d3d.current_rsx_state, first, count, raw_cont);
            raw_used = emitted != 0;
        }
    }
    /* a kept block, on another path: its bytes to the ring (raw has done that if it declined) */
    if (!raw_used && s_kept_seen) {
        u32 kio = 0;
        const rsx_kept* k = kept_find(s_d3d.current_rsx_state, &kio);
        if (k) kept_fill(s_d3d.current_rsx_state, k, kio, first, count);
    }
    /* the vertex pass's record, on another path: lit in guest memory, once */
    if (!raw_used && s_vpass.set) vpass_guest(s_d3d.current_rsx_state);
    if (!raw_used && primitive == RSX_PRIMITIVE_TRIANGLES && ix_on()) {
        ix_cont = ix_can_continue(first);
        emitted = upload_tris_vp_ix(s_d3d.current_rsx_state, first, count, ix_cont);
        ix_used = emitted != 0;
    }
    if (raw_used || ix_used) { /* a run took it */ }
    else if (primitive == RSX_PRIMITIVE_QUADS)        emitted = upload_quads_vp_indexed(s_d3d.current_rsx_state, first, count);
    else if (primitive == RSX_PRIMITIVE_TRIANGLES)    emitted = upload_tris_vp_indexed(s_d3d.current_rsx_state, first, count);
    else if (primitive == RSX_PRIMITIVE_TRIANGLE_STRIP) emitted = upload_strip_vp_indexed(s_d3d.current_rsx_state, first, count, 0);
    else if (primitive == RSX_PRIMITIVE_TRIANGLE_FAN)   emitted = upload_strip_vp_indexed(s_d3d.current_rsx_state, first, count, 1);
    else {
        static int _skip = 0;
        if (PS3_LOG_FIRST(_skip, 3))
            printf("[D3D12] draw_indexed: skipping prim=%u (not wired)\n", primitive);
        return;
    }
    vs_end(vs_t0, emitted);
    if (vs_t0) {
        const int k = s_vr_cls;
        if (k && s_d3d.current_rsx_state) {
            const u32 ck = rsx_cull_key(s_d3d.current_rsx_state) & 7u;
            if (s_d3d.current_rsx_state->vertex_attribs[0].stride == 54) s_vs.c_cull54[ck]++; else s_vs.c_cullo[ck]++;
        }
        s_vs.c_draws[k]++;
        s_vs.c_refs[k] += count;
        if (raw_used) s_vs.c_raw[k]++; else if (ix_used) s_vs.c_ix[k]++; else s_vs.c_cpu[k]++;
        s_vs.c_t[k] += vs_now() - vs_t0;
    }
    if (raw_used && raw_cont) {        /* a batch of the previous record's raw run */
        D3D12DrawRecord* pv = &s_d3d.draws[s_d3d.draw_count - 1];
        pv->index_count += emitted;
        pv->seg_n = s_seg_n - pv->seg_first;
        s_d3d.merge_first_end = first + count;
        return;
    }
    if (ix_used && ix_cont) {          /* a batch of the previous record's run */
        D3D12DrawRecord* pv = &s_d3d.draws[s_d3d.draw_count - 1];
        pv->index_count += emitted;
        pv->vertex_count = s_ix_nverts;
        s_d3d.merge_first_end = first + count;
        return;
    }
    /* Same BEGIN/END batch concatenation as the non-indexed path: this title's
     * fluid arrives as ~3000 DRAW_INDEX_ARRAY batches of 256, and 256 is not a
     * multiple of 3, so every batch after the first assembled out of phase. */
    if (emitted && !ix_used && !raw_used && s_d3d.merge_prev_draw && s_d3d.draw_count > 0 &&
        s_d3d.merge_first_end == first && primitive == RSX_PRIMITIVE_TRIANGLES &&
        !s_d3d.draws[s_d3d.draw_count - 1].indexed) {
        D3D12DrawRecord* pv = &s_d3d.draws[s_d3d.draw_count - 1];
        u32 fpnow = s_d3d.current_rsx_state
                    ? s_d3d.current_rsx_state->shader_program : 0;
        if (pv->is_vp && pv->topology == RSX_TOPOLOGY_TRIANGLES &&
            pv->fp_addr == fpnow &&
            pv->begin_epoch == (s_d3d.current_rsx_state
                                ? s_d3d.current_rsx_state->begin_epoch : 0) &&
            pv->vb_byte_offset + pv->vertex_count * VP_VERT_STRIDE == rec) {
            pv->vertex_count += emitted;
            s_d3d.merge_first_end = first + count;
            return;
        }
    }
    if (emitted && rsx_draw_room()) {
        D3D12DrawRecord* dr = &s_d3d.draws[s_d3d.draw_count];
        dr->vb_byte_offset = rec;
        dr->vertex_count   = emitted;
        dr->indexed        = ix_used || raw_used;
        dr->raw            = raw_used;
        if (ix_used) {
            dr->vb_byte_offset = s_ix_vbase * VP_VERT_STRIDE;
            dr->vertex_count   = s_ix_nverts;
            dr->ib_first       = ib0;
            dr->index_count    = emitted;
            s_raw_run_valid    = 0;
        }
        if (raw_used) {
            dr->vb_byte_offset = 0;
            dr->vertex_count   = 0;
            dr->ib_first       = ib0;
            dr->index_count    = emitted;
            dr->seg_first      = seg0;
            dr->seg_n          = s_seg_n - seg0;
            s_ix_run_valid     = 0;
        }
        dr->topology       = RSX_TOPOLOGY_TRIANGLES;
        dr->textured       = s_d3d.tex_bound;
        dr->is_vp          = 1;
        dr->fp_addr = s_d3d.current_rsx_state ? s_d3d.current_rsx_state->shader_program : 0;
        dr->fp_ctrl = rec_fp_ctrl(); dr->wpos_win = rec_wpos_win(); dr->l2_inval = rec_l2_inval();
        dr->cull = rsx_cull_key(s_d3d.current_rsx_state); dr->zs_key = rsx_zs_key(s_d3d.current_rsx_state); dr->poly = rsx_poly_key(s_d3d.current_rsx_state); rec_step2_state(dr, s_d3d.current_rsx_state); dr->stencil_ref = s_d3d.current_rsx_state ? s_d3d.current_rsx_state->stencil_ref : 0;
        dr->begin_epoch = s_d3d.current_rsx_state ? s_d3d.current_rsx_state->begin_epoch : 0;
        dr->cmask = 0xF;
        if (s_d3d.current_rsx_state) {
            u32 _cm = s_d3d.current_rsx_state->color_mask;
            dr->cmask = ((_cm & 0x00010000) ? 1u : 0u)   /* R */
                      | ((_cm & 0x00000100) ? 2u : 0u)   /* G */
                      | ((_cm & 0x00000001) ? 4u : 0u)   /* B */
                      | ((_cm & 0x01000000) ? 8u : 0u);  /* A */
        }
        dr->alpha_ctl = 0;
        if (s_d3d.current_rsx_state)
            dr->alpha_ctl = ((s_d3d.current_rsx_state->alpha_test_enable ? 1u : 0u) << 16)
                          | ((s_d3d.current_rsx_state->alpha_func & 0xFFu) << 8)
                          | (s_d3d.current_rsx_state->alpha_ref & 0xFFu);
        for (int _u = 0; _u < VP_TEX_UNITS; _u++) {
            dr->tex[_u].off = s_d3d.cur_texs[_u].off;
            dr->tex[_u].raw = s_d3d.cur_texs[_u].raw;
            dr->tex[_u].w   = s_d3d.cur_texs[_u].w;
            dr->tex[_u].h   = s_d3d.cur_texs[_u].h;
            dr->tex[_u].fmt = s_d3d.cur_texs[_u].fmt;
                    dr->tex[_u].ctrl1 = s_d3d.cur_texs[_u].ctrl1;
                    dr->tex[_u].cube  = s_d3d.cur_texs[_u].cube;
                    dr->tex[_u].depth = s_d3d.cur_texs[_u].depth;
                    dr->tex[_u].mips  = s_d3d.cur_texs[_u].mips; dr->tex[_u].addr = s_d3d.cur_texs[_u].addr; dr->tex[_u].filter = s_d3d.cur_texs[_u].filter; dr->tex[_u].ctrl0 = s_d3d.cur_texs[_u].ctrl0; dr->tex[_u].border = s_d3d.cur_texs[_u].border;
            dr->tex[_u].set = s_d3d.cur_texs[_u].set;
            dr->tex_rt[_u]  = -1;
        }
        dr->tex_slot = -1;
        dr->vs_idx = vp_get_vs(s_d3d.current_rsx_state);
        dr->is_clear = 0; rec_not_copy(dr);
        dr->blend = s_d3d.current_rsx_state ? s_d3d.current_rsx_state->blend_enable : 1;
        dr->blend_key = rsx_blend_key(s_d3d.current_rsx_state, dr->blend);
        dr->rt_off = current_rt_off(&dr->rt_w, &dr->rt_h, dr->rt_mrt); rec_zeta(dr); rec_mwb(dr); dr->surf_a = rec_surface_a();
        dr->rt_fmt = s_d3d.current_rsx_state ? s_d3d.current_rsx_state->surface_format : 0;
        if (s_d3d.current_rsx_state) {
            dr->vp_x = s_d3d.current_rsx_state->viewport_x;
            dr->vp_y = s_d3d.current_rsx_state->viewport_y;
            dr->vp_w = s_d3d.current_rsx_state->viewport_w;
            dr->vp_h = s_d3d.current_rsx_state->viewport_h;
        } else { dr->vp_x = dr->vp_y = dr->vp_w = dr->vp_h = 0; }
        rec_scissor(dr);
        dr->cb_slot = s_d3d.draw_count;
                vp_record_cb(s_d3d.draw_count, dr->vs_idx, dr);
        if (dr->raw) vp_record_raw_cb(s_d3d.draw_count, s_d3d.current_rsx_state);
        vp_cb_commit(dr, s_d3d.draw_count);
        s_d3d.draw_count++;
        /* Anchor for merging the next batch of this indexed stream. */
        s_d3d.merge_prev_draw = (primitive == RSX_PRIMITIVE_TRIANGLES);
        s_d3d.merge_first_end = first + count;
    }
}

static void d3d12_bind_texture(void* ud, u32 unit, const rsx_texture_state* tex)
{
    (void)ud;
    extern uint8_t* vm_base;
    extern u32 cellGcmResolveOffset(u32);

    u32 width  = (tex->image_rect >> 16) & 0xFFFF;
    u32 height = tex->image_rect & 0xFFFF;
    u32 format = (tex->format >> 8) & 0xFF;
    u32 offset = tex->offset;
    /* TEXFMT_ALL=1: every distinct texture a draw binds, whatever its format.
     * TEXFMTDBG logs only the formats this backend binds; a format it drops
     * (the draw then samples nothing) never shows up there. */
    { static int ta = -1;
      if (ta < 0) ta = getenv("TEXFMT_ALL") ? 1 : 0;
      if (ta) {
          static u32 seen[256]; static int ns = 0; int known = 0;
          const u32 key = offset ^ (format << 24) ^ unit;
          for (int k = 0; k < ns; k++) if (seen[k] == key) known = 1;
          if (!known && ns < 256) { seen[ns++] = key;
              fprintf(stderr, "[TEXFMT_ALL] unit=%u fmt=0x%02X %ux%u pitch=%u off=0x%08X "
                              "remap=0x%08X ctrl0=0x%08X filter=0x%08X%c",
                      unit, format, (tex->image_rect >> 16) & 0xFFFF, tex->image_rect & 0xFFFF,
                      tex->control3 & 0xFFFFF, offset, tex->control1, tex->control0, tex->filter, 10); }
      } }
    /* TEXHI_AFTER=<flips>: the distinct textures bound at units 4 and up once
     * that many flips are requested -- this backend binds units 0-3 only, and
     * a scene reached late has its own (TEXFMT_ALL's 256 fill up on the way). */
    { static long after = -2;
      if (after == -2) { const char* e = getenv("TEXHI_AFTER"); after = e ? atol(e) : -1; }
      extern unsigned cellGcm_flip_request_count(void);
      if (after >= 0 && unit >= 4 && cellGcm_flip_request_count() >= (unsigned)after) {
          static u32 seen[64]; static int ns = 0; int known = 0;
          const u32 fp = s_d3d.current_rsx_state ? s_d3d.current_rsx_state->shader_program : 0;
          const u32 key = offset ^ (format << 24) ^ unit ^ (fp << 4);
          for (int k = 0; k < ns; k++) if (seen[k] == key) known = 1;
          if (!known && ns < 64) { seen[ns++] = key;
              fprintf(stderr, "[TEXHI] unit=%u fmt=0x%02X %ux%u off=0x%08X fp=0x%08X%c",
                      unit, format, width, height, offset, fp, 10); }
      } }
    /* CUBEDBG=1: NV4097_SET_TEXTURE_FORMAT bit 2 is the cubemap flag, and the
     * line above throws it away with the rest of the low byte. Nothing in this
     * backend handles cube textures, so a cubemap bound for an environment
     * reflection is sampled as a plain 2D image -- which is what the chrome
     * faucet's black patches and banded escutcheons look like. */
    { static int cd = -1;
      if (cd < 0) { const char* e = getenv("CUBEDBG"); cd = e ? atoi(e) : 0; }
      if (cd && (tex->format & 4)) {
          static u32 seen[8]; static int ns = 0; int known = 0;
          for (int k = 0; k < ns; k++) if (seen[k] == offset) known = 1;
          if (!known && ns < 8) { seen[ns++] = offset;
              fprintf(stderr, "[CUBE] unit=%u offset=0x%08X %ux%u fmt=0x%02X"
                              " raw_format=0x%08X (cubemap bit SET)%c",
                      unit, offset, width, height, format, tex->format, 10); }
      } }

    static int log_count = 0;
    if (log_count < 10) {
        printf("[D3D12] bind_texture(unit=%u, offset=0x%X, fmt=0x%02X, %ux%u)\n",
               unit, offset, format, width, height);
        log_count++;
    }
    /* MOVIE_BIND=1: trace movie-plane binds (640x360 Y / 320x180 U/V) with the
     * resolved EA + a content probe -- used to diagnose the Bink frame-buffer
     * ring mismatch (the draw binds a cleared buffer 0x4D80 before the one the
     * decoder actually fills). */
    if (env_on("MOVIE_BIND", &s_env_movie_bind) &&
        ((width == 640 && height == 360) || (width == 320 && height == 180))) {
        extern u32 cellGcmResolveLocated(int local, u32 offset);
        static int _mv = 0; if (PS3_LOG_FIRST(_mv, 24)) {
            /* Resolve the SAME raw offset both ways -- as LOCAL (VRAM) and as MAIN
             * (IO-mapped) -- and count nonzero bytes over the whole plane for each.
             * If the plane the game declares (loc bits fmt&3) is zero but the OTHER
             * pool has content, the decoder wrote to a different memory space than
             * our texture resolve picked (the local-vs-main mismatch). */
            u32 loc = tex->format & 3;               /* 1=LOCAL, 2=MAIN */
            u32 ea_loc  = cellGcmResolveLocated(1, offset);
            u32 ea_main = cellGcmResolveLocated(0, offset);
            u32 plane = width * height;
            u32 nz_loc = 0, nz_main = 0;
            if (vm_base) for (u32 i = 0; i < plane; i += 137) {
                if (vm_base[ea_loc  + i]) nz_loc++;
                if (vm_base[ea_main + i]) nz_main++;
            }
            fprintf(stderr, "[mv-bind] unit=%u fmt=0x%02X loc=%u %ux%u off=0x%X | LOCAL ea=0x%08X nz=%u | MAIN ea=0x%08X nz=%u\n",
                    unit, format, loc, width, height, offset,
                    ea_loc, nz_loc, ea_main, nz_main);
        }
    }

    if (!vm_base || width == 0 || height == 0) return;

    /* Record the currently-bound atlas so subsequent quad draws sample it. The
     * actual GPU upload happens in render_frame (we have no open command list
     * here). Only the 8-bit single-channel font atlas (B8, RSX fmt base 0x81 /
     * as-seen 0xA1 with the LN flag) is wired up so far; other formats fall
     * back to untextured. */
    u32 base_fmt = format & 0x9F;   /* strip LN(0x20)/UN(0x40) flags */
    /* VP path: record the latest bound texture (any supported format) so draws
     * can carry it per-draw. Location bits (format[1:0]): 1 = LOCAL, 2 = MAIN. */
    extern u32 cellGcmResolveLocated(int local, u32 offset);
    /* D3D12_TEX_UNITS=<n>: record units below n only (diagnostic). */
    static int s_units = -1;
    if (s_units < 0) { const char* e = getenv("D3D12_TEX_UNITS"); s_units = e ? atoi(e) : VP_TEX_UNITS;
                       if (s_units > VP_TEX_UNITS) s_units = VP_TEX_UNITS; }
    if ((int)unit < s_units &&
        (base_fmt == 0x81 /* B8 */ || base_fmt == 0x85 /* A8R8G8B8 */ ||
         base_fmt == 0x8B /* G8B8: LBP's font atlas */ ||
         (base_fmt >= 0x86 && base_fmt <= 0x88) /* DXT1/23/45 */ ||
         base_fmt == 0x9A /* W16Z16Y16X16 half-float: RTT intermediates */ ||
         /* One- and two-channel 16/32-bit formats rsx_texture_layout already
          * decodes: X16 (God of War: Ascension's 256x256 font atlas -- without
          * it every glyph drew as a solid quad), Y16_X16, X32_FLOAT and
          * Y16_X16_FLOAT (its post-process intermediates). */
         base_fmt == 0x94 || base_fmt == 0x95 || base_fmt == 0x9C || base_fmt == 0x9F ||
         /* Depth textures, read from the depth surface at their offset
          * (dr_tex_depth_fmt): God of War: Ascension's shadow maps. Dropped,
          * the unit kept the texture bound before. */
         (base_fmt >= 0x90 && base_fmt <= 0x93))) {
        /* TEX_RESOLVE_AUTO=1: resolve through the page tables (local-page map
         * then IO table) instead of trusting the format's location bits. A
         * texture the guest built in main memory but tagged local resolves to
         * untouched VRAM and samples as all-zero -- geometry renders, flat. */
        { static int _ra = -1; if (_ra < 0) _ra = getenv("TEX_RESOLVE_AUTO") ? 1 : 0;
          extern u32 cellGcmResolveIO(u32);
          u32 _r = 0;
          if (_ra) _r = cellGcmResolveIO(offset);          /* IO table first */
          if (!_r) _r = cellGcmResolveLocated((tex->format & 3) == 1, offset);
          /* TEX_PROBE=1: the location bits are not always right. This title
           * binds its menu textures with a location that resolves into main
           * memory, where the bytes are all zero, while the same raw offset
           * resolved as LOCAL lands on real data -- so 100k menu draws sample
           * black and the menu never appears. When the chosen resolution is
           * empty over a sample and the other one is not, take the other. */
          { static int _tp = -1;
            if (_tp < 0) _tp = getenv("TEX_PROBE") ? 1 : 0;
            if (_tp && vm_base && _r) {
                u32 alt = cellGcmResolveLocated((tex->format & 3) != 1, offset);
                if (alt && alt != _r && alt < 0xE0000000u && _r < 0xE0000000u) {
                    u32 span = width * height / 2;
                    if (span > 4096) span = 4096;
                    u32 nz_r = 0, nz_a = 0;
                    for (u32 i = 0; i < span; i += 7) {
                        if (vm_base[_r  + i]) nz_r++;
                        if (vm_base[alt + i]) nz_a++;
                    }
                    if (nz_r == 0 && nz_a > 0) {
                        static u32 seen[32]; static int ns = 0; int known = 0;
                        for (int k = 0; k < ns; k++) if (seen[k] == offset) known = 1;
                        if (!known && ns < 32) { seen[ns++] = offset;
                            fprintf(stderr, "[TEX_PROBE] 0x%08X: %s empty, using %s"
                                            " (%u non-zero)%c", offset,
                                    ((tex->format & 3) == 1) ? "local" : "main",
                                    ((tex->format & 3) == 1) ? "main" : "local",
                                    nz_a, 10); }
                        _r = alt;
                    }
                }
            } }
          s_d3d.cur_texs[unit].off = _r; }
        s_d3d.cur_texs[unit].raw = offset;
        s_d3d.cur_texs[unit].w = width; s_d3d.cur_texs[unit].h = height;
        s_d3d.cur_texs[unit].fmt = format;   /* full byte: LN(0x20)/UN(0x40) kept */
        s_d3d.cur_texs[unit].ctrl1 = tex->control1;
        s_d3d.cur_texs[unit].cube  = (tex->format & 4) ? 1 : 0;
        /* A volume: dimension 3 (SET_TEXTURE_FORMAT bits 4..7), its depth in
         * CONTROL3 bits 20..31. */
        s_d3d.cur_texs[unit].depth = (((tex->format >> 4) & 0xFu) == 3u && !(tex->format & 4) &&
                                      (tex->control3 >> 20) > 1u) ? (tex->control3 >> 20) : 1u;
        s_d3d.cur_texs[unit].mips  = (tex->format >> 16) & 0xFFFFu;
        s_d3d.cur_texs[unit].addr   = tex->address;
        s_d3d.cur_texs[unit].filter = tex->filter;
        s_d3d.cur_texs[unit].ctrl0  = tex->control0;
        s_d3d.cur_texs[unit].border = tex->border_color;
        if (env_on("TEXFMTDBG", &s_env_texfmtdbg)) { static u32 seen[64]; static int ns=0;
            u32 k = (unit<<24) | (((tex->format>>8) & 0xFFu)<<16) | (width & 0xFFFFu);
            int f=0; for (int i=0;i<ns;i++) if (seen[i]==k) f=1;
            if (!f && ns < 64) { seen[ns++]=k;
                fprintf(stderr, "[TEXFMT] unit=%u fmt=0x%02X %ux%u mips=%u cube=%d off=0x%X%c",
                        unit, (tex->format >> 8) & 0xFFu, width, height,
                        (tex->format>>16)&0xFFFFu, (tex->format&4)?1:0, offset, 10); } }
        s_d3d.cur_texs[unit].set = 1;
    }
    if (base_fmt == 0x81 /* B8 */) {
        s_d3d.tex_src_offset = cellGcmResolveLocated((tex->format & 3) == 1, offset);
        if (s_d3d.tex_w != width || s_d3d.tex_h != height) {
            /* dims changed -> resource must be (re)created in render_frame */
            s_d3d.tex_ready = 0;
        }
        s_d3d.tex_w     = width;
        s_d3d.tex_h     = height;
        s_d3d.tex_bound = 1;
        s_d3d.tex_dirty = 1;
    } else {
        s_d3d.tex_bound = 0;
    }
}

static void d3d12_set_vertex_attribs(void* ud, const rsx_state* state)
{
    (void)ud;
    s_d3d.current_rsx_state = state;

    /* Log enabled vertex attributes for debugging. RSX_VTXDBG=<N> raises the
     * cap: the fixed 5 were all consumed by boot-time setup, so the layouts the
     * real draws use were never printed. */
    static int log_count = 0, log_cap = -1;
    if (log_cap < 0) { const char* e = getenv("RSX_VTXDBG"); log_cap = e ? atoi(e) : 5; }
    if (log_count < log_cap) {
        printf("[D3D12] set_vertex_attribs:\n");
        for (int i = 0; i < 16; i++) {
            const rsx_vertex_attrib* a = &state->vertex_attribs[i];
            if (a->enabled) {
                const char* type_name = "?";
                switch (a->type) {
                case 1: type_name = "snorm16"; break;
                case 2: type_name = "float32"; break;
                case 3: type_name = "float16"; break;
                case 4: type_name = "ubyte"; break;
                case 5: type_name = "s16"; break;
                case 7: type_name = "ubyte256"; break;
                }
                printf("  attrib[%d]: %s x%u, stride=%u, offset=0x%X\n",
                       i, type_name, a->size, a->stride, a->offset);
            }
        }
        log_count++;
    }
}

static void d3d12_set_shader(void* ud, const rsx_state* state)
{
    (void)ud;
    s_d3d.current_rsx_state = state;

    static int log_count = 0;
    if (log_count < 5) {
        printf("[D3D12] set_shader: fp_addr=0x%08X, vp_load=%u, output_mask=0x%08X\n",
               state->fragment_program_addr, state->transform_program_load,
               state->vertex_attrib_output_mask);
        log_count++;
    }

    /* TODO: Look up or compile a PSO matching this shader combination.
     * For now we use the basic vertex-colored PSO for everything. */
}

static void d3d12_set_blend(void* ud, const rsx_state* state)
{
    (void)ud;
    /* TODO: modify PSO blend state or use dynamic state.
     * D3D12 requires PSO recreation for blend state changes,
     * so we'd need a PSO cache keyed by blend configuration. */
    static int log_count = 0;
    if (log_count < 5) {
        printf("[D3D12] set_blend(enable=%d, sfactor=0x%X, dfactor=0x%X)\n",
               state->blend_enable, state->blend_sfactor, state->blend_dfactor);
        log_count++;
    }
}

static void d3d12_set_depth_stencil(void* ud, const rsx_state* state)
{
    (void)ud;
    static int log_count = 0;
    if (log_count < 5) {
        printf("[D3D12] set_depth_stencil(depth=%d, stencil=%d, func=0x%X)\n",
               state->depth_test_enable, state->stencil_test_enable,
               state->depth_func);
        log_count++;
    }
}

/* ---------------------------------------------------------------------------
 * Backend registration
 * -----------------------------------------------------------------------*/

static rsx_backend s_d3d12_backend = {0};

/* ---------------------------------------------------------------------------
 * Public API
 * -----------------------------------------------------------------------*/

int rsx_d3d12_backend_init(u32 width, u32 height, const char* title)
{
    memset(&s_d3d, 0, sizeof(s_d3d));
    vp_tex_cache_reset();
    s_d3d.width = width;
    s_d3d.height = height;
    s_d3d.pw = width;
    s_d3d.ph = height;
    s_d3d.clear_color[0] = 0.0f;
    s_d3d.clear_color[1] = 0.0f;
    s_d3d.clear_color[2] = 0.1f;
    s_d3d.clear_color[3] = 1.0f;

    /* Debug: dump the first N presented frames to BMP if CELLMARK_DUMP is set
     * (its numeric value when > 1, else 24). */
    {
        const char* dv = getenv("CELLMARK_DUMP");
        int n = dv ? atoi(dv) : 0;
        s_d3d.dump_frames_left = dv ? (n > 1 ? n : 24) : 0;
        /* CELLMARK_DUMP_SKIP=N: ignore the first N presents before dumping.
         * The opening frames are the loading screen; the interesting content
         * only appears once the sim SPUs have advanced the scene. */
        { const char* sv = getenv("CELLMARK_DUMP_SKIP");
          s_d3d.dump_skip_left = sv ? atoi(sv) : 0; }
    }

    /* Create window */
    {
        extern char g_rsx_title_base[128];
        snprintf(g_rsx_title_base, sizeof(g_rsx_title_base), "%s",
                 title ? title : "ps3recomp");
    }
    s_d3d.hwnd = create_window(width, height, title);
    if (!s_d3d.hwnd) {
        printf("[D3D12] ERROR: Window creation failed\n");
        return -1;
    }

    /* Initialize D3D12 */
    if (init_d3d12(width, height) != 0) {
        printf("[D3D12] ERROR: D3D12 initialization failed\n");
        return -1;
    }

    /* Set up backend callbacks */
    s_d3d12_backend.userdata          = &s_d3d;
    s_d3d12_backend.init              = d3d12_init;
    s_d3d12_backend.shutdown          = d3d12_shutdown;
    s_d3d12_backend.begin_frame       = d3d12_begin_frame;
    s_d3d12_backend.end_frame         = d3d12_end_frame;
    s_d3d12_backend.present           = d3d12_present;
    s_d3d12_backend.clear             = d3d12_clear;
    s_d3d12_backend.set_render_target = d3d12_set_render_target;
    s_d3d12_backend.set_viewport      = d3d12_set_viewport;
    s_d3d12_backend.set_blend         = d3d12_set_blend;
    s_d3d12_backend.set_depth_stencil = d3d12_set_depth_stencil;
    s_d3d12_backend.set_shader        = d3d12_set_shader;
    s_d3d12_backend.set_vertex_attribs = d3d12_set_vertex_attribs;
    s_d3d12_backend.draw_arrays       = d3d12_draw_arrays;
    s_d3d12_backend.draw_indexed      = d3d12_draw_indexed;
    s_d3d12_backend.bind_texture      = d3d12_bind_texture;

    rsx_set_backend(&s_d3d12_backend);
    { extern void (*cellGcm_blit_observer)(int, u32, u32, u32, u32, u32, u32, u32, u32);
      extern void (*cellGcm_blit_local_observer)(u32, u32, u32, u32, u32, u32);
      cellGcm_blit_observer = d3d12_blit_observed;
      cellGcm_blit_local_observer = d3d12_blit_local_observed;
      extern void (*cellGcm_flip_observer)(u32);
      cellGcm_flip_observer = d3d12_flip_observed;
      extern void (*cellGcm_zpass_observer)(int, u32, u32);
      if (zq_wanted()) cellGcm_zpass_observer = d3d12_zpass_observed; }

    s_d3d.initialized = 1;
    s_d3d.last_fps_time = GetTickCount64();
    rt_start();

    printf("[D3D12] Backend ready: %ux%u\n", width, height);
    return 0;
}

void rsx_d3d12_backend_shutdown(void)
{
    if (!s_d3d.initialized) return;
    rt_stop();

    wait_for_gpu();
    sj_stop();   /* a worker may be creating a pipeline on the device */

    /* Release D3D12 resources */
    if (s_d3d.vertex_buffer) {
        s_d3d.vertex_buffer->lpVtbl->Unmap(s_d3d.vertex_buffer, 0, NULL);
        s_d3d.vertex_buffer->lpVtbl->Release(s_d3d.vertex_buffer);
    }
    if (s_d3d.pipeline_state)        s_d3d.pipeline_state->lpVtbl->Release(s_d3d.pipeline_state);
    if (s_d3d.pipeline_state_lines)  s_d3d.pipeline_state_lines->lpVtbl->Release(s_d3d.pipeline_state_lines);
    if (s_d3d.pipeline_state_points) s_d3d.pipeline_state_points->lpVtbl->Release(s_d3d.pipeline_state_points);
    if (s_d3d.depth_buffer) s_d3d.depth_buffer->lpVtbl->Release(s_d3d.depth_buffer);
    if (s_d3d.dsv_heap)     s_d3d.dsv_heap->lpVtbl->Release(s_d3d.dsv_heap);
    if (s_d3d.root_signature) s_d3d.root_signature->lpVtbl->Release(s_d3d.root_signature);
    if (s_d3d.fence) s_d3d.fence->lpVtbl->Release(s_d3d.fence);
    if (s_d3d.fence_event) CloseHandle(s_d3d.fence_event);
    if (s_d3d.cmd_list) s_d3d.cmd_list->lpVtbl->Release(s_d3d.cmd_list);
    if (s_d3d.tex_ring) s_d3d.tex_ring->lpVtbl->Release(s_d3d.tex_ring);
    for (u32 i = 0; i < FRAME_COUNT; i++) {
        if (s_d3d.cmd_allocators[i]) s_d3d.cmd_allocators[i]->lpVtbl->Release(s_d3d.cmd_allocators[i]);
        if (s_d3d.render_targets[i]) s_d3d.render_targets[i]->lpVtbl->Release(s_d3d.render_targets[i]);
        if (s_d3d.backbuffers[i]) s_d3d.backbuffers[i]->lpVtbl->Release(s_d3d.backbuffers[i]);
    }
    {
        if (s_d3d.pp_pso) s_d3d.pp_pso->lpVtbl->Release(s_d3d.pp_pso);
        if (s_d3d.pp_pso_easu) s_d3d.pp_pso_easu->lpVtbl->Release(s_d3d.pp_pso_easu);
        if (s_d3d.pp_pso_rcas) s_d3d.pp_pso_rcas->lpVtbl->Release(s_d3d.pp_pso_rcas);
        if (s_d3d.pp_tmp) s_d3d.pp_tmp->lpVtbl->Release(s_d3d.pp_tmp);
        if (s_d3d.ov_pso) s_d3d.ov_pso->lpVtbl->Release(s_d3d.ov_pso);
        if (s_d3d.ov_tex) s_d3d.ov_tex->lpVtbl->Release(s_d3d.ov_tex);
        for (u32 i = 0; i < FRAME_COUNT; i++)
            if (s_d3d.ov_up[i]) s_d3d.ov_up[i]->lpVtbl->Release(s_d3d.ov_up[i]);
        if (s_d3d.shot_buf) s_d3d.shot_buf->lpVtbl->Release(s_d3d.shot_buf);
        for (int i = 0; i < 3; i++)
            if (s_d3d.aa_pso[i]) s_d3d.aa_pso[i]->lpVtbl->Release(s_d3d.aa_pso[i]);
        ID3D12Resource* const aa_res[6] = { s_d3d.aa_edges, s_d3d.aa_blend, s_d3d.aa_out,
                                            s_d3d.aa_area, s_d3d.aa_search, s_d3d.aa_up };
        for (int i = 0; i < 6; i++)
            if (aa_res[i]) aa_res[i]->lpVtbl->Release(aa_res[i]);
        if (s_d3d.aa_rtv_heap) s_d3d.aa_rtv_heap->lpVtbl->Release(s_d3d.aa_rtv_heap);
        if (s_d3d.aa_root) s_d3d.aa_root->lpVtbl->Release(s_d3d.aa_root);
        if (s_d3d.pp_root) s_d3d.pp_root->lpVtbl->Release(s_d3d.pp_root);
        if (s_d3d.pp_srv_heap) s_d3d.pp_srv_heap->lpVtbl->Release(s_d3d.pp_srv_heap);
        if (s_d3d.bb_rtv_heap) s_d3d.bb_rtv_heap->lpVtbl->Release(s_d3d.bb_rtv_heap);
    }
    if (s_d3d.rtv_heap) s_d3d.rtv_heap->lpVtbl->Release(s_d3d.rtv_heap);
    if (s_d3d.swap_chain) s_d3d.swap_chain->lpVtbl->Release(s_d3d.swap_chain);
    if (s_d3d.cmd_queue) s_d3d.cmd_queue->lpVtbl->Release(s_d3d.cmd_queue);
    if (s_d3d.device) s_d3d.device->lpVtbl->Release(s_d3d.device);

    if (s_d3d.hwnd) DestroyWindow(s_d3d.hwnd);

    rsx_set_backend(NULL);
    s_d3d.initialized = 0;

    printf("[D3D12] Backend shut down\n");
}

int rsx_d3d12_backend_pump_messages(void)
{
    MSG msg;
    while (PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE)) {
        if (msg.message == WM_QUIT) return -1;
        TranslateMessage(&msg);
        DispatchMessageA(&msg);
    }
    return s_d3d.window_closed ? -1 : 0;
}

/* D3D12_RECORD_AHEAD=1: see "render_frame() in stages". Off while a debug
 * filter that edits the batch at render time is set. */
static int rf_ahead_on(void)
{
    static int v = -1;
    if (v < 0) {
        const char* e = getenv("D3D12_RECORD_AHEAD");
        v = (e && atoi(e) > 0) ? 1 : 0;
        static const char* const filters[] = { "RTT_PASS", "DRAW_LIMIT", "DRAW_SKIP_TEX",
            "DRAW_KEEP_TEX", "DRAW_LAST_TEX", "RENDERDOC_DLL", "RSX_ACCUM_FRAME" };
        for (unsigned i = 0; v && i < sizeof filters / sizeof filters[0]; i++)
            if (getenv(filters[i])) v = 0;
    }
    return v;
}

/* Record the batch's records before `upto` ahead of its end. A pass that
 * catches only a few new records is left for the next one. wait_gpu: open the
 * batch while the GPU still has the last one, rf_open waiting for it (the
 * render thread); the drain leaves it to its next pass rather than stall. */
#define RF_AHEAD_MIN 16
static void rf_ahead(u32 upto, int wait_gpu)
{
    if (!s_rf.open) {
        if (upto < RF_AHEAD_MIN || !s_d3d.vp_ready) return;
        for (u32 i = 0; i < upto; i++)
            if (!s_d3d.draws[i].is_vp) return;    /* legacy records: in one go */
        if (!wait_gpu && s_d3d.fence->lpVtbl->GetCompletedValue(s_d3d.fence) + 1 <
            s_d3d.fence_values[s_d3d.frame_index])
            return;
        s_rf_open_ahead = 1;
        rf_open();
        s_rf_open_ahead = 0;
        rf_target();
        rf_vp_begin();
        s_rf.open = 1;
    } else if (upto < s_rf.done + RF_AHEAD_MIN) {
        return;
    }
    for (u32 i = s_rf.pre_done; i < upto; i++)
        if (!s_d3d.draws[i].is_vp) return;        /* the end of the batch takes them */
    rf_prepass(s_rf.pre_done, upto);
    const u32 vupto = cap_stop(upto);   /* RSX_CAP_STOP */
    if (vupto > s_rf.done) s_rf.done = rf_vp_draws(s_rf.done, vupto, 1);
}

/* ---------------------------------------------------------------------------
 * The render thread
 * -----------------------------------------------------------------------*/
/* graphics.render_thread (RSX_RENDER_THREAD): on (mode 2, the default), off,
 * or a mode's number. The batch's recording, its submission and the presents
 * on a thread of their own, "rsx render", while the frame clock thread walks
 * the FIFO and captures the draws (their records and uploads).
 * - The walk publishes the records it is done with, all but the last (the next
 *   batch of indexes may still extend it, record-ahead's rule), at each draw
 *   and after each drain pass; the render thread records them as record-ahead
 *   did between drain passes.
 * - A present, or a render at a display clear, is handed to the render thread
 *   while the walk waits. The batch's state (records, segments, upload offsets,
 *   the parity) is not doubled: the walk goes on once render_frame has reset
 *   it, while the render thread submits and presents (mode 1). Mode 2, a
 *   render at a clear and a capture (which reads the records after the reset)
 *   wait for all of it.
 * - Texture uploads read guest memory in rf_prepass, on the render thread: in
 *   mode 2 no later than the batch's end, the walk waiting there, as without
 *   record-ahead.
 * - What else both touch: the vertex programs' entries (s_vs_lock), the base
 *   pipeline's build (rt_excl_begin), and the walker's table of ZPASS reports
 *   (a lock in cellGcmSys.c). The ZPASS and main-memory surface polls are the
 *   render thread's.
 * It needs record-ahead (D3D12_RECORD_AHEAD=1, the default).
 * RSX_RENDER_THREAD=3, a diagnostic: the walk records ahead itself (under the
 * render thread's lock) and only the batch's end moves.
 *
 * Status (2026-11-05, God of War: Ascension's gate): the walk thread falls
 * from ~73% of a core to ~41%, the render thread takes ~34%, and the frame
 * rate falls ~3% (20.87 -> 20.18 flips/s, mode 2, two runs each): the SPU
 * workers set the frame, not this thread.
 * 2026-11-08, mode 2 the default: the walk had come to bind at the gate with
 * shadows on (98% of a core); it falls to ~50%, the render thread takes
 * ~60%, and the gate goes 30.66/28.57 -> 33.65/32.65 flips/s (ABBA), the
 * fight 32.20 -> 35.20 (one run each). The fight with shadows on is the PPU
 * main thread's (95%): 27.82/30.01 -> 27.23/28.90, within its spread. Under
 * frame generation's cap (FSR 3.1, VSync, 30 flips/s at 60 Hz) the fight
 * is the same, 29.79 against 29.74 (three runs each).
 * Mode 1 put the GPU in a hang (DXGI_ERROR_DEVICE_HUNG) in two of four runs,
 * after ~2,000 and ~4,000 frames. A candidate, not tried: from the reset the
 * walk writes the next batch into the upload halves (vp_parity) of the batch
 * before, which the GPU may still be reading -- move_to_next_frame waits for
 * that batch only after the present -- and the raw-fetch draws read through
 * root views, unchecked. Modes 2 and 3 did not hang, in 25 and two runs. */
static HANDLE s_rt_thread, s_rt_work_ev, s_rt_done_ev;
static SRWLOCK s_rt_excl = SRWLOCK_INIT;   /* the render thread's while it works */
static volatile LONG s_rt_pub;             /* records the walk is done with */
static volatile LONG s_rt_req;             /* renders handed over (the walk) */
static volatile LONG s_rt_rel;             /* ... the walk may go on from (the render thread) */
static volatile LONG s_rt_done;            /* ... done, their presents included (the render thread) */
static volatile LONG s_rt_sleeping;        /* the render thread waits, or is about to */
static volatile LONG s_rt_quit;
static int s_rt_req_present, s_rt_req_full;   /* the request's: written before s_rt_req moves */
static u32 s_rt_woke;                      /* the walk: published when it last woke the render thread */
static LONG s_rt_cur;                      /* the render thread: the request render_frame runs for */

static LONG rt_get(volatile LONG* v) { return InterlockedCompareExchange(v, 0, 0); }

/* A capture reads the records after render_frame has reset the batch. */
static int rt_cap_on(void)
{
    static int v = -1;
    if (v < 0) v = (getenv("RSX_CAP") || getenv("RSX_CAP_SHOT") || getenv("RSX_CAP_YELLOW") ||
                    getenv("RSX_CAP_DIP")) ? 1 : 0;
    return v;
}

/* ZPASS counts or main-memory surfaces waiting for the GPU. */
static int rt_polls_pending(void)
{
    for (u32 fi = 0; fi < FRAME_COUNT; fi++)
        if ((s_zq_resolved[fi] && s_zq_fence[fi]) || (s_mwb_n[fi] && s_mwb_fence[fi])) return 1;
    return 0;
}

static DWORD WINAPI rt_main(LPVOID arg)
{
    (void)arg;
    ps3_thread_name("rsx render");
    LONG done = 0;
    u32 stall = 0;   /* records published when a pass could not go on (a pipeline being built) */
    for (;;) {
        InterlockedExchange(&s_rt_sleeping, 1);
        if (!rt_get(&s_rt_quit) && rt_get(&s_rt_req) == done) {
            const u32 pub = (u32)rt_get(&s_rt_pub);
            if (pub == stall || pub < (s_rf.open ? s_rf.done : 0) + RF_AHEAD_MIN)
                WaitForSingleObject(s_rt_work_ev, (stall || rt_polls_pending()) ? 1 : INFINITE);
        }
        InterlockedExchange(&s_rt_sleeping, 0);
        if (rt_get(&s_rt_quit)) break;
        AcquireSRWLockExclusive(&s_rt_excl);
        zq_poll();   /* ZPASS counts and main-memory surfaces of a batch the GPU has finished */
        mwb_poll();
        const LONG req = rt_get(&s_rt_req);
        if (req != done) {
            s_rt_cur = req;
            s_present_this_frame = s_rt_req_present;
            render_frame();
            s_present_this_frame = 1;
            ReleaseSRWLockExclusive(&s_rt_excl);
            done = req;
            stall = 0;
            InterlockedExchange(&s_rt_rel, req);
            InterlockedExchange(&s_rt_done, req);
            SetEvent(s_rt_done_ev);
            continue;
        }
        const u32 pub = (u32)rt_get(&s_rt_pub);
        stall = 0;
        if (pub >= (s_rf.open ? s_rf.done : 0) + RF_AHEAD_MIN) {
            const int o0 = s_rf.open;
            const u32 d0 = s_rf.done, p0 = s_rf.pre_done;
            rf_ahead(pub, 1);
            if (s_rf.open == o0 && s_rf.done == d0 && s_rf.pre_done == p0) stall = pub;
        }
        ReleaseSRWLockExclusive(&s_rt_excl);
    }
    return 0;
}

/* The walk, waiting on the render thread. The window is this thread's: it
 * takes all of the window's messages meanwhile and waits for input too, as a
 * window's thread should while another one presents. A thread blocked in a
 * wait for sent messages alone, its queue unread for 5 s (a PeekMessage on an
 * empty queue does not count), is a hung one to Windows: over the borderless
 * window the taskbar and the pointer came up every ~5.5 s, the window ghosted.
 * A WM_QUIT goes back for the main loop's pump, and from then on only sent
 * messages are taken here. */
static int s_rt_wm_quit;
static void rt_pump(void)
{
    MSG m;
    if (s_rt_wm_quit) { PeekMessageA(&m, NULL, 0, 0, PM_NOREMOVE | PM_QS_SENDMESSAGE); return; }
    while (PeekMessageA(&m, NULL, 0, 0, PM_REMOVE)) {
        if (m.message == WM_QUIT) { PostQuitMessage((int)m.wParam); s_rt_wm_quit = 1; return; }
        TranslateMessage(&m);
        DispatchMessageA(&m);
    }
}
static DWORD rt_wake_mask(void) { return s_rt_wm_quit ? QS_SENDMESSAGE : QS_ALLINPUT; }

/* The walk: wait until *v reaches seq. */
static void rt_wait(volatile LONG* v, LONG seq)
{
    while ((LONG)(rt_get(v) - seq) < 0) {
        rt_pump();
        MsgWaitForMultipleObjects(1, &s_rt_done_ev, FALSE, 50, rt_wake_mask());
    }
}

/* The walk: the batch's records are done (all but the last). */
static void rt_publish(void)
{
    if (s_rt_mode == 3) return;       /* the walk records ahead itself */
    const u32 n = s_d3d.draw_count < MAX_DRAWS ? s_d3d.draw_count : MAX_DRAWS;
    if (n < 2) return;
    const u32 upto = n - 1;            /* the last record may still be extended */
    if ((LONG)upto <= s_rt_pub) return;
    InterlockedExchange(&s_rt_pub, (LONG)upto);
    if (upto >= s_rt_woke + RF_AHEAD_MIN && rt_get(&s_rt_sleeping)) {
        s_rt_woke = upto;
        SetEvent(s_rt_work_ev);
    }
}

/* The walk: the render thread renders the batch (present: s_present_this_frame
 * for it). Back once render_frame has reset the batch; full: once all of it is
 * done. */
static void rt_render(int present, int full)
{
    rt_wait(&s_rt_done, rt_get(&s_rt_req));   /* the frame before, its present included */
    InterlockedExchange(&s_rt_pub, 0);        /* the records are the render's until the reset */
    s_rt_woke = 0;
    s_rt_req_present = present;
    s_rt_req_full = full || s_rt_mode == 2 || rt_cap_on();
    const LONG req = InterlockedIncrement(&s_rt_req);
    SetEvent(s_rt_work_ev);
    rt_wait(s_rt_req_full ? &s_rt_done : &s_rt_rel, req);
}

/* The render thread, in render_frame: the batch is reset. */
static void rt_released(void)
{
    if (!s_rt_cur || s_rt_req_full) return;
    InterlockedExchange(&s_rt_rel, s_rt_cur);
    SetEvent(s_rt_done_ev);
}

/* The walk: what the render thread may be using, kept from it meanwhile (the
 * base pipeline's build). */
static void rt_excl_begin(void)
{
    if (s_rt_mode <= 0) return;
    while (!TryAcquireSRWLockExclusive(&s_rt_excl)) {
        rt_pump();
        MsgWaitForMultipleObjects(0, NULL, FALSE, 1, rt_wake_mask());
    }
}
static void rt_excl_end(void)
{
    if (s_rt_mode > 0) ReleaseSRWLockExclusive(&s_rt_excl);
}

/* graphics.render_thread: on (mode 2), off, or a mode's number. */
static int rt_mode_wanted(void)
{
    const char* s = ps3_settings_str("graphics", "render_thread", "RSX_RENDER_THREAD", "on");
    if (!s || !*s) return 2;
    if (ps3_settings_ieq(s, "off") || ps3_settings_ieq(s, "no") || ps3_settings_ieq(s, "false")) return 0;
    if (ps3_settings_ieq(s, "on") || ps3_settings_ieq(s, "yes") || ps3_settings_ieq(s, "true")) return 2;
    return atoi(s);
}

static void rt_start(void)
{
    const int m = rt_mode_wanted();
    if (m <= 0) return;
    if (!rf_ahead_on()) {
        fprintf(stderr, "[D3D12] the render thread needs record-ahead (D3D12_RECORD_AHEAD=1, no debug filter): off%c", 10);
        return;
    }
    s_rt_work_ev = CreateEventA(NULL, FALSE, FALSE, NULL);
    s_rt_done_ev = CreateEventA(NULL, FALSE, FALSE, NULL);
    if (s_rt_work_ev && s_rt_done_ev)
        s_rt_thread = CreateThread(NULL, 0, rt_main, NULL, CREATE_SUSPENDED, NULL);
    if (!s_rt_thread) {
        fprintf(stderr, "[D3D12] the render thread did not start: off%c", 10);
        return;
    }
    SetThreadPriority(s_rt_thread, GetThreadPriority(GetCurrentThread()));   /* the frame clock's */
    s_rt_mode = m >= 3 ? 3 : m >= 2 ? 2 : 1;
    ResumeThread(s_rt_thread);
    fprintf(stderr, "[D3D12] render thread on (mode %d): the walk goes on %s%c", s_rt_mode,
            s_rt_mode == 2 ? "after each present" : "from each batch's reset", 10);
}

static void rt_stop(void)
{
    if (!s_rt_thread) return;
    if (s_rt_req) rt_wait(&s_rt_done, rt_get(&s_rt_req));
    InterlockedExchange(&s_rt_quit, 1);
    SetEvent(s_rt_work_ev);
    WaitForSingleObject(s_rt_thread, 5000);
    CloseHandle(s_rt_thread);
    s_rt_thread = NULL;
    s_rt_mode = 0;
}

void rsx_d3d12_backend_record_ahead(void)
{
    if (s_rt_mode == 3) {             /* RSX_RENDER_THREAD=3: the walk records ahead, the render thread renders */
        if (!s_d3d.initialized) return;
        rt_excl_begin();
        zq_poll(); mwb_poll();
        const u32 n3 = s_d3d.draw_count < MAX_DRAWS ? s_d3d.draw_count : MAX_DRAWS;
        if (n3 >= 2 && rf_ahead_on()) rf_ahead(n3 - 1, 0);
        rt_excl_end();
        return;
    }
    if (s_rt_mode > 0) { rt_publish(); return; }   /* RSX_RENDER_THREAD: it records and polls */
    if (s_d3d.initialized) { zq_poll(); mwb_poll(); }   /* ZPASS counts and main-memory surfaces of a batch the GPU has finished */
    if (!s_d3d.initialized || !rf_ahead_on()) return;
    const u32 n = s_d3d.draw_count < MAX_DRAWS ? s_d3d.draw_count : MAX_DRAWS;
    if (n < 2) return;
    rf_ahead(n - 1, 0);               /* the last record may still be extended */
}

void rsx_d3d12_backend_present(void)
{
    /* RSX_RENDER_THREAD: the frame before is done, its composite source read */
    if (s_rt_mode > 0) rt_wait(&s_rt_done, rt_get(&s_rt_req));
    if (blink_dbg())
        printf("[PRESENT] draws=%u clears_since_last=%u clear_presents=%u\n",
               s_d3d.draw_count, s_dbg_clears_since_present, s_clear_presents);
    s_dbg_clears_since_present = 0;

    /* A clear boundary may have already rendered this exact guest flip. Skip
     * only that duplicate. A lifetime `s_clear_presents > 0` gate permanently
     * disabled flip presentation when a later title phase stopped issuing
     * display clears, so records accumulated to MAX_DRAWS and were truncated. */
    {
        extern unsigned cellGcm_flip_request_count(void);
        if (rsx_present_flip_already_rendered(
                s_clear_presents, s_last_clear_presented_flip,
                cellGcm_flip_request_count()))
            return;
    }

    /* Same display gate as d3d12_present: a batch of offscreen pass work only
     * (render-to-texture) keeps accumulating until its composite arrives.
     * Empty batches present only until the first real frame -- after that an
     * empty present is a flip/drain race and wipes the screen for a frame
     * (wave: black flashes and layout flicker between frames). */
    static int s_seen_content = 0;
    int has_display = (s_d3d.draw_count == 0 && !s_seen_content);
    for (u32 _i = 0; _i < s_d3d.draw_count && _i < MAX_DRAWS; _i++)
        if (!s_d3d.draws[_i].is_clear && s_d3d.draws[_i].rt_off == 0) {
            has_display = 1;
            break;
        }
    /* The guest-framebuffer present needs the pass to run at all. */
    { static int gfb = -1;
      if (gfb < 0) { const char* e = getenv("GCM_GUEST_FB"); gfb = e ? 1 : 0; }
      if (gfb) has_display = 1; }
    if (display_rt_on()) {
        extern u32 cellGcmGetCurrentDisplayBufferId(void);
        extern u32 cellGcm_display_buffer_offset(u32 id);
        const u32 id = cellGcmGetCurrentDisplayBufferId();
        const u32 o = cellGcm_display_buffer_offset(id);
        if (o != 0xFFFFFFFFu) s_composite_src = DISP_RT_TAG | o;
        /* DISP_RT_DBG=<n>: per present (first n), the buffer shown and what
         * this batch drew and cleared in each display buffer. */
        { static int cap = -1, n = 0;
          if (cap < 0) { const char* e = getenv("DISP_RT_DBG"); cap = e ? atoi(e) : 0; }
          if (cap && n < cap) { n++;
              u32 dr3[3] = {0, 0, 0}, cl3[3] = {0, 0, 0};
              for (u32 _i = 0; _i < s_d3d.draw_count && _i < MAX_DRAWS; _i++) {
                  const D3D12DrawRecord* r = &s_d3d.draws[_i];
                  if (!(r->rt_off & DISP_RT_TAG) || r->snap_ea) continue;
                  for (u32 b = 0; b < 3; b++)
                      if (r->rt_off == (DISP_RT_TAG | cellGcm_display_buffer_offset(b))) {
                          if (r->is_clear) cl3[b]++; else dr3[b]++;
                      }
              }
              fprintf(stderr, "[disp-rt] present id=%u off=0x%X rt=%d | draws %u/%u/%u clears %u/%u/%u records=%u%c",
                      id, o, off_rt_find(DISP_RT_TAG | o), dr3[0], dr3[1], dr3[2],
                      cl3[0], cl3[1], cl3[2], s_d3d.draw_count, 10); } }
    }
    if (s_composite_src) has_display = 1;
    if (has_display && s_d3d.draw_count > 0) s_seen_content = 1;

    /* VP_SUBMIT=<N>: has_display gates render_frame() entirely, so a batch whose
     * records all target an OFFSCREEN rt (rt_off != 0) presents without ever
     * running the draw pass -- the backbuffer then shows only the clear, which
     * reads as "nothing rasterizes" from every downstream check. */
    { static int cap = -1, n = 0;
      if (cap < 0) { const char* e = getenv("VP_SUBMIT"); cap = e ? atoi(e) : 0; }
      if (cap && n < cap && s_d3d.draw_count) { n++;
        u32 onscreen = 0, offscreen = 0, clears = 0;
        for (u32 _i = 0; _i < s_d3d.draw_count && _i < MAX_DRAWS; _i++) {
            if (s_d3d.draws[_i].is_clear) { clears++; continue; }
            if (s_d3d.draws[_i].rt_off) offscreen++; else onscreen++;
        }
        { /* Which surfaces the batch's draws actually target -- "offscreen=57"
           * does not say whether they all go to one RT or scatter. */
          u32 offs[8]; u32 cnt[8]; int nd = 0;
          for (u32 _i = 0; _i < s_d3d.draw_count && _i < MAX_DRAWS; _i++) {
              if (s_d3d.draws[_i].is_clear) continue;
              u32 o = s_d3d.draws[_i].rt_off; int f = -1;
              for (int _j = 0; _j < nd; _j++) if (offs[_j] == o) { f = _j; break; }
              if (f < 0) { if (nd < 8) { offs[nd] = o; cnt[nd] = 1; nd++; } }
              else cnt[f]++;
          }
          { u32 vp = 0, novp = 0;
            for (u32 _i = 0; _i < s_d3d.draw_count && _i < MAX_DRAWS; _i++) {
                if (s_d3d.draws[_i].is_clear) continue;
                if (s_d3d.draws[_i].is_vp) vp++; else novp++;
            }
            /* The execution loop skips anything without is_vp, so a batch full
             * of !is_vp records is counted, gated and then never drawn. */
            fprintf(stderr, "[PRESENTGATE] is_vp=%u not_vp=%u%c", vp, novp, 10); }
          fprintf(stderr, "[PRESENTGATE] targets:");
          for (int _j = 0; _j < nd; _j++)
              fprintf(stderr, " 0x%08X x%u", offs[_j], cnt[_j]);
          fprintf(stderr, "%c", 10); }
        fprintf(stderr, "[PRESENTGATE] records=%u onscreen=%u offscreen=%u clears=%u"
                        " has_display=%d seen_content=%d -> render_frame=%s\n",
                s_d3d.draw_count, onscreen, offscreen, clears, has_display,
                s_seen_content, (s_d3d.initialized && has_display) ? "YES" : "SKIPPED"); } }

    /* Execute the batch whenever it has draws. Gating the whole call on
     * has_display meant a render-to-texture pass -- every draw targeting an
     * offscreen surface -- was DISCARDED rather than deferred, so the texture
     * it produces was never written and whatever sampled it later read an
     * empty resource. Only the Present needs onscreen content. */
    if (s_d3d.initialized && (has_display || s_d3d.draw_count > 0)) {
        { extern void rsx_reset_upload_claims(void);
          static int remap = -1;
          if (remap < 0) { const char* e = getenv("TEX_REMAP"); remap = e ? atoi(e) : 0; }
          if (remap) rsx_reset_upload_claims(); }
        if (s_rt_mode > 0) rt_render(has_display, 0);   /* RSX_RENDER_THREAD */
        else {
            s_present_this_frame = has_display;
            render_frame();
            s_present_this_frame = 1;
        }
    }
}

#else /* !_WIN32 */

#include <ps3emu/ps3types.h>   /* u32 (header includes above are inside the _WIN32 guard) */
#include <stdio.h>

/* Stub for non-Windows — D3D12 is Windows-only */
int rsx_d3d12_backend_init(u32 w, u32 h, const char* t)
{
    (void)w; (void)h; (void)t;
    printf("[D3D12] Not available on this platform (use Vulkan backend)\n");
    return -1;
}
void rsx_d3d12_backend_shutdown(void) {}
int rsx_d3d12_backend_pump_messages(void) { return 0; }
void rsx_d3d12_backend_present(void) {}
int rsx_d3d12_backend_generating(void) { return 0; }
int rsx_d3d12_backend_gpu_stuck(void) { return 0; }
double rsx_d3d12_backend_refresh_hz(void) { return 0.0; }
void rsx_d3d12_backend_record_ahead(void) {}
void rsx_d3d12_register_vertex_ring(u32 io_offset, u32 size) { (void)io_offset; (void)size; }
void rsx_d3d12_vertex_pass_set(const char* hlsl, u32 stride, u32 max_record,
                               void (*cpu)(const void* rec, const u8* src, u8* dst, u32 n))
{ (void)hlsl; (void)stride; (void)max_record; (void)cpu; }
void rsx_d3d12_vertex_pass_publish(u32 io_offset, u32 n, const void* rec, u32 bytes, u32 applied,
                                   const void* tag16, const void* skin, u32 skin_bytes)
{ (void)io_offset; (void)n; (void)rec; (void)bytes; (void)applied; (void)tag16; (void)skin; (void)skin_bytes; }

#endif /* _WIN32 */
