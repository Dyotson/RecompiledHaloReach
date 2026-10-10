// reach - Xbox Live services from a Reach Live server: the XAM messages a title sends
// to the XLiveBase (0xFC) and XGI (0xFB) apps, and the logon queries.
//
// With Live sign-in (identity.h: LiveSignin) the game believes it is signed in to Xbox
// Live and asks for:
//
// - XStringVerify (0xFC/0x5000C): the profanity check of user text. Every string is
//   accepted (the SDK failed it, and the game retried every frame).
// - XFriendsCreateEnumerator (0xFC/0x58020): the friends list. Every other player in our
//   room on the server is a friend, with the presence it reported (online, playing,
//   joinable session). The SDK failed it.
// - XSessionCreate (0xFB/0xB0010) for a session we host: the SDK leaves XSESSION_INFO
//   empty. We fill it (an online peer session id, our XNADDR, a random key) and, for a
//   presence session, publish it, so friends see it as joinable. Delete and modify
//   update that.
// - XPresenceSubscribe / Unsubscribe / CreateEnumerator (0xFC/0x5801E, 0x58044,
//   0x58019): the lobby's active roster asks for the presence (state, session id) of
//   the friends it lists; answered from the room's roster.
// - XSessionSearchByIds / ByID (0xFB/0xB0060, 0xB001B): friends' session ids to
//   XSESSION_INFO, from what each player published.
// - XNetQosListen / XNetQosLookup: the host's game description travels with its
//   presence; lookups are answered from the roster.
// - XInviteSend / XInviteGetAcceptedInfo (0xFC/0x50002, 0x58023): "Invite to Party"
//   goes through the server to the friend, whose game accepts it (live_accept_invites)
//   and joins our session.
// - XNetLogonGetTitleID / XNetLogonGetMachineID: the SDK stubs return garbage.
//
// Everything else goes to the SDK. REACH_NETTRACE=1 logs every XMsg call (as net.cpp
// did before) and what we answered. Message layouts follow the Xenia netplay fork
// (AdrianCassar/xenia-canary, BSD), see docs/online_plan.md.

#include "../platform/guest_memory.h"
#include "../platform/sdk_import.h"
#include "identity.h"
#include "live.h"

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/ppc/context.h>
#include <rex/ppc/func.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xenumerator.h>
#include <rex/system/xevent.h>
#include <rex/system/xmemory.h>
#include <rex/system/xthread.h>
#include <rex/system/xtypes.h>


#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <random>
#include <string>
#include <unordered_map>
#include <vector>

using reach::GuestPtr;

REXCVAR_DEFINE_BOOL(live_accept_invites, true, "Network/Reach Live",
                    "Accept game invites from other players at once (there is no Xbox Guide "
                    "to accept them in); the game declines them itself while busy");

namespace {

using GuestFunc = void (*)(PPCContext&, uint8_t*);

constexpr uint32_t kAppXgi = 0xFB;
constexpr uint32_t kAppXLiveBase = 0xFC;
constexpr uint32_t kTitleId = 0x4D53085B;
constexpr uint32_t kNotOurs = 0xFFFFFFFF;  // handler result: let the SDK answer
constexpr uint32_t kSuccess = 0;
constexpr uint32_t kInvalidArg = 0x80070057;  // X_E_INVALIDARG
constexpr uint32_t kIoPending = 0x3E5;

// X_ONLINE_FRIENDSTATE flags.
constexpr uint32_t kFriendOnline = 0x1;
constexpr uint32_t kFriendPlaying = 0x2;
constexpr uint32_t kFriendJoinable = 0x10;
constexpr uint32_t kFriendJoinableFriendsOnly = 0x100;

// XSESSION_CREATE flags.
constexpr uint32_t kSessionHost = 0x1;
constexpr uint32_t kSessionUsesPresence = 0x2;
constexpr uint32_t kSessionJoinViaPresenceDisabled = 0x200;
constexpr uint32_t kSessionJoinViaPresenceFriendsOnly = 0x800;

constexpr size_t kSessionInfoSize = 0x3C;  // XSESSION_INFO
constexpr size_t kFriendSize = 0xC4;       // X_ONLINE_FRIEND

bool Trace() {
  static const bool on = [] {
    const char* v = std::getenv("REACH_NETTRACE");
    return v && *v && *v != '0' && std::string(v) != "packets";
  }();
  return on;
}

uint32_t Load32(const uint8_t* p) { return uint32_t(p[0]) << 24 | p[1] << 16 | p[2] << 8 | p[3]; }
void Store32(uint8_t* p, uint32_t v) {
  p[0] = uint8_t(v >> 24);
  p[1] = uint8_t(v >> 16);
  p[2] = uint8_t(v >> 8);
  p[3] = uint8_t(v);
}
void Store64(uint8_t* p, uint64_t v) {
  Store32(p, uint32_t(v >> 32));
  Store32(p + 4, uint32_t(v));
}

uint64_t RandomU64() {
  static std::mutex mutex;
  static std::mt19937_64 rng(std::random_device{}());
  std::lock_guard<std::mutex> lock(mutex);
  return rng();
}

uint64_t FileTimeNow() {
  using namespace std::chrono;
  const uint64_t unix_100ns =
      duration_cast<nanoseconds>(system_clock::now().time_since_epoch()).count() / 100;
  return unix_100ns + 116444736000000000ull;
}

// The value of argument `index` of an X_ARGUMENT_LIST-style block: 16-byte entries
// {native size u32, pad, value pointer u64}.
uint32_t ArgPointer(const uint8_t* base, uint32_t args, int index) {
  return Load32(GuestPtr(base, args + 16 * index + 12));
}

// The presence our player shows when not hosting a joinable session.
void SetIdlePresence() {
  reach::LiveSetPresence(kFriendOnline | kFriendPlaying, nullptr, "Halo: Reach");
}

// --- XLiveBase -------------------------------------------------------------------

// XStringVerify: the message is an XLIVEBASE_ASYNC_MESSAGE whose task holds a
// marshalled request (title id, flags, locale size u16, string count u16 — the wire
// format is little-endian) and a results buffer: STRING_VERIFY_RESPONSE {count u16,
// results pointer u32} followed by one HRESULT per string.
uint32_t XStringVerify(uint8_t* base, uint32_t message) {
  if (!message) return kInvalidArg;
  const uint32_t task = Load32(GuestPtr(base, message));
  if (!task) return kInvalidArg;
  const uint32_t request = Load32(GuestPtr(base, task + 0x18)), request_size = Load32(GuestPtr(base, task + 0x1C));
  const uint32_t results = Load32(GuestPtr(base, task + 0x2C)), results_size = Load32(GuestPtr(base, task + 0x30));
  if (!request || request_size < 12 || !results || results_size < 6) return kInvalidArg;
  uint32_t count = base[request + 10] | base[request + 11] << 8;
  count = std::min<uint32_t>(count, (results_size - 6) / 4);
  std::memset(GuestPtr(base, results), 0, results_size);
  base[results] = uint8_t(count >> 8);
  base[results + 1] = uint8_t(count);
  Store32(GuestPtr(base, results + 2), results + 6);  // all results stay S_OK
  return kSuccess;
}

// XFriendsCreateEnumerator(user, first, count, buffer size out, handle out): arguments
// in an X_ARGUMENT_LIST. Items are X_ONLINE_FRIEND.
uint32_t XFriendsCreateEnumerator(uint8_t* base, uint32_t args) {
  using namespace rex;
  using namespace rex::system;
  if (!args) return kInvalidArg;
  const uint32_t first_ptr = ArgPointer(base, args, 1), count_ptr = ArgPointer(base, args, 2);
  const uint32_t size_out = ArgPointer(base, args, 3), handle_out = ArgPointer(base, args, 4);
  if (!handle_out || !size_out || !count_ptr) return kInvalidArg;
  Store32(GuestPtr(base, handle_out), 0);
  const uint32_t first = first_ptr ? Load32(GuestPtr(base, first_ptr)) : 0;
  const uint32_t count = std::min<uint32_t>(Load32(GuestPtr(base, count_ptr)), 100);
  if (!count) return kInvalidArg;
  Store32(GuestPtr(base, size_out), count * kFriendSize);

  auto e = make_object<XStaticUntypedEnumerator>(REX_KERNEL_STATE(), count, kFriendSize);
  if (XFAILED(e->Initialize(0xFFFFFFFF, kAppXLiveBase, 0x58021, 0x58022, 0))) return kInvalidArg;
  const auto roster = reach::LiveRoster();
  const uint64_t now = FileTimeNow();
  for (size_t i = first; i < roster.size() && e->item_count() < count; ++i) {
    const reach::LiveFriend& f = roster[i];
    uint8_t* item = e->AppendItem();
    std::memset(item, 0, kFriendSize);
    Store64(item, f.xuid);
    std::memcpy(item + 8, f.gamertag.data(), std::min<size_t>(f.gamertag.size(), 15));
    Store32(item + 0x18, f.state);
    std::memcpy(item + 0x1C, f.session, 8);  // session id
    if (f.state & kFriendPlaying) Store32(item + 0x24, kTitleId);
    Store64(item + 0x28, now);
    const size_t chars = std::min<size_t>(f.status.size(), 63);
    for (size_t c = 0; c < chars; ++c) item[0x45 + 2 * c] = uint8_t(f.status[c]);  // UTF-16BE
    Store32(item + 0x40, uint32_t(chars ? chars + 1 : 0));
  }
  if (Trace()) {
    REXLOG_INFO("REACH_LIVE: friends list: {} of {} players", e->item_count(), roster.size());
  }
  Store32(GuestPtr(base, handle_out), e->handle());
  return kSuccess;
}

// One X_ONLINE_PRESENCE (0xA4 bytes): XUID, state, session id, title id, time, rich
// presence (X_ONLINE_FRIEND has the same fields with the gamertag after the XUID).
constexpr size_t kPresenceSize = 0xA4;

void WritePresence(uint8_t* item, const reach::LiveFriend& f) {
  std::memset(item, 0, kPresenceSize);
  Store64(item, f.xuid);
  Store32(item + 0x8, f.state);
  std::memcpy(item + 0xC, f.session, 8);  // session id
  if (f.state & kFriendPlaying) Store32(item + 0x14, kTitleId);
  Store64(item + 0x18, FileTimeNow());
  const size_t chars = std::min<size_t>(f.status.size(), 63);
  for (size_t c = 0; c < chars; ++c) item[0x25 + 2 * c] = uint8_t(f.status[c]);  // UTF-16BE
  Store32(item + 0x20, uint32_t(chars ? chars + 1 : 0));
}

// XPresenceCreateEnumerator(user, peer count, peer XUIDs, first, max, buffer size out,
// handle out): the presence of each requested player we know (the room's players).
uint32_t XPresenceCreateEnumerator(uint8_t* base, uint32_t args) {
  using namespace rex;
  using namespace rex::system;
  if (!args) return kInvalidArg;
  const uint32_t count_ptr = ArgPointer(base, args, 1), xuids = ArgPointer(base, args, 2);
  const uint32_t first_ptr = ArgPointer(base, args, 3);
  const uint32_t size_out = ArgPointer(base, args, 5), handle_out = ArgPointer(base, args, 6);
  if (!handle_out || !size_out || !count_ptr || !xuids) return kInvalidArg;
  Store32(GuestPtr(base, handle_out), 0);
  const uint32_t count = std::min<uint32_t>(Load32(GuestPtr(base, count_ptr)), 100);
  const uint32_t first = first_ptr ? Load32(GuestPtr(base, first_ptr)) : 0;
  if (!count) return kInvalidArg;
  Store32(GuestPtr(base, size_out), count * kPresenceSize);
  auto e = make_object<XStaticUntypedEnumerator>(REX_KERNEL_STATE(), count, kPresenceSize);
  if (XFAILED(e->Initialize(0, kAppXLiveBase, 0x5801A, 0x5801B, 0))) return kInvalidArg;
  const auto roster = reach::LiveRoster();
  for (uint32_t i = first; i < count; ++i) {
    const uint64_t xuid =
        uint64_t(Load32(GuestPtr(base, xuids + 8 * i))) << 32 | Load32(GuestPtr(base, xuids + 8 * i + 4));
    for (const auto& f : roster) {
      if (f.xuid == xuid) {
        WritePresence(e->AppendItem(), f);
        break;
      }
    }
  }
  if (Trace()) {
    REXLOG_INFO("REACH_LIVE: presence of {} players: {} known", count, e->item_count());
  }
  Store32(GuestPtr(base, handle_out), e->handle());
  return kSuccess;
}

// --- XGI sessions ----------------------------------------------------------------

struct HostedSession {
  uint8_t info[kSessionInfoSize];
  uint32_t flags;
  uint32_t max_public, max_private;     // slots
  uint32_t filled_public, filled_private;
  uint64_t order;                       // creation order: the newest presence session wins
};
std::mutex sessions_mutex;
std::unordered_map<uint32_t, HostedSession> hosted_sessions;  // by session object
std::unordered_map<uint64_t, std::vector<uint8_t>> qos_data;  // XNetQosListen data by key id
uint64_t session_order = 0;
uint8_t joined_session[kSessionInfoSize] = {};  // the last session we joined as a guest

uint64_t SessionId(const uint8_t* info) {
  return uint64_t(Load32(info)) << 32 | Load32(info + 4);
}

void PublishMatchLocked();  // Matchmaking, below

// The bytes our presence carries after the status line (friends read them back in
// LiveFriend::extra): slots (max public, max private, filled public, filled private, one
// byte each), then the session's QoS data (u16 length + bytes).
std::vector<uint8_t> PresenceExtra(const HostedSession& s) {
  std::vector<uint8_t> extra = {uint8_t(std::min<uint32_t>(s.max_public, 255)),
                                uint8_t(std::min<uint32_t>(s.max_private, 255)),
                                uint8_t(std::min<uint32_t>(s.filled_public, 255)),
                                uint8_t(std::min<uint32_t>(s.filled_private, 255))};
  auto it = qos_data.find(SessionId(s.info));
  const size_t size = it == qos_data.end() ? 0 : std::min<size_t>(it->second.size(), 1000);
  extra.push_back(uint8_t(size >> 8));
  extra.push_back(uint8_t(size));
  if (size) extra.insert(extra.end(), it->second.begin(), it->second.begin() + size);
  return extra;
}

// Publishes the newest joinable presence session we host, or idle presence. Needs
// sessions_mutex.
void PublishPresenceLocked() {
  const HostedSession* best = nullptr;
  for (const auto& [object, s] : hosted_sessions) {
    const bool joinable = (s.flags & kSessionUsesPresence) &&
                          !(s.flags & kSessionJoinViaPresenceDisabled);
    if (joinable && (!best || s.order > best->order)) best = &s;
  }
  if (!best) {
    SetIdlePresence();
    return;
  }
  uint32_t state = kFriendOnline | kFriendPlaying | kFriendJoinable;
  if (best->flags & kSessionJoinViaPresenceFriendsOnly) state |= kFriendJoinableFriendsOnly;
  reach::LiveSetPresence(state, best->info, "Halo: Reach", PresenceExtra(*best));
}

// XSessionCreate {session object, flags, public slots, private slots, user index,
// XSESSION_INFO*, nonce*}: for a session we host, fill the info before the SDK
// completes the request.
void XSessionCreate(uint8_t* base, uint32_t buffer) {
  const uint32_t object = Load32(GuestPtr(base, buffer)), flags = Load32(GuestPtr(base, buffer + 4));
  const uint32_t info_ptr = Load32(GuestPtr(base, buffer + 20)), nonce_ptr = Load32(GuestPtr(base, buffer + 24));
  if (!info_ptr) return;
  if (!(flags & kSessionHost)) {  // joining someone's session: remember it for invites
    std::lock_guard<std::mutex> lock(sessions_mutex);
    std::memcpy(joined_session, GuestPtr(base, info_ptr), kSessionInfoSize);
    return;
  }
  HostedSession s{};
  s.flags = flags;
  s.max_public = Load32(GuestPtr(base, buffer + 8));
  s.max_private = Load32(GuestPtr(base, buffer + 12));
  Store64(s.info, RandomU64());
  s.info[0] = uint8_t((s.info[0] & 0x0F) | 0x80);  // XNET_XNKID_ONLINE_PEER
  reach::LiveSelfXnAddr(s.info + 8);
  Store64(s.info + 0x2C, RandomU64());
  Store64(s.info + 0x34, RandomU64());
  std::memcpy(GuestPtr(base, info_ptr), s.info, kSessionInfoSize);
  if (nonce_ptr) Store64(GuestPtr(base, nonce_ptr), RandomU64());
  REXLOG_INFO("REACH_LIVE: hosting session {:016X} (flags {:08X}, {} public + {} private slots)",
              SessionId(s.info), flags, s.max_public, s.max_private);
  std::lock_guard<std::mutex> lock(sessions_mutex);
  s.order = ++session_order;
  hosted_sessions[object] = s;
  PublishPresenceLocked();
  PublishMatchLocked();
}

// XSessionDelete {session object, ...}
void XSessionDelete(uint8_t* base, uint32_t buffer) {
  std::lock_guard<std::mutex> lock(sessions_mutex);
  auto it = hosted_sessions.find(Load32(GuestPtr(base, buffer)));
  if (it == hosted_sessions.end()) return;
  qos_data.erase(SessionId(it->second.info));
  hosted_sessions.erase(it);
  PublishPresenceLocked();
  PublishMatchLocked();
}

// XSessionModify {session object, flags, public slots, private slots}: only the join
// flags (XSESSION_CREATE_MODIFIERS_MASK) change.
void XSessionModify(uint8_t* base, uint32_t buffer) {
  constexpr uint32_t kModifiers = 0xF00;
  std::lock_guard<std::mutex> lock(sessions_mutex);
  auto it = hosted_sessions.find(Load32(GuestPtr(base, buffer)));
  if (it == hosted_sessions.end()) return;
  it->second.flags = (it->second.flags & ~kModifiers) | (Load32(GuestPtr(base, buffer + 4)) & kModifiers);
  it->second.max_public = Load32(GuestPtr(base, buffer + 8));
  it->second.max_private = Load32(GuestPtr(base, buffer + 12));
  PublishPresenceLocked();
  PublishMatchLocked();
}

// XSessionJoin{Local,Remote} (0xB0012) / Leave (0xB0013) {session object, user count,
// XUIDs (null: local users), user indices, private-slot flags}: keeps the slot counts.
void XSessionMembers(uint8_t* base, uint32_t buffer, bool join) {
  const uint32_t count = Load32(GuestPtr(base, buffer + 4)), privates = Load32(GuestPtr(base, buffer + 16));
  std::lock_guard<std::mutex> lock(sessions_mutex);
  auto it = hosted_sessions.find(Load32(GuestPtr(base, buffer)));
  if (it == hosted_sessions.end()) return;
  HostedSession& s = it->second;
  for (uint32_t i = 0; i < count && i < 16; ++i) {
    const bool is_private = join && privates && Load32(GuestPtr(base, privates + 4 * i)) != 0;
    uint32_t& filled = is_private ? s.filled_private : s.filled_public;
    if (join) {
      ++filled;
    } else if (s.filled_public) {
      --s.filled_public;
    } else if (s.filled_private) {
      --s.filled_private;
    }
  }
  PublishPresenceLocked();
  PublishMatchLocked();
}

// XSessionSearchByIds {user, id count, ids, results size, results, ...} (0xB0060) and
// XSessionSearchByID {user, id (8 bytes), results size, results} (0xB001B): the sessions
// the room's players published. Results: XSESSION_SEARCHRESULT_HEADER {count, pointer}
// followed by 0x5C-byte XSESSION_SEARCHRESULTs (info, open public, open private,
// filled public, filled private, property count, context count, pointers).
uint32_t XSessionSearch(uint8_t* base, const std::vector<uint64_t>& ids, uint32_t results,
                        uint32_t results_size) {
  constexpr uint32_t kResultSize = 0x5C;
  if (!results || results_size < 8) return kInvalidArg;
  std::memset(GuestPtr(base, results), 0, results_size);
  Store32(GuestPtr(base, results + 4), results + 8);
  const auto roster = reach::LiveRoster();
  uint32_t found = 0;
  for (uint64_t id : ids) {
    for (const auto& f : roster) {
      if (!id || SessionId(f.session) != id) continue;
      if (8 + (found + 1) * kResultSize > results_size) break;
      uint8_t* r = GuestPtr(base, results + 8 + found * kResultSize);
      std::memcpy(r, f.session, kSessionInfoSize);
      if (f.extra.size() >= 4) {
        const uint32_t max_public = f.extra[0], max_private = f.extra[1];
        const uint32_t filled_public = f.extra[2], filled_private = f.extra[3];
        Store32(r + 0x3C, max_public > filled_public ? max_public - filled_public : 0);
        Store32(r + 0x40, max_private > filled_private ? max_private - filled_private : 0);
        Store32(r + 0x44, filled_public);
        Store32(r + 0x48, filled_private);
      }
      ++found;
      break;
    }
  }
  Store32(GuestPtr(base, results), found);
  if (Trace()) REXLOG_INFO("REACH_LIVE: session search: {} of {} found", found, ids.size());
  return kSuccess;
}

uint32_t XSessionSearchByIds(uint8_t* base, uint32_t buffer) {
  const uint32_t count = std::min<uint32_t>(Load32(GuestPtr(base, buffer + 4)), 100);
  const uint32_t ids_ptr = Load32(GuestPtr(base, buffer + 8));
  std::vector<uint64_t> ids;
  for (uint32_t i = 0; i < count && ids_ptr; ++i) {
    ids.push_back(uint64_t(Load32(GuestPtr(base, ids_ptr + 8 * i))) << 32 | Load32(GuestPtr(base, ids_ptr + 8 * i + 4)));
  }
  return XSessionSearch(base, ids, Load32(GuestPtr(base, buffer + 16)), Load32(GuestPtr(base, buffer + 12)));
}

uint32_t XSessionSearchById(uint8_t* base, uint32_t buffer) {
  const uint64_t id = uint64_t(Load32(GuestPtr(base, buffer + 4))) << 32 | Load32(GuestPtr(base, buffer + 8));
  return XSessionSearch(base, {id}, Load32(GuestPtr(base, buffer + 16)), Load32(GuestPtr(base, buffer + 12)));
}

// The session a friend we invite should join: the newest joinable session we host, else
// the one we joined. False when there is none.
bool InviteSession(uint8_t* out) {
  std::lock_guard<std::mutex> lock(sessions_mutex);
  const HostedSession* best = nullptr;
  for (const auto& [object, s] : hosted_sessions) {
    if ((s.flags & kSessionUsesPresence) && (!best || s.order > best->order)) best = &s;
  }
  if (best) {
    std::memcpy(out, best->info, kSessionInfoSize);
    return true;
  }
  std::memcpy(out, joined_session, kSessionInfoSize);
  for (size_t i = 0; i < 8; ++i) {
    if (out[i]) return true;
  }
  return false;
}

// XInviteSend (0xFC/0x50002): an XLIVEBASE_ASYNC_MESSAGE whose marshalled request is
// (big-endian) user index, invitee count, invitee XUIDs, display text, message handle.
uint32_t XInviteSend(uint8_t* base, uint32_t message) {
  if (!message) return kInvalidArg;
  const uint32_t task = Load32(GuestPtr(base, message));
  if (!task) return kInvalidArg;
  const uint32_t request = Load32(GuestPtr(base, task + 0x18)), size = Load32(GuestPtr(base, task + 0x1C));
  if (!request || size < 8) return kInvalidArg;
  const uint32_t count = std::min<uint32_t>(Load32(GuestPtr(base, request + 4)), (size - 8) / 8);
  uint8_t session[kSessionInfoSize];
  if (!InviteSession(session)) return 0x80155206;  // X_ONLINE_E_SESSION_NOT_FOUND
  for (uint32_t i = 0; i < count; ++i) {
    const uint64_t xuid =
        uint64_t(Load32(GuestPtr(base, request + 8 + 8 * i))) << 32 | Load32(GuestPtr(base, request + 12 + 8 * i));
    reach::LiveSendInvite(xuid, session);
    REXLOG_INFO("REACH_LIVE: invite sent to {:016X}", xuid);
  }
  return kSuccess;
}

// XInviteGetAcceptedInfo (0xFC/0x58023): (user index, X_INVITE_INFO*) in an argument
// list. X_INVITE_INFO: invitee XUID, inviter XUID, title id, the session (XSESSION_INFO),
// from-game-invite flag.
uint32_t XInviteGetAcceptedInfo(uint8_t* base, uint32_t args) {
  if (!args) return kInvalidArg;
  const uint32_t info = ArgPointer(base, args, 1);
  reach::LiveInvite invite;
  if (!info || !reach::LiveTakeInvite(invite)) return 0x80155206;  // session not found
  std::memset(GuestPtr(base, info), 0, 0x54);
  Store64(GuestPtr(base, info), reach::IdentityXuid());
  Store64(GuestPtr(base, info + 8), invite.inviter_xuid);
  Store32(GuestPtr(base, info + 0x10), kTitleId);
  std::memcpy(GuestPtr(base, info + 0x14), invite.session, kSessionInfoSize);
  Store32(GuestPtr(base, info + 0x50), 1);
  REXLOG_INFO("REACH_LIVE: joining {}'s game", invite.inviter);
  return kSuccess;
}

// --- Matchmaking ---------------------------------------------------------------------
//
// Matchmaking (docs/online_plan.md section 5.4) searches for sessions with
// XSessionSearchWeighted, filtered by the playlist (the searcher's property
// 0x10000015, the hopper id), and hosts one with XSessionCreate (flag
// XSESSION_CREATE_USES_MATCHMAKING) when it finds none. The session we host is published
// on the server with our playlist and the record below; a search asks the server for the
// room's sessions with the searched playlist. The record: XSESSION_INFO, slots (max
// public, max private, filled public, filled private: u8 each), our user properties
// (u8 count, then id u32, type u8, value 8 bytes) and contexts (u8 count, then id u32,
// value u32), the QoS data (u16 length + bytes).

constexpr uint32_t kSessionUsesMatchmaking = 0x8;
constexpr uint32_t kPropertyHopper = 0x10000015;

struct UserValue {
  uint8_t type = 0;
  uint8_t value[8] = {};
};
std::unordered_map<uint32_t, UserValue> user_properties;  // ours, by property id
std::unordered_map<uint32_t, uint32_t> user_contexts;     // by context id
bool match_published = false;
uint32_t match_playlist = 0;  // what we last searched for: the playlist our session is for

// A session a search found: its record, and the QoS data lookups answer with.
struct FoundSession {
  uint64_t xuid = 0;
  uint8_t info[kSessionInfoSize] = {};
  uint8_t slots[4] = {};
  std::vector<std::pair<uint32_t, UserValue>> properties;
  std::vector<std::pair<uint32_t, uint32_t>> contexts;
  std::vector<uint8_t> qos;
};
std::mutex match_mutex;
std::condition_variable match_answered;
uint32_t match_request = 0, match_answer = 0;
std::vector<FoundSession> match_results;                    // of the last answer
std::unordered_map<uint64_t, std::vector<uint8_t>> found_qos;  // by session id

// XUserSetProperty {user index, unused, XUID, property id, size, value pointer}: the
// property's type is its id's top nibble; types with an inline value are kept.
void XUserSetProperty(uint8_t* base, uint32_t buffer) {
  const uint32_t id = Load32(GuestPtr(base, buffer + 16)), size = Load32(GuestPtr(base, buffer + 20));
  const uint32_t value = Load32(GuestPtr(base, buffer + 24));
  const uint8_t type = uint8_t(id >> 28);
  if (!value || type == 4 || type == 6 || size > 8) return;  // strings, binary
  UserValue v;
  v.type = type;
  std::memcpy(v.value, GuestPtr(base, value), size);
  std::lock_guard<std::mutex> lock(sessions_mutex);
  user_properties[id] = v;
}

// XUserSetContext {user index, unused, XUID, context id, value}
void XUserSetContext(uint8_t* base, uint32_t buffer) {
  std::lock_guard<std::mutex> lock(sessions_mutex);
  user_contexts[Load32(GuestPtr(base, buffer + 16))] = Load32(GuestPtr(base, buffer + 20));
}

// Our session's playlist: the property if we set it, else what we search for (Reach
// sets the hopper id only on its searches).
uint32_t PlaylistLocked() {
  auto it = user_properties.find(kPropertyHopper);
  return it == user_properties.end() ? match_playlist : Load32(it->second.value);
}

// Publishes the newest matchmaking session we host, or withdraws ours. Needs
// sessions_mutex.
void PublishMatchLocked() {
  const HostedSession* best = nullptr;
  for (const auto& [object, s] : hosted_sessions) {
    if ((s.flags & kSessionUsesMatchmaking) && (!best || s.order > best->order)) best = &s;
  }
  if (!best) {
    if (match_published) reach::LiveSendMatch(false, {0});
    match_published = false;
    return;
  }
  std::vector<uint8_t> body(5);
  body[0] = 1;
  Store32(body.data() + 1, PlaylistLocked());
  body.insert(body.end(), best->info, best->info + kSessionInfoSize);
  for (uint32_t n : {best->max_public, best->max_private, best->filled_public, best->filled_private}) {
    body.push_back(uint8_t(std::min<uint32_t>(n, 255)));
  }
  body.push_back(uint8_t(std::min<size_t>(user_properties.size(), 32)));
  size_t count = 0;
  for (const auto& [id, v] : user_properties) {
    if (count++ == 32) break;
    body.resize(body.size() + 13);
    uint8_t* p = body.data() + body.size() - 13;
    Store32(p, id);
    p[4] = v.type;
    std::memcpy(p + 5, v.value, 8);
  }
  body.push_back(uint8_t(std::min<size_t>(user_contexts.size(), 32)));
  count = 0;
  for (const auto& [id, value] : user_contexts) {
    if (count++ == 32) break;
    body.resize(body.size() + 8);
    Store32(body.data() + body.size() - 8, id);
    Store32(body.data() + body.size() - 4, value);
  }
  auto it = qos_data.find(SessionId(best->info));
  const size_t qos = it == qos_data.end() ? 0 : std::min<size_t>(it->second.size(), 1000);
  body.push_back(uint8_t(qos >> 8));
  body.push_back(uint8_t(qos));
  if (qos) body.insert(body.end(), it->second.begin(), it->second.begin() + qos);
  reach::LiveSendMatch(false, body);
  match_published = true;
}

// Parses one record of a search answer; false when it is cut short.
bool ParseRecord(const uint8_t* p, size_t n, FoundSession& s) {
  size_t pos = kSessionInfoSize + 4;
  if (n < pos + 1) return false;
  std::memcpy(s.info, p, kSessionInfoSize);
  std::memcpy(s.slots, p + kSessionInfoSize, 4);
  for (size_t i = 0, count = p[pos++]; i < count; ++i, pos += 13) {
    if (pos + 13 > n) return false;
    UserValue v;
    v.type = p[pos + 4];
    std::memcpy(v.value, p + pos + 5, 8);
    s.properties.emplace_back(Load32(p + pos), v);
  }
  if (pos + 1 > n) return false;
  for (size_t i = 0, count = p[pos++]; i < count; ++i, pos += 8) {
    if (pos + 8 > n) return false;
    s.contexts.emplace_back(Load32(p + pos), Load32(p + pos + 4));
  }
  if (pos + 2 > n) return false;
  const size_t qos = size_t(p[pos]) << 8 | p[pos + 1];
  if (pos + 2 + qos > n) return false;
  s.qos.assign(p + pos + 2, p + pos + 2 + qos);
  return true;
}

// XSessionSearchWeighted {procedure, user, max results, weighted property count u16,
// weighted context count u16, their pointers, property count u16, context count u16,
// properties, contexts, results size, results}. XUSER_PROPERTY is {id, pad, XUSER_DATA
// {type u8, pad, value at +8}} (0x18 bytes), XUSER_CONTEXT {id, value}. Results as
// XSessionSearch above, with each session's properties and contexts after them.
uint32_t XSessionSearchWeighted(uint8_t* base, uint32_t buffer) {
  constexpr uint32_t kResultSize = 0x5C, kPropertySize = 0x18;
  const uint32_t max_results = Load32(GuestPtr(base, buffer + 8));
  const uint32_t property_count = Load32(GuestPtr(base, buffer + 0x18)) >> 16;
  const uint32_t properties = Load32(GuestPtr(base, buffer + 0x1C));
  const uint32_t results_size = Load32(GuestPtr(base, buffer + 0x24)), results = Load32(GuestPtr(base, buffer + 0x28));
  if (!results || results_size < 8) return kInvalidArg;
  uint32_t playlist = 0;
  for (uint32_t i = 0; i < property_count && i < 64 && properties; ++i) {
    const uint8_t* prop = GuestPtr(base, properties + kPropertySize * i);
    if (Load32(prop) == kPropertyHopper) playlist = Load32(prop + 0x10);
  }
  {
    std::lock_guard<std::mutex> lock(sessions_mutex);
    if (playlist != match_playlist) {
      match_playlist = playlist;
      PublishMatchLocked();
    }
  }
  std::vector<FoundSession> found;
  {
    std::unique_lock<std::mutex> lock(match_mutex);
    const uint32_t request = ++match_request;
    std::vector<uint8_t> body(8);
    Store32(body.data(), playlist);
    Store32(body.data() + 4, request);
    reach::LiveSendMatch(true, body);
    if (match_answered.wait_for(lock, std::chrono::milliseconds(500),
                                [&] { return match_answer == request; })) {
      found = match_results;
    }
  }
  std::memset(GuestPtr(base, results), 0, results_size);
  const uint32_t count = std::min<uint32_t>(
      {uint32_t(found.size()), max_results, (results_size - 8) / kResultSize});
  Store32(GuestPtr(base, results), count);
  Store32(GuestPtr(base, results + 4), results + 8);
  uint32_t extra = results + 8 + count * kResultSize;  // properties and contexts go here
  const uint32_t end = results + results_size;
  for (uint32_t i = 0; i < count; ++i) {
    const FoundSession& s = found[i];
    uint8_t* r = GuestPtr(base, results + 8 + i * kResultSize);
    std::memcpy(r, s.info, kSessionInfoSize);
    Store32(r + 0x3C, s.slots[0] > s.slots[2] ? s.slots[0] - s.slots[2] : 0);
    Store32(r + 0x40, s.slots[1] > s.slots[3] ? s.slots[1] - s.slots[3] : 0);
    Store32(r + 0x44, s.slots[2]);
    Store32(r + 0x48, s.slots[3]);
    const uint32_t props_size = uint32_t(s.properties.size()) * kPropertySize;
    const uint32_t contexts_size = uint32_t(s.contexts.size()) * 8;
    if (extra + props_size + contexts_size > end) continue;
    Store32(r + 0x4C, uint32_t(s.properties.size()));
    Store32(r + 0x50, uint32_t(s.contexts.size()));
    Store32(r + 0x54, extra);
    for (const auto& [id, v] : s.properties) {
      Store32(GuestPtr(base, extra), id);
      base[extra + 8] = v.type;
      std::memcpy(GuestPtr(base, extra + 0x10), v.value, 8);
      extra += kPropertySize;
    }
    Store32(r + 0x58, extra);
    for (const auto& [id, value] : s.contexts) {
      Store32(GuestPtr(base, extra), id);
      Store32(GuestPtr(base, extra + 4), value);
      extra += 8;
    }
  }
  REXLOG_INFO("REACH_LIVE: matchmaking search (playlist {}): {} sessions", playlist, count);
  return kSuccess;
}

// Our part of a message: kNotOurs lets the SDK answer (after any bookkeeping above).
uint32_t Handle(uint8_t* base, uint32_t app, uint32_t message, uint32_t arg1, uint32_t arg2) {
  if (!reach::LiveSignin()) return kNotOurs;
  static std::once_flag presence_once;
  std::call_once(presence_once, SetIdlePresence);
  if (app == kAppXLiveBase) {
    switch (message) {
      case 0x0005000C:
        return XStringVerify(base, arg1);
      case 0x00058020:
        return XFriendsCreateEnumerator(base, arg2);
      case 0x00058019:
        return XPresenceCreateEnumerator(base, arg2);
      case 0x00050002:
        return XInviteSend(base, arg1);
      case 0x00058023:
        return XInviteGetAcceptedInfo(base, arg2);
      case 0x0005801E:  // XPresenceSubscribe: every room player's presence is known
      case 0x00058044:  // XPresenceUnsubscribe
      // XUserMuteListQuery {user index, XUID}: nobody is muted (the result stays false).
      // In a matchmade game Reach asks it for every other player several times a second;
      // the SDK fails it and logs each call.
      case 0x0005800E:
        return kSuccess;
    }
  } else if (app == kAppXgi && arg1) {
    switch (message) {
      case 0x000B0010:
        XSessionCreate(base, arg1);
        break;
      case 0x000B0011:
        XSessionDelete(base, arg1);
        break;
      case 0x000B0018:
        XSessionModify(base, arg1);
        break;
      case 0x000B0012:
        XSessionMembers(base, arg1, true);
        break;
      case 0x000B0013:  // XSessionLeave: the SDK has no handler and fails it
        XSessionMembers(base, arg1, false);
        return kSuccess;
      case 0x000B001B:
        return XSessionSearchById(base, arg1);
      case 0x000B0060:
        return XSessionSearchByIds(base, arg1);
      case 0x000B0006:
        XUserSetContext(base, arg1);
        break;
      case 0x000B0007:
        XUserSetProperty(base, arg1);
        break;
      case 0x000B0065:
        return XSessionSearchWeighted(base, arg1);
    }
  }
  return kNotOurs;
}

void LogMessage(const char* name, const uint32_t (&in)[6], uint32_t result, bool ours) {
  if (!Trace() || in[0] == 0xFA) return;  // the music player is noise
  REXLOG_INFO("NETTRACE {}(app {:02X}, msg {:08X}, {:08X}, {:08X}, {:08X}, {:08X}) -> {:08X}{}",
              name, in[0], in[1], in[2], in[3], in[4], in[5], result, ours ? " (REACH_LIVE)" : "");
}

GuestFunc Sdk(const char* name) { return reach::SdkImport(name); }

}  // namespace

// DWORD XMsgInProcessCall(DWORD app, DWORD message, void* arg1, void* arg2)
extern "C" REX_FUNC(__imp__XMsgInProcessCall) {
  static GuestFunc sdk = Sdk("__imp__XMsgInProcessCall");
  const uint32_t in[6] = {ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, ctx.r7.u32, ctx.r8.u32};
  // The SDK's content-aggregate enumeration (XAM app 0xFE, message 0x2000E) writes the item
  // count only when it finds an item, and answers NO_MORE_FILES without touching it.
  // Reach's task (0x828057B0) points it at an uninitialized stack slot and passes what it
  // finds there on as the count; a leftover 0xBEBEBEBE read as "a full buffer", so the
  // enumeration never moved to the next device and Firefight spun in it forever at
  // "Readying map".
  if (in[0] == 0xFE && in[1] == 0x2000E && in[2]) {
    const uint32_t length_ptr = Load32(GuestPtr(base, in[2] + 24));
    if (length_ptr) Store32(GuestPtr(base, length_ptr), 0);
  }
  // XMPGetPlaybackController (music player app 0xFA, message 0x7001B; buffer: client,
  // controller*, locked*): no controller, not locked, as the SDK answers, but without the
  // SDK's 10 ms sleep on threads other than the main thread (meant for a game that polls it
  // in a tight loop). Reach's AUDIO thread asks during every frame (about 20 ms of sleeps)
  // and the main thread waits for it, which held the game to 45 fps (docs/framerate.md).
  if (in[0] == 0xFA && in[1] == 0x7001B && in[2]) {
    const uint32_t controller_ptr = Load32(GuestPtr(base, in[2] + 4));
    const uint32_t locked_ptr = Load32(GuestPtr(base, in[2] + 8));
    if (controller_ptr) Store32(GuestPtr(base, controller_ptr), 0);
    if (locked_ptr) Store32(GuestPtr(base, locked_ptr), 0);
    ctx.r3.u64 = 0;  // X_E_SUCCESS
    return;
  }
  const uint32_t result = Handle(base, in[0], in[1], in[2], in[3]);
  if (result != kNotOurs) {
    ctx.r3.u64 = result;
  } else if (sdk) {
    sdk(ctx, base);
  }
  LogMessage("XMsgInProcessCall", in, ctx.r3.u32, result != kNotOurs);
}

namespace {
// XMsgStartIORequest[Ex](app, message, XOVERLAPPED*, buffer, buffer size[, unknown]):
// like the SDK, a request is answered at once and its XOVERLAPPED completed with the
// result.
void StartIORequest(const char* name, GuestFunc sdk, PPCContext& ctx, uint8_t* base) {
  const uint32_t in[6] = {ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, ctx.r7.u32, ctx.r8.u32};
  const uint32_t overlapped = in[2];
  const uint32_t result = Handle(base, in[0], in[1], in[3], in[4]);
  if (result == kNotOurs) {
    if (sdk) sdk(ctx, base);
    LogMessage(name, in, ctx.r3.u32, false);
    return;
  }
  if (overlapped) {
    REX_KERNEL_STATE()->CompleteOverlappedImmediate(overlapped, result);
    ctx.r3.u64 = kIoPending;
  } else {
    ctx.r3.u64 = result;
  }
  rex::system::XThread::SetLastError(0);
  LogMessage(name, in, ctx.r3.u32, true);
}
}  // namespace

extern "C" REX_FUNC(__imp__XMsgStartIORequest) {
  static GuestFunc sdk = Sdk("__imp__XMsgStartIORequest");
  StartIORequest("XMsgStartIORequest", sdk, ctx, base);
}

extern "C" REX_FUNC(__imp__XMsgStartIORequestEx) {
  static GuestFunc sdk = Sdk("__imp__XMsgStartIORequestEx");
  StartIORequest("XMsgStartIORequestEx", sdk, ctx, base);
}

namespace reach {
// An invite arrived: accept it for the player (XN_LIVE_INVITE_ACCEPTED, parameter: the
// user index); the game then asks XInviteGetAcceptedInfo for it and joins, or tells the
// player it can't right now.
void LiveInviteReceived(const LiveInvite& invite) {
  REXLOG_INFO("REACH_LIVE: {} invited you{}", invite.inviter,
              REXCVAR_GET(live_accept_invites) ? "; accepting" : " (live_accept_invites is off)");
  if (!LiveSignin() || !REXCVAR_GET(live_accept_invites)) return;
  if (auto* kernel = REX_KERNEL_STATE()) kernel->BroadcastNotification(0x02000002, 0);
}

// The server's answer to a matchmaking search: request u32, count u16, then per session
// the host's id u32, XUID u64 and record (u16 length + bytes).
void LiveMatchResults(const uint8_t* p, size_t n) {
  if (n < 6) return;
  std::vector<FoundSession> found;
  size_t pos = 6;
  for (uint32_t i = 0, count = uint32_t(p[4]) << 8 | p[5]; i < count; ++i) {
    if (pos + 14 > n) break;
    const size_t size = size_t(p[pos + 12]) << 8 | p[pos + 13];
    if (pos + 14 + size > n) break;
    FoundSession s;
    s.xuid = uint64_t(Load32(p + pos + 4)) << 32 | Load32(p + pos + 8);
    if (ParseRecord(p + pos + 14, size, s)) found.push_back(std::move(s));
    pos += 14 + size;
  }
  std::lock_guard<std::mutex> lock(match_mutex);
  for (const auto& s : found) found_qos[SessionId(s.info)] = s.qos;
  match_results = std::move(found);
  match_answer = Load32(p);
  match_answered.notify_all();
}

// Notifications for the game's friends code (XN_FRIENDS_PRESENCE_CHANGED,
// XN_FRIENDS_FRIEND_ADDED; the parameter is the user index).
void LiveRosterChanged(bool membership_changed) {
  if (!LiveSignin()) return;
  auto* kernel = REX_KERNEL_STATE();
  if (!kernel) return;
  if (membership_changed) kernel->BroadcastNotification(0x04000002, 0);
  kernel->BroadcastNotification(0x04000001, 0);
  if (Trace()) REXLOG_INFO("REACH_LIVE: friends changed{}", membership_changed ? " (came/went)" : "");
}
}  // namespace reach

// DWORD XamUserAreUsersFriends(DWORD user_index, XUID* xuids, DWORD count,
//                              BOOL* are_friends, XOVERLAPPED* overlapped)
// Players in our room are friends.
extern "C" REX_FUNC(__imp__XamUserAreUsersFriends) {
  static GuestFunc sdk = Sdk("__imp__XamUserAreUsersFriends");
  if (!reach::LiveSignin()) {
    if (sdk) sdk(ctx, base);
    return;
  }
  const uint32_t xuids = ctx.r4.u32, count = ctx.r5.u32, result = ctx.r6.u32;
  const uint32_t overlapped = ctx.r7.u32;
  const auto roster = reach::LiveRoster();
  bool all = count > 0;
  for (uint32_t i = 0; i < count && xuids; ++i) {
    const uint64_t xuid = uint64_t(Load32(GuestPtr(base, xuids + 8 * i))) << 32 | Load32(GuestPtr(base, xuids + 8 * i + 4));
    bool found = false;
    for (const auto& f : roster) found = found || f.xuid == xuid;
    all = all && found;
  }
  if (result) Store32(GuestPtr(base, result), all ? 1 : 0);
  if (overlapped) {
    REX_KERNEL_STATE()->CompleteOverlappedImmediate(overlapped, kSuccess);
    ctx.r3.u64 = kIoPending;
  } else {
    ctx.r3.u64 = kSuccess;
  }
}

// --- QoS ---------------------------------------------------------------------------

// INT XNetQosListen(const XNKID* id, const BYTE* data, UINT size, DWORD bits_per_sec,
//                   DWORD flags): a host publishes its game description (Reach: the
// same bitstream as the System Link reply) for players probing it. We keep the data
// per key id and send it with our presence.
extern "C" REX_FUNC(__imp__NetDll_XNetQosListen) {
  static GuestFunc sdk = Sdk("__imp__NetDll_XNetQosListen");
  if (!reach::NetworkOn()) {
    if (sdk) sdk(ctx, base);
    return;
  }
  constexpr uint32_t kSetData = 0x4, kDisable = 0x2, kRelease = 0x10;
  const uint32_t id_ptr = ctx.r4.u32, data = ctx.r5.u32, size = ctx.r6.u32, flags = ctx.r8.u32;
  if (reach::LiveSignin() && id_ptr) {
    const uint64_t id = uint64_t(Load32(GuestPtr(base, id_ptr))) << 32 | Load32(GuestPtr(base, id_ptr + 4));
    std::lock_guard<std::mutex> lock(sessions_mutex);
    if ((flags & kSetData) && data) {
      qos_data[id].assign(GuestPtr(base, data), GuestPtr(base, data) + std::min<uint32_t>(size, 1000));
    }
    if (flags & (kDisable | kRelease)) qos_data.erase(id);
    PublishPresenceLocked();
    PublishMatchLocked();
  }
  ctx.r3.u64 = 0;
}

// INT XNetQosLookup(UINT cxna, const XNADDR* apxna[], const XNKID* apxnkid[],
//     const XNKEY* apxnkey[], UINT cina, const IN_ADDR aina[], const DWORD
//     adwServiceId[], UINT cProbes, DWORD dwBitsPerSec, DWORD dwFlags, HANDLE hEvent,
//     XNQOS** ppxnqos): probing hosts before joining. Answered at once from the room's
// roster: each Live host's published QoS data, a nominal round trip and bandwidth.
// XNetQosRelease (the SDK) frees the result with SystemHeapFree.
extern "C" REX_FUNC(__imp__NetDll_XNetQosLookup) {
  static GuestFunc sdk = Sdk("__imp__NetDll_XNetQosLookup");
  if (!reach::LiveSignin()) {
    if (sdk) sdk(ctx, base);
    return;
  }
  auto stack_arg = [&](int index) { return Load32(GuestPtr(base, ctx.r1.u32 + 0x54 + (index - 8) * 8)); };
  const uint32_t count = std::min<uint32_t>(ctx.r4.u32, 64), xnas = ctx.r5.u32;
  const uint32_t xnkids = ctx.r6.u32;
  const uint32_t probes = stack_arg(8), event = stack_arg(11), out = stack_arg(12);
  const auto roster = reach::LiveRoster();
  std::vector<std::vector<uint8_t>> data(count);
  std::vector<bool> known(count, false);
  for (uint32_t i = 0; i < count && xnas; ++i) {
    const uint32_t xna = Load32(GuestPtr(base, xnas + 4 * i));
    if (!xna) continue;
    const uint32_t ina = Load32(GuestPtr(base, xna));
    if ((ina & 0xFF000000) != 0xF0000000) continue;  // not a Reach Live player
    if (const uint32_t kid = xnkids ? Load32(GuestPtr(base, xnkids + 4 * i)) : 0) {
      std::lock_guard<std::mutex> lock(match_mutex);  // a matchmaking session we found
      auto it = found_qos.find(uint64_t(Load32(GuestPtr(base, kid))) << 32 | Load32(GuestPtr(base, kid + 4)));
      if (it != found_qos.end()) {
        known[i] = true;
        data[i] = it->second;
        continue;
      }
    }
    for (const auto& f : roster) {
      if (f.id != (ina & 0x00FFFFFF)) continue;
      known[i] = true;
      if (f.extra.size() >= 6) {
        const size_t size = std::min<size_t>(f.extra[4] << 8 | f.extra[5], f.extra.size() - 6);
        data[i].assign(f.extra.begin() + 6, f.extra.begin() + 6 + size);
      }
      break;
    }
  }
  constexpr uint32_t kInfoSize = 0x18;
  uint32_t total = 8 + kInfoSize * std::max<uint32_t>(count, 1);
  for (const auto& d : data) total += uint32_t(d.size());
  const uint32_t qos = REX_KERNEL_MEMORY()->SystemHeapAlloc(total);
  if (!qos) {
    ctx.r3.u64 = 0x8007000E;  // E_OUTOFMEMORY
    return;
  }
  std::memset(GuestPtr(base, qos), 0, total);
  Store32(GuestPtr(base, qos), count);  // cxnqos; cxnqosPending stays 0
  uint32_t data_ptr = qos + 8 + kInfoSize * std::max<uint32_t>(count, 1);
  for (uint32_t i = 0; i < count; ++i) {
    uint8_t* info = GuestPtr(base, qos + 8 + kInfoSize * i);
    // XNQOSINFO: flags, reserved, probes sent u16, probes received u16, data size u16,
    // data pointer, RTT min u16, RTT median u16, up and down bits per second.
    const uint16_t sent = uint16_t(std::max<uint32_t>(probes, 1));
    info[0] = known[i] ? uint8_t(0x01 | 0x02 | (data[i].empty() ? 0 : 0x08)) : 0x01;
    info[2] = uint8_t(sent >> 8);
    info[3] = uint8_t(sent);
    if (known[i]) {
      info[4] = uint8_t(sent >> 8);
      info[5] = uint8_t(sent);
      info[6] = uint8_t(data[i].size() >> 8);
      info[7] = uint8_t(data[i].size());
      if (!data[i].empty()) {
        Store32(info + 8, data_ptr);
        std::memcpy(GuestPtr(base, data_ptr), data[i].data(), data[i].size());
        data_ptr += uint32_t(data[i].size());
      }
      info[0xD] = 20;  // ms
      info[0xF] = 25;
      Store32(info + 0x10, 2000000);
      Store32(info + 0x14, 2000000);
    }
  }
  if (out) Store32(GuestPtr(base, out), qos);
  if (event) {
    if (auto ev = REX_KERNEL_OBJECTS()->LookupObject<rex::system::XEvent>(event)) ev->Set(0, false);
  }
  if (Trace()) {
    uint32_t answered = 0;
    for (bool k : known) answered += k;
    REXLOG_INFO("REACH_LIVE: QoS lookup of {} hosts: {} answered", count, answered);
  }
  ctx.r3.u64 = 0;
}

// DWORD XNetLogonGetTitleID()
extern "C" REX_FUNC(__imp__XNetLogonGetTitleID) {
  ctx.r3.u64 = kTitleId;
}

// HRESULT XNetLogonGetMachineID(ULONGLONG* machine_id): Live machine ids are
// 0xFA000000xxxxxxxx; ours comes from the XUID.
extern "C" REX_FUNC(__imp__XNetLogonGetMachineID) {
  if (ctx.r3.u32) {
    Store64(GuestPtr(base, ctx.r3.u32), 0xFA00000000000000ull | (reach::IdentityXuid() & 0xFFFFFFFFull));
  }
  ctx.r3.u64 = 0;
}
