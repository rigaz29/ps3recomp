/*
 * ps3recomp - sceNpTus HLE implementation
 *
 * PSN's Title User Storage, answered locally. Data slots follow the SDK's
 * API (title and transaction contexts, Get/Set/DeleteMultiSlot data, sync
 * and async) and each one is a file, so it lasts from run to run (see the
 * section below). The variable calls further up keep their old in-memory
 * stand-ins.
 */

#include "sceNpTus.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "../../runtime/ppu/ppu_memory.h"   /* vm_write*: guest EA -> host, byte-swapped */
#include "../guest_struct.h"   /* GUEST_EA, vm_read/vm_write: guest EA -> host */
#include <time.h>
#ifdef _WIN32
#include <direct.h>
#define TUS_MKDIR(p) _mkdir(p)
#else
#include <sys/stat.h>
#define TUS_MKDIR(p) mkdir((p), 0755)
#endif

/* ---------------------------------------------------------------------------
 * Internal state
 * -----------------------------------------------------------------------*/

static int s_initialized = 0;

typedef struct {
    int in_use;
} TusCtxSlot;

static TusCtxSlot s_ctx[SCE_NP_TUS_MAX_CTX];

/* Local variable storage */
typedef struct {
    int valid;
    s64 value;
    u64 timestamp;
} TusVarSlot;

static TusVarSlot s_variables[SCE_NP_TUS_MAX_SLOT_NUM];

/* Local data storage */
typedef struct {
    int valid;
    u8* data;
    u32 dataSize;
    SceNpTusDataInfo info;
} TusDataSlot;

static TusDataSlot s_data[SCE_NP_TUS_MAX_SLOT_NUM];

/* ---------------------------------------------------------------------------
 * Lifecycle
 * -----------------------------------------------------------------------*/

s32 sceNpTusInit(s32 prio, s32 stackSize, void* option)
{
    (void)prio; (void)stackSize; (void)option;
    printf("[sceNpTus] Init()\n");

    if (s_initialized)
        return (s32)SCE_NP_TUS_ERROR_ALREADY_INITIALIZED;

    memset(s_ctx, 0, sizeof(s_ctx));
    memset(s_variables, 0, sizeof(s_variables));
    memset(s_data, 0, sizeof(s_data));
    s_initialized = 1;
    return CELL_OK;
}

s32 sceNpTusTerm(void)
{
    printf("[sceNpTus] Term()\n");

    if (!s_initialized)
        return (s32)SCE_NP_TUS_ERROR_NOT_INITIALIZED;

    /* Free allocated data blobs */
    for (int i = 0; i < SCE_NP_TUS_MAX_SLOT_NUM; i++) {
        if (s_data[i].valid && s_data[i].data) {
            free(s_data[i].data);
            s_data[i].data = NULL;
        }
    }

    s_initialized = 0;
    return CELL_OK;
}

/* ---------------------------------------------------------------------------
 * Context management
 * -----------------------------------------------------------------------*/

s32 sceNpTusCreateCtx(SceNpTusCtx* ctx)
{
    printf("[sceNpTus] CreateCtx()\n");

    if (!s_initialized)
        return (s32)SCE_NP_TUS_ERROR_NOT_INITIALIZED;
    if (!ctx)
        return (s32)SCE_NP_TUS_ERROR_INVALID_ARGUMENT;

    for (int i = 0; i < SCE_NP_TUS_MAX_CTX; i++) {
        if (!s_ctx[i].in_use) {
            s_ctx[i].in_use = 1;
            vm_write32((u32)(uintptr_t)ctx, (u32)i);
            return CELL_OK;
        }
    }
    return (s32)SCE_NP_TUS_ERROR_OUT_OF_MEMORY;
}

s32 sceNpTusDestroyCtx(SceNpTusCtx ctx)
{
    printf("[sceNpTus] DestroyCtx(ctx=%u)\n", ctx);

    if (ctx >= SCE_NP_TUS_MAX_CTX || !s_ctx[ctx].in_use)
        return (s32)SCE_NP_TUS_ERROR_INVALID_ARGUMENT;

    s_ctx[ctx].in_use = 0;
    return CELL_OK;
}

/* ---------------------------------------------------------------------------
 * Variable operations
 * -----------------------------------------------------------------------*/

s32 sceNpTusSetVariable(SceNpTusCtx ctx, u32 slotId, s64 variable,
                          void* option)
{
    (void)ctx; (void)option;

    printf("[sceNpTus] SetVariable(slot=%u, val=%lld)\n",
           slotId, (long long)variable);

    if (!s_initialized)
        return (s32)SCE_NP_TUS_ERROR_NOT_INITIALIZED;
    if (slotId >= SCE_NP_TUS_MAX_SLOT_NUM)
        return (s32)SCE_NP_TUS_ERROR_INVALID_ARGUMENT;

    s_variables[slotId].valid = 1;
    s_variables[slotId].value = variable;
    s_variables[slotId].timestamp++;
    return CELL_OK;
}

/* SceNpTusVariable is { s64 variable; u64 lastChangedDate; char ownerNpId[20];
 * u8 hasData; u8 reserved[3]; }. The two 64-bit fields rule out a
 * word-at-a-time copy -- big-endian puts the high word first and a
 * little-endian host the low one -- so it goes out field by field. */
static void tus_variable_store(u32 ea, const SceNpTusVariable* h)
{
    if (!ea)
        return;
    vm_write64(ea + (u32)offsetof(SceNpTusVariable, variable),        (u64)h->variable);
    vm_write64(ea + (u32)offsetof(SceNpTusVariable, lastChangedDate), h->lastChangedDate);
    memcpy(vm_base + ea + offsetof(SceNpTusVariable, ownerNpId),
           h->ownerNpId, sizeof(h->ownerNpId));
    vm_write8(ea + (u32)offsetof(SceNpTusVariable, hasData), h->hasData);
    for (u32 i = 0; i < sizeof(h->reserved); i++)
        vm_write8(ea + (u32)offsetof(SceNpTusVariable, reserved) + i, 0);
}

s32 sceNpTusGetVariable(SceNpTusCtx ctx, const void* npId, u32 slotId,
                          SceNpTusVariable* outVariable, void* option)
{
    (void)ctx; (void)npId; (void)option;

    if (!s_initialized)
        return (s32)SCE_NP_TUS_ERROR_NOT_INITIALIZED;
    if (slotId >= SCE_NP_TUS_MAX_SLOT_NUM || !outVariable)
        return (s32)SCE_NP_TUS_ERROR_INVALID_ARGUMENT;

    if (!s_variables[slotId].valid)
        return (s32)SCE_NP_TUS_ERROR_DATA_NOT_FOUND;

    SceNpTusVariable hv;
    memset(&hv, 0, sizeof(hv));
    hv.variable        = s_variables[slotId].value;
    hv.lastChangedDate = s_variables[slotId].timestamp;
    hv.hasData         = 1;
    strncpy(hv.ownerNpId, "ps3recomp_user", sizeof(hv.ownerNpId) - 1);
    tus_variable_store(GUEST_EA(outVariable), &hv);
    return CELL_OK;
}

s32 sceNpTusTryAndSetVariable(SceNpTusCtx ctx, u32 slotId,
                                s32 opType, s64 variable,
                                SceNpTusVariable* outVariable, void* option)
{
    (void)opType;
    s32 rc = sceNpTusSetVariable(ctx, slotId, variable, option);
    if (rc == CELL_OK && outVariable) {
        sceNpTusGetVariable(ctx, NULL, slotId, outVariable, option);
    }
    return rc;
}

s32 sceNpTusAddAndGetVariable(SceNpTusCtx ctx, u32 slotId,
                                s64 inVariable, SceNpTusVariable* outVariable,
                                void* option)
{
    if (slotId >= SCE_NP_TUS_MAX_SLOT_NUM)
        return (s32)SCE_NP_TUS_ERROR_INVALID_ARGUMENT;

    s64 current = s_variables[slotId].valid ? s_variables[slotId].value : 0;
    return sceNpTusTryAndSetVariable(ctx, slotId, 0, current + inVariable,
                                      outVariable, option);
}

s32 sceNpTusDeleteMultiSlotVariable(SceNpTusCtx ctx, const u32* slotIds,
                                      u32 slotNum, void* option)
{
    (void)ctx; (void)option;

    if (!slotIds)
        return (s32)SCE_NP_TUS_ERROR_INVALID_ARGUMENT;

    for (u32 i = 0; i < slotNum; i++) {
        if (slotIds[i] < SCE_NP_TUS_MAX_SLOT_NUM)
            s_variables[slotIds[i]].valid = 0;
    }
    return CELL_OK;
}

/* ---------------------------------------------------------------------------
 * Title and transaction contexts, data slots (the SDK's API)
 *
 * TUS is PSN's per-player storage: a title keeps small blobs in numbered
 * slots of a player's account (God of War: Ascension keeps the multiplayer
 * profile there). Here a slot is a file:
 *   <PS3_NP_TUS_DIR, else np_tus beside the exe>/<communication id>/<online id>/slot<n>.tus
 * holding "TUS1", the info's size and 384 bytes, the last change (CellRtcTick),
 * the data's size and the data. Requests finish at the call; PollAsync and
 * WaitAsync report 0 (done) and the request's result.
 *
 * Layouts (SDK ABI, big-endian):
 *   SceNpTusDataInfo   { u32 infoSize; u8 pad[4]; u8 data[384]; }            392 bytes
 *   SceNpTusDataStatus { SceNpId ownerId (36); s32 hasData; CellRtcTick lastChangedDate (+40);
 *                        SceNpId lastChangedAuthorId (+48); void* data (+84); u32 dataSize (+88);
 *                        u8 pad[4]; SceNpTusDataInfo info (+96); }          488 bytes
 * -----------------------------------------------------------------------*/

#define TUS_TITLE_MAX 8
#define TUS_TRANS_MAX 32
#define TUS_INFO_MAX  384
#define TUS_STATUS_SIZE 488
#define TUS_ERROR_INVALID_ID 0x8002A106u   /* SCE_NP_COMMUNITY_ERROR_INVALID_ID */
#define TUS_ERROR_TOO_MANY   0x8002A10Bu   /* SCE_NP_COMMUNITY_ERROR_TOO_MANY_OBJECTS */
#define TUS_ERROR_ARG        0x8002A103u   /* SCE_NP_COMMUNITY_ERROR_INVALID_ARGUMENT */

static struct { int used; char comm[16]; } s_tus_title[TUS_TITLE_MAX + 1];
static struct { int used; int title; s32 result; } s_tus_trans[TUS_TRANS_MAX + 1];

static int tus_trans_ok(s32 id) { return id >= 1 && id <= TUS_TRANS_MAX && s_tus_trans[id].used; }

/* A name safe as one folder: letters, digits, '-', '_'; anything else '_'. */
static void tus_safe(char* out, size_t n, const char* in, size_t max)
{
    size_t k = 0;
    for (size_t i = 0; i < max && in[i] && k + 1 < n; i++) {
        const char c = in[i];
        out[k++] = ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_')
                 ? c : '_';
    }
    out[k] = '\0';
    if (!k) snprintf(out, n, "_");
}

static void tus_mkdirs(const char* path)
{
    char p[1024];
    snprintf(p, sizeof(p), "%s", path);
    for (char* c = p + 1; *c; c++) {
        if (*c == '/' || *c == '\\') {
            const char keep = *c;
            *c = '\0';
            TUS_MKDIR(p);
            *c = keep;
        }
    }
    TUS_MKDIR(p);
}

/* The slot's file for the title of transaction transId and the player npId (guest SceNpId). */
static int tus_path(s32 transId, u32 npId, s32 slot, char* out, size_t n, int make)
{
    const char* base = getenv("PS3_NP_TUS_DIR");
    char owner[32], dir[1024];
    tus_safe(owner, sizeof(owner), npId ? (const char*)vm_ptr8(npId) : "", 16);
    snprintf(dir, sizeof(dir), "%s/%s/%s", (base && *base) ? base : "np_tus",
             s_tus_title[s_tus_trans[transId].title].comm, owner);
    if (make) tus_mkdirs(dir);
    return snprintf(out, n, "%s/slot%d.tus", dir, slot) < (int)n;
}

static u64 tus_now_tick(void)
{
    return (u64)time(NULL) * 1000000ull + 62135596800000000ull;   /* CellRtcTick: microseconds since 0001-01-01 */
}

s32 sceNpTusCreateTitleCtx(u32 communicationId, u32 passphrase, u32 selfNpId)
{
    (void)passphrase; (void)selfNpId;
    if (!s_initialized) return (s32)SCE_NP_TUS_ERROR_NOT_INITIALIZED;
    if (!communicationId) return (s32)TUS_ERROR_ARG;
    for (int i = 1; i <= TUS_TITLE_MAX; i++) {
        if (!s_tus_title[i].used) {
            /* SceNpCommunicationId { char data[9]; char term; u8 num; char dummy; } -> "NPWR00802_00" */
            char id[10];
            memcpy(id, vm_ptr8(communicationId), 9);
            id[9] = '\0';
            char safe[12];
            tus_safe(safe, sizeof(safe), id, 9);
            snprintf(s_tus_title[i].comm, sizeof(s_tus_title[i].comm), "%s_%02u", safe, vm_read8(communicationId + 10));
            s_tus_title[i].used = 1;
            printf("[sceNpTus] CreateTitleCtx(%s) -> %d\n", s_tus_title[i].comm, i);
            return i;
        }
    }
    return (s32)TUS_ERROR_TOO_MANY;
}

s32 sceNpTusDestroyTitleCtx(s32 titleCtxId)
{
    if (titleCtxId < 1 || titleCtxId > TUS_TITLE_MAX || !s_tus_title[titleCtxId].used)
        return (s32)TUS_ERROR_INVALID_ID;
    for (int t = 1; t <= TUS_TRANS_MAX; t++)
        if (s_tus_trans[t].used && s_tus_trans[t].title == titleCtxId) s_tus_trans[t].used = 0;
    s_tus_title[titleCtxId].used = 0;
    return CELL_OK;
}

s32 sceNpTusCreateTransactionCtx(s32 titleCtxId)
{
    if (titleCtxId < 1 || titleCtxId > TUS_TITLE_MAX || !s_tus_title[titleCtxId].used)
        return (s32)TUS_ERROR_INVALID_ID;
    for (int i = 1; i <= TUS_TRANS_MAX; i++) {
        if (!s_tus_trans[i].used) {
            s_tus_trans[i].used = 1;
            s_tus_trans[i].title = titleCtxId;
            s_tus_trans[i].result = 0;
            return i;
        }
    }
    return (s32)TUS_ERROR_TOO_MANY;
}

s32 sceNpTusDestroyTransactionCtx(s32 transId)
{
    if (!tus_trans_ok(transId)) return (s32)TUS_ERROR_INVALID_ID;
    s_tus_trans[transId].used = 0;
    return CELL_OK;
}

s32 sceNpTusSetTimeout(s32 ctxId, u32 timeout)
{
    (void)ctxId; (void)timeout;
    return CELL_OK;
}

s32 sceNpTusAbortTransaction(s32 transId)
{
    return tus_trans_ok(transId) ? CELL_OK : (s32)TUS_ERROR_INVALID_ID;
}

s32 sceNpTusPollAsync(s32 transId, u32 result)
{
    if (!tus_trans_ok(transId)) return (s32)TUS_ERROR_INVALID_ID;
    if (result) vm_write32(result, (u32)s_tus_trans[transId].result);
    return 0;   /* finished */
}

s32 sceNpTusWaitAsync(s32 transId, u32 result)
{
    return sceNpTusPollAsync(transId, result);
}

static s32 tus_get(s32 transId, u32 targetNpId, s32 slotId, u32 dataStatus, u32 dataStatusSize, u32 data, u32 recvSize)
{
    if (!tus_trans_ok(transId)) return (s32)TUS_ERROR_INVALID_ID;
    if (!targetNpId || slotId < 0) return (s32)TUS_ERROR_ARG;
    char path[1100];
    u8 head[4 + 4 + TUS_INFO_MAX + 8 + 4];
    u32 info_size = 0, size = 0;
    u64 changed = 0;
    int has = 0;
    if (tus_path(transId, targetNpId, slotId, path, sizeof(path), 0)) {
        FILE* f = fopen(path, "rb");
        if (f) {
            if (fread(head, 1, sizeof(head), f) == sizeof(head) && memcmp(head, "TUS1", 4) == 0) {
                info_size = ((u32)head[4] << 24) | ((u32)head[5] << 16) | ((u32)head[6] << 8) | head[7];
                for (int i = 0; i < 8; i++) changed = (changed << 8) | head[8 + TUS_INFO_MAX + i];
                const u8* z = head + 8 + TUS_INFO_MAX + 8;
                size = ((u32)z[0] << 24) | ((u32)z[1] << 16) | ((u32)z[2] << 8) | z[3];
                has = 1;
                if (data && recvSize) {
                    const u32 want = size < recvSize ? size : recvSize;
                    if (fread(vm_ptr8(data), 1, want, f) != want) has = 0;
                }
            }
            fclose(f);
        }
    }
    if (dataStatus && dataStatusSize >= TUS_STATUS_SIZE) {
        memset(vm_ptr8(dataStatus), 0, TUS_STATUS_SIZE);
        memcpy(vm_ptr8(dataStatus), vm_ptr8(targetNpId), 36);              /* ownerId */
        vm_write32(dataStatus + 36, (u32)has);
        if (has) {
            vm_write64(dataStatus + 40, changed);
            memcpy(vm_ptr8(dataStatus + 48), vm_ptr8(targetNpId), 36);     /* lastChangedAuthorId */
            vm_write32(dataStatus + 84, data);
            vm_write32(dataStatus + 88, size);
            vm_write32(dataStatus + 96, info_size > TUS_INFO_MAX ? TUS_INFO_MAX : info_size);
            memcpy(vm_ptr8(dataStatus + 104), head + 8, TUS_INFO_MAX);
        }
    }
    printf("[sceNpTus] GetData(slot %d of %.16s) -> %s, %u bytes\n", slotId, (const char*)vm_ptr8(targetNpId),
           has ? "found" : "empty", size);
    return CELL_OK;
}

static s32 tus_set(s32 transId, u32 targetNpId, s32 slotId, u32 totalSize, u32 sendSize, u32 data, u32 info,
                   u32 infoStructSize)
{
    if (!tus_trans_ok(transId)) return (s32)TUS_ERROR_INVALID_ID;
    if (!targetNpId || slotId < 0 || (sendSize && !data) || sendSize > totalSize) return (s32)TUS_ERROR_ARG;
    char path[1100];
    if (!tus_path(transId, targetNpId, slotId, path, sizeof(path), 1)) return (s32)TUS_ERROR_ARG;
    u8 head[4 + 4 + TUS_INFO_MAX + 8 + 4];
    memset(head, 0, sizeof(head));
    memcpy(head, "TUS1", 4);
    u32 info_size = 0;
    if (info && infoStructSize >= 8) {
        info_size = vm_read32(info);
        if (info_size > TUS_INFO_MAX) info_size = TUS_INFO_MAX;
        if (infoStructSize >= 8 + info_size) memcpy(head + 8, vm_ptr8(info + 8), info_size);
    }
    head[4] = (u8)(info_size >> 24); head[5] = (u8)(info_size >> 16); head[6] = (u8)(info_size >> 8); head[7] = (u8)info_size;
    const u64 now = tus_now_tick();
    for (int i = 0; i < 8; i++) head[8 + TUS_INFO_MAX + i] = (u8)(now >> (56 - 8 * i));
    u8* z = head + 8 + TUS_INFO_MAX + 8;
    z[0] = (u8)(sendSize >> 24); z[1] = (u8)(sendSize >> 16); z[2] = (u8)(sendSize >> 8); z[3] = (u8)sendSize;
    FILE* f = fopen(path, "wb");
    if (!f) {
        printf("[sceNpTus] SetData(slot %d): cannot write %s\n", slotId, path);
        return (s32)SCE_NP_TUS_ERROR_OUT_OF_MEMORY;
    }
    int ok = fwrite(head, 1, sizeof(head), f) == sizeof(head);
    if (sendSize) ok = ok && fwrite(vm_ptr8(data), 1, sendSize, f) == sendSize;
    ok = (fclose(f) == 0) && ok;
    printf("[sceNpTus] SetData(slot %d of %.16s, %u bytes) -> %s\n", slotId, (const char*)vm_ptr8(targetNpId), sendSize,
           ok ? "kept" : "write failed");
    return ok ? CELL_OK : (s32)SCE_NP_TUS_ERROR_OUT_OF_MEMORY;
}

s32 sceNpTusGetData(s32 transId, u32 targetNpId, s32 slotId, u32 dataStatus, u32 dataStatusSize,
                    u32 data, u32 recvSize, u32 option)
{
    (void)option;
    return tus_get(transId, targetNpId, slotId, dataStatus, dataStatusSize, data, recvSize);
}

s32 sceNpTusGetDataAsync(s32 transId, u32 targetNpId, s32 slotId, u32 dataStatus, u32 dataStatusSize,
                         u32 data, u32 recvSize, u32 option)
{
    (void)option;
    if (!tus_trans_ok(transId)) return (s32)TUS_ERROR_INVALID_ID;
    s_tus_trans[transId].result = tus_get(transId, targetNpId, slotId, dataStatus, dataStatusSize, data, recvSize);
    return CELL_OK;
}

s32 sceNpTusSetData(s32 transId, u32 targetNpId, s32 slotId, u32 totalSize, u32 sendSize, u32 data,
                    u32 info, u32 infoStructSize)
{
    return tus_set(transId, targetNpId, slotId, totalSize, sendSize, data, info, infoStructSize);
}

s32 sceNpTusSetDataAsync(s32 transId, u32 targetNpId, s32 slotId, u32 totalSize, u32 sendSize, u32 data,
                         u32 info, u32 infoStructSize)
{
    if (!tus_trans_ok(transId)) return (s32)TUS_ERROR_INVALID_ID;
    s_tus_trans[transId].result = tus_set(transId, targetNpId, slotId, totalSize, sendSize, data, info, infoStructSize);
    return CELL_OK;
}

static s32 tus_delete(s32 transId, u32 targetNpId, u32 slotIdArray, s32 arrayNum)
{
    if (!tus_trans_ok(transId)) return (s32)TUS_ERROR_INVALID_ID;
    if (!targetNpId || (arrayNum > 0 && !slotIdArray)) return (s32)TUS_ERROR_ARG;
    for (s32 i = 0; i < arrayNum; i++) {
        char path[1100];
        if (tus_path(transId, targetNpId, (s32)vm_read32(slotIdArray + 4u * (u32)i), path, sizeof(path), 0))
            remove(path);
    }
    return CELL_OK;
}

s32 sceNpTusDeleteMultiSlotData(s32 transId, u32 targetNpId, u32 slotIdArray, s32 arrayNum, u32 option)
{
    (void)option;
    return tus_delete(transId, targetNpId, slotIdArray, arrayNum);
}

s32 sceNpTusDeleteMultiSlotDataAsync(s32 transId, u32 targetNpId, u32 slotIdArray, s32 arrayNum, u32 option)
{
    (void)option;
    if (!tus_trans_ok(transId)) return (s32)TUS_ERROR_INVALID_ID;
    s_tus_trans[transId].result = tus_delete(transId, targetNpId, slotIdArray, arrayNum);
    return CELL_OK;
}
