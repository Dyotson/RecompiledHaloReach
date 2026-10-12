"""Abuse tests: malformed packets, per-address limits, upload caps."""

import os
import socket
import struct
import subprocess
import sys
import tempfile
import time
import unittest
import urllib.request

from test_reach_live_server import Client, free_port, HERE, MAGIC, HELLO, WELCOME, ERROR, LIST

import reach_live_files as files


class Server:
    def __init__(self, *extra):
        self.port, self.http = free_port(), free_port()
        self.data = tempfile.TemporaryDirectory()
        self.proc = subprocess.Popen(
            [sys.executable, os.path.join(HERE, "reach_live_server.py"), "--host", "127.0.0.1",
             "--port", str(self.port), "--http-port", str(self.http), "--data-dir", self.data.name,
             *extra], stderr=subprocess.DEVNULL)
        time.sleep(0.6)
        self.addr = ("127.0.0.1", self.port)

    def close(self):
        self.proc.terminate()
        self.proc.wait()
        self.data.cleanup()


class SecurityTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.s = Server()

    @classmethod
    def tearDownClass(cls):
        cls.s.close()

    def test_garbage_does_not_stop_the_server(self):
        sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        for kind in range(0, 40):
            for body in (b"", b"\xff", b"\x00" * 3, b"\xff" * 40, b"\x01" * 200):
                sock.sendto(MAGIC + bytes([kind]) + body, self.s.addr)
        c = Client(self.s.addr, b"Alive", 0x0009000000000101)
        c.hello()  # still answers
        c.send(LIST, b"")
        self.assertEqual(c.recv()[0], 12)
        sock.close()

    def test_welcome_is_not_much_larger_than_hello(self):
        c = Client(self.s.addr, b"Amp", 0x0009000000000102)
        port = c.sock.getsockname()[1]
        hello = MAGIC + bytes([HELLO]) + struct.pack(">HQQIH", 1, 1, c.xuid, 0, port) + b"\0\3Amp"
        c.sock.sendto(hello, self.s.addr)
        reply, _ = c.sock.recvfrom(65536)
        self.assertLess(len(reply), 3 * len(hello))

    def test_unregistered_errors_are_rate_limited(self):
        c = Client(self.s.addr, b"Spoof", 0x0009000000000103)
        for _ in range(100):
            c.send(LIST, b"")
        c.sock.settimeout(0.5)
        errors = 0
        try:
            while True:
                errors += c.recv()[0] == ERROR
        except socket.timeout:
            pass
        self.assertLess(errors, 30)

    def test_http_oversized_body_is_refused(self):
        sock = socket.create_connection(("127.0.0.1", self.s.http), timeout=3)
        sock.sendall(b"POST /gameapi_omaha/FilesUpload.ashx HTTP/1.0\r\n"
                     b"Content-Length: 99999999999\r\n\r\n")
        self.assertEqual(sock.recv(100), b"")  # closed without a reply or an allocation
        sock.close()
        status = urllib.request.urlopen("http://127.0.0.1:%d/status" % self.s.http, timeout=3)
        text = status.read().decode()
        self.assertNotIn("127.0.0.1", text)  # no player addresses on the public page

    def test_http_garbage_headers(self):
        for raw in (b"\xff\xfe\r\n\r\n", b"GET\r\n\r\n", b"A" * 100000 + b"\r\n\r\n",
                    b"POST / HTTP/1.0\r\nContent-Length: -5\r\n\r\n"):
            sock = socket.create_connection(("127.0.0.1", self.s.http), timeout=3)
            sock.sendall(raw)
            sock.settimeout(2)
            try:
                sock.recv(100)
            except (socket.timeout, ConnectionError):
                pass
            sock.close()
        urllib.request.urlopen("http://127.0.0.1:%d/" % self.s.http, timeout=3).read()


class PerIpTest(unittest.TestCase):
    def test_peers_per_address_are_capped(self):
        s = Server()
        try:
            welcomed = 0
            clients = [Client(s.addr, b"P%d" % i, 0x0009000000000200 + i) for i in range(24)]
            for i, c in enumerate(clients):
                port = c.sock.getsockname()[1]
                c.send(HELLO, struct.pack(">HQQIH", 1, 1, c.xuid, 0, port) + b"\0" + bytes([2]) + b"Pn")
                c.sock.settimeout(0.5)
                try:
                    welcomed += c.recv()[0] == WELCOME
                except socket.timeout:
                    pass
            self.assertEqual(welcomed, 16)
        finally:
            s.close()


class UploadTest(unittest.TestCase):
    def setUp(self):
        self.dir = tempfile.TemporaryDirectory()
        files.configure(self.dir.name)

    def tearDown(self):
        self.dir.cleanup()
        files._usage[0] = 0

    def test_upload_size_caps_and_traversal_safe_ids(self):
        q = {"shareId": ["0009000000000001"], "uniqueId": ["1"], "compressedSize": [str(files.MAX_FILE + 1)]}
        self.assertEqual(files.new_upload(q)[0], 400)
        q["compressedSize"] = ["100"]
        status, _, sid = files.new_upload(q)
        self.assertEqual(status, 200)
        self.assertEqual(files.u64({"x": ["../../etc/passwd"]}, "x"), 0)
        self.assertEqual(files.u64({"x": ["-1"]}, "x"), 0)
        # data past the announced size is refused
        body = (b"--BUNGIEr0x0rz\r\nContent-Disposition: form-data; name=\"upload\"\r\n\r\n" +
                b"x" * 500 + b"\r\n--BUNGIEr0x0rz--\r\n")
        headers = {"shareid": "0009000000000001", "serverid": sid.decode()}
        self.assertEqual(files.upload_data({}, body, headers)[0], 400)

    def test_slots_and_pending_are_capped(self):
        q = {"shareId": ["0009000000000002"], "uniqueId": ["1"], "compressedSize": ["10"]}
        for _ in range(20):
            files.new_upload(q)
        self.assertLessEqual(len(files.load_index(0x0009000000000002)["pending"]), files.MAX_PENDING)


if __name__ == "__main__":
    unittest.main()
