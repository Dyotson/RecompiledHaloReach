# Frame rate above 30 fps: investigation

Status: the 45 fps floor is fixed (a 10 ms sleep in the SDK's music player app, below), and
`frame_rate = 60` in `reach.toml` (or `--frame_rate=60`) runs the game at 60 fps. The game
ticks at half the display's refresh rate, so at 60 fps it also simulates 60 ticks a second
(below); the campaign's cinematics needed a fix for that. Addresses are in `default.xex`
(base version); `docs/perf.md` explains the measurements.

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
8.3 ms instead of 16.7 ms. Network co-op (campaign, Firefight) needs every player on the same
frame rate.

Beyond 60: the SDK clamps the reported refresh rate to 24-240 Hz (`xboxkrnl_video.cpp`), so
this scheme tops out at 120 fps, and its VSync thread checks for due vblanks every 1 ms, so
each vblank comes up to 1 ms late (p50 33.6 ms rather than 33.3 at 30 fps). 144 fps needs a
288 Hz vblank (an SDK change) or a frame pacing that is not tied to whole vblanks.

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

**Next:** find the per-frame update that breaks 60/30 network co-op (below), check how a
60-tick player looks to a 30-tick one in a multiplayer game, and measure on a clean machine. Then measure what still looks like 30 Hz (objects, the first-person
weapon) and decide whether (b) is worth it, starting with the objects closest to the camera.
For 144 fps, see the limits under "The frame_rate setting".

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
start) is not enough: both machines then ran 30 ticks with their game times in step (232/233,
532/534, ...) for about 40 s, and the same error came. So something updated per frame feeds the
synchronous simulation, which only stays identical when every machine runs one tick per frame
(30/30 at 30 ticks, 60/60 at 60). That override was dropped (it would also put 60/60 co-op at
30 ticks, two frames per tick, untested). Network co-op needs the same `frame_rate` on every
machine; the README says so. Not traced: which per-frame update it is (`sub_82208CC8`, the
per-frame calls at the end of `sub_82440410` and player input sampling are the candidates).
