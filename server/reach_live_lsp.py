"""Reach Live title servers ("LSP"): the HTTP services Bungie ran for Halo: Reach.

reach_live_server.py calls handle() for every HTTP request on --http-port that isn't the
status page. The game reaches this through its title server list (src/kernel/live_lsp.cpp)
and speaks HTTP/1.0; uploads are multipart/form-data (boundary BUNGIEr0x0rz) holding a
BLF file or a raw chunk, replies are BLF files: a `_blf` header chunk, data chunks, `_eof`.
Every chunk is {fourcc, u32 size including the 12-byte header, u16 major, u16 minor},
big-endian. The game matches chunks by fourcc and major version.

What is answered (docs/online_plan.md section 5.1, docs/progression_re.md section 2):

- POST /gameapi_omaha/UserUpdateRewards.ashx (rewards sync): `rpul` v3 upload (+ `chpr`
  challenge progress), `rpdl` v2 reply. The server keeps each player's Credits,
  commendation progress and Armory flags and never lowers them, so a reinstalled game gets
  its progression back. The game syncs at sign-in, every 10 minutes, after a purchase and
  after every matchmade game.
- POST /ReachPresenceApi/heartbeat.ashx (`phbt` v5): `phbr` v2 reply with no reservations.
- POST /ReachPresenceApi/query.ashx (`preq` v3): `pplr` v5 reply with no records.
- /gameapi_omaha/Files*.ashx: File Share (reach_live_files.py).
- UserGetServiceRecord.ashx: an empty service record.
- GET /gameapi_omaha/ArenaGetSeasonStats.ashx: `arhs` v3, Arena season 1 with no ratings.
  The game counts the Halo: Reach server as up once this answers (docs/online_plan.md
  section 5.4).
- Everything else (title/user/machine storage, Arena, stats uploads): 404, which the game
  treats as "file not there" or "service unavailable". Files an operator puts under
  DATA_DIR/storage/<request path> are served as they are.
"""

import json
import logging
import os
import struct
import time
import urllib.parse

import reach_live_files as files

log = logging.getLogger("reach-live")

DATA_DIR = "reach_live_data"
DUMP = False
BOUNDARY = b"--BUNGIEr0x0rz"


def add_arguments(parser):
    parser.add_argument("--data-dir", default=os.environ.get("REACH_LIVE_DATA", "reach_live_data"),
                        help="player data and operator-provided title storage files "
                             "(default ./reach_live_data)")
    parser.add_argument("--dump-requests", action="store_true",
                        help="save every title server request to DATA_DIR/requests")


def configure(args):
    global DATA_DIR, DUMP
    DATA_DIR = args.data_dir
    DUMP = args.dump_requests
    os.makedirs(os.path.join(DATA_DIR, "players"), exist_ok=True)
    files.configure(DATA_DIR)


# --- BLF files ---------------------------------------------------------------------

def chunk(fourcc, major, minor, payload):
    return struct.pack(">4sIHH", fourcc, 12 + len(payload), major, minor) + payload


def blf(*chunks):
    """A BLF file as the game writes them: `_blf` v1.2 with the big-endian byte order
    mark, the chunks, and `_eof` v1.1 holding the length before it."""
    body = chunk(b"_blf", 1, 2, b"\xff\xfe" + bytes(0x22)) + b"".join(chunks)
    return body + chunk(b"_eof", 1, 1, struct.pack(">IB", len(body), 0))


def chunks(data):
    """{fourcc: (major, minor, payload)} of a BLF file (first chunk of each kind)."""
    out, pos = {}, 0
    while pos + 12 <= len(data):
        fourcc, size, major, minor = struct.unpack_from(">4sIHH", data, pos)
        if size < 12 or pos + size > len(data):
            break
        out.setdefault(fourcc, (major, minor, data[pos + 12:pos + size]))
        if fourcc == b"_eof":
            break
        pos += size
    return out


def upload(body):
    """The file inside a multipart/form-data upload."""
    start = body.find(b"\r\n\r\n")
    if not body.startswith(BOUNDARY) or start < 0:
        return body
    end = body.rfind(b"\r\n" + BOUNDARY)
    return body[start + 4:end if end > start else len(body)]


# --- Player records ----------------------------------------------------------------

def player_path(xuid):
    return os.path.join(DATA_DIR, "players", "%016X.json" % xuid)


def load_player(xuid):
    try:
        with open(player_path(xuid)) as f:
            return json.load(f)
    except (OSError, ValueError):
        return {}


MAX_PLAYERS = int(os.environ.get("REACH_LIVE_MAX_PLAYERS", "20000"))  # player records on disk
MAX_PRESENCE = 4096


def save_player(xuid, record):
    path = player_path(xuid)
    if not os.path.exists(path) and len(os.listdir(os.path.dirname(path))) >= MAX_PLAYERS:
        log.warning("player limit reached: %016X not saved", xuid)
        return
    with open(path + ".tmp", "w") as f:
        json.dump(record, f, indent=1)
    os.replace(path + ".tmp", path)


# --- Rewards (Credits, rank, Armory) -----------------------------------------------

BLOCK_SIZE = 0x208   # cookies i32, award count i32, i16[128] commendation progress, u8 itemflags[256]
RPUL_SIZE = 0x778
RPDL_SIZE = 0x21B


def merge_block(saved, uploaded):
    """The server's view of the totals block: never lower than either side. Credits,
    award counts and commendation progress take the maximum, Armory flags the union."""
    if not saved:
        return uploaded
    cookies = max(struct.unpack_from(">i", saved, 0)[0], struct.unpack_from(">i", uploaded, 0)[0])
    count = max(struct.unpack_from(">i", saved, 4)[0], struct.unpack_from(">i", uploaded, 4)[0])
    counters = struct.pack(">128h", *(max(a, b) for a, b in zip(
        struct.unpack_from(">128h", saved, 8), struct.unpack_from(">128h", uploaded, 8))))
    flags = bytes(a | b for a, b in zip(saved[0x108:0x208], uploaded[0x108:0x208]))
    return struct.pack(">ii", cookies, count) + counters + flags


def user_update_rewards(query, body):
    xuid = int(query.get("userId", ["0"])[0], 16)
    sent = chunks(upload(body))
    rpul = sent.get(b"rpul")
    if not xuid or not rpul or rpul[0] != 3 or len(rpul[2]) < RPUL_SIZE:
        return 400, "text/plain", b"bad rewards upload"
    record = load_player(xuid)
    saved = bytes.fromhex(record.get("rewards", "")) if record.get("rewards") else b""
    block = merge_block(saved, rpul[2][:BLOCK_SIZE])
    record["rewards"] = block.hex()
    record["gamertag"] = rpul[2][0x735:0x745].split(b"\0")[0].decode("utf-8", "replace")
    if b"chpr" in sent:
        record["challenge_progress"] = sent[b"chpr"][2].hex()
    record["updated"] = int(time.time())
    save_player(xuid, record)
    cookies = struct.unpack_from(">i", block, 0)[0]
    log.info("rewards %016X %s: %d cR, %d Armory items", xuid, record["gamertag"], cookies,
             sum(1 for b in block[0x108:0x208] if b & 1))
    # rpdl: the totals block, then u16, u32, u64 the game stores without using them here
    # (zero), and a bonus grant (i32 amount, u8 type) of nothing.
    rpdl = block + bytes(RPDL_SIZE - BLOCK_SIZE)
    # No `dcha`: every client picks the same challenges from the date
    # (src/hooks/offline_challenges.cpp), so the server does not choose them.
    return 200, "application/octet-stream", blf(chunk(b"rpdl", 2, 1, rpdl))


# --- Presence (ReachPresenceApi) -----------------------------------------------------

presence = {}  # xuid -> (time, phbt payload) of the latest heartbeat that named it


def heartbeat(body):
    """`phbt` v5 (0x1BB bytes): in-matchmaking flag, the machine's signed-in players
    (u8 count, 4 x 0x38: u64 XUID, ...), machine id at +0xE2, then matchmaking party
    state. The reply `phbr` v2 (0x93 bytes) can flag players and list XUIDs reserved
    for this party (join checks count them); all zero here."""
    data = upload(body)
    if data[:4] == b"phbt" and len(data) >= 12 + 0x1BB:
        payload = data[12:12 + 0x1BB]
        for i in range(min(payload[1], 4)):
            (xuid,) = struct.unpack_from(">Q", payload, 2 + 0x38 * i)
            if xuid:
                if len(presence) >= MAX_PRESENCE and xuid not in presence:
                    presence.pop(min(presence, key=lambda k: presence[k][0]))
                presence[xuid] = (time.time(), payload)
    return 200, "application/octet-stream", blf(chunk(b"phbr", 2, 1, bytes(0x93)))


def presence_query(body):
    """`preq` v3: u32, u32 count, u64 XUIDs[16]. Reply `pplr` v5: u32 count, then up to
    16 records of 0x109 bytes (XUID at +8) that the active roster shows next to each
    friend and uses in its join checks. Their layout is not mapped yet, so no records:
    the roster then relies on XAM presence and the session's QoS details."""
    data = upload(body)
    if data[:4] == b"preq" and len(data) >= 12 + 8:
        _, count = struct.unpack_from(">II", data, 12)
        xuids = struct.unpack_from(">%dQ" % min(count, 16), data, 20)
        log.debug("presence query for %s", " ".join("%016X" % x for x in xuids))
    return 200, "application/octet-stream", blf(chunk(b"pplr", 5, 1, bytes(0x1094)))


# --- Service record ------------------------------------------------------------------

SRID_SIZE = 0xD48


def service_record(query):
    """UserGetServiceRecord: the game reads the reply straight into a 0xD48-byte `srid`
    v7.1 chunk (no BLF wrapper). Bungie filled it from uploaded game results; this server
    has none, so the record is empty."""
    return 200, "application/octet-stream", chunk(b"srid", 7, 1, bytes(SRID_SIZE - 12))


ARHS_SIZE = 0x16 + 0x20 * 0x1E


def arena_season_stats(query):
    """ArenaGetSeasonStats (?machineId=&players=&version=3): `arhs` v3 is {i32 season (the
    game drops a reply below 1), i32, u64, u8, u8, u32 rating count (32 at most), then per
    player and playlist a 30-byte rating starting with the XUID}. Matchmaking stays
    "server unavailable" until a reply parses; season 1 with no ratings is enough."""
    payload = struct.pack(">iiQBBI", 1, 0, 0, 0, 0, 0)
    return 200, "application/octet-stream", blf(chunk(b"arhs", 3, 1, payload.ljust(ARHS_SIZE, b"\0")))


# --- Dispatch ------------------------------------------------------------------------

def dump(method, path, headers, body):
    folder = os.path.join(DATA_DIR, "requests")
    os.makedirs(folder, exist_ok=True)
    name = "%d_%s_%s" % (time.time() * 1000, method,
                         urllib.parse.urlsplit(path).path.strip("/").replace("/", "_")[:80])
    with open(os.path.join(folder, name + ".txt"), "w") as f:
        f.write("%s %s\n" % (method, path))
        for key, value in headers.items():
            f.write("%s: %s\n" % (key, value))
    if body:
        with open(os.path.join(folder, name + ".body"), "wb") as f:
            f.write(body)


def stored_file(path):
    """An operator-provided file for a storage path, or None."""
    root = os.path.realpath(os.path.join(DATA_DIR, "storage"))
    full = os.path.realpath(os.path.join(root, path.lstrip("/")))
    if not full.startswith(root + os.sep) or not os.path.isfile(full):
        return None
    with open(full, "rb") as f:
        return f.read()


def hopper_population(server):
    """The Playlist screen's player counts: the players of every matchmaking session
    published for a playlist (the search key is the hopper id), from the server's peers.
    An operator-provided file in storage replaces it."""
    import reach_live_hoppers
    stored = stored_file("storage/title/4d53085b/tracked/11860/default_hoppers/"
                         "dynamic_pres_hopper_statistics.bin")
    if stored is not None:
        return stored
    counts = {}
    for peer in list(getattr(server, "peers", {}).values()):
        match = getattr(peer, "match", None)
        if not match:
            continue
        key, record = match[0], match[1]
        # record: XSESSION_INFO (0x3C), then max public/private, filled public/private slots
        players = record[0x3E] + record[0x3F] if len(record) > 0x3F else 0
        counts[key] = counts.get(key, 0) + max(1, players)
    return reach_live_hoppers.hopper_statistics(counts)


def handle(server, method, path, headers, body, peer):
    """Returns (HTTP status, content type, body bytes)."""
    if DUMP:
        dump(method, path, headers, body)
    url = urllib.parse.urlsplit(path)
    query = urllib.parse.parse_qs(url.query)
    route = url.path.lower()
    if route == "/gameapi_omaha/userupdaterewards.ashx" and method == "POST":
        return user_update_rewards(query, body)
    if route == "/reachpresenceapi/heartbeat.ashx" and method == "POST":
        return heartbeat(body)
    if route == "/reachpresenceapi/query.ashx" and method == "POST":
        return presence_query(body)
    if route == "/gameapi_omaha/usergetservicerecord.ashx":
        return service_record(query)
    if route == "/gameapi_omaha/arenagetseasonstats.ashx":
        return arena_season_stats(query)
    if route.startswith("/gameapi_omaha/files"):
        reply = files.handle(method, route, query, body, blf, chunk, upload, headers)
        if reply is not None:
            return reply
    if route.endswith("/default_hoppers/dynamic_pres_hopper_statistics.bin") and method == "GET":
        return 200, "application/octet-stream", hopper_population(server)
    if route.startswith("/storage/") and method == "GET":
        data = stored_file(url.path)
        if data is not None:
            return 200, "application/octet-stream", data
    return 404, "text/plain", b"not found"
