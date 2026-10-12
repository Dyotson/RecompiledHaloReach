"""Reach Live File Share: each player's shared files (map and game variants, films,
screenshots) on the Reach Live server.

reach_live_lsp.handle() passes /gameapi_omaha/Files*.ashx requests here. Files are stored
per share (the owner's XUID) under DATA_DIR/fileshare/<XUID>/, with index.json describing
them. Formats: docs/online_plan.md section 5.3.
"""

import json
import logging
import os
import random
import struct
import threading
import time
import zlib

log = logging.getLogger("reach-live")

ITEM_SIZE = 0x298         # file share item: a content header (chdr) without three ids
METADATA_SIZE = 0x2B0     # s_content_item_metadata, the `chdr` chunk payload after 4 bytes
SLOTS = 24                # file share slots per player
QUOTA = 100 << 20         # bytes per player
MAX_FILE = 32 << 20       # bytes of one shared file (uncompressed: MAX_INFLATED)
MAX_INFLATED = 64 << 20
MAX_PENDING = 8           # unfinished uploads per player
PENDING_AGE = 3600        # seconds before an unfinished upload is forgotten
DISK_CAP = int(os.environ.get("REACH_LIVE_DISK_CAP_MB", "4096")) << 20  # all shared files
_usage = [0.0, 0]


def disk_usage():
    """Bytes of all shared files (rescanned every 30 s)."""
    if time.monotonic() - _usage[0] > 30:
        total = 0
        for root, _, names in os.walk(os.path.join(_data_dir, "fileshare")):
            for name in names:
                try:
                    total += os.path.getsize(os.path.join(root, name))
                except OSError:
                    pass
        _usage[0], _usage[1] = time.monotonic(), total
    return _usage[1]

_lock = threading.Lock()
_data_dir = "reach_live_data"


def configure(data_dir):
    global _data_dir
    _data_dir = data_dir


def share_dir(xuid):
    return os.path.join(_data_dir, "fileshare", "%016X" % xuid)


def load_index(xuid):
    try:
        with open(os.path.join(share_dir(xuid), "index.json")) as f:
            return json.load(f)
    except (OSError, ValueError):
        return {"files": []}


def save_index(xuid, index):
    folder = share_dir(xuid)
    os.makedirs(folder, exist_ok=True)
    path = os.path.join(folder, "index.json")
    with open(path + ".tmp", "w") as f:
        json.dump(index, f, indent=1)
    os.replace(path + ".tmp", path)


def gamertag_of(xuid):
    """The player's gamertag as the rewards sync recorded it, or ""."""
    try:
        with open(os.path.join(_data_dir, "players", "%016X.json" % xuid)) as f:
            return json.load(f).get("gamertag", "")
    except (OSError, ValueError):
        return ""


def item_from_metadata(metadata):
    """The 0x298-byte file share item from a 0x2B0-byte content header: the unique id
    first, then file type and size, then everything after the parent / root / game ids."""
    return metadata[8:16] + metadata[0:8] + metadata[0x28:METADATA_SIZE]


def u64(query, key):
    try:
        value = int(query.get(key, ["0"])[0], 16)
    except ValueError:
        return 0
    return value if 0 <= value < 1 << 64 else 0


# --- Replies -------------------------------------------------------------------------

def counts_record(files):
    """The 0x24-byte `finf` record for a set of files: u32 count per file type 0-8 (the
    UI's per-type counters; slots and space come from the catalog header)."""
    counts = [0] * 9
    for f in files:
        file_type = bytes.fromhex(f["item"])[8]
        if file_type < 9:
            counts[file_type] += 1
    return struct.pack(">9I", *counts)


def catalog_info(query, blf, chunk):
    """FilesGetCatalogInfo: `finf` v1, u16 count, then a 0x24-byte record per share."""
    shares = [s for s in query.get("shareIDs", [""])[0].split(",") if s] or \
        ["%016x" % u64(query, "shareId")]
    records = b"".join(counts_record(load_index(int(share, 16))["files"]) for share in shares)
    payload = struct.pack(">HH", len(shares), 0) + records
    return 200, "application/octet-stream", blf(chunk(b"finf", 1, 1, payload))


def catalog_header(xuid, count):
    """The 0x28-byte `fitm` header (read by sub_827CCE78): +0 u64 share, +8 owner gamertag
    char[16], +0x1C u32 quota in bytes, +0x20 u8 slot count, +0x22 u16 item count, +0x24 u8
    length of a UTF-16 notice that follows the items (none here). Bytes used are the sum of
    the items' sizes (item +0xC)."""
    header = bytearray(0x28)
    struct.pack_into(">Q", header, 0, xuid)
    owner = gamertag_of(xuid).encode("latin-1", "replace")[:15]
    header[8:8 + len(owner)] = owner
    struct.pack_into(">I", header, 0x1C, QUOTA)
    header[0x20] = SLOTS
    struct.pack_into(">H", header, 0x22, count)
    return bytes(header)


def catalog(query, blf, chunk):
    """FilesGetCatalog: `fitm` v4, the header, then items of 0x29C bytes: the 0x298-byte
    item and a u32 extra-data size (0 in listings)."""
    xuid = u64(query, "shareId")
    files = load_index(xuid)["files"]
    items = b"".join(bytes.fromhex(f["item"]) + b"\0\0\0\0" for f in files)
    return 200, "application/octet-stream", blf(chunk(b"fitm", 4, 1,
                                                     catalog_header(xuid, len(files)) + items))


def new_upload(query):
    """FilesNewUpload: reserve a slot. The reply is the upload id as hex text."""
    xuid = u64(query, "shareId") or u64(query, "userId")
    server_id = random.getrandbits(63) | 1
    pending = {"server_id": server_id, "unique_id": "%016X" % u64(query, "uniqueId"),
               "file_type": query.get("fileType", ["0"])[0][:8],
               "size": int(query.get("compressedSize", ["0"])[0] or 0),
               "uncompressed_size": int(query.get("uncompressedSize", ["0"])[0] or 0),
               "created": int(time.time())}
    if not xuid or not 0 < pending["size"] <= MAX_FILE or not 0 <= pending["uncompressed_size"] <= MAX_INFLATED:
        return 400, "text/plain", b"bad upload"
    with _lock:
        index = load_index(xuid)
        index.setdefault("files", [])
        now = int(time.time())
        waiting = index.setdefault("pending", {})
        for sid in [k for k, p in waiting.items() if now - p.get("created", 0) > PENDING_AGE]:
            del waiting[sid]
        while len(waiting) >= MAX_PENDING:  # the oldest unfinished upload makes room
            del waiting[min(waiting, key=lambda k: waiting[k].get("created", 0))]
        if len(index["files"]) >= SLOTS:
            return 400, "text/plain", b"share full"
        if (sum(f.get("size", 0) for f in index["files"]) + pending["size"] > QUOTA
                or disk_usage() + pending["size"] > DISK_CAP):
            return 400, "text/plain", b"quota reached"
        index.setdefault("pending", {})["%016X" % server_id] = pending
        save_index(xuid, index)
    log.info("files %016X new upload %016X (%s)", xuid, server_id, pending)
    return 200, "text/plain", b"%016X" % server_id


def multipart(body):
    """{name: value bytes} of a multipart/form-data body (boundary BUNGIEr0x0rz)."""
    parts = {}
    for part in body.split(b"--BUNGIEr0x0rz"):
        head, sep, value = part.partition(b"\r\n\r\n")
        if not sep:
            continue
        name = head.split(b'name="', 1)[1].split(b'"', 1)[0] if b'name="' in head else b""
        if value.endswith(b"\r\n"):
            value = value[:-2]
        parts[name.decode("latin-1")] = value
    return parts


def parse_chunks(data):
    """[(fourcc, major, minor, payload)] of a BLF file, `_cmp` chunks (zlib) expanded."""
    out, pos = [], 0
    while pos + 12 <= len(data):
        fourcc, size, major, minor = struct.unpack_from(">4sIHH", data, pos)
        if size < 12 or pos + size > len(data):
            break
        payload = data[pos + 12:pos + size]
        if fourcc == b"_cmp":
            for skip in range(0, 8):  # a small header precedes the zlib stream
                try:
                    inflater = zlib.decompressobj()
                    data = inflater.decompress(payload[skip:], MAX_INFLATED)
                    if inflater.unconsumed_tail:
                        break  # a decompression bomb
                    out += parse_chunks(data)
                    break
                except zlib.error:
                    continue
        else:
            out.append((fourcc, major, minor, payload))
        if fourcc == b"_eof":
            break
        pos += size
    return out


def metadata_of(data):
    """The 0x2B0-byte content header of an uploaded file, or None."""
    for fourcc, major, _, payload in parse_chunks(data):
        if fourcc == b"chdr" and len(payload) >= 4 + METADATA_SIZE:
            return payload[4:4 + METADATA_SIZE]
    return None


def file_path(xuid, server_id):
    return os.path.join(share_dir(xuid), "%016X.bin" % server_id)


def upload_data(query, body, headers):
    """FilesUpload: a multipart POST whose part "upload" is the file (a BLF with an
    uncompressed `chdr`); the ids come as quoted HTTP headers (machineid, userid, shareid,
    serverid; startposition when resuming). When the last byte arrives the file joins the
    share."""
    parts = multipart(body)
    fields = {k: [v.decode("latin-1", "replace")] for k, v in parts.items() if len(v) < 64}
    fields.update(query)
    names = {"machineid": "machineId", "userid": "userId", "shareid": "shareId",
             "serverid": "serverId", "startposition": "startPosition"}
    for key, value in headers.items():
        if key in names:
            fields[names[key]] = [value.strip().strip('"')]
    xuid = u64(fields, "shareId") or u64(fields, "userId")
    server_id = u64(fields, "serverId")
    start = int(fields.get("startPosition", ["0"])[0] or 0)
    if not 0 <= start <= MAX_FILE:
        return 400, "text/plain", b"bad position"
    data = max((v for k, v in parts.items() if k not in fields or len(v) >= 64),
               key=len, default=b"")
    log.info("files %016X upload %016X at %d: %d bytes (parts %s)", xuid, server_id, start,
             len(data), {k: len(v) for k, v in parts.items()})
    with _lock:
        index = load_index(xuid)
        pending = index.get("pending", {}).get("%016X" % server_id)
        if pending is None:
            return 404, "text/plain", b"no such upload"
        path = file_path(xuid, server_id)
        if start + len(data) > min(MAX_FILE, pending["size"] or MAX_FILE):
            return 400, "text/plain", b"upload larger than announced"
        if disk_usage() + len(data) > DISK_CAP:
            return 400, "text/plain", b"quota reached"
        _usage[1] += len(data)
        os.makedirs(share_dir(xuid), exist_ok=True)
        with open(path, "r+b" if os.path.exists(path) else "wb") as f:
            f.seek(start)
            f.write(data)
            f.truncate(start + len(data))
        received = start + len(data)
        if received >= pending["size"] > 0:
            finish_upload(xuid, index, pending, path)
        save_index(xuid, index)
    return 200, "text/plain", b"%X" % received


def finish_upload(xuid, index, pending, path):
    with open(path, "rb") as f:
        data = f.read()
    metadata = metadata_of(data)
    if metadata is None:
        log.warning("files %016X upload %016X has no content header", xuid, pending["server_id"])
        metadata = bytes(METADATA_SIZE)
    item = bytearray(item_from_metadata(metadata))
    struct.pack_into(">Q", item, 0, pending["server_id"])  # the share's id for the file
    index["files"].append({"server_id": "%016X" % pending["server_id"],
                           "unique_id": pending["unique_id"], "file_type": pending["file_type"],
                           "size": len(data), "item": bytes(item).hex(),
                           "uploaded": int(time.time())})
    del index["pending"]["%016X" % pending["server_id"]]
    name = metadata[0x80:0x180].decode("utf-16-be", "replace").split("\0")[0]
    log.info("files %016X now shares %r (%d bytes)", xuid, name, len(data))


def upload_progress(query):
    """FilesGetUploadProgress: how much of an upload the server has, as hex text."""
    xuid = u64(query, "shareId") or u64(query, "userId")
    path = file_path(xuid, u64(query, "serverId"))
    received = os.path.getsize(path) if os.path.exists(path) else 0
    return 200, "text/plain", b"%X" % received


def find_file(server_id):
    """(owner XUID, entry) of a shared file, from any share."""
    root = os.path.join(_data_dir, "fileshare")
    for name in os.listdir(root) if os.path.isdir(root) else []:
        xuid = int(name, 16)
        for entry in load_index(xuid)["files"]:
            if int(entry["server_id"], 16) == server_id:
                return xuid, entry
    return None, None


def stage_download(query):
    """FilesStageForDownload: text lines "Size: n", "FullSize: n" (both must be non-zero)
    and "InitialUrl: path", which the client then GETs for the data (sub_8236D380)."""
    server_id = u64(query, "serverId")
    xuid, entry = find_file(server_id)
    if entry is None:
        return 404, "text/plain", b"not found"
    size = os.path.getsize(file_path(xuid, server_id))
    url = "/gameapi_omaha/FilesData.ashx?serverId=%016X" % server_id
    log.info("files stage %016X from %016X: %d bytes", server_id, xuid, size)
    return 200, "text/plain", ("Size: %d\r\nFullSize: %d\r\nInitialUrl: %s\r\n"
                               % (size, size, url)).encode()


def download(query):
    """The stored file from startPosition on (our InitialUrl, also FilesResumeDownload)."""
    server_id = u64(query, "serverId")
    xuid, entry = find_file(server_id)
    if entry is None:
        return 404, "text/plain", b"not found"
    start = max(0, int(query.get("startPosition", ["0"])[0] or 0))
    with open(file_path(xuid, server_id), "rb") as f:
        f.seek(start)
        data = f.read()
    log.info("files download %016X from %016X at %d: %d bytes", server_id, xuid, start, len(data))
    return 200, "application/octet-stream", data


def delete(query):
    """FilesDelete: remove a file from the caller's share."""
    xuid = u64(query, "shareId") or u64(query, "userId")
    server_id = u64(query, "serverId")
    with _lock:
        index = load_index(xuid)
        index["files"] = [f for f in index["files"] if int(f["server_id"], 16) != server_id]
        save_index(xuid, index)
    try:
        os.remove(file_path(xuid, server_id))
    except OSError:
        pass
    return 200, "text/plain", b"0"


def all_files():
    """[(owner XUID, entry)] of every shared file, newest first."""
    root = os.path.join(_data_dir, "fileshare")
    out = []
    for name in os.listdir(root) if os.path.isdir(root) else []:
        try:
            xuid = int(name, 16)
        except ValueError:
            continue
        out += [(xuid, entry) for entry in load_index(xuid)["files"]]
    out.sort(key=lambda pair: pair[1].get("uploaded", 0), reverse=True)
    return out


def matches(query, entry):
    item = bytes.fromhex(entry["item"])
    file_type = query.get("fileType", [""])[0]
    if file_type and file_type.lstrip("-").isdigit() and int(file_type) >= 0 \
            and item[8] != int(file_type):
        return False
    author = query.get("gamertag", [""])[0].lower()
    if author:
        creator = item[0x30:0x40].split(b"\0")[0].decode("latin-1").lower()
        modifier = item[0x54:0x64].split(b"\0")[0].decode("latin-1").lower()
        if author not in (creator, modifier):
            return False
    map_id = query.get("mapId", [""])[0]
    if map_id and map_id.lstrip("-").isdigit() and int(map_id) != -1 \
            and struct.unpack_from(">i", item, 0x14)[0] != int(map_id):
        return False
    return True


PAGE = 50


def search(query, blf, chunk):
    """FilesGetSearch: `fitm` v4 of matching files from every share, 50 per page."""
    found = [entry for _, entry in all_files() if matches(query, entry)]
    page = int(query.get("page", ["0"])[0] or 0)
    found = found[page * PAGE:(page + 1) * PAGE]
    items = b"".join(bytes.fromhex(f["item"]) + b"\0\0\0\0" for f in found)
    return 200, "application/octet-stream", blf(chunk(b"fitm", 4, 1,
                                                     catalog_header(0, len(found)) + items))


def search_count(query, blf, chunk):
    """FilesGetSearchCount: `finf` v1 with one record for the search."""
    found = [entry for _, entry in all_files() if matches(query, entry)]
    return 200, "application/octet-stream", blf(chunk(b"finf", 1, 1, struct.pack(
        ">HH", 1, 0) + counts_record(found)))


def details(query, blf, chunk):
    """FilesGetDetails: `fitm` v4 with the one file (no tags, no extra data)."""
    _, entry = find_file(u64(query, "serverId"))
    if entry is None:
        return 404, "text/plain", b"not found"
    payload = catalog_header(0, 1) + bytes.fromhex(entry["item"]) + b"\0\0\0\0" + bytes(0x24)
    return 200, "application/octet-stream", blf(chunk(b"fitm", 4, 1, payload))


def handle(method, route, query, body, blf, chunk, upload, headers=None):
    """Returns (status, content type, body) or None for an unknown request."""
    if route == "/gameapi_omaha/filesgetcataloginfo.ashx":
        return catalog_info(query, blf, chunk)
    if route == "/gameapi_omaha/filesgetcatalog.ashx":
        return catalog(query, blf, chunk)
    if route == "/gameapi_omaha/filesnewupload.ashx":
        return new_upload(query)
    if route == "/gameapi_omaha/filesupload.ashx":
        return upload_data(query, body, headers or {})
    if route == "/gameapi_omaha/filesgetuploadprogress.ashx":
        return upload_progress(query)
    if route == "/gameapi_omaha/filesstagefordownload.ashx":
        return stage_download(query)
    if route in ("/gameapi_omaha/filesdata.ashx", "/gameapi_omaha/filesresumedownload.ashx"):
        return download(query)
    if route == "/gameapi_omaha/filesdelete.ashx":
        return delete(query)
    if route == "/gameapi_omaha/filesgetsearch.ashx":
        return search(query, blf, chunk)
    if route == "/gameapi_omaha/filesgetsearchcount.ashx":
        return search_count(query, blf, chunk)
    if route == "/gameapi_omaha/filesgetdetails.ashx":
        return details(query, blf, chunk)
    return None
