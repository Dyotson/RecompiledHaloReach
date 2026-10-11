#!/usr/bin/env python3
"""Tests for reach_live_lsp.py with synthetic uploads (no game data).

    python3 server/test_reach_live_lsp.py
"""

import os
import struct
import sys
import tempfile
import types
import unittest
import zlib

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import reach_live_lsp as lsp  # noqa: E402

BOUNDARY = b"--BUNGIEr0x0rz"


def multipart(data, kind="reward-sync"):
    return (BOUNDARY + b'\r\nContent-Disposition: form-data; name="upload"; filename="blob"\r\n'
            b"Content-Type: application/x-reach-" + kind.encode() + b"\r\n\r\n" + data +
            b"\r\n" + BOUNDARY + b"--\r\n")


def rpul(cookies, owned=(), gamertag=b"Noble Six", commendations=None):
    payload = bytearray(lsp.RPUL_SIZE)
    struct.pack_into(">ii", payload, 0, cookies, 1)
    for index, progress in (commendations or {}).items():
        struct.pack_into(">h", payload, 8 + 2 * index, progress)
    for item in owned:
        payload[0x108 + item] = 1
    payload[0x735:0x735 + len(gamertag)] = gamertag
    return lsp.blf(lsp.chunk(b"rpul", 3, 1, bytes(payload)),
                   lsp.chunk(b"chpr", 2, 1, bytes(88)))


class LspTest(unittest.TestCase):
    def setUp(self):
        self.dir = tempfile.TemporaryDirectory()
        lsp.configure(types.SimpleNamespace(data_dir=self.dir.name, dump_requests=False))

    def tearDown(self):
        self.dir.cleanup()

    def post(self, path, data, kind="reward-sync"):
        return lsp.handle(None, "POST", path, {}, multipart(data, kind), ("127.0.0.1", 1))

    def rpdl(self, reply):
        status, _, body = reply
        self.assertEqual(status, 200)
        found = lsp.chunks(body)
        self.assertIn(b"_eof", found)
        major, _, payload = found[b"rpdl"]
        self.assertEqual((major, len(payload) + 12), (2, 0x227))  # the game insists on 0x227
        return payload

    def test_blf_layout(self):
        body = lsp.blf(lsp.chunk(b"test", 1, 1, b"abcd"))
        self.assertEqual(body[:4], b"_blf")
        self.assertEqual(struct.unpack_from(">IHHH", body, 4), (0x30, 1, 2, 0xFFFE))
        eof = body.index(b"_eof")
        self.assertEqual(struct.unpack_from(">IHHIB", body, eof + 4), (0x11, 1, 1, eof, 0))

    def test_rewards_never_go_down(self):
        url = "/gameapi_omaha/UserUpdateRewards.ashx?&getDailyChallenges=1&userId=0009000000000042"
        first = self.rpdl(self.post(url, rpul(12000, owned=(3, 7))))
        self.assertEqual(struct.unpack_from(">i", first, 0)[0], 12000)
        # A reinstalled game uploads less: the server answers with what it knows.
        second = self.rpdl(self.post(url, rpul(5000, owned=(9,))))
        self.assertEqual(struct.unpack_from(">i", second, 0)[0], 12000)
        self.assertEqual([i for i in range(256) if second[0x108 + i] & 1], [3, 7, 9])
        record = lsp.load_player(0x0009000000000042)
        self.assertEqual(record["gamertag"], "Noble Six")
        self.assertIn("challenge_progress", record)

    def test_commendation_progress_is_kept(self):
        # Block A +8: one i16 of progress per commendation (raised by kills in a matchmade
        # game, for example).
        url = "/gameapi_omaha/UserUpdateRewards.ashx?&getDailyChallenges=1&userId=0009000000000043"
        self.rpdl(self.post(url, rpul(5753, commendations={0: 1, 7: 1, 8: 1, 13: 1})))
        reply = self.rpdl(self.post(url, rpul(5000, commendations={7: 4})))
        progress = struct.unpack_from(">128h", reply, 8)
        self.assertEqual({i: v for i, v in enumerate(progress) if v}, {0: 1, 7: 4, 8: 1, 13: 1})

    def test_presence(self):
        payload = bytearray(0x1BB)
        payload[1] = 1
        struct.pack_into(">Q", payload, 2, 0x0009000000000077)
        status, _, body = self.post("/ReachPresenceApi/heartbeat.ashx",
                                    lsp.chunk(b"phbt", 5, 1, bytes(payload)), "presence")
        self.assertEqual(status, 200)
        major, _, reply = lsp.chunks(body)[b"phbr"]
        self.assertEqual((major, len(reply) + 12), (2, 0x9F))
        self.assertIn(0x0009000000000077, lsp.presence)
        query = struct.pack(">II", 0, 1) + struct.pack(">Q", 0x0009000000000077) + bytes(0x78)
        status, _, body = self.post("/ReachPresenceApi/query.ashx",
                                    lsp.chunk(b"preq", 3, 1, query), "presence")
        major, _, reply = lsp.chunks(body)[b"pplr"]
        self.assertEqual((major, len(reply)), (5, 0x1094))

    def test_storage(self):
        path = "/storage/title/4d53085b/tracked/11860/default_hoppers/manifest_001.bin"
        self.assertEqual(lsp.handle(None, "GET", path, {}, b"", None)[0], 404)
        full = os.path.join(self.dir.name, "storage", path.lstrip("/"))
        os.makedirs(os.path.dirname(full))
        with open(full, "wb") as f:
            f.write(b"operator file")
        self.assertEqual(lsp.handle(None, "GET", path, {}, b"", None)[2], b"operator file")
        self.assertEqual(lsp.handle(None, "GET", "/storage/../../etc/passwd", {}, b"", None)[0], 404)


def variant_file(name, file_type=5, map_id=1000, author=b"Noble Six", compress=False):
    """A synthetic content file: `_blf`, `chdr` (u16 build, u16, 0x2B0 metadata), data."""
    md = bytearray(0x2B0)
    md[0] = file_type
    struct.pack_into(">QI", md, 8, 0x1122334455667788, 0)
    struct.pack_into(">i", md, 0x2C, map_id)
    md[0x48:0x48 + len(author)] = author        # creator
    md[0x6C:0x6C + len(author)] = author        # last modified by
    md[0x80:0x80 + 2 * len(name)] = name.encode("utf-16-be")
    inner = lsp.chunk(b"chdr", 10, 2, struct.pack(">HH", 11860, 0) + bytes(md)) + \
        lsp.chunk(b"mvar", 31, 1, bytes(64))
    if compress:
        inner = lsp.chunk(b"_cmp", 1, 1, b"\0" + struct.pack(">I", len(inner)) +
                          zlib.compress(inner))
    return lsp.blf(inner)


class FileShareTest(unittest.TestCase):
    def setUp(self):
        self.dir = tempfile.TemporaryDirectory()
        lsp.configure(types.SimpleNamespace(data_dir=self.dir.name, dump_requests=False))
        self.ids = "machineId=fa00000000000001&userId=0009000000000001&shareId=0009000000000001"

    def tearDown(self):
        self.dir.cleanup()

    def get(self, path):
        return lsp.handle(None, "GET", path, {}, b"", ("127.0.0.1", 1))

    def upload(self, data):
        status, _, reply = self.get("/gameapi_omaha/FilesNewUpload.ashx?&%s&uniqueId=1122334455667788"
                                    "&fileType=5&uncompressedSize=%d&compressedSize=%d"
                                    % (self.ids, len(data), len(data)))
        self.assertEqual(status, 200)
        server_id = int(reply, 16)
        headers = {"machineid": '"fa00000000000001"', "userid": '"0009000000000001"',
                   "shareid": '"0009000000000001"', "serverid": '"%x"' % server_id}
        body = BOUNDARY + b'\r\nContent-Disposition: form-data; name="upload"; ' \
            b'filename="blob"\r\nContent-Type: application/octet-stream\r\n\r\n' + data + \
            b"\r\n" + BOUNDARY + b"--\r\n"
        status, _, _ = lsp.handle(None, "POST", "/gameapi_omaha/FilesUpload.ashx", headers, body,
                                  None)
        self.assertEqual(status, 200)
        return server_id

    def items(self, reply):
        major, _, payload = lsp.chunks(reply[2])[b"fitm"]
        self.assertEqual(major, 4)
        count = struct.unpack_from(">H", payload, 0x22)[0]
        return [payload[0x28 + i * 0x29C:0x28 + (i + 1) * 0x29C] for i in range(count)]

    def test_upload_list_search_download(self):
        data = variant_file("Pit Stop", compress=True)
        server_id = self.upload(data)
        items = self.items(self.get("/gameapi_omaha/FilesGetCatalog.ashx?&%s&locale=en" % self.ids))
        self.assertEqual(len(items), 1)
        item = items[0]
        self.assertEqual(struct.unpack_from(">Q", item, 0)[0], server_id)
        self.assertEqual(item[8], 5)                                   # map variant
        self.assertEqual(struct.unpack_from(">i", item, 0x14)[0], 1000)
        self.assertEqual(item[0x68:0x78].decode("utf-16-be"), "Pit Stop")
        found = self.items(self.get("/gameapi_omaha/FilesGetSearch.ashx?&%s&fileType=5"
                                    "&gamertag=noble%%20six&page=0" % self.ids))
        self.assertEqual(len(found), 1)
        self.assertEqual(self.items(self.get("/gameapi_omaha/FilesGetSearch.ashx?&%s&fileType=6"
                                             % self.ids)), [])
        status, _, body = self.get("/gameapi_omaha/FilesStageForDownload.ashx?&%s&serverId=%x"
                                   "&startPosition=0&fromAutoQueue=0&preview=0"
                                   % (self.ids, server_id))
        lines = dict(line.split(": ", 1) for line in body.decode().strip().split("\r\n"))
        self.assertEqual((status, int(lines["Size"]), int(lines["FullSize"])),
                         (200, len(data), len(data)))
        status, _, body = self.get(lines["InitialUrl"])
        self.assertEqual((status, body), (200, data))
        self.get("/gameapi_omaha/FilesDelete.ashx?&%s&serverId=%x" % (self.ids, server_id))
        self.assertEqual(self.items(self.get("/gameapi_omaha/FilesGetCatalog.ashx?&%s" % self.ids)), [])


    def test_hopper_statistics(self):
        """The Playlist screen's player counts: the mmhs file from the published sessions."""
        path = "/storage/title/4d53085b/tracked/11860/default_hoppers/dynamic_pres_hopper_statistics.bin"
        record = bytes(0x3C) + bytes([8, 0, 2, 1])      # info, max public/private, filled 2 + 1
        peers = {1: types.SimpleNamespace(match=(101, record, 0)),
                 2: types.SimpleNamespace(match=(101, bytes(0x3C) + bytes(4), 0)),  # empty: 1
                 3: types.SimpleNamespace(match=(103, record, 0)),
                 4: types.SimpleNamespace()}
        status, _, body = lsp.handle(types.SimpleNamespace(peers=peers), "GET", path, {}, b"", None)
        self.assertEqual(status, 200)
        major, _, payload = lsp.chunks(body)[b"mmhs"]
        self.assertEqual((major, len(payload)), (4, 0x80E))
        total, count = struct.unpack_from(">I8xh", payload)
        self.assertEqual((total, count), (7, 2))
        self.assertEqual(struct.unpack_from(">BxhI", payload, 0xE), (2, 101, 4))
        self.assertEqual(struct.unpack_from(">BxhI", payload, 0x16), (2, 103, 3))


if __name__ == "__main__":
    unittest.main()
