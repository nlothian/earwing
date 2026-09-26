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

// The TFT's rotation (landscape 320x240): main.cpp passes it to
// tft.setRotation(), and takeTap() reports taps in the same orientation.
// 1 or 3 (the two landscape ones, 180° apart); board_io.cpp asserts it.
static const uint8_t TFT_ROTATION = 1;

// A touch-down edge since the last call, calibrated, in screen coordinates
// at rotation TFT_ROTATION (x 0..319, y 0..239), so hit tests can use the
// same numbers as the drawing. Holding a finger down gives one tap, not a
// stream. The calibration is weakest along one long edge, the top ~20 px at
// rotation 1 (the bottom at 3): keep touch targets off it.
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
