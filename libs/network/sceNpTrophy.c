/*
 * ps3recomp - sceNpTrophy HLE implementation
 *
 * Trophy management with persistent storage.  Unlocked trophies are saved
 * to a JSON file, {commId}.json, beside the saves (sceNpTrophyStorageFile).
 *
 * JSON format (hand-written, no external dependency):
 * { "trophies": [ { "id": N, "timestamp": T }, ... ] }
 *
 * The set itself -- how many trophies, their grades, names and icons -- comes
 * from the title's own TROPHY.TRP (trophy_set.h), found where the PS3 finds
 * it when the title registers its context. With it the platinum is given as
 * the PS3 gives it: when the last trophy that counts toward it is unlocked
 * (less those a port marks as unobtainable), and a port can show each unlock
 * (sceNpTrophy_unlock_observer). Without it the trophies are numbers only.
 */

#include "sceNpTrophy.h"
#include "../system/cellSysutil.h"
#include "../../runtime/syscalls/sys_fs.h"   /* sys_fs_translate_path: a guest path's host file */
#include "ps3emu/guest_call.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <stdint.h>
#include "../../runtime/ppu/ppu_memory.h"   /* vm_base (guest mem) */
#include "ps3emu/endian.h"                   /* ps3_bswap64 -- guest is big-endian */
#include "../guest_struct.h"   /* GUEST_EA, guest_struct_load/store */
/* HLE args arrive as guest effective addresses; translate before deref. */
#define GUEST_PTR(p, T) ((T)((p) ? (void*)(vm_base + (uint32_t)(uintptr_t)(p)) : (void*)0))

#ifdef _WIN32
#include <direct.h>
#include <io.h>
#define mkdir_p(path) _mkdir(path)
#define PATH_SEP '\\'
#else
#include <sys/stat.h>
#include <unistd.h>
#define mkdir_p(path) mkdir(path, 0755)
#define PATH_SEP '/'
#endif

/* ---------------------------------------------------------------------------
 * Internal state
 * -----------------------------------------------------------------------*/

static int s_trophy_initialized = 0;
static char s_storage_path[512] = "gamedata/trophies";
static int  s_storage_path_set = 0;

/* The folder the unlocks are kept in, as cellSaveData.c picks the saves': $PS3_GAMEDATA_ROOT/trophies, else
 * beside the saves under $PS3_HDD0_ROOT (where the PS3 keeps its trophies, /dev_hdd0/home/<user>/trophy), else
 * the cwd-relative default -- which moved with whatever folder the game was started from. */
static void trophy_storage_dir(const char* hdd0, char* out, size_t cap)
{
    const char* env = getenv("PS3_GAMEDATA_ROOT");
    if (!hdd0) hdd0 = getenv("PS3_HDD0_ROOT");
    if (env && *env)
        snprintf(out, cap, "%s/trophies", env);
    else if (hdd0 && *hdd0)
        snprintf(out, cap, "%s/home/00000001/trophy", hdd0);
    else
        snprintf(out, cap, "gamedata/trophies");
}

/* An explicit sceNpTrophySetStoragePath still wins. */
static void trophy_adopt_gamedata_root(void)
{
    static int s_done = 0;
    if (s_done || s_storage_path_set) return;
    s_done = 1;
    trophy_storage_dir(NULL, s_storage_path, sizeof(s_storage_path));
}

static int trophy_file_exists(const char* path);

void sceNpTrophyStorageFile(const char* hdd0, const char* npcommid, int read, char* out, size_t cap)
{
    char dir[512];
    trophy_storage_dir(hdd0, dir, sizeof dir);
    snprintf(out, cap, "%s/%s.json", dir, npcommid);
    if (read && !trophy_file_exists(out)) {
        char old[600];
        snprintf(old, sizeof old, "gamedata/trophies/%s.json", npcommid);
        if (trophy_file_exists(old))
            snprintf(out, cap, "%s", old);
    }
}

/* Per-context data */
typedef struct {
    int                      in_use;
    int                      registered;
    SceNpCommunicationId     commId;
    u8                       unlocked[SCE_NP_TROPHY_MAX_NUM_TROPHIES];
    u64                      unlock_time[SCE_NP_TROPHY_MAX_NUM_TROPHIES];
    u32                      total_trophies;
    PS3TrophySet*            set;           /* from the title's TROPHY.TRP, or NULL */
} TrophyContext;

typedef struct {
    int in_use;
} TrophyHandle;

/* ID zero is the firmware invalid sentinel; retain all four usable slots. */
static TrophyContext s_contexts[SCE_NP_TROPHY_MAX_CONTEXTS + 1];
static TrophyHandle  s_handles[SCE_NP_TROPHY_MAX_HANDLES + 1];

/* ---------------------------------------------------------------------------
 * Persistent storage helpers
 * -----------------------------------------------------------------------*/

static void trophy_ensure_dir(void)
{
    char tmp[512];
    char* p;

    trophy_adopt_gamedata_root();
    strncpy(tmp, s_storage_path, sizeof(tmp) - 1);
    tmp[sizeof(tmp) - 1] = '\0';

    /* Create directories recursively */
    for (p = tmp + 1; *p; p++) {
        if (*p == '/' || *p == '\\') {
            *p = '\0';
            mkdir_p(tmp);
            *p = PATH_SEP;
        }
    }
    mkdir_p(tmp);
}

static void trophy_get_filepath(const SceNpCommunicationId* commId,
                                 char* buf, size_t bufsize)
{
    trophy_adopt_gamedata_root();
    snprintf(buf, bufsize, "%s/%s.json", s_storage_path, commId->data);
}

static void trophy_save(TrophyContext* ctx)
{
    char filepath[1024];
    FILE* fp;
    int first = 1;

    trophy_ensure_dir();
    trophy_get_filepath(&ctx->commId, filepath, sizeof(filepath));

    fp = fopen(filepath, "w");
    if (!fp) {
        printf("[sceNpTrophy] WARNING: Could not save trophies to %s\n",
               filepath);
        return;
    }

    fprintf(fp, "{\n  \"trophies\": [");
    for (u32 i = 0; i < ctx->total_trophies; i++) {
        if (ctx->unlocked[i]) {
            if (!first) fprintf(fp, ",");
            fprintf(fp, "\n    { \"id\": %u, \"timestamp\": %llu }",
                    i, (unsigned long long)ctx->unlock_time[i]);
            first = 0;
        }
    }
    fprintf(fp, "\n  ]\n}\n");
    fclose(fp);
}

static void trophy_load(TrophyContext* ctx)
{
    char filepath[1024];
    FILE* fp;
    char line[256];

    trophy_get_filepath(&ctx->commId, filepath, sizeof(filepath));

    fp = fopen(filepath, "r");
    if (!fp && !s_storage_path_set) {
        /* kept by an older build in ./gamedata/trophies: read once, then saved in the new place */
        snprintf(filepath, sizeof(filepath), "gamedata/trophies/%.*s.json",
                 SCE_NP_COMMUNICATION_ID_MAX_LENGTH, ctx->commId.data);
        fp = fopen(filepath, "r");
    }
    if (!fp)
        return; /* no saved data, all locked */

    /*
     * Simple JSON parser -- look for "id": N and "timestamp": T patterns.
     * This avoids any external JSON library dependency.
     */
    while (fgets(line, sizeof(line), fp)) {
        const char* id_pos = strstr(line, "\"id\":");
        if (id_pos) {
            u32 tid = 0;
            u64 ts = 0;
            const char* ts_pos;

            if (sscanf(id_pos, "\"id\": %u", &tid) == 1 &&
                tid < SCE_NP_TROPHY_MAX_NUM_TROPHIES) {
                ts_pos = strstr(line, "\"timestamp\":");
                if (ts_pos)
                    sscanf(ts_pos, "\"timestamp\": %llu",
                           (unsigned long long*)&ts);
                ctx->unlocked[tid] = 1;
                ctx->unlock_time[tid] = ts;
            }
        }
    }
    fclose(fp);

    printf("[sceNpTrophy] Loaded trophy data from %s\n", filepath);
}

static u32 trophy_count_unlocked(TrophyContext* ctx)
{
    u32 count = 0;
    for (u32 i = 0; i < ctx->total_trophies; i++)
        if (ctx->unlocked[i]) count++;
    return count;
}

/* ---------------------------------------------------------------------------
 * The title's set, the platinum, and the port's observer
 * -----------------------------------------------------------------------*/

void (*sceNpTrophy_unlock_observer)(const PS3TrophySet* set, int id) = NULL;

#define TROPHY_UNOBTAINABLE_SETS 4
#define TROPHY_UNOBTAINABLE_IDS  16
static struct {
    char commid[SCE_NP_COMMUNICATION_ID_MAX_LENGTH + 1];
    int  ids[TROPHY_UNOBTAINABLE_IDS];
    int  n;
} s_unobtainable[TROPHY_UNOBTAINABLE_SETS];

void sceNpTrophySetUnobtainable(const char* npcommid, const int* ids, int n)
{
    const char* key = npcommid && *npcommid ? npcommid : "*";   /* "*": any set */
    for (int k = 0; k < TROPHY_UNOBTAINABLE_SETS; k++) {
        if (s_unobtainable[k].commid[0] && strcmp(s_unobtainable[k].commid, key)) continue;
        snprintf(s_unobtainable[k].commid, sizeof s_unobtainable[k].commid, "%s", key);
        s_unobtainable[k].n = 0;
        for (int i = 0; ids && i < n && i < TROPHY_UNOBTAINABLE_IDS; i++)
            s_unobtainable[k].ids[s_unobtainable[k].n++] = ids[i];
        return;
    }
}

int sceNpTrophyIsUnobtainable(const char* npcommid, int id)
{
    for (int k = 0; k < TROPHY_UNOBTAINABLE_SETS; k++) {
        if (!s_unobtainable[k].commid[0]) continue;
        if (strcmp(s_unobtainable[k].commid, "*") &&
            (!npcommid || strncmp(npcommid, s_unobtainable[k].commid, SCE_NP_COMMUNICATION_ID_MAX_LENGTH)))
            continue;
        for (int i = 0; i < s_unobtainable[k].n; i++)
            if (s_unobtainable[k].ids[i] == id) return 1;
    }
    return 0;
}

static int trophy_unobtainable(const TrophyContext* ctx, int id)
{
    return sceNpTrophyIsUnobtainable(ctx->commId.data, id);
}

static int trophy_file_exists(const char* path)
{
    FILE* f = fopen(path, "rb");
    if (!f) return 0;
    fclose(f);
    return 1;
}

/* The title's TROPHY.TRP for its communication id, where the PS3 looks for it: TROPDIR in the application's
 * own folder (the update's, for a disc booted from it: PS3_BOOT_PATH), then the disc's. A host path into out;
 * 0 if there is one. PS3_TROPHY_TRP names the file outright. */
static int trophy_find_trp(const SceNpCommunicationId* id, char* out, size_t cap)
{
    const char* forced = getenv("PS3_TROPHY_TRP");
    if (forced && *forced) {
        snprintf(out, cap, "%s", forced);
        return trophy_file_exists(out) ? 0 : -1;
    }
    char dir[32], guest[1024];
    snprintf(dir, sizeof dir, "%.*s_%02u", SCE_NP_COMMUNICATION_ID_MAX_LENGTH, id->data, (unsigned)id->num);
    const char* boot = getenv("PS3_BOOT_PATH");
    const char* usr = boot ? strstr(boot, "/USRDIR/") : NULL;
    if (usr) {
        snprintf(guest, sizeof guest, "%.*s/TROPDIR/%s/TROPHY.TRP", (int)(usr - boot), boot, dir);
        sys_fs_translate_path(guest, out, (int)cap);
        if (trophy_file_exists(out)) return 0;
    }
    static const char* const roots[] = { "/dev_bdvd/PS3_GAME", "/app_home" };
    for (unsigned i = 0; i < sizeof roots / sizeof roots[0]; i++) {
        snprintf(guest, sizeof guest, "%s/TROPDIR/%s/TROPHY.TRP", roots[i], dir);
        sys_fs_translate_path(guest, out, (int)cap);
        if (trophy_file_exists(out)) return 0;
    }
    return -1;
}

/* The set of a context being registered: its count, grades and names, in the title's language. */
static void trophy_load_set(TrophyContext* ctx)
{
    char trp[1024];
    if (trophy_find_trp(&ctx->commId, trp, sizeof trp)) {
        printf("[sceNpTrophy] no TROPHY.TRP for %s: its trophies are numbers only\n", ctx->commId.data);
        return;
    }
    PS3TrophySet* s = (PS3TrophySet*)calloc(1, sizeof *s);
    if (!s) return;
    if (ps3_trophy_set_load(trp, (int)cellSysutil_language(), s)) {
        printf("[sceNpTrophy] %s could not be read: its trophies are numbers only\n", trp);
        free(s);
        return;
    }
    free(ctx->set);
    ctx->set = s;
    ctx->total_trophies = (u32)s->count;
    int g[5] = {0, 0, 0, 0, 0};
    for (int i = 0; i < s->count; i++)
        if (s->t[i].grade >= 1 && s->t[i].grade <= 4) g[s->t[i].grade]++;
    printf("[sceNpTrophy] %s: \"%s\", %d trophies (%d platinum, %d gold, %d silver, %d bronze), %u unlocked; "
           "names in %s\n", trp, s->title, s->count, g[1], g[2], g[3], g[4], trophy_count_unlocked(ctx),
           s->lang < 0 ? "the set's own language" : "the system's");
}

/* The platinum that every trophy counting toward it now completes -- less the ones that cannot be had here
 * (sceNpTrophySetUnobtainable) -- if it is still locked: its id, else -1. */
static int trophy_platinum_due(const TrophyContext* ctx)
{
    const PS3TrophySet* s = ctx->set;
    if (!s) return -1;
    for (int p = 0; p < s->count; p++) {
        if (s->t[p].grade != SCE_NP_TROPHY_GRADE_PLATINUM || ctx->unlocked[p]) continue;
        int counted = 0, all = 1;
        for (int i = 0; i < s->count && all; i++) {
            if (i == p || !s->t[i].grade || s->t[i].pid != p || trophy_unobtainable(ctx, i)) continue;
            counted++;
            if (!ctx->unlocked[i]) all = 0;
        }
        if (all && counted) return p;
    }
    return -1;
}

/* Trophy id unlocked now: saved, said in the log, and told to the port. */
static void trophy_mark(TrophyContext* ctx, int id)
{
    ctx->unlocked[id] = 1;
    ctx->unlock_time[id] = (u64)time(NULL);
    trophy_save(ctx);
    const PS3TrophyEntry* t = ps3_trophy_set_entry(ctx->set, id);
    if (t)
        printf("[sceNpTrophy] trophy unlocked: %d %s (%s)\n", id, t->name, ps3_trophy_grade_name(t->grade));
    else
        printf("[sceNpTrophy] trophy unlocked: %d\n", id);
    if (t && sceNpTrophy_unlock_observer)
        sceNpTrophy_unlock_observer(ctx->set, id);
}

/* A NUL-terminated string into a guest char[cap] at ea, the rest of it zeroed. */
static void trophy_put_str(u32 ea, const char* s, u32 cap)
{
    u32 n = (u32)strlen(s);
    if (n >= cap) n = cap - 1;
    memcpy(vm_base + ea, s, n);
    memset(vm_base + ea + n, 0, cap - n);
}

/* ---------------------------------------------------------------------------
 * API implementations
 * -----------------------------------------------------------------------*/

void sceNpTrophySetStoragePath(const char* path)
{
    path = GUEST_PTR(path, const char*);
    if (path) {
        strncpy(s_storage_path, path, sizeof(s_storage_path) - 1);
        s_storage_path[sizeof(s_storage_path) - 1] = '\0';
        s_storage_path_set = 1;
    }
}

s32 sceNpTrophyInit(void* poolPtr, u32 poolSize, u32 containerId, u64 options)
{
    (void)poolPtr; (void)poolSize; (void)containerId; (void)options;

    printf("[sceNpTrophy] Init()\n");

    if (s_trophy_initialized)
        return SCE_NP_TROPHY_ERROR_ALREADY_INITIALIZED;

    memset(s_contexts, 0, sizeof(s_contexts));
    memset(s_handles, 0, sizeof(s_handles));
    s_trophy_initialized = 1;
    return CELL_OK;
}

s32 sceNpTrophyTerm(void)
{
    printf("[sceNpTrophy] Term()\n");

    if (!s_trophy_initialized)
        return SCE_NP_TROPHY_ERROR_NOT_INITIALIZED;

    /* Save all registered contexts */
    for (int i = 1; i <= SCE_NP_TROPHY_MAX_CONTEXTS; i++) {
        if (s_contexts[i].in_use && s_contexts[i].registered)
            trophy_save(&s_contexts[i]);
        free(s_contexts[i].set);
    }

    memset(s_contexts, 0, sizeof(s_contexts));
    memset(s_handles, 0, sizeof(s_handles));
    s_trophy_initialized = 0;
    return CELL_OK;
}

s32 sceNpTrophyCreateContext(SceNpTrophyContext* context,
                             const SceNpCommunicationId* commId,
                             const SceNpCommunicationSignature* commSign,
                             u64 options)
{
    (void)commSign; (void)options;

    if (!s_trophy_initialized)
        return SCE_NP_TROPHY_ERROR_NOT_INITIALIZED;

    if (!context || !commId)
        return SCE_NP_TROPHY_ERROR_INVALID_ARGUMENT;
    const SceNpCommunicationId* commId_h = GUEST_PTR(commId, const SceNpCommunicationId*);

    for (s32 i = 1; i <= SCE_NP_TROPHY_MAX_CONTEXTS; i++) {
        if (!s_contexts[i].in_use) {
            memset(&s_contexts[i], 0, sizeof(TrophyContext));
            s_contexts[i].in_use = 1;
            s_contexts[i].commId = *commId_h;
            s_contexts[i].total_trophies = SCE_NP_TROPHY_MAX_NUM_TROPHIES;
            vm_write32(GUEST_EA(context), (u32)i);
            printf("[sceNpTrophy] CreateContext(commId=\"%s\") -> ctx=%d\n",
                   commId_h->data, i);
            return CELL_OK;
        }
    }

    return SCE_NP_TROPHY_ERROR_OUT_OF_MEMORY;
}

s32 sceNpTrophyDestroyContext(SceNpTrophyContext context)
{
    if (!s_trophy_initialized)
        return SCE_NP_TROPHY_ERROR_NOT_INITIALIZED;

    if (context <= 0 || context > SCE_NP_TROPHY_MAX_CONTEXTS ||
        !s_contexts[context].in_use)
        return SCE_NP_TROPHY_ERROR_INVALID_CONTEXT;

    if (s_contexts[context].registered)
        trophy_save(&s_contexts[context]);

    free(s_contexts[context].set);
    s_contexts[context].set = NULL;
    s_contexts[context].in_use = 0;
    printf("[sceNpTrophy] DestroyContext(ctx=%d)\n", context);
    return CELL_OK;
}

s32 sceNpTrophyCreateHandle(SceNpTrophyHandle* handle)
{
    if (!s_trophy_initialized)
        return SCE_NP_TROPHY_ERROR_NOT_INITIALIZED;

    if (!handle)
        return SCE_NP_TROPHY_ERROR_INVALID_ARGUMENT;

    for (s32 i = 1; i <= SCE_NP_TROPHY_MAX_HANDLES; i++) {
        if (!s_handles[i].in_use) {
            s_handles[i].in_use = 1;
            vm_write32(GUEST_EA(handle), (u32)i);
            printf("[sceNpTrophy] CreateHandle() -> handle=%d\n", i);
            return CELL_OK;
        }
    }

    return SCE_NP_TROPHY_ERROR_OUT_OF_MEMORY;
}

s32 sceNpTrophyDestroyHandle(SceNpTrophyHandle handle)
{
    if (!s_trophy_initialized)
        return SCE_NP_TROPHY_ERROR_NOT_INITIALIZED;

    if (handle <= 0 || handle > SCE_NP_TROPHY_MAX_HANDLES ||
        !s_handles[handle].in_use)
        return SCE_NP_TROPHY_ERROR_INVALID_HANDLE;

    s_handles[handle].in_use = 0;
    printf("[sceNpTrophy] DestroyHandle(handle=%d)\n", handle);
    return CELL_OK;
}

s32 sceNpTrophyRegisterContext(SceNpTrophyContext context,
                               SceNpTrophyHandle handle,
                               u32 statusCb,
                               u32 arg,
                               u64 options)
{
    (void)options;

    if (!s_trophy_initialized)
        return SCE_NP_TROPHY_ERROR_NOT_INITIALIZED;

    if (context <= 0 || context > SCE_NP_TROPHY_MAX_CONTEXTS ||
        !s_contexts[context].in_use)
        return SCE_NP_TROPHY_ERROR_INVALID_CONTEXT;

    if (handle <= 0 || handle > SCE_NP_TROPHY_MAX_HANDLES ||
        !s_handles[handle].in_use)
        return SCE_NP_TROPHY_ERROR_INVALID_HANDLE;

    if (s_contexts[context].registered)
        return SCE_NP_TROPHY_ERROR_CONTEXT_ALREADY_REG;

    if (!statusCb) return SCE_NP_TROPHY_ERROR_INVALID_ARGUMENT;
    if (!g_ps3_guest_caller) return SCE_NP_TROPHY_ERROR_UNKNOWN;

    /* Load any previously saved trophy data, and the title's set */
    trophy_load(&s_contexts[context]);
    trophy_load_set(&s_contexts[context]);
    s_contexts[context].registered = 1;

    printf("[sceNpTrophy] RegisterContext(ctx=%d, handle=%d, statusCb=0x%08X, "
           "arg=0x%08X) -> loaded saved data\n", context, handle, statusCb, arg);

    /* Use the runner's OPD bridge, not the optional code/TOC scaffold entry
     * point. Registration status is synchronous; its terminal notification is
     * delivered through sysutil polling, after the HLE storage load completes.
     * Intermediate firmware installation progress is not simulated here. */
    g_ps3_guest_caller(statusCb, (u32)context, SCE_NP_TROPHY_STATUS_INSTALLED,
                       0, 0, arg, 0, 0, 0);
    if (!s_trophy_initialized) return SCE_NP_TROPHY_ERROR_NOT_INITIALIZED;
    if (!s_contexts[context].in_use) return SCE_NP_TROPHY_ERROR_INVALID_CONTEXT;
    if (!s_handles[handle].in_use) return SCE_NP_TROPHY_ERROR_INVALID_HANDLE;
    const u64 completion[8] = {(u32)context, SCE_NP_TROPHY_STATUS_PROCESSING_COMPLETE,
                               0, 0, arg, 0, 0, 0};
    if (cellSysutilQueueGuestCallbackArgs(statusCb, completion) != CELL_OK) {
        s_contexts[context].registered = 0;
        return SCE_NP_TROPHY_ERROR_OUT_OF_MEMORY;
    }

    /* A platinum the saved trophies already complete: they were unlocked before the set was read, or before
     * a trophy was marked as unobtainable. */
    const int plat = trophy_platinum_due(&s_contexts[context]);
    if (plat >= 0)
        trophy_mark(&s_contexts[context], plat);

    return CELL_OK;
}

s32 sceNpTrophyGetRequiredDiskSpace(SceNpTrophyContext context,
                                    SceNpTrophyHandle handle,
                                    u64* reqSpace, u64 options)
{
    (void)handle; (void)options;

    if (!s_trophy_initialized)
        return SCE_NP_TROPHY_ERROR_NOT_INITIALIZED;

    if (context <= 0 || context > SCE_NP_TROPHY_MAX_CONTEXTS ||
        !s_contexts[context].in_use)
        return SCE_NP_TROPHY_ERROR_INVALID_CONTEXT;

    if (!reqSpace)
        return SCE_NP_TROPHY_ERROR_INVALID_ARGUMENT;
    reqSpace = GUEST_PTR(reqSpace, u64*);

    /* Typical trophy pack size. The guest is big-endian and reads this u64
     * back in BE order, so the value MUST be byte-swapped on write -- otherwise
     * a host-LE 0x100000 is read as 0x0010000000000000 (2^44). LBP feeds this
     * "required disk space" straight into its save-data free-space accounting
     * (sub_379EC0: errB = budget - usedKB - reqSpaceKB); a garbage 2^44 makes
     * errB underflow negative -> cellGame GameData check returns ERROR-B ->
     * boot bails. (Same LE-write class as the cellUserInfo fix.) */
    *reqSpace = ps3_bswap64((u64)(1024 * 1024)); /* 1 MB, big-endian */
    printf("[sceNpTrophy] GetRequiredDiskSpace(ctx=%d) -> 1MB\n", context);
    return CELL_OK;
}

s32 sceNpTrophyGetGameInfo(SceNpTrophyContext context,
                           SceNpTrophyHandle handle,
                           SceNpTrophyGameDetails* details,
                           SceNpTrophyGameData* data)
{
    (void)handle;

    if (!s_trophy_initialized)
        return SCE_NP_TROPHY_ERROR_NOT_INITIALIZED;

    if (context <= 0 || context > SCE_NP_TROPHY_MAX_CONTEXTS ||
        !s_contexts[context].in_use)
        return SCE_NP_TROPHY_ERROR_INVALID_CONTEXT;

    if (!s_contexts[context].registered)
        return SCE_NP_TROPHY_ERROR_CONTEXT_NOT_REGISTERED;

    /* The guest's SceNpTrophyGameDetails: five u32 counts, title[128] at +20, description[1024] at +148 and
     * four bytes reserved, 1176 in all; SceNpTrophyGameData: five u32 counts. Big-endian, written in place. */
    const TrophyContext* c = &s_contexts[context];
    u32 all[5] = {0, 0, 0, 0, 0}, got[5] = {0, 0, 0, 0, 0};   /* [0] every grade, then by grade */
    for (u32 i = 0; i < c->total_trophies; i++) {
        const PS3TrophyEntry* t = ps3_trophy_set_entry(c->set, (int)i);
        if (c->set && !t) continue;
        const int g = t ? t->grade : SCE_NP_TROPHY_GRADE_BRONZE;
        all[0]++;
        all[g]++;
        if (c->unlocked[i]) { got[0]++; got[g]++; }
    }
    if (details) {
        const u32 ea = GUEST_EA(details);
        memset(vm_base + ea, 0, 1176);
        for (int k = 0; k < 5; k++) vm_write32(ea + 4 * k, all[k]);
        trophy_put_str(ea + 20, c->set ? c->set->title : "PS3 Game", 128);
        trophy_put_str(ea + 148, c->set ? c->set->detail : "Trophy set", 1024);
    }
    if (data) {
        const u32 ea = GUEST_EA(data);
        for (int k = 0; k < 5; k++) vm_write32(ea + 4 * k, got[k]);
    }

    printf("[sceNpTrophy] GetGameInfo(ctx=%d)\n", context);
    return CELL_OK;
}

s32 sceNpTrophyGetTrophyInfo(SceNpTrophyContext context,
                             SceNpTrophyHandle handle,
                             SceNpTrophyId trophyId,
                             SceNpTrophyDetails* details,
                             SceNpTrophyData* data)
{
    (void)handle;

    if (!s_trophy_initialized)
        return SCE_NP_TROPHY_ERROR_NOT_INITIALIZED;

    if (context <= 0 || context > SCE_NP_TROPHY_MAX_CONTEXTS ||
        !s_contexts[context].in_use)
        return SCE_NP_TROPHY_ERROR_INVALID_CONTEXT;

    if (!s_contexts[context].registered)
        return SCE_NP_TROPHY_ERROR_CONTEXT_NOT_REGISTERED;

    if (trophyId < 0 || (u32)trophyId >= s_contexts[context].total_trophies)
        return SCE_NP_TROPHY_ERROR_INVALID_ARGUMENT;
    /* The guest's SceNpTrophyDetails: trophyId, trophyGrade, name[128] at +8, description[1024] at +136,
     * hidden at +1160 and three bytes reserved, 1164 in all; SceNpTrophyData: a CellRtcTick (microseconds
     * from 0001-01-01) at +0, trophyId at +8, unlocked at +12, 16 in all. Big-endian, written in place. */
    const TrophyContext* c = &s_contexts[context];
    const PS3TrophyEntry* t = ps3_trophy_set_entry(c->set, trophyId);
    if (details) {
        const u32 ea = GUEST_EA(details);
        char name[32];
        snprintf(name, sizeof name, "Trophy %d", trophyId);
        memset(vm_base + ea, 0, 1164);
        vm_write32(ea + 0, (u32)trophyId);
        vm_write32(ea + 4, t ? (u32)t->grade : SCE_NP_TROPHY_GRADE_BRONZE);
        trophy_put_str(ea + 8, t ? t->name : name, 128);
        trophy_put_str(ea + 136, t ? t->detail : "", 1024);
        vm_write8(ea + 1160, t ? (u8)t->hidden : 0);
    }
    if (data) {
        const u32 ea = GUEST_EA(data);
        const u64 when = c->unlock_time[trophyId];
        memset(vm_base + ea, 0, 16);
        vm_write64(ea + 0, c->unlocked[trophyId] && when ? (when + 62135596800ull) * 1000000ull : 0);
        vm_write32(ea + 8, (u32)trophyId);
        vm_write8(ea + 12, c->unlocked[trophyId] ? 1 : 0);
    }

    return CELL_OK;
}

s32 sceNpTrophyUnlockTrophy(SceNpTrophyContext context,
                            SceNpTrophyHandle handle,
                            SceNpTrophyId trophyId,
                            SceNpTrophyId* platinumId)
{
    (void)handle;

    if (!s_trophy_initialized)
        return SCE_NP_TROPHY_ERROR_NOT_INITIALIZED;

    if (context <= 0 || context > SCE_NP_TROPHY_MAX_CONTEXTS ||
        !s_contexts[context].in_use)
        return SCE_NP_TROPHY_ERROR_INVALID_CONTEXT;

    if (!s_contexts[context].registered)
        return SCE_NP_TROPHY_ERROR_CONTEXT_NOT_REGISTERED;

    TrophyContext* c = &s_contexts[context];
    if (trophyId < 0 || (u32)trophyId >= c->total_trophies)
        return SCE_NP_TROPHY_ERROR_INVALID_ARGUMENT;

    /* The system gives a platinum; a title cannot unlock one. */
    const PS3TrophyEntry* t = ps3_trophy_set_entry(c->set, trophyId);
    if (t && t->grade == SCE_NP_TROPHY_GRADE_PLATINUM)
        return SCE_NP_TROPHY_ERROR_PLATINUM_CANNOT_UNLOCK;

    if (c->unlocked[trophyId])
        return SCE_NP_TROPHY_ERROR_ALREADY_UNLOCKED;

    trophy_mark(c, trophyId);

    /* The platinum, if this was the last trophy toward it; its id goes back to the title, as from the PS3. */
    const int plat = trophy_platinum_due(c);
    if (plat >= 0)
        trophy_mark(c, plat);
    if (platinumId)
        vm_write32(GUEST_EA(platinumId), (u32)(plat >= 0 ? plat : SCE_NP_TROPHY_INVALID_TROPHY_ID));

    return CELL_OK;
}

s32 sceNpTrophyGetTrophyUnlockState(SceNpTrophyContext context,
                                    SceNpTrophyHandle handle,
                                    SceNpTrophyFlagArray* flags,
                                    u32* count)
{
    (void)handle;

    if (!s_trophy_initialized)
        return SCE_NP_TROPHY_ERROR_NOT_INITIALIZED;

    if (context <= 0 || context > SCE_NP_TROPHY_MAX_CONTEXTS ||
        !s_contexts[context].in_use)
        return SCE_NP_TROPHY_ERROR_INVALID_CONTEXT;

    if (!s_contexts[context].registered)
        return SCE_NP_TROPHY_ERROR_CONTEXT_NOT_REGISTERED;

    if (!flags || !count)
        return SCE_NP_TROPHY_ERROR_INVALID_ARGUMENT;

    u32 flags_ea = (u32)(uintptr_t)flags;
    for (u32 o = 0; o < sizeof(SceNpTrophyFlagArray); o += 4)
        vm_write32(flags_ea + o, 0);

    for (u32 i = 0; i < s_contexts[context].total_trophies; i++) {
        if (s_contexts[context].unlocked[i])
            {
            u32 word_ea = flags_ea + (i / 32) * 4;
            vm_write32(word_ea, vm_read32(word_ea) | (1u << (i % 32)));
        }
    }

    vm_write32((u32)(uintptr_t)count, (u32)s_contexts[context].total_trophies);
    return CELL_OK;
}

s32 sceNpTrophyGetGameProgress(SceNpTrophyContext context,
                               SceNpTrophyHandle handle,
                               s32* percentage)
{
    (void)handle;

    if (!s_trophy_initialized)
        return SCE_NP_TROPHY_ERROR_NOT_INITIALIZED;

    if (context <= 0 || context > SCE_NP_TROPHY_MAX_CONTEXTS ||
        !s_contexts[context].in_use)
        return SCE_NP_TROPHY_ERROR_INVALID_CONTEXT;

    if (!s_contexts[context].registered)
        return SCE_NP_TROPHY_ERROR_CONTEXT_NOT_REGISTERED;

    if (!percentage)
        return SCE_NP_TROPHY_ERROR_INVALID_ARGUMENT;

    u32 total = s_contexts[context].total_trophies;
    u32 unlocked = trophy_count_unlocked(&s_contexts[context]);

    s32 pct = total > 0 ? (s32)((unlocked * 100) / total) : 0;
    vm_write32(GUEST_EA(percentage), (u32)pct);

    printf("[sceNpTrophy] GetGameProgress(ctx=%d) -> %d%%\n",
           context, pct);
    return CELL_OK;
}
