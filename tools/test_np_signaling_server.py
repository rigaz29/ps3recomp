#!/usr/bin/env python3
"""tools/np_signaling_server.py, the signaling server for sceNpSignaling.

Players say HELLO; activating a connection answers with the peer's address and P2P port and tells the peer; a
peer that is not there yet is "not found"; deactivating and leaving tell the other side; a name that is taken is
refused. Over UDP: a probe from the P2P socket learns how it looks from outside (and moves the player's address for
its peers), and datagrams are relayed between players linked by an activation.

Usage:
    python3 tools/test_np_signaling_server.py
"""
import asyncio
import os
import socket
import struct
import sys
import time
import unittest
from unittest import mock

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import np_signaling_server as srvmod  # noqa: E402


class Client:
    def __init__(self, port, name, p2p, src="127.0.0.1", local_ip=None):
        self.name = name
        self.s = socket.create_connection(("127.0.0.1", port), timeout=5, source_address=(src, 0))
        self.buf = b""
        hello = srvmod.oid(name) + struct.pack(">H", p2p)
        if local_ip:
            hello += srvmod.ip4(local_ip)
        self.send(srvmod.HELLO, hello)

    def send(self, kind, payload=b""):
        self.s.sendall(srvmod.msg(kind, payload))

    def recv(self, timeout=5):
        end = time.time() + timeout
        while True:
            if len(self.buf) >= 2:
                n = struct.unpack(">H", self.buf[:2])[0]
                if len(self.buf) >= 2 + n:
                    body, self.buf = self.buf[2:2 + n], self.buf[2 + n:]
                    return body[0], body[1:]
            self.s.settimeout(max(0.05, end - time.time()))
            data = self.s.recv(4096)
            if not data:
                raise EOFError
            self.buf += data

    def close(self):
        self.s.close()


def peer(p):
    name = p[:16].split(b"\0")[0].decode()
    return name, p[16], ".".join(str(b) for b in p[17:21]), struct.unpack(">H", p[21:23])[0]


def endpoint(p):
    """ACTIVATED and MOVED: online id, address, port."""
    return p[:16].split(b"\0")[0].decode(), ".".join(str(b) for b in p[16:20]), struct.unpack(">H", p[20:22])[0]


def refusing_udp(first_only, tried):
    """create_datagram_endpoint that refuses a socket on 127.0.0.1 as Windows does a port it keeps for something else
    (WinError 10013): the first one asked for, or every one. tried: the ports asked for there."""
    real = asyncio.BaseEventLoop.create_datagram_endpoint

    async def endpoint(loop, *a, **k):
        addr = k.get("local_addr")
        if addr and addr[0] == "127.0.0.1":
            tried.append(addr[1])
            if not first_only or len(tried) == 1:
                raise PermissionError(13, "An attempt was made to access a socket in a way forbidden by its access "
                                          "permissions")
        return await real(loop, *a, **k)
    return mock.patch.object(asyncio.BaseEventLoop, "create_datagram_endpoint", endpoint)


class Ports(unittest.TestCase):
    def test_another_number_when_udp_cannot_have_the_one_tcp_got(self):
        tried = []
        with refusing_udp(True, tried):
            srv = srvmod.start(port=0, log=lambda t: None)
        self.addCleanup(srv.stop)
        self.assertEqual(len(tried), 2)
        self.assertEqual((tried[1], srv.srv.udp_port), (srv.port, srv.port))
        c = Client(srv.port, "PlayerA", 3658)
        self.addCleanup(c.close)
        self.assertEqual(c.recv()[0], srvmod.WELCOME)

    def test_a_port_asked_for_stays(self):
        s = socket.socket()
        s.bind(("127.0.0.1", 0))
        port = s.getsockname()[1]
        s.close()
        tried = []
        with refusing_udp(False, tried), self.assertRaises(PermissionError):
            srvmod.start(port=port, log=lambda t: None)
        self.assertEqual(tried, [port])


class Signaling(unittest.TestCase):
    def setUp(self):
        self.lines = []
        self.srv = srvmod.start(port=0, log=self.lines.append)
        self.addCleanup(self.srv.stop)

    def client(self, name, p2p, src="127.0.0.1", local_ip=None):
        try:
            c = Client(self.srv.port, name, p2p, src, local_ip)
        except OSError:
            self.skipTest("no %s here (macOS has only 127.0.0.1 on lo0)" % src)
        self.addCleanup(c.close)
        kind, p = c.recv()
        self.assertEqual(kind, srvmod.WELCOME)
        self.assertEqual(p[:4], srvmod.ip4(src))
        c.token, udp_port, flags = struct.unpack(">IHB", p[4:11])
        self.assertEqual((udp_port, flags), (self.srv.port, srvmod.WELCOME_RELAY))
        return c

    def udp(self, src="127.0.0.1"):
        u = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        u.bind((src, 0))
        u.settimeout(3)
        self.addCleanup(u.close)
        return u

    def probe(self, u, c, token=None):
        u.sendto(srvmod.UDP_MAGIC + bytes([srvmod.U_PROBE]) + srvmod.oid(c.name) +
                 struct.pack(">I", c.token if token is None else token), ("127.0.0.1", self.srv.port))

    def test_activate_tells_both_sides(self):
        a, b = self.client("PlayerA", 3658), self.client("PlayerB", 3659)
        a.send(srvmod.ACTIVATE, srvmod.oid("PlayerB"))
        kind, p = a.recv()
        self.assertEqual(kind, srvmod.PEER)
        self.assertEqual(peer(p), ("PlayerB", 1, "127.0.0.1", 3659))
        kind, p = b.recv()
        self.assertEqual(kind, srvmod.ACTIVATED)
        self.assertEqual(endpoint(p), ("PlayerA", "127.0.0.1", 3658))
        # the other way round: A hears of it too (the mutual activation)
        b.send(srvmod.ACTIVATE, srvmod.oid("PlayerA"))
        self.assertEqual(peer(b.recv()[1]), ("PlayerA", 1, "127.0.0.1", 3658))
        kind, p = a.recv()
        self.assertEqual((kind, endpoint(p)), (srvmod.ACTIVATED, ("PlayerB", "127.0.0.1", 3659)))

    def test_activating_again_tells_the_peer_once(self):
        a, b = self.client("PlayerA", 3658), self.client("PlayerB", 3659)
        for _ in range(2):
            a.send(srvmod.ACTIVATE, srvmod.oid("PlayerB"))
            self.assertEqual(a.recv()[0], srvmod.PEER)
        self.assertEqual(b.recv()[0], srvmod.ACTIVATED)
        with self.assertRaises((socket.timeout, TimeoutError)):
            b.recv(timeout=0.3)

    def test_a_peer_not_there_yet(self):
        a = self.client("PlayerA", 3658)
        a.send(srvmod.ACTIVATE, srvmod.oid("PlayerB"))
        self.assertEqual(peer(a.recv()[1])[:2], ("PlayerB", 0))
        b = self.client("PlayerB", 3659)
        a.send(srvmod.ACTIVATE, srvmod.oid("PlayerB"))           # the runtime asks again
        self.assertEqual(peer(a.recv()[1]), ("PlayerB", 1, "127.0.0.1", 3659))
        self.assertEqual(b.recv()[0], srvmod.ACTIVATED)

    def test_lookup_tells_nobody(self):
        a, b = self.client("PlayerA", 3658), self.client("PlayerB", 3659)
        a.send(srvmod.LOOKUP, srvmod.oid("PlayerB"))
        self.assertEqual(peer(a.recv()[1])[1], 1)
        with self.assertRaises((socket.timeout, TimeoutError)):
            b.recv(timeout=0.3)

    def test_deactivate_and_leave(self):
        a, b = self.client("PlayerA", 3658), self.client("PlayerB", 3659)
        a.send(srvmod.ACTIVATE, srvmod.oid("PlayerB"))
        a.recv()
        b.recv()
        a.send(srvmod.DEACTIVATE, srvmod.oid("PlayerB"))
        kind, p = b.recv()
        self.assertEqual((kind, p[:7]), (srvmod.DEACTIVATED, b"PlayerA"))
        b.send(srvmod.ACTIVATE, srvmod.oid("PlayerA"))
        b.recv()
        a.recv()
        b.close()
        kind, p = a.recv()
        self.assertEqual((kind, p[:7]), (srvmod.GONE, b"PlayerB"))

    def test_four_players_every_pair(self):
        names = ("PlayerA", "PlayerB", "PlayerC", "PlayerD")
        cs = {n: self.client(n, 3658 + i) for i, n in enumerate(names)}
        for n, c in cs.items():                   # the game activates every other player in its world
            for m in names:
                if m != n:
                    c.send(srvmod.ACTIVATE, srvmod.oid(m))
        for n, c in cs.items():
            heard = {}
            for _ in range(2 * (len(names) - 1)):     # a PEER for each activation sent, an ACTIVATED for each received
                kind, p = c.recv()
                heard.setdefault(kind, set()).add(peer(p)[0] if kind == srvmod.PEER else endpoint(p)[0])
            others = set(names) - {n}
            self.assertEqual(heard, {srvmod.PEER: others, srvmod.ACTIVATED: others})
        cs["PlayerB"].close()
        for n in ("PlayerA", "PlayerC", "PlayerD"):
            kind, p = cs[n].recv()
            self.assertEqual((kind, p[:7]), (srvmod.GONE, b"PlayerB"))

    def test_names_are_one_per_player(self):
        self.client("PlayerA", 3658)
        try:
            c = Client(self.srv.port, "PlayerA", 3700, src="127.0.0.2")   # another machine, as the server sees it
        except OSError:
            self.skipTest("no 127.0.0.2 here (macOS has only 127.0.0.1 on lo0)")
        self.addCleanup(c.close)
        kind, p = c.recv()
        self.assertEqual((kind, p[0]), (srvmod.REFUSED, srvmod.REFUSED_NAME_TAKEN))

    def test_the_same_player_again_replaces_the_old_link(self):
        self.client("PlayerA", 3658)
        again = self.client("PlayerA", 3660)
        b = self.client("PlayerB", 3659)
        b.send(srvmod.ACTIVATE, srvmod.oid("PlayerA"))
        self.assertEqual(peer(b.recv()[1])[3], 3660)
        self.assertEqual(again.recv()[0], srvmod.ACTIVATED)

    def test_a_probe_tells_the_p2p_socket_how_it_looks(self):
        a = self.client("PlayerA", 3658)
        u = self.udp()
        self.probe(u, a)
        data, _ = u.recvfrom(64)
        self.assertEqual(data[:5], srvmod.UDP_MAGIC + bytes([srvmod.U_PROBE_REPLY]))
        self.assertEqual((data[5:9], struct.unpack(">H", data[9:11])[0]), (srvmod.ip4("127.0.0.1"), u.getsockname()[1]))

    def test_a_probe_without_the_token_is_ignored(self):
        a = self.client("PlayerA", 3658)
        u = self.udp()
        self.probe(u, a, token=a.token ^ 1)
        with self.assertRaises((socket.timeout, TimeoutError)):
            u.recvfrom(64)

    def test_a_probe_from_elsewhere_moves_the_players_address(self):
        a = self.client("PlayerA", 3658)
        b = self.client("PlayerB", 3659, src="127.0.0.2")
        a.send(srvmod.ACTIVATE, srvmod.oid("PlayerB"))
        self.assertEqual(peer(a.recv()[1]), ("PlayerB", 1, "127.0.0.2", 3659))    # before any probe: the TCP address
        b.recv()
        ub = self.udp("127.0.0.2")
        self.probe(ub, b)
        ub.recvfrom(64)
        kind, p = a.recv()
        self.assertEqual((kind, endpoint(p)), (srvmod.MOVED, ("PlayerB", "127.0.0.2", ub.getsockname()[1])))

    def test_the_relay_between_linked_players(self):
        a, b = self.client("PlayerA", 3658), self.client("PlayerB", 3659)
        ua, ub = self.udp(), self.udp()
        for u, c in ((ua, a), (ub, b)):
            self.probe(u, c)
            u.recvfrom(64)
        relay = srvmod.UDP_MAGIC + bytes([srvmod.U_RELAY]) + srvmod.oid("PlayerB") + b"game data"
        ua.sendto(relay, ("127.0.0.1", self.srv.port))                 # not linked yet: dropped
        ub.settimeout(0.3)
        with self.assertRaises((socket.timeout, TimeoutError)):
            ub.recvfrom(256)
        a.send(srvmod.ACTIVATE, srvmod.oid("PlayerB"))
        a.recv()
        b.recv()
        ua.sendto(relay, ("127.0.0.1", self.srv.port))
        ub.settimeout(3)
        data, _ = ub.recvfrom(256)
        self.assertEqual(data, srvmod.UDP_MAGIC + bytes([srvmod.U_RELAYED]) + srvmod.oid("PlayerA") + b"game data")

    def test_where_a_player_is_for_each_viewer(self):
        def player(ip, port, local=None, udp=None):
            p = srvmod.Player.__new__(srvmod.Player)
            p.srv = self.srv.srv
            p.ip, p.server_ip, p.port, p.local_ip, p.udp = ip, "192.168.1.10", port, local, udp
            return p
        home = player("11.22.33.44", 3658, local="192.168.0.20", udp=("11.22.33.44", 40001))
        same_router = player("11.22.33.44", 3659, local="192.168.0.21")
        elsewhere = player("12.34.56.78", 3658)
        self.assertEqual(home.where_for(same_router), ("192.168.0.20", 3658))      # behind one router: its own address
        self.assertEqual(home.where_for(elsewhere), ("11.22.33.44", 40001))        # elsewhere: what the probe saw
        on_server_lan = player("192.168.1.30", 3658, udp=("192.168.1.30", 3658))
        self.assertEqual(on_server_lan.where_for(elsewhere), ("192.168.1.10", 3658))   # the server's own address
        self.srv.srv.public_ip = "13.14.15.16"
        self.assertEqual(on_server_lan.where_for(elsewhere), ("13.14.15.16", 3658))   # ...or the one it was given
        self.srv.srv.public_ip = None
        local = player("127.0.0.1", 3659)
        self.assertEqual(local.where_for(player("127.0.0.1", 3658)), ("127.0.0.1", 3659))
        # A player on the server's own machine is never handed out as loopback to a player elsewhere.
        on_server = player("127.0.0.1", 3658, udp=("127.0.0.1", 3658))
        self.assertEqual(on_server.where_for(player("192.168.1.40", 3659)), ("192.168.1.10", 3658))
        self.assertEqual(on_server.where_for(elsewhere), ("192.168.1.10", 3658))
        self.srv.srv.public_ip = "13.14.15.16"
        self.assertEqual(on_server.where_for(elsewhere), ("13.14.15.16", 3658))
        self.assertEqual(on_server.where_for(player("192.168.1.40", 3659)), ("192.168.1.10", 3658))
        self.srv.srv.public_ip = None


if __name__ == "__main__":
    unittest.main()
