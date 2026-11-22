/*
 * ps3recomp - sceNpTus HLE
 *
 * NP Title User Storage: per-user cloud storage for game data
 * (leaderboards, stats, etc.). Offline stub that stores data locally.
 */

#ifndef PS3RECOMP_SCE_NP_TUS_H
#define PS3RECOMP_SCE_NP_TUS_H

#include "ps3emu/ps3types.h"
#include "ps3emu/error_codes.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---------------------------------------------------------------------------
 * Error codes
 * -----------------------------------------------------------------------*/
#define SCE_NP_TUS_ERROR_NOT_INITIALIZED       0x80022E01
#define SCE_NP_TUS_ERROR_ALREADY_INITIALIZED   0x80022E02
#define SCE_NP_TUS_ERROR_INVALID_ARGUMENT      0x80022E03
#define SCE_NP_TUS_ERROR_OUT_OF_MEMORY         0x80022E04
#define SCE_NP_TUS_ERROR_NOT_CONNECTED         0x80022E05
#define SCE_NP_TUS_ERROR_DATA_NOT_FOUND        0x80022E06
#define SCE_NP_TUS_ERROR_TIMEOUT               0x80022E07

/* ---------------------------------------------------------------------------
 * Constants
 * -----------------------------------------------------------------------*/
#define SCE_NP_TUS_MAX_SLOT_NUM           64
#define SCE_NP_TUS_DATA_INFO_MAX_SIZE     384
#define SCE_NP_TUS_MAX_CTX               8

/* ---------------------------------------------------------------------------
 * Types
 * -----------------------------------------------------------------------*/
typedef u32 SceNpTusCtx;
typedef u32 SceNpTusRequestId;

typedef struct SceNpTusVariable {
    s64 variable;
    u64 lastChangedDate;
    char ownerNpId[20];
    u8  hasData;
    u8  reserved[3];
} SceNpTusVariable;

typedef struct SceNpTusDataInfo {
    u32 infoSize;
    u8  data[SCE_NP_TUS_DATA_INFO_MAX_SIZE];
} SceNpTusDataInfo;

/* ---------------------------------------------------------------------------
 * Functions
 * -----------------------------------------------------------------------*/

/* Lifecycle */
s32 sceNpTusInit(s32 prio, s32 stackSize, void* option);
s32 sceNpTusTerm(void);

/* Context management */
s32 sceNpTusCreateCtx(SceNpTusCtx* ctx);
s32 sceNpTusDestroyCtx(SceNpTusCtx ctx);

/* Variable operations (leaderboard scores, etc.) */
s32 sceNpTusSetVariable(SceNpTusCtx ctx, u32 slotId, s64 variable,
                          void* option);
s32 sceNpTusGetVariable(SceNpTusCtx ctx, const void* npId, u32 slotId,
                          SceNpTusVariable* outVariable, void* option);
s32 sceNpTusTryAndSetVariable(SceNpTusCtx ctx, u32 slotId,
                                s32 opType, s64 variable,
                                SceNpTusVariable* outVariable, void* option);
s32 sceNpTusAddAndGetVariable(SceNpTusCtx ctx, u32 slotId,
                                s64 inVariable, SceNpTusVariable* outVariable,
                                void* option);
s32 sceNpTusDeleteMultiSlotVariable(SceNpTusCtx ctx, const u32* slotIds,
                                      u32 slotNum, void* option);

/* Title and transaction contexts, and data slots: the SDK's API. Every pointer is a guest address. A slot's data
 * is a file of its own (sceNpTus.c), so it lasts from run to run. */
s32 sceNpTusCreateTitleCtx(u32 communicationId, u32 passphrase, u32 selfNpId);
s32 sceNpTusDestroyTitleCtx(s32 titleCtxId);
s32 sceNpTusCreateTransactionCtx(s32 titleCtxId);
s32 sceNpTusDestroyTransactionCtx(s32 transId);
s32 sceNpTusSetTimeout(s32 ctxId, u32 timeout);
s32 sceNpTusAbortTransaction(s32 transId);
s32 sceNpTusPollAsync(s32 transId, u32 result);
s32 sceNpTusWaitAsync(s32 transId, u32 result);

/* (transId, targetNpId*, slotId, dataStatus*, dataStatusSize, data*, recvSize, option*) */
s32 sceNpTusGetData(s32 transId, u32 targetNpId, s32 slotId, u32 dataStatus, u32 dataStatusSize,
                    u32 data, u32 recvSize, u32 option);
s32 sceNpTusGetDataAsync(s32 transId, u32 targetNpId, s32 slotId, u32 dataStatus, u32 dataStatusSize,
                         u32 data, u32 recvSize, u32 option);
/* (transId, targetNpId*, slotId, totalSize, sendSize, data*, info*, infoStructSize, option*): the ninth argument is
 * on the guest stack and goes unread; it is reserved (NULL) in the SDK. */
s32 sceNpTusSetData(s32 transId, u32 targetNpId, s32 slotId, u32 totalSize, u32 sendSize, u32 data,
                    u32 info, u32 infoStructSize);
s32 sceNpTusSetDataAsync(s32 transId, u32 targetNpId, s32 slotId, u32 totalSize, u32 sendSize, u32 data,
                         u32 info, u32 infoStructSize);
/* (transId, targetNpId*, slotIdArray*, arrayNum, option*) */
s32 sceNpTusDeleteMultiSlotData(s32 transId, u32 targetNpId, u32 slotIdArray, s32 arrayNum, u32 option);
s32 sceNpTusDeleteMultiSlotDataAsync(s32 transId, u32 targetNpId, u32 slotIdArray, s32 arrayNum, u32 option);

#ifdef __cplusplus
}
#endif

#endif /* PS3RECOMP_SCE_NP_TUS_H */
