// reach - REACH_TICKAUDIT=1: which durations no longer fit where the game keeps them at a high
// tick rate (docs/framerate.md, "144 fps").
//
// The game converts durations in seconds to ticks with game_seconds_to_ticks (sub_8214FAE8,
// round(seconds * tick rate)), 427 call sites. About a fifth of them keep the result in 16
// bits and twenty in a byte, some clamped to 255 or 127 first: at 30 ticks a second a byte
// holds 8.5 s, at 144 only 1.77 s. This logs each call site (by its return address) the first
// time its result passes 127, 255 and 32767 ticks, with the seconds asked for and what the
// result would be at 30 ticks; ghidra (or the list in docs/framerate.md) tells what the site
// does with it. Off unless the variable is set; then a few atomic operations per call.

#include <rex/logging.h>
#include <rex/ppc/context.h>
#include <rex/ppc/func.h>

#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>

REX_EXTERN(__imp__sub_8214FAE8);  // game_seconds_to_ticks(f1 seconds) -> r3 ticks

namespace {

bool Enabled() {
  static const bool on = [] {
    const char* v = std::getenv("REACH_TICKAUDIT");
    return v && *v && *v != '0';
  }();
  return on;
}

// Return address -> highest threshold logged (open addressing, never removed).
constexpr size_t kSlots = 2048;
std::array<std::atomic<uint32_t>, kSlots> slot_lr{};
std::array<std::atomic<int>, kSlots> slot_level{};

int Level(int32_t ticks) {
  if (ticks > 32767) return 3;
  if (ticks > 255) return 2;
  if (ticks > 127) return 1;
  return 0;
}

}  // namespace

extern "C" REX_FUNC(sub_8214FAE8) {
  if (!Enabled()) return __imp__sub_8214FAE8(ctx, base);
  const double seconds = ctx.f1.f64;
  const uint32_t lr = uint32_t(ctx.lr);
  __imp__sub_8214FAE8(ctx, base);
  const int32_t ticks = ctx.r3.s32;
  const int level = Level(ticks);
  if (level == 0) return;
  size_t i = (lr >> 2) % kSlots;
  for (size_t n = 0; n < kSlots; ++n, i = (i + 1) % kSlots) {
    uint32_t cur = slot_lr[i].load(std::memory_order_relaxed);
    if (cur == 0 && slot_lr[i].compare_exchange_strong(cur, lr)) cur = lr;
    if (cur != lr) continue;
    int logged = slot_level[i].load(std::memory_order_relaxed);
    while (level > logged) {
      if (slot_level[i].compare_exchange_weak(logged, level)) {
        REXLOG_INFO("TICKAUDIT: call {:#010x} {} s -> {} ticks (> {}; {} at 30 ticks a second)",
                    lr - 4, seconds, ticks, level == 3 ? 32767 : level == 2 ? 255 : 127,
                    int(std::lround(seconds * 30)));
        break;
      }
    }
    return;
  }
}
