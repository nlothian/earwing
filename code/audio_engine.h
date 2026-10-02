// The on-device audio engine: synchronised 4-mic I2S capture, the processing
// task that runs proc::Processor, and the hand-off points for the UI (status,
// controls), the audio server (live stream ring, raw capture) and calibration
// persistence.
//
// Tasks (both pinned to core 1; WiFi owns core 0):
//   capture  (prio CAPTURE_PRIO)  reads one DMA buffer per controller at a
//            time, keeps them aligned, pairs them into 4-channel frames and
//            writes them to the frame ring (and to a raw capture, if armed).
//   process  (prio PROCESS_PRIO)  one hop at a time from the ring: Processor,
//            stats, then the stream ring for /beam.wav.
// Arduino's loopTask (prio 1, core 1) sits below both. Neither task prints:
// all serial output happens in loop().
//
// Two microphone sources, switchable at run time (boot is always Array):
//   Array    the 4× INMP441 on I2S0 (master) + I2S1 (slave), GPIO2/3/14/21
//   Inbuilt  the board's own mic through its ES8311 codec, on I2S0 alone
//            (MCLK GPIO4, BCLK 5, WS 7, data in 6). One channel, no geometry.
// The S3 has two I2S controllers and the array needs both, so the sources
// can't run side by side: a switch tears one down completely (channels
// deleted, pins released) and builds the other, inside the capture task,
// because the I2S calls take mutexes. The codec is configured over I2C, which
// belongs to loop() (the touch panel shares the bus), so the capture task
// hands it the codec jobs below and keeps draining DMA until they're answered.
#pragma once

#include "processor.h"
#include "sync_check.h"

namespace engine {

using audio::NUM_MICS;

enum class Source : uint8_t { Array, Inbuilt };
const char* sourceName(Source s);                  // "array", "inbuilt"
bool        parseSource(const char* s, Source* out);
int         sourceChannels(Source s);              // live capture channels: 4 or 1

// The ES8311's MCLK, as a multiple of the sample rate: what the I2S channel
// generates on GPIO4 and what board_io programs the codec's dividers for.
static const int CODEC_MCLK_MULTIPLE = 256;

enum class CodecInit : uint8_t { Never, Ok, Failed };   // result of the last ES8311 init

struct Status {
  // ---- source ----
  Source   source;             // what the capture hardware is built for right now
  Source   sourceTarget;       // last request; differs from source while switching
  uint32_t sourceSwitches;     // completed teardown + rebuilds (never counted in resyncs)
  CodecInit codecInit;         // last ES8311 init
  bool     codecPowered;       // ES8311 ADC path configured (inbuilt only)
  uint32_t codecInits, codecFailures;   // init attempts, and those that failed or timed out
  uint32_t hwFaults;           // an I2S channel could not be created (retried each second)
  // ---- capture (per controller: [0] = I2S0 master, [1] = I2S1 slave; the
  //      board mic uses [0] only) ----
  uint32_t frames[2];          // frames read since the last (re)start
  uint32_t overflows[2];       // DMA queue overflows (on_recv_q_ovf), total
  uint32_t timeouts[2];        // reads that came back short or late, total
  uint32_t mismatches;         // controllers delivered different frame counts
  uint32_t resyncs;            // stop/restart cycles after a fault or a forced resync
  uint32_t ringDrops;          // frames dropped because the frame ring was full
  uint16_t ringDepth, ringHigh;// frame ring fill, hops (current, high-water)
  bool     running;            // past warm-up and the alignment self-test (array), or
                               // codec init and warm-up (inbuilt)
  sync_check::Result align;    // last alignment self-test
  uint32_t alignRuns;          // self-tests run
  uint32_t alignConclusive;    // ... that found a match
  uint32_t alignChanges;       // conclusive results that differed from the first one
  uint32_t alignRebuilds;      // channel rebuilds after a misread self-test (total)
  int      alignFirstOffset;   // first conclusive offset, slots
  // ---- processing ----
  proc::Mode mode;               // the array's mode, kept (not applied) while inbuilt
  float    fixedDeg;
  float    doaDeg, confidence;   // latest scan
  float    smoothDeg;            // tracked DOA (cone angle for a linear array)
  float    smoothAzDeg;          // ... as a table azimuth: what to show people
  bool     smoothValid, speech;
  float    steerDeg, steerAzDeg; // beam steering, and as a table azimuth
  float    micDb[NUM_MICS];      // per channel, ~100 ms average, after trim (dBFS);
                                 // inbuilt: [0] is the board mic, the rest aren't live
  float    micPeakDb[NUM_MICS];  // per channel, decaying peak of the raw input (tap test)
  float    beamDb;               // output, ~100 ms average (dBFS)
  float    lead[proc::NUM_LEADS], leadCoh[proc::NUM_LEADS];   // last gated GCC-PHAT
  uint32_t leadMs;               // millis() when lead[] was measured, 0 = never
  float    dspMeanUs, dspMaxUs;  // per hop, over the last second
  float    dspPeakUs;            // worst hop since boot
  uint32_t hops;
  uint32_t doaAccepts;           // hops that moved the DOA estimate (onsets), since boot
  uint32_t streamDrops;          // hops a /beam.wav client missed
  uint32_t clipped;              // samples clipped converting to 16-bit
  int      gainCal;              // 0 idle, 1 running, 2 done, −1 failed
  // ---- boot checks ----
  float    fftMaxErr;            // esp-dsp vs portable FFT, max |diff| / max |X|
  float    fftEspUs, fftRefUs;   // one 512-point forward FFT
};

bool begin();                  // from setup(): allocate, start I2S and tasks
void loopPoll();               // from loop(): deferred NVS writes
void status(Status* out);

void setMode(proc::Mode m);
void setFixedAngle(float deg);
audio::Calibration calibration();
void setCalibration(const audio::Calibration& c);   // applies and saves
bool startGainCal(float seconds);   // 0.5-30 s; broadband source at broadside first; array only
void requestResync();               // forced stop/restart (sync tests)
const dsp::Geometry& geometry();

// ---- source switching ----
// Applied by the capture task; watch Status::source for completion. False,
// and nothing changes, while a raw capture is armed, so a /raw.wav never
// spans two sources. Asking for the current target is a no-op that succeeds.
bool   requestSource(Source s);
Source sourceTarget();

// ---- ES8311 jobs (board_io, from loop()) ----
// Init is posted right after the codec channel starts MCLK (the reference
// drivers configure the codec with its clocks already running); Shutdown
// before MCLK stops on the way back to the array. The capture task keeps
// reading DMA while it waits and gives up after a timeout, counting a failure.
// `ticket` pairs the answer with the job, so a late answer can't be taken for
// a newer job's.
enum class CodecJob : uint8_t { None, Init, Shutdown };
CodecJob codecJob(uint32_t* ticket);          // None, or the job to run now
void     codecJobDone(uint32_t ticket, bool ok);

// ---- raw capture (audio server) ----
// 24-bit little-endian, `channels` per frame in capture-channel order (4 for
// the array, 1 for the board mic), into a caller-owned buffer (PSRAM) of
// frames × channels × 3 bytes. Begin fails if `channels` doesn't match the
// running source. A capture that meets a source switch is Aborted by the
// capture task, which stops writing first, so the buffer may be freed.
enum class RawState : uint8_t { Pending, Done, Aborted };
int      rawChannels();   // what a capture armed now would record
bool     rawCaptureBegin(uint8_t* buf, uint32_t frames, int channels);
RawState rawCaptureState(uint32_t* resyncsDuring);
void     rawCaptureAbort();   // blocks ~50 ms; the buffer may be freed afterwards

// ---- live stream (audio server) ----
// Records of HOP 16-bit samples. `source`: −1 = beam output, 0..3 = raw mic
// at that geometry row (delayed to line up with the beam). With the inbuilt
// source every one of them is the board mic, high-passed, same gain. `*seq`
// is the reader's position; start it at streamHead(). A reader that falls
// behind is moved forward and the skipped hops are counted as stream drops.
uint32_t streamHead();
bool     streamRead(uint32_t* seq, int source, int16_t* out);

}  // namespace engine
