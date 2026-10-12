// reach - frame-time statistics (frame rate and stutter), off by default.
//
// RECOMP_PERF=1 (or the older REACH_FPSLOG=1) times two streams: guest frames (VdSwap) and
// host presents (vkQueuePresentKHR, hooked in src/platform/vulkan_hooks.cpp), and logs one
// line per stream every 10 s and a summary at exit:
//
//   PERF: scene=<name> src=<guest|present> t=<s> fps=<avg> p50=<ms> p95=<ms> p99=<ms>
//         max=<ms> low1=<fps> stutters=<n> hitches=<n> ontarget=<%> pipelines=<n>
//
// The summary line ends with "final=1 from=<s>" (it covers the frames since <s>).
// RECOMP_PERF_SCENE=<name> labels the lines, RECOMP_PERF_CSV=<path> writes every frame time
// (scene,src,t,ms), and RECOMP_PERF_RESET=<path> restarts the summary when that file appears
// (it is deleted again), so a script can measure from the moment a scene is reached.
// Definitions and results: docs/perf.md.

#pragma once

#include <cstdint>

namespace reach::perf {

bool Enabled();

// A guest frame was handed to VdSwap.
void OnGuestFrame();

// Guest frames handed to VdSwap so far (counted also without RECOMP_PERF), to line up
// frame dumps with main-thread records (sync_diff.cpp, REACH_CINEREC).
uint32_t GuestFrameCount();

// The host presenter called vkQueuePresentKHR.
void OnHostPresent();

// The GPU plugin created `count` graphics or compute pipelines (guest shaders and
// render-target helpers; src/platform/vulkan_hooks.cpp).
void OnPipelinesCreated(uint32_t count);

// Logs the summary lines and closes the CSV (once; the game exits on a window close).
void Shutdown();

}  // namespace reach::perf
