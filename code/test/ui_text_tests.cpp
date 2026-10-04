// Host tests for ui_text.h: word wrap and paging. Build and run: make -C code/test
//
// Widths come from a table rather than a font: every character 6 px except
// ' ' 3, '.' 2, 'i' 2 and 'W' 10, so the expected breaks can be worked out
// by hand.
#include <stdio.h>
#include <string.h>

#include <string>

#include "check.h"
#include "ui_text.h"

using namespace ui_text;

static int charW(char c) {
  switch (c) {
    case ' ': return 3;
    case '.': return 2;
    case 'i': return 2;
    case 'W': return 10;
    default:  return 6;
  }
}

static int calls = 0;
static int tableWidth(const char* s, size_t n, void*) {
  calls++;
  int w = 0;
  for (size_t i = 0; i < n; i++) w += charW(s[i]);
  return w;
}
static int strW(const std::string& s) { return tableWidth(s.data(), s.size(), nullptr); }

// A line as text, runs joined by '|' (so the run boundaries show).
static std::string lineText(const Line& l) {
  std::string out;
  for (int i = 0; i < l.n; i++) {
    if (i) out += '|';
    out.append(l.run[i].s, l.run[i].len);
  }
  return out;
}

static const uint16_t WHITE = 0xFFFF, GREY = 0x8410, RED = 0xF800;

static int wrap1(const char* text, int maxW, Line* out, int maxLines, bool* cut = nullptr) {
  Seg s{text, WHITE};
  return wrap(&s, 1, maxW, tableWidth, nullptr, out, maxLines, cut);
}

// Every line within maxW, its w the last run's end, runs left to right.
static void checkLines(const Line* l, int n, int maxW, const char* what) {
  for (int i = 0; i < n; i++) {
    CHECK(l[i].n >= 1 && l[i].n <= MAX_RUNS, "%s line %d: %d runs", what, i, l[i].n);
    CHECK(l[i].w <= maxW, "%s line %d: width %d > %d", what, i, l[i].w, maxW);
    const Run& last = l[i].run[l[i].n - 1];
    CHECK(l[i].w == last.x + tableWidth(last.s, last.len, nullptr), "%s line %d: w %d", what, i, l[i].w);
    for (int r = 1; r < l[i].n; r++) {
      const Run& p = l[i].run[r - 1];
      CHECK(l[i].run[r].x >= p.x + tableWidth(p.s, p.len, nullptr), "%s line %d run %d overlaps", what, i, r);
    }
  }
}

static void testGreedy() {
  // "aaa bbb ccc ddd": words 18 px, a space 3. At 40 px two words fit
  // (18+3+18 = 39), a third doesn't.
  Line l[9];
  int n = wrap1("aaa bbb ccc ddd", 40, l, 9);
  CHECK(n == 2, "greedy: %d lines", n);
  CHECK(lineText(l[0]) == "aaa bbb", "greedy line 0: '%s'", lineText(l[0]).c_str());
  CHECK(lineText(l[1]) == "ccc ddd", "greedy line 1: '%s'", lineText(l[1]).c_str());
  CHECK(l[0].w == 39, "greedy w %d", l[0].w);
  checkLines(l, n, 40, "greedy");

  // Exactly the width fits; one pixel less doesn't.
  n = wrap1("aaa bbb", 39, l, 9);
  CHECK(n == 1, "exact fit: %d lines", n);
  n = wrap1("aaa bbb", 38, l, 9);
  CHECK(n == 2, "one short: %d lines", n);

  // Extra spaces, leading and trailing, don't make empty runs or lines, and
  // a double space isn't merged into a run (it would draw wider than measured).
  n = wrap1("  aaa  bbb   ", 100, l, 9);
  CHECK(n == 1, "spaces: %d lines", n);
  CHECK(lineText(l[0]) == "aaa|bbb", "spaces: '%s'", lineText(l[0]).c_str());
  CHECK(l[0].run[1].x == 18 + 3, "spaces: second word at %d", l[0].run[1].x);
  checkLines(l, n, 100, "spaces");

  // Nothing to draw.
  CHECK(wrap1("", 100, l, 9) == 0, "empty string");
  CHECK(wrap1("    ", 100, l, 9) == 0, "only spaces");
  CHECK(wrap(nullptr, 0, 100, tableWidth, nullptr, l, 9) == 0, "no segments");

  // Proportional: "Wi" is 12, "WWW" 30.
  n = wrap1("Wi WWW Wi", 45, l, 9);
  CHECK(n == 2 && l[0].w == 45, "proportional: %d lines, w %d", n, l[0].w);
  CHECK(lineText(l[0]) == "Wi WWW", "proportional line 0: '%s'", lineText(l[0]).c_str());
}

static void testLongWord() {
  // A word wider than the line is split: 10 chars of 6 px at 40 px is 6 + 4.
  Line l[9];
  int n = wrap1("abcdefghij", 40, l, 9);
  CHECK(n == 2, "long word: %d lines", n);
  CHECK(lineText(l[0]) == "abcdef" && lineText(l[1]) == "ghij", "long word: '%s' '%s'",
        lineText(l[0]).c_str(), lineText(l[1]).c_str());
  checkLines(l, n, 40, "long word");

  // It starts on a fresh line when there's already something on this one,
  // and the next word follows its tail.
  n = wrap1("ab abcdefghijklmnop xy", 40, l, 9);
  CHECK(n == 4, "long word mid-item: %d lines", n);
  CHECK(lineText(l[0]) == "ab", "mid 0 '%s'", lineText(l[0]).c_str());
  CHECK(lineText(l[1]) == "abcdef", "mid 1 '%s'", lineText(l[1]).c_str());
  CHECK(lineText(l[2]) == "ghijklm", "mid 2 '%s'", lineText(l[2]).c_str());   // 'i' is narrow: 38 px
  CHECK(lineText(l[3]) == "nop xy", "mid 3 '%s'", lineText(l[3]).c_str());
  checkLines(l, n, 40, "long word mid-item");

  // A line narrower than one character still makes progress (one per line).
  n = wrap1("abc", 4, l, 9);
  CHECK(n == 3, "tiny width: %d lines", n);
}

static void testRuns() {
  // An item then its citation in grey: two runs, the grey one a space after.
  Seg segs[] = {{"Launch moves to 14 Oct", WHITE}, {"09:40", GREY}};
  Line l[9];
  int n = wrap(segs, 2, 300, tableWidth, nullptr, l, 9);
  CHECK(n == 1, "runs: %d lines", n);
  CHECK(l[0].n == 2, "runs: %d runs", l[0].n);
  CHECK(lineText(l[0]) == "Launch moves to 14 Oct|09:40", "runs: '%s'", lineText(l[0]).c_str());
  CHECK(l[0].run[0].color == WHITE && l[0].run[1].color == GREY, "runs: colours");
  CHECK(l[0].run[1].x == strW("Launch moves to 14 Oct") + 3, "runs: grey at %d", l[0].run[1].x);
  checkLines(l, n, 300, "runs");

  // The citation wraps as a word like any other.
  const int w = strW("Launch moves to 14 Oct");
  n = wrap(segs, 2, w + 10, tableWidth, nullptr, l, 9);
  CHECK(n == 2, "runs wrap: %d lines", n);
  CHECK(lineText(l[1]) == "09:40" && l[1].run[0].color == GREY && l[1].run[0].x == 0, "runs wrap: '%s'",
        lineText(l[1]).c_str());

  // Same colour in two segments stays two runs (the source isn't contiguous).
  Seg same[] = {{"aa", WHITE}, {"bb", WHITE}};
  n = wrap(same, 2, 300, tableWidth, nullptr, l, 9);
  CHECK(n == 1 && l[0].n == 2 && l[0].run[1].x == 12 + 3, "same colour segs: %d runs", l[0].n);

  // No more than MAX_RUNS on a line: the fifth colour change breaks it.
  Seg many[] = {{"a", WHITE}, {"b", GREY}, {"c", RED}, {"d", WHITE}, {"e", GREY}, {"f", RED}};
  n = wrap(many, 6, 300, tableWidth, nullptr, l, 9);
  CHECK(n == 2, "max runs: %d lines", n);
  CHECK(l[0].n == MAX_RUNS && l[1].n == 2, "max runs: %d + %d", l[0].n, l[1].n);
  CHECK(lineText(l[1]) == "e|f" && l[1].run[0].x == 0, "max runs line 1: '%s'", lineText(l[1]).c_str());
  checkLines(l, n, 300, "max runs");
}

static void testTruncate() {
  // Twelve 3-letter words, two per 40 px line, is 6 lines; allow 3.
  const char* text = "w01 w02 w03 w04 w05 w06 w07 w08 w09 w10 w11 w12";
  Line l[9];
  bool cut = false;
  int n = wrap1(text, 40, l, 9, &cut);
  CHECK(n == 6 && !cut, "fits in 9: %d lines, cut %d", n, cut);
  n = wrap1(text, 40, l, 6, &cut);
  CHECK(n == 6 && !cut, "fits in exactly 6: %d lines, cut %d", n, cut);

  n = wrap1(text, 40, l, 3, &cut);
  CHECK(n == 3 && cut, "cut to 3: %d lines, cut %d", n, cut);
  CHECK(lineText(l[0]) == "w01 w02" && lineText(l[1]) == "w03 w04", "cut: first lines");
  // "w05 w06" is 39 px; ".." (4) doesn't fit after it, so it's trimmed to
  // "w05 w0" (33) + ".." = 37.
  CHECK(lineText(l[2]) == "w05 w0|..", "cut last line: '%s'", lineText(l[2]).c_str());
  CHECK(l[2].run[1].s == ELLIPSIS && l[2].run[1].x == 33, "cut: ellipsis at %d", l[2].run[1].x);
  checkLines(l, n, 40, "cut");

  // Room for ".." without trimming.
  n = wrap1("aa bb cc dd ee ff", 31, l, 2, &cut);   // "cc dd" 27 + ".." 4
  CHECK(n == 2 && cut, "cut with room: %d, %d", n, cut);
  CHECK(lineText(l[1]) == "cc dd|..", "cut with room: '%s'", lineText(l[1]).c_str());

  // A trim that reaches a space drops it too ("w05 " + ".." would look odd).
  n = wrap1("w01 w02 w03 w04 w05 w06", 33, l, 1, &cut);
  CHECK(cut && lineText(l[0]) == "w01|..", "trim past space: '%s'", lineText(l[0]).c_str());

  // One line allowed, one long word: split, then cut.
  n = wrap1("abcdefghijklmnop", 40, l, 1, &cut);
  CHECK(n == 1 && cut && lineText(l[0]) == "abcdef|..", "long word cut: '%s'", lineText(l[0]).c_str());
  checkLines(l, n, 40, "long word cut");

  // A full line of MAX_RUNS runs has no room for a fifth: the last run goes.
  Seg many[] = {{"a", WHITE}, {"b", GREY}, {"c", RED}, {"d", WHITE}, {"e", GREY}};
  n = wrap(many, 5, 300, tableWidth, nullptr, l, 1, &cut);
  CHECK(n == 1 && cut && l[0].n == MAX_RUNS, "cut max runs: %d runs", l[0].n);
  CHECK(lineText(l[0]) == "a|b|c|..", "cut max runs: '%s'", lineText(l[0]).c_str());

  // maxLines 0 is nothing.
  CHECK(wrap1("abc", 40, l, 0, &cut) == 0, "maxLines 0");
}

static void testPaginate() {
  uint8_t start[12];
  // 9 rows a page: 3+3+3 | 4+4 | 9 | 1
  const uint8_t rows[] = {3, 3, 3, 4, 4, 9, 1};
  int p = paginate(rows, 7, 9, start, 12);
  CHECK(p == 4, "pages: %d", p);
  CHECK(start[0] == 0 && start[1] == 3 && start[2] == 5 && start[3] == 6, "page starts %d %d %d %d", start[0],
        start[1], start[2], start[3]);

  // An item never splits: 8 then 2 is two pages, not 9 + 1.
  const uint8_t r2[] = {8, 2};
  p = paginate(r2, 2, 9, start, 12);
  CHECK(p == 2 && start[1] == 1, "no split: %d pages", p);

  // Taller than a page counts as a full page (wrap() cut it to fit).
  const uint8_t r3[] = {20, 1, 0};
  p = paginate(r3, 3, 9, start, 12);
  CHECK(p == 2 && start[1] == 1, "tall item: %d pages", p);

  // None.
  CHECK(paginate(rows, 0, 9, start, 12) == 0, "no items");

  // Beyond maxPages, the rest share the last page.
  const uint8_t r4[] = {9, 9, 9, 9};
  p = paginate(r4, 4, 9, start, 2);
  CHECK(p == 2 && start[0] == 0 && start[1] == 1, "max pages: %d", p);

  // Wrap and paginate together: 12 items of 2 lines at 9 rows is 4 per page, 3 pages.
  Line l[9];
  uint8_t rs[12];
  for (int i = 0; i < 12; i++) rs[i] = (uint8_t)wrap1("aaa bbb ccc", 40, l, 9);
  p = paginate(rs, 12, 9, start, 12);
  CHECK(rs[0] == 2 && p == 3 && start[1] == 4 && start[2] == 8, "wrap+paginate: %d rows, %d pages", rs[0], p);
}

int main() {
  testGreedy();
  testLongWord();
  testRuns();
  testTruncate();
  testPaginate();
  return report("ui_text_tests");
}
