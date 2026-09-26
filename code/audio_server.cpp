#include "audio_server.h"

#include <Arduino.h>
#include <WiFi.h>
#include <esp_heap_caps.h>
#include <string.h>
#include "audio_engine.h"

namespace audio_server {

using namespace audio;

static WiFiServer server(PORT);

static const int MAX_RAW_SECONDS = 20;       // array: 20 s × 16 kHz × 12 bytes = 3.8 MB of PSRAM
static const int STREAM_BATCH_HOPS = 4;      // hops per socket write (64 ms)
static const uint32_t REQUEST_TIMEOUT_MS = 2000;

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

// Read "GET /path?query HTTP/1.1" and skip the headers. False on timeout.
static bool readRequest(WiFiClient& c, String& path, String& query) {
  const uint32_t deadline = millis() + REQUEST_TIMEOUT_MS;
  String line, first;
  bool gotFirst = false;
  while ((int32_t)(millis() - deadline) < 0 && c.connected()) {
    if (!c.available()) { vTaskDelay(pdMS_TO_TICKS(5)); continue; }
    const char ch = (char)c.read();
    if (ch == '\r') continue;
    if (ch != '\n') { if (line.length() < 512) line += ch; continue; }
    if (!gotFirst) { first = line; gotFirst = true; }
    else if (!line.length()) break;   // blank line: end of headers
    line = "";
  }
  if (!gotFirst) return false;
  const int s1 = first.indexOf(' '), s2 = first.indexOf(' ', s1 + 1);
  if (s1 < 0) return false;
  String target = first.substring(s1 + 1, s2 < 0 ? first.length() : s2);
  const int q = target.indexOf('?');
  path = q < 0 ? target : target.substring(0, q);
  query = q < 0 ? String() : target.substring(q + 1);
  return true;
}

static int queryInt(const String& q, const char* key, int def) {
  const String k = String(key) + "=";
  int at = 0;
  if (!q.startsWith(k)) {
    at = q.indexOf("&" + k);
    if (at < 0) return def;
    at += 1;
  }
  return q.substring(at + k.length()).toInt();
}

static void sendText(WiFiClient& c, int code, const char* status, const char* body) {
  c.printf("HTTP/1.1 %d %s\r\nContent-Type: text/plain\r\nConnection: close\r\n\r\n%s\n",
           code, status, body);
}

static void streamWav(WiFiClient& c, int source) {
  uint8_t h[44];
  // Endless stream: a data size near 2 GB (not 0xFFFFFFFF, which some
  // readers treat as signed) so players just keep going.
  wavHeader(h, 1, 16, 0x7FFF0000u);
  c.print("HTTP/1.1 200 OK\r\nContent-Type: audio/wav\r\nCache-Control: no-store\r\n"
          "Connection: close\r\n\r\n");
  c.write(h, sizeof(h));
  static int16_t buf[STREAM_BATCH_HOPS * HOP];   // one client at a time
  uint32_t seq = engine::streamHead();
  while (c.connected()) {
    int n = 0;
    while (n < STREAM_BATCH_HOPS && engine::streamRead(&seq, source, buf + n * HOP)) n++;
    if (!n) { vTaskDelay(pdMS_TO_TICKS(8)); continue; }
    const size_t bytes = n * HOP * sizeof(int16_t);
    if (c.write((const uint8_t*)buf, bytes) != bytes) break;   // client gone or stuck
  }
}

static void serveRaw(WiFiClient& c, int seconds) {
  if (seconds < 1) seconds = 1;
  if (seconds > MAX_RAW_SECONDS) seconds = MAX_RAW_SECONDS;
  // Four channels from the array, one from the board mic. The engine checks
  // the count again when arming, in case the source changed in between.
  const int channels = engine::rawChannels();
  const uint32_t frames = (uint32_t)seconds * SAMPLE_RATE;
  const uint32_t bytes = frames * channels * 3;
  // Capture to RAM first: WiFi hiccups can't disturb the timing that way.
  uint8_t* buf = (uint8_t*)heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM);
  if (!buf) { sendText(c, 503, "Service Unavailable", "not enough PSRAM"); return; }
  if (!engine::rawCaptureBegin(buf, frames, channels)) {
    heap_caps_free(buf);
    sendText(c, 503, "Service Unavailable", "capture busy, or the mic source is switching");
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
      sendText(c, 409, "Conflict", "the mic source switched during the capture; try again");
      return;
    }
    if ((int32_t)(millis() - deadline) > 0) {
      engine::rawCaptureAbort();
      heap_caps_free(buf);
      sendText(c, 504, "Gateway Timeout", "capture did not complete: is the I2S capture running? see /");
      return;
    }
    vTaskDelay(pdMS_TO_TICKS(20));
  }
  uint8_t h[44];
  wavHeader(h, channels, 24, bytes);
  // A resync mid-capture means a discontinuity in the file; say so.
  c.printf("HTTP/1.1 200 OK\r\nContent-Type: audio/wav\r\nContent-Length: %lu\r\n"
           "Content-Disposition: attachment; filename=\"gulltoppr-raw.wav\"\r\n"
           "X-Gulltoppr-Resyncs-During-Capture: %lu\r\nX-Gulltoppr-Source: %s\r\n"
           "Connection: close\r\n\r\n",
           (unsigned long)(sizeof(h) + bytes), (unsigned long)resyncs,
           engine::sourceName(channels == 1 ? engine::Source::Inbuilt : engine::Source::Array));
  c.write(h, sizeof(h));
  for (uint32_t at = 0; at < bytes && c.connected();) {
    const uint32_t n = bytes - at < 4096 ? bytes - at : 4096;
    const size_t w = c.write(buf + at, n);
    if (!w) break;
    at += w;
  }
  heap_caps_free(buf);
}

static void handle(WiFiClient& c) {
  String path, query;
  if (!readRequest(c, path, query)) return;
  if (path == "/beam.wav") {
    const int mic = queryInt(query, "mic", 0);   // 1-based geometry row, 0 = beam
    streamWav(c, mic >= 1 && mic <= NUM_MICS ? mic - 1 : -1);
  } else if (path == "/raw.wav") {
    serveRaw(c, queryInt(query, "s", 5));
  } else {
    sendText(c, 404, "Not Found",
             "gulltoppr audio:\n"
             "  /beam.wav        live beam output (16 kHz, 16-bit mono)\n"
             "  /beam.wav?mic=N  raw mic N (1-4) instead, same gain and latency\n"
             "  /raw.wav?s=N     N seconds (1-20) of all 4 channels, 24-bit\n"
             "With the inbuilt source (the board mic) every stream is that mic,\n"
             "and /raw.wav is 1 channel.");
  }
}

static void task(void*) {
  server.begin();
  server.setNoDelay(true);
  for (;;) {
    WiFiClient c = server.accept();
    if (!c) { vTaskDelay(pdMS_TO_TICKS(20)); continue; }
    handle(c);
    c.stop();
  }
}

void begin() {
  // Core 0 with WiFi, low priority: it only ever waits on sockets and the
  // stream ring, and nothing waits on it.
  xTaskCreatePinnedToCore(task, "audiosrv", 6144, NULL, 2, NULL, 0);
}

}  // namespace audio_server
