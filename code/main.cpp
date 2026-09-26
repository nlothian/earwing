// gulltoppr — 4-mic beamforming array on an ESP32-S3 (Freenove FNK0104, see
// platformio.ini). This file is the WiFi lifecycle and the user interface:
// status page and controls on port 80, the TFT, and a 1 Hz serial line. The
// audio lives in audio_engine.* (I2S capture + DSP tasks on core 1),
// processor.* / doa.* / beamformer.* (the portable DSP) and audio_server.*
// (streams on port 81); the board's I2C devices (ES8311 codec for the
// inbuilt mic, FT6336U touch) in board_io.*.
//
// Mic source: "array" (the 4× INMP441, every boot) or "inbuilt" (the board's
// own mic), switched from /control?source= or the on-screen button.
//
// WiFi lifecycle: STA join with saved credentials, captive-portal fallback
// for first-time setup, and an mDNS-advertised webserver at
// http://gulltoppr.local/ once online. No WiFiManager: it wants to own the
// radio and blocks by default, whereas everything here is called from loop()
// and never blocks, so the audio tasks and the UI share core 1 happily.
//
//   CONNECTING --ok--> ONLINE --link lost--> CONNECTING (retry quietly)
//       |                 |
//    timeout /       hold right button
//    no creds             v
//       +-----------> PORTAL --form join ok / saved join ok--> ONLINE
//
// Portal: open AP "gulltoppr" at 192.168.4.1 with a wildcard DNS server, so
// phones/laptops pop the setup page automatically.
//
// Buttons: hold BOOT (GPIO0) for 2s to reopen the setup portal, e.g. to move
// the device to a different network. Saved credentials stay in NVS until a
// portal join succeeds with replacements. Tap the MIC button on the screen to
// toggle the mic source.

#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <ESPmDNS.h>
#include <Preferences.h>
#include <TFT_eSPI.h>   // configured entirely via build_flags in platformio.ini
#include <esp_heap_caps.h>
#include <stdarg.h>
#include "audio_engine.h"
#include "audio_server.h"
#include "board_io.h"

static const char* NAME = "gulltoppr";   // AP SSID, mDNS host, DHCP hostname, NVS namespace

// ---------- pins ----------
static const int PIN_BTN_BOOT = 0;     // the board's only user button; boot-strap pin, fine after reset

// ---------- timing ----------
static const uint32_t JOIN_TIMEOUT_MS        = 15000;   // boot / reconnect attempt
static const uint32_t FORM_JOIN_TIMEOUT_MS   = 20000;   // join from portal form
static const uint32_t SAVED_RETRY_PERIOD_MS  = 180000;  // portal: retry saved creds this often
static const uint32_t PORTAL_HOLD_MS         = 2000;

// ---------- persistence (ESP32 NVS via Preferences) ----------
// Credentials live in our own namespace, not the WiFi stack's (persistent(false)).
static Preferences prefs;

static bool credsLoad(char* ssid, size_t ssidLen, char* pass, size_t passLen) {
  ssid[0] = 0; pass[0] = 0;   // getString leaves the buffer untouched if the key is absent
  prefs.begin(NAME, true);
  prefs.getString("w_ssid", ssid, ssidLen);
  prefs.getString("w_pass", pass, passLen);
  prefs.end();
  return ssid[0] != 0;
}

static void credsSave(const char* ssid, const char* pass) {
  prefs.begin(NAME, false);
  prefs.putString("w_ssid", ssid);
  prefs.putString("w_pass", pass);
  prefs.end();
}

// ---------- WiFi state ----------
enum WifiState : uint8_t { WF_CONNECTING, WF_PORTAL, WF_ONLINE };
enum JoinSource : uint8_t { JOIN_NONE, JOIN_FORM, JOIN_SAVED };

static WifiState  wf = WF_CONNECTING;
static WebServer  http(80);
static DNSServer  dns;
static uint32_t   wfDeadline = 0;          // CONNECTING: timeout / retry; PORTAL: join attempt timeout
static uint32_t   nextSavedRetry = 0;      // PORTAL: when to try the saved network again
static JoinSource joining = JOIN_NONE;     // PORTAL: STA join in flight, and who asked for it
static bool       joinPending = false;     // portal posted creds; begin STA join next tick
static bool       joinFailed = false;      // last form join failed; shown on the portal page
static bool       wasOnline = false;       // distinguishes first-boot timeout from a drop
static bool       httpRegistered = false;  // handlers attach once; WebServer can't deregister
static char       wfSsid[33] = "";         // credentials currently being tried / in use
static char       wfPass[65] = "";

static void htmlEscape(String& s) {
  s.replace("&", "&amp;");
  s.replace("<", "&lt;");
  s.replace(">", "&gt;");
  s.replace("\"", "&quot;");
}

static const char PAGE_HEAD[] =
  "<!DOCTYPE html><html><head><meta charset=utf-8>"
  "<meta name=viewport content='width=device-width,initial-scale=1'>"
  "<title>gulltoppr</title>"
  "<style>body{font-family:sans-serif;max-width:320px;margin:2em auto;padding:0 1em}"
  "input{width:100%;padding:.5em;margin:.3em 0 1em;box-sizing:border-box}"
  "button{width:100%;padding:.6em;font-size:1em}.err{color:#b00}"
  "td{padding:.2em .6em .2em 0}</style></head><body>";

// Portal form. Networks found by the async scan are offered via a <datalist>
// so hidden SSIDs can still be typed in.
static void portalPage() {
  String nets;
  int n = WiFi.scanComplete();
  for (int i = 0; i < n; i++) {
    String s = WiFi.SSID(i);
    if (!s.length()) continue;
    htmlEscape(s);
    nets += "<option value=\"" + s + "\">";
  }
  // Keep the list fresh, but don't scan over a join in flight: scanning hops
  // channels and would stall the association.
  if (n >= 0 && joining == JOIN_NONE) { WiFi.scanDelete(); WiFi.scanNetworks(true); }

  String err;
  if (joinFailed) {
    String s = wfSsid;
    htmlEscape(s);
    err = "<p class=err>Couldn't join <b>" + s + "</b>. Check the password and try again.</p>";
  }

  String page = PAGE_HEAD;
  page += "<h2>gulltoppr wifi setup</h2>" + err +
          "<form method=POST action=/save>"
          "<label>Network</label><input name=ssid list=nets required maxlength=32>"
          "<datalist id=nets>" + nets + "</datalist>"
          "<label>Password</label><input name=pass type=password maxlength=64>"
          "<button>Connect</button></form></body></html>";
  http.send(200, "text/html", page);
}

// ---------- audio status / controls ----------
// printf into a String; the pages are built from many small numeric fields.
static void appendf(String& s, const char* fmt, ...) {
  char buf[256];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  s += buf;
}

// "1234": digit c is the geometry position (1 = −x end) of the mic on capture
// channel c. Rejects anything that isn't a permutation.
static bool parseMap(const String& s, uint8_t* out) {
  if (s.length() != audio::NUM_MICS) return false;
  bool seen[audio::NUM_MICS] = {};
  for (int c = 0; c < audio::NUM_MICS; c++) {
    int p = s[c] - '1';
    if (p < 0 || p >= audio::NUM_MICS || seen[p]) return false;
    seen[p] = true;
    out[c] = (uint8_t)p;
  }
  return true;
}

// "a,b,c,d" → four floats.
static bool parseFloats(const String& s, float* out) {
  int at = 0;
  for (int i = 0; i < audio::NUM_MICS; i++) {
    int comma = s.indexOf(',', at);
    if ((comma < 0) != (i == audio::NUM_MICS - 1)) return false;
    out[i] = s.substring(at, comma < 0 ? s.length() : comma).toFloat();
    if (!isfinite(out[i])) return false;   // atof accepts "nan" and "inf"
    at = comma + 1;
  }
  return true;
}

static const char* alignText(const engine::Status& a) {
  if (!a.alignRuns) return "pending";
  return sync_check::statusName(a.align.status);
}

static const char* codecInitText(engine::CodecInit c) {
  switch (c) {
    case engine::CodecInit::Never:  return "never run";
    case engine::CodecInit::Ok:     return "ok";
    case engine::CodecInit::Failed: return "FAILED";
  }
  return "?";
}

// Board mic source with the codec down after a failed init: nothing is
// captured, and every display says so.
static bool codecDead(const engine::Status& a) {
  return a.source == engine::Source::Inbuilt && a.codecInit == engine::CodecInit::Failed && !a.codecPowered;
}

static void audioStatusHtml(String& page, const String& ip) {
  engine::Status a;
  engine::status(&a);
  const audio::Calibration cal = engine::calibration();
  const dsp::Geometry& g = engine::geometry();
  const board_io::Info b = board_io::info();
  const bool board = a.source == engine::Source::Inbuilt;

  page += "<h3>audio</h3><table>";
  appendf(page, "<tr><td>Source</td><td><b>%s</b> %s%s</td></tr>", engine::sourceName(a.source),
          board ? "(the board's own mic through its ES8311; 1 channel live, no DOA or beam)"
                : "(4&times; INMP441 array)",
          a.source != a.sourceTarget ? (a.sourceTarget == engine::Source::Inbuilt ? " &rarr; switching to inbuilt"
                                                                                  : " &rarr; switching to array")
                                     : "");
  appendf(page, "<tr><td>Codec (ES8311)</td><td>%sinit %s, %s; %lu inits, %lu failed; last: %s</span></td></tr>",
          a.codecInit == engine::CodecInit::Failed ? "<span class=err>" : "<span>",
          codecInitText(a.codecInit), a.codecPowered ? "ADC on" : "off",
          (unsigned long)a.codecInits, (unsigned long)a.codecFailures, b.codecDetail);
  if (board) {
    appendf(page, "<tr><td>Mode</td><td><b>%s</b> (the array's; kept, not active on the board mic)</td></tr>",
            proc::modeName(a.mode));
    page += "<tr><td>DOA</td><td>n/a (one mic)</td></tr>"
            "<tr><td>Beam steering</td><td>n/a</td></tr>"
            "<tr><td>Speech gate</td><td>n/a</td></tr>";
    appendf(page, "<tr><td>Mic RMS / peak</td><td>board mic %.1f / %.1f dBFS (the only live channel)</td></tr>",
            a.micDb[0], a.micPeakDb[0]);
    appendf(page, "<tr><td>Output RMS</td><td>%.1f dBFS</td></tr>", a.beamDb);
  } else {
    appendf(page, "<tr><td>Mode</td><td><b>%s</b></td></tr>", proc::modeName(a.mode));
    appendf(page, "<tr><td>DOA (scan)</td><td>%+.1f&deg;, confidence %.2f</td></tr>", a.doaDeg, a.confidence);
    if (a.smoothValid) appendf(page, "<tr><td>DOA (tracked)</td><td>%+.1f&deg;</td></tr>", a.smoothDeg);
    else page += "<tr><td>DOA (tracked)</td><td>no speech yet</td></tr>";
    appendf(page, "<tr><td>Beam steering</td><td>%+.1f&deg; (fixed angle %+.1f&deg;)</td></tr>", a.steerDeg, a.fixedDeg);
    appendf(page, "<tr><td>Speech gate</td><td>%s</td></tr>", a.speech ? "open" : "closed");
    page += "<tr><td>Mic RMS / peak</td><td>";
    for (int c = 0; c < audio::NUM_MICS; c++)
      appendf(page, "ch%d %.1f / %.1f<br>", c + 1, a.micDb[c], a.micPeakDb[c]);
    page += "dBFS</td></tr>";
    appendf(page, "<tr><td>Beam out RMS</td><td>%.1f dBFS</td></tr>", a.beamDb);
  }
  appendf(page, "<tr><td>DSP per hop</td><td>mean %.0f &micro;s, max %.0f &micro;s (%.0f%% of %.0f), worst %.0f</td></tr>",
          a.dspMeanUs, a.dspMaxUs, 100.0f * a.dspMaxUs / audio::HOP_US, audio::HOP_US, a.dspPeakUs);
  appendf(page, "<tr><td>Frame ring</td><td>%u hops (high %u), %lu frames dropped</td></tr>",
          a.ringDepth, a.ringHigh, (unsigned long)a.ringDrops);
  appendf(page, "<tr><td>DMA overflows</td><td>I2S0 %lu, I2S1 %lu</td></tr>",
          (unsigned long)a.overflows[0], (unsigned long)a.overflows[1]);
  appendf(page, "<tr><td>Read timeouts</td><td>I2S0 %lu, I2S1 %lu</td></tr>",
          (unsigned long)a.timeouts[0], (unsigned long)a.timeouts[1]);
  appendf(page, "<tr><td>Resyncs / mismatches</td><td>%lu / %lu (faults and forced; source switches: %lu%s)</td></tr>",
          (unsigned long)a.resyncs, (unsigned long)a.mismatches, (unsigned long)a.sourceSwitches,
          a.hwFaults ? ", <span class=err>I2S channel faults</span>" : "");
  appendf(page, "<tr><td>Low byte &ne; 0</td><td>I2S0 %lu, I2S1 %lu slots</td></tr>",
          (unsigned long)a.lowByteNonzero[0], (unsigned long)a.lowByteNonzero[1]);
  appendf(page, "<tr><td>Alignment test</td><td>%s%s, offset %d slots, drop %d/%d; "
          "%lu runs, %lu matched, first %d, %lu changed</span></td></tr>",
          a.alignChanges || a.align.status == sync_check::Status::NoMatch ||
              a.align.status == sync_check::Status::BitShift ? "<span class=err>" : "<span>",
          alignText(a), a.align.slotOffset, a.align.dropMaster, a.align.dropSlave,
          (unsigned long)a.alignRuns, (unsigned long)a.alignConclusive, a.alignFirstOffset,
          (unsigned long)a.alignChanges);
  page += "<tr><td>Pair lead (samples)</td><td>";
  if (a.leadMs) {
    for (int n = 0; n < proc::NUM_LEADS; n++) {
      int i, j;
      proc::leadPair(n, &i, &j);
      appendf(page, "pos%d over pos%d: %+.2f (coh %.2f)<br>", j + 1, i + 1, a.lead[n], a.leadCoh[n]);
    }
    appendf(page, "%lu s ago", (unsigned long)((millis() - a.leadMs) / 1000));
  } else {
    page += "no gated sound yet";
  }
  page += "</td></tr>";
  appendf(page, "<tr><td>Stream</td><td>%lu hops dropped, %lu samples clipped</td></tr>",
          (unsigned long)a.streamDrops, (unsigned long)a.clipped);
  appendf(page, "<tr><td>Geometry</td><td>%s, %.0f mm aperture, DOA band %.0f&ndash;%.0f Hz "
          "(aliasing from %.0f Hz)</td></tr>",
          g.linear ? "linear (&minus;90..+90&deg;, a cone around the axis)" : "planar",
          g.aperture * 1000, g.binLo * audio::BIN_HZ, g.binHi * audio::BIN_HZ, g.aliasHz);
  appendf(page, "<tr><td>Calibration</td><td>map %d%d%d%d, gain %+.1f/%+.1f/%+.1f/%+.1f dB, "
          "advance %.0f/%.0f/%.0f/%.0f &micro;s%s</td></tr>",
          cal.posOfChannel[0] + 1, cal.posOfChannel[1] + 1, cal.posOfChannel[2] + 1, cal.posOfChannel[3] + 1,
          cal.gainDb[0], cal.gainDb[1], cal.gainDb[2], cal.gainDb[3],
          cal.advanceUs[0], cal.advanceUs[1], cal.advanceUs[2], cal.advanceUs[3],
          a.gainCal == 1 ? " (measuring)" : a.gainCal == -1 ? " <span class=err>(gain cal failed)</span>" : "");
  appendf(page, "<tr><td>FFT check</td><td>esp-dsp vs portable: %.1e rel, %.0f vs %.0f &micro;s</td></tr>",
          a.fftMaxErr, a.fftEspUs, a.fftRefUs);
  appendf(page, "<tr><td>Touch (FT6336U)</td><td>%s, %lu taps, %lu unanswered polls</td></tr>",
          !b.touchReady ? "starting" : b.touchOk ? "ok" : "<span class=err>not answering</span>",
          (unsigned long)b.taps, (unsigned long)b.touchErrors);
  page += "</table>";

  page += "<h3>controls</h3><form action=/control>"
          "<label>Source</label><select name=source>";
  appendf(page, "<option value=array%s>array (4&times; INMP441)</option>"
          "<option value=inbuilt%s>inbuilt (board mic)</option></select>",
          a.sourceTarget == engine::Source::Array ? " selected" : "",
          a.sourceTarget == engine::Source::Inbuilt ? " selected" : "");
  page += "<label>Mode</label><select name=mode>";
  const proc::Mode modes[] = {proc::Mode::Raw, proc::Mode::DoaOnly, proc::Mode::FixedBeam, proc::Mode::AutoBeam};
  const char* keys[] = {"raw", "doa", "fixed", "auto"};
  for (int i = 0; i < 4; i++)
    appendf(page, "<option value=%s%s>%s</option>", keys[i], a.mode == modes[i] ? " selected" : "",
            proc::modeName(modes[i]));
  appendf(page, "</select><label>Fixed angle (&deg;, + toward Mic 4)</label>"
          "<input name=angle value='%.0f'><button>Apply</button></form>", a.fixedDeg);
  page += "<p><a href='/control?calgain=3'>Calibrate gains</a> (3 s, broadband source at broadside; array only) &middot; "
          "<a href='/control?resync=1'>Force resync</a> &middot; "
          "<a href='/control?resetcal=1'>Reset calibration</a></p>"
          "<p>Channel map / trims: <code>/control?map=1234</code>, "
          "<code>gain=a,b,c,d</code> (dB), <code>advance=a,b,c,d</code> (&micro;s)</p>";
  page += "<h3>audio (port 81)</h3><p>"
          "<a href='http://" + ip + ":81/beam.wav'>beam.wav</a> (live) &middot; raw mic ";
  for (int m = 1; m <= audio::NUM_MICS; m++)
    page += "<a href='http://" + ip + ":81/beam.wav?mic=" + m + "'>" + m + "</a> ";
  page += String("&middot; <a href='http://") + ip + ":81/raw.wav?s=5'>raw.wav (5 s, " +
          engine::sourceChannels(a.source) + " ch)</a> &middot; "
          "<a href='/status.json'>status.json</a></p>";
  if (board) page += "<p>With the inbuilt source every stream above is the board mic.</p>";
}

static void statusJson() {
  engine::Status a;
  engine::status(&a);
  const audio::Calibration cal = engine::calibration();
  const board_io::Info b = board_io::info();
  const bool board = a.source == engine::Source::Inbuilt;
  String j = "{";
  appendf(j, "\"source\":\"%s\",\"sourceTarget\":\"%s\",\"switching\":%s,\"sourceSwitches\":%lu,"
          "\"liveChannels\":%d,",
          engine::sourceName(a.source), engine::sourceName(a.sourceTarget),
          a.source != a.sourceTarget ? "true" : "false", (unsigned long)a.sourceSwitches,
          engine::sourceChannels(a.source));
  appendf(j, "\"codec\":{\"init\":\"%s\",\"powered\":%s,\"inits\":%lu,\"failures\":%lu,\"detail\":\"%s\"},",
          a.codecInit == engine::CodecInit::Never ? "never" : a.codecInit == engine::CodecInit::Ok ? "ok" : "failed",
          a.codecPowered ? "true" : "false", (unsigned long)a.codecInits, (unsigned long)a.codecFailures,
          b.codecDetail);
  appendf(j, "\"touch\":{\"ready\":%s,\"ok\":%s,\"taps\":%lu,\"errors\":%lu},",
          b.touchReady ? "true" : "false", b.touchOk ? "true" : "false", (unsigned long)b.taps,
          (unsigned long)b.touchErrors);
  // The board mic has no DOA or beam: null, not a made-up 0°.
  appendf(j, "\"mode\":\"%s\",\"doaAvailable\":%s,", proc::modeName(a.mode), board ? "false" : "true");
  if (board)
    j += "\"doa\":null,\"confidence\":null,\"tracked\":null,\"trackedValid\":false,\"steer\":null,";
  else
    appendf(j, "\"doa\":%.2f,\"confidence\":%.3f,\"tracked\":%.2f,\"trackedValid\":%s,\"steer\":%.2f,",
            a.doaDeg, a.confidence, a.smoothDeg, a.smoothValid ? "true" : "false", a.steerDeg);
  appendf(j, "\"fixed\":%.2f,\"speech\":%s,", a.fixedDeg, a.speech ? "true" : "false");
  if (board)
    appendf(j, "\"micDb\":[%.2f,null,null,null],\"micPeakDb\":[%.2f,null,null,null],\"beamDb\":%.2f,",
            a.micDb[0], a.micPeakDb[0], a.beamDb);
  else
    appendf(j, "\"micDb\":[%.2f,%.2f,%.2f,%.2f],\"micPeakDb\":[%.2f,%.2f,%.2f,%.2f],\"beamDb\":%.2f,",
            a.micDb[0], a.micDb[1], a.micDb[2], a.micDb[3],
            a.micPeakDb[0], a.micPeakDb[1], a.micPeakDb[2], a.micPeakDb[3], a.beamDb);
  appendf(j, "\"lead\":[%.3f,%.3f,%.3f,%.3f],\"leadCoh\":[%.3f,%.3f,%.3f,%.3f],\"leadAgeMs\":%ld,",
          a.lead[0], a.lead[1], a.lead[2], a.lead[3], a.leadCoh[0], a.leadCoh[1], a.leadCoh[2], a.leadCoh[3],
          a.leadMs ? (long)(millis() - a.leadMs) : -1L);
  appendf(j, "\"dspMeanUs\":%.1f,\"dspMaxUs\":%.1f,\"dspPeakUs\":%.1f,\"hopUs\":%.1f,\"hops\":%lu,",
          a.dspMeanUs, a.dspMaxUs, a.dspPeakUs, audio::HOP_US, (unsigned long)a.hops);
  appendf(j, "\"running\":%s,\"frames\":[%lu,%lu],\"overflows\":[%lu,%lu],\"timeouts\":[%lu,%lu],"
          "\"lowByteNonzero\":[%lu,%lu],\"mismatches\":%lu,\"resyncs\":%lu,\"hwFaults\":%lu,",
          a.running ? "true" : "false", (unsigned long)a.frames[0], (unsigned long)a.frames[1],
          (unsigned long)a.overflows[0], (unsigned long)a.overflows[1],
          (unsigned long)a.timeouts[0], (unsigned long)a.timeouts[1],
          (unsigned long)a.lowByteNonzero[0], (unsigned long)a.lowByteNonzero[1],
          (unsigned long)a.mismatches, (unsigned long)a.resyncs, (unsigned long)a.hwFaults);
  appendf(j, "\"ringDepth\":%u,\"ringHigh\":%u,\"ringDrops\":%lu,\"streamDrops\":%lu,\"clipped\":%lu,",
          a.ringDepth, a.ringHigh, (unsigned long)a.ringDrops, (unsigned long)a.streamDrops,
          (unsigned long)a.clipped);
  appendf(j, "\"align\":{\"status\":\"%s\",\"offset\":%d,\"bitShift\":%d,\"dropMaster\":%d,\"dropSlave\":%d,"
          "\"runs\":%lu,\"matched\":%lu,\"firstOffset\":%d,\"changes\":%lu},",
          alignText(a), a.align.slotOffset, a.align.bitShift, a.align.dropMaster, a.align.dropSlave,
          (unsigned long)a.alignRuns, (unsigned long)a.alignConclusive, a.alignFirstOffset,
          (unsigned long)a.alignChanges);
  appendf(j, "\"cal\":{\"map\":\"%d%d%d%d\",\"gainDb\":[%.2f,%.2f,%.2f,%.2f],\"advanceUs\":[%.1f,%.1f,%.1f,%.1f],"
          "\"gainCal\":%d},",
          cal.posOfChannel[0] + 1, cal.posOfChannel[1] + 1, cal.posOfChannel[2] + 1, cal.posOfChannel[3] + 1,
          cal.gainDb[0], cal.gainDb[1], cal.gainDb[2], cal.gainDb[3],
          cal.advanceUs[0], cal.advanceUs[1], cal.advanceUs[2], cal.advanceUs[3], a.gainCal);
  appendf(j, "\"fftMaxErr\":%.3e,\"fftEspUs\":%.1f,\"fftRefUs\":%.1f,\"heapInternal\":%u,\"heapPsram\":%u}",
          a.fftMaxErr, a.fftEspUs, a.fftRefUs,
          (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
          (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
  http.send(200, "application/json", j);
}

// /control: GET or POST, any combination of source, mode, angle, map, gain,
// advance, calgain (seconds), resync, resetcal. Redirects back to / unless
// ?json. 400 for a bad value; 409 if the source can't switch right now.
static void controlHandler() {
  String err;
  bool busy = false;
  if (http.hasArg("source")) {
    engine::Source src;
    if (!engine::parseSource(http.arg("source").c_str(), &src)) err += "source: array|inbuilt\n";
    else if (!engine::requestSource(src)) busy = true;   // a raw capture is armed
  }
  if (http.hasArg("mode")) {
    proc::Mode m;
    if (proc::parseMode(http.arg("mode").c_str(), &m)) engine::setMode(m);
    else err += "mode: raw|doa|fixed|auto\n";
  }
  if (http.hasArg("angle") && http.arg("angle").length()) {
    const float a = http.arg("angle").toFloat();
    if (isfinite(a) && fabsf(a) <= 180.0f) engine::setFixedAngle(a);
    else err += "angle: degrees, -180..180\n";
  }
  if (http.hasArg("map") || http.hasArg("gain") || http.hasArg("advance")) {
    audio::Calibration c = engine::calibration();
    bool ok = true;
    if (http.hasArg("map") && !parseMap(http.arg("map"), c.posOfChannel)) { ok = false; err += "map: a permutation of 1234\n"; }
    if (http.hasArg("gain") && !parseFloats(http.arg("gain"), c.gainDb)) { ok = false; err += "gain: four comma-separated dB values\n"; }
    if (http.hasArg("advance") && !parseFloats(http.arg("advance"), c.advanceUs)) { ok = false; err += "advance: four comma-separated us values\n"; }
    for (int i = 0; ok && i < audio::NUM_MICS; i++)
      if (fabsf(c.gainDb[i]) > audio::GAIN_TRIM_MAX_DB || fabsf(c.advanceUs[i]) > audio::ADVANCE_TRIM_MAX_US) {
        ok = false;
        err += "gain within +-6 dB, advance within +-500 us\n";
      }
    if (ok) engine::setCalibration(c);
  }
  if (http.hasArg("resetcal")) engine::setCalibration(audio::DEFAULT_CALIBRATION);
  if (http.hasArg("calgain") && !engine::startGainCal(http.arg("calgain").toFloat()))
    err += "calgain: 0.5-30 seconds, not already running, array source only\n";
  if (http.hasArg("resync")) engine::requestResync();

  if (err.length()) { http.send(400, "text/plain", err); return; }
  if (busy) { http.send(409, "text/plain", "source: a /raw.wav capture is running; try again when it's done\n"); return; }
  if (http.hasArg("json")) { statusJson(); return; }
  http.sendHeader("Location", "/", true);
  http.send(303, "text/plain", "");
}

static void statusPage() {
  String ssid = WiFi.SSID();
  htmlEscape(ssid);
  uint32_t up = millis() / 1000;
  char upBuf[24];
  snprintf(upBuf, sizeof(upBuf), "%lud %02lu:%02lu:%02lu",
           (unsigned long)(up / 86400), (unsigned long)(up / 3600 % 24),
           (unsigned long)(up / 60 % 60), (unsigned long)(up % 60));

  String ip = WiFi.localIP().toString();
  String page = PAGE_HEAD;
  page += "<style>body{max-width:640px}</style>"
          "<h2>gulltoppr</h2><table>"
          "<tr><td>Network</td><td>" + ssid + "</td></tr>"
          "<tr><td>IP</td><td>" + ip + "</td></tr>"
          "<tr><td>Signal</td><td>" + String(WiFi.RSSI()) + " dBm</td></tr>"
          "<tr><td>Uptime</td><td>" + upBuf + "</td></tr>"
          "<tr><td>Free RAM</td><td>" +
          String(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024) + " KB internal, " +
          String(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024) + " KB PSRAM</td></tr>"
          "</table>";
  audioStatusHtml(page, ip);
  page += "</body></html>";
  http.send(200, "text/html", page);
}

// Handlers are registered exactly once (WebServer can't deregister) and
// branch on wf so the same server backs both the portal and gulltoppr.local.
static void httpSetup() {
  if (httpRegistered) { http.begin(); return; }   // begin() is restart-safe
  httpRegistered = true;
  http.on("/", []() {
    if (wf == WF_ONLINE) statusPage();
    else                 portalPage();
  });
  http.on("/save", HTTP_POST, []() {
    if (wf != WF_PORTAL) { http.send(409, "text/plain", "not in setup mode"); return; }
    String ssid = http.arg("ssid"), pass = http.arg("pass");
    if (!ssid.length() || ssid.length() > 32 || pass.length() > 64) {
      http.send(400, "text/plain", "ssid required (max 32), password max 64");
      return;
    }
    snprintf(wfSsid, sizeof(wfSsid), "%s", ssid.c_str());
    snprintf(wfPass, sizeof(wfPass), "%s", pass.c_str());
    String page = PAGE_HEAD;
    page += "<p>Connecting to your network &mdash; the <b>gulltoppr</b> hotspot will "
            "disappear once joined. Then open <a href='http://gulltoppr.local/'>"
            "gulltoppr.local</a> from your network.</p>"
            "<p>If the hotspot is still here after ~30s, reopen "
            "<a href='/'>the setup page</a>.</p></body></html>";
    http.send(200, "text/html", page);
    joinPending = true;   // WiFi.begin() runs in wifiLoop(), never in a handler
  });
  // Audio controls work in portal mode too: the array doesn't need a network.
  http.on("/control", controlHandler);
  http.on("/status.json", statusJson);
  http.onNotFound([]() {
    if (wf == WF_PORTAL) {
      // Universal captive trigger: OS probe URLs (generate_204,
      // hotspot-detect.html, connecttest.txt, ...) all land here.
      http.sendHeader("Location", "http://192.168.4.1/", true);
      http.send(302, "text/plain", "");
    } else {
      http.send(404, "text/plain", "not found");
    }
  });
  http.begin();
}

static void portalStart() {
  WiFi.mode(WIFI_AP_STA);          // STA half stays usable for join attempts
  WiFi.softAP(NAME);               // open AP at 192.168.4.1
  dns.start(53, "*", WiFi.softAPIP());
  WiFi.scanNetworks(true);         // async; results feed the form's datalist
  httpSetup();
  wf = WF_PORTAL;
  joining = JOIN_NONE;
  nextSavedRetry = millis() + SAVED_RETRY_PERIOD_MS;
  Serial.printf("wifi: portal up — join '%s' AP to configure\n", NAME);
}

static void portalStop() {
  dns.stop();
  WiFi.softAPdisconnect(true);
  WiFi.mode(WIFI_STA);
}

static void goOnline() {
  // Re-run mDNS on every (re)connect so the responder binds the fresh netif.
  MDNS.end();
  if (MDNS.begin(NAME)) MDNS.addService("http", "tcp", 80);
  httpSetup();
  wf = WF_ONLINE;
  wasOnline = true;
  Serial.printf("wifi: online on '%s', http://%s.local/ (%s)\n",
                WiFi.SSID().c_str(), NAME, WiFi.localIP().toString().c_str());
}

static void wifiStart() {
  WiFi.persistent(false);          // creds live in our NVS namespace, not the stack's
  WiFi.setHostname(NAME);          // must precede mode()/begin() to reach DHCP
  if (credsLoad(wfSsid, sizeof(wfSsid), wfPass, sizeof(wfPass))) {
    WiFi.mode(WIFI_STA);
    WiFi.begin(wfSsid, wfPass);
    wf = WF_CONNECTING;
    wfDeadline = millis() + JOIN_TIMEOUT_MS;
    Serial.printf("wifi: joining saved network '%s'\n", wfSsid);
  } else {
    portalStart();
  }
}

// Reopen the setup portal on demand (long-press on BOOT).
static void wifiForcePortal() {
  if (wf == WF_PORTAL) return;
  MDNS.end();
  joinPending = false;
  joinFailed = false;
  wasOnline = false;               // a failed join from here should keep the portal up
  WiFi.disconnect(false);          // drop the STA link; portalStart brings up AP_STA
  portalStart();
}

static void wifiLoop(uint32_t now) {
  switch (wf) {
    case WF_CONNECTING:
      if (WiFi.status() == WL_CONNECTED) { goOnline(); break; }
      if ((int32_t)(now - wfDeadline) > 0) {
        if (wasOnline) {
          // Link dropped after a good session — keep retrying quietly rather
          // than reopening the portal for a transient AP outage.
          WiFi.reconnect();
          wfDeadline = now + JOIN_TIMEOUT_MS;
        } else {
          WiFi.disconnect(false);
          portalStart();
        }
      }
      break;

    case WF_PORTAL:
      dns.processNextRequest();
      http.handleClient();

      if (joinPending) {
        joinPending = false;
        joinFailed = false;
        joining = JOIN_FORM;
        WiFi.scanDelete();
        WiFi.begin(wfSsid, wfPass);    // AP stays up (AP_STA) while we try
        wfDeadline = now + FORM_JOIN_TIMEOUT_MS;
        Serial.printf("wifi: portal join '%s'\n", wfSsid);
      } else if (joining == JOIN_NONE && (int32_t)(now - nextSavedRetry) > 0) {
        // The router may simply have been slower to boot than us (power cut).
        // Retry the saved network now and then, but only while nobody is
        // using the portal: joining moves the AP to the router's channel and
        // would knock a phone mid-setup off the hotspot.
        nextSavedRetry = now + SAVED_RETRY_PERIOD_MS;
        if (WiFi.softAPgetStationNum() == 0 &&
            credsLoad(wfSsid, sizeof(wfSsid), wfPass, sizeof(wfPass))) {
          joining = JOIN_SAVED;
          WiFi.begin(wfSsid, wfPass);
          wfDeadline = now + JOIN_TIMEOUT_MS;
          Serial.printf("wifi: portal retrying saved '%s'\n", wfSsid);
        }
      }

      if (joining != JOIN_NONE) {
        if (WiFi.status() == WL_CONNECTED) {
          // Only persist credentials that actually worked.
          if (joining == JOIN_FORM) credsSave(wfSsid, wfPass);
          joining = JOIN_NONE;
          joinFailed = false;
          portalStop();
          goOnline();
        } else if ((int32_t)(now - wfDeadline) > 0) {
          // Stop the STA retrying (a bad password makes it hop channels,
          // which disrupts the AP) and stay in the portal.
          Serial.printf("wifi: join '%s' failed\n", wfSsid);
          if (joining == JOIN_FORM) joinFailed = true;
          joining = JOIN_NONE;
          WiFi.disconnect(false);
          WiFi.scanNetworks(true);
        }
      }
      break;

    case WF_ONLINE:
      http.handleClient();
      if (WiFi.status() != WL_CONNECTED) {
        Serial.println("wifi: link lost, reconnecting");
        MDNS.end();
        wf = WF_CONNECTING;
        wfDeadline = now + JOIN_TIMEOUT_MS;
        WiFi.reconnect();
      }
      break;
  }
}

// ---------- display ----------
// Direct drawing, each element repainted only when what it shows changes. A
// full-screen sprite (150KB at 320x240) would fit in PSRAM, but a needle and
// four bars at 10 Hz don't need one.
//
//   y   0- 20  name, audio mode (or BOARD MIC)
//   y  22- 93  WiFi status (x 0-211), MIC source button (x 214-313, y 30-83)
//   y  96-112  DOA / beam readout (or capture state / sync fault / codec state)
//   y 114-220  DOA dial (left), per-channel level bars (right)
//   y 224-240  footer
static TFT_eSPI tft;

static const int WIFI_Y = 22, WIFI_H = 72;
static const int READOUT_Y = 96;
static const int FOOTER_Y = 224;
static const uint32_t AUDIO_DRAW_MS = 100;

// Mic source button: the right of the WiFi block, which the WiFi text is
// clipped to stay out of, and clear of the top and bottom edges where the
// touch calibration is weakest (BOARD-CALIBRATION.md). A tap within
// SRC_BTN_SLOP of it counts: the calibrated map is good to ~2-3 px here, a
// fingertip much less so.
static const int SRC_BTN_X = 214, SRC_BTN_Y = 30, SRC_BTN_W = 100, SRC_BTN_H = 54, SRC_BTN_R = 6;
static const int SRC_BTN_SLOP = 8;
static const int WIFI_TEXT_X = 12;
static const int WIFI_TEXT_W = SRC_BTN_X - WIFI_TEXT_X - 6;   // what the WiFi lines may use

// Dial: broadside (θ = 0) points up, + toward Mic 4 to the right. A linear
// array gets a half dial, −90..+90 — it can't see front from back; a planar
// layout gets a full circle.
struct Dial { int cx, cy, r; bool full; };
static Dial dial;

// Level bars, one per capture channel: ~100 ms RMS, with a white tick at the
// decaying raw peak (the tap test reads that tick). With the board mic only
// the first bar is live; the others are blanked, not shown at zero.
static const int BAR_X0 = 230, BAR_DX = 22, BAR_W = 14, BAR_TOP = 118, BAR_BOT = 202;
static const int BAR_LABEL_Y = BAR_BOT + 3, BAR_LABEL_H = 17;
static const float BAR_DB_MIN = -90.0f, BAR_DB_MAX = -10.0f;
static const uint16_t BAR_TRACK = 0x2104;   // dark grey

// Colours that mean the source, on the button and the mode label.
static const uint16_t COLOR_ARRAY = TFT_CYAN, COLOR_BOARD = TFT_ORANGE;

// Trim a line to fit `maxW` px in `font`, with ".." when shortened.
static String fitText(String s, int font, int maxW) {
  if (tft.textWidth(s, font) <= maxW) return s;
  while (s.length() && tft.textWidth(s + "..", font) > maxW) s.remove(s.length() - 1);
  return s + "..";
}

static void drawWifi() {
  // Everything the block shows, so we can skip redundant repaints.
  static String last;
  String line1, line2, line3;
  uint16_t color = TFT_WHITE;
  switch (wf) {
    case WF_CONNECTING:
      line1 = "connecting";
      line2 = wfSsid;
      color = TFT_YELLOW;
      break;
    case WF_PORTAL:
      if (joining == JOIN_FORM || joining == JOIN_SAVED) {
        line1 = "joining";
        line2 = wfSsid;
        color = TFT_YELLOW;
      } else {
        line1 = "wifi setup";
        line2 = String("join AP: ") + NAME;
        line3 = joinFailed ? "join failed - retry" : "then open 192.168.4.1";
        color = joinFailed ? TFT_RED : TFT_CYAN;
      }
      break;
    case WF_ONLINE:
      line1 = "online";
      line2 = String(NAME) + ".local";
      line3 = WiFi.localIP().toString();
      color = TFT_GREEN;
      break;
  }
  String key = line1 + '|' + line2 + '|' + line3 + '|' + color + '|' + (wf == WF_PORTAL);
  if (key == last) return;
  last = key;

  // Left of the source button only: it repaints itself.
  tft.fillRect(0, WIFI_Y, SRC_BTN_X - 2, WIFI_H, TFT_BLACK);
  tft.setTextDatum(TL_DATUM);
  tft.setTextColor(color, TFT_BLACK);
  tft.drawString(fitText(line1, 4, WIFI_TEXT_W), WIFI_TEXT_X, WIFI_Y + 2, 4);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  // Font 2; a long SSID is shortened rather than run under the button.
  tft.drawString(fitText(line2, 2, WIFI_TEXT_W), WIFI_TEXT_X, WIFI_Y + 34, 2);
  tft.drawString(fitText(line3, 2, WIFI_TEXT_W), WIFI_TEXT_X, WIFI_Y + 52, 2);
  tft.fillRect(0, FOOTER_Y, 320, 240 - FOOTER_Y, TFT_BLACK);
  if (wf != WF_PORTAL) {
    tft.setTextColor(TFT_DARKGREY, TFT_BLACK);
    tft.drawString("hold BOOT: wifi setup", 12, FOOTER_Y, 2);
  }
}

// The button shows the requested source, so a tap answers at once; it is
// yellow until that source is actually running, red if the codec failed.
static void drawSourceButton(const engine::Status& a) {
  const bool wantBoard = a.sourceTarget == engine::Source::Inbuilt;
  const bool settling = a.source != a.sourceTarget || !a.running;
  const bool dead = codecDead(a) && !(a.source != a.sourceTarget);
  const char* top = dead ? "CODEC FAIL" : settling ? "SWITCHING" : "MIC";
  const uint16_t color = dead ? TFT_RED : settling ? TFT_YELLOW : wantBoard ? COLOR_BOARD : COLOR_ARRAY;
  static String last;
  const String key = String(top) + '|' + wantBoard + '|' + color;
  if (key == last) return;
  last = key;
  tft.fillRect(SRC_BTN_X, SRC_BTN_Y, SRC_BTN_W, SRC_BTN_H, TFT_BLACK);
  tft.drawRoundRect(SRC_BTN_X, SRC_BTN_Y, SRC_BTN_W, SRC_BTN_H, SRC_BTN_R, color);
  tft.drawRoundRect(SRC_BTN_X + 1, SRC_BTN_Y + 1, SRC_BTN_W - 2, SRC_BTN_H - 2, SRC_BTN_R - 1, color);
  tft.setTextDatum(TC_DATUM);
  tft.setTextColor(color, TFT_BLACK);
  tft.drawString(top, SRC_BTN_X + SRC_BTN_W / 2, SRC_BTN_Y + 5, 2);
  tft.drawString(wantBoard ? "BOARD" : "ARRAY", SRC_BTN_X + SRC_BTN_W / 2, SRC_BTN_Y + 23, 4);
  tft.setTextDatum(TL_DATUM);
}

static bool inSourceButton(int x, int y) {
  return x >= SRC_BTN_X - SRC_BTN_SLOP && x < SRC_BTN_X + SRC_BTN_W + SRC_BTN_SLOP &&
         y >= SRC_BTN_Y - SRC_BTN_SLOP && y < SRC_BTN_Y + SRC_BTN_H + SRC_BTN_SLOP;
}

// Taps: the button toggles the requested source. Everything else on the
// screen is display only.
static void touchLoop() {
  int x, y;
  if (!board_io::takeTap(&x, &y)) return;
  if (!inSourceButton(x, y)) return;
  const engine::Source to = engine::sourceTarget() == engine::Source::Array ? engine::Source::Inbuilt
                                                                            : engine::Source::Array;
  const bool ok = engine::requestSource(to);
  Serial.printf("touch: tap %d,%d on MIC -> %s%s\n", x, y, engine::sourceName(to),
                ok ? "" : " refused (raw capture running)");
}

static void dialPoint(float deg, float r, int* x, int* y) {
  const float t = dsp::deg2rad(deg);
  *x = dial.cx + (int)lroundf(r * sinf(t));
  *y = dial.cy - (int)lroundf(r * cosf(t));
}

// Ticks every 30° and the end labels; redrawn after each needle erase, which
// can clip them.
static void drawDialMarks() {
  for (int a = dial.full ? -150 : -90; a <= (dial.full ? 180 : 90); a += 30) {
    int x0, y0, x1, y1;
    dialPoint(a, dial.r - 9, &x0, &y0);
    dialPoint(a, dial.r - 3, &x1, &y1);
    tft.drawLine(x0, y0, x1, y1, TFT_DARKGREY);
  }
  if (!dial.full) {
    tft.setTextColor(TFT_DARKGREY, TFT_BLACK);
    tft.setTextDatum(BL_DATUM);
    tft.drawString("M1", dial.cx - dial.r + 10, dial.cy - 4, 2);
    tft.setTextDatum(BR_DATUM);
    tft.drawString("M4", dial.cx + dial.r - 10, dial.cy - 4, 2);
    tft.setTextDatum(TL_DATUM);
  }
}

// "DOA n/a" inside the dial while the board mic plays: one mic has no
// direction, and an empty dial would look like silence rather than "can't".
// The box sits inside the arc (clear of it at the top corners) and above the hub.
static const int DIAL_NA_W = 110, DIAL_NA_H = 50, DIAL_NA_DY = 60;

static void dialNotAvailable(bool show) {
  const int x = dial.cx - DIAL_NA_W / 2, y = dial.cy - DIAL_NA_DY;
  tft.fillRect(x, y, DIAL_NA_W, DIAL_NA_H, TFT_BLACK);
  if (show) {
    tft.setTextDatum(TC_DATUM);
    tft.setTextColor(COLOR_BOARD, TFT_BLACK);
    tft.drawString("DOA n/a", dial.cx, y + 2, 4);
    tft.setTextColor(TFT_DARKGREY, TFT_BLACK);
    tft.drawString("single mic", dial.cx, y + 32, 2);
    tft.setTextDatum(TL_DATUM);
  } else {
    drawDialMarks();
  }
}

static void drawBarLabels(int live) {
  tft.fillRect(BAR_X0 - 6, BAR_LABEL_Y, 320 - (BAR_X0 - 6), BAR_LABEL_H, TFT_BLACK);
  tft.setTextColor(TFT_DARKGREY, TFT_BLACK);
  if (live == 1) {
    tft.setTextDatum(TL_DATUM);
    tft.drawString("board", BAR_X0 - 4, BAR_LABEL_Y, 2);
    return;
  }
  tft.setTextDatum(TC_DATUM);
  for (int c = 0; c < audio::NUM_MICS; c++)
    tft.drawString(String(c + 1), BAR_X0 + c * BAR_DX + BAR_W / 2, BAR_LABEL_Y, 2);
  tft.setTextDatum(TL_DATUM);
}

static void drawAudioFrame() {
  const dsp::Geometry& g = engine::geometry();
  dial.full = !g.linear;
  if (dial.full) { dial.cx = 110; dial.cy = 170; dial.r = 48; }
  else           { dial.cx = 110; dial.cy = 212; dial.r = 88; }
  // TFT_eSPI arcs start at 6 o'clock and run clockwise: 90..270 is the top half.
  tft.drawArc(dial.cx, dial.cy, dial.r, dial.r - 2, dial.full ? 0 : 90, dial.full ? 360 : 270,
              TFT_DARKGREY, TFT_BLACK, false);
  drawDialMarks();
  // Bar labels are drawn by drawAudio(), which knows how many channels are live.
}

static int barPx(float db) {
  const float f = (db - BAR_DB_MIN) / (BAR_DB_MAX - BAR_DB_MIN);
  const int h = (int)lroundf(f * (BAR_BOT - BAR_TOP));
  return h < 0 ? 0 : h > BAR_BOT - BAR_TOP ? BAR_BOT - BAR_TOP : h;
}

static void steerTriangle(float deg, uint16_t color) {
  int x0, y0, x1, y1, x2, y2;
  dialPoint(deg, dial.r + 2, &x0, &y0);
  dialPoint(deg - 4, dial.r + 10, &x1, &y1);
  dialPoint(deg + 4, dial.r + 10, &x2, &y2);
  tft.fillTriangle(x0, y0, x1, y1, x2, y2, color);
}

static void drawAudio(uint32_t now) {
  static uint32_t next = 0;
  if ((int32_t)(now - next) < 0) return;
  next = now + AUDIO_DRAW_MS;

  engine::Status a;
  engine::status(&a);
  const bool board = a.source == engine::Source::Inbuilt;
  const int live = engine::sourceChannels(a.source);

  drawSourceButton(a);

  // Mode, top right: the array's mode, or that the board mic has none.
  static String lastMode;
  String mode = board ? "BOARD MIC" : proc::modeName(a.mode);
  if (mode != lastMode) {
    lastMode = mode;
    tft.fillRect(160, 0, 160, 20, TFT_BLACK);
    tft.setTextDatum(TR_DATUM);
    tft.setTextColor(board ? COLOR_BOARD : COLOR_ARRAY, TFT_BLACK);
    tft.drawString(mode, 308, 4, 2);
    tft.setTextDatum(TL_DATUM);
  }

  // Readout line: numbers when running, otherwise why not.
  const bool beam = !board && (a.mode == proc::Mode::FixedBeam || a.mode == proc::Mode::AutoBeam);
  const bool fault = !board && (a.alignChanges || a.align.status == sync_check::Status::NoMatch ||
                                a.align.status == sync_check::Status::BitShift);
  char buf[64];
  uint16_t color = TFT_WHITE;
  if (board && codecDead(a)) {
    snprintf(buf, sizeof(buf), "BOARD MIC: codec init failed (see /)");
    color = TFT_RED;
  } else if (board && !a.running) {
    snprintf(buf, sizeof(buf), "board mic starting (codec init, warm-up)");
    color = TFT_YELLOW;
  } else if (board) {
    snprintf(buf, sizeof(buf), "board mic %.0f dBFS   DOA/beam n/a", a.micDb[0]);
  } else if (fault) {
    snprintf(buf, sizeof(buf), "SYNC FAULT: %s", a.alignChanges ? "offset changed" : sync_check::statusName(a.align.status));
    color = TFT_RED;
  } else if (!a.running) {
    snprintf(buf, sizeof(buf), "capture starting (warm-up, self-test)");
    color = TFT_YELLOW;
  } else if (a.mode == proc::Mode::Raw) {
    snprintf(buf, sizeof(buf), "raw capture, no DOA");
  } else {
    // The tracked DOA only: a raw scan of silence still has a (meaningless) peak.
    int n = a.smoothValid ? snprintf(buf, sizeof(buf), "DOA %+.0f  conf %.2f", a.smoothDeg, a.confidence)
                          : snprintf(buf, sizeof(buf), "DOA --  conf %.2f", a.confidence);
    if (beam) snprintf(buf + n, sizeof(buf) - n, "  beam %+.0f", a.steerDeg);
    color = a.speech ? TFT_GREEN : TFT_WHITE;
  }
  static String lastReadout;
  String readout = String(buf) + '|' + color;
  if (readout != lastReadout) {
    lastReadout = readout;
    tft.fillRect(0, READOUT_Y, 320, 17, TFT_BLACK);
    tft.setTextColor(color, TFT_BLACK);
    tft.drawString(buf, 12, READOUT_Y, 2);
  }

  // Board mic: the "n/a" panel replaces needle and steering marker. Cleared
  // before the needle can come back, drawn after it has been erased.
  static bool naShown = false;
  if (!board && naShown) { dialNotAvailable(false); naShown = false; }

  // Needle at the tracked DOA: bright while the speech gate is open, dim in
  // silence, absent until something has been tracked.
  static int needleDeg = 9999;
  static uint16_t needleColor = 0;
  const bool showNeedle = !board && a.smoothValid && a.mode != proc::Mode::Raw;
  const int nd = showNeedle ? (int)lroundf(a.smoothDeg) : 9999;
  const uint16_t nc = a.speech ? TFT_GREEN : TFT_DARKGREEN;
  if (nd != needleDeg || (showNeedle && nc != needleColor)) {
    const float nr = dial.r - 12;
    int x, y;
    if (needleDeg != 9999) {
      dialPoint(needleDeg, nr, &x, &y);
      tft.drawWideLine(dial.cx, dial.cy, x, y, 5, TFT_BLACK, TFT_BLACK);
      drawDialMarks();
    }
    if (showNeedle) {
      dialPoint(nd, nr, &x, &y);
      tft.drawWideLine(dial.cx, dial.cy, x, y, 3, nc, TFT_BLACK);
    }
    tft.fillCircle(dial.cx, dial.cy, 4, TFT_LIGHTGREY);
    needleDeg = nd;
    needleColor = nc;
  }

  // Beam steering marker, outside the arc, in beam modes only.
  static int steerDeg = 9999;
  const int sd = beam ? (int)lroundf(a.steerDeg) : 9999;
  if (sd != steerDeg) {
    if (steerDeg != 9999) steerTriangle(steerDeg, TFT_BLACK);
    if (sd != 9999) steerTriangle(sd, TFT_ORANGE);
    steerDeg = sd;
  }

  if (board && !naShown) { dialNotAvailable(true); naShown = true; }

  // Level bars: the live channels only. A change in how many are live
  // relabels, blanks the dead ones and forces the live ones to repaint.
  static int lastLive = 0;
  static int lastH[audio::NUM_MICS] = {-1, -1, -1, -1}, lastPk[audio::NUM_MICS] = {-1, -1, -1, -1};
  if (live != lastLive) {
    lastLive = live;
    drawBarLabels(live);
    for (int c = 0; c < audio::NUM_MICS; c++) {
      lastH[c] = lastPk[c] = -1;
      if (c >= live) tft.fillRect(BAR_X0 + c * BAR_DX, BAR_TOP, BAR_W, BAR_BOT - BAR_TOP, TFT_BLACK);
    }
  }
  for (int c = 0; c < live; c++) {
    const int h = barPx(a.micDb[c]), pk = barPx(a.micPeakDb[c]);
    if (h == lastH[c] && pk == lastPk[c]) continue;
    lastH[c] = h;
    lastPk[c] = pk;
    const int x = BAR_X0 + c * BAR_DX;
    tft.fillRect(x, BAR_TOP, BAR_W, (BAR_BOT - BAR_TOP) - h, BAR_TRACK);
    tft.fillRect(x, BAR_BOT - h, BAR_W, h, a.micPeakDb[c] > -3.0f ? TFT_RED : TFT_GREEN);
    if (pk > 0) tft.drawFastHLine(x, BAR_BOT - pk, BAR_W, TFT_WHITE);
  }
}

// ---------- serial ----------
// One compact line a second, only from here (loop()): the audio tasks never
// print. With setTxTimeoutMs(0) in setup() this never blocks either.
static void serialLoop(uint32_t now) {
  static uint32_t next = 0;
  if ((int32_t)(now - next) < 0) return;
  next = now + 1000;
  engine::Status a;
  engine::status(&a);
  const char* to = a.source != a.sourceTarget ? (a.sourceTarget == engine::Source::Inbuilt ? "->inbuilt" : "->array") : "";
  if (a.source == engine::Source::Inbuilt) {
    // One channel and no spatial processing: say so instead of printing zeros.
    Serial.printf("[audio] src=inbuilt%s codec=%s%s run=%d lvl=%.0f pk=%.0f out=%.0f dBFS doa=n/a beam=n/a "
                  "(array mode %s kept) dsp=%.0f/%.0fus(%.0f%%) ring=%u/%u rdrop=%lu ovf=%lu to=%lu "
                  "rs=%lu sw=%lu sdrop=%lu clip=%lu\n",
                  to, codecInitText(a.codecInit), a.codecPowered ? "" : "(off)", a.running,
                  a.micDb[0], a.micPeakDb[0], a.beamDb, proc::modeName(a.mode),
                  a.dspMeanUs, a.dspMaxUs, 100.0f * a.dspMaxUs / audio::HOP_US,
                  a.ringDepth, a.ringHigh, (unsigned long)a.ringDrops, (unsigned long)a.overflows[0],
                  (unsigned long)a.timeouts[0], (unsigned long)a.resyncs, (unsigned long)a.sourceSwitches,
                  (unsigned long)a.streamDrops, (unsigned long)a.clipped);
    return;
  }
  Serial.printf("[audio] src=array%s %s doa=%+.1f c=%.2f trk=%s%+.1f beam=%+.1f gate=%d "
                "lvl=%.0f/%.0f/%.0f/%.0f out=%.0f dBFS dsp=%.0f/%.0fus(%.0f%%) "
                "ring=%u/%u rdrop=%lu ovf=%lu/%lu to=%lu/%lu rs=%lu sw=%lu mm=%lu lb=%lu/%lu "
                "align=%s@%d(%lu/%lu chg=%lu) sdrop=%lu\n",
                to, proc::modeName(a.mode), a.doaDeg, a.confidence, a.smoothValid ? "" : "?", a.smoothDeg,
                a.steerDeg, a.speech, a.micDb[0], a.micDb[1], a.micDb[2], a.micDb[3], a.beamDb,
                a.dspMeanUs, a.dspMaxUs, 100.0f * a.dspMaxUs / audio::HOP_US,
                a.ringDepth, a.ringHigh, (unsigned long)a.ringDrops,
                (unsigned long)a.overflows[0], (unsigned long)a.overflows[1],
                (unsigned long)a.timeouts[0], (unsigned long)a.timeouts[1],
                (unsigned long)a.resyncs, (unsigned long)a.sourceSwitches, (unsigned long)a.mismatches,
                (unsigned long)a.lowByteNonzero[0], (unsigned long)a.lowByteNonzero[1],
                alignText(a), a.align.slotOffset, (unsigned long)a.alignConclusive,
                (unsigned long)a.alignRuns, (unsigned long)a.alignChanges,
                (unsigned long)a.streamDrops);
}

// ---------- buttons ----------
static void buttonsLoop(uint32_t now) {
  static uint32_t downAt = 0;
  static bool fired = false;
  bool down = digitalRead(PIN_BTN_BOOT) == LOW;
  if (!down) { downAt = 0; fired = false; return; }
  if (!downAt) downAt = now ? now : 1;
  if (!fired && now - downAt >= PORTAL_HOLD_MS) {
    fired = true;
    Serial.println("button: reopening setup portal");
    wifiForcePortal();
  }
}

// ---------- entry points ----------
void setup() {
  Serial.begin(115200);
  // USB-CDC writes otherwise block up to 100 ms per chunk, with retries, when
  // the Mac has the port open but isn't reading — the normal state with the
  // board plugged in and no monitor running.
  Serial.setTxTimeoutMs(0);
  pinMode(PIN_BTN_BOOT, INPUT_PULLUP);

  tft.init();
  tft.setRotation(3);              // landscape 320x240
  tft.fillScreen(TFT_BLACK);
  tft.setTextDatum(TL_DATUM);
  tft.setTextColor(TFT_DARKGREY, TFT_BLACK);
  tft.drawString(NAME, 12, 4, 2);

  board_io::begin();               // I2C bus + touch reset; the codec stays untouched until inbuilt
  wifiStart();
  const bool audioOk = engine::begin();
  audio_server::begin();           // after wifiStart(): needs the network stack up
  drawAudioFrame();
  drawWifi();
  if (!audioOk) {
    tft.setTextColor(TFT_RED, TFT_BLACK);
    tft.drawString("audio engine failed to start", 12, READOUT_Y, 2);
  }

  engine::Status a;
  engine::status(&a);
  Serial.printf("audio: %s; fft check esp-dsp vs portable %.1e rel (%.0f vs %.0f us)\n",
                audioOk ? "running" : "FAILED to start", a.fftMaxErr, a.fftEspUs, a.fftRefUs);
  // PSRAM size 0 means the octal-PSRAM build keys in platformio.ini are off.
  Serial.printf("[mem] internal free %u (largest %u), psram: %u/%u free\n",
                (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
                (unsigned)ESP.getFreePsram(), (unsigned)ESP.getPsramSize());
}

void loop() {
  uint32_t now = millis();
  wifiLoop(now);
  buttonsLoop(now);
  board_io::loop(now);             // codec jobs for the capture task, touch polling
  touchLoop();
  engine::loopPoll();
  drawWifi();
  drawAudio(now);
  serialLoop(now);
  delay(2);   // yield to the WiFi/idle tasks
}
