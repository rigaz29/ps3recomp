/* The SPURS kernel's choice of workload (spurs_claim, spurs_pick,
 * spurs_kernel_poll) and _cellSpursWorkloadAttributeInitialize's ninth
 * argument, on a hand-built 32-workload instance. No scheduler thread runs, so
 * every answer is deterministic. The priorities and maxima are God of War:
 * Ascension's (docs: the RPCS3 comparison of 2026-10-31). */
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

static void (*s_attr_init)(ppu_context*);
void ps3_hle_register_ctx(uint32_t nid, const char* name, void (*fn)(ppu_context*))
{
    (void)name;
    if (nid == 0xEFEB2679u) s_attr_init = fn;
}

static void pm_body(spu_context* c) { (void)c; }

#define EA 0x1000u
static void add(u32 wid, const u8 prio[8], u32 max, u32 ready)
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
}

int main(void)
{
    vm_base = calloc(1, ppu_vm_size);
    assert(vm_base);

    /* maxContention arrives from SP + 0x70; the eight registers carry the rest. */
    cellSpurs_register_ctx_handlers();
    assert(s_attr_init);
    ppu_context ctx;
    memset(&ctx, 0, sizeof ctx);
    ctx.gpr[1] = 0x6000;
    vm_write64(0x6000 + 0x70, 6);
    ctx.gpr[3] = 0x7000; ctx.gpr[4] = 1; ctx.gpr[5] = 0x330000; ctx.gpr[6] = 0x9000;
    ctx.gpr[7] = 16; ctx.gpr[8] = 0x70B14E80; ctx.gpr[9] = 0; ctx.gpr[10] = 1;
    s_attr_init(&ctx);
    assert(ctx.gpr[3] == 0);
    assert(vm_read32(0x7000 + WKATTR_MIN_CONT) == 1 && vm_read32(0x7000 + WKATTR_MAX_CONT) == 6);
    assert(vm_read64(0x7000 + WKATTR_DATA) == 0x70B14E80 && vm_read32(0x7000 + WKATTR_SIZE) == 16);
    puts("SPURS workload attribute: maxContention from the ninth argument PASS");

    memset(vm_base + 0x9000, 0x5A, 16);
    spu_workload_register_img(spu_workload_fingerprint(vm_base + 0x9000, 16), pm_body, 90, "select fixture");
    struct SpursInst* si = &s_inst[0];
    si->ea = EA; si->nspus = 6; si->is2 = 1;
    vm_base[EA + SPURS_FLAGS1] = SPURS_SF1_32_WORKLOADS;
    vm_base[EA + SPURS_WKL_FLAG_RECV] = 0xFF;
    vm_write32(EA + SPURS_WKL_FLAG_WORD, 0xFFFFFFFFu);
    for (u32 sn = 0; sn < 6; sn++) { si->slots[sn].instance = si; si->slots[sn].spu_num = sn; si->slots[sn].wid = 31; }
    static const u8 edge[8] = { 3, 3, 3, 3, 0, 0, 0, 0 };
    static const u8 gen6[8] = { 13, 13, 13, 12, 11, 11, 0, 0 };
    static const u8 rjs[8]  = { 8, 8, 8, 8, 8, 8, 0, 0 };
    add(2, gen6, 6, 8);     /* General6: the job manager asks for 8 */
    add(8, rjs, 8, 0);      /* RenJobSet: nothing asked */
    add(19, edge, 8, 4);    /* an EDGE list in the second set: asks for 4 */
    const u32 en = vm_read32(EA + SPURS_WKL_ENABLED);

    int asked;
    assert(spurs_claim(si, 19, 0, en, 0, 31, &asked) == (13u << 4) && asked);
    assert(spurs_claim(si, 2, 0, en, 0, 31, &asked) == (3u << 4) && asked);
    assert(spurs_claim(si, 2, 0, en, 0, 2, &asked) == ((3u << 4) | 4));      /* its SPU ran it last */
    assert(spurs_claim(si, 19, 4, en, 0, 31, &asked) == 0);                   /* priority 0 on SPU 4 */
    assert(spurs_claim(si, 19, 0, en, 4, 31, &asked) == (13u << 4) && !asked); /* ready 4 = 4 SPUs */
    assert(spurs_claim(si, 19, 0, en, 8, 31, &asked) == 0);                   /* at its maximum */
    assert(spurs_claim(si, 8, 0, en, 0, 31, &asked) == (8u << 4) && !asked);
    vm_write16(EA + SPURS_WKL_SIGNAL1, 0x0080);                               /* wid 8's signal bit */
    assert(spurs_claim(si, 8, 0, en, 0, 31, &asked) && asked);
    vm_write16(EA + SPURS_WKL_SIGNAL1, 0);
    vm_base[EA + SPURS_WKL_FLAG_RECV] = 8;
    vm_write32(EA + SPURS_WKL_FLAG_WORD, 0);                                  /* the flag pulled to 0 */
    assert(spurs_claim(si, 8, 0, en, 0, 31, &asked) && asked);
    vm_base[EA + SPURS_WKL_FLAG_RECV] = 0xFF;
    vm_write32(EA + SPURS_WKL_FLAG_WORD, 0xFFFFFFFFu);
    puts("SPURS claim: priority weight, ready count against contention, signal, flag, maximum PASS");

    /* SPUs 0-3 go to the EDGE list (priority 3) until its 4 are there, then to
     * General6; SPUs 4-5, where EDGE has priority 0, to General6. The ready
     * counts stay as the job manager stored them. */
    for (u32 sn = 0; sn < 4; sn++)
        assert(spurs_pick(si, &si->slots[sn], SPURS_PICK_KICK) && si->slots[sn].wid == 19);
    assert(si->active[19] == 4 && vm_base[EA + wkl_ready_off(19)] == 4);
    assert((vm_base[EA + SPURS_WKL_CURCONT + (19 & 15)] >> 4) == 4);
    assert(spurs_pick(si, &si->slots[4], SPURS_PICK_KICK) && si->slots[4].wid == 2);
    assert(spurs_pick(si, &si->slots[5], SPURS_PICK_KICK) && si->slots[5].wid == 2);
    assert(si->active[2] == 2 && vm_base[EA + wkl_ready_off(2)] == 8);
    puts("SPURS pick: strongest asked-for workload, ready counts kept PASS");

    /* A polling module goes on while its workload is still asked for... */
    uint64_t r = spurs_kernel_poll(EA, 0, 19);
    assert((r >> 32) == 19 && (r & 1));
    /* ...and its signal bit is consumed when the answer is its own. */
    vm_write16(EA + SPURS_WKL_SIGNAL2, 0x1000);                               /* wid 19 */
    r = spurs_kernel_poll(EA, 0, 19);
    assert((r >> 32) == 19 && (r & 2) && !vm_read16(EA + SPURS_WKL_SIGNAL2));
    /* A stronger workload asked for takes the SPU at the next poll: on SPU 4,
     * RenJobSet (8) outweighs General6 (11 there). */
    vm_base[EA + wkl_ready_off(8)] = 1;
    r = spurs_kernel_poll(EA, 4, 2);
    assert((r >> 32) == 8);
    vm_base[EA + wkl_ready_off(8)] = 0;
    assert((spurs_kernel_poll(EA, 4, 2) >> 32) == 2);
    /* The job manager drops the EDGE list's ready count: its modules give way
     * (here to General6, still asked for), and with nothing asked the answer
     * is the system service, 0x20. */
    vm_base[EA + wkl_ready_off(19)] = 0;
    assert((spurs_kernel_poll(EA, 0, 19) >> 32) == 2);
    vm_base[EA + wkl_ready_off(2)] = 0;
    assert((spurs_kernel_poll(EA, 0, 19) >> 32) == 0x20);
    puts("SPURS poll: own workload while asked for, stronger one takes over, none gives the system service PASS");
    free(vm_base);
    return 0;
}
