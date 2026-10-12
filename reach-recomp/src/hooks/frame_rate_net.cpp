// reach - keeps players whose games cannot run together out of each other's network games
// (docs/framerate.md, "Network games with different frame rates").
//
// Each machine takes its tick rate from its own display (round(refresh) / 2, so 60 ticks a
// second with frame_rate = 60). Multiplayer games (distributed: each machine simulates its own
// players) run fine with each machine at its own rate. Campaign and Firefight co-op run the
// same simulation on every machine and fail with "An error occurred while playing co-op" unless
// every machine runs the same tick rate; frame_rate.cpp runs them at 30 ticks a second on every
// machine, so by default nothing here applies and players at 30 and 60 fps play together in
// every mode. With REACH_SYNC_TICKS30=0 a 60 fps machine runs co-op at 60 ticks a second, and
// then only plays with machines at its own frame rate, through the check below.
//
// The game already keeps different builds apart with a version number (0x82BD2660, the build
// number 11860) that travels in two places:
//   - the join-request (message 8: +4 the lowest host version the joiner accepts, +8 its
//     own version, +0xC the executable type, 5). The host's handler (sub_82319380) refuses
//     a joiner whose version is below its own (reason 25, peer-version-too-low) or who
//     wants a newer host (26, host-version-too-low);
//   - the header of a session's status (sub_82270170: +4 executable type, +8 version, +0xC
//     compatible version), sent in System Link's broadcast-reply and in the QoS data of
//     Live sessions. The menus offer to join only a session whose version range takes ours
//     (sub_826EA6C8(executable type, version, compatible version), from the System Link
//     list and the roster: X "Join User" does nothing otherwise), and the roster rates such a
//     session as not joinable (sub_822CABB8, status 4).
// Here the network copies of the version carry the co-op tick rate: a machine whose co-op
// does not run at 30 ticks a second sends 11860 + (tick rate << 8) instead of 11860 (16 bits
// on the wire), and the two checks compare what arrives with what this machine would send.
// Equal versions are restored to 11860 for the game's comparison; a peer at another rate gets
// a version the game refuses. Machines with co-op at 30 ticks send the plain build number, so
// they play with each other whatever their frame rates, and with 30 fps builds from before
// this check. Builds from before co-op at 30 ticks that run at 60 fps send 0x6A54 and stay
// apart from these (their co-op runs at 60 ticks). Saved content (films, maps, game types)
// keeps the plain number: only these four functions see the tagged one.
//
// REACH_MIXED_FRAME_RATE=1 turns the check off (and the matchmaking key in live_xmsg.cpp)
// also with REACH_SYNC_TICKS30=0, to test mixed games.

#include "frame_rate.h"

#include "../platform/guest_memory.h"

#include <rex/logging.h>
#include <rex/ppc/context.h>
#include <rex/ppc/func.h>
#include <rex/ui/flags.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>

using reach::GuestPtr;

REX_EXTERN(__imp__sub_82326AE8);  // encode a join-request (stream, -, message)
REX_EXTERN(__imp__sub_82319380);  // host: handle a join-request (session, from, message)
REX_EXTERN(__imp__sub_823275D8);  // decode a join-refuse (stream, -, message)
REX_EXTERN(__imp__sub_82270170);  // encode a session status header (stream, header)
REX_EXTERN(__imp__sub_822CABB8);  // can we join the session with this status (status, ...)
REX_EXTERN(__imp__sub_826EA6C8);  // menus: does a session's version range take ours (type, version, compatible)

namespace {

constexpr uint32_t kBuildVersion = 0x82BD2660;  // s32: the build number (11860)
constexpr int kRefusePeerVersionTooLow = 25, kRefuseHostVersionTooLow = 26;

uint32_t LoadBE32(const uint8_t* p) {
  uint32_t v;
  std::memcpy(&v, p, 4);
  return __builtin_bswap32(v);
}

void StoreBE32(uint8_t* p, uint32_t v) {
  v = __builtin_bswap32(v);
  std::memcpy(p, &v, 4);
}

bool MixedAllowed() {
  static const bool allowed = [] {
    const char* v = std::getenv("REACH_MIXED_FRAME_RATE");
    return v && *v && *v != '0';
  }();
  return allowed;
}

// The tick rate network peers have to share: 30 (the plain build number) when co-op runs at
// 30 ticks a second or mixed games are allowed, else ours.
int NetTickRate() { return MixedAllowed() || reach::SyncGamesAt30() ? 30 : reach::TickRate(); }

// What this machine sends for the version `v`, and what the checks make of a received one.
uint32_t Tagged(uint32_t v) {
  const int rate = NetTickRate();
  return rate == 30 ? v : (v + (uint32_t(rate) << 8)) & 0xFFFF;
}

// A received version: ours (tagged as we tag it) becomes the build number again; the plain
// build number from a 30 tick machine, when we are not at 30, becomes 0 (too low). Anything
// else stays and the game compares it as it is.
uint32_t Untagged(uint32_t received, uint32_t version, bool* mismatch) {
  const uint32_t ours = Tagged(version);
  if (received == ours) return version;
  *mismatch = true;
  return received == version ? 0 : received;
}

int TheirTickRate(uint32_t received, uint32_t version) {
  return received == version ? 30 : int(((received - version) & 0xFFFF) >> 8);
}

// Rewrites the two s32 version fields at `a` and `b` for a call and restores them after.
struct VersionFields {
  uint8_t *a, *b;
  uint32_t old_a, old_b;
  VersionFields(uint8_t* a, uint8_t* b) : a(a), b(b), old_a(LoadBE32(a)), old_b(LoadBE32(b)) {}
  void Set(uint32_t va, uint32_t vb) { StoreBE32(a, va), StoreBE32(b, vb); }
  ~VersionFields() { StoreBE32(a, old_a), StoreBE32(b, old_b); }
};

uint32_t BuildVersion(uint8_t* base) { return LoadBE32(GuestPtr(base, kBuildVersion)); }

// A session at another tick rate is checked every frame while it is listed: log it once.
void LogUnjoinable(uint32_t received, uint32_t version) {
  static std::atomic<bool> logged{false};
  if (logged.exchange(true)) return;
  REXLOG_WARN("frame_rate: a session at {} ticks a second is not joinable (we run {}): players "
              "need the same frame_rate (docs/framerate.md)",
              TheirTickRate(received, version), NetTickRate());
}

}  // namespace

namespace reach {

int TickRate() {
  // The game's rule (sub_82476A60): round(refresh rate) / 2, with the refresh rate the SDK
  // reports (clamped to 24-240 Hz in xboxkrnl_video.cpp).
  static const int rate = int(std::lround(std::clamp(REXCVAR_GET(video_mode_refresh_rate), 24.0, 240.0))) / 2;
  return rate;
}

uint32_t MatchmakingKey(uint32_t playlist) {
  const int rate = NetTickRate();
  return rate == 30 ? playlist : playlist ^ (uint32_t(rate) << 24);
}

}  // namespace reach

extern "C" REX_FUNC(sub_82326AE8) {
  const uint32_t version = BuildVersion(base);
  uint8_t* message = GuestPtr(base, ctx.r5.u32);
  VersionFields fields(message + 4, message + 8);
  if (fields.old_a == version && fields.old_b == version)
    fields.Set(Tagged(version), Tagged(version));
  __imp__sub_82326AE8(ctx, base);
}

extern "C" REX_FUNC(sub_82319380) {
  const uint32_t version = BuildVersion(base);
  uint8_t* message = GuestPtr(base, ctx.r5.u32);
  VersionFields fields(message + 4, message + 8);
  bool mismatch = false;
  const uint32_t lowest_host = Untagged(fields.old_a, version, &mismatch);
  const uint32_t theirs = Untagged(fields.old_b, version, &mismatch);
  if (mismatch) {
    REXLOG_WARN("frame_rate: refusing a join from a machine at {} ticks a second (we run {}): "
                "players need the same frame_rate (docs/framerate.md)",
                TheirTickRate(fields.old_b, version), NetTickRate());
  }
  fields.Set(lowest_host, theirs);
  __imp__sub_82319380(ctx, base);
}

extern "C" REX_FUNC(sub_823275D8) {
  const uint32_t message = ctx.r5.u32;
  __imp__sub_823275D8(ctx, base);
  const uint32_t reason = LoadBE32(GuestPtr(base, message + 8));
  if (reason == kRefusePeerVersionTooLow || reason == kRefuseHostVersionTooLow) {
    REXLOG_WARN("frame_rate: the host refused our join ({}): it runs another frame_rate "
                "(we run {} ticks a second) or another build",
                reason == kRefusePeerVersionTooLow ? "peer-version-too-low" : "host-version-too-low",
                NetTickRate());
  }
}

extern "C" REX_FUNC(sub_82270170) {
  const uint32_t version = BuildVersion(base);
  uint8_t* header = GuestPtr(base, ctx.r4.u32);
  VersionFields fields(header + 8, header + 0xC);
  if (fields.old_a == version && fields.old_b == version)
    fields.Set(Tagged(version), Tagged(version));
  __imp__sub_82270170(ctx, base);
}

extern "C" REX_FUNC(sub_822CABB8) {
  const uint32_t version = BuildVersion(base);
  uint8_t* status = GuestPtr(base, ctx.r3.u32);
  VersionFields fields(status + 8, status + 0xC);
  bool mismatch = false;
  const uint32_t newest = Untagged(fields.old_a, version, &mismatch);
  const uint32_t oldest = Untagged(fields.old_b, version, &mismatch);
  if (mismatch) LogUnjoinable(fields.old_a, version);
  fields.Set(newest, oldest);
  __imp__sub_822CABB8(ctx, base);
}

extern "C" REX_FUNC(sub_826EA6C8) {
  const uint32_t version = BuildVersion(base);
  bool mismatch = false;
  const uint32_t received = ctx.r4.u32;
  ctx.r4.u64 = Untagged(received, version, &mismatch);
  ctx.r5.u64 = Untagged(ctx.r5.u32, version, &mismatch);
  if (mismatch) LogUnjoinable(received, version);
  __imp__sub_826EA6C8(ctx, base);
}

