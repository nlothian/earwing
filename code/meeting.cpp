// See meeting.h. The store is one Snapshot in PSRAM behind a mutex; the
// handlers (ports 80 and 443, two tasks, possibly at once) parse into scratch
// memory of their own and take the lock only to decide and memcpy, and the
// UI (loop(), core 1) takes it only to memcpy out. Nothing slow happens under
// the lock.
//
// Memory: the server tasks' stacks are small (6 KB on port 80), so the body,
// the Parsed result and /meeting.json's snapshot and output are all per
// request, in PSRAM, freed before the handler returns. cJSON's nodes go to
// PSRAM too (cJSON_InitHooks), so a POST costs no internal RAM beyond a few
// hundred bytes of stack.
//
// With -DMEETING_DEMO the UI side (generation(), copy(), lastSeenMs()) serves
// the canned demo sequence (meeting_demo.h), a new step every DEMO_STEP_MS,
// so the Meeting tab can be tried without a browser. POST /meeting still
// parses and stores as usual, and /meeting.json shows that store (with
// "demo":true), not the demo.
#include "meeting.h"

#include <Arduino.h>
#include <esp_heap_caps.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <stdarg.h>

#include "cJSON.h"
#include "meeting_demo.h"

namespace meeting {

namespace {

// Receive timeouts (the server's 5 s on port 80, 3 s on 443) allowed while a
// body is arriving before the request is given up.
const int RECV_RETRIES = 3;
// A refused body up to this size is left for the server to drain, so the
// client reads the error; past it the socket is closed.
const size_t DRAIN_MAX = 4 * MAX_BODY;

SemaphoreHandle_t lock = nullptr;
Snapshot* store = nullptr;               // PSRAM; store->gen == gen
volatile uint32_t gen = 0;               // lock-free reads by the UI
volatile uint32_t lastSeen = 0;          // millis() of the last replaced/touched post

void* psramMalloc(size_t n) {
  return heap_caps_malloc_prefer(n, 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
}

struct Lock {
  Lock() { xSemaphoreTake(lock, portMAX_DELAY); }
  ~Lock() { xSemaphoreGive(lock); }
};

// A JSON reply of the {"ok":false,"error":...} kind. `reason` is one of
// parse()'s static strings or ours: no escaping needed.
void fail(web::Req& q, int code, const char* reason, bool close = false) {
  char buf[96];
  const int n = snprintf(buf, sizeof(buf), "{\"ok\":false,\"error\":\"%s\"}\n", reason);
  q.send(code, "application/json", buf, n);
  // Otherwise the server reads and discards the unread body after the
  // handler, which is fine for a modest one (and lets the client see the
  // response); a huge one closes the socket instead.
  if (close) q.abort();
}

// printf / JSON strings into a fixed buffer; `over` once anything didn't fit.
struct Out {
  char*  p;
  size_t n, cap;
  bool   over;

  void f(const char* fmt, ...) __attribute__((format(printf, 2, 3))) {
    if (over) return;
    va_list ap;
    va_start(ap, fmt);
    const int w = vsnprintf(p + n, cap - n, fmt, ap);
    va_end(ap);
    if (w < 0 || (size_t)w >= cap - n) { over = true; return; }
    n += w;
  }

  void c(char ch) {
    if (over || n + 1 >= cap) { over = true; return; }
    p[n++] = ch;
    p[n] = 0;
  }

  // A quoted, escaped JSON string. The store's text is printable ASCII
  // (sanitize()), so only " and \ really occur; anything else gets \u00XX.
  void s(const char* str) {
    c('"');
    for (const unsigned char* q = (const unsigned char*)str; *q && !over; q++) {
      if (*q == '"' || *q == '\\') { c('\\'); c((char)*q); }
      else if (*q < 0x20 || *q >= 0x7F) f("\\u%04x", *q);
      else c((char)*q);
    }
    c('"');
  }
};

}  // namespace

void begin() {
  if (lock) return;
  lock = xSemaphoreCreateMutex();
  store = (Snapshot*)heap_caps_calloc(1, sizeof(Snapshot), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  // Global, and the prebuilt json component is the only cJSON user linked
  // in; heap_caps_free() frees from either heap, so anything cJSON allocated
  // before this is still freed correctly.
  cJSON_Hooks hooks = {psramMalloc, heap_caps_free};
  cJSON_InitHooks(&hooks);
}

void postHandler(web::Req& q) {
  httpd_req_t* r = q.raw();
  const size_t len = r->content_len;
  const bool huge = len > DRAIN_MAX;   // an error closes the socket rather than drain this
  if (!lock || !store) { fail(q, 503, "meeting store unavailable", huge); return; }

  // The media type exactly, parameters (charset) aside: "text/plain;
  // x=application/json" is a CORS-simple request any page could send, so a
  // substring match would give up the preflight this relies on.
  String type = q.header("Content-Type");
  const int semi = type.indexOf(';');
  if (semi >= 0) type.remove(semi);
  type.trim();
  type.toLowerCase();
  if (type != "application/json") { fail(q, 415, "Content-Type must be application/json", huge); return; }
  if (len == 0) { fail(q, 400, "empty body", huge); return; }
  if (len > MAX_BODY) { fail(q, 413, "body over 16384 bytes", huge); return; }

  char* body = (char*)heap_caps_malloc(len + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  Parsed* in = (Parsed*)heap_caps_malloc(sizeof(Parsed), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!body || !in) {
    heap_caps_free(body);
    heap_caps_free(in);
    fail(q, 503, "out of memory", true);
    return;
  }

  size_t got = 0;
  int timeouts = 0;
  while (got < len) {
    const int n = httpd_req_recv(r, body + got, len - got);
    if (n == HTTPD_SOCK_ERR_TIMEOUT && ++timeouts <= RECV_RETRIES) continue;
    if (n <= 0) break;
    got += n;
  }
  if (got < len) {
    heap_caps_free(body);
    heap_caps_free(in);
    fail(q, 400, "body incomplete", true);
    return;
  }
  body[len] = 0;

  const char* err = nullptr;
  const bool ok = parse(body, len, in, &err);
  heap_caps_free(body);
  if (!ok) {
    heap_caps_free(in);
    fail(q, 400, err ? err : "bad request");
    return;
  }

  Result res;
  char session[SESSION_MAX + 1];
  uint32_t rev;
  {
    Lock l;
    const uint32_t now = millis();
    res = decide(*store, lastSeen, *in, now);
    if (res == Result::Replaced) {
      const bool first = store->gen == 0 || strcmp(store->session, in->s.session) != 0;
      const uint32_t g = gen + 1 ? gen + 1 : 1;   // 0 means empty
      memcpy(store, &in->s, sizeof(Snapshot));
      store->gen = g;
      store->recvMs = now;
      store->firstOfSession = first;
      gen = g;
    }
    if (res == Result::Replaced || res == Result::Touched) lastSeen = now ? now : 1;
    memcpy(session, store->session, sizeof(session));
    rev = store->rev;
  }
  heap_caps_free(in);

  // The session is sanitize()d, but may still hold a quote or backslash.
  char buf[128];
  Out o = {buf, 0, sizeof(buf), false};
  buf[0] = 0;
  o.f("{\"ok\":true,\"result\":\"%s\",\"session\":", resultName(res));
  o.s(session);
  o.f(",\"rev\":%lu}\n", (unsigned long)rev);
  q.send(200, "application/json", buf, o.n);
}

void jsonHandler(web::Req& q) {
  if (!lock || !store) { fail(q, 503, "meeting store unavailable"); return; }
  // Every string doubled (escapes) plus the structure: comfortably enough.
  const size_t cap = 2 * sizeof(Snapshot) + 4096;
  Snapshot* s = (Snapshot*)heap_caps_malloc(sizeof(Snapshot), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  char* buf = (char*)heap_caps_malloc(cap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!s || !buf) {
    heap_caps_free(s);
    heap_caps_free(buf);
    fail(q, 503, "out of memory");
    return;
  }
  uint32_t seen;
  {
    Lock l;
    memcpy(s, store, sizeof(Snapshot));
    seen = lastSeen;
  }
  const uint32_t now = millis();

  Out o = {buf, 0, cap, false};
  buf[0] = 0;
  o.f("{\"gen\":%lu,", (unsigned long)s->gen);
#ifdef MEETING_DEMO
  o.f("\"demo\":true,");
#endif
  if (seen) o.f("\"lastSeenAgoS\":%lu", (unsigned long)((now - seen) / 1000));
  else o.f("\"lastSeenAgoS\":null");
  if (s->gen) {
    const uint32_t ageS = (now - s->recvMs) / 1000;
    o.f(",\"recvAgoS\":%lu,\"firstOfSession\":%s,\"session\":", (unsigned long)ageS,
        s->firstOfSession ? "true" : "false");
    o.s(s->session);
    o.f(",\"rev\":%lu,\"coversTo\":", (unsigned long)s->rev);
    o.s(s->coversTo);
    o.f(",\"summary\":");
    o.s(s->summary);
    o.f(",\"sections\":{");
    for (int l = 0; l < LIST_COUNT; l++) {
      o.f("%s\"%s\":[", l ? "," : "", secName((Sec)l));
      for (int i = 0; i < s->count[l] && i < ITEMS_MAX; i++) {
        const Item& it = s->items[l][i];
        o.f("%s{\"text\":", i ? "," : "");
        o.s(it.text);
        o.f(",\"t\":");
        o.s(it.t);
        o.f(",\"new\":%s}", it.fresh ? "true" : "false");
      }
      o.c(']');
    }
    o.f("},\"feed\":[");
    for (int i = 0; i < s->feedCount && i < FEED_MAX; i++) {
      const FeedEntry& e = s->feed[i];
      o.f("%s{\"sec\":\"%s\",\"kind\":\"%s\",\"text\":", i ? "," : "", secName(e.sec), kindName(e.kind));
      o.s(e.text);
      o.f(",\"t\":");
      o.s(e.t);
      // "ago" as the POST has it, but as of now.
      o.f(",\"lvl\":%u,\"ago\":%lu}", e.lvl, (unsigned long)(e.agoS + ageS));
    }
    o.c(']');
  }
  o.f("}\n");
  heap_caps_free(s);

  if (o.over) fail(q, 500, "snapshot too large to show");
  else q.send(200, "application/json", buf, o.n);
  heap_caps_free(buf);
}

uint32_t lastSeenMs() {
#ifdef MEETING_DEMO
  return millis() - (millis() % DEMO_STEP_MS);
#else
  return lastSeen;
#endif
}

#ifdef MEETING_DEMO
uint32_t generation() { return 1 + millis() / DEMO_STEP_MS; }

bool copy(Snapshot* out, uint32_t haveGen) {
  const uint32_t g = generation();
  if (g == haveGen) return false;
  out->gen = g;
  out->recvMs = millis();
  out->firstOfSession = ((g - 1) % DEMO_STEPS) == 0;
  demoFill(out, g - 1);
  return true;
}
#else
uint32_t generation() { return gen; }

bool copy(Snapshot* out, uint32_t haveGen) {
  if (!lock || !store || gen == haveGen) return false;
  Lock l;
  memcpy(out, store, sizeof(Snapshot));
  return true;
}
#endif

}  // namespace meeting
