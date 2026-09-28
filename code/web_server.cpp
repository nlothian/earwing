#include "web_server.h"

#include <LittleFS.h>
#include <esp_heap_caps.h>
#include <esp_tls.h>
#include <mbedtls/ssl.h>
#include <mbedtls/ssl_cache.h>
#include <freertos/semphr.h>
#include <lwip/sockets.h>
#include <mbedtls/platform.h>
#include "audio_server.h"

namespace web {

// ---------- tasks ----------
// Core 0, beside WiFi and lwIP (both higher priority), and above the idle
// task and the port-81 listener; the stream workers sit just above these so a
// TLS handshake (~100 ms of ECC on this core) can't starve a stream. Internal
// stacks: the handlers read LittleFS, and a task whose stack is in PSRAM must
// not touch flash (the cache is off during the read).
static const UBaseType_t SERVER_PRIO = 4;
static const size_t HTTP_STACK = 6144;
static const size_t HTTPS_STACK = 10240;   // the handshake (ECDHE + ECDSA) runs in this task
static const uint16_t HTTP_CTRL_PORT = 32768, HTTPS_CTRL_PORT = 32769;   // loopback UDP, one per server

// A request's headers are ~500-900 bytes from a desktop browser; the
// prebuilt default of 1 KB would 431 a bigger one.
static const size_t MAX_REQ_HDR = 2048;
static const size_t MAX_FORM_BODY = 1024;
static const size_t FILE_CHUNK = 4096;
static const size_t SMALL_BODY = 1400;     // sent with its headers in one write

// Seconds. Sends: a stalled client lets go after this, which bounds a stream
// worker's exit too. Receives: how long a request (or a handshake flight) may
// dawdle once it has started arriving.
static const uint16_t HTTP_RECV_TIMEOUT_S = 5, HTTPS_RECV_TIMEOUT_S = 3, SEND_TIMEOUT_S = 5;
// HTTPS: see the transport below. A client gets CLIENT_HELLO_WAIT_MS after
// the TCP connect to start its handshake, and the handshake as a whole
// TLS_HANDSHAKE_TIMEOUT_MS.
static const uint32_t CLIENT_HELLO_WAIT_MS = 500;
static const uint32_t TLS_HANDSHAKE_TIMEOUT_MS = 3000;
// Session-ID resumption: sessions kept, and for how long. A resumed
// handshake skips the ECDHE and the ECDSA signature, most of its cost.
static const int TLS_CACHE_ENTRIES = 16, TLS_CACHE_SECONDS = 3600;

static httpd_handle_t httpHandle = nullptr;
static httpd_handle_t volatile httpsHandle = nullptr;   // read by handlers to tell the servers apart
static volatile Https httpsSt = Https::Off;
static char httpsErr[48] = "";
static const Route* routeTable = nullptr;
static int routeCount = 0;
static Handler notFoundFn = nullptr;

// ---------- mbedTLS memory ----------
// The prebuilt sdkconfig has CONFIG_MBEDTLS_INTERNAL_MEM_ALLOC: everything
// mbedTLS allocates comes from internal RAM, ~40 KB per TLS session, mostly
// its two 16 KB record buffers — against ~95 KB free once WiFi and the audio
// engine are up, so two sessions at most. mbedTLS lets the application
// replace its allocator at run time: blocks of TLS_PSRAM_MIN and up (the
// record buffers) go to PSRAM, the small ones (bignums, contexts) stay
// internal for speed. The AES/SHA drivers bounce PSRAM buffers through
// internal DMA memory themselves (CONFIG_MBEDTLS_EXTERNAL_MEM_ALLOC is a
// supported configuration on the S3). Either heap is the fallback for the
// other, and heap_caps_free() frees from both, so switching while mbedTLS
// has blocks out (the WiFi supplicant) is safe. 1 KB, not 4: the session
// state (transform, GCM contexts) moves out too, ~5.4 KB of internal RAM
// per session at 4 KB.
static const size_t TLS_PSRAM_MIN = 1024;

// Plain malloc() too, from moreRam() on: the Arduino build keeps every block
// under 4 KB internal (CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL), and lwIP's
// pbufs are plain malloc() — every unacknowledged TCP segment, up to 5.7 KB
// per socket. Three streams, a TLS page load and a file send took internal
// RAM down to 1.2 KB free (measured), and sends began failing mid-response.
// This is what CONFIG_SPIRAM_TRY_ALLOCATE_WIFI_LWIP does in a custom build;
// the WiFi driver's own buffers keep their explicit internal/DMA caps, and
// small blocks (lists, sockets, short strings) stay internal.
static const size_t MALLOC_PSRAM_MIN = 1024;

static void* tlsCalloc(size_t n, size_t size) {
  if (size && n > SIZE_MAX / size) return nullptr;
  const uint32_t INTERNAL = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT, PSRAM = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT;
  return n * size >= TLS_PSRAM_MIN ? heap_caps_calloc_prefer(n, size, 2, PSRAM, INTERNAL)
                                   : heap_caps_calloc_prefer(n, size, 2, INTERNAL, PSRAM);
}

static void tlsFree(void* p) { heap_caps_free(p); }

void begin() { mbedtls_platform_set_calloc_free(tlsCalloc, tlsFree); }

void moreRam() { heap_caps_malloc_extmem_enable(MALLOC_PSRAM_MIN); }

// ---------- responses ----------
static const char* statusText(int code) {
  switch (code) {
    case 200: return "OK";
    case 204: return "No Content";
    case 302: return "Found";
    case 303: return "See Other";
    case 400: return "Bad Request";
    case 404: return "Not Found";
    case 405: return "Method Not Allowed";
    case 409: return "Conflict";
    case 413: return "Payload Too Large";
    case 415: return "Unsupported Media Type";
    case 500: return "Internal Server Error";
    case 503: return "Service Unavailable";
    case 504: return "Gateway Timeout";
  }
  return "";
}

// httpd_send() may take part of a buffer (a TLS record at most); negative is
// a socket error or the send timeout.
static bool sendAll(httpd_req_t* r, const char* p, size_t n) {
  while (n) {
    const int w = httpd_send(r, p, n);
    if (w <= 0) return false;
    p += w;
    n -= w;
  }
  return true;
}

bool Req::secure() const { return httpsHandle && r_->handle == httpsHandle; }

String Req::path() const {
  const char* q = strchr(r_->uri, '?');
  return q ? String(r_->uri).substring(0, q - r_->uri) : String(r_->uri);
}

String Req::header(const char* name) const {
  const size_t n = httpd_req_get_hdr_value_len(r_, name);
  if (!n || n > 512) return String();
  char buf[513];
  if (httpd_req_get_hdr_value_str(r_, name, buf, n + 1) != ESP_OK) return String();
  return String(buf);
}

// For a Location header: host name characters only (IPv6 brackets too), so
// nothing a client sends can shape the redirect beyond picking the host.
String Req::host() const {
  String h = header("Host");
  const int end = h.startsWith("[") ? h.indexOf(']') + 1 : h.indexOf(':');
  if (end > 0) h = h.substring(0, end);
  for (size_t i = 0; i < h.length(); i++)
    if (!isalnum((unsigned char)h[i]) && !strchr(".-[]:", h[i])) return "earwing.local";
  return h.length() ? h : String("earwing.local");
}

void Req::parseArgs() {
  parsed_ = true;
  const char* q = strchr(r_->uri, '?');
  if (q) args_ = q + 1;
  if (r_->method != HTTP_POST || !r_->content_len || r_->content_len > MAX_FORM_BODY) return;
  if (header("Content-Type").indexOf("x-www-form-urlencoded") < 0) return;
  char buf[MAX_FORM_BODY + 1];
  size_t got = 0;
  while (got < r_->content_len) {
    const int n = httpd_req_recv(r_, buf + got, r_->content_len - got);
    if (n == HTTPD_SOCK_ERR_TIMEOUT) continue;   // httpd's own examples retry on this
    if (n <= 0) break;
    got += n;
  }
  buf[got] = 0;
  if (args_.length()) args_ += '&';
  args_ += buf;
}

static int hexVal(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

static String urlDecode(const char* s, size_t n) {
  String out;
  out.reserve(n);
  for (size_t i = 0; i < n; i++) {
    if (s[i] == '+') { out += ' '; continue; }
    if (s[i] == '%' && i + 2 < n) {
      const int hi = hexVal(s[i + 1]), lo = hexVal(s[i + 2]);
      if (hi >= 0 && lo >= 0) { out += (char)(hi * 16 + lo); i += 2; continue; }
    }
    out += s[i];
  }
  return out;
}

// Find `key` in "a=1&b&c=3": true if present (with or without a value).
static bool findArg(const String& args, const char* key, String* value) {
  const size_t klen = strlen(key);
  const char* p = args.c_str();
  while (*p) {
    const char* amp = strchr(p, '&');
    const size_t len = amp ? (size_t)(amp - p) : strlen(p);
    const char* eq = (const char*)memchr(p, '=', len);
    const size_t nameLen = eq ? (size_t)(eq - p) : len;
    if (nameLen == klen && !memcmp(p, key, klen)) {
      if (value) *value = eq ? urlDecode(eq + 1, len - nameLen - 1) : String();
      return true;
    }
    if (!amp) break;
    p = amp + 1;
  }
  return false;
}

bool Req::hasArg(const char* key) {
  if (!parsed_) parseArgs();
  return findArg(args_, key, nullptr);
}

String Req::arg(const char* key) {
  if (!parsed_) parseArgs();
  String v;
  findArg(args_, key, &v);
  return v;
}

void Req::addHeader(const char* name, const String& value) {
  hdrs_ += name;
  hdrs_ += ": ";
  hdrs_ += value;
  hdrs_ += "\r\n";
}

bool Req::begin(int code, const char* type, long length) {
  char head[160];
  int n = snprintf(head, sizeof(head), "HTTP/1.1 %d %s\r\n", code, statusText(code));
  if (type) n += snprintf(head + n, sizeof(head) - n, "Content-Type: %s\r\n", type);
  if (length >= 0) snprintf(head + n, sizeof(head) - n, "Content-Length: %ld\r\n", length);
  close_ = length < 0;
  String h = head;
  h += hdrs_;
  h += close_ ? "Connection: close\r\n\r\n" : "\r\n";
  if (!sendAll(r_, h.c_str(), h.length())) failed_ = true;
  return !failed_;
}

bool Req::write(const void* p, size_t n) {
  if (!failed_ && !sendAll(r_, (const char*)p, n)) failed_ = true;
  return !failed_;
}

bool Req::send(int code, const char* type, const char* body, size_t len) {
  if (len > SMALL_BODY) return begin(code, type, len) && write(body, len);
  // One write for headers and body: two small ones would be two packets.
  char head[160];
  int n = snprintf(head, sizeof(head), "HTTP/1.1 %d %s\r\n", code, statusText(code));
  if (type) n += snprintf(head + n, sizeof(head) - n, "Content-Type: %s\r\n", type);
  snprintf(head + n, sizeof(head) - n, "Content-Length: %u\r\n", (unsigned)len);
  String h = head;
  h += hdrs_;
  h += "\r\n";
  h.concat(body, len);
  if (!sendAll(r_, h.c_str(), h.length())) failed_ = true;
  return !failed_;
}

bool Req::redirect(int code, const String& location) {
  addHeader("Location", location);
  return send(code, "text/plain", "", 0);
}

// ---------- servers ----------
// Every route goes through here with its Handler as user_ctx. A failed write,
// or a response without a length, closes the socket (ESP_FAIL does that).
static esp_err_t trampoline(httpd_req_t* r) {
  Req q(r);
  ((Handler)r->user_ctx)(q);
  return q.closeAfter() ? ESP_FAIL : ESP_OK;
}

// No route for the URI (404), or a route but not for this method (405: the
// "/*" wildcards match every URI). The portal's captive redirect lives in
// notFoundFn and must catch both.
static esp_err_t errTrampoline(httpd_req_t* r, httpd_err_code_t) {
  Req q(r);
  notFoundFn(q);
  return q.closeAfter() ? ESP_FAIL : ESP_OK;
}

// Nagle would hold a response's last short segment until the client ACKs
// the one before, which a delayed ACK makes ~40-200 ms.
//
// And a new session counts as just used. httpd starts it at the server's
// current LRU count, which sessions served a moment later overtake: in a
// browser's burst of 6 connections onto 3-4 sockets, the purge then picked a
// socket whose request hadn't been read yet and the browser got an empty
// response. Bumped, the victim is always the longest-idle socket instead,
// usually one the browser has finished with (and a reused socket that turns
// out closed is one browsers retry on).
static esp_err_t onOpen(httpd_handle_t hd, int fd) {
  const int one = 1;
  setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
  httpd_sess_update_lru_counter(hd, fd);
  return ESP_OK;
}

// ---------- the TLS transport ----------
// Port 443 is a plain esp_http_server with this transport, not
// esp_https_server, for two things that one can't do:
//  - esp_http_server has no "open but not ready" state, so the handshake has
//    to finish inside the accept (open_fn), holding the server task. A
//    client that connects and sends nothing (a speculative socket) held it
//    for the recv timeout twice over: ~6 s each, 12 s for two, measured —
//    the original port-80 bug again. Real clients send the ClientHello right
//    after connecting, so open_fn first waits CLIENT_HELLO_WAIT_MS for it and
//    closes a socket that stays silent (the browser just opens another when
//    it needs one).
//  - Resumption. esp_https_server's session tickets need an esp-tls option
//    the prebuilt libs leave off; here each session's mbedTLS config gets a
//    shared session cache between esp_tls_server_session_init() and the
//    handshake, so a returning client skips the expensive part.
// Everything else is what esp_https_server does: an esp_tls session per
// socket as the transport context, and send/recv/pending overrides.
static esp_tls_cfg_server_t tlsCfg;      // the PEMs (our copies) for every session
static TlsStats tlsCount;                // written by the HTTPS task only
static mbedtls_ssl_cache_context tlsCache;
static bool tlsCacheReady = false;

// mbedtls_ssl_cache_get(), noting a hit: the handshake in progress (one at
// a time, in the HTTPS task) is a resumption.
static bool cacheHit;
static int cacheGet(void* cache, const unsigned char* id, size_t idLen, mbedtls_ssl_session* session) {
  const int r = mbedtls_ssl_cache_get(cache, id, idLen, session);
  if (r == 0) cacheHit = true;
  return r;
}

TlsStats tlsStats() { return tlsCount; }

static esp_tls_t* tlsOf(httpd_handle_t hd, int fd) { return (esp_tls_t*)httpd_sess_get_transport_ctx(hd, fd); }

static int tlsSend(httpd_handle_t hd, int fd, const char* buf, size_t len, int) {
  esp_tls_t* t = tlsOf(hd, fd);
  if (!t) return HTTPD_SOCK_ERR_INVALID;
  const ssize_t n = esp_tls_conn_write(t, buf, len);
  if (n >= 0) return n;
  return n == ESP_TLS_ERR_SSL_WANT_READ || n == ESP_TLS_ERR_SSL_WANT_WRITE ? HTTPD_SOCK_ERR_TIMEOUT : HTTPD_SOCK_ERR_FAIL;
}

static int tlsRecv(httpd_handle_t hd, int fd, char* buf, size_t len, int) {
  esp_tls_t* t = tlsOf(hd, fd);
  if (!t) return HTTPD_SOCK_ERR_INVALID;
  const ssize_t n = esp_tls_conn_read(t, buf, len);
  if (n >= 0) return n;   // 0: the peer closed
  if (n == ESP_TLS_ERR_SSL_WANT_READ || n == ESP_TLS_ERR_SSL_WANT_WRITE) return HTTPD_SOCK_ERR_TIMEOUT;
  return n == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY ? 0 : HTTPD_SOCK_ERR_FAIL;
}

// Decrypted bytes mbedTLS already holds: select() can't see those.
static int tlsPending(httpd_handle_t hd, int fd) {
  esp_tls_t* t = tlsOf(hd, fd);
  return t ? esp_tls_get_bytes_avail(t) : 0;
}

static void tlsClose(void* ctx) { esp_tls_server_session_delete((esp_tls_t*)ctx); }

static esp_err_t tlsOpen(httpd_handle_t hd, int fd) {
  fd_set rd;
  FD_ZERO(&rd);
  FD_SET(fd, &rd);
  timeval wait = {0, (suseconds_t)(CLIENT_HELLO_WAIT_MS * 1000)};
  if (select(fd + 1, &rd, nullptr, nullptr, &wait) <= 0) {   // silent: httpd closes it
    tlsCount.silent++;
    return ESP_FAIL;
  }
  esp_tls_t* tls = esp_tls_init();
  if (!tls) return ESP_ERR_NO_MEM;
  if (esp_tls_server_session_init(&tlsCfg, fd, tls) != ESP_OK) {
    esp_tls_server_session_delete(tls);
    tlsCount.failed++;
    return ESP_FAIL;
  }
  // This session's own config (esp-tls makes one per session), before the
  // handshake reads it.
  mbedtls_ssl_context* ssl = (mbedtls_ssl_context*)esp_tls_get_ssl_context(tls);
  mbedtls_ssl_config* conf = (mbedtls_ssl_config*)mbedtls_ssl_context_get_config(ssl);
  mbedtls_ssl_conf_session_cache(conf, &tlsCache, cacheGet, mbedtls_ssl_cache_set);
  // The socket is blocking with httpd's receive timeout, so each step waits
  // for the client's next flight rather than spinning.
  const uint32_t t0 = millis();
  cacheHit = false;
  int r;
  while ((r = esp_tls_server_session_continue_async(tls)) != 0) {
    if ((r != ESP_TLS_ERR_SSL_WANT_READ && r != ESP_TLS_ERR_SSL_WANT_WRITE) || millis() - t0 > TLS_HANDSHAKE_TIMEOUT_MS) {
      esp_tls_server_session_delete(tls);
      tlsCount.failed++;
      return ESP_FAIL;
    }
  }
  (cacheHit ? tlsCount.resumed : tlsCount.full)++;
  httpd_sess_set_transport_ctx(hd, fd, tls, tlsClose);
  httpd_sess_set_send_override(hd, fd, tlsSend);
  httpd_sess_set_recv_override(hd, fd, tlsRecv);
  httpd_sess_set_pending_override(hd, fd, tlsPending);
  return onOpen(hd, fd);
}

static void tlsCfgFree() {
  free((void*)tlsCfg.servercert_buf);
  free((void*)tlsCfg.serverkey_buf);
  tlsCfg = esp_tls_cfg_server_t{};
  if (tlsCacheReady) mbedtls_ssl_cache_free(&tlsCache);   // sessions of the old certificate go too
  tlsCacheReady = false;
}

static bool tlsCfgSet(const tls_cert::Pem& pem) {
  tlsCfgFree();
  uint8_t* c = (uint8_t*)malloc(pem.certLen);
  uint8_t* k = (uint8_t*)malloc(pem.keyLen);
  if (!c || !k) { free(c); free(k); return false; }
  memcpy(c, pem.cert, pem.certLen);
  memcpy(k, pem.key, pem.keyLen);
  tlsCfg.servercert_buf = c;
  tlsCfg.servercert_bytes = pem.certLen;
  tlsCfg.serverkey_buf = k;
  tlsCfg.serverkey_bytes = pem.keyLen;
  tlsCfg.tls_handshake_timeout_ms = TLS_HANDSHAKE_TIMEOUT_MS;
  mbedtls_ssl_cache_init(&tlsCache);
  mbedtls_ssl_cache_set_max_entries(&tlsCache, TLS_CACHE_ENTRIES);
  mbedtls_ssl_cache_set_timeout(&tlsCache, TLS_CACHE_SECONDS);
  tlsCacheReady = true;
  return true;
}

static httpd_config_t serverConfig(uint16_t port, int sockets, uint16_t ctrlPort, size_t stack, uint16_t recvTimeout) {
  httpd_config_t c = HTTPD_DEFAULT_CONFIG();
  c.task_priority = SERVER_PRIO;
  c.stack_size = stack;
  c.core_id = 0;
  c.max_req_hdr_len = MAX_REQ_HDR;
  c.server_port = port;
  c.ctrl_port = ctrlPort;
  c.max_open_sockets = sockets;
  c.max_uri_handlers = 20;
  c.backlog_conn = 4;
  c.lru_purge_enable = true;
  c.recv_wait_timeout = recvTimeout;
  c.send_wait_timeout = SEND_TIMEOUT_S;
  c.open_fn = onOpen;
  c.uri_match_fn = httpd_uri_match_wildcard;
  return c;
}

// The audio routes first (audio_server's own handlers: they may hand the
// request to a stream task), then main.cpp's in order.
static bool registerAll(httpd_handle_t h, int sockets) {
  bool ok = audio_server::addRoutes(h, sockets);
  for (int i = 0; i < routeCount; i++) {
    httpd_uri_t u = {};
    u.uri = routeTable[i].uri;
    u.method = routeTable[i].method;
    u.handler = trampoline;
    u.user_ctx = (void*)routeTable[i].fn;
    ok &= httpd_register_uri_handler(h, &u) == ESP_OK;
  }
  ok &= httpd_register_err_handler(h, HTTPD_404_NOT_FOUND, errTrampoline) == ESP_OK;
  ok &= httpd_register_err_handler(h, HTTPD_405_METHOD_NOT_ALLOWED, errTrampoline) == ESP_OK;
  return ok;
}

bool startHttp(const Route* routes, int n, Handler notFound) {
  routeTable = routes;
  routeCount = n;
  notFoundFn = notFound;
  httpd_config_t c = serverConfig(HTTP_PORT, HTTP_SOCKETS, HTTP_CTRL_PORT, HTTP_STACK, HTTP_RECV_TIMEOUT_S);
  const esp_err_t e = httpd_start(&httpHandle, &c);
  if (e != ESP_OK) {
    Serial.printf("web: HTTP server failed to start: %s\n", esp_err_to_name(e));
    return false;
  }
  if (!registerAll(httpHandle, HTTP_SOCKETS)) Serial.println("web: some HTTP routes failed to register");
  return true;
}

bool startHttps(const tls_cert::Pem& pem) {
  if (httpsSt != Https::Off) return httpsSt == Https::Running;
  if (!pem.cert || !pem.key || !tlsCfgSet(pem)) {
    snprintf(httpsErr, sizeof(httpsErr), "start failed: no certificate in memory");
    Serial.printf("web: HTTPS server %s\n", httpsErr);
    return false;
  }
  httpd_config_t c = serverConfig(HTTPS_PORT, HTTPS_SOCKETS, HTTPS_CTRL_PORT, HTTPS_STACK, HTTPS_RECV_TIMEOUT_S);
  c.open_fn = tlsOpen;
  httpd_handle_t h = nullptr;
  const esp_err_t e = httpd_start(&h, &c);
  if (e != ESP_OK) {
    tlsCfgFree();
    snprintf(httpsErr, sizeof(httpsErr), "start failed: %s", esp_err_to_name(e));
    Serial.printf("web: HTTPS server %s\n", httpsErr);
    return false;
  }
  httpsErr[0] = 0;
  if (!registerAll(h, HTTPS_SOCKETS)) Serial.println("web: some HTTPS routes failed to register");
  httpsHandle = h;
  httpsSt = Https::Running;
  Serial.printf("web: HTTPS server up on port %d\n", HTTPS_PORT);
  return true;
}

void stopHttps() {
  if (httpsSt != Https::Running) return;
  httpsSt = Https::Stopping;
  audio_server::abortStreams(httpsHandle);   // also refuses new ones on it
}

void poll() {
  if (httpsSt != Https::Stopping || audio_server::streamsOn(httpsHandle)) return;
  // No stream holds one of its requests any more; httpd_stop() waits for
  // the server task to exit and closes the remaining sessions (tlsClose).
  httpd_handle_t h = httpsHandle;
  httpsHandle = nullptr;
  httpd_stop(h);
  tlsCfgFree();
  audio_server::removeServer(h);
  httpsSt = Https::Off;
  Serial.println("web: HTTPS server stopped");
}

Https httpsState() { return httpsSt; }
const char* httpsError() { return httpsErr; }

// ---------- certificate ----------
static SemaphoreHandle_t certLock;
static String certPem;
static char certFp[100] = "";

void setCert(const tls_cert::Pem& pem, const char* fingerprint) {
  if (!certLock) certLock = xSemaphoreCreateMutex();
  xSemaphoreTake(certLock, portMAX_DELAY);
  certPem = pem.cert ? pem.cert : "";
  snprintf(certFp, sizeof(certFp), "%s", fingerprint ? fingerprint : "");
  xSemaphoreGive(certLock);
}

void certFingerprint(char* out, size_t len) {
  out[0] = 0;
  if (!certLock) return;
  xSemaphoreTake(certLock, portMAX_DELAY);
  snprintf(out, len, "%s", certFp);
  xSemaphoreGive(certLock);
}

// The PEM as a download: application/x-x509-ca-cert is what macOS, iOS and
// Android hand to their certificate installers.
void certHandler(Req& q) {
  String pem;
  if (certLock) {
    xSemaphoreTake(certLock, portMAX_DELAY);
    pem = certPem;
    xSemaphoreGive(certLock);
  }
  if (!pem.length()) { q.send(404, "text/plain", "no certificate: see /status\n"); return; }
  q.addHeader("Content-Disposition", "attachment; filename=\"earwing.crt\"");
  q.addHeader("Cache-Control", "no-cache");
  q.send(200, "application/x-x509-ca-cert", pem);
}

// ---------- web app (LittleFS) ----------
// The transcriber at "/" is a static Astro build (../site) on the "spiffs"
// partition as LittleFS, mirroring its dist/ tree with every file stored
// gzipped as <name>.gz only: the browser inflates it, we just copy bytes, and
// every browser takes gzip, so Accept-Encoding isn't checked. A plain <name>
// is served if there's no .gz, but the build script never writes one.
// LittleFS locks per filesystem, so both server tasks may read at once.
static bool mounted = false;
static bool installed = false;

// Type from the un-gzipped name; a JS module must not be octet-stream, nor
// wasm for instantiateStreaming().
static const char* siteType(const String& p) {
  static const struct { const char* ext; const char* type; } TYPES[] = {
    {".html", "text/html"}, {".js", "text/javascript"}, {".mjs", "text/javascript"},
    {".css", "text/css"}, {".json", "application/json"}, {".svg", "image/svg+xml"},
    {".png", "image/png"}, {".ico", "image/x-icon"}, {".webmanifest", "application/manifest+json"},
    {".wasm", "application/wasm"}, {".txt", "text/plain"}, {".woff2", "font/woff2"},
  };
  for (const auto& t : TYPES)
    if (p.endsWith(t.ext)) return t.type;
  return "application/octet-stream";
}

// <p>.gz, else <p>, as a regular file (LittleFS opens directories too), or
// an invalid File. *gz says which. A missing name is just an invalid File:
// no exists() first, which would open it twice.
static File siteFind(const String& p, bool* gz) {
  for (int z = 1; z >= 0; z--) {
    File f = LittleFS.open(z ? p + ".gz" : p, "r");
    if (f && !f.isDirectory()) { *gz = z; return f; }
  }
  return File();
}

// Headers on every site response. COOP + COEP make the page cross-origin
// isolated, which lets onnxruntime-web use threaded WASM (SharedArrayBuffer)
// where the browser counts the origin as secure (https://earwing.local, or
// localhost; not plain-http earwing.local). Same-origin fetches (/beam.wav,
// /status.json) need nothing under require-corp; cross-origin ones (a dev
// server, port 81) get CORS/CORP from audio_server. /_astro/ names are
// content-hashed, so cache them for good; everything else (index.html, which
// names them) revalidates.
void siteHeaders(Req& q, const String& path) {
  q.addHeader("Cross-Origin-Opener-Policy", "same-origin");
  q.addHeader("Cross-Origin-Embedder-Policy", "require-corp");
  q.addHeader("Cache-Control", path.startsWith("/_astro/") ? "public, max-age=31536000, immutable" : "no-cache");
}

// "/x/" means "/x/index.html", and an extensionless "/x" may be a directory
// too; <p>.gz is tried before <p>. Content-Encoding is ours to set, once,
// whatever the type. The body goes out in FILE_CHUNK reads from a PSRAM
// buffer, from this server's task: other requests on the same server wait
// for it (a 65 KB bundle is well under a second), the other server and
// loop() don't.
bool siteServe(Req& q, const String& path) {
  if (!mounted || path.indexOf("..") >= 0) return false;
  String p = path.endsWith("/") ? path + "index.html" : path;
  bool gz = false;
  File f = siteFind(p, &gz);
  if (!f && p.indexOf('.', p.lastIndexOf('/')) < 0) {
    p += "/index.html";
    f = siteFind(p, &gz);
  }
  if (!f) return false;
  uint8_t* buf = (uint8_t*)heap_caps_malloc(FILE_CHUNK, MALLOC_CAP_SPIRAM);
  if (!buf) {
    f.close();
    q.send(503, "text/plain", "out of memory\n");
    return true;
  }
  siteHeaders(q, path);
  if (gz) q.addHeader("Content-Encoding", "gzip");
  size_t left = f.size();
  if (q.begin(200, siteType(p), left)) {
    while (left) {
      const size_t n = f.read(buf, left < FILE_CHUNK ? left : FILE_CHUNK);
      if (!n || !q.write(buf, n)) break;
      left -= n;
    }
    // Short read: the length is already promised, so the socket must go.
    if (left) q.abort();
  }
  heap_caps_free(buf);
  f.close();
  return true;
}

// Mount, never format: an unformatted partition just means the web app was
// never uploaded, and uploadfs writes a whole image anyway.
void siteBegin() {
  mounted = LittleFS.begin(false);
  bool gz;
  installed = mounted && siteFind("/index.html", &gz);
  if (mounted)
    Serial.printf("web: LittleFS mounted (%u/%u KB used), site %s\n", (unsigned)(LittleFS.usedBytes() / 1024),
                  (unsigned)(LittleFS.totalBytes() / 1024), installed ? "installed" : "not installed");
  else
    Serial.println("web: LittleFS not mounted (never uploaded?), site not installed");
}

bool siteInstalled() { return installed; }
bool siteMounted() { return mounted; }

Mem mem() {
  Mem m;
  m.internalFree = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
  m.internalLargest = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
  m.internalMin = heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL);
  m.psramFree = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
  return m;
}

}  // namespace web
