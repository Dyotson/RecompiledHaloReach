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
