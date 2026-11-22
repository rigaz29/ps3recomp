#!/usr/bin/env python3
"""A signaling server for sceNpSignaling (libs/network/sceNpSignaling.c; the protocol is in
libs/network/np_signal_client.h).

PSN told a console where a peer named by its NP ID could be reached, and told the peer that someone was
connecting. Titles that find their players through servers of their own (God of War: Ascension's SAM) then
activate a signaling connection to each one; this server does PSN's part. Every player says who it is and which
P2P port it takes peer traffic on; activating a connection to a peer answers with the peer's address and tells the
peer.

Players on other networks: the runtime sends a probe from its P2P socket to this server's UDP port (the same number
as the TCP one), so the server learns how that socket looks from outside its router, and hands that to the player's
peers; both sides then punch toward each other. Peers that still cannot reach each other have their datagrams
relayed through this server's UDP port (unless --no-relay).

Usage:
    python3 tools/np_signaling_server.py [--host 0.0.0.0] [--port 36101] [--public-ip A.B.C.D] [--no-relay]
                                         [--log FILE]

--public-ip: the address players elsewhere reach this server at, when it runs behind a router (its own network's
players are then given that address, with their P2P ports forwarded there). A player's address otherwise is the
one this server sees it at: players on one machine get 127.0.0.1, players behind one router their own network's
addresses, players elsewhere their routers' public endpoints.
"""
import argparse
import asyncio
import ipaddress
import os
import struct
import sys
import threading
import time

HELLO, PING, ACTIVATE, DEACTIVATE, LOOKUP = 0x01, 0x02, 0x03, 0x04, 0x05
WELCOME, REFUSED, PEER, ACTIVATED, DEACTIVATED, GONE, MOVED = 0x81, 0x8F, 0x83, 0xA1, 0xA2, 0xA3, 0xA4
REFUSED_NAME_TAKEN = 1
WELCOME_RELAY = 1
UDP_MAGIC = b"NPSG"
U_PROBE, U_PROBE_REPLY, U_PUNCH, U_RELAY, U_RELAYED = 0x01, 0x02, 0x03, 0x04, 0x05
DEFAULT_PORT = 36101


def msg(kind, payload=b""):
    return struct.pack(">HB", len(payload) + 1, kind) + payload


def oid(name):
    return name.encode("ascii", "replace")[:16].ljust(16, b"\0")


def name_of(raw):
    return raw[:16].split(b"\0")[0].decode("ascii", "replace")


def ip4(text):
    return bytes(int(x) for x in text.split("."))


def local_or_private(text):
    try:
        a = ipaddress.ip_address(text)
        return a.is_loopback or a.is_private or a.is_link_local
    except ValueError:
        return False


class Player:
    def __init__(self, srv, reader, writer):
        self.srv, self.r, self.w = srv, reader, writer
        peer = writer.get_extra_info("peername") or ("0.0.0.0", 0)
        local = writer.get_extra_info("sockname") or ("0.0.0.0", 0)
        self.ip, self.server_ip = peer[0], local[0]
        self.name = None
        self.port = 0              # the P2P port it binds
        self.local_ip = None       # its own idea of its address (HELLO), for players behind the same router
        self.udp = None            # its P2P socket as this server's UDP port sees it (a probe)
        self.token = struct.unpack(">I", os.urandom(4))[0]
        self.activated = set()     # players this one activated a connection to

    def send(self, kind, payload=b""):
        try:
            self.w.write(msg(kind, payload))
        except Exception:
            pass

    # The relay's two ends, so a player of another kind (one behind another server, which carries its messages and
    # datagrams there) takes the same path: whether a relayed datagram can be brought to this player, and bringing it.
    def relay_ready(self):
        return self.udp is not None

    def relayed_from(self, sender, payload, transport):
        transport.sendto(UDP_MAGIC + bytes([U_RELAYED]) + oid(sender.name) + payload, self.udp)

    def address_for(self, viewer):
        return self.where_for(viewer)[0]

    def where_for(self, viewer):
        """(address, port) of this player's P2P socket, as viewer should use them."""
        if self.ip == viewer.ip:                     # one machine, or one router
            if ipaddress.ip_address(self.ip).is_loopback:
                return "127.0.0.1", self.port
            if self.local_ip and self.local_ip != "0.0.0.0":
                return self.local_ip, self.port
            return self.ip, self.port
        a, port = self.udp if self.udp is not None else (self.ip, self.port)
        # On this server's own machine (seen at loopback) for a viewer that is not, or on its network for a viewer
        # elsewhere: the viewer reaches it where it reaches the server (from elsewhere, at the address the server was
        # given, if any).
        on_server = ipaddress.ip_address(a).is_loopback and not ipaddress.ip_address(viewer.ip).is_loopback
        if on_server or (local_or_private(a) and not local_or_private(viewer.ip)):
            a = (None if local_or_private(viewer.ip) else self.srv.public_ip) or viewer.server_ip
        return a, port

    def endpoint_for(self, viewer):
        a, port = self.where_for(viewer)
        return ip4(a) + struct.pack(">H", port)

    async def run(self):
        try:
            while True:
                n = struct.unpack(">H", await self.r.readexactly(2))[0]
                if n < 1:
                    break
                body = await self.r.readexactly(n)
                if not self.handle(body[0], body[1:]):
                    break
                await self.w.drain()
        except (asyncio.IncompleteReadError, ConnectionError, OSError):
            pass
        finally:
            self.srv.left(self)
            try:
                self.w.close()
            except Exception:
                pass

    def handle(self, kind, p):
        srv = self.srv
        if kind == HELLO and self.name is None and len(p) >= 18:
            name, port = name_of(p[:16]), struct.unpack(">H", p[16:18])[0]
            old = srv.players.get(name.lower())
            if old is not None and old.ip != self.ip:
                srv.log("%s from %s refused: the name is taken (%s)" % (name, self.ip, old.ip))
                self.send(REFUSED, bytes([REFUSED_NAME_TAKEN]))
                return False
            if old is not None:                      # the same player again (a restart): the old link goes
                srv.log("%s again from %s; the old connection goes" % (name, self.ip))
                srv.left(old)
                try:
                    old.w.close()
                except Exception:
                    pass
            self.name, self.port = name, port
            if len(p) >= 22:
                self.local_ip = ".".join(str(b) for b in p[18:22])
            srv.players[name.lower()] = self
            srv.log("%s from %s, P2P port %d%s" % (name, self.ip, port,
                                                    ", here %s" % self.local_ip if self.local_ip else ""))
            self.send(WELCOME, ip4(self.ip) + struct.pack(">IHB", self.token, srv.udp_port,
                                                            WELCOME_RELAY if srv.relay else 0))
            return True
        if self.name is None:
            return False
        if kind == PING:
            return True
        if kind in (ACTIVATE, DEACTIVATE, LOOKUP) and len(p) >= 16:
            name = name_of(p[:16])
            other = srv.players.get(name.lower())
            if kind == DEACTIVATE:
                srv.log("%s deactivates %s" % (self.name, name))
                if other is not None and other in self.activated:
                    self.activated.discard(other)
                    other.send(DEACTIVATED, oid(self.name))
                return True
            if other is None or other is self:
                self.send(PEER, oid(name) + b"\0" + bytes(6))
                if kind == ACTIVATE:
                    srv.log("%s activates %s: not here" % (self.name, name))
                return True
            self.send(PEER, oid(other.name) + b"\1" + other.endpoint_for(self))
            if kind == ACTIVATE and other not in self.activated:
                a, port = other.where_for(self)
                srv.log("%s activates %s (%s:%d)" % (self.name, other.name, a, port))
                self.activated.add(other)
                other.send(ACTIVATED, oid(self.name) + self.endpoint_for(other))
            return True
        return True


def linked(a, b):
    return b in a.activated or a in b.activated


class Udp(asyncio.DatagramProtocol):
    """Probes (a P2P socket's outside endpoint) and the relay."""

    def __init__(self, srv):
        self.srv = srv

    def connection_made(self, transport):
        self.t = transport

    def datagram_received(self, data, addr):
        srv = self.srv
        if len(data) < 5 or data[:4] != UDP_MAGIC:
            return
        kind, ep = data[4], (addr[0], addr[1])
        if kind == U_PROBE and len(data) >= 25:
            p = srv.players.get(name_of(data[5:21]).lower())
            if p is None or p.token != struct.unpack(">I", data[21:25])[0]:
                return
            self.t.sendto(UDP_MAGIC + bytes([U_PROBE_REPLY]) + ip4(addr[0]) + struct.pack(">H", addr[1]), addr)
            if p.udp == ep:
                return
            viewers = [o for o in srv.players.values() if o is not p and linked(o, p)]
            before = {o: p.endpoint_for(o) for o in viewers}
            if p.udp is not None:
                srv.by_udp.pop(p.udp, None)
            p.udp = ep
            srv.by_udp[ep] = p
            srv.log("%s's P2P socket is %s:%d from here" % (p.name, ep[0], ep[1]))
            for o in viewers:
                now = p.endpoint_for(o)
                if now != before[o]:
                    o.send(MOVED, oid(p.name) + now)
        elif kind == U_RELAY and len(data) >= 21 and srv.relay:
            sender = srv.by_udp.get(ep)
            to = srv.players.get(name_of(data[5:21]).lower())
            if sender is None or to is None or not to.relay_ready() or not linked(sender, to):
                return
            pair = (sender.name, to.name)
            if pair not in srv.relaying:
                srv.relaying.add(pair)
                srv.log("relaying %s -> %s" % pair)
            srv.relayed += len(data) - 21
            to.relayed_from(sender, data[21:], self.t)


class Server:
    def __init__(self, log, public_ip=None, relay=True):
        self.log = log
        self.public_ip = public_ip
        self.relay = relay
        self.players = {}
        self.by_udp = {}
        self.relaying = set()
        self.relayed = 0
        self.udp_port = 0

    def left(self, p):
        if p.name is None or self.players.get(p.name.lower()) is not p:
            return
        del self.players[p.name.lower()]
        if p.udp is not None:
            self.by_udp.pop(p.udp, None)
        self.log("%s left" % p.name)
        for other in list(self.players.values()):
            if other in p.activated or p in other.activated:
                other.activated.discard(p)
                other.send(GONE, oid(p.name))
        p.activated.clear()


class Running:
    """The server on an event loop of its own (a thread); port the one it took (TCP and UDP)."""

    def __init__(self, host, port, log, public_ip=None, relay=True):
        self.loop = asyncio.new_event_loop()
        self.srv = Server(log, public_ip, relay)
        self.port = None
        self.error = None
        ready = threading.Event()

        async def start():
            # TCP and UDP on one number. With a port of the system's choosing (0) the number TCP got may be one UDP
            # cannot have (Windows keeps ranges of ports by protocol, for Hyper-V and the like): then another. A port
            # asked for is not changed: players elsewhere are told it.
            for attempt in range(1 if port else 20):
                self.server = await asyncio.start_server(lambda r, w: Player(self.srv, r, w).run(), host, port)
                self.port = self.server.sockets[0].getsockname()[1]
                try:
                    self.udp, _ = await self.loop.create_datagram_endpoint(lambda: Udp(self.srv),
                                                                           local_addr=(host, self.port))
                    break
                except OSError:
                    self.server.close()
                    await self.server.wait_closed()
                    if port or attempt == 19:
                        raise
            self.srv.udp_port = self.port

        def run():
            asyncio.set_event_loop(self.loop)
            try:
                self.loop.run_until_complete(start())
            except OSError as e:
                self.error = e
                ready.set()
                return
            ready.set()
            self.loop.run_forever()

        self.thread = threading.Thread(target=run, daemon=True)
        self.thread.start()
        ready.wait(10)
        if self.error:
            raise self.error

    def stop(self):
        async def shut():
            self.server.close()
            self.udp.close()
            for p in list(self.srv.players.values()):
                p.w.close()
            tasks = [t for t in asyncio.all_tasks() if t is not asyncio.current_task()]
            for t in tasks:
                t.cancel()
            await asyncio.gather(*tasks, return_exceptions=True)
            await asyncio.sleep(0.05)
        try:
            asyncio.run_coroutine_threadsafe(shut(), self.loop).result(5)
        finally:
            self.loop.call_soon_threadsafe(self.loop.stop)
            self.thread.join(5)
            if not self.thread.is_alive():
                self.loop.close()


def start(host="127.0.0.1", port=DEFAULT_PORT, log=print, public_ip=None, relay=True):
    return Running(host, port, log, public_ip, relay)


def main(argv):
    ap = argparse.ArgumentParser(description="Signaling server for sceNpSignaling (ps3recomp).")
    ap.add_argument("--host", default="0.0.0.0")
    ap.add_argument("--port", type=int, default=DEFAULT_PORT)
    ap.add_argument("--public-ip", help="the address players elsewhere reach this server at, behind a router")
    ap.add_argument("--no-relay", action="store_true", help="never relay datagrams between players")
    ap.add_argument("--log", help="also write the log here")
    a = ap.parse_args(argv)
    out = open(a.log, "a", encoding="utf-8") if a.log else None

    def log(text):
        line = time.strftime("%H:%M:%S ") + text
        print(line, flush=True)
        if out:
            out.write(line + "\n")
            out.flush()

    srv = start(a.host, a.port, log, a.public_ip, not a.no_relay)
    log("signaling server on %s:%d (TCP and UDP)%s" % (a.host, srv.port, "" if not a.no_relay else ", no relay"))
    try:
        while True:
            time.sleep(1)
    except KeyboardInterrupt:
        srv.stop()
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
