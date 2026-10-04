// The meeting display's data: what the site's in-browser assistant sends to
// POST /meeting after each summary (and as a 30 s heartbeat), and what the
// Meeting tab draws. Pure C++ (no Arduino), so the parser and the accept
// policy are host-tested (make -C code/test).
//
// The browser does the thinking: it parses the LLM's Markdown summary into
// sections, diffs it against the previous one, builds the Follow feed, works
// out each entry's fade level and transliterates everything to printable
// ASCII (the TFT's fonts have nothing else). The device stores the latest
// snapshot and draws it; it never merges or diffs.
//
// The wire format, one JSON object (site/scripts/fixtures/meeting-sample.json
// is a complete example; site/src/components/meetingContract.js mirrors these
// limits):
//   v         1
//   session   [A-Za-z0-9-]{1,23}: "<random6 per page load>-<take>"
//   rev       summaries completed in this session (1, 2, ...)
//   seq       posts sent by this page (heartbeats included)
//   hb        true: a heartbeat re-sending rev with fresh "ago"s
//   coversTo  "mm:ss" or "h:mm:ss": how far into the take the summary reaches
//   summary   the summary paragraph
//   sections  {"key":[], "decisions":[], "actions":[], "questions":[]}, each
//             item {"text", "t" (its [mm:ss] citation, may be ""),
//             "new" (changed in the latest rev that changed anything,
//             so a re-run with no changes keeps the highlights)}
//   feed      newest first: {"sec" key|decisions|actions|questions|summary,
//             "kind" new|edited|summary, "text", "t", "lvl" 0..3 (fade: 0 is
//             the latest change set), "ago" (seconds since that change, at
//             send time)}
// Strings are printable ASCII, citations and Markdown already stripped. The
// browser keeps to the caps below; the device truncates anything longer (and
// sanitizes anything non-ASCII to '?') rather than rejecting it.
//
// Responses: 200 {"ok":true,"result":"replaced|touched|ignored|old",
// "session":"...","rev":N}; 400 bad JSON or v != 1; 413 body over MAX_BODY;
// 415 not application/json (which also means a cross-site page can't POST
// here without a CORS preflight, and there's no OPTIONS route to pass one).
#pragma once

#include <stddef.h>
#include <stdint.h>

namespace meeting {

static const size_t   MAX_BODY      = 16384;   // POST body limit (the browser aims under 15000)
static const size_t   SUMMARY_MAX   = 560;     // the summary paragraph
static const size_t   TEXT_MAX      = 180;     // one section item
static const size_t   FEED_TEXT_MAX = 360;     // one feed entry (a summary entry is the long one)
static const size_t   T_MAX         = 8;       // "h:mm:ss"
static const size_t   SESSION_MAX   = 23;
static const int      ITEMS_MAX     = 12;      // per section
static const int      FEED_MAX      = 12;
static const uint32_t FRESH_MS      = 75000;   // UI: "Connected" while the last post is younger than this (or audio streams)
static const uint32_t STALE_MS      = 150000;  // decide(): another tab may take over

// The four item lists, in this order everywhere (Snapshot::items, the
// Meeting tab's mode buttons after Follow). Summary is only a feed section.
enum class Sec : uint8_t { Key = 0, Decisions, Actions, Questions, Summary };
static const int LIST_COUNT = 4;
enum class Kind : uint8_t { New = 0, Edited, Summary };

struct Item {
  char text[TEXT_MAX + 1];
  char t[T_MAX + 1];
  bool fresh;                  // "new": changed in the latest change set
};

struct FeedEntry {
  char     text[FEED_TEXT_MAX + 1];
  char     t[T_MAX + 1];
  Sec      sec;
  Kind     kind;
  uint8_t  lvl;                // 0..3, clamped
  uint32_t agoS;               // at recvMs; the age now is agoS + (now - recvMs) / 1000
};

struct Snapshot {
  uint32_t  gen;               // the store's generation: bumped on every replace, 0 = empty
  char      session[SESSION_MAX + 1];
  uint32_t  rev;
  char      coversTo[T_MAX + 1];
  char      summary[SUMMARY_MAX + 1];
  uint8_t   count[LIST_COUNT];
  Item      items[LIST_COUNT][ITEMS_MAX];
  uint8_t   feedCount;
  FeedEntry feed[FEED_MAX];
  uint32_t  recvMs;            // millis() when this was stored
  bool      firstOfSession;    // this replace started a session the store hadn't had
};

// One parsed POST: the snapshot fields (gen, recvMs, firstOfSession are left
// 0 for the store to set) plus the envelope.
struct Parsed {
  Snapshot s;
  uint32_t seq;
  bool     hb;
};

enum class Result : uint8_t { Replaced, Touched, Ignored, Old };
const char* resultName(Result r);   // "replaced", "touched", "ignored", "old"
const char* secName(Sec s);         // the wire names: "key" ... "summary"
const char* kindName(Kind k);       // "new", "edited", "summary"

// Parse a POST body (not necessarily NUL-terminated). false with *err set to
// a short static reason on bad JSON, a missing/wrong "v", or a missing
// session. Strings are truncated to their caps and sanitize()d; lists beyond
// ITEMS_MAX / FEED_MAX are dropped; unknown "sec"/"kind" entries are skipped.
bool parse(const char* body, size_t len, Parsed* out, const char** err);

// In place: each UTF-8 sequence (or stray byte >= 0x80) becomes one '?',
// control characters become spaces, runs of spaces collapse, ends trimmed.
void sanitize(char* s);

// The accept policy, given the store's current snapshot and when the last
// post was accepted (replaced or touched; 0 = never):
//   store empty (cur.gen == 0)                 -> Replaced
//   same session, higher rev                   -> Replaced
//   same session, same rev                     -> Touched (heartbeat/retry: last-seen only)
//   same session, lower rev                    -> Old
//   other session, not a heartbeat             -> Replaced (last writer wins)
//   other session, heartbeat                   -> Ignored, unless nothing was
//                                                 accepted for STALE_MS: Replaced
// So two open tabs don't ping-pong on heartbeats, and a rebooted device is
// refilled by the next heartbeat.
Result decide(const Snapshot& cur, uint32_t lastSeenMs, const Parsed& in, uint32_t nowMs);

}  // namespace meeting
