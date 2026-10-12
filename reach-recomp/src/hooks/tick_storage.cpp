// reach - durations kept in ticks in a byte or 16 bits, above 30 ticks a second
// (docs/framerate.md, "120 and 144 fps").
//
// The game converts durations in seconds to ticks with game_seconds_to_ticks (sub_8214FAE8,
// round(seconds * tick rate)), 427 call sites. About 150 keep the result in 16 bits and 27 in a
// byte (7 of them clamp it to 255 or 127 first): at 30 ticks a second a byte holds 8.5 s, at 60
// 4.25 s, at 72 3.5 s, at 144 1.77 s. A byte site without the clamp wraps: the 2 s hold of a
// multiplayer client's predicted grenade count (sub_82394068) is 288 ticks at 144, stored as 32
// (0.22 s). Above 30 ticks a second this hook saturates the result at the 20 byte sites that
// store it unclamped (255) and at the 16-bit sites (32767), so a duration that no longer fits
// comes out as long as the field allows instead of wrapping to a short one. At 30 ticks a second
// nothing changes. Many byte tick counters are also signed and compared as such (unit actions,
// 0x7F), which no hook here can widen; that is why the tick rate stays at 72 or below
// (frame_rate.cpp).
//
// REACH_TICKAUDIT=1 logs each call site (its return address) the first time its result passes
// 127, 255 and 32767 ticks, with the seconds asked for and the result at 30 ticks a second, and
// each saturation.

#include "../platform/guest_memory.h"

#include <rex/logging.h>
#include <rex/ppc/context.h>
#include <rex/ppc/func.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>

REX_EXTERN(__imp__sub_8214FAE8);  // game_seconds_to_ticks(f1 seconds) -> r3 ticks

namespace {

// Return addresses of the calls whose result goes straight into a byte (stb r3) without a clamp.
constexpr uint32_t kByteSites[] = {
    0x820C1ECC, 0x8222AE58, 0x8222AE64, 0x823940C8, 0x823B13E8, 0x823BF9B0, 0x823BF9E0,
    0x823BFA10, 0x823BFA38, 0x823BFB3C, 0x823F135C, 0x823F15C4, 0x823FC800, 0x823FC818,
    0x823FC838, 0x823FC858, 0x823FD4F0, 0x823FD508, 0x8266DDEC, 0x826FC110,
};

// ... into 16 bits (sth, extsh or a 16-bit mask of r3).
constexpr uint32_t kShortSites[] = {
    0x820C3D9C, 0x820E0B38, 0x8210E874, 0x8210FCC8, 0x8210FFDC, 0x82117DA0, 0x8212A754,
    0x8212F00C, 0x8212F01C, 0x8212F08C, 0x8212F09C, 0x8212F0E8, 0x8212F1A0, 0x8212F1E4,
    0x8212F234, 0x82142E5C, 0x82147258, 0x82150FC0, 0x821525D4, 0x8217BA90, 0x8217BBA8,
    0x8218955C, 0x82195000, 0x8219C8C0, 0x821A1F00, 0x821C2570, 0x821C25F8, 0x821EEAE4,
    0x821EF5A4, 0x821FE0B4, 0x823746F4, 0x8239EB68, 0x823BD638, 0x823BE868, 0x823BE940,
    0x823BE954, 0x823C118C, 0x823C26E0, 0x823CC79C, 0x823CC7E8, 0x823DB00C, 0x823DF1F8,
    0x823DF224, 0x823DF23C, 0x823DF904, 0x823DF964, 0x823DF984, 0x823DF990, 0x823DF9B4,
    0x823DFBC8, 0x823EB4DC, 0x823F19EC, 0x8241DB30, 0x8241DCA8, 0x82461F40, 0x825B3CE4,
    0x825D238C, 0x825D2430, 0x825D8790, 0x82615054, 0x82617040, 0x826180B0, 0x826187CC,
    0x82619A0C, 0x82619F64, 0x82619FD0, 0x8261A10C, 0x8261EF34, 0x82626D34, 0x82626D9C,
    0x826296FC, 0x826297E8, 0x82629854, 0x82629BCC, 0x8262A154, 0x8262B278, 0x8262E15C,
    0x8262E1B0, 0x8262E228, 0x8262E3A0, 0x8262E3B8, 0x8262E560, 0x8262E858, 0x82631BF8,
    0x82631CCC, 0x826324E0, 0x826324EC, 0x82635C68, 0x8263D648, 0x82640294, 0x82640C74,
    0x82640C90, 0x82640D60, 0x82641D80, 0x82641D8C, 0x82642C34, 0x82643F2C, 0x82643F38,
    0x826462BC, 0x82648B94, 0x82649BAC, 0x82649BB8, 0x8264BE88, 0x8264D9F0, 0x8264F5E8,
    0x82650E2C, 0x82652C48, 0x8265323C, 0x82653248, 0x82653C8C, 0x826557E8, 0x82655974,
    0x82656C68, 0x82656F14, 0x826576FC, 0x82658824, 0x826647F4, 0x8266B3B4, 0x8266B5C0,
    0x8266B5CC, 0x8266B5D8, 0x8266B620, 0x8266B6A4, 0x8266DBD4, 0x8266DC40, 0x8266F4EC,
    0x8266F90C, 0x826767D8, 0x82676D00, 0x82676D0C, 0x82678E28, 0x82679890, 0x8267BE00,
    0x8267D48C, 0x8268D5B0, 0x8268E6C0, 0x8269958C, 0x8269C9F0, 0x826A0314, 0x826A6414,
    0x826AC7F8, 0x826ADC98, 0x826AF180, 0x826B0928, 0x826B0A50, 0x826BF74C, 0x826BFA7C,
    0x826BFDFC, 0x826D852C,
};

bool Contains(const uint32_t* begin, const uint32_t* end, uint32_t lr) {
  return std::binary_search(begin, end, lr);
}

bool AuditEnabled() {
  static const bool on = [] {
    const char* v = std::getenv("REACH_TICKAUDIT");
    return v && *v && *v != '0';
  }();
  return on;
}

// Return address -> highest level logged (open addressing, never removed).
constexpr size_t kSlots = 2048;
std::array<std::atomic<uint32_t>, kSlots> slot_lr{};
std::array<std::atomic<int>, kSlots> slot_level{};

// Whether this (site, level) is new; levels: 1 > 127, 2 > 255, 3 > 32767, 4 saturated.
bool FirstTime(uint32_t lr, int level) {
  size_t i = (lr >> 2) % kSlots;
  for (size_t n = 0; n < kSlots; ++n, i = (i + 1) % kSlots) {
    uint32_t cur = slot_lr[i].load(std::memory_order_relaxed);
    if (cur == 0 && slot_lr[i].compare_exchange_strong(cur, lr)) cur = lr;
    if (cur != lr) continue;
    int logged = slot_level[i].load(std::memory_order_relaxed);
    while (level > logged) {
      if (slot_level[i].compare_exchange_weak(logged, level)) return true;
    }
    return false;
  }
  return false;
}

}  // namespace

extern "C" REX_FUNC(sub_8214FAE8) {
  const double seconds = ctx.f1.f64;
  const uint32_t lr = uint32_t(ctx.lr);
  __imp__sub_8214FAE8(ctx, base);
  const int32_t ticks = ctx.r3.s32;
  if (ticks <= 127) return;
  int32_t limit = 0;
  if (ticks > 255 && Contains(std::begin(kByteSites), std::end(kByteSites), lr)) {
    limit = 255;
  } else if (ticks > 32767 && Contains(std::begin(kShortSites), std::end(kShortSites), lr)) {
    limit = 32767;
  }
  if (limit) {
    uint32_t tls, game_time;
    std::memcpy(&tls, reach::GuestPtr(base, ctx.r13.u32), 4);
    std::memcpy(&game_time, reach::GuestPtr(base, __builtin_bswap32(tls) + 0x6C), 4);
    const uint8_t* gt = reach::GuestPtr(base, __builtin_bswap32(game_time));
    const int tick_rate = int16_t(gt[4] << 8 | gt[5]);
    if (tick_rate > 30) {
      ctx.r3.u64 = uint64_t(limit);
      if (FirstTime(lr, 4)) {
        REXLOG_INFO("tick_storage: call {:#010x} {} s -> {} ticks at {} ticks a second, kept as {} "
                    "(the field's limit)", lr - 4, seconds, ticks, tick_rate, limit);
      }
    }
  }
  if (!AuditEnabled()) return;
  const int level = ticks > 32767 ? 3 : ticks > 255 ? 2 : 1;
  if (FirstTime(lr, level)) {
    REXLOG_INFO("TICKAUDIT: call {:#010x} {} s -> {} ticks (> {}; {} at 30 ticks a second)", lr - 4,
                seconds, ticks, level == 3 ? 32767 : level == 2 ? 255 : 127,
                int(std::lround(seconds * 30)));
  }
}
