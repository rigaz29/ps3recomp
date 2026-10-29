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

void spu_jobcap_init(void);
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
/* The kernel's workload selection copies the SPURS instance's first line
 * from guest memory into LS 0x100 (no DMA): recorded, and served in a replay. */
void spu_jobcap_kernel_line(spu_context* ctx, uint32_t ea);
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
