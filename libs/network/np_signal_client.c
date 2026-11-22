/*
 * np_signal_client.c - see np_signal_client.h. The socket is nonblocking
 * after the HELLO; npsig_poll reads what has arrived and cuts it into
 * messages.
 */
#include "np_signal_client.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef _WIN32
#  if !defined(_WIN32_WINNT) || _WIN32_WINNT < 0x0600
#    undef _WIN32_WINNT
#    define _WIN32_WINNT 0x0600   /* WSAPoll, GetTickCount64 */
#  endif
#  define WIN32_LEAN_AND_MEAN
#  include <winsock2.h>
#  include <ws2tcpip.h>
#  ifdef _MSC_VER
#    pragma comment(lib, "ws2_32.lib")
#  endif
   typedef SOCKET sock_t;
#  define BAD_SOCK INVALID_SOCKET
#  define sock_close closesocket
#  define WOULD_BLOCK() (WSAGetLastError() == WSAEWOULDBLOCK)
#  define IN_PROGRESS() (WSAGetLastError() == WSAEWOULDBLOCK)
#  define poll WSAPoll
   typedef WSAPOLLFD pollfd_t;
#else
#  include <sys/socket.h>
#  include <netdb.h>
#  include <netinet/in.h>
#  include <netinet/tcp.h>
#  include <fcntl.h>
#  include <unistd.h>
#  include <errno.h>
#  include <poll.h>
   typedef int sock_t;
#  define BAD_SOCK (-1)
#  define sock_close close
#  define WOULD_BLOCK() (errno == EAGAIN || errno == EWOULDBLOCK)
#  define IN_PROGRESS() (errno == EINPROGRESS)
   typedef struct pollfd pollfd_t;
#endif

#include "host_socket_send.h"

#define PING_MS 20000
#define RX_MAX  4096

struct npsig_client {
    sock_t   s;
    uint8_t  rx[RX_MAX];
    size_t   rx_len;
    uint64_t last_send;
    int      dead;
};

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

static void set_nonblocking(sock_t s, int on)
{
#ifdef _WIN32
    u_long v = on ? 1 : 0;
    ioctlsocket(s, FIONBIO, &v);
#else
    int fl = fcntl(s, F_GETFL, 0);
    fcntl(s, F_SETFL, on ? (fl | O_NONBLOCK) : (fl & ~O_NONBLOCK));
#endif
}

static int wait_for(sock_t s, short events, int timeout_ms)
{
    pollfd_t p;
    memset(&p, 0, sizeof(p));
    p.fd = s;
    p.events = events;
    return poll(&p, 1, timeout_ms) > 0 && (p.revents & events);
}

/* Write all of buf; the socket may be nonblocking, so wait while it is full. */
static int send_all(npsig_client* c, const uint8_t* p, size_t n)
{
    while (n) {
        int k = send(c->s, (const char*)p, (int)n, PS3_NET_HOST_SEND_FLAGS);
        if (k < 0) {
            if (WOULD_BLOCK() && wait_for(c->s, POLLOUT, 1000)) continue;
            c->dead = 1;
            return -1;
        }
        p += k;
        n -= (size_t)k;
    }
    c->last_send = now_ms();
    return 0;
}

static int send_msg(npsig_client* c, uint8_t type, const uint8_t* body, uint16_t len)
{
    uint8_t buf[64];
    if (c->dead || len + 3u > sizeof(buf)) return -1;
    buf[0] = (uint8_t)((len + 1) >> 8);
    buf[1] = (uint8_t)(len + 1);
    buf[2] = type;
    if (len) memcpy(buf + 3, body, len);
    return send_all(c, buf, len + 3u);
}

static void put_id(uint8_t out[16], const char* id)
{
    memset(out, 0, 16);
    for (int i = 0; i < 16 && id && id[i]; i++) out[i] = (uint8_t)id[i];
}

/* One complete message at the front of rx: its length, or 0. */
static size_t frame_len(const npsig_client* c)
{
    if (c->rx_len < 2) return 0;
    size_t n = 2u + ((size_t)c->rx[0] << 8 | c->rx[1]);
    return c->rx_len >= n ? n : 0;
}

static void parse(const uint8_t* f, size_t n, npsig_msg* out)
{
    memset(out, 0, sizeof(*out));
    out->type = n > 2 ? f[2] : 0;
    const uint8_t* p = f + 3;
    size_t left = n > 3 ? n - 3 : 0;
    if (out->type == NPSIG_WELCOME && left >= 4) memcpy(out->ip, p, 4);
    if (out->type == NPSIG_REFUSED && left >= 1) out->found = p[0];
    if ((out->type == NPSIG_PEER || out->type == NPSIG_ACTIVATED || out->type == NPSIG_DEACTIVATED ||
         out->type == NPSIG_GONE) && left >= 16) {
        memcpy(out->peer, p, 16);
        p += 16;
        left -= 16;
        if (out->type == NPSIG_PEER && left >= 7) {
            out->found = p[0];
            memcpy(out->ip, p + 1, 4);
            out->port = (uint16_t)(p[5] << 8 | p[6]);
        }
        if (out->type == NPSIG_ACTIVATED && left >= 6) {
            memcpy(out->ip, p, 4);
            out->port = (uint16_t)(p[4] << 8 | p[5]);
        }
    }
}

/* Read what has arrived (never blocks). -1 when the connection is gone. */
static int fill(npsig_client* c)
{
    while (c->rx_len < RX_MAX) {
        int k = recv(c->s, (char*)c->rx + c->rx_len, (int)(RX_MAX - c->rx_len), 0);
        if (k > 0) { c->rx_len += (size_t)k; continue; }
        if (k < 0 && WOULD_BLOCK()) return 0;
        c->dead = 1;
        return -1;
    }
    return 0;
}

static void drop_front(npsig_client* c, size_t n)
{
    memmove(c->rx, c->rx + n, c->rx_len - n);
    c->rx_len -= n;
}

npsig_client* npsig_connect(const char* host, uint16_t port, const char* online_id, uint16_t p2p_port,
                            int timeout_ms, uint8_t out_ip[4], int* refused)
{
    struct addrinfo hints, *res = NULL;
    char service[8];
    if (refused) *refused = 0;
#ifdef _WIN32
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
#endif
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    snprintf(service, sizeof(service), "%u", port);
    if (getaddrinfo(host, service, &hints, &res) != 0 || !res) return NULL;
    sock_t s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == BAD_SOCK) { freeaddrinfo(res); return NULL; }
    set_nonblocking(s, 1);
    int r = connect(s, res->ai_addr, (int)res->ai_addrlen);
    freeaddrinfo(res);
    if (r != 0 && !(IN_PROGRESS() && wait_for(s, POLLOUT, timeout_ms))) { sock_close(s); return NULL; }
    int err = 0;
    socklen_t elen = sizeof(err);
    getsockopt(s, SOL_SOCKET, SO_ERROR, (char*)&err, &elen);
    if (err) { sock_close(s); return NULL; }
    int one = 1;
    setsockopt(s, IPPROTO_TCP, TCP_NODELAY, (const char*)&one, sizeof(one));

    npsig_client* c = calloc(1, sizeof(*c));
    if (!c) { sock_close(s); return NULL; }
    c->s = s;
    uint8_t hello[18];
    put_id(hello, online_id);
    hello[16] = (uint8_t)(p2p_port >> 8);
    hello[17] = (uint8_t)p2p_port;
    if (send_msg(c, NPSIG_HELLO, hello, sizeof(hello)) < 0) { npsig_close(c); return NULL; }

    uint64_t end = now_ms() + (uint64_t)timeout_ms;
    for (;;) {
        size_t n = frame_len(c);
        if (n) {
            npsig_msg m;
            parse(c->rx, n, &m);
            drop_front(c, n);
            if (m.type == NPSIG_WELCOME) {
                if (out_ip) memcpy(out_ip, m.ip, 4);
                return c;
            }
            if (m.type == NPSIG_REFUSED && refused) *refused = m.found ? m.found : -1;
            npsig_close(c);
            return NULL;
        }
        uint64_t now = now_ms();
        if (now >= end || !wait_for(s, POLLIN, (int)(end - now)) || fill(c) < 0) {
            npsig_close(c);
            return NULL;
        }
    }
}

int npsig_send_peer(npsig_client* c, uint8_t type, const char* peer_online_id)
{
    uint8_t id[16];
    put_id(id, peer_online_id);
    return send_msg(c, type, id, sizeof(id));
}

int npsig_poll(npsig_client* c, npsig_msg* out)
{
    if (!c) return -1;
    size_t n = frame_len(c);
    if (!n) {
        if (c->dead || fill(c) < 0) return -1;
        n = frame_len(c);
    }
    if (!n) {
        if (now_ms() - c->last_send >= PING_MS && send_msg(c, NPSIG_PING, NULL, 0) < 0) return -1;
        return c->dead ? -1 : 0;
    }
    parse(c->rx, n, out);
    drop_front(c, n);
    return 1;
}

void npsig_close(npsig_client* c)
{
    if (!c) return;
    if (c->s != BAD_SOCK) sock_close(c->s);
    free(c);
}
