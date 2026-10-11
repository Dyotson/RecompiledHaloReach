// reach - trace game state save/load and its integrity check.
//
// Campaign checkpoints copy the 0xA70000-byte game state and protect it with a
// keyed SHA-1 stored at +0x1E708. sub_82429300 verifies that hash and raises
// the fatal "game_state: failed to verify hash for in-place gamestate" error
// when it fails for the live state. With REACH_GSTRACE=1 the verify, the hash
// helper and the load paths that lead to it are logged with their callers and
// results, then forwarded to the recompiled functions. Reverts are logged too: a script's
// game_revert with the script's call stack, the checkpoint state before and after, the
// checkpoint saves and the ticks of the frames after a revert (docs/framerate.md).

#include "../platform/guest_memory.h"

#include <rex/logging.h>
#include <rex/ppc/context.h>
#include <rex/ppc/func.h>

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>

using reach::GuestPtr;

REX_EXTERN(__imp__sub_82429300);  // game_state verify(buffer, allow_unset)
REX_EXTERN(__imp__sub_8242E968);  // keyed SHA-1 of (data, size, mode, expected, out)
REX_EXTERN(__imp__sub_824288F0);  // core load (content "mmiof.bmf" or cache slot)
REX_EXTERN(__imp__sub_824B9A90);  // read a file from the user's save content
REX_EXTERN(__imp__sub_82429858);  // load game state from a compressed buffer
REX_EXTERN(__imp__sub_824285C0);  // checkpoint revert
REX_EXTERN(__imp__sub_82554848);  // load a checkpoint from a cache1:\savegames slot
REX_EXTERN(__imp__sub_826F5660);  // fatal error: (format, argument)
REX_EXTERN(__imp__sub_825CD110);  // hs game_revert: request a revert (reason 12)
REX_EXTERN(__imp__sub_82428468);  // checkpoint save (main game, once per frame when requested)
REX_EXTERN(__imp__sub_82441888);  // main game: map reset after a failed revert (0x8367C0A7)
REX_EXTERN(__imp__sub_820EE150);  // game time update: ticks for this frame
REX_EXTERN(__imp__sub_82428D98);  // post-load fixups
REX_EXTERN(__imp__sub_824DDA88);  // before replacing the game state
REX_EXTERN(__imp__sub_824DDBE8);  // after replacing the game state

namespace {

bool Enabled() {
  static const bool enabled = [] {
    const char* v = std::getenv("REACH_GSTRACE");
    return v && *v && *v != '0';
  }();
  return enabled;
}

uint32_t LoadBE32(const uint8_t* p) {
  uint32_t v;
  std::memcpy(&v, p, 4);
  return __builtin_bswap32(v);
}

std::string Hex(const uint8_t* p, size_t n) {
  static const char kDigits[] = "0123456789abcdef";
  std::string s;
  for (size_t i = 0; i < n; ++i) {
    s += kDigits[p[i] >> 4];
    s += kDigits[p[i] & 15];
  }
  return s;
}

// Globals of the game state system.
constexpr uint32_t kLiveStatePtr = 0x83597B04;
constexpr uint32_t kHeaderStatePtr = 0x83597B14;
constexpr uint32_t kHashOffset = 0x1E708;

}  // namespace

extern "C" REX_FUNC(sub_82429300) {
  if (!Enabled()) return __imp__sub_82429300(ctx, base);
  const uint32_t buffer = ctx.r3.u32, flag = ctx.r4.u32, lr = static_cast<uint32_t>(ctx.lr);
  const uint32_t live = LoadBE32(GuestPtr(base, kLiveStatePtr));
  const uint32_t header = LoadBE32(GuestPtr(base, kHeaderStatePtr));
  const uint32_t hash_at = (buffer ? buffer : header) + kHashOffset;
  const std::string stored = Hex(GuestPtr(base, hash_at), 20);
  __imp__sub_82429300(ctx, base);
  REXLOG_INFO("GSTRACE verify(buffer={:#x}, flag={}) lr={:#x} live={:#x} header={:#x} stored={} -> {}",
              buffer, flag, lr, live, header, stored, ctx.r3.u32 & 0xFF);
}

extern "C" REX_FUNC(sub_8242E968) {
  if (!Enabled()) return __imp__sub_8242E968(ctx, base);
  const uint32_t data = ctx.r3.u32, size = ctx.r4.u32, mode = ctx.r5.u32;
  const uint32_t expected = ctx.r6.u32, out = ctx.r7.u32, lr = static_cast<uint32_t>(ctx.lr);
  const std::string want = expected ? Hex(GuestPtr(base, expected), 20) : "-";
  __imp__sub_8242E968(ctx, base);
  REXLOG_INFO("GSTRACE sha(data={:#x}, size={:#x}, mode={}) lr={:#x} expected={} computed={} -> {}",
              data, size, mode, lr, want, out ? Hex(GuestPtr(base, out), 20) : "-", ctx.r3.u32 & 0xFF);
}

#define REACH_GSTRACE_PASSTHROUGH(addr, what)                                               \
  extern "C" REX_FUNC(sub_##addr) {                                                         \
    if (!Enabled()) return __imp__sub_##addr(ctx, base);                                    \
    const uint32_t r3 = ctx.r3.u32, r4 = ctx.r4.u32, r5 = ctx.r5.u32, r6 = ctx.r6.u32;     \
    const uint32_t lr = static_cast<uint32_t>(ctx.lr);                                      \
    REXLOG_INFO("GSTRACE " what " enter r3={:#x} r4={:#x} r5={:#x} r6={:#x} lr={:#x}", r3, r4, \
                r5, r6, lr);                                                                \
    __imp__sub_##addr(ctx, base);                                                           \
    REXLOG_INFO("GSTRACE " what " -> {:#x}", ctx.r3.u32);                                   \
  }

REACH_GSTRACE_PASSTHROUGH(824288F0, "core_load")
REACH_GSTRACE_PASSTHROUGH(824B9A90, "content_read")
REACH_GSTRACE_PASSTHROUGH(82429858, "load_from_buffer")
REACH_GSTRACE_PASSTHROUGH(82554848, "cache_slot_load")

REACH_GSTRACE_PASSTHROUGH(82428D98, "post_load_fixups")
REACH_GSTRACE_PASSTHROUGH(824DDA88, "pre_replace")
REACH_GSTRACE_PASSTHROUGH(824DDBE8, "post_replace")

namespace {

uint64_t Vblank(uint8_t* base) {
  uint64_t v;
  std::memcpy(&v, GuestPtr(base, 0x8319F800), 8);
  return __builtin_bswap64(v);
}

// The checkpoint state (0x83597AF0: valid, slot, ..., the game time of the last revert at
// +0x10) and the main game's pending requests (0x8367C0A7: map reset, ..., +3 revert, +4
// revert reason).
std::string CheckpointState(uint8_t* base) {
  return Hex(GuestPtr(base, 0x83597AF0), 20) + " req=" + Hex(GuestPtr(base, 0x8367C0A7), 9);
}

// Game time (ticks) and tick rate, from the game time globals (TLS + 0x6C).
uint32_t GameTimeGlobals(uint8_t* base, uint32_t r13) {
  const uint32_t tls = LoadBE32(GuestPtr(base, r13));
  return LoadBE32(GuestPtr(base, tls + 0x6C));
}
uint32_t GameTime(uint8_t* base, uint32_t r13) {
  const uint32_t gt = GameTimeGlobals(base, r13);
  return gt ? LoadBE32(GuestPtr(base, gt + 0xC)) : 0xFFFFFFFFu;
}
int TickRate(uint8_t* base, uint32_t r13) {
  const uint32_t gt = GameTimeGlobals(base, r13);
  return gt ? static_cast<int16_t>((GuestPtr(base, gt + 4)[0] << 8) | GuestPtr(base, gt + 4)[1]) : 0;
}

// hs syntax nodes are 0x18 bytes (salt, function index or constant type, value type, flags,
// next, source offset, value or first argument) in the array at *0x82C40E7C (+0x44
// elements); a datum index's upper 16 bits repeat the node's salt. Scenario scripts:
// *0x82BD6C60 +0x444 count, +0x448 elements of 0x18 bytes (string_id name, type, return
// type, root expression at +8). The hs function table is at 0x82A31040.
uint32_t HsNode(uint8_t* base, uint32_t index) {
  if (index == 0xFFFFFFFFu) return 0;
  const uint32_t arr = LoadBE32(GuestPtr(base, 0x82C40E7C));
  if (!arr) return 0;
  const uint32_t node = LoadBE32(GuestPtr(base, arr + 0x44)) + (index & 0xFFFF) * 0x18;
  const uint8_t* p = GuestPtr(base, node);
  if (((p[0] << 8) | p[1]) != (index >> 16)) return 0;
  return node;
}

void DumpHsTree(uint8_t* base, uint32_t index, int depth, int* budget) {
  while (index != 0xFFFFFFFFu && *budget > 0) {
    const uint32_t node = HsNode(base, index);
    if (!node) return;
    --*budget;
    const uint8_t* p = GuestPtr(base, node);
    REXLOG_INFO("GSTRACE   {:>{}}{:#010x}: {}", "", depth * 2, index, Hex(p, 0x18));
    const uint16_t flags = (p[6] << 8) | p[7];
    if (!(flags & 1) && depth < 12) DumpHsTree(base, LoadBE32(p + 0x10), depth + 1, budget);
    index = LoadBE32(p + 8);
  }
}

// A script thread (0x52C bytes, TLS + 0x34 array) keeps a stack of frames; +0x14 is the
// current frame's offset, a frame's expression is at +0x2C and its parent's offset at +0x3A.
// Logs the frames, the expressions of the three innermost callers and the script whose root
// expression the outermost frame evaluates.
void DumpHsStack(uint8_t* base, uint32_t datum) {
  int16_t off = static_cast<int16_t>((GuestPtr(base, datum + 0x14)[0] << 8) |
                                     GuestPtr(base, datum + 0x14)[1]);
  uint32_t bottom = 0xFFFFFFFFu;
  for (int i = 0; i < 24 && off > 0 && off < 0x52C; ++i) {
    const uint32_t frame = datum + off;
    const uint32_t expr = LoadBE32(GuestPtr(base, frame + 0x2C));
    REXLOG_INFO("GSTRACE  frame {} off={:#x} expr={:#x} [{}]", i, off, expr,
                Hex(GuestPtr(base, frame + 0x28), 0x14));
    bottom = expr;
    const uint8_t* q = GuestPtr(base, frame + 0x3A);
    const int16_t parent = static_cast<int16_t>((q[0] << 8) | q[1]);
    if (i >= 1 && i <= 3) {
      int budget = 60;
      DumpHsTree(base, expr, 0, &budget);
    }
    if (parent == off) break;
    off = parent;
  }
  const uint32_t scen = LoadBE32(GuestPtr(base, 0x82BD6C60));
  if (!scen) return;
  const uint32_t count = LoadBE32(GuestPtr(base, scen + 0x444));
  const uint32_t scripts = LoadBE32(GuestPtr(base, scen + 0x448));
  for (uint32_t i = 0; i < count && i < 4096; ++i) {
    const uint32_t e = scripts + i * 0x18;
    if (LoadBE32(GuestPtr(base, e + 8)) == bottom)
      REXLOG_INFO("GSTRACE  script {} of {} [{}]", i, count, Hex(GuestPtr(base, e), 0x18));
  }
}

int g_frames_after_revert = 0;  // frames whose tick count is logged after a revert

}  // namespace

// A script's game_revert (r4 = the script thread): log the script's call stack.
extern "C" REX_FUNC(sub_825CD110) {
  if (Enabled()) {
    const uint32_t thread = ctx.r4.u32 & 0xFFFF;
    const uint32_t tls = LoadBE32(GuestPtr(base, ctx.r13.u32));
    const uint32_t arr = LoadBE32(GuestPtr(base, tls + 0x34));
    const uint32_t elems = arr ? LoadBE32(GuestPtr(base, arr + 0x44)) : 0;
    const uint32_t datum = elems ? elems + thread * 0x52C : 0;
    REXLOG_INFO("GSTRACE hs game_revert thread={:#x} datum={:#x} time={} vblank={}", thread, datum,
                GameTime(base, ctx.r13.u32), Vblank(base));
    if (datum) DumpHsStack(base, datum);
  }
  __imp__sub_825CD110(ctx, base);
}

extern "C" REX_FUNC(sub_824285C0) {
  if (!Enabled()) return __imp__sub_824285C0(ctx, base);
  const uint32_t r3 = ctx.r3.u32, lr = static_cast<uint32_t>(ctx.lr);
  REXLOG_INFO("GSTRACE checkpoint_revert enter r3={:#x} lr={:#x} state={} time={} vblank={}", r3,
              lr, CheckpointState(base), GameTime(base, ctx.r13.u32), Vblank(base));
  __imp__sub_824285C0(ctx, base);
  REXLOG_INFO("GSTRACE checkpoint_revert done state={} time={}", CheckpointState(base),
              GameTime(base, ctx.r13.u32));
  g_frames_after_revert = 12;
}

extern "C" REX_FUNC(sub_82428468) {
  if (Enabled())
    REXLOG_INFO("GSTRACE checkpoint_save time={} rate={} vblank={}", GameTime(base, ctx.r13.u32),
                TickRate(base, ctx.r13.u32), Vblank(base));
  __imp__sub_82428468(ctx, base);
}

extern "C" REX_FUNC(sub_82441888) {
  if (Enabled())
    REXLOG_INFO("GSTRACE map_reset_after_failed_revert state={}", CheckpointState(base));
  __imp__sub_82441888(ctx, base);
}

// The ticks each frame runs (r5 = where the count goes), for a few frames after a revert,
// and every 10 s the game time against the wall clock.
extern "C" REX_FUNC(sub_820EE150) {
  const uint32_t out = ctx.r5.u32;
  __imp__sub_820EE150(ctx, base);
  if (!Enabled()) return;
  static auto last = std::chrono::steady_clock::now();
  const auto now = std::chrono::steady_clock::now();
  if (now - last >= std::chrono::seconds(10)) {
    last = now;
    REXLOG_INFO("GSTRACE clock time={} rate={} vblank={}", GameTime(base, ctx.r13.u32),
                TickRate(base, ctx.r13.u32), Vblank(base));
  }
  if (g_frames_after_revert > 0) {
    --g_frames_after_revert;
    REXLOG_INFO("GSTRACE frame ticks={} time={} rate={} vblank={}",
                out ? static_cast<int32_t>(LoadBE32(GuestPtr(base, out))) : -1,
                GameTime(base, ctx.r13.u32), TickRate(base, ctx.r13.u32), Vblank(base));
  }
}

// Logs the message of a game fatal error before the game halts.
extern "C" REX_FUNC(sub_826F5660) {
  if (Enabled()) {
    auto str = [&](uint32_t addr) {
      if (addr < 0x80000000u || addr >= 0x90000000u) return std::string("?");
      return std::string(reinterpret_cast<const char*>(GuestPtr(base, addr)), 0, 200);
    };
    REXLOG_ERROR("GSTRACE fatal_error lr={:#x} format=\"{}\" arg=\"{}\"",
                 static_cast<uint32_t>(ctx.lr), str(ctx.r3.u32), str(ctx.r4.u32));
  }
  __imp__sub_826F5660(ctx, base);
}
