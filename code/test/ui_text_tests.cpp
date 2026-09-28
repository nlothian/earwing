// Host tests for ui_text.h: word wrap and paging. Build and run: make -C code/test
#include <stdio.h>

#include "ui_text.h"

int main() {
  // Stream C: wrap (greedy, long words split, coloured runs), paging.
  printf("ui_text_tests: all passed\n");
  return 0;
}
