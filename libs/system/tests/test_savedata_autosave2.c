/*
 * cellSaveDataAutoSave2 hands the title's stat callback the directory name it
 * was given, wherever the guest's memory is.
 *
 * It translated dirName to a host pointer and passed that on to
 * cellSaveDataAutoSave, which translated it again. With the guest's memory
 * below 4 GB the host pointer read as a guest address, the name came out
 * empty, and the save went into the save root itself (Ascension-R issue #10).
 * This maps the guest's memory low, at 0x10000, where the second translation
 * lands on zeros inside the same mapping, and checks the name the stat
 * callback is given. Built and run by tools/test_savedata_autosave2.py, which
 * passes a scratch directory (the save root) as argv[1]; a system that will not
 * map memory that low skips it.
 */
#include "../cellSaveData.c"
#ifdef _WIN32
#include <windows.h>
#else
#include <sys/mman.h>
#endif
uint8_t* vm_base;
uint32_t ppu_vm_size;
int g_resv_store_active;
uint32_t g_ww_lo, g_ww_hi;
void ppu_resv_break_store(uint64_t ea) { (void)ea; }
void ps3_ww_report_inline(uint32_t a, uint64_t v, int w) { (void)a; (void)v; (void)w; }
int spu_coh_is_reserved(uint32_t a) { (void)a; return 0; }
void spu_coh_notify_write(uint32_t a) { (void)a; }
void spu_lockline_lock(void) {}
void spu_lockline_unlock(void) {}
void spu_lockline_lock_ea(uint32_t ea, uint32_t len) { (void)ea; (void)len; }
void spu_lockline_unlock_ea(uint32_t ea, uint32_t len) { (void)ea; (void)len; }
ps3_guest_caller_fn g_ps3_guest_caller;
void ps3_hle_register_ctx(uint32_t nid, const char* name, void (*fn)(ppu_context*))
{
    (void)nid; (void)name; (void)fn;
}

#define LOW 0x10000u
#define SIZE 0x200000u
#define STAT_OPD 0x900u
static int s_stat_calls;
static char s_seen[CELL_SAVEDATA_DIRNAME_SIZE + 1];

static void guest_callback(u32 opd, u64 cb, u64 get, u64 set,
                           u64 a, u64 b, u64 c, u64 d, u64 e)
{
    (void)set; (void)a; (void)b; (void)c; (void)d; (void)e;
    if (opd != STAT_OPD) { vm_write32((u32)cb, (u32)-1); return; }
    s_stat_calls++;
    /* CellSaveDataStatGet: dir.dirName at +32 */
    snprintf(s_seen, sizeof s_seen, "%s", (const char*)vm_base + (u32)get + 32);
    vm_write32((u32)cb, 1);  /* OK_LAST: finish without a file operation */
}

static void* map_low(void)
{
#ifdef _WIN32
    return VirtualAlloc((void*)(uintptr_t)LOW, SIZE, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
#elif defined(MAP_FIXED_NOREPLACE)
    void* m = mmap((void*)(uintptr_t)LOW, SIZE, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
    return m == MAP_FAILED ? NULL : m;
#else
    return NULL;
#endif
}

int main(int argc, char** argv)
{
    if (argc < 2 || strlen(argv[1]) >= sizeof s_save_root) {
        puts("FAIL: usage: test_savedata_autosave2 SCRATCH_DIR");
        return 1;
    }
    strcpy(s_save_root, argv[1]);
    void* m = map_low();
    if (m != (void*)(uintptr_t)LOW) {
        puts("SKIP: this system will not map memory at 0x10000");
        return 0;
    }
    vm_base = (uint8_t*)m;
    if (cellSaveData_set_scratch_region(0x100000, 0x20000) != CELL_OK) {
        puts("FAIL: scratch region"); return 1;
    }
    strcpy((char*)vm_base + 0x300, "TEST00002");
    g_ps3_guest_caller = guest_callback;

    cellSaveDataAutoSave2(0, (const char*)(uintptr_t)0x300, 0, (CellSaveDataSetBuf*)(uintptr_t)0x200,
                          (CellSaveDataStatCallback)(uintptr_t)STAT_OPD,
                          (CellSaveDataFileCallback)(uintptr_t)0xA00, 0, NULL);
    int failed = 0;
    printf("%s: funcStat ran once\n", s_stat_calls == 1 ? "ok  " : "FAIL");
    failed += s_stat_calls != 1;
    printf("%s: the stat callback was given dir '%s' (want 'TEST00002')\n",
           strcmp(s_seen, "TEST00002") == 0 ? "ok  " : "FAIL", s_seen);
    failed += strcmp(s_seen, "TEST00002") != 0;
    if (failed) { printf("FAIL (%d failed)\n", failed); return 1; }
    puts("PASS (0 failed)");
    return 0;
}
