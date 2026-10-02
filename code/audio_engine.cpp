#include "audio_engine.h"

#include <Arduino.h>
#include <Preferences.h>
#include <math.h>
#include <string.h>
#include <driver/gpio.h>
#include <driver/i2s_std.h>
#include <esp_heap_caps.h>
#include <esp_rom_gpio.h>
#include <esp_timer.h>
#include <soc/gpio_sig_map.h>

namespace engine {

using namespace audio;

// ---------- pins (the board's only four free GPIOs, all on socket P3) ----------
static const gpio_num_t PIN_BCLK = GPIO_NUM_2;    // → SCK of all four mics
static const gpio_num_t PIN_WS   = GPIO_NUM_3;    // → WS of all four mics
static const gpio_num_t PIN_SD_A = GPIO_NUM_14;   // Mic 1 (L) + Mic 2 (R), read by I2S0
static const gpio_num_t PIN_SD_B = GPIO_NUM_21;   // Mic 3 (L) + Mic 4 (R), read by I2S1

// ---------- pins: the board mic's ES8311 (FNK0104-BOARD.md), inbuilt only ----------
// The codec's DAC input (GPIO8) is left unconnected, so the speaker path never
// gets data, and GPIO1 (the speaker amp enable) is never touched at all.
static const gpio_num_t PIN_CODEC_MCLK = GPIO_NUM_4;   // CODEC_MCLK_MULTIPLE × fs
static const gpio_num_t PIN_CODEC_BCLK = GPIO_NUM_5;
static const gpio_num_t PIN_CODEC_WS   = GPIO_NUM_7;
static const gpio_num_t PIN_CODEC_DIN  = GPIO_NUM_6;   // ← codec ASDOUT

// ---------- capture parameters ----------
// One DMA buffer = one hop, so each read pair is exactly one hop of audio.
static const int DMA_FRAMES = HOP;
static const int DMA_DESC = 6;
static const int SLOTS = DMA_FRAMES * 2;                 // stereo: L, R per frame
static const size_t BUF_BYTES = SLOTS * sizeof(int32_t);
static const uint32_t READ_TIMEOUT_MS = 100;             // a buffer takes 16 ms

// The slave samples the external BCLK with its own clock at bclk_div × BCLK.
// The IDF minimum (8, the default macro's value) can land data a BCLK late;
// a larger divider reduces that lag. 16 keeps the internal clock at 16 MHz
// for 16 kHz audio (49 MHz at 48 kHz), well inside the 160 MHz source.
static const uint32_t SLAVE_BCLK_DIV = 16;

// INMP441: zeros for 2^18 SCK cycles after the clock starts (256 ms at 16 kHz).
// Stopping BCLK for a resync may count as a power-down rather than standby,
// so every restart waits the full start-up time, plus ~40 ms of margin.
static const int WARMUP_FRAMES = (1 << 18) / 64 + SAMPLE_RATE * 40 / 1000;

// Alignment self-test: after re-routing a data pin, drop enough buffers to
// flush every DMA descriptor that might hold the old routing, then compare
// TEST_BUFS buffers from each controller.
static const int SETTLE_BUFS = DMA_DESC + 1;
static const int TEST_BUFS = 4;
static const int TEST_SLOTS = TEST_BUFS * SLOTS;
static const int TEST_MAX_OFFSET = SLOTS;   // search ±one DMA buffer

// A self-test that finds the slave misreading bits (NO MATCH, or every bit
// one early: BIT SHIFT) is retried on rebuilt channels. Both controllers'
// clocks are fractional divisions of the 160 MHz PLL, and the phase between
// them, picked afresh whenever the channels are created, decides where the
// slave samples each bit: on ~30% of builds every fourth bit or so lands on
// a transition. It holds until the channels are rebuilt, so a resync never
// clears it, but a rebuild draws again. Each attempt costs the ~300 ms
// warm-up.
static const int ALIGN_REBUILDS_MAX = 8;

// FIFO per controller between DMA reads and frame pairing: one buffer plus
// whatever the alignment drop leaves over (at most TEST_MAX_OFFSET + 1).
static const int FIFO_SLOTS = 4 * SLOTS;

// Frame ring, capture → processing: 8 hops (128 ms) of slack for the
// processing task. In PSRAM: it is touched once per sample on each side,
// ~64 KB/s, and internal RAM is better left to WiFi and the DSP.
static const int RING_FRAMES = 8 * HOP;

// Stream ring, processing → audio server: 32 hops (~0.5 s).
static const int STREAM_RECS = 32;

// Board mic start-up: after the codec init, drop this much before the frames
// count. The ES8311's ADC ramps its volume up (reg15) and its digital DC
// canceller has to settle; the reference drivers give no figure. Measured on
// this board with a /raw.wav armed during a switch: from the first 50 ms after
// it the noise floor is within 2 dB of its settled level, and what is left is
// a ~1000 LSB (−78 dBFS) DC step that the high-pass removes.
static const int CODEC_WARMUP_FRAMES = SAMPLE_RATE * 300 / 1000;
// loop() normally answers a codec job within a few ms; this long means it is
// stuck, and the capture task stops waiting rather than hang the switch.
static const uint32_t CODEC_JOB_TIMEOUT_MS = 2000;
// Switch: how long to let the processing task finish the whole hops of the
// old source (at most RING_FRAMES, ~128 ms of work) before the ring is reset.
static const uint32_t RING_DRAIN_TIMEOUT_MS = 500;
// An I2S channel that can't be created is retried at this interval rather
// than spun on at capture priority.
static const uint32_t HW_RETRY_MS = 1000;

static const int CAPTURE_PRIO = 6;   // above processing, which is above loopTask (1)
static const int PROCESS_PRIO = 3;
static const int CORE_AUDIO = 1;     // WiFi runs on core 0

// ---------- shared state ----------
static portMUX_TYPE mux = portMUX_INITIALIZER_UNLOCKED;   // guards st and pending
static Status st;                                          // published status

// ISR → capture task. Static globals live in internal DRAM, which
// CONFIG_I2S_ISR_IRAM_SAFE requires of the callback's user_ctx.
struct OvfFlag { volatile uint32_t count; volatile bool pending; };
static OvfFlag ovf[2];

// [0] I2S0 master, [1] I2S1 slave (array); the board mic uses [0] alone, on
// the codec pins. Null while torn down for a switch.
static i2s_chan_handle_t rx[2];
// Which of them are enabled, so teardown only disables running channels (the
// driver logs an error for the others, and only loop() may print).
static bool rxOn[2];
static int32_t* dmaCopy[2];                  // one buffer's worth of raw slots each
static int32_t* fifo[2];                     // raw slots awaiting pairing
static int      fifoLen[2];
static int      dropPending[2];              // slots still to discard for alignment
// Each controller's right slot from the previous frame (see pairFrames).
static int32_t  prevRight[2];
static int32_t* testBuf[2];                  // self-test capture (PSRAM)

static int32_t* ring;                        // RING_FRAMES × NUM_MICS, 24-bit samples
// Frame counters, free-running (the ring index is the count mod RING_FRAMES).
// Only a source switch moves ringW back, to ringR, to drop a partial hop.
static volatile uint32_t ringW, ringR;
static TaskHandle_t processTaskHandle;

static volatile bool resyncRequested;
static bool rebuildRequested;                // capture task: rebuild for a new self-test draw
static int  alignRebuildRun;                 // rebuilds since the last clean self-test

// Source. `src` is written only by the capture task (volatile so the audio
// server can check a raw capture's channel count against it); `srcTarget` by
// requestSource(). `ringSrc` tells the processing task what the ring's frames
// are: it changes only while the ring is empty, see ringReset().
static volatile Source src = Source::Array;
static volatile Source srcTarget = Source::Array;
static volatile Source ringSrc = Source::Array;
static bool hwUp;                            // capture task: the current source's channels exist

// Codec job mailbox, capture task ↔ loop(). Posted when jobTicket moves past
// jobDone; answered when jobDone catches up. jobKind is written before the
// ticket moves and jobOk before jobDone does, so each side reads a complete job.
static volatile uint32_t jobTicket, jobDone;
static volatile CodecJob jobKind;
static volatile bool jobOk;
static uint32_t jobDeadline;                 // capture task: millis() by which loop() must answer

// Raw capture, armed by the audio server, filled by the capture task.
static uint8_t* rawBuf;
static volatile uint32_t rawFrames, rawPos;
static volatile int rawCh;                   // channels per frame, fixed when armed
static volatile bool rawArmed;
static volatile bool rawAborted;             // the capture task dropped it (source switch)
static uint32_t rawResyncStart;

struct StreamRec { int16_t beam[HOP]; int16_t mic[NUM_MICS][HOP]; };
static StreamRec* streamRing;                // PSRAM
static volatile uint32_t streamHeadSeq;      // records completed

// Controls from loop()/web handlers, applied by the processing task at the
// start of a hop so the Processor is only ever touched by one task.
struct Pending {
  bool mode, angle, cal;
  proc::Mode m;
  float a;
  Calibration c;
};
static Pending pending;

static proc::Processor processor;
static proc::MonoConditioner boardMic;       // processing task: the inbuilt source's high-pass
static Calibration calCurrent = DEFAULT_CALIBRATION;   // what loop() sees; processor has its copy
static volatile bool calDirty;               // save calCurrent to NVS from loop()

// Gain calibration (processing task).
static volatile int gainCalHopsLeft;
static double gainCalSum[NUM_MICS];
static int gainCalHops;

// ---------- calibration persistence ----------
static const char* NVS_NS = "earwing";     // shared with main.cpp's credentials
static const uint8_t CAL_VERSION = 1;

static void calLoad() {
  Preferences p;
  p.begin(NVS_NS, true);
  uint8_t blob[1 + sizeof(Calibration)];
  if (p.getBytes("cal", blob, sizeof(blob)) == sizeof(blob) && blob[0] == CAL_VERSION)
    memcpy(&calCurrent, blob + 1, sizeof(Calibration));
  p.end();
}

static void calSave(const Calibration& c) {
  Preferences p;
  p.begin(NVS_NS, false);
  uint8_t blob[1 + sizeof(Calibration)];
  blob[0] = CAL_VERSION;
  memcpy(blob + 1, &c, sizeof(Calibration));
  p.putBytes("cal", blob, sizeof(blob));
  p.end();
}

// ---------- I2S ----------
// GDMA interrupt context: flag and count only.
static bool IRAM_ATTR onRecvOverflow(i2s_chan_handle_t, i2s_event_data_t*, void* ctx) {
  OvfFlag* f = (OvfFlag*)ctx;
  f->count = f->count + 1;
  f->pending = true;
  return false;
}

static void rxEnable(int c)  { if (rx[c] && !rxOn[c]) rxOn[c] = i2s_channel_enable(rx[c]) == ESP_OK; }
static void rxDisable(int c) { if (rx[c] && rxOn[c]) { i2s_channel_disable(rx[c]); rxOn[c] = false; } }

// Delete whatever channels exist, disabling running ones first. Deleting
// frees the controller and unreserves the pins, but the GPIO matrix keeps
// routing the old output signals to them: the next user of I2S0 would also
// drive these pads. Reset the clock outputs to plain, undriven GPIOs so each
// source's pads are quiet while the other runs. Data inputs are left alone:
// the array's keep their pull-downs, and an input the codec drives needs nothing.
static void channelsDelete(const gpio_num_t* outPins, int nOut) {
  for (int c = 0; c < 2; c++) {
    if (!rx[c]) continue;
    rxDisable(c);
    i2s_del_channel(rx[c]);
    rx[c] = nullptr;
  }
  for (int i = 0; i < nOut; i++) gpio_reset_pin(outPins[i]);
}

static const gpio_num_t ARRAY_OUT_PINS[] = {PIN_BCLK, PIN_WS};
static const gpio_num_t CODEC_OUT_PINS[] = {PIN_CODEC_MCLK, PIN_CODEC_BCLK, PIN_CODEC_WS};

static void arrayDelete() {
  // Normal routing first, in case we stopped mid self-test; the next rebuild
  // sets it again anyway.
  esp_rom_gpio_connect_in_signal(PIN_SD_B, I2S1I_SD_IN_IDX, false);
  channelsDelete(ARRAY_OUT_PINS, 2);
}

static bool i2sInitArray() {
  // Both controllers RX-only. I2S0 is the master and drives BCLK/WS on
  // GPIO2/3; I2S1 is a slave whose BCLK/WS inputs come from the same pads.
  // The slave's pin setup only enables the pad input and connects the input
  // signal, so the master's output routing survives either init order.
  i2s_chan_config_t cm = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
  cm.dma_desc_num = DMA_DESC;
  cm.dma_frame_num = DMA_FRAMES;
  i2s_chan_config_t cs = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_1, I2S_ROLE_SLAVE);
  cs.dma_desc_num = DMA_DESC;
  cs.dma_frame_num = DMA_FRAMES;
  if (i2s_new_channel(&cm, NULL, &rx[0]) != ESP_OK) return false;
  if (i2s_new_channel(&cs, NULL, &rx[1]) != ESP_OK) return false;

  // INMP441: 24-bit two's complement, MSB first, in 32-bit Philips slots.
  i2s_std_config_t cfg = {
    .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(SAMPLE_RATE),
    .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_32BIT, I2S_SLOT_MODE_STEREO),
    .gpio_cfg = {
      .mclk = I2S_GPIO_UNUSED,
      .bclk = PIN_BCLK,
      .ws = PIN_WS,
      .dout = I2S_GPIO_UNUSED,
      .din = PIN_SD_A,
      .invert_flags = {false, false, false},
    },
  };
  if (i2s_channel_init_std_mode(rx[0], &cfg) != ESP_OK) return false;
  cfg.clk_cfg.bclk_div = SLAVE_BCLK_DIV;
  cfg.gpio_cfg.din = PIN_SD_B;
  if (i2s_channel_init_std_mode(rx[1], &cfg) != ESP_OK) return false;

  // The INMP441 tri-states SD after its LSB and its datasheet asks for a
  // pull-down; the wiring has none, so use the pads' own. With it the low
  // byte of every raw slot reads 0, which the capture task checks.
  gpio_pulldown_en(PIN_SD_A);
  gpio_pulldown_en(PIN_SD_B);

  i2s_event_callbacks_t cb = {};
  cb.on_recv_q_ovf = onRecvOverflow;
  if (i2s_channel_register_event_callback(rx[0], &cb, &ovf[0]) != ESP_OK) return false;
  if (i2s_channel_register_event_callback(rx[1], &cb, &ovf[1]) != ESP_OK) return false;
  return true;
}

// The array, built exactly as at boot; a half-built one is taken down again.
static bool i2sInit() {
  if (i2sInitArray()) return true;
  arrayDelete();
  return false;
}

// The board mic: one master RX channel on I2S0 with the codec's pins. It
// supplies MCLK, which the ES8311 (an I2S slave clocked from its MCLK pin)
// needs before it can be configured. 32-bit MSB-justified slots are the
// format the AMY driver proved on this board (FNK0104-BOARD.md), and board_io
// programs the codec's serial port to match. Stereo slots, so BCLK is 64 × fs
// like the array's; the codec is mono and the left slot carries its ADC.
static bool codecChannelInit() {
  i2s_chan_config_t cc = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
  cc.dma_desc_num = DMA_DESC;
  cc.dma_frame_num = DMA_FRAMES;
  if (i2s_new_channel(&cc, NULL, &rx[0]) != ESP_OK) return false;
  i2s_std_config_t cfg = {
    .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(SAMPLE_RATE),
    .slot_cfg = I2S_STD_MSB_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_32BIT, I2S_SLOT_MODE_STEREO),
    .gpio_cfg = {
      .mclk = PIN_CODEC_MCLK,
      .bclk = PIN_CODEC_BCLK,
      .ws = PIN_CODEC_WS,
      .dout = I2S_GPIO_UNUSED,
      .din = PIN_CODEC_DIN,
      .invert_flags = {false, false, false},
    },
  };
  static_assert(CODEC_MCLK_MULTIPLE == 256, "clk_cfg.mclk_multiple below must follow it");
  cfg.clk_cfg.mclk_multiple = I2S_MCLK_MULTIPLE_256;
  if (i2s_channel_init_std_mode(rx[0], &cfg) != ESP_OK) return false;
  i2s_event_callbacks_t cb = {};
  cb.on_recv_q_ovf = onRecvOverflow;
  if (i2s_channel_register_event_callback(rx[0], &cb, &ovf[0]) != ESP_OK) return false;
  rxEnable(0);
  return rxOn[0];
}

static bool codecChannelStart() {
  if (codecChannelInit()) return true;
  channelsDelete(CODEC_OUT_PINS, 3);
  return false;
}

// ---------- capture task ----------
// Array:   WARMUP → TEST_SETTLE → TEST_COLLECT → TEST_RESTORE → RUN
// Inbuilt: CODEC_WAIT → WARMUP → RUN, then CODEC_STOP before a switch away;
//          CODEC_OFF when the codec isn't configured (init failed, or shut down).
enum Phase : uint8_t { WARMUP, TEST_SETTLE, TEST_COLLECT, TEST_RESTORE, RUN, CODEC_WAIT, CODEC_STOP, CODEC_OFF };
static Phase phase;
static int phaseCount;   // WARMUP: frames left; SETTLE/RESTORE: buffers left; COLLECT: slots so far

static void startStreams() {
  // Slave first, so it is already waiting when the master's enable starts
  // BCLK/WS. The alignment self-test still measures the result rather than
  // trusting this order.
  rxEnable(1);
  rxEnable(0);
  for (int c = 0; c < 2; c++) {
    ovf[c].pending = false;
    fifoLen[c] = 0;
    dropPending[c] = 0;
    prevRight[c] = 0;
    st.frames[c] = 0;
  }
  st.running = false;
  phase = WARMUP;
  phaseCount = WARMUP_FRAMES;
}

static void resync() {
  rxDisable(0);
  rxDisable(1);
  // Restore normal routing in case we stopped mid self-test.
  esp_rom_gpio_connect_in_signal(PIN_SD_B, I2S1I_SD_IN_IDX, false);
  st.resyncs++;
  startStreams();
}

// ---- raw capture, capture-task side ----
// The capture task is the only writer, so when it drops a capture itself no
// write can be in flight: rawArmed goes false first, then rawAborted tells the
// audio server it may free the buffer.
static void rawDrop() {
  if (!rawArmed) return;
  rawArmed = false;
  __sync_synchronize();
  rawAborted = true;
}

// One frame of `ch` 24-bit samples. A capture armed for another channel
// count (it raced a switch) is dropped rather than filled with the wrong layout.
static void rawPut(const int32_t* s, int ch) {
  if (!rawArmed || rawPos >= rawFrames) return;
  if (ch != rawCh) { rawDrop(); return; }
  uint8_t* o = rawBuf + (size_t)rawPos * ch * 3;
  for (int c = 0; c < ch; c++) {
    o[3 * c] = (uint8_t)s[c];
    o[3 * c + 1] = (uint8_t)(s[c] >> 8);
    o[3 * c + 2] = (uint8_t)(s[c] >> 16);
  }
  rawPos = rawPos + 1;
}

// Hand n frames written at ringW onward to the processing task.
static void ringCommit(int n) {
  __sync_synchronize();   // samples visible before the counter moves
  ringW = ringW + n;
  if (ringW - ringR >= (uint32_t)HOP) xTaskNotifyGive(processTaskHandle);
}

static bool sourceStart();

static void selfTestEvaluate() {
  sync_check::Result r = sync_check::analyze(testBuf[0], testBuf[1], TEST_SLOTS, TEST_MAX_OFFSET);
  // A bad clock draw is rebuilt (up to a point) and the next test's result
  // is the one shown, so the screen says "warm-up" rather than flashing a fault.
  const bool rebuild = (r.status == sync_check::Status::NoMatch || r.status == sync_check::Status::BitShift) &&
                       alignRebuildRun < ALIGN_REBUILDS_MAX;
  portENTER_CRITICAL(&mux);
  if (!rebuild) st.align = r;
  st.alignRuns++;
  if (r.status == sync_check::Status::Ok) {
    if (st.alignConclusive == 0) st.alignFirstOffset = r.slotOffset;
    else if (r.slotOffset != st.alignFirstOffset) st.alignChanges++;
    st.alignConclusive++;
  }
  portEXIT_CRITICAL(&mux);
  // Only a clean match is corrected; what's left after the rebuilds runs
  // uncorrected and is reported, because a guessed correction would hide the fault.
  if (rebuild) {
    alignRebuildRun++;
    rebuildRequested = true;
  } else if (r.status == sync_check::Status::Ok) {
    dropPending[0] = r.dropMaster;
    dropPending[1] = r.dropSlave;
    alignRebuildRun = 0;
  }
}

// Append a DMA buffer to a controller's FIFO, discarding alignment drops first.
static void fifoAppend(int c, const int32_t* src, int n) {
  const int d = dropPending[c] < n ? dropPending[c] : n;
  dropPending[c] -= d;
  src += d;
  n -= d;
  if (fifoLen[c] + n > FIFO_SLOTS) {   // can't happen unless pairing broke: start over
    fifoLen[c] = 0;
    st.mismatches++;
    resyncRequested = true;
  }
  memcpy(fifo[c] + fifoLen[c], src, n * sizeof(int32_t));
  fifoLen[c] += n;
}

static void pairFrames() {
  if (dropPending[0] || dropPending[1]) return;   // still aligning
  int n = (fifoLen[0] < fifoLen[1] ? fifoLen[0] : fifoLen[1]) / 2;
  if (n <= 0) return;

  // The ring can drop frames when processing falls behind (whole frames,
  // never single channels); the raw capture never does, so timing analysis
  // of a /raw.wav stays valid even then.
  const bool room = ringW - ringR + n <= (uint32_t)RING_FRAMES;
  if (!room) st.ringDrops += n;
  for (int f = 0; f < n; f++) {
    // The right slot of a frame holds a sample taken one frame later than
    // the left slot's: clap tests from both ends of the array (same spot,
    // array rotated 180°) put Mic 2 and Mic 4 1.00 ± 0.02 samples early
    // against Mic 1 and Mic 3, on both controllers alike. So the right slot
    // is held back one frame: frame f−1's right goes with frame f's left,
    // and the first frame after a restart gets a zero there. The alignment
    // self-test can't see this, since it only compares controller against
    // controller.
    // 24-bit MSB-aligned samples: arithmetic shift keeps the sign.
    int32_t s[NUM_MICS] = {
      fifo[0][2 * f] >> 8,       // I2S0 left:  Mic 1
      prevRight[0],              // I2S0 right: Mic 2
      fifo[1][2 * f] >> 8,       // I2S1 left:  Mic 3
      prevRight[1],              // I2S1 right: Mic 4
    };
    prevRight[0] = fifo[0][2 * f + 1] >> 8;
    prevRight[1] = fifo[1][2 * f + 1] >> 8;
    if (room) memcpy(ring + ((ringW + f) % RING_FRAMES) * NUM_MICS, s, sizeof(s));
    rawPut(s, NUM_MICS);
  }
  if (room) ringCommit(n);
  for (int c = 0; c < 2; c++) {
    fifoLen[c] -= 2 * n;
    memmove(fifo[c], fifo[c] + 2 * n, fifoLen[c] * sizeof(int32_t));
  }
}

// One lockstep read of both array controllers, then the phase machine.
static void arrayStep() {
  if (rebuildRequested) {
    rebuildRequested = false;
    resyncRequested = false;
    arrayDelete();
    portENTER_CRITICAL(&mux);
    st.alignRebuilds++;
    portEXIT_CRITICAL(&mux);
    hwUp = sourceStart();   // captureTask retries if the channels can't be made
    if (!hwUp) st.hwFaults++;
    return;
  }
  if (resyncRequested || ovf[0].pending || ovf[1].pending) {
    resyncRequested = false;
    resync();
  }

  size_t got[2] = {0, 0};
  esp_err_t e0 = i2s_channel_read(rx[0], dmaCopy[0], BUF_BYTES, &got[0], READ_TIMEOUT_MS);
  esp_err_t e1 = i2s_channel_read(rx[1], dmaCopy[1], BUF_BYTES, &got[1], READ_TIMEOUT_MS);
  // Always whole buffers: i2s_channel_read abandons a partly read buffer
  // when its queue is nearly full, which would silently slip one stream.
  bool bad = false;
  for (int c = 0; c < 2; c++) {
    if ((c ? e1 : e0) != ESP_OK || got[c] != BUF_BYTES) { st.timeouts[c]++; bad = true; }
    st.frames[c] += got[c] / 8;
  }
  if (got[0] != got[1]) st.mismatches++;
  if (bad) { resyncRequested = true; return; }
  if (ovf[0].pending || ovf[1].pending) return;   // this data may already be slipped
  st.overflows[0] = ovf[0].count;
  st.overflows[1] = ovf[1].count;

  for (int c = 0; c < 2; c++)
    for (int i = 0; i < SLOTS; i++)
      if (dmaCopy[c][i] & 0xFF) st.lowByteNonzero[c]++;

  switch (phase) {
    case WARMUP:
      phaseCount -= DMA_FRAMES;
      if (phaseCount <= 0) {
        // Feed GPIO14 (Mics 1/2) to I2S1 too: both controllers now read
        // the same line, so their streams must match word for word.
        esp_rom_gpio_connect_in_signal(PIN_SD_A, I2S1I_SD_IN_IDX, false);
        phase = TEST_SETTLE;
        phaseCount = SETTLE_BUFS;
      }
      break;
    case TEST_SETTLE:
      if (--phaseCount <= 0) { phase = TEST_COLLECT; phaseCount = 0; }
      break;
    case TEST_COLLECT:
      memcpy(testBuf[0] + phaseCount, dmaCopy[0], BUF_BYTES);
      memcpy(testBuf[1] + phaseCount, dmaCopy[1], BUF_BYTES);
      phaseCount += SLOTS;
      if (phaseCount >= TEST_SLOTS) {
        esp_rom_gpio_connect_in_signal(PIN_SD_B, I2S1I_SD_IN_IDX, false);
        selfTestEvaluate();
        phase = TEST_RESTORE;
        phaseCount = SETTLE_BUFS;
      }
      break;
    case TEST_RESTORE:
      if (--phaseCount <= 0) { phase = RUN; st.running = true; }
      break;
    case RUN:
      fifoAppend(0, dmaCopy[0], SLOTS);
      fifoAppend(1, dmaCopy[1], SLOTS);
      pairFrames();
      break;
    default:
      break;
  }
}

// ---- board mic ----
static void codecJobPost(CodecJob j) {
  jobKind = j;
  __sync_synchronize();
  jobTicket = jobTicket + 1;
  jobDeadline = millis() + CODEC_JOB_TIMEOUT_MS;
}

// True once loop() has answered (or the wait timed out, which counts as a
// failure and withdraws the job so a late loop() doesn't run it after all).
static bool codecJobAnswered(bool* ok) {
  if (jobDone == jobTicket) { __sync_synchronize(); *ok = jobOk; return true; }
  if ((int32_t)(millis() - jobDeadline) < 0) return false;
  jobDone = jobTicket;
  *ok = false;
  return true;
}

static void codecStartUp() {
  ovf[0].pending = false;
  st.frames[0] = st.frames[1] = 0;
  st.running = false;
  portENTER_CRITICAL(&mux);
  st.codecInits++;
  portEXIT_CRITICAL(&mux);
  codecJobPost(CodecJob::Init);
  phase = CODEC_WAIT;
}

// A fault restart of the codec channel. The ES8311 keeps its registers while
// MCLK pauses, so this is warm-up again, not another init. A codec job in
// flight is left to finish: its phase doesn't change.
static void boardResync() {
  rxDisable(0);
  rxEnable(0);
  ovf[0].pending = false;
  st.resyncs++;
  st.frames[0] = 0;
  if (phase == WARMUP || phase == RUN) {
    st.running = false;
    phase = WARMUP;
    phaseCount = CODEC_WARMUP_FRAMES;
  }
}

static void pushBoardFrames(const int32_t* slots) {
  const int n = DMA_FRAMES;
  const bool room = ringW - ringR + n <= (uint32_t)RING_FRAMES;
  if (!room) st.ringDrops += n;
  for (int f = 0; f < n; f++) {
    // Left slot, MSB-justified 24-bit like the INMP441s: same shift, same
    // full scale. The ring stays NUM_MICS wide; the other channels are zero
    // and the processing task (told by ringSrc) never reads them.
    const int32_t s[NUM_MICS] = {slots[2 * f] >> 8, 0, 0, 0};
    if (room) memcpy(ring + ((ringW + f) % RING_FRAMES) * NUM_MICS, s, sizeof(s));
    rawPut(s, 1);
  }
  if (room) ringCommit(n);
}

static void boardStep() {
  if (resyncRequested || ovf[0].pending) {
    resyncRequested = false;
    boardResync();
  }
  size_t got = 0;
  const esp_err_t e = i2s_channel_read(rx[0], dmaCopy[0], BUF_BYTES, &got, READ_TIMEOUT_MS);
  st.frames[0] += got / 8;
  if (e != ESP_OK || got != BUF_BYTES) { st.timeouts[0]++; resyncRequested = true; return; }
  if (ovf[0].pending) return;
  st.overflows[0] = ovf[0].count;

  bool ok;
  switch (phase) {
    case CODEC_WAIT:   // the frames so far predate the codec's configuration
      if (!codecJobAnswered(&ok)) break;
      portENTER_CRITICAL(&mux);
      st.codecInit = ok ? CodecInit::Ok : CodecInit::Failed;
      st.codecPowered = ok;
      if (!ok) st.codecFailures++;
      portEXIT_CRITICAL(&mux);
      if (ok) { phase = WARMUP; phaseCount = CODEC_WARMUP_FRAMES; }
      else phase = CODEC_OFF;   // nothing to capture; the status says why
      break;
    case WARMUP:
      phaseCount -= DMA_FRAMES;
      if (phaseCount <= 0) { phase = RUN; st.running = true; }
      break;
    case RUN:
      pushBoardFrames(dmaCopy[0]);
      break;
    case CODEC_STOP:
      if (!codecJobAnswered(&ok)) break;
      portENTER_CRITICAL(&mux);
      st.codecPowered = false;   // even if the shutdown failed: MCLK is about to stop
      portEXIT_CRITICAL(&mux);
      phase = CODEC_OFF;
      // Asked back to inbuilt while shutting down: no switch will happen, so
      // bring the codec up again instead of sitting here powered down.
      if (srcTarget == Source::Inbuilt) codecStartUp();
      break;
    default:
      break;
  }
}

// ---- switching ----
// Hand the processing task a clean boundary between sources: let it finish
// the whole hops already queued, then drop the partial hop left over. It only
// reads once HOP frames are available, so while fewer are queued it isn't
// touching them and moving ringW back to ringR is safe. ringSrc changes here,
// with the ring empty, so every hop the processing task sees is one source's.
static void ringReset(Source next) {
  const uint32_t t0 = millis();
  while (ringW - ringR >= (uint32_t)HOP && millis() - t0 < RING_DRAIN_TIMEOUT_MS) vTaskDelay(1);
  // After a timeout (processing stalled) the stale hops are dropped with the rest.
  ringW = ringR;
  __sync_synchronize();
  ringSrc = next;
}

// Build the current source's hardware and start its phase machine.
static bool sourceStart() {
  if (src == Source::Array) {
    if (!i2sInit()) return false;
    startStreams();   // warm-up and the alignment self-test, exactly as at boot
  } else {
    if (!codecChannelStart()) return false;
    codecStartUp();
  }
  return true;
}

// Can the capture task tear down now? The array always can. The board mic
// first answers any codec job it has posted, and powers the codec down while
// MCLK still runs (posting that job here).
static bool readyToSwitch() {
  if (src == Source::Array || !hwUp) return true;
  switch (phase) {
    case CODEC_OFF:  return true;
    case CODEC_WAIT: // init still in flight: let it finish, then shut down
    case CODEC_STOP: return false;
    default:
      codecJobPost(CodecJob::Shutdown);
      phase = CODEC_STOP;
      return false;
  }
}

static void switchSource() {
  const Source to = srcTarget;
  if (hwUp) {
    if (src == Source::Array) arrayDelete();
    else channelsDelete(CODEC_OUT_PINS, 3);
  }
  hwUp = false;
  st.running = false;
  rawDrop();   // a raw capture holds one source's frames only
  ringReset(to);
  src = to;
  portENTER_CRITICAL(&mux);
  st.source = to;
  st.sourceSwitches++;
  portEXIT_CRITICAL(&mux);
  hwUp = sourceStart();
  if (!hwUp) st.hwFaults++;
}

static void captureTask(void*) {
  startStreams();   // begin() built the array
  hwUp = true;
  for (;;) {
    if (srcTarget != src && readyToSwitch()) { switchSource(); continue; }
    if (!hwUp) {
      // Channels couldn't be created. Nothing to read; retry calmly rather
      // than spin at capture priority.
      vTaskDelay(pdMS_TO_TICKS(HW_RETRY_MS));
      hwUp = sourceStart();
      if (!hwUp) st.hwFaults++;
      continue;
    }
    if (src == Source::Array) arrayStep();
    else boardStep();
  }
}

// ---------- processing task ----------
static float* hopIn;        // HOP × NUM_MICS, full scale ±1
static float* hopBeam;      // HOP
static float* hopMics;      // NUM_MICS × HOP, geometry rows

static int16_t toPcm16(float x, float gain) {
  float v = x * gain * 32768.0f;
  if (v > 32767.0f) { st.clipped++; return 32767; }
  if (v < -32768.0f) { st.clipped++; return -32768; }
  return (int16_t)lrintf(v);
}

// Gain calibration: at the end of the run, set the trims so every channel
// matches the mean level (processing task).
static void gainCalFinish() {
  Calibration c = processor.calibration();
  double meanDb = 0;
  bool dead = false;
  for (int m = 0; m < NUM_MICS; m++) {
    const double db = 10.0 * log10(gainCalSum[m] / gainCalHops + 1e-20);
    if (db < -100.0) dead = true;
    meanDb += db / NUM_MICS;
    c.gainDb[m] = (float)db;   // temporarily the measured level
  }
  for (int m = 0; m < NUM_MICS; m++) c.gainDb[m] = (float)(meanDb - c.gainDb[m]);
  bool inRange = true;
  for (int m = 0; m < NUM_MICS; m++) if (fabsf(c.gainDb[m]) > GAIN_TRIM_MAX_DB) inRange = false;
  if (!dead && inRange) {
    processor.setCalibration(c);
    portENTER_CRITICAL(&mux);
    calCurrent = processor.calibration();
    st.gainCal = 2;
    portEXIT_CRITICAL(&mux);
    calDirty = true;
  } else {
    st.gainCal = -1;   // a dead or wildly different channel is a wiring fault, not a trim
  }
}

static void processTask(void*) {
  const float streamGain = powf(10.0f, STREAM_GAIN_DB / 20.0f);
  const float hopMs = 1000.0f * HOP / SAMPLE_RATE;
  const int hopsPerSec = (int)lroundf(1000.0f / hopMs);
  // ~100 ms exponential averaging for displayed levels; peak meters fall 20 dB/s.
  const float lvlAlpha = hopMs / 100.0f;
  const float peakFallDb = 20.0f * hopMs / 1000.0f;
  float micPow[NUM_MICS] = {}, beamPow = 0, peakDb[NUM_MICS];
  for (auto& p : peakDb) p = -120.0f;
  float winSum = 0, winMax = 0;
  int winN = 0;
  proc::HopResult r;
  Source lastSrc = Source::Array;   // to restart the board mic's filter on each switch to it

  for (;;) {
    ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(50));
    while (ringW - ringR >= (uint32_t)HOP) {
      // Read after the availability check: ringSrc only changes while the
      // ring is empty (ringReset), so it describes every frame of this hop.
      const Source hopSrc = ringSrc;
      const bool board = hopSrc == Source::Inbuilt;
      const int live = sourceChannels(hopSrc);
      const uint16_t depth = (uint16_t)((ringW - ringR) / HOP);
      for (int f = 0; f < HOP; f++) {
        const int32_t* src = ring + ((ringR + f) % RING_FRAMES) * NUM_MICS;
        for (int c = 0; c < NUM_MICS; c++) hopIn[f * NUM_MICS + c] = src[c] * (1.0f / 8388608.0f);
      }
      __sync_synchronize();   // done reading before the slots are handed back
      ringR = ringR + HOP;

      // Controls apply in either source: while the board mic plays, the
      // Processor sits idle but keeps the user's mode and angle for the
      // array's return.
      Pending p;
      portENTER_CRITICAL(&mux);
      p = pending;
      pending.mode = pending.angle = pending.cal = false;
      portEXIT_CRITICAL(&mux);
      if (p.cal) processor.setCalibration(p.c);
      if (p.angle) processor.setFixedAngle(p.a);
      if (p.mode) processor.setMode(p.m);

      if (hopSrc != lastSrc) {
        // A new source: its meters start from silence rather than decaying
        // from the other source's levels, and the board mic's filter has no
        // state from a previous session.
        for (int c = 0; c < NUM_MICS; c++) { micPow[c] = 0; peakDb[c] = -120.0f; }
        beamPow = 0;
        if (board) boardMic.init();
        lastSrc = hopSrc;
      }

      StreamRec* rec = streamRing + (streamHeadSeq % STREAM_RECS);
      float us, micHopDb[NUM_MICS], beamHopDb;
      const int64_t t0 = esp_timer_get_time();
      if (!board) {
        processor.process(hopIn, hopBeam, hopMics, &r);
        us = (float)(esp_timer_get_time() - t0);
        memcpy(micHopDb, r.micDb, sizeof(micHopDb));
        beamHopDb = r.beamDb;
        // Stream record: beam + the four delayed raw mics, same fixed gain.
        for (int n = 0; n < HOP; n++) rec->beam[n] = toPcm16(hopBeam[n], streamGain);
        for (int m = 0; m < NUM_MICS; m++)
          for (int n = 0; n < HOP; n++) rec->mic[m][n] = toPcm16(hopMics[m * HOP + n], streamGain);
      } else {
        // The board mic, channel 0 of the ring. Conditioned in hopBeam; no
        // DOA, no beam, nothing spatial to report.
        for (int n = 0; n < HOP; n++) hopBeam[n] = hopIn[n * NUM_MICS];
        beamHopDb = boardMic.process(hopBeam, hopBeam);
        us = (float)(esp_timer_get_time() - t0);
        micHopDb[0] = beamHopDb;
        // Every stream (beam and ?mic=N) is the board mic, at the array's
        // stream gain, so /beam.wav keeps working and A/B levels compare.
        for (int n = 0; n < HOP; n++) rec->beam[n] = toPcm16(hopBeam[n], streamGain);
        for (int m = 0; m < NUM_MICS; m++) memcpy(rec->mic[m], rec->beam, sizeof(rec->beam));
      }
      __sync_synchronize();
      streamHeadSeq = streamHeadSeq + 1;

      // Gain calibration: mean power per channel before the trim. It needs
      // the array; a switch to the board mic cancels it.
      if (gainCalHopsLeft > 0 && board) {
        gainCalHopsLeft = 0;
        st.gainCal = -1;
      } else if (gainCalHopsLeft > 0) {
        for (int c = 0; c < NUM_MICS; c++) gainCalSum[c] += pow(10.0, r.micPreTrimDb[c] / 10.0);
        gainCalHops++;
        gainCalHopsLeft = gainCalHopsLeft - 1;
        if (gainCalHopsLeft == 0) gainCalFinish();
      }

      // Stats. Channels that aren't live read as silence; the UI hides them.
      for (int c = 0; c < NUM_MICS; c++) {
        if (c >= live) { micPow[c] = 0; peakDb[c] = -120.0f; continue; }
        micPow[c] += lvlAlpha * (powf(10.0f, micHopDb[c] / 10.0f) - micPow[c]);
        float pk = 0;
        for (int n = 0; n < HOP; n++) pk = fmaxf(pk, fabsf(hopIn[n * NUM_MICS + c]));
        const float pkDb = 20.0f * log10f(pk + 1e-9f);
        peakDb[c] = fmaxf(pkDb, peakDb[c] - peakFallDb);
      }
      beamPow += lvlAlpha * (powf(10.0f, beamHopDb / 10.0f) - beamPow);
      winSum += us;
      winMax = fmaxf(winMax, us);
      winN++;

      portENTER_CRITICAL(&mux);
      st.ringDepth = depth;
      if (depth > st.ringHigh) st.ringHigh = depth;
      st.mode = processor.mode();
      st.fixedDeg = processor.fixedAngle();
      if (!board) {
        if (r.spectral) { st.doaDeg = r.doa.thetaDeg; st.confidence = r.doa.confidence; }
        st.smoothDeg = r.smoothDeg;
        st.smoothValid = r.smoothValid;
        st.speech = r.speech;
        st.steerDeg = r.steerDeg;
        if (r.leadsUpdated) {
          memcpy(st.lead, r.lead, sizeof(st.lead));
          memcpy(st.leadCoh, r.leadCoh, sizeof(st.leadCoh));
          st.leadMs = millis() | 1;
        }
      } else {
        st.speech = false;   // no gate runs on the board mic
      }
      for (int c = 0; c < NUM_MICS; c++) {
        st.micDb[c] = 10.0f * log10f(micPow[c] + 1e-20f);
        st.micPeakDb[c] = peakDb[c];
      }
      st.beamDb = 10.0f * log10f(beamPow + 1e-20f);
      st.hops++;
      if (us > st.dspPeakUs) st.dspPeakUs = us;
      if (winN >= hopsPerSec) {
        st.dspMeanUs = winSum / winN;
        st.dspMaxUs = winMax;
        winSum = winMax = 0;
        winN = 0;
      }
      portEXIT_CRITICAL(&mux);
    }
  }
}

// ---------- boot check: esp-dsp against the portable FFT ----------
static void fftSelfCheck() {
  dsp::Fft esp, ref;
  float* a = (float*)dsp::alloc(sizeof(float) * 2 * FFT_N);
  float* b = (float*)dsp::alloc(sizeof(float) * 2 * FFT_N);
  if (!a || !b || !esp.init(FFT_N, dsp::FftBackend::EspDsp) || !ref.init(FFT_N, dsp::FftBackend::Portable)) {
    st.fftMaxErr = -1;
    return;
  }
  uint32_t seed = 1;   // fixed input: an LCG, so every boot checks the same data
  for (int i = 0; i < 2 * FFT_N; i++) {
    seed = seed * 1664525u + 1013904223u;
    a[i] = b[i] = ((int32_t)seed >> 8) * (1.0f / 8388608.0f);
  }
  int64_t t0 = esp_timer_get_time();
  esp.forward(a);
  int64_t t1 = esp_timer_get_time();
  ref.forward(b);
  int64_t t2 = esp_timer_get_time();
  float maxDiff = 0, maxMag = 0;
  for (int i = 0; i < 2 * FFT_N; i++) {
    maxDiff = fmaxf(maxDiff, fabsf(a[i] - b[i]));
    maxMag = fmaxf(maxMag, fabsf(b[i]));
  }
  st.fftMaxErr = maxDiff / maxMag;
  st.fftEspUs = (float)(t1 - t0);
  st.fftRefUs = (float)(t2 - t1);
  // The portable tables are small and only needed for this check; the Fft
  // objects don't own a destructor, so leaving them allocated is ~3 KB.
  dsp::release(a);
  dsp::release(b);
}

// ---------- public API ----------
const char* sourceName(Source s) { return s == Source::Inbuilt ? "inbuilt" : "array"; }

bool parseSource(const char* s, Source* out) {
  if (!strcmp(s, "array"))        *out = Source::Array;
  else if (!strcmp(s, "inbuilt")) *out = Source::Inbuilt;
  else return false;
  return true;
}

int sourceChannels(Source s) { return s == Source::Inbuilt ? 1 : NUM_MICS; }

bool begin() {
  calLoad();
  fftSelfCheck();
  if (!processor.init(ACTIVE_GEOMETRY, calCurrent)) return false;
  calCurrent = processor.calibration();   // after validation/clamping

  for (int c = 0; c < 2; c++) {
    dmaCopy[c] = (int32_t*)dsp::alloc(BUF_BYTES);
    fifo[c] = (int32_t*)dsp::alloc(FIFO_SLOTS * sizeof(int32_t));
    testBuf[c] = (int32_t*)heap_caps_malloc(TEST_SLOTS * sizeof(int32_t), MALLOC_CAP_SPIRAM);
    if (!dmaCopy[c] || !fifo[c] || !testBuf[c]) return false;
  }
  ring = (int32_t*)heap_caps_calloc(RING_FRAMES * NUM_MICS, sizeof(int32_t), MALLOC_CAP_SPIRAM);
  streamRing = (StreamRec*)heap_caps_calloc(STREAM_RECS, sizeof(StreamRec), MALLOC_CAP_SPIRAM);
  hopIn = (float*)dsp::alloc(sizeof(float) * HOP * NUM_MICS);
  hopBeam = (float*)dsp::alloc(sizeof(float) * HOP);
  hopMics = (float*)dsp::alloc(sizeof(float) * HOP * NUM_MICS);
  if (!ring || !streamRing || !hopIn || !hopBeam || !hopMics) return false;

  boardMic.init();
  st.mode = processor.mode();
  st.source = st.sourceTarget = Source::Array;   // every boot; the source is never persisted
  if (!i2sInit()) return false;
  if (xTaskCreatePinnedToCore(processTask, "dsp", 8192, NULL, PROCESS_PRIO, &processTaskHandle, CORE_AUDIO) != pdPASS)
    return false;
  if (xTaskCreatePinnedToCore(captureTask, "i2scap", 4096, NULL, CAPTURE_PRIO, NULL, CORE_AUDIO) != pdPASS)
    return false;
  return true;
}

void loopPoll() {
  if (calDirty) {
    calDirty = false;
    portENTER_CRITICAL(&mux);
    Calibration c = calCurrent;
    portEXIT_CRITICAL(&mux);
    calSave(c);
  }
}

void status(Status* out) {
  portENTER_CRITICAL(&mux);
  *out = st;
  portEXIT_CRITICAL(&mux);
}

void setMode(proc::Mode m) {
  portENTER_CRITICAL(&mux);
  pending.mode = true;
  pending.m = m;
  portEXIT_CRITICAL(&mux);
}

void setFixedAngle(float deg) {
  portENTER_CRITICAL(&mux);
  pending.angle = true;
  pending.a = deg;
  portEXIT_CRITICAL(&mux);
}

Calibration calibration() {
  portENTER_CRITICAL(&mux);
  Calibration c = calCurrent;
  portEXIT_CRITICAL(&mux);
  return c;
}

void setCalibration(const Calibration& in) {
  const Calibration c = proc::sanitizeCalibration(in);
  portENTER_CRITICAL(&mux);
  calCurrent = c;
  pending.cal = true;
  pending.c = c;
  portEXIT_CRITICAL(&mux);
  // Saved from loopPoll(), so the HTTP handler returns first. A flash write
  // stalls both cores' caches for a few ms (an erase can take longer); the
  // six DMA descriptors absorb ~80 ms, and anything longer shows up as a
  // counted overflow + resync, never as silent misalignment.
  calDirty = true;
}

bool startGainCal(float seconds) {
  if (gainCalHopsLeft > 0 || !isfinite(seconds) || seconds < 0.5f || seconds > 30.0f) return false;
  if (src != Source::Array || srcTarget != Source::Array) return false;   // trims are the array's
  for (auto& s : gainCalSum) s = 0;
  gainCalHops = 0;
  st.gainCal = 1;
  gainCalHopsLeft = (int)(seconds * SAMPLE_RATE / HOP);
  return true;
}

void requestResync() { resyncRequested = true; }

const dsp::Geometry& geometry() { return processor.geometry(); }

bool requestSource(Source s) {
  if (s == srcTarget) return true;
  if (rawArmed) return false;
  srcTarget = s;
  portENTER_CRITICAL(&mux);
  st.sourceTarget = s;
  portEXIT_CRITICAL(&mux);
  return true;
}

Source sourceTarget() { return srcTarget; }

CodecJob codecJob(uint32_t* ticket) {
  const uint32_t t = jobTicket;
  if (t == jobDone) return CodecJob::None;
  __sync_synchronize();   // jobKind was written before the ticket moved
  *ticket = t;
  return jobKind;
}

void codecJobDone(uint32_t ticket, bool ok) {
  if (ticket != jobTicket || jobDone == ticket) return;   // withdrawn (timed out) or superseded
  jobOk = ok;
  __sync_synchronize();
  jobDone = ticket;
}

int rawChannels() { return sourceChannels(src); }

bool rawCaptureBegin(uint8_t* buf, uint32_t frames, int channels) {
  if (rawArmed || channels != sourceChannels(src) || src != srcTarget) return false;
  rawBuf = buf;
  rawFrames = frames;
  rawCh = channels;
  rawPos = 0;
  rawAborted = false;
  rawResyncStart = st.resyncs;
  __sync_synchronize();
  rawArmed = true;
  return true;
}

RawState rawCaptureState(uint32_t* resyncsDuring) {
  if (rawAborted) { rawAborted = false; return RawState::Aborted; }
  if (!rawArmed || rawPos < rawFrames) return RawState::Pending;
  rawArmed = false;
  if (resyncsDuring) *resyncsDuring = st.resyncs - rawResyncStart;
  return RawState::Done;
}

void rawCaptureAbort() {
  rawArmed = false;
  // The capture task may be mid-frame with the old flag; a pairing pass takes
  // well under a millisecond, so after this delay it is safe to free the buffer.
  vTaskDelay(pdMS_TO_TICKS(50));
  rawAborted = false;
}

uint32_t streamHead() { return streamHeadSeq; }

bool streamRead(uint32_t* seq, int source, int16_t* out) {
  uint32_t head = streamHeadSeq;
  if (*seq == head) return false;
  // Keep a couple of records between us and the writer.
  if (head - *seq > (uint32_t)(STREAM_RECS - 4)) {
    const uint32_t skip = head - 2 - *seq;
    portENTER_CRITICAL(&mux);
    st.streamDrops += skip;
    portEXIT_CRITICAL(&mux);
    *seq = head - 2;
  }
  const StreamRec* rec = streamRing + (*seq % STREAM_RECS);
  memcpy(out, source < 0 ? rec->beam : rec->mic[source], sizeof(int16_t) * HOP);
  __sync_synchronize();
  if (streamHeadSeq - *seq >= (uint32_t)STREAM_RECS) {   // overwritten while copying
    portENTER_CRITICAL(&mux);
    st.streamDrops++;
    portEXIT_CRITICAL(&mux);
    *seq = streamHeadSeq - 2;
    return false;
  }
  (*seq)++;
  return true;
}

}  // namespace engine
