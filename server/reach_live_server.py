#!/usr/bin/env python3
"""Reach Live: a self-hostable rendezvous and relay server for the Halo: Reach PC port.

Players point the game at a server with REACH_SERVER=host[:port]. Everyone connected
to the same server (and room) sees everyone else's System Link games, and game traffic
flows between them: directly when UDP hole punching works, through this server when it
doesn't. Nothing here talks to Microsoft or Bungie services.

    python3 server/reach_live_server.py [--port 21100] [--http-port 21101]

The wire format is in docs/online_plan.md ("Reach Live protocol"). Standard library only.
"""

import argparse
import asyncio
import json
import logging
import os
import random
import signal
import struct
import time

import reach_live_lsp as lsp

MAGIC = b"RLV1"
VERSION = 1

(HELLO, WELCOME, ERROR, BROADCAST, RELAY, FORWARD, PUNCH, PUNCH_ACK, DATA, BYE, LIST, PEERS,
 PRESENCE, INVITE) = range(1, 15)
MATCH_PUBLISH, MATCH_SEARCH, MATCH_RESULTS = 15, 16, 17  # matchmaking sessions
MAX_MATCH_RECORD = 1300
SESSION_INFO_SIZE = 0x3C  # XSESSION_INFO: session id, host XNADDR, key-exchange key
MAX_PRESENCE_EXTRA = 1024

PEER_TIMEOUT = 30.0  # seconds without a HELLO before a peer is dropped
MAX_PAYLOAD = 1500
# Per-player limit on bytes the server sends for it (relayed datagrams; a broadcast counts
# once per recipient): a two-player match is about 2 KB/s per player, a full lobby of 16
# a few times that. Defaults to 64 KB/s with 256 KB bursts (--rate-limit).
RATE_LIMIT = 64 * 1024
RATE_BURST = 4 * RATE_LIMIT


def set_rate_limit(bytes_per_second):
    global RATE_LIMIT, RATE_BURST
    RATE_LIMIT, RATE_BURST = bytes_per_second, 4 * bytes_per_second
MAX_NAME = 15  # XUSER_NAME_SIZE minus the terminator
MAX_ROOM = 64
MAX_MOTD = 64  # keeps the WELCOME about as large as the HELLO that asks for it
# Limits against abuse from the internet (every datagram is untrusted):
MAX_PEERS_PER_IP = 16  # registered players behind one public address (a LAN, a CGNAT)
PACKETS_PER_IP = (1500, 3000)  # datagrams/s and burst accepted from one address
HELLOS_PER_IP = (2, 20)  # new registrations/s and burst from one address
ERRORS_PER_IP = (2, 10)  # "not registered" replies/s and burst to one address


def take(buckets, key, limit, cost=1.0):
    """Token bucket per key: limit = (tokens per second, burst). False = over the limit."""
    rate, burst = limit
    now = time.monotonic()
    tokens, last = buckets.get(key, (burst, now))
    tokens = min(burst, tokens + (now - last) * rate)
    ok = tokens >= cost
    buckets[key] = (tokens - cost if ok else tokens, now)
    return ok

log = logging.getLogger("reach-live")


class Peer:
    def __init__(self, peer_id, addr, token, xuid, name, room, local):
        self.id = peer_id
        self.addr = addr  # public (ip, port) as this server sees it
        self.token = token
        self.xuid = xuid
        self.name = name
        self.room = room
        self.local = local  # (ip, port) the client reports for its LAN side
        self.seen = time.monotonic()
        self.since = time.time()
        self.relayed_bytes = 0
        # Token bucket for what this player makes the server send (relay, broadcast).
        self.budget = RATE_BURST
        self.budget_time = time.monotonic()
        # Presence, as friends see it: X_ONLINE_FRIENDSTATE flags, the joinable session
        # (an XSESSION_INFO, zeros when none) and a status line.
        self.state = 0
        self.session = bytes(SESSION_INFO_SIZE)
        self.status = ""
        self.extra = b""  # slots and QoS data of the hosted session, opaque to the server


def ip_to_u32(ip):
    a, b, c, d = (int(x) for x in ip.split("."))
    return a << 24 | b << 16 | c << 8 | d


def u32_to_ip(v):
    return f"{v >> 24}.{v >> 16 & 255}.{v >> 8 & 255}.{v & 255}"


class ReachLive(asyncio.DatagramProtocol):
    def __init__(self, motd, max_peers):
        self.motd = motd.encode()[:MAX_MOTD]
        self.packet_buckets = {}  # ip -> token bucket of datagrams accepted
        self.hello_buckets = {}
        self.error_buckets = {}
        self.per_ip = {}  # ip -> registered peers
        self.max_peers = max_peers
        self.epoch = random.randrange(1, 0x10000)  # changes when the server restarts
        self.peers = {}  # id -> Peer
        self.by_addr = {}  # (ip, port) -> Peer
        self.next_id = random.randrange(1, 0x1000)
        self.transport = None
        self.started = time.time()
        self.relayed_packets = 0
        self.http_port = 0  # TCP port of the title servers, told to clients; 0 = none
        self.dropped_packets = 0  # over a player's rate limit

    def connection_made(self, transport):
        self.transport = transport

    def send(self, addr, kind, body=b""):
        self.transport.sendto(MAGIC + bytes([kind]) + body, addr)

    def error(self, addr, code, message):
        text = message.encode()
        self.send(addr, ERROR, struct.pack(">BH", code, len(text)) + text)

    def datagram_received(self, data, addr):
        if len(data) < 5 or data[:4] != MAGIC or len(data) > 2048:
            return
        if not take(self.packet_buckets, addr[0], PACKETS_PER_IP):
            self.dropped_packets += 1
            return
        kind, body = data[4], data[5:]
        try:
            if kind == HELLO:
                self.on_hello(body, addr)
                return
            peer = self.by_addr.get(addr)
            if peer is None:
                # Unknown sender (e.g. the server restarted): ask it to register again
                # (rate limited: the sender's address may be forged).
                if take(self.error_buckets, addr[0], ERRORS_PER_IP):
                    self.error(addr, 1, "not registered")
                return
            peer.seen = time.monotonic()
            if kind == BROADCAST:
                self.on_broadcast(peer, body)
            elif kind == RELAY:
                self.on_relay(peer, body)
            elif kind == LIST:
                self.on_list(peer, addr)
            elif kind == PRESENCE:
                self.on_presence(peer, body)
            elif kind == INVITE:
                self.on_invite(peer, body)
            elif kind == BYE:
                self.drop(peer, "left")
            elif kind in (MATCH_PUBLISH, MATCH_SEARCH):
                self.on_match(peer, kind, body)
        except Exception as e:  # a malformed datagram must never end the server
            log.debug("bad packet from %s: %r", addr, e)

    def on_hello(self, body, addr):
        version, token, xuid, local_ip, local_port = struct.unpack_from(">HQQIH", body, 0)
        pos = 24
        room_len = body[pos]
        room = body[pos + 1:pos + 1 + room_len].decode()[:MAX_ROOM]
        pos += 1 + room_len
        name_len = body[pos]
        name = body[pos + 1:pos + 1 + name_len].decode()[:MAX_NAME]
        if version != VERSION:
            self.error(addr, 2, f"server speaks protocol {VERSION}, client {version}")
            return
        peer = self.by_addr.get(addr)
        if peer is not None and peer.token != token:
            self.drop(peer, "restarted")  # same address, new game process
            peer = None
        if peer is None:
            if len(self.peers) >= self.max_peers or self.per_ip.get(addr[0], 0) >= MAX_PEERS_PER_IP:
                if take(self.error_buckets, addr[0], ERRORS_PER_IP):
                    self.error(addr, 3, "server full")
                return
            if not take(self.hello_buckets, addr[0], HELLOS_PER_IP):
                return
            while self.next_id in self.peers or self.next_id == 0:
                self.next_id = (self.next_id + 1) & 0xFFFFFF
            peer = Peer(self.next_id, addr, token, xuid, name, room, (local_ip, local_port))
            self.next_id = (self.next_id + 1) & 0xFFFFFF
            self.peers[peer.id] = peer
            self.by_addr[addr] = peer
            self.per_ip[addr[0]] = self.per_ip.get(addr[0], 0) + 1
            log.info("join  #%d %r xuid %016X room %r from %s:%d (lan %s:%d)", peer.id, name, xuid,
                     room, addr[0], addr[1], u32_to_ip(local_ip), local_port)
        peer.seen = time.monotonic()
        peer.name, peer.xuid, peer.room, peer.local = name, xuid, room, (local_ip, local_port)
        body = struct.pack(">IHIHH", peer.id, self.epoch, ip_to_u32(addr[0]), addr[1], len(self.motd))
        self.send(addr, WELCOME, body + self.motd + struct.pack(">H", self.http_port))

    def forward_header(self, src):
        return struct.pack(">IIHIH", src.id, ip_to_u32(src.addr[0]), src.addr[1], src.local[0],
                           src.local[1])

    def room_peers(self, src):
        return [p for p in self.peers.values() if p.room == src.room and p is not src]

    def spend(self, peer, nbytes):
        """Takes nbytes from the player's budget; False (drop it) when over the limit."""
        now = time.monotonic()
        peer.budget = min(RATE_BURST, peer.budget + (now - peer.budget_time) * RATE_LIMIT)
        peer.budget_time = now
        if peer.budget < nbytes:
            self.dropped_packets += 1
            return False
        peer.budget -= nbytes
        return True

    def on_broadcast(self, src, body):
        if len(body) < 4 or len(body) - 4 > MAX_PAYLOAD:
            return
        if not self.spend(src, len(body) * max(1, len(self.room_peers(src)))):
            return
        packet = self.forward_header(src) + body
        for peer in self.room_peers(src):
            self.send(peer.addr, FORWARD, packet)

    def on_relay(self, src, body):
        if len(body) < 8 or len(body) - 8 > MAX_PAYLOAD:
            return
        if not self.spend(src, len(body)):
            return
        (dst_id,) = struct.unpack_from(">I", body, 0)
        dst = self.peers.get(dst_id)
        if dst is None or dst.room != src.room:
            return
        src.relayed_bytes += len(body) - 8
        self.relayed_packets += 1
        self.send(dst.addr, FORWARD, self.forward_header(src) + body[4:])

    def on_presence(self, peer, body):
        """State u32, XSESSION_INFO, status (u8 length + UTF-8), then optional bytes the
        clients define (session slots, QoS data), passed on as they are."""
        (state,) = struct.unpack_from(">I", body, 0)
        session = body[4:4 + SESSION_INFO_SIZE]
        status_len = body[4 + SESSION_INFO_SIZE]
        end = 5 + SESSION_INFO_SIZE + status_len
        status = body[5 + SESSION_INFO_SIZE:end].decode()
        extra = body[end:end + MAX_PRESENCE_EXTRA]
        if len(session) != SESSION_INFO_SIZE:
            return
        if (state, session, status) != (peer.state, peer.session, peer.status):
            log.info("presence #%d %r state %08X session %s %r", peer.id, peer.name, state,
                     session[:8].hex(), status)
        peer.state, peer.session, peer.status, peer.extra = state, session, status, extra

    def on_invite(self, src, body):
        """A game invite: target XUID u64 + the inviter's XSESSION_INFO. Delivered to
        that player in the same room as: inviter id u32, inviter XUID u64, gamertag
        (u8 length + UTF-8), XSESSION_INFO."""
        (target,) = struct.unpack_from(">Q", body, 0)
        session = body[8:8 + SESSION_INFO_SIZE]
        if len(session) != SESSION_INFO_SIZE:
            return
        name = src.name.encode()
        for peer in self.room_peers(src):
            if peer.xuid == target:
                log.info("invite #%d %r -> #%d %r", src.id, src.name, peer.id, peer.name)
                self.send(peer.addr, INVITE, struct.pack(">IQB", src.id, src.xuid, len(name)) +
                          name + session)

    def on_list(self, src, addr):
        """The room's players (the asker first): the friends list of a Live player.
        Each: id u32, XUID u64, gamertag (u8 length + UTF-8), presence state u32,
        XSESSION_INFO, status (u8 length + UTF-8), extra (u16 length + bytes). Sent in
        parts that fit a datagram."""
        peers = ([src] + self.room_peers(src))[:100]
        if not self.spend(src, 600):
            return
        entries = []
        for p in peers:
            name, status = p.name.encode(), p.status.encode()[:255]
            entries.append(struct.pack(">IQB", p.id, p.xuid, len(name)) + name +
                           struct.pack(">I", p.state) + p.session + bytes([len(status)]) + status +
                           struct.pack(">H", len(p.extra)) + p.extra)
        # Part header: total players u16, index of the first entry u16, entries in part u16.
        start = 0
        while True:
            part, size = [], 6
            while start + len(part) < len(entries) and size + len(entries[start + len(part)]) <= 1200:
                size += len(entries[start + len(part)])
                part.append(entries[start + len(part)])
            self.send(addr, PEERS, struct.pack(">HHH", len(entries), start, len(part)) + b"".join(part))
            start += len(part)
            if start >= len(entries) or not part:
                break

    # --- Matchmaking sessions (docs/online_plan.md section 5.4) ----------------------
    # A player hosting a matchmaking session publishes it with a search key (the
    # playlist; clients put their tick rate in the top byte when it is not 30, so players
    # with different frame_rate settings are never matched, docs/framerate.md); players
    # searching with that key get the room's matching sessions. The
    # session record is the client's (XSESSION_INFO, slots, properties, QoS data) and
    # opaque here. Every searching group also hosts a session, and two groups that find
    # each other both join the other and wait for it forever, so a searcher with a
    # session of its own only sees older sessions: newer groups join older ones.

    def on_match(self, peer, kind, body):
        if kind == MATCH_PUBLISH:
            # op u8: 0 withdraw, 1 publish (then search key u32 + the record)
            if body[0] == 0:
                peer.match = None
            else:
                (key,) = struct.unpack_from(">I", body, 1)
                record = body[5:5 + MAX_MATCH_RECORD]
                old = getattr(peer, "match", None)
                since = old[2] if old and old[1][:8] == record[:8] else time.monotonic()
                peer.match = (key, record, since)  # since: when this session first appeared
            return
        # search key u32, request number u32. Reply: request number u32, count u16, then
        # per session the host's id u32, XUID u64 and record (u16 length + bytes), oldest
        # first, as many as fit a datagram.
        key, request = struct.unpack_from(">II", body, 0)
        own = getattr(peer, "match", None)
        found = [p for p in self.room_peers(peer) if getattr(p, "match", None) and p.match[0] == key
                 and not (own and own[0] == key and p.match[2] >= own[2])]
        found.sort(key=lambda p: p.match[2])
        entries, size = [], 6
        for p in found:
            entry = struct.pack(">IQH", p.id, p.xuid, len(p.match[1])) + p.match[1]
            if size + len(entry) > 1400:
                break
            entries.append(entry)
            size += len(entry)
        if self.spend(peer, size):
            self.send(peer.addr, MATCH_RESULTS, struct.pack(">IH", request, len(entries)) + b"".join(entries))

    def drop(self, peer, why):
        self.peers.pop(peer.id, None)
        if self.by_addr.get(peer.addr) is peer:
            del self.by_addr[peer.addr]
            left = self.per_ip.get(peer.addr[0], 1) - 1
            if left > 0:
                self.per_ip[peer.addr[0]] = left
            else:
                self.per_ip.pop(peer.addr[0], None)
        log.info("leave #%d %r (%s)", peer.id, peer.name, why)

    def expire(self):
        now = time.monotonic()
        for peer in [p for p in self.peers.values() if now - p.seen > PEER_TIMEOUT]:
            self.drop(peer, "timed out")
        for buckets in (self.packet_buckets, self.hello_buckets, self.error_buckets):
            if len(buckets) > 5000:  # forgotten addresses (forged sources)
                for ip in [ip for ip, (_, last) in buckets.items() if now - last > 60]:
                    del buckets[ip]

    def status(self):
        rooms = {}
        for p in self.peers.values():
            rooms.setdefault(p.room, []).append(
                {"id": p.id, "gamertag": p.name, "xuid": f"{p.xuid:016X}",
                 "online_for": int(time.time() - p.since), "relayed_bytes": p.relayed_bytes,
                 "state": f"{p.state:08X}", "session": p.session[:8].hex(), "status": p.status})
        return {"server": "reach-live", "protocol": VERSION, "uptime": int(time.time() - self.started),
                "players": len(self.peers), "relayed_packets": self.relayed_packets,
                "dropped_packets": self.dropped_packets, "rooms": rooms}


async def read_request(reader):
    """One HTTP/1.x request: (method, path, headers, body)."""
    head = await asyncio.wait_for(reader.readuntil(b"\r\n\r\n"), 15)
    lines = head.decode("latin-1").split("\r\n")
    method, path = lines[0].split(" ")[:2]
    headers = {}
    for line in lines[1:]:
        if ":" in line:
            key, value = line.split(":", 1)
            headers[key.strip().lower()] = value.strip()
    if len(lines) > MAX_HEADERS:
        raise ValueError("too many headers")
    length = int(headers.get("content-length", 0) or 0)
    if not 0 <= length <= MAX_BODY:
        raise ValueError("body of %d bytes refused" % length)
    body = await asyncio.wait_for(reader.readexactly(length), 30) if length else b""
    return method, path, headers, body


MAX_BODY = 16777216  # bytes of one request body (uploads); larger requests are refused
MAX_HEADERS = 64
HTTP_PER_IP = 32  # simultaneous connections from one address
HTTP_TOTAL = 1024
http_conns = {}  # ip -> open connections


def response(status, body, content_type="application/octet-stream", extra=()):
    reason = {200: "OK", 400: "Bad Request", 403: "Forbidden", 404: "Not Found",
              500: "Internal Server Error"}
    head = [f"HTTP/1.0 {status} {reason.get(status, 'OK')}", f"Content-Type: {content_type}",
            f"Content-Length: {len(body)}", "Access-Control-Allow-Origin: *", *extra]
    return ("\r\n".join(head) + "\r\n\r\n").encode() + body


async def http_handler(server, reader, writer):
    """The HTTP side of the server, on --http-port. The game reaches it as its title
    servers ("LSP": title/user storage, rewards; reach_live_lsp.py). GET / or /status
    returns a JSON status page for people and web front ends."""
    peer = writer.get_extra_info("peername") or ("?", 0)
    ip = peer[0]
    if http_conns.get(ip, 0) >= HTTP_PER_IP or sum(http_conns.values()) >= HTTP_TOTAL:
        writer.close()
        return
    http_conns[ip] = http_conns.get(ip, 0) + 1
    try:
        method, path, headers, body = await read_request(reader)
        if path in ("/", "/status"):
            reply = response(200, json.dumps(server.status(), indent=2).encode(), "application/json")
        else:
            try:
                status, content_type, data = lsp.handle(server, method, path, headers, body, peer)
            except Exception:  # a bad request must not take the server down
                log.exception("LSP %s %s failed", method, path)
                status, content_type, data = 500, "text/plain", b"error"
            log.info("http  %s %s %s -> %d (%d bytes)", peer[0], method, path, status, len(data))
            reply = response(status, data, content_type)
        writer.write(reply)
        await writer.drain()
    except (asyncio.TimeoutError, asyncio.IncompleteReadError, ConnectionError, ValueError,
            asyncio.LimitOverrunError, UnicodeError, OSError):
        pass
    except Exception:  # never let one connection's task fail loudly or take the server down
        log.exception("http connection from %s failed", peer)
    finally:
        http_conns[ip] = http_conns.get(ip, 1) - 1
        if http_conns[ip] <= 0:
            http_conns.pop(ip, None)
        writer.close()


async def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--host", default="0.0.0.0", help="address to listen on")
    parser.add_argument("--port", type=int, default=int(os.environ.get("REACH_LIVE_PORT", 21100)),
                        help="UDP port for the game (default 21100)")
    parser.add_argument("--http-port", type=int, default=0,
                        help="TCP port for the game's title servers (LSP) and a JSON status "
                             "page (default: off)")
    parser.add_argument("--motd", default="Welcome to Reach Live", help="message sent to clients")
    parser.add_argument("--max-peers", type=int, default=1024)
    parser.add_argument("--rate-limit", type=int, default=RATE_LIMIT,
                        help="bytes per second the server relays for one player (bursts of 4 s)")
    parser.add_argument("-v", "--verbose", action="store_true")
    lsp.add_arguments(parser)
    args = parser.parse_args()
    lsp.configure(args)
    set_rate_limit(args.rate_limit)
    logging.basicConfig(level=logging.DEBUG if args.verbose else logging.INFO,
                        format="%(asctime)s %(message)s")

    loop = asyncio.get_running_loop()
    transport, server = await loop.create_datagram_endpoint(
        lambda: ReachLive(args.motd, args.max_peers), local_addr=(args.host, args.port))
    log.info("Reach Live listening on UDP %s:%d (epoch %04X)", args.host, args.port, server.epoch)
    if args.http_port:
        server.http_port = args.http_port
        await asyncio.start_server(lambda r, w: http_handler(server, r, w), args.host, args.http_port,
                                 limit=1 << 15)
        log.info("title servers and status page on http://%s:%d/", args.host, args.http_port)
    try:
        while True:
            await asyncio.sleep(5)
            server.expire()
    finally:
        transport.close()


if __name__ == "__main__":
    # SIGTERM (docker stop) ends the server like Ctrl+C; as a container's first process
    # it would otherwise be ignored.
    signal.signal(signal.SIGTERM, signal.default_int_handler)
    try:
        asyncio.run(main())
    except KeyboardInterrupt:
        pass
