# ReXGlue SDK patches we carry

The ReXGlue SDK's Xenos GPU emulation is a port of Xenia from around early December 2025
(nightly 0.10.0.24, commit `bd833a2`). Upstream does not accept Xenos GPU changes
(rexglue/rexglue-sdk#456), so fixes live in `patches/rexglue-sdk/` and
`tools/build_rexglue_sdk.sh` builds a patched `librexgpu-xenos.so` from them (see
`docs/PROJECT.md` > Tooling). One patch (0025) is in the CPU code generator instead: the
script also builds a patched `rexglue`, and the reach build runs codegen with it. Most of
them are Xenia fixes the SDK never got; a few are porting slips. Every patch says "as in Xenia" in its comments and was checked against
Xenia Canary `82d0cd1f4`, the build we use as the reference.

| Patch | Area | What was wrong | Visible effect in Reach |
| --- | --- | --- | --- |
| 0001 | SPIR-V tfetch | Result exponent bias read from fetch-constant word 4 (LOD bias bits) instead of word 3 | Every texture 256x too dark: black title menu. **Fixed.** |
| 0002 | SPIR-V tfetch | Stacked-texture layer lerp started from the second layer (porting slip) | none seen |
| 0003 | SPIR-V tfetch | Missing clamp of the stacked-texture layer index (later Xenia fix) | none seen |
| 0004 | Vulkan RT cache | `k_2_10_10_10` render targets hosted as 8-bit `A8B8G8R8` (Xenia dc66d67a31 moved to `A2B10G10R10`) | lighting precision |
| 0005 | Vulkan shared memory | Resolve (compute) writes declared as `SHADER_READ` in the next barrier (Xenia a716dc6fac) | possible stale resolve data |
| 0006 | RT ownership transfer | 8888 <-> GAMMA conversion direction inverted (porting slip when gamma-as-UNORM16 was ported) | wrong colors when Reach aliases its gamma albedo target |
| 0007 | Vulkan texture cache | No barrier between two uploads into the same texture (Xenia 1fdbe569e4, c8f214e2a9) | possible mixed texels |
| 0008 | Texture cache | Texture marked up to date even if its memory was invalidated while loading (Xenia 9781a75a22) | possible stale textures |
| 0009 | Shader translator | Scalar ALU operand ignored the Z component when paired with a 3-source vector op (Xenia 92ada8ebc0) | wrong shader math |
| 0011 | Resolve clears | 64bpp clear value halves swapped (Xenia 16e1eb8e28) | 16_16_16_16 / 32_32 clears in wrong colors |
| 0013 | Draw filtering | Draws with `kill_pix_post_hi_z` but no VIZ query were rasterized (Xenia draw_util) | proxy geometry drawn into targets |
| 0015 | Vulkan RT transfer | Raw gamma bytes decoded to the lower edge of their linear range before being stored in the R16G16B16A16_UNORM gamma target; UNORM16 rounding then re-encodes some bytes one lower (Xenia decodes to the midpoint, `GammaByteToLinearMidpoint`) | red/green speckled terrain, foliage and stars in the campaign. **Fixed.** |
| 0016 | Vulkan tessellation | The adaptive tessellation vertex shader converted the edge factor's raw bits to float (`float(value)`) instead of reinterpreting them (`uintBitsToFloat`), so every edge got the maximum factor; 16-in-32 index endianness also applied an 8-in-16 swap (Xenia `tessellation_adaptive.vs`) | needed for water; the GPU hang persists (see below) |
| 0017 | SPIR-V domain shaders | Triangle domains and patch-indexed quads leave r0.w at 0; Xenia sets it to 1 | none seen yet (tessellated draws) |
| 0018 | Primitive processor | New option `skip_adaptive_triangle_tessellation` (default on): drops adaptive triangle-patch draws with a one-time warning, because the first one hangs the GPU | not a fix: those surfaces (probably water) are missing |
| 0019 | Defaults | `execute_unclipped_draw_vs_on_cpu` true, `readback_memexport` false, `gpu_allow_invalid_fetch_constants` true, as in Xenia. Without the first, a screen-space draw with clipping disabled was taken to use the whole EDRAM, so a depth buffer claimed the scene's tiles and the next ownership transfer replaced the lit scene with zeros | black 3D world in gameplay (HUD fine). **Fixed.** |
| 0020 | VIZ queries | `PA_SC_VIZ_QUERY_STATUS` bits were set ("visible") at every VIZ_QUERY end and never cleared; Xenia leaves the register alone. Also logs each query's begin/end at debug instead of info level (Reach issues dozens per frame: over 1000 log lines a second in play) | none seen; much smaller logs |
| 0021 | SPIR-V vertex position | A vertex position with W = 0 is clipped on the host; on the Xenos X/W and Y/W go to infinity and a rectangle drawn with clipping disabled covers the target. Now W = 1 and X, Y = ±2^16 (Xenia has the same gap) | Spartan missing from the Armory preview and the post-game/PROMOTED screens. **Fixed.** |
| 0022 | Vulkan occlusion queries | Host occlusion queries were switched off for good at the first query, because D3D's begin and end reports sit at different addresses and the SDK expected one; every query then returned the fake 1000 samples ("visible"). Reports are now snapshots of a running sample counter, as on the Xenos, written without waiting for the GPU: a guess (the query's last real result, zero included, as Xenia's "fast-alt") is rewritten with the real value when the GPU has finished | Reach issued its adaptive-tessellation water every frame (the draw that hangs the GPU) because the visibility test behind it always passed. **Fixed:** the draw is no longer issued in Forge or the campaign |
| 0025 | Codegen: `vmsum3fp128`/`vmsum4fp128` | The dot products ran SSE `dpps`, which gives +/-infinity (or NaN) when the sum of finite inputs overflows float32. Xenon gives QNaN `0x7FC00000`, as Xenia emulates (`DOT_PRODUCT_3/4`). Reported upstream as rexglue/rexglue-sdk#491 | none seen in Reach; in Forza Horizon the collision GJK's first termination test relied on it, and free roam crashed about half the time |

`experimental/0014` (tessellated triangle strips/fans as lists) is not applied; see its
README.

Audit method (worth repeating for the rest of the GPU code): normalize SDK and Xenia
sources (strip comments/whitespace, `xe::`/`rex::`), diff statement by statement, and look
for near-identical statements with one token changed; then list Xenia commits after the
fork point that touch the same files and check which the SDK has. RenderDoc scripts for
confirming a suspect on a live capture are in `tools/renderdoc/`.

## The red terrain (fixed by 0015)

Reach draws its alpha-tested and forward-lit surfaces (foliage, terrain detail, stars) into
the light buffer as `k_2_10_10_10_FLOAT` (7e3, three exponent and seven mantissa bits per
channel), re-aliases those EDRAM tiles as `k_8_8_8_8_GAMMA` and then as `k_8_8_8_8`, and
resolves them to 0x02354000. A full-screen "combine" pass reads that memory as
`k_2_10_10_10_AS_16_16_16_16` with an exponent bias of +3 and decodes the 7e3 bits by hand
(`floor`/`fract`/`exp2`). The bits only survive if every host-side conversion along the way is
exact. Our gamma render targets are stored as linear R16G16B16A16_UNORM: the transfer decoded
each gamma byte to the exact lower edge of its linear range (e.g. 72/1023), UNORM16 storage
rounded some of those just below the edge, and the truncating re-encode in the EDRAM dump
returned the byte minus one. Because the 10-bit fields straddle bytes, one LSB in byte 1 moved
the top bits of red from 68 to 835, which the combine decodes as 2^6: bright red. Xenia
decodes to the midpoint of the range, which survives the round trip; 0015 ports that.

Found by capturing the same cinematic shot in Xenia (profile copied into its content folder,
virtual pad, `trigger_capture.py`) and in our build (`REACH_RDCAPTURE_TRIGGER`), then
`resolves.py` (identical resolve sequences), `pick.py` on the combine draw's inputs at matching
foliage pixels (Xenia: small 7e3 values; ours: R field 835), `phist.py` to follow one pixel's
bytes back to the ownership transfer, and recomputing the expected bytes by hand.

Wrong turns worth knowing about: the combine pass writes garbage-red for every pixel that
does not hold 7e3 data in Xenia too; later material draws (blending off) overwrite it, so a
red combine output alone is not a bug. Xenia 82d0cd1 converts only auto-indexed tessellated
triangle strips/fans to lists; indexed ones still hit `default: return false` there, so the
dropped indexed strips are not a difference between the two.

## The black gameplay world (fixed by 0019)

In gameplay the 3D world was black while the HUD drew. A capture of such a frame showed the
tone-map pass reading an all-zero scene; bisecting the HDR scene target found an ownership
transfer (EID 11431) that copied a freshly created R10G10B10A2 target over it. Before that,
the resolve of tile 675 dumped its EDRAM from a *depth* buffer at base 1350: the SDK believed
the depth buffer owned the scene's tiles because a draw with clipping disabled was assumed to
cover all of EDRAM. Xenia estimates such draws' extent by running the vertex shader on the CPU
(`execute_unclipped_draw_vs_on_cpu`, default true; the SDK had it false). Its help text
describes exactly this corruption. Setting it also removed the cyan/teal cast on foliage.

## The missing Armory Spartan (fixed by 0021)

The Armory preview panel and the post-game screens (CREDITS EARNED, PROMOTED) were empty. The
Spartan itself renders fine: an off-screen 640x576 pass draws it into a G-buffer, lights it
into an 8888 target, resolves that to 0x0240E000, and the UI composites the texture with
premultiplied alpha (`src + dst * (1 - src.a)`), so its alpha channel is the model's coverage.
The material draws leave alpha at 0 on the model. Two full-screen rectangles end the pass: the
first writes alpha = 1 everywhere (alpha-only write mask, no depth test); the second, depth
tested, clears colour and alpha to 0 on the background. The first never rasterized, so the
model went out with alpha 0 and the composite drew nothing.

Its vertex shader (`shader_18355F1696162C19` with `--dump_shaders`) is one of Bungie's
screen-space rectangle shaders, but fetches the position as `xy0_` where the others use `xy1_`,
and exports `oPos = r1.xyzz`: W is 0. `PA_CL_VTE_CNTL` says W is not reciprocal and X/Y are not
divided, so the Xenos divides X and Y by 0 and, with clipping disabled, the rectangle covers the
whole target. A host GPU clips every primitive with W = 0. 0021 makes the vertex epilogue emit
W = 1 and X, Y = sign * 2^16 for such vertices (the first try, a tiny positive W, covered only
half the panel: the clipper's `1 + w` rounded to 1). Xenia 82d0cd1 has the same empty panel and
the same clipped draw.

Found with a RenderDoc capture of the helmet screen: `savetex.py` on the composite draw showed
the texture's RGB holding the lit Spartan and alpha 0 everywhere; saving the off-screen target
at each draw of the lighting pass found the draw that should set alpha, the pixel history listed
no fragments for it, and `postvs.py` showed W = 0.

Open GPU issues in the campaign (Winter Contingency):

- Faint cyan speckles on the near ground in the Falcon landing shot were seen before 0019;
  recheck (0019 removed the teal cast in gameplay).
- About 4 indexed, discrete-tessellated triangle strip draws per frame are dropped
  ("Unsupported tessellation mode 0 for primitive type 6"); Xenia 82d0cd1 drops indexed ones
  too (it converts auto-indexed strips/fans only, see `experimental/0014`).
- GPU hang (amdgpu `ring gfx timeout`, `VK_ERROR_DEVICE_LOST`) about 4 minutes into the
  campaign as the Falcons take off, and on Sword Base in Forge: the first adaptive-tessellation
  draw (`kTrianglePatch`, float32 edge factors, `VGT_HOS_MAX_TESS_LEVEL` 15; the water) hangs
  RADV. 0016 fixed the factor decoding and 0017 the domain-shader setup, but the hang stays;
  0018 skips these draws. **Why the game issues them at all (fixed by 0022):** in Xenia the
  game issues the water pair (a 996-point memexport that computes the edge factors, VS
  `E43930922B695042`, then the patch draw, VS `9ABC75772F0D46B1`) only in the first frames of
  Sword Base and never at the Falcon shots; in our build it issued them every frame. The
  difference was occlusion queries: the SDK disabled host queries at the first EVENT_WRITE_ZPD
  pair (begin and end reports at different addresses), so every query answered "1000
  samples". With real results (0022) the game stops issuing the water once its visibility
  query reports zero: no water draw in 6 minutes of Forge on Sword Base or through the campaign's Falcon takeoff and
  first gameplay, at 30 fps. A
  blocking first version (waiting for each query) cost a third of the frame rate (18.5 fps in
  Forge); guessing "visible" while waiting (Xenia's default "fast") let the water through
  every other frame, because Reach reads a result before the GPU has produced it. The hang
  itself is unsolved, so 0018 stays on for the frames where the game does issue the draw.
- The SDK's D3D12 backend needs the same midpoint decode (Xenia has it in
  `d3d12_render_target_cache.cc`) before Windows builds, and its DXBC translator the W = 0
  handling of 0021.

## Dot products: overflow is QNaN (0025)

Xenia's x64 backend (`DOT_PRODUCT_3`/`DOT_PRODUCT_4` in `x64_sequences.cc`, Canary
`82d0cd1`) multiplies and adds in float64, in the order (x + z) + y, or (x + z) + (y + w),
and rounds the sum to float32. If that float32 has an all-ones exponent while the float64 sum
is finite (finite inputs whose dot product overflows), the result is QNaN `0x7FC00000` in all
four lanes. An infinity or NaN that came from the inputs passes through.

The codegen keeps `dpps` and adds one branch on the exponent of its result (an `extractps`,
an `add` and a fused compare-and-jump, never taken in practice). Only a non-finite result
takes the cold path, which redoes the sum as Xenia does, so every non-finite case matches
Xenia: overflow gives QNaN, products that overflow float32 but cancel give Xenia's finite
float64 sum, and inputs with infinities or NaNs keep them. What remains different: a finite
`dpps` result can differ from the float64 sum in the last bit (Xenia keeps float64 because
it matches the console more often), and a sum within half an ulp of FLT_MAX could round to
FLT_MAX in float32 where float64 overflows. Accumulating every dot product in float64
cost 11% (vmsum3fp128) and 54% (vmsum4fp128) more latency than `dpps` plus the check in a
dependent chain on the Ryzen 7 7800X3D, at the same throughput, and would change the last
bit of ordinary results everywhere; it was left out.

The patch also adds cases to the SDK's PPC tests (`tests/ppc/asm/instr_vmsum3fp128.s`,
`instr_vmsum4fp128.s`): an overflow, an infinite input, and products that overflow but
cancel.

## Runtime fixes outside the patches

`librexruntime.so` is not rebuilt, so kernel and XAM fixes are `__imp__` overrides in
`reach-recomp/src/kernel/` (they forward to the SDK through `dlsym(RTLD_NEXT)` where they
only adjust it):

- **Critical sections** (`critical_section.cpp`): a solo Firefight game froze after four
  minutes. The main thread waited forever in `RtlEnterCriticalSection` on one of the
  game's named locks (table 0x8394DFD0, entry 14), which nobody held: lock count 0, no
  owner. The SDK, like Xenia, hands the lock over through the auto-reset event in the
  critical section's dispatcher header. The runtime binds that event to a host object by a
  handle it writes into the header itself (`wait_list_blink`). Here the same handle
  (0xF8000250) was also in a heap object at 0x30655018, so two guest objects shared one host
  event and the release's signal never reached the waiter. Contended waiters now block on a
  host semaphore keyed by the critical section's address; the guest-visible fields keep their
  meaning. Firefight then ran 10+ minutes several times without freezing.
  `REACH_SDK_CRITICAL_SECTIONS=1` restores the SDK's version.
- **Presentation** (`src/main.cpp`, `src/platform/vulkan_hooks.cpp`): the game sets the
  SDK's `vulkan_allow_present_mode_*` defaults to FIFO only (the SDK's IMMEDIATE tears) and
  wraps calls in the Vulkan device's function table to log swapchain present modes and time
  presents. On Linux the SDK repainted the window nonstop because its achievement
  notification dialog is always registered with the ImGui drawer;
  `src/platform/achievement_toast.cpp` registers it only while a toast shows. Not fixed: the
  presenter rebuilds its output pipeline on every paint (it never stores `swapchain_format`;
  Xenia has the same gap), now once per guest frame. [`docs/perf.md`](perf.md) has the
  details.
- `XamEnumerate` completion codes (`xam_enumerate.cpp`), headless `XamShowKeyboardUI`
  (`xam_keyboard.cpp`), files marked delete-on-close (`file_delete_on_close.cpp`), file size
  after writes (`file_size_refresh.cpp`), Winsock TCP (`live_tcp.cpp`) and the Xbox LIVE
  messages (`live_xmsg.cpp`): see the comments at the top of each file.
