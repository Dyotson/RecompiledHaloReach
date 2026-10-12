#!/usr/bin/env python3
"""Read a REACH_CINEREC recording (src/debug/sync_diff.cpp): per main-game frame, the guest
frames presented so far, the game time, the frame's ticks and blocks of thread globals.

  cinerec.py FILE summary             frames, game times, ticks per frame
  cinerec.py FILE flips [MIN]         32-bit words that go back to their value of two frames
                                      before (A B A) in at least MIN frames (default 5), by block
  cinerec.py FILE word OFF HEX [N]    one word (block at thread-globals offset OFF, byte offset
                                      HEX) on every frame, as hex and float (N words)
  cinerec.py FILE steps [FIRST N]     per frame: how far the camera (observer +0x10) and each of the
                                      cinematic's objects (pseudo block 0xFFFF0000) moved since the
                                      frame before; 0.00 on every other line means 30 Hz motion
  cinerec.py FILE view [FIRST N]      per frame: the camera's move and turn, and how each object's
                                      angle off the camera's forward changed (judder against a
                                      turning camera)
"""
import struct
import sys


def records(path):
    data = open(path, "rb").read()
    pos = 0
    while pos + 28 <= len(data):
        magic, swaps, time, ticks = struct.unpack_from("<4I", data, pos)
        if magic != 0x43455243:
            raise SystemExit(f"bad record at {pos}")
        (vblank,) = struct.unpack_from("<Q", data, pos + 16)
        (count,) = struct.unpack_from("<I", data, pos + 24)
        pos += 28
        blocks = {}
        for _ in range(count):
            off, size, addr = struct.unpack_from("<3I", data, pos)
            pos += 12
            blocks[off] = (addr, data[pos:pos + size])
            pos += size
        yield {"swaps": swaps, "time": time, "ticks": ticks, "vblank": vblank, "blocks": blocks}


def word(raw, at):
    return struct.unpack_from(">I", raw, at)[0] if at + 4 <= len(raw) else None


def as_float(v):
    return struct.unpack(">f", struct.pack(">I", v))[0]


def main():
    path, cmd = sys.argv[1], sys.argv[2]
    recs = list(records(path))
    if cmd == "summary":
        for r in recs:
            print(f"swaps={r['swaps']} time={r['time']} ticks={r['ticks']} vblank={r['vblank']}")
    elif cmd == "flips":
        minimum = int(sys.argv[3]) if len(sys.argv) > 3 else 5
        for off in recs[0]["blocks"]:
            size = len(recs[0]["blocks"][off][1])
            hits = []
            for at in range(0, size - 3, 4):
                vals = [word(r["blocks"][off][1], at) for r in recs]
                flips = sum(1 for k in range(2, len(vals))
                            if vals[k] == vals[k - 2] != vals[k - 1])
                if flips >= minimum:
                    hits.append((flips, at, len(set(vals))))
            for flips, at, distinct in sorted(hits, reverse=True)[:40]:
                print(f"block {off:#x} +{at:#06x}: {flips} flips, {distinct} distinct values")
    elif cmd == "steps":
        first = int(sys.argv[3]) if len(sys.argv) > 3 else 0
        n = int(sys.argv[4]) if len(sys.argv) > 4 else 40
        def pos(raw, at):
            return struct.unpack_from(">3f", raw, at)
        def dist(a, b):
            return sum((x - y) ** 2 for x, y in zip(a, b)) ** 0.5
        prev = None
        for r in recs[first:first + n]:
            cam = pos(r["blocks"][0x354][1], 0x10)
            objs = r["blocks"].get(0xFFFF0000, (0, b""))[1]
            cur = {struct.unpack_from(">I", objs, i)[0]: pos(objs, i + 4) for i in range(0, len(objs), 16)}
            if prev:
                moves = " ".join(f"{dist(cur[h], prev[1][h]):5.2f}" for h in list(cur)[:12] if h in prev[1])
                print(f"time={r['time']} camera {dist(cam, prev[0]):5.2f} objects {moves}")
            prev = (cam, cur)
    elif cmd == "view":
        # Per main-loop frame: the frame's ticks, how far the camera moved and turned (observer
        # +0x10 position, +0x1C forward), and for each recorded object the change of its
        # direction as seen from the camera, relative to the camera's forward vector (degrees):
        # with fewer ticks than frames, an object that moves only on ticks while the camera
        # turns every frame shows up as an alternating step.
        import math
        first = int(sys.argv[3]) if len(sys.argv) > 3 else 0
        n = int(sys.argv[4]) if len(sys.argv) > 4 else 40
        def vec(raw, at):
            return struct.unpack_from(">3f", raw, at)
        def sub(a, b): return tuple(x - y for x, y in zip(a, b))
        def norm(a):
            length = math.sqrt(sum(x * x for x in a)) or 1.0
            return tuple(x / length for x in a)
        def angle(a, b):
            return math.degrees(math.acos(max(-1.0, min(1.0, sum(x * y for x, y in zip(norm(a), norm(b)))))))
        prev = None
        for r in recs[first:first + n]:
            obs = r["blocks"][0x354][1]
            cam, fwd = vec(obs, 0x10), vec(obs, 0x1C)
            objs = r["blocks"].get(0xFFFF0000, (0, b""))[1]
            cur = {struct.unpack_from(">I", objs, i)[0]: vec(objs, i + 4) for i in range(0, len(objs), 16)}
            # yaw/pitch of each object relative to the camera's forward
            rel = {h: angle(sub(p, cam), fwd) for h, p in cur.items()}
            if prev:
                moved = math.dist(cam, prev[0]); turned = angle(fwd, prev[1])
                cols = " ".join(f"{rel[h] - prev[2][h]:+6.3f}" for h in list(rel)[:6] if h in prev[2])
                print(f"time={r['time']} ticks={r['ticks']} cam moved {moved:5.3f} turned {turned:6.3f} deg  objects (change of angle off forward, deg) {cols}")
            prev = (cam, fwd, rel)
    elif cmd == "word":
        off, at = int(sys.argv[3], 0), int(sys.argv[4], 16)
        n = int(sys.argv[5]) if len(sys.argv) > 5 else 1
        for r in recs:
            raw = r["blocks"][off][1]
            vals = [word(raw, at + 4 * i) for i in range(n)]
            text = " ".join(f"{v:08x}({as_float(v):.4g})" for v in vals if v is not None)
            print(f"swaps={r['swaps']} time={r['time']} ticks={r['ticks']} {text}")


if __name__ == "__main__":
    main()
