# P2P online: findings and plan

Status (2026-10-09, late night): M0-M4 done. Players connect to a self-hosted **Reach Live
server** (section 5) and play together over the internet in two ways:

- **Xbox LIVE (emulated, the default with a server):** the profile is signed in to "Xbox
  LIVE"; lobbies are open to friends, and every other player on the server is a friend. The
  lobby roster lists them with their game ("In Firefight Lobby 1/16") and "Join" works:
  presence, session search, QoS game details and the secure connection are answered by our
  layer (section 5.2). Verified: a friend joins a Firefight lobby from the roster and both
  play the match; a Slayer custom game on Sword Base played to its time limit goes through
  the postgame (Credits earned), the carnage report and back to the shared lobby.
- **System Link:** every connected player's System Link games appear in the game's own
  browser (also on one machine with `REACH_NET=1`, the virtual network in section 4).
  Verified: Firefight with two players, Slayer on Sword Base with three, host migration
  (killing the host mid-match: the other player sees "Waiting for host…", becomes the host
  and the match goes on).

Traffic goes directly between players through UDP hole punching, or through the server when
that fails. Not yet tried: separate machines across real NATs.
Not available: matchmaking (signed playlists, section 5.1), file share, Xbox LIVE party.
`tools/system_link_pair.sh` sets up a pair (with `REACH_SERVER` set, through a server).
"Guess" marks statements not confirmed by code or a run.

## Progress log

- **System Link froze the game.** Selecting it ran the network session state machine
  `sub_82276B90`, whose 10-entry jump table codegen had recovered as one entry, so state 2+
  hit `__builtin_trap()` forever. Fixed with `hints/switch_tables.toml`.
- **Link status.** `XNetGetEthernetLinkStatus` returning a link makes "SYSTEM LINK GAMES"
  appear in Network Mode.
- **Ports.** Binding guest port 1000 fails on Linux (privileged). The virtual network never
  binds guest ports on the host: each instance owns one UDP port in 21000-21007 and
  multiplexes guest ports over it with an 8-byte header.
- **XNetRandom.** The SDK fills 0xBB, so every instance generated the same session nonce and
  dropped the other's discovery broadcasts as its own. Random bytes fixed discovery: each
  instance answers the other's 13-byte `broadcast-search` with a 151/165-byte
  `broadcast-reply`.
- **Identity.** Every SDK profile is XUID 0xB13EBABEBABEBABE "User"; `REACH_XUID` and
  `REACH_GAMERTAG` override them (`src/kernel/xam_signin.cpp`). A second instance needs its
  own `XDG_DATA_HOME` too.
- **Join.** X ("Join User") stores a deferred join record (`sub_822C6050`, record at
  0x832FAD00), which the per-frame network join update `sub_82200A00` picks up. That update
  was skipped forever: the SDK completes an asynchronous `XamEnumerate` that has run out of
  items with the raw error, while the console (and Xenia) completes it with
  ERROR_FUNCTION_FAILED plus the error as an HRESULT in the extended error. Reach's content
  enumeration accepted only the console's form, so it restarted about five times a second
  and kept a "content operation in flight" flag up (byte 0x8391DEB8), which gates the join
  update. `src/kernel/xam_enumerate.cpp` fixes the completion codes. Now the joiner registers
  the host's key, resolves its address, waits 8 s without sending anything but discovery,
  then sent join-abort (out-of-band message 10). The wait was for a secure connection that
  never started: Reach calls `XNetConnect` only for addresses whose first byte is 0
  (`sub_82273FD0`), which is what XNetXnAddrToInAddr returns on a 360, and our virtual IPs
  were 10.77.0.N. With 0.77.0.N the joiner connects, sends join-request (message 8, first
  byte 0x90) to the host's port 1001, the host connects back and the session traffic
  (membership, parameters, players) flows on port 1000.
- **Discovery packets** are Blam bitstreams: a 13-byte `broadcast-search` (type byte, a
  constant, 8-byte nonce) and a 151-169-byte `broadcast-reply` echoing the nonce and carrying,
  bit-packed, the host's XNADDR, the game mode name in UTF-16 and the host's XUID.
  `REACH_NETTRACE=packets` logs them without the call trace.

References used:

- SDK: ReXGlue `~/rexglue-sdk-src/sdk` at `bd833a2` (the nightly we build against).
- Xenia Canary `82d0cd1f4` (our GPU reference).
- The Xenia netplay fork, AdrianCassar/xenia-canary branch `netplay_canary_experimental` at
  `6dbaa1fefd` (2026-09-10). It is BSD licensed like Xenia, so its code can be ported with
  attribution.
- Guest addresses are in `default.xex`, from the generated code and the Ghidra MCP server.

## 1. What Reach imports

Only `default.xex` imports networking; the four guest DLLs import none. The import thunks
are in `reach-recomp/generated/default`.

**Sockets:**

- `NetDll_WSAStartup`, `WSACleanup`, `WSAGetLastError`, `__WSAFDIsSet`
- `socket`, `bind`, `connect`, `listen`, `accept`, `select`, `setsockopt`, `ioctlsocket`
- `send`, `recv`, `sendto`, `recvfrom`, `shutdown`, `closesocket`, `inet_addr`

**XNet:**

- `XNetStartup`, `XNetCleanup`, `XNetRandom`
- `XNetGetEthernetLinkStatus`, `XNetGetTitleXnAddr`
- `XNetCreateKey`, `XNetRegisterKey`, `XNetUnregisterKey`
- `XNetXnAddrToInAddr`, `XNetInAddrToXnAddr`, `XNetUnregisterInAddr`, `XNetXnAddrToMachineId`
- `XNetConnect`, `XNetGetConnectStatus`, `XNetServerToInAddr`
- `XNetQosListen`, `XNetQosLookup`, `XNetQosServiceLookup`, `XNetQosRelease`,
  `XNetQosGetListenStats`
- `XNetLogonGetMachineID`, `XNetLogonGetTitleID`

**XAM:**

- `XamSessionCreateHandle`, `XamSessionRefObjByHandle`
- `XamVoiceCreate`, `XamVoiceSubmitPacket`, `XamVoiceHeadsetPresent`, `XamVoiceClose`
- `XamUserAreUsersFriends`, `XamShowFriendRequestUI`

**XSession / XUser.** These have no imports of their own: the XDK library compiles them into
`XMsgStartIORequest` / `XMsgInProcessCall` messages to the XAM "apps". The call sites and IDs
come from the `li r3` / `lis+ori r4` sequences in front of each call:

| App | Message | Wrapper | Meaning (from the SDK / Xenia handlers) |
| --- | --- | --- | --- |
| 0xFB XGI | 0xB0006, 0xB0007, 0xB0008 | 82803580, 82803610, 82803490 | UserSetContext, UserSetProperty, WriteAchievements |
| 0xFB | 0xB0010 | 829219C0 | XSessionCreate (after `XamSessionCreateHandle`) |
| 0xFB | 0xB0011 | 82921C30 | XSessionDelete |
| 0xFB | 0xB0012 | 82921CE0 | XSessionJoinLocal/Remote |
| 0xFB | 0xB0013 | 82921D90 | probably XSessionLeave (guess from numbering). **The SDK has no case**: logs "Unimplemented XGI message" and returns X_E_FAIL (`apps/xgi_app.cpp:450`) |
| 0xFB | 0xB0014, 0xB0015 | 82921F18, 82921FB8 | XSessionStart, XSessionEnd |
| 0xFB | 0xB0018, 0xB001A, 0xB001D, 0xB001E | 82921B70, 82921E38, 82922920, 82922A50 | Modify, ArbitrationRegister, GetDetails, MigrateHost |
| 0xFB | 0xB0021, 0xB0025 | 82803740, 82922180 | UserReadStats, SessionWriteStats |
| 0xFB | 0xB0060, 0xB0065, 0xB0071 | 82922240, 82922068, 82803508 | SearchByIds, SearchWeighted, AwardAvatarAssets |
| 0xFC XLiveBase | 0x58004, 0x58006, 0x5800E, 0x58019, 0x5801E, 0x58020, 0x58023, 0x58044, 0x58046 | 82922548, 820BA2E8, 82922878, 82921248, 82921500, 82920A90, 82920BF0, 82921640, 829213C8 | LogonId, NAT type, friends/presence (Live only) |
| 0xFA XMP, 0xFE XAM | 0x7001A/B, 0x21012, 0x20021 | | music player, guest sign-in, device type |

The game drives all XSession calls from one task dispatcher, `sub_82301800`:

| Op | Calls |
| --- | --- |
| 0 | Create: game flags are mapped to XSESSION_CREATE_* bits; gated by `Function_821E76B0`, possibly "is a Live session" (guess) |
| 1 | GetDetails, then Delete |
| 2 | MigrateHost |
| 3 | Modify |
| 4 | Join |
| 5 | Leave |
| 6 | Start |
| 7 | End |

`sub_822B7608` calls Join/Leave/Modify directly too.

## 2. What the SDK does with them today

| Area | SDK behaviour (`src/kernel/xam/xam_net.cpp`, `src/system/xsocket.cpp`) | Effect on Reach |
| --- | --- | --- |
| Link status | `XNetGetEthernetLinkStatus` returns 0 (`:508`) | `sub_822738D0`, which creates a session key, only succeeds when the link status is non-zero **and** `XNetCreateKey` returns 0. Otherwise only offline sessions exist, so System Link cannot work |
| XNADDR | `XNetGetTitleXnAddr`: loopback IP, MAC `CC×6` for every instance (`:438`) | two instances would look identical |
| Address mapping | `XNetXnAddrToInAddr` / `InAddrToXnAddr` return 1 (failure) (`:481`, `:488`) | `Function_82273988` turns a peer's XNADDR into an IP:port; it fails |
| Keys, connect | `XNetCreateKey`, `RegisterKey`, `UnregisterKey`, `Connect`, `GetConnectStatus`, `QosLookup`, `ServerToInAddr`, `UnregisterInAddr`, `QosGetListenStats` are `REX_EXPORT_STUB` (`:1036-1054`) | `REX_STUB` leaves r3 untouched, so each "returns" its first argument: the XDK wrappers' caller id, 1, which reads as failure |
| QoS | `XNetQosListen` returns `X_ERROR_FUNCTION_FAILED` (`:560`); `XNetQosServiceLookup` succeeds with no data | |
| Sockets | Real host sockets. VDP (protocol 254) becomes plain UDP (`xsocket.cpp:50`). The secure-key options 0x5801/0x5802 are swallowed; `SO_BROADCAST` passes through. No XNet encryption and no port remapping (`xsocket.cpp:117`) | Binding 1000/1001 fails for a normal user: `net.ipv4.ip_unprivileged_port_start` is 1024 on this host |
| XSession | The XGI handlers log their arguments and return success without filling `XSESSION_INFO` or the nonce (`apps/xgi_app.cpp:96-116`); 0xB0013 fails; `XamSessionRefObjByHandle` returns a dummy object (`xam_user.cpp:720`) | Fine for offline; a host would advertise an empty session id/key |
| Identity | Every instance has XUID `0xB13EBABEBABEBABE` (`src/system/xam/user_profile.cpp:28`) | two local instances collide |

Xenia Canary mainline `82d0cd1f4` is not better here: `xam_net.cc` and `apps/xgi_app.cc` have
the same stubs. The XSession messages say "implemented in netplay".

The netplay fork has the missing layer:

- **XNet** (`src/xenia/kernel/xam/xam_net.cc`, 2,723 lines):
  - `XNetGetEthernetLinkStatus` returns ACTIVE | 100MBPS | FULL_DUPLEX unless offline (`:1064`).
  - `XNetGetTitleXnAddr` fills the real LAN IP, the public IP and a persistent MAC (`:696`).
  - `XNetXnAddrToInAddr` returns `xnaddr.ina` in System Link mode and `inaOnline` in Live mode;
    its own MAC maps to loopback (`:882`).
  - `XNetCreateKey` generates a session id with type `XNKID_SYSTEM_LINK` (0x00 in the top
    nibble, `xnet.h:1508`) (`:2615`).
  - `XNetRegisterKey` records the System Link id (`:2628`).
  - `XNetConnect` sleeps 150 ms and succeeds (`:786`); `GetConnectStatus` returns CONNECTED
    (`:796`).
  - QoS lookups go through the server.
- **Sockets** (`xsocket.cc`, 1,365 lines): guest-to-host port remapping on
  bind/connect/sendto/recvfrom (`:256`, `:280`, `:516`, `:1208`; tables in `upnp.h:137-143`),
  plus UPnP port forwarding.
- **Sessions** (`xsession.cc`, 1,367 lines): System Link and Live sessions.
- **Live backend** (`XLiveAPI.cpp`, 2,606 lines): a central web service (`api_address`, default
  list `xenia-netplay-...herokuapp.com`) for matchmaking, sessions, QoS data, friends and
  presence.
- **Modes**: `network_mode` 0 offline / 1 System Link / 2 Live (`XLiveAPI.cpp:41`);
  `bind_interface` picks the adapter, for tunnels/VPNs.

What a port would take: our SDK keeps Xenia's structure (`XSocket`, XGI/XLiveBase apps,
`REX_EXPORT` entry points), so these functions move over almost one-to-one. We can't change the
SDK binary's exports, so they would live either in `patches/rexglue-sdk` or as `__imp__`
overrides in `reach-recomp/src/` (the pattern of `xam_signin.cpp` and `xam_keyboard.cpp`).
Overrides avoid rebuilding `librexruntime.so`, which we don't ship (see
`tools/build_rexglue_sdk.sh`). The SDK's XGI and XSocket internals are reachable from our code
only through exported symbols, so a self-contained net layer in our tree is simpler than
patching them.

## 3. How Reach's system link works (as far as the code shows)

- **Endpoints.** `sub_822F9F98` opens two endpoints through `sub_822F9D80`:
  - port **1000** with transport mode 1;
  - port **1001** with mode 0 and the flag that makes `sub_822F9D80` set
    `SO_BROADCAST` (`setsockopt(s, 0xFFFF, 0x20)`).
- **Socket modes.** `sub_822A5020` maps mode 0 to UDP (17), mode 1 to **VDP (254)** and mode 2
  to TCP (6), all over AF_INET. So game traffic is VDP on 1000 and discovery is UDP broadcast
  on 1001 (the broadcast-to-1001 detail is a guess from the flags, but consistent).
- **Transport flags.** Both endpoints require the flags at 0x82BD2820/0x82BD2821.
  `sub_82274328` initialises the transport: `XNetStartup` with 16 datagram sockets, 24 stream
  sockets, 10 key registrations and 75 security associations, then `WSAStartup`.
  `sub_822742C0` polls the link status: bit 0 means active, and bit 5 (wireless) is stored at
  0x82BD2823. `Function_822741F8` shuts the transport down.
- **Out-of-band message types** (`Function_823262C0`):

  | Type | Name | Size |
  | --- | --- | --- |
  | 0 | `ping` | 12 bytes |
  | 1 | `pong` | 12 bytes |
  | 2 | `broadcast-search` | 16 bytes |
  | 3 | `broadcast-reply` | 0x1280 bytes (presumably the game description) |

  Session messages follow, as in Halo 3: `connect-request/refuse/establish/closed`,
  `join-request`, `peer-connect`, `join-refuse/abort`, `leave-session`, `session-boot/disband`,
  `host-handoff`, `membership-update`, `peer-properties`, `parameters-update/request`, and so
  on (strings at 0x82051F94..0x820524D8).
- **Addressing.** A peer is addressed by XNADDR plus session id: `Function_82273988` calls
  `XNetXnAddrToInAddr(xnaddr, xnkid)` and builds {IP, port, family 4} from the result. The host
  creates the session key with `XNetCreateKey` (`sub_822738D0`); joiners register it with
  `XNetRegisterKey` (`sub_822B8E20`, `sub_822B91F0`) and connect with `XNetConnect`
  (`sub_82273B80`, `sub_822AB038`).
- **QoS.** `XNetQosListen` (4 sites) / `XNetQosLookup` (`sub_8226D1D0`) are probably only used
  by Live matchmaking (strings `qos-*`, `matchmaking-*`); the broadcast reply carries what system
  link needs (guess).
- **No encryption is needed between our own instances.** On a 360, XNet encrypts VDP with the
  registered key. Between two copies of this port, both ends skip that, so VDP payloads (2-byte
  game-data length + data + voice) can pass through unchanged. Talking to real 360s or Xenia is
  out of scope.

## 4. Plan

Design choice: a small **virtual network layer** in `reach-recomp/src/net/`, not raw host
sockets.

- **One host UDP socket per instance.** The guest sockets (ports 1000/1001, plus any the game
  opens later) are multiplexed over it with a 4-byte header: guest source port and destination
  port.
- **Virtual addresses.** Every instance gets a virtual IPv4 (e.g. `10.77.0.N`) and a unique MAC
  for its XNADDR. `XNetXnAddrToInAddr` returns the virtual IP; the layer maps it to the peer's
  real `ip:port`.
- **Broadcast** to `255.255.255.255:1001` is sent to every known peer.
- **Why this shape.**
  - No privileged ports and no port collisions, so two instances run on one machine.
  - Only one port to forward or hole-punch.
  - LAN and internet differ only in how peers are found: local broadcast on the host port vs a
    rendezvous server.

Milestones:

- **M0 – observe.** Make every XNet/XSession/socket call log at info level (our `__imp__`
  overrides can wrap the SDK). Record the sequence when the lobby's "Select Network" (Y) is
  opened and System Link is chosen. Confirm what greys out System Link today (expected: link
  status 0).
- **M1 – System Link available on one instance.** Override:
  - `XNetGetEthernetLinkStatus` → `0x0B`;
  - `XNetGetTitleXnAddr` → virtual IP, a per-instance MAC and STATIC|ETHERNET flags;
  - `XNetCreateKey`, `RegisterKey`, `UnregisterKey`, `XnAddrToInAddr`, `InAddrToXnAddr`,
    `UnregisterInAddr` (map-based), plus `Connect` and `GetConnectStatus`;
  - `XNetQosListen` → success.

  Add the XGI 0xB0013 handler and fill `XSESSION_INFO` (id, host XNADDR, key) in
  XSessionCreate in case System Link uses it (find out in M0). Add per-instance identity:
  `REACH_XUID` / gamertag overrides, and separate profiles via `--user_data_root` or
  `XDG_DATA_HOME` (`runtime.cpp:32`, `filesystem_posix.cpp:108`).

  Done when a System Link lobby can be hosted and the 1000/1001 endpoints are bound through the
  virtual layer.
- **M2 – two instances see each other on one machine.** Use a static peer list from an env var.
  Done when instance B's System Link browser lists A's game (`broadcast-search` /
  `broadcast-reply` observed in the log). Drive both with `REACH_AUTOPRESS_FIFO`, one FIFO
  each.
- **M3 – a game starts and plays.** Join, membership updates, map load, gameplay, ending, host
  leaving / host migration. Measure bandwidth and latency through the layer.
- **M4 – internet.** Done as Reach Live (section 5): rooms, each instance registers the
  public endpoint the server observes, UDP hole punching through it, and a relay fallback for
  symmetric NAT; self-hostable Python, no Microsoft services. Optionally add UPnP, as in the
  fork.
- **M5 – polish.**
  - Room and peer UI.
  - Voice: `XamVoice*` are SDK stubs today, so VDP voice payloads would need a host voice path.
  - Version checks: the game already rejects mismatched builds (`host-version-too-low`).

Testing on one machine:

- Two instances need about 2 × 6 GB RAM and 2 × 5 GB of `/dev/shm`, which is a 16 GB tmpfs
  here. Both instances also share the GPU.
- With the virtual layer no namespaces are needed: give each instance its own host port.
- To test raw sockets instead, user namespaces are enabled (`max_user_namespaces` = 125748), so
  `unshare --user --map-root-user --net` gives each instance its own network stack. Inside it,
  binding port 1000 is allowed. Linking two namespaces needs a veth pair created inside a shared
  user namespace, or `slirp4netns`.

Risks:

- The game may expect XNet behaviour our stubs don't show: connect status transitions, key
  limits (10 registered keys), or `XNetUnregisterInAddr` timing.
- Session creation may need a real `XSESSION_INFO` even on System Link (open; M0 answers it).
- The intermittent runtime hang seen when entering the Forge lobby (`docs/PROJECT.md`) would
  also hit network sessions.
- Determinism and simulation model: not investigated. Both ends run the same binary, so
  differences between the recompilation and a 360 do not matter between our own instances.
- Interoperability with real consoles, Xenia or Xenia netplay is not a goal; it would need
  XNet encryption, or the fork's server protocol.

Possibly useful later: the Ghidra project also has `haloreach.dll` open (MCC's PC build, with
its own network layer). It may help name Blam network functions; it is not needed for the
plan above.

## 5. Reach Live (custom servers)

Players connect to a community-run server instead of Xbox Live:

```sh
python3 server/reach_live_server.py --port 21100 --http-port 21101   # on the server
REACH_SERVER=example.org tools/run_reach.sh 3600                         # each player
```

- **Server** (`server/reach_live_server.py`, Python standard library only, one UDP port):
  registers players, forwards System Link broadcasts to everyone in the same room, relays
  datagrams between players that can't reach each other, and tells each player its public
  address. `--http-port` serves a JSON status page (players, rooms, relay totals).
  `--rate-limit` (bytes/s, default 64 KB/s, 4 s bursts) caps what the server relays or
  broadcasts for one player; `server/Dockerfile` packages it.
  `server/test_reach_live_server.py` tests the protocol.
- **Client** (`src/kernel/net.cpp`): `REACH_SERVER=host[:port]` turns on the virtual network
  in Live mode. Options: `REACH_ROOM=name` (only players in the same room see each other),
  `REACH_NET_PORT=n` (fixed local UDP port, for a manual port forward),
  `REACH_SERVER_RELAY=1` (never connect directly; for testing), `REACH_NET_LAN=1` (also
  broadcast on the LAN).
- **Identity** (`src/kernel/identity.cpp`): the first Live run writes
  `~/.local/share/reach/4D53085B/live_identity.txt` with an online-style XUID
  (`0009xxxxxxxxxxxx`) and a gamertag (the login name; edit the file to change it).
  `REACH_GAMERTAG` / `REACH_XUID` override it. The game shows these in lobbies.
- **Addresses.** A Live player's XNADDR carries the id the server gave it (`ina` =
  `0xF0000000 | id`, `inaOnline` = public IP, `wPortOnline` = server epoch). Peers map it to a
  virtual IP 0.77.x.y as for LAN peers; the network layer routes that IP to the player's
  direct path or through the server.
- **NAT traversal.** When a player first hears from another (any forwarded datagram), both
  send `punch` packets to the other's public and LAN addresses every 300 ms for 6 s; the
  first that arrives opens the direct path (and is answered). Without one, traffic keeps
  going through the server; a new attempt can start after 30 s.

Protocol (UDP, big-endian; every packet starts with `RLV1` and a type byte):

| Type | Direction | Body |
| --- | --- | --- |
| 1 hello | client → server, every 5 s | version u16, process token u64, XUID u64, LAN IP u32, LAN port u16, room (u8 length + bytes), gamertag (u8 length + UTF-8) |
| 2 welcome | server → client | player id u32, server epoch u16, public IP u32, public port u16, message (u16 length + bytes) |
| 3 error | server → client | code u8 (1 not registered, 2 version, 3 full), text (u16 length + bytes) |
| 4 broadcast | client → server | guest source port u16, guest destination port u16, payload |
| 5 relay | client → server | destination player id u32, guest ports, payload |
| 6 forward | server → client | source player id u32, its public IP u32 + port u16, its LAN IP u32 + port u16, guest ports, payload |
| 7 punch / 8 punch-ack | player ↔ player | sender id u32, receiver id u32, server epoch u16 |
| 9 data | player ↔ player | sender id u32, guest ports, payload |
| 10 bye, 11 list / 12 peers | | leaving; who is in the room (with presence, section 5.2) |
| 13 presence | client → server | friend state u32, XSESSION_INFO, status (u8 length + UTF-8), extras (slots, QoS data) |
| 14 invite | client → server → invitee | target XUID u64 + XSESSION_INFO; delivered as inviter id u32, XUID u64, gamertag, XSESSION_INFO |

Measured on one machine (relay forced with `REACH_SERVER_RELAY=1`): a Firefight match between
two players is about 4 KB/s and 60 packets/s through the server in total, so a small VPS can
relay many games. The lobby, the join and the match work both directly and relayed.

Settings: the same options live in `reach.toml` next to the executable and in the F4
settings overlay under "Network/Reach Live" (`live_server`, `live_signin`, `live_room`,
`gamertag`); the environment variables override them. Changes apply on the next start.

### 5.1 Title servers ("LSP")

Bungie ran Reach's online services as Xbox Live title servers ("LSP"): HTTP/1.0 over TCP.
With Live sign-in (the default with a server) the Reach Live server's HTTP port plays that role
(`src/kernel/live_lsp.cpp`, `src/kernel/live_tcp.cpp`, `server/reach_live_lsp.py`).

- **Discovery.** The game enumerates title servers with XTitleServerCreateEnumerator (XAM
  enumerator: app 0xFC, open message 0x58039, items X_TITLE_SERVER {inaServer, flags,
  szServerInfo[200]}, 0xD0 bytes; the SDK refused it). The LSP manager splits each
  description at `,` and matches the tokens against its eight service names (4-byte strings
  at 0x8325114C from the network configuration: `ttl,usr,shr,upl,web,prs,std,dbg`), so the
  one server we return names all of them (`Function_82271658`). It then calls
  XNetServerToInAddr(ina, 0x4D530064) and connects to a random port of the configured range
  (1011-1026, `Lsp_ResolveServerAddress` 0x82271E38); net.cpp sends any TCP connection to
  the server's address to its HTTP port.
- **TCP.** The SDK's host TCP sockets passed the Windows FIONBIO code to Linux and set no
  Winsock errors, so `ioctlsocket` failed and the game closed every connection before
  `connect` (transport code `sub_822A4C58`: socket, FIONBIO, connect expecting
  WSAEWOULDBLOCK, select). With the virtual network on, stream sockets are host TCP sockets
  with Winsock semantics in `live_tcp.cpp`.
- **HTTP.** Requests: `GET path HTTP/1.0` or `POST` with `multipart/form-data;
  boundary=BUNGIEr0x0rz` (one part named `upload`, content type `application/x-reach-*`).
  The reply parser accepts `HTTP/1.0 ` / `HTTP/1.1 ` and reads `Content-Length: `.
  Payloads are BLF files: `_blf` (0x30, v1.2, big-endian byte-order mark FFFE), chunks
  `{fourcc, u32 size with the 12-byte header, u16 major, u16 minor}`, `_eof` (v1.1: u32
  length before it, u8 authentication type). Chunks are found by fourcc and major version.
- **Signatures.** `_eof` authentication (checked by `Function_822E1DC0` with the type each
  file requires): 0 none, 1 CRC32, 2 SHA-1, 3 SHA-1 + 256-byte RSA signature verified with
  the key in XEX resource "00" (`Crypto_VerifyRsaSignatureResource00` 0x8242EA68). Signed
  title files from Bungie verify unchanged; new ones would need that check relaxed.

Requests seen after sign-in (main menu, about two minutes):

| Request | What it is | Server answer |
| --- | --- | --- |
| `POST /gameapi_omaha/UserUpdateRewards.ashx?getDailyChallenges=1&userId=&machineId=` | rewards sync: `rpul` v3 (Credits block, Armory flags, purchase log), `chpr` v2 (challenge progress), `loca` | `rpdl` v2 (chunk 0x227: the server's totals block + zero tail). Per-XUID record in `DATA_DIR/players/<xuid>.json`; Credits, counters and flags never go down (max / union), so the game's merge (`new = local + server - uploaded`) restores a reinstalled profile. No `dcha`: every client picks the same challenges from the date. Verified: rewards state 2 ("synced"), and raising the stored Credits to 20,000 made the game adopt them and show "New Armory items" |
| `POST /ReachPresenceApi/heartbeat.ashx` | `phbt` v5, 0x1BB bytes: in-matchmaking flag, u8 player count, 4 × 0x38 player entries (u64 XUID first), machine id at +0xE2, matchmaking party state from +0xF2 (party list of u64 XUID + u8 at +0xFF, stride 9) | `phbr` v2 (0x93 bytes, all zero). Its fields: +0 u8 flag, +1 u8 per-player flag mask, +2 u32 count, +6 u64 XUID[16] of players reserved for this party (counted by join checks), +0x86 u64, +0x8E u32 (`Function_8232F578`) |
| `POST /ReachPresenceApi/query.ashx` | `preq` v3: u32, u32 count, u64 XUID[16] of roster players with Bungie presence pending | `pplr` v5 (0x1094: u32 count + 16 records of 0x109 bytes, XUID at +8), sent with count 0. The roster copies a record to its entry +0x15C; the join check reads +0x44 (status), +0x49 max players, +0x4A players, +0x4C (`Function_822CAAC0`). The record layout beyond that is not mapped |
| `GET /storage/title/4d53085b/tracked/11860/default_hoppers/manifest_001.bin`, `en/rsa_manifest.bin`, `dynamic_pres_hopper_statistics.bin` | matchmaking playlists (11860 is the build) | the statistics file is generated per request; the others 404 unless the operator provides the file under `DATA_DIR/storage/<path>` |
| `GET /storage/user/4d53085b/…/<xuid>/user.bin`, `recent_players.bin`, `/storage/machine/…/machine.bin` | per-player and per-machine files | 404 (a new player has none) |
| `GET /gameapi_omaha/ArenaGetSeasonStats.ashx`, `UserGetBnetSubscription.ashx`; `POST /upload_server/stats.ashx` | Arena, Bungie Pro, stats upload | 404 |

Active roster (for the session side): entries come from a provider per roster type
(`Function_8231A660`); each roster player's XAM presence comes from XPresenceSubscribe
(0x5801E) / XPresenceCreateEnumerator (0x58019) as XONLINE_PRESENCE (0xA4 bytes, session
id at +0xC; `Function_8231ACF8`). The session ids are then resolved to XSESSION_INFO (0x3C:
id, host XNADDR, key) by `Function_8226CAD8` (`Function_8231B3D0` / `8231B098`), the game
details (0x1270 bytes, as in the System Link reply) come from QoS, and the Bungie record
from `query.ashx`. Joining a roster player goes through the deferred join
`sub_822C6050` (`Function_822CB2D8`).

Not done: hopper files (matchmaking playlists, game and map variants, all signed), file
share (`FilesGetCatalog.ashx`, `FilesUpload.ashx`, … and user storage), Arena, the
Bungie presence record layout. (The `machineId` the game sends comes from
`XNetXnAddrToMachineId`; it is now 0xFA000000 plus the low half of the player's XUID, stable
across runs and server restarts, like `XNetLogonGetMachineID`.)

### 5.2 Xbox LIVE: friends, presence, sessions

With a server and `live_signin` (on by default), `src/kernel/xam_signin.cpp` reports user 0
signed in to Live (sign-in state 2, Live-enabled flag, all privileges, ONLINE XNADDR flag)
and `src/kernel/live_xmsg.cpp` answers the XAM messages behind Live, which the SDK fails or
stubs. Every player in our room on the server is a friend.

| Message | What the game does with it | Our answer |
| --- | --- | --- |
| XStringVerify (0xFC/0x5000C) | checks user text; retried every frame while it failed | every string OK |
| XFriendsCreateEnumerator (0xFC/0x58020) | the friends list (100 × 0xC4 X_ONLINE_FRIEND), read at sign-in and again on XN_FRIENDS_FRIEND_ADDED | the room's players with their presence |
| XPresenceSubscribe / Unsubscribe / CreateEnumerator (0xFC/0x5801E, 0x58044, 0x58019) | the roster's presence (X_ONLINE_PRESENCE, 0xA4: state, session id at +0xC); subscribe was retried every frame while it failed | presence from the roster |
| XSessionCreate / Delete / Modify / Join / Leave (0xFB/0xB0010-0xB0013, 0xB0018) | sessions for lobbies and parties | for a session we host: an online peer session id (0x80 type), our XNADDR and a random key in XSESSION_INFO; the newest joinable presence session is published with its slots. Leave (0xB0013) had no SDK handler |
| XSessionSearchByIds / ByID (0xFB/0xB0060, 0xB001B) | turns friends' session ids into XSESSION_INFO (host XNADDR, key) | the session each player published |
| XNetQosListen / XNetQosLookup | the host publishes its game description (the same bitstream as the System Link reply, 0x98 bytes); joiners probe it for "In Firefight Lobby 1/16" | the data travels with the host's presence; lookups are answered at once (XNQOS from `SystemHeapAlloc`, released by the SDK's XNetQosRelease) |
| XInviteSend (0xFC/0x50002) / XInviteGetAcceptedInfo (0xFC/0x58023) | "Invite to Party" in the player menu; on XN_LIVE_INVITE_ACCEPTED (0x02000002) the notification loop asks for the accepted invite and joins its session | the invite goes through the server (UDP `invite`, type 14) with our joinable session (hosted, else joined); the friend's client accepts it at once (`live_accept_invites`, on by default: there is no Xbox Guide to accept in) and answers the X_INVITE_INFO. Verified: Kat invites Jun from the roster, Jun joins her Firefight lobby |
| XamUserAreUsersFriends, XNetLogonGetTitleID / MachineID | | from the roster; 0x4D53085B; 0xFA000000 + XUID |

Presence on the server: each client sends its friend state, the XSESSION_INFO of its joinable
session, a status line and opaque extras (slots and QoS data) in a `presence` message (type
13); `list` returns them for the room. When the roster changes the client posts
XN_FRIENDS_FRIEND_ADDED (0x04000002) and XN_FRIENDS_PRESENCE_CHANGED (0x04000001); Reach's
notification loop (`Function_826F7BD0`) then re-reads friends (`0x82320778`) and presence
(`0x8232E190`).

Roster internals (from Ghidra): the active roster's providers are at 0x82A43E08 (type 1:
friends, per local user at `*0x8315107C`, count at +0x7C; type 2: Xbox LIVE party at
`*0x83151040` + 0x1E18, 0x78-byte XPARTY_USER_INFO). The `XFriendsCreateEnumerator` task at
0x82A9666C (started by `Function_822688D8` on the Guide's gamercard notification 0x06010004)
is the Guide's "join from gamercard" path, not the roster.

### 5.3 File Share

Each player's File Share lives on the Reach Live server (`server/reach_live_files.py`,
files and an `index.json` per player under `DATA_DIR/fileshare/<XUID>/`; 24 slots and 100 MB
each). Verified with two instances: Lsp Tester uploaded a Forge map variant from Forge's
map browser (Local Files, X "File Options", "Upload to File Share": "Upload complete"); Kat
opened Lsp Tester's File Share from the Xbox LIVE roster (player menu, "File Share"), saw
the map with its name, description, author, date and size, and downloaded it ("Download
complete"); the package in Kat's local storage is byte-identical to the original save. The
listings and files are unsigned (`_eof` authentication 0), so no client-side relaxation
was needed.

Requests (`machineId`, `userId` and `shareId` are on every one; ids are hex):

| Request | Reply |
| --- | --- |
| `GET FilesGetCatalog.ashx?shareId&locale` (a player's share) | BLF `fitm` v4: 0x28-byte header (+0 u64 share, +8 owner gamertag char[16], +0x1C u32 quota in bytes, +0x20 u8 slot count, +0x22 u16 item count, +0x24 u8 length of a UTF-16 notice after the items), then items of 0x29C bytes. The UI reads the header in `sub_827CCE78`: slots free = slots − items, bytes used = Σ item size |
| `GET FilesGetCatalogInfo.ashx?shareIDs=a,b,…` | BLF `finf` v1: u16 count, u16, then a 0x24-byte record per share. The record does not feed the slot or space figures; its layout is not mapped (the server sends per-type counts) |
| `GET FilesNewUpload.ashx?uniqueId&fileType&uncompressedSize&compressedSize` | plain text: the server's id for the upload, hex (`strtoull(…, 16)`) |
| `POST FilesUpload.ashx` (multipart, part `upload` = the file; HTTP headers `machineid`, `userid`, `shareid`, `serverid` with quoted values, `startposition` on resume) | 200; the body is not read. The file is the game's BLF: `_blf`, `chdr` (uncompressed), `_cmp` (zlib, window bits 15), `_eof`, padded to whole 4 KB pages |
| `GET FilesGetUploadProgress.ashx?serverId` | bytes received, hex (resume) |
| `GET FilesGetDetails.ashx?serverId` | `fitm` v4 with one item and its extra data (u8 tag count at item +9, u32 extra size at item +0x298: none here) |
| `GET FilesStageForDownload.ashx?serverId&startPosition&fromAutoQueue&preview` | text lines `Size: n`, `FullSize: n` (both non-zero) and `InitialUrl: path` (`sub_8236D380`); the client then GETs `InitialUrl` from the same server (headers `machineId`, `serverId`, `userID`, `shareID`) and stores the bytes. `FilesResumeDownload.ashx` continues it |
| `GET FilesGetSearch.ashx` / `FilesGetSearchCount.ashx` (`fileType`, `gamertag` or `authortaghex`, `mapId`, `gameEngine`, `megaloCategoryIndex`, `fileAge`, `sortBy`, `taghex`, `page`) | `fitm` v4 (50 items a page) / `finf` v1. Implemented over every share (file type, author, map), not exercised in game yet |
| `GET FilesDelete.ashx?serverId` | 200 |

Item (0x298 bytes) = the file's content header with its first ids reordered: the content
header is the `chdr` payload after u16 build and u16 (0x2B0 bytes: +0 u8 file type, +4 u32
size, +8 u64 unique id, +0x10/+0x18/+0x20 parent, root and game ids, +0x28 activity, mode,
+0x2C i32 map id, +0x38 and +0x5C creator / modifier {u64 time, u64 XUID, char[16] name, u8
online}, +0x80 wchar name[128], +0x180 wchar description[128], …), and the item is
`header[8:0x10] + header[0:8] + header[0x28:]` (unique id first; parent, root and game ids
dropped). The server replaces the unique id with its own file id. File types seen: 2
screenshot, 3 film, 5 map variant, 6 game variant.

Runtime fix found on the way (`src/kernel/file_size_refresh.cpp`): the SDK caches each
host file's size and NtWriteFile does not refresh it, so after writing a download in 4 KB
pieces the game's attribute query saw 4096 of 8192 bytes, a chunk ran past the end and
the transfer failed. The override refreshes the size after writes that extend a file.

Not done: tags, recommendations, predefined queries (`fpre`) and megalo categories
(`fmca`), screenshot previews, the per-type counters next to a share (they stay 0), and
the service record (`UserGetServiceRecord.ashx` returns an empty `srid` v7 chunk of 0xD48
bytes, read straight into the game's buffer; not checked in game).

### 5.4 Matchmaking

Goal: Matchmaking on a Reach Live server with the server's own playlists. The playlists are
title storage files the game downloads over HTTP from the server
(`/storage/title/4d53085b/tracked/11860/default_hoppers/…`); `server/reach_live_hoppers.py`
writes them under `DATA_DIR/storage/` and the server serves them as stored files. No Bungie
file is in the repository: the generator builds every file from scratch. What it needs
from the operator's game is read from the operator's copy: the hash salt and the built-in
network configuration (from a running client), the map signatures and map ids (from the
maps directory) and the game types (saved in game and uploaded to the server's File Share).

```
python3 server/reach_live_hoppers.py DATA_DIR --from-client PID     # once; PID = a running client
#   game types: Custom Game, Game Type, pick one, Game Options (Teams on for a team
#     playlist), X "Save Game Type", then in the game type list X "File Options",
#     "Upload to File Share" (to this server)
python3 server/reach_live_hoppers.py DATA_DIR --maps extracted/xbox360/maps
```

`DATA_DIR/playlists.json` replaces the built-in playlist list (`DEFAULT_PLAYLISTS` in the
generator: "Free For All", 2-8 players, the game type "Slayer" on Sword Base, Zealot and
Powerhouse; "Team Slayer", two teams of 1-4, the game type "Classic Slayer" on Sword Base,
Boardwalk and Countdown; "Capture the Flag", two teams of 1-4, "Capture the Flag" on Sword
Base, Countdown and Boardwalk; "Objective", 2-8 players, "Oddball" on Zealot and Sword Base
and "King of the Hill" on Sword Base and Powerhouse). The game type names are the names the
game gives a saved copy of the built-in type (Classic Slayer is a team type; Slayer,
Oddball and King of the Hill saved with Teams off). Without a keyboard the save keeps that
name, and of two uploads with the same name the newest wins. Each game names a game type (the newest upload of that name) and
a map (by its name in `maps/info`); the map variant is the map's default one, which the
generator builds, unless `"map_variant"` names a Forge map uploaded to the File Share.
Games whose files are missing are left out with a warning, and playlists without games.
Category ids must be 0-15. Clients pick up new files when they restart.

**Status (two instances on one machine, one server): working.** Both press START
MATCHMAKING; within about 15-60 s they find each other, merge, pass arbitration and host
selection, load the map ("Starting match… Brace for carnage") and play together: Slayer
on Sword Base, Zealot and Powerhouse, and Team Slayer 1v1 (Red Team Carter, Blue Team Jun).
After a match both save stats, leave the group, search again and are matched into the next
game together; three games in a row were tested with a 1-minute game type. Other game
types work the same way (2026-10-10, 1-minute versions saved and uploaded as above): six
Capture the Flag games in a row on Countdown (two teams of one), and an "Objective" FFA
playlist that picked Oddball on Zealot and then King of the Hill on Sword Base.

Open:
- More than two players (needs a third instance, about 26 GB free on this machine).
- Arena (only the season reply exists).
- Next steps: a third instance for 3+ player matches; if the Custom Game fault loop
  returns, catch it with gdb (`handle SIGSEGV stop`, filter si_addr = guest 0) and read
  the guest backtrace.
- Arena: needs the season stats the server already answers plus per-playlist ratings
  (the 30-byte records of `arhs`), the Arena hoppers' `%05u/images/hopper_histogram.jpg`
  (the rating histogram image, same request class as `hopper.jpg`) and a hopper flagged
  as an arena one ("arena hopper no longer available" is the string it checks). Not
  implemented.
- Once (2026-10-10, with a second instance on the same server), leaving the Matchmaking
  lobby and opening Custom Game made the game fault in a loop (null read of guest address 0
  on the main thread, 0.2 s after pressing A). It did not happen again in six attempts:
  the same inputs on a freshly started instance (START menu, Challenges, back, leave the
  lobby, Custom Game), after a matchmade game and "Return to lobby", after the Playlist
  screen, and through the Campaign lobby.

After every match the game itself dissolves the group: the matchmaking-recycle state
(`sub_82283548`) leaves the group session and each party searches again, keeping its own
members through the "matchmaking-rematch-data" parameter (`sub_8228D9F0`, the parties'
session infos). Players who were idle are sent back to their lobby instead with "the party
leader appears to be away from the game": a peer reports status bit 19 (`sub_8228EA18`)
when its controller saw no input for 480 s (network configuration +0x5D8, `sub_822DA810`),
and the post-match and recycle states return everyone to the lobby when a peer has it
(`sub_8228E538`, `sub_82282EE8` reason 7). Scripted test runs need regular input.

Three things blocked it, all in the data rather than the client:

1. The ping limit of the search stages (hopper +0x134…, below) was 0, so every session
   found was rejected (`sub_822CD310`).
2. The member dropped every session parameters update: the "matchmaking-hopper"
   parameter (the host's hopper, `sub_822E2B70` / `sub_822E31D0`) carries the hopper's
   voting block (+0x98…+0xB0), whose decoder `sub_822D9508` wants +0xAC in 1–30 while the
   host's own check (`sub_82291260`) allowed 0. With the whole update rejected, the member
   stayed one update behind and the host timed out in arbitration (reason 0x11 after 30 s,
   network configuration +0x4CC). Found by hooking the per-parameter decoders (the
   parameter table at `*0x83150EA0`, 0x28-byte entries {flag, name, encode, decode, …,
   size}; the update is decoded by `sub_822AB588`).
3. A multiplayer game only starts with a map variant: `sub_82280140`, called when the
   countdown ends, needs both the game variant and the map variant parameters (and the
   map variant's map id at +0x2B4 to match the map). Without one, the state machine went
   in-match → end of match → recycle within a second. So playlists use variant source 0
   (game and map variant files). The map variant does not have to come from Forge: a
   custom game without a saved map uses the map's default variant, which is an empty
   variant flagged built-in for that map id (`sub_824CE7A0`; `sub_824CEDE8` keeps it and
   the game places the map's own objects). The generator writes that variant as an `mvar`
   file, below.

The client side only gained XUserMuteListQuery (XLiveBase 0x5800E, answered "not muted"):
Reach asks it for every other player several times a second during a match, and the SDK
failed and logged every call.

#### Download and validation

All files are BLF files (`_blf` v1.2, chunks, `_eof` v1.1 with authentication 0; the chunk
finder `sub_822E1FB8` matches fourcc and major version). Each title file is an object in
the container at 0x83336D80 (built by `sub_822DF7F0`), with a name, a request at +0x4C
(URL at +0x8, state at +0x66C: 5 = done, 7 = failed) and a load state at +0x34 (1 =
loaded). Requests have a validation mode (+0x10C): 0 none, 1 hash listed in the manifest,
2 the same for a localized file under `en/`.

The hash is **SHA-1 over a 34-byte salt followed by the file** (`Crypto_Sha1Buffer`
0x82214080 with its third argument 1; the salt is the first 0x22 bytes of the executable's
resource "00", 0x138 bytes at 0x83AB0000, whose RSA public key starts at +0x28). In
`sub_822DD4C8` a downloaded mode-1/2 file is checked against the manifest entry for its
path; a path the manifest does not list is accepted unchecked (and its hash remembered
for a while), so an empty manifest would also do. A game set, game variant or map variant
is instead checked against the hash its parent lists (request flag +4 bit 0, expected hash
at +0x120), with no fallback, so the generator needs the salt from the operator's game.
No client change was needed: the manifest itself is not signed (mode 0).

| File | Chunk | Contents |
| --- | --- | --- |
| `manifest_001.bin` | `onfm` v1 | u32 count, then {char path[0x50] relative to `default_hoppers` with a leading `/`, lowercase; salted SHA-1} per file |
| `en/rsa_manifest.bin` (object +0, required) | `mapm` v1, 0x8004 bytes | u32 count, 128 RSA signatures of 0x100 bytes. When a map loads (`sub_826E93C8` → `sub_822DBE78`) its header signature (file offset 0x36C) must be listed, or the console is flagged as running modified content (byte 0x8330E589). The generator lists the operator's maps |
| `network_configuration_241.bin` (required) | `netc` v241, 0x2254 bytes raw | Network tuning, applied to 0x82BD28A8 (`sub_82299860`). The game fills the same block with built-in defaults at start; while the file is missing (object at 0x83347D04 in state 3) `sub_8228FB68` reports the server unavailable, so the server serves the client's defaults back. Includes the search intervals (+0x52C, +0x530), the arbitration timeouts (+0x4C8, +0x4CC), a 100 ms cap on ping limits (+0x538), the map id table (+0x18F4, 64 × {id, flags}) and the Arena-disable byte (+0x1B02) |
| `dlc_map_manifest.bin` (required) | `dlcd` v1, 0xF504 bytes raw | DLC maps; all zero = none |
| `en/matchmaking_banhammer_messages.bin` | `bhms` v1 | optional, not generated |
| `matchmaking_hopper_027.bin` (required) | `mhcf` v27 | the hopper table, below |
| `en/matchmaking_hopper_descriptions_003.bin` (required) | `mhdf` v3 | bitstream: count − 1 (6 bits), then per hopper u16 id (16 bits), a flag (1 bit), a NUL-terminated description (8-bit characters, 256 at most) |
| `dynamic_pres_hopper_statistics.bin` | `mmhs` v4 | Playlist screen player counts, built per request from the server's peers (below) |
| `en/dynamic_global_nag.bin` | | optional, not generated |
| `%05u/game_set_015.bin` | `gset` v15 | per hopper, below |
| `%05u/images/hopper.jpg` | JPEG | playlist image, from `"image"` in `playlists.json` or `DATA_DIR/images/<id>.jpg` (below) |

**Player counts and images on the Playlist screen** (2026-10-10, verified with screenshots).
The title storage object for `dynamic_pres_hopper_statistics.bin` (constructor `sub_820C95C8`,
refresh interval 600 s at +0x108, class vtable 0x820017E4, the object's load state at
0x83395180 must be 1) copies the chunk payload, 0x80E bytes, as it is to 0x833968D4
(`sub_822E0C60`). Its reader is `sub_820C8DC0(hopper id, out[2])`, used by the Playlist screen
and the lobby: `out[0]` = the u32 at +0 (total players), and for a hopper id the sum of the u32
counts of the entries with type 2 and that id. The payload is a raw big-endian struct: u32 total
at +0, 8 bytes not read there, i16 entry count at +0xC, then up to 256 entries from +0xE of 8
bytes `{u8 type, pad, i16 hopper id, u32 players}`. `reach_live_hoppers.hopper_statistics()`
writes it and `reach_live_lsp.hopper_population()` serves it from the server's peers: each peer's
published matchmaking session counts max(1, filled slots) players for its search key (the
hopper id); a file at that path in `DATA_DIR/storage` replaces the generated one. The Playlist
screen then says "N CURRENT PLAYERS" for the highlighted playlist (tested with 3, 5, 21, 7 and 12 from
a static file; with the generated file the screen said 0, and while one client searched the
server sent total 1 and 1 for hopper 101). Counts are only as
fresh as the client's last download. The file is not in the manifest.

Images: `%05u/images/hopper.jpg` is a plain JPEG that the screen stretches to a box of about
2.3:1 whatever its size (256x128 to 1024x440 all fill it; 512x220 is natural) and draws the
playlist name over its bottom edge; the Searching screen shows it larger. A missing image
leaves a dark box (the 404 is remembered until the game restarts). The generator adds
`"image": "file.jpg"` per playlist (relative to `DATA_DIR`, else `DATA_DIR/images/<id>.jpg`)
and lists it in the manifest like the other files. The category's own image (`u16 image`
in the category record) is built into the game.

Bitstreams are read most significant bit first in 64-bit big-endian words. The `mhcf`
and `gset` payloads are compressed: a 14-bit byte count, then that many bytes: a u32
big-endian uncompressed size and a zlib stream (`sub_822D8E38`, `sub_824E7FC8` →
`uncompress` at 0x827D9860). They decode to fixed-size big-endian structures.

**Hopper table** (0x8F48 bytes, at 0x83382E80 in the object at 0x83378FB8): +0 u32 hopper
count, +4 u32 category count, 16 categories of 0x44 bytes at +8 ({u16 id, char name[32],
u16 image, …}), 32 hoppers of 0x458 bytes at +0x448. Hopper fields:

| Offset | Meaning |
| --- | --- |
| +0x00 | char name[32], shown in the lobby |
| +0x20 | salted SHA-1 of the hopper's game set |
| +0x34 / +0x36 | u16 id / u16 category |
| +0x44 | byte, selection group |
| +0x48 / +0x50 | u64 start / end time, 0 = always (errors 0x32 / 0x33) |
| +0x58 / +0x5C, +0x60 / +0x64, +0x68 / +0x6C | i32 player requirement ranges (games played, experience, rank; a 0 maximum = none) checked per player by `sub_82290EA8` |
| +0x70 / +0x74 | i32 min / max party size (errors 0x22 / 0x21) |
| +0x78 / +0x7C | i32 range of another player value (errors 0x24 / 0x23) |
| +0x80 | i32 required access bit, −1 = none |
| +0x84 | i32 account type: 0 / 1 / 2 = any |
| +0x88 – +0x8C | bytes: requirement applies to every party member rather than one |
| +0x93, +0x94, +0xB6, +0xB8 | bytes: further start checks (0x35, 0x25, 0x2D, 0x34) |
| +0x98 … +0xB0 | voting: i32 options per vote (1–3), +0x9C (0–4), rounds (1–8), +0xA4 (0–8), +0xA8 (0–4), +0xAC (1–30), byte +0xB0 (3 bits); checked by `sub_82291260` and, stricter, by the other players' decoder of the "matchmaking-hopper" session parameter (`sub_822D9508`) |
| +0x130 | i32 seconds before the search widens to its next stage |
| +0x134 | four search stages of 0x94 bytes (`sub_822D0760`): per stage skill / range limits (+0x04 byte + i32, +0x0C byte + float, +0x14 byte + 3 floats, +0x24 / +0x25 bytes) and the ping limit, base + step × increment (+0x28, +0x2C; `sub_822D0D68`). A limit of 0 ms rejects every session found (`sub_822CD310`) |
| +0x384 | byte, variant source: 0 game and map variant files, 1 or 2 game variant only (the match then never starts: `sub_82280140` needs a map variant), 3 none (keeps the session's previous variant; the first game of a session reads a null pointer in `sub_8227FAA0`) |
| +0x385 | byte, no teams |
| +0x388 / +0x38C | i32 min / max players without teams (`sub_82286418`, `sub_8227C040`); with a minimum of 1 a lone player's match starts at once |
| +0x394 / +0x398 | i32 min / max team count (teams) |
| +0x39C | 8 teams of 0x10 bytes: i32 min players (0 = unused), i32 max players, then two bytes (+0x8, +0xC) the "matchmaking-hopper" parameter sends in 3 bits each (`sub_8227C1D8`, `sub_822CFBA8`) |

`sub_82290028` returns why a party cannot start a hopper (0x32 not started, 0x33 expired,
0x22 / 0x21 party too small / large, 0x25, 0x34, 0x26 – 0x2F per-player requirements, or
the value of `sub_8228FB68`: title files missing or failed, the Arena check, the game set
still downloading (7 = game set failed)).

**Game set** (0xD404 bytes): u32 count, then entries of 0xD4 bytes: +0 i32 weight (0 =
off), +4 / +8 i32 min / max players, +0x14 i32 (at most 1), +0x18 i32 (at least 50),
+0x2C flags (bit 0 clear: +0x34–+0x3F must be zero), +0x2E i16 (0–255), +0x30 i32 (0–31),
+0x44 i32 map id (must be in the network configuration's map table), +0x48 game variant
{+0 u8 used, +0x11 char name[32], +0x31 salted SHA-1}, +0x8D map variant (same layout);
"used" must match the hopper's variant source (`sub_82291260`, `sub_822916A0`). The
variants are `%05u/%s_054.bin` and `%05u/map_variants/%s_031.bin`.

**Game variants** (`gvar` v54) hold the same bitstream (decoded by `sub_824CD9E8`) as the
`mpvr` v54 chunk of a game type the player saves: `mpvr` is {salted SHA-1 of the size
and the bitstream, 4 bytes, u32 size, the bitstream (at most 0x5000 bytes)}
(`sub_824CDDD8`), so the generator takes the file uploaded to the File Share (its `_cmp`
chunk zlib-expanded) and serves the bitstream as `gvar`.

**Map variants** (`mvar` v31): {salted SHA-1 of the next two fields, u32 size, bitstream
(at most 0x7000 bytes)} (`sub_824D4980`). The bitstream (`sub_824CFBA0`): the content
header (`sub_824E9E88`: type + 1 in 4 bits, u32 size, 4 u64 ids, activity + 1, mode,
engine in 3 bits each, u32 map id, a byte, creator and modifier {u64 time, u64 XUID, name in
8-bit characters NUL-ended below 16, online bit}, name and description in 16-bit characters
NUL-ended below 128, then fields that depend on type, activity and mode), version 31 (8
bits), u32 map checksum (−1 = not checked), u32, budget entry count (9 bits), u32 map id,
built-in and a second flag (1 bit each), 6 u32 world bounds, 2 u32s, a 9-bit count of
12-bit entries, one presence bit (then the object) for each of the 651 object slots, and 3
bytes per budget entry. The default variant: type 5, activity 4, mode 3, no objects, no
budget entries, bounds ±FLT_MAX, built-in set. A Forge map uploaded to the File Share is
served as its saved `mvar` chunk. Map ids come from the operator's `maps/info/*.mapinfo`
(`levl` v7: u32 map id at +0, English name at +8): Sword Base 1000, Countdown 1020,
Boardwalk 1035, Zealot 1040, Powerhouse 1055, Boneyard 1080, Reflection 1150, Spire 1200,
Forge World 3006.

#### The Playlist screen

The lobby (`sub_8227D688`) turns the hopper table into the "matchmaking-hopper-list" session
parameter: the categories {u16 id, name}, and per hopper a 0x34-byte entry {u16 id, u16
category (+0x36), u8 +0x38, u8 +0x44, u32 +0x3C, u8 visible (`sub_8228F938`), u8 joinable,
u8 +0x384, u8 +0x39, char name[32]}. While doing so it sets bit (+0x38) of a 16-bit mask
(+0xC of the parameter) for every visible hopper. The Playlist screen lists the categories
whose bit is set (`sub_826F4ED0`) and, for a category, the hoppers whose category field
equals the bit number (`sub_826F52A8`, `sub_82792C18`, which also takes the description
from `matchmaking_hopper_descriptions` and the player count from the hopper statistics).
So hopper +0x38 must hold the category id, and category ids must be 0-15. With +0x38 left
at 0 and the category id 1, the screen had one unnamed category and no playlists, though
the lobby showed the selected playlist (it reads the entry by id). The generator now sets
+0x38: the screen lists "Reach Live" with its playlists, shows the highlighted one's
description, and selecting one changes the lobby's playlist.

#### "The Halo: Reach server is unavailable"

The lobby state (`sub_8227D688`) is "available" when the hopper table is loaded and
`sub_820B3D68` holds: no Arena-disable flag in the network configuration and byte
0x8330D92C bit 0, which is set when a `GET /gameapi_omaha/ArenaGetSeasonStats.ashx
?machineId&players&version=3` reply parses (`sub_822D84A8` → `sub_822D7F20`). The server
answers it with `arhs` v3: {i32 season (replies below 1 are dropped), i32, u64, u8, u8, u32
count (≤ 32), then 30-byte ratings starting with an XUID}; season 1 with no ratings is
enough.

#### Searching and joining

Matchmaking runs on the network life cycle at 0x8322AD50 (+0 current state, +8 the state
objects; each has a vtable {update, check, enter, exit, name}). States seen: 1 pre-game
(lobby), 7 matchmaking-start, 8 matchmaking-recycle, 11 find-and-assemble-match, 12
assemble-match, 13 arbitration, 14 select-host, 15 prepare-map, 16 in-match, 17–19 end of
match. While searching, each party leader hosts a "group" session (XSessionCreate flags
0x2D: host, stats, matchmaking, peer network; 0x27 is the party's presence session) and
every 10 s runs the "session-search-weighted" task (task object `*0x83150E24`, results at
+0x18): XSessionSearchWeighted (0xB0065) {procedure, user, max results, weighted
property / context counts (u16 each) and pointers, property / context counts (u16
each), properties, contexts, results size, results}. Reach searches with 7 properties
(0x10000015 = the hopper id, 0x10000047/48, 0x1000003A, 0x10000050, 0x1000004D,
0x1000003B) and contexts 0x800A / 0x800B; it sets 0x10000015 on its own user only for the
search.

`live_xmsg.cpp` (section "Matchmaking") publishes the newest session we host with
XSESSION_CREATE_USES_MATCHMAKING to the server with its playlist (the hopper id we last
searched for), slots, our user properties and contexts (XUserSetProperty 0xB0007 /
XUserSetContext 0xB0006 are recorded) and its QoS data (XNetQosListen). A search sends
the playlist to the server and waits up to 0.5 s for the answer, then fills the results
(0x5C-byte XSESSION_SEARCHRESULTs with the properties and contexts after them). The search
key is the playlist with the tick rate in its top byte when it is not 30
(`docs/framerate.md`, players with different frame rates are not matched). The
server (`reach_live_server.py`, messages 15–17) returns the room's sessions for that
playlist; a searcher that hosts a session itself only gets older ones, otherwise two
groups that find each other join each other and both wait forever.

The game copies the results (`sub_8226CE50`: at most 4 properties, 2 contexts, property
ids from the table at 0x82A44718, index 2 being a string), skips its own XNADDR, makes a
candidate per result (`sub_8231CD68`; candidates at `*0x83559218`, 0x1308 bytes, status
byte at +0x58), probes it with XNetQosLookup (RTT and bandwidth land at candidate
+0x6C…+0x80) and joins a candidate whose median RTT is within the stage's ping limit
(`sub_822CD310`), through the same deferred join record (0x832FAD50) a friend join uses.

After the merge the host runs arbitration (13: every member must report the current
session update and status bit 5, `sub_8229C7F8`), host selection (14) and prepare-map (15:
"Loading game and map data", then the countdown), and `sub_8228C200` (in-match) starts the
game through `sub_82280140`. The matchmaking progress parameter the members follow is set
by `sub_822A2FD0`: 0xB searching, 0xC–0xF assembly and arbitration, 0x10 countdown, 0x11
set by `sub_8228C200` when the game cannot start or has ended, then 0x12 and 0x13.


#### Progression from matchmade games

Matchmade games pay out as on LIVE with no extra code (details and the reverse engineering
in `docs/progression_re.md` section 3.1). At the end of a game each player's console awards
the game-completion Credits from the `gcrg` tag: in matchmaking 28 cR per minute (up to 20
minutes), ×1.2 for the winner or ×1.1 for the top half, against 5 cR per minute in custom
games once the profile has synced with a server. It saves the profile and runs a rewards
sync with the server at once, so the server's record (`DATA_DIR/players/<xuid>.json`) and
the profile agree after every game, and either one restores the other: a restarted game
loads the same total, and a reinstalled one gets the server's.

Kills in a matchmade game count for the daily and weekly challenges and for commendations
the same way as elsewhere: the incident system (`Function_821DBB88`) has per-reaction
flags for "only in matchmaking" and "only outside it". Commendation progress is part of the
rewards block (block A +0x008), so the server keeps it with the Credits.

Tested with Carter (host) and Jun in 1-minute Team Slayer games: four 0-0 games paid +30 cR
each to both (5578 → 5698, 5645 → 5765), both totals survived a restart of both games, and
a 2-0 game in which Carter killed Jun twice paid Carter 11 cR per kill (two commendations)
plus 33 for the win (5698 → 5753) and Jun 28 (5765 → 5793); Carter's "kill 50 enemies"
daily went to 2/50 and the server stored the challenge progress and four commendation
counters. Capture the Flag, Oddball and King of the Hill games paid the same +30 per 0-0
game; at the end of the round Carter had 5993 cR and Jun 6033, and the challenge progress
and commendation counters were still there after each restart.

**Playlist Credits options.** When the host starts a matchmade game it copies the hopper's
matchmaking options into the game options (`sub_8227FE48`, game options +0xFA54; +0xFA50 is
the "matchmaking game" flag the incident filter tests as game globals +0xFA60), and the
chosen game set entry's +0x2C…+0x3C into the "matchmaking-game-configuration" session
parameter (`sub_8228B300`). At the end of the game they are in the game results globals
(0x8373FE80) that `GameResults_AwardGameCompletionCookies` reads:

| Game results | Source | Effect |
|---|---|---|
| +0x70 u8 | hopper +0x39 | `gcrg` multiplayer entry: 0 or 1 = 28 cR/min with ×1.2 winner and ×1.1 top-half bonuses, 2 = 27 cR/min, no top-half bonus |
| +0xAC bit 0 | game set entry +0x2C bit 0 | turns the three multipliers on |
| +0xB4 / +0xB8 / +0xBC f32 | game set entry +0x34 / +0x38 / +0x3C | multiply the entry's rate, winner bonus and top-half bonus |
| +0x78 / +0x7C | matchmaking-hopper parameter +0x6C / +0x70 | bonus cR (award kind 8) when the game lasted that many minutes; the lobby (`sub_8227D688`) always leaves them 0 |

The game set check (`sub_82291260`) wants the three floats to be zero when bit 0 is off;
+0x2E (u16, 0-255) and +0x30 (i32, 0-31) are unknown. The generator sets them from a
playlist's (or a game's) `"credits_multiplier"` (e.g. 2 for double cR), and optionally
`"winner_multiplier"` and `"top_half_multiplier"` (default 1). Tested: with
`"credits_multiplier": 2` on Team Slayer, a 1-minute 0-0 game paid each player 61 cR
(28 × 2 × 1.1) instead of 30, and the game results showed the multipliers (2, 1, 1).
