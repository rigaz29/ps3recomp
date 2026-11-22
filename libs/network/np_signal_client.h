/*
 * np_signal_client - the runtime's side of a signaling server
 * (tools/np_signaling_server.py), for sceNpSignaling.
 *
 * PSN told a player's console where a peer named by its NP ID could be
 * reached, and told the peer that someone was connecting. Titles that find
 * their players through servers of their own (God of War: Ascension's SAM)
 * then activate a signaling connection to each one. The signaling server
 * does PSN's part: every player says who it is and which P2P port it takes
 * peer traffic on; activating a connection to a peer returns the peer's
 * address and tells the peer.
 *
 * TCP. Each message is u16 length (of what follows), u8 type, then the
 * fields, big-endian. An online id is 16 bytes, NUL-padded.
 *   to the server
 *     0x01 HELLO        online id, u16 P2P port, u8[4] this machine's address
 *                       toward the server (for peers behind the same router)
 *     0x02 PING         (no fields; now and then, so a dead link shows)
 *     0x03 ACTIVATE     peer online id      -> PEER; the peer gets ACTIVATED
 *     0x04 DEACTIVATE   peer online id      -> the peer gets DEACTIVATED
 *     0x05 LOOKUP       peer online id      -> PEER, nobody told
 *   from the server
 *     0x81 WELCOME      u8[4] the address the server sees this player at,
 *                       u32 token (for UDP probes), u16 the server's UDP port,
 *                       u8 flags (1: it relays)
 *     0x8F REFUSED      u8 reason (1: another player has that online id)
 *     0x83 PEER         peer online id, u8 found, u8[4] address, u16 P2P port
 *     0xA1 ACTIVATED    peer online id, u8[4] address, u16 P2P port
 *     0xA2 DEACTIVATED  peer online id
 *     0xA3 GONE         peer online id (it left; sent to players it had
 *                       activated with)
 *     0xA4 MOVED        peer online id, u8[4] address, u16 P2P port (its
 *                       socket looks different from outside than first told)
 * A peer's address is the one its P2P socket has as seen from where we are:
 * two players on one machine are both 127.0.0.1 (told apart by their P2P
 * ports), two behind one router their own network's addresses, others what
 * a probe from their P2P socket showed the server.
 *
 * UDP, on the P2P socket (each datagram starts with "NPSG" and a type;
 * sceNpSignaling keeps them from the title):
 *     0x01 PROBE        online id, u32 token     -> the server's UDP port
 *     0x02 PROBE_REPLY  u8[4] address, u16 port  <- how the socket looks
 *     0x03 PUNCH        online id                -> a peer, now and then
 *     0x04 RELAY        peer online id, payload  -> the server, for the peer
 *     0x05 RELAYED      peer online id, payload  <- from the peer, by the server
 *     0x06 PUNCH_ACK    online id                -> a peer that punched us
 * A peer whose PUNCH_ACK comes back is reachable both ways; one that never
 * answers gets our datagrams through the relay.
 *
 * No threads: npsig_poll never blocks. Not thread-safe; the caller locks.
 */
#ifndef PS3RECOMP_NP_SIGNAL_CLIENT_H
#define PS3RECOMP_NP_SIGNAL_CLIENT_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
    NPSIG_HELLO = 0x01, NPSIG_PING = 0x02, NPSIG_ACTIVATE = 0x03, NPSIG_DEACTIVATE = 0x04, NPSIG_LOOKUP = 0x05,
    NPSIG_WELCOME = 0x81, NPSIG_REFUSED = 0x8F, NPSIG_PEER = 0x83,
    NPSIG_ACTIVATED = 0xA1, NPSIG_DEACTIVATED = 0xA2, NPSIG_GONE = 0xA3, NPSIG_MOVED = 0xA4
};
enum {
    NPSIG_U_PROBE = 0x01, NPSIG_U_PROBE_REPLY = 0x02, NPSIG_U_PUNCH = 0x03, NPSIG_U_RELAY = 0x04,
    NPSIG_U_RELAYED = 0x05, NPSIG_U_PUNCH_ACK = 0x06
};
#define NPSIG_DEFAULT_PORT 36101
#define NPSIG_REFUSED_NAME_TAKEN 1
#define NPSIG_WELCOME_RELAY 1
#define NPSIG_UDP_HEAD 21            /* "NPSG", type, online id: what RELAY adds to a payload */

typedef struct npsig_client npsig_client;

typedef struct {
    uint8_t  type;
    char     peer[17];     /* online id, NUL-terminated */
    uint8_t  found;        /* PEER; REFUSED: the reason */
    uint8_t  ip[4];        /* PEER, ACTIVATED, MOVED; WELCOME: as the server sees us */
    uint16_t port;         /* PEER, ACTIVATED, MOVED: the peer's P2P port */
} npsig_msg;

/* What WELCOME said, and where the server is. */
typedef struct {
    uint8_t  ip[4];          /* this player, as the server sees it */
    uint32_t token;          /* for PROBE */
    uint16_t udp_port;       /* the server's UDP port (0: an old server, no UDP) */
    uint8_t  flags;          /* NPSIG_WELCOME_RELAY */
    uint8_t  server_ip[4];   /* the server's address, as we reached it */
} npsig_welcome;

/* Connect to host:port and say HELLO; waits up to timeout_ms for WELCOME.
 * NULL on failure; *refused is then the REFUSED reason, or 0 if the server
 * was not reached. */
npsig_client* npsig_connect(const char* host, uint16_t port, const char* online_id, uint16_t p2p_port,
                            int timeout_ms, npsig_welcome* out, int* refused);

/* 0 if sent (queued), -1 if the connection is gone. */
int npsig_send_peer(npsig_client* c, uint8_t type, const char* peer_online_id);

/* 1: *out holds the next message. 0: nothing yet. -1: the connection is gone.
 * Also sends the PING when one is due. */
int npsig_poll(npsig_client* c, npsig_msg* out);

void npsig_close(npsig_client* c);

/* UDP datagrams. The builders return the length written (out must hold
 * NPSIG_UDP_HEAD + 4 bytes, or the payload more for RELAY), 0 if it does
 * not fit. */
size_t npsig_udp_probe(uint8_t* out, size_t cap, const char* online_id, uint32_t token);
size_t npsig_udp_punch(uint8_t* out, size_t cap, uint8_t type, const char* online_id);   /* PUNCH, PUNCH_ACK */
size_t npsig_udp_relay(uint8_t* out, size_t cap, const char* to_online_id, const void* payload, size_t len);

typedef struct {
    uint8_t        type;     /* 0: not ours */
    char           peer[17]; /* PUNCH, PUNCH_ACK, RELAYED */
    uint8_t        ip[4];    /* PROBE_REPLY */
    uint16_t       port;
    const uint8_t* payload;  /* RELAYED */
    size_t         len;
} npsig_udp;

/* Fills *out and returns 1 if buf is one of ours (type 0 if malformed). */
int npsig_udp_parse(const void* buf, size_t len, npsig_udp* out);

#ifdef __cplusplus
}
#endif

#endif /* PS3RECOMP_NP_SIGNAL_CLIENT_H */
