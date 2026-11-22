#!/usr/bin/env python3
"""A signaling server for sceNpSignaling (libs/network/sceNpSignaling.c; the protocol is in
libs/network/np_signal_client.h).

PSN told a console where a peer named by its NP ID could be reached, and told the peer that someone was
connecting. Titles that find their players through servers of their own (God of War: Ascension's SAM) then
activate a signaling connection to each one; this server does PSN's part. Every player says who it is and which
P2P port it takes peer traffic on; activating a connection to a peer answers with the peer's address and tells the
peer.

Usage:
    python3 tools/np_signaling_server.py [--host 0.0.0.0] [--port 36101] [--log FILE]

Each runtime instance: PS3_NP_SIGNALING_SERVER=<this host>[:36101], and a P2P port of its own
(PS3_NET_P2P_PORT) when two run on one machine. A player's address is the one this server sees it at; one on this
server's own machine, seen as 127.0.0.1, is given to players elsewhere as the address they reached the server at.
"""
import argparse
import asyncio
import ipaddress
import struct
import sys
import threading
import time

HELLO, PING, ACTIVATE, DEACTIVATE, LOOKUP = 0x01, 0x02, 0x03, 0x04, 0x05
WELCOME, REFUSED, PEER, ACTIVATED, DEACTIVATED, GONE = 0x81, 0x8F, 0x83, 0xA1, 0xA2, 0xA3
REFUSED_NAME_TAKEN = 1
DEFAULT_PORT = 36101


def msg(kind, payload=b""):
    return struct.pack(">HB", len(payload) + 1, kind) + payload


def oid(name):
    return name.encode("ascii", "replace")[:16].ljust(16, b"\0")


def name_of(raw):
    return raw[:16].split(b"\0")[0].decode("ascii", "replace")


class Player:
    def __init__(self, srv, reader, writer):
        self.srv, self.r, self.w = srv, reader, writer
        peer = writer.get_extra_info("peername") or ("0.0.0.0", 0)
        local = writer.get_extra_info("sockname") or ("0.0.0.0", 0)
        self.ip, self.server_ip = peer[0], local[0]
        self.name = None
        self.port = 0
        self.activated = set()   # players this one activated a connection to

    def send(self, kind, payload=b""):
        try:
            self.w.write(msg(kind, payload))
        except Exception:
            pass

    def address_for(self, viewer):
        """This player's address as viewer should use it."""
        try:
            if ipaddress.ip_address(self.ip).is_loopback and not ipaddress.ip_address(viewer.ip).is_loopback:
                return viewer.server_ip
        except ValueError:
            pass
        return self.ip

    def endpoint_for(self, viewer):
        return bytes(int(x) for x in self.address_for(viewer).split(".")) + struct.pack(">H", self.port)

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
            srv.players[name.lower()] = self
            srv.log("%s from %s, P2P port %d" % (name, self.ip, port))
            self.send(WELCOME, bytes(int(x) for x in self.ip.split(".")))
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
                srv.log("%s activates %s (%s:%d)" % (self.name, other.name, other.address_for(self), other.port))
                self.activated.add(other)
                other.send(ACTIVATED, oid(self.name) + self.endpoint_for(other))
            return True
        return True


class Server:
    def __init__(self, log):
        self.log = log
        self.players = {}

    def left(self, p):
        if p.name is None or self.players.get(p.name.lower()) is not p:
            return
        del self.players[p.name.lower()]
        self.log("%s left" % p.name)
        for other in list(self.players.values()):
            if other in p.activated or p in other.activated:
                other.activated.discard(p)
                other.send(GONE, oid(p.name))
        p.activated.clear()


class Running:
    """The server on an event loop of its own (a thread); port the one it took."""

    def __init__(self, host, port, log):
        self.loop = asyncio.new_event_loop()
        self.srv = Server(log)
        self.port = None
        self.error = None
        ready = threading.Event()

        async def start():
            self.server = await asyncio.start_server(lambda r, w: Player(self.srv, r, w).run(), host, port)
            self.port = self.server.sockets[0].getsockname()[1]

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


def start(host="127.0.0.1", port=DEFAULT_PORT, log=print):
    return Running(host, port, log)


def main(argv):
    ap = argparse.ArgumentParser(description="Signaling server for sceNpSignaling (ps3recomp).")
    ap.add_argument("--host", default="0.0.0.0")
    ap.add_argument("--port", type=int, default=DEFAULT_PORT)
    ap.add_argument("--log", help="also write the log here")
    a = ap.parse_args(argv)
    out = open(a.log, "a", encoding="utf-8") if a.log else None

    def log(text):
        line = time.strftime("%H:%M:%S ") + text
        print(line, flush=True)
        if out:
            out.write(line + "\n")
            out.flush()

    srv = start(a.host, a.port, log)
    log("signaling server on %s:%d" % (a.host, srv.port))
    try:
        while True:
            time.sleep(1)
    except KeyboardInterrupt:
        srv.stop()
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
