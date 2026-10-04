// Host tests for the meeting store's pure parts (meeting_types.h): the POST
// body parser, sanitize() and the accept policy, plus the demo snapshots.
// Build and run: make -C code/test
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <string>

#include "check.h"
#include "meeting_demo.h"
#include "meeting_types.h"

using namespace meeting;

static Snapshot snap;
static Parsed parsed;   // ~16 KB: static, as the device keeps it off the stack

static void testDemo() {
  for (uint32_t step = 0; step < DEMO_STEPS; step++) {
    memset(&snap, 0, sizeof(snap));
    demoFill(&snap, step);
    CHECK(snap.rev == step + 1, "demo rev %u", (unsigned)snap.rev);
    CHECK(snap.feedCount > 0 && snap.feedCount <= FEED_MAX, "demo feedCount %u", snap.feedCount);
    CHECK(snap.feed[0].lvl == 0, "demo newest entry lvl %u", snap.feed[0].lvl);
  }
}

// ---------- helpers ----------
static bool parseStr(const std::string& body, Parsed* out, const char** err) {
  return parse(body.data(), body.size(), out, err);
}

static std::string rep(char c, size_t n) { return std::string(n, c); }

// The fixture, relative to where make runs the tests (code/test), or to this
// source file if run from elsewhere.
static std::string readFixture() {
  const char* rel = "../../site/scripts/fixtures/meeting-sample.json";
  std::string paths[2] = {rel, ""};
  std::string here = __FILE__;
  const size_t slash = here.find_last_of('/');
  paths[1] = (slash == std::string::npos ? std::string(".") : here.substr(0, slash)) + "/" + rel;
  for (const std::string& p : paths) {
    FILE* f = fopen(p.c_str(), "rb");
    if (!f) continue;
    std::string s;
    char buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) s.append(buf, n);
    fclose(f);
    return s;
  }
  return "";
}

// ---------- sanitize ----------
static void expectClean(const char* in, const char* want) {
  char buf[256];
  snprintf(buf, sizeof(buf), "%s", in);
  sanitize(buf);
  CHECK(!strcmp(buf, want), "sanitize(\"%s\") = \"%s\", want \"%s\"", in, buf, want);
}

static void testSanitize() {
  expectClean("plain text", "plain text");
  expectClean("", "");
  expectClean("   ", "");
  expectClean("  lead and trail  ", "lead and trail");
  expectClean("a\tb\nc\r\nd", "a b c d");
  expectClean("a    b", "a b");
  expectClean("a\x01\x1f\x7f" "b", "a b");
  expectClean("caf\xC3\xA9", "caf?");                        // 2-byte
  expectClean("\xE2\x80\x9Cquoted\xE2\x80\x9D", "?quoted?");  // 3-byte curly quotes
  expectClean("emoji \xF0\x9F\x98\x80!", "emoji ?!");         // 4-byte
  expectClean("stray \x80\x80 bytes", "stray ?? bytes");      // continuations alone
  expectClean("cut \xE2\x80", "cut ?");                       // truncated sequence
  expectClean("bad \xFF\xFE", "bad ??");
  expectClean("\xC3\xA9\xC3\xA9", "??");
  expectClean("keep \"quotes\" and \\slashes\\", "keep \"quotes\" and \\slashes\\");
}

// ---------- parse ----------
static void testFixture() {
  const std::string body = readFixture();
  CHECK(!body.empty(), "fixture not found (site/scripts/fixtures/meeting-sample.json)");
  if (body.empty()) return;
  const char* err = nullptr;
  const bool ok = parseStr(body, &parsed, &err);
  CHECK(ok, "fixture parse failed: %s", err ? err : "?");
  if (!ok) return;
  const Snapshot& s = parsed.s;
  CHECK(!strcmp(s.session, "q7k2xa-3"), "session %s", s.session);
  CHECK(s.rev == 7, "rev %u", (unsigned)s.rev);
  CHECK(parsed.seq == 42, "seq %u", (unsigned)parsed.seq);
  CHECK(!parsed.hb, "hb");
  CHECK(!strcmp(s.coversTo, "12:34"), "coversTo %s", s.coversTo);
  CHECK(!strncmp(s.summary, "The team reviewed the Q3 budget", 31), "summary %s", s.summary);
  CHECK(s.gen == 0 && s.recvMs == 0 && !s.firstOfSession, "store fields left for the store");
  CHECK(s.count[(int)Sec::Key] == 2, "key count %u", s.count[0]);
  CHECK(s.count[(int)Sec::Decisions] == 1, "decisions count %u", s.count[1]);
  CHECK(s.count[(int)Sec::Actions] == 1, "actions count %u", s.count[2]);
  CHECK(s.count[(int)Sec::Questions] == 0, "questions count %u", s.count[3]);
  CHECK(!strcmp(s.items[0][0].text, "Hardware costs are about 10% over the Q3 budget"), "key[0] %s", s.items[0][0].text);
  CHECK(!strcmp(s.items[0][0].t, "03:12") && !s.items[0][0].fresh, "key[0] t/new");
  CHECK(!strcmp(s.items[1][0].text, "Launch moves from 30 Sep to 14 Oct") && s.items[1][0].fresh, "decisions[0]");
  CHECK(!strcmp(s.items[2][0].t, "11:02") && s.items[2][0].fresh, "actions[0]");
  CHECK(s.feedCount == 4, "feedCount %u", s.feedCount);
  CHECK(s.feed[0].sec == Sec::Actions && s.feed[0].kind == Kind::New && s.feed[0].lvl == 0 && s.feed[0].agoS == 0,
        "feed[0]");
  CHECK(s.feed[1].sec == Sec::Decisions && s.feed[1].kind == Kind::Edited, "feed[1]");
  CHECK(s.feed[2].sec == Sec::Summary && s.feed[2].kind == Kind::Summary && s.feed[2].lvl == 1 &&
        s.feed[2].agoS == 41 && !strcmp(s.feed[2].t, "10:15"), "feed[2]");
  CHECK(s.feed[3].sec == Sec::Key && s.feed[3].lvl == 2 && s.feed[3].agoS == 95, "feed[3]");

  // Not NUL-terminated: parse() must stay within len.
  std::string padded = body + "XXXX";
  CHECK(parse(padded.data(), body.size(), &parsed, &err), "len-bounded parse: %s", err);
}

static void testRejects() {
  const char* err = nullptr;
  struct Case { const char* body; const char* why; };
  const Case bad[] = {
    {"", "empty"},
    {"not json", "garbage"},
    {"{\"v\":1,\"session\":\"a\"", "truncated"},
    {"{\"v\":1,\"session\":\"a\"} trailing", "trailing data"},
    {"[1,2,3]", "not an object"},
    {"{\"session\":\"a\"}", "missing v"},
    {"{\"v\":2,\"session\":\"a\"}", "v 2"},
    {"{\"v\":\"1\",\"session\":\"a\"}", "v as a string"},
    {"{\"v\":1}", "missing session"},
    {"{\"v\":1,\"session\":\"\"}", "empty session"},
    {"{\"v\":1,\"session\":\"   \"}", "blank session"},
    {"{\"v\":1,\"session\":7}", "session not a string"},
  };
  for (const Case& c : bad) {
    err = nullptr;
    CHECK(!parse(c.body, strlen(c.body), &parsed, &err), "accepted %s", c.why);
    CHECK(err && *err, "no reason for %s", c.why);
  }
  CHECK(!parse(nullptr, 0, &parsed, nullptr), "null body with null err");

  err = nullptr;
  parse("{\"v\":2,\"session\":\"a\"}", 22, &parsed, &err);
  CHECK(err && !strcmp(err, "v must be 1"), "v 2 reason %s", err ? err : "(null)");
  parse("{\"v\":1}", 7, &parsed, &err);
  CHECK(err && !strcmp(err, "missing session"), "session reason %s", err ? err : "(null)");

  // Deep nesting is refused before cJSON recurses into it (the server task's
  // stack is 6 KB); the format's own depth (4) is fine.
  std::string deep = "{\"v\":1,\"session\":\"a\",\"x\":" + rep('[', 500) + rep(']', 500) + "}";
  CHECK(!parseStr(deep, &parsed, &err), "accepted 500-deep nesting");
  std::string brackets = "{\"v\":1,\"session\":\"a\",\"x\":\"" + rep('[', 50) + "\"}";
  CHECK(parseStr(brackets, &parsed, &err), "brackets inside a string counted as nesting: %s", err);

  // Minimal: everything optional but v and session.
  CHECK(parseStr("{\"v\":1,\"session\":\"abc-1\"}", &parsed, &err), "minimal: %s", err);
  CHECK(parsed.s.rev == 0 && parsed.seq == 0 && !parsed.hb && parsed.s.feedCount == 0, "minimal defaults");
  for (int l = 0; l < LIST_COUNT; l++) CHECK(parsed.s.count[l] == 0, "minimal list %d", l);
  CHECK(parseStr("{\"v\":1.0,\"session\":\"a\",\"hb\":true,\"sections\":{}}", &parsed, &err) && parsed.hb,
        "v 1.0, hb true");
}

static void testTruncation() {
  const char* err = nullptr;
  const std::string long1 = rep('s', 100);
  std::string body = "{\"v\":1,\"session\":\"" + long1 + "\",\"rev\":3,\"coversTo\":\"" + rep('9', 20) +
                     "\",\"summary\":\"" + rep('m', 1000) + "\",\"sections\":{\"key\":[{\"text\":\"" +
                     rep('k', 400) + "\",\"t\":\"" + rep('1', 20) + "\"}]},\"feed\":[{\"sec\":\"key\",\"kind\":\"new\","
                     "\"text\":\"" + rep('f', 800) + "\",\"t\":\"" + rep('2', 20) + "\",\"lvl\":0,\"ago\":1}]}";
  CHECK(parseStr(body, &parsed, &err), "long fields: %s", err);
  const Snapshot& s = parsed.s;
  CHECK(strlen(s.session) == SESSION_MAX, "session len %zu", strlen(s.session));
  CHECK(strlen(s.coversTo) == T_MAX, "coversTo len %zu", strlen(s.coversTo));
  CHECK(strlen(s.summary) == SUMMARY_MAX, "summary len %zu", strlen(s.summary));
  CHECK(s.count[0] == 1 && strlen(s.items[0][0].text) == TEXT_MAX, "item text len %zu", strlen(s.items[0][0].text));
  CHECK(strlen(s.items[0][0].t) == T_MAX, "item t len %zu", strlen(s.items[0][0].t));
  CHECK(s.feedCount == 1 && strlen(s.feed[0].text) == FEED_TEXT_MAX, "feed text len %zu", strlen(s.feed[0].text));
  CHECK(strlen(s.feed[0].t) == T_MAX, "feed t len %zu", strlen(s.feed[0].t));

  // Exactly at the cap: kept whole.
  body = "{\"v\":1,\"session\":\"" + rep('a', SESSION_MAX) + "\",\"summary\":\"" + rep('b', SUMMARY_MAX) + "\"}";
  CHECK(parseStr(body, &parsed, &err), "at cap: %s", err);
  CHECK(strlen(parsed.s.session) == SESSION_MAX && strlen(parsed.s.summary) == SUMMARY_MAX, "at-cap lengths");

  // A cut at a word boundary leaves no trailing space.
  body = "{\"v\":1,\"session\":\"a\",\"sections\":{\"key\":[{\"text\":\"" + rep('x', TEXT_MAX - 1) + " yyy\"}]}}";
  CHECK(parseStr(body, &parsed, &err), "cut at space: %s", err);
  CHECK(strlen(parsed.s.items[0][0].text) == TEXT_MAX - 1, "cut at space len %zu", strlen(parsed.s.items[0][0].text));

  // Multi-byte characters count once against the cap, after sanitizing.
  std::string e;
  for (int i = 0; i < 300; i++) e += "\xC3\xA9";
  body = "{\"v\":1,\"session\":\"a\",\"summary\":\"" + e + "\"}";
  CHECK(parseStr(body, &parsed, &err), "utf-8 summary: %s", err);
  CHECK(strlen(parsed.s.summary) == 300 && parsed.s.summary[0] == '?' && parsed.s.summary[299] == '?',
        "utf-8 summary len %zu", strlen(parsed.s.summary));
}

static void testUtf8AndControl() {
  const char* err = nullptr;
  // Raw UTF-8 bytes, JSON \u escapes (cJSON decodes them to UTF-8) and control
  // characters, in every string field.
  const std::string body =
      "{\"v\":1,\"session\":\"s\xC3\xA9-1\",\"coversTo\":\"1\\u00e92\","
      "\"summary\":\"Caf\xC3\xA9 \\u2014 na\\u00efve\\n\\tdone \\ud83d\\ude00\","
      "\"sections\":{\"decisions\":[{\"text\":\"\xE2\x80\x9CGo\xE2\x80\x9D\\u0007now\",\"t\":\"\\u00e9\"}]},"
      "\"feed\":[{\"sec\":\"decisions\",\"kind\":\"new\",\"text\":\"\\u201cGo\\u201d\",\"t\":\"1:00\"}]}";
  CHECK(parseStr(body, &parsed, &err), "utf-8 body: %s", err);
  const Snapshot& s = parsed.s;
  CHECK(!strcmp(s.session, "s?-1"), "session %s", s.session);
  CHECK(!strcmp(s.coversTo, "1?2"), "coversTo %s", s.coversTo);
  CHECK(!strcmp(s.summary, "Caf? ? na?ve done ?"), "summary \"%s\"", s.summary);
  CHECK(s.count[1] == 1 && !strcmp(s.items[1][0].text, "?Go? now"), "item \"%s\"", s.items[1][0].text);
  CHECK(!strcmp(s.items[1][0].t, "?"), "item t %s", s.items[1][0].t);
  CHECK(s.feedCount == 1 && !strcmp(s.feed[0].text, "?Go?"), "feed \"%s\"", s.feed[0].text);
  for (const unsigned char* p = (const unsigned char*)s.summary; *p; p++)
    CHECK(*p >= 0x20 && *p < 0x7F, "non-printable 0x%02x in summary", *p);
}

// Lists over the caps, unknown sec/kind, junk entries and clamped numbers.
static void testLists() {
  const char* err = nullptr;
  std::string items, feed;
  for (int i = 0; i < 20; i++) {
    items += std::string(i ? "," : "") + "{\"text\":\"item " + std::to_string(i) + "\",\"t\":\"00:0" +
             std::to_string(i % 10) + "\",\"new\":" + (i % 2 ? "true" : "false") + "}";
  }
  // Feed: 3 junk entries first (unknown sec, unknown kind, not an object),
  // then 20 good ones.
  feed = "{\"sec\":\"agenda\",\"kind\":\"new\",\"text\":\"x\"},{\"sec\":\"key\",\"kind\":\"moved\",\"text\":\"x\"},42";
  for (int i = 0; i < 20; i++)
    feed += ",{\"sec\":\"key\",\"kind\":\"edited\",\"text\":\"feed " + std::to_string(i) + "\",\"lvl\":" +
            std::to_string(i - 2) + ",\"ago\":" + std::to_string(i * 10) + "}";
  const std::string body = "{\"v\":1,\"session\":\"a\",\"rev\":-5,\"seq\":1e12,\"sections\":{\"key\":[" + items +
                           "],\"actions\":[\"a string\",{\"t\":\"1:00\"},{\"text\":\"   \"},{\"text\":\"real\"}],"
                           "\"questions\":{\"not\":\"a list\"},\"extra\":[{\"text\":\"ignored\"}]},\"feed\":[" +
                           feed + "]}";
  CHECK(parseStr(body, &parsed, &err), "lists: %s", err);
  const Snapshot& s = parsed.s;
  CHECK(s.rev == 0, "negative rev -> 0, got %u", (unsigned)s.rev);
  CHECK(parsed.seq == 0xFFFFFFFFu, "huge seq clamps, got %u", (unsigned)parsed.seq);
  CHECK(s.count[0] == ITEMS_MAX, "key count %u", s.count[0]);
  CHECK(!strcmp(s.items[0][0].text, "item 0") && !strcmp(s.items[0][11].text, "item 11"), "first 12 kept");
  CHECK(!s.items[0][0].fresh && s.items[0][1].fresh, "new flags");
  CHECK(s.count[1] == 0, "decisions absent");
  CHECK(s.count[2] == 1 && !strcmp(s.items[2][0].text, "real"), "junk items skipped, count %u", s.count[2]);
  CHECK(s.count[3] == 0, "questions not a list");
  CHECK(s.feedCount == FEED_MAX, "feedCount %u", s.feedCount);
  CHECK(!strcmp(s.feed[0].text, "feed 0") && !strcmp(s.feed[11].text, "feed 11"), "junk feed skipped, first 12 kept");
  CHECK(s.feed[0].lvl == 0 && s.feed[1].lvl == 0 && s.feed[2].lvl == 0 && s.feed[4].lvl == 2 && s.feed[5].lvl == 3 &&
        s.feed[11].lvl == 3, "lvl clamped to 0..3");
  CHECK(s.feed[3].agoS == 30 && s.feed[0].kind == Kind::Edited, "ago/kind");

  // Missing sections key, and sections that isn't an object.
  CHECK(parseStr("{\"v\":1,\"session\":\"a\",\"sections\":[1],\"feed\":{}}", &parsed, &err), "odd types: %s", err);
  CHECK(parsed.s.count[0] == 0 && parsed.s.feedCount == 0, "odd types empty");

  // Every sec/kind name maps.
  const char* secs[] = {"key", "decisions", "actions", "questions", "summary"};
  const char* kinds[] = {"new", "edited", "summary"};
  for (int i = 0; i < 5; i++)
    for (int k = 0; k < 3; k++) {
      std::string b = std::string("{\"v\":1,\"session\":\"a\",\"feed\":[{\"sec\":\"") + secs[i] + "\",\"kind\":\"" +
                      kinds[k] + "\",\"text\":\"x\"}]}";
      CHECK(parseStr(b, &parsed, &err) && parsed.s.feedCount == 1 && parsed.s.feed[0].sec == (Sec)i &&
            parsed.s.feed[0].kind == (Kind)k, "sec %s kind %s", secs[i], kinds[k]);
    }
}

// ---------- decide ----------
static Snapshot cur;
static Parsed in;

static void setCur(const char* session, uint32_t rev, uint32_t g = 5) {
  memset(&cur, 0, sizeof(cur));
  snprintf(cur.session, sizeof(cur.session), "%s", session);
  cur.rev = rev;
  cur.gen = g;
}

static void setIn(const char* session, uint32_t rev, bool hb) {
  memset(&in, 0, sizeof(in));
  snprintf(in.s.session, sizeof(in.s.session), "%s", session);
  in.s.rev = rev;
  in.hb = hb;
}

static void expectDecide(Result want, uint32_t seen, uint32_t now, const char* what) {
  const Result got = decide(cur, seen, in, now);
  CHECK(got == want, "%s: got %s, want %s", what, resultName(got), resultName(want));
}

static void testDecide() {
  const uint32_t T = 1000000;
  for (int hb = 0; hb < 2; hb++) {
    // Store empty: anything is taken, even an old rev or a heartbeat.
    setCur("", 0, 0);
    setIn("aaaaaa-1", 1, hb);
    expectDecide(Result::Replaced, 0, T, "empty store");
    setCur("aaaaaa-1", 9, 0);
    setIn("aaaaaa-1", 1, hb);
    expectDecide(Result::Replaced, T, T, "empty store, lower rev");

    // Same session.
    setCur("aaaaaa-1", 5);
    setIn("aaaaaa-1", 6, hb);
    expectDecide(Result::Replaced, T, T + 1000, "same session, higher rev");
    setIn("aaaaaa-1", 5, hb);
    expectDecide(Result::Touched, T, T + 1000, "same session, same rev");
    expectDecide(Result::Touched, T, T + 10 * STALE_MS, "same session, same rev, stale");
    setIn("aaaaaa-1", 4, hb);
    expectDecide(Result::Old, T, T + 1000, "same session, lower rev");
    expectDecide(Result::Old, T, T + 10 * STALE_MS, "same session, lower rev, stale");
  }

  // Other session, a real summary: last writer wins, fresh or not.
  setCur("aaaaaa-1", 5);
  setIn("bbbbbb-1", 1, false);
  expectDecide(Result::Replaced, T, T + 1000, "other session, summary");
  setIn("aaaaaa-2", 1, false);   // a new take in the same page
  expectDecide(Result::Replaced, T, T + 1000, "new take, summary");

  // Other session, heartbeat: ignored while the current one is fresh...
  setIn("bbbbbb-1", 50, true);
  expectDecide(Result::Ignored, T, T, "other session, heartbeat, just seen");
  expectDecide(Result::Ignored, T, T + STALE_MS - 1, "other session, heartbeat, just under stale");
  expectDecide(Result::Ignored, T, T + STALE_MS, "other session, heartbeat, exactly STALE_MS");
  // ... and takes over once it's stale (over STALE_MS since the last accept).
  expectDecide(Result::Replaced, T, T + STALE_MS + 1, "stale takeover");
  expectDecide(Result::Replaced, T, T + 600000, "stale takeover, long gone");
  expectDecide(Result::Replaced, 0, T, "never seen counts as stale");
  // Across millis() wrapping.
  expectDecide(Result::Ignored, 0xFFFFF000u, 0x00001000u, "fresh across wrap");
  expectDecide(Result::Replaced, 0xFFFFF000u, 0xFFFFF000u + STALE_MS + 5, "stale across wrap");

  // Session compare is exact.
  setCur("aaaaaa-1", 5);
  setIn("aaaaaa-10", 5, true);
  expectDecide(Result::Ignored, T, T, "prefix session is another session");
}

static void testResultName() {
  CHECK(!strcmp(resultName(Result::Replaced), "replaced"), "replaced");
  CHECK(!strcmp(resultName(Result::Touched), "touched"), "touched");
  CHECK(!strcmp(resultName(Result::Ignored), "ignored"), "ignored");
  CHECK(!strcmp(resultName(Result::Old), "old"), "old");
  CHECK(!strcmp(secName(Sec::Key), "key") && !strcmp(secName(Sec::Questions), "questions") &&
        !strcmp(secName(Sec::Summary), "summary"), "secName");
  CHECK(!strcmp(kindName(Kind::New), "new") && !strcmp(kindName(Kind::Summary), "summary"), "kindName");
}

int main() {
  testDemo();
  testSanitize();
  testFixture();
  testRejects();
  testTruncation();
  testUtf8AndControl();
  testLists();
  testDecide();
  testResultName();
  return report("meeting_tests");
}
