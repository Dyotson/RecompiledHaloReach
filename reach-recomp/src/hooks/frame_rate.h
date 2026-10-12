// reach - tick rates (src/hooks/frame_rate.cpp, src/hooks/frame_rate_net.cpp).

#pragma once

#include <cstdint>

namespace reach {

// The tick rate this machine simulates: round(guest refresh rate) / 2, so 30 at the
// console's 60 Hz and 60 with frame_rate = 60 (docs/framerate.md).
int TickRate();

// Whether synchronous network games (co-op) run at 30 ticks a second whatever the frame rate
// (the default; REACH_SYNC_TICKS30=0 turns it off), so co-op does not need the same frame rate
// on every machine.
bool SyncGamesAt30();

// Games started so far (game time initializations, sub_8247B530): the main menu is the first.
int GamesStarted();

// The matchmaking search key for a playlist: the playlist id, with the tick rate in the top
// byte when co-op runs at the display's rate (REACH_SYNC_TICKS30=0) and that is not 30, so the
// Reach Live server only matches players at the same rate. The playlist id itself otherwise.
uint32_t MatchmakingKey(uint32_t playlist);

}  // namespace reach
