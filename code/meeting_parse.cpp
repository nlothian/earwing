// See meeting_types.h: the POST body parser (cJSON, from ESP-IDF's json
// component on the device and its cJSON.c in the host tests) and the accept
// policy. Pure C++, no Arduino.
//
// Stack: this runs in the HTTP server tasks (6 KB on port 80), so nothing big
// lives on the stack; the caller passes *out in PSRAM. cJSON's parser recurses
// once per nesting level and allows 1000 levels, far more than 6 KB holds, so
// the body's nesting is checked before cJSON sees it (the format needs 4).
#include "meeting_types.h"

#include <string.h>

#include "cJSON.h"

namespace meeting {

namespace {

// Root object > "sections" > a list > an item: 4. A little slack for fields a
// newer site might add.
const int MAX_DEPTH = 8;

// Deepest [ / { nesting outside strings; stops counting past MAX_DEPTH.
int nesting(const char* p, size_t len) {
  int depth = 0, deepest = 0;
  bool str = false, esc = false;
  for (size_t i = 0; i < len; i++) {
    const char c = p[i];
    if (str) {
      if (esc) esc = false;
      else if (c == '\\') esc = true;
      else if (c == '"') str = false;
    } else if (c == '"') {
      str = true;
    } else if (c == '[' || c == '{') {
      if (++depth > deepest) deepest = depth;
      if (deepest > MAX_DEPTH) return deepest;
    } else if (c == ']' || c == '}') {
      if (depth) depth--;
    }
  }
  return deepest;
}

bool isSpace(char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; }

// sanitize() from src into dst, keeping at most maxChars characters (dst has
// maxChars + 1 bytes). dst may be src: the output never gets ahead of the
// input. Truncating after cleaning means a multi-byte character costs one
// character of the cap, not up to four, and a cut never leaves a trailing
// space.
void clean(char* dst, size_t maxChars, const char* src) {
  const unsigned char* p = (const unsigned char*)src;
  size_t n = 0;
  bool space = false;   // a whitespace run is pending, written only before the next character
  while (*p) {
    unsigned char c = *p;
    if (c >= 0x80) {
      // One '?' for a lead byte and the continuation bytes it announces (as
      // many as are there); a stray continuation or invalid byte is its own.
      size_t len = 1, want = c >= 0xF0 ? 4 : c >= 0xE0 ? 3 : c >= 0xC0 ? 2 : 1;
      if (c >= 0xF8) want = 1;
      while (len < want && (p[len] & 0xC0) == 0x80) len++;
      p += len;
      c = '?';
    } else if (c <= 0x20 || c == 0x7F) {
      p++;
      space = true;
      continue;
    } else {
      p++;
    }
    const size_t need = (space && n) ? 2 : 1;
    if (n + need > maxChars) break;
    if (need == 2) dst[n++] = ' ';
    dst[n++] = (char)c;
    space = false;
  }
  dst[n] = 0;
}

// A string field into a buffer of cap + 1 bytes; "" if absent or not a string.
void getStr(const cJSON* obj, const char* key, char* dst, size_t cap) {
  const cJSON* v = cJSON_GetObjectItemCaseSensitive(obj, key);
  if (cJSON_IsString(v) && v->valuestring) clean(dst, cap, v->valuestring);
  else dst[0] = 0;
}

// A non-negative integer field, clamped to uint32; def if absent or not a number.
uint32_t getU32(const cJSON* obj, const char* key, uint32_t def) {
  const cJSON* v = cJSON_GetObjectItemCaseSensitive(obj, key);
  if (!cJSON_IsNumber(v)) return def;
  const double d = v->valuedouble;
  if (!(d > 0)) return 0;   // negative and NaN
  if (d >= 4294967295.0) return 0xFFFFFFFFu;
  return (uint32_t)d;
}

// The wire names, in enum order (the first LIST_COUNT are the section keys).
const char* const SEC_NAMES[]  = {"key", "decisions", "actions", "questions", "summary"};
const char* const KIND_NAMES[] = {"new", "edited", "summary"};

bool secFromName(const char* s, Sec* out) {
  for (int i = 0; i < (int)(sizeof(SEC_NAMES) / sizeof(*SEC_NAMES)); i++)
    if (!strcmp(s, SEC_NAMES[i])) { *out = (Sec)i; return true; }
  return false;
}

bool kindFromName(const char* s, Kind* out) {
  for (int i = 0; i < (int)(sizeof(KIND_NAMES) / sizeof(*KIND_NAMES)); i++)
    if (!strcmp(s, KIND_NAMES[i])) { *out = (Kind)i; return true; }
  return false;
}

// Short enum-ish strings ("decisions", "edited") straight from the node.
const char* rawStr(const cJSON* obj, const char* key) {
  const cJSON* v = cJSON_GetObjectItemCaseSensitive(obj, key);
  return cJSON_IsString(v) && v->valuestring ? v->valuestring : nullptr;
}

void parseList(const cJSON* arr, Item* items, uint8_t* count) {
  *count = 0;
  const cJSON* e;
  cJSON_ArrayForEach(e, arr) {
    if (*count >= ITEMS_MAX) break;
    if (!cJSON_IsObject(e)) continue;
    Item& it = items[*count];
    getStr(e, "text", it.text, TEXT_MAX);
    if (!it.text[0]) continue;   // nothing to draw
    getStr(e, "t", it.t, T_MAX);
    it.fresh = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(e, "new"));
    (*count)++;
  }
}

void parseFeed(const cJSON* arr, Snapshot* s) {
  s->feedCount = 0;
  const cJSON* e;
  cJSON_ArrayForEach(e, arr) {
    if (s->feedCount >= FEED_MAX) break;
    if (!cJSON_IsObject(e)) continue;
    const char* sec = rawStr(e, "sec");
    const char* kind = rawStr(e, "kind");
    FeedEntry& f = s->feed[s->feedCount];
    if (!sec || !kind || !secFromName(sec, &f.sec) || !kindFromName(kind, &f.kind)) continue;
    getStr(e, "text", f.text, FEED_TEXT_MAX);
    if (!f.text[0]) continue;
    getStr(e, "t", f.t, T_MAX);
    const uint32_t lvl = getU32(e, "lvl", 0);
    f.lvl = (uint8_t)(lvl > 3 ? 3 : lvl);
    f.agoS = getU32(e, "ago", 0);
    s->feedCount++;
  }
}

}  // namespace

const char* secName(Sec s) { return SEC_NAMES[(int)s]; }
const char* kindName(Kind k) { return KIND_NAMES[(int)k]; }

const char* resultName(Result r) {
  switch (r) {
    case Result::Replaced: return "replaced";
    case Result::Touched:  return "touched";
    case Result::Ignored:  return "ignored";
    case Result::Old:      return "old";
  }
  return "";
}

void sanitize(char* s) {
  if (s) clean(s, strlen(s), s);
}

bool parse(const char* body, size_t len, Parsed* out, const char** err) {
  const char* dummy;
  if (!err) err = &dummy;
  memset(out, 0, sizeof(*out));
  if (!body || !len) { *err = "empty body"; return false; }
  if (nesting(body, len) > MAX_DEPTH) { *err = "nested too deeply"; return false; }

  const char* end = nullptr;
  cJSON* root = cJSON_ParseWithLengthOpts(body, len, &end, false);
  if (!root) { *err = "bad JSON"; return false; }
  // Nothing but whitespace (or the caller's NUL) after the value.
  for (const char* p = end; p && p < body + len && *p; p++)
    if (!isSpace(*p)) { cJSON_Delete(root); *err = "bad JSON: trailing data"; return false; }

  bool ok = false;
  Snapshot& s = out->s;
  do {
    if (!cJSON_IsObject(root)) { *err = "not a JSON object"; break; }
    const cJSON* v = cJSON_GetObjectItemCaseSensitive(root, "v");
    if (!cJSON_IsNumber(v) || v->valuedouble != 1) { *err = "v must be 1"; break; }
    getStr(root, "session", s.session, SESSION_MAX);
    if (!s.session[0]) { *err = "missing session"; break; }

    s.rev = getU32(root, "rev", 0);
    out->seq = getU32(root, "seq", 0);
    out->hb = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(root, "hb"));
    getStr(root, "coversTo", s.coversTo, T_MAX);
    getStr(root, "summary", s.summary, SUMMARY_MAX);

    const cJSON* sections = cJSON_GetObjectItemCaseSensitive(root, "sections");
    for (int i = 0; i < LIST_COUNT; i++) {
      const cJSON* list = cJSON_IsObject(sections) ? cJSON_GetObjectItemCaseSensitive(sections, SEC_NAMES[i]) : nullptr;
      if (cJSON_IsArray(list)) parseList(list, s.items[i], &s.count[i]);
    }
    const cJSON* feed = cJSON_GetObjectItemCaseSensitive(root, "feed");
    if (cJSON_IsArray(feed)) parseFeed(feed, &s);
    ok = true;
  } while (false);

  cJSON_Delete(root);
  return ok;
}

Result decide(const Snapshot& cur, uint32_t lastSeenMs, const Parsed& in, uint32_t nowMs) {
  if (cur.gen == 0) return Result::Replaced;
  if (!strcmp(cur.session, in.s.session)) {
    if (in.s.rev > cur.rev) return Result::Replaced;
    if (in.s.rev == cur.rev) return Result::Touched;
    return Result::Old;
  }
  if (!in.hb) return Result::Replaced;
  // Unsigned subtraction: right across millis() wrapping.
  const bool stale = lastSeenMs == 0 || (uint32_t)(nowMs - lastSeenMs) > STALE_MS;
  return stale ? Result::Replaced : Result::Ignored;
}

}  // namespace meeting
