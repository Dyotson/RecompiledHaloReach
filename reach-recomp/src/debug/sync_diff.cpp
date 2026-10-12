// reach - tools for synchronous network games (campaign and Firefight co-op), where every
// machine runs the same simulation tick for tick: game state dumps to compare two machines,
// and experiments with the tick and frame rate (docs/framerate.md, "60/30 co-op: what
// diverges"). Everything is off unless its environment variable is set.
//
// REACH_GSDUMP=DIR and REACH_GSDUMP_TICKS=T1,T2,...: in synchronous network games (and local
// ones after the main menu with REACH_GSDUMP_LOCAL=1), right after the tick that brings the
// game time to Ti (sub_821224D8, one game tick, called by sub_821F4DF0 once per tick), the
// live game state (0xA70000 bytes at *0x83597B04) goes to DIR/gs_Ti.bin and the thread's
// globals block (*r13, 0x800 bytes of pointers into it) to DIR/tls_Ti.bin. Run both
// machines with the same ticks and compare the files with tools/gsdiff.py.
//
// REACH_FRAMEDIFF, REACH_SYNC_TICKS30, REACH_SYNC_FPS30 and the reach_in_tick symbol: below.
//
// REACH_CINEREC=FILE: whenever FILE.trigger appears (it is deleted again), the next
// REACH_CINEREC_FRAMES (default 600) main-game frame updates (sub_82440410) each append a record
// to FILE: the guest frames handed to VdSwap so far, the game time, the frame's ticks, the
// vblank counter, blocks of thread globals (REACH_CINEREC_BLOCKS="off:size,...", default the
// cinematic, observer, director, scripted camera and game time globals) and the positions of
// the cinematic's objects. While it records, each view's cinematic clip planes are logged.
// tools/cinerec.py reads the file. Used to find what alternated between frames in cinematics
// at frame_rate = 60: the shot camera moved at 30 Hz (docs/rendering.md).

#include "../hooks/frame_rate.h"
#include "../platform/guest_memory.h"
#include "frame_stats.h"

#include <rex/logging.h>
#include <rex/ppc/context.h>
#include <rex/ppc/func.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <set>
#include <string>
#include <thread>
#include <utility>
#include <vector>

using reach::GuestPtr;

REX_EXTERN(__imp__sub_821224D8);  // one game tick
REX_EXTERN(__imp__sub_8247B530);  // game time init: tick rate from the game globals (+0x1C)
REX_EXTERN(__imp__sub_82440410);  // the main game's per-frame update (runs the frame's ticks)
REX_EXTERN(__imp__sub_820E1D18);  // Main_FrameLoopStep: waits for the frame's vblanks, runs a frame
REX_EXTERN(__imp__sub_82476568);  // game start: copies the game options into the game globals (+0x10)

namespace {

constexpr uint32_t kLiveStatePtr = 0x83597B04;
constexpr uint32_t kStateSize = 0xA70000;
constexpr uint32_t kTlsSize = 0x800;

uint32_t LoadBE32(const uint8_t* p) {
  uint32_t v;
  std::memcpy(&v, p, 4);
  return __builtin_bswap32(v);
}

struct Config {
  std::string dir;
  std::set<uint32_t> ticks;
};

const Config& GetConfig() {
  static const Config config = [] {
    Config c;
    const char* dir = std::getenv("REACH_GSDUMP");
    const char* ticks = std::getenv("REACH_GSDUMP_TICKS");
    if (!dir || !*dir || !ticks) return c;
    c.dir = dir;
    for (const char* p = ticks; *p;) {
      char* end = nullptr;
      const unsigned long t = std::strtoul(p, &end, 10);
      if (end == p) break;
      c.ticks.insert(uint32_t(t));
      p = *end ? end + 1 : end;
    }
    return c;
  }();
  return config;
}

bool Write(const std::string& path, const uint8_t* data, size_t size) {
  FILE* f = std::fopen(path.c_str(), "wb");
  if (!f) return false;
  const bool ok = std::fwrite(data, 1, size, f) == size;
  std::fclose(f);
  return ok;
}

}  // namespace

namespace {
int ticks_this_frame = 0;  // main thread only
int games_started = 0;     // game time initializations (sub_8247B530): the main menu is the first
}  // namespace

// 1 while the main thread runs a game tick: a debugger can tell writes from inside a tick from
// writes outside (gdb: watch ... if reach_in_tick == 0).
extern "C" volatile int reach_in_tick;
volatile int reach_in_tick = 0;

extern "C" REX_FUNC(sub_821224D8) {
  reach_in_tick = 1;
  __imp__sub_821224D8(ctx, base);
  reach_in_tick = 0;
  ++ticks_this_frame;
  const Config& config = GetConfig();
  if (config.ticks.empty()) return;
  const uint32_t tls = LoadBE32(GuestPtr(base, ctx.r13.u32));
  const uint32_t game_time = LoadBE32(GuestPtr(base, tls + 0x6C));
  if (!game_time) return;
  const uint32_t now = LoadBE32(GuestPtr(base, game_time + 0xC));
  if (!config.ticks.count(now)) return;
  // Only synchronous network games (game globals +0x14: 2 sync-client, 3 sync-server); the
  // main menu runs a game too.
  const uint32_t globals = LoadBE32(GuestPtr(base, tls + 0x3C));
  const uint32_t network = globals ? LoadBE32(GuestPtr(base, globals + 0x14)) : 0;
  static const bool local = [] {
    const char* v = std::getenv("REACH_GSDUMP_LOCAL");
    return v && *v && *v != '0';
  }();
  // REACH_GSDUMP_LOCAL=1: also local games after the main menu (the first game).
  if (network != 2 && network != 3 && !(local && network == 1 && games_started >= 2)) return;
  const uint32_t state = LoadBE32(GuestPtr(base, kLiveStatePtr));
  const std::string suffix = std::to_string(now) + ".bin";
  const bool ok = state && Write(config.dir + "/gs_" + suffix, GuestPtr(base, state), kStateSize) &&
                  Write(config.dir + "/tls_" + suffix, GuestPtr(base, tls), kTlsSize);
  const uint8_t* gt = GuestPtr(base, game_time);
  REXLOG_INFO("GSDUMP tick {} (network type {}, {} ticks/s) state {:#x} tls {:#x}: {}", now, network,
              int16_t(gt[4] << 8 | gt[5]), state, tls, ok ? "written" : "FAILED");
}

REX_EXTERN(__imp__sub_820D4A98);  // render: the cinematic's user clip planes for this view

namespace {

std::atomic<int> cinerec_left{0};  // frames still to record (REACH_CINEREC)

// REACH_CINEREC (see the top of the file), called after each main-game frame update.
void CineRecord(PPCContext& ctx, uint8_t* base) {
  struct Config {
    std::string path, trigger;
    int frames = 600;
    std::vector<std::pair<uint32_t, uint32_t>> blocks;  // thread-globals offset, size
  };
  static const Config config = [] {
    Config c;
    const char* path = std::getenv("REACH_CINEREC");
    if (!path || !*path) return c;
    c.path = path;
    c.trigger = c.path + ".trigger";
    if (const char* v = std::getenv("REACH_CINEREC_FRAMES")) c.frames = std::max(1, std::atoi(v));
    // cinematic (0x50), cinematic globals (0x84), its non-deterministic part (0x1F4), observer
    // (0x354), director (0xC8), scripted camera (0x44), game time (0x6C)
    std::string spec = "0x50:0x1a4c,0x84:0x40,0x1F4:0x10,0x354:0x1018,0xC8:0x690,0x44:0xF0,0x6C:0x20";
    if (const char* v = std::getenv("REACH_CINEREC_BLOCKS")) spec = v;
    for (const char* p = spec.c_str(); *p;) {
      char* end = nullptr;
      const unsigned long off = std::strtoul(p, &end, 0);
      if (end == p || *end != ':') break;
      p = end + 1;
      const unsigned long size = std::strtoul(p, &end, 0);
      if (end == p) break;
      c.blocks.emplace_back(uint32_t(off), uint32_t(size));
      p = *end ? end + 1 : end;
    }
    return c;
  }();
  if (config.path.empty()) return;
  static int polls = 0;
  static FILE* out = nullptr;
  int left = cinerec_left.load();
  if (left == 0) {
    if (++polls % 10 != 0 || std::remove(config.trigger.c_str()) != 0) return;
    left = config.frames;
    if (!out) out = std::fopen(config.path.c_str(), "ab");
    REXLOG_INFO("CINEREC: recording {} frames from guest frame {}", left,
                reach::perf::GuestFrameCount());
  }
  if (!out) return;
  const uint32_t tls = LoadBE32(GuestPtr(base, ctx.r13.u32));
  const uint32_t game_time = LoadBE32(GuestPtr(base, tls + 0x6C));
  uint64_t vblank;
  std::memcpy(&vblank, GuestPtr(base, 0x8319F800), 8);
  const uint32_t header[4] = {0x43455243u /* "CREC" */, reach::perf::GuestFrameCount(),
                              game_time ? LoadBE32(GuestPtr(base, game_time + 0xC)) : 0,
                              uint32_t(ticks_this_frame)};
  std::fwrite(header, sizeof(header), 1, out);
  vblank = __builtin_bswap64(vblank);
  std::fwrite(&vblank, 8, 1, out);
  const uint32_t count = uint32_t(config.blocks.size()) + 1;
  std::fwrite(&count, 4, 1, out);
  for (const auto& [off, size] : config.blocks) {
    const uint32_t addr = LoadBE32(GuestPtr(base, tls + off));
    const uint32_t meta[3] = {off, addr ? size : 0, addr};
    std::fwrite(meta, sizeof(meta), 1, out);
    if (addr) std::fwrite(GuestPtr(base, addr), 1, size, out);
  }
  // Pseudo block 0xFFFF0000: the cinematic's objects (cinematic globals +0x14 count, +0x1C
  // entries of 12 bytes starting with the object handle), each as the handle and the object's
  // bounding sphere centre (object +0x20, 3 floats), in guest byte order.
  uint8_t objects[32 * 16] = {};
  uint32_t used = 0;
  const uint32_t cine = LoadBE32(GuestPtr(base, tls + 0x50));
  const uint32_t data = LoadBE32(GuestPtr(base, tls + 0x18));
  if (cine && data) {
    const uint32_t elements = LoadBE32(GuestPtr(base, data + 0x44));
    const int n = std::min<int>(32, int32_t(LoadBE32(GuestPtr(base, cine + 0x14))));
    for (int i = 0; i < n; ++i) {
      const uint32_t handle = LoadBE32(GuestPtr(base, cine + 0x1C + 12 * i));
      if (handle == 0xFFFFFFFFu) continue;
      const uint32_t object = LoadBE32(GuestPtr(base, elements + (handle & 0xFFFF) * 0x10 + 0xC));
      std::memcpy(objects + used, GuestPtr(base, cine + 0x1C + 12 * i), 4);
      if (object) std::memcpy(objects + used + 4, GuestPtr(base, object + 0x20), 12);
      used += 16;
    }
  }
  const uint32_t meta[3] = {0xFFFF0000u, used, 0};
  std::fwrite(meta, sizeof(meta), 1, out);
  std::fwrite(objects, 1, used, out);
  cinerec_left.store(--left);
  if (left == 0) {
    std::fflush(out);
    REXLOG_INFO("CINEREC: done at guest frame {}", reach::perf::GuestFrameCount());
  }
}

}  // namespace

// While REACH_CINEREC records: each time a view sets up the cinematic's user clip planes
// (sub_820D4A98: for each of the shot's clip planes, active while start <= game time < end),
// the thread, the game time it sees and each plane's window and result.
extern "C" REX_FUNC(sub_820D4A98) {
  __imp__sub_820D4A98(ctx, base);
  if (cinerec_left.load() == 0) return;
  const uint32_t tls = LoadBE32(GuestPtr(base, ctx.r13.u32));
  const uint32_t cine = LoadBE32(GuestPtr(base, tls + 0x50));
  const uint32_t game_time = LoadBE32(GuestPtr(base, tls + 0x6C));
  const uint32_t now = game_time ? LoadBE32(GuestPtr(base, game_time + 0xC)) : 0;
  const int count = cine ? int32_t(LoadBE32(GuestPtr(base, cine + 0x9C8))) : 0;
  std::string planes;
  for (int i = 0; i < count && i < 8; ++i) {
    const uint8_t* e = GuestPtr(base, cine + 0x9CC + 0x84 * i);
    const uint8_t* out = GuestPtr(base, 0x82B7F838 + 16 * i);
    planes += " [" + std::to_string(int32_t(LoadBE32(e + 0x18))) + "," +
              std::to_string(int32_t(LoadBE32(e + 0x1C))) + ") n=" +
              std::to_string(int32_t(LoadBE32(e + 0x20))) + " out=" +
              (LoadBE32(out + 4) | LoadBE32(out + 8) | LoadBE32(out + 12) ? "on" : "off");
  }
  REXLOG_INFO("CINEREC clip: tls {:#x} time {} frames {} clips {}{}", tls, now,
              reach::perf::GuestFrameCount(), count, planes);
}

// REACH_FRAMEDIFF=DIR and REACH_FRAMEDIFF_TICKS=T1,...: in a game that runs fewer ticks than
// frames (frame_rate = 60 with REACH_SYNC_TICKS30), the live game state after the frame that
// ran tick Ti (DIR/fd_Ti_a.bin) and after the next frame, which ran no tick (fd_Ti_b.bin), with
// the thread globals (fd_Ti_tls.bin). What differs was written outside the game ticks.
extern "C" REX_FUNC(sub_82440410) {
  ticks_this_frame = 0;
  __imp__sub_82440410(ctx, base);
  CineRecord(ctx, base);
  static const Config config = [] {
    Config c;
    const char* dir = std::getenv("REACH_FRAMEDIFF");
    const char* ticks = std::getenv("REACH_FRAMEDIFF_TICKS");
    if (!dir || !*dir || !ticks) return c;
    c.dir = dir;
    for (const char* p = ticks; *p;) {
      char* end = nullptr;
      const unsigned long t = std::strtoul(p, &end, 10);
      if (end == p) break;
      c.ticks.insert(uint32_t(t));
      p = *end ? end + 1 : end;
    }
    return c;
  }();
  if (config.ticks.empty()) return;
  static uint32_t pending = 0xFFFFFFFFu;
  const uint32_t tls = LoadBE32(GuestPtr(base, ctx.r13.u32));
  const uint32_t game_time = LoadBE32(GuestPtr(base, tls + 0x6C));
  if (!game_time) return;
  const uint32_t now = LoadBE32(GuestPtr(base, game_time + 0xC));
  const uint32_t state = LoadBE32(GuestPtr(base, kLiveStatePtr));
  if (!state) return;
  const std::string prefix = config.dir + "/fd_" + std::to_string(now);
  static std::set<uint32_t> done;  // each tick once
  if (done.count(now)) return;
  // Only from the second game on (the first is the main menu).
  if (games_started < 2) return;
  if (pending == now && ticks_this_frame == 0) {
    Write(prefix + "_b.bin", GuestPtr(base, state), kStateSize);
    REXLOG_INFO("FRAMEDIFF tick {}: after a frame without a tick", now);
    pending = 0xFFFFFFFFu;
    done.insert(now);
  } else if (ticks_this_frame > 0) {
    pending = 0xFFFFFFFFu;
    if (config.ticks.count(now)) {
      Write(prefix + "_a.bin", GuestPtr(base, state), kStateSize);
      Write(prefix + "_tls.bin", GuestPtr(base, tls), kTlsSize);
      REXLOG_INFO("FRAMEDIFF tick {}: after the frame that ran it ({} tick(s)), state {:#x}", now,
                  ticks_this_frame, state);
      pending = now;
    }
  }
}

// Experiment (docs/framerate.md, "60/30 co-op"): REACH_SYNC_TICKS30=1 runs synchronous games
// (network type 2 sync-client, 3 sync-server: game globals +0x14) at 30 ticks a second
// whatever the frame rate; REACH_SYNC_TICKS30=2 also local games (type 1). The game time
// globals take the tick rate from the game globals (+0x1C, a copy of the game options' tick
// rate) when a game starts; sub_82476568 below patches that copy as soon as it is made.
extern "C" REX_FUNC(sub_8247B530) {
  ++games_started;
  static const int force = [] {
    const char* v = std::getenv("REACH_SYNC_TICKS30");
    return v && *v ? std::atoi(v) : 0;
  }();
  if (force) {
    const uint32_t tls = LoadBE32(GuestPtr(base, ctx.r13.u32));
    uint8_t* globals = GuestPtr(base, LoadBE32(GuestPtr(base, tls + 0x3C)));
    const uint32_t network = LoadBE32(globals + 0x14);
    const int rate = int16_t(globals[0x1C] << 8 | globals[0x1D]);
    if ((network == 2 || network == 3 || (force == 2 && network == 1)) && rate != 30) {
      REXLOG_INFO("frame_rate: synchronous game (network type {}): 30 ticks a second instead of {}",
                  network, rate);
      globals[0x1C] = 0, globals[0x1D] = 30;
    }
  }
  __imp__sub_8247B530(ctx, base);
}

// Experiment: REACH_SYNC_FPS30=1 lets synchronous games (network type 2 or 3) run at most one
// frame every 4 guest vblanks: 30 fps at frame_rate = 60 (120 Hz vblank), so with
// REACH_SYNC_TICKS30 every frame runs exactly one tick, as on a 30 fps machine.
extern "C" REX_FUNC(sub_820E1D18) {
  static const bool on = [] {
    const char* v = std::getenv("REACH_SYNC_FPS30");
    return v && *v && *v != '0';
  }();
  if (on) {
    static uint64_t last = 0;
    auto vblank = [&] {
      uint64_t v;
      std::memcpy(&v, GuestPtr(base, 0x8319F800), 8);
      return __builtin_bswap64(v);
    };
    const uint32_t tls = LoadBE32(GuestPtr(base, ctx.r13.u32));
    const uint32_t globals = tls ? LoadBE32(GuestPtr(base, tls + 0x3C)) : 0;
    const uint32_t network = globals ? LoadBE32(GuestPtr(base, globals + 0x14)) : 0;
    uint64_t now = vblank();
    // One frame per 30 Hz tick: 4 vblanks at 120 Hz, 2 (the game's own pace) at 60 Hz.
    const uint64_t frame = uint64_t(reach::TickRate()) * 2 / 30;
    if ((network == 2 || network == 3) && frame > 2) {
      while (now < last + frame) {
        std::this_thread::sleep_for(std::chrono::microseconds(300));
        now = vblank();
      }
    }
    last = now;
  }
  __imp__sub_820E1D18(ctx, base);
}

// With REACH_SYNC_TICKS30, also right after the game options are copied into the game globals
// at game start (sub_82476568), before anything else reads the tick rate there.
extern "C" REX_FUNC(sub_82476568) {
  __imp__sub_82476568(ctx, base);
  const char* v = std::getenv("REACH_SYNC_TICKS30");
  if (!v || !*v || *v == '0') return;
  const uint32_t tls = LoadBE32(GuestPtr(base, ctx.r13.u32));
  uint8_t* globals = GuestPtr(base, LoadBE32(GuestPtr(base, tls + 0x3C)));
  const uint32_t network = LoadBE32(globals + 0x14);
  const int rate = int16_t(globals[0x1C] << 8 | globals[0x1D]);
  if ((network == 2 || network == 3) && rate != 30) {
    REXLOG_INFO("frame_rate: game start, synchronous game: options tick rate {} -> 30", rate);
    globals[0x1C] = 0, globals[0x1D] = 30;
  }
}
