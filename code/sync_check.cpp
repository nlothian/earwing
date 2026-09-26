#include "sync_check.h"

namespace sync_check {

const char* statusName(Status s) {
  switch (s) {
    case Status::NotRun:       return "not run";
    case Status::Ok:           return "ok";
    case Status::Inconclusive: return "inconclusive";
    case Status::NoMatch:      return "NO MATCH";
    case Status::BitShift:     return "BIT SHIFT";
  }
  return "?";
}

void dropsFor(int s, int* dm, int* ds) {
  // Time-aligned means slave[i + ds] == master[i + dm], i.e. dm − ds = s.
  // Rounding dm up to even keeps each master frame's L slot first; the slave
  // then gets the same L/R boundaries because WS is shared.
  int m = s > 0 ? s : 0;
  if (m & 1) m++;
  *dm = m;
  *ds = m - s;
}

// Enough signal to make a match meaningful: most words non-zero and the
// stream actually changing. Digital silence matches at every offset.
static bool active(const int32_t* w, int n) {
  int nonzero = 0, changes = 0;
  for (int i = 0; i < n; i++) {
    if (w[i] & ~0xFF) nonzero++;
    if (i && w[i] != w[i - 1]) changes++;
  }
  return nonzero >= n / 4 && changes >= n / 8;
}

// Compare at slot offset s under a word transform; returns slots compared, or
// −1 at the first mismatch.
static int compareAt(const int32_t* m, const int32_t* sl, int n, int s, int shift) {
  int count = 0;
  for (int i = 0; i < n; i++) {
    const int k = i + s;
    if (k < 0 || k >= n) continue;
    uint32_t a = (uint32_t)m[k], b = (uint32_t)sl[i];
    if (shift > 0) {          // slave late: its word is master << shift, new low bits unknown
      a <<= shift;
      b &= ~((1u << shift) - 1);
    } else if (shift < 0) {   // slave early: master >> shift, new high bits unknown
      a >>= -shift;
      b &= 0xFFFFFFFFu >> -shift;
    }
    if (a != b) return -1;
    count++;
  }
  return count;
}

Result analyze(const int32_t* m, const int32_t* sl, int n, int maxOffset) {
  Result r;
  if (!active(m, n) || !active(sl, n)) { r.status = Status::Inconclusive; return r; }
  static const int shifts[] = {0, 1, -1, 2, -2};
  for (int shift : shifts) {
    // Nearest offsets first: 0, +1, −1, +2, ...
    for (int d = 0; d <= maxOffset; d++) {
      for (int sgn = 0; sgn < (d ? 2 : 1); sgn++) {
        const int s = sgn ? -d : d;
        const int c = compareAt(m, sl, n, s, shift);
        if (c >= n / 2) {
          r.status = shift ? Status::BitShift : Status::Ok;
          r.slotOffset = s;
          r.bitShift = shift;
          r.compared = c;
          dropsFor(s, &r.dropMaster, &r.dropSlave);
          return r;
        }
      }
    }
  }
  r.status = Status::NoMatch;
  return r;
}

}  // namespace sync_check
