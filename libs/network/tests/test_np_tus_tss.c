/* TUS data slots (sceNpTus.c: title and transaction contexts, Get/Set/
 * DeleteMultiSlot data, kept as files), Title Small Storage from
 * PS3_NP_TSS_FILE (sceNpLookup.c) and the game update check
 * (cellGameUpdate.c: NO_UPDATE through the Ex callback).
 *
 * clang -std=gnu17 -I include libs/network/tests/test_np_tus_tss.c -o /tmp/test_np_tus_tss && /tmp/test_np_tus_tss
 * clang-cl /I include /Fe:test_np_tus_tss.exe libs/network/tests/test_np_tus_tss.c
 */
#include "../sceNpTus.c"
#include "../sceNpLookup.c"
#include "../cellGameUpdate.c"
#include <assert.h>
#include <stdlib.h>
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
static unsigned queued;
static u32 queued_opd;
static u64 queued_args[8];
s32 cellSysutilQueueGuestCallbackArgs(u32 opd, const u64 args[8])
{
    queued++; queued_opd = opd;
    for (int i = 0; i < 8; i++) queued_args[i] = args[i];
    return 0;
}
unsigned int ps3_hle_guest_alloc(unsigned int size, unsigned int align) { (void)size; (void)align; return 0xF000u; }

static void put_env(const char* k, const char* v)
{
#ifdef _WIN32
    _putenv_s(k, v ? v : "");
#else
    if (v) setenv(k, v, 1); else unsetenv(k);
#endif
}

#define COMM   0x1000u   /* SceNpCommunicationId */
#define NPID   0x1100u   /* SceNpId */
#define STATUS 0x1200u   /* SceNpTusDataStatus, 488 bytes */
#define DATA   0x1400u
#define INFO   0x1600u   /* SceNpTusDataInfo, 392 bytes */
#define SLOTS  0x1800u
#define RESULT 0x1810u
#define SIZE   0x1820u
#define TSSBUF 0x2000u

int main(int argc, char** argv)
{
    (void)argc;
    setvbuf(stdout, NULL, _IONBF, 0);
    vm_base = calloc(1, 65536);
    assert(vm_base);

    /* ---- TUS: files under PS3_NP_TUS_DIR ---- */
    char dir[512];
    snprintf(dir, sizeof(dir), "%s_tus", argv[0]);
    put_env("PS3_NP_TUS_DIR", dir);
    memcpy(vm_base + COMM, "NPWR00802", 9);
    vm_base[COMM + 10] = 0;
    memcpy(vm_base + NPID, "marge", 6);

    assert(sceNpTusInit(0, 0, NULL) == CELL_OK);
    const s32 title = sceNpTusCreateTitleCtx(COMM, 0, NPID);
    assert(title >= 1);
    const s32 trans = sceNpTusCreateTransactionCtx(title);
    assert(trans >= 1);
    assert((u32)sceNpTusCreateTransactionCtx(99) == TUS_ERROR_INVALID_ID);

    /* An empty slot: done, no data. */
    vm_write32(RESULT, 0xDEAD);
    assert(sceNpTusDeleteMultiSlotData(trans, NPID, 0, 0, 0) == CELL_OK);
    vm_write32(SLOTS, 3);
    assert(sceNpTusDeleteMultiSlotData(trans, NPID, SLOTS, 1, 0) == CELL_OK);
    assert(sceNpTusGetDataAsync(trans, NPID, 3, STATUS, 488, DATA, 64, 0) == CELL_OK);
    assert(sceNpTusPollAsync(trans, RESULT) == 0 && vm_read32(RESULT) == 0);
    assert(vm_read32(STATUS + 36) == 0);                       /* hasData */
    assert(memcmp(vm_base + STATUS, "marge", 6) == 0);         /* ownerId */

    /* Set, then get it back: data, size, info, author. */
    memcpy(vm_base + DATA, "{\"profile\":{}}", 15);
    vm_write32(INFO, 5);
    memcpy(vm_base + INFO + 8, "hello", 5);
    assert(sceNpTusSetDataAsync(trans, NPID, 3, 15, 15, DATA, INFO, 392) == CELL_OK);
    assert(sceNpTusPollAsync(trans, RESULT) == 0 && vm_read32(RESULT) == 0);
    memset(vm_base + DATA, 0, 64);
    memset(vm_base + STATUS, 0xAA, 488);
    assert(sceNpTusGetDataAsync(trans, NPID, 3, STATUS, 488, DATA, 64, 0) == CELL_OK);
    assert(vm_read32(STATUS + 36) == 1);
    assert(vm_read32(STATUS + 84) == DATA && vm_read32(STATUS + 88) == 15);
    assert(memcmp(vm_base + DATA, "{\"profile\":{}}", 15) == 0);
    assert(vm_read32(STATUS + 96) == 5 && memcmp(vm_base + STATUS + 104, "hello", 5) == 0);
    assert(memcmp(vm_base + STATUS + 48, "marge", 6) == 0);    /* lastChangedAuthorId */
    assert(vm_read64(STATUS + 40) > 63713433600ull * 1000000ull);

    /* A buffer smaller than the data: cut, the size stays the whole. */
    memset(vm_base + DATA, 0, 64);
    assert(sceNpTusGetData(trans, NPID, 3, STATUS, 488, DATA, 4, 0) == CELL_OK);
    assert(vm_read32(STATUS + 88) == 15 && memcmp(vm_base + DATA, "{\"pr", 4) == 0 && vm_base[DATA + 4] == 0);

    /* Another player's slot is another file. */
    memcpy(vm_base + NPID, "bart", 5);
    assert(sceNpTusGetData(trans, NPID, 3, STATUS, 488, DATA, 64, 0) == CELL_OK && vm_read32(STATUS + 36) == 0);
    memcpy(vm_base + NPID, "marge", 6);

    /* Deleted: empty again. */
    assert(sceNpTusDeleteMultiSlotDataAsync(trans, NPID, SLOTS, 1, 0) == CELL_OK);
    assert(sceNpTusGetData(trans, NPID, 3, STATUS, 488, DATA, 64, 0) == CELL_OK && vm_read32(STATUS + 36) == 0);

    assert(sceNpTusDestroyTransactionCtx(trans) == CELL_OK);
    assert((u32)sceNpTusPollAsync(trans, RESULT) == TUS_ERROR_INVALID_ID);
    assert(sceNpTusDestroyTitleCtx(title) == CELL_OK);
    assert(sceNpTusTerm() == CELL_OK);

    /* ---- TSS: the file's bytes, or a failed request ---- */
    char tss[512];
    snprintf(tss, sizeof(tss), "%s_tss.bin", argv[0]);
    FILE* f = fopen(tss, "wb");
    assert(f);
    fwrite("TSS-BYTES-0123456789", 1, 20, f);
    fclose(f);
    assert(sceNpLookupInit() == CELL_OK);
    const s32 lt = sceNpLookupCreateTitleCtx(COMM, NPID);
    assert(lt >= 1);
    s32 lx = sceNpLookupCreateTransactionCtx(lt);
    assert(lx >= 1);
    put_env("PS3_NP_TSS_FILE", tss);
    assert(sceNpLookupTitleSmallStorageAsync(lx, TSSBUF, 0x10000, SIZE, 0, 0) == CELL_OK);
    assert(sceNpLookupPollAsync(lx, RESULT) == 0 && vm_read32(RESULT) == 0);
    assert(vm_read32(SIZE) == 20 && memcmp(vm_base + TSSBUF, "TSS-BYTES-0123456789", 20) == 0);
    assert(sceNpLookupTitleSmallStorageAsync(lx, TSSBUF, 8, SIZE, 0, 0) == CELL_OK);   /* cut to the buffer */
    assert(vm_read32(SIZE) == 8);
    put_env("PS3_NP_TSS_FILE", NULL);
    assert(sceNpLookupTitleSmallStorageAsync(lx, TSSBUF, 0x10000, SIZE, 0, 0) == CELL_OK);
    assert(sceNpLookupPollAsync(lx, RESULT) == 0 && vm_read32(RESULT) == LOOKUP_ERROR_SERVICE_UNAVAILABLE);
    assert((u32)sceNpLookupTitleSmallStorage(lx, TSSBUF, 0x10000, SIZE, 0) == LOOKUP_ERROR_SERVICE_UNAVAILABLE);
    remove(tss);

    /* ---- Game update: no update, through the callback on the next poll ---- */
    assert((u32)cellGameUpdateCheckStartWithoutDialogAsyncEx(0x3000, 0x4000) == CELL_GAMEUPDATE_ERROR_NOT_INITIALIZED);
    assert(cellGameUpdateInit() == CELL_OK);
    queued = 0;
    assert(cellGameUpdateCheckStartWithoutDialogAsyncEx(0x3000, 0x4000) == CELL_OK);
    assert(queued == 1 && queued_opd == 0x3000 && queued_args[0] == 0xF000u && queued_args[1] == 0x4000);
    assert(vm_read32(0xF000) == CELL_GAMEUPDATE_RESULT_STATUS_NO_UPDATE && vm_read32(0xF004) == 0);
    assert(cellGameUpdateCheckFinishAsyncEx(0x3000, 0) == CELL_OK);
    assert(vm_read32(0xF000) == CELL_GAMEUPDATE_RESULT_STATUS_FINISHED);
    queued = 0;
    assert(cellGameUpdateCheckStartWithoutDialogAsync(0x3100, 0x4100) == CELL_OK);
    assert(queued == 1 && queued_args[0] == CELL_GAMEUPDATE_RESULT_STATUS_NO_UPDATE && queued_args[2] == 0x4100);
    assert(cellGameUpdateTerm() == CELL_OK);

    free(vm_base);
    puts("test_np_tus_tss: OK");
    return 0;
}
