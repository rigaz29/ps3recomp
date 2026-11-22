/*
 * ps3recomp - cellGameUpdate (exported by libnetctl): the check for a newer version of the title on PSN.
 *
 * There is no PSN to ask, so every check finds no update, as RPCS3 answers: the callback comes on the title's
 * next cellSysutilCheckCallback with status NO_UPDATE (a finish call reports FINISHED). Titles that hold their
 * online play until the check is done go on; God of War: Ascension waits on it ("waiting for TPPS") and reads the
 * Ex result's status and error code. Unimplemented, these answered CELL_OK and the callback never came.
 *
 * Layouts (SDK ABI): CellGameUpdateResult { s32 status; s32 error_code; char app_ver[6]; u8 padding[2]; };
 * callbacks: void cb(s32 status, s32 error_code, void* userdata), void cbEx(CellGameUpdateResult*, void* userdata).
 */
#include "../system/cellSysutil.h"
#include "../../runtime/ppu/ppu_memory.h"

#include <stdio.h>
#include <string.h>

#define CELL_GAMEUPDATE_RESULT_STATUS_NO_UPDATE 0
#define CELL_GAMEUPDATE_RESULT_STATUS_FINISHED  5
#define CELL_GAMEUPDATE_ERROR_NOT_INITIALIZED     0x8002cc01u
#define CELL_GAMEUPDATE_ERROR_ALREADY_INITIALIZED 0x8002cc02u
#define CELL_GAMEUPDATE_ERROR_INVALID_ADDR        0x8002cc03u

#define RESULT_SIZE 16

extern unsigned int ps3_hle_guest_alloc(unsigned int size, unsigned int align);   /* runtime/ppu/ppu_sysprx.cpp */

static int s_init;
static u32 s_result_ea;   /* the Ex result handed to the title; one is enough, a check at a time */

static void answer(u32 cb, u32 userdata, s32 status)
{
    const u64 args[8] = { (u64)(u32)status, 0, userdata, 0, 0, 0, 0, 0 };
    cellSysutilQueueGuestCallbackArgs(cb, args);
}

static s32 answer_ex(u32 cb, u32 userdata, s32 status)
{
    if (!s_result_ea) s_result_ea = ps3_hle_guest_alloc(RESULT_SIZE, 16);
    if (!s_result_ea) return (s32)CELL_GAMEUPDATE_ERROR_INVALID_ADDR;
    memset(vm_ptr8(s_result_ea), 0, RESULT_SIZE);
    vm_write32(s_result_ea, (u32)status);
    vm_write32(s_result_ea + 4, 0);   /* error_code; app_ver stays empty */
    const u64 args[8] = { s_result_ea, userdata, 0, 0, 0, 0, 0, 0 };
    cellSysutilQueueGuestCallbackArgs(cb, args);
    return CELL_OK;
}

static s32 check(u32 cb, u32 userdata, int ex, s32 status, const char* what)
{
    if (!s_init) return (s32)CELL_GAMEUPDATE_ERROR_NOT_INITIALIZED;
    if (!cb) return (s32)CELL_GAMEUPDATE_ERROR_INVALID_ADDR;
    printf("[cellGameUpdate] %s -> %s\n", what, status == CELL_GAMEUPDATE_RESULT_STATUS_NO_UPDATE ? "no update" : "finished");
    if (ex) return answer_ex(cb, userdata, status);
    answer(cb, userdata, status);
    return CELL_OK;
}

s32 cellGameUpdateInit(void)
{
    if (s_init) return (s32)CELL_GAMEUPDATE_ERROR_ALREADY_INITIALIZED;
    s_init = 1;
    return CELL_OK;
}

s32 cellGameUpdateTerm(void)
{
    if (!s_init) return (s32)CELL_GAMEUPDATE_ERROR_NOT_INITIALIZED;
    s_init = 0;
    return CELL_OK;
}

s32 cellGameUpdateCheckStartAsync(u32 param, u32 cb, u32 userdata)
{
    (void)param;
    return check(cb, userdata, 0, CELL_GAMEUPDATE_RESULT_STATUS_NO_UPDATE, "CheckStartAsync");
}

s32 cellGameUpdateCheckStartAsyncEx(u32 param, u32 cb, u32 userdata)
{
    (void)param;
    return check(cb, userdata, 1, CELL_GAMEUPDATE_RESULT_STATUS_NO_UPDATE, "CheckStartAsyncEx");
}

s32 cellGameUpdateCheckStartWithoutDialogAsync(u32 cb, u32 userdata)
{
    return check(cb, userdata, 0, CELL_GAMEUPDATE_RESULT_STATUS_NO_UPDATE, "CheckStartWithoutDialogAsync");
}

s32 cellGameUpdateCheckStartWithoutDialogAsyncEx(u32 cb, u32 userdata)
{
    return check(cb, userdata, 1, CELL_GAMEUPDATE_RESULT_STATUS_NO_UPDATE, "CheckStartWithoutDialogAsyncEx");
}

s32 cellGameUpdateCheckFinishAsync(u32 cb, u32 userdata)
{
    return check(cb, userdata, 0, CELL_GAMEUPDATE_RESULT_STATUS_FINISHED, "CheckFinishAsync");
}

s32 cellGameUpdateCheckFinishAsyncEx(u32 cb, u32 userdata)
{
    return check(cb, userdata, 1, CELL_GAMEUPDATE_RESULT_STATUS_FINISHED, "CheckFinishAsyncEx");
}

s32 cellGameUpdateCheckAbort(void)
{
    return s_init ? CELL_OK : (s32)CELL_GAMEUPDATE_ERROR_NOT_INITIALIZED;
}
