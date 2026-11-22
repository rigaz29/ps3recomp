/*
 * ps3recomp - sceNpSignaling HLE (see sceNpSignaling.h)
 *
 * A connection is one peer seen from one context. Activating it asks the
 * signaling server (np_signal_client.h) where the peer is and tells the
 * peer; the answer makes the connection ACTIVE, and the title's handlers
 * hear ESTABLISHED a moment later (PS3_NP_SIGNALING_DELAY_MS, default 500:
 * on PSN it took a round trip or two, and titles that act on the event count
 * on having filed the connection id first). A peer that activates one to us
 * gives EXT_PEER_ACTIVATED, both sides activated EXT_MUTUAL_ACTIVATED. A
 * peer that cannot be found within 15 seconds gives DEAD (TIMEOUT), one
 * that leaves DEAD (TERMINATED_BY_PEER).
 *
 * Handler calls are queued for the title's next cellSysutilCheckCallback,
 * which also runs np_signaling_pump through np_psnr's ticks.
 */

#include "sceNpSignaling.h"
#include "np_signal_client.h"
#include "np_psnr.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "../../runtime/ppu/ppu_memory.h"   /* vm_write*: guest EA -> host, byte-swapped */

#ifdef _WIN32
#  define WIN32_LEAN_AND_MEAN
#  include <windows.h>
static SRWLOCK s_lock = SRWLOCK_INIT;
#  define LOCK()   AcquireSRWLockExclusive(&s_lock)
#  define UNLOCK() ReleaseSRWLockExclusive(&s_lock)
#else
#  include <pthread.h>
static pthread_mutex_t s_lock = PTHREAD_MUTEX_INITIALIZER;
#  define LOCK()   pthread_mutex_lock(&s_lock)
#  define UNLOCK() pthread_mutex_unlock(&s_lock)
#endif

s32 cellSysutilQueueGuestCallbackArgs(u32 opd, const u64 args[8]);
int np_is_initialized(void);

#define ACTIVATE_TIMEOUT_MS 15000
#define ACTIVATE_RETRY_MS   1000
#define RTT_US              20000u      /* GetConnectionInfo: what we report */
#define BANDWIDTH_BPS       10000000u
#define NETINFO_REQ_MAX     8

static int s_initialized = 0;

typedef struct {
    int in_use;
    char self[17];             /* the context's own online id */
    u32 handler, handler_arg;  /* guest OPD and argument; 0 = none */
    u32 ext_handler, ext_arg;
    s32 bw_probe;
} SignalingCtx;

typedef struct {
    int in_use;
    u32 ctx;                   /* 1-based */
    char peer[17];
    u8 npid[SCE_NP_SIGNALING_NPID_SIZE];
    int status;
    int we_active, peer_active;
    int known;                 /* the peer's address came from the server */
    int confirmed;             /* the server has our activation (PEER, found): the peer was told */
    u8 ip[4];
    u16 port;
    int established_sent, mutual_sent;
    uint64_t due;              /* when ESTABLISHED (and MUTUAL) may go out */
    uint64_t deadline, retry_at;
} SignalingConn;

typedef struct {
    int in_use;
    u32 ctx;
    char peer[17];
    int done, found;
    u8 ip[4];
    u16 port;
} NetInfoReq;

static SignalingCtx  s_ctx[SCE_NP_SIGNALING_CTX_MAX];      /* ctx id = index + 1 */
static SignalingConn s_conn[SCE_NP_SIGNALING_CONN_MAX];    /* conn id = index + 1 */
static NetInfoReq    s_req[NETINFO_REQ_MAX];               /* req id = index + 1 */
static npsig_client* s_server;
static u8  s_mapped_ip[4] = {127, 0, 0, 1};
static int s_tick_added;

static int sig_ready(void) { return s_initialized || np_is_initialized(); }

static uint64_t now_ms(void)
{
#ifdef _WIN32
    return GetTickCount64();
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
#endif
}

static uint64_t delay_ms(void)
{
    const char* v = getenv("PS3_NP_SIGNALING_DELAY_MS");
    return v ? (uint64_t)strtoul(v, NULL, 10) : 500u;
}

static u32 ip_word(const u8 ip[4])
{
    return (u32)ip[0] << 24 | (u32)ip[1] << 16 | (u32)ip[2] << 8 | ip[3];
}

static SignalingCtx* get_ctx(u32 id)
{
    return (id >= 1 && id <= SCE_NP_SIGNALING_CTX_MAX && s_ctx[id - 1].in_use) ? &s_ctx[id - 1] : NULL;
}

static SignalingConn* get_conn(u32 ctx, u32 id)
{
    if (id < 1 || id > SCE_NP_SIGNALING_CONN_MAX) return NULL;
    SignalingConn* c = &s_conn[id - 1];
    return (c->in_use && c->ctx == ctx) ? c : NULL;
}

static u32 conn_id(const SignalingConn* c) { return (u32)(c - s_conn) + 1; }

static void read_online_id(u32 npid_ea, char out[17])
{
    memset(out, 0, 17);
    for (int i = 0; i < 16; i++) out[i] = (char)vm_read8(npid_ea + (u32)i);
}

static SignalingConn* find_conn(u32 ctx, const char* peer)
{
    for (int i = 0; i < SCE_NP_SIGNALING_CONN_MAX; i++)
        if (s_conn[i].in_use && s_conn[i].ctx == ctx && strncmp(s_conn[i].peer, peer, 16) == 0) return &s_conn[i];
    return NULL;
}

static SignalingConn* new_conn(u32 ctx, const char* peer)
{
    for (int i = 0; i < SCE_NP_SIGNALING_CONN_MAX; i++)
        if (!s_conn[i].in_use) {
            memset(&s_conn[i], 0, sizeof(s_conn[i]));
            s_conn[i].in_use = 1;
            s_conn[i].ctx = ctx;
            strncpy(s_conn[i].peer, peer, 16);
            memcpy(s_conn[i].npid, s_conn[i].peer, strlen(s_conn[i].peer));   /* an SceNpId of just the online id */
            return &s_conn[i];
        }
    return NULL;
}

static void free_conn(SignalingConn* c) { memset(c, 0, sizeof(*c)); }

/* Queue the handlers' call: the main handler takes DEAD, ESTABLISHED and
 * the net-info events; the extended handler takes every event. */
static void emit(u32 ctx, u32 subject, s32 event, s32 error)
{
    SignalingCtx* c = get_ctx(ctx);
    if (!c) return;
    printf("[sceNpSignaling] -> ctx %u conn %u event %d error 0x%08X\n", ctx, subject, event, (u32)error);
    if (c->handler && event < SCE_NP_SIGNALING_EVENT_EXT_PEER_ACTIVATED) {
        const u64 args[8] = {ctx, subject, (u64)(u32)event, (u64)(u32)error, c->handler_arg, 0, 0, 0};
        cellSysutilQueueGuestCallbackArgs(c->handler, args);
    }
    if (c->ext_handler) {
        const u64 args[8] = {ctx, subject, (u64)(u32)event, (u64)(u32)error, c->ext_arg, 0, 0, 0};
        cellSysutilQueueGuestCallbackArgs(c->ext_handler, args);
    }
}

static void server_lost(void)
{
    printf("[sceNpSignaling] signaling server connection lost\n");
    npsig_close(s_server);
    s_server = NULL;
}

static void server_send(uint8_t type, const char* peer)
{
    if (s_server && npsig_send_peer(s_server, type, peer) < 0) server_lost();
}

/* At a context's start, while not connected. */
static void server_connect(void)
{
    const char* srv = getenv("PS3_NP_SIGNALING_SERVER");
    if (s_server || !srv || !*srv) return;
    char host[256];
    uint16_t port = NPSIG_DEFAULT_PORT;
    snprintf(host, sizeof(host), "%s", srv);
    char* colon = strrchr(host, ':');
    if (colon) { *colon = 0; port = (uint16_t)atoi(colon + 1); }
    int refused = 0;
    s_server = npsig_connect(host, port, np_psnr_online_id(), np_psnr_p2p_port(), 3000, s_mapped_ip, &refused);
    if (s_server)
        printf("[sceNpSignaling] signaling server %s:%u: \"%s\", P2P port %u, seen from %u.%u.%u.%u\n", host, port,
               np_psnr_online_id(), np_psnr_p2p_port(), s_mapped_ip[0], s_mapped_ip[1], s_mapped_ip[2],
               s_mapped_ip[3]);
    else if (refused == NPSIG_REFUSED_NAME_TAKEN)
        printf("[sceNpSignaling] signaling server %s:%u: another player is \"%s\"\n", host, port,
               np_psnr_online_id());
    else
        printf("[sceNpSignaling] signaling server %s:%u not reached\n", host, port);
}

/* We activated and the server knows where the peer is: ACTIVE now, its
 * events when due. */
static void make_active(SignalingConn* c, const u8 ip[4], u16 port)
{
    memcpy(c->ip, ip, 4);
    c->port = port;
    c->known = 1;
    c->status = SCE_NP_SIGNALING_CONN_STATUS_ACTIVE;
    if (!c->due) c->due = now_ms() + delay_ms();
}

/* The peer activated first: its address is known, the connection ACTIVE. */
static void peer_opened(SignalingConn* c, const npsig_msg* m)
{
    c->peer_active = 1;
    memcpy(c->ip, m->ip, 4);
    c->port = m->port;
    c->known = 1;
    c->status = SCE_NP_SIGNALING_CONN_STATUS_ACTIVE;
    emit(c->ctx, conn_id(c), SCE_NP_SIGNALING_EVENT_EXT_PEER_ACTIVATED, 0);
}

static void on_message(const npsig_msg* m)
{
    int seen = 0;
    for (int i = 0; i < SCE_NP_SIGNALING_CONN_MAX; i++) {
        SignalingConn* c = &s_conn[i];
        if (!c->in_use || strncmp(c->peer, m->peer, 16) != 0) continue;
        seen = 1;
        switch (m->type) {
        case NPSIG_PEER:
            if (!m->found || !c->we_active) break;
            c->confirmed = 1;
            if (!c->known) make_active(c, m->ip, m->port);
            break;
        case NPSIG_ACTIVATED:
            if (c->peer_active) break;
            if (c->we_active) {
                c->peer_active = 1;                       /* MUTUAL goes out with ESTABLISHED */
                if (!c->known) make_active(c, m->ip, m->port);
            } else {
                peer_opened(c, m);
            }
            break;
        case NPSIG_DEACTIVATED:
            if (!c->peer_active) break;
            c->peer_active = 0;
            c->mutual_sent = 0;
            emit(c->ctx, conn_id(c), SCE_NP_SIGNALING_EVENT_EXT_PEER_DEACTIVATED, 0);
            if (!c->we_active) free_conn(c);
            break;
        case NPSIG_GONE:
            if (c->status != SCE_NP_SIGNALING_CONN_STATUS_INACTIVE)
                emit(c->ctx, conn_id(c), SCE_NP_SIGNALING_EVENT_DEAD, (s32)SCE_NP_SIGNALING_ERROR_TERMINATED_BY_PEER);
            free_conn(c);
            break;
        }
    }
    /* A peer we had no connection with activated one to us: it opens on the
     * first context. */
    if (m->type == NPSIG_ACTIVATED && !seen)
        for (u32 x = 1; x <= SCE_NP_SIGNALING_CTX_MAX; x++)
            if (get_ctx(x)) {
                SignalingConn* c = new_conn(x, m->peer);
                if (c) peer_opened(c, m);
                break;
            }
    if (m->type == NPSIG_PEER)
        for (int i = 0; i < NETINFO_REQ_MAX; i++) {
            NetInfoReq* r = &s_req[i];
            if (!r->in_use || r->done || strncmp(r->peer, m->peer, 16) != 0) continue;
            r->done = 1;
            r->found = m->found;
            memcpy(r->ip, m->ip, 4);
            r->port = m->port;
            emit(r->ctx, (u32)i + 1,
                 m->found ? SCE_NP_SIGNALING_EVENT_NETINFO_RESULT : SCE_NP_SIGNALING_EVENT_NETINFO_ERROR,
                 m->found ? 0 : (s32)SCE_NP_SIGNALING_ERROR_PEER_UNREACHABLE);
        }
}

void np_signaling_pump(void)
{
    npsig_msg m;
    LOCK();
    while (s_server) {
        int r = npsig_poll(s_server, &m);
        if (r == 0) break;
        if (r < 0) { server_lost(); break; }
        on_message(&m);
    }
    uint64_t now = now_ms();
    for (int i = 0; i < SCE_NP_SIGNALING_CONN_MAX; i++) {
        SignalingConn* c = &s_conn[i];
        if (!c->in_use) continue;
        if (c->we_active && !c->known && (!s_server || now >= c->deadline)) {
            emit(c->ctx, conn_id(c), SCE_NP_SIGNALING_EVENT_DEAD,
                 (s32)(s_server ? SCE_NP_SIGNALING_ERROR_TIMEOUT : SCE_NP_SIGNALING_ERROR_PEER_UNREACHABLE));
            free_conn(c);
            continue;
        }
        /* Until the server has our activation: the peer may sign in later, or
         * have activated first (then its address is known, but it has not
         * heard of ours yet). */
        if (c->we_active && !c->confirmed && now >= c->retry_at && now < c->deadline) {
            server_send(NPSIG_ACTIVATE, c->peer);
            c->retry_at = now + ACTIVATE_RETRY_MS;
        }
        if (c->we_active && !c->known) continue;
        if (c->we_active && c->due && now >= c->due) {
            if (!c->established_sent) {
                c->established_sent = 1;
                emit(c->ctx, conn_id(c), SCE_NP_SIGNALING_EVENT_ESTABLISHED, 0);
            }
            if (c->peer_active && !c->mutual_sent) {
                c->mutual_sent = 1;
                emit(c->ctx, conn_id(c), SCE_NP_SIGNALING_EVENT_EXT_MUTUAL_ACTIVATED, 0);
            }
        }
    }
    UNLOCK();
}

/* ---------------------------------------------------------------------------
 * Lifecycle and contexts
 * -----------------------------------------------------------------------*/

s32 sceNpSignalingInit(void)
{
    printf("[sceNpSignaling] Init()\n");
    if (s_initialized)
        return (s32)SCE_NP_SIGNALING_ERROR_ALREADY_INITIALIZED;
    s_initialized = 1;
    return CELL_OK;
}

s32 sceNpSignalingTerm(void)
{
    printf("[sceNpSignaling] Term()\n");
    s_initialized = 0;
    return CELL_OK;
}

/* There is no sceNpSignalingInit in the firmware's export list; titles create
 * contexts right after sceNpInit (God of War: Ascension's SAM transport does),
 * so a context is allowed once either has run. */
s32 sceNpSignalingCreateCtx(u32 npId, u32 handler, u32 arg, u32 ctxId)
{
    printf("[sceNpSignaling] CreateCtx(handler 0x%08X)\n", handler);
    if (!sig_ready()) return (s32)SCE_NP_SIGNALING_ERROR_NOT_INITIALIZED;
    if (!ctxId) return (s32)SCE_NP_SIGNALING_ERROR_INVALID_ARGUMENT;
    LOCK();
    for (int i = 0; i < SCE_NP_SIGNALING_CTX_MAX; i++) {
        if (s_ctx[i].in_use) continue;
        memset(&s_ctx[i], 0, sizeof(s_ctx[i]));
        s_ctx[i].in_use = 1;
        s_ctx[i].handler = handler;
        s_ctx[i].handler_arg = arg;
        if (npId) read_online_id(npId, s_ctx[i].self);
        else strncpy(s_ctx[i].self, np_psnr_online_id(), 16);
        server_connect();
        if (!s_tick_added) {
            s_tick_added = 1;
            np_psnr_add_tick(np_signaling_pump);
        }
        UNLOCK();
        vm_write32(ctxId, (u32)i + 1);
        return CELL_OK;
    }
    UNLOCK();
    return (s32)SCE_NP_SIGNALING_ERROR_CTX_MAX;
}

s32 sceNpSignalingDestroyCtx(u32 ctxId)
{
    printf("[sceNpSignaling] DestroyCtx(%u)\n", ctxId);
    LOCK();
    SignalingCtx* c = get_ctx(ctxId);
    if (!c) { UNLOCK(); return (s32)SCE_NP_SIGNALING_ERROR_CTX_NOT_FOUND; }
    for (int i = 0; i < SCE_NP_SIGNALING_CONN_MAX; i++)
        if (s_conn[i].in_use && s_conn[i].ctx == ctxId) {
            if (s_conn[i].we_active) server_send(NPSIG_DEACTIVATE, s_conn[i].peer);
            free_conn(&s_conn[i]);
        }
    for (int i = 0; i < NETINFO_REQ_MAX; i++)
        if (s_req[i].in_use && s_req[i].ctx == ctxId) memset(&s_req[i], 0, sizeof(s_req[i]));
    memset(c, 0, sizeof(*c));
    UNLOCK();
    return CELL_OK;
}

s32 sceNpSignalingAddExtendedHandler(u32 ctxId, u32 handler, u32 arg)
{
    printf("[sceNpSignaling] AddExtendedHandler(ctx %u, handler 0x%08X)\n", ctxId, handler);
    LOCK();
    SignalingCtx* c = get_ctx(ctxId);
    if (c) { c->ext_handler = handler; c->ext_arg = arg; }
    UNLOCK();
    return c ? CELL_OK : (s32)SCE_NP_SIGNALING_ERROR_CTX_NOT_FOUND;
}

s32 sceNpSignalingSetCtxOpt(u32 ctxId, s32 optname, s32 optval)
{
    LOCK();
    SignalingCtx* c = get_ctx(ctxId);
    s32 r = CELL_OK;
    if (!c) r = (s32)SCE_NP_SIGNALING_ERROR_CTX_NOT_FOUND;
    else if (optname != SCE_NP_SIGNALING_CTX_OPT_BANDWIDTH_PROBE ||
             (optval != SCE_NP_SIGNALING_CTX_OPT_BANDWIDTH_PROBE_DISABLE &&
              optval != SCE_NP_SIGNALING_CTX_OPT_BANDWIDTH_PROBE_ENABLE))
        r = (s32)SCE_NP_SIGNALING_ERROR_INVALID_ARGUMENT;
    else c->bw_probe = optval;
    UNLOCK();
    return r;
}

s32 sceNpSignalingGetCtxOpt(u32 ctxId, s32 optname, u32 optval)
{
    LOCK();
    SignalingCtx* c = get_ctx(ctxId);
    s32 v = c ? c->bw_probe : 0;
    UNLOCK();
    if (!c) return (s32)SCE_NP_SIGNALING_ERROR_CTX_NOT_FOUND;
    if (optname != SCE_NP_SIGNALING_CTX_OPT_BANDWIDTH_PROBE || !optval)
        return (s32)SCE_NP_SIGNALING_ERROR_INVALID_ARGUMENT;
    vm_write32(optval, (u32)v);
    return CELL_OK;
}

/* ---------------------------------------------------------------------------
 * Connections
 * -----------------------------------------------------------------------*/

s32 sceNpSignalingActivateConnection(u32 ctxId, u32 npId, u32 connId)
{
    char peer[17];
    if (!npId || !connId) return (s32)SCE_NP_SIGNALING_ERROR_INVALID_ARGUMENT;
    read_online_id(npId, peer);
    LOCK();
    SignalingCtx* x = get_ctx(ctxId);
    if (!x) { UNLOCK(); return (s32)SCE_NP_SIGNALING_ERROR_CTX_NOT_FOUND; }
    if (!peer[0]) { UNLOCK(); return (s32)SCE_NP_SIGNALING_ERROR_INVALID_ARGUMENT; }
    if (strncmp(peer, x->self, 16) == 0) { UNLOCK(); return (s32)SCE_NP_SIGNALING_ERROR_OWN_NP_ID; }
    SignalingConn* c = find_conn(ctxId, peer);
    if (!c && !(c = new_conn(ctxId, peer))) { UNLOCK(); return (s32)SCE_NP_SIGNALING_ERROR_TOO_MANY_CONN; }
    for (int i = 0; i < SCE_NP_SIGNALING_NPID_SIZE; i++) c->npid[i] = vm_read8(npId + (u32)i);
    if (!c->we_active) {
        uint64_t now = now_ms();
        c->we_active = 1;
        c->deadline = now + ACTIVATE_TIMEOUT_MS;
        c->retry_at = now + ACTIVATE_RETRY_MS;
        if (c->known) {
            c->due = now + delay_ms();   /* the peer activated first: ESTABLISHED and MUTUAL when due */
        } else {
            c->status = SCE_NP_SIGNALING_CONN_STATUS_PENDING;
        }
        server_send(NPSIG_ACTIVATE, peer);
    }
    u32 id = conn_id(c);
    int online = s_server != NULL;
    UNLOCK();
    printf("[sceNpSignaling] ActivateConnection(ctx %u, \"%s\") -> conn %u%s\n", ctxId, peer, id,
           online ? "" : " (no signaling server: it will be DEAD)");
    vm_write32(connId, id);
    return CELL_OK;
}

static s32 close_conn(u32 ctxId, u32 connId, const char* what)
{
    LOCK();
    if (!get_ctx(ctxId)) { UNLOCK(); return (s32)SCE_NP_SIGNALING_ERROR_CTX_NOT_FOUND; }
    SignalingConn* c = get_conn(ctxId, connId);
    if (!c) { UNLOCK(); return (s32)SCE_NP_SIGNALING_ERROR_CONN_NOT_FOUND; }
    printf("[sceNpSignaling] %s(ctx %u, conn %u \"%s\")\n", what, ctxId, connId, c->peer);
    if (c->we_active) server_send(NPSIG_DEACTIVATE, c->peer);
    free_conn(c);
    UNLOCK();
    return CELL_OK;
}

s32 sceNpSignalingDeactivateConnection(u32 ctxId, u32 connId)
{
    return close_conn(ctxId, connId, "DeactivateConnection");
}

s32 sceNpSignalingTerminateConnection(u32 ctxId, u32 connId)
{
    return close_conn(ctxId, connId, "TerminateConnection");
}

s32 sceNpSignalingGetConnectionStatus(u32 ctxId, u32 connId, u32 connStatus, u32 peerAddr, u32 peerPort)
{
    LOCK();
    int ok_ctx = get_ctx(ctxId) != NULL;
    SignalingConn* c = get_conn(ctxId, connId);
    int status = c ? c->status : SCE_NP_SIGNALING_CONN_STATUS_INACTIVE;
    u32 addr = (c && c->known) ? ip_word(c->ip) : 0;
    u16 port = (c && c->known) ? c->port : 0;
    UNLOCK();
    if (!ok_ctx) return (s32)SCE_NP_SIGNALING_ERROR_CTX_NOT_FOUND;
    if (connStatus) vm_write32(connStatus, (u32)status);
    if (peerAddr) vm_write32(peerAddr, addr);     /* in_addr, network order */
    if (peerPort) vm_write16(peerPort, port);     /* network order */
    return c ? CELL_OK : (s32)SCE_NP_SIGNALING_ERROR_CONN_NOT_FOUND;
}

s32 sceNpSignalingGetConnectionInfo(u32 ctxId, u32 connId, s32 code, u32 info)
{
    if (!info) return (s32)SCE_NP_SIGNALING_ERROR_INVALID_ARGUMENT;
    SignalingConn copy;
    LOCK();
    int ok_ctx = get_ctx(ctxId) != NULL;
    SignalingConn* c = get_conn(ctxId, connId);
    if (c) copy = *c;
    UNLOCK();
    if (!ok_ctx) return (s32)SCE_NP_SIGNALING_ERROR_CTX_NOT_FOUND;
    if (!c) return (s32)SCE_NP_SIGNALING_ERROR_CONN_NOT_FOUND;
    switch (code) {
    case SCE_NP_SIGNALING_CONN_INFO_RTT:         vm_write32(info, RTT_US); break;
    case SCE_NP_SIGNALING_CONN_INFO_BANDWIDTH:   vm_write32(info, BANDWIDTH_BPS); break;
    case SCE_NP_SIGNALING_CONN_INFO_PACKET_LOSS: vm_write32(info, 0); break;
    case SCE_NP_SIGNALING_CONN_INFO_PEER_NPID:
        for (int i = 0; i < SCE_NP_SIGNALING_NPID_SIZE; i++) vm_write8(info + (u32)i, copy.npid[i]);
        break;
    case SCE_NP_SIGNALING_CONN_INFO_PEER_ADDRESS:
        vm_write32(info, copy.known ? ip_word(copy.ip) : 0);
        vm_write16(info + 4, copy.known ? copy.port : 0);
        break;
    case SCE_NP_SIGNALING_CONN_INFO_MAPPED_ADDRESS:
        vm_write32(info, ip_word(s_mapped_ip));
        vm_write16(info + 4, np_psnr_p2p_port());
        break;
    default:
        return (s32)SCE_NP_SIGNALING_ERROR_INVALID_ARGUMENT;
    }
    return CELL_OK;
}

s32 sceNpSignalingGetConnectionFromNpId(u32 ctxId, u32 npId, u32 connId)
{
    char peer[17];
    if (!npId || !connId) return (s32)SCE_NP_SIGNALING_ERROR_INVALID_ARGUMENT;
    read_online_id(npId, peer);
    LOCK();
    int ok_ctx = get_ctx(ctxId) != NULL;
    SignalingConn* c = ok_ctx ? find_conn(ctxId, peer) : NULL;
    u32 id = c ? conn_id(c) : 0;
    UNLOCK();
    if (!ok_ctx) return (s32)SCE_NP_SIGNALING_ERROR_CTX_NOT_FOUND;
    if (!id) return (s32)SCE_NP_SIGNALING_ERROR_CONN_NOT_FOUND;
    vm_write32(connId, id);
    return CELL_OK;
}

s32 sceNpSignalingGetConnectionFromPeerAddress(u32 ctxId, u32 peerAddr, u32 peerPort, u32 connId)
{
    if (!connId) return (s32)SCE_NP_SIGNALING_ERROR_INVALID_ARGUMENT;
    LOCK();
    int ok_ctx = get_ctx(ctxId) != NULL;
    u32 id = 0;
    for (int i = 0; ok_ctx && i < SCE_NP_SIGNALING_CONN_MAX && !id; i++) {
        SignalingConn* c = &s_conn[i];
        if (c->in_use && c->ctx == ctxId && c->known && ip_word(c->ip) == peerAddr && c->port == (u16)peerPort)
            id = conn_id(c);
    }
    UNLOCK();
    if (!ok_ctx) return (s32)SCE_NP_SIGNALING_ERROR_CTX_NOT_FOUND;
    if (!id) return (s32)SCE_NP_SIGNALING_ERROR_CONN_NOT_FOUND;
    vm_write32(connId, id);
    return CELL_OK;
}

/* ---------------------------------------------------------------------------
 * Net info
 * -----------------------------------------------------------------------*/

static void write_netinfo(u32 info, const u8 ip[4], u16 port)
{
    vm_write32(info + 4, ip_word(ip));                                  /* local_addr */
    vm_write32(info + 8, ip_word(ip));                                  /* mapped_addr */
    vm_write32(info + 12, SCE_NP_SIGNALING_NETINFO_NAT_STATUS_TYPE2);
    vm_write32(info + 16, SCE_NP_SIGNALING_NETINFO_UPNP_STATUS_VALID);
    vm_write32(info + 20, SCE_NP_SIGNALING_NETINFO_NPPORT_STATUS_OPEN);
    vm_write16(info + 24, port);
}

s32 sceNpSignalingGetLocalNetInfo(u32 ctxId, u32 info)
{
    if (!info) return (s32)SCE_NP_SIGNALING_ERROR_INVALID_ARGUMENT;
    u8 ip[4];
    LOCK();
    int ok_ctx = get_ctx(ctxId) != NULL;
    memcpy(ip, s_mapped_ip, 4);
    UNLOCK();
    if (!ok_ctx) return (s32)SCE_NP_SIGNALING_ERROR_CTX_NOT_FOUND;
    write_netinfo(info, ip, np_psnr_p2p_port());
    return CELL_OK;
}

s32 sceNpSignalingGetPeerNetInfo(u32 ctxId, u32 npId, u32 reqId)
{
    char peer[17];
    if (!npId || !reqId) return (s32)SCE_NP_SIGNALING_ERROR_INVALID_ARGUMENT;
    read_online_id(npId, peer);
    LOCK();
    if (!get_ctx(ctxId)) { UNLOCK(); return (s32)SCE_NP_SIGNALING_ERROR_CTX_NOT_FOUND; }
    for (int i = 0; i < NETINFO_REQ_MAX; i++) {
        if (s_req[i].in_use) continue;
        memset(&s_req[i], 0, sizeof(s_req[i]));
        s_req[i].in_use = 1;
        s_req[i].ctx = ctxId;
        memcpy(s_req[i].peer, peer, sizeof(peer));
        if (s_server) {
            server_send(NPSIG_LOOKUP, peer);
        } else {
            s_req[i].done = 1;
            emit(ctxId, (u32)i + 1, SCE_NP_SIGNALING_EVENT_NETINFO_ERROR,
                 (s32)SCE_NP_SIGNALING_ERROR_PEER_UNREACHABLE);
        }
        UNLOCK();
        vm_write32(reqId, (u32)i + 1);
        return CELL_OK;
    }
    UNLOCK();
    return (s32)SCE_NP_SIGNALING_ERROR_REQID_NOT_AVAILABLE;
}

s32 sceNpSignalingCancelPeerNetInfo(u32 ctxId, u32 reqId)
{
    LOCK();
    int ok = get_ctx(ctxId) && reqId >= 1 && reqId <= NETINFO_REQ_MAX && s_req[reqId - 1].in_use &&
             s_req[reqId - 1].ctx == ctxId;
    if (ok) memset(&s_req[reqId - 1], 0, sizeof(s_req[0]));
    UNLOCK();
    return ok ? CELL_OK : (s32)SCE_NP_SIGNALING_ERROR_REQ_NOT_FOUND;
}

s32 sceNpSignalingGetPeerNetInfoResult(u32 ctxId, u32 reqId, u32 info)
{
    if (!info) return (s32)SCE_NP_SIGNALING_ERROR_INVALID_ARGUMENT;
    NetInfoReq r;
    LOCK();
    int ok = get_ctx(ctxId) && reqId >= 1 && reqId <= NETINFO_REQ_MAX && s_req[reqId - 1].in_use &&
             s_req[reqId - 1].ctx == ctxId;
    if (ok) r = s_req[reqId - 1];
    UNLOCK();
    if (!ok) return (s32)SCE_NP_SIGNALING_ERROR_REQ_NOT_FOUND;
    if (!r.done) return (s32)SCE_NP_SIGNALING_ERROR_RESULT_NOT_FOUND;
    if (!r.found) return (s32)SCE_NP_SIGNALING_ERROR_PEER_UNREACHABLE;
    write_netinfo(info, r.ip, r.port);
    return CELL_OK;
}
