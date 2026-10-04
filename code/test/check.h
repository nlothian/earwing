// The host tests' shared harness: CHECK() counts and prints a failure and
// carries on; report() prints the summary and is main()'s exit code.
#pragma once

#include <stdio.h>

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

static int report(const char* name) {
  if (failures) {
    printf("%s: %d FAILED\n", name, failures);
    return 1;
  }
  printf("%s: all passed\n", name);
  return 0;
}
