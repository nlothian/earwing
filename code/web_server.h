// The web servers: ESP-IDF's esp_http_server on port 80, and on port 443 the
// same server with a TLS transport of ours (esp-tls sessions; why not
// esp_https_server: see web_server.cpp), one route table for both, and the
// static web app from LittleFS. main.cpp owns the pages and the WiFi state; audio_server owns the
// streams (on these two servers and on its legacy port 81).
//
// Why not Arduino's WebServer: it serves one client at a time from loop(),
// and a connection that opens without sending a request (a browser's
// preconnect or spare socket) holds it for its 5 s read timeout — each one
// stalls every other request, the TFT and the WiFi state machine. Here each
// server is a select() loop in its own task on core 0 (the DSP owns core 1),
// idle keep-alive sockets cost nothing, and when a server's sockets are all
// taken the least recently used idle one is closed for the newcomer
// (lru_purge_enable). Long responses (the audio streams) leave the server
// task through httpd_req_async_handler_begin, so they never hold it either.
//
// Sockets: lwIP has 16 in all (CONFIG_LWIP_MAX_SOCKETS in the prebuilt
// libs), and every esp_http_server instance keeps 3 for itself (listener and
// two control sockets). HTTP_SOCKETS + HTTPS_SOCKETS + 6 + audio_server's
// port 81 (a listener and PORT81_SOCKETS clients) must stay within that.
//
// Handlers run in the server tasks, concurrently with loop() and with each
// other (80 and 443 are two tasks): anything they share with loop() goes
// through main.cpp's snapshot and hand-off flags, never its globals directly.
#pragma once

#include <Arduino.h>
#include <esp_http_server.h>
#include "tls_cert.h"

namespace web {

static const int HTTP_PORT = 80;
static const int HTTPS_PORT = 443;
// Client sockets per server, see above. The HTTPS count is also bounded by
// RAM: begin()'s mbedTLS allocator keeps a session's two 16 KB record
// buffers in PSRAM, so each costs only a few KB of internal RAM (see
// /status.json's heap figures).
static const int HTTP_SOCKETS = 3;
static const int HTTPS_SOCKETS = 4;

// ---------- one request ----------
// A thin wrapper for handlers, the same on both servers. Responses are
// written raw (status line and headers by hand, then the body), with a
// Content-Length unless begin() is told the length is unknown, in which case
// it says Connection: close and the socket is closed after the handler.
class Req {
 public:
  explicit Req(httpd_req_t* r) : r_(r) {}
  httpd_req_t* raw() const { return r_; }
  bool   secure() const;                 // arrived on the HTTPS server
  bool   isGet() const { return r_->method == HTTP_GET; }
  bool   isPost() const { return r_->method == HTTP_POST; }
  String path() const;                   // the URI without its query
  // Query arguments, then a form POST's (application/x-www-form-urlencoded,
  // up to 1 KB), URL-decoded, as Arduino's WebServer did.
  bool   hasArg(const char* key);
  String arg(const char* key);           // "" if absent
  String header(const char* name) const; // "" if absent
  String host() const;                   // Host without the port, or heimdallr.local

  void addHeader(const char* name, const String& value);   // before the response
  bool send(int code, const char* type, const char* body, size_t len);
  bool send(int code, const char* type, const String& body) { return send(code, type, body.c_str(), body.length()); }
  bool redirect(int code, const String& location);
  // A body sent in pieces: begin() with its length (or -1 if unknown: the
  // connection closes after it), then write() until done.
  bool begin(int code, const char* type, long length);
  bool write(const void* p, size_t n);
  void abort() { failed_ = true; }       // the response can't be finished: close the socket
  bool failed() const { return failed_; }
  bool closeAfter() const { return close_ || failed_; }

 private:
  void parseArgs();
  httpd_req_t* r_;
  String hdrs_;            // extra response headers, "Name: value\r\n" each
  String args_;            // "query&body", still URL-encoded
  bool parsed_ = false;
  bool failed_ = false;    // a write failed: the handler's socket is closed
  bool close_ = false;     // the response said Connection: close
};

using Handler = void (*)(Req&);
struct Route {
  const char*    uri;      // exact, or a wildcard ending in "*"
  httpd_method_t method;
  Handler        fn;
};

// ---------- servers ----------
// Both take main.cpp's route table (kept, not copied), in match order: the
// first route whose method and URI match wins, so the wildcards go last.
// `notFound` answers everything else (no route, or a route but not that
// method). The audio routes are added by audio_server on both.
void begin();     // setup(), first: the mbedTLS allocator (see web_server.cpp)
void moreRam();   // setup(), after the audio engine has allocated: malloc() >= 1 KB from PSRAM
bool startHttp(const Route* routes, int n, Handler notFound);   // setup(); never stopped

// HTTPS: started when the device is online, stopped for the setup portal and
// to change the certificate. Stopping has to wait for the audio streams on
// it to end (they hold its requests), so stopHttps() only asks, and loop()
// calls poll() until the state is Off; that takes a few ms, at worst a send
// timeout (5 s) for a stream stuck on a dead client.
enum class Https : uint8_t { Off, Running, Stopping };
bool  startHttps(const tls_cert::Pem& pem);   // from loop()/setup(); the PEMs are copied
void  stopHttps();
void  poll();                                 // loop(): finishes a stop
Https httpsState();
const char* httpsError();                     // why the last start failed, or ""

// The certificate for /heimdallr.crt and the status page: copied here from
// tls_cert's buffers (which regenerate() frees) whenever loop() loads a new
// one, so handlers never touch those.
void setCert(const tls_cert::Pem& pem, const char* fingerprint);
void certFingerprint(char* out, size_t len);   // "" if none
void certHandler(Req& q);                      // GET /heimdallr.crt

// ---------- web app (LittleFS) ----------
void siteBegin();                              // setup(), before startHttp()
bool siteInstalled();
bool siteMounted();
// Send a request path from LittleFS; false if there's nothing there.
bool siteServe(Req& q, const String& path);
void siteHeaders(Req& q, const String& path); // COOP/COEP and caching, on every site response

// HTTPS handshakes since boot, for /status.json: full, resumed (session
// cache), silent (closed: no ClientHello within CLIENT_HELLO_WAIT_MS), failed.
struct TlsStats { uint32_t full, resumed, silent, failed; };
TlsStats tlsStats();

// The memory the servers can't do without, for /status.json.
struct Mem { uint32_t internalFree, internalLargest, internalMin, psramFree; };
Mem mem();

}  // namespace web
