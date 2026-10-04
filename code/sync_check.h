// Alignment self-test analysis for the two I2S controllers. Portable so the
// host tests can feed it synthetic slot streams.
//
// During the test both controllers read the SAME data line (GPIO14 is routed
// into I2S1 as well), so their raw 32-bit slot streams must be identical
// apart from where each one started. Comparing them word for word gives the
// exact offset between the controllers, in slots (half frames), and shows a
// bit-level shift if the slave samples a BCLK late.
#pragma once

#include <stdint.h>

namespace sync_check {

enum class Status : uint8_t {
  NotRun,
  Ok,             // streams identical at slotOffset
  Inconclusive,   // not enough signal to tell (mics unwired, all zeros)
  NoMatch,        // no offset within range makes them identical
  BitShift,       // identical only after shifting words by bitShift bits
};
const char* statusName(Status s);
// The streams disagree: the clocks or the wiring, not just too little signal.
inline bool misread(Status s) { return s == Status::NoMatch || s == Status::BitShift; }

struct Result {
  Status status = Status::NotRun;
  int slotOffset = 0;   // slave[i] == master[i + slotOffset]
  int bitShift = 0;     // BitShift: slave word == master word << bitShift (neg: >>)
  int compared = 0;     // slots compared at the reported offset
  int dropMaster = 0;   // slots to discard from each stream to line them up;
  int dropSlave = 0;    // dropMaster is even so master frames keep L/R order
};

// master/slave: n raw slots each, read in lockstep (buffer k from each).
// Searches offsets in [−maxOffset, +maxOffset] and needs at least n/2
// overlapping slots to call a match.
Result analyze(const int32_t* master, const int32_t* slave, int n, int maxOffset);

// Drops that realign a measured offset while keeping the master's frame
// boundaries: dropMaster − dropSlave == offset, dropMaster even, both >= 0.
void dropsFor(int slotOffset, int* dropMaster, int* dropSlave);

}  // namespace sync_check
