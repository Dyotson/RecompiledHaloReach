// reach - the tick rate network peers have to share (src/hooks/frame_rate_net.cpp).

#pragma once

#include <cstdint>

namespace reach {

// The tick rate this machine simulates: round(guest refresh rate) / 2, so 30 at the
// console's 60 Hz and 60 with frame_rate = 60 (docs/framerate.md).
int TickRate();

// The matchmaking search key for a playlist: the playlist id, with the tick rate in the top
// byte when it is not 30, so the Reach Live server only matches players at the same rate.
// The playlist id itself with REACH_MIXED_FRAME_RATE=1.
uint32_t MatchmakingKey(uint32_t playlist);

}  // namespace reach
