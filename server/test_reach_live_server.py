#!/usr/bin/env python3
"""Protocol test for reach_live_server.py: two fake clients register, broadcast and relay.

    python3 server/test_reach_live_server.py
"""

import os
import socket
import struct
import subprocess
import sys
import time
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
MAGIC = b"RLV1"
HELLO, WELCOME, ERROR, BROADCAST, RELAY, FORWARD = 1, 2, 3, 4, 5, 6
LIST, PEERS, PRESENCE, INVITE = 11, 12, 13, 14
MATCH_PUBLISH, MATCH_SEARCH, MATCH_RESULTS = 15, 16, 17


def free_port():
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.bind(("127.0.0.1", 0))
    port = s.getsockname()[1]
    s.close()
    return port


class Client:
    def __init__(self, server, name, xuid, room=b""):
        self.server = server
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.sock.bind(("127.0.0.1", 0))
        self.sock.settimeout(2)
        self.name, self.xuid, self.room = name, xuid, room

    def send(self, kind, body):
        self.sock.sendto(MAGIC + bytes([kind]) + body, self.server)

    def recv(self):
        data, _ = self.sock.recvfrom(65536)
        assert data[:4] == MAGIC
        return data[4], data[5:]

    def hello(self, token=1):
        port = self.sock.getsockname()[1]
        body = struct.pack(">HQQIH", 1, token, self.xuid, 0x7F000001, port)
        body += bytes([len(self.room)]) + self.room + bytes([len(self.name)]) + self.name
        self.send(HELLO, body)
        kind, body = self.recv()
        assert kind == WELCOME, kind
        self.id, epoch, ip, port, motd_len = struct.unpack_from(">IHIHH", body)
        return self.id


class ServerTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.port = free_port()
        cls.proc = subprocess.Popen([sys.executable, os.path.join(HERE, "reach_live_server.py"),
                                     "--host", "127.0.0.1", "--port", str(cls.port)],
                                    stderr=subprocess.DEVNULL)
        time.sleep(0.5)
        cls.addr = ("127.0.0.1", cls.port)

    @classmethod
    def tearDownClass(cls):
        cls.proc.terminate()
        cls.proc.wait()

    def test_broadcast_and_relay(self):
        a = Client(self.addr, b"Alpha", 0x0009000000000001)
        b = Client(self.addr, b"Bravo", 0x0009000000000002)
        c = Client(self.addr, b"Other", 0x0009000000000003, room=b"elsewhere")
        a_id, b_id, c_id = a.hello(), b.hello(), c.hello()
        self.assertEqual(len({a_id, b_id, c_id}), 3)

        a.send(BROADCAST, struct.pack(">HH", 1001, 1001) + b"search")
        kind, body = b.recv()
        self.assertEqual(kind, FORWARD)
        src, _, src_port, _, _, gsrc, gdst = struct.unpack_from(">IIHIHHH", body)
        self.assertEqual((src, gsrc, gdst, body[20:]), (a_id, 1001, 1001, b"search"))
        self.assertEqual(src_port, a.sock.getsockname()[1])
        with self.assertRaises(socket.timeout):
            c.sock.settimeout(0.3)
            c.recv()  # another room hears nothing

        b.send(RELAY, struct.pack(">IHH", a_id, 1000, 1000) + b"reply")
        kind, body = a.recv()
        self.assertEqual(kind, FORWARD)
        self.assertEqual(struct.unpack_from(">I", body)[0], b_id)
        self.assertEqual(body[20:], b"reply")

    def test_presence_in_roster(self):
        a = Client(self.addr, b"Carter", 0x0009000000000011, room=b"r2")
        b = Client(self.addr, b"Jun", 0x0009000000000012, room=b"r2")
        a_id, b_id = a.hello(), b.hello()
        session = bytes(range(0x3C))
        a.send(PRESENCE, struct.pack(">I", 0x13) + session + bytes([5]) + b"lobby" + b"qos!")
        time.sleep(0.2)
        b.send(LIST, b"")
        kind, body = b.recv()
        self.assertEqual(kind, PEERS)
        total, first, count = struct.unpack_from(">HHH", body)
        self.assertEqual((total, first, count), (2, 0, 2))
        pos, seen = 6, {}
        for _ in range(count):
            pid, xuid, name_len = struct.unpack_from(">IQB", body, pos)
            pos += 13
            name = body[pos:pos + name_len]
            pos += name_len
            (state,) = struct.unpack_from(">I", body, pos)
            info = body[pos + 4:pos + 4 + 0x3C]
            pos += 4 + 0x3C
            status = body[pos + 1:pos + 1 + body[pos]]
            pos += 1 + body[pos]
            (extra_len,) = struct.unpack_from(">H", body, pos)
            extra = body[pos + 2:pos + 2 + extra_len]
            pos += 2 + extra_len
            seen[pid] = (name, state, info, status, extra)
        self.assertEqual(list(seen)[0], b_id)  # the asker first
        self.assertEqual(seen[a_id], (b"Carter", 0x13, session, b"lobby", b"qos!"))

    def test_invite_reaches_the_invitee(self):
        a = Client(self.addr, b"Kat", 0x0009000000000021, room=b"r3")
        b = Client(self.addr, b"Jun", 0x0009000000000022, room=b"r3")
        a_id, _ = a.hello(), b.hello()
        session = bytes(range(0x3C))
        a.send(INVITE, struct.pack(">Q", 0x0009000000000022) + session)
        kind, body = b.recv()
        self.assertEqual(kind, INVITE)
        src, xuid, name_len = struct.unpack_from(">IQB", body)
        self.assertEqual((src, xuid, body[13:13 + name_len]), (a_id, 0x0009000000000021, b"Kat"))
        self.assertEqual(body[13 + name_len:], session)

    def test_matchmaking_search_finds_published_sessions(self):
        a = Client(self.addr, b"Emile", 0x0009000000000041, room=b"r4")
        b = Client(self.addr, b"Noble", 0x0009000000000042, room=b"r4")
        a_id, _ = a.hello(), b.hello()
        a.send(MATCH_PUBLISH, struct.pack(">BI", 1, 101) + b"record")
        time.sleep(0.2)
        b.send(MATCH_SEARCH, struct.pack(">II", 101, 7))
        kind, body = b.recv()
        self.assertEqual(kind, MATCH_RESULTS)
        request, count = struct.unpack_from(">IH", body)
        self.assertEqual((request, count), (7, 1))
        pid, xuid, size = struct.unpack_from(">IQH", body, 6)
        self.assertEqual((pid, xuid, body[20:20 + size]), (a_id, 0x0009000000000041, b"record"))
        b.send(MATCH_SEARCH, struct.pack(">II", 102, 8))  # another playlist
        self.assertEqual(struct.unpack_from(">IH", b.recv()[1]), (8, 0))
        b.send(MATCH_SEARCH, struct.pack(">II", 101 ^ (60 << 24), 10))  # the playlist at 60 ticks/s
        self.assertEqual(struct.unpack_from(">IH", b.recv()[1]), (10, 0))
        a.send(MATCH_PUBLISH, b"\0")  # withdrawn
        time.sleep(0.2)
        b.send(MATCH_SEARCH, struct.pack(">II", 101, 9))
        self.assertEqual(struct.unpack_from(">IH", b.recv()[1]), (9, 0))

    def test_newer_matchmaking_session_joins_the_older(self):
        a = Client(self.addr, b"Kat", 0x0009000000000051, room=b"r5")
        b = Client(self.addr, b"Jorge", 0x0009000000000052, room=b"r5")
        a_id, _ = a.hello(), b.hello()
        a.send(MATCH_PUBLISH, struct.pack(">BI", 1, 101) + b"A" * 8)
        time.sleep(0.1)
        b.send(MATCH_PUBLISH, struct.pack(">BI", 1, 101) + b"B" * 8)
        time.sleep(0.1)
        a.send(MATCH_PUBLISH, struct.pack(">BI", 1, 101) + b"A" * 8 + b"more")  # same session
        time.sleep(0.1)
        a.send(MATCH_SEARCH, struct.pack(">II", 101, 1))
        self.assertEqual(struct.unpack_from(">IH", a.recv()[1]), (1, 0))  # nothing older
        b.send(MATCH_SEARCH, struct.pack(">II", 101, 2))
        body = b.recv()[1]
        self.assertEqual(struct.unpack_from(">IH", body), (2, 1))
        self.assertEqual(struct.unpack_from(">I", body, 6)[0], a_id)

    def test_unregistered_sender_is_told(self):
        d = Client(self.addr, b"Delta", 0x0009000000000004)
        d.send(RELAY, struct.pack(">IHH", 1, 1000, 1000) + b"x")
        kind, body = d.recv()
        self.assertEqual((kind, body[0]), (ERROR, 1))


class RateLimitTest(unittest.TestCase):
    def test_relay_over_the_limit_is_dropped(self):
        port = free_port()
        proc = subprocess.Popen([sys.executable, os.path.join(HERE, "reach_live_server.py"),
                                 "--host", "127.0.0.1", "--port", str(port), "--rate-limit", "1000"],
                                stderr=subprocess.DEVNULL)
        try:
            time.sleep(0.5)
            addr = ("127.0.0.1", port)
            a = Client(addr, b"Flood", 0x0009000000000031)
            b = Client(addr, b"Target", 0x0009000000000032)
            a.hello()
            b_id = b.hello()
            for _ in range(40):  # 40 KB in an instant against a 4 KB burst
                a.send(RELAY, struct.pack(">IHH", b_id, 1000, 1000) + bytes(1000))
            b.sock.settimeout(0.3)
            received = 0
            try:
                while True:
                    kind, _ = b.recv()
                    received += kind == FORWARD
            except socket.timeout:
                pass
            self.assertGreater(received, 0)
            self.assertLess(received, 8)
        finally:
            proc.terminate()
            proc.wait()


if __name__ == "__main__":
    unittest.main()
