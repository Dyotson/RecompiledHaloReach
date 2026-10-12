# Halo Reach PC — native static recompilation of the Xbox 360 build

Goal: run Halo: Reach (Xbox 360, Title ID `4D53085B`, base version 1.0 / no TU,
Media ID `566C10D3`) natively on x86-64 Linux (and later Windows) by statically
recompiling `default.xex` (PowerPC Xenon) to C++ with the ReXGlue SDK, then add:

- the original Credits (cR) / rank / Armory progression (no microtransactions),
- peer-to-peer online play (replacing Xbox Live / system link),
- Forge plus file share (upload/download maps, gametypes, mods).

## Layout (all paths relative to `/home/dyotson/Documents/Halo Reach PC`)

| Path | What |
| --- | --- |
| `Halo - Reach (...).iso` | Original disc image. READ ONLY. Never modify or delete. |
| `extracted/xbox360/` | Game partition extracted from the ISO (`default.xex`, `maps/*.map`, ...). Treat as read only. |
| `tools/xdvdfs_extract.py` | Read-only XDVDFS extractor. |
| `tools/start_ghidra_headless_mcp.sh` | Starts the headless GhidraMCP server (already running in tmux session `ghidra-headless`). |
| `ghidra_projects/HaloReach` | Ghidra project: `/xbox360/default.xex` (PPC BE 64, 32-bit addr) and `/mcc/haloreach.dll` (MCC PC build of Reach, x86-64). Both auto-analyzed. |
| `reach-recomp/` | ReXGlue project. `reach_manifest.toml` drives codegen; `generated/` is codegen output (do not hand-edit); `src/` is our runtime code. |
| `docs/` | Design docs and findings. |

MCC install (READ ONLY): `/home/dyotson/.local/share/Steam/steamapps/common/Halo The Master Chief Collection/haloreach/`.

## Tooling

- ReXGlue SDK: build against the **0.10.0.24 nightly** at `~/rexglue-sdk-nightly/0.10.0.24/linux-amd64`
  (fixes `stwcx.` atomics, sync fences and VMX pack aliasing; regenerate with its `bin/rexglue`).
  Build dir `reach-recomp/out/build/linux-nightly`. v0.10.0 is still at `~/rexglue-sdk/linux-amd64`
  (do not build `linux-release` any more: its codegen step would overwrite `generated/` with 0.10.0 output). (`bin/rexglue`, CMake package under `lib/cmake/rexglue`). Docs: https://github.com/rexglue/rexglue-sdk/wiki
- **Patched GPU plugin (required):** the SDK's Vulkan shader translator scales every texture
  sample by 2^(bits of the LOD bias) (rexglue-sdk#456, closed upstream as "not accepting Xenos GPU
  edits"). `tools/build_rexglue_sdk.sh` builds `librexgpu-xenos.so` from the nightly's commit
  `bd833a2` with `patches/rexglue-sdk/*.patch` into the overlay prefix
  `~/rexglue-sdk-patched/0.10.0.24/linux-amd64` (source checkout `~/rexglue-sdk-src/sdk`); the reach
  build copies it next to the binary after each link (`REACH_XENOS_PLUGIN`). Building the SDK on this
  immutable host uses Homebrew's X11/XCB/Wayland headers (see the script). Upstream will not take GPU
  fixes, so further Xenos fixes go into `patches/rexglue-sdk/` too.
- **Patched codegen tool:** the same script builds `rexglue` (patch 0025: Xenon's dot products
  give QNaN on overflow, rexglue-sdk#491) into the overlay's `bin/`, and the reach build runs
  codegen with it (`REACH_REXGLUE`). When the tool changes, the script deletes the
  `generated/*/codegen.stamp` files, since codegen's fingerprint covers only its inputs, and the
  next build regenerates everything.
- Clang 23 / CMake / Ninja from Homebrew: `/home/linuxbrew/.linuxbrew/bin`.
- JDK 21: `/home/linuxbrew/.linuxbrew/opt/openjdk@21/libexec`.
- Build: `cmake -S reach-recomp -B reach-recomp/out/build/linux-release -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_COMPILER=/home/linuxbrew/.linuxbrew/bin/clang++ -DCMAKE_PREFIX_PATH=$HOME/rexglue-sdk/linux-amd64` then `ninja -C reach-recomp/out/build/linux-release`.

### Ghidra MCP HTTP API

The headless GhidraMCP server listens on `http://127.0.0.1:8089`. Every endpoint takes
`program=/xbox360/default.xex` or `program=/mcc/haloreach.dll`. Endpoint catalog with
params: `~/ghidra-mcp/tests/endpoints.json`. Useful ones:

- `GET /decompile_function?address=0x82....&program=...`
- `POST /disassemble_bytes` JSON `{start_address,end_address,include_instructions:true,program}`
- `GET /get_xrefs_to?address=...&program=...`
- `GET /search_strings?...`, `GET /list_imports`, `GET /list_exports`, `GET /search_functions_by_name`
- `POST /rename_function`, `POST /set_decompiler_comment` (annotate as you learn things)
- `GET /save_program?program=...` after edits

Check exact param names in `endpoints.json` before calling.

## Hard rules

- Never delete, overwrite, or modify the ISO, the zip, `extracted/`, or anything in the Steam/MCC install.
- Never run `rm`, `find -delete`, etc. on user files. Scratch files go in `/tmp`.
- Don't kill the tmux sessions `ghidra-headless`, `ghidra-gui`, `reach-build`.
- Commit your own work in git (`git -c user.name=agent -c user.email=agent@local commit`), docs in `docs/`.

## Status log

- 2026-10-08: ISO extracted. Both binaries imported + analyzed in Ghidra. ReXGlue codegen of
  `default.xex` succeeds (325 files, 167 MB C++) with one manual function hint; warnings in
  `reach-recomp/codegen-warnings.log`. First native build in progress.
- 2026-10-08 (later): `reach` + the four Waves audio DLLs build and boot (GPU plugin xenos,
  cache0/cache1 mounts, 375+ missed entry points pinned via `hints/`). Boot then died in an
  endless null write on the ASYNC_0 thread (`sub_82602C28`): the statically linked XAPI fiber
  code (`SwitchToFiber` = 0x8295DC48) swaps guest GPRs and "returns" into another fiber, which
  recompiled code cannot do. Fixed by mapping the five XAPI fiber routines to the SDK's native
  `rexcrt_*` fibers (`hints/rexcrt.toml`). Implemented `XeCryptBnQwNeRsaPubCrypt`
  (`src/kernel/xecrypt_rsa.cpp`; the SDK stubs it on Linux, the game verifies RSA-2048
  signatures with it). The game now plays the intro Bink video, then shows the (auto-answered)
  storage device selector and writes profile settings; after that the screen stays black.

- 2026-10-08 (evening): moved to the ReXGlue 0.10.0.24 nightly (atomic `stwcx.`, fences, VMX
  pack fix). Fixed a codegen bug where `bdz`/`bdzf` switch-chain tail calls were emitted as bare
  `return`s (`hints/switch_tailcalls.toml` + `src/hooks/switch_tailcalls.cpp`). Ghidra analysis
  repaired (no-return cascade, ~1.6k merged fragments); function names exported to
  `docs/symbols/default_xex_functions.csv`. Credits/rank/Armory mapped in `docs/progression_re.md`.
  Open blocker: the title menu renders black; see `docs/menu_black_screen.md`.

- 2026-10-09: **title menu fixed.** Root cause was an SDK bug, not game state: the SPIR-V
  translator read the texture-fetch `exp_adjust` from fetch-constant word 4 instead of word 3, so with
  Reach's LOD bias every texture sample came back 1/256 as bright (`docs/menu_black_screen.md`).
  Patched plugin built from source (`tools/build_rexglue_sdk.sh`). Title screen and start menu
  (Armory, player card with rank/cR) render. Also fixed: directories marked delete-on-close are now
  deleted (`cache1:\webcache`), and `REACH_NO_SIGNIN=1` mimics Xenia's no-profile setup.

- 2026-10-09 (later): **campaign starts.** START, A on START SOLO CAMPAIGN, A on Normal loads
  Winter Contingency: the loading cinematic, the in-engine opening cinematic (Warthog, Falcon, Noble Team
  briefing) and Noble Six at the base render. Open issue: terrain/foliage show saturated red/green
  patches that change every frame. Traced to the lighting buffer texture (`k_2_10_10_10_AS_16_16_16_16`
  at 0x02354000) that an EDRAM resolve fills; it contains garbage in our build. Two more SDK porting bugs
  found by diffing against Xenia and patched (stacked-texture layer lerp, missing layer clamp), but they
  are not the red-patch cause. Patch 0004: the SDK hosted `k_2_10_10_10` render targets (Reach's HDR
  lighting) as 8-bit `A8B8G8R8` instead of Xenia's `A2B10G10R10`; fixed, red patches remain.
- Xenia can now be driven in-game with `tools/virtual_pad.py` (virtual Xbox 360 pad over /dev/uinput),
  but its campaign needs a Xenia profile (Profile > Create Profile, once). Don't automate the mouse
  for that: ydotool's absolute moves land at (0,0) on this KDE session and hit other windows.
  `renderdoccmd capture` under `timeout` leaves the captured child running; kill it afterwards.

- 2026-10-09 (night): **checkpoint revert crash fixed.** ~4.5 min into the campaign the game reverted
  to a checkpoint and died with "Call to invalid or unregistered function at 0x82429440": a
  post-revert callback (table 0x82A39BF0) is a real function placed right after a call to the
  no-return fatal-error handler, so codegen merged it into its predecessor. `find_missing_entries.py`
  now also accepts data-referenced addresses after a `bl` (and skips `.reloc`);
  `hints/indirect_entries_2.toml` adds 18 such entries. `REACH_GSTRACE=1`
  (`src/debug/gamestate_trace.cpp`) traces game state save/load/verify. Nine more SDK GPU patches
  (0005-0013, from an audit against Xenia; see `docs/sdk_patches.md`). Open: red terrain patches,
  dropped tessellated strips, GPU hang ~5 min in. `REACH_AUTOPRESS` now drives sticks and triggers
  (`300:LSUP:4`, `RT`).

- 2026-10-09 (morning): **red terrain fixed** (SDK patch 0015). The red/green speckle on terrain,
  foliage and stars was a precision loss in the 7e3 -> 8_8_8_8_GAMMA -> 8_8_8_8 EDRAM round trip
  Reach uses for its forward-lit surfaces; one gamma byte came back one lower and the combine
  pass's hand-written 7e3 decode turned that into 2^6 red. Xenia decodes gamma bytes to the
  midpoint of their linear range; ported (`docs/sdk_patches.md` has the full trace). Found by
  capturing the same cinematic shot in both: Xenia uses a profile copied from the user's EmuDeck
  Xenia (`content/E03000003463994D` into `~/.local/share/Xenia/content` plus
  `logged_profile_slot_0_xuid` in its config), driven with `tools/virtual_pad.py -` (reads one
  input per line from stdin, so menus can be stepped while watching screenshots). New RenderDoc
  scripts: `resolves.py`, `phist.py`, `pick.py`, `blend.py`, `savetex.py`, `bufdump.py`.
  Open: faint cyan speckles on near ground in the Falcon landing, dropped indexed tessellated
  strips, GPU hang ~4-5 min in.
- Scripted runs need every SDL pad ignored: with a pad connected (including Steam Input's and
  Sunshine's virtual ones) the scripted presses never reach the campaign. `run_reach.sh` sets
  `SDL_*_IGNORE_DEVICES_EXCEPT` when `REACH_AUTOPRESS` is set; Xenia needs
  `SDL_GAMECONTROLLER_IGNORE_DEVICES=<vid/pid,...>` for the same reason, or the virtual pad
  becomes player 2 (unsigned profile).

- 2026-10-09 (midday): **gameplay reached** (with adaptive tessellation draws skipped by a local
  debug switch). The next crash, "Call to invalid or unregistered function at 0x82853FB0" ~6 min
  in, was a function only reachable through a callback table the game fills at run time
  (`sub_82874A20` indexes it by type), so no static reference exists. Found the caller with gdb
  (`break rex::runtime::InvalidFunctionTrap`, then `bt`). `tools/find_orphan_functions.py`
  lists code no generated function covers and nothing static reaches; `hints/indirect_entries_3.toml`
  registers 138 such functions (11 with an explicit `end`). Seven CRT entries in
  `indirect_entries_2.toml` that split functions are pruned again. Open: the GPU hang on the
  first adaptive-tessellation draw (patch 0016 is part of it), black 3D world in most gameplay
  frames (HUD fine), cyan tint on foliage.

- 2026-10-09 (afternoon): **gameplay renders.** The black 3D world was the SDK defaulting
  `execute_unclipped_draw_vs_on_cpu` to false: an unclipped screen-space draw claimed all of
  EDRAM, a depth buffer took over the scene's tiles and a transfer wiped the scene (SDK patch
  0019 restores Xenia's defaults; trace in `docs/sdk_patches.md`). Patch 0017 sets r0.w = 1 in
  domain shaders as Xenia does; 0018 drops adaptive triangle-patch draws by default
  (`--skip_adaptive_triangle_tessellation=false` to draw them) because the first one still hangs
  the GPU. A default `tools/run_reach.sh` run now plays the opening and reaches the "locate
  distress beacon" objective with the world lit like Xenia. Missing: those tessellated surfaces
  (probably water).

- 2026-10-09 (evening): **Forge works, and the Armory shows the Spartan.**
  - Forge: lobby, map list (Local Files), Sword Base, editing mode, object placement and budget,
    "Save As New Map" (a BLF map variant under `~/.local/share/reach/<xuid>/4D53085B/00000001/`;
    it appears in the lobby and the variant list), ending the game, carnage report, and cR for
    the session.
  - Fixes:
    - The SDK's headless keyboard (`--headless=true`) returned its default text byte-swapped,
      so saved maps were named in CJK glyphs; `src/kernel/xam_keyboard.cpp` overrides
      `XamShowKeyboardUI` in headless mode.
    - The Spartan was missing from the Armory preview and the post-game screens: a full-screen
      rectangle with W = 0 that sets the model's alpha was clipped on the host (SDK patch 0021;
      Xenia has the same gap; trace in `docs/sdk_patches.md`).
  - Open:
    - Once, entering the Forge lobby froze the game: a new guest thread faulted forever on a
      null virtual call inside the runtime's `XThread::Create` start lambda (gdb: the thread
      sat in `ExceptionHandlerCallback`, re-running `mov (%r14),%rax` with r14 = 0). It did not
      reproduce on the next run.
    - D-pad DOWN is sometimes ignored in menus during FIFO sessions (UP works; menus wrap).

- 2026-10-11: **Forge with two players through Reach Live (one run, Sword Base).** Server on
  21500/21501, two instances (ForgeHost, ForgeGuest). Host: START, B, A (Main Menu), Forge
  (3 x DOWN from Matchmaking, A). The roster (RIGHT, DOWN, DOWN on the other instance) lists
  "In Forge Lobby 1/16"; X joins; the lobby shows 2/18 and the guest waits for the leader.
  Host START GAME: both load Sword Base and spawn as Spartans (a spawn-in of about 30 s with
  the frame unchanged, then input works). Edit mode: START menu "Go to Edit Mode" on the host
  (A) or D-pad UP in game; both show the budget bar. The host placed a weapon (X, A, A, A):
  budget $1535 -> $1525 on both; the guest placed one: $1515 on both. D-pad UP switched both
  back to Spartan and edit again. START, DOWN, DOWN, A ("Save As New Map") on the host gave
  "Save succeeded" and `sandbox.map` under `B13EBABEBABEBABE/4D53085B/00000001/mgal...`.
  - No hang entering the lobby, no desync seen, both stayed responsive for 9 minutes.
  - Not tried: moving or deleting an object, seeing the other monitor (the two spawned in
    different rooms; the placed objects and budget were checked instead), the System Link
    browser route, a Live party invite (the roster join is the Live path).
  - Host log: 3,000+ `[error] [gpu] Primitive processor: Unsupported tessellation mode 0 for
    primitive type 6` and failed PM4_DRAW_INDX with prim_type=6 from the map load on, only on
    the host (the guest log has none, same scene). **Gone with SDK patches 0034/0035** (2026-10-12: Sword Base
    in Forge, 3 minutes, 0 such lines; the remaining errors are a memexport constant warning and heap allocs). Nothing visibly missing. Lead: type-6
    primitives (quad/rect lists with tess_mode 0) rejected in the SDK's primitive processor.

- 2026-10-09 (night): **no more water draws; System Link discovery works.**
  - SDK patch 0022: real occlusion query results, without stalling the GPU. The SDK had
    switched host queries off at the first query and answered "visible" to all of them, so
    Reach kept issuing its adaptive-tessellation water every frame (the draw that hangs RADV).
    Now Forge (6 minutes on Sword Base) and the campaign through the Falcon takeoff run at
    30 fps with no water draw issued (patch 0018 still skips it if it comes). Note: the title
    and main menus remember their last selection in the profile, so the default
    `run_reach.sh` presses can land in Forge instead of the campaign. Trace in `docs/sdk_patches.md`.
  - System Link: a codegen jump-table gap froze the game when System Link was selected
    (`hints/switch_tables.toml`). `src/kernel/net.cpp` adds a virtual network
    (`REACH_NET=1`); two instances on one machine list each other's parties. Joining is the
    next step (`docs/online_plan.md`).
  - Debug aids: `REACH_NETTRACE=1` (network calls and datagrams), `REACH_FPSLOG=1` (frame rate
    every 5 s), `REACH_XUID` / `REACH_GAMERTAG` (a second identity).

- 2026-10-09 (late night): **System Link plays; challenges offline.**
  - System Link: the XamEnumerate completion codes (a content enumeration retried forever and
    blocked joins) and 360-style secure addresses (0.x.y.z) were the last blockers; two
    instances join one lobby and play Firefight together (`docs/online_plan.md`).
  - Daily/weekly challenges without Bungie's servers: picked from the date, shown in the
    START menu, saved per profile (`docs/progression_re.md` 4.1). Progress counting in play
    is not verified yet.

- 2026-10-09 (late night): **internet play through Reach Live.** `server/reach_live_server.py`
  (self-hostable, standard-library Python) plus a Live mode in `src/kernel/net.cpp`
  (`REACH_SERVER=host`): players on a server see each other's System Link games, join and play
  Firefight, directly via UDP hole punching or relayed by the server (about 4 KB/s for a
  two-player match). Each player gets a persistent online identity
  (`src/kernel/identity.cpp`). `docs/online_plan.md` section 5.

- 2026-10-09 (late night): **keyboard and mouse.** A synthetic device on guest user 0
  (`src/input/kbm.cpp`, merged with any pad) with bindings for Reach's Default layout, and
  mouse look applied as an angle inside player control (`sub_8247C978`) after the stick's
  acceleration and turn-rate cap, keeping zoom scaling and inversion
  (`src/hooks/mouse_look.cpp`, `hints/mouse_look.toml`). Verified with the FIFO's new
  `MOUSE:dx,dy` / `KEY:name` commands: 1636 counts turn exactly 90° (30° through the 3x DMR
  scope), pitch clamps at straight up/down, fire/zoom/swap/jump/crouch/move and menu keys work.
  `docs/input.md`.

- 2026-10-09 (late night): **emulated Xbox LIVE and keyboard/mouse.**
  - Reach Live signs the profile in to "Xbox LIVE" (on by default with a server): friends
    roster from the server, presence, session search, QoS game details; a friend joins a
    Firefight lobby from the roster and they play the match (`docs/online_plan.md` 5.2).
    Bungie's title servers: rewards sync answered by the server (5.1).
  - System Link through the server: Slayer on Sword Base with three players; host
    migration works.
  - Keyboard and mouse with direct mouse look (`docs/input.md`).
  - SDK patch 0020 also moves the viz-query logs to debug level (1000+ lines a second).

- 2026-10-09 (night, later): **challenges verified, a freeze fixed, File Share.**
  - Offline challenges count real kills and pay out on completion (`docs/progression_re.md` 4.1).
  - A Firefight freeze was a lost wakeup in the SDK's critical sections (a guest event
    header shared by two objects); `src/kernel/critical_section.cpp` replaces them
    (`docs/sdk_patches.md`, "Runtime fixes").
  - File Share through Reach Live: upload from Forge, browse a friend's share from the
    roster, download (`docs/online_plan.md` 5.3).

- 2026-10-10 (night): **Windows build runs; README covers building and playing.**
  - `tools/build_windows.sh` cross-compiles the patched SDK (Vulkan on) and the game with
    clang/clang-cl + xwin and packages `reach-recomp/out/dist/windows`. Under Proton 10 the
    game plays the intro and reaches the first-run Armory screen with the Vulkan backend.
  - It first hung at startup in our RtlEnterCriticalSection: on Windows the runtime maps
    guest addresses >= 0xE0000000 0x1000 bytes further on (64 KB mapping granularity), so
    `base + addr` in our code missed the SDK's VdHSIOCalibrationLock (0xFFCAB000). All our
    guest pointers now use `GuestPtr(base, addr)` (`src/platform/guest_memory.h`).
    Found with gdb on the Wine process: the user-mode RSP of a thread blocked in a syscall
    is in Wine's syscall frame (`*(gs:0x328) + 0x88`); scanning that stack for return
    addresses and symbolizing them with a `/DEBUG` PDB named the waiting function.
  - The SDK cross build needs the MSVC headers after clang's (`-idirafter`), or the SSE
    intrinsics become undefined external functions.
  - A fresh clone now builds with one configure + build (the first configure runs codegen).
  - The README has requirements, Linux and Windows build steps, playing, running a Reach Live
    server (Python or Docker; checked with podman) and troubleshooting.

- 2026-10-10: **Release builds in CI, private.** The private codegen repo
  (`DecompiledHaloReach-generated`) has `.github/workflows/release.yml` (Actions → Release →
  Run workflow, or push a `v*` tag there). It checks out this repo at the commit the private
  README records, copies the generated code in, builds Linux on ubuntu-22.04 (glibc 2.35 floor;
  Homebrew clang 23 against GCC 13's libstdc++, as the SDK nightly) and Windows (the
  `tools/build_windows.sh` cross build; xwin splats the MSVC CRT in CI), and publishes
  `RecompiledHaloReach-linux-x64.tar.gz`, `RecompiledHaloReach-windows-x64.zip` and checksums
  as a release of that private repo. The binaries contain translated game code: never attach
  them to this repo. It refuses a commit whose manifest or hints differ from the ones the code
  was generated with: after changing hints, regenerate and run `tools/sync_generated_repo.sh`.
  - Pieces here: `-DREACH_SKIP_CODEGEN=ON` (compile a supplied `generated/`),
    `tools/package_linux.sh` (flat package with `play.sh DATA_DIR`), `tools/build_rexglue_sdk.sh
    --source-only`.
  - Cost: first run about 55 runner minutes (Windows SDK 15 min, Windows game 8-16 min, Linux
    plugin 5 min, Linux game 7-15 min; GitHub's 2-vCPU runners vary 2x). With the SDK trees,
    the xwin splat and ccache (PCH-aware, depend mode) cached, a release whose generated code
    is unchanged takes about 9 minutes.
  - The game rejects an option given twice (CLI11), so wrappers must not add `--log_file` when
    the caller passes one.

- 2026-10-10: **No more tearing; frame-time statistics.** A shimmer crawling down the menus
  was tearing: the SDK presents with IMMEDIATE, KWin lets fullscreen games tear, and the
  tear line of a 30 fps image on a 144 Hz display lands at a new height every frame. Guest
  frames dumped three times a second on the title and main menus have no moving band. The
  game now defaults the SDK's present-mode settings to FIFO (`src/main.cpp`); MAILBOX was not
  chosen because the SDK's UI thread repaints nonstop on Linux (thousands of presents a
  second without a vsync wait, each rebuilding a pipeline). On a quiet machine FIFO takes the
  GPU from 44% to 21% busy on the main menu and from 55% to 32% in a SWAT game, the game's CPU
  use from 2.4 to 1.8 cores (menu), and the game holds 30.0 fps either way. `RECOMP_PERF=1`
  (`src/debug/frame_stats.cpp`, replacing `REACH_FPSLOG`) logs fps, percentiles, 1% lows,
  stutters and new pipelines for guest frames and host presents; `tools/bench.sh` measures
  the main menu or a Custom Game, warm or cold (`docs/perf.md`). The nonstop repaint came
  from the SDK's achievement toast dialog, always registered with the ImGui drawer; it is
  now registered only while a toast shows (`src/platform/achievement_toast.cpp`), and the
  window is repainted once per guest frame in every present mode.

- 2026-10-10: **Firefight freezes while loading a mission: half fixed, still open.**
  - Report: Firefight froze after picking Courtyard (the ONI Sword Base map, hence "Sword
    Base") following an Overlook game. The log's last lines, failed opens of
    `cache1:\autosave\cache1:\autosave\asq_ff10_pr_BBBBBBBB.film`, are a red herring: every
    game start after a game in the same session opens the previous film with that doubled
    path, in working runs too.
  - Reproduces with the user's profile copied to a separate `XDG_DATA_HOME`: Overlook game,
    END GAME, back to the lobby, pick Courtyard and press START GAME while the lobby still
    says "Readying map" (about 1 in 2 tries).
  - Cause found: loading a mission runs Reach's XEnumerate on a content aggregate
    enumerator thousands of times (about 2,000 a second for a moment, then ~15 a second).
    Each call is a XamTaskSchedule thread ending with `ObDereferenceObject(enumerator)`.
    X_KENUMERATOR has no dispatcher header, so its fields overwrite the handle the runtime
    stashes at +8/+12; the runtime then binds a bogus event there, frees it, and every later
    dereference releases whatever object reuses that handle (`REACH_SDK_OBJECT_REFS=1
    REACH_OBJTRACE=1` logs 4,000+ such releases of thread objects per load). Threads freed
    while running fault forever in `XThread::Exit` or their start lambda (gdb: 47 such threads,
    main thread spinning in `sub_820D4270`); threads never freed exhaust the guest heap
    ("XAM task creation failed: C0000017", then `memset` of a null XamAlloc result in
    `sub_828055B8` faults forever on the main thread). The old critical-section freeze (a
    handle also stashed in "a heap object at 0x30655018") has the same shape: enumerator
    objects sit at 0x3xxx5018.
  - `src/kernel/object_refs.cpp` fixes the dereference (finds the enumerator, never releases
    an object that does not own the pointer) and frees XAM task threads when they end. With it
    the thread use-after-free is gone, but the second Courtyard load still froze: the guest
    heap ran out (`BaseHeap::Alloc failed`, then the same null `memset`). Next: find what
    leaks per enumeration (the XamAlloc'd task block from `sub_828055B8`, freed only on
    failure?, or task thread stacks/TLS) and why the load enumerates thousands of times
    (probably a completion code the game treats as "retry", like the `XamEnumerate` fix).
    Hang diagnosis: `gdb -p PID -batch -ex 'thread apply all bt 25'`; threads faulting in
    `XThread::Exit` mean a freed thread object.

- 2026-10-10: **Firefight mission-load freeze fixed (two causes after the object-refs fix).**
  - Why a load listed content thousands of times: Reach's XEnumerate task (0x828057B0) passes
    the XAM app (0xFE, message 0x2000E) an output slot for the item count that is an
    uninitialized stack slot. The SDK writes it only when it finds an item and answers
    NO_MORE_FILES without touching it, so the task read back the guest stack's 0xBEBEBEBE
    fill as the count ("buffer full"), the content task (completion 0x825F49F0) never moved on
    to the next device, and the main thread's task drain (0x824BA280) re-ran it forever
    (about 1,000 passes a second; the old runs ended only when something else broke).
    `live_xmsg.cpp` now zeroes the count before the call.
  - The per-pass leak: the task message block (XamAlloc'd in 0x828055B8, one system-heap page
    or more) is freed by Reach only if XamTaskSchedule fails; the console's task runtime frees
    it when the task returns, the SDK's does not. `XamTaskMessageDone` (object_refs.cpp, called
    from the XMsgCompleteIORequest wrapper in net.cpp when the caller is the task's last
    call at 0x828058AC) frees the block and the event reference it holds. XamTaskSchedule now
    stores the dummy task handle before creating the thread.
  - Verified (copy of the profile, 3 x Overlook then Courtyard started while "Readying map",
    plus Corvette and Holdout 60 s each, plus an offline custom game): no freeze, 44-47 guest
    threads flat across loads, no "task creation failed". `REACH_OBJTRACE=1` also logs
    "ENUMTRACE" lines (enumerator creation args, enumerate callers) and freed task messages.
    Note: a START GAME press while the row is greyed ("Readying map") is ignored by the game;
    press it again when it says Ready.
  - Not XAM: the frame loop's wait in `sub_820E40F0` (docs/framerate.md) is an
    NtWaitForSingleObjectEx on a render hand-off event (via 0x826129A8/0x820CCF70), not an
    XAM task or overlapped; the signaller is the guest RENDER thread (`sub_820CA090` loop).
    (Correction, below: it is set by the guest AUDIO thread.)

- 2026-10-10: **45 fps floor removed; `frame_rate = 60`.** At a 120 Hz guest vblank every
  scene ran 45-46 fps (21.8 ms) with all threads mostly waiting. The guest AUDIO thread asks
  the music player app for XMPGetPlaybackController during every frame, and the SDK answers
  it after a 10 ms sleep on any non-main thread (a Xenia throttle for another game); the main
  and render threads wait for AUDIO each frame. `live_xmsg.cpp` answers it without the sleep:
  the title screen and a SWAT game run 60.0 fps (p50 16.8 ms) at a 120 Hz vblank, ~235 fps
  with `--vsync=false`, and still exactly 30 fps by default. `frame_rate = 60` sets the guest
  vblank to 120 Hz, because the render thread's schedule is in vblanks too (a hook on the
  limiter alone left the screen at 30 fps). Found with the new `REACH_SYNCTRACE`
  (`src/debug/sync_trace.cpp`). Details and the limits for 144 fps: `docs/framerate.md`.

- 2026-10-11: **`frame_rate = 60` is 60 ticks a second; campaign cinematics fixed.** The game
  sets its tick rate to half the display refresh rate (`sub_82476A60`), so a 120 Hz vblank
  means 60 ticks. Winter Contingency's opening replayed forever at 60: cinematics end with a
  scripted revert to the checkpoint `cinematic_skip_start` saved, and the restored script asks
  `game_reverted` (game time == revert time) one tick too late at 60 ticks.
  `src/hooks/frame_rate.cpp` widens it to one 30 Hz tick; the campaign now reaches gameplay.
  Firefight, matchmaking (60/60, 60/30) and System Link co-op at 60/60 checked; network co-op
  with a 30 fps player errors out ("An error occurred while playing co-op"). `REACH_GSTRACE=1`
  now logs script reverts with the script's call stack. Details: `docs/framerate.md`.
- 2026-10-11: **Players with different `frame_rate` settings are kept apart online.** Each
  machine ticks at its own display rate, so mixed co-op breaks. `src/hooks/frame_rate_net.cpp`
  folds the tick rate into the build number the game already compares when joining (System
  Link list, roster, join-request), and matchmaking searches carry it in their key, so the
  Reach Live server only matches equal rates. `REACH_MIXED_FRAME_RATE=1` turns it off for
  tests. Details: `docs/framerate.md`.
- 2026-10-11: **Mixed frame rates: multiplayer looks fine, co-op diverges in physics.** A
  60 fps host and a 30 fps client in a System Link Slayer game (gate off) moved, shot, damaged
  and killed each other normally, same fire rate and clocks. In co-op, forcing 30 ticks (and
  even 30 fps) on the 60 fps machine is not enough: game state dumps of both machines
  (`REACH_GSDUMP`, `tools/gsdiff.py`) agree at tick 5 and differ at tick 10 in the physics
  props dropping at the start of the map, then in the AI, until "An error occurred while
  playing co-op"; a 30/30 control stays identical. Something besides the game's tick rate
  reads the 120 Hz display rate, most likely the physics. Details: `docs/framerate.md`.
- 2026-10-12: **Players at 30 and 60 fps play together, co-op included.** The co-op
  divergence was Havok's collision tolerances: the game computes them from the tick rate of
  the session's first Havok world (the main menu's, 60 ticks a second at 60 fps) and copies
  them back after every later world creation (`sub_8251D3F8`), so a co-op game forced to 30
  ticks still collided with 1/60 s TOI thresholds and its falling props drifted from tick 8.
  `src/hooks/frame_rate.cpp` now computes them for 1/30 s always (`sub_8284FD70`) and runs
  network co-op at 30 ticks a second on every machine (60 fps machines still present 60
  frames a second). A local reproduction (solo Firefight, `REACH_SYNC_TICKS30=3`) matches a
  30 fps run in every object; 60/30 and 30/60 System Link Firefight ran 140 s and 7 min
  without the co-op error, 60/60 and 30/30 co-op and the campaign at 60 fps still work. The
  frame rate gate in `frame_rate_net.cpp` is off unless `REACH_SYNC_TICKS30=0` (co-op at the
  display's rate).
  New debug switches: `REACH_HKTRACE`, `REACH_GSDUMP_XEX`, `REACH_HAVOK_NATIVE_STEP`.
  Details: `docs/framerate.md`.
- 2026-10-11: **Carter's and Kat's faces render (SDK patches 0034, 0035).** Their faces in
  cinematics are indexed triangle strips with discrete tessellation, which the SDK (and
  Xenia) dropped ("Unsupported tessellation mode 0 for primitive type 6"). 0035 draws
  tessellated strips and fans as lists. 0034 fixes the GLSL tessellation shaders, which read
  their factors and index bounds from the shared memory buffer instead of the system
  constants. That was most likely why the earlier attempt (`experimental/0014`) and the
  water hung the GPU. GPU plugins load from the executable's folder, so `REXSDK` A/B runs
  never swapped the plugin. Details: `docs/rendering.md`. Open there: Falcon "back and forth"
  check at 60 (one suspicious burst in the formation flight, 30 fps and START-skip runs not
  done), and faces on/off perf numbers.
- 2026-10-12: **Cinematic cameras move at 60 Hz (Falcon flight flicker fixed).** At
  `frame_rate = 60`, the near Spartans in Winter Contingency's Falcon flight vanished on
  alternate frames and the cabin jumped back and forth; 30 fps was clean. Cause: a cinematic
  scene shot's camera is a list of 30 Hz frames picked by `(int)(ticks * seconds_per_tick *
  30)`, so the camera moved on every other tick while the Falcon moved on every tick, and
  trailed it by 0.4 m on alternate frames. Occlusion queries and clip planes were ruled out.
  `src/hooks/frame_rate.cpp` blends the camera between two frames (`sub_8249FEB0`). Now
  clean in two 60 fps runs, and gameplay after the flight works. New tools:
  `REACH_CINEREC` (per-frame records of thread-global blocks and cinematic objects,
  `src/debug/sync_diff.cpp`) and `tools/cinerec.py`. Details: `docs/rendering.md`.

## Debugging recipes

- Run: `tools/run_reach.sh <secs> [flags]` (logs `/tmp/reach_run.log`, rotates at 5 MB). It skips
  the intro by pressing START at 9 s (`REACH_AUTOPRESS`, the title screen is up by ~15 s) and removes
  orphaned `/dev/shm/xenia_memory_*` (5 GB each; leaked ones fill /dev/shm and every later run dies
  with SIGBUS).
- GPU differential debugging with RenderDoc (how the black menu was solved): capture ours with
  `REACH_RDCAPTURE=<secs>` under `renderdoccmd capture` (layer setup in `docs/menu_black_screen.md`),
  capture Xenia with `tools/renderdoc/trigger_capture.py`, then run the `tools/renderdoc/*.py`
  scripts with `qrenderdoc --python` to match draws, diff constants/textures/interpolators, and
  swap in instrumented SPIR-V (`instrument.py`) to see intermediate register values.
  Load times vary by tens of seconds between runs (more under RenderDoc), so fixed capture times
  miss shots: set `REACH_RDCAPTURE_TRIGGER=<file>` and touch that file to capture (the file is
  polled every 10 swaps and deleted), e.g. from a loop that converts periodic `REACH_FRAMEDUMP`s
  and checks them. `/tmp` is RAM-backed here; delete captures you are done with.
- Stepping through menus by hand: `tools/live_session.sh DIR` starts the game in the
  background with an input FIFO; `tools/live_step.sh DIR WAIT [INPUT...]` presses inputs
  (`A`, `UP`, `START:0.5`), waits and writes a screenshot to `DIR/last.png`;
  `tools/menu_state.py title|main PNG` reads which title/main menu item is highlighted (the
  game remembers the last choice in the profile, so never count presses). `--headless=true`
  answers system dialogs (keyboard, device selector) with their defaults, which scripted
  Forge saves need; without it they are ImGui overlays that frame dumps don't show. If
  `live_step.sh` prints "no frame", the game has stopped presenting.
- System Link on one machine: `tools/system_link_pair.sh DIR_A DIR_B` starts two instances
  with the virtual network (`REACH_NET=1`; the second gets its own profile, XUID and
  gamertag via `REACH_INSTANCE=2`), makes A host a Firefight lobby and selects A's party on
  B. `echo X:0.3 > DIR_B/in.fifo` presses "Join". `REACH_NETTRACE=packets` logs datagrams.
- Through a Reach Live server: start `server/reach_live_server.py --host 127.0.0.1 --http-port
  21101`, then `REACH_SERVER=127.0.0.1 tools/system_link_pair.sh DIR_A DIR_B` (profiles in
  `DIR_A/data`, `DIR_B/data`; keep them on disk, not in RAM-backed `/tmp`).
  `REACH_SERVER_RELAY=1` forces the relay path. `curl localhost:21101` shows players and
  relayed bytes.
- `tools/live_shot.sh DIR` saves what the window shows (the presenter's output, at the draw
  resolution scale) as `DIR/shot.png`; `live_step.sh` frame dumps read guest memory and stay
  at 1152×720. `live_session.sh` removes orphaned `/dev/shm/xenia_memory_*` files before
  starting (killed instances leave 5 GB each; a full /dev/shm kills new instances with SIGBUS).
- `qrenderdoc --python` crashes with "Illegal instruction" under `QT_QPA_PLATFORM=offscreen`;
  run it without that variable.
- Each instance takes about 6 GB of RAM and 5 GB of `/dev/shm`. `REACH_NETTRACE=1` logs every
  network call; prefer `REACH_NETTRACE=packets` for long runs (logs go to RAM-backed `/tmp`).
- Frame rate and stutter: `tools/bench.sh menu|gameplay [--cold]`, or `RECOMP_PERF=1` in any
  run (`docs/perf.md`). Measure only with no other game running (it prints `CONTENDED`
  otherwise).
- Who waits on whom: `REACH_SYNCTRACE=FILE`, then `touch FILE.trigger`: 3 s of every guest
  kernel wait and signal with thread, handle, duration and guest call stack
  (`src/debug/sync_trace.cpp`, analysis in `docs/framerate.md`).
- Frame dumps without screenshots: `REACH_FRAMEDUMP=20,28` + `--vulkan_readback_resolve=true`, then
  `tools/frame_to_png.py`. `REACH_FRAMEDUMP_BURST_TRIGGER=<file>` dumps `REACH_FRAMEDUMP_BURST`
  (default 60) consecutive frames each time the file appears, to check motion frame by frame.
- What changes from one frame to the next in game state: `REACH_CINEREC=FILE`, then `touch
  FILE.trigger`. It records thread-global blocks (cinematic, observer, director, scripted
  camera, game time, or any `REACH_CINEREC_BLOCKS="off:size,..."`) and the cinematic's object
  positions after each main-thread frame. Read it with `tools/cinerec.py FILE flips|steps`.
  This is how the 30 Hz cinematic camera was found (`docs/rendering.md`).
- Comparing GPU plugins: the SDK loads `librexgpu-xenos.so` from the executable's folder only;
  `REXSDK` switches `librexruntime.so`, not the plugin. Copy `reach`, the `reach_*` module
  libraries and the plugin into a folder and start it there (`docs/rendering.md`). Texture binding trace: `REACH_TEXTRACE=1`. Live guest memory: `tools/guestmem.py`.
  `--gpu_allow_invalid_fetch_constants=true` silences thousands of fetch-constant warnings.
- Find the guest function behind an "Unhandled guest access violation": run under gdb with
  `catch signal SIGSEGV` conditioned on `$_siginfo._sifields._sigfault.si_addr` (guest address
  `X` is host `0x100000000 + X`); the backtrace shows `__imp__sub_XXXXXXXX` frames with
  generated source lines. Other SIGSEGVs are normal (MMIO / GPU write watches), so pass them.
- Stall diagnosis: `gdb -p $(pgrep -x reach) -batch -ex 'thread apply all bt 30'`; guest
  frames appear as `sub_XXXXXXXX`.
- Screenshots: `spectacle -b -n -a -o shot.png` while the game window is focused.
- Kernel imports can be overridden from `src/` by defining `extern "C" REX_FUNC(__imp__Name)`
  (generated code calls `__imp__Name` directly; the executable's definition wins).
- Shader stores (ours: `~/.local/share/reach/cache/shaders/shareable/4D53085B.xsh`, Xenia:
  `~/.local/share/Xenia/cache_host/...`): `tools/xsh_fetches.py STORE [HASH..]` lists texture/vertex
  fetch slots per shader; `--dump_shaders=<dir>` writes microcode disassembly.
- Ghidra: `tools/ghidra_scripts/ReachFixSaveRestHelpers.java` repairs prologue-truncated
  functions (run via `/run_script_inline`; the headless server must be started with
  `GHIDRA_MCP_ALLOW_SCRIPTS=1`). Decompile endpoint is `/force_decompile`.
