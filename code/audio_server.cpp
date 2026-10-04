#include "audio_server.h"

#include <Arduino.h>
#include <esp_heap_caps.h>
#include <freertos/queue.h>
#include <lwip/sockets.h>
#include <string.h>
#include "audio_engine.h"
#include "web_server.h"

namespace audio_server {

using namespace audio;

static const int MAX_RAW_SECONDS = 20;       // array: 20 s × 16 kHz × 12 bytes = 3.8 MB of PSRAM
static const int STREAM_BATCH_HOPS = 4;      // hops per socket write (64 ms): a quarter of the
                                             // packets (and TLS records) of one per hop
static const int SLOTS = MAX_BEAM_STREAMS + 1;   // + the one /raw.wav
static const uint32_t REQUEST_TIMEOUT_MS = 2000; // port 81: the whole request, from accept
static const size_t MAX_REQUEST = 1024;          // port 81: request line + headers we look at
static const int SEND_TIMEOUT_S = 5;             // port 81; the web servers set their own (also 5)

// Core 0 with WiFi. The workers sit above the web servers (4), so a TLS
// handshake or a file send can't hold a stream past its ring's ~450 ms of
// slack; they only ever wait on sockets and the ring. The listener is below
// both. PSRAM stacks, which is fine as none of them touches flash, and costs
// no internal RAM, so they're generous: with lwIP's core locking a send()
// runs TCP, IP and the WiFi driver's transmit on the caller's stack (4 KB
// overflowed on the listener's first 404), and a worker adds TLS on top.
static const UBaseType_t WORKER_PRIO = 5, LISTENER_PRIO = 3;
static const uint32_t WORKER_STACK = 10240, LISTENER_STACK = 8192;

// On every response: any origin may read it, and a COEP require-corp page
// may load it.
static const char CORS_HEADERS[] =
  "Access-Control-Allow-Origin: *\r\nCross-Origin-Resource-Policy: cross-origin\r\n";

// ---------- where a response goes ----------
// A request on 80/443 (after httpd_req_async_handler_begin, when a worker
// has it) or a port-81 socket. httpd_send() goes through the session's
// transport, so the same bytes come out encrypted on 443.
struct Sink {
  httpd_req_t* req;   // null: port 81
  int fd;
};

static bool sinkWrite(const Sink& s, const void* p, size_t n) {
  const char* c = (const char*)p;
  while (n) {
    const int w = s.req ? httpd_send(s.req, c, n) : send(s.fd, c, n, 0);
    if (w <= 0) return false;   // gone, reset, or stuck for the send timeout
    c += w;
    n -= w;
  }
  return true;
}

// A short text answer. On 80/443 from the server task, the connection stays
// open for the next request; port 81 always closes.
static void sinkText(const Sink& s, int code, const char* body) {
  char buf[1024];
  const int blen = strlen(body) + 1;   // + the newline
  const int n = snprintf(buf, sizeof(buf), "HTTP/1.1 %d %s\r\nContent-Type: text/plain\r\nContent-Length: %d\r\n%s%s\r\n%s\n",
                         code, web::statusText(code), blen, CORS_HEADERS, s.req ? "" : "Connection: close\r\n", body);
  sinkWrite(s, buf, n < (int)sizeof(buf) ? n : sizeof(buf) - 1);
}

// CORS preflight. A plain fetch() of a WAV never needs one; a request with
// custom headers (or a dev server's fetch wrapper) might.
static void sinkPreflight(const Sink& s) {
  char buf[256];
  const int n = snprintf(buf, sizeof(buf), "HTTP/1.1 204 No Content\r\n%sAccess-Control-Allow-Methods: GET\r\n"
                         "Access-Control-Allow-Headers: *\r\nAccess-Control-Max-Age: 86400\r\n%s\r\n",
                         CORS_HEADERS, s.req ? "" : "Connection: close\r\n");
  sinkWrite(s, buf, n);
}

// The end of a stream's connection (every stream response says Connection:
// close). On 80/443: shut the socket down while the worker still owns the
// request, then give the request back; the server sees the dead socket on
// its next select() and deletes the session itself. Closing it from here
// instead (httpd_sess_trigger_close) could, after the hand-back, hit a new
// session that got the same fd number.
static void sinkEnd(const Sink& s) {
  if (s.req) {
    shutdown(httpd_req_to_sockfd(s.req), SHUT_RDWR);
    httpd_req_async_handler_complete(s.req);
  } else {
    close(s.fd);
  }
}

// ---------- slots ----------
enum class Kind : uint8_t { Beam, Raw };

struct Slot {
  bool           used;
  Kind           kind;
  httpd_handle_t server;       // null: port 81
  volatile bool  abort;        // web_server is stopping `server`
  int16_t*       buf;          // PSRAM, STREAM_BATCH_HOPS × HOP
};

// The web servers streams may run on, and how many sockets each has.
struct Server {
  httpd_handle_t h;
  int            sockets;
  bool           closing;
};

static portMUX_TYPE lock = portMUX_INITIALIZER_UNLOCKED;   // guards slots[] and servers[]
static Slot slots[SLOTS];
static Server servers[2];
static QueueHandle_t jobs;

struct Job {
  int  slot;
  int  param;   // Beam: source (−1 beam, 0..3 mic); Raw: seconds
  Sink sink;
};

// Reserve a slot for a stream on `server` (null: port 81): null and *slot
// set, or why not. Nothing is sent from in here: it's a spinlock.
static const char* reserve(Kind kind, httpd_handle_t server, int* slot) {
  const char* why = nullptr;
  portENTER_CRITICAL(&lock);
  int beams = 0, raws = 0, here = 0, free = -1;
  for (int i = 0; i < SLOTS; i++) {
    if (!slots[i].used) { if (free < 0 && slots[i].buf) free = i; continue; }   // no buf: PSRAM ran out at boot
    (slots[i].kind == Kind::Beam ? beams : raws)++;
    if (slots[i].server == server) here++;
  }
  int cap = PORT81_SOCKETS - 1;
  if (server) {
    cap = 0;
    for (auto& s : servers)
      if (s.h == server) cap = s.closing ? 0 : s.sockets - 1;
  }
  if (kind == Kind::Beam && beams >= MAX_BEAM_STREAMS) why = "too many audio streams";
  else if (kind == Kind::Raw && raws) why = "capture busy: another /raw.wav is running";
  else if (here >= cap || free < 0) why = "too many audio streams on this port";
  else {
    slots[free].used = true;
    slots[free].kind = kind;
    slots[free].server = server;
    slots[free].abort = false;
    *slot = free;
  }
  portEXIT_CRITICAL(&lock);
  return why;
}

static void release(int slot) {
  portENTER_CRITICAL(&lock);
  slots[slot].used = false;
  portEXIT_CRITICAL(&lock);
}

// ---------- the endpoints ----------
// Canonical 44-byte PCM WAV header.
static void wavHeader(uint8_t* h, int channels, int bits, uint32_t dataBytes) {
  const uint32_t byteRate = SAMPLE_RATE * channels * bits / 8;
  const uint16_t align = channels * bits / 8;
  auto u32 = [&](int at, uint32_t v) { for (int i = 0; i < 4; i++) h[at + i] = (uint8_t)(v >> (8 * i)); };
  auto u16 = [&](int at, uint16_t v) { h[at] = (uint8_t)v; h[at + 1] = (uint8_t)(v >> 8); };
  memcpy(h, "RIFF", 4);
  u32(4, 36 + dataBytes);
  memcpy(h + 8, "WAVEfmt ", 8);
  u32(16, 16);
  u16(20, 1);   // PCM
  u16(22, (uint16_t)channels);
  u32(24, SAMPLE_RATE);
  u32(28, byteRate);
  u16(32, align);
  u16(34, (uint16_t)bits);
  memcpy(h + 36, "data", 4);
  u32(40, dataBytes);
}

// Raw bytes after our own headers, no chunked encoding: exactly what port 81
// always sent, so players, curl and fetch() readers see the same stream on
// every port, and there is no per-write chunk framing to add or strip. No
// Content-Length (it's endless), hence Connection: close.
static void streamWav(const Sink& s, int source, Slot& slot) {
  char head[320];
  int n = snprintf(head, sizeof(head), "HTTP/1.1 200 OK\r\nContent-Type: audio/wav\r\nCache-Control: no-store\r\n"
                   "%sConnection: close\r\n\r\n", CORS_HEADERS);
  // Endless stream: a data size near 2 GB (not 0xFFFFFFFF, which some
  // readers treat as signed) so players just keep going.
  wavHeader((uint8_t*)head + n, 1, 16, 0x7FFF0000u);
  if (!sinkWrite(s, head, n + 44)) return;
  uint32_t seq = engine::streamHead();   // this client's own position in the ring
  int have = 0;
  while (!slot.abort) {
    while (have < STREAM_BATCH_HOPS && engine::streamRead(&seq, source, slot.buf + have * HOP)) have++;
    if (have < STREAM_BATCH_HOPS) { vTaskDelay(pdMS_TO_TICKS(16)); continue; }   // a hop is 16 ms
    if (!sinkWrite(s, slot.buf, STREAM_BATCH_HOPS * HOP * sizeof(int16_t))) break;   // client gone or stuck
    have = 0;
  }
}

static void serveRaw(const Sink& s, int seconds, Slot& slot) {
  if (seconds < 1) seconds = 1;
  if (seconds > MAX_RAW_SECONDS) seconds = MAX_RAW_SECONDS;
  // Four channels from the array, one from the board mic. The engine checks
  // the count again when arming, in case the source changed in between.
  const int channels = engine::rawChannels();
  const uint32_t frames = (uint32_t)seconds * SAMPLE_RATE;
  const uint32_t bytes = frames * channels * 3;
  // Capture to RAM first: WiFi hiccups can't disturb the timing that way.
  uint8_t* buf = (uint8_t*)heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM);
  if (!buf) { sinkText(s, 503, "not enough PSRAM"); return; }
  if (!engine::rawCaptureBegin(buf, frames, channels)) {
    heap_caps_free(buf);
    sinkText(s, 503, "capture busy, or the mic source is switching");
    return;
  }
  uint32_t resyncs = 0;
  const uint32_t deadline = millis() + seconds * 1000 + 3000;
  for (;;) {
    const engine::RawState rs = engine::rawCaptureState(&resyncs);
    if (rs == engine::RawState::Done) break;
    if (rs == engine::RawState::Aborted) {
      // The capture task dropped it for a source switch and has stopped
      // writing, so the buffer is ours again.
      heap_caps_free(buf);
      sinkText(s, 409, "the mic source switched during the capture; try again");
      return;
    }
    if (slot.abort || (int32_t)(millis() - deadline) > 0) {
      engine::rawCaptureAbort();   // then the capture task has let go of buf
      heap_caps_free(buf);
      if (!slot.abort)
        sinkText(s, 504, "capture did not complete: is the I2S capture running? see /status");
      return;
    }
    vTaskDelay(pdMS_TO_TICKS(20));
  }
  // A resync mid-capture means a discontinuity in the file; say so, readably
  // from another origin too.
  char head[512];
  int n = snprintf(head, sizeof(head), "HTTP/1.1 200 OK\r\nContent-Type: audio/wav\r\nContent-Length: %lu\r\n"
                   "Content-Disposition: attachment; filename=\"earwing-raw.wav\"\r\n"
                   "X-Earwing-Resyncs-During-Capture: %lu\r\nX-Earwing-Source: %s\r\n"
                   "%sAccess-Control-Expose-Headers: X-Earwing-Resyncs-During-Capture, X-Earwing-Source\r\n"
                   "Connection: close\r\n\r\n",
                   (unsigned long)(44 + bytes), (unsigned long)resyncs,
                   engine::sourceName(channels == 1 ? engine::Source::Inbuilt : engine::Source::Array), CORS_HEADERS);
  wavHeader((uint8_t*)head + n, channels, 24, bytes);
  if (sinkWrite(s, head, n + 44)) {
    for (uint32_t at = 0; at < bytes && !slot.abort;) {
      const uint32_t len = bytes - at < 4096 ? bytes - at : 4096;
      if (!sinkWrite(s, buf + at, len)) break;
      at += len;
    }
  }
  heap_caps_free(buf);
}

static void worker(void*) {
  Job j;
  for (;;) {
    if (xQueueReceive(jobs, &j, portMAX_DELAY) != pdTRUE) continue;
    Slot& slot = slots[j.slot];
    if (slot.kind == Kind::Beam) streamWav(j.sink, j.param, slot);
    else serveRaw(j.sink, j.param, slot);
    sinkEnd(j.sink);
    release(j.slot);   // after sinkEnd: streamsOn() must not reach 0 while we hold a request
  }
}

// "?a=1&s=5" → 5 for "s"; `def` if absent.
static int queryInt(const char* uri, const char* key, int def) {
  const char* q = strchr(uri, '?');
  if (!q) return def;
  const size_t k = strlen(key);
  for (const char* p = q + 1; *p;) {
    if (!strncmp(p, key, k) && p[k] == '=') return atoi(p + k + 1);
    const char* amp = strchr(p, '&');
    if (!amp) break;
    p = amp + 1;
  }
  return def;
}

// Which stream a path asks for; false if it's neither.
static bool parseStream(const char* uri, Kind* kind, int* param) {
  const size_t len = strcspn(uri, "?");
  if (len == 9 && !strncmp(uri, "/beam.wav", 9)) {
    const int mic = queryInt(uri, "mic", 0);   // 1-based geometry row, 0 = beam
    *kind = Kind::Beam;
    *param = mic >= 1 && mic <= NUM_MICS ? mic - 1 : -1;
    return true;
  }
  if (len == 8 && !strncmp(uri, "/raw.wav", 8)) {
    *kind = Kind::Raw;
    *param = queryInt(uri, "s", 5);
    return true;
  }
  return false;
}

// ---------- on the web servers ----------
// Runs in the server's task: decide, reserve, hand over, return. Only the
// quick answers (preflight, 503) are sent from here.
static esp_err_t httpdHandler(httpd_req_t* r) {
  const Sink now = {r, -1};
  if (r->method == HTTP_OPTIONS) { sinkPreflight(now); return ESP_OK; }
  Kind kind;
  int param, slot;
  if (!parseStream(r->uri, &kind, &param)) return ESP_FAIL;   // can't happen: registered per path
  if (const char* why = reserve(kind, r->handle, &slot)) {
    sinkText(now, 503, why);
    return ESP_OK;
  }
  httpd_req_t* async = nullptr;
  if (httpd_req_async_handler_begin(r, &async) != ESP_OK) {
    release(slot);
    sinkText(now, 503, "out of memory");
    return ESP_OK;
  }
  const Job j = {slot, param, {async, -1}};
  xQueueSend(jobs, &j, portMAX_DELAY);   // never waits: one entry per slot, and the slot is ours
  return ESP_OK;
}

bool addRoutes(httpd_handle_t h, int sockets) {
  portENTER_CRITICAL(&lock);
  for (auto& s : servers)
    if (!s.h) { s.h = h; s.sockets = sockets; s.closing = false; break; }
  portEXIT_CRITICAL(&lock);
  bool ok = true;
  for (const char* uri : {"/beam.wav", "/raw.wav"})
    for (httpd_method_t m : {HTTP_GET, HTTP_OPTIONS}) {
      httpd_uri_t u = {};
      u.uri = uri;
      u.method = m;
      u.handler = httpdHandler;
      ok &= httpd_register_uri_handler(h, &u) == ESP_OK;
    }
  return ok;
}

void abortStreams(httpd_handle_t h) {
  portENTER_CRITICAL(&lock);
  for (auto& s : servers)
    if (s.h == h) s.closing = true;
  for (auto& s : slots)
    if (s.used && s.server == h) s.abort = true;
  portEXIT_CRITICAL(&lock);
}

int streamsOn(httpd_handle_t h) {
  int n = 0;
  portENTER_CRITICAL(&lock);
  for (auto& s : slots)
    if (s.used && s.server == h) n++;
  portEXIT_CRITICAL(&lock);
  return n;
}

void removeServer(httpd_handle_t h) {
  portENTER_CRITICAL(&lock);
  for (auto& s : servers)
    if (s.h == h) s = Server{};
  portEXIT_CRITICAL(&lock);
}

Counts counts() {
  Counts c = {};
  portENTER_CRITICAL(&lock);
  for (auto& s : slots)
    if (s.used) (s.kind == Kind::Beam ? c.beam : c.raw)++;
  portEXIT_CRITICAL(&lock);
  return c;
}

// ---------- port 81 ----------
// Read "METHOD /path?query HTTP/1.1" and the headers up to the blank line
// (ignored), within REQUEST_TIMEOUT_MS. False if the client didn't finish.
static bool readRequest(int fd, char* buf, size_t len) {
  const uint32_t deadline = millis() + REQUEST_TIMEOUT_MS;
  size_t got = 0;
  while ((int32_t)(millis() - deadline) < 0 && got < len - 1) {
    const int n = recv(fd, buf + got, len - 1 - got, 0);
    if (n == 0) return false;                          // closed
    if (n < 0) { if (errno == EAGAIN || errno == EWOULDBLOCK) continue; return false; }
    got += n;
    buf[got] = 0;
    if (strstr(buf, "\r\n\r\n") || strstr(buf, "\n\n")) return true;
  }
  return got > 0 && strchr(buf, '\n');   // headers too long to keep: the request line will do
}

static const char HELP[] =
  "earwing audio:\n"
  "  /beam.wav        live beam output (16 kHz, 16-bit mono)\n"
  "  /beam.wav?mic=N  raw mic N (1-4) instead, same gain and latency\n"
  "  /raw.wav?s=N     N seconds (1-20) of all 4 channels, 24-bit\n"
  "With the inbuilt source (the board mic) every stream is that mic,\n"
  "and /raw.wav is 1 channel. Any origin may fetch these (CORS). The same\n"
  "endpoints are on http://earwing.local/ and https://earwing.local/,\n"
  "which the transcriber web app uses; this port is kept for players.";

// One accepted socket: answer it, or hand it to a worker. Either way this
// task is done with it on return.
static void handleLegacy(int fd) {
  const Sink s = {nullptr, fd};
  char req[MAX_REQUEST];
  if (!readRequest(fd, req, sizeof(req))) { close(fd); return; }
  // Request line: METHOD SP target SP version
  char* sp1 = strchr(req, ' ');
  char* sp2 = sp1 ? strpbrk(sp1 + 1, " \r\n") : nullptr;
  if (!sp1 || !sp2) { close(fd); return; }
  *sp1 = 0;
  *sp2 = 0;
  const char* method = req;
  const char* uri = sp1 + 1;
  Kind kind;
  int param, slot;
  if (!strcmp(method, "OPTIONS")) {
    sinkPreflight(s);
  } else if (!parseStream(uri, &kind, &param)) {
    sinkText(s, 404, HELP);
  } else if (const char* why = reserve(kind, nullptr, &slot)) {
    sinkText(s, 503, why);
  } else {
    const Job j = {slot, param, s};
    xQueueSend(jobs, &j, portMAX_DELAY);
    return;
  }
  close(fd);
}

// Dual-stack, like esp_http_server's own listeners: an IPv6 socket bound to
// the any address takes IPv4 connections too in lwIP, and mDNS now hands out
// the link-local IPv6 address as well as the IPv4 one.
static int openListener() {
  const int ls = socket(AF_INET6, SOCK_STREAM, IPPROTO_IP);
  if (ls < 0) return -1;
  const int one = 1;
  setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
  sockaddr_in6 a = {};
  a.sin6_family = AF_INET6;
  a.sin6_port = htons(PORT);
  a.sin6_addr = in6addr_any;
  if (bind(ls, (sockaddr*)&a, sizeof(a)) < 0 || listen(ls, 2) < 0) {
    close(ls);
    return -1;
  }
  return ls;
}

// One connection at a time, and never more than PORT81_SOCKETS open: this
// task holds at most one while it reads a request, reserve() lets one more
// stream. Others wait in the backlog for the moment it takes.
static void listener(void*) {
  int ls = -1;
  for (;;) {
    if (ls < 0 && (ls = openListener()) < 0) { vTaskDelay(pdMS_TO_TICKS(1000)); continue; }
    const int fd = accept(ls, nullptr, nullptr);
    if (fd < 0) { vTaskDelay(pdMS_TO_TICKS(50)); continue; }
    const timeval rcv = {0, 200 * 1000}, snd = {SEND_TIMEOUT_S, 0};   // recv: short polls within the deadline
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &rcv, sizeof(rcv));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &snd, sizeof(snd));
    const int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    handleLegacy(fd);
  }
}

void begin() {
  jobs = xQueueCreate(SLOTS, sizeof(Job));
  for (int i = 0; i < SLOTS; i++) {
    slots[i].buf = (int16_t*)heap_caps_malloc(STREAM_BATCH_HOPS * HOP * sizeof(int16_t), MALLOC_CAP_SPIRAM);
    char name[12];
    snprintf(name, sizeof(name), "stream%d", i);
    xTaskCreatePinnedToCoreWithCaps(worker, name, WORKER_STACK, NULL, WORKER_PRIO, NULL, 0,
                                    MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  }
  xTaskCreatePinnedToCoreWithCaps(listener, "audio81", LISTENER_STACK, NULL, LISTENER_PRIO, NULL, 0,
                                  MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
}

}  // namespace audio_server
