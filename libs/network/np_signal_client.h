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
 *     0x01 HELLO        online id, u16 P2P port
 *     0x02 PING         (no fields; now and then, so a dead link shows)
 *     0x03 ACTIVATE     peer online id      -> PEER; the peer gets ACTIVATED
 *     0x04 DEACTIVATE   peer online id      -> the peer gets DEACTIVATED
 *     0x05 LOOKUP       peer online id      -> PEER, nobody told
 *   from the server
 *     0x81 WELCOME      u8[4] the address the server sees this player at
 *     0x8F REFUSED      u8 reason (1: another player has that online id)
 *     0x83 PEER         peer online id, u8 found, u8[4] address, u16 P2P port
 *     0xA1 ACTIVATED    peer online id, u8[4] address, u16 P2P port
 *     0xA2 DEACTIVATED  peer online id
 *     0xA3 GONE         peer online id (it left; sent to players it had
 *                       activated with)
 * A peer's address is the one the server sees it at: two players on one
 * machine are both 127.0.0.1, told apart by their P2P ports.
 *
 * No threads: npsig_poll never blocks. Not thread-safe; the caller locks.
 */
#ifndef PS3RECOMP_NP_SIGNAL_CLIENT_H
#define PS3RECOMP_NP_SIGNAL_CLIENT_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
    NPSIG_HELLO = 0x01, NPSIG_PING = 0x02, NPSIG_ACTIVATE = 0x03, NPSIG_DEACTIVATE = 0x04, NPSIG_LOOKUP = 0x05,
    NPSIG_WELCOME = 0x81, NPSIG_REFUSED = 0x8F, NPSIG_PEER = 0x83,
    NPSIG_ACTIVATED = 0xA1, NPSIG_DEACTIVATED = 0xA2, NPSIG_GONE = 0xA3
};
#define NPSIG_DEFAULT_PORT 36101
#define NPSIG_REFUSED_NAME_TAKEN 1

typedef struct npsig_client npsig_client;

typedef struct {
    uint8_t  type;
    char     peer[17];     /* online id, NUL-terminated */
    uint8_t  found;        /* PEER */
    uint8_t  ip[4];        /* PEER, ACTIVATED */
    uint16_t port;         /* PEER, ACTIVATED: the peer's P2P port */
} npsig_msg;

/* Connect to host:port and say HELLO; waits up to timeout_ms for WELCOME.
 * out_ip: the address the server sees us at. NULL on failure; *refused is
 * then the REFUSED reason, or 0 if the server was not reached. */
npsig_client* npsig_connect(const char* host, uint16_t port, const char* online_id, uint16_t p2p_port,
                            int timeout_ms, uint8_t out_ip[4], int* refused);

/* 0 if sent (queued), -1 if the connection is gone. */
int npsig_send_peer(npsig_client* c, uint8_t type, const char* peer_online_id);

/* 1: *out holds the next message. 0: nothing yet. -1: the connection is gone.
 * Also sends the PING when one is due. */
int npsig_poll(npsig_client* c, npsig_msg* out);

void npsig_close(npsig_client* c);

#ifdef __cplusplus
}
#endif

#endif /* PS3RECOMP_NP_SIGNAL_CLIENT_H */
