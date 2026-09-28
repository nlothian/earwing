// Canned meeting snapshots for building the Meeting tab without a browser:
// build with -DMEETING_DEMO (PLATFORMIO_BUILD_FLAGS=-DMEETING_DEMO pio run)
// and meeting.cpp serves these instead of what's been POSTed, a new step
// every DEMO_STEP_MS. Pure C++, so the host tests can use them too.
#pragma once

#include "meeting_types.h"

namespace meeting {

static const uint32_t DEMO_STEP_MS = 20000;
static const uint32_t DEMO_STEPS   = 6;     // then it wraps to step 0 (a new session)

// Fill *s as the summary would stand after `step` updates (step % DEMO_STEPS).
// Leaves gen, recvMs and firstOfSession for the caller.
void demoFill(Snapshot* s, uint32_t step);

}  // namespace meeting
