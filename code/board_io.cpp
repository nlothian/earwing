#include "board_io.h"

#include <Arduino.h>
#include <Wire.h>
#include <stdio.h>
#include "audio_engine.h"

namespace board_io {

// ---------- bus ----------
static const int PIN_I2C_SDA = 16;
static const int PIN_I2C_SCL = 15;
static const uint32_t I2C_HZ = 400000;        // both devices are fine at 400 kHz
// A transaction takes well under 1 ms at 400 kHz; a stuck bus gives up after
// this instead of the default 50 ms, so loop() can't stall on it for long.
static const uint16_t I2C_TIMEOUT_MS = 10;

static bool regWrite(uint8_t addr, uint8_t reg, uint8_t v) {
  Wire.beginTransmission(addr);
  Wire.write(reg);
  Wire.write(v);
  return Wire.endTransmission() == 0;
}

static bool regRead(uint8_t addr, uint8_t reg, uint8_t* out, int n) {
  Wire.beginTransmission(addr);
  Wire.write(reg);
  if (Wire.endTransmission(false) != 0) return false;   // repeated start
  if (Wire.requestFrom((int)addr, n) != n) return false;
  for (int i = 0; i < n; i++) out[i] = (uint8_t)Wire.read();
  return true;
}

// ---------- ES8311 ----------
// The ADC path of the Espressif reference driver, in its order, as ported by
// AMY's es8311.c (branch es8311-codec-support), which is proven on this board
// (FNK0104-BOARD.md). What differs, and why:
//   * the DAC is powered down (reg12 = 0x02, the value AMY's deinit uses) and
//     muted, its serial input muted, and the headphone-driver enable (reg13)
//     and DAC ramp (reg37) writes are left out: nothing here plays audio;
//   * the power-amp step is gone. GPIO1 is never driven: its default pull
//     keeps the speaker amp off.
// The codec is an I2S slave clocked from its MCLK pin; the engine's codec
// channel is already running MCLK/BCLK/WS when Init is posted, which is the
// order both reference drivers use.
static const uint8_t ES8311_ADDR = 0x18;

// Registers (names from Espressif's es8311_reg.h).
static const uint8_t R_RESET = 0x00, R_CLK01 = 0x01, R_CLK02 = 0x02, R_CLK03 = 0x03,
                     R_CLK04 = 0x04, R_CLK05 = 0x05, R_CLK06 = 0x06, R_CLK07 = 0x07,
                     R_CLK08 = 0x08, R_SDPIN = 0x09, R_SDPOUT = 0x0A, R_SYS0B = 0x0B,
                     R_SYS0C = 0x0C, R_SYS0D = 0x0D, R_SYS0E = 0x0E, R_SYS10 = 0x10,
                     R_SYS11 = 0x11, R_SYS12 = 0x12, R_SYS14 = 0x14, R_ADC15 = 0x15,
                     R_ADC16 = 0x16, R_ADC17 = 0x17, R_ADC1B = 0x1B, R_ADC1C = 0x1C,
                     R_DAC31 = 0x31, R_DAC32 = 0x32, R_GPIO44 = 0x44, R_GP45 = 0x45,
                     R_CHIPID1 = 0xFD, R_CHIPID2 = 0xFE;

// Board-mic gain. The references use 30 dB of analog PGA (reg14 = 0x1A, which
// also selects the analog mic input rather than a PDM mic), 24 dB of ADC
// digital scale (reg16 = 0x24) and 0 dB ADC volume (reg17 = 0xBF). See the
// ADC_SCALE comment for what this firmware changes.
static const uint8_t PGA_ANALOG_MIC_30DB = 0x1A;
// reg16 bits 2:0 are the ADC scale in 6 dB steps (Espressif's
// es8311_mic_gain_t: 0 = 0 dB ... 7 = 42 dB); 0x20 is the rest of the
// reference value, kept as it was.
static const uint8_t ADC_SCALE_BITS = 0x20;
static const uint8_t ADC_SCALE_STEP = 4;   // × 6 dB
static const uint8_t ADC_VOLUME_0DB = 0xBF;   // 0.5 dB steps, 0xBF = 0 dB
// Serial ports: bits 4:2 word length (4 = 32-bit), bits 1:0 format (1 = MSB /
// left-justified), bit 6 mute. Matches the engine's 32-bit MSB-justified
// slots, the combination AMY runs on this board.
static const uint8_t SDP_32BIT_MSB = 0x11;
static const uint8_t SDP_MUTE = 0x40;
static const uint8_t DAC_MUTE_BITS = 0x60;    // reg31 bits 6:5
static const uint8_t DAC_POWER_DOWN = 0x02;   // reg12
static const uint8_t ANALOG_POWER_DOWN = 0xFA;// reg0D, AMY's shutdown value

// Clock dividers for MCLK = CODEC_MCLK_MULTIPLE × fs, the reference table's
// rows for the rates this firmware may run at (same values at each rate).
struct Coeff {
  uint32_t mclk, rate;
  uint8_t preDiv, preMultCode, adcDiv, dacDiv, fsMode, lrckH, lrckL, bclkDiv, adcOsr, dacOsr;
};
static const Coeff COEFFS[] = {
  // mclk      rate   prediv mult adcdiv dacdiv fsmode lrch  lrcl  bckdiv adcosr dacosr
  { 4096000, 16000, 0x01, 0,   0x01,  0x01,  0x00,  0x00, 0xff, 0x04,  0x10,  0x10},
  { 8192000, 32000, 0x01, 0,   0x01,  0x01,  0x00,  0x00, 0xff, 0x04,  0x10,  0x10},
  {12288000, 48000, 0x01, 0,   0x01,  0x01,  0x00,  0x00, 0xff, 0x04,  0x10,  0x10},
};

// Why the last job failed ("ok" when it didn't), for the status page.
static char codecDetail[48] = "not started";

static bool codecFail(const char* what, uint8_t reg) {
  snprintf(codecDetail, sizeof(codecDetail), "%s (reg 0x%02X)", what, reg);
  return false;
}

static bool cw(uint8_t reg, uint8_t v) { return regWrite(ES8311_ADDR, reg, v) || codecFail("write failed", reg); }

// Read-modify-write: v = (v & keep) | set.
static bool crmw(uint8_t reg, uint8_t keep, uint8_t set) {
  uint8_t v;
  if (!regRead(ES8311_ADDR, reg, &v, 1)) return codecFail("read failed", reg);
  return cw(reg, (uint8_t)((v & keep) | set));
}

struct RegVal { uint8_t reg, val; };

static bool cwAll(const RegVal* rv, int n) {
  for (int i = 0; i < n; i++) if (!cw(rv[i].reg, rv[i].val)) return false;
  return true;
}

static bool codecInit() {
  const uint32_t mclk = (uint32_t)audio::SAMPLE_RATE * engine::CODEC_MCLK_MULTIPLE;
  const Coeff* k = nullptr;
  for (const Coeff& c : COEFFS) if (c.mclk == mclk && c.rate == (uint32_t)audio::SAMPLE_RATE) k = &c;
  if (!k) return codecFail("no clock dividers for this rate", 0);

  // The ES8311 can NACK its first transaction; the reference writes reg44
  // (I2C noise immunity) twice and trusts the second.
  regWrite(ES8311_ADDR, R_GPIO44, 0x08);
  if (!regWrite(ES8311_ADDR, R_GPIO44, 0x08)) return codecFail("no answer at 0x18", R_GPIO44);
  uint8_t id[2];
  if (!regRead(ES8311_ADDR, R_CHIPID1, &id[0], 1) || !regRead(ES8311_ADDR, R_CHIPID2, &id[1], 1))
    return codecFail("chip id read failed", R_CHIPID1);
  if (id[0] != 0x83 || id[1] != 0x11) {
    snprintf(codecDetail, sizeof(codecDetail), "chip id 0x%02X%02X, not 0x8311", id[0], id[1]);
    return false;
  }

  // Clock manager and power, the reference's open() sequence.
  static const RegVal OPEN[] = {
    {R_CLK01, 0x30}, {R_CLK02, 0x00}, {R_CLK03, 0x10},
    {R_ADC16, ADC_SCALE_BITS | ADC_SCALE_STEP},
    {R_CLK04, 0x10}, {R_CLK05, 0x00}, {R_SYS0B, 0x00}, {R_SYS0C, 0x00},
    {R_SYS10, 0x1F}, {R_SYS11, 0x7F},
    {R_RESET, 0x80},   // power up, state machine and clocks on
  };
  if (!cwAll(OPEN, sizeof(OPEN) / sizeof(OPEN[0]))) return false;
  if (!crmw(R_RESET, 0xBF, 0x00)) return false;   // bit 6 clear: I2S slave
  if (!cw(R_CLK01, 0x3F)) return false;           // all clocks on
  if (!crmw(R_CLK01, 0x7F, 0x00)) return false;   // bit 7 clear: MCLK from the MCLK pin

  // Dividers for this MCLK / rate.
  if (!crmw(R_CLK02, 0x07, (uint8_t)(((k->preDiv - 1) << 5) | (k->preMultCode << 3)))) return false;
  if (!cw(R_CLK05, (uint8_t)(((k->adcDiv - 1) << 4) | (k->dacDiv - 1)))) return false;
  if (!crmw(R_CLK03, 0x80, (uint8_t)((k->fsMode << 6) | k->adcOsr))) return false;
  if (!crmw(R_CLK04, 0x80, k->dacOsr)) return false;
  if (!crmw(R_CLK07, 0xC0, k->lrckH)) return false;
  if (!cw(R_CLK08, k->lrckL)) return false;
  if (!crmw(R_CLK06, 0xE0, (uint8_t)(k->bclkDiv < 19 ? k->bclkDiv - 1 : k->bclkDiv))) return false;
  if (!crmw(R_CLK01, (uint8_t)~0x40, 0x00)) return false;   // MCLK not inverted
  if (!crmw(R_CLK06, (uint8_t)~0x20, 0x00)) return false;   // BCLK not inverted

  // ADC path: high-pass stage 1, then equaliser bypass + digital DC removal.
  // Serial ports: ADC out unmuted in the engine's format; DAC in muted.
  // Then the reference's start() for the ADC, with the DAC held off.
  static const RegVal START[] = {
    {R_ADC1B, 0x0A}, {R_ADC1C, 0x6A},
    {R_SDPIN, SDP_MUTE | SDP_32BIT_MSB}, {R_SDPOUT, SDP_32BIT_MSB},
    {R_ADC17, ADC_VOLUME_0DB},
    {R_SYS0E, 0x02},              // analog PGA and ADC modulator on
    {R_SYS12, DAC_POWER_DOWN},
    {R_SYS14, PGA_ANALOG_MIC_30DB},
    {R_SYS0D, 0x01},              // analog circuitry on
    {R_ADC15, 0x40},              // ADC ramp rate
    {R_GP45, 0x00},
    {R_DAC32, 0x00},              // DAC volume to the bottom
  };
  if (!cwAll(START, sizeof(START) / sizeof(START[0]))) return false;
  if (!crmw(R_DAC31, 0xFF, DAC_MUTE_BITS)) return false;
  snprintf(codecDetail, sizeof(codecDetail), "ok");
  return true;
}

// Before MCLK stops (back to the array): ADC to the bottom and its port
// muted, DAC kept muted and off, analog blocks down (AMY's shutdown values).
static bool codecShutdown() {
  if (!cw(R_ADC17, 0x00)) return false;
  if (!crmw(R_SDPOUT, 0xFF, SDP_MUTE)) return false;
  if (!cw(R_DAC32, 0x00)) return false;
  if (!crmw(R_DAC31, 0xFF, DAC_MUTE_BITS)) return false;
  if (!cw(R_SYS12, DAC_POWER_DOWN)) return false;
  if (!cw(R_SYS0D, ANALOG_POWER_DOWN)) return false;
  snprintf(codecDetail, sizeof(codecDetail), "ok (powered down)");
  return true;
}

static void codecLoop() {
  uint32_t ticket;
  const engine::CodecJob j = engine::codecJob(&ticket);
  if (j == engine::CodecJob::None) return;
  // ~40 register accesses, a few ms at 400 kHz: the one blocking step loop()
  // takes, once per switch.
  const bool ok = j == engine::CodecJob::Init ? codecInit() : codecShutdown();
  engine::codecJobDone(ticket, ok);
}

// ---------- FT6336U touch ----------
static const uint8_t FT6336_ADDR = 0x38;
static const int PIN_TOUCH_RST = 18;
static const int PIN_TOUCH_INT = 17;          // not used for polling; held as the docs say
static const uint32_t TOUCH_RESET_LOW_MS = 10;   // FNK0104-BOARD.md: ≥10 ms low,
static const uint32_t TOUCH_BOOT_MS = 300;       // then ~300 ms to boot
static const uint32_t TOUCH_POLL_MS = 20;        // 50 Hz: taps feel instant, the bus stays idle
// Debounce: a finger counts as lifted only after this many empty polls in a
// row, so contact flicker at the start or end of a press can't make a second
// tap; and taps closer together than the lockout are one tap.
static const int TOUCH_RELEASE_POLLS = 3;
static const uint32_t TAP_LOCKOUT_MS = 250;
static const uint8_t FT_TD_STATUS = 0x02;     // then P1_XH, P1_XL, P1_YH, P1_YL

// Raw readings at the visible edges, measured on this board
// (amy-fnk-keyboard BOARD-CALIBRATION.md, src/common/pins.h). The fit maps
// onto the rotation-3 canvas, the orientation it was measured in: raw X is
// the short axis and drives canvas y; raw Y is the long axis and drives
// canvas x, backwards: Y_LO is at canvas x = 319. The fit is good to ~2 px
// except along the rotation-3 bottom edge (~20 px), which is the top edge at
// rotation 1: keep touch targets off it.
static const int TOUCH_RAW_X_LO = 21, TOUCH_RAW_X_HI = 224;
static const int TOUCH_RAW_Y_LO = 5,  TOUCH_RAW_Y_HI = 306;
static const int CANVAS_W = 320, CANVAS_H = 240;   // landscape, either rotation
// Rotations 1 and 3 are the two landscape ones, 180° apart: rotation-3 (x, y)
// is rotation-1 (319 − x, 239 − y). touchPoll() fits in rotation 3 and
// flips when the screen runs at 1.
static_assert(TFT_ROTATION == 1 || TFT_ROTATION == 3, "touch mapping supports landscape rotations 1 and 3");

enum TouchState : uint8_t { T_RESET_LOW, T_BOOTING, T_READY };
static TouchState tState = T_RESET_LOW;
static uint32_t tSince;        // when the current reset step began
static uint32_t tNextPoll;
static bool     tDown;         // a finger is on the glass (debounced)
static int      tEmptyPolls;   // consecutive polls without contact
static uint32_t tLastTap;      // millis() of the last tap, for the lockout
static bool     tapPending;    // takeTap() hasn't collected it yet
static int      tapX, tapY;
static bool     touchOk;
static uint32_t taps, touchErrors;

// One raw axis onto 0..outMax, rounded (amy-fnk-keyboard board.cpp map_axis).
static int mapAxis(int raw, int lo, int hi, int outMax) {
  const int span = hi - lo;
  if (span <= 0) return 0;
  const int v = ((raw - lo) * outMax + span / 2) / span;
  return v < 0 ? 0 : v > outMax ? outMax : v;
}

static void touchPoll(uint32_t now) {
  uint8_t b[5];
  if (!regRead(FT6336_ADDR, FT_TD_STATUS, b, sizeof(b))) {
    // No answer says nothing about the finger: keep the state as it was.
    touchOk = false;
    touchErrors++;
    return;
  }
  touchOk = true;
  const int n = b[0] & 0x0F;
  if (n < 1 || n > 2) {   // 0 = nothing; >2 is the controller's "invalid"
    if (++tEmptyPolls >= TOUCH_RELEASE_POLLS) tDown = false;
    return;
  }
  tEmptyPolls = 0;
  if (tDown) return;   // held: not a new tap
  tDown = true;
  const int rx = ((b[1] & 0x0F) << 8) | b[2];   // short axis
  const int ry = ((b[3] & 0x0F) << 8) | b[4];   // long axis
  if (taps && now - tLastTap < TAP_LOCKOUT_MS) return;
  tLastTap = now;
  taps++;
  // Rotation-3 canvas, as calibrated...
  tapX = (CANVAS_W - 1) - mapAxis(ry, TOUCH_RAW_Y_LO, TOUCH_RAW_Y_HI, CANVAS_W - 1);
  tapY = mapAxis(rx, TOUCH_RAW_X_LO, TOUCH_RAW_X_HI, CANVAS_H - 1);
  // ...then the screen's own orientation.
  if (TFT_ROTATION == 1) {
    tapX = (CANVAS_W - 1) - tapX;
    tapY = (CANVAS_H - 1) - tapY;
  }
  tapPending = true;
}

static void touchLoop(uint32_t now) {
  switch (tState) {
    case T_RESET_LOW:
      if (now - tSince < TOUCH_RESET_LOW_MS) return;
      digitalWrite(PIN_TOUCH_RST, HIGH);
      tState = T_BOOTING;
      tSince = now;
      return;
    case T_BOOTING:
      if (now - tSince < TOUCH_BOOT_MS) return;
      tState = T_READY;
      tNextPoll = now;
      return;
    case T_READY:
      if ((int32_t)(now - tNextPoll) < 0) return;
      tNextPoll = now + TOUCH_POLL_MS;
      touchPoll(now);
      return;
  }
}

// ---------- public ----------
void begin() {
  Wire.begin(PIN_I2C_SDA, PIN_I2C_SCL, I2C_HZ);
  Wire.setTimeOut(I2C_TIMEOUT_MS);
  // Reset pulse, finished from loop() so setup() doesn't wait 310 ms.
  pinMode(PIN_TOUCH_INT, INPUT_PULLUP);
  pinMode(PIN_TOUCH_RST, OUTPUT);
  digitalWrite(PIN_TOUCH_RST, LOW);
  tState = T_RESET_LOW;
  tSince = millis();
}

void loop(uint32_t now) {
  codecLoop();   // first: the capture task is draining DMA while it waits
  touchLoop(now);
}

bool takeTap(int* x, int* y) {
  if (!tapPending) return false;
  tapPending = false;
  *x = tapX;
  *y = tapY;
  return true;
}

Info info() {
  Info i;
  i.touchReady = tState == T_READY;
  i.touchOk = touchOk;
  i.taps = taps;
  i.touchErrors = touchErrors;
  i.codecDetail = codecDetail;
  return i;
}

}  // namespace board_io
