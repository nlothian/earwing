// See ui_text.h.
#include "ui_text.h"

namespace ui_text {

const char ELLIPSIS[] = "..";

namespace {

struct Wrapper {
  int maxW;
  WidthFn width;
  void* ctx;
  Line* out;
  int maxLines;
  int n = 0;           // lines finished
  bool cut = false;    // ran out of lines: stop
  int spaceW = 0;

  Line& cur() { return out[n]; }

  int runEnd(const Run& r) { return r.x + width(r.s, r.len, ctx); }

  // Put s[0..len) at the end of the current line, which the caller has
  // checked it fits. Joins the previous run when it's the same colour and
  // the source continues across exactly one space, so a line is usually one
  // run per segment rather than one per word.
  void append(const char* s, int len, uint16_t color, int x) {
    Line& l = cur();
    if (l.n > 0) {
      Run& r = l.run[l.n - 1];
      if (r.color == color && r.s + r.len + 1 == s && r.s[r.len] == ' ') {
        r.len = (uint16_t)(s + len - r.s);
        l.w = (int16_t)runEnd(r);
        return;
      }
    }
    Run& r = l.run[l.n++];
    r.s = s;
    r.len = (uint16_t)len;
    r.color = color;
    r.x = (int16_t)x;
    l.w = (int16_t)(x + width(s, len, ctx));
  }

  // Would s[0..len) (width ww) go on the current line? Where, in *x.
  bool fits(const char* s, int ww, uint16_t color, int* x) {
    const Line& l = cur();
    const int sep = l.n > 0 ? spaceW : 0;
    if (l.n > 0 && l.w + sep + ww > maxW) return false;
    if (l.n == 0 && ww > maxW) return false;
    if (l.n == MAX_RUNS) {   // only if it would merge into the last run
      const Run& r = l.run[l.n - 1];
      if (!(r.color == color && r.s + r.len + 1 == s && r.s[r.len] == ' ')) return false;
    }
    *x = l.n > 0 ? l.w + sep : 0;
    return true;
  }

  // Finish the current line and start the next; false (and the item cut
  // with "..") if that was the last one allowed.
  bool newLine() {
    if (n + 1 >= maxLines) {
      ellipsize();
      cut = true;
      return false;
    }
    n++;
    cur().n = 0;
    cur().w = 0;
    return true;
  }

  // The current (last allowed) line gets ".." at its end, shortened as needed.
  void ellipsize() {
    Line& l = cur();
    const int ew = width(ELLIPSIS, 2, ctx);
    uint16_t color = l.n > 0 ? l.run[l.n - 1].color : 0xFFFF;
    for (;;) {
      if (l.n == 0) { l.w = 0; break; }
      Run& r = l.run[l.n - 1];
      l.w = (int16_t)runEnd(r);
      if (l.n < MAX_RUNS && l.w + ew <= maxW) break;
      if (l.n == MAX_RUNS || r.len <= 1) { l.n--; continue; }   // no room for a 5th run / nothing to trim
      r.len--;
      while (r.len > 1 && r.s[r.len - 1] == ' ') r.len--;
    }
    Run& e = l.run[l.n++];
    e.s = ELLIPSIS;
    e.len = 2;
    e.color = color;
    e.x = l.w;
    l.w = (int16_t)(l.w + ew);
  }

  void word(const char* s, int len, uint16_t color) {
    while (len > 0 && !cut) {
      const int ww = width(s, len, ctx);
      int x;
      if (fits(s, ww, color, &x)) { append(s, len, color, x); return; }
      if (cur().n > 0) {                 // try again on a fresh line
        if (!newLine()) return;
        continue;
      }
      // Longer than a whole line: as much as fits (at least one character), then the rest.
      int k = 1;
      while (k < len && width(s, k + 1, ctx) <= maxW) k++;
      append(s, k, color, 0);
      s += k;
      len -= k;
      if (len > 0 && !newLine()) return;
    }
  }
};

}  // namespace

int wrap(const Seg* segs, int nSegs, int maxW, WidthFn width, void* ctx, Line* out, int maxLines,
         bool* truncated) {
  if (truncated) *truncated = false;
  if (maxLines <= 0) return 0;
  Wrapper w;
  w.maxW = maxW;
  w.width = width;
  w.ctx = ctx;
  w.out = out;
  w.maxLines = maxLines;
  w.spaceW = width(" ", 1, ctx);
  out[0].n = 0;
  out[0].w = 0;
  for (int i = 0; i < nSegs && !w.cut; i++) {
    const char* p = segs[i].s;
    if (!p) continue;
    while (*p && !w.cut) {
      while (*p == ' ') p++;
      const char* q = p;
      while (*q && *q != ' ') q++;
      if (q > p) w.word(p, (int)(q - p), segs[i].color);
      p = q;
    }
  }
  if (truncated) *truncated = w.cut;
  return w.out[w.n].n > 0 ? w.n + 1 : w.n;
}

int paginate(const uint8_t* rows, int nItems, int rowsPerPage, uint8_t* pageStart, int maxPages) {
  int pages = 0, used = 0;
  for (int i = 0; i < nItems; i++) {
    int r = rows[i];
    if (r < 1) r = 1;
    if (r > rowsPerPage) r = rowsPerPage;
    if ((pages == 0 || used + r > rowsPerPage) && pages < maxPages) {
      pageStart[pages++] = (uint8_t)i;
      used = 0;
    }
    used += r;
  }
  return pages;
}

}  // namespace ui_text
