/*
 * ps3recomp - SPU job capture and replay
 *
 * A policy module (SPURS job manager) runs its jobs one after another on a
 * worker; a job is the code it calls above LS 0x4000. To measure what a
 * change to the lifted code does to one job, without a level run that only
 * shows the frame rate, this records chosen jobs and runs them again alone.
 *
 * Capture, SPU_JOB_CAPTURE=<dir>:
 *   SPU_JOB_CAPTURE_WIDS=2,12,15  workloads to take jobs from (default 2,7,12,15)
 *   SPU_JOB_CAPTURE_N=40          jobs per workload (default 40)
 *   SPU_JOB_CAPTURE_EVERY=20      take every n-th job of a workload (default 20),
 *                                 to spread the captures over many frames
 *   SPU_JOB_CAPTURE_T0=240        start this many seconds after the first job (default 0)
 *   SPU_JOB_CAPTURE_LINK=0x3368   the job manager's return address after a job
 * A job starts where the policy module branches above LS 0x4000 with the
 * return address of its job call as the link (SPU_JOB_CAPTURE_LINK, default
 * 0x3368: the Wws job manager's). The module's own helpers live above 0x4000
 * too, and job code runs under the module's image id, so neither the target
 * nor the image tells a job call apart. It ends when the drain with that link
 * returns. The file holds the context and local store at the
 * start, then in order every DMA GET and PUT with their bytes, every atomic
 * with its line and status, every channel read and count, every outbound
 * mailbox write; then the registers and local store at the end. A job that
 * stops or halts is not kept: what follows depends on more than the record
 * holds. Interrupts are kept (see spu_jobcap_abandon).
 *
 * Both need a build with PS3_SPU_JOBCAP (spu_jobcap.h).
 *
 * Replay, SPU_JOB_REPLAY=<dir> (the port starts it instead of the game):
 *   SPU_JOB_REPLAY_REPS=10        runs per job (default 10)
 *   SPU_JOB_REPLAY_ONLY=_w15_     only files whose name contains this
 * Each job runs from its saved context with every input served from the
 * record, in order, and every output compared with it: PUT bytes, outbound
 * mailbox words, and at the end the local store and the registers. A request
 * that differs from the record (another address or size) is a divergence; an
 * output word that differs is read as a single and counted as two SPU zeros
 * (a sign apart), within 1, 16 or 256 ulps, or other. It prints the time per job
 * (fastest run, and the median) and each workload's totals, and writes
 * replay.csv next to the captures.
 */
#include "spu_jobcap.h"
#include "spu_context.h"
#include "../../include/ps3emu/log_first.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <setjmp.h>
#if defined(__x86_64__) || defined(_M_X64)
#include <xmmintrin.h>
#endif
#ifdef _WIN32
#include <windows.h>
#include <malloc.h>
#else
#include <dirent.h>
#include <sys/stat.h>
#include <time.h>
#endif

#define JC_INC(x) __atomic_add_fetch(&(x), 1, __ATOMIC_SEQ_CST)
#define JC_DEC(x) __atomic_sub_fetch(&(x), 1, __ATOMIC_SEQ_CST)

SPU_THREAD_LOCAL int t_spu_jobcap;
SPU_THREAD_LOCAL int t_spu_pm_img = -1;
int g_spu_jobcap_armed;

extern void spu_overlay_note_get(spu_context* ctx, uint32_t ea, const uint8_t* ls, uint32_t size);
extern void spu_mfc_release(spu_context* ctx);
extern void spu_jmp_no_unwind(void* env);
extern int g_spu_float_rz;                  /* spu_helpers.h: SPU code's float forms chosen at run time */
#define JC_MXCSR_RZFZ 0xE040u               /* SPU_MXCSR_RZFZ: round toward zero, FTZ, DAZ */

enum { JC_GET = 1, JC_PUT = 2, JC_SDCRZ = 3, JC_ATOMIC = 4, JC_RDCH = 5, JC_RCHCNT = 6, JC_WROUT = 7, JC_KLINE = 8 };
static const char* const jc_names[] = { "?", "GET", "PUT", "SDCRZ", "atomic", "rdch", "rchcnt", "out mailbox", "kernel line" };

typedef struct { uint32_t type, a, b, c, len; } jc_ev;

typedef struct {
    char     magic[8];          /* "SPUJCAP1" */
    uint32_t ctx_size;          /* sizeof(spu_context) of the build that wrote it */
    uint32_t wid, link, entry;
    uint32_t n_ev, ev_len;
    int32_t  pm_img;
    uint32_t reserved[5];
} jc_hdr;

#define JC_MAGIC "SPUJCAP1"

static uint32_t be32(const uint8_t* p) { return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3]; }

static uint64_t jc_now_us(void)
{
#ifdef _WIN32
    static LARGE_INTEGER f;
    LARGE_INTEGER c;
    if (!f.QuadPart) QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&c);
    return (uint64_t)(c.QuadPart / f.QuadPart) * 1000000ull + (uint64_t)(c.QuadPart % f.QuadPart) * 1000000ull / (uint64_t)f.QuadPart;
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000ull + (uint64_t)ts.tv_nsec / 1000u;
#endif
}

/* LS bytes with the local store's wrap-around. */
static void ls_read(const spu_context* ctx, uint32_t lsa, uint8_t* out, uint32_t n)
{
    for (uint32_t i = 0; i < n; i++) out[i] = ctx->ls[(lsa + i) & SPU_LS_MASK];
}
static void ls_write(spu_context* ctx, uint32_t lsa, const uint8_t* in, uint32_t n)
{
    for (uint32_t i = 0; i < n; i++) ctx->ls[(lsa + i) & SPU_LS_MASK] = in[i];
}

/* ------------------------------------------------------------------ capture */

static char s_dir[512];
static uint32_t s_wids = (1u << 2) | (1u << 7) | (1u << 12) | (1u << 15);
static unsigned s_n = 40, s_every = 20;
static uint32_t s_link = 0x3368;   /* the job manager's return address after it calls a job */
static uint64_t s_t0_us, s_first_us;
static volatile long s_seen[32], s_taken[32], s_written, s_dropped_stop, s_dropped_irq;

typedef struct {
    const uint8_t* ev; size_t len, pos;
    uint32_t idx, n_ev;
    uint32_t diverged, put_bytes_differ, out_differ;
    /* the PUT/atomic words that differ, as singles: both an SPU zero (sign or
     * fraction only), same sign within 1 / 16 / 256 ulps, anything else */
    uint32_t w_zero, w_ulp1, w_ulp16, w_ulp256, w_other, max_ulp;
    char first[200];
    spu_context* ctx;
    jmp_buf jb;                 /* back to the replay driver at the job's end */
} jc_rep;
static SPU_THREAD_LOCAL jc_rep* t_rep;

typedef struct {
    uint8_t* buf; size_t len, cap;
    uint32_t n_ev, wid, link, entry;
    spu_context* ctx;
    uint8_t* snap;
} jc_cap;
static SPU_THREAD_LOCAL jc_cap* t_cap;
static void jc_finish(spu_context* ctx);

void spu_jobcap_init(void)
{
    const char* d = getenv("SPU_JOB_CAPTURE");
    if (!d || !*d) return;
#ifndef PS3_SPU_JOBCAP
    fprintf(stderr, "[jobcap] SPU_JOB_CAPTURE needs a build with PS3_SPU_JOBCAP (gowa.py build --jobcap); nothing captured\n");
    return;
#endif
    snprintf(s_dir, sizeof s_dir, "%s", d);
#ifdef _WIN32
    CreateDirectoryA(s_dir, NULL);
#else
    mkdir(s_dir, 0755);
#endif
    const char* w = getenv("SPU_JOB_CAPTURE_WIDS");
    if (w && *w) {
        s_wids = 0;
        for (const char* p = w; *p; ) {
            char* e;
            long v = strtol(p, &e, 0);
            if (e == p) { p++; continue; }
            if (v >= 0 && v < 32) s_wids |= 1u << v;
            p = e;
        }
    }
    const char* n = getenv("SPU_JOB_CAPTURE_N");
    if (n && atoi(n) > 0) s_n = (unsigned)atoi(n);
    const char* ev = getenv("SPU_JOB_CAPTURE_EVERY");
    if (ev && atoi(ev) > 0) s_every = (unsigned)atoi(ev);
    const char* lk = getenv("SPU_JOB_CAPTURE_LINK");
    if (lk && *lk) s_link = (uint32_t)strtoul(lk, NULL, 0) & SPU_LS_MASK;
    const char* t0 = getenv("SPU_JOB_CAPTURE_T0");
    s_t0_us = t0 ? (uint64_t)(atof(t0) * 1e6) : 0;
    fprintf(stderr, "[jobcap] capturing into %s: workloads 0x%08X, %u each, every %u-th, from %.0f s, job calls returning to 0x%05X\n",
            s_dir, s_wids, s_n, s_every, s_t0_us / 1e6, s_link);
    g_spu_jobcap_armed = 1;
}

static void cap_put(jc_cap* c, uint32_t type, uint32_t a, uint32_t b, uint32_t cc, const void* data, uint32_t len)
{
    const size_t need = c->len + sizeof(jc_ev) + len;
    if (need > c->cap) {
        size_t cap = c->cap ? c->cap : (1u << 20);
        while (cap < need) cap *= 2;
        uint8_t* nb = (uint8_t*)realloc(c->buf, cap);
        if (!nb) { spu_jobcap_abandon("no memory"); return; }
        c->buf = nb; c->cap = cap;
    }
    jc_ev e = { type, a, b, cc, len };
    memcpy(c->buf + c->len, &e, sizeof e);
    if (len) memcpy(c->buf + c->len + sizeof e, data, len);
    c->len = need;
    c->n_ev++;
}

void spu_jobcap_maybe_begin(spu_context* ctx)
{
    /* The job's end: control comes back to the job manager by a dispatch to
     * its return address (the Wws job modules return through a continuation
     * that runs in a nested drain, so the drain of the call does not see it). */
    if (t_spu_jobcap) {
        if ((ctx->pc & SPU_LS_MASK) == s_link) {
            if (t_spu_jobcap == 1 && t_cap && t_cap->ctx == ctx)
                jc_finish(ctx);
            else if (t_spu_jobcap == 2 && t_rep && t_rep->ctx == ctx)
                longjmp(t_rep->jb, 1);
        }
        return;
    }
    /* SPU_JOB_CAPTURE_DEBUG=1: the first branches above 0x4000 a policy context takes. */
    { static int dbg = -1; static volatile long n;
      if (dbg < 0) dbg = getenv("SPU_JOB_CAPTURE_DEBUG") ? 1 : 0;
      if (dbg && ctx->policy_mode && (ctx->pc & SPU_LS_MASK) >= 0x4000 && JC_INC(n) <= 40)
          fprintf(stderr, "[jobcap-dbg] pc=0x%05X r0=0x%05X img=%d pm_img=%d wid=%u depth=%u state=%d\n",
                  ctx->pc & SPU_LS_MASK, ctx->gpr[0]._u32[0] & SPU_LS_MASK, ctx->image_id, t_spu_pm_img,
                  be32(ctx->ls + 0x1DC), ctx->host_depth, t_spu_jobcap); }
    if (t_spu_jobcap || !ctx->policy_mode) return;
    const uint32_t pc = ctx->pc & SPU_LS_MASK, link = ctx->gpr[0]._u32[0] & SPU_LS_MASK;
    if (pc < 0x4000 || link != s_link) return;
    const uint32_t wid = be32(ctx->ls + 0x1DC);
    if (wid >= 32 || !(s_wids & (1u << wid))) return;
    const uint64_t now = jc_now_us();
    if (!s_first_us) s_first_us = now;
    if (now - s_first_us < s_t0_us) return;
    if (s_taken[wid] >= (long)s_n) return;
    if ((unsigned long)JC_INC(s_seen[wid]) % s_every) return;
    if (JC_INC(s_taken[wid]) > (long)s_n) return;
    jc_cap* c = t_cap;
    if (!c) {
        c = (jc_cap*)calloc(1, sizeof *c);
        if (!c || !(c->snap = (uint8_t*)malloc(sizeof(spu_context)))) { free(c); return; }
        t_cap = c;
    }
    c->len = 0; c->n_ev = 0;
    c->wid = wid; c->link = link; c->entry = pc; c->ctx = ctx;
    memcpy(c->snap, ctx, sizeof(spu_context));
    t_spu_jobcap = 1;
}

void spu_jobcap_abandon(const char* why)
{
    if (t_spu_jobcap != 1) return;
    /* The job manager runs its jobs with interrupts on, and the events behind
     * them are the job's own (tag-status updates it asks for): a replay takes
     * the same interrupts at the same places. An event from outside (a lost
     * reservation) is not in the record; the replay's output check finds it. */
    if (!strcmp(why, "interrupt")) { JC_INC(s_dropped_irq); return; }
    t_spu_jobcap = 0;
    if (t_cap) {
        JC_DEC(s_taken[t_cap->wid]);   /* take another one instead */
        const long n = JC_INC(s_dropped_stop);
        if (n <= 4)
            fprintf(stderr, "[jobcap] w%u job at 0x%05X dropped: %s at pc 0x%05X\n",
                    t_cap->wid, t_cap->entry, why, t_cap->ctx ? t_cap->ctx->pc & SPU_LS_MASK : 0);
    }
}

static void jc_finish(spu_context* ctx)
{
    jc_cap* c = t_cap;
    t_spu_jobcap = 0;
    const long k = JC_INC(s_written);
    char path[640];
    snprintf(path, sizeof path, "%s/job_w%02u_%04ld.bin", s_dir, c->wid, k);
    FILE* f = fopen(path, "wb");
    if (!f) return;
    jc_hdr h;
    memset(&h, 0, sizeof h);
    memcpy(h.magic, JC_MAGIC, 8);
    h.ctx_size = (uint32_t)sizeof(spu_context);
    h.wid = c->wid; h.link = c->link; h.entry = c->entry;
    h.n_ev = c->n_ev; h.ev_len = (uint32_t)c->len; h.pm_img = t_spu_pm_img;
    fwrite(&h, sizeof h, 1, f);
    fwrite(c->snap, sizeof(spu_context), 1, f);
    if (c->len) fwrite(c->buf, 1, c->len, f);
    fwrite(ctx->gpr, sizeof ctx->gpr, 1, f);
    fwrite(ctx->ls, 1, SPU_LS_SIZE, f);
    fclose(f);
    if (k <= 8 || (k % 50) == 0)
        fprintf(stderr, "[jobcap] %ld jobs written (w%u: %u events, %zu KB of them); dropped %ld (stop/halt); %ld interrupts taken in captured jobs\n",
                k, c->wid, c->n_ev, c->len >> 10, s_dropped_stop, s_dropped_irq);
}

void spu_jobcap_drain_done(spu_context* ctx, uint32_t return_pc)
{
    jc_cap* c = t_cap;
    if (t_spu_jobcap != 1 || !c || c->ctx != ctx || (return_pc & SPU_LS_MASK) != c->link) return;
    jc_finish(ctx);
}

/* ------------------------------------------------------------------- replay */


static const jc_ev* rep_next(jc_rep* r, uint32_t type, uint32_t a, uint32_t b, uint32_t c, uint32_t len, const uint8_t** data)
{
    if (r->pos + sizeof(jc_ev) > r->len) {
        if (!r->diverged++)
            snprintf(r->first, sizeof r->first, "event %u: %s past the end of the record", r->idx, jc_names[type]);
        return NULL;
    }
    static SPU_THREAD_LOCAL jc_ev e;
    memcpy(&e, r->ev + r->pos, sizeof e);
    if (e.type != type || e.a != a || e.b != b || (type != JC_ATOMIC && e.c != c) || (len && e.len != len)) {
        if (!r->diverged++)
            snprintf(r->first, sizeof r->first, "event %u: %s a=0x%X b=0x%X c=0x%X len=%u, the record has %s a=0x%X b=0x%X c=0x%X len=%u",
                     r->idx, jc_names[type], a, b, c, len, jc_names[e.type < 9 ? e.type : 0], e.a, e.b, e.c, e.len);
        return NULL;
    }
    *data = r->ev + r->pos + sizeof e;
    r->pos += sizeof e + e.len;
    r->idx++;
    return &e;
}

extern uint8_t* vm_base;
void spu_jobcap_kernel_line(spu_context* ctx, uint32_t ea)
{
    if (t_spu_jobcap == 2) {
        jc_rep* r = t_rep;
        const uint8_t* d;
        if (r && rep_next(r, JC_KLINE, 0x100, ea, 0, 0x80, &d))
            memcpy(ctx->ls + 0x100, d, 0x80);
        return;
    }
    memcpy(ctx->ls + 0x100, vm_base + ea, 0x80);
    jc_cap* c = t_cap;
    if (t_spu_jobcap == 1 && c && c->ctx == ctx)
        cap_put(c, JC_KLINE, 0x100, ea, 0, ctx->ls + 0x100, 0x80);
}

void spu_jobcap_restart(spu_context* ctx)
{
    if (t_spu_jobcap == 2 && t_rep && t_rep->ctx == ctx)
        longjmp(t_rep->jb, 2);
}

static uint32_t jc_class(uint32_t cmd)
{
    const uint32_t c = cmd & 0xFFFFu;
    if (c == 0x64u) return JC_SDCRZ;                       /* MFC_SDCRZ_CMD */
    const uint32_t hi = c & 0xF0u;
    if (hi == 0x40u || hi == 0x50u) return JC_GET;
    return JC_PUT;
}

/* Output bytes against the record: every byte that differs, and each 4-byte
 * word (by its offset in the transfer) read as a big-endian single. */
static void rep_cmp_out(jc_rep* r, const spu_context* ctx, uint32_t lsa, const uint8_t* d, uint32_t n)
{
    lsa &= SPU_LS_MASK;
    if (lsa + n <= SPU_LS_SIZE && !memcmp(&ctx->ls[lsa], d, n)) return;   /* the usual case, timed with the job */
    for (uint32_t w = 0; w < n; w += 4) {
        uint32_t got = 0, want = 0, diff = 0;
        for (uint32_t i = w; i < w + 4 && i < n; i++) {
            const uint8_t g = ctx->ls[(lsa + i) & SPU_LS_MASK];
            if (g != d[i]) { r->put_bytes_differ++; diff = 1; }
            got = got << 8 | g; want = want << 8 | d[i];
        }
        if (!diff) continue;
        const uint32_t eg = got & 0x7F800000u, ew = want & 0x7F800000u;
        if (!eg && !ew) { r->w_zero++; continue; }
        if ((got ^ want) >> 31 || !eg || !ew || eg == 0x7F800000u || ew == 0x7F800000u) { r->w_other++; continue; }
        const uint32_t u = got > want ? got - want : want - got;
        if (u > r->max_ulp) r->max_ulp = u;
        if (u <= 1) r->w_ulp1++; else if (u <= 16) r->w_ulp16++; else if (u <= 256) r->w_ulp256++; else r->w_other++;
    }
}

int spu_jobcap_transfer(spu_context* ctx, uint32_t lsa, uint64_t ea, uint32_t size, uint32_t cmd)
{
    jc_rep* r = t_rep;
    if (t_spu_jobcap != 2 || !r) return 0;
    const uint32_t k = jc_class(cmd);
    const uint8_t* d;
    const jc_ev* e = rep_next(r, k, lsa & SPU_LS_MASK, (uint32_t)ea, cmd & 0xFFFFu, k == JC_SDCRZ ? 0 : size, &d);
    if (!e) return 1;
    if (k == JC_GET) {
        ls_write(ctx, lsa, d, size);
        spu_overlay_note_get(ctx, (uint32_t)ea, &ctx->ls[lsa & SPU_LS_MASK], size);
    } else if (k == JC_PUT) {
        rep_cmp_out(r, ctx, lsa, d, size);
    }
    return 1;
}

void spu_jobcap_transfer_done(spu_context* ctx, uint32_t lsa, uint64_t ea, uint32_t size, uint32_t cmd)
{
    jc_cap* c = t_cap;
    if (t_spu_jobcap != 1 || !c || c->ctx != ctx) return;
    const uint32_t k = jc_class(cmd);
    if (k == JC_SDCRZ) { cap_put(c, k, lsa & SPU_LS_MASK, (uint32_t)ea, cmd & 0xFFFFu, NULL, 0); return; }
    uint8_t stackbuf[4096];
    uint8_t* tmp = size <= sizeof stackbuf ? stackbuf : (uint8_t*)malloc(size);
    if (!tmp) { spu_jobcap_abandon("no memory"); return; }
    ls_read(ctx, lsa, tmp, size);
    cap_put(c, k, lsa & SPU_LS_MASK, (uint32_t)ea, cmd & 0xFFFFu, tmp, size);
    if (tmp != stackbuf) free(tmp);
}

int spu_jobcap_atomic(spu_context* ctx, uint32_t cmd)
{
    jc_rep* r = t_rep;
    if (t_spu_jobcap != 2 || !r) return 0;
    const uint32_t ea = ctx->mfc_eal & ~127u, lsa = ctx->mfc_lsa & SPU_LS_MASK & ~127u;
    const uint8_t* d;
    const jc_ev* e = rep_next(r, JC_ATOMIC, lsa, ea, cmd, 128, &d);
    if (!e) return 1;
    if ((e->c & 0xFFFFu) != cmd && !r->diverged++)
        snprintf(r->first, sizeof r->first, "event %u: atomic 0x%X, the record has 0x%X", r->idx - 1, cmd, e->c & 0xFFFFu);
    if (cmd == 0xD0u) {                                     /* GETLLAR: the line read */
        memcpy(&ctx->ls[lsa], d, 128);
    } else {
        rep_cmp_out(r, ctx, lsa, d, 128);
    }
    ctx->atomic_stat = e->c >> 16;
    return 1;
}

void spu_jobcap_atomic_done(spu_context* ctx, uint32_t cmd)
{
    jc_cap* c = t_cap;
    if (t_spu_jobcap != 1 || !c || c->ctx != ctx) return;
    const uint32_t ea = ctx->mfc_eal & ~127u, lsa = ctx->mfc_lsa & SPU_LS_MASK & ~127u;
    cap_put(c, JC_ATOMIC, lsa, ea, (cmd & 0xFFFFu) | (ctx->atomic_stat << 16), &ctx->ls[lsa], 128);
}

int spu_jobcap_rdch(spu_context* ctx, uint32_t channel, u128* value)
{
    (void)ctx;
    jc_rep* r = t_rep;
    if (t_spu_jobcap != 2 || !r) return 0;
    const uint8_t* d;
    if (!rep_next(r, JC_RDCH, channel, 0, 0, sizeof(u128), &d)) { memset(value, 0, sizeof *value); return 1; }
    memcpy(value, d, sizeof *value);
    return 1;
}

void spu_jobcap_rdch_done(spu_context* ctx, uint32_t channel, u128 value)
{
    jc_cap* c = t_cap;
    if (t_spu_jobcap != 1 || !c || c->ctx != ctx) return;
    cap_put(c, JC_RDCH, channel, 0, 0, &value, sizeof value);
}

int spu_jobcap_rchcnt(spu_context* ctx, uint32_t channel, uint32_t* count)
{
    (void)ctx;
    jc_rep* r = t_rep;
    if (t_spu_jobcap != 2 || !r) return 0;
    const uint8_t* d;
    const jc_ev* e = rep_next(r, JC_RCHCNT, channel, 0, 0, 0, &d);
    *count = e ? e->c : 0;
    return 1;
}

void spu_jobcap_rchcnt_done(spu_context* ctx, uint32_t channel, uint32_t count)
{
    jc_cap* c = t_cap;
    if (t_spu_jobcap != 1 || !c || c->ctx != ctx) return;
    cap_put(c, JC_RCHCNT, channel, 0, count, NULL, 0);
}

int spu_jobcap_wrch_out(spu_context* ctx, uint32_t channel, uint32_t value)
{
    if (t_spu_jobcap == 1) {
        jc_cap* c = t_cap;
        if (c && c->ctx == ctx) cap_put(c, JC_WROUT, channel, value, 0, NULL, 0);
        return 0;
    }
    jc_rep* r = t_rep;
    if (t_spu_jobcap != 2 || !r) return 0;
    const uint8_t* d;
    if (!rep_next(r, JC_WROUT, channel, value, 0, 0, &d)) r->out_differ++;
    return 1;                                               /* nobody reads it here */
}

typedef struct { char name[64]; uint32_t wid, entry, n_ev; double best, med; int ok; } jc_row;

static int cmp_d(const void* a, const void* b)
{
    const double x = *(const double*)a, y = *(const double*)b;
    return x < y ? -1 : x > y;
}

static uint8_t* read_file(const char* path, size_t* len)
{
    FILE* f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    const long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t* b = n > 0 ? (uint8_t*)malloc((size_t)n) : NULL;
    if (b && fread(b, 1, (size_t)n, f) != (size_t)n) { free(b); b = NULL; }
    fclose(f);
    *len = b ? (size_t)n : 0;
    return b;
}

static int replay_one(const char* dir, const char* name, int reps, jc_row* row)
{
    char path[640];
    snprintf(path, sizeof path, "%s/%s", dir, name);
    size_t len;
    uint8_t* file = read_file(path, &len);
    if (!file) { fprintf(stderr, "[replay] %s: cannot read\n", name); return 0; }
    jc_hdr h;
    memcpy(&h, file, sizeof h);
    const size_t need = sizeof h + h.ctx_size + h.ev_len + sizeof(((spu_context*)0)->gpr) + SPU_LS_SIZE;
    if (memcmp(h.magic, JC_MAGIC, 8) || h.ctx_size != sizeof(spu_context) || len < need) {
        fprintf(stderr, "[replay] %s: not a capture of this build (context %u bytes, here %zu)\n",
                name, h.ctx_size, sizeof(spu_context));
        free(file);
        return 0;
    }
    const uint8_t* snap = file + sizeof h;
    const uint8_t* ev = snap + h.ctx_size;
    const uint8_t* end_gpr = ev + h.ev_len;
    const uint8_t* end_ls = end_gpr + sizeof(((spu_context*)0)->gpr);
#ifdef _WIN32
    spu_context* ctx = (spu_context*)_aligned_malloc(sizeof(spu_context), 64);
#else
    spu_context* ctx = (spu_context*)aligned_alloc(64, (sizeof(spu_context) + 63) & ~(size_t)63);
#endif
    double* t = (double*)malloc(sizeof(double) * (size_t)reps);
    jc_rep* const r = (jc_rep*)malloc(sizeof(jc_rep));
    int ok = 1;
    s_link = h.link;
    g_spu_jobcap_armed = 1;             /* spu_indirect_branch reports the job's end */
    for (int i = 0; i < reps && ctx && t && r; i++) {
        memcpy(ctx, snap, sizeof(spu_context));
        ctx->ls = ctx->ls_store;
        ctx->ch_wait_cv = NULL; ctx->ch_wait_lock = NULL;
        ctx->irq_frame = NULL; ctx->coh_listed = 0; ctx->stop_request = 0;
        memset(r, 0, sizeof *r);
        r->ev = ev; r->len = h.ev_len; r->n_ev = h.n_ev; r->ctx = ctx;
        t_rep = r;
        t_spu_pm_img = h.pm_img;
        g_spu_trampoline_fn = 0;
        t_spu_jobcap = 2;
        /* The float mode SPU code runs in (spu_run_with_halt sets it in the game). */
#if defined(__x86_64__) || defined(_M_X64)
        const unsigned csr = _mm_getcsr();
        _mm_setcsr(csr | JC_MXCSR_RZFZ);
#endif
        g_spu_float_rz = 1;
        /* The policy module's call: spu_indirect_branch runs the job's first
         * function, the drain the rest. A Wws job module returns through a
         * continuation the drain cannot always resume; in the game the drain
         * then jumps back to the execution driver (spu_restart_dispatch ->
         * spu_run_with_halt), which dispatches on at the pc. Here the replay is
         * that driver. The job ends at the dispatch back to the job manager. */
        volatile int how = 0, restarts = 0;  /* how: 1 back at the link by a dispatch, 0 the drain returned */
        const uint64_t t0 = jc_now_us();
        for (;;) {
            const int jr = setjmp(r->jb);
            if (jr == 1) { how = 1; break; }
            if (jr == 2 && ++restarts > 64) break;
            spu_jmp_no_unwind(r->jb);
            g_spu_trampoline_fn = 0;
            spu_indirect_branch(ctx);
            spu_drain_call(ctx, h.link);
            if ((ctx->pc & SPU_LS_MASK) == h.link || ++restarts > 64) break;
        }
        const uint64_t t1 = jc_now_us();
#if defined(__x86_64__) || defined(_M_X64)
        _mm_setcsr(csr);
#endif
        { static int dbg = -1;
          if (dbg < 0) dbg = getenv("SPU_JOB_CAPTURE_DEBUG") ? 1 : 0;
          if (dbg && i == 0)
              fprintf(stderr, "[replay-dbg] %s ended %s at pc 0x%05X after %u of %u events, host_depth %u, %d restarts\n",
                      name, how ? "by a dispatch to the link" : "with the drain's return",
                      ctx->pc & SPU_LS_MASK, r->idx, h.n_ev, ctx->host_depth, restarts); }
        t_spu_jobcap = 0;
        t_rep = NULL;
        t[i] = (double)(t1 - t0);
        if (r->idx != h.n_ev && !r->diverged++)
            snprintf(r->first, sizeof r->first, "%u of %u recorded events used", r->idx, h.n_ev);
        const int ls_same = !memcmp(ctx->ls, end_ls, SPU_LS_SIZE);
        const int gpr_same = !memcmp(ctx->gpr, end_gpr, sizeof ctx->gpr);
        if (r->diverged || r->put_bytes_differ || r->out_differ || !ls_same || !gpr_same) {
            uint32_t first = 0, last = 0, nd = 0;
            for (uint32_t o = 0; o < SPU_LS_SIZE; o++)
                if (ctx->ls[o] != end_ls[o]) { if (!nd++) first = o; last = o; }
            if (ok)
                fprintf(stderr, "[replay] %s run %d differs: %s%s%u PUT/atomic bytes (words: %u zero, %u/%u/%u within 1/16/256 ulps, %u other; max %u ulps), "
                        "%u mailbox words, local store: %u bytes differ (0x%05X..0x%05X), registers %s\n",
                        name, i, r->diverged ? r->first : "", r->diverged ? "; " : "", r->put_bytes_differ,
                        r->w_zero, r->w_ulp1, r->w_ulp16, r->w_ulp256, r->w_other, r->max_ulp, r->out_differ,
                        nd, first, last, gpr_same ? "same" : "differ");
            ok = 0;
        }
        spu_mfc_release(ctx);                               /* a fresh MFC engine for the next run */
    }
    if (ctx && t) {
        qsort(t, (size_t)reps, sizeof *t, cmp_d);
        snprintf(row->name, sizeof row->name, "%s", name);
        row->wid = h.wid; row->entry = h.entry; row->n_ev = h.n_ev;
        row->best = t[0]; row->med = t[reps / 2]; row->ok = ok;
    }
#ifdef _WIN32
    _aligned_free(ctx);
#else
    free(ctx);
#endif
    free(t);
    free(r);
    free(file);
    return ctx != NULL;
}

int spu_job_replay_main(const char* dir)
{
#ifndef PS3_SPU_JOBCAP
    fprintf(stderr, "[replay] this build has no SPU job hooks: build with PS3_SPU_JOBCAP (gowa.py build --jobcap)\n");
    (void)dir;
    return 1;
#endif
    const char* rp = getenv("SPU_JOB_REPLAY_REPS");
    const int reps = rp && atoi(rp) > 0 ? atoi(rp) : 10;
    const char* only = getenv("SPU_JOB_REPLAY_ONLY");
    size_t nrows = 0, caprows = 256;
    jc_row* rows = (jc_row*)calloc(caprows, sizeof *rows);
    char** names = NULL;
    size_t nn = 0, capn = 0;
#ifdef _WIN32
    char pat[640];
    snprintf(pat, sizeof pat, "%s\\job_w*.bin", dir);
    WIN32_FIND_DATAA fd;
    HANDLE hf = FindFirstFileA(pat, &fd);
    if (hf != INVALID_HANDLE_VALUE) {
        do {
            if (nn == capn) { capn = capn ? capn * 2 : 256; names = (char**)realloc(names, capn * sizeof *names); }
            names[nn++] = _strdup(fd.cFileName);
        } while (FindNextFileA(hf, &fd));
        FindClose(hf);
    }
#else
    DIR* dp = opendir(dir);
    for (struct dirent* de; dp && (de = readdir(dp)); ) {
        if (strncmp(de->d_name, "job_w", 5)) continue;
        if (nn == capn) { capn = capn ? capn * 2 : 256; names = (char**)realloc(names, capn * sizeof *names); }
        names[nn++] = strdup(de->d_name);
    }
    if (dp) closedir(dp);
#endif
    qsort(names, nn, sizeof *names, (int (*)(const void*, const void*))strcmp);
    fprintf(stderr, "[replay] %zu captures in %s, %d runs each\n", nn, dir, reps);
    for (size_t i = 0; i < nn; i++) {
        if (only && !strstr(names[i], only)) continue;
        if (nrows == caprows) { caprows *= 2; rows = (jc_row*)realloc(rows, caprows * sizeof *rows); }
        if (replay_one(dir, names[i], reps, &rows[nrows])) nrows++;
    }
    char csv[640];
    snprintf(csv, sizeof csv, "%s/replay.csv", dir);
    FILE* f = fopen(csv, "w");
    if (f) fprintf(f, "file,wid,entry,events,best_us,median_us,same\n");
    double sum[32] = {0}, sum_med[32] = {0};
    unsigned cnt[32] = {0}, bad[32] = {0};
    for (size_t i = 0; i < nrows; i++) {
        const jc_row* r = &rows[i];
        if (f) fprintf(f, "%s,%u,0x%05X,%u,%.1f,%.1f,%d\n", r->name, r->wid, r->entry, r->n_ev, r->best, r->med, r->ok);
        if (r->wid < 32) { sum[r->wid] += r->best; sum_med[r->wid] += r->med; cnt[r->wid]++; bad[r->wid] += !r->ok; }
    }
    if (f) fclose(f);
    fprintf(stderr, "[replay] per workload (fastest of %d runs per job, then the median run):\n", reps);
    for (int w = 0; w < 32; w++)
        if (cnt[w])
            fprintf(stderr, "[replay]   w%-2d %4u jobs  %9.1f us/job best  %9.1f us/job median  %u differ\n",
                    w, cnt[w], sum[w] / cnt[w], sum_med[w] / cnt[w], bad[w]);
    fprintf(stderr, "[replay] written %s\n", csv);
    for (size_t i = 0; i < nn; i++) free(names[i]);
    free(names);
    free(rows);
    return 0;
}
