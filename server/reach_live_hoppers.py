#!/usr/bin/env python3
"""Reach Live matchmaking playlists: builds the title-storage files the game downloads before
it shows Matchmaking (docs/online_plan.md section 5.4).

    python3 server/reach_live_hoppers.py DATA_DIR --from-client PID   # once, see below
    python3 server/reach_live_hoppers.py DATA_DIR --maps GAME/maps     # writes DATA_DIR/storage/...

The files are BLF files (`_blf`, data chunks, `_eof` with no authentication). The game
downloads them from /storage/title/4d53085b/tracked/<build>/default_hoppers/ and checks each
against the hash listed for it in manifest_001.bin (`onfm`), and each game set and variant
against the hash its parent lists. The hash is SHA-1 over a 34-byte salt followed by the
file. Two inputs come from the operator's game and are not in this repository: the salt
(the start of the executable's resource "00") and the game's built-in network configuration,
which the server must serve back (the lobby needs the file). `--from-client PID` copies both
from a running client on this machine (Linux, /proc/PID/mem) into DATA_DIR/title_key.bin and
DATA_DIR/network_configuration.bin; `--maps` reads the map signatures from the operator's
.map files. docs/online_plan.md section 5.4 has the formats.
"""

import hashlib
import os
import struct
import sys
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from reach_live_lsp import blf, chunk  # noqa: E402

TITLE_ID = 0x4D53085B
BUILD = 11860
HOPPERS_DIR = "storage/title/%08x/tracked/%d/default_hoppers" % (TITLE_ID, BUILD)


SALT_SIZE = 0x22
SALT_ADDRESS = 0x83AB0000       # resource "00" in the loaded executable
NETWORK_CONFIGURATION_ADDRESS = 0x82BD28A8
NETWORK_CONFIGURATION_SIZE = 0x2254
GUEST_BASE = 0x100000000         # where the client maps guest memory


def from_client(pid, data_dir):
    with open("/proc/%d/mem" % pid, "rb", buffering=0) as mem:
        for name, address, size in (("title_key.bin", SALT_ADDRESS, SALT_SIZE),
                                    ("network_configuration.bin", NETWORK_CONFIGURATION_ADDRESS,
                                     NETWORK_CONFIGURATION_SIZE)):
            mem.seek(GUEST_BASE + address)
            data = mem.read(size)
            os.makedirs(data_dir, exist_ok=True)
            with open(os.path.join(data_dir, name), "wb") as f:
                f.write(data)
            print(os.path.join(data_dir, name))


def load_salt(path):
    with open(path, "rb") as f:
        salt = f.read(SALT_SIZE)
    if len(salt) != SALT_SIZE:
        raise ValueError("%s: need the %d-byte salt" % (path, SALT_SIZE))
    return salt


def file_hash(salt, data):
    """The game's hash of a downloaded file (Crypto_Sha1Buffer with its salt flag set)."""
    return hashlib.sha1(salt + data).digest()


def manifest(salt, files):
    """`onfm` v1: u32 count, then {path[0x50] relative to default_hoppers with a leading
    slash, hash of the file} per file. A file the manifest does not list is accepted as it
    is."""
    body = struct.pack(">I", len(files))
    for path, data in sorted(files.items()):
        name = ("/" + path.lstrip("/")).lower().encode()[:0x4F]
        body += name.ljust(0x50, b"\0") + file_hash(salt, data)
    return blf(chunk(b"onfm", 1, 1, body))


def compressed_chunk(fourcc, major, minor, struct_bytes):
    """A chunk whose payload is a bitstream holding a 14-bit byte count and that many bytes:
    the uncompressed size (u32 big-endian) and a zlib stream of the struct."""
    packed = struct.pack(">I", len(struct_bytes)) + zlib.compress(struct_bytes, 9)
    if len(packed) >= 1 << 14:
        raise ValueError("%s too large" % fourcc)
    bits = BitWriter()
    bits.write(len(packed), 14)
    for b in packed:
        bits.write(b, 8)
    return chunk(fourcc, major, minor, bits.data())


class BitWriter:
    """Most significant bit first, as the game's bitstream reads."""

    def __init__(self):
        self.out, self.acc, self.n = bytearray(), 0, 0

    def write(self, value, bits):
        for i in range(bits - 1, -1, -1):
            self.acc = (self.acc << 1) | ((value >> i) & 1)
            self.n += 1
            if self.n == 8:
                self.out.append(self.acc)
                self.acc, self.n = 0, 0

    def data(self):
        if self.n:
            return bytes(self.out) + bytes([self.acc << (8 - self.n)])
        return bytes(self.out)


TABLE_SIZE = 0x8F48     # decoded hopper configuration table (s_hopper_configuration_table)
CATEGORY_SIZE = 0x44
HOPPER_SIZE = 0x458
CATEGORIES_AT = 0x08
HOPPERS_AT = 0x448


def hopper_table(categories, hoppers):
    """The decoded `mhcf` table: u32 hopper count, u32 category count, 16 categories
    {u16 id, char name[32], u16 image, ...} and 32 hoppers {char name[32], game set hash
    at +0x20, u16 id at +0x34, u16 category at +0x36, i32 min/max party at +0x70, ...}."""
    t = bytearray(TABLE_SIZE)
    struct.pack_into(">II", t, 0, len(hoppers), len(categories))
    for i, c in enumerate(categories):
        o = CATEGORIES_AT + i * CATEGORY_SIZE
        name = c["name"].encode()[:31]
        struct.pack_into(">H", t, o, c["id"])
        t[o + 2:o + 2 + len(name)] = name
        struct.pack_into(">H", t, o + 0x22, c.get("image", 0))
    for i, h in enumerate(hoppers):
        o = HOPPERS_AT + i * HOPPER_SIZE
        name = h["name"].encode()[:31]
        t[o:o + len(name)] = name
        t[o + 0x20:o + 0x34] = h.get("game_set_hash", bytes(20))
        struct.pack_into(">HH", t, o + 0x34, h["id"], h["category"])
        # The Playlist screen lists the categories whose bit (1 << +0x38) is set in the
        # hopper list the lobby builds (sub_8227D688), then the hoppers whose category
        # (+0x36) is that bit's number (sub_826F4ED0, sub_826F52A8): +0x38 must be the
        # category id too, or the screen shows nothing.
        t[o + 0x38] = h["category"]
        # Player requirements (sub_82290EA8): none. Experience, games played and rank
        # ranges, access bit -1 = none, account type 2 = any.
        struct.pack_into(">iiiiii", t, o + 0x58, 0, 0, 0, 0, -128, 127)
        struct.pack_into(">ii", t, o + 0x70, h.get("min_party", 1), h.get("max_party", 16))
        struct.pack_into(">iiii", t, o + 0x78, -0x80000000, 0x7FFFFFFF, -1, 2)
        # Voting: options per vote (1-3), +0x9C (0-4), rounds (1-8), +0xA4 (0-8), +0xA8
        # (0-4), +0xAC (1-30), +0xB0 (3 bits). The host checks them (sub_82291260) and
        # sends them to the other players in the "matchmaking-hopper" session parameter,
        # whose decoder (sub_822D9508) drops the whole update for a value out of range.
        struct.pack_into(">iiiiii", t, o + 0x98, 1, 0, 1, 0, 0, 10)
        t[o + 0xB6] = 1
        # Four search stages of 0x94 bytes at +0x134 (sub_822D0760). The stage's ping
        # limit is base + step * increment (+0x28, +0x2C; sub_822D0D68), capped by the
        # network configuration (100 ms); 0 rejects every session found.
        for stage in range(4):
            struct.pack_into(">ii", t, o + 0x134 + stage * 0x94 + 0x28, h.get("max_ping", 200), 0)
        t[o + 0x384] = h["variant_source"]
        if h.get("teams"):
            # Teams: +0x394 / +0x398 min / max team count, then 8 teams of 0x10 bytes at
            # +0x39C {i32 min players (0 = team unused), i32 max players, ...}
            # (sub_8227C1D8, sub_82286418, sub_8227C040, sub_822CFBA8).
            low, high = h.get("team_size", (1, 4))
            struct.pack_into(">ii", t, o + 0x394, h["teams"], h["teams"])
            for team in range(h["teams"]):
                struct.pack_into(">ii", t, o + 0x39C + team * 0x10, low, high)
        else:
            # No teams: a match needs +0x388 to +0x38C players. With a minimum of 1 a lone
            # player's match starts at once.
            t[o + 0x385] = 1
            struct.pack_into(">ii", t, o + 0x388, h.get("min_players", 2), h.get("max_players", 8))
    return bytes(t)


# Hopper +0x384, where a game's variants come from: 0 a game and a map variant file,
# 1 or 2 a game variant file only, 3 none. A multiplayer game only starts with both
# (sub_82280140 needs the map variant parameter), and with none the first game of a
# session crashes reading a null game variant.
GAME_AND_MAP_VARIANT = 0

GAME_SET_SIZE = 0xD404
CREDITS_KEYS = ("credits_multiplier", "winner_multiplier", "top_half_multiplier")
GAME_SET_ENTRY = 0xD4


def game_set(entries, variant_source):
    """The decoded `gset` structure: u32 count, then 0xD4-byte entries (sub_822916A0,
    sub_82291260): +0 i32 weight, +4 / +8 i32 min / max players, +0x14 i32 (at most 1),
    +0x18 i32 (at least 50), +0x2C u8 bit 0: Credits multipliers on, +0x34 / +0x38 / +0x3C
    f32 multipliers of the Credits rate, the winner bonus and the top-half bonus (the
    "matchmaking-game-configuration" session parameter, read by
    GameResults_AwardGameCompletionCookies; zero when off), +0x44 i32 map id, +0x48 game
    variant {u8 used, +0x11 char name[32] (file `<hopper>/<name>_054.bin`), +0x31 hash},
    +0x8D map variant (the same, `<hopper>/map_variants/<name>_031.bin`)."""
    t = bytearray(GAME_SET_SIZE)
    struct.pack_into(">I", t, 0, len(entries))
    for i, e in enumerate(entries):
        o = 4 + i * GAME_SET_ENTRY
        struct.pack_into(">iii", t, o, e.get("weight", 1), e.get("min_players", 1),
                         e.get("max_players", 16))
        struct.pack_into(">ii", t, o + 0x14, 0, 100)
        if e.get("credits"):
            t[o + 0x2C] = 1
            struct.pack_into(">fff", t, o + 0x34, *e["credits"])
        struct.pack_into(">i", t, o + 0x44, e["map"])
        for at, key, used in ((0x48, "game_variant", variant_source in (0, 1, 2)),
                              (0x8D, "map_variant", variant_source == 0)):
            t[o + at] = used
            if used:
                name, digest = e[key]
                t[o + at + 0x11:o + at + 0x11 + len(name)] = name.encode()[:31]
                t[o + at + 0x31:o + at + 0x45] = digest
    return bytes(t)


def hopper_descriptions(hoppers):
    """`mhdf` v3, a bitstream: hopper count - 1 (6 bits), then per hopper its ID (16 bits),
    a flag (1 bit) and a NUL-terminated description (8 bits a character, 256 at most)."""
    bits = BitWriter()
    bits.write(len(hoppers) - 1, 6)
    for h in hoppers:
        bits.write(h["id"], 16)
        bits.write(0, 1)
        for b in h.get("description", "").encode()[:0xFF] + b"\0":
            bits.write(b, 8)
    return blf(chunk(b"mhdf", 3, 1, bits.data()))


MAP_SIGNATURE_AT = 0x36C    # RSA signature in a .map file's header
MAP_SIGNATURE_SIZE = 0x100
MAP_MANIFEST_SIZE = 0x8004  # `mapm`: u32 count, 128 signatures
DLC_MANIFEST_SIZE = 0xF504  # `dlcd`


STATISTICS_SIZE = 0x80E      # `mmhs` v4 payload, copied as it is to 0x833968D4
STATISTICS_ENTRIES = 256
STATISTICS_PATH = "dynamic_pres_hopper_statistics.bin"


def hopper_statistics(counts):
    """The `mmhs` v4 population file the Playlist screen reads (the game's reader is
    sub_820C8DC0): a raw big-endian struct, u32 total players at +0, 8 bytes of unknown
    use, i16 entry count at +0xC and from +0xE up to 256 entries of 8 bytes {u8 type,
    pad, i16 hopper id, u32 players}. A hopper's population is the sum of its type 2
    entries. `counts` is {hopper id: players}."""
    entries = sorted((h, n) for h, n in counts.items() if 0 <= h < 0x8000 and n > 0)
    entries = entries[:STATISTICS_ENTRIES]
    body = struct.pack(">I8xh", sum(n for _, n in entries), len(entries))
    for hopper, players in entries:
        body += struct.pack(">BxhI", 2, hopper, players)
    return blf(chunk(b"mmhs", 4, 1, body.ljust(STATISTICS_SIZE, b"\0")))


def map_signatures(maps_dir):
    """The header signatures of the operator's .map files: a map whose signature the RSA
    manifest does not list marks the console as running modified content."""
    signatures = []
    for name in sorted(os.listdir(maps_dir)):
        if name.endswith(".map"):
            with open(os.path.join(maps_dir, name), "rb") as f:
                f.seek(MAP_SIGNATURE_AT)
                signatures.append(f.read(MAP_SIGNATURE_SIZE))
    return signatures[:(MAP_MANIFEST_SIZE - 4) // MAP_SIGNATURE_SIZE]


def map_manifest(signatures):
    body = struct.pack(">I", len(signatures)) + b"".join(signatures)
    return blf(chunk(b"mapm", 1, 1, body.ljust(MAP_MANIFEST_SIZE, b"\0")))


def map_ids(maps_dir):
    """{lowercase map name: map id} from the operator's maps/info/*.mapinfo (`levl` v7:
    u32 map id, u32, then the English name, UTF-16, 32 characters)."""
    from reach_live_lsp import chunks
    out = {}
    folder = os.path.join(maps_dir, "info")
    for name in sorted(os.listdir(folder)) if os.path.isdir(folder) else []:
        if not name.endswith(".mapinfo"):
            continue
        with open(os.path.join(folder, name), "rb") as f:
            levl = chunks(f.read()).get(b"levl")
        if levl and len(levl[2]) >= 0x48:
            (map_id,) = struct.unpack_from(">I", levl[2], 0)
            title = levl[2][8:0x48].decode("utf-16-be", "ignore").split("\0")[0]
            out.setdefault(title.lower(), map_id)
    return out

# DATA_DIR/playlists.json replaces this. Game and map variants are named after files on the
# server's File Share (save a game type or map in game, then "Upload to File Share").
DEFAULT_PLAYLISTS = {
    "categories": [{"id": 1, "name": "Reach Live"}],
    "playlists": [
        {"id": 101, "category": 1, "name": "Free For All",
         "description": "Every Spartan for themselves.", "max_party": 8, "max_players": 8,
         "games": [{"game_variant": "Slayer", "map": "Sword Base"},
                   {"game_variant": "Slayer", "map": "Zealot"},
                   {"game_variant": "Slayer", "map": "Powerhouse"}]},
        {"id": 102, "category": 1, "name": "Team Slayer",
         "description": "Two teams, kills win.", "max_party": 4, "teams": 2, "team_size": [1, 4],
         "games": [{"game_variant": "Classic Slayer", "map": "Sword Base"},
                   {"game_variant": "Classic Slayer", "map": "Boardwalk"},
                   {"game_variant": "Classic Slayer", "map": "Countdown"}]},
        {"id": 103, "category": 1, "name": "Capture the Flag",
         "description": "Take their flag, bring it home.", "max_party": 4, "teams": 2,
         "team_size": [1, 4],
         "games": [{"game_variant": "Capture the Flag", "map": "Sword Base"},
                   {"game_variant": "Capture the Flag", "map": "Countdown"},
                   {"game_variant": "Capture the Flag", "map": "Boardwalk"}]},
        {"id": 104, "category": 1, "name": "Objective",
         "description": "Hold the skull, hold the hill.", "max_party": 8, "max_players": 8,
         "games": [{"game_variant": "Oddball", "map": "Zealot"},
                   {"game_variant": "Oddball", "map": "Sword Base"},
                   {"game_variant": "King of the Hill", "map": "Sword Base"},
                   {"game_variant": "King of the Hill", "map": "Powerhouse"}]},
    ],
}
# A game's map variant is the map's default one unless "map_variant" names a map saved in
# Forge and uploaded to the File Share. Games whose files are missing are left out. The game
# type names are those the game gives a saved copy of a built-in type (Classic Slayer is a
# team type; Slayer, Oddball and King of the Hill saved with Teams off).


def shared_variants(data_dir):
    """{(file type, lowercase name): chunk payload} of the game variants (`mpvr`, type 6)
    and map variants (`mvar`, type 5) on the server's File Share; of two files with the
    same name (the game saves under the variant's default name), the newest upload."""
    import json
    import reach_live_files as share
    out = {}
    uploads = []
    root = os.path.join(data_dir, "fileshare")
    for owner in os.listdir(root) if os.path.isdir(root) else []:
        try:
            with open(os.path.join(root, owner, "index.json")) as f:
                index = json.load(f)
        except (OSError, ValueError):
            continue
        for entry in index.get("files", []):
            uploads.append((entry.get("uploaded", 0), os.path.join(root, owner,
                                                                   entry["server_id"] + ".bin")))
    for _, path in sorted(uploads, reverse=True):
        try:
            with open(path, "rb") as f:
                chunks = share.parse_chunks(f.read())
        except OSError:
            continue
        header = next((p for c, _, _, p in chunks if c == b"chdr"), None)
        if header is None:
            continue
        title = header[4 + 0x80:4 + 0x180].decode("utf-16-be", "ignore").split("\0")[0]
        for fourcc, major, _, payload in chunks:
            if (fourcc, major) == (b"mpvr", 54):
                out.setdefault((6, title.lower()), payload)
            elif (fourcc, major) == (b"mvar", 31):
                out.setdefault((5, title.lower()), payload)
    return out


def game_variant_file(mpvr):
    """`gvar` v54 holds the bitstream that a saved game variant's `mpvr` v54 chunk holds
    after its hash (+0), padding and u32 size (+0x18)."""
    (size,) = struct.unpack_from(">I", mpvr, 0x18)
    return blf(chunk(b"gvar", 54, 1, mpvr[0x1C:0x1C + size]))


def default_map_variant(salt, map_id, name):
    """The `mvar` v31 chunk payload of a map's default variant, as a custom game uses when
    nobody picked a saved one: no objects of its own, flagged built-in, so the game places
    the map's default objects. Payload: salted SHA-1 of the next two fields, u32 size, the
    bitstream (`sub_824D4980`). Bitstream (`sub_824CFBA0`): the content header
    (`sub_824E9E88`), version 31, map checksum (−1 = not checked), a u32, 9-bit budget
    count, map id, built-in and a second flag, 6 bounds, 2 u32s, 9-bit count, then a
    presence bit for each of the 651 object slots."""
    bits = BitWriter()
    # Content header: type + 1 (5 map variant), size, unique / parent / root / game ids,
    # activity + 1, mode, engine, map id, a signed byte, creator and modifier {time, XUID,
    # 8-bit name (NUL-ended below 16), online bit}, name and description (16-bit characters,
    # NUL-ended below 128). Activity 4 and mode 3 add no further fields.
    for value, width in ((6, 4), (0x7329, 32), (0, 64), (0, 64), (0, 64), (0, 64),
                         (5, 3), (3, 3), (0, 3), (map_id, 32), (0xFF, 8)):
        bits.write(value, width)
    for _ in range(2):
        bits.write(0, 64)
        bits.write(0, 64)
        bits.write(0, 8)   # empty author name
        bits.write(0, 1)
    for text in (name, ""):
        for c in text[:127]:
            bits.write(ord(c), 16)
        bits.write(0, 16)
    bits.write(31, 8)
    bits.write(0xFFFFFFFF, 32)
    bits.write(0, 32)
    bits.write(0, 9)
    bits.write(map_id, 32)
    bits.write(1, 1)       # built-in: no object budget or checksum to match
    bits.write(0, 1)
    for _ in range(3):     # world bounds, unset
        bits.write(0x7F7FFFFF, 32)
        bits.write(0xFF7FFFFF, 32)
    bits.write(0, 32)
    bits.write(0, 32)
    bits.write(0, 9)
    for _ in range(651):
        bits.write(0, 1)
    data = bits.data()
    sized = struct.pack(">I", len(data)) + data
    return hashlib.sha1(salt + sized).digest() + sized


def file_name(title):
    return "".join(c if c.isalnum() else "_" for c in title.lower())[:24]


def build(salt, signatures, network_configuration, playlists, variants, maps):
    """{path relative to default_hoppers: file bytes}"""
    files = {}
    # `netc` v241, raw. The lobby reports the server unavailable while it is missing.
    files["network_configuration_241.bin"] = blf(chunk(b"netc", 241, 1, network_configuration))
    hoppers = [dict(h) for h in playlists["playlists"]]
    for c in playlists["categories"]:
        if not 0 <= c["id"] < 16:  # the Playlist screen keeps categories in a 16-bit mask
            raise SystemExit("category %r: the id must be 0-15" % c["name"])
    for h in hoppers:
        h["variant_source"] = GAME_AND_MAP_VARIANT
        entries = []
        for g in h["games"]:
            map_id = g["map"] if isinstance(g["map"], int) else maps.get(g["map"].lower())
            if map_id is None:
                print("playlist %r: left out %s on %s, no such map in maps/info"
                      % (h["name"], g["game_variant"], g["map"]), file=sys.stderr)
                continue
            if (6, g["game_variant"].lower()) not in variants:
                print("playlist %r: left out %s on %s, no game type named %r on the File Share"
                      % (h["name"], g["game_variant"], g["map"], g["game_variant"]), file=sys.stderr)
                continue
            if g.get("map_variant") and (5, g["map_variant"].lower()) not in variants:
                print("playlist %r: left out %s on %s, no map variant named %r on the File Share"
                      % (h["name"], g["game_variant"], g["map"], g["map_variant"]), file=sys.stderr)
                continue
            entry = {"map": map_id, "weight": g.get("weight", 1)}
            # Credits: "credits_multiplier" (e.g. 2 for double cR) and optionally
            # "winner_multiplier" / "top_half_multiplier", per game or for the playlist.
            mult = [g.get(k, h.get(k)) for k in CREDITS_KEYS]
            if any(m is not None for m in mult):
                entry["credits"] = [1.0 if m is None else float(m) for m in mult]
            # The game type, and the named map variant or else the map's default one.
            map_name = g.get("map_variant") or (g["map"] if isinstance(g["map"], str) else "map %d" % map_id)
            mvar = (variants[(5, g["map_variant"].lower())] if g.get("map_variant")
                    else default_map_variant(salt, map_id, map_name))
            for key, title, folder, version, data in (
                    ("game_variant", g["game_variant"], "", 54,
                     game_variant_file(variants[(6, g["game_variant"].lower())])),
                    ("map_variant", map_name, "map_variants/", 31, blf(chunk(b"mvar", 31, 1, mvar)))):
                name = file_name(title)
                files["%05u/%s%s_%03u.bin" % (h["id"], folder, name, version)] = data
                entry[key] = (name, file_hash(salt, data))
            entries.append(entry)
        if not entries:
            print("playlist %r: left out, none of its games is available" % h["name"],
                  file=sys.stderr)
            h["skip"] = True
            continue
        data = blf(compressed_chunk(b"gset", 15, 1, game_set(entries, h["variant_source"])))
        files["%05u/game_set_015.bin" % h["id"]] = data
        h["game_set_hash"] = file_hash(salt, data)
    hoppers = [h for h in hoppers if not h.get("skip")]
    if not hoppers:
        raise SystemExit("no playlist has a game type on the File Share")
    files["matchmaking_hopper_027.bin"] = blf(
        compressed_chunk(b"mhcf", 27, 1, hopper_table(playlists["categories"], hoppers)))
    files["en/matchmaking_hopper_descriptions_003.bin"] = hopper_descriptions(hoppers)
    files["en/rsa_manifest.bin"] = map_manifest(signatures)
    files["dlc_map_manifest.bin"] = blf(chunk(b"dlcd", 1, 1, bytes(DLC_MANIFEST_SIZE)))
    return files


def write(data_dir, salt, maps_dir, network_configuration):
    playlists = DEFAULT_PLAYLISTS
    config = os.path.join(data_dir, "playlists.json")
    if os.path.isfile(config):
        import json
        with open(config) as f:
            playlists = json.load(f)
    files = build(salt, map_signatures(maps_dir), network_configuration, playlists,
                  shared_variants(data_dir), map_ids(maps_dir))
    # Playlist images (the Playlist screen stretches them to a 2.3:1 box, 512x220 fits):
    # a JPEG named by the playlist's "image" (relative to DATA_DIR) or DATA_DIR/images/<id>.jpg.
    for h in playlists["playlists"]:
        if "%05u/game_set_015.bin" % h["id"] not in files:
            continue
        image = os.path.join(data_dir, h.get("image") or "images/%d.jpg" % h["id"])
        if not os.path.isfile(image):
            if h.get("image"):
                print("playlist %r: no image file %s" % (h["name"], image), file=sys.stderr)
            continue
        with open(image, "rb") as f:
            jpeg = f.read()
        if jpeg[:2] != b"\xff\xd8":
            print("playlist %r: %s is not a JPEG" % (h["name"], image), file=sys.stderr)
            continue
        files["%05u/images/hopper.jpg" % h["id"]] = jpeg
    root = os.path.join(data_dir, "storage", HOPPERS_DIR)
    files["manifest_001.bin"] = manifest(salt, {k: v for k, v in files.items()})
    for path, data in files.items():
        full = os.path.join(root, path)
        os.makedirs(os.path.dirname(full), exist_ok=True)
        with open(full, "wb") as f:
            f.write(data)
    return sorted(files)


if __name__ == "__main__":
    import argparse
    parser = argparse.ArgumentParser(description="Write the Reach Live matchmaking files.")
    parser.add_argument("data_dir", nargs="?", default="reach_live_data")
    parser.add_argument("--maps", help="the game's maps directory (map signatures and ids)")
    parser.add_argument("--from-client", type=int, metavar="PID",
                        help="copy the salt and network configuration from a running client")
    args = parser.parse_args()
    if args.from_client:
        from_client(args.from_client, args.data_dir)
        sys.exit(0)
    inputs = [os.path.join(args.data_dir, n) for n in ("title_key.bin", "network_configuration.bin")]
    missing = [p for p in inputs if not os.path.isfile(p)]
    if missing or not args.maps:
        sys.exit("need %s and --maps: run once with --from-client PID (a running client)"
                 % " and ".join(missing or inputs))
    salt = load_salt(inputs[0])
    with open(inputs[1], "rb") as f:
        network_configuration = f.read()
    for name in write(args.data_dir, salt, args.maps, network_configuration):
        print(name)
