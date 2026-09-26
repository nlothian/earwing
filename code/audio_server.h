// Audio over HTTP: the same endpoints on the web servers (80 and 443, see
// web_server.h) and on port 81, which is kept as a plain-HTTP legacy port
// for players (VLC, curl, ffmpeg) and old links.
//
//   /beam.wav          live 16-bit mono beam output, endless
//   /beam.wav?mic=N    the same, raw mic at geometry row N (1-4) for A/B
//   /raw.wav?s=N       capture N seconds (1-20) of all four channels into
//                      PSRAM first, then serve it: 24-bit, 4-channel WAV in
//                      capture-channel order
//
// With the inbuilt source (the board's own mic) there is one channel: both
// /beam.wav forms stream it, and /raw.wav is a 1-channel 24-bit WAV. A source
// switch mid-capture ends a /raw.wav with 409 rather than a mixed file.
//
// The transcriber web app fetches /beam.wav from its own origin (80 or 443);
// a dev server on localhost, or a page on 80 reading port 81, is another
// origin. So every response carries Access-Control-Allow-Origin: * and, for
// a Cross-Origin-Embedder-Policy: require-corp page, Cross-Origin-Resource-
// Policy: cross-origin; an OPTIONS preflight gets a 204 allowing GET.
//
// Streams run in a pool of worker tasks on core 0 (PSRAM stacks: they never
// touch flash), never in a server task: on 80/443 the handler hands the
// request over with httpd_req_async_handler_begin and returns, so the server
// keeps answering pages while it streams; port 81 is a small accept loop of
// its own that parses the request and hands the socket over the same way.
// Each worker reads the stream ring at its own position, so clients don't
// share or steal hops. Limits, checked before a stream starts, answered with
// 503 when exceeded:
//   - MAX_BEAM_STREAMS /beam.wav at once, over all ports;
//   - one /raw.wav at a time (the engine has one capture buffer);
//   - per port, one socket always left for ordinary requests. On 80/443 that
//     is more than politeness: esp_http_server only closes idle sockets for a
//     newcomer, so with every socket streaming it would spin on the listener.
// A stream ends when the client goes (the next write fails, within one batch
// or at worst the 5 s send timeout), which frees its slot.
#pragma once

#include <esp_http_server.h>

namespace audio_server {

static const int PORT = 81;
static const int MAX_BEAM_STREAMS = 3;
static const int PORT81_SOCKETS = 2;   // one streaming, one being answered

void begin();   // setup(), after WiFi.mode(): the workers and the port-81 listener

// ---- for web_server ----
bool addRoutes(httpd_handle_t h, int sockets);   // /beam.wav, /raw.wav (+ OPTIONS) on a server
void abortStreams(httpd_handle_t h);             // end h's streams and refuse new ones (it's stopping)
int  streamsOn(httpd_handle_t h);                // streams still holding one of h's requests
void removeServer(httpd_handle_t h);             // after h has stopped

// ---- for the status page ----
struct Counts {
  int beam, raw;        // streams running now
  int stackFree;        // the workers' smallest stack headroom so far, bytes
};
Counts counts();

}  // namespace audio_server
