# RecompiledHaloReach

A native PC port of **Halo: Reach (Xbox 360)** built by statically recompiling the
original PowerPC executable to C++ with the [ReXGlue SDK](https://github.com/rexglue/rexglue-sdk),
plus the reverse-engineering notes and tooling needed to get it running.

> **Status: playable on Linux, work in progress.** The campaign, Firefight, Forge,
> Theater and custom games run natively with keyboard and mouse or a controller.
> Credits, ranks, the Armory and daily/weekly challenges work without Bungie's servers.
> Online play goes through a self-hostable **Reach Live** server that stands in for
> Xbox LIVE and Bungie's services: friends, invites, System Link over the internet and
> File Share. Matchmaking and Windows builds are not done yet.

This repository contains **no game code and no game data**. You need your own copy of
the game (see [Legal](#legal)).

## Goals

1. Run the Xbox 360 build of Halo: Reach natively on x86-64 Linux, then Windows.
2. Bring back the original **Credits (cR), rank and Armory progression**: earn cR by
   playing, rank up, buy armor with cR. No microtransactions.
3. **Peer-to-peer online play** in place of Xbox Live / system link.
4. **Forge plus file share**: upload and download maps, gametypes and mods.

## How it works

- `default.xex` (Title ID `4D53085B`, base version 1.0, Media ID `566C10D3`) and the
  four Waves audio DLLs it loads are translated function by function into C++ by
  `rexglue codegen`, driven by `reach-recomp/reach_manifest.toml`.
- The ReXGlue runtime (derived from [Xenia](https://github.com/xenia-canary/xenia-canary))
  provides the Xbox 360 kernel/XAM high-level emulation, the Xenos GPU on Vulkan,
  audio and the virtual file system.
- This repo supplies what the generic toolchain can't work out on its own:
  - `reach-recomp/hints/`: function boundaries and entry points the code scanner
    missed, plus statically linked XAPI routines mapped to native SDK versions
    (for example, the game's fiber switching).
  - `reach-recomp/src/`: the app class and native overrides of kernel and XAM exports:
    the virtual network and Reach Live client, Xbox LIVE services (friends, presence,
    sessions, invites, title servers), offline challenges, keyboard and mouse,
    runtime fixes (critical sections, file sizes, enumeration codes), RSA public-key
    crypto, scripted input and frame dumps for automated tests, cross-DLL import thunks.
  - `server/`: Reach Live, the self-hostable online server (standard-library Python).
  - `tools/`: an ISO extractor, entry-point discovery against Ghidra, thunk
    generation, run and debug scripts, and Ghidra repair scripts.
  - `docs/`: findings, the status log and debugging recipes.

## Status

| Area | State |
| --- | --- |
| Codegen of `default.xex` + 4 Waves DLLs | Done (~24k functions, about 167 MB of C++) |
| Native build (Linux, Clang) | Builds and links |
| Windows build | Cross-compiled from Linux (`tools/build_windows.sh`); under Proton it plays the intro and reaches the menus with the Vulkan backend. Not yet tried on Windows itself |
| Boot | Kernel init, threads, cache partitions, fibers, RSA signature checks |
| Intro video | Plays |
| Title screen and start menu | Render correctly with the patched GPU plugin (the SDK bug behind the black menu is described in `docs/menu_black_screen.md`) |
| Campaign (Winter Contingency) | Loads, plays the opening and reaches gameplay; rendering matches Xenia after 19 SDK GPU patches ([`docs/sdk_patches.md`](docs/sdk_patches.md)); real occlusion queries, so the game no longer issues the water draw that hangs the GPU |
| Firefight, custom games, Theater | Play locally; films of past games play back in Theater |
| Progression (cR/rank/Armory) | Works offline: cR earned in play, rank-ups, Armory purchases saved in the profile, Spartan preview in the Armory and post-game screens, daily and weekly challenges picked locally each day, counting kills and paying out on completion ([`docs/progression_re.md`](docs/progression_re.md)); with a Reach Live server, progression is also stored server-side |
| Forge | Works locally: editing on Sword Base, object placement, saving map variants |
| Online | Over the internet through a self-hosted **Reach Live** server: an emulated Xbox LIVE (friends roster, presence, joining a friend, invites) and System Link (the game's browser lists everyone's games). Tested with up to three players on one machine: Firefight and Slayer through the postgame, host migration. Direct peer-to-peer with NAT hole punching, relayed by the server when that fails ([`docs/online_plan.md`](docs/online_plan.md) section 5). Matchmaking (in progress) and Xbox LIVE party are not available |
| File share | Through Reach Live: upload maps and game types from the game's File Share UI, browse a friend's share from the roster and download into Local Files ([`docs/online_plan.md`](docs/online_plan.md) section 5.3) |
| Higher resolution | `resolution_scale = 2` in `reach.toml` (or `--resolution_scale=2`) renders at 2304×1440 instead of 1152×720; menus and Firefight checked, no artifacts seen. Frame rate on a dedicated GPU still to be measured |
| Keyboard and mouse | On by default next to pads: Halo-style bindings (rebindable `kbm_bind_*` cvars) and raw mouse look fed straight into player control, no stick emulation ([`docs/input.md`](docs/input.md)) |

See [`docs/PROJECT.md`](docs/PROJECT.md) for the detailed status log and debugging recipes.

## Requirements

- **Linux x86-64** with a Vulkan 1.3 GPU (tested on AMD with Mesa RADV). About 8 GB of free
  RAM per running game and 30 GB of disk (6.6 GB of extracted game files plus the build).
  Windows builds are cross-compiled from Linux (see [Windows](#windows)).
- **Your own copy of Halo: Reach** for Xbox 360 as a disc image (`.iso`, base version, no
  title update).
- **Tools:** Clang (tested with 23), CMake ≥ 3.25, Ninja, Python 3, Git, `unzip`.
  Building the patched GPU plugin also needs the X11/XCB and Wayland development headers
  (on Fedora: `libX11-devel libxcb-devel libXext-devel libXfixes-devel libXcursor-devel
  libXi-devel libXrandr-devel libXScrnSaver-devel libXrender-devel libXtst-devel
  wayland-devel`; on immutable systems, Homebrew's `libx11 libxcb libxext libxfixes
  libxcursor libxi libxrandr libxscrnsaver libxrender libxtst xorgproto wayland` work too).
- **[ReXGlue SDK](https://github.com/rexglue/rexglue-sdk) 0.10.0.24 nightly**
  (`nightly-20261002-bd833a2a`; later nightlies may not match the patches).
- Optional, for reverse engineering: Ghidra 12.1.4 with
  [XEXLoaderWV](https://github.com/zeroKilo/XEXLoaderWV) and
  [GhidraMCP](https://github.com/bethington/ghidra-mcp).

## Building (Linux)

All commands run from the repository root.

```sh
# 1. The ReXGlue SDK nightly, unpacked to ~/rexglue-sdk-nightly/0.10.0.24/linux-amd64
mkdir -p ~/rexglue-sdk-nightly/0.10.0.24 && cd ~/rexglue-sdk-nightly/0.10.0.24
curl -LO https://github.com/rexglue/rexglue-sdk/releases/download/nightly-20261002-bd833a2a/rexglue-sdk-0.10.0.24-dev.gbd833a2-linux-amd64.zip
unzip rexglue-sdk-0.10.0.24-dev.gbd833a2-linux-amd64.zip   # creates linux-amd64/
cd -

# 2. The game files, extracted from your disc image (the image is only read)
python3 tools/xdvdfs_extract.py "/path/to/Halo - Reach.iso" extract extracted/xbox360

# 3. The patched Xenos GPU plugin and codegen tool: clones the SDK source to
#    ~/rexglue-sdk-src, applies patches/rexglue-sdk/ and installs an overlay at
#    ~/rexglue-sdk-patched (the stock plugin renders the menus black and lacks this
#    project's 22 rendering fixes; the stock rexglue's dot products differ from Xenon's)
tools/build_rexglue_sdk.sh

# 4. Configure and build. The first configure runs codegen on your default.xex with the
#    overlay's rexglue (the recompiled C++ goes to reach-recomp/generated/, a few
#    minutes); the build compiles it.
cmake -S reach-recomp -B reach-recomp/out/build/linux-nightly -G Ninja \
  -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_COMPILER=clang++ \
  -DCMAKE_PREFIX_PATH=$HOME/rexglue-sdk-nightly/0.10.0.24/linux-amd64
ninja -C reach-recomp/out/build/linux-nightly
```

When `reach-recomp/generated/` already holds codegen output made elsewhere, configure with
`-DREACH_SKIP_CODEGEN=ON` to compile it without `default.xex`. `tools/package_linux.sh` makes a
self-contained copy of the build in `reach-recomp/out/dist/linux`: the game, the SDK runtime
and patched GPU plugin, the ISO extractor and a launcher (`./play.sh /path/to/extracted/xbox360`).

## Playing

```sh
tools/play.sh                        # play
tools/play.sh --resolution_scale=2   # render at 2304x1440 instead of 1152x720
```

- **Controls:** keyboard and mouse work out of the box next to any controller: WASD,
  mouse look, left click fire, right click zoom, Space jump, Ctrl crouch, Q melee,
  G grenade, E/R reload and action, Shift armor ability, 1/2 or the wheel to switch
  weapons, Tab scoreboard, Esc menu ([`docs/input.md`](docs/input.md) has every binding).
- **Settings** live in `reach-recomp/out/build/linux-nightly/reach.toml`, one `name =
  value` per line, and in the in-game settings overlay (**F4**). For example:

  ```toml
  resolution_scale = 2
  kbm_sensitivity = 3.0
  live_server = "reach.example.org"   # Reach Live server (see below)
  gamertag = "Noble Six"
  ```

- **Vsync** is on (no tearing); `vulkan_allow_present_mode_immediate = true` in `reach.toml`
  turns it off, `vulkan_allow_present_mode_mailbox = true` picks mailbox ([`docs/perf.md`](docs/perf.md)).
- **Saves** (profile, Credits, Armory, maps, films) are in `~/.local/share/reach/`; the log
  is `~/.local/share/reach/reach.log`.
- Skip the intro video with Esc (Start); in menus Enter is A and Backspace is B.

## Windows

The Windows version is cross-compiled from Linux with clang-cl and lld-link, after the Linux
build above (its codegen output is reused, and step 3 checks out the SDK source).
`tools/build_windows.sh` builds the ReXGlue SDK for Windows from that source with this
project's patches and the Vulkan backend (the SDK's prebuilt Windows GPU plugin has none),
then the game, and packages it in `reach-recomp/out/dist/windows`:

```sh
# Homebrew's LLVM and lld provide clang, clang-cl and lld-link
brew install llvm lld

# The MSVC CRT and Windows SDK, unpacked by xwin (a release binary from
# https://github.com/Jake-Shadle/xwin/releases, or cargo install xwin).
# Running it means accepting Microsoft's license.
xwin --accept-license splat --output ~/.local/opt/xwin/sdk

tools/build_windows.sh
```

On Windows, copy `reach-recomp/out/dist/windows` anywhere, install the Microsoft Visual C++
2015-2022 x64 redistributable, and run it with your extracted game files:

```bat
reach.exe --game_data_root=C:\path\to\extracted\xbox360
```

`reach.toml` next to `reach.exe` takes the same settings as on Linux; `gpu_backend =
"vulkan"` (the default) uses the backend with this project's fixes, `"d3d12"` the SDK's
Direct3D 12 one. Saves and the Reach Live identity go to `Documents\reach`.
So far the Windows build is tested under Proton on Linux, not on Windows itself.

## Playing online (Reach Live)

Reach Live is a small server that stands in for Xbox LIVE and Bungie's services. With it
the game signs in to an emulated Xbox LIVE: everyone on the server is your friend,
friends in a game show up in the lobby roster, **X** joins them and **Invite to Party** in a
friend's player menu brings them to you; System Link (**Y**, Select Network) lists
everyone's System Link games. Credits, ranks and Armory unlocks sync to the server, and
File Share uploads and downloads go through it.

**Joining a server:** add `live_server = "host"` (or `"host:port"`) to `reach.toml`, or run
`REACH_SERVER=host tools/play.sh`. Your gamertag is in
`~/.local/share/reach/4D53085B/live_identity.txt` (created on the first online run; edit it
or set `gamertag`). `live_room = "name"` keeps a group of players to themselves;
`live_signin = false` turns the Xbox LIVE emulation off (System Link only).

**Running a server** needs Python 3 (no packages) or Docker:

```sh
python3 server/reach_live_server.py --port 21100 --http-port 21101 --data-dir reach_live_data
# or
docker build -t reach-live server/
docker run -d -p 21100:21100/udp -p 21101:21101 -v reach-live-data:/data reach-live
```

- Open **UDP 21100** (game traffic, rendezvous and relay) and **TCP 21101** (Bungie's
  services: rewards sync, File Share, presence; also a JSON status page at `/status`).
- The data directory keeps players' progression and File Share files.
- Players connect directly to each other through UDP hole punching; when their routers
  don't allow it the server relays the game (a few KB/s per player; `--rate-limit` caps it,
  64 KB/s per player by default). Players behind strict NATs can forward one UDP port and
  set `REACH_NET_PORT` to it.
- Protocol and design: [`docs/online_plan.md`](docs/online_plan.md) section 5.

## Developer tools

- `tools/run_reach.sh SECONDS` runs a time-limited test that skips the intro (logs in
  `/tmp/reach_run.log`).
- `tools/bench.sh menu|gameplay [--cold]` measures frame rate and stutter on the main menu or
  in a Custom Game (`RECOMP_PERF=1` logs the same statistics in any run;
  [`docs/perf.md`](docs/perf.md)).
- `tools/live_session.sh DIR` / `tools/live_step.sh DIR WAIT INPUT...` drive a game from a
  script (input FIFO, frame dumps); `tools/live_shot.sh DIR` saves what the window shows.
- `tools/system_link_pair.sh DIR_A DIR_B` starts two games on one machine in a System Link
  lobby (with `REACH_SERVER`, through a Reach Live server).
- `server/test_reach_live_server.py`, `server/test_reach_live_lsp.py`: server tests.
- [`docs/PROJECT.md`](docs/PROJECT.md) has the status log and debugging recipes (RenderDoc,
  gdb, guest memory).

## Troubleshooting

- **Menus or textures are black / too dark:** the stock GPU plugin is loaded. Run
  `tools/build_rexglue_sdk.sh` and use `tools/play.sh` (it loads the patched overlay).
- **The game dies with SIGBUS at start:** `/dev/shm` is full of guest memory left by killed
  games (5 GB each); `tools/play.sh` removes orphaned `/dev/shm/xenia_memory_*` files when it
  exits, or delete the ones no running game uses.
- **No online players:** check that the server's UDP port is reachable and that both
  players use the same `live_room`; the server's `/status` page lists who is connected.

## Repository layout

| Path | What |
| --- | --- |
| `reach-recomp/reach_manifest.toml` | Codegen manifest: main executable and guest DLL modules |
| `reach-recomp/hints/` | Function boundary, entry point and native-replacement (`[rexcrt]`) hints |
| `reach-recomp/src/` | Our runtime code: app setup, kernel overrides, cross-DLL thunks |
| `reach-recomp/generated/` | Codegen output. **Not committed** here (translated game code), except the SDK's `rexglue.cmake`. The maintainer keeps a private copy, synced with `tools/sync_generated_repo.sh` |
| `patches/rexglue-sdk/` | Fixes we carry on top of the ReXGlue SDK (built by `tools/build_rexglue_sdk.sh`) |
| `server/` | Reach Live, the self-hostable online server (and its Dockerfile) |
| `tools/` | Extraction, analysis, run and debug tooling (`tools/renderdoc/`: GPU capture analysis) |
| `docs/` | Project log, debugging recipes, reverse-engineering notes |
| `docs/symbols/` | Function names recovered in Ghidra (`address,name` CSV) |

## Legal

This project is not affiliated with, endorsed by or sponsored by Microsoft, Xbox Game
Studios, 343 Industries or Bungie. *Halo* and *Halo: Reach* are trademarks of
Microsoft Corporation.

The repository distributes no copyrighted game material: no executables, no
recompiled game code, no maps, videos or other assets. The build only works with
game files you extract from a copy you legally own.

## Credits

- [ReXGlue SDK](https://github.com/rexglue/rexglue-sdk): recompiler and runtime
- [Xenia](https://github.com/xenia-canary/xenia-canary): the emulator the runtime's
  kernel and GPU layers derive from, and our reference for differential debugging
- [XenonRecomp](https://github.com/hedge-dev/XenonRecomp): the pioneering Xbox 360
  static recompiler
- [GhidraMCP](https://github.com/bethington/ghidra-mcp) and
  [XEXLoaderWV](https://github.com/zeroKilo/XEXLoaderWV): reverse-engineering tooling

## License

The code in this repository is released under the [MIT License](LICENSE). This does
not cover the game, its assets, or the third-party SDKs and tools it uses.
