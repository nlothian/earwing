// earwing — 4-mic beamforming array on an ESP32-S3 (Freenove FNK0104, see
// platformio.ini). This file is the WiFi lifecycle and the user interface:
// the web pages, the TFT, and a 1 Hz serial line. The audio lives in
// audio_engine.* (I2S capture + DSP tasks on core 1), processor.* / doa.* /
// beamformer.* (the portable DSP) and audio_server.* (the WAV streams); the
// servers themselves and the static web app in web_server.*; the HTTPS
// certificate in tls_cert.*; the board's I2C devices (ES8311 codec for the
// inbuilt mic, FT6336U touch) in board_io.*.
//
// Once online (http://earwing.local/ and https://earwing.local/):
//   /                 the transcriber: a static web app (source in ../site),
//                     served gzipped from LittleFS, that runs speech-to-text
//                     in the browser on /beam.wav from its own origin. Over
//                     HTTPS when that's up — plain http://.../ redirects
//                     there, because only a secure origin gets WebGPU and
//                     threaded WASM; over HTTP if HTTPS couldn't start
//   /status           status page, audio controls, HTTPS and certificate
//                     trust instructions (in the portal too)
//   /control, /status.json   the same as an API
//   /earwing.crt    the HTTPS certificate, to trust it once per device
//   /beam.wav, /raw.wav   audio (audio_server.h); also on port 81 (legacy)
//   /meeting          POST: the web app's live meeting summary, for the
//                     Meeting tab (meeting.h; format in meeting_types.h);
//                     /meeting.json shows what's stored
// Every other path is the web app's files, on both ports. The certificate is
// a self-signed ECDSA key made on the device at first boot and kept in NVS
// (tls_cert.h); a POST of regencert=1 to /control makes a new one.
//
// Firmware: `pio run -t upload` here. Web app: `cd ../site && npm run deploy`,
// or `npm run build:device` there (fills data/) and `pio run -t uploadfs`
// here. The two flash independently; without the web app "/" says so.
//
// The page handlers run in the web servers' tasks on core 0 (web_server.h),
// not in loop(): they read the WiFi state from a snapshot loop() publishes
// (NetView) and leave anything that changes it — a portal join, a scan, a new
// certificate — as a flag for loop() to act on. Engine controls from
// concurrent requests are serialised by ctlLock. The TFT is loop()'s alone.
//
// Mic source: "array" (the 4× INMP441, every boot) or "inbuilt" (the board's
// own mic), switched from /control?source= or the on-screen button.
//
// WiFi lifecycle: STA join with saved credentials, captive-portal fallback
// for first-time setup, and mDNS-advertised web servers at earwing.local
// once online. No WiFiManager: it wants to own the radio and blocks by
// default, whereas everything here is called from loop() and never blocks,
// so the audio tasks and the UI share core 1 happily.
//
//   CONNECTING --ok--> ONLINE --link lost--> CONNECTING (retry quietly)
//       |                 |
//    timeout /       hold right button
//    no creds             v
//       +-----------> PORTAL --form join ok / saved join ok--> ONLINE
//
// Portal: open AP "earwing" at 192.168.4.1 with a wildcard DNS server, so
// phones/laptops pop the setup page automatically. HTTP only: HTTPS starts
// when the device is online and stops for the portal; it keeps running
// through a link drop (its listener isn't tied to the address).
//
// Screen: three tabs (ui_meeting.h). DEVICE, at boot, is the device's own
// status: the web app's address, the DOA dials and the level bars.
// SETTINGS has the WiFi settings button and the Array | Board mic switch.
// MEETING is the web app's live meeting summary (POST /meeting), as a Follow
// feed of what changed or one section at a time; the first summary of a
// session switches to it once, in Follow.
//
// Setup portal: on Settings, tap WiFi settings and confirm, or hold BOOT
// (GPIO0) for 2s, to reopen it, e.g. to move the device to a different
// network. Saved credentials stay in NVS until a portal join succeeds with
// replacements. On the touch screen: tap MEETING, DEVICE or SETTINGS at the
// top to change tabs; on Settings, tap a side of the mic switch to pick the
// source; on Meeting, tap a mode button along the bottom, or the text to
// turn the page.

#include <Arduino.h>
#include <WiFi.h>
#include <DNSServer.h>
#include <ESPmDNS.h>
#include <Preferences.h>
#include <TFT_eSPI.h>   // configured entirely via build_flags in platformio.ini
#include <esp_heap_caps.h>
#include <esp_system.h>
#include <freertos/semphr.h>
#include <stdarg.h>
#include "audio_engine.h"
#include "audio_server.h"
#include "board_io.h"
#include "meeting.h"
#include "tls_cert.h"
#include "ui_meeting.h"
#include "web_server.h"

static const char* NAME = "earwing";   // AP SSID, mDNS host, DHCP hostname

// ---------- pins ----------
static const int PIN_BTN_BOOT = 0;     // the board's only user button; boot-strap pin, fine after reset

// ---------- timing ----------
static const uint32_t JOIN_TIMEOUT_MS        = 15000;   // boot / reconnect attempt
static const uint32_t FORM_JOIN_TIMEOUT_MS   = 20000;   // join from portal form
static const uint32_t SAVED_RETRY_PERIOD_MS  = 60000;   // portal: retry saved creds this often (nobody on the AP)
static const uint32_t SCAN_PERIOD_MS         = 30000;   // portal: refresh the network list this often
static const uint32_t SCAN_RETRY_MS          = 2000;    // portal: after a scan that wouldn't start
static const uint32_t JOIN_RETRY_MS          = 4000;    // portal: re-issue a join the AP didn't answer

// ---------- radio ----------
// TX power, set after every WiFi.mode(). At the default (~20 dBm) this board
// hears the network fine (-54 dBm) but nothing hears it: every join timed
// out with AUTH_EXPIRE / 4WAY_HANDSHAKE_TIMEOUT on all three mesh nodes and
// with a different MAC, and a Mac next to it never saw the setup AP. Turned
// down to 2 dBm the AP appeared at once, and the next join at 13 dBm went
// straight through: the supply sagging under full-power TX bursts (USB cable
// or port, or the board's regulator), not anything in the protocol. 13 dBm
// is plenty for a node at -54 dBm and leaves the regulator some headroom.
static const wifi_power_t TX_POWER = WIFI_POWER_13dBm;

// After every WiFi.mode(): the TX power above, and no modem sleep. With the
// default power save the radio naps between beacons, which put 60-140 ms on
// every round trip; one TCP socket's 5.7 KB send buffer over that is
// ~80 KB/s, less than three 32 KB/s streams and a page load need (they fell
// behind: stream drops, a 65 KB bundle taking 1-5 s). It costs average
// current, not the TX peaks above.
static void radioSetup() {
  WiFi.setTxPower(TX_POWER);
  WiFi.setSleep(false);
}
static const uint32_t PORTAL_HOLD_MS         = 2000;

// ---------- persistence (ESP32 NVS via Preferences) ----------
// Credentials live in our own namespace (engine::NVS_NS, with the
// calibration), not the WiFi stack's (persistent(false)).
static Preferences prefs;

static void credsSave(const char* ssid, const char* pass) {
  prefs.begin(engine::NVS_NS, false);
  prefs.putString("w_ssid", ssid);
  prefs.putString("w_pass", pass);
  prefs.end();
}

// The namespace was the device's name, so each rename (gulltoppr → heimdallr → earwing)
// stranded the saved network. Boards flashed with an older name get their
// credentials copied across once, on the first boot that finds none under NVS_NS.
static const char* const OLD_NAMES[] = {"heimdallr", "gulltoppr"};

static bool credsLoadFrom(const char* ns, char* ssid, size_t ssidLen, char* pass, size_t passLen) {
  ssid[0] = 0; pass[0] = 0;   // getString leaves the buffer untouched if the key is absent
  if (!prefs.begin(ns, true)) return false;   // read-only begin fails if ns doesn't exist
  prefs.getString("w_ssid", ssid, ssidLen);
  prefs.getString("w_pass", pass, passLen);
  prefs.end();
  return ssid[0] != 0;
}

static bool credsLoad(char* ssid, size_t ssidLen, char* pass, size_t passLen) {
  if (credsLoadFrom(engine::NVS_NS, ssid, ssidLen, pass, passLen)) return true;
  for (const char* old : OLD_NAMES) {
    if (credsLoadFrom(old, ssid, ssidLen, pass, passLen)) {
      Serial.printf("wifi: migrated saved network '%s' from the '%s' settings\n", ssid, old);
      credsSave(ssid, pass);
      return true;
    }
  }
  return false;
}

// ---------- WiFi state ----------
enum WifiState : uint8_t { WF_CONNECTING, WF_PORTAL, WF_ONLINE };
enum JoinSource : uint8_t { JOIN_NONE, JOIN_FORM, JOIN_SAVED };

// loop()'s own: the handlers see these only through the NetView snapshot.
static WifiState  wf = WF_CONNECTING;
static DNSServer  dns;
static uint32_t   wfDeadline = 0;          // CONNECTING: timeout / retry; PORTAL: join attempt timeout
static uint32_t   nextSavedRetry = 0;      // PORTAL: when to try the saved network again
static JoinSource joining = JOIN_NONE;     // PORTAL: STA join in flight, and who asked for it
static bool       joinFailed = false;      // last form join failed; shown on the portal page
static bool       wasOnline = false;       // distinguishes first-boot timeout from a drop
static bool       mdnsUp = false;          // MDNS.begin() succeeded for this connection
static bool       mdnsHttps = false;       // ... and the https service is advertised
static bool       scanTaken = false;       // the finished scan's results are in net.nets
static char       wfSsid[33] = "";         // credentials currently being tried / in use
static char       wfPass[65] = "";
static volatile uint8_t lastDiscReason = 0;   // from the WiFi event task, see wifiStart()
static volatile uint32_t discCount = 0;       // STA disconnect events, ditto
static uint32_t   scanAt = 0;              // PORTAL: when the list was last refreshed / a scan tried
static uint32_t   joinBeganAt = 0;         // PORTAL: last WiFi.begin() of a join
static uint32_t   joinDiscs = 0;           // PORTAL: discCount then
// PORTAL: the saved network's access points in the last scan, strongest
// first. Portal joins go through them in turn, one per attempt: on a mesh,
// the node heard loudest isn't always one that answers.
struct ApPick { uint8_t bssid[6]; int32_t channel; int32_t rssi; };
static ApPick     aps[4];
static int        apCount = 0;
static char       apSsid[33] = "";
static uint32_t   joinTry = 0;             // PORTAL: attempts so far, picks aps[joinTry % apCount]

// ---------- what the web handlers see ----------
// The handlers run in the web servers' tasks, concurrently with loop(). They
// never touch the state above (or the WiFi scan list, which a scanDelete()
// here would free under them): loop() publishes this copy, and they take one
// under netLock. Their requests come back the other way as flags, with the
// portal's credentials under the same lock.
struct NetView {
  WifiState wf;
  bool      joinFailed;
  String    tried;       // the SSID that failed
  String    nets;        // the portal form's <option>s, from the last finished scan
  String    ssid;        // the network we're on
  String    ip;          // ours: STA address, or the AP's in the portal
  int       rssi;
};
static SemaphoreHandle_t netLock;          // guards net, postSsid, postPass
static NetView net;
static char    postSsid[33], postPass[65]; // /save → loop()
static volatile bool joinPending = false;  // portal posted creds; begin STA join next tick
static volatile bool rescanWanted = false; // the portal page was shown: refresh its network list
static volatile bool regenWanted = false;  // POST /control regencert=1: new certificate, restart HTTPS

// Engine controls from /control run in whichever server task got the
// request, maybe both at once: calibration() then setCalibration() must not
// interleave with another request's, nor two startGainCal()s race.
static SemaphoreHandle_t ctlLock;

static NetView netView() {
  xSemaphoreTake(netLock, portMAX_DELAY);
  NetView v = net;
  xSemaphoreGive(netLock);
  return v;
}

static WifiState netState() {
  xSemaphoreTake(netLock, portMAX_DELAY);
  const WifiState s = net.wf;
  xSemaphoreGive(netLock);
  return s;
}

// ---------- HTTPS state ----------
// tls_cert's current pair (valid until the next regenerate()), and a line for
// the status page on why HTTPS is or isn't running (a literal, or
// web::httpsError(); swapped whole, so a handler reads one or the other).
static tls_cert::Pem pem;
static bool certOk = false;
static bool regenerating = false;          // HTTPS stopping so the certificate can change
static uint32_t httpsRetryAt = 0;          // after a failed start
static const char* volatile httpsNote = "starts when the device is online";

static void htmlEscape(String& s) {
  s.replace("&", "&amp;");
  s.replace("<", "&lt;");
  s.replace(">", "&gt;");
  s.replace("\"", "&quot;");
}

static const char PAGE_HEAD[] =
  "<!DOCTYPE html><html><head><meta charset=utf-8>"
  "<meta name=viewport content='width=device-width,initial-scale=1'>"
  "<title>earwing</title>"
  "<style>body{font-family:sans-serif;max-width:320px;margin:2em auto;padding:0 1em}"
  "input{width:100%;padding:.5em;margin:.3em 0 1em;box-sizing:border-box}"
  "button{width:100%;padding:.6em;font-size:1em}.err{color:#b00}"
  "td{padding:.2em .6em .2em 0}</style></head><body>";

// Portal form. Networks found by the async scan are offered via a <datalist>
// so hidden SSIDs can still be typed in. Showing it asks loop() for a fresh
// scan, so the next view is current.
static void portalPage(web::Req& q, const NetView& v) {
  rescanWanted = true;
  String err;
  if (v.joinFailed) {
    String s = v.tried;
    htmlEscape(s);
    err = "<p class=err>Couldn't join <b>" + s + "</b>. Check the password and try again.</p>";
  }

  String page = PAGE_HEAD;
  page += "<h2>earwing wifi setup</h2>" + err +
          "<form method=POST action=/save>"
          "<label>Network</label><input name=ssid list=nets required maxlength=32>"
          "<datalist id=nets>" + v.nets + "</datalist>"
          "<label>Password</label><input name=pass type=password maxlength=64>"
          "<button>Connect</button></form></body></html>";
  q.send(200, "text/html", page);
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

// The self-test found the streams misaligned, now or at some point since boot.
static bool alignFault(const engine::Status& a) {
  return a.alignChanges || sync_check::misread(a.align.status);
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
    if (a.smoothValid)
      appendf(page, "<tr><td>DOA (tracked)</td><td>%+.1f&deg; (table azimuth %+.1f&deg;, talker assumed %.0f&deg; up)</td></tr>",
              a.smoothDeg, a.smoothAzDeg, audio::DOA_ASSUMED_ELEV_DEG);
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
  appendf(page, "<tr><td>Alignment test</td><td>%s%s, offset %d slots, drop %d/%d; "
          "%lu runs, %lu matched, first %d, %lu changed, %lu rebuilt</span></td></tr>",
          alignFault(a) ? "<span class=err>" : "<span>",
          alignText(a), a.align.slotOffset, a.align.dropMaster, a.align.dropSlave,
          (unsigned long)a.alignRuns, (unsigned long)a.alignConclusive, a.alignFirstOffset,
          (unsigned long)a.alignChanges, (unsigned long)a.alignRebuilds);
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
  const audio_server::Counts sc = audio_server::counts();
  page += "<h3>audio streams</h3><p>"
          "<a href='/beam.wav'>beam.wav</a> (live) &middot; raw mic ";
  for (int m = 1; m <= audio::NUM_MICS; m++)
    page += String("<a href='/beam.wav?mic=") + m + "'>" + m + "</a> ";
  page += String("&middot; <a href='/raw.wav?s=5'>raw.wav (5 s, ") + engine::sourceChannels(a.source) +
          " ch)</a> &middot; <a href='/status.json'>status.json</a></p>";
  appendf(page, "<p>Running: %d of %d beam streams, %d of 1 raw capture. The same endpoints on "
          "<code>http://%s:%d/</code> are the legacy port for players and old links.</p>",
          sc.beam, audio_server::MAX_BEAM_STREAMS, sc.raw, ip.c_str(), audio_server::PORT);
  if (board) page += "<p>With the inbuilt source every stream above is the board mic.</p>";
}

// State, certificate and how to trust it. Only a trusted certificate gets
// the page a secure context without a warning in the way.
static void httpsStatusHtml(String& page) {
  const web::Https st = web::httpsState();
  char fp[100];
  web::certFingerprint(fp, sizeof(fp));
  page += "<h3>https</h3><table>";
  appendf(page, "<tr><td>State</td><td>%s</td></tr>",
          st == web::Https::Running ? "<b>running</b> on port 443"
          : st == web::Https::Stopping ? "stopping" : "<span class=err>off</span>");
  if (st != web::Https::Running) appendf(page, "<tr><td>Why</td><td>%s</td></tr>", (const char*)httpsNote);
  if (fp[0])
    appendf(page, "<tr><td>Certificate</td><td>self-signed ECDSA P-256 for %s.local, made on this device. "
            "SHA-256:<br><code style='word-break:break-all'>%s</code></td></tr>", NAME, fp);
  else
    page += "<tr><td>Certificate</td><td><span class=err>none</span> (see the serial log)</td></tr>";
  page += "</table>";
  appendf(page, "<p><a href='https://%s.local/'>https://%s.local/</a>", NAME, NAME);
  if (fp[0]) page += " &middot; <a href='/earwing.crt'>download earwing.crt</a>";
  // A form, not a link: POST only, so a prefetch or a crawler can't swap the
  // certificate out from under every device that trusts it.
  page += " &middot; <form method=post action=/control style='display:inline'>"
          "<input type=hidden name=regencert value=1>"
          "<button style='width:auto;padding:0;font-size:inherit;border:0;background:none;"
          "color:#00e;text-decoration:underline;cursor:pointer' "
          "onclick=\"return confirm('Make a new certificate? Every device that trusts the "
          "current one will have to trust the new one.')\">make a new certificate</button></form></p>";
  page += "<p>The browser warns about the certificate until you trust it once on each device; "
          "check the fingerprint above against what it shows. Then:</p><ul>"
          "<li><b>macOS</b>: open earwing.crt (it goes into Keychain Access, login keychain), "
          "double-click the <i>earwing.local</i> certificate, Trust &rarr; <i>Always Trust</i>, "
          "then restart the browser.</li>"
          "<li><b>iOS / iPadOS</b>: download it in Safari and install the profile (Settings &rarr; "
          "Profile Downloaded), then Settings &rarr; General &rarr; About &rarr; Certificate Trust "
          "Settings and turn on full trust for it.</li>"
          "<li><b>Android</b>: Settings &rarr; Security &rarr; More &rarr; Encryption &amp; credentials "
          "&rarr; Install a certificate &rarr; CA certificate, and pick earwing.crt.</li>"
          "<li><b>Windows</b>: open earwing.crt &rarr; Install Certificate &rarr; Local Machine &rarr; "
          "<i>Trusted Root Certification Authorities</i>, then restart the browser.</li>"
          "<li><b>Firefox</b> (its own store): accept the warning once (Advanced &rarr; Accept the Risk); "
          "it keeps the exception.</li></ul>"
          "<p>A new certificate means trusting it again everywhere.</p>";
}

static void statusJson(web::Req& q) {
  engine::Status a;
  engine::status(&a);
  const audio::Calibration cal = engine::calibration();
  const board_io::Info b = board_io::info();
  const bool board = a.source == engine::Source::Inbuilt;
  String j;
  j.reserve(3072);   // ~2.5 KB: one allocation, not one per append
  j += '{';
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
    j += "\"doa\":null,\"confidence\":null,\"tracked\":null,\"trackedAz\":null,\"trackedValid\":false,\"steer\":null,";
  else
    appendf(j, "\"doa\":%.2f,\"confidence\":%.3f,\"tracked\":%.2f,\"trackedAz\":%.2f,\"trackedValid\":%s,\"steer\":%.2f,",
            a.doaDeg, a.confidence, a.smoothDeg, a.smoothAzDeg, a.smoothValid ? "true" : "false", a.steerDeg);
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
          "\"mismatches\":%lu,\"resyncs\":%lu,\"hwFaults\":%lu,",
          a.running ? "true" : "false", (unsigned long)a.frames[0], (unsigned long)a.frames[1],
          (unsigned long)a.overflows[0], (unsigned long)a.overflows[1],
          (unsigned long)a.timeouts[0], (unsigned long)a.timeouts[1],
          (unsigned long)a.mismatches, (unsigned long)a.resyncs, (unsigned long)a.hwFaults);
  appendf(j, "\"ringDepth\":%u,\"ringHigh\":%u,\"ringDrops\":%lu,\"streamDrops\":%lu,\"clipped\":%lu,",
          a.ringDepth, a.ringHigh, (unsigned long)a.ringDrops, (unsigned long)a.streamDrops,
          (unsigned long)a.clipped);
  appendf(j, "\"align\":{\"status\":\"%s\",\"offset\":%d,\"bitShift\":%d,\"dropMaster\":%d,\"dropSlave\":%d,"
          "\"runs\":%lu,\"matched\":%lu,\"firstOffset\":%d,\"changes\":%lu,\"rebuilds\":%lu},",
          alignText(a), a.align.slotOffset, a.align.bitShift, a.align.dropMaster, a.align.dropSlave,
          (unsigned long)a.alignRuns, (unsigned long)a.alignConclusive, a.alignFirstOffset,
          (unsigned long)a.alignChanges, (unsigned long)a.alignRebuilds);
  appendf(j, "\"cal\":{\"map\":\"%d%d%d%d\",\"gainDb\":[%.2f,%.2f,%.2f,%.2f],\"advanceUs\":[%.1f,%.1f,%.1f,%.1f],"
          "\"gainCal\":%d},",
          cal.posOfChannel[0] + 1, cal.posOfChannel[1] + 1, cal.posOfChannel[2] + 1, cal.posOfChannel[3] + 1,
          cal.gainDb[0], cal.gainDb[1], cal.gainDb[2], cal.gainDb[3],
          cal.advanceUs[0], cal.advanceUs[1], cal.advanceUs[2], cal.advanceUs[3], a.gainCal);
  const audio_server::Counts sc = audio_server::counts();
  const web::Https hs = web::httpsState();
  appendf(j, "\"uptimeS\":%lu,\"resetReason\":%d,", (unsigned long)(millis() / 1000), (int)esp_reset_reason());
  // Stack headroom (bytes never touched) of the web and stream tasks.
  {
    const UBaseType_t cap = uxTaskGetNumberOfTasks() + 4;
    TaskStatus_t* ts = (TaskStatus_t*)heap_caps_malloc(cap * sizeof(TaskStatus_t), MALLOC_CAP_SPIRAM);
    const UBaseType_t n = ts ? uxTaskGetSystemState(ts, cap, nullptr) : 0;
    j += "\"stackFree\":{";
    bool first = true;
    for (UBaseType_t i = 0; i < n; i++) {
      const char* nm = ts[i].pcTaskName;
      if (strcmp(nm, "httpd") && strncmp(nm, "stream", 6) && strcmp(nm, "audio81") && strcmp(nm, "loopTask")) continue;
      appendf(j, "%s\"%s#%u\":%u", first ? "" : ",", nm, (unsigned)ts[i].xTaskNumber,
              (unsigned)ts[i].usStackHighWaterMark);
      first = false;
    }
    heap_caps_free(ts);
    j += "},";
  }
  const web::TlsStats ts = web::tlsStats();
  appendf(j, "\"tls\":{\"full\":%lu,\"resumed\":%lu,\"silent\":%lu,\"failed\":%lu},",
          (unsigned long)ts.full, (unsigned long)ts.resumed, (unsigned long)ts.silent, (unsigned long)ts.failed);
  appendf(j, "\"streams\":{\"beam\":%d,\"raw\":%d,\"maxBeam\":%d},\"https\":\"%s\",",
          sc.beam, sc.raw, audio_server::MAX_BEAM_STREAMS,
          hs == web::Https::Running ? "running" : hs == web::Https::Stopping ? "stopping" : "off");
  const web::Mem m = web::mem();
  appendf(j, "\"fftMaxErr\":%.3e,\"fftEspUs\":%.1f,\"fftRefUs\":%.1f,\"heapInternal\":%u,"
          "\"heapInternalLargest\":%u,\"heapInternalMin\":%u,\"heapPsram\":%u}",
          a.fftMaxErr, a.fftEspUs, a.fftRefUs, (unsigned)m.internalFree, (unsigned)m.internalLargest,
          (unsigned)m.internalMin, (unsigned)m.psramFree);
  q.send(200, "application/json", j);
}

// /control: GET or POST, any combination of source, mode, angle, map, gain,
// advance, calgain (seconds), resync, resetcal, regencert. Redirects back to
// /status unless ?json. 400 for a bad value; 409 if the source can't switch
// right now. regencert is POST only, and refused from another site's page
// (an Origin that isn't this host), so neither a prefetched link nor a
// cross-site form can replace the certificate. It only raises a flag: loop()
// swaps the certificate and
// restarts HTTPS, which can't happen from inside a handler that may be
// running on that very server.
static void controlHandler(web::Req& q) {
  // ?json is the API: readable from any origin, e.g. the site's dev server.
  if (q.hasArg("json")) q.addHeader("Access-Control-Allow-Origin", "*");
  String err;
  bool busy = false;
  xSemaphoreTake(ctlLock, portMAX_DELAY);
  if (q.hasArg("source")) {
    engine::Source src;
    if (!engine::parseSource(q.arg("source").c_str(), &src)) err += "source: array|inbuilt\n";
    else if (!engine::requestSource(src)) busy = true;   // a raw capture is armed
  }
  if (q.hasArg("mode")) {
    proc::Mode m;
    if (proc::parseMode(q.arg("mode").c_str(), &m)) engine::setMode(m);
    else err += "mode: raw|doa|fixed|auto\n";
  }
  if (q.hasArg("angle") && q.arg("angle").length()) {
    const float a = q.arg("angle").toFloat();
    if (isfinite(a) && fabsf(a) <= 180.0f) engine::setFixedAngle(a);
    else err += "angle: degrees, -180..180\n";
  }
  if (q.hasArg("map") || q.hasArg("gain") || q.hasArg("advance")) {
    audio::Calibration c = engine::calibration();
    bool ok = true;
    if (q.hasArg("map") && !parseMap(q.arg("map"), c.posOfChannel)) { ok = false; err += "map: a permutation of 1234\n"; }
    if (q.hasArg("gain") && !parseFloats(q.arg("gain"), c.gainDb)) { ok = false; err += "gain: four comma-separated dB values\n"; }
    if (q.hasArg("advance") && !parseFloats(q.arg("advance"), c.advanceUs)) { ok = false; err += "advance: four comma-separated us values\n"; }
    for (int i = 0; ok && i < audio::NUM_MICS; i++)
      if (fabsf(c.gainDb[i]) > audio::GAIN_TRIM_MAX_DB || fabsf(c.advanceUs[i]) > audio::ADVANCE_TRIM_MAX_US) {
        ok = false;
        appendf(err, "gain within +-%.0f dB, advance within +-%.0f us\n", audio::GAIN_TRIM_MAX_DB,
                audio::ADVANCE_TRIM_MAX_US);
      }
    if (ok) engine::setCalibration(c);
  }
  if (q.hasArg("resetcal")) engine::setCalibration(audio::DEFAULT_CALIBRATION);
  if (q.hasArg("calgain") && !engine::startGainCal(q.arg("calgain").toFloat()))
    appendf(err, "calgain: %g-%g seconds, not already running, array source only\n", engine::GAIN_CAL_MIN_S,
            engine::GAIN_CAL_MAX_S);
  if (q.hasArg("resync")) engine::requestResync();
  xSemaphoreGive(ctlLock);
  if (q.hasArg("regencert")) {
    // Browsers send Origin on every POST; curl sends none, which is fine.
    const String origin = q.header("Origin");
    const int at = origin.indexOf("://");
    const String oHost = web::stripPort(at < 0 ? origin : origin.substring(at + 3));
    if (!q.isPost()) err += "regencert: POST only\n";
    else if (origin.length() && oHost != q.host()) err += "regencert: not from another site's page\n";
    else regenWanted = true;
  }

  if (err.length()) { q.send(400, "text/plain", err); return; }
  if (busy) { q.send(409, "text/plain", "source: a /raw.wav capture is running; try again when it's done\n"); return; }
  if (q.hasArg("json")) { statusJson(q); return; }
  q.redirect(303, "/status");
}

static void statusJsonHandler(web::Req& q) {
  q.addHeader("Access-Control-Allow-Origin", "*");   // for the site's dev server
  statusJson(q);
}

// Served in any WiFi state: in the portal the array still streams, on the AP
// address.
static void statusPage(web::Req& q) {
  const NetView v = netView();
  String ssid = v.wf == WF_PORTAL ? String("none (setup portal)") : v.ssid;
  htmlEscape(ssid);
  uint32_t up = millis() / 1000;
  char upBuf[24];
  snprintf(upBuf, sizeof(upBuf), "%lud %02lu:%02lu:%02lu",
           (unsigned long)(up / 86400), (unsigned long)(up / 3600 % 24),
           (unsigned long)(up / 60 % 60), (unsigned long)(up % 60));

  const web::Mem m = web::mem();
  String page;
  page.reserve(10240);   // ~9 KB: one allocation, not one per append
  page += PAGE_HEAD;
  page += "<style>body{max-width:640px}</style><h2>earwing</h2>";
  if (v.wf != WF_PORTAL) page += "<p><a href='/'>&larr; transcriber</a></p>";
  page += "<table>"
          "<tr><td>Network</td><td>" + ssid + "</td></tr>"
          "<tr><td>IP</td><td>" + v.ip + "</td></tr>"
          "<tr><td>Signal</td><td>" + String(v.rssi) + " dBm</td></tr>"
          "<tr><td>Uptime</td><td>" + upBuf + "</td></tr>";
  appendf(page, "<tr><td>Free RAM</td><td>%u KB internal (largest block %u KB, low %u KB), %u KB PSRAM</td></tr>"
          "</table>", (unsigned)(m.internalFree / 1024), (unsigned)(m.internalLargest / 1024),
          (unsigned)(m.internalMin / 1024), (unsigned)(m.psramFree / 1024));
  httpsStatusHtml(page);
  audioStatusHtml(page, v.ip);
  page += "</body></html>";
  q.send(200, "text/html", page);
}

// "/" without the web app on LittleFS: say how to put it there.
static void siteMissingPage(web::Req& q) {
  web::siteHeaders(q, "/");
  String page = PAGE_HEAD;
  page += "<h2>earwing</h2><p>The transcriber web app isn't installed";
  page += web::siteMounted() ? "." : " (no LittleFS filesystem on the device yet).";
  page += "</p><p>From the repo: <code>cd site &amp;&amp; npm run deploy</code>, or "
          "<code>npm run build:device</code> in <code>site/</code> and then "
          "<code>pio run -t uploadfs</code> in <code>code/</code>.</p>"
          "<p><a href='/status'>Status and audio controls</a></p></body></html>";
  q.send(200, "text/html", page);
}

// ---------- routes ----------
// One table for both servers, branching on the WiFi state so the same
// routes back the portal and earwing.local.

// Unknown paths, and known ones with the wrong method. In the portal this is
// the universal captive trigger: OS probe URLs (generate_204,
// hotspot-detect.html, connecttest.txt, ...) all land here.
static void notFound(web::Req& q) {
  if (netState() == WF_PORTAL) q.redirect(302, "http://192.168.4.1/");
  else q.send(404, "text/plain", "not found");
}

// "/": the default path lands on the secure origin when there is one; the
// app's other files stay reachable over plain HTTP too.
static void rootHandler(web::Req& q) {
  const NetView v = netView();
  if (v.wf == WF_PORTAL) portalPage(q, v);
  else if (!q.secure() && web::httpsState() == web::Https::Running) q.redirect(302, "https://" + q.host() + "/");
  else if (!web::siteServe(q, q.path())) siteMissingPage(q);
}

static void saveHandler(web::Req& q) {
  if (netState() != WF_PORTAL) { q.send(409, "text/plain", "not in setup mode"); return; }
  String ssid = q.arg("ssid"), pass = q.arg("pass");
  if (!ssid.length() || ssid.length() > 32 || pass.length() > 64) {
    q.send(400, "text/plain", "ssid required (max 32), password max 64");
    return;
  }
  xSemaphoreTake(netLock, portMAX_DELAY);
  snprintf(postSsid, sizeof(postSsid), "%s", ssid.c_str());
  snprintf(postPass, sizeof(postPass), "%s", pass.c_str());
  xSemaphoreGive(netLock);
  joinPending = true;   // WiFi.begin() runs in wifiLoop(), never in a handler
  String page = PAGE_HEAD;
  page += "<p>Connecting to your network &mdash; the <b>earwing</b> hotspot will "
          "disappear once joined. Then open <a href='http://earwing.local/'>"
          "earwing.local</a> from your network.</p>"
          "<p>If the hotspot is still here after ~30s, reopen "
          "<a href='/'>the setup page</a>.</p></body></html>";
  q.send(200, "text/html", page);
}

// Everything else GET: the web app's files, or the captive redirect.
static void siteHandler(web::Req& q) {
  if (netState() == WF_PORTAL || !web::siteServe(q, q.path())) notFound(q);
}

// Audio controls work in portal mode too: the array doesn't need a network.
// The audio routes (audio_server) come before these; the wildcard is last.
static const web::Route ROUTES[] = {
  {"/",              HTTP_GET,  rootHandler},
  {"/index.html",    HTTP_GET,  rootHandler},
  {"/save",          HTTP_POST, saveHandler},
  {"/status",        HTTP_GET,  statusPage},
  {"/status.json",   HTTP_GET,  statusJsonHandler},
  {"/control",       HTTP_GET,  controlHandler},
  {"/control",       HTTP_POST, controlHandler},
  {"/earwing.crt", HTTP_GET,  web::certHandler},
  {"/meeting",       HTTP_POST, meeting::postHandler},
  {"/meeting.json",  HTTP_GET,  meeting::jsonHandler},
#ifdef UI_DEBUG
  {"/debug/screen.bmp", HTTP_GET, ui_meeting::debugScreenHandler},
  {"/debug/tap",     HTTP_GET,  ui_meeting::debugTapHandler},
#endif
  {"/*",             HTTP_GET,  siteHandler},
};

// ---------- WiFi ----------
// The portal's network list, from loop() only: WiFi.SSID(i) reads the scan
// list that scanDelete() frees.
static void scanStart() {
  WiFi.scanNetworks(true);   // async; WIFI_SCAN_FAILED from scanComplete() if it wouldn't start
  scanTaken = false;
  scanAt = millis();
}

// One line per scan, and one per access point of the saved network: the
// evidence for a join that fails (several mesh nodes? weak? WPA3/PMF?).
static void scanLog(int n) {
  static const char* const AUTH[] = {"open", "WEP", "WPA", "WPA2", "WPA/WPA2", "WPA2-ENT", "WPA3",
                                     "WPA2/WPA3", "WAPI", "OWE", "WPA3-ENT-192", "WPA3-EXT", "WPA3-EXT-mixed"};
  char saved[33], pw[65];
  credsLoad(saved, sizeof(saved), pw, sizeof(pw));
  Serial.printf("wifi: portal scan: %d networks\n", n);
  apCount = 0;
  snprintf(apSsid, sizeof(apSsid), "%s", saved);
  for (int i = 0; i < n; i++) {   // scan results come sorted by RSSI
    if (!saved[0] || WiFi.SSID(i) != saved) continue;
    if (apCount < (int)(sizeof(aps) / sizeof(aps[0]))) {
      memcpy(aps[apCount].bssid, WiFi.BSSID(i), 6);
      aps[apCount].channel = WiFi.channel(i);
      aps[apCount].rssi = WiFi.RSSI(i);
      apCount++;
    }
    const int a = WiFi.encryptionType(i);
    Serial.printf("wifi:   '%s' %s ch %ld rssi %ld %s\n", saved, WiFi.BSSIDstr(i).c_str(), (long)WiFi.channel(i),
                  (long)WiFi.RSSI(i), a >= 0 && a < (int)(sizeof(AUTH) / sizeof(AUTH[0])) ? AUTH[a] : "?");
  }
}

// A portal join. Auto-reconnect is off in the portal (it would keep the
// STA "connecting" and every scan would fail), so a join the AP didn't
// answer is re-issued from wifiLoop() until the deadline.
static void joinBegin(uint32_t now) {
  WiFi.scanDelete();
  // AP stays up (AP_STA) while we try.
  if (apCount && !strcmp(wfSsid, apSsid)) {
    const ApPick& a = aps[joinTry++ % apCount];
    Serial.printf("wifi: joining '%s' via %02X:%02X:%02X:%02X:%02X:%02X ch %ld (%ld dBm)\n", wfSsid, a.bssid[0],
                  a.bssid[1], a.bssid[2], a.bssid[3], a.bssid[4], a.bssid[5], (long)a.channel, (long)a.rssi);
    WiFi.begin(wfSsid, wfPass, a.channel, a.bssid);
  } else {
    WiFi.begin(wfSsid, wfPass);
  }
  joinBeganAt = now;
  joinDiscs = discCount;
}

static String scanOptions(int n) {
  String nets;
  for (int i = 0; i < n; i++) {
    String s = WiFi.SSID(i);
    if (!s.length()) continue;
    htmlEscape(s);
    nets += "<option value=\"" + s + "\">";
  }
  return nets;
}

// Refresh the handlers' copy: at once when the state changes, and every
// second for the signal. `nets` is passed only when a scan has finished.
static void netPublish(uint32_t now, const String* nets = nullptr) {
  static uint32_t next = 0;
  static WifiState lastWf = WF_CONNECTING;
  static bool lastFailed = false;
  if (!nets && (int32_t)(now - next) < 0 && wf == lastWf && joinFailed == lastFailed) return;
  next = now + 1000;
  lastWf = wf;
  lastFailed = joinFailed;
  const bool portal = wf == WF_PORTAL;
  // Built outside the lock: WiFi calls take their own locks and may be slow.
  const String ssid = portal ? String() : WiFi.SSID();
  const String ip = (portal ? WiFi.softAPIP() : WiFi.localIP()).toString();
  const int rssi = WiFi.RSSI();
  xSemaphoreTake(netLock, portMAX_DELAY);
  net.wf = wf;
  net.joinFailed = joinFailed;
  net.tried = wfSsid;
  if (nets) net.nets = *nets;
  net.ssid = ssid;
  net.ip = ip;
  net.rssi = rssi;
  xSemaphoreGive(netLock);
}

static void portalStart() {
  // Our own saved-network retries (every SAVED_RETRY_PERIOD_MS) replace
  // Arduino's auto-reconnect here: while it keeps the STA "connecting",
  // esp_wifi refuses to scan ("sta is connecting, cannot set config") and
  // the form's list stays empty.
  WiFi.setAutoReconnect(false);
  WiFi.disconnect(false);
  WiFi.mode(WIFI_AP_STA);          // STA half stays usable for join attempts
  radioSetup();
  WiFi.softAP(NAME);               // open AP at 192.168.4.1
  dns.start(53, "*", WiFi.softAPIP());
  scanStart();                     // results feed the form's datalist
  web::stopHttps();                // the portal is HTTP only; no-op if it isn't running
  wf = WF_PORTAL;
  ui_meeting::showDevice();        // the join instructions are on Device
  joining = JOIN_NONE;
  nextSavedRetry = millis() + SAVED_RETRY_PERIOD_MS;
  Serial.printf("wifi: portal up — join '%s' AP to configure\n", NAME);
}

static void portalStop() {
  dns.stop();
  WiFi.softAPdisconnect(true);
  WiFi.mode(WIFI_STA);
  radioSetup();
}

static void mdnsStop() {
  MDNS.end();
  mdnsUp = mdnsHttps = false;
}

static void goOnline() {
  WiFi.setAutoReconnect(true);     // off in the portal; a link drop reconnects by itself
  // Re-run mDNS on every (re)connect so the responder binds the fresh netif.
  // httpsLoop() adds the https service once that server is up.
  mdnsStop();
  mdnsUp = MDNS.begin(NAME);
  if (mdnsUp) MDNS.addService("http", "tcp", web::HTTP_PORT);
  wf = WF_ONLINE;
  wasOnline = true;
  Serial.printf("wifi: online on '%s', http://%s.local/ (%s)\n",
                WiFi.SSID().c_str(), NAME, WiFi.localIP().toString().c_str());
}

static void wifiStart() {
  // The last STA disconnect reason (wifi_err_reason_t) for the join
  // failure lines: 2 AUTH_EXPIRE (the AP never answered), 15 4WAY_HANDSHAKE
  // (wrong password), 201 NO_AP_FOUND. Kept here, printed from loop().
  WiFi.onEvent([](arduino_event_id_t, arduino_event_info_t info) {
    lastDiscReason = info.wifi_sta_disconnected.reason;
    discCount = discCount + 1;
  }, ARDUINO_EVENT_WIFI_STA_DISCONNECTED);
  WiFi.persistent(false);          // creds live in our NVS namespace, not the stack's
  // Join the strongest access point of the network, not the first one heard:
  // the default fast scan stops at the first match, which on a mesh or with
  // extenders can be a far node that never answers (AUTH_EXPIRE).
  WiFi.setScanMethod(WIFI_ALL_CHANNEL_SCAN);
  WiFi.setSortMethod(WIFI_CONNECT_AP_BY_SIGNAL);
  WiFi.setHostname(NAME);          // must precede mode()/begin() to reach DHCP
  // A link-local IPv6 address as well, so mDNS can answer the AAAA query for
  // earwing.local. Without one it answers only A, and a client that asks
  // for both (macOS curl, getaddrinfo) waits ~5 s for the missing AAAA before
  // using the IPv4 address. The servers listen dual-stack.
  WiFi.enableIPv6(true);
  if (credsLoad(wfSsid, sizeof(wfSsid), wfPass, sizeof(wfPass))) {
    WiFi.mode(WIFI_STA);
    radioSetup();
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
  mdnsStop();
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
          // than reopening the portal for a transient AP outage. A plain
          // begin(), not reconnect(): a portal join pinned one AP's BSSID and
          // channel, and any AP of the network will do now.
          WiFi.begin(wfSsid, wfPass);
          wfDeadline = now + JOIN_TIMEOUT_MS;
        } else {
          Serial.printf("wifi: join '%s' timed out (last disconnect reason %u)\n", wfSsid, lastDiscReason);
          WiFi.disconnect(false);
          portalStart();
        }
      }
      break;

    case WF_PORTAL: {
      dns.processNextRequest();

      // A finished scan becomes the form's list; a page view since asks for
      // a new one. Not over a join in flight: scanning hops channels and
      // would stall the association.
      const int n = WiFi.scanComplete();
      if (n >= 0 && !scanTaken) {
        scanLog(n);
        const String nets = scanOptions(n);
        netPublish(now, &nets);
        scanTaken = true;
        scanAt = now;
      }
      // A new scan whenever none is running and no join is in flight: when
      // the last one failed to start, a page view asked, or the list is old.
      if (joining == JOIN_NONE && n != WIFI_SCAN_RUNNING &&
          (n == WIFI_SCAN_FAILED ? now - scanAt > SCAN_RETRY_MS : rescanWanted || now - scanAt > SCAN_PERIOD_MS)) {
        rescanWanted = false;
        WiFi.scanDelete();
        scanStart();
      }

      if (joinPending) {
        xSemaphoreTake(netLock, portMAX_DELAY);
        memcpy(wfSsid, postSsid, sizeof(wfSsid));
        memcpy(wfPass, postPass, sizeof(wfPass));
        joinPending = false;
        xSemaphoreGive(netLock);
        joinFailed = false;
        joining = JOIN_FORM;
        joinBegin(now);
        wfDeadline = now + FORM_JOIN_TIMEOUT_MS;
        Serial.printf("wifi: portal join '%s'\n", wfSsid);
      } else if (joining == JOIN_NONE && (int32_t)(now - nextSavedRetry) > 0) {
        // The router may simply have been slower to boot than us (power cut).
        // Retry the saved network now and then, but only while nobody is
        // using the portal: joining moves the AP to the router's channel and
        // would knock a phone mid-setup off the hotspot.
        nextSavedRetry = now + SAVED_RETRY_PERIOD_MS;
        // Into locals first: wfSsid still names a failed form join for the page.
        char ssid[sizeof(wfSsid)], pass[sizeof(wfPass)];
        if (WiFi.softAPgetStationNum() == 0 && credsLoad(ssid, sizeof(ssid), pass, sizeof(pass))) {
          memcpy(wfSsid, ssid, sizeof(wfSsid));
          memcpy(wfPass, pass, sizeof(wfPass));
          joining = JOIN_SAVED;
          joinBegin(now);
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
        } else if (discCount != joinDiscs && now - joinBeganAt > JOIN_RETRY_MS &&
                   (int32_t)(now - wfDeadline) < -(int32_t)JOIN_RETRY_MS) {
          Serial.printf("wifi: join '%s' not answered (reason %u), retrying\n", wfSsid, lastDiscReason);
          joinBegin(now);
        } else if ((int32_t)(now - wfDeadline) > 0) {
          // Stop the STA retrying (a bad password makes it hop channels,
          // which disrupts the AP) and stay in the portal.
          Serial.printf("wifi: join '%s' failed (last disconnect reason %u)\n", wfSsid, lastDiscReason);
          if (joining == JOIN_FORM) joinFailed = true;
          joining = JOIN_NONE;
          WiFi.disconnect(false);
          scanStart();
        }
      }
      break;
    }

    case WF_ONLINE:
      if (WiFi.status() != WL_CONNECTED) {
        Serial.println("wifi: link lost, reconnecting");
        mdnsStop();
        wf = WF_CONNECTING;
        wfDeadline = now + JOIN_TIMEOUT_MS;
        WiFi.begin(wfSsid, wfPass);   // unpins a portal join's BSSID (see WF_CONNECTING)
      }
      break;
  }
  netPublish(now);
}

// ---------- HTTPS ----------
// Level-triggered: wanted whenever there's a certificate and the device is
// on the user's network, or was and is getting back onto it (the listener
// outlives the address, and a phone's tab reconnects by itself). Stopped for
// the portal. A certificate change stops it, waits for the stop to finish
// (web::poll(): the audio streams on it end first), regenerates — ~1 s of
// ECC and an NVS write, here in loop() like the first load in setup() — and
// lets it start again.
static void httpsLoop(uint32_t now) {
  web::poll();
  const web::Https st = web::httpsState();
  const bool wanted = certOk && !regenerating && (wf == WF_ONLINE || (wf == WF_CONNECTING && wasOnline));

  if (regenWanted && !regenerating) {
    regenWanted = false;
    regenerating = true;
    httpsNote = "restarting with a new certificate";
    web::stopHttps();
  } else if (regenerating && st == web::Https::Off) {
    regenerating = false;
    certOk = tls_cert::regenerate(&pem);
    if (!certOk) pem = tls_cert::Pem{};   // the old buffers are gone either way
    web::setCert(pem, certOk ? tls_cert::fingerprint() : "");
    httpsNote = certOk ? "starts when the device is online" : "no certificate: regenerating failed (see serial)";
    httpsRetryAt = now;
  } else if (wanted && st == web::Https::Off && (int32_t)(now - httpsRetryAt) >= 0) {
    if (!web::startHttps(pem)) {
      httpsNote = web::httpsError();
      httpsRetryAt = now + 30000;
    }
  } else if (!wanted && !regenerating && st == web::Https::Running) {
    web::stopHttps();
  }
  if (!wanted && !regenerating && st == web::Https::Off && certOk)
    httpsNote = wf == WF_PORTAL ? "off in the setup portal: HTTPS runs on your network only"
                                : "starts when the device is online";

  if (mdnsUp && !mdnsHttps && web::httpsState() == web::Https::Running)
    mdnsHttps = MDNS.addService("https", "tcp", web::HTTPS_PORT);
}

// ---------- display ----------
// Direct drawing, each element repainted only when what it shows changes. A
// full-screen sprite (150KB at 320x240) would fit in PSRAM, but a needle and
// four bars at 10 Hz don't need one.
//
// The tab strip along the top and the Meeting tab are ui_meeting.*; these
// are the Device and Settings tabs, below the strip.
// Device (display only, no taps):
//   y  32- 58  the web app's address (font 4), or what WiFi is doing
//   y  60- 76  note line: WiFi detail (connecting, the setup portal), else
//              why capture isn't running normally; blank when all is well
//   y  81-237  DOA: two half dials, front (up) and back (down), with M1/M4
//              between them; per-channel level bars on the right
// Settings:
//   y  44- 91  WiFi settings button (asks, then opens the setup portal;
//              with the portal open, shows its steps), the network below it
//   y 120-212  "Microphone": the Array | Board switch, a line on the source
// A popup (the setup question, or the portal's steps) replaces the Settings
// tab while it's up.
// Neither tab's content stays on the screen while another is up. Coming
// back, viewLoop() clears below the strip and bumps screenEpoch, and each
// function's repaint cache forgets what it had drawn when that changes.
static TFT_eSPI tft;

static void clearBelowStrip() {
  tft.fillRect(0, ui_meeting::STRIP_H, board_io::SCREEN_W, board_io::SCREEN_H - ui_meeting::STRIP_H, TFT_BLACK);
}

static const int ADDR_X = 12, ADDR_Y = 32, ADDR_W = board_io::SCREEN_W - 2 * ADDR_X;
static const int NOTE_Y = 60, NOTE_H = 17;
static const uint32_t AUDIO_DRAW_MS = 100;

// Dials: broadside (θ = 0) up, + toward Mic 4 to the right. The upper half
// dial is the front (|θ| ≤ 90°), the lower one the back, apart, with M1/M4
// on the array axis between them, so the pair reads as the array seen from
// above rather than as a clock face. A linear array can't tell front from
// back, so it gets every needle and marker in both halves (θ, and 180° − θ
// below); a planar layout shows the one direction it resolves, in whichever
// half it falls. As big as fits in y 81-237 with a steering marker (r + 10)
// at either pole.
static const int DIAL_CX = 100, DIAL_R = 58;
static const int DIAL_UP_CY = 149, DIAL_DN_CY = 169;   // the halves' hubs, on their flat edges
static const uint16_t DIAL_EDGE = 0x4208;              // the flat edges, dimmer than the arcs
static bool dialLinear;

// Level bars, one per capture channel: ~100 ms RMS, with a white tick at the
// decaying raw peak (the tap test reads that tick). With the board mic only
// the first bar is live; the others are blanked, not shown at zero.
static const int BAR_X0 = 219, BAR_DX = 22, BAR_W = 14, BAR_TOP = 84, BAR_BOT = 216;
static const int BAR_LABEL_Y = BAR_BOT + 3, BAR_LABEL_H = 17;
static const float BAR_DB_MIN = -90.0f, BAR_DB_MAX = -10.0f;
static const uint16_t BAR_TRACK = 0x2104;   // dark grey

// Colours that mean the source, on the switch and the "DOA n/a" panel.
static const uint16_t COLOR_ARRAY = TFT_CYAN, COLOR_BOARD = TFT_ORANGE;

// Settings controls: clear of the top and bottom edges, where the touch
// calibration is weakest (BOARD-CALIBRATION.md). A tap within SET_SLOP of
// one counts: the calibrated map is good to ~2-3 px here, a fingertip much
// less so. The WiFi button's slop stops at y 36, the tab strip's tap band.
static const int SET_X = 14, SET_W = board_io::SCREEN_W - 2 * SET_X, SET_SLOP = 8, SET_R = 6;
static const int WIFI_BTN_Y = 44, WIFI_BTN_H = 48, WIFI_NOTE_Y = 94;
static const int MIC_LABEL_Y = 120, MIC_SW_Y = 140, MIC_SW_H = 48, MIC_GAP = 6, MIC_NOTE_Y = 196;
static const int MIC_BTN_W = (SET_W - MIC_GAP) / 2;

// "Open WiFi setup?", over the middle of the tab: Cancel | Setup. The
// portal's steps use the same box, with OK where Setup is.
static const int ASK_X = 14, ASK_Y = 50, ASK_W = 281, ASK_H = 150, ASK_R = 8;
static const int ASK_BTN_Y = ASK_Y + 96, ASK_BTN_W = 110, ASK_BTN_H = 42, ASK_BTN_SLOP = 8;
static const int ASK_CANCEL_X = ASK_X + 22, ASK_OK_X = ASK_X + ASK_W - 22 - ASK_BTN_W;
enum class Popup : uint8_t { None, AskSetup, SetupSteps };
static Popup popup = Popup::None;   // while one is up, the Settings tab stays undrawn

// What main.cpp has below the strip; None while the Meeting tab or a popup
// has it.
enum class View : uint8_t { None, Device, Settings };
static View view = View::None;
static uint32_t screenEpoch = 1;
static bool audioUp = true;     // engine::begin() succeeded

// A draw function's repaint throttle: due() every AUDIO_DRAW_MS, and at once
// after viewLoop() cleared the screen, which it reports in `fresh`.
struct DrawTimer {
  uint32_t next = 0, epoch = 0;
  bool fresh = false;
  bool due(uint32_t now) {
    fresh = epoch != screenEpoch;
    if (!fresh && (int32_t)(now - next) < 0) return false;
    epoch = screenEpoch;
    next = now + AUDIO_DRAW_MS;
    return true;
  }
};

// Trim a line to fit `maxW` px in `font`, with ".." when shortened.
static String fitText(String s, int font, int maxW) {
  if (tft.textWidth(s, font) <= maxW) return s;
  while (s.length() && tft.textWidth(s + "..", font) > maxW) s.remove(s.length() - 1);
  return s + "..";
}

// A one-line text slot: cleared across the screen, then drawn in font 2.
static void drawLine(const String& s, int x, int y, uint16_t color) {
  tft.fillRect(0, y, board_io::SCREEN_W, NOTE_H, TFT_BLACK);
  tft.setTextDatum(TL_DATUM);
  tft.setTextColor(color, TFT_BLACK);
  tft.drawString(fitText(s, 2, board_io::SCREEN_W - x - 12), x, y, 2);
}

// What WiFi is doing: the address line and its colour, and a note for under
// it (empty once online).
static void wifiText(String* addr, String* note, uint16_t* color) {
  *addr = *note = "";
  *color = TFT_WHITE;
  switch (wf) {
    case WF_CONNECTING:
      *addr = "connecting";
      *note = wfSsid;
      *color = TFT_YELLOW;
      break;
    case WF_PORTAL:
      if (joining != JOIN_NONE) {
        *addr = "joining";
        *note = wfSsid;
        *color = TFT_YELLOW;
      } else {
        *addr = "wifi setup";
        *note = String("join AP: ") + NAME + (joinFailed ? " - join failed, retry" : " - then open 192.168.4.1");
        *color = joinFailed ? TFT_RED : TFT_CYAN;
      }
      break;
    case WF_ONLINE:
      // The name to type, or the address when mDNS didn't come up.
      *addr = mdnsUp ? String(NAME) + ".local" : WiFi.localIP().toString();
      *color = TFT_GREEN;
      break;
  }
}

static void drawAddress(uint32_t now) {
  if (view != View::Device) return;
  static DrawTimer timer;
  if (!timer.due(now)) return;
  static String last;
  if (timer.fresh) last = "";
  String addr, note;
  uint16_t color;
  wifiText(&addr, &note, &color);
  const String key = addr + '|' + color;
  if (key == last) return;
  last = key;
  tft.fillRect(0, ADDR_Y, board_io::SCREEN_W, NOTE_Y - ADDR_Y, TFT_BLACK);
  tft.setTextDatum(TL_DATUM);
  tft.setTextColor(color, TFT_BLACK);
  tft.drawString(fitText(addr, 4, ADDR_W), ADDR_X, ADDR_Y + 1, 4);
}

// ---- Settings ----
static void drawWifiButton(bool fresh) {
  String note;
  if (wf == WF_ONLINE) note = String("on ") + wfSsid;
  else if (wf == WF_CONNECTING) note = String("connecting to ") + wfSsid;
  else note = "setup portal open: tap for the steps";
  static String last;
  if (fresh) last = "";
  if (note == last) return;
  last = note;
  const uint16_t c = TFT_CYAN;
  tft.fillRect(SET_X, WIFI_BTN_Y, SET_W, WIFI_BTN_H, TFT_BLACK);
  tft.drawRoundRect(SET_X, WIFI_BTN_Y, SET_W, WIFI_BTN_H, SET_R, c);
  tft.drawRoundRect(SET_X + 1, WIFI_BTN_Y + 1, SET_W - 2, WIFI_BTN_H - 2, SET_R - 1, c);
  tft.setTextDatum(TC_DATUM);
  tft.setTextColor(c, TFT_BLACK);
  tft.drawString("WiFi settings", SET_X + SET_W / 2, WIFI_BTN_Y + WIFI_BTN_H / 2 - 12, 4);
  tft.setTextDatum(TL_DATUM);
  drawLine(note, SET_X + 2, WIFI_NOTE_Y, TFT_DARKGREY);
}

static bool inWifiButton(int x, int y) {
  return x >= SET_X - SET_SLOP && x < SET_X + SET_W + SET_SLOP &&
         y >= WIFI_BTN_Y - SET_SLOP && y < WIFI_BTN_Y + WIFI_BTN_H + SET_SLOP;
}

// One side of the microphone switch: filled in `on` when it's the requested
// source, outlined in grey otherwise.
static void drawMicSide(int i, const char* label, bool sel, uint16_t on) {
  const int x = SET_X + i * (MIC_BTN_W + MIC_GAP);
  tft.fillRoundRect(x, MIC_SW_Y, MIC_BTN_W, MIC_SW_H, SET_R, sel ? on : TFT_BLACK);
  if (!sel) tft.drawRoundRect(x, MIC_SW_Y, MIC_BTN_W, MIC_SW_H, SET_R, TFT_DARKGREY);
  tft.setTextDatum(TC_DATUM);
  tft.setTextColor(sel ? TFT_BLACK : TFT_LIGHTGREY, sel ? on : TFT_BLACK);
  tft.drawString(label, x + MIC_BTN_W / 2, MIC_SW_Y + MIC_SW_H / 2 - 12, 4);
  tft.setTextDatum(TL_DATUM);
}

// The switch shows the requested source, so a tap answers at once; that
// side is yellow until the source is actually running, red if the codec
// failed.
static void drawMicSwitch(const engine::Status& a, bool fresh) {
  const bool wantBoard = a.sourceTarget == engine::Source::Inbuilt;
  const bool settling = a.source != a.sourceTarget || !a.running;
  const bool dead = codecDead(a) && a.source == a.sourceTarget;
  const uint16_t on = dead ? TFT_RED : settling ? TFT_YELLOW : wantBoard ? COLOR_BOARD : COLOR_ARRAY;
  static String last;
  if (fresh) last = "";
  const String key = String(wantBoard) + '|' + on;
  if (key == last) return;
  last = key;
  drawMicSide(0, "ARRAY", !wantBoard, on);
  drawMicSide(1, "BOARD", wantBoard, on);
  String note;
  uint16_t c = TFT_DARKGREY;
  if (dead) { note = "codec init failed (see /status)"; c = TFT_RED; }
  else if (settling) { note = "switching..."; c = TFT_YELLOW; }
  else if (wantBoard) note = "board mic: one channel, no direction";
  else note = String(audio::NUM_MICS) + "-mic array: direction and beam";
  drawLine(note, SET_X + 2, MIC_NOTE_Y, c);
}

// The switch side a tap is on: 0 Array, 1 Board, -1 neither.
static int micSideAt(int x, int y) {
  if (y < MIC_SW_Y - SET_SLOP || y >= MIC_SW_Y + MIC_SW_H + SET_SLOP) return -1;
  if (x < SET_X - SET_SLOP || x >= SET_X + SET_W + SET_SLOP) return -1;
  return x < SET_X + SET_W / 2 ? 0 : 1;
}

static void drawSettings(uint32_t now) {
  if (view != View::Settings) return;
  static DrawTimer timer;
  if (!timer.due(now)) return;
  const bool fresh = timer.fresh;
  if (fresh) drawLine("Microphone", SET_X + 2, MIC_LABEL_Y, TFT_LIGHTGREY);
  engine::Status a;
  engine::status(&a);
  drawWifiButton(fresh);
  drawMicSwitch(a, fresh);
}

static void drawAskButton(int x, const char* label, uint16_t color) {
  tft.drawRoundRect(x, ASK_BTN_Y, ASK_BTN_W, ASK_BTN_H, 6, color);
  tft.drawRoundRect(x + 1, ASK_BTN_Y + 1, ASK_BTN_W - 2, ASK_BTN_H - 2, 5, color);
  tft.setTextColor(color, TFT_BLACK);
  tft.drawString(label, x + ASK_BTN_W / 2, ASK_BTN_Y + ASK_BTN_H / 2 - 12, 4);
}

// The box and title of a popup. The whole tab goes, rather than a box over
// half-hidden controls. Leaves TC_DATUM set for the caller's lines.
static void drawPopupFrame(const char* title) {
  clearBelowStrip();
  tft.drawRoundRect(ASK_X, ASK_Y, ASK_W, ASK_H, ASK_R, TFT_LIGHTGREY);
  tft.setTextDatum(TC_DATUM);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.drawString(title, ASK_X + ASK_W / 2, ASK_Y + 12, 4);
  tft.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
}

static void showAskSetup() {
  popup = Popup::AskSetup;
  drawPopupFrame("Open WiFi setup?");
  tft.drawString("Leaves this network and starts", ASK_X + ASK_W / 2, ASK_Y + 46, 2);
  tft.drawString(String("the setup hotspot \"") + NAME + "\".", ASK_X + ASK_W / 2, ASK_Y + 63, 2);
  drawAskButton(ASK_CANCEL_X, "Cancel", TFT_LIGHTGREY);
  drawAskButton(ASK_OK_X, "Setup", TFT_CYAN);
  tft.setTextDatum(TL_DATUM);
}

// How to finish setup on a phone or laptop, while the portal is open.
static void showSetupSteps() {
  popup = Popup::SetupSteps;
  drawPopupFrame("WiFi setup");
  tft.setTextDatum(TL_DATUM);
  const int x = ASK_X + 16;
  tft.drawString(String("1. Join the WiFi \"") + NAME + "\"", x, ASK_Y + 44, 2);
  tft.drawString("2. Open 192.168.4.1 in a browser", x, ASK_Y + 61, 2);
  tft.drawString("3. Pick your network and password", x, ASK_Y + 78, 2);
  tft.setTextDatum(TC_DATUM);
  drawAskButton(ASK_OK_X, "OK", TFT_CYAN);
  tft.setTextDatum(TL_DATUM);
}

// Closing one lets viewLoop() see Settings come back and repaint it all.
static void closePopup() { popup = Popup::None; }

static bool inAskButton(int bx, int x, int y) {
  return x >= bx - ASK_BTN_SLOP && x < bx + ASK_BTN_W + ASK_BTN_SLOP &&
         y >= ASK_BTN_Y - ASK_BTN_SLOP && y < ASK_BTN_Y + ASK_BTN_H + ASK_BTN_SLOP;
}

// A popup takes every tap while it's up. The question: Setup opens the
// portal and shows its steps, Cancel or a tap outside the box closes it. The
// steps: OK or a tap outside closes them. The rest of the box does nothing.
static void popupTap(int x, int y) {
  const bool outside = x < ASK_X || x >= ASK_X + ASK_W || y < ASK_Y || y >= ASK_Y + ASK_H;
  if (popup == Popup::SetupSteps) {
    if (outside || inAskButton(ASK_OK_X, x, y)) closePopup();
  } else if (inAskButton(ASK_OK_X, x, y)) {
    Serial.printf("touch: tap %d,%d on Setup -> reopening setup portal\n", x, y);
    wifiForcePortal();
    showSetupSteps();
  } else if (outside || inAskButton(ASK_CANCEL_X, x, y)) {
    closePopup();
  }
}

// Taps: a popup, while it's up, takes them all. Otherwise the tab strip and
// the Meeting tab are ui_meeting's; on Settings the WiFi button asks about
// WiFi setup (or, with the portal open, shows its steps) and a side of the
// switch requests that mic source;
// Device is display only.
// (A -DUI_DEBUG build also takes taps from GET /debug/tap, see ui_meeting.h.)
static void touchLoop() {
  // Something else took the screen (the first meeting summary switches tabs)
  // or the popup no longer fits (BOOT opened the portal under the question,
  // a portal join went online under the steps): it goes without an answer.
  if (popup != Popup::None &&
      (ui_meeting::tab() != ui_meeting::Tab::Settings ||
       (popup == Popup::AskSetup) == (wf == WF_PORTAL)))
    closePopup();
  int x, y;
  if (!board_io::takeTap(&x, &y) && !ui_meeting::debugTakeTap(&x, &y)) return;
  if (popup != Popup::None) { popupTap(x, y); return; }
  if (ui_meeting::handleTap(x, y)) return;
  if (ui_meeting::tab() != ui_meeting::Tab::Settings) return;
  if (inWifiButton(x, y)) {
    Serial.printf("touch: tap %d,%d on WiFi settings\n", x, y);
    if (wf == WF_PORTAL) showSetupSteps();
    else showAskSetup();
    return;
  }
  const int side = micSideAt(x, y);
  if (side < 0) return;
  const engine::Source to = side ? engine::Source::Inbuilt : engine::Source::Array;
  if (to == engine::sourceTarget()) return;
  const bool ok = engine::requestSource(to);
  Serial.printf("touch: tap %d,%d on MIC -> %s%s\n", x, y, engine::sourceName(to),
                ok ? "" : " refused (raw capture running)");
}

// ---- Device ----
// Which half dial θ is drawn in: its hub's y.
static int dialHub(float deg) {
  return fabsf(dsp::wrapDeg(deg)) <= 90 ? DIAL_UP_CY : DIAL_DN_CY;
}

static void dialPoint(float deg, float r, int cy, int* x, int* y) {
  const float t = dsp::deg2rad(deg);
  *x = DIAL_CX + (int)lroundf(r * sinf(t));
  *y = cy - (int)lroundf(r * cosf(t));
}

static void drawHubs() {
  tft.fillCircle(DIAL_CX, DIAL_UP_CY, 4, TFT_LIGHTGREY);
  tft.fillCircle(DIAL_CX, DIAL_DN_CY, 4, TFT_LIGHTGREY);
}

// Ticks every 30°, the flat edges and the axis labels; redrawn after each
// needle erase, which can clip them.
static void drawDialMarks() {
  for (int a = -150; a <= 180; a += 30) {
    if (a == 90 || a == -90) continue;   // the ends of the flat edges
    const int cy = dialHub(a);
    int x0, y0, x1, y1;
    dialPoint(a, DIAL_R - 9, cy, &x0, &y0);
    dialPoint(a, DIAL_R - 3, cy, &x1, &y1);
    tft.drawLine(x0, y0, x1, y1, TFT_DARKGREY);
  }
  tft.drawFastHLine(DIAL_CX - DIAL_R, DIAL_UP_CY, 2 * DIAL_R + 1, DIAL_EDGE);
  tft.drawFastHLine(DIAL_CX - DIAL_R, DIAL_DN_CY, 2 * DIAL_R + 1, DIAL_EDGE);
  if (dialLinear) {
    // The array's ends, between the halves, inside the arcs' ends.
    const int y = (DIAL_UP_CY + DIAL_DN_CY) / 2;
    tft.setTextColor(TFT_DARKGREY, TFT_BLACK);
    tft.setTextDatum(ML_DATUM);
    tft.drawString("M1", DIAL_CX - DIAL_R + 4, y, 2);
    tft.setTextDatum(MR_DATUM);
    tft.drawString("M4", DIAL_CX + DIAL_R - 4, y, 2);
    tft.setTextDatum(TL_DATUM);
  }
}

// "DOA n/a" between the dials while the board mic plays: one mic has no
// direction, and empty dials would look like silence rather than "can't".
// The box covers both hubs and stays inside the arcs.
static const int DIAL_NA_W = 70, DIAL_NA_H = 34;

static void dialNotAvailable(bool show) {
  const int x = DIAL_CX - DIAL_NA_W / 2, y = (DIAL_UP_CY + DIAL_DN_CY) / 2 - DIAL_NA_H / 2;
  tft.fillRect(x, y, DIAL_NA_W, DIAL_NA_H, TFT_BLACK);
  if (show) {
    tft.setTextDatum(TC_DATUM);
    tft.setTextColor(COLOR_BOARD, TFT_BLACK);
    tft.drawString("DOA n/a", DIAL_CX, y + 1, 2);
    tft.setTextColor(TFT_DARKGREY, TFT_BLACK);
    tft.drawString("single mic", DIAL_CX, y + 17, 2);
    tft.setTextDatum(TL_DATUM);
  } else {
    drawDialMarks();
    drawHubs();
  }
}

static void drawBarLabels(int live) {
  tft.fillRect(BAR_X0 - 6, BAR_LABEL_Y, board_io::SCREEN_W - (BAR_X0 - 6), BAR_LABEL_H, TFT_BLACK);
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

static void drawDialFrame() {
  dialLinear = engine::geometry().linear;
  // TFT_eSPI arcs start at 6 o'clock and run clockwise: 90..270 is the top half.
  tft.drawArc(DIAL_CX, DIAL_UP_CY, DIAL_R, DIAL_R - 2, 90, 270, TFT_DARKGREY, TFT_BLACK, false);
  tft.drawArc(DIAL_CX, DIAL_DN_CY, DIAL_R, DIAL_R - 2, 270, 90, TFT_DARKGREY, TFT_BLACK, false);
  drawDialMarks();
  drawHubs();
  // Bar labels are drawn by drawAudio(), which knows how many channels are live.
}

// Device or Settings just came up (or back, from the Meeting tab or the
// popup): clear below the strip and repaint it all, from the
// static frame to every cached element.
static void viewLoop() {
  const ui_meeting::Tab t = ui_meeting::tab();
  const View want = t == ui_meeting::Tab::Device                  ? View::Device
                    : t == ui_meeting::Tab::Settings && popup == Popup::None ? View::Settings
                                                                  : View::None;
  if (want == view) return;
  view = want;
  if (want == View::None) return;   // the Meeting tab or a popup: theirs to draw
  screenEpoch++;
  clearBelowStrip();
  if (want == View::Device) drawDialFrame();
}

static int barPx(float db) {
  const float f = (db - BAR_DB_MIN) / (BAR_DB_MAX - BAR_DB_MIN);
  const int h = (int)lroundf(f * (BAR_BOT - BAR_TOP));
  return h < 0 ? 0 : h > BAR_BOT - BAR_TOP ? BAR_BOT - BAR_TOP : h;
}

// drawWideLine, in absolute screen coordinates. TFT_eSPI 2.5.43's
// drawWedgeLine shifts its scan box by the viewport but not the line itself,
// so inside our viewport it lands SCREEN_INSET_L px left and clipped. Lift the
// viewport around it instead; the dial is nowhere near the blank strip.
static void dialWideLine(int x0, int y0, int x1, int y1, float w, uint16_t color) {
  const int dx = board_io::SCREEN_INSET_L;
  tft.resetViewport();
  tft.drawWideLine(x0 + dx, y0, x1 + dx, y1, w, color, TFT_BLACK);
  tft.setViewport(dx, 0, board_io::SCREEN_W, board_io::SCREEN_H);
}

// θ reflected across a linear array's axis: the direction it can't tell θ from.
static float dialMirror(float deg) { return deg >= 0 ? 180 - deg : -180 - deg; }

static void needleAt(float deg, float w, uint16_t color) {
  const int cy = dialHub(deg);
  int x, y;
  dialPoint(deg, DIAL_R - 12, cy, &x, &y);
  dialWideLine(DIAL_CX, cy, x, y, w, color);
}

// One needle, or a linear array's mirrored pair.
static void drawNeedle(float deg, float w, uint16_t color) {
  needleAt(deg, w, color);
  if (dialLinear) needleAt(dialMirror(deg), w, color);
}

// Filled sector lo..hi (lo ≤ hi) out to the needle's length, as a fan of
// triangles no wider than 3° each; all on the hub of its middle, like the
// steering marker.
static void wedgeAt(float lo, float hi, uint16_t color) {
  const int cy = dialHub(0.5f * (lo + hi));
  const int n = (int)ceilf((hi - lo) / 3.0f);
  int x0, y0;
  dialPoint(lo, DIAL_R - 12, cy, &x0, &y0);
  for (int i = 1; i <= n; i++) {
    int x1, y1;
    dialPoint(lo + (hi - lo) * i / n, DIAL_R - 12, cy, &x1, &y1);
    tft.fillTriangle(DIAL_CX, cy, x0, y0, x1, y1, color);
    x0 = x1;
    y0 = y1;
  }
}

static void drawWedge(float lo, float hi, uint16_t color) {
  if (hi <= lo) return;
  wedgeAt(lo, hi, color);
  if (dialLinear) wedgeAt(dialMirror(hi), dialMirror(lo), color);
}

static void steerTriangleAt(float deg, uint16_t color) {
  const int cy = dialHub(deg);   // the whole marker on its centre's half
  int x0, y0, x1, y1, x2, y2;
  dialPoint(deg, DIAL_R + 2, cy, &x0, &y0);
  dialPoint(deg - 4, DIAL_R + 10, cy, &x1, &y1);
  dialPoint(deg + 4, DIAL_R + 10, cy, &x2, &y2);
  tft.fillTriangle(x0, y0, x1, y1, x2, y2, color);
}

static void steerTriangle(float deg, uint16_t color) {
  steerTriangleAt(deg, color);
  if (dialLinear) steerTriangleAt(dialMirror(deg), color);
}

static void drawAudio(uint32_t now) {
  if (view != View::Device) return;
  // Device was just (re)entered on a cleared screen: everything below repaints.
  static DrawTimer timer;
  if (!timer.due(now)) return;
  const bool fresh = timer.fresh;

  engine::Status a;
  engine::status(&a);
  const bool board = a.source == engine::Source::Inbuilt;
  const int live = engine::sourceChannels(a.source);

  // Note line: what WiFi is doing until it's online, then blank while
  // capture runs normally (the dials and bars say the rest), otherwise why
  // it isn't.
  const bool beam = !board && (a.mode == proc::Mode::FixedBeam || a.mode == proc::Mode::AutoBeam);
  const bool fault = !board && alignFault(a);
  char buf[64] = "";
  uint16_t color = TFT_WHITE;
  if (!audioUp) {
    snprintf(buf, sizeof(buf), "audio engine failed to start");
    color = TFT_RED;
  } else if (board && codecDead(a)) {
    snprintf(buf, sizeof(buf), "codec init failed (see /status)");
    color = TFT_RED;
  } else if (board && !a.running) {
    snprintf(buf, sizeof(buf), "starting: codec init, warm-up");
    color = TFT_YELLOW;
  } else if (board) {
    // running: the dials say "DOA n/a"
  } else if (fault) {
    snprintf(buf, sizeof(buf), "SYNC FAULT: %s", a.alignChanges ? "offset changed" : sync_check::statusName(a.align.status));
    color = TFT_RED;
  } else if (!a.running) {
    snprintf(buf, sizeof(buf), "starting: warm-up, self-test");
    color = TFT_YELLOW;
  } else if (a.mode == proc::Mode::Raw) {
    snprintf(buf, sizeof(buf), "raw capture, no DOA");
  }
  String addr, note;
  uint16_t addrColor;
  wifiText(&addr, &note, &addrColor);
  if (!note.length()) note = buf;
  else color = TFT_WHITE;
  static String lastNote;
  if (fresh) lastNote = "";
  const String noteKey = note + '|' + color;
  if (noteKey != lastNote) {
    lastNote = noteKey;
    drawLine(note, ADDR_X, NOTE_Y, color);
  }

  // Board mic: the "n/a" panel replaces needles and steering markers.
  // Cleared before a needle can come back, drawn after they have been erased.
  static bool naShown = false;
  if (fresh) naShown = false;
  if (!board && naShown) { dialNotAvailable(false); naShown = false; }

  // Needle at the tracked DOA, as a table azimuth: bright while the speech
  // gate is open, dim in silence, absent until something has been tracked.
  // Behind it, a wedge over every azimuth the talker's unknown height allows
  // (DOA_ELEV_MIN_DEG..MAX): a line near broadside, wide toward the axis.
  static int needleDeg = 9999, wedgeLo = 0, wedgeHi = 0;
  static uint16_t needleColor = 0;
  if (fresh) needleDeg = 9999;   // nothing to erase on a cleared screen
  const bool showNeedle = !board && a.smoothValid && a.mode != proc::Mode::Raw;
  const int nd = showNeedle ? (int)lroundf(a.smoothAzDeg) : 9999;
  int wl = 0, wh = 0;
  if (showNeedle) {
    const float e0 = doa::tableAzimuthDeg(a.smoothDeg, dialLinear, audio::DOA_ELEV_MIN_DEG);
    const float e1 = doa::tableAzimuthDeg(a.smoothDeg, dialLinear, audio::DOA_ELEV_MAX_DEG);
    wl = (int)lroundf(fminf(e0, e1));
    wh = (int)lroundf(fmaxf(e0, e1));
  }
  const uint16_t nc = a.speech ? TFT_GREEN : TFT_DARKGREEN;
  if (nd != needleDeg || wl != wedgeLo || wh != wedgeHi || (showNeedle && nc != needleColor)) {
    if (needleDeg != 9999) {
      drawNeedle(needleDeg, 5, TFT_BLACK);
      drawWedge(wedgeLo, wedgeHi, TFT_BLACK);
      drawDialMarks();
    }
    if (showNeedle) {
      drawWedge(wl, wh, a.speech ? 0x0320 : 0x0180);   // dim greens, under the needle's
      drawNeedle(nd, 3, nc);
    }
    drawHubs();
    needleDeg = nd;
    wedgeLo = wl;
    wedgeHi = wh;
    needleColor = nc;
  }

  // Beam steering marker, outside the arcs, in beam modes only; a table
  // azimuth like the needle, so the two line up on a tracked talker.
  static int steerDeg = 9999;
  if (fresh) steerDeg = 9999;
  const int sd = beam ? (int)lroundf(a.steerAzDeg) : 9999;
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
  if (fresh) lastLive = 0;   // relabels and repaints every bar
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
  Serial.printf("[audio] src=array%s %s doa=%+.1f c=%.2f trk=%s%+.1f az=%+.1f acc=%lu beam=%+.1f gate=%d "
                "lvl=%.0f/%.0f/%.0f/%.0f out=%.0f dBFS dsp=%.0f/%.0fus(%.0f%%) "
                "ring=%u/%u rdrop=%lu ovf=%lu/%lu to=%lu/%lu rs=%lu sw=%lu mm=%lu "
                "align=%s@%d(%lu/%lu chg=%lu) sdrop=%lu\n",
                to, proc::modeName(a.mode), a.doaDeg, a.confidence, a.smoothValid ? "" : "?", a.smoothDeg, a.smoothAzDeg,
                (unsigned long)a.doaAccepts,
                a.steerDeg, a.speech, a.micDb[0], a.micDb[1], a.micDb[2], a.micDb[3], a.beamDb,
                a.dspMeanUs, a.dspMaxUs, 100.0f * a.dspMaxUs / audio::HOP_US,
                a.ringDepth, a.ringHigh, (unsigned long)a.ringDrops,
                (unsigned long)a.overflows[0], (unsigned long)a.overflows[1],
                (unsigned long)a.timeouts[0], (unsigned long)a.timeouts[1],
                (unsigned long)a.resyncs, (unsigned long)a.sourceSwitches, (unsigned long)a.mismatches,
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

  netLock = xSemaphoreCreateMutex();
  ctlLock = xSemaphoreCreateMutex();
  web::begin();                    // before anything uses mbedTLS

  tft.init();
  tft.setRotation(board_io::TFT_ROTATION);   // landscape 320x240, either way up
  tft.fillScreen(TFT_BLACK);
  // Everything from here on draws right of the blank left strip.
  tft.setViewport(board_io::SCREEN_INSET_L, 0, board_io::SCREEN_W, board_io::SCREEN_H);
  ui_meeting::begin(&tft);         // the tab strip; boots on Device, which loop() draws
  tft.setTextDatum(TL_DATUM);
  tft.setTextColor(TFT_DARKGREY, TFT_BLACK);

  board_io::begin();               // I2C bus + touch reset; the codec stays untouched until inbuilt
  web::siteBegin();                // before the server starts: the portal serves at once

  // The HTTPS certificate: from NVS, or made now on the first boot (~1 s,
  // before the audio tasks exist). Without one the app is served over HTTP.
  tft.drawString("TLS certificate... (first boot: new key)", ADDR_X, NOTE_Y, 2);
  certOk = tls_cert::load(&pem);
  if (certOk) web::setCert(pem, tls_cert::fingerprint());
  else httpsNote = "no certificate: tls_cert::load() failed (see serial); serving HTTP only";
  Serial.printf("tls: %s\n", certOk ? "certificate ready, HTTPS starts when online" : "no certificate, HTTP only");
  tft.fillRect(0, NOTE_Y, board_io::SCREEN_W, NOTE_H, TFT_BLACK);

  wifiStart();
  audioUp = engine::begin();
  web::moreRam();                  // after the engine's internal-RAM allocations
  audio_server::begin();           // after wifiStart(): needs the network stack up
  meeting::begin();                // before the server takes POST /meeting
  web::startHttp(ROUTES, sizeof(ROUTES) / sizeof(ROUTES[0]), notFound);   // after audio_server: it adds the streams
  netPublish(millis());

  engine::Status a;
  engine::status(&a);
  Serial.printf("audio: %s; fft check esp-dsp vs portable %.1e rel (%.0f vs %.0f us)\n",
                audioUp ? "running" : "FAILED to start", a.fftMaxErr, a.fftEspUs, a.fftRefUs);
  // PSRAM size 0 means the octal-PSRAM build keys in platformio.ini are off.
  Serial.printf("[mem] internal free %u (largest %u), psram: %u/%u free\n",
                (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
                (unsigned)ESP.getFreePsram(), (unsigned)ESP.getPsramSize());
}

void loop() {
  uint32_t now = millis();
  wifiLoop(now);
  httpsLoop(now);
  buttonsLoop(now);
  board_io::loop(now);             // codec jobs for the capture task, touch polling
  touchLoop();
  engine::loopPoll();
  ui_meeting::loop(now);           // the tab strip, and the Meeting tab while it's up
  viewLoop();                      // Device or Settings coming up: clear and draw the frame
  drawAddress(now);                // these two: the Device tab only
  drawAudio(now);
  drawSettings(now);               // the Settings tab only
  serialLoop(now);
  delay(2);   // yield to the WiFi/idle tasks
}
