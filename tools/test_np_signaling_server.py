#!/usr/bin/env python3
"""tools/np_signaling_server.py, the signaling server for sceNpSignaling.

Players say HELLO; activating a connection answers with the peer's address and P2P port and tells the peer; a
peer that is not there yet is "not found"; deactivating and leaving tell the other side; a name that is taken is
refused.

Usage:
    python3 tools/test_np_signaling_server.py
"""
import os
import socket
import struct
import sys
import time
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import np_signaling_server as srvmod  # noqa: E402


class Client:
    def __init__(self, port, name, p2p, src="127.0.0.1"):
        self.s = socket.create_connection(("127.0.0.1", port), timeout=5, source_address=(src, 0))
        self.buf = b""
        self.send(srvmod.HELLO, srvmod.oid(name) + struct.pack(">H", p2p))

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


def activated(p):
    return p[:16].split(b"\0")[0].decode(), ".".join(str(b) for b in p[16:20]), struct.unpack(">H", p[20:22])[0]


class Signaling(unittest.TestCase):
    def setUp(self):
        self.lines = []
        self.srv = srvmod.start(port=0, log=self.lines.append)
        self.addCleanup(self.srv.stop)

    def client(self, name, p2p):
        c = Client(self.srv.port, name, p2p)
        self.addCleanup(c.close)
        kind, p = c.recv()
        self.assertEqual(kind, srvmod.WELCOME)
        self.assertEqual(p, bytes([127, 0, 0, 1]))
        return c

    def test_activate_tells_both_sides(self):
        a, b = self.client("PlayerA", 3658), self.client("PlayerB", 3659)
        a.send(srvmod.ACTIVATE, srvmod.oid("PlayerB"))
        kind, p = a.recv()
        self.assertEqual(kind, srvmod.PEER)
        self.assertEqual(peer(p), ("PlayerB", 1, "127.0.0.1", 3659))
        kind, p = b.recv()
        self.assertEqual(kind, srvmod.ACTIVATED)
        self.assertEqual(activated(p), ("PlayerA", "127.0.0.1", 3658))
        # the other way round: A hears of it too (the mutual activation)
        b.send(srvmod.ACTIVATE, srvmod.oid("PlayerA"))
        self.assertEqual(peer(b.recv()[1]), ("PlayerA", 1, "127.0.0.1", 3658))
        kind, p = a.recv()
        self.assertEqual((kind, activated(p)), (srvmod.ACTIVATED, ("PlayerB", "127.0.0.1", 3659)))

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

    def test_a_player_on_the_servers_machine_as_seen_from_elsewhere(self):
        p = srvmod.Player.__new__(srvmod.Player)
        p.ip, p.port = "127.0.0.1", 3658
        viewer = srvmod.Player.__new__(srvmod.Player)
        viewer.ip, viewer.server_ip = "192.168.1.20", "192.168.1.10"
        self.assertEqual(p.address_for(viewer), "192.168.1.10")
        viewer.ip = "127.0.0.1"
        self.assertEqual(p.address_for(viewer), "127.0.0.1")


if __name__ == "__main__":
    unittest.main()
