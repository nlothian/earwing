// The board's I2C devices: the ES8311 codec (0x18, for the inbuilt mic) and
// the FT6336U touch panel (0x38). They share one bus (SDA 16, SCL 15), and
// every transaction on it happens here, from loop(), so the bus needs no lock.
// The audio engine asks for codec work through its job mailbox
// (engine::codecJob) instead of touching I2C itself.
//
// Only the codec's ADC path is ever configured: its DAC stays powered down
// and muted, and GPIO1 (the speaker amp enable) is never driven.
#pragma once

#include <stdint.h>

namespace board_io {

void begin();             // setup(): bus up, touch reset pulse started (no waiting)
void loop(uint32_t now);  // loop(): touch reset/poll, and any codec job the engine posted

// A touch-down edge on the landscape canvas (rotation 3, calibrated), since
// the last call. Holding a finger down gives one tap, not a stream.
bool takeTap(int* x, int* y);

struct Info {
  bool        touchReady;     // reset done, settle time over
  bool        touchOk;        // last poll was answered
  uint32_t    taps;           // touch-down edges since boot
  uint32_t    touchErrors;    // polls that got no answer
  const char* codecDetail;    // last codec job: "ok", or the step that failed
};
Info info();

}  // namespace board_io
