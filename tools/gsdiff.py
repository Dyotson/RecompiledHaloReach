#!/usr/bin/env python3
"""Compare two game state dumps (src/debug/sync_diff.cpp: REACH_GSDUMP, REACH_FRAMEDIFF).

Usage:
  gsdiff.py names PID OUT.json
      Names of the game state allocations the thread globals point to (TLS offset ->
      name, e.g. 0x6c "game time globals"), read from the code of a running `reach`.
  gsdiff.py diff GS_A GS_B TLS_A BASE_A BASE_B [--names N.json] [--show K]
      Where two dumps differ. BASE_x: the guest address of each state (the GSDUMP log
      line prints it); words that differ by exactly BASE_A - BASE_B and point into the
      state count as equal. Differences are grouped by the thread-globals slot below them
      and by the data array they are in ('d@t@' header, name 0x24 bytes before it).
      --show K prints the first K differing ranges of each group in hex.
  gsdiff.py objects GS_A GS_B BASE_A BASE_B
      Per object (the 'object' data array: 16-byte headers, +6 size, +0xC address),
      which byte offsets differ.

Two machines in one synchronous game (co-op) should only differ in non-deterministic
state (render caches, particles, effects, HUD); docs/framerate.md has examples.
"""
import bisect
import json
import struct
import sys

ARENA = 0x100000000


def load_pair(path_a, path_b, base_a, base_b):
    a = bytearray(open(path_a, "rb").read())
    b = bytearray(open(path_b, "rb").read())
    delta = (base_a - base_b) & 0xFFFFFFFF
    for o in range(0, min(len(a), len(b)) - 3, 4):
        if a[o:o + 4] != b[o:o + 4]:
            x = struct.unpack_from(">I", a, o)[0]
            y = struct.unpack_from(">I", b, o)[0]
            if (x - y) & 0xFFFFFFFF == delta and base_a <= x < base_a + len(a):
                b[o:o + 4] = a[o:o + 4]
    return a, b


def ranges_of(a, b):
    out, o, n = [], 0, min(len(a), len(b))
    while o < n:
        if a[o] != b[o]:
            s = o
            while o < n and (a[o] != b[o] or (o + 16 < n and a[o:o + 16] != b[o:o + 16])):
                o += 1
            out.append((s, o))
        o += 1
    return out


def data_array(a, name):
    i = a.find(b"d@t@")
    while i != -1:
        h = i - 0x24
        if h >= 0 and a[h:h + len(name) + 1] == name + b"\0":
            return h
        i = a.find(b"d@t@", i + 1)
    return None


def cmd_names(pid, out):
    """li rX, OFF; stwx r3, rTLS, rX after a call that got a name string in r4."""
    with open("/proc/%d/mem" % pid, "rb", buffering=0) as f:
        f.seek(ARENA + 0x82000000)
        d = f.read(0x990000)
    def w(off):
        return struct.unpack_from(">I", d, off)[0]
    names = {}
    for off in range(0xB0000, len(d) - 16, 4):
        v = w(off)
        if v >> 26 != 14 or (v >> 16) & 31 or (v & 0xFFFF) >= 0x400 or v & 3:
            continue
        slot, rt = v & 0xFFFF, (v >> 21) & 31
        for k in range(1, 5):
            x = w(off + 4 * k)
            if not (x >> 26 == 31 and ((x >> 1) & 0x3FF) == 151 and (x >> 21) & 31 == 3
                    and rt in ((x >> 16) & 31, (x >> 11) & 31)):
                continue
            for j in range(1, 16):
                y = w(off - 4 * j)
                if y >> 26 == 14 and (y >> 16) & 31 and (y >> 21) & 31 == 4:
                    ra, lo = (y >> 16) & 31, y & 0xFFFF
                    for jj in range(j + 1, j + 10):
                        z = w(off - 4 * jj)
                        if z >> 26 == 15 and (z >> 21) & 31 == ra:
                            addr = (((z & 0xFFFF) << 16) + (lo - 0x10000 if lo & 0x8000 else lo)) & 0xFFFFFFFF
                            if 0x82000000 <= addr < 0x82100000:
                                s = d[addr - 0x82000000:addr - 0x82000000 + 64].split(b"\0")[0]
                                if len(s) > 3 and all(32 <= c < 127 for c in s):
                                    names.setdefault(hex(slot), s.decode())
                            break
                    break
            break
    json.dump(dict(sorted(names.items(), key=lambda kv: int(kv[0], 16))), open(out, "w"), indent=0)
    print("%d names" % len(names))


def cmd_diff(path_a, path_b, path_tls, base_a, base_b, names, show):
    a, b = load_pair(path_a, path_b, base_a, base_b)
    tls = open(path_tls, "rb").read()
    ptrs = sorted((struct.unpack_from(">I", tls, o)[0], o) for o in range(0, len(tls), 4)
                  if base_a <= struct.unpack_from(">I", tls, o)[0] < base_a + len(a))
    keys = [p[0] for p in ptrs]
    heads, i = [], a.find(b"d@t@")
    while i != -1:
        heads.append(i)
        i = a.find(b"d@t@", i + 1)
    def slot(addr):
        k = bisect.bisect_right(keys, addr) - 1
        if k < 0:
            return "-"
        return "tls+%x %s" % (ptrs[k][1], names.get(hex(ptrs[k][1]), "")[:22])
    def array(off):
        k = bisect.bisect_right(heads, off) - 1
        if k < 0:
            return "-"
        s = max(0, heads[k] - 0x24)
        return "%s@%x" % (bytes(a[s:s + 0x20]).split(b"\0")[0].decode("latin-1", "replace"), base_a + s)
    rs = ranges_of(a, b)
    print("differing ranges: %d, bytes: %d" % (len(rs), sum(e - s for s, e in rs)))
    groups = {}
    for s, e in rs:
        groups.setdefault((slot(base_a + s), array(s)), []).append((s, e))
    for k, g in sorted(groups.items(), key=lambda kv: -sum(e - s for s, e in kv[1])):
        print("%-32s %-36s %6d bytes %4d ranges: %s" % (k[0], k[1][:36], sum(e - s for s, e in g), len(g),
              ", ".join("%x+%x" % (base_a + s, e - s) for s, e in g[:3])))
        for s, e in g[:show]:
            print("     A %x: %s\n     B %s  %s" % (base_a + s, bytes(a[s:min(e, s + 32)]).hex(), " " * 8,
                  bytes(b[s:min(e, s + 32)]).hex()))


def cmd_objects(path_a, path_b, base_a, base_b):
    a, b = load_pair(path_a, path_b, base_a, base_b)
    h = data_array(a, b"object")
    data = struct.unpack_from(">I", a, h + 0x44)[0] - base_a
    objs = []
    for k in range(2048):
        e = data + k * 16
        if e + 16 > len(a):
            break
        salt, size = struct.unpack_from(">H", a, e)[0], struct.unpack_from(">H", a, e + 6)[0]
        addr = struct.unpack_from(">I", a, e + 12)[0]
        if salt and base_a <= addr < base_a + len(a):
            objs.append((addr - base_a, k, size))
    objs.sort()
    ends = [o[0] for o in objs[1:]] + [len(a)]
    for (start, k, size), end in sorted(zip(objs, ends), key=lambda oe: oe[0][1]):
        offs = [o for o in range(start, min(end, start + size)) if a[o] != b[o]]
        if not offs:
            continue
        runs = []
        for o in offs:
            if runs and o - start <= runs[-1][1] + 4:
                runs[-1][1] = o - start
            else:
                runs.append([o - start, o - start])
        print("object %4d tag %08x size %5x: %4d bytes: %s" % (k, struct.unpack_from(">I", a, start)[0], size,
              len(offs), " ".join("%x-%x" % (r[0], r[1]) for r in runs[:12])))


def main():
    args = sys.argv[1:]
    if len(args) >= 3 and args[0] == "names":
        cmd_names(int(args[1]), args[2])
    elif len(args) >= 6 and args[0] == "diff":
        names = json.load(open(args[args.index("--names") + 1])) if "--names" in args else {}
        show = int(args[args.index("--show") + 1]) if "--show" in args else 0
        cmd_diff(args[1], args[2], args[3], int(args[4], 16), int(args[5], 16), names, show)
    elif len(args) == 5 and args[0] == "objects":
        cmd_objects(args[1], args[2], int(args[3], 16), int(args[4], 16))
    else:
        sys.exit(__doc__)


if __name__ == "__main__":
    main()
