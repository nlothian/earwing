// See meeting_demo.h.
#include "meeting_demo.h"

#include <stdio.h>
#include <string.h>

namespace meeting {

namespace {

struct DemoItem {
  Sec         sec;
  const char* text;
  const char* t;
  uint32_t    from;    // first step it appears in
  uint32_t    edited;  // step its wording changes in (0 = never), with text2
  const char* text2;
};

const DemoItem ITEMS[] = {
  {Sec::Key,       "Hardware costs are running about 10% over the Q3 budget", "01:12", 0, 0, nullptr},
  {Sec::Key,       "The new enclosure passed the drop test on the second revision", "02:40", 0, 0, nullptr},
  {Sec::Decisions, "Launch moves from 30 Sep to 14 Oct", "04:05", 1, 4, "Launch moves from 30 Sep to 21 Oct, to leave a week for certification"},
  {Sec::Actions,   "Alice: send the revised budget to Bob by Friday", "04:30", 1, 0, nullptr},
  {Sec::Questions, "Do we need a second supplier for the MEMS mics?", "05:10", 2, 0, nullptr},
  {Sec::Key,       "Beamforming now tracks a moving speaker within about 5 degrees, which is good enough for the meeting-room use case", "06:02", 2, 0, nullptr},
  {Sec::Actions,   "Sam: book the certification lab for the week of 7 Oct", "07:15", 3, 0, nullptr},
  {Sec::Decisions, "Ship the 4-mic array only; the 6-mic variant waits for v2", "08:20", 3, 0, nullptr},
  {Sec::Actions,   "Priya: draft the launch announcement and circulate it by Wednesday", "09:45", 4, 0, nullptr},
  {Sec::Questions, "Who owns the firmware update process after launch?", "10:30", 5, 0, nullptr},
  {Sec::Key,       "Battery life on the portable unit is 6 hours with WiFi on", "11:05", 5, 0, nullptr},
};

const char* const SUMMARIES[] = {
  "The team reviewed the hardware budget and the enclosure test results.",
  "The team reviewed the budget and agreed to move the launch by two weeks. Alice will send a revised budget.",
  "The launch moves to 14 Oct. Beamforming performance is now good enough for meeting rooms; the mic supply is an open question.",
  "The launch moves to 14 Oct with the 4-mic array only. Sam is booking the certification lab.",
  "Launch now 21 Oct to allow a week for certification. Priya is drafting the announcement.",
  "Launch on 21 Oct with the 4-mic array. Open questions remain on mic supply and post-launch firmware ownership.",
};

void put(char* dst, size_t cap, const char* src) {
  snprintf(dst, cap, "%s", src ? src : "");
}

}  // namespace

void demoFill(Snapshot* s, uint32_t step) {
  step %= DEMO_STEPS;
  const uint32_t g = s->gen, recv = s->recvMs;
  const bool first = s->firstOfSession;
  memset(s, 0, sizeof(*s));
  s->gen = g; s->recvMs = recv; s->firstOfSession = first;

  put(s->session, sizeof(s->session), "demo00-1");
  s->rev = step + 1;
  snprintf(s->coversTo, sizeof(s->coversTo), "%02u:%02u", (unsigned)(2 + step * 2), (unsigned)((step * 17) % 60));
  put(s->summary, sizeof(s->summary), SUMMARIES[step]);

  // Feed: newest change first. Build per step from newest to oldest.
  for (int st = (int)step; st >= 0 && s->feedCount < FEED_MAX; st--) {
    const uint8_t lvl = (uint8_t)((step - st) > 3 ? 3 : (step - st));
    const uint32_t ago = (step - st) * (DEMO_STEP_MS / 1000);
    for (const DemoItem& d : ITEMS) {
      if (s->feedCount >= FEED_MAX) break;
      const bool added = d.from == (uint32_t)st;
      const bool edited = d.edited && d.edited == (uint32_t)st;
      if (!added && !edited) continue;
      // An item edited later is listed once, at its newest change.
      if (added && d.edited && d.edited <= step) continue;
      FeedEntry& f = s->feed[s->feedCount++];
      put(f.text, sizeof(f.text), (d.edited && d.edited <= step) ? d.text2 : d.text);
      put(f.t, sizeof(f.t), d.t);
      f.sec = d.sec;
      f.kind = edited ? Kind::Edited : Kind::New;
      f.lvl = lvl;
      f.agoS = ago;
    }
  }
  if (s->feedCount < FEED_MAX) {
    FeedEntry& f = s->feed[s->feedCount++];
    put(f.text, sizeof(f.text), s->summary);
    put(f.t, sizeof(f.t), s->coversTo);
    f.sec = Sec::Summary;
    f.kind = Kind::Summary;
    f.lvl = 0;
    f.agoS = 0;
  }

  for (const DemoItem& d : ITEMS) {
    if (d.from > step) continue;
    const int l = (int)d.sec;
    if (s->count[l] >= ITEMS_MAX) continue;
    Item& it = s->items[l][s->count[l]++];
    const bool isEdited = d.edited && d.edited <= step;
    put(it.text, sizeof(it.text), isEdited ? d.text2 : d.text);
    put(it.t, sizeof(it.t), d.t);
    it.fresh = d.from == step || (d.edited && d.edited == step);
  }
}

}  // namespace meeting
