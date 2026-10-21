// See ui_meeting.h.
#include "ui_meeting.h"

#include <Arduino.h>
#include <TFT_eSPI.h>
#include <esp_heap_caps.h>
#include <string.h>

#include "audio_server.h"
#include "board_io.h"
#include "meeting.h"
#include "ui_text.h"
#ifdef UI_DEBUG
#include <freertos/semphr.h>
#include "web_server.h"
#endif

namespace ui_meeting {

namespace {

using meeting::Sec;
using meeting::Snapshot;
using ui_text::Line;
using ui_text::Seg;

// ---------- geometry ----------
using board_io::SCREEN_W; using board_io::SCREEN_H;   // the viewport right of the blank strip
const int FONT = 2, FONT_H = 16;
const int TABS = 3, TAB_W = 66;                // MEETING x 0-65, DEVICE x 66-131, SETTINGS x 132-197
const int TAB_LINE = 3;                        // the active tab's underline
const int FRESH_X = TABS * TAB_W;              // status: x 198-308 (text from 210), not tappable
const int FRESH_TEXT_R = 289, FRESH_DOT_X = 299, FRESH_DOT_R = 5;
// Taps on the strip: taller than it (the calibration is weak along the top
// edge); nothing below it on any tab takes taps above y 36.
const int TAP_STRIP_Y = 36, TAP_STRIP_X = FRESH_X;
const char* const TAB_LABEL[TABS] = {"MEETING", "DEVICE", "SETTINGS"};
const int HDR_Y = 32;                          // y 32-48
const int BODY_Y = 50, ROW_H = 17, ROWS = 9;   // rows at 50 + 17k, the last ending at 202
const int BTN_Y = 206, BTN_H = 34, BTN_DX = 62, BTN_W = 60, BTN_R = 4;
const int TAP_BTN_Y = 200;
const int SEC_TEXT_X = 14, SEC_TEXT_W = 291;   // section modes: bullet at x 6, text 14-304
const int SEC_BAR_W = 3, SEC_BULLET_X = 8, SEC_BULLET_R = 2;
const int FOL_TAG_X = 2, FOL_CODE_W = 30, FOL_TEXT_X = 64, FOL_TEXT_W = 241;   // tag column 0-63 (58 px + gap)
const int MODES = 5;                           // Follow, then the four lists
const int MAX_ITEMS = meeting::ITEMS_MAX > meeting::FEED_MAX ? meeting::ITEMS_MAX : meeting::FEED_MAX;
const int MAX_LINES = MAX_ITEMS * ROWS;
const int EMPTY_ROW = 2;                       // where an empty-state message starts

// ---------- colours ----------
const uint16_t TAB_FILL = 0x1926;              // active tab, and Follow's newest change set
const uint16_t GREY = 0x8410;
const uint16_t TAB_IDLE_LINE = 0x2945;
const uint16_t FADE[4] = {TFT_WHITE, 0xC618, 0x8410, 0x52AA};   // Follow text by lvl
const uint16_t C_FOLLOW = TFT_WHITE;

uint16_t secColor(Sec s) {
  switch (s) {
    case Sec::Key:       return TFT_GREEN;
    case Sec::Decisions: return TFT_YELLOW;
    case Sec::Actions:   return TFT_ORANGE;
    case Sec::Questions: return TFT_MAGENTA;
    case Sec::Summary:   return TFT_CYAN;
  }
  return TFT_WHITE;
}
const char* secTag(Sec s) {
  switch (s) {
    case Sec::Key:       return "KEY";
    case Sec::Decisions: return "DEC";
    case Sec::Actions:   return "ACT";
    case Sec::Questions: return "Q?";
    case Sec::Summary:   return "SUM";
  }
  return "?";
}
// Mode m: 0 Follow, 1 + the list index (meeting::Sec order).
uint16_t modeColor(int m) { return m == 0 ? C_FOLLOW : secColor((Sec)(m - 1)); }

const char* const MODE_TITLE[MODES] = {"FOLLOW", "KEY POINTS", "DECISIONS", "ACTIONS", "OPEN QUESTIONS"};
// Button labels, longest first: the first that fits is used.
const char* const MODE_LABEL[MODES][2] = {
    {"Follow", "Follow"}, {"Key points", "Key pts"}, {"Decisions", "Decis."}, {"Actions", "Actions"}, {"Open Q", "Open Q"}};
const char* const EMPTY_LIST[MODES] = {"No changes yet", "No key points yet", "No decisions yet", "No actions yet",
                                       "No open questions yet"};
const char* const WAIT_1 = "Waiting for the browser -";
const char* const WAIT_2 = "open https://earwing.local and load the Assistant";

// ---------- state ----------
TFT_eSPI* tft = nullptr;
Tab curTab = Tab::Device;
Net net = Net::Connecting;
int mode = 0, page = 0;
Snapshot* snap = nullptr;     // PSRAM: our copy of the store (~16 KB)
uint32_t haveGen = 0;
uint8_t fontW[128];           // font 2 advance widths, for ui_text

// Sessions already seen, so the auto-switch to Follow happens once per session.
char seen[4][meeting::SESSION_MAX + 1];
int seenNext = 0;

// Layout: the current mode's items wrapped into lines (pointing into *snap).
enum class Body : uint8_t { Wait, Empty, Follow, List };
struct ItemLayout {
  uint16_t first;             // index into lines
  uint8_t  n;                 // lines
  uint8_t  src;               // feed or list index
};
Line* lines = nullptr;        // PSRAM, MAX_LINES
ItemLayout items[MAX_ITEMS];
int nItems = 0, pages = 1;
uint8_t pageStart[MAX_ITEMS];
Body body = Body::Wait;
bool layoutValid = false;

// What's on screen (0 = unknown: repaint).
bool stripDirty = true, bodyDirty = true, tickDue = true;
uint32_t rowHash[ROWS], tagHash[ROWS], hdrHash, btnHash[MODES], freshHash;
int8_t btnDot[MODES];          // -1 unknown, else whether the dot is drawn
uint32_t nextTick = 0;

// ---------- helpers ----------
int widthFn(const char* s, size_t n, void*) {
  int w = 0;
  for (size_t i = 0; i < n; i++) w += fontW[(uint8_t)s[i] & 0x7F];
  return w;
}
int textW(const char* s) { return widthFn(s, strlen(s), nullptr); }

uint32_t fnv(uint32_t h, const void* p, size_t n) {
  const uint8_t* b = (const uint8_t*)p;
  for (size_t i = 0; i < n; i++) { h ^= b[i]; h *= 16777619u; }
  return h;
}
template <typename T> uint32_t fnvv(uint32_t h, T v) { return fnv(h, &v, sizeof(v)); }
uint32_t fnvs(uint32_t h, const char* s) { return fnv(h, s, strlen(s) + 1); }
const uint32_t FNV0 = 2166136261u;

// Text over its own background: from x, padded with bg out to padTo, so
// whatever was there before is overwritten in one pass (no clear first).
void drawText(const char* s, size_t n, int x, int y, uint16_t fg, uint16_t bg, int padTo) {
  char buf[128];
  if (n >= sizeof(buf)) n = sizeof(buf) - 1;
  memcpy(buf, s, n);
  buf[n] = 0;
  tft->setTextDatum(TL_DATUM);
  tft->setTextColor(fg, bg);
  tft->setTextPadding(padTo > x ? padTo - x : 0);
  if (n) tft->drawString(buf, x, y, FONT);
  else if (padTo > x) tft->fillRect(x, y, padTo - x, FONT_H, bg);
  tft->setTextPadding(0);
}
void drawText(const char* s, int x, int y, uint16_t fg, uint16_t bg, int padTo) {
  drawText(s, strlen(s), x, y, fg, bg, padTo);
}

// ms from then to now, 0 if then is later: loop() samples `now` before it
// copies a snapshot stamped with a fresh millis(), and the store stamps
// lastSeen from core 0, so then can be a few ms ahead.
uint32_t since(uint32_t now, uint32_t then) {
  const int32_t d = (int32_t)(now - then);
  return d > 0 ? (uint32_t)d : 0;
}

// "now", "42s", "3m", "2h" (at most "99h", so it fits the tag column): a
// Follow entry's age.
void fmtAge(char* out, size_t n, uint32_t s) {
  if (s < 10) snprintf(out, n, "now");
  else if (s < 60) snprintf(out, n, "%lus", (unsigned long)s);
  else if (s < 3600) snprintf(out, n, "%lum", (unsigned long)(s / 60));
  else snprintf(out, n, "%luh", (unsigned long)(s / 3600 > 99 ? 99 : s / 3600));
}

bool sessionSeen(const char* s) {
  for (auto& e : seen)
    if (e[0] && !strcmp(e, s)) return true;
  return false;
}
void rememberSession(const char* s) {
  if (sessionSeen(s)) return;
  snprintf(seen[seenNext], sizeof(seen[0]), "%s", s);
  seenNext = (seenNext + 1) % 4;
}

void forgetScreen() {
  for (int k = 0; k < ROWS; k++) rowHash[k] = tagHash[k] = 0;
  for (int i = 0; i < MODES; i++) btnHash[i] = 0;
  hdrHash = 0;
  bodyDirty = true;
  tickDue = true;
}

void setTab(Tab t) {
  if (t == curTab) return;
  curTab = t;
  stripDirty = true;
  if (t == Tab::Meeting) {
    tft->fillRect(0, STRIP_H, SCREEN_W, SCREEN_H - STRIP_H, TFT_BLACK);
    layoutValid = false;
    forgetScreen();
  }
  // Device, Settings: main.cpp clears below the strip and repaints its own content.
}

void setMode(int m) {
  mode = m;
  page = 0;
  layoutValid = false;
  bodyDirty = true;
}

// ---------- layout ----------
// Wrap one item into lines[used..], as items[nItems]; false when full.
bool addItem(const Seg* segs, int nSegs, int width, int src, int* used) {
  if (nItems >= MAX_ITEMS || *used >= MAX_LINES) return false;
  const int room = MAX_LINES - *used < ROWS ? MAX_LINES - *used : ROWS;
  const int n = ui_text::wrap(segs, nSegs, width, widthFn, nullptr, lines + *used, room);
  if (n == 0) return true;   // nothing printable: skip it
  items[nItems++] = {(uint16_t)*used, (uint8_t)n, (uint8_t)src};
  *used += n;
  return true;
}

void buildLayout() {
  nItems = 0;
  int used = 0;
  if (snap->gen == 0) {
    body = Body::Wait;
    const Seg segs[] = {{WAIT_1, TFT_WHITE}, {WAIT_2, GREY}};
    addItem(segs, 2, SEC_TEXT_W, 0, &used);
  } else if (mode == 0) {
    body = Body::Follow;
    for (int i = 0; i < snap->feedCount && i < meeting::FEED_MAX; i++) {
      const auto& f = snap->feed[i];
      const Seg seg = {f.text, FADE[f.lvl > 3 ? 3 : f.lvl]};
      if (!addItem(&seg, 1, FOL_TEXT_W, i, &used)) break;
    }
  } else {
    body = Body::List;
    const int l = mode - 1;
    for (int i = 0; i < snap->count[l] && i < meeting::ITEMS_MAX; i++) {
      const auto& it = snap->items[l][i];
      const Seg segs[] = {{it.text, TFT_WHITE}, {it.t, GREY}};
      if (!addItem(segs, it.t[0] ? 2 : 1, SEC_TEXT_W, i, &used)) break;
    }
  }
  if (nItems == 0 && body != Body::Wait) {
    body = Body::Empty;
    const Seg seg = {EMPTY_LIST[mode], GREY};
    addItem(&seg, 1, SEC_TEXT_W, 0, &used);
  }
  uint8_t rows[MAX_ITEMS];
  for (int i = 0; i < nItems; i++) rows[i] = items[i].n;
  pages = ui_text::paginate(rows, nItems, ROWS, pageStart, MAX_ITEMS);
  if (pages < 1) { pages = 1; pageStart[0] = 0; }
  if (page >= pages) page = 0;
  layoutValid = true;
}

// ---------- drawing ----------
void drawTab(Tab t) {
  const int x = (int)t * TAB_W;
  const char* label = TAB_LABEL[(int)t];
  const bool on = curTab == t;
  const uint16_t bg = on ? TAB_FILL : TFT_BLACK;
  tft->fillRect(x, 0, TAB_W, STRIP_H - TAB_LINE, bg);
  tft->fillRect(x, STRIP_H - TAB_LINE, TAB_W, TAB_LINE, TFT_BLACK);
  if (on) tft->fillRect(x, STRIP_H - TAB_LINE, TAB_W, TAB_LINE, TFT_CYAN);
  else tft->drawFastHLine(x, STRIP_H - 1, TAB_W, TAB_IDLE_LINE);
  tft->setTextDatum(TC_DATUM);
  tft->setTextColor(on ? TFT_WHITE : GREY, bg);
  tft->drawString(label, x + TAB_W / 2, 6, FONT);
  tft->setTextDatum(TL_DATUM);
}

void drawStrip() {
  for (int t = 0; t < TABS; t++) drawTab((Tab)t);
  stripDirty = false;
}

// The status indicator. "Streaming", filled green dot, while a browser is
// streaming /beam.wav or the last accepted post is younger than FRESH_MS (the
// stream counts on its own because a freshly opened page posts nothing until
// its first summary, which can be minutes away). Otherwise what WiFi is doing:
// "Online" with a green ring, "Connecting" yellow, "WiFi setup" cyan.
void drawFreshness(uint32_t now) {
  const uint32_t seenAt = meeting::lastSeenMs();
  const bool browser = audio_server::counts().beam > 0 || (seenAt && since(now, seenAt) < meeting::FRESH_MS);
  const char* txt;
  uint16_t c;
  if (browser) { txt = "Streaming"; c = TFT_GREEN; }
  else if (net == Net::Online) { txt = "Online"; c = TFT_GREEN; }
  else if (net == Net::Portal) { txt = "WiFi setup"; c = TFT_CYAN; }
  else { txt = "Connecting"; c = TFT_YELLOW; }
  const uint32_t h = fnvv(fnvv(fnvs(FNV0, txt), c), (uint8_t)browser) | 1;
  if (h == freshHash) return;
  freshHash = h;
  const int tx = FRESH_TEXT_R - textW(txt);
  if (tx > FRESH_X) tft->fillRect(FRESH_X, 7, tx - FRESH_X, FONT_H, TFT_BLACK);
  drawText(txt, tx, 7, c, TFT_BLACK, FRESH_TEXT_R + 2);
  tft->fillCircle(FRESH_DOT_X, STRIP_H / 2 - 1, FRESH_DOT_R, browser ? c : TFT_BLACK);
  if (!browser) tft->drawCircle(FRESH_DOT_X, STRIP_H / 2 - 1, FRESH_DOT_R, c);
}

void drawHeader() {
  char title[32], to[16], pg[8];
  const uint16_t c = modeColor(mode);
  if (body == Body::Wait) snprintf(title, sizeof(title), "MEETING");
  else if (mode == 0) snprintf(title, sizeof(title), "%s - %u", MODE_TITLE[0], (unsigned)snap->feedCount);
  else snprintf(title, sizeof(title), "%s - %u", MODE_TITLE[mode], (unsigned)snap->count[mode - 1]);
  if (body != Body::Wait && snap->coversTo[0]) snprintf(to, sizeof(to), "to %s", snap->coversTo);
  else to[0] = 0;
  if (pages > 1) snprintf(pg, sizeof(pg), "%d/%d", page + 1, pages);
  else pg[0] = 0;
  const uint16_t tc = body == Body::Wait ? GREY : c;
  const uint32_t h = fnvv(fnvs(fnvs(fnvs(FNV0, title), to), pg), tc) | 1;
  if (h == hdrHash) return;
  hdrHash = h;
  const int pgX = pg[0] ? SCREEN_W - 4 - textW(pg) : SCREEN_W;
  const int toX = 4 + textW(title) + 10;
  tft->fillRect(0, HDR_Y, 4, FONT_H, TFT_BLACK);
  drawText(title, 4, HDR_Y, tc, TFT_BLACK, toX);
  drawText(to, toX, HDR_Y, GREY, TFT_BLACK, pgX);
  if (pg[0]) drawText(pg, pgX, HDR_Y, TFT_WHITE, TFT_BLACK, SCREEN_W);
}

// Which sections changed in the latest rev: a dot on their buttons.
bool sectionFresh(int l) {
  if (snap->gen == 0) return false;
  for (int i = 0; i < snap->count[l] && i < meeting::ITEMS_MAX; i++)
    if (snap->items[l][i].fresh) return true;
  return false;
}

void drawButtons() {
  for (int m = 0; m < MODES; m++) {
    const bool on = m == mode;
    const bool dot = m > 0 && sectionFresh(m - 1);
    const int x = m * BTN_DX + 1;
    const uint16_t c = modeColor(m);
    const uint16_t bg = on ? c : TFT_BLACK;
    // The button itself (only when it turns on or off), then its dot.
    const uint32_t h = fnvv(fnvv(FNV0, (uint8_t)on), (uint8_t)m) | 1;
    if (h != btnHash[m]) {
      btnHash[m] = h;
      btnDot[m] = -1;
      tft->fillRoundRect(x, BTN_Y, BTN_W, BTN_H, BTN_R, bg);
      if (!on) tft->drawRoundRect(x, BTN_Y, BTN_W, BTN_H, BTN_R, c);
      const char* label = MODE_LABEL[m][0];
      if (textW(label) > BTN_W - 4) label = MODE_LABEL[m][1];
      tft->setTextDatum(TC_DATUM);
      tft->setTextColor(on ? TFT_BLACK : c, bg);
      tft->drawString(label, x + BTN_W / 2, BTN_Y + 10, FONT);
      tft->setTextDatum(TL_DATUM);
    }
    if (btnDot[m] != (int8_t)dot) {
      if (dot || btnDot[m] == 1) tft->fillCircle(x + BTN_W - 7, BTN_Y + 6, 2, dot ? (on ? TFT_BLACK : c) : bg);
      btnDot[m] = (int8_t)dot;
    }
  }
}

// One body row as the current page has it.
struct RowView {
  const Line* line;   // nullptr: blank
  int8_t item;        // index into items
  bool first;         // the item's first line
  uint16_t bg;
};

void pageRows(RowView* rv) {
  for (int k = 0; k < ROWS; k++) rv[k] = {nullptr, -1, false, TFT_BLACK};
  int row = body == Body::Wait ? EMPTY_ROW : 0;
  const int end = page + 1 < pages ? pageStart[page + 1] : nItems;
  for (int i = pageStart[page]; i < end && row < ROWS; i++) {
    uint16_t bg = TFT_BLACK;
    if (body == Body::Follow && snap->feed[items[i].src].lvl == 0) bg = TAB_FILL;
    for (int j = 0; j < items[i].n && row < ROWS; j++)
      rv[row++] = {&lines[items[i].first + j], (int8_t)i, j == 0, bg};
  }
}

uint32_t rowKey(const RowView& r) {
  uint32_t h = fnvv(fnvv(FNV0, (uint8_t)body), r.bg);
  if (!r.line) return h | 1;
  h = fnvv(h, (uint8_t)r.first);
  // Every line of a List item carries its fresh bar, so every line hashes it.
  if (body == Body::List) {
    h = fnvv(h, (uint8_t)snap->items[mode - 1][items[r.item].src].fresh);
    h = fnvv(h, (uint8_t)mode);
  }
  for (int i = 0; i < r.line->n; i++) {
    const auto& run = r.line->run[i];
    h = fnvv(fnvv(fnvv(h, run.x), run.color), run.len);
    h = fnv(h, run.s, run.len);
  }
  return h | 1;
}

void drawRow(int k, const RowView& r) {
  const int y = BODY_Y + k * ROW_H;
  const uint16_t bg = r.bg;
  if (!r.line) {
    tft->fillRect(0, y, SCREEN_W, ROW_H, bg);
    tagHash[k] = 0;
    return;
  }
  int x0;
  if (body == Body::Follow) {
    x0 = FOL_TEXT_X;
    tagHash[k] = 0;   // the tag column follows the row's background: drawTags() repaints it
  } else {
    x0 = SEC_TEXT_X;
    bool bar = false;
    if (body == Body::List) bar = snap->items[mode - 1][items[r.item].src].fresh;
    tft->fillRect(0, y, SEC_BAR_W, FONT_H, bar ? modeColor(mode) : bg);
    tft->fillRect(SEC_BAR_W, y, SEC_TEXT_X - SEC_BAR_W, FONT_H, bg);
    if (r.first && body == Body::List) tft->fillCircle(SEC_BULLET_X, y + FONT_H / 2, SEC_BULLET_R, modeColor(mode));
  }
  const Line& l = *r.line;
  for (int i = 0; i < l.n; i++) {
    const int x = x0 + l.run[i].x;
    const int next = i + 1 < l.n ? x0 + l.run[i + 1].x : SCREEN_W;
    drawText(l.run[i].s, l.run[i].len, x, y, l.run[i].color, bg, next);
  }
  tft->drawFastHLine(0, y + FONT_H, SCREEN_W, bg);   // the gap to the next row
}

void drawBody() {
  RowView rv[ROWS];
  pageRows(rv);
  for (int k = 0; k < ROWS; k++) {
    const uint32_t h = rowKey(rv[k]);
    if (h == rowHash[k]) continue;
    rowHash[k] = h;
    drawRow(k, rv[k]);
  }
}

// Follow's tag column: section code and a ticking age on each entry's first
// line. Fixed width, so an age changing never rewraps the text beside it.
void drawTags(uint32_t now) {
  if (body != Body::Follow) return;
  RowView rv[ROWS];
  pageRows(rv);
  for (int k = 0; k < ROWS; k++) {
    if (!rv[k].line) continue;
    const int y = BODY_Y + k * ROW_H;
    const uint16_t bg = rv[k].bg;
    char age[8] = "";
    const char* code = "";
    uint16_t cc = TFT_WHITE, ac = TFT_WHITE;
    if (rv[k].first) {
      const auto& f = snap->feed[items[rv[k].item].src];
      const uint8_t lvl = f.lvl > 3 ? 3 : f.lvl;
      fmtAge(age, sizeof(age), f.agoS + since(now, snap->recvMs) / 1000);
      code = secTag(f.sec);
      cc = lvl <= 1 ? secColor(f.sec) : FADE[lvl];
      ac = FADE[lvl];
    }
    const uint32_t h = fnvv(fnvv(fnvv(fnvs(fnvs(FNV0, code), age), cc), ac), bg) | 1;
    if (h == tagHash[k]) continue;
    tagHash[k] = h;
    tft->fillRect(0, y, FOL_TAG_X, FONT_H, bg);
    drawText(code, FOL_TAG_X, y, cc, bg, FOL_TAG_X + FOL_CODE_W);
    drawText(age, FOL_TAG_X + FOL_CODE_W, y, ac, bg, FOL_TEXT_X);
  }
}

#ifdef UI_DEBUG
SemaphoreHandle_t shotLock;
uint16_t* shotBuf;
volatile bool shotWanted, shotReady;
volatile bool injPending;
volatile int injX, injY;

void debugLoop() {
  if (!shotWanted) return;
  tft->readRect(0, 0, SCREEN_W, SCREEN_H, shotBuf);
  shotWanted = false;
  shotReady = true;
}
#endif

}  // namespace

void begin(TFT_eSPI* t) {
  tft = t;
  for (int c = 0; c < 128; c++) {
    const char s[2] = {(char)(c >= 32 && c < 127 ? c : '?'), 0};
    fontW[c] = (uint8_t)tft->textWidth(s, FONT);
  }
  snap = (Snapshot*)heap_caps_calloc(1, sizeof(Snapshot), MALLOC_CAP_SPIRAM);
  lines = (Line*)heap_caps_calloc(MAX_LINES, sizeof(Line), MALLOC_CAP_SPIRAM);
  if (!snap || !lines) Serial.println("ui: no PSRAM for the meeting tab");
#ifdef UI_DEBUG
  shotLock = xSemaphoreCreateMutex();
#endif
  tft->fillRect(0, 0, SCREEN_W, STRIP_H, TFT_BLACK);
  drawStrip();
  drawFreshness(millis());
}

Tab tab() { return curTab; }

void showDevice() { setTab(Tab::Device); }

void setNet(Net n) {
  if (n == net) return;
  net = n;
  nextTick = millis();   // the status indicator, at once
}

void loop(uint32_t now) {
#ifdef UI_DEBUG
  debugLoop();
#endif
  if (!snap || !lines) return;
  if (meeting::copy(snap, haveGen)) {
    haveGen = snap->gen;
    layoutValid = false;
    bodyDirty = true;
    nextTick = now;   // the status indicator, at once
    if (mode == 0) page = 0;
    // The first summary of a session brings the Meeting tab up once, in
    // Follow; after that only taps change tabs.
    const bool fresh = snap->firstOfSession && !sessionSeen(snap->session);
    rememberSession(snap->session);
    if (fresh) {
      Serial.printf("ui: new meeting session %s: showing Follow\n", snap->session);
      setTab(Tab::Meeting);
      setMode(0);
    }
  }
  if (stripDirty) drawStrip();
  if ((int32_t)(now - nextTick) >= 0) {
    nextTick = now + 1000;
    drawFreshness(now);
    tickDue = true;
  }
  if (curTab != Tab::Meeting) return;
  if (!layoutValid) buildLayout();
  if (bodyDirty) {
    drawHeader();
    drawBody();
    drawButtons();
    bodyDirty = false;
    tickDue = true;
  }
  if (tickDue) {
    drawTags(now);
    tickDue = false;
  }
}

bool handleTap(int x, int y) {
  if (y < TAP_STRIP_Y && x < TAP_STRIP_X) {
    int i = x / TAB_W;
    if (i < 0) i = 0;
    if (i >= TABS) i = TABS - 1;
    const Tab t = (Tab)i;
    Serial.printf("touch: tap %d,%d on tab %s\n", x, y, TAB_LABEL[i]);
    setTab(t);
    return true;
  }
  if (curTab != Tab::Meeting) return false;
  if (y >= TAP_BTN_Y) {
    int m = x / BTN_DX;
    if (m < 0) m = 0;
    if (m >= MODES) m = MODES - 1;
    Serial.printf("touch: tap %d,%d on mode %s\n", x, y, MODE_TITLE[m]);
    setMode(m);
  } else if (y >= TAP_STRIP_Y) {
    if (pages > 1) {
      page = (page + 1) % pages;
      bodyDirty = true;
    }
    Serial.printf("touch: tap %d,%d on body: page %d/%d\n", x, y, page + 1, pages);
  }
  return true;
}

#ifdef UI_DEBUG
void debugScreenHandler(web::Req& q) {
  if (xSemaphoreTake(shotLock, pdMS_TO_TICKS(5000)) != pdTRUE) {
    q.send(503, "text/plain", "busy\n");
    return;
  }
  if (!shotBuf) shotBuf = (uint16_t*)heap_caps_malloc(SCREEN_W * SCREEN_H * 2, MALLOC_CAP_SPIRAM);
  bool ok = false;
  if (shotBuf) {
    shotReady = false;
    shotWanted = true;
    for (int i = 0; i < 300 && !shotReady; i++) vTaskDelay(pdMS_TO_TICKS(10));
    ok = shotReady;
  }
  if (!ok) {
    shotWanted = false;
    xSemaphoreGive(shotLock);
    q.send(503, "text/plain", "no screenshot (loop() busy, or no PSRAM)\n");
    return;
  }
  // readRect gives RGB565 byte-swapped (for pushRect); ?swap=0 / ?bgr=1 to
  // experiment if the colours come out wrong.
  const bool swap = q.arg("swap") != "0", bgr = q.arg("bgr") == "1";
  // BMP rows are padded to 4 bytes: 309 px is 927 bytes of pixels.
  const uint32_t rowBytes = (SCREEN_W * 3 + 3) & ~3u, size = 54 + rowBytes * SCREEN_H;
  uint8_t hdr[54] = {'B', 'M'};
  auto put32 = [&](int o, uint32_t v) { for (int i = 0; i < 4; i++) hdr[o + i] = (uint8_t)(v >> (8 * i)); };
  put32(2, size);
  put32(10, 54);
  put32(14, 40);
  put32(18, SCREEN_W);
  put32(22, SCREEN_H);   // positive: bottom-up rows
  hdr[26] = 1;
  hdr[28] = 24;
  put32(34, rowBytes * SCREEN_H);
  static uint8_t row[(SCREEN_W * 3 + 3) & ~3];   // padding stays zero
  if (q.begin(200, "image/bmp", size) && q.write(hdr, sizeof(hdr))) {
    for (int y = SCREEN_H - 1; y >= 0; y--) {
      for (int x = 0; x < SCREEN_W; x++) {
        uint16_t p = shotBuf[y * SCREEN_W + x];
        if (swap) p = (uint16_t)((p >> 8) | (p << 8));
        uint8_t r = (uint8_t)(((p >> 11) & 31) * 255 / 31), g = (uint8_t)(((p >> 5) & 63) * 255 / 63),
                b = (uint8_t)((p & 31) * 255 / 31);
        if (bgr) { const uint8_t t = r; r = b; b = t; }
        row[x * 3] = b;
        row[x * 3 + 1] = g;
        row[x * 3 + 2] = r;
      }
      if (!q.write(row, rowBytes)) break;
    }
  }
  xSemaphoreGive(shotLock);
}

void debugTapHandler(web::Req& q) {
  if (!q.hasArg("x") || !q.hasArg("y")) {
    q.send(400, "text/plain", "want ?x=&y=\n");
    return;
  }
  injX = q.arg("x").toInt();
  injY = q.arg("y").toInt();
  injPending = true;
  q.send(200, "text/plain", "ok\n");
}

bool debugTakeTap(int* x, int* y) {
  if (!injPending) return false;
  *x = injX;
  *y = injY;
  injPending = false;
  return true;
}
#endif

}  // namespace ui_meeting
