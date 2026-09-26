// The HTTPS server's certificate: a self-signed ECDSA P-256 certificate for
// gulltoppr.local, generated on the device the first time it's needed and
// kept in NVS, so every board has its own key and nothing secret lives in the
// repo. Browsers warn about it until the user trusts it once; /gulltoppr.crt
// (served by main.cpp) hands the certificate over for that.
//
// Only HTTPS makes the page a secure context, which is what the browser
// needs before it offers WebGPU and threaded WASM to the transcriber.
//
// load() and regenerate() run from setup()/loop() only, one at a time (they
// aren't reentrant). Generate with the radio on where possible: the ESP32's
// hardware RNG, which seeds the key, is only truly random then.
#pragma once

#include <stddef.h>

namespace tls_cert {

struct Pem {
  const char* cert;   // PEM, NUL-terminated
  size_t      certLen;  // including the NUL, as esp_https_server wants it
  const char* key;    // PEM, NUL-terminated
  size_t      keyLen;   // including the NUL
};

// Load the certificate and key from NVS, generating and saving them first if
// there are none (or the stored ones fail to parse, or expire within 30
// days). Blocks: generation takes on the order of a second, so call it from
// setup()/loop(), never from an audio task. The buffers live until
// regenerate(). False (and a serial line saying why) if nothing is usable.
bool load(Pem* out);

// Throw the stored pair away and make a new one, e.g. when the user wants a
// fresh key. Pointers from an earlier load() are invalid afterwards.
bool regenerate(Pem* out);

// SHA-256 fingerprint of the certificate as "AB:CD:...", for the status page
// so the user can check what they're trusting. Empty before load().
const char* fingerprint();

}  // namespace tls_cert
