// reach - script timing that has to hold at frame_rate = 60 (docs/framerate.md).
//
// Reach's tick rate is half the display's refresh rate: the game options get
// round(refresh) / 2 (sub_82476A60, options + 0xC; 25 on a 50 Hz PAL set), and the game time
// globals (TLS + 0x6C: +4 tick rate, +8 seconds per tick, +0xC game time in ticks) take it
// from there. frame_rate = 60 runs the guest vblank at 120 Hz, so the game simulates 60
// ticks a second. Durations are converted with the tick rate (game_seconds_to_ticks,
// sub_8214FAE8), so scripts written for 30 ticks keep their real-time lengths: `sleep 1`
// is 1/30 s, which is 2 ticks at 60.
//
// game_reverted (hs function 0x47D, sub_825CD9C0) is true only while the game time equals
// the time the last revert restored (0x83597B00, set by the revert). Cinematics rely on it:
// cinematic_skip_start saves a checkpoint and asks for it, cinematic_skip_stop reverts to
// that save at the end of the cinematic, and the restored script, asleep in
// `sleep_until (not (game_saving)) 1` after the save, sees game_reverted and skips the
// cinematic it has just shown (as a skip does). The checkpoint is taken one tick after the
// script asked for it, while the script sleeps 1/30 s. At 30 ticks a second that is the same
// tick and the script wakes up in the reverted tick; at 60 it wakes one tick later, sees
// false and plays the cinematic again, from the start: Winter Contingency's opening
// looped back to the planet instead of reaching gameplay.
//
// Here game_reverted also counts the rest of the 30 Hz tick the revert happened in (round(tick
// rate / 30) ticks). At 30 ticks a second it is unchanged. The game reads 0x83597B00 nowhere
// else but in its checkpoint serialization.
//
// Cinematic cameras (sub_8249FEB0, the camera of a cinematic scene shot, scripted camera mode
// 5): a shot's camera is a list of frames at 30 a second (position, forward, up). The scripted
// camera's tick (sub_820C2938) counts the ticks since the shot started (+0x60) and turns them into
// the frame to show, (int)(ticks * seconds per tick * 30) (+0x4C, +0x48 within the shot). At 60
// ticks a second that frame changes on every other tick, so the camera moves at 30 Hz while
// the objects it films move on every tick: the Falcon and its passengers in Winter
// Contingency's flight run 0.4 m a tick ahead of a camera that waits, then catches up, so near
// objects jump back and forth and drop out of the view on alternate frames
// (docs/rendering.md). Here the camera is placed between that frame and the next, at the
// fraction the truncation dropped; at 30 ticks a second the fraction is 0 and the game's own
// frame is used. REACH_CINEMATIC_CAMERA_STEPS=1 turns this off (for comparisons).
//
// Each machine takes the tick rate from its own display, also in network games, so network
// co-op (campaign and Firefight are synchronous games: every machine runs the same
// simulation) needs the same frame_rate on every machine; frame_rate_net.cpp keeps machines
// with different ones apart (docs/framerate.md).

#include "../platform/guest_memory.h"

#include <rex/logging.h>
#include <rex/ppc/context.h>
#include <rex/ppc/func.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>

using reach::GuestPtr;

REX_EXTERN(__imp__sub_825CD9C0);  // hs game_reverted
REX_EXTERN(__imp__sub_8249FEB0);  // scripted camera, mode 5: matrix of a shot camera frame

namespace {

constexpr uint32_t kRevertTime = 0x83597B00;  // game time the last revert restored, or -1

uint32_t LoadBE32(const uint8_t* p) {
  uint32_t v;
  std::memcpy(&v, p, 4);
  return __builtin_bswap32(v);
}

void StoreBE32(uint8_t* p, uint32_t v) {
  v = __builtin_bswap32(v);
  std::memcpy(p, &v, 4);
}

float LoadBEFloat(const uint8_t* p) {
  const uint32_t v = LoadBE32(p);
  float f;
  std::memcpy(&f, &v, 4);
  return f;
}

void StoreBEFloat(uint8_t* p, float f) {
  uint32_t v;
  std::memcpy(&v, &f, 4);
  StoreBE32(p, v);
}

constexpr uint32_t kTagInstances = 0x82BE8018;  // pointer to the tag instance table (8 bytes each)
constexpr uint32_t kCameraFrameSize = 36;          // position, forward, up

void Normalize(float* v) {
  const float length = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
  if (length > 1e-6f) v[0] /= length, v[1] /= length, v[2] /= length;
}

}  // namespace

extern "C" REX_FUNC(sub_825CD9C0) {
  const uint32_t tls = LoadBE32(GuestPtr(base, ctx.r13.u32));
  const uint32_t game_time = LoadBE32(GuestPtr(base, tls + 0x6C));
  uint8_t* revert_time = GuestPtr(base, kRevertTime);
  const int32_t reverted_at = static_cast<int32_t>(LoadBE32(revert_time));
  if (game_time == 0 || reverted_at == -1) return __imp__sub_825CD9C0(ctx, base);
  const int32_t now = static_cast<int32_t>(LoadBE32(GuestPtr(base, game_time + 0xC)));
  const uint8_t* r = GuestPtr(base, game_time + 4);
  const int tick_rate = static_cast<int16_t>((r[0] << 8) | r[1]);
  const int ticks_per_30hz_tick = std::max(1, (tick_rate + 15) / 30);
  if (now <= reverted_at || now - reverted_at >= ticks_per_30hz_tick)
    return __imp__sub_825CD9C0(ctx, base);
  // The game compares the game time with 0x83597B00: let it see the revert at this tick.
  REXLOG_INFO("frame_rate: game_reverted {} tick(s) after the revert (tick rate {})",
              now - reverted_at, tick_rate);
  StoreBE32(revert_time, static_cast<uint32_t>(now));
  __imp__sub_825CD9C0(ctx, base);
  StoreBE32(revert_time, static_cast<uint32_t>(reverted_at));
}

// sub_8249FEB0(frame, out): r3 is the shot camera frame to build the matrix from (0: the
// current one), r4 the matrix.
extern "C" REX_FUNC(sub_8249FEB0) {
  static const bool steps = [] {
    const char* v = std::getenv("REACH_CINEMATIC_CAMERA_STEPS");
    return v && *v && *v != '0';
  }();
  if (steps) return __imp__sub_8249FEB0(ctx, base);
  const uint32_t tls = LoadBE32(GuestPtr(base, ctx.r13.u32));
  const uint32_t camera = LoadBE32(GuestPtr(base, tls + 0x44));     // scripted camera globals
  const uint32_t game_time = LoadBE32(GuestPtr(base, tls + 0x6C));  // game time globals
  if (!camera || !game_time) return __imp__sub_8249FEB0(ctx, base);
  const uint8_t* cam = GuestPtr(base, camera);
  const uint32_t scene = LoadBE32(cam + 0x40);
  if (int16_t(cam[2] << 8 | cam[3]) != 5 || scene == 0xFFFFFFFFu) return __imp__sub_8249FEB0(ctx, base);
  const uint8_t* gt = GuestPtr(base, game_time);
  if (int16_t(gt[4] << 8 | gt[5]) <= 30) return __imp__sub_8249FEB0(ctx, base);
  // The frame as sub_820C2938 computed it this tick, and the part it truncated.
  const int32_t ticks = int32_t(LoadBE32(cam + 0x60));
  const int32_t frame = int32_t(LoadBE32(cam + 0x4C));
  const float t = float(ticks) * LoadBEFloat(gt + 8) * 30.0f - float(frame);
  if (!(t > 0.01f && t < 0.99f)) return __imp__sub_8249FEB0(ctx, base);
  // The shot (scene tag +0x30, 0xD0 bytes each, +0x2C of them): +0xB8 frames, +0xBC the frames.
  const uint32_t instances = LoadBE32(GuestPtr(base, kTagInstances));
  const uint32_t scene_data = LoadBE32(GuestPtr(base, instances + (scene & 0xFFFF) * 8 + 4));
  if (!scene_data) return __imp__sub_8249FEB0(ctx, base);
  const int32_t shots = int32_t(LoadBE32(GuestPtr(base, scene_data + 0x2C)));
  const int32_t shot_index = int32_t(LoadBE32(cam + 0x44));
  if (shot_index < 0 || shot_index >= shots) return __imp__sub_8249FEB0(ctx, base);
  const uint32_t shot = LoadBE32(GuestPtr(base, scene_data + 0x30)) + uint32_t(shot_index) * 0xD0;
  const int32_t count = int32_t(LoadBE32(GuestPtr(base, shot + 0xB8)));
  const uint32_t frames = LoadBE32(GuestPtr(base, shot + 0xBC));
  const int32_t shot_frame = int32_t(LoadBE32(cam + 0x48));
  if (shot_frame < 0 || shot_frame + 1 >= count) return __imp__sub_8249FEB0(ctx, base);
  const uint32_t current = frames + uint32_t(shot_frame) * kCameraFrameSize;
  if (ctx.r3.u32 != 0 && ctx.r3.u32 != current) return __imp__sub_8249FEB0(ctx, base);
  float a[9], b[9], out[9];
  for (int i = 0; i < 9; ++i) {
    a[i] = LoadBEFloat(GuestPtr(base, current + 4 * i));
    b[i] = LoadBEFloat(GuestPtr(base, current + kCameraFrameSize + 4 * i));
  }
  // A jump between two frames is a cut inside the shot: no blend across it.
  const float dx = b[0] - a[0], dy = b[1] - a[1], dz = b[2] - a[2];
  const float turn = a[3] * b[3] + a[4] * b[4] + a[5] * b[5];
  if (dx * dx + dy * dy + dz * dz > 4.0f * 4.0f || turn < 0.9f) return __imp__sub_8249FEB0(ctx, base);
  for (int i = 0; i < 9; ++i) out[i] = a[i] + (b[i] - a[i]) * t;
  // Forward and up stay an orthonormal pair (the matrix is built from them as they are).
  Normalize(out + 3);
  const float along = out[6] * out[3] + out[7] * out[4] + out[8] * out[5];
  for (int i = 0; i < 3; ++i) out[6 + i] -= along * out[3 + i];
  Normalize(out + 6);
  static uint32_t scratch = 0;  // main thread only
  if (!scratch) scratch = REX_KERNEL_MEMORY()->SystemHeapAlloc(64);
  if (!scratch) return __imp__sub_8249FEB0(ctx, base);
  for (int i = 0; i < 9; ++i) StoreBEFloat(GuestPtr(base, scratch + 4 * i), out[i]);
  ctx.r3.u64 = scratch;
  __imp__sub_8249FEB0(ctx, base);
}
