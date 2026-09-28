// Word wrap and paging for the Meeting tab (ui_meeting.cpp). Pure C++, no
// TFT: text widths come from a callback (tft.textWidth on the device, a
// table in the host tests: make -C code/test).
//
// wrap() lays one item (a bullet, a feed entry) out as lines of up to
// MAX_RUNS coloured runs: greedy, one space between words, a word longer
// than a line split across lines, and an item that would need more than
// maxLines cut at the last line with "..". The runs point into the caller's
// strings (nothing is copied), so a layout is only good while they are.
// paginate() then packs whole items onto pages: an item is never split
// across two pages.
#pragma once

#include <stddef.h>
#include <stdint.h>

namespace ui_text {

using WidthFn = int (*)(const char* s, size_t n, void* ctx);   // pixel width of s[0..n)

static const int MAX_RUNS = 4;   // colour runs per line

// One piece of an item's text in one colour, e.g. the item then its "[t]".
// Consecutive segments are joined by a space.
struct Seg {
  const char* s;
  uint16_t    color;
};

// Draw s[0..len) in `color` at the line's x + x.
struct Run {
  const char* s;
  uint16_t    len;
  uint16_t    color;
  int16_t     x;
};

struct Line {
  uint8_t n;               // runs used
  int16_t w;               // the line's width: the last run's x + width
  Run     run[MAX_RUNS];
};

extern const char ELLIPSIS[];   // "..", what a cut item ends with

// Wrap segs into out[0..maxLines) at most maxW px wide. Returns the lines
// used (0 for no words at all). *truncated, if given, says whether the item
// was cut to fit maxLines (its last line then ends in ELLIPSIS).
int wrap(const Seg* segs, int nSegs, int maxW, WidthFn width, void* ctx, Line* out, int maxLines,
         bool* truncated = nullptr);

// Pages for items of rows[i] lines each (clamped to 1..rowsPerPage) packed
// in order, no item split: pageStart[p] is page p's first item. Returns the
// page count (0 for no items); items beyond maxPages pages share the last.
int paginate(const uint8_t* rows, int nItems, int rowsPerPage, uint8_t* pageStart, int maxPages);

}  // namespace ui_text
