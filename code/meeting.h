// The meeting store: POST /meeting from the site's assistant (the format and
// the accept policy are in meeting_types.h), kept in PSRAM for the Meeting
// tab. The handlers run in the web servers' tasks on core 0 (80 and 443
// concurrently); the UI reads from loop() on core 1 through copy(), so the
// two only ever meet under the store's lock.
#pragma once

#include "meeting_types.h"
#include "web_server.h"

namespace meeting {

void begin();                      // setup(), before web::startHttp()

void postHandler(web::Req& q);     // POST /meeting
void jsonHandler(web::Req& q);     // GET /meeting.json: the stored snapshot, for debugging

// Lock-free: the store's current generation (0 = nothing received yet).
uint32_t generation();
// Copy the store into *out if its generation isn't haveGen; true if it did.
bool copy(Snapshot* out, uint32_t haveGen);
// millis() of the last accepted post (replaced or touched), 0 = never.
uint32_t lastSeenMs();

}  // namespace meeting
