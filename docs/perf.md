# Frame rate and stutter

How to measure the game's frame pacing, what the numbers mean, and what we measured.
Reach runs at 30 fps on the Xbox 360 (its menus at 30, the pre-title screens at 60).

## Measuring

`RECOMP_PERF=1` turns on the frame statistics (`reach-recomp/src/debug/frame_stats.cpp`; the
older `REACH_FPSLOG=1` does the same). They are off by default and cost nothing then; when on,
they keep the frame times in memory and log two lines every 10 seconds and two at exit:

```
PERF: scene=menu-warm src=guest t=50.9 fps=30.0 p50=33.6 p95=33.7 p99=33.7 max=34.0 low1=29.5 stutters=0 hitches=0 ontarget=100.0 pipelines=0
PERF: scene=menu-warm src=present t=60.5 fps=144.0 p50=6.9 p95=7.5 p99=7.9 max=10.8 low1=116.8 stutters=0 hitches=0 ontarget=0.0 pipelines=0 final=1 from=28.8
```

- `RECOMP_PERF_SCENE=<name>` labels the lines.
- `RECOMP_PERF_CSV=<path>` writes every frame time (`scene,src,t,ms`).
- `RECOMP_PERF_RESET=<path>`: when that file appears (it is deleted again), the summary
  restarts, so it covers only what follows, e.g. from the moment a scene is reached. The
  summary line ends in `final=1 from=<s>`.

`tools/bench.sh menu|gameplay [--cold]` does all of this with a scripted run (input FIFO,
`tools/live_session.sh`): START skips the intro at 9 s, then it goes to the main menu
(`menu`), or into the Custom Game lobby and START GAME with the profile's last game type and
map, SWAT on Sword Base here (`gameplay`, turning in place with the right stick). Once the
scene is on screen it restarts the summary, measures 30 s (`--seconds`), quits the game
and prints the two summary lines, the game's CPU use, the GPU's load and the swapchain's
present mode. Each step leaves a screenshot in the run's directory. `--cold` starts with an
empty shader storage (a temporary `cache_root` sharing the game's `cache0`/`cache1`) and
Mesa's shader cache off, so every pipeline is compiled during the run; the default (warm)
uses the caches as they are. Other arguments go to the game, e.g.
`--vulkan_allow_present_mode_immediate=true`. Runs are windowed unless `--fullscreen`.

Other programs skew the numbers, other games most of all. `bench.sh` records other `reach`
and `forza` processes, the load average, available memory and the GPU's load (AMD) before
the launch and at the start and end of the measurement, and checks for other games every
second. A run with another game running, a 1-minute load average above 2 before the launch
or the GPU more than 20% busy prints `CONTENDED` and is not a baseline: such runs do not go
in the table below. On a shared machine, `BENCH_LOCK=<file> BENCH_OWNER=<name>` makes it
refuse to start while someone else holds the lock in that file (`owner expiry-epoch
purpose`).

## Metrics

Two streams are timed:

- **guest**: the game handing a frame to `VdSwap`. This is the game's own frame rate.
- **present**: the host presenter calling `vkQueuePresentKHR`, hooked in the SDK's Vulkan
  device function table (`reach-recomp/src/platform/vulkan_hooks.cpp`). How often the window
  is updated; with FIFO in a window, about how often the compositor takes a new image.

Per 10 s window, and over the whole measurement in the summary:

| Field | Meaning |
| --- | --- |
| `fps` | frames / total frame time |
| `p50`, `p95`, `p99`, `max` | frame time percentiles and maximum, in ms (nearest rank) |
| `low1` | 1000 / mean of the slowest 1% of frame times ("1% low", fps) |
| `stutters` | frames longer than 2 × that window's p50 |
| `hitches` | frames longer than 100 ms |
| `ontarget` | % of frames within ±20% of 33.3 ms (the 30 fps target); meaningless for `present` |
| `pipelines` | graphics and compute pipelines the GPU plugin created in the window (guest shaders, render-target helpers): stutter from shader compilation shows here |

The pipeline count leaves out the SDK presenter's own pipeline, which it rebuilds on every
present (see "Repaint pacing" below).

## Present modes

Every swapchain creation logs its present mode and the modes the surface offers:

```
Vulkan swapchain 1920x1080: present mode FIFO (the surface offers MAILBOX FIFO IMMEDIATE; ...)
```

On KDE Plasma 6.7 (Wayland) with Mesa 26.2 RADV, a Wayland surface offers MAILBOX, FIFO and
IMMEDIATE (no FIFO_RELAXED). The SDK takes the first allowed of IMMEDIATE, MAILBOX,
FIFO_RELAXED, FIFO. The game now allows only FIFO by default (`src/main.cpp`), because:

- **IMMEDIATE tears.** KWin lets fullscreen windows that ask for it tear. Reach's menus pan
  slowly, and a 30 fps image on a 144 Hz display puts the tear line at a different height
  every frame: a band of light that seems to crawl down the screen. The guest frames have no
  such band (frame dumps of the title and main menus three times a second: row brightness
  changes by at most 4/255, with no moving peak; only the clouds and the selection pulse
  move), so it is the presentation.
- **FIFO_RELAXED tears too** at 30 fps: every image arrives after the vblank it was due for.
- **MAILBOX does not tear**, and since the repaint fix below it is a fine choice too: no
  vsync wait, so up to one refresh less latency than FIFO (7 ms at 144 Hz, 17 ms at 60 Hz).
  It was not when the default was chosen: the window was then repainted thousands of times
  a second (see below). FIFO stays the default because a vsync wait can never let the
  presenter run away again, and it waits on the UI thread, never on the GPU emulation.

To change it, in `reach.toml`:

```toml
vulkan_allow_present_mode_mailbox = true     # no tearing, lowest latency
vulkan_allow_present_mode_immediate = true   # the SDK's default: tears
```

The command line works too (`tools/play.sh --vulkan_allow_present_mode_immediate=true`).

### Repaint pacing

The SDK's ImGui drawer asks for another repaint after every frame while any dialog is
registered, and the SDK registers its achievement notification dialog for the whole run.
On Windows the SDK paces those repaints with DXGI vblank waits; on Linux nothing did but a
FIFO swapchain. With MAILBOX or IMMEDIATE the window was repainted about 4000 times a second
on the main menu and 3300 in a game (idle machine), with FIFO 144 (the refresh rate), each
time also rebuilding the presenter's output pipeline and waiting for the previous repaint on
the GPU: the presenter never stores the swapchain format it built that pipeline for
(`swapchain_effect_pipeline.swapchain_format` in `vulkan_presenter.cpp`; Xenia has the same
gap), so every paint sees a mismatch.

`reach-recomp/src/platform/achievement_toast.cpp` now registers the toast dialog only while a
toast is on screen (4.5 s per achievement). The rest of the time no dialog is registered, the
drawer detaches from the presenter, and the window is repainted once per guest frame in every
present mode: 30 presents a second on the menus (60 during the intro), 144 while a toast or
an overlay (F3, F4) is open. The pipeline rebuild is still there, now 30 times a second; it
is in `librexruntime.so`, which this project does not rebuild on Linux.

## Results

Machine: Ryzen 7 7800X3D, Radeon RX 9060 XT (Mesa 26.2.1 RADV), KDE Plasma 6.7.4 on Wayland,
1920x1080 at 144 Hz. `tools/bench.sh`, 30 s measured, windowed (1280x720), no other game
running and the load average under 2 at launch (a reserved, idle machine). IMMEDIATE was the
default before commit `c7cccb2`; FIFO is the default since. Times in ms; `present fps` is
vkQueuePresentKHR calls per second; CPU is the game's, in % of one core; GPU is the whole
GPU's busy percentage.

| Date | Commit | Scenario | Cache | Present mode | fps | p50 | p95 | p99 | max | 1% low | stutters | hitches | on target % | pipelines | present fps | CPU % | GPU % |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| 2026-10-10 | c7cccb2 | menu | warm | IMMEDIATE | 29.9 | 33.6 | 33.8 | 34.1 | 67.3 | 24.0 | 1 | 0 | 99.8 | 1 | 3937 | 240 | 44 |
| 2026-10-10 | c7cccb2 | menu | warm | MAILBOX | 29.9 | 33.6 | 33.7 | 33.8 | 66.4 | 24.3 | 0 | 0 | 99.8 | 1 | 4072 | 241 | 44 |
| 2026-10-10 | c7cccb2 | menu | warm | FIFO | 30.0 | 33.6 | 33.7 | 33.9 | 34.2 | 29.4 | 0 | 0 | 100.0 | 0 | 144 | 178 | 21 |
| 2026-10-10 | c7cccb2 | menu | cold | FIFO | 30.0 | 33.6 | 33.7 | 33.8 | 34.0 | 29.6 | 0 | 0 | 100.0 | 0 | 144 | 184 | 21 |
| 2026-10-10 | c7cccb2 | gameplay (SWAT, Sword Base) | warm | IMMEDIATE | 30.0 | 33.6 | 33.7 | 33.8 | 34.2 | 29.5 | 0 | 0 | 100.0 | 6 | 3348 | 206 | 55 |
| 2026-10-10 | c7cccb2 | gameplay (SWAT, Sword Base) | warm | FIFO | 30.0 | 33.6 | 33.7 | 33.8 | 33.8 | 29.6 | 0 | 0 | 100.0 | 2 | 144 | 152 | 32 |
| 2026-10-10 | cb092d4 | menu | warm | FIFO | 30.0 | 33.6 | 33.7 | 33.8 | 66.3 | 26.7 | 0 | 0 | 99.9 | 1 | 30 | 175 | 23 |
| 2026-10-10 | cb092d4 | menu | warm | IMMEDIATE | 30.0 | 33.6 | 33.7 | 33.8 | 33.8 | 29.6 | 0 | 0 | 100.0 | 0 | 30 | 175 | 23 |
| 2026-10-10 | cb092d4 | gameplay (SWAT, Sword Base) | warm | FIFO | 30.0 | 33.6 | 33.7 | 33.8 | 34.1 | 29.5 | 0 | 0 | 100.0 | 2 | 30 | 149 | 38 |
| 2026-10-10 | cb092d4 | gameplay (SWAT, Sword Base) | cold | FIFO | 29.9 | 33.6 | 33.7 | 34.0 | 62.7 | 24.1 | 0 | 0 | 99.6 | 31 | 30 | 152 | 37 |
| 2026-10-10 | build before 0025 | menu | warm | FIFO | 30.0 | 33.7 | 33.7 | 33.8 | 34.3 | 29.5 | 0 | 0 | 100.0 | 0 | 30.0 | 177 | 23 |
| 2026-10-10 | b0954ba (patch 0025) | menu | warm | FIFO | 30.0 | 33.6 | 33.7 | 33.8 | 33.9 | 29.6 | 0 | 0 | 100.0 | 0 | 30.0 | 175 | 23 |
| 2026-10-10 | build before 0025 | gameplay (SWAT, Sword Base) | warm | FIFO | 30.0 | 33.7 | 33.7 | 33.8 | 66.1 | 26.7 | 0 | 0 | 99.9 | 0 | 30.0 | 153 | 40 |
| 2026-10-10 | b0954ba (patch 0025) | gameplay (SWAT, Sword Base) | warm | FIFO | 30.0 | 33.6 | 33.7 | 33.8 | 67.4 | 26.6 | 1 | 0 | 99.9 | 2 | 29.9 | 153 | 45 |

- Patch 0025 (dot products give QNaN on overflow, one predicted branch per `vmsum3fp128`/
  `vmsum4fp128`) against the build just before it, in one reserved window: no measurable cost
  (same frame times, same CPU use).

- The game itself keeps 30.0 fps in every mode on an idle machine. Vsync costs it nothing;
  dropping the nonstop repaints frees about a quarter of the GPU and 0.5-0.6 of a CPU core.
- FIFO presents at exactly the display's 144 Hz.
- Cold vs warm: the warm run restores 1797 pipelines from the shader storage before the
  first frame. The cold run compiles 147 pipelines in the first 10 s of frames (the intro
  and title screen; the worst frame took 642 ms) and 13 more in the next 10 s (67 ms); by the
  main menu nothing is left to compile.
- `cb092d4` (the repaint fix): one present per guest frame in every mode. IMMEDIATE on the
  main menu went from 3937 presents a second and 44% GPU to 30 and 23%, the same as FIFO.
  The cold SWAT run compiled 31 pipelines in the window, with one 233 ms present hitch.
- At a 120 Hz guest vblank (the limiter's 60 fps target) the menu ran 45.6 fps and the game
  45.1 fps, both at a flat ~22 ms per frame: see `docs/framerate.md`.
- Runs made earlier on a busy machine (two more Reach instances, each repainting nonstop) are
  not in the table: there the windowed FIFO and MAILBOX runs got only 12-20 presents a second
  in gameplay, because the compositor itself was starved of GPU time.

## GPU emulation speed (SDK patches 0028-0032)

Measured in Forza Horizon (RecompiledForzaHorizon), which shares this SDK and Reach's GPU
patches: at `frame_rate = 60` its races ran 41-42 fps, bound by the emulated GPU (the guest
render thread spent ~70% of its time in D3D's `BlockOnFence`, waiting for the command
processor to reach a fence). The fixes are general: Reach runs the same command processor
and the same memory watches.

**Profiling without `perf`.** samply refuses to run at `perf_event_paranoid = 2`, but
user-space sampling of our own processes through `perf_event_open` works there
(`exclude_kernel`, frame-pointer call chains from the kernel). A small sampler did that
(scratchpad `gpu-perf/fpprof.c`, with a plugin built with `-fno-omit-frame-pointer
-gline-tables-only`), plus temporary counters in the plugin and a wall-clock sampler of the
command processor thread (`/proc/PID/task/TID/{stat,syscall}` and a stack scan through
`/proc/PID/mem`) for where it waited.

**What the command processor did in a race** (before the patches; ~190,000 draws and 40 M
register writes a second, 4,600 draws a frame):

- On the CPU 70% of the time. Of that, 30% went to register writes: type-0 packets wrote one
  register at a time through a virtual call, and each write ran `RegisterFile::GetRegisterInfo`
  (a switch over every register) only to log unknown registers. Then `IssueDraw` (shared-memory
  requests for vertex/index buffers 9%, descriptor and constant updates 7.5%, texture bindings
  5%, render-target state 4%) and replaying the deferred command buffer into Vulkan at each
  submission (11-15%, mostly in RADV).
- Off the CPU about a third of the time: half of it waiting for the global critical region in
  `SharedMemory::RequestRanges` and `MakeRangeValid`, a third in `WAIT_REG_MEM` (the frame
  flip: a value the guest's vblank interrupt clears), the rest preempted.
- The lock was held by guest threads in the write-watch fault handler (the CPU writing
  vertex data the GPU had uploaded: ~45,000 uploads, 260,000 pages a second). For every
  fault, `MMIOHandler` rechecks the page's protection under that lock with
  `memory::QueryProtect`, which on Linux read and parsed all of `/proc/self/maps` (~1,800
  lines once guest memory is watched page by page).

**The patches** (`docs/sdk_patches.md` has the table):

| Patch | Change |
| --- | --- |
| 0028 | `QueryProtect`/`Protect` look the mapping up with the `PROCMAP_QUERY` ioctl (Linux 6.11+, ~0.5 µs) instead of parsing `/proc/self/maps`; older kernels keep the parse. In `librexruntime.so` |
| 0029 | Type-0 packets go to `WriteRegistersFromMem` as a range (the constant blocks are copied in one go); known registers come from a bitmap built once; the Vulkan backend splits mixed ranges instead of a virtual call per register |
| 0030 | `WAIT_REG_MEM` sleeps on an event set after each guest vblank interrupt instead of a fixed 1 ms, so the flip wait ends as soon as the interrupt handler has run |
| 0031 | `RequestRanges` reuses its range vector and returns without the global lock when every page is valid already (85% of the requests) |
| 0032 | Fetch constants rewritten with the same value (73% of the fetch dwords written) no longer invalidate the texture bindings, the vertex buffer residency and the fetch constant buffer |

**Results.** Forza race (GoPro Millennia Masters, throttle held, ~28 s measured), `frame_rate =
60`, warm caches, windowed, a reserved machine: no other game running and the load average
under 2 at every start. Same game binary throughout; "runtime" is the nightly
`librexruntime.so` or one built with 0028. Times in ms.

| Plugin | Runtime | fps | p50 | p95 | p99 | max | 1% low | on target % |
| --- | --- | --- | --- | --- | --- | --- | --- | --- |
| before | nightly | 41.4 / 42.4 | 24.8 / 24.7 | 29.4 / 30.5 | 32.8 / 33.6 | 50.1 / 53.7 | 26.6 / 26.8 | 15.8 / 23.8 |
| before | 0028 | 49.3 | 17.8 | 25.9 | 26.8 | 32.4 | 35.8 | 56.7 |
| 0029-0032 | nightly | 50.3 | 18.4 | 26.2 | 27.8 | 42.1 | 32.7 | 60.2 |
| 0029-0032 | 0028 | 59.7 / 59.7 | 16.8 / 16.8 | 17.9 / 17.9 | 20.9 / 20.7 | 33.6 / 35.7 | 42.6 / 43.2 | 98.6 / 98.6 |

(two values: two runs.) Frame times snap to the 120 Hz guest vblank (8.3 ms steps): a frame
the command processor cannot finish in two vblanks takes three, so the race sat at 25 ms
frames and each fix alone only moved part of the frames back to 16.7 ms. With all of them
the command processor has slack: off the CPU 14% of the time, nearly all of it in the flip
wait, and waiting for the global lock 0.2% (was 17%).
Screenshots of the race menu and of the race match the unpatched build (differences are the
menu camera's sway and the race timing). In Reach (`tools/bench.sh menu`, Reach's patch set
with and without 0028-0032, its own runtime with and without 0028) the main menu renders the
same (mean pixel difference 1/255, only the moving clouds) and holds its 30 fps cap.

Not done (bigger changes): the 45,000 vertex uploads a second each end the render pass for a
copy and a barrier (about 40,000 render pass instances and 100,000 barriers a second), which
is where most of the driver time goes; replaying the deferred command buffer on another
thread would take that off the command processor. The SDK's `Audio Worker` thread spins at
~75% of a core in `PosixConditionBase::WaitMultiple` (runtime): fixed by 0033 (below).

## Patch 0033: `WaitMultiple` sleeps until a handle is signalled (2026-10-11)

`PosixConditionBase::WaitMultiple` polled every handle each millisecond; now state changes
bump a global epoch and multi-handle waits sleep on one condition variable (20 ms cap).
Validated under gameplay. "Before" is the same runtime without 0033 (for Forza a rebuild
with 0033 reversed), same game binary, `frame_rate = 60`, Reach headless and Forza windowed,
on a shared machine (another game ran throughout, so fps differs run to run; the pairs ran
back to back). Per-thread CPU from `/proc/<pid>/task/*/stat`.

| Scenario | Audio Worker CPU before / after | Frame pacing before / after |
| --- | --- | --- |
| Reach campaign (cinematic, then the Falcon) | 75-77% / 1.6-2.2% | opening cinematic 60.0 fps both, p99 17-18 ms both; Falcon seat 41-50 fps both (render-thread bound) |
| Reach Firefight, 130 s of play (3 pairs) | 71-75% / 2.5-2.7% | 47.9, 53.4, 44.1 / 42.5, 48.3, 54.0 fps (the spread between runs is larger than the difference) |
| Reach System Link Firefight, 2 instances, ~5 min | not sampled / 1.8% on both | both play in step (same HUD clock), no hang |
| Forza free roam | 77% / 2.4% | 60.0 / 59.8 fps, p99 20.2 / 20.1 ms |
| Forza race (GoPro Millennia Masters) | 81% / 3.1% | 58.3 / 59.8 fps, p99 24.3 / 19.4 ms |

Load: the Reach level-load stall (the 460 ms frame) came 76-92 s after launch before and
78-83 s after (a few runs on both sides were later than the 107 s the run watched, the
machine was busy): no difference outside the noise. No hangs, no audio errors, no new log
errors. The faceless Carter and Kat in the campaign briefing show in all these runs, with
and without 0029-0032 and 0033, so those patches don't cause it (triage 2026-10-11).

Upstream: rexglue/rexglue-sdk issue #500 and PR #501 (branch `perf/wait-multiple-epoch`).
