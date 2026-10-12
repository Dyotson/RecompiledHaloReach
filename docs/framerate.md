# Frame rate above 30 fps: investigation

Status: the 45 fps floor is fixed (a 10 ms sleep in the SDK's music player app, below), and
`frame_rate = 60` in `reach.toml` (or `--frame_rate=60`) runs the game at 60 fps. The game
ticks at half the display's refresh rate, so at 60 fps it also simulates 60 ticks a second
(below); the campaign's cinematics needed a fix for that. Network co-op (campaign and
Firefight) runs at 30 ticks a second on every machine, so players at 30 and 60 fps play
together in every mode ("Mixed co-op", below). Addresses are in `default.xex` (base
version); `docs/perf.md` explains the measurements.

## How Reach paces itself

**Frame limiter.** The main loop (`sub_822377E0`) calls `Main_FrameLoopStep`
(`sub_820E1D18`) until the game quits. Before each frame it waits on the vblank event until
a 64-bit vblank counter at `0x8319F800` (incremented by the game's vblank interrupt
callback) has moved on by 2 since the last frame (`li r31, 2` at `0x820E1E18`; the
`addi r9, r11, 2` at `0x820E1E04` on another path), or by 3 after a flag at
`0x8367C184` is set. That flag is part of the network clock sync: `sub_8247BA48`, which
lines a client's game time up with the host's, sets it when the client runs ahead. At the
console's 60 Hz that is 30 fps. (The intro video and loading screens run at 60 fps; how they
get past this wait was not traced. A byte at `0x831510D9` skips it.)

**Game time.** `sub_82440410` is the per-frame game update. It measures the real time since
the last frame, and `sub_820EE150` (game time update) turns it into whole ticks:

```
ticks    = floor(tick_rate * dt * game_speed + leftover)    // tick_rate: short at game_time+4 (30)
leftover = the fraction left over, kept at game_time+0x14   // seconds per tick: float at +8 (1/30)
```

with a cap on ticks per frame (3 while a cinematic plays). In a networked game the tick count
comes from the simulation's network clock instead (`sub_8220F130`), adjusted by
`sub_8247BA48`. The ticks then run the simulation (`sub_821F4DF0`, profiled as
`main_thread_raw_game_ticks_ms`), and player control gets `ticks * seconds_per_tick`
(`sub_82442790` → `sub_8247C978`, the function the mouse-look hooks live in). So game time
follows real time whatever the frame rate.

**The tick rate is half the refresh rate.** The game options get `round(refresh rate) / 2`
as their tick rate (`sub_82476A60`, options + 0xC, 60 if the rate is unknown), and the game
time globals take it from there (`sub_8247B530`; TLS + 0x6C: +4 tick rate, +8 seconds per
tick, +0xC game time in ticks). That is 30 at the console's 60 Hz, 25 on a 50 Hz PAL set,
and 60 with `frame_rate = 60` (a 120 Hz vblank): checkpoint saves logged `rate=60`, and
during cinematics every 16.7 ms frame ran one tick. Durations are converted with it
(`game_seconds_to_ticks`, `sub_8214FAE8`): a script's `sleep 1` is 1/30 s, so 2 ticks at 60.
Everything measured below at a 120 Hz vblank ran at 60 ticks a second. After the ticks, some
systems update once per frame with the real or game delta time: `sub_82208CC8` (per-player
frame callbacks, effects) and six `*(dt)` calls at the end of `sub_82440410`.

**No render interpolation on the Xbox 360.** The 360 build has no interpolation system (its
only "interpolat" string is the scenario interpolators, a scripting feature). MCC's PC build
of Reach (`haloreach.dll`) added one: a `halo_interpolation` thread-local block, buffers of
110 MB and 2 × 27 MB allocated next to it, and locks named "CS:Interpolator Pending New
Objects" / "Pending Deleted Objects". MCC also kept Reach at 30 ticks a second: its 2024
change to 60 Hz ticks covered Halo 2, 3, 4 and Halo 2 Anniversary, not Reach, and needed
follow-up fixes (hitscan range and projectile speed halved per tick).

**Community patches.** xenia-canary/game-patches has no frame rate patch for Reach (base or
TU1), nor for Halo 3, ODST or Halo 4; only aspect ratio, lens flare and similar patches.

## Experiments

All with `tools/bench.sh` and `RECOMP_PERF`; the guest vblank was raised to 120 Hz with
`--video_mode_refresh_rate=120`, so the unchanged 2-vblank limiter targets 60 fps. These ran
next to other games (contended): fine for behaviour, not for frame rates.

- **The limiter follows the vblank.** The counter at `0x8319F800` advanced 242 in 2 s (read
  with `tools/guestmem.py`), and the game rendered 43-46 fps instead of 30 on the menus and
  in a SWAT game. (The intro video ran at 111 fps: it presents once per vblank.)
- **The simulation stays at real-time speed.** The SWAT clock went from 11:58 to 11:15 in
  42 s of real time at 43 fps (at 60 ticks a second, see "The tick rate is half the refresh
  rate").
- **The camera moves on every frame.** Turning in place (right stick held) at ~45 fps, 13
  consecutive frames dumped from guest memory were each rotated 68-120 px further than the
  one before; with the view updated only on 30 Hz ticks, about one frame in three would
  repeat the previous view. A hash of every presented frame found no frame identical to the
  one before it in 2000+ frames of play. Effects and other per-frame systems move too.
  With 60 ticks a second most frames run a tick, so this does not show what a frame
  without a tick looks like.

**Clean measurements** (reserved idle machine, 2026-10-10, commit `cb092d4`, windowed): with
the 2-vblank limiter at a 120 Hz vblank the main menu ran 45.6 fps and a SWAT game 45.1 fps,
both with a flat frame time (menu p50 21.9 ms, p99 22.3; game p50 22.2, p99 23.6); the title
screen 45.9 fps at 21.8 ms, the same with `--vulkan_readback_resolve` on or off.

**Where the 22 ms go (functional runs, title screen).** Not the limiter: with `--vsync=false`
(a vblank every millisecond, so the limiter never waits) the title screen still runs 45.7 fps
at 21.8 ms. Not CPU work: over 10 s the guest RENDER thread used 54% of a core, MAIN_THREAD
10%, nothing else more than 10%. gdb stack samples (12): MAIN_THREAD sat in an infinite wait
on a sync object in `sub_820E40F0` (called from `Main_FrameLoopStep` via `sub_820E1B60`) in 9;
RENDER either waited on an event in `sub_820CCFD8` (6) or spun in `sub_820CA090`'s loop
that polls for the main thread's next frame packet for up to 66 ms (5); the GPU command
processor thread was idle in 10 of 12. So the frame is bounded by a fixed hand-off or
synchronization latency, not by work.

## The 45 fps floor: a sleep in XMPGetPlaybackController (fixed)

`REACH_SYNCTRACE` (`src/debug/sync_trace.cpp`, see below) timed every kernel wait and
signal for 3 s on the title screen at a 120 Hz vblank (46.0 fps):

- MAIN_THREAD waited 88% of the time, 19.4 ms per frame, on the event in `sub_820E40F0`
  (handle F8000050). It was set each time by the guest thread named **AUDIO** (entry near
  `0x8242AB0C`, call site `0x8243FB88`), not by RENDER.
- RENDER waited 43%: 9.3 ms per frame on semaphores F8000070/74 at the end of its frame
  (`sub_8244C018`, the frame packet's semaphore), released by the same AUDIO thread
  (`0x82432898`).
- AUDIO itself waited only 7% (1.5 ms per frame, for MAIN_THREAD's per-frame kick, event
  F800004C set at `0x8220F074`). eu-stack samples of it: 12 of 12 in
  `rex::thread::Sleep` ← `XmpApp::DispatchMessageSync` ← `XMsgInProcessCall`.

The SDK answers XMPGetPlaybackController (music player app 0xFA, message 0x7001B) after
`Sleep(10 ms)` on any thread but the main thread, a throttle Xenia added for a game that
calls it in a tight loop (`kernel/xam/apps/xmp_app.cpp`). Reach's AUDIO thread calls it
during every frame (about 20 ms of sleeps), and MAIN_THREAD and RENDER wait for that thread,
so every frame took at least ~21.8 ms. At the console's 60 Hz the 2-vblank limiter (33.3 ms)
hid it. `live_xmsg.cpp` now answers the message itself (no controller, not locked, as the SDK
does) without the sleep; no SDK change.

After the fix, at a 120 Hz vblank the main thread waits 86% of the time on the vblank event
in the limiter (`0x820E1E58`) and AUDIO 99% for its kick: the limiter is the only cap.

| Run (contended, windowed) | before | after |
| --- | --- | --- |
| title screen, 120 Hz vblank | 45.9 fps, p50 21.8 ms, p99 22.1 | 60.0 fps, p50 16.8, p99 16.9 |
| SWAT on Sword Base, turning, `frame_rate = 60` | 45.1 fps, p50 22.2 (clean, above) | 60.0 fps, p50 16.8, p99 17.4-17.9 |
| title screen, `--vsync=false` (1 kHz vblank) | 45.7 fps, p50 21.8 | ~235 fps, p50 4.2, p95 4.8 |
| title screen, defaults | 30.0 fps, p50 33.6 | 30.0 fps, p50 33.6 |

The SWAT game clock ran 31 s in 30.5 s of real time at 60 fps.

## How a frame is scheduled in vblanks

The limiter is one of several places that count vblanks:

- `Main_FrameLoopStep` (`0x820E1D18`): waits until the vblank counter is 2 (or 3) past the
  count saved after the previous frame (above).
- `sub_820F09D0`, first thing in the game update (`sub_82440410`): waits for at least one
  vblank since the last update, sets the frame time to (vblanks since the last update) /
  refresh rate (the video mode's), and stores the vblank this frame is to be shown at: the
  update's vblank + 4 with the render thread (+2 without) at `*(*(r13) + 324) + 32`.
- RENDER, `sub_820E19E0` (from `0x821F9B5C`): waits until a flip happened since its last
  frame (`0x8319F814`, the flip count D3D's vblank interrupt hands the game's vblank callback
  `sub_8223C5C0`) and until the vblank counter reaches that target - 2.
- D3D queues each swap's flip for the vblank at least its presentation interval after the
  last flip (`sub_826C83B0`); Reach's swaps carry interval 1 (0x10A).

Halving only the limiter (a hook that moved the saved count back one vblank) made
MAIN_THREAD loop at 60 Hz, but it handed RENDER a frame only on every second loop (the
frame packet copy in `sub_820E1B60` ran 30 times a second), so the screen stayed at 30 fps;
moving the render target back 2 vblanks as well changed nothing. Which check skips the render
was not traced. Raising the vblank rate instead scales all of these together, and the frame
time stays right because it divides by the video mode's refresh rate.

## The frame_rate setting

`frame_rate = 60` (`src/main.cpp`; 30 is the default, the setting needs a restart) makes
the guest vblank 120 Hz (`video_mode_refresh_rate`, which `VdQueryVideoMode` reports and the
SDK's VSync thread follows), unless `video_mode_refresh_rate` is set explicitly. The game
then ticks 60 times a second. Checked: title screen and menus 60 fps, a SWAT game at 60 fps
with the clock at real-time speed, the intro video, which presents every vblank (120 fps)
but still ends at 46 s like at 60 Hz, the campaign, Firefight, matchmaking and System Link
(below). In network games the clock sync's extra vblank for a client running ahead becomes
8.3 ms instead of 16.7 ms. Co-op over the network runs at 30 ticks a second also at 60 fps,
and players with different frame rates play together ("Network games with different frame
rates", below).

Beyond 60: `frame_rate` takes 30 or 60 to 240 (120 and 144 are the tested ones); the guest
vblank runs at twice the frame rate, and the game ticks at most 72 times a second, two frames
per tick at 120 and 144 ("120 and 144 fps", below). The SDK clamps the reported refresh rate to 24-240 Hz (`xboxkrnl_video.cpp`);
SDK patch 0037 raises that to 480 Hz, so 144 fps (a 288 Hz vblank) needs the patched
runtime; without it the game logs a warning and runs at 120. The SDK's VSync thread checks
for due vblanks every 1 ms, so each vblank comes up to 1 ms late (p50 33.6 ms rather than
33.3 at 30 fps).

**`REACH_SYNCTRACE=FILE`** records the guest's kernel waits and signals (thread, handle,
duration, guest call stack) for `REACH_SYNCTRACE_SECS` seconds (default 3) after
`touch FILE.trigger`, and marks each VdSwap. It is how the sleep above was found: per
thread, the longest waits and the thread that signalled each one show the critical path.

## Options

**(a) A higher tick rate (60 Hz).** This is what `frame_rate = 60` does, through the game's
own rule (tick rate = refresh rate / 2, above), which this section did not know when it was
written. The rate is data (`game_time+4`, with 1/30 at +8), but the game is built around 30
ticks a second: animations advance per tick, scripts and AI
count ticks, physics steps per tick, and the network protocol exchanges ticks, so every peer
(Reach Live, System Link) would have to run the same rate. The constant 1/30 also appears
about 90 times in the read-only data. MCC moved four other Halo games to 60 Hz ticks and
had to fix hitscan range and projectile speed afterwards; it did not do it for Reach. Not
recommended as a design, but it is what runs today; the campaign check below found one
script that depended on it.

**(b) 30 Hz ticks, interpolated rendering.** What MCC does: keep the previous and current
tick's state of every rendered object (node matrices, camera) and draw at
`previous + (current - previous) × fraction`, the fraction being the leftover tick already
kept at `game_time+0x14`. Smooth motion for everything, one tick of extra latency, no change
to gameplay or the network. It is the largest change: find where render data is taken from
the game state, snapshot it per tick, and handle objects created, deleted or teleported
between ticks (MCC's "Pending New/Deleted Objects" locks and large buffers show the size of
it).

**(c) Unlock the limiter, keep everything else.** Wait 1 vblank instead of 2 in
`Main_FrameLoopStep` (a mid-function hook on `0x820E1E18`/`0x820E1E04` that keeps the
network clock sync's extra vblank, or a higher guest vblank rate). Since the camera and
effects already update per frame, turning and looking around get 60 fps or more; objects
that only move on ticks still step at 30 Hz. No gameplay
or network change, as ticks are untouched (through a higher vblank rate they are not
untouched: the tick rate follows it, see (a)). Cost: about twice the CPU and GPU work per second,
so the port has to hold 60 fps first (it does now: the title screen runs ~235 fps with the
limiter off). Done as `frame_rate = 60` through the vblank rate (above): a hook on the limiter
alone does not raise the frame rate.

**Next:** measure on a clean machine. Then measure what still looks like 30 Hz (objects, the
first-person weapon), now also in network co-op, which runs at 30 ticks a second at 60 fps
("Mixed co-op", below), and decide whether (b) is worth it, starting with the objects closest
to the camera. For 144 fps, see the limits under "The frame_rate setting".

## Checks at frame_rate = 60 beyond SWAT (2026-10-10, contended, windowed)

**Campaign, Winter Contingency (offline, Easy): the opening looped; fixed.** The opening plays
at a steady 60.0 fps (p50 16.8 ms, p99 ~25 ms, one 180-450 ms hitch at the level load). Before
the fix, at the end of the opening cinematic (about 3:37 after the level loads, the shot after
the Falcon seat) the game reverted to a checkpoint and played the whole opening again from
the planet (in all 5 runs at 60, none of 3 at 30).

The revert is the normal end of a cinematic. `checkpoint_revert` (`sub_824285C0`) is called
by the main game's per-frame update (`sub_8243FF28`, return address 0x8243FFF4) for a revert
request at 0x8367C0AA with its reason at 0x8367C0AB; reason 12 is set only by the script
function `game_revert` (hs function 0x445, `sub_825CD110`; the hs function table is at
0x82A31040). The script that called it (`REACH_GSTRACE=1` now logs the script's frames) does

```
(begin (cinematic_skip_stop_internal) (if (not (game_reverted)) (begin (game_revert) (sleep 1))))
```

the same at 30 fps (logged at game time 6519 at 30, 13038 at 60). `game_reverted` (hs function
0x47D, `sub_825CD9C0`) is `game time == 0x83597B00`, the game time the last revert restored.
So a cinematic is played between a checkpoint saved by `cinematic_skip_start` and a revert to
it; the restored script, asleep in `sleep_until (not (game_saving)) 1` after the save, wakes
in the reverted tick, sees `game_reverted` and skips what it has just shown, as a skip does.

The checkpoint is taken (`sub_82428468`, at the start of the next frame) one tick after the
script asks for it. At 30 ticks a second the script's `sleep_until ... 1` (1/30 s) is that same
tick: save at game time 3, the restored script ran at 3, `game_reverted` true. At 60 the sleep
is 2 ticks: save at 5, the script slept until 6, so after the revert to 5 it asked at 6 and got
false, played the cinematic again and reverted at its end, forever. Logged with
`REACH_GSTRACE=1` before the fix: `checkpoint_save time=5 rate=60`, after the revert `frame ticks=1 time=5`,
`ticks=0 time=6`, `ticks=1 time=6` and `game_reverted? time=6 revert_time=5`.

Fix (`src/hooks/frame_rate.cpp`): `game_reverted` also counts the rest of the 30 Hz tick
the revert happened in (game time - revert time < round(tick rate / 30)), so it is unchanged
at 30 ticks a second. Nothing else reads 0x83597B00 but the checkpoint serialization. With
it the opening reaches gameplay at 60 ("use to look around", "Current objective: locate
distress beacon", "Checkpoint... done", the Falcon lands); the second cinematic, the Falcon
flight, ends the same way (both log `frame_rate: game_reverted 1 tick(s) after the revert`).
The 30 fps control run is unchanged (no such line; same reverts at the same points). The
`XmaContext 127: There are no bits to copy!` error shows up once in the opening at both 30
and 60.

**Cinematic cameras moved at 30 Hz; fixed (2026-10-12).** A cinematic scene shot's camera
is a list of 30 Hz frames. The scripted camera picks frame `(int)(ticks * seconds_per_tick *
30)`, so at 60 ticks a second it moved on every other tick only, while the objects it films
moved on every tick. In the Falcon flight the camera then trailed the Falcon by 0.4 m on
every other frame: the near Spartans dropped out and the cabin jumped back and forth.
`src/hooks/frame_rate.cpp` now blends the camera between that frame and the next
(`sub_8249FEB0`). Details: `docs/rendering.md`. Other places that count 30 Hz frames against
ticks are listed there under "Left over".

**Campaign after the Falcon (60).** Out of the Falcon, walking, turning, firing and reloading the
assault rifle, "Loading... done", "Checkpoint... done". Game time 600 ticks per 10 s of wall
time (`REACH_GSTRACE=1` logs `clock time=... rate=...` every 10 s); no other reverts than the
two cinematic ends. Guest 56 fps over the whole run (contended).

**Firefight, Overlook, Normal, solo (60).** About 2 minutes of play: moving, firing, taking
damage and recharging shields, the out-of-bounds countdown, a death, the respawn countdown and
respawning. Game time 600 ticks per 10 s; the HUD clock went from 1:02 to 2:06 between two
screenshots 67 s apart (each taken about 1 s after its inputs). No audio errors in the log.
Guest 51.8 fps (contended).

**Matchmaking, own Reach Live server (port 21500), Team Slayer 1v1 on Countdown with the
1-minute game type.** 60/60: matched in under 40 s, played (the HUD clock went from :18 to :03
in 14 s), post-game, searched again and were matched into the next game. 60/30: matched, played
the whole game, both showed the scoreboard and searched again; no errors. In that game each
machine ran its own tick rate (60 and 30: multiplayer is a distributed game and every machine
takes the rate from its own display). The two players never met, so how a 60-tick player looks
to a 30-tick one (movement, hits) is not checked.

**System Link Firefight (co-op).** 60/60 works: both ran 60 ticks with their game times in
step (8306 and 8300 at the same moment), kills, deaths and loadout respawns for about 2
minutes. 60/30 failed as the game started, with "GAME ERROR: An error occurred while playing
co-op" on both: campaign co-op and Firefight are synchronous games (network type sync-server /
sync-client, options + 4 = 2 / 3, names at 0x82A3A744), which run the same simulation on every
machine, and the two ran 60 and 30 ticks a second. Forcing 30 ticks in synchronous games
(the network type in the game globals after `sub_82476568` copies the options in at game
start) was not enough at the time: both machines then ran 30 ticks with their game times in
step (232/233, 532/534, ...) for about 40 s, and the same error came. The cause turned out to
be Havok's collision tolerances, computed for the main menu's 60 ticks and kept in the next
game; with that fixed, network co-op now runs at 30 ticks a second on every machine and
60/30 co-op works ("Mixed co-op", below).

## Network games with different frame rates

Every machine takes its tick rate from its own display, also in network games, so in
multiplayer (a distributed game: each machine simulates its own players) a 60 fps player
simulates 60 ticks a second next to a 30 fps one, which works ("Mixed multiplayer", below).
Co-op over the network (campaign and Firefight, synchronous games) needs the same tick rate on
every machine and runs at 30 ticks a second everywhere ("Mixed co-op", below). So players at
30 and 60 fps play together in every mode: System Link, the Reach Live roster and matchmaking.

Until 2026-10-12 machines with different frame rates were kept apart. That gate is still in
`src/hooks/frame_rate_net.cpp` and applies only when co-op runs at the display's rate
(`REACH_SYNC_TICKS30=0`, so 60/60 co-op runs 60 ticks a second); it uses the game's own check
that keeps different builds apart:

- The build number (11860, at `0x82BD2660`) travels in the join-request (message 8: +4 the
  lowest host version the joiner takes, +8 its own version, +0xC the executable type 5),
  which the host refuses (`sub_82319380`) with join-refuse reason 25 `peer-version-too-low` or
  26 `host-version-too-low`, and in the header of a session's status (`sub_82270170`: +4
  executable type, +8 version, +0xC compatible version), which System Link's
  broadcast-reply and a Live session's QoS data carry. The menus offer to join a session only
  when its range takes our version (`sub_826EA6C8`, from the System Link list and the
  roster), and the roster rates the session as not joinable otherwise (`sub_822CABB8`).
- A machine whose co-op does not run 30 ticks a second sends 11860 + (tick rate << 8) in
  these four places (16 bits on the wire: 0x6A54 at 60). The checks compare what arrives with
  what this machine sends: the same number becomes 11860 again, a different one a version the
  game refuses. Machines with co-op at 30 ticks (all of them by default) send the plain
  number, so they play with each other and with 30 fps builds from before the gate. 60 fps
  builds from 2026-10-11 (co-op at 60 ticks, tagged 0x6A54) stay apart from them, as they
  should. Saved content (films, maps, game types) keeps the plain number; nothing else reads
  the tagged one.
- Matchmaking: the search key a client sends the Reach Live server (`live_xmsg.cpp`) is the
  playlist, with the tick rate in its top byte when the gate applies, so the server only
  matches such a player with players at the same rate (`server/reach_live_server.py` compares
  keys).
- `REACH_MIXED_FRAME_RATE=1` (on every machine) turns the gate off also with
  `REACH_SYNC_TICKS30=0`, for tests.

Checked with the gate on (System Link, two instances, 2026-10-11): 60 host and 60 joiner: X
("Join User") on the host's party joins its Firefight lobby. 60 host and 30 joiner: the host's
lobby is listed
but X does nothing, and both log `frame_rate: a session at 60 ticks a second is not joinable
(we run 30)` (the host for the joiner's main menu party: `at 30 ... (we run 60)`). Same
frame rates behave as before. Matchmaking (Reach Live, Team Slayer, 60 and 30 fps): both
searched for 90 s and never found each other, while the server held the 30 fps player's
session under key 0x65 (playlist 101) and the 60 fps player's under 0x3C000065 (before the
change a 60/30 pair was matched within 40 s). Not exercised: a join that reaches the host's
check (an invite, or a roster join: from the matchmaking lobby the roster's Join Player did
nothing and the invite was not sent, not compared at equal rates), so the join-refuse path
and its log lines are untested.

**Mixed multiplayer.** A short test: System Link custom game, Slayer (free for all) on Sword
Base, 60 fps host and 30 fps client with `REACH_MIXED_FRAME_RATE=1` (2026-10-11).
Each machine ran its own tick rate (60 and 30). Players were put in view of each other by
writing the client's biped position in its own process (writes on the host don't stick: the
client owns its biped's movement).

- Movement: a player's position, read from both processes every 50 ms while it walked forward
  2 s and strafed 2 s, followed the same path on both machines: the 60 tick player as the 30
  tick machine saw it within 3 cm, the 30 tick player on the 60 tick machine within 5 cm, no
  visible lag at 50 ms sampling. Both walked at 2.1-2.2 m/s.
- Shooting and damage, both ways: each saw the other's muzzle flash, tracers and shield flare;
  the 30 fps player killed the 60 fps one with an assault rifle burst and the other way round.
  Both machines showed each kill (medal, "You killed Spartan3", the victim's death camera with
  "Respawn in"), the scoreboard at 1-1 and the respawns.
- Rate of fire: holding the trigger for 2 s fired 21 rounds on both.
- The game clocks agreed.

Earlier, a 60/30 matchmaking pair (before the gate) played a whole 1-minute Team Slayer game
and searched again without errors, though the players never met ("Checks at frame_rate = 60",
above). Not checked: grenades and other projectiles at range, vehicles, more than two players.

## Mixed co-op: what diverged, and the fix (2026-10-12)

**Now.** Campaign and Firefight co-op over the network (synchronous games, network type 2
sync-client / 3 sync-server: every machine runs the same simulation tick for tick) run at
30 ticks a second on every machine (`src/hooks/frame_rate.cpp`). A 60 fps machine keeps
presenting 60 frames a second there, one tick every other frame (option (c) above: the camera
and effects move every frame, objects every tick). Havok's collision tolerances are always
computed for 1/30 s steps, so a 60 fps machine's co-op game is bit for bit the game of a
30 fps machine. Players at 30 and 60 fps play co-op together, and the network gate is off
("Network games with different frame rates", above).

**Root cause: Havok collision tolerances from the main menu.** The game creates its Havok
world at each map load (`sub_8251C3A0`, from the systems' structure-bsp initialization) with
`hkpWorldCinfo::m_expectedMinPsiDeltaTime` = the game time's seconds per tick (cinfo +0x68).
The world constructor (`sub_8287DDB0`) hands it to
`hkpCollisionDispatcher::initCollisionQualityInfo` (`sub_8284FD70`), which stores it in the
dispatcher (a static object at `0x82CC8870`: +0x1E40 the maximum linear velocity, 60.0,
+0x1E44 the minimum step) and derives the TOI separations of its collision quality infos
(+0x1C60, 0x40 bytes each; `m_toiSeparation` and `m_toiExtraSeparation` scale with gravity ×
step²). Right after each world creation `sub_8251D3F8` copies the dispatcher's first 7760
bytes: at the session's first world into a static copy (`0x82AD2AC4`, flag at `0x82AD4914`),
at every later one back from that copy. So the tolerances of the first world, the main
menu's, hold for the whole session. On the console every game runs 30 ticks a second; on a
60 fps machine the menu runs 60, its world asks for 1/60 s, and every later game got 1/60 s
tolerances (TOI separations a quarter of a 30 fps machine's), also a co-op game forced to 30
ticks. A gdb write watchpoint on +0x1E44 shows it in a Firefight game: the world constructor
writes 1/30, then `sub_8251D3F8`'s copy writes 1/60 back, for each of the game's world
creations (two at the start, then one every 10 s in Firefight, every 5 s in the campaign:
the second path, `sub_8251E738` from `sub_8251C8B0`, was not identified further). So the 60 fps machine ran its collisions
with tighter TOI thresholds: the physics props that drop at the start of Overlook came to
rest differently from tick 8, the AI walked into different props later, and the co-op game
failed ("An error occurred while playing co-op").

**Fix.** `sub_8284FD70` is hooked: the dispatcher's quality infos are always computed for
1/30 s (`0x3D088889`, 1.0f / 30, as the game time computes it), the console's tick, whatever
the world asks for, so the menu's copy holds the console's values. At 30 ticks a second the
world always asks for 1/30, so nothing changes there; at 60 ticks (single player,
multiplayer) the TOI thresholds are now the console's instead of the 60 tick menu's (they are
millimetres: -0.0023 m instead of -0.0006 m). The log shows the first initialization and
each change of the requested step: `frame_rate: Havok collision tolerances (dispatcher
0x82cc8870) for steps of 0.016666668 s, computed for 1/30 s`. `REACH_HAVOK_NATIVE_STEP=1`
passes the game's own value (comparisons).

**How it was found** (local games, no network: much faster than co-op pairs).

1. A local reproduction. Solo Firefight on Overlook, game state dumped at ticks 1-90
   (`REACH_GSDUMP_LOCAL=1`): a 30 fps run and a 60 fps run whose Firefight game is forced to
   30 ticks with the main menu still at 60 (`REACH_SYNC_TICKS30=3`) show exactly the
   co-op divergence: objects 391, 392, 394, 397, 400, 407, 412, 416-419 (the dropping
   props) agree through tick 7 and differ from tick 8 (+0x23-0x2b, +0x47-0x7f and the Havok
   fields). Local games are deterministic enough for this: two 30 fps runs agree in every
   object apart from byte +0x23c of the 8db324e6 props (it differs between any two runs).
2. With the main menu at 30 ticks as well (`REACH_SYNC_TICKS30=2`, still 60 fps, two frames
   per tick) the props match the 30 fps run through tick 90. Pacing the forced game to 30 fps
   (`REACH_SYNC_FPS30=2`, one tick per frame, main menu at 60) changes nothing: the props
   still differ from tick 8. So the cause is state left over from the menu's 60 ticks, not
   per-frame updates, the display rate or the game options.
3. Havok's steps are the same: `REACH_HKTRACE=N` logs the first N `hkpSimulation::integrate`
   calls of each game (`sub_82892BB8`: dt, current and PSI time as raw bits) and each world
   creation with the game time's rate. In the Firefight game both runs step 1/30 s from PSI
   time 33.0 with bit-identical times, and create their worlds with 1/30.
4. The executable's writable data at tick 1 (`REACH_GSDUMP_XEX=1`: 0x82A00000-0x839E0000 as
   `xex_Ti.bin`; `fr30/xexdiff.py` in the session scratchpad lists words whose values pair
   up as 30/60 or 1/30 vs 1/60): among the expected ones (the video mode's refresh rate,
   frame pacing counters) the dispatcher block above.
5. With the fix, the forced 60 fps run matches the 30 fps run in every object through tick
   90 (apart from the +0x23c byte), and the 30 fps run is unchanged.

**Co-op checks** (System Link Firefight on Overlook, two idle players, two instances,
2026-10-12):

- 60 fps host, 30 fps client: joined from the System Link list (the gate is off), the game
  ran 140 s (to tick 4200: the AI killed the idle host, who respawned) without the co-op
  error, both at 30 ticks with their game times in step (the old error came after 40-65 s).
  The objects of the two machines agree in every field through tick 900; later only single
  bytes differ, the same kind of noise a 30/30 pair shows.
- 30 fps host, 60 fps client: 420 s (tick 12600, the Firefight clock at 7:21 on both), no
  error; every object field agrees through tick 1800, after that one weapon differs in 4
  single bytes.
- 60 fps host, 60 fps client (both now at 30 ticks): 120 s (to tick 3600) in step, AI
  fighting the players, no error; the objects agree at the dumped ticks 1 and 10.
- 30 fps host, 30 fps client (unchanged: no forced rate, the world always asks for 1/30): 120 s,
  no error; every object agrees through tick 900, then two objects differ in a few single
  bytes, as before.

Single player at 60 fps with the fix (campaign, Winter Contingency on Easy, contended): the
opening and the Falcon flight both end with their revert (`frame_rate: game_reverted 1 tick(s)
after the revert (tick rate 60)`, twice each, as before), then gameplay: out of the Falcon,
walking, turning and firing; guest 60.0 fps, p50 16.8 ms. The cinematic camera hook was not
re-checked frame by frame (nothing it reads changed).

**Mixed multiplayer, longer run (2026-10-12, partial).** System Link custom game, Slayer on
Sword Base (default time limit, played to the end: "Credits earned" on both), 60 fps host and
30 fps client, about 9.5 min, no gate. No error, kick or desync; the game clocks agreed to the
second (5:06/5:06, 1:43/1:43, 0:09/0:09). Both players walked and turned for 24 s; each
player's position read in both processes every 30 ms differed by a median 5 mm (p95 4.8 cm,
max 8 cm, no lag) both ways, so no rubber-banding. The players met only briefly (grenade
thrown by the 30 fps one, seen, no kill). Grenades at range, vehicles and the 30 host / 60
client direction, matchmaking and campaign co-op are still open (paused: low memory).
In this input mapping LT throws a grenade (LB does nothing).

Same run the other way (30 fps host, 60 fps client, Slayer on Sword Base, 7 min, 40 s of the
players placed 2.5 m apart): no error; clocks equal on both machines at every sample (6:44,
5:52, 4:34), scores equal (1-2), positions again within a median 4 mm (p95 4.4 cm, max 8 cm).

**Mixed matchmaking (2026-10-12, local Reach Live server, Team Slayer / Classic Slayer on
Countdown).** 60 fps (Alpha) and 30 fps (Jun) searched, found each other in about 60 s, played
the 1-minute game (both in the same game, clocks equal, scoreboard shown), and searched again,
no error. The 30 fps side saw the session first (1 session, the 60 fps one 0, then both).

**Mixed campaign co-op (2026-10-12, System Link, Noble Actual on Normal, 60 fps host, 30 fps
client).** The cutscene played in step on both (identical frames), then both landed and played
about 6 min of the mission walking, turning and shooting: no error. Positions read in both
processes agreed (median 0, max 7 cm). No checkpoint revert was triggered (nothing killed
the players), and co-op through Reach Live and 30 host / 60 client were not run.

**Not checked yet** (next steps for mixed play):
- Campaign co-op: a checkpoint revert at mixed rates, 30 host / 60 client, and co-op through
  Reach Live instead of System Link (same game code, other transport).
- Matchmaking with the 30 fps player first in the pair's host role (one direction was run).
- Mixed multiplayer: grenades at range hitting, vehicles, more than two players ("Mixed multiplayer", above).
- How co-op looks on a 60 fps machine at 30 ticks (objects step at 30 Hz, the camera every
  frame): a reason to try interpolation (option (b)).
- What re-creates the Havok world every 5-10 s (`sub_8251E738` ← `sub_8251C8B0` ←
  `sub_824DD998`); harmless here, since every creation restores the same tolerances.

**Earlier runs** (2026-10-11, before the cause was known; System Link Firefight, Overlook, two
idle players, 60 fps host with 30 ticks forced, 30 fps client, gate off): both ran 30 ticks
with their game times in step and still got "An error occurred while playing co-op" after
40-65 s (4 runs), also with the host paced to 30 fps. The props differed from tick 5-10; once
the AI moved (ticks ~900-1500), actors, squads, AI bipeds, Havok proxies and props differed
more and more until the error; random seeds, the game time and the players' bipeds stayed
equal. A 30/30 control had no object field difference through tick 3000 apart from 7 bytes in
one Grunt. Frames without a tick on a 60 fps machine write the player control globals (a clock
that advances per frame), the game time's leftover fraction, a game sound counter, render,
effect and HUD state, and one float in a few AI bipeds (Grunts +0x2778, Elites ~+0x3930,
+0.013 per frame; gdb watchpoints on Elite node data caught writes from the `job0` worker
thread, `sub_8242AAA0` → `sub_827D5C90` → `sub_82373010` → `sub_82374010` → `sub_82379DA8`
→ animation code `sub_82128D20`, `sub_8216FE58`, all inside ticks). None of these broke the
game once the tolerances matched.

**Tools** (`src/debug/sync_diff.cpp` unless noted, all off unless their variable is set):

- `REACH_GSDUMP=DIR REACH_GSDUMP_TICKS=T1,T2,...`: in a synchronous game (with
  `REACH_GSDUMP_LOCAL=1` also in local games after the main menu), right after the
  tick that brings the game time to Ti (`sub_821224D8`, one game tick), the live game state
  (0xA70000 bytes at `*0x83597B04`) goes to `DIR/gs_Ti.bin` and the thread globals block
  (`*r13`, 0x800 bytes) to `DIR/tls_Ti.bin`; the log line prints the state's address.
  `REACH_GSDUMP_XEX=1` adds the executable's writable data (`DIR/xex_Ti.bin`, 16 MB). Both
  machines dump the same ticks; `tools/gsdiff.py diff` compares them (the states sit at
  different addresses, so pointer differences of exactly that offset are ignored) and groups
  the differences by thread-globals slot (`gsdiff.py names PID` reads the allocation names from
  the code, e.g. 0x6c "game time globals", 0x74 "player control globals", 0x338 "random
  math") and data array; `gsdiff.py objects` lists differing object fields. A local game's
  state lands at the same address in every run.
- `REACH_FRAMEDIFF=DIR REACH_FRAMEDIFF_TICKS=...`: on a machine with more frames than ticks,
  the state after the frame that ran tick Ti and after the next frame, which ran none
  (`sub_82440410` hooked): what changed was written outside the ticks.
- `REACH_SYNC_TICKS30` (`src/hooks/frame_rate.cpp`): unset or 1, synchronous network games
  at 30 ticks a second (the game globals' copy of the options' tick rate is patched when
  `sub_82476568` copies the options in at game start, and before `sub_8247B530` sets up the
  game time); 0, at the display's rate (the gate comes back); 2, also every local game; 3,
  local games after the main menu (the menu keeps the display's rate).
- `REACH_SYNC_FPS30=1`: synchronous games at most one frame per 4 vblanks at 120 Hz (30 fps,
  one tick per frame); `=2` also local games.
- `REACH_HKTRACE=N`: Havok steps and world creations (step 3 above).
- `reach_in_tick` (a symbol) is 1 while a tick runs, for conditional watchpoints in gdb.
- Session scratchpad `fr30/` (may be gone): `local3.sh TAG NAME INST "ENV" "FLAGS" ...` runs
  solo Firefight games side by side with dumps; `cmp.sh DIR_A DIR_B [TICKS]` counts differing
  objects per dumped tick; `coop2.sh TAG "FLAGS_A" "FLAGS_B"` is a System Link Firefight co-op
  pair (A hosts) with dumps and side-by-side shots; `xexdiff.py`, `objfield.py`,
  `gdbwatch.sh` (a solo game with a gdb write watchpoint on one guest word).

## 120 and 144 fps (2026-10-12, in progress)

**Now.** `frame_rate = 120` or `144` runs a 240 or 288 Hz guest vblank (288 needs SDK patch
0037), so the game draws 120 or 144 frames a second. The simulation ticks at most 72 times a
second, at a whole number of frames per tick: 60 ticks at 120 fps and 72 at 144, each tick
shown for two frames (`src/hooks/frame_rate.cpp`, the display's rate divided by
ceil(rate / 72); 60 fps keeps 60 ticks). Co-op over the network stays at 30 ticks.
`REACH_TICK_RATE_MAX=0` gives the game's own rule back (one tick per frame, 144 at 144 fps),
`=N` another limit.

### The design: 72 ticks, not 144, and no interpolation

Three ways to get 144 frames a second:

1. **144 ticks a second**, the game's own rule (tick rate = refresh / 2). The engine is built
   for a variable tick rate: 427 calls convert seconds with `game_seconds_to_ticks`, 260
   functions read the tick rate and 312 the seconds per tick, and the 30 Hz assumptions found so
   far are fixed for any rate (cinematic camera frames, the revert window, Havok's tolerances,
   cinematic clip planes). It runs: the campaign opening at 144 plays at 140-144 fps with one
   tick per frame, both cinematics end with their revert (`game_reverted 4 tick(s) after the
   revert (tick rate 144)`), the Falcon flight's camera and objects move together on every
   tick (a CINEREC recording of 1500 ticks: no tick where they differ by more than 5 cm, apart
   from the 2 cuts), and gameplay, a revert to the last save from the pause menu and firing all
   work. **But many tick counters are a byte**, a good part of them signed: the unit action
   timers (`unit + *(short *)(unit + 0x1B6)`: +8 ticks elapsed, +9 the event tick, +0xA the
   length, compared as signed chars by the action updates such as `sub_823FCA28`, the grenade
   throw), the player's action lock (`players + 0x88`, clamped to 127), AI and unit timers that
   count a signed byte up and compare `ticks * seconds per tick` with a duration
   (`sub_823BDB98`, `sub_823BDE60`, `sub_823BE420`, `sub_82418610`), and 20 calls that store
   `game_seconds_to_ticks` in a byte without a clamp (the animation lengths of unit actions,
   the 2 s hold of a multiplayer client's predicted grenade count, `sub_82394068`, which is 288
   ticks at 144 and wraps to 32). A signed byte holds 127 ticks: 4.2 s on the console, 2.1 s at
   60, 1.76 s at 72, 0.88 s at 144. A static scan finds 21 more functions with byte tick
   counters next to the tick rate; a complete audit of the engine's byte fields is not
   feasible, so 144 ticks would leave actions longer than 0.88 s broken in ways nobody has
   listed.
2. **30 or 60 ticks and an interpolating renderer** (what MCC's PC Reach does). The Xbox 360
   build has nothing to build on: nothing reads the game time's leftover tick fraction
   (+0x14), and the per-frame updates after the ticks (`sub_82440410`: player control and the
   camera's look, effects, `sub_82208CC8` and six `*(dt)` calls) only cover the view, effects
   and the HUD. MCC's interpolation is a subsystem of its own (`halo_interpolation` thread-local
   block, 110 MB and 2 × 27 MB of buffers, about 30 functions plus hooks in the objects'
   node matrices, cameras, effects and first person, "CS:Interpolator Pending New/Deleted
   Objects" locks for objects created and deleted between ticks). Porting it through hooks in
   a static recompilation is a renderer project of its own, with its own failure modes
   (teleports, attachments, first person).
3. **Fewer ticks than frames, an even number of frames per tick.** What co-op already does on
   60 fps machines (30 ticks, two frames each), with every system at a rate it handles: the
   camera's look, aiming, effects, particles and the HUD update every frame (144 times a
   second), objects and the camera's position every tick (72). At 120 fps this is exactly the
   tested 60 tick simulation. At 144 the byte limits are 1.76 s (signed) and 3.5 s
   (unsigned), close to the 2.1 s and 4.25 s that 60 fps has been played with.

The third is the robust one: the simulation stays within the margins that 60 fps has been
tested at, the frame rate is the full 144, and nothing new runs per tick. Its cost is that
moving objects advance on every other frame at 144 (72 Hz motion, even cadence), not 144. An
uneven 60 ticks at 144 fps (2 or 3 frames per tick) was not chosen for that reason.

### Fixes for rates above 60 (any tick rate)

- **Cinematic clip planes** (`sub_8246E5A8`, `cinematic_clips_initialize_for_shot`): a shot's
  clip planes are on from a start to an end frame, 30 Hz shot frames that the game added to
  the game time in ticks, so above 30 ticks a second a plane went on and off too early (half
  way at 60). The copies are moved to round(frame × tick rate / 30). Logged as `frame_rate:
  cinematic clip plane ...`; none of Winter Contingency's two opening cinematics has one.
- **Byte and 16-bit tick fields** (`src/hooks/tick_storage.cpp`): above 30 ticks a second,
  `game_seconds_to_ticks` results stored unclamped in a byte (20 call sites) or in 16 bits (149)
  saturate at 255 / 32767 instead of wrapping (logged once per site as `tick_storage: ...`).
  At 72 ticks only durations over 3.5 s reach that.
- `REACH_TICKAUDIT=1` logs each call site whose result passes 127, 255 and 32767 ticks
  (`TICKAUDIT: call ...`). In the campaign opening, the flight and a few minutes of play at 144
  ticks no byte site passed 127; 16-bit sites got up to 3258 ticks.
- The PERF `ontarget` uses 1000 / `frame_rate`.
