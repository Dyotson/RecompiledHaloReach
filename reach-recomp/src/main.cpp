// reach - ReXGlue Recompiled Project

#include "generated/default/reach_init.h"

#include "reach_app.h"

#include <rex/cvar.h>
#include <rex/logging.h>

#include <string_view>

REX_DEFINE_APP(reach, ReachApp::Create)

REXCVAR_DEFINE_STRING(gpu_backend, "vulkan", "GPU",
                      "GPU backend on Windows: vulkan (with this project's fixes) or d3d12")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

std::string ReachGpuBackend() { return REXCVAR_GET(gpu_backend); }

// Reach draws a frame every 2 guest vblanks and schedules its render thread in vblanks, so
// the frame rate follows the guest's vblank rate (docs/framerate.md).
REXCVAR_DEFINE_INT32(frame_rate, 30, "Display",
                     "Frame rate: 30 (the console's) or 60. 60 runs the guest vblank at 120 Hz; "
                     "the game ticks at half the refresh rate, so it also simulates 60 ticks "
                     "a second (docs/framerate.md)")
    .validator([](std::string_view v) { return v == "30" || v == "60"; })
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

namespace {

// Gives an SDK cvar this game's default. A value from reach.toml, the environment
// (REX_<NAME>) or the command line stays; the settings UI and SaveConfig treat the new
// value as the default.
void SetSdkCvarDefault(std::string_view name, std::string_view value) {
  for (rex::cvar::FlagEntry& entry : rex::cvar::GetRegistry()) {
    if (entry.name != name) continue;
    if (entry.source == rex::cvar::Source::kDefault) entry.setter(value);
    entry.default_value = value;
    return;
  }
  REXLOG_WARN("SDK setting {} not found", name);
}

}  // namespace

void ApplyReachCvarDefaults() {
  // Present with FIFO (vsync) only. The SDK prefers IMMEDIATE, which lets a fullscreen game
  // tear (KDE and other Wayland compositors allow it): on the slowly panning menus the tear
  // line reads as a shimmer crawling down the screen. FIFO_RELAXED tears at 30 fps too,
  // since every frame arrives after the vblank it was due for. MAILBOX does not tear, but
  // nothing paces the SDK's UI-thread repaints on Linux except a vsync wait: while an ImGui
  // dialog was always registered, that was thousands of presents a second
  // (src/platform/achievement_toast.h). FIFO keeps a cap on it, at up to one refresh of
  // extra latency. reach.toml can allow the others again; docs/perf.md has the measurements.
  SetSdkCvarDefault("vulkan_allow_present_mode_immediate", "false");
  SetSdkCvarDefault("vulkan_allow_present_mode_mailbox", "false");
  SetSdkCvarDefault("vulkan_allow_present_mode_fifo_relaxed", "false");
  // frame_rate = 60: twice the console's vblank rate. The game's frame limiter (2 vblanks),
  // its render thread's schedule (a frame is shown 4 vblanks after its update starts), its
  // frame time (vblanks / refresh rate) and its tick rate (refresh rate / 2) all scale with
  // it; the intro video keeps its speed. A video_mode_refresh_rate set in reach.toml or on
  // the command line wins. src/hooks/frame_rate.cpp keeps cinematics working at 60 ticks.
  if (REXCVAR_GET(frame_rate) == 60) SetSdkCvarDefault("video_mode_refresh_rate", "120");
}
