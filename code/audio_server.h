// Audio over HTTP on port 81, in its own task on core 0. Port 80's WebServer
// is synchronous and runs in loop(), so a live stream there would stall the
// UI and the WiFi state machine; this server can block on a slow client
// without touching either, and the DSP never waits for it.
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
// One client at a time: others wait in the accept backlog.
#pragma once

namespace audio_server {

static const int PORT = 81;

void begin();   // after WiFi.mode(); the listener binds all interfaces

}  // namespace audio_server
