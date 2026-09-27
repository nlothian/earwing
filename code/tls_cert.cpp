#include "tls_cert.h"

#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <mbedtls/ctr_drbg.h>
#include <mbedtls/ecp.h>
#include <mbedtls/entropy.h>
#include <mbedtls/error.h>
#include <mbedtls/oid.h>
#include <mbedtls/pk.h>
#include <mbedtls/platform_util.h>
#include <mbedtls/sha256.h>
#include <mbedtls/x509_crt.h>

#if defined(ESP_PLATFORM)
#include <Arduino.h>
#include <Preferences.h>
#include <esp_heap_caps.h>
#endif

// Everything here but the "device glue" section is plain mbedTLS 3.x, so the
// generation and checking can be built on a host against a desktop mbedTLS
// and the result read with openssl (the way dsp.cpp has a portable path).
//
// Thread-safety: load() and regenerate() are called from setup()/loop() only,
// never concurrently. fingerprint()'s string is written only by those two, so
// any task may read it between them (e.g. a status handler).

namespace tls_cert {

// ---------- the certificate's profile ----------
// What a browser checks on a user-trusted self-signed server certificate: the
// host name in the SAN (the CN alone is ignored), serverAuth in the EKU, a
// validity no longer than 825 days (Apple rejects longer ones even when the
// user trusts them), and a SHA-256 signature. It's a leaf, not a CA:
// CA:FALSE, and the key signs the ECDHE handshake (digitalSignature) and
// nothing else.
static const char HOST[] = "heimdallr.local";
static const char DN[] = "CN=heimdallr.local,O=heimdallr";   // subject = issuer
static const int64_t DAY_S = 86400;
static const int VALID_DAYS = 825;
// notBefore is a day before "now", so a device clock (or build date) a bit
// ahead of the browser's doesn't make the certificate not-yet-valid.
static const int BACKDATE_DAYS = 1;
static const int RENEW_DAYS = 30;       // load() replaces a pair expiring this soon
static const size_t SERIAL_LEN = 16;    // random, positive (RFC 5280: ≤ 20 bytes)
// PEM output space: the certificate comes out at ~700 bytes, the SEC1 key at
// ~230; generous so a longer DN can't silently fail.
static const size_t CERT_PEM_MAX = 2048;
static const size_t KEY_PEM_MAX = 512;

// ---------- device glue ----------
// NVS, the serial port, the millisecond clock and PSRAM: the only parts that
// touch the ESP32. Off the device the host test supplies the store (so it can
// plant stale or broken pairs) and the rest falls back to the C library.
#if defined(ESP_PLATFORM)

static const char* NVS_NS = "tls";

static void logLine(const char* fmt, ...) {
  char buf[256];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  Serial.println(buf);
}

static uint32_t nowMs() { return millis(); }

// The PEMs and the scratch space go to PSRAM: nothing here is hot.
static void* bigAlloc(size_t n) {
  void* p = heap_caps_calloc(1, n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  return p ? p : calloc(1, n);
}

static char* strDup(const String& s) {
  char* p = (char*)bigAlloc(s.length() + 1);
  if (p) memcpy(p, s.c_str(), s.length() + 1);
  return p;
}

// The stored pair as heap strings (the caller frees them), or false if there
// isn't one. The namespace doesn't exist until the first save, and a
// read-only begin() fails then.
static bool storeLoad(char** cert, char** key) {
  *cert = *key = nullptr;
  Preferences p;
  if (!p.begin(NVS_NS, true)) return false;
  if (p.isKey("cert") && p.isKey("key")) {
    *cert = strDup(p.getString("cert"));
    *key = strDup(p.getString("key"));
  }
  p.end();
  if (*cert && *key) return true;
  free(*cert);
  free(*key);
  *cert = *key = nullptr;
  return false;
}

// putString() returns the bytes written, 0 on failure. A power cut between the
// two leaves a mismatched pair, which load() notices and replaces.
static bool storeSave(const char* cert, const char* key) {
  Preferences p;
  if (!p.begin(NVS_NS, false)) return false;
  const bool ok = p.putString("cert", cert) > 0 && p.putString("key", key) > 0;
  p.end();
  return ok;
}

static void storeClear() {
  Preferences p;
  if (!p.begin(NVS_NS, false)) return;
  p.clear();
  p.end();
}

#else   // host build

bool storeLoad(char** cert, char** key);   // defined by the host test
bool storeSave(const char* cert, const char* key);
void storeClear();

static void logLine(const char* fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  vprintf(fmt, ap);
  va_end(ap);
  printf("\n");
}

static uint32_t nowMs() { return (uint32_t)((uint64_t)clock() * 1000 / CLOCKS_PER_SEC); }

static void* bigAlloc(size_t n) { return calloc(1, n); }

#endif

// ---------- time ----------
// The device has no RTC and SNTP may not have run, so time() can be seconds
// since boot. This file's compile time is a lower bound on the real date, so
// "now" is the later of the two. Without SNTP the certificate is dated from
// the build: fine for the browser, which only needs notBefore in its past.
// (One consequence: a board that never gets SNTP and keeps the same build for
// ~795 days never notices its certificate expiring; regenerate() fixes that.)

// Days since 1970-01-01 of a proleptic Gregorian date (Howard Hinnant's
// days_from_civil): newlib has no timegm(), and no time zone is wanted.
static int64_t daysFromCivil(int y, int m, int d) {
  y -= m <= 2;
  const int64_t era = (y >= 0 ? y : y - 399) / 400;
  const int yoe = (int)(y - era * 400);
  const int doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
  const int doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097 + doe - 719468;
}

static int64_t epochOf(const mbedtls_x509_time& t) {
  return daysFromCivil(t.year, t.mon, t.day) * DAY_S + t.hour * 3600 + t.min * 60 + t.sec;
}

// __DATE__ ("Sep  6 2026") and __TIME__ ("12:34:56"), read as UTC (the build
// machine's zone is at most a day off, which BACKDATE_DAYS covers).
static int64_t buildTime() {
  static const char MONTHS[] = "JanFebMarAprMayJunJulAugSepOctNovDec";
  const char* date = __DATE__;
  const char* hms = __TIME__;
  int mon = 1;
  for (int i = 0; i < 12; i++) if (strncmp(date, MONTHS + 3 * i, 3) == 0) mon = i + 1;
  mbedtls_x509_time t;
  t.year = atoi(date + 7);
  t.mon = mon;
  t.day = atoi(date + 4);
  t.hour = atoi(hms);
  t.min = atoi(hms + 3);
  t.sec = atoi(hms + 6);
  return epochOf(t);
}

static int64_t nowS() {
  const int64_t t = (int64_t)time(nullptr);
  const int64_t b = buildTime();
  return t > b ? t : b;
}

// mbedTLS's validity format, "YYYYMMDDhhmmss" UTC.
static void formatTime(int64_t s, char out[15]) {
  const time_t tt = (time_t)s;
  struct tm g;
  gmtime_r(&tt, &g);
  snprintf(out, 15, "%04d%02d%02d%02d%02d%02d", g.tm_year + 1900, g.tm_mon + 1, g.tm_mday,
           g.tm_hour, g.tm_min, g.tm_sec);
}

// ---------- state ----------
static const size_t FP_LEN = 32 * 3;   // "AB:CD:..." + NUL
static const size_t DATE_LEN = 11;     // "YYYY-MM-DD" + NUL

static char* certPem;          // the current pair (heap), handed out by load()
static char* keyPem;
static char  fp[FP_LEN];       // the current pair's fingerprint
static char  validTo[DATE_LEN];   // and end date, for the log lines

// Where the last attempt stopped, for its serial line: the step, and the
// mbedTLS error (0 when the failure wasn't mbedTLS's).
static const char* failStep = "";
static int failErr = 0;

static bool fail(const char* step, int err) {
  failStep = step;
  failErr = err;
  return false;
}

static void logFailure(const char* what) {
  char msg[96] = "";
  if (failErr) mbedtls_strerror(failErr, msg, sizeof(msg));
  if (failErr) logLine("tls: %s: failed at %s: -0x%04X %s", what, failStep, -failErr, msg);
  else logLine("tls: %s: failed at %s", what, failStep);
}

// All the mbedTLS state one load()/regenerate() needs, on the heap: the
// entropy pool alone is ~1 KB, and the loop task has 8 KB of stack.
struct Work {
  mbedtls_entropy_context  entropy;
  mbedtls_ctr_drbg_context drbg;
  mbedtls_pk_context       key;
  mbedtls_x509_crt         crt;
  mbedtls_x509write_cert   w;
  char certOut[CERT_PEM_MAX];    // generate()'s output
  char keyOut[KEY_PEM_MAX];
  char fp[FP_LEN];               // check()'s findings, made current by adopt()
  char validTo[DATE_LEN];
};

static void workFree(Work* k) {
  if (!k) return;
  mbedtls_x509write_crt_free(&k->w);
  mbedtls_x509_crt_free(&k->crt);
  mbedtls_pk_free(&k->key);
  mbedtls_ctr_drbg_free(&k->drbg);
  mbedtls_entropy_free(&k->entropy);
  mbedtls_platform_zeroize(k, sizeof(Work));   // the key's PEM was in keyOut
  free(k);
}

// Entropy: on the ESP32 mbedTLS's pool polls the hardware RNG
// (MBEDTLS_ENTROPY_HARDWARE_ALT, i.e. esp_fill_random), which is truly
// random once the radio is on, so generate after WiFi is up where possible.
static Work* workNew() {
  Work* k = (Work*)bigAlloc(sizeof(Work));
  if (!k) { fail("allocating scratch space", 0); return nullptr; }
  mbedtls_entropy_init(&k->entropy);
  mbedtls_ctr_drbg_init(&k->drbg);
  mbedtls_pk_init(&k->key);
  mbedtls_x509_crt_init(&k->crt);
  mbedtls_x509write_crt_init(&k->w);
  static const unsigned char PERS[] = "heimdallr-tls";
  const int e = mbedtls_ctr_drbg_seed(&k->drbg, mbedtls_entropy_func, &k->entropy, PERS, sizeof(PERS) - 1);
  if (e == 0) return k;
  fail("seeding the RNG", e);
  workFree(k);
  return nullptr;
}

// ---------- generate ----------
// A fresh key and certificate into k->certOut / k->keyOut.
static bool generate(Work* k) {
  int e;
  mbedtls_pk_free(&k->key);
  mbedtls_pk_init(&k->key);
  if ((e = mbedtls_pk_setup(&k->key, mbedtls_pk_info_from_type(MBEDTLS_PK_ECKEY))) != 0)
    return fail("pk setup", e);
  if ((e = mbedtls_ecp_gen_key(MBEDTLS_ECP_DP_SECP256R1, mbedtls_pk_ec(k->key),
                               mbedtls_ctr_drbg_random, &k->drbg)) != 0)
    return fail("generating the P-256 key", e);

  mbedtls_x509write_cert* w = &k->w;
  mbedtls_x509write_crt_free(w);
  mbedtls_x509write_crt_init(w);
  mbedtls_x509write_crt_set_version(w, MBEDTLS_X509_CRT_VERSION_3);
  mbedtls_x509write_crt_set_md_alg(w, MBEDTLS_MD_SHA256);
  mbedtls_x509write_crt_set_subject_key(w, &k->key);
  mbedtls_x509write_crt_set_issuer_key(w, &k->key);   // self-signed

  // Serial: random, top bit clear (a DER INTEGER with it set is negative) and
  // the next set, so it's always the full 16 bytes.
  unsigned char serial[SERIAL_LEN];
  if ((e = mbedtls_ctr_drbg_random(&k->drbg, serial, sizeof(serial))) != 0) return fail("serial", e);
  serial[0] = (unsigned char)((serial[0] & 0x7F) | 0x40);
  if ((e = mbedtls_x509write_crt_set_serial_raw(w, serial, sizeof(serial))) != 0) return fail("serial", e);

  if ((e = mbedtls_x509write_crt_set_subject_name(w, DN)) != 0) return fail("subject", e);
  if ((e = mbedtls_x509write_crt_set_issuer_name(w, DN)) != 0) return fail("issuer", e);

  const int64_t from = nowS() - BACKDATE_DAYS * DAY_S;
  char nb[15], na[15];
  formatTime(from, nb);
  formatTime(from + VALID_DAYS * DAY_S, na);
  if ((e = mbedtls_x509write_crt_set_validity(w, nb, na)) != 0) return fail("validity", e);

  // Extensions. CA:FALSE, no path length; mbedTLS marks keyUsage critical.
  if ((e = mbedtls_x509write_crt_set_basic_constraints(w, 0, -1)) != 0) return fail("basicConstraints", e);
  if ((e = mbedtls_x509write_crt_set_key_usage(w, MBEDTLS_X509_KU_DIGITAL_SIGNATURE)) != 0)
    return fail("keyUsage", e);

  mbedtls_asn1_sequence eku;
  memset(&eku, 0, sizeof(eku));
  eku.buf.tag = MBEDTLS_ASN1_OID;
  eku.buf.p = (unsigned char*)MBEDTLS_OID_SERVER_AUTH;
  eku.buf.len = MBEDTLS_OID_SIZE(MBEDTLS_OID_SERVER_AUTH);
  if ((e = mbedtls_x509write_crt_set_ext_key_usage(w, &eku)) != 0) return fail("extKeyUsage", e);

  mbedtls_x509_san_list san;
  memset(&san, 0, sizeof(san));
  san.node.type = MBEDTLS_X509_SAN_DNS_NAME;
  san.node.san.unstructured_name.p = (unsigned char*)HOST;
  san.node.san.unstructured_name.len = sizeof(HOST) - 1;
  if ((e = mbedtls_x509write_crt_set_subject_alternative_name(w, &san)) != 0)
    return fail("subjectAltName", e);

  // Key identifiers (SHA-1 of the public key, the same one in both as it's
  // self-signed): these need the subject and issuer keys set, above.
  if ((e = mbedtls_x509write_crt_set_subject_key_identifier(w)) != 0) return fail("subjectKeyIdentifier", e);
  if ((e = mbedtls_x509write_crt_set_authority_key_identifier(w)) != 0)
    return fail("authorityKeyIdentifier", e);

  if ((e = mbedtls_x509write_crt_pem(w, (unsigned char*)k->certOut, sizeof(k->certOut),
                                     mbedtls_ctr_drbg_random, &k->drbg)) != 0)
    return fail("signing the certificate", e);
  if ((e = mbedtls_pk_write_key_pem(&k->key, (unsigned char*)k->keyOut, sizeof(k->keyOut))) != 0)
    return fail("writing the key", e);
  return true;
}

// ---------- check ----------
static const char* problem(const char* what, int err) {
  failErr = err;
  return what;
}

// Parse a PEM pair, check the key is the certificate's and it has more than
// RENEW_DAYS left, and note its fingerprint and end date in k. Null if it's
// usable, otherwise why not (failErr has the mbedTLS error, if any).
static const char* check(Work* k, const char* cert, const char* key) {
  int e;
  failErr = 0;
  mbedtls_x509_crt_free(&k->crt);
  mbedtls_x509_crt_init(&k->crt);
  mbedtls_pk_free(&k->key);
  mbedtls_pk_init(&k->key);
  // PEM parsing wants the NUL counted in the length.
  if ((e = mbedtls_x509_crt_parse(&k->crt, (const unsigned char*)cert, strlen(cert) + 1)) != 0)
    return problem("certificate doesn't parse", e);
  if ((e = mbedtls_pk_parse_key(&k->key, (const unsigned char*)key, strlen(key) + 1, nullptr, 0,
                                mbedtls_ctr_drbg_random, &k->drbg)) != 0)
    return problem("key doesn't parse", e);
  if ((e = mbedtls_pk_check_pair(&k->crt.pk, &k->key, mbedtls_ctr_drbg_random, &k->drbg)) != 0)
    return problem("key doesn't match the certificate", e);
  if (epochOf(k->crt.valid_to) - nowS() < RENEW_DAYS * DAY_S) return "expires within 30 days";

  unsigned char h[32];
  if ((e = mbedtls_sha256(k->crt.raw.p, k->crt.raw.len, h, 0)) != 0) return problem("fingerprint", e);
  static const char DIGITS[] = "0123456789ABCDEF";   // (Arduino #defines HEX)
  for (int i = 0; i < 32; i++) {
    k->fp[3 * i] = DIGITS[h[i] >> 4];
    k->fp[3 * i + 1] = DIGITS[h[i] & 15];
    k->fp[3 * i + 2] = i < 31 ? ':' : 0;
  }
  const mbedtls_x509_time& t = k->crt.valid_to;
  snprintf(k->validTo, sizeof(k->validTo), "%04d-%02d-%02d", t.year % 10000, t.mon % 100, t.day % 100);
  return nullptr;
}

// "<whose>: <problem>[ (-0xNNNN)]", the reason a pair is being replaced.
static void describe(char* out, size_t n, const char* whose, const char* bad) {
  if (failErr) snprintf(out, n, "%s: %s (-0x%04X)", whose, bad, -failErr);
  else snprintf(out, n, "%s: %s", whose, bad);
}

// ---------- public ----------
// Make a checked pair current: its heap PEMs (owned from here on) and the
// fingerprint and end date check() found for it.
static void adopt(Work* k, char* cert, char* key) {
  certPem = cert;
  keyPem = key;
  memcpy(fp, k->fp, sizeof(fp));
  memcpy(validTo, k->validTo, sizeof(validTo));
}

// A heap copy the caller can keep.
static char* keep(const char* s) {
  const size_t n = strlen(s) + 1;
  char* p = (char*)bigAlloc(n);
  if (p) memcpy(p, s, n);
  return p;
}

// Make, check, store and adopt a new pair; `why` is for the log. The previous
// buffers are left alone: the caller frees them if it may.
static bool makeNew(Work* k, const char* why) {
  const uint32_t t0 = nowMs();
  if (!generate(k)) return false;
  // Parse what was just written, as load() will at the next boot. A failure
  // here is a bug, not bad luck.
  if (const char* bad = check(k, k->certOut, k->keyOut)) return fail(bad, failErr);
  char* c = keep(k->certOut);
  char* y = keep(k->keyOut);
  if (!c || !y) {
    free(c);
    free(y);
    return fail("allocating the PEM buffers", 0);
  }
  adopt(k, c, y);
  const bool saved = storeSave(certPem, keyPem);
  logLine("tls: generated a new certificate in %u ms (%s), valid to %s%s, SHA-256 %s",
          (unsigned)(nowMs() - t0), why, validTo, saved ? "" : ", NOT saved (NVS write failed)", fp);
  return true;
}

static void handOut(Pem* out) {
  out->cert = certPem;
  out->certLen = strlen(certPem) + 1;
  out->key = keyPem;
  out->keyLen = strlen(keyPem) + 1;
}

bool load(Pem* out) {
  Work* k = workNew();
  if (!k) { logFailure("load"); return false; }
  char why[96] = "none stored";
  if (certPem) {
    // Already loaded: the same buffers, unless they're about to expire (a
    // long uptime after SNTP). Then a new pair, and the old buffers stay
    // allocated, ~1 KB once per ~795 days: the header promises they live
    // until regenerate(), and a running server may still hold them. If
    // making one fails, the old pair is still good for a while.
    if (const char* bad = check(k, certPem, keyPem)) {
      describe(why, sizeof(why), "current one", bad);
      if (!makeNew(k, why)) logFailure("renewing (keeping the current one)");
    }
  } else {
    char* c;
    char* y;
    if (storeLoad(&c, &y)) {
      if (const char* bad = check(k, c, y)) {
        describe(why, sizeof(why), "stored one", bad);
        free(c);
        free(y);
      } else {
        adopt(k, c, y);
        logLine("tls: loaded the stored certificate, valid to %s, SHA-256 %s", validTo, fp);
      }
    }
    if (!certPem && !makeNew(k, why)) logFailure("no certificate");
  }
  const bool ok = certPem != nullptr;
  if (ok) handOut(out);
  workFree(k);
  return ok;
}

bool regenerate(Pem* out) {
  Work* k = workNew();
  if (!k) { logFailure("regenerate"); return false; }
  storeClear();
  char* oldCert = certPem;
  char* oldKey = keyPem;
  certPem = keyPem = nullptr;
  fp[0] = 0;
  const bool ok = makeNew(k, "asked for");
  if (oldKey) mbedtls_platform_zeroize(oldKey, strlen(oldKey));
  free(oldCert);
  free(oldKey);
  if (ok) handOut(out);
  else logFailure("regenerate");
  workFree(k);
  return ok;
}

const char* fingerprint() { return fp; }

}  // namespace tls_cert
