/* SPURS_RESERVE with SPURS_LEND (spurs_reserved_away, spurs_lend_mark, spurs_claim,
 * spurs_pick, spurs_kernel_poll) on a hand-built 32-workload instance, God of War:
 * Ascension's priority tables: audio (7) keeps SPU 5; RenJobSet (by name), Batch (by id) and
 * HighestPrio6 (by name) may take it too, never as strong as audio; General6 may not; an EDGE
 * list lent by id still has priority 0 there. No scheduler thread runs. */
#define spu_workload_dispatch_async fixture_unbounded_dispatch
#include "../libs/spurs/cellSpurs.c"
#undef spu_workload_dispatch_async
#include <assert.h>

void spu_indirect_branch(spu_context*);
uint8_t* vm_base;
uint32_t ppu_vm_size = 0x20000;
int g_resv_store_active;
uint32_t g_ww_lo, g_ww_hi, g_barrier_sync_watch;
uint32_t g_spu_image_src_ea, g_spu_image_ls_start, g_spu_image_span;
unsigned long long ps3_ms_now(void) { return 0; }
void ppu_resv_break_store(uint64_t ea) { (void)ea; }
void ps3_ww_report_inline(uint32_t a, uint64_t v, int w) { (void)a; (void)v; (void)w; }
uint64_t spurs_pm_build_context(uint8_t* ls, uint32_t ts, uint32_t id, uint32_t spu, uint32_t tag)
{ (void)ls; (void)ts; (void)id; (void)spu; (void)tag; return 0; }
void spu_thread_publish_ctx(uint32_t tid, void* c) { (void)tid; (void)c; }
void ydkj_wake_all_event_flags(void) {}
void sys_event_queue_cancel_by_id(u32 id) { (void)id; }
void sys_event_queue_uncancel_by_id(u32 id) { (void)id; }
int fixture_unbounded_dispatch(const uint8_t* pm, uint32_t size, uint32_t data)
{ (void)pm; (void)size; (void)data; return 0; }
void ps3_hle_register_ctx(uint32_t nid, const char* name, void (*fn)(ppu_context*))
{ (void)nid; (void)name; (void)fn; }

static void pm_body(spu_context* c) { (void)c; }

#define EA 0x1000u
static void add(u32 wid, const u8 prio[8], u32 max, u32 ready, const char* name)
{
    s_workloads[wid].in_use = 1;
    s_workloads[wid].spurs_ea = EA;
    s_workloads[wid].pm = (const void*)(uintptr_t)0x9000;
    s_workloads[wid].sizePm = 16;
    vm_write32(EA + SPURS_WKL_ENABLED, vm_read32(EA + SPURS_WKL_ENABLED) | (0x80000000u >> wid));
    vm_base[EA + wkl_state_off(wid)] = 2;
    memcpy(vm_base + EA + wkl_info_off(wid) + 0x18, prio, 8);
    wkl_count_set(EA, SPURS_WKL_MAXCONT, wid, 1, max);
    vm_base[EA + wkl_ready_off(wid)] = (u8)ready;
    snprintf(s_workloads[wid].name, sizeof s_workloads[wid].name, "%s", name ? name : "");
    s_workloads[wid].lent = 0;
    spurs_lend_mark(wid);
}

int main(void)
{
    setenv("SPURS_RESERVE", "7:5", 1);
    setenv("SPURS_LEND", "RenJobSet,6,HighestPrio6,19", 1);
    vm_base = calloc(1, ppu_vm_size);
    assert(vm_base);
    memset(vm_base + 0x9000, 0x5A, 16);
    spu_workload_register_img(spu_workload_fingerprint(vm_base + 0x9000, 16), pm_body, 90, "lend fixture");
    struct SpursInst* si = &s_inst[0];
    si->ea = EA; si->nspus = 6; si->is2 = 1;
    vm_base[EA + SPURS_FLAGS1] = SPURS_SF1_32_WORKLOADS;
    vm_base[EA + SPURS_WKL_FLAG_RECV] = 0xFF;
    vm_write32(EA + SPURS_WKL_FLAG_WORD, 0xFFFFFFFFu);
    for (u32 sn = 0; sn < 6; sn++) { si->slots[sn].instance = si; si->slots[sn].spu_num = sn; si->slots[sn].wid = 31; }
    static const u8 audio[8] = { 0, 0, 0, 0, 0, 1, 0, 0 };
    static const u8 hp6[8]   = { 3, 3, 3, 2, 1, 1, 0, 0 };
    static const u8 gen6[8]  = { 13, 13, 13, 12, 11, 11, 0, 0 };
    static const u8 batch[8] = { 2, 2, 2, 2, 2, 2, 0, 0 };
    static const u8 rjs[8]   = { 8, 8, 8, 8, 8, 8, 0, 0 };
    static const u8 edge[8]  = { 3, 3, 3, 3, 0, 0, 0, 0 };
    add(7, audio, 1, 0, "BrBDblBufWrkld");
    add(1, hp6, 6, 0, "HighestPrio6");
    add(2, gen6, 6, 8, "General6");
    add(6, batch, 6, 0, "BatchJobList");
    add(8, rjs, 8, 1, "RenJobSet");
    add(19, edge, 8, 4, "EDGE0");
    assert(s_workloads[8].lent && s_workloads[6].lent && s_workloads[1].lent && s_workloads[19].lent);
    assert(!s_workloads[2].lent && !s_workloads[7].lent);
    puts("SPURS lend: workloads named by instance name and by id PASS");

    const u32 en = vm_read32(EA + SPURS_WKL_ENABLED);
    int asked;
    assert(spurs_claim(si, 2, 5, en, 0, 31, &asked) == 0);                      /* General6: kept off */
    assert(spurs_claim(si, 8, 5, en, 0, 31, &asked) == ((0x10u - 8) << 4) && asked);
    assert(spurs_claim(si, 6, 5, en, 0, 31, &asked) == ((0x10u - 2) << 4));     /* Batch 2: below audio's 1 */
    assert(spurs_claim(si, 1, 5, en, 0, 31, &asked) == ((0x10u - 2) << 4));     /* HighestPrio6 1 there: made 2 */
    assert(spurs_claim(si, 1, 4, en, 0, 31, &asked) == ((0x10u - 1) << 4));     /* SPU 4: its own 1 */
    assert(spurs_claim(si, 19, 5, en, 0, 31, &asked) == 0);                     /* the title's 0 there stands */
    assert(spurs_claim(si, 7, 5, en, 0, 31, &asked) == ((0x10u - 1) << 4));
    assert(spurs_claim(si, 2, 4, en, 0, 31, &asked) == ((0x10u - 11) << 4) && asked);   /* other SPUs as before */
    puts("SPURS lend: the reserved SPU takes lent workloads below the reserved one, the rest as before PASS");

    /* SPU 5 idle: General6 alone is asked for and may not take it... */
    vm_base[EA + wkl_ready_off(8)] = 0;
    assert(!spurs_pick(si, &si->slots[5], SPURS_PICK_KICK));
    /* ...RenJobSet asked for takes it... */
    vm_base[EA + wkl_ready_off(8)] = 1;
    assert(spurs_pick(si, &si->slots[5], SPURS_PICK_KICK) && si->slots[5].wid == 8);
    /* ...goes on at its polls while audio is not asked for... */
    assert((spurs_kernel_poll(EA, 5, 8) >> 32) == 8);
    /* ...and gives way at the next one when it is. */
    vm_base[EA + wkl_ready_off(7)] = 1;
    assert((spurs_kernel_poll(EA, 5, 8) >> 32) == 7);
    si->slots[5].busy = 0;
    if (si->active[8]) si->active[8]--;
    assert(spurs_pick(si, &si->slots[5], SPURS_PICK_KICK) && si->slots[5].wid == 7);
    puts("SPURS lend: a lent workload yields the reserved SPU to its own workload at the next poll PASS");
    free(vm_base);
    return 0;
}
