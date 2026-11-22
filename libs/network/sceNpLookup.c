/*
 * ps3recomp - sceNpLookup (player profiles and avatars), answered locally
 *
 * A player's profile is their online ID: that is all psnr knows about anyone,
 * so the lookup needs no server. The name comes back as the NP ID's handle,
 * with an empty "about me", English, country "us" and no avatar (an image
 * of size 0). Transactions complete immediately; PollAsync reports done.
 *
 * Simpsons Arcade retries Lookup title-context creation every frame until it
 * succeeds and holds its online session until it does, so a context that
 * merely "fakes OK" (id 0) kept every match from starting.
 *
 * Layouts (SDK ABI):
 *   SceNpUserInfo { SceNpId userId (36); SceNpOnlineName name (48); SceNpAvatarUrl icon (128) }
 *   SceNpAvatarImage { u8 data[200 KB]; u32 size; u8 reserved[12] }
 *   SceNpMyLanguages { s32 language1..3; pad } / SceNpCountryCode { char data[2]; term; pad }
 */

#include "sceNp.h"
#include "../../runtime/ppu/ppu_memory.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define LOOKUP_ERROR_INVALID_ID   0x8002A106u
#define LOOKUP_ERROR_SERVICE_UNAVAILABLE 0x8002A409u   /* SCE_NP_COMMUNITY_SERVER_ERROR_SERVICE_UNAVAILABLE */
#define AVATAR_IMAGE_DATA_SIZE    (200u * 1024u)

#define TITLE_MAX 8
#define TRANS_MAX 32

static int s_lookup_init;
static int s_titles[TITLE_MAX + 1];
static int s_trans[TRANS_MAX + 1];   /* 0 free, else owning title */
static s32 s_trans_result[TRANS_MAX + 1];   /* what PollAsync/WaitAsync report for it */

s32 sceNpLookupInit(void)  { s_lookup_init = 1; return CELL_OK; }
s32 sceNpLookupTerm(void)  { s_lookup_init = 0; return CELL_OK; }

s32 sceNpLookupCreateTitleCtx(u32 commId, u32 selfNpId)
{
    (void)commId; (void)selfNpId;
    for (int i = 1; i <= TITLE_MAX; i++)
        if (!s_titles[i]) { s_titles[i] = 1; return i; }
    return (s32)SCE_NP_ERROR_OUT_OF_MEMORY;
}

s32 sceNpLookupDestroyTitleCtx(s32 titleCtxId)
{
    if (titleCtxId < 1 || titleCtxId > TITLE_MAX || !s_titles[titleCtxId])
        return (s32)LOOKUP_ERROR_INVALID_ID;
    s_titles[titleCtxId] = 0;
    return CELL_OK;
}

s32 sceNpLookupCreateTransactionCtx(s32 titleCtxId)
{
    if (titleCtxId < 1 || titleCtxId > TITLE_MAX || !s_titles[titleCtxId])
        return (s32)LOOKUP_ERROR_INVALID_ID;
    for (int i = 1; i <= TRANS_MAX; i++)
        if (!s_trans[i]) { s_trans[i] = titleCtxId; s_trans_result[i] = 0; return i; }
    return (s32)SCE_NP_ERROR_OUT_OF_MEMORY;
}

s32 sceNpLookupDestroyTransactionCtx(s32 transId)
{
    if (transId < 1 || transId > TRANS_MAX || !s_trans[transId])
        return (s32)LOOKUP_ERROR_INVALID_ID;
    s_trans[transId] = 0;
    return CELL_OK;
}

static int trans_ok(s32 id) { return id >= 1 && id <= TRANS_MAX && s_trans[id]; }

s32 sceNpLookupAbortTransaction(s32 transId)
{
    return trans_ok(transId) ? CELL_OK : (s32)LOOKUP_ERROR_INVALID_ID;
}

/* Everything completes at the call; the result is the request's own. */
s32 sceNpLookupPollAsync(s32 transId, u32 result)
{
    if (!trans_ok(transId)) return (s32)LOOKUP_ERROR_INVALID_ID;
    if (result) vm_write32(result, (u32)s_trans_result[transId]);
    return 0;
}

s32 sceNpLookupWaitAsync(s32 transId, u32 result)
{
    return sceNpLookupPollAsync(transId, result);
}

static void empty_avatar(u32 image)
{
    if (image) vm_write32(image + AVATAR_IMAGE_DATA_SIZE, 0);   /* size = 0 */
}

/* (transId, npId*, userInfo*, aboutMe*, languages*, countryCode*, avatarImage*, prio, option) */
s32 sceNpLookupUserProfileAsync(s32 transId, u32 npId, u32 userInfo, u32 aboutMe,
                                u32 languages, u32 countryCode, u32 avatarImage, s32 prio)
{
    (void)prio;
    if (!trans_ok(transId)) return (s32)LOOKUP_ERROR_INVALID_ID;
    if (!npId) return (s32)SCE_NP_ERROR_INVALID_ARGUMENT;
    if (userInfo) {
        memset(vm_ptr8(userInfo), 0, 36 + 48 + 128);
        memcpy(vm_ptr8(userInfo), vm_ptr8(npId), 36);               /* userId */
        memcpy(vm_ptr8(userInfo + 36), vm_ptr8(npId), 16);          /* name = handle */
    }
    if (aboutMe) memset(vm_ptr8(aboutMe), 0, 64);
    if (languages) {
        vm_write32(languages, SCE_NP_LANG_ENGLISH);
        vm_write32(languages + 4, 0);
        vm_write32(languages + 8, 0);
    }
    if (countryCode) memcpy(vm_ptr8(countryCode), "us\0", 4);
    empty_avatar(avatarImage);
    return CELL_OK;
}

s32 sceNpLookupUserProfile(s32 transId, u32 npId, u32 userInfo, u32 aboutMe,
                           u32 languages, u32 countryCode, u32 avatarImage)
{
    return sceNpLookupUserProfileAsync(transId, npId, userInfo, aboutMe,
                                       languages, countryCode, avatarImage, 0);
}

/* (transId, avatarUrl*, avatarImage*, prio, option) */
s32 sceNpLookupAvatarImageAsync(s32 transId, u32 avatarUrl, u32 avatarImage, s32 prio, u32 option)
{
    (void)avatarUrl; (void)prio; (void)option;
    if (!trans_ok(transId)) return (s32)LOOKUP_ERROR_INVALID_ID;
    empty_avatar(avatarImage);
    return CELL_OK;
}

s32 sceNpLookupAvatarImage(s32 transId, u32 avatarUrl, u32 avatarImage, u32 option)
{
    return sceNpLookupAvatarImageAsync(transId, avatarUrl, avatarImage, 0, option);
}

/* Title Small Storage: one small file per title that PSN serves to every player, which titles read their online
 * settings from. psnr keeps none, so it comes from the file PS3_NP_TSS_FILE names; without one the request fails
 * as a service that is not there. The title gets the file's bytes as they are (God of War: Ascension's is
 * encrypted with a key of its own; its writer is that port's business).
 * (transId, data*, maxSize, contentLength* (a size_t, 32-bit), prio, option) */
static s32 tss_read(u32 data, u32 maxSize, u32 contentLength)
{
    const char* path = getenv("PS3_NP_TSS_FILE");
    FILE* f = (path && *path) ? fopen(path, "rb") : NULL;
    if (!f) {
        printf("[sceNpLookup] TitleSmallStorage: %s\n",
               (path && *path) ? "PS3_NP_TSS_FILE cannot be opened" : "none here (PS3_NP_TSS_FILE not set)");
        return (s32)LOOKUP_ERROR_SERVICE_UNAVAILABLE;
    }
    fseek(f, 0, SEEK_END);
    const long whole = ftell(f);
    fseek(f, 0, SEEK_SET);
    const u32 n = whole < 0 ? 0 : ((u32)whole > maxSize ? maxSize : (u32)whole);
    const size_t got = (data && n) ? fread(vm_ptr8(data), 1, n, f) : 0;
    fclose(f);
    if (contentLength) vm_write32(contentLength, (u32)got);
    printf("[sceNpLookup] TitleSmallStorage: %u bytes from %s%s\n", (unsigned)got, path,
           (u32)whole > maxSize ? " (cut to the title's buffer)" : "");
    return CELL_OK;
}

s32 sceNpLookupTitleSmallStorageAsync(s32 transId, u32 data, u32 maxSize, u32 contentLength, s32 prio, u32 option)
{
    (void)prio; (void)option;
    if (!trans_ok(transId)) return (s32)LOOKUP_ERROR_INVALID_ID;
    if (!data || !maxSize) return (s32)SCE_NP_ERROR_INVALID_ARGUMENT;
    s_trans_result[transId] = tss_read(data, maxSize, contentLength);
    return CELL_OK;
}

s32 sceNpLookupTitleSmallStorage(s32 transId, u32 data, u32 maxSize, u32 contentLength, u32 option)
{
    (void)option;
    if (!trans_ok(transId)) return (s32)LOOKUP_ERROR_INVALID_ID;
    if (!data || !maxSize) return (s32)SCE_NP_ERROR_INVALID_ARGUMENT;
    return tss_read(data, maxSize, contentLength);
}
