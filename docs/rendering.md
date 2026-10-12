# Rendering issues in cinematics

Two reports from watching campaign runs (2026-10-11): the skin of Carter's and Kat's faces
rendered black in Winter Contingency's opening (eyes, teeth and hair were fine), and Falcons
seemed to jump back and forth and objects to vanish in cutscenes.

## Faceless Carter and Kat (fixed by SDK patches 0034 and 0035)

**Cause.** The faces of the main characters in cinematics are tessellated: Reach draws them
as *indexed triangle strips with discrete tessellation* (`VGT_OUTPUT_PATH_CNTL` = tessellation,
`VGT_HOS_CNTL.tess_mode` = 0, levels 1 to 1, so a factor of 2). The SDK's primitive
processor (like Xenia's) only tessellated triangle lists. It rejected these draws with
`Primitive processor: Unsupported tessellation mode 0 for primitive type 6`, about 2,000 a
minute in the briefing. Without the face draw, the albedo and normal buffers show the wall
behind the head, and the dark room behind it shows through the material pass. Eyes, teeth
and hair are separate meshes that are not tessellated. Jorge's face is an ordinary strip, so
it rendered.

Xenia Canary 3eab2b8b39 converts tessellated strips and fans to lists, but only auto-indexed
ones. Indexed ones still fail there (`assert_unhandled_case`), so Xenia has the same black
faces.

**Fix, part 1: patch 0035.** It converts tessellated triangle strips and fans to triangle
lists, both auto-indexed and indexed (16- and 32-bit, primitive reset ranges, every other
strip triangle swapped to keep the winding). This is the old `experimental/0014`. The cvar
`--tessellated_strips_and_fans_as_lists=false` brings back the old drop. The first such draw
logs its tessellation levels once (`tessellated triangle strip drawn as a list ...`).

**Why 0014 had hung the GPU: patch 0034.** The SDK generates its own GLSL for the
tessellation stages (the indexed and adaptive vertex shaders and the hull shader in
`src/graphics/vulkan/pipeline_cache.cpp`). Their system constant block was declared at
`set = 0, binding = 0`. In the SDK's pipeline layout, that binding is the shared memory
storage buffer; the system constants are at set 1 (`kDescriptorSetConstants`), binding 0.
The tessellation factor range and the vertex index endian, base and bounds were therefore
read from whatever guest physical memory sits at those offsets. 0034 binds the block to set
1, binding 0, like every translated shader. With 0034 and 0035, the face draws run on RADV
without a hang (campaign runs through the briefing and the Falcon takeoff, see below).

This is also the most likely cause of the old water hang (adaptive tessellation, patch 0018).
The adaptive hull and vertex shaders clamp their edge factors with that same garbage range.
That hang is not retested yet: the water is only issued in a few frames since 0022, and each
hang test costs a GPU reset. Next step: run Forge on Sword Base with
`--skip_adaptive_triangle_tessellation=false` on a quiet machine. If it holds, turn 0018's
default off.

**Before / after** (frame dumps of the same shot, 1152×720, no RenderDoc):
`scratchpad/faces/before_after_kat.png` and `before_after_carter.png` (session scratchpad,
not in the repo).

**How it was found.**
1. A RenderDoc capture of Kat's close-up (`REACH_RDCAPTURE_TRIGGER`, timed from Jorge's
   shot), then `tools/renderdoc/phist.py` at a cheek pixel. The HDR target got (0, 0, 0)
   from the material pass of a *background* mesh; the face itself had no fragment, failing
   or passing, in any pass.
2. The material pass's inputs (`savetex.py`): the albedo and normal buffers already lacked
   the face skin, while the eyes and teeth were there. So the draw was missing, not shaded
   black.
3. Grepping the run's log for draw failures found the unsupported-tessellation errors. A run
   with 0035 at `--tessellated_strips_and_fans_as_lists=false` logged levels 1 to 1. Reading
   the GLSL generators before the first live test turned up the wrong descriptor set.

**Testing GPU plugins side by side.** The SDK loads `librexgpu-xenos.so` from the
*executable's folder* (`gpu_plugin_loader.cpp`), never from `LD_LIBRARY_PATH`. `REXSDK=...`
(as in `tools/live_session.sh`) only switches `librexruntime.so`. To compare plugins, copy
`reach`, the four `reach_*` module libraries (without them the game shows the dirty-disc
error) and the plugin into a folder, then start `./reach` there. The 2026-10-11 triage that
compared "with and without 0029-0032" by `REXSDK` alone ran the same plugin twice. Its
conclusion that the black faces were older than those patches still holds.

Lavapipe (`VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/lvp_icd.x86_64.json`) runs the menus and
the intro video, but takes more than 10 s per frame in the 3D cinematic. It is too slow to
check a shot, but safe for anything that could hang the GPU.

## Cutscene "Falcons jumping back and forth" (fixed 2026-10-12 in `src/hooks/frame_rate.cpp`)

**Symptom (60 fps only).** In the formation flight of Winter Contingency's opening cinematic,
the Spartan nearest the camera disappears on alternate frames, in runs of 1-3 frames, and
the cabin around it jumps back and forth. The background and the other Falcon move smoothly.
In the bad frames the camera sits behind the cabin's door frame, which then fills the left of
the picture. At 60 fps it happens in every shot of the flight with a Spartan close to the
camera: the cabin shot, the helmet close-ups and the gunner. It is not a second camera angle:
both images look the same way, from points about 0.4 m apart.

**Measurements** (frame dumps from guest memory, bursts of 150 consecutive frames from the
start of the flight, `abpat.py` labels each frame by the image it matches):

| Run | Bursts | Result |
| --- | --- | --- |
| 30 fps (`fl30b`) | 6 (30 s of the flight, including the cabin shot and the close-ups) | clean: the only changes are the shot cuts |
| 60 fps, occlusion queries off (`fl60noq`) | 5 | alternation in 3 bursts (125 flagged frames) |
| 60 fps (`fl60cr`) | 5 | alternation in 4 bursts |
| 60 fps with the fix (`fl60fix`) | 5 (12 s) | clean: 0-2 flags a burst, all at shot cuts |
| 60 fps with the fix (`fl60fix2`) | 3, then 90 shots into gameplay | clean; the flight ends with its revert, then gameplay in the Falcon with the HUD |
| 30 fps, old and new binary side by side (`fl30old`, `fl30new`) | 4 each | the same: no alternation, 12 and 10 flags (the dump artifact below) |

**A frame dump artifact to ignore.** Some dumps are exact copies of the dump two swaps
before (an X Y X Y pattern with a 0.00 difference), with consecutive guest frame numbers.
This happens at 30 fps with both binaries (10-12 per 600 frames), and 75 times with
`REACH_CINEREC` logging. Over the same frames the game state advanced one tick per frame,
so these are stale copies of the front buffer in guest memory, probably the resolve
readback. They are not the bug, and `abpat.py`'s threshold ignores them.

**Ruled out.** Occlusion queries: with `--occlusion_query_enable=false` (every query
"visible") the alternation is the same. Reach does use them, but for BSP instances
(`sub_82163668` reads them, `sub_821674E0` issues a box query) and screen effects
(`sub_821AD720`, by type mask), not for objects. Cinematic clip planes (`sub_820D4A98`
logged 0 planes all through the flight). Ticks per frame: one tick in 1198 of 1200 frames.

**Cause.** The camera of a cinematic scene shot is a list of frames at 30 a second: position,
forward and up, 36 bytes each, in the shot (`0xD0` bytes, scene tag +0x30) at +0xBC, with the
count at +0xB8. The scripted camera's tick (`sub_820C2938`, mode 5; scripted camera globals at
thread globals +0x44) counts the ticks since the scene started (+0x60). It takes the frame
`(int)(ticks * seconds_per_tick * 30)` (+0x4C, and +0x48 within the shot). The director's
camera update (`sub_8249ED18`, through the vtable thunk `sub_824A0260`) then builds the camera
from that frame (`sub_8249FEB0`). At 60 ticks a second the frame changes on every other tick.
The cinematic's objects (the Falcon and its passengers) move on every tick. Recorded with
`REACH_CINEREC` (below): the observer camera (observer globals +0x10) moved 0.86, 0, 0.86,
0 m per tick, and the objects 0.43 m on every tick. The Falcon flies at about 26 m/s.

So on every other tick the camera lags the scene by 0.43 m. In these shots that is enough to
put it behind the cabin wall: the near Spartan is cut by the near plane and the door frame
comes into view. The camera's per-tick velocity (observer +0x3D0, scripted camera +0xAC)
alternated between twice its value and 0.

**Fix.** A hook on `sub_8249FEB0` works at tick rates above 30, when the scripted camera
plays a scene shot (mode 5). It replaces the frame passed in with a blend of that frame and
the next. The blend fraction is the part the game's truncation dropped:
`ticks * seconds_per_tick * 30 - frame`. Forward and up are blended, normalized and made
orthogonal again. There is no blend at a shot's last frame, nor across a jump of more than
4 m or about 25 degrees (a cut inside a shot). At 30 ticks a second the fraction is 0, so
nothing changes. `REACH_CINEMATIC_CAMERA_STEPS=1` brings back the old steps for comparisons.

The hook does a few dozen float operations per frame, in cinematics only. Contended
`RECOMP_PERF` windows of the opening cinematic read p50 16.8 ms before and after; no clean
bench was run, because neither the GPU plugin nor the runtime changed.

With the fix, the camera and the objects both move 0.43 m on every tick. The objects'
positions relative to the camera change by 0.005 m a tick or less (`fl60fix`). The pictures
match the 30 fps run shot for shot, with the near Spartans in place.

**Evidence** (session scratchpad, not in the repo):
- `scratchpad/fl/before_after_flight.png`: 8 consecutive frames of a helmet close-up, before
  and after.
- Run folders `/var/home/dyotson/faces_r/fl*`: frame thumbnails in `bpng/`, `bursts.txt`,
  `cine.rec`, `reach.log`.

**Left over.**
- The scripted camera's velocity (+0xAC..+0xB4) is the position change times 30. At 60 it
  is half the real speed. `sub_8249ED18` uses it when it blends out of a cinematic camera
  (+0x68 == 2). Not seen to matter yet.
- Cinematic clip planes (`cinematic_clips_initialize_for_shot`, `sub_8246E5A8`) add the
  shot's start and end values to the game time in ticks. If those values are 30 Hz frames,
  a clip plane is active for only half its time at 60. This is unverified: the flight has no
  clip planes. The render side is `sub_820D4A98` (plane on while start <= time < end) and
  `sub_821732D0` (each listed object's plane mask).
- The other camera modes interpolate already: animated cameras (`camera_set_animation`, mode
  3, `sub_8249FBD8`) sample their animation at a fractional time.

**Tools.**
- `REACH_CINEREC=FILE` (`src/debug/sync_diff.cpp`): touch `FILE.trigger` to record the next
  `REACH_CINEREC_FRAMES` main-thread frames. Each record has the game time, the ticks, the
  guest frame count, thread-global blocks (cinematic, observer, director, scripted camera,
  game time) and the cinematic's objects (handle and bounding-sphere centre). While it
  records, it also logs each view's clip planes (`CINEREC clip`).
- `tools/cinerec.py FILE flips|steps|word|summary` reads the file. `flips` finds the words
  that go A B A; `steps` prints how far the camera and the objects moved per frame.
- Frame dumps now carry the guest frame count (`"swap"` in the JSON), to line them up with
  the records.
- Scratch scripts (session scratchpad `fl/`, may be gone):
  - `motion3.sh`/`motion4.sh TAG FR`: the campaign up to the flight, then back-to-back
    bursts and a CINEREC recording; `BIN=` selects the run folder.
  - `motion5.sh`: the same, then into gameplay (`AFTER=` shots).
  - `ana.py DIR`: converts the bursts with the C untiler `untile` and deletes the `.bin`
    files.
  - `abpat.py DIR`: labels each frame by the image it matches.
- **START at Jorge's shot** (60 fps) opens the pause menu ("Game Paused"). It is not a skip.
