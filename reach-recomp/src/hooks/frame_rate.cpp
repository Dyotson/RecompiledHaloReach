// reach - the tick rate above 30 fps, and the timing that has to hold at it (docs/framerate.md).
//
// Reach's tick rate is half the display's refresh rate: the game options get
// round(refresh) / 2 (sub_82476A60, options + 0xC; 25 on a 50 Hz PAL set), and the game time
// globals (TLS + 0x6C: +4 tick rate, +8 seconds per tick, +0xC game time in ticks) take it
// from there. frame_rate = 60 runs the guest vblank at 120 Hz, so the game simulates 60
// ticks a second; 120 and 144 run 240 and 288 Hz, and the tick rate is held at 72 or below
// (60 at 120 fps, 72 at 144: two frames per tick; "The tick rate of games", below). Durations are converted with the tick rate (game_seconds_to_ticks,
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
// Synchronous network games (campaign and Firefight co-op over System Link or Reach Live: every
// machine runs the same simulation, tick for tick) run at 30 ticks a second on every machine,
// whatever its frame rate: a 60 fps machine runs one tick every other frame (and draws every
// frame: "Frames without a tick", below), so players at 30 and 60 fps can play co-op together. The game globals' copy of
// the game options' tick rate (+0x1C, round(refresh) / 2) is set to 30 right after the options
// are copied in at game start (sub_82476568) and again before the game time takes it
// (sub_8247B530). REACH_SYNC_TICKS30=0 keeps the display's rate in co-op (60/60 co-op at 60
// ticks; frame_rate_net.cpp then keeps machines with other frame rates out, as before);
// =2 also runs local games at 30 ticks, =3 local games after the main menu (tests).
//
// Havok's collision tolerances (hkpCollisionDispatcher::initCollisionQualityInfo,
// sub_8284FD70): the Havok world is created with hkpWorldCinfo::m_expectedMinPsiDeltaTime = the
// game time's seconds per tick (sub_8251C3A0), and the dispatcher derives its TOI separations
// from it (gravity * dt^2). After each world creation sub_8251D3F8 copies the dispatcher's first
// 7760 bytes: the first time into a static copy (0x82AD2AC4), every later time back from it.
// So the tolerances of the session's first world, the main menu's, hold for every later game:
// on a 60 fps machine the menu runs 60 ticks a second, so a co-op game at 30 ticks still ran
// its collisions with 1/60 s tolerances. The props that drop at the start of a Firefight map
// came to rest differently from a 30 fps machine's within 10 ticks, the AI followed, and the
// game failed with "An error occurred while playing co-op" (docs/framerate.md, "Mixed co-op").
// Here the quality infos are always computed for 1/30 s, the console's tick, whatever the
// tick rate; at 30 ticks a second nothing changes. REACH_HAVOK_NATIVE_STEP=1 lets the game pass
// its own value (for comparisons).

#include "../platform/guest_memory.h"

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/ppc/context.h>
#include <rex/ppc/func.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>

using reach::GuestPtr;

REX_EXTERN(__imp__sub_825CD9C0);  // hs game_reverted
REX_EXTERN(__imp__sub_8249FEB0);  // scripted camera, mode 5: matrix of a shot camera frame
REX_EXTERN(__imp__sub_82476568);  // game start: copies the game options into the game globals (+0x10)
REX_EXTERN(__imp__sub_8247B530);  // game time init: tick rate from the game globals (+0x1C)
REX_EXTERN(__imp__sub_8284FD70);  // hkpCollisionDispatcher::initCollisionQualityInfo(this, input)
REX_EXTERN(__imp__sub_8246E5A8);  // cinematic_clips_initialize_for_shot(shot)
REX_EXTERN(__imp__sub_820E1B60);  // main loop: hand the frame to the render thread if it is worth drawing

REXCVAR_DECLARE(int32_t, frame_rate);

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

// sub_8246E5A8(shot): cinematic_clips_initialize_for_shot. A shot's clip planes (+0x34 count,
// +0x38 entries of 0x2C bytes: the plane, then the first and the last frame it applies to at
// +0x18 and +0x1C, then the objects it clips) are copied into the cinematic globals (TLS +0x50:
// +0x9C8 count, +0x9CC entries of 0x84 bytes) with the frames turned into game times by adding
// the game time in ticks. The frames are the shot's 30 Hz camera frames (the shot starts when
// this runs), so above 30 ticks a second a plane came on and went off too early: at 60 ticks
// half way, at 144 after a fifth of its time. Here the copies are moved to the tick of their
// frame, round(frame * tick rate / 30). At 30 ticks a second nothing changes.
extern "C" REX_FUNC(sub_8246E5A8) {
  __imp__sub_8246E5A8(ctx, base);
  const uint32_t tls = LoadBE32(GuestPtr(base, ctx.r13.u32));
  const uint32_t cinematic = LoadBE32(GuestPtr(base, tls + 0x50));
  const uint32_t game_time = LoadBE32(GuestPtr(base, tls + 0x6C));
  if (!cinematic || !game_time) return;
  const uint8_t* gt = GuestPtr(base, game_time);
  const int tick_rate = int16_t(gt[4] << 8 | gt[5]);
  const int32_t count = int32_t(LoadBE32(GuestPtr(base, cinematic + 0x9C8)));
  if (tick_rate == 30 || tick_rate <= 0 || count <= 0 || count > 32) return;
  const int32_t now = int32_t(LoadBE32(GuestPtr(base, game_time + 0xC)));
  for (int32_t i = 0; i < count; ++i) {
    uint8_t* clip = GuestPtr(base, cinematic + 0x9CC + uint32_t(i) * 0x84);
    const int32_t first = int32_t(LoadBE32(clip + 0x18)) - now;
    const int32_t last = int32_t(LoadBE32(clip + 0x1C)) - now;
    const auto at_tick = [&](int32_t frame) {
      return now + int32_t(std::lround(double(frame) * tick_rate / 30.0));
    };
    StoreBE32(clip + 0x18, uint32_t(at_tick(first)));
    StoreBE32(clip + 0x1C, uint32_t(at_tick(last)));
    REXLOG_INFO("frame_rate: cinematic clip plane {} for frames {}-{}: ticks {}-{} from now ({} "
                "ticks a second)", i, first, last, at_tick(first) - now, at_tick(last) - now,
                tick_rate);
  }
}

// --- The tick rate of games ---------------------------------------------------------------------

namespace {

int games_started = 0;  // game time initializations (sub_8247B530); main thread only

// REACH_SYNC_TICKS30: unset or 1 synchronous network games at 30 ticks a second (the default),
// 0 at the display's rate, 2 also every local game, 3 local games after the main menu.
int SyncTicks30() {
  static const int mode = [] {
    const char* v = std::getenv("REACH_SYNC_TICKS30");
    return v && *v ? std::atoi(v) : 1;
  }();
  return mode;
}

// Whether the game about to start (game globals +0x14: network type, 1 local, 2 sync-client,
// 3 sync-server) runs at 30 ticks a second; `games_before`: games started before it.
bool Ticks30(uint32_t network, int games_before) {
  const int mode = SyncTicks30();
  if (network == 2 || network == 3) return mode != 0;
  return network == 1 && (mode == 2 || (mode == 3 && games_before >= 1));
}

// Every other game ticks at most kMaxTickRate times a second, at a whole number of frames per
// tick: the display's rate divided by ceil(rate / 72), so 60 ticks at 60 fps, 60 at 120, 72 at
// 144 (two frames per tick), 60 at 240. Many tick counters are bytes, a good part of them signed
// and compared as such (unit actions, AI, players): 127 ticks are 4.2 s at the console's 30, 2.1 s
// at 60, 1.76 s at 72 and 0.88 s at 144, where actions longer than that would break
// (docs/framerate.md, "120 and 144 fps"). REACH_TICK_RATE_MAX=N changes the limit (0: none, the
// game's own rule of one tick per frame).
constexpr int kMaxTickRate = 72;

int TickRateMax() {
  static const int max = [] {
    const char* v = std::getenv("REACH_TICK_RATE_MAX");
    return v && *v ? std::max(0, std::atoi(v)) : kMaxTickRate;
  }();
  return max;
}

// Sets the game globals' copy of the options' tick rate (+0x1C) to 30 for such a game, and to at
// most TickRateMax() for the others.
void PatchGameTickRate(PPCContext& ctx, uint8_t* base, int games_before, const char* when) {
  const uint32_t tls = LoadBE32(GuestPtr(base, ctx.r13.u32));
  const uint32_t globals_addr = LoadBE32(GuestPtr(base, tls + 0x3C));
  if (!globals_addr) return;
  uint8_t* globals = GuestPtr(base, globals_addr);
  const uint32_t network = LoadBE32(globals + 0x14);
  const int rate = int16_t(globals[0x1C] << 8 | globals[0x1D]);
  int target = rate;
  if (Ticks30(network, games_before)) {
    target = 30;
  } else if (TickRateMax() >= 30 && rate > TickRateMax()) {
    target = rate / ((rate + TickRateMax() - 1) / TickRateMax());
  }
  if (target == rate || target <= 0) return;
  REXLOG_INFO("frame_rate: {} (network type {}): {} ticks a second instead of {}", when, network,
              target, rate);
  globals[0x1C] = uint8_t(target >> 8), globals[0x1D] = uint8_t(target);
}

}  // namespace

namespace reach {
int GamesStarted() { return games_started; }
bool SyncGamesAt30() { return SyncTicks30() != 0; }
}  // namespace reach

// Right after the game options are copied into the game globals at game start, before the
// systems' new-map initialization (sub_82475D48) reads them.
extern "C" REX_FUNC(sub_82476568) {
  __imp__sub_82476568(ctx, base);
  PatchGameTickRate(ctx, base, games_started, "game start");
}

extern "C" REX_FUNC(sub_8247B530) {
  const int games_before = games_started++;
  PatchGameTickRate(ctx, base, games_before, "game time");
  __imp__sub_8247B530(ctx, base);
}

// sub_8284FD70(dispatcher, input): input +0 gravity length, +4 collision tolerance, +8 the
// minimum expected step (s), +0xC the maximum linear velocity.
extern "C" REX_FUNC(sub_8284FD70) {
  static const bool native = [] {
    const char* v = std::getenv("REACH_HAVOK_NATIVE_STEP");
    return v && *v && *v != '0';
  }();
  constexpr uint32_t kThirtieth = 0x3D088889;  // 1.0f / 30, as the game time computes it
  uint8_t* input = GuestPtr(base, ctx.r4.u32);
  const uint32_t asked = LoadBE32(input + 8);
  // The world is created again every few seconds (5 s in the campaign, 10 s in Firefight):
  // log only when the requested step changes.
  static uint32_t last_asked = 0;  // main thread only
  if (asked != last_asked) {
    last_asked = asked;
    REXLOG_INFO("frame_rate: Havok collision tolerances (dispatcher {:#x}) for steps of {} s{}",
                ctx.r3.u32, LoadBEFloat(input + 8),
                native || asked == kThirtieth ? "" : ", computed for 1/30 s");
  }
  if (native || asked == kThirtieth) return __imp__sub_8284FD70(ctx, base);
  StoreBE32(input + 8, kThirtieth);
  __imp__sub_8284FD70(ctx, base);
  StoreBE32(input + 8, asked);
}

// --- Frames without a tick ----------------------------------------------------------------------
//
// sub_820E1B60 hands the main loop's frame to the render thread only if the frame's flags
// (*(TLS + 0x80), set at the end of the game update sub_82440410) have one of the bits 0x3FFF
// set. Bit 0 is "ticks ran since the last drawn frame"; at the end of the update all flags are
// cleared when the frame took less than 25 ms and none of a few others (paused, a state change)
// is set. So a frame without a tick is not drawn unless 25 ms have passed: at the console's 30
// fps every frame has a tick, but with fewer ticks than frames (72 ticks at 144 fps, co-op's 30
// ticks at 60 or 144 fps) only the frames with a tick were drawn, 72 or 30 a second. Here such a
// frame gets bit 0x10 (no other user seen) when the game ticks less often than it draws, so
// every frame is drawn. REACH_RENDER_TICKLESS=0 turns this off.
//
// Drawn as it is, such a frame repeats the last tick's view: the player's look is integrated
// over the ticks of the frame (sub_8247C978 gets ticks * seconds per tick), and the observer
// camera (TLS + 0x354, the first local player's: +0x148 position, +0x170 forward, +0x17C up) is
// only set on the ticks, so without a tick the camera neither turns nor moves. For such a frame
// the view is predicted, for drawing only: before the observer's per-frame post-process
// (sub_821EF820 -> sub_82457A20, which feeds the HUD and the views the frame is drawn with), the
// observer is moved on by the last tick's motion (the change between the last two frames with a
// tick) times the part of a tick since the last one; after sub_820E1B60 has handed the frame to
// the render thread the observer gets its values back, before the next frame's ticks. Only on
// foot in the player's own view: not while a scripted camera plays (TLS + 0x44, mode +2: the
// cinematic camera moves with the cinematic's objects on the ticks), not in a vehicle or seat
// (the director's target unit, TLS + 0xC8 +8, has a parent at +0x14: the vehicle around the
// camera moves on the ticks too), and not across a jump of more than 2 m or 45 degrees in a
// tick (respawn, teleport). Moving objects still step on the ticks. Off by default:
// REACH_TICKLESS_VIEW=1 turns the prediction on.
namespace {

struct TickView {
  bool valid = false;
  double t = 0;           // seconds (steady clock) when the frame was handed over
  uint32_t observer = 0;  // address of the observer the values came from
  float pos[3], fwd[3], up[3];
};
TickView view_last, view_prev;  // main thread only

double NowSeconds() {
  return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

void LoadVec(const uint8_t* p, float* v) {
  for (int i = 0; i < 3; ++i) v[i] = LoadBEFloat(p + 4 * i);
}

void StoreVec(uint8_t* p, const float* v) {
  for (int i = 0; i < 3; ++i) StoreBEFloat(p + 4 * i, v[i]);
}

void Cross(const float* a, const float* b, float* out) {
  out[0] = a[1] * b[2] - a[2] * b[1];
  out[1] = a[2] * b[0] - a[0] * b[2];
  out[2] = a[0] * b[1] - a[1] * b[0];
}

float Dot(const float* a, const float* b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }

// Rotates v by `angle` radians about the unit axis k (Rodrigues).
void Rotate(const float* v, const float* k, float angle, float* out) {
  float kxv[3];
  Cross(k, v, kxv);
  const float c = std::cos(angle), s = std::sin(angle), kv = Dot(k, v) * (1 - c);
  for (int i = 0; i < 3; ++i) out[i] = v[i] * c + kxv[i] * s + k[i] * kv;
}

// The rotation that takes the basis (f0, u0) to (f1, u1) as an axis and an angle.
bool DeltaRotation(const float* f0, const float* u0, const float* f1, const float* u1, float* axis,
                   float* angle) {
  float l0[3], l1[3];
  Cross(u0, f0, l0);
  Cross(u1, f1, l1);
  // R = B1 * B0^T with the bases as columns (f, l, u).
  float r[3][3];
  for (int i = 0; i < 3; ++i)
    for (int j = 0; j < 3; ++j) r[i][j] = f1[i] * f0[j] + l1[i] * l0[j] + u1[i] * u0[j];
  const float trace = r[0][0] + r[1][1] + r[2][2];
  const float a = std::acos(std::clamp((trace - 1) / 2, -1.0f, 1.0f));
  if (a < 1e-5f) {
    *angle = 0;
    axis[0] = 0, axis[1] = 0, axis[2] = 1;
    return true;
  }
  axis[0] = r[2][1] - r[1][2], axis[1] = r[0][2] - r[2][0], axis[2] = r[1][0] - r[0][1];
  const float n = std::sqrt(Dot(axis, axis));
  if (n < 1e-6f) return false;  // close to 180 degrees: no prediction
  for (int i = 0; i < 3; ++i) axis[i] /= n;
  *angle = a;
  return true;
}

// The first local player's view may be predicted: no scripted camera, a target unit on foot.
bool OnFootOwnView(uint8_t* base, uint32_t tls) {
  const uint32_t scripted = LoadBE32(GuestPtr(base, tls + 0x44));
  if (!scripted || int16_t(GuestPtr(base, scripted)[2] << 8 | GuestPtr(base, scripted)[3]) != 0) return false;
  const uint32_t director = LoadBE32(GuestPtr(base, tls + 0xC8));
  const uint32_t data = LoadBE32(GuestPtr(base, tls + 0x18));
  if (!director || !data) return false;
  const uint32_t unit = LoadBE32(GuestPtr(base, director + 8));
  if (unit == 0xFFFFFFFFu || unit == 0) return false;
  const uint32_t elements = LoadBE32(GuestPtr(base, data + 0x44));
  const uint8_t* header = GuestPtr(base, elements + (unit & 0xFFFF) * 0x10);
  if (uint16_t(header[0] << 8 | header[1]) != unit >> 16) return false;
  const uint32_t object = LoadBE32(header + 0xC);
  return object && LoadBE32(GuestPtr(base, object + 0x14)) == 0xFFFFFFFFu;
}

bool RenderTickless() {
  static const bool enabled = [] {
    const char* v = std::getenv("REACH_RENDER_TICKLESS");
    return !(v && *v == '0');
  }();
  return enabled;
}

// Off by default until checked further (co-op at 30 ticks, the position part, vehicles).
bool PredictView() {
  static const bool enabled = [] {
    const char* v = std::getenv("REACH_TICKLESS_VIEW");
    return v && *v && *v != '0';
  }();
  return enabled;
}

// The game ticks less often than it draws (and so has frames without a tick).
bool FewerTicksThanFrames(uint8_t* base, uint32_t tls) {
  const uint32_t game_time = LoadBE32(GuestPtr(base, tls + 0x6C));
  if (!game_time || REXCVAR_GET(frame_rate) <= 30) return false;
  const uint8_t* gt = GuestPtr(base, game_time);
  const int tick_rate = int16_t(gt[4] << 8 | gt[5]);
  return tick_rate > 0 && tick_rate < REXCVAR_GET(frame_rate);
}

uint8_t view_saved[0x40];      // the observer's +0x148..+0x187 before the prediction
uint32_t view_saved_at = 0;    // its address while a prediction is in place, else 0

void RestoreView(uint8_t* base) {
  if (!view_saved_at) return;
  std::memcpy(GuestPtr(base, view_saved_at), view_saved, sizeof(view_saved));
  view_saved_at = 0;
}

REX_EXTERN(__imp__sub_821EF820);  // per-frame updates after the ticks, among them the observer's post-process

}  // namespace

// sub_821EF820(dt), called once a frame from the game update after the ticks (return address
// 0x82440774), runs the observer's per-frame post-process (sub_82457A20: shake, then +0x148,
// +0x170, +0x17C written back) before the HUD and the views are set up: the prediction goes in
// here and is taken out again after sub_820E1B60 has handed the frame over.
extern "C" REX_FUNC(sub_821EF820) {
  const uint32_t lr = uint32_t(ctx.lr);
  RestoreView(base);  // a frame that never reached sub_820E1B60
  const uint32_t tls = LoadBE32(GuestPtr(base, ctx.r13.u32));
  if (lr != 0x82440774 || !RenderTickless() || !PredictView() || !FewerTicksThanFrames(base, tls))
    return __imp__sub_821EF820(ctx, base);
  const uint32_t frame_addr = LoadBE32(GuestPtr(base, tls + 0x80));
  const uint32_t observers = LoadBE32(GuestPtr(base, tls + 0x354));
  if (!frame_addr || !observers) return __imp__sub_821EF820(ctx, base);
  uint8_t* obs = GuestPtr(base, observers);
  const double now = NowSeconds();
  // +0x10 of the frame: ticks since the last drawn frame (every frame is drawn here).
  if (LoadBE32(GuestPtr(base, frame_addr + 0x10)) != 0) {
    view_prev = view_last;
    view_last.valid = true;
    view_last.t = now;
    view_last.observer = observers;
    LoadVec(obs + 0x148, view_last.pos);
    LoadVec(obs + 0x170, view_last.fwd);
    LoadVec(obs + 0x17C, view_last.up);
    return __imp__sub_821EF820(ctx, base);
  }
  if (view_last.valid && view_prev.valid && view_last.observer == observers &&
      view_prev.observer == observers && view_last.t > view_prev.t && OnFootOwnView(base, tls)) {
    float pos[3], fwd[3], up[3], axis[3], angle = 0, step[3];
    LoadVec(obs + 0x148, pos);
    LoadVec(obs + 0x170, fwd);
    LoadVec(obs + 0x17C, up);
    for (int i = 0; i < 3; ++i) step[i] = view_last.pos[i] - view_prev.pos[i];
    const float f = float(std::clamp((now - view_last.t) / (view_last.t - view_prev.t), 0.0, 1.0));
    if (Dot(step, step) < 4.0f &&
        DeltaRotation(view_prev.fwd, view_prev.up, view_last.fwd, view_last.up, axis, &angle) &&
        angle < 0.785f) {
      float pf[3], pu[3], pp[3];
      Rotate(fwd, axis, angle * f, pf);
      Rotate(up, axis, angle * f, pu);
      for (int i = 0; i < 3; ++i) pp[i] = pos[i] + step[i] * f;
      std::memcpy(view_saved, obs + 0x148, sizeof(view_saved));
      view_saved_at = observers + 0x148;
      StoreVec(obs + 0x148, pp);
      StoreVec(obs + 0x170, pf);
      StoreVec(obs + 0x17C, pu);
    }
  }
  __imp__sub_821EF820(ctx, base);
}

extern "C" REX_FUNC(sub_820E1B60) {
  const uint32_t tls = LoadBE32(GuestPtr(base, ctx.r13.u32));
  if (RenderTickless() && FewerTicksThanFrames(base, tls)) {
    const uint32_t frame_addr = LoadBE32(GuestPtr(base, tls + 0x80));
    if (frame_addr) {
      uint8_t* frame = GuestPtr(base, frame_addr);
      const uint32_t flags = LoadBE32(frame);
      if ((flags & 0x3FFF) == 0) StoreBE32(frame, flags | 0x10);  // drawn as well
      if (flags & 0x2042) view_last.valid = false;  // paused (0x2, 0x40), a state change (0x2000)
    }
  }
  __imp__sub_820E1B60(ctx, base);
  RestoreView(base);
}
