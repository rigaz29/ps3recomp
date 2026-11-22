/*
 * ps3recomp - sceNpSignaling HLE
 *
 * Peer-to-peer connections between players named by their NP IDs. A title
 * creates a context, activates a connection to each peer, and learns from
 * its handlers when a connection is established (or dead) and when a peer
 * activates one to it; the peer's address and P2P port then come from
 * GetConnectionStatus/GetConnectionInfo, and the title's own traffic goes
 * over its P2P sockets.
 *
 * PSN found the peer for the title. Here a signaling server does
 * (PS3_NP_SIGNALING_SERVER=host[:port], tools/np_signaling_server.py; see
 * np_signal_client.h for its protocol). Without one, a connection cannot be
 * activated (SCE_NP_SIGNALING_ERROR_PEER_UNREACHABLE).
 *
 * Every pointer argument is a guest address.
 */

#ifndef PS3RECOMP_SCE_NP_SIGNALING_H
#define PS3RECOMP_SCE_NP_SIGNALING_H

#include "ps3emu/ps3types.h"
#include "ps3emu/error_codes.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Error codes (the SDK's) */
#define SCE_NP_SIGNALING_ERROR_NOT_INITIALIZED        0x80022A01
#define SCE_NP_SIGNALING_ERROR_ALREADY_INITIALIZED    0x80022A02
#define SCE_NP_SIGNALING_ERROR_OUT_OF_MEMORY          0x80022A03
#define SCE_NP_SIGNALING_ERROR_CTXID_NOT_AVAILABLE    0x80022A04
#define SCE_NP_SIGNALING_ERROR_CTX_NOT_FOUND          0x80022A05
#define SCE_NP_SIGNALING_ERROR_REQID_NOT_AVAILABLE    0x80022A06
#define SCE_NP_SIGNALING_ERROR_REQ_NOT_FOUND          0x80022A07
#define SCE_NP_SIGNALING_ERROR_PARSER_CREATE_FAILED   0x80022A08
#define SCE_NP_SIGNALING_ERROR_PARSER_FAILED          0x80022A09
#define SCE_NP_SIGNALING_ERROR_INVALID_NAMESPACE      0x80022A0A
#define SCE_NP_SIGNALING_ERROR_NETINFO_NOT_AVAILABLE  0x80022A0B
#define SCE_NP_SIGNALING_ERROR_PEER_NOT_RESPONDING    0x80022A0C
#define SCE_NP_SIGNALING_ERROR_CONNID_NOT_AVAILABLE   0x80022A0D
#define SCE_NP_SIGNALING_ERROR_CONN_NOT_FOUND         0x80022A0E
#define SCE_NP_SIGNALING_ERROR_PEER_UNREACHABLE       0x80022A0F
#define SCE_NP_SIGNALING_ERROR_TERMINATED_BY_PEER     0x80022A10
#define SCE_NP_SIGNALING_ERROR_TIMEOUT                0x80022A11
#define SCE_NP_SIGNALING_ERROR_CTX_MAX                0x80022A12
#define SCE_NP_SIGNALING_ERROR_RESULT_NOT_FOUND       0x80022A13
#define SCE_NP_SIGNALING_ERROR_CONN_IN_PROGRESS       0x80022A14
#define SCE_NP_SIGNALING_ERROR_INVALID_ARGUMENT       0x80022A15
#define SCE_NP_SIGNALING_ERROR_OWN_NP_ID              0x80022A16
#define SCE_NP_SIGNALING_ERROR_TOO_MANY_CONN          0x80022A17
#define SCE_NP_SIGNALING_ERROR_TIMERID_NOT_AVAILABLE  0x80022A18

/* Limits */
#define SCE_NP_SIGNALING_CTX_MAX           8
#define SCE_NP_SIGNALING_CONN_MAX          64     /* ours: connections per process */

/* Connection status */
#define SCE_NP_SIGNALING_CONN_STATUS_INACTIVE    0
#define SCE_NP_SIGNALING_CONN_STATUS_PENDING     1
#define SCE_NP_SIGNALING_CONN_STATUS_ACTIVE      2

/* Handler events; the extended handler also gets the first four */
#define SCE_NP_SIGNALING_EVENT_DEAD                  0
#define SCE_NP_SIGNALING_EVENT_ESTABLISHED           1
#define SCE_NP_SIGNALING_EVENT_NETINFO_ERROR         2
#define SCE_NP_SIGNALING_EVENT_NETINFO_RESULT        3
#define SCE_NP_SIGNALING_EVENT_EXT_PEER_ACTIVATED    10
#define SCE_NP_SIGNALING_EVENT_EXT_PEER_DEACTIVATED  11
#define SCE_NP_SIGNALING_EVENT_EXT_MUTUAL_ACTIVATED  12

/* GetConnectionInfo codes */
#define SCE_NP_SIGNALING_CONN_INFO_RTT             1
#define SCE_NP_SIGNALING_CONN_INFO_BANDWIDTH       2
#define SCE_NP_SIGNALING_CONN_INFO_PEER_NPID       3
#define SCE_NP_SIGNALING_CONN_INFO_PEER_ADDRESS    4
#define SCE_NP_SIGNALING_CONN_INFO_MAPPED_ADDRESS  5
#define SCE_NP_SIGNALING_CONN_INFO_PACKET_LOSS     6

/* Context options */
#define SCE_NP_SIGNALING_CTX_OPT_BANDWIDTH_PROBE          1
#define SCE_NP_SIGNALING_CTX_OPT_BANDWIDTH_PROBE_DISABLE  0
#define SCE_NP_SIGNALING_CTX_OPT_BANDWIDTH_PROBE_ENABLE   1

/* SceNpSignalingNetInfo: u32 size, u32 local_addr, u32 mapped_addr,
 * s32 nat_status, s32 upnp_status, s32 npport_status, u16 npport (28 bytes,
 * big-endian in guest memory) */
#define SCE_NP_SIGNALING_NETINFO_SIZE                 28
#define SCE_NP_SIGNALING_NETINFO_NAT_STATUS_TYPE2     2
#define SCE_NP_SIGNALING_NETINFO_UPNP_STATUS_VALID    2
#define SCE_NP_SIGNALING_NETINFO_NPPORT_STATUS_OPEN   1

/* SceNpId: the online id is its first 16 bytes; 36 bytes in all */
#define SCE_NP_SIGNALING_NPID_SIZE  36

typedef u32 SceNpSignalingCtxId;
typedef u32 SceNpSignalingConnId;

/* The handlers, guest functions:
 * void handler(u32 ctx_id, u32 conn_id, s32 event, s32 error_code, void* arg) */

s32 sceNpSignalingInit(void);
s32 sceNpSignalingTerm(void);

s32 sceNpSignalingCreateCtx(u32 npId, u32 handler, u32 arg, u32 ctxId);
s32 sceNpSignalingDestroyCtx(u32 ctxId);
s32 sceNpSignalingAddExtendedHandler(u32 ctxId, u32 handler, u32 arg);
s32 sceNpSignalingSetCtxOpt(u32 ctxId, s32 optname, s32 optval);
s32 sceNpSignalingGetCtxOpt(u32 ctxId, s32 optname, u32 optval);

s32 sceNpSignalingActivateConnection(u32 ctxId, u32 npId, u32 connId);
s32 sceNpSignalingDeactivateConnection(u32 ctxId, u32 connId);
s32 sceNpSignalingTerminateConnection(u32 ctxId, u32 connId);

s32 sceNpSignalingGetConnectionStatus(u32 ctxId, u32 connId, u32 connStatus, u32 peerAddr, u32 peerPort);
s32 sceNpSignalingGetConnectionInfo(u32 ctxId, u32 connId, s32 code, u32 info);
s32 sceNpSignalingGetConnectionFromNpId(u32 ctxId, u32 npId, u32 connId);
s32 sceNpSignalingGetConnectionFromPeerAddress(u32 ctxId, u32 peerAddr, u32 peerPort, u32 connId);

s32 sceNpSignalingGetLocalNetInfo(u32 ctxId, u32 info);
s32 sceNpSignalingGetPeerNetInfo(u32 ctxId, u32 npId, u32 reqId);
s32 sceNpSignalingCancelPeerNetInfo(u32 ctxId, u32 reqId);
s32 sceNpSignalingGetPeerNetInfoResult(u32 ctxId, u32 reqId, u32 info);

/* From cellSysutilCheckCallback (through np_psnr's ticks): read the
 * signaling server, time connections out, queue handler calls. */
void np_signaling_pump(void);

#ifdef __cplusplus
}
#endif

#endif /* PS3RECOMP_SCE_NP_SIGNALING_H */
