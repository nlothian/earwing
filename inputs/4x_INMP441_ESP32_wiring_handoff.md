# 4-Microphone INMP441 Array for ESP32-S3 (Freenove FNK0104)

## What we are building

This project is a **4-microphone linear or spatial microphone array** using four INMP441 digital MEMS microphones connected to a **Freenove FNK0104** (2.8" ESP32-S3 display board).

The microphones are intended to be sampled synchronously so the ESP32-S3 can compare the very small timing differences between microphones. That enables later DSP features such as:

- direction-of-arrival estimation
- beamforming toward a selected speaker
- improved speech signal-to-noise ratio
- partial rejection of sound arriving from other directions

The four INMP441 microphones are arranged electrically as **two I2S stereo pairs**.

- Mic 1 + Mic 2 share one I2S data line.
- Mic 3 + Mic 4 share a second I2S data line.
- All four microphones share the same I2S bit clock and word-select clock.

The INMP441 `L/R` pin determines whether each microphone transmits in the left or right I2S time slot.

---

## Board connections used

The four signals go to the board's **Expanded IO socket (P3)**, which carries exactly four free GPIOs. P3 has no power pins, so the mics take **3V3 and GND from the I2C socket (P4)**.

| Function | GPIO | Board socket pin |
|---|---|---|
| Microphone power | — | **P4 pin 1 (3V3)** |
| Ground | — | **P4 pin 2 (GND)** |
| I2S bit clock / SCK | **GPIO2** | **P3 pin 1** |
| I2S word select / WS / LRCLK | **GPIO3** | **P3 pin 2** |
| I2S data for Mic 1 + Mic 2 | **GPIO14** | **P3 pin 3** |
| I2S data for Mic 3 + Mic 4 | **GPIO21** | **P3 pin 4** |

Pin numbers are from Freenove's schematic; check the silkscreen for which end of each socket is pin 1. Do **not** connect anything to P4 pins 3–4 (SCL/SDA): that bus is shared with the touch panel and the audio codec.

Full board pin map and the schematic: [docs/FNK0104-pinout.md](../docs/FNK0104-pinout.md).

GPIO2, GPIO3, GPIO14, and GPIO21 are separate electrical signals and must **not** be connected to each other.

---

## Pin-to-pin wiring

### Mic 1

| INMP441 pin | Connect to board |
|---|---|
| `VDD` | **3V3** (P4 pin 1) |
| `GND` | **GND** (P4 pin 2) |
| `SCK` | **GPIO2** (P3 pin 1) |
| `WS` | **GPIO3** (P3 pin 2) |
| `SD` | **GPIO14** (P3 pin 3) |
| `L/R` | **GND** |

Mic 1 is the **left channel** of the GPIO14 data pair.

---

### Mic 2

| INMP441 pin | Connect to board |
|---|---|
| `VDD` | **3V3** (P4 pin 1) |
| `GND` | **GND** (P4 pin 2) |
| `SCK` | **GPIO2** (P3 pin 1) |
| `WS` | **GPIO3** (P3 pin 2) |
| `SD` | **GPIO14** (P3 pin 3) |
| `L/R` | **3V3** |

Mic 2 is the **right channel** of the GPIO14 data pair.

---

### Mic 3

| INMP441 pin | Connect to board |
|---|---|
| `VDD` | **3V3** (P4 pin 1) |
| `GND` | **GND** (P4 pin 2) |
| `SCK` | **GPIO2** (P3 pin 1) |
| `WS` | **GPIO3** (P3 pin 2) |
| `SD` | **GPIO21** (P3 pin 4) |
| `L/R` | **GND** |

Mic 3 is the **left channel** of the GPIO21 data pair.

---

### Mic 4

| INMP441 pin | Connect to board |
|---|---|
| `VDD` | **3V3** (P4 pin 1) |
| `GND` | **GND** (P4 pin 2) |
| `SCK` | **GPIO2** (P3 pin 1) |
| `WS` | **GPIO3** (P3 pin 2) |
| `SD` | **GPIO21** (P3 pin 4) |
| `L/R` | **3V3** |

Mic 4 is the **right channel** of the GPIO21 data pair.

---

## Shared connections

All four microphones share:

- `VDD` -> **3V3** (P4 pin 1)
- `GND` -> **GND** (P4 pin 2)
- `SCK` -> **GPIO2** (P3 pin 1)
- `WS` -> **GPIO3** (P3 pin 2)

The data lines are split into two pairs:

- Mic 1 `SD` + Mic 2 `SD` -> **GPIO14** (P3 pin 3)
- Mic 3 `SD` + Mic 4 `SD` -> **GPIO21** (P3 pin 4)

The `L/R` selections are:

- Mic 1 `L/R` -> **GND**
- Mic 2 `L/R` -> **3V3**
- Mic 3 `L/R` -> **GND**
- Mic 4 `L/R` -> **3V3**

---

## Important electrical notes

Do **not** connect any of these signal pins together:

- GPIO2
- GPIO3
- GPIO14
- GPIO21

They are four distinct nets:

- GPIO2 = clock
- GPIO3 = word select
- GPIO14 = data pair 1
- GPIO21 = data pair 2

The microphones should be powered from **3.3 V**, not 5 V. Pin 1 of the board's UART socket (P2) is **5 V**, so don't take power from it.

GPIO3 is an ESP32-S3 strapping pin, but it only acts as one if a specific eFuse has been burned, which isn't the case on this board. It is safe to use for WS.

---

## Firmware notes

- The ESP32-S3 has two I2S controllers, and each reads **one** data line in standard I2S mode. Two data lines (GPIO14 and GPIO21) therefore need both controllers:
  - I2S0 is the master: it drives SCK and WS on GPIO2/GPIO3 and reads GPIO14.
  - I2S1 is a slave clocked from the same GPIO2/GPIO3 and reads GPIO21.
  - The firmware starts the slave before the master, but nothing in the driver guarantees both capture the same first frame. So after every start and resync it briefly routes GPIO14 into I2S1 as well, compares the two streams word for word, and removes any constant offset. The result is on the status page ("Alignment test"); see `code/audio_engine.cpp`.
- The firmware enables the ESP32-S3's internal pull-downs on GPIO14 and GPIO21, which the INMP441 datasheet asks for on SD. External 100 kΩ pull-downs would also do.
- Capture channel order: ch1 = I2S0 left (Mic 1), ch2 = I2S0 right (Mic 2), ch3 = I2S1 left (Mic 3), ch4 = I2S1 right (Mic 4). Which physical position each mic occupies is **not yet tap-tested**. Once it is, record it here and set it with `/control?map=…` (stored in NVS).
- The onboard ES8311 codec (speaker, onboard mic) also needs an I2S controller, so it can't run alongside this mic array without extra work.
- The INMP441 outputs 24-bit samples in 32-bit slots, so SCK = 64 × sample rate (e.g. 1.024 MHz at 16 kHz).
- These four GPIOs are all of P3. Any further I/O (buttons, etc.) has to go on the I2C socket, e.g. via an MCP23017 expander.
