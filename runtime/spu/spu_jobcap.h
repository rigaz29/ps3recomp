/*
 * ps3recomp - SPU job capture and replay
 *
 * SPU_JOB_CAPTURE=<dir> writes, for chosen jobs of a policy module's
 * workloads, everything a job takes from outside its local store: the context
 * and local store at the job's first instruction, then in order every DMA GET
 * (with its bytes), every PUT (with its bytes, to compare), every atomic and
 * every channel read. SPU_JOB_REPLAY=<dir> runs those jobs again in a loop,
 * outside the game, serving the same inputs, and reports the time per job and
 * whether the output (PUTs, local store, registers) came out the same.
 * See spu_jobcap.c for the switches.
 */
#ifndef SPU_JOBCAP_H
#define SPU_JOBCAP_H

#include "spu_context.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Per host thread: 0 off, 1 capturing the job running on this thread,
 * 2 replaying one. The hooks below test it first. */
extern SPU_THREAD_LOCAL int t_spu_jobcap;
/* SPU_JOB_CAPTURE is set: spu_indirect_branch looks for job starts. */
extern int g_spu_jobcap_armed;
/* The image id of the policy module this thread is running (set by
 * spu_run_policy_module): code with this image that branches above LS 0x4000
 * with its link below is a policy module starting a job. */
extern SPU_THREAD_LOCAL int t_spu_pm_img;

/* What the hooks in the hot paths test. Built only with PS3_SPU_JOBCAP (a
 * port's measurement build): they wrap every DMA, channel read and drain, so a
 * default build compiles those paths exactly as without them, whatever the
 * wrappers cost (too little for a level A/B to see, behind the global test). */
#ifdef PS3_SPU_JOBCAP
#define SPU_JOBCAP_ARMED()   (__builtin_expect(g_spu_jobcap_armed != 0, 0))
#define SPU_JOBCAP_ANY()     (SPU_JOBCAP_ARMED() && t_spu_jobcap != 0)
#define SPU_JOBCAP_ON(state) (SPU_JOBCAP_ARMED() && t_spu_jobcap == (state))
#else
#define SPU_JOBCAP_ARMED()   0
#define SPU_JOBCAP_ANY()     0
#define SPU_JOBCAP_ON(state) 0
#endif

/* SPU_JOB_TIME: what the runtime does for a job, counted per host thread in
 * its hot paths (measurement builds only). spu_jobcap.c takes each timed job's
 * share and reports it per workload, per job, beside the job's time. */
enum {
    SPU_JS_CALL, SPU_JS_IB, SPU_JS_IB_FULL, SPU_JS_IB_LOW, SPU_JS_IB_IRET,
    SPU_JS_LOOKUP, SPU_JS_LOOKUP_MISS, SPU_JS_LOOKUP_FULL, SPU_JS_DRAIN_FULL, SPU_JS_KPOLL,
    SPU_JS_GET, SPU_JS_GET_B, SPU_JS_PUT, SPU_JS_PUT_B, SPU_JS_LIST, SPU_JS_LIST_EL,
    SPU_JS_NOTE_GET, SPU_JS_CODE_GET_B, SPU_JS_KEPT_CMP_B,
    SPU_JS_LLAR, SPU_JS_LLC, SPU_JS_LLUC, SPU_JS_LOCK, SPU_JS_LOCK_WAIT, SPU_JS_LOCK_SPIN,
    SPU_JS_RDCH, SPU_JS_RCHCNT, SPU_JS_WRCH, SPU_JS_EVACK, SPU_JS_IRQ,
    SPU_JS_RES_DROP, SPU_JS_RES_PEND, SPU_JS_RES_NEW, SPU_JS_RES_PIC, SPU_JS_VERIFY_B,
    SPU_JS_N
};
#ifdef PS3_SPU_JOBCAP
extern SPU_THREAD_LOCAL uint64_t t_spu_js[SPU_JS_N];
#define SPU_JS_ADD(k, n) ((void)(t_spu_js[k] += (uint64_t)(n)))
#else
#define SPU_JS_ADD(k, n) ((void)0)
#endif

void spu_jobcap_init(void);
/* SPU_JOB_TIME: told at each timed job's start (1) and end (0); the SPURS
 * runtime sets it to tag its worker's profiler samples (cellSpurs.c). */
extern void (*g_spu_job_mark_hook)(int in_job);
/* spu_indirect_branch, before it calls the target. */
void spu_jobcap_maybe_begin(spu_context* ctx);
/* spu_drain_call, after the drain with return_pc ends. */
void spu_jobcap_drain_done(spu_context* ctx, uint32_t return_pc);

/* Hooks. Each one returns 1 when a replay served the operation (the caller
 * then skips its own work) and 0 when the caller goes on as usual (it may
 * have been recorded). */
int spu_jobcap_transfer(spu_context* ctx, uint32_t lsa, uint64_t ea, uint32_t size, uint32_t cmd);
void spu_jobcap_transfer_done(spu_context* ctx, uint32_t lsa, uint64_t ea, uint32_t size, uint32_t cmd);
int spu_jobcap_atomic(spu_context* ctx, uint32_t cmd);
void spu_jobcap_atomic_done(spu_context* ctx, uint32_t cmd);
int spu_jobcap_rdch(spu_context* ctx, uint32_t channel, u128* value);
void spu_jobcap_rdch_done(spu_context* ctx, uint32_t channel, u128 value);
int spu_jobcap_rchcnt(spu_context* ctx, uint32_t channel, uint32_t* count);
void spu_jobcap_rchcnt_done(spu_context* ctx, uint32_t channel, uint32_t count);
int spu_jobcap_wrch_out(spu_context* ctx, uint32_t channel, uint32_t value);
/* The kernel poll (cellSpursModulePoll): its answer, and the SPURS instance's
 * first line its workload selection copies from guest memory into LS 0x100
 * (no DMA). Recorded, and served in a replay; returns the answer to use. */
uint64_t spu_jobcap_kernel_poll(spu_context* ctx, uint32_t ea, uint64_t answer);
/* spu_restart_dispatch during a replay: jump back to the replay, which
 * dispatches on at ctx->pc as the execution driver would. */
void spu_jobcap_restart(spu_context* ctx);
/* A stop, halt or interrupt during a captured job: the job cannot be
 * replayed, so the capture is dropped. */
void spu_jobcap_abandon(const char* why);

/* SPU_JOB_REPLAY: run the captures in dir, print the report, return 0. */
int spu_job_replay_main(const char* dir);

#ifdef __cplusplus
}
#endif

#endif /* SPU_JOBCAP_H */
