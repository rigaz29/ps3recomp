/*
 * ps3recomp - sceNp HLE implementation
 *
 * Provides fake PSN identity so games can proceed through NP checks.
 * The username defaults to "PS3Player" but is configurable.
 */

#include "sceNp.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>
#include "../../runtime/ppu/ppu_memory.h"   /* vm_write*: guest EA -> host, byte-swapped */
#include "np_psnr.h"
#include "../system/cellSysutil.h"
#include "../filesystem/edat.h"


/* ---------------------------------------------------------------------------
 * Internal state
 * -----------------------------------------------------------------------*/

static int  s_np_initialized = 0;
static int  s_score_initialized = 0;
static char s_fake_username[SCE_NP_ONLINEID_MAX_LENGTH + 1] = "PS3Player";

/* ---------------------------------------------------------------------------
 * Configuration
 * -----------------------------------------------------------------------*/

void sceNpSetFakeUsername(const char* username)
{
    if (username) {
        strncpy(s_fake_username, GUEST_PTR(username, const char*),
            SCE_NP_ONLINEID_MAX_LENGTH);
        s_fake_username[SCE_NP_ONLINEID_MAX_LENGTH] = '\0';
    }
}

const char* np_fake_username(void)
{
    return s_fake_username;
}

/* Build a fake NP ID from the current username (PS3_NP_ONLINE_ID wins, so
 * two instances on one machine can be two players). */
static void np_build_fake_id(SceNpId* npId)
{
    memset(npId, 0, sizeof(SceNpId));
    strncpy(npId->handle.data, np_psnr_online_id(), SCE_NP_ONLINEID_MAX_LENGTH);
    npId->handle.term = '\0';
}

/* ---------------------------------------------------------------------------
 * API implementations
 * -----------------------------------------------------------------------*/

s32 sceNpInit(u32 poolSize, void* poolPtr)
{
    (void)poolSize;
    (void)poolPtr;

    printf("[sceNp] Init(poolSize=%u, username=\"%s\")\n",
           poolSize, np_psnr_online_id());

    if (s_np_initialized)
        return SCE_NP_ERROR_ALREADY_INITIALIZED;

    s_np_initialized = 1;
    return CELL_OK;
}

/* Whether sceNpInit has run; sceNpBasic follows it (see sceNpBasic.c). */
int np_is_initialized(void) { return s_np_initialized; }

s32 sceNpTerm(void)
{
    printf("[sceNp] Term()\n");

    if (!s_np_initialized)
        return SCE_NP_ERROR_NOT_INITIALIZED;

    s_np_initialized = 0;
    return CELL_OK;
}

s32 sceNpGetNpId(s32 userId, SceNpId* npId)
{
    (void)userId;

    if (!s_np_initialized)
        return SCE_NP_ERROR_NOT_INITIALIZED;

    if (!npId)
        return SCE_NP_ERROR_INVALID_ARGUMENT;

    /* A guest address, like every sibling getter's: writing through it
     * untranslated hit a host address and crashed Simpsons Arcade the first
     * time it asked for its NP ID online. */
    npId = GUEST_PTR(npId, SceNpId*);
    np_build_fake_id(npId);
    printf("[sceNp] GetNpId(user=%d) -> \"%s\"\n", userId, np_psnr_online_id());
    return CELL_OK;
}

s32 sceNpGetOnlineId(s32 userId, SceNpOnlineId* onlineId)
{
    onlineId = GUEST_PTR(onlineId, SceNpOnlineId*);
    (void)userId;

    if (!s_np_initialized)
        return SCE_NP_ERROR_NOT_INITIALIZED;

    if (!onlineId)
        return SCE_NP_ERROR_INVALID_ARGUMENT;

    memset(onlineId, 0, sizeof(SceNpOnlineId));
    strncpy(onlineId->data, np_psnr_online_id(), SCE_NP_ONLINEID_MAX_LENGTH);
    onlineId->term = '\0';

    printf("[sceNp] GetOnlineId(user=%d) -> \"%s\"\n", userId, np_psnr_online_id());
    return CELL_OK;
}

s32 sceNpGetOnlineName(s32 userId, SceNpOnlineName* onlineName)
{
    onlineName = GUEST_PTR(onlineName, SceNpOnlineName*);
    (void)userId;

    if (!s_np_initialized)
        return SCE_NP_ERROR_NOT_INITIALIZED;

    if (!onlineName)
        return SCE_NP_ERROR_INVALID_ARGUMENT;

    memset(onlineName, 0, sizeof(SceNpOnlineName));
    strncpy(onlineName->data, np_psnr_online_id(),
            SCE_NP_ONLINENAME_MAX_LENGTH - 1);

    printf("[sceNp] GetOnlineName(user=%d) -> \"%s\"\n",
           userId, np_psnr_online_id());
    return CELL_OK;
}

s32 sceNpGetUserProfile(s32 userId, SceNpUserInfo* userInfo)
{
    userInfo = GUEST_PTR(userInfo, SceNpUserInfo*);
    (void)userId;

    if (!s_np_initialized)
        return SCE_NP_ERROR_NOT_INITIALIZED;

    if (!userInfo)
        return SCE_NP_ERROR_INVALID_ARGUMENT;

    memset(userInfo, 0, sizeof(SceNpUserInfo));
    np_build_fake_id(&userInfo->npId);
    strncpy(userInfo->onlineName.data, np_psnr_online_id(),
            SCE_NP_ONLINENAME_MAX_LENGTH - 1);
    /* Leave avatar URL empty */

    printf("[sceNp] GetUserProfile(user=%d) -> \"%s\"\n",
           userId, np_psnr_online_id());
    return CELL_OK;
}

s32 sceNpGetAccountRegion(s32 userId, u32* region)
{
    (void)userId;

    if (!s_np_initialized)
        return SCE_NP_ERROR_NOT_INITIALIZED;

    if (!region)
        return SCE_NP_ERROR_INVALID_ARGUMENT;

    /* Region: US (SCEA) = 0x5553 ('US') */
    vm_write32((u32)(uintptr_t)region, (u32)0x5553);

    printf("[sceNp] GetAccountRegion(user=%d) -> US\n", userId);
    return CELL_OK;
}

s32 sceNpGetAccountAge(s32 userId, s32* age)
{
    (void)userId;

    if (!s_np_initialized)
        return SCE_NP_ERROR_NOT_INITIALIZED;

    if (!age)
        return SCE_NP_ERROR_INVALID_ARGUMENT;

    vm_write32((u32)(uintptr_t)age, 25); /* default adult age */
    printf("[sceNp] GetAccountAge(user=%d) -> 25\n", userId);
    return CELL_OK;
}

s32 sceNpGetMyLanguages(SceNpMyLanguages* langs)
{
    if (!s_np_initialized)
        return SCE_NP_ERROR_NOT_INITIALIZED;

    if (!langs)
        return SCE_NP_ERROR_INVALID_ARGUMENT;

    u32 langs_ea = (u32)(uintptr_t)langs;
    vm_write32(langs_ea + 0, SCE_NP_LANG_ENGLISH);
    vm_write32(langs_ea + 4, 0);
    vm_write32(langs_ea + 8, 0);

    printf("[sceNp] GetMyLanguages() -> English\n");
    return CELL_OK;
}

/* ---------------------------------------------------------------------------
 * NP Manager (sign-in state)
 *
 * The toolkit runs with a fake local profile but no live PSN connection. So the
 * manager reports OFFLINE — games gate their online flows on GetStatus and skip
 * them cleanly — while the identity getters still hand back the fake account,
 * matching how NP behaves on a real signed-in-but-disconnected console.
 * Imported by most online-capable titles.
 * -----------------------------------------------------------------------*/

static SceNpManagerCallback s_npmgr_cb = NULL;
static void*                s_npmgr_cb_arg = NULL;

s32 sceNpManagerGetStatus(s32* status)
{
    if (!s_np_initialized)
        return SCE_NP_ERROR_NOT_INITIALIZED;
    if (!status)
        return SCE_NP_ERROR_INVALID_ARGUMENT;
    /* `status` is a GUEST address -- the HLE ABI adapter passes pointer
     * parameters straight through as guest values, so dereferencing one
     * writes to whatever host address shares that number. Same trap as
     * cellGcmSys had. Tokyo Jungle calls this during its online init. */
    /* Signed in when there is a psnr server to be signed in to. */
    s32 st = np_psnr_enabled() ? SCE_NP_MANAGER_STATUS_ONLINE : SCE_NP_MANAGER_STATUS_OFFLINE;
    vm_write32((uint32_t)(uintptr_t)status, (uint32_t)st);
    { static s32 last = -2;
      if (st != last) printf("[sceNp] ManagerGetStatus() -> %s\n", st < 0 ? "OFFLINE" : "ONLINE");
      last = st; }
    return CELL_OK;
}

s32 sceNpManagerRegisterCallback(SceNpManagerCallback callback, void* arg)
{
    if (!s_np_initialized)
        return SCE_NP_ERROR_NOT_INITIALIZED;
    s_npmgr_cb     = callback;
    s_npmgr_cb_arg = arg;
    printf("[sceNp] ManagerRegisterCallback()\n");
    /* Online: tell the title it is signed in, as the console does once the
     * manager reaches ONLINE. Delivered on its next cellSysutilCheckCallback. */
    if (np_psnr_enabled() && callback) {
        const u64 args[8] = { (u64)(u32)SCE_NP_MANAGER_STATUS_ONLINE, 0,
                              (u64)(u32)(uintptr_t)arg, 0, 0, 0, 0, 0 };
        cellSysutilQueueGuestCallbackArgs((u32)(uintptr_t)callback, args);
    }
    return CELL_OK;
}

s32 sceNpManagerUnregisterCallback(void)
{
    s_npmgr_cb     = NULL;
    s_npmgr_cb_arg = NULL;
    return CELL_OK;
}

/* Identity getters: reuse the fake-profile implementations (offline-with-account). */
s32 sceNpManagerGetNpId(SceNpId* npId)               { return sceNpGetNpId(0, npId); }
s32 sceNpManagerGetOnlineId(SceNpOnlineId* onlineId) { return sceNpGetOnlineId(0, onlineId); }
s32 sceNpManagerGetOnlineName(SceNpOnlineName* name) { return sceNpGetOnlineName(0, name); }
s32 sceNpManagerGetAccountAge(s32* age)              { return sceNpGetAccountAge(0, age); }

/* Parental controls: an unrestricted adult account. Unimplemented, these
 * returned OK without writing, and the title read whatever was in its stack. */
s32 sceNpManagerGetContentRatingFlag(s32* isRestricted, s32* age)
{
    if (!s_np_initialized) return SCE_NP_ERROR_NOT_INITIALIZED;
    if (isRestricted) vm_write32((u32)(uintptr_t)isRestricted, 0);
    if (age) vm_write32((u32)(uintptr_t)age, 25);
    return CELL_OK;
}

s32 sceNpManagerGetChatRestrictionFlag(s32* isRestricted)
{
    if (!s_np_initialized) return SCE_NP_ERROR_NOT_INITIALIZED;
    if (isRestricted) vm_write32((u32)(uintptr_t)isRestricted, 0);
    return CELL_OK;
}

/* Tickets, the account region and the network time come from the PSN
 * servers. Unimplemented, these imports answered CELL_OK with nothing
 * written: a title that requested a ticket waited for a GOT_TICKET event
 * that never came, and one that read the region or the time used whatever
 * was in its buffer. Checks and offline answers as RPCS3's sceNp.cpp:
 * without a psnr server the manager is OFFLINE and so are these. Signed in
 * to psnr, which issues no tickets, a request gets a failed GOT_TICKET at
 * once and there is never a ticket to read (unless PS3_NP_TEST_TICKET, below);
 * the region is the fake account's (US, English) and the time is the host's. */
#define SCE_NP_COOKIE_MAX_SIZE 1024
#define SCE_NP_TICKET_PARAM_SERIAL_ID   0
#define SCE_NP_TICKET_PARAM_SUBJECT_DOB 11

/* PS3_NP_TEST_TICKET=1 (signed in to psnr): a ticket of our own making in
 * place of the failed GOT_TICKET, laid out as an NP ticket 2.1 (a body
 * section of typed fields, a footer with an empty signature). Nothing on
 * this side checks it: it is what a title sends to servers of its own,
 * which then have to be stand-ins of ours. For tracing what a title does
 * once it holds a ticket. */
static u8  s_ticket[512];
static u32 s_ticket_len;
static u64 s_ticket_acct;
static u64 s_ticket_issued, s_ticket_expire;   /* CellRtcTick microseconds */
static char s_ticket_service[25];

static u64 np_test_account(void)
{
    u64 h = 1469598103934665603ull;              /* FNV-1a of the online id: steady per player */
    for (const char* p = np_psnr_online_id(); *p; p++) { h ^= (u8)*p; h *= 1099511628211ull; }
    return h & 0x7FFFFFFFFFFFFFFFull;
}

static u8* tk_put16(u8* p, u32 v) { p[0] = (u8)(v >> 8); p[1] = (u8)v; return p + 2; }
static u8* tk_put32(u8* p, u32 v) { p = tk_put16(p, v >> 16); return tk_put16(p, v & 0xFFFF); }
static u8* tk_put64(u8* p, u64 v) { p = tk_put32(p, (u32)(v >> 32)); return tk_put32(p, (u32)v); }
static u8* tk_field(u8* p, u32 type, const void* data, u32 len)
{
    p = tk_put16(p, type); p = tk_put16(p, len);
    memcpy(p, data, len);
    return p + len;
}

static void np_test_ticket_build(void)
{
    u8 body[384], *b = body, v[32];
    const u64 now = (u64)time(NULL) * 1000000ull + 62135596800000000ull;
    s_ticket_acct = np_test_account();
    s_ticket_issued = now;
    s_ticket_expire = now + 3600ull * 1000000ull;
    memset(v, 0, sizeof(v)); memcpy(v, "ascension-r-test", 16);
    b = tk_field(b, 0x0008, v, 20);                                    /* serial id */
    tk_put32(v, 0x100); b = tk_field(b, 0x0001, v, 4);                 /* issuer id */
    tk_put64(v, s_ticket_issued); b = tk_field(b, 0x0007, v, 8);       /* issued */
    tk_put64(v, s_ticket_expire); b = tk_field(b, 0x0007, v, 8);       /* expires */
    tk_put64(v, s_ticket_acct); b = tk_field(b, 0x0002, v, 8);         /* account id */
    memset(v, 0, sizeof(v)); strncpy((char*)v, np_psnr_online_id(), 31);
    b = tk_field(b, 0x0004, v, 32);                                    /* online id */
    memset(v, 0, 4); memcpy(v, "us", 2); b = tk_field(b, 0x0008, v, 4); /* region */
    memset(v, 0, 4); memcpy(v, "un", 2); b = tk_field(b, 0x0004, v, 4); /* domain */
    memset(v, 0, 24); memcpy(v, s_ticket_service, strlen(s_ticket_service));
    b = tk_field(b, 0x0008, v, 24);                                    /* service id */
    tk_put32(v, 0); b = tk_field(b, 0x0001, v, 4);                     /* status */
    b = tk_field(b, 0x0010, v, 0); b = tk_field(b, 0x0000, v, 0);      /* no dob, no entitlements */
    const u32 blen = (u32)(b - body);
    u8* t = s_ticket;
    t = tk_put32(t, 0x21010000u);                                      /* version 2.1 */
    u8* size_at = t; t += 4;
    t = tk_put16(t, 0x3000); t = tk_put16(t, blen); memcpy(t, body, blen); t += blen;
    t = tk_put16(t, 0x3002); t = tk_put16(t, 4 + 4 + 4 + 56);          /* footer: cipher id, signature */
    memset(v, 0, 8); t = tk_field(t, 0x0008, v, 4);
    u8 sig[56]; memset(sig, 0, sizeof(sig)); t = tk_field(t, 0x0008, sig, 56);
    s_ticket_len = (u32)(t - s_ticket);
    tk_put32(size_at, s_ticket_len - 8);
}

s32 sceNpManagerRequestTicket2(u32 npId, u32 version, u32 serviceId, u32 cookie,
                               u32 cookieSize, u32 entitlementId, u32 consumedCount)
{
    (void)version; (void)cookie; (void)entitlementId; (void)consumedCount;
    if (!s_np_initialized) return SCE_NP_ERROR_NOT_INITIALIZED;
    if (!npId || !serviceId || cookieSize > SCE_NP_COOKIE_MAX_SIZE)
        return SCE_NP_AUTH_EINVALID_ARGUMENT;
    if (!np_psnr_enabled()) {
        printf("[sceNp] ManagerRequestTicket2() -> OFFLINE\n");
        return SCE_NP_ERROR_OFFLINE;
    }
    if (getenv("PS3_NP_TEST_TICKET")) {
        snprintf(s_ticket_service, sizeof(s_ticket_service), "%s", GUEST_PTR(serviceId, const char*));
        np_test_ticket_build();
        if (s_npmgr_cb) {
            const u64 args[8] = { SCE_NP_MANAGER_EVENT_GOT_TICKET, (u64)s_ticket_len,
                                  (u64)(u32)(uintptr_t)s_npmgr_cb_arg, 0, 0, 0, 0, 0 };
            cellSysutilQueueGuestCallbackArgs((u32)(uintptr_t)s_npmgr_cb, args);
        }
        printf("[sceNp] ManagerRequestTicket2(service \"%s\", entitlement %s, consumed %u) -> test ticket, %u bytes, "
               "account %llu\n", s_ticket_service,
               entitlementId ? GUEST_PTR(entitlementId, const char*) : "-", consumedCount,
               s_ticket_len, (unsigned long long)s_ticket_acct);
        return CELL_OK;
    }
    if (s_npmgr_cb) {
        const u64 args[8] = { SCE_NP_MANAGER_EVENT_GOT_TICKET, (u64)(u32)SCE_NP_ERROR_INVALID_STATE,
                              (u64)(u32)(uintptr_t)s_npmgr_cb_arg, 0, 0, 0, 0, 0 };
        cellSysutilQueueGuestCallbackArgs((u32)(uintptr_t)s_npmgr_cb, args);
    }
    printf("[sceNp] ManagerRequestTicket2() -> GOT_TICKET failed (psnr issues no tickets)\n");
    return CELL_OK;
}

s32 sceNpManagerGetTicket(u32 buffer, u32 bufferSize)
{
    if (!s_np_initialized) return SCE_NP_ERROR_NOT_INITIALIZED;
    if (!bufferSize) return SCE_NP_ERROR_INVALID_ARGUMENT;
    if (s_ticket_len) {
        const u32 room = vm_read32(bufferSize);
        if (buffer) {
            if (room < s_ticket_len) return SCE_NP_ERROR_INVALID_ARGUMENT;
            memcpy(vm_ptr8(buffer), s_ticket, s_ticket_len);
        }
        vm_write32(bufferSize, s_ticket_len);
        printf("[sceNp] ManagerGetTicket(buffer %s, room %u) -> %u bytes\n", buffer ? "set" : "NULL", room, s_ticket_len);
        return CELL_OK;
    }
    vm_write32(bufferSize, 0);   /* no ticket: nothing to copy */
    return CELL_OK;
}

s32 sceNpManagerGetTicketParam(s32 paramId, u32 param)
{
    if (!s_np_initialized) return SCE_NP_ERROR_NOT_INITIALIZED;
    if (!param || paramId < SCE_NP_TICKET_PARAM_SERIAL_ID || paramId > SCE_NP_TICKET_PARAM_SUBJECT_DOB)
        return SCE_NP_ERROR_INVALID_ARGUMENT;
    if (!s_ticket_len) return SCE_NP_ERROR_INVALID_STATE;   /* no ticket was ever issued */
    /* SceNpTicketParam: a 256-byte union, big-endian values at its start. */
    u8* p = vm_ptr8(param);
    memset(p, 0, 256);
    switch (paramId) {
    case 0:  memcpy(p, "ascension-r-test", 16); break;                         /* serial id */
    case 1:  tk_put32(p, 0x100); break;                                          /* issuer id */
    case 2:  tk_put64(p, s_ticket_issued); break;
    case 3:  tk_put64(p, s_ticket_expire); break;
    case 4:  tk_put64(p, s_ticket_acct); break;                                  /* subject account id */
    case 5:  strncpy((char*)p, np_psnr_online_id(), 31); break;                  /* subject online id */
    case 6:  memcpy(p, "us", 2); break;                                          /* subject region */
    case 7:  memcpy(p, "un", 2); break;                                          /* subject domain */
    case 8:  memcpy(p, s_ticket_service, strlen(s_ticket_service)); break;       /* service id */
    case 9:  break;                                                              /* subject status */
    case 10: break;                                                              /* status duration */
    case 11: tk_put16(p, 1990); p[2] = 1; p[3] = 1; break;                       /* subject dob */
    }
    printf("[sceNp] ManagerGetTicketParam(%d)\n", paramId);
    return CELL_OK;
}

s32 sceNpManagerGetAccountRegion(u32 countryCode, u32 language)
{
    if (!s_np_initialized) return SCE_NP_ERROR_NOT_INITIALIZED;
    if (!countryCode || !language) return SCE_NP_ERROR_INVALID_ARGUMENT;
    if (!np_psnr_enabled()) return SCE_NP_ERROR_OFFLINE;
    vm_write8(countryCode + 0, 'u'); vm_write8(countryCode + 1, 's');   /* SceNpCountryCode */
    vm_write8(countryCode + 2, 0);   vm_write8(countryCode + 3, 0);
    vm_write32(language, 1);         /* CELL_SYSUTIL_LANG_ENGLISH_US */
    return CELL_OK;
}

s32 sceNpManagerGetNetworkTime(u32 pTick)
{
    if (!s_np_initialized) return SCE_NP_ERROR_NOT_INITIALIZED;
    if (!pTick) return SCE_NP_ERROR_INVALID_ARGUMENT;
    if (!np_psnr_enabled()) return SCE_NP_ERROR_OFFLINE;
    /* CellRtcTick: microseconds since 0001-01-01 00:00 UTC. */
    const u64 unix_us = (u64)time(NULL) * 1000000ull;
    vm_write64(pTick, unix_us + 62135596800000000ull);
    return CELL_OK;
}

/* Score setup is local even while the NP manager is offline. Games may
 * initialize it before starting the worker that decides whether to use PSN. */
s32 sceNpScoreInit(void)
{
    if (s_score_initialized)
        return SCE_NP_COMMUNITY_ERROR_ALREADY_INITIALIZED;
    if (!s_np_initialized)
        return SCE_NP_ERROR_NOT_INITIALIZED;
    s_score_initialized = 1;
    return CELL_OK;
}

s32 sceNpScoreTerm(void)
{
    if (!s_score_initialized)
        return SCE_NP_COMMUNITY_ERROR_NOT_INITIALIZED;
    if (!s_np_initialized)
        return SCE_NP_ERROR_NOT_INITIALIZED;
    s_score_initialized = 0;
    return CELL_OK;
}

/* ---------------------------------------------------------------------------
 * NPDRM
 *
 * On hardware sceNpDrmIsAvailable[2](klicensee, path) checks the licence for an
 * NPDRM file and leaves the klicensee with the kernel, so later cellFsOpen calls
 * on EDATs return plaintext. Free (license type 3) and disc-bound game data use a
 * title-specific klicensee that only the game knows; record it for the EDAT
 * reader (libs/filesystem/edat.c), which verifies it against each file's
 * dev_hash before using it. The two calls differ only in licence-check details
 * that do not apply here.
 * -----------------------------------------------------------------------*/

static s32 np_drm_is_available(const u8* k_licensee, const char* drm_path)
{
    const u8* k = GUEST_PTR(k_licensee, const u8*);
    const char* path = GUEST_PTR(drm_path, const char*);
    /* A NULL klicensee is legal for content that needs none; nothing to record. */
    if (k) edat_add_klicensee(k);
    fprintf(stderr, "[sceNp] DRM availability check: %s\n", path ? path : "(null)");
    return CELL_OK;
}

s32 sceNpDrmIsAvailable(const u8* k_licensee, const char* drm_path)
{
    return np_drm_is_available(k_licensee, drm_path);
}

s32 sceNpDrmIsAvailable2(const u8* k_licensee, const char* drm_path)
{
    return np_drm_is_available(k_licensee, drm_path);
}
