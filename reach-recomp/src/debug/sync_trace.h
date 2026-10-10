#pragma once

// REACH_SYNCTRACE=FILE: a timeline of the guest's kernel waits and signals (sync_trace.cpp).

#include <rex/ppc/context.h>

#include <cstdint>

namespace reach::synctrace {

// Records a point event (no duration) such as a guest frame handed to VdSwap.
void Mark(PPCContext& ctx, uint8_t* base, const char* what);

}  // namespace reach::synctrace
