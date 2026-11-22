/*
 * test_np_signaling - two players connect through the signaling server.
 *
 * tools/test_np_signaling.py starts tools/np_signaling_server.py and runs
 * this twice at once, as PlayerA (P2P port 3658) and PlayerB (3659). Each
 * creates a context with both handlers, activates a connection to the other
 * and pumps until ESTABLISHED and EXT_MUTUAL_ACTIVATED have arrived; the
 * connection is then ACTIVE with the other's address and P2P port. B leaves,
 * and A sees DEAD (TERMINATED_BY_PEER). With no server, an activation ends
 * DEAD (PEER_UNREACHABLE).
 *
 *   cc -std=gnu17 -I include -o test_np_signaling libs/network/tests/test_np_signaling.c \
 *      libs/network/np_signal_client.c [-lws2_32]
 *   test_np_signaling a|b|offline
 * with PS3_NP_SIGNALING_SERVER, PS3_NP_ONLINE_ID, PS3_NET_P2P_PORT, PEER_NAME and PEER_PORT set.
 */
#include "../sceNpSignaling.c"
#include <stdlib.h>
#ifndef _WIN32
#include <unistd.h>
static void sleep_ms(int ms) { usleep((useconds_t)ms * 1000); }
#else
static void sleep_ms(int ms) { Sleep((DWORD)ms); }
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

int np_is_initialized(void) { return 1; }
const char* np_psnr_online_id(void) { const char* v = getenv("PS3_NP_ONLINE_ID"); return v ? v : "PS3Player"; }
uint16_t np_psnr_p2p_port(void) { const char* v = getenv("PS3_NET_P2P_PORT"); return (uint16_t)(v ? atoi(v) : 3658); }
static void (*s_tick)(void);
void np_psnr_add_tick(void (*fn)(void)) { s_tick = fn; }

#define MAIN_OPD 0x2000u
#define EXT_OPD  0x2100u
#define EVENTS_MAX 64
static struct { u32 opd; u64 a[5]; } s_ev[EVENTS_MAX];
static int s_nev;
s32 cellSysutilQueueGuestCallbackArgs(u32 opd, const u64 args[8])
{
    if (s_nev < EVENTS_MAX) {
        s_ev[s_nev].opd = opd;
        memcpy(s_ev[s_nev].a, args, sizeof(s_ev[0].a));
        s_nev++;
    }
    return 0;
}

static int seen(u32 opd, u32 conn, s32 event, s32 error)
{
    for (int i = 0; i < s_nev; i++)
        if (s_ev[i].opd == opd && (u32)s_ev[i].a[1] == conn && (s32)s_ev[i].a[2] == event &&
            (s32)s_ev[i].a[3] == error)
            return 1;
    return 0;
}

static int fails = 0;
#define CHECK(cond, what) do { int ok_ = (cond) ? 1 : 0; printf("[%s] %s\n", ok_ ? "PASS" : "FAIL", what); \
        if (!ok_) fails++; } while (0)

#define SELF_NPID 0x1000u
#define PEER_NPID 0x1100u
#define CTX_OUT   0x1200u
#define CONN_OUT  0x1204u
#define STATUS    0x1210u
#define ADDR      0x1214u
#define PORT      0x1218u
#define INFO      0x1300u

static void put_npid(u32 ea, const char* name)
{
    for (u32 i = 0; i < SCE_NP_SIGNALING_NPID_SIZE; i++) vm_write8(ea + i, 0);
    for (u32 i = 0; i < 16 && name[i]; i++) vm_write8(ea + i, (u8)name[i]);
}

/* Pump for up to ms, until done() says so. */
static int pump_until(int (*done)(u32), u32 conn, int ms)
{
    for (int t = 0; t < ms; t += 20) {
        if (s_tick) s_tick();
        if (done(conn)) return 1;
        sleep_ms(20);
    }
    return 0;
}

static int connected(u32 conn)
{
    return seen(EXT_OPD, conn, SCE_NP_SIGNALING_EVENT_ESTABLISHED, 0) &&
           seen(EXT_OPD, conn, SCE_NP_SIGNALING_EVENT_EXT_MUTUAL_ACTIVATED, 0) &&
           seen(MAIN_OPD, conn, SCE_NP_SIGNALING_EVENT_ESTABLISHED, 0);
}

static int dead_by_peer(u32 conn)
{
    return seen(EXT_OPD, conn, SCE_NP_SIGNALING_EVENT_DEAD, (s32)SCE_NP_SIGNALING_ERROR_TERMINATED_BY_PEER);
}

static int dead_unreachable(u32 conn)
{
    return seen(EXT_OPD, conn, SCE_NP_SIGNALING_EVENT_DEAD, (s32)SCE_NP_SIGNALING_ERROR_PEER_UNREACHABLE);
}

int main(int argc, char** argv)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    const char* role = argc > 1 ? argv[1] : "offline";
    const char* peer = getenv("PEER_NAME") ? getenv("PEER_NAME") : "PlayerB";
    u16 peer_port = (u16)(getenv("PEER_PORT") ? atoi(getenv("PEER_PORT")) : 3659);
    vm_base = calloc(1, 65536);
    if (!vm_base) return 2;
    put_npid(SELF_NPID, np_psnr_online_id());
    put_npid(PEER_NPID, peer);

    CHECK(sceNpSignalingCreateCtx(SELF_NPID, MAIN_OPD, 0xA1, CTX_OUT) == CELL_OK, "CreateCtx");
    u32 ctx = vm_read32(CTX_OUT);
    CHECK(ctx == 1, "the first context is 1 (titles take 0 for none)");
    CHECK(sceNpSignalingAddExtendedHandler(ctx, EXT_OPD, 0xE1) == CELL_OK, "AddExtendedHandler");
    CHECK(sceNpSignalingSetCtxOpt(ctx, SCE_NP_SIGNALING_CTX_OPT_BANDWIDTH_PROBE, 0) == CELL_OK, "SetCtxOpt");
    CHECK(sceNpSignalingSetCtxOpt(ctx, 7, 0) == (s32)SCE_NP_SIGNALING_ERROR_INVALID_ARGUMENT, "SetCtxOpt: no option 7");
    CHECK(sceNpSignalingActivateConnection(ctx, SELF_NPID, CONN_OUT) == (s32)SCE_NP_SIGNALING_ERROR_OWN_NP_ID,
          "ActivateConnection to ourselves");
    CHECK(sceNpSignalingActivateConnection(ctx, PEER_NPID, CONN_OUT) == CELL_OK, "ActivateConnection");
    u32 conn = vm_read32(CONN_OUT);
    CHECK(conn >= 1, "a connection id");

    if (strcmp(role, "offline") == 0) {
        CHECK(pump_until(dead_unreachable, conn, 2000), "no server: DEAD, PEER_UNREACHABLE");
        CHECK(sceNpSignalingGetConnectionStatus(ctx, conn, STATUS, ADDR, PORT) ==
              (s32)SCE_NP_SIGNALING_ERROR_CONN_NOT_FOUND, "the dead connection is gone");
        printf("%s\n", fails ? "FAILED" : "all passed");
        return fails ? 1 : 0;
    }

    CHECK(pump_until(connected, conn, 10000), "ESTABLISHED (both handlers) and EXT_MUTUAL_ACTIVATED");
    for (int i = 0; i < s_nev; i++)
        if (s_ev[i].opd == MAIN_OPD && s_ev[i].a[2] >= SCE_NP_SIGNALING_EVENT_EXT_PEER_ACTIVATED)
            CHECK(0, "the main handler takes no extended event");
    CHECK(s_nev > 0 && s_ev[0].a[0] == ctx && s_ev[0].a[4] == ((s_ev[0].opd == EXT_OPD) ? 0xE1u : 0xA1u),
          "handler arguments: context id first, the handler's own argument last");
    CHECK(sceNpSignalingGetConnectionStatus(ctx, conn, STATUS, ADDR, PORT) == CELL_OK, "GetConnectionStatus");
    CHECK(vm_read32(STATUS) == SCE_NP_SIGNALING_CONN_STATUS_ACTIVE, "ACTIVE");
    CHECK(vm_read32(ADDR) == 0x7F000001u, "the peer at 127.0.0.1");
    CHECK(vm_read16(PORT) == peer_port, "the peer's P2P port");
    CHECK(sceNpSignalingGetConnectionInfo(ctx, conn, SCE_NP_SIGNALING_CONN_INFO_PEER_ADDRESS, INFO) == CELL_OK &&
          vm_read32(INFO) == 0x7F000001u && vm_read16(INFO + 4) == peer_port, "GetConnectionInfo: PEER_ADDRESS");
    CHECK(sceNpSignalingGetConnectionInfo(ctx, conn, SCE_NP_SIGNALING_CONN_INFO_PEER_NPID, INFO) == CELL_OK &&
          vm_read8(INFO) == (u8)peer[0] && vm_read8(INFO + 1) == (u8)peer[1], "GetConnectionInfo: PEER_NPID");
    CHECK(sceNpSignalingGetConnectionInfo(ctx, conn, SCE_NP_SIGNALING_CONN_INFO_RTT, INFO) == CELL_OK &&
          vm_read32(INFO) > 0, "GetConnectionInfo: RTT");
    vm_write32(CONN_OUT, 0);
    CHECK(sceNpSignalingGetConnectionFromNpId(ctx, PEER_NPID, CONN_OUT) == CELL_OK && vm_read32(CONN_OUT) == conn,
          "GetConnectionFromNpId");
    CHECK(sceNpSignalingGetConnectionFromPeerAddress(ctx, 0x7F000001u, peer_port, CONN_OUT) == CELL_OK &&
          vm_read32(CONN_OUT) == conn, "GetConnectionFromPeerAddress");
    CHECK(sceNpSignalingGetLocalNetInfo(ctx, INFO) == CELL_OK && vm_read16(INFO + 24) == np_psnr_p2p_port(),
          "GetLocalNetInfo: our P2P port");
    CHECK(sceNpSignalingActivateConnection(ctx, PEER_NPID, CONN_OUT) == CELL_OK && vm_read32(CONN_OUT) == conn,
          "activating again gives the same connection");

    if (strcmp(role, "a") == 0) {
        CHECK(pump_until(dead_by_peer, conn, 10000), "the peer left: DEAD, TERMINATED_BY_PEER");
    } else {
        /* let A see its MUTUAL before we go */
        for (int t = 0; t < 1500; t += 20) { if (s_tick) s_tick(); sleep_ms(20); }
        CHECK(sceNpSignalingDestroyCtx(ctx) == CELL_OK, "DestroyCtx");
        CHECK(sceNpSignalingGetConnectionStatus(ctx, conn, STATUS, ADDR, PORT) ==
              (s32)SCE_NP_SIGNALING_ERROR_CTX_NOT_FOUND, "the context is gone");
    }
    printf("%s\n", fails ? "FAILED" : "all passed");
    return fails ? 1 : 0;
}
