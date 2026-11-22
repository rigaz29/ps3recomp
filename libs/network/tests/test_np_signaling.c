/*
 * test_np_signaling - two players connect through the signaling server.
 *
 * tools/test_np_signaling.py starts tools/np_signaling_server.py and runs
 * this twice at once, as PlayerA (P2P port 3658) and PlayerB (3659). Each
 * has a real UDP socket for its "P2P socket", read the way sysNet reads a
 * title's (every datagram through the signaling filter first). Each creates
 * a context with both handlers, activates a connection to the other and
 * pumps until ESTABLISHED and EXT_MUTUAL_ACTIVATED have arrived: the socket
 * has probed the server and the punches have been answered (or, with
 * PS3_NP_SIGNALING_RELAY=1, the server relays). The connection is then
 * ACTIVE with the other's address and P2P port, and "game" datagrams go
 * both ways, the other's address on what arrives. B leaves, and A sees DEAD
 * (TERMINATED_BY_PEER). With no server, an activation ends DEAD
 * (PEER_UNREACHABLE).
 *
 *   cc -std=gnu17 -I include -o test_np_signaling libs/network/tests/test_np_signaling.c \
 *      libs/network/np_signal_client.c [-lws2_32]
 *   test_np_signaling a|b|offline
 * with PS3_NP_SIGNALING_SERVER, PS3_NP_ONLINE_ID, PS3_NET_P2P_PORT, PEER_NAME and PEER_PORT set.
 */
#include "../sceNpSignaling.c"
#include <stdlib.h>
#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
typedef SOCKET usock_t;
static void sleep_ms(int ms) { Sleep((DWORD)ms); }
static void set_nb(usock_t s) { u_long v = 1; ioctlsocket(s, FIONBIO, &v); }
#else
#include <unistd.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <netinet/in.h>
typedef int usock_t;
static void sleep_ms(int ms) { usleep((useconds_t)ms * 1000); }
static void set_nb(usock_t s) { fcntl(s, F_SETFL, fcntl(s, F_GETFL, 0) | O_NONBLOCK); }
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
static int s_allowed;
void sys_net_allow_peer(const uint8_t ip[4]) { (void)ip; s_allowed++; }

/* The "P2P socket": a UDP socket on our P2P port, and np_psnr's hooks. */
static usock_t s_udp = (usock_t)-1;
static np_p2p_filter_fn s_filter;
static np_p2p_route_fn s_route;
void np_psnr_set_p2p_hooks(np_p2p_filter_fn filter, np_p2p_route_fn route) { s_filter = filter; s_route = route; }
int np_psnr_p2p_send(const void* buf, uint32_t len, const uint8_t ip[4], uint16_t port)
{
    struct sockaddr_in a;
    if (s_udp == (usock_t)-1) return -1;
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    memcpy(&a.sin_addr, ip, 4);
    a.sin_port = htons(port);
    return (int)sendto(s_udp, (const char*)buf, (int)len, 0, (struct sockaddr*)&a, sizeof(a));
}

/* What sysNet's sendto does with a title's datagram. */
static void game_send(const void* buf, uint32_t len, const uint8_t ip[4], uint16_t port)
{
    if (s_route && s_route(buf, len, ip, port)) return;
    np_psnr_p2p_send(buf, len, ip, port);
}

/* What sysNet's recvfrom does: our datagrams go to the filter first; the
 * title's land here. */
static char s_got[256];
static uint8_t s_got_ip[4];
static uint16_t s_got_port;
static void net_poll(void)
{
    uint8_t buf[2048];
    for (;;) {
        struct sockaddr_in a;
        socklen_t al = sizeof(a);
        int n = (int)recvfrom(s_udp, (char*)buf, sizeof(buf), 0, (struct sockaddr*)&a, &al);
        if (n <= 0) return;
        uint32_t len = (uint32_t)n;
        uint8_t ip[4];
        uint16_t port = ntohs(a.sin_port);
        memcpy(ip, &a.sin_addr, 4);
        if (s_filter && s_filter(buf, &len, ip, &port)) continue;
        if (len < sizeof(s_got)) {
            memcpy(s_got, buf, len);
            s_got[len] = 0;
            memcpy(s_got_ip, ip, 4);
            s_got_port = port;
        }
    }
}

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

static void step(void)
{
    net_poll();
    if (s_tick) s_tick();
    sleep_ms(20);
}

/* Pump for up to ms, until done() says so. */
static int pump_until(int (*done)(u32), u32 conn, int ms)
{
    for (int t = 0; t < ms; t += 20) {
        step();
        if (done(conn)) return 1;
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

static const char* s_expect;
static int got_expected(u32 conn) { (void)conn; return strcmp(s_got, s_expect) == 0; }

int main(int argc, char** argv)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    const char* role = argc > 1 ? argv[1] : "offline";
    const char* peer = getenv("PEER_NAME") ? getenv("PEER_NAME") : "PlayerB";
    u16 peer_port = (u16)(getenv("PEER_PORT") ? atoi(getenv("PEER_PORT")) : 3659);
    int relay = getenv("PS3_NP_SIGNALING_RELAY") != NULL;
    vm_base = calloc(1, 65536);
    if (!vm_base) return 2;
#ifdef _WIN32
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
#endif
    s_udp = socket(AF_INET, SOCK_DGRAM, 0);
    struct sockaddr_in me;
    memset(&me, 0, sizeof(me));
    me.sin_family = AF_INET;
    me.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    me.sin_port = htons(np_psnr_p2p_port());
    if (bind(s_udp, (struct sockaddr*)&me, sizeof(me)) != 0) { printf("FAIL: P2P port %u taken\n", np_psnr_p2p_port()); return 1; }
    set_nb(s_udp);
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

    CHECK(pump_until(connected, conn, 15000), "ESTABLISHED (both handlers) and EXT_MUTUAL_ACTIVATED");
    CHECK(s_probed, "the P2P socket probed the server");
    LOCK();
    SignalingConn* sc = get_conn(ctx, conn);
    int direct = sc && sc->direct, relayed = sc && sc->relay;
    UNLOCK();
    if (relay) CHECK(relayed, "PS3_NP_SIGNALING_RELAY: the path is the relay");
    else CHECK(direct, "punches answered: the path is direct");
    CHECK(s_allowed > 0, "the peer's address went to the network guard");
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
          "GetLocalNetInfo: our P2P port, as the probe saw it");
    CHECK(sceNpSignalingActivateConnection(ctx, PEER_NPID, CONN_OUT) == CELL_OK && vm_read32(CONN_OUT) == conn,
          "activating again gives the same connection");

    /* The title's own datagrams, both ways, from the address the game knows. */
    const uint8_t lo[4] = {127, 0, 0, 1};
    char mine[64], theirs[64];
    snprintf(mine, sizeof(mine), "game data from %s", np_psnr_online_id());
    snprintf(theirs, sizeof(theirs), "game data from %s", peer);
    s_expect = theirs;
    int got = 0;
    for (int t = 0; t < 5000 && !got; t += 100) {
        game_send(mine, (uint32_t)strlen(mine), lo, peer_port);
        for (int k = 0; k < 5 && !got; k++) { step(); got = got_expected(0); }
    }
    CHECK(got, relay ? "the peer's datagram arrived through the relay" : "the peer's datagram arrived");
    CHECK(got && memcmp(s_got_ip, lo, 4) == 0 && s_got_port == peer_port, "...from the peer's address");
    /* let the peer get ours before anyone leaves */
    for (int t = 0; t < 1000; t += 20) { game_send(mine, (uint32_t)strlen(mine), lo, peer_port); step(); }

    if (strcmp(role, "a") == 0) {
        CHECK(pump_until(dead_by_peer, conn, 10000), "the peer left: DEAD, TERMINATED_BY_PEER");
    } else {
        for (int t = 0; t < 1500; t += 20) step();
        CHECK(sceNpSignalingDestroyCtx(ctx) == CELL_OK, "DestroyCtx");
        CHECK(sceNpSignalingGetConnectionStatus(ctx, conn, STATUS, ADDR, PORT) ==
              (s32)SCE_NP_SIGNALING_ERROR_CTX_NOT_FOUND, "the context is gone");
    }
    printf("%s\n", fails ? "FAILED" : "all passed");
    return fails ? 1 : 0;
}
