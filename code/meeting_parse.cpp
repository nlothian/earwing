// See meeting_types.h: the POST body parser (cJSON, from ESP-IDF's json
// component on the device and its cJSON.c in the host tests) and the accept
// policy. Pure C++, no Arduino. STUB (contract commit).
#include "meeting_types.h"

namespace meeting {

const char* resultName(Result r) {
  switch (r) {
    case Result::Replaced: return "replaced";
    case Result::Touched:  return "touched";
    case Result::Ignored:  return "ignored";
    case Result::Old:      return "old";
  }
  return "";
}

bool parse(const char*, size_t, Parsed*, const char** err) {
  if (err) *err = "not implemented";
  return false;
}

void sanitize(char*) {}

Result decide(const Snapshot&, uint32_t, const Parsed&, uint32_t) { return Result::Ignored; }

}  // namespace meeting
