// See meeting.h. STUB (contract commit): the endpoint answers 503 until the
// store lands. With -DMEETING_DEMO the store is the canned demo sequence
// (meeting_demo.h), a new step every DEMO_STEP_MS, so the UI can be built
// and tried without a browser. The real implementation must keep that.
#include "meeting.h"

#include <Arduino.h>

#include "meeting_demo.h"

namespace meeting {

void begin() {}

void postHandler(web::Req& q) { q.send(503, "text/plain", "meeting store not implemented yet\n"); }

void jsonHandler(web::Req& q) { q.send(200, "application/json", "{\"gen\":0}\n"); }

#ifdef MEETING_DEMO
uint32_t generation() { return 1 + millis() / DEMO_STEP_MS; }

bool copy(Snapshot* out, uint32_t haveGen) {
  const uint32_t g = generation();
  if (g == haveGen) return false;
  out->gen = g;
  out->recvMs = millis();
  out->firstOfSession = ((g - 1) % DEMO_STEPS) == 0;
  demoFill(out, g - 1);
  return true;
}

uint32_t lastSeenMs() { return millis() - (millis() % DEMO_STEP_MS); }
#else
uint32_t generation() { return 0; }
bool copy(Snapshot*, uint32_t) { return false; }
uint32_t lastSeenMs() { return 0; }
#endif

}  // namespace meeting
