// Host tests for the meeting store's pure parts (meeting_types.h): the POST
// body parser, sanitize() and the accept policy, plus the demo snapshots.
// Build and run: make -C code/test
#include <stdio.h>
#include <string.h>

#include "meeting_demo.h"
#include "meeting_types.h"

using namespace meeting;

static int failures = 0;
#define CHECK(cond, ...)                                  \
  do {                                                    \
    if (!(cond)) {                                        \
      failures++;                                         \
      printf("  FAIL %s:%d: ", __FILE__, __LINE__);       \
      printf(__VA_ARGS__);                                \
      printf("\n");                                       \
    }                                                     \
  } while (0)

static Snapshot snap;

static void testDemo() {
  for (uint32_t step = 0; step < DEMO_STEPS; step++) {
    memset(&snap, 0, sizeof(snap));
    demoFill(&snap, step);
    CHECK(snap.rev == step + 1, "demo rev %u", (unsigned)snap.rev);
    CHECK(snap.feedCount > 0 && snap.feedCount <= FEED_MAX, "demo feedCount %u", snap.feedCount);
    CHECK(snap.feed[0].lvl == 0, "demo newest entry lvl %u", snap.feed[0].lvl);
  }
}

int main() {
  testDemo();
  // Stream B: parse() on site/scripts/fixtures/meeting-sample.json, truncation,
  // sanitize() (UTF-8 -> '?'), the full decide() table.
  if (failures) {
    printf("meeting_tests: %d FAILED\n", failures);
    return 1;
  }
  printf("meeting_tests: all passed\n");
  return 0;
}
