// gulltoppr — WiFi bring-up for the mic-array ESP32-S3 (Freenove FNK0104,
// see platformio.ini).
//
// WiFi lifecycle: STA join with saved credentials, captive-portal fallback
// for first-time setup, and an mDNS-advertised webserver at
// http://gulltoppr.local/ once online. No WiFiManager: it wants to own the
// radio and blocks by default, whereas everything here is called from loop()
// and never blocks, leaving room for the audio pipeline to come.
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
// portal join succeeds with replacements.

#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <ESPmDNS.h>
#include <Preferences.h>
#include <TFT_eSPI.h>   // configured entirely via build_flags in platformio.ini

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

static void statusPage() {
  String ssid = WiFi.SSID();
  htmlEscape(ssid);
  uint32_t up = millis() / 1000;
  char upBuf[24];
  snprintf(upBuf, sizeof(upBuf), "%lud %02lu:%02lu:%02lu",
           (unsigned long)(up / 86400), (unsigned long)(up / 3600 % 24),
           (unsigned long)(up / 60 % 60), (unsigned long)(up % 60));

  String page = PAGE_HEAD;
  page += "<h2>gulltoppr</h2><table>"
          "<tr><td>Network</td><td>" + ssid + "</td></tr>"
          "<tr><td>IP</td><td>" + WiFi.localIP().toString() + "</td></tr>"
          "<tr><td>Signal</td><td>" + String(WiFi.RSSI()) + " dBm</td></tr>"
          "<tr><td>Uptime</td><td>" + upBuf + "</td></tr>"
          "<tr><td>Free heap</td><td>" + String(ESP.getFreeHeap() / 1024) + " KB</td></tr>"
          "</table></body></html>";
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
// Direct drawing, repainted only when the shown text changes. A full-screen
// sprite (150KB at 320x240) would fit in PSRAM, but nothing here animates.
static TFT_eSPI tft;

static void drawStatus() {
  // Everything the screen shows, so we can skip redundant repaints.
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
  String key = line1 + '|' + line2 + '|' + line3 + '|' + color;
  if (key == last) return;
  last = key;

  tft.fillScreen(TFT_BLACK);
  tft.setTextDatum(TL_DATUM);
  tft.setTextColor(TFT_DARKGREY, TFT_BLACK);
  tft.drawString(NAME, 12, 10, 2);
  tft.setTextColor(color, TFT_BLACK);
  tft.drawString(line1, 12, 50, 4);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.drawString(line2, 12, 100, 2);   // font 2: a 32-char SSID still fits 320px
  tft.drawString(line3, 12, 124, 2);
  if (wf != WF_PORTAL) {
    tft.setTextColor(TFT_DARKGREY, TFT_BLACK);
    tft.drawString("hold BOOT: wifi setup", 12, 220, 2);
  }
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
  pinMode(PIN_BTN_BOOT, INPUT_PULLUP);

  tft.init();
  tft.setRotation(3);              // landscape 320x240
  tft.fillScreen(TFT_BLACK);

  wifiStart();
  drawStatus();
  // PSRAM size 0 means the octal-PSRAM build keys in platformio.ini are off.
  Serial.printf("[mem] heap after wifi: %lu, psram: %lu/%lu free\n",
                (unsigned long)ESP.getFreeHeap(),
                (unsigned long)ESP.getFreePsram(), (unsigned long)ESP.getPsramSize());
}

void loop() {
  uint32_t now = millis();
  wifiLoop(now);
  buttonsLoop(now);
  drawStatus();
  delay(2);   // yield to the WiFi/idle tasks
}
