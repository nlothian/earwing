# Freenove FNK0104 (2.8") — pin reference

What every ESP32-S3 GPIO on the board is wired to, and what is left for the
mic array. Read from Freenove's schematic
([fnk0104-2.8inch-schematic.pdf](fnk0104-2.8inch-schematic.pdf), dated
2025-06-11), with the SD-card pins cross-checked against Freenove's own SD
example sketch.

**This applies to the 2.8" board only** (FNK0104A/B, ILI9341 240×320). The
3.5" (FNK0104N) and 4.0" (FNK0104S) variants are wired differently — on the
3.5" board, for example, GPIO2 and GPIO3 are SD-card lines, not free pins.

## Chip and memory

| Item | Value |
|---|---|
| SoC | ESP32-S3R8 (8 MB octal PSRAM in-package) |
| Flash | 16 MB quad, external (25VQ128) |
| USB | Native USB-Serial/JTAG on GPIO19/20 (USB-C) |

## GPIO map

| GPIO | Schematic net | Wired to |
|---|---|---|
| 0 | IO0 | BOOT button (KEY2), active-low. Strapping pin. |
| 1 | AUDIO_EN | Speaker amp shutdown (SC8002B), active-low |
| **2** | **IO2** | **Free — Expanded IO socket P3 pin 1** |
| **3** | **IO3** | **Free — Expanded IO socket P3 pin 2.** Strapping pin (JTAG source), but only acts as one if the `STRAP_JTAG_SEL` eFuse is burned |
| 4 | I2S_MCK | ES8311 codec MCLK |
| 5 | I2S_SCK | ES8311 BCLK |
| 6 | I2S_DO | ES8311 ASDOUT → ESP (record data) |
| 7 | I2S_LRC | ES8311 LRCK |
| 8 | I2S_DI | ESP → ES8311 DSDIN (playback data) |
| 9 | BAT_ADC | Battery voltage, 200K/200K divider (2:1) |
| 10 | LCD_CS | Display |
| 11 | LCD_MOSI | Display |
| 12 | LCD_SCK | Display |
| 13 | LCD_MISO | Display |
| **14** | **IO14** | **Free — Expanded IO socket P3 pin 3** |
| 15 | IIC_SCL | I2C bus: ES8311 (0x18), FT6336U touch (0x38), socket P4 pin 3 |
| 16 | IIC_SDA | I2C bus: as above, socket P4 pin 4 |
| 17 | CTP_INT | Touch interrupt |
| 18 | CTP_RST | Touch reset |
| 19 | USB− | USB-C |
| 20 | USB+ | USB-C |
| **21** | **IO21** | **Free — Expanded IO socket P3 pin 4** |
| 26–32 | FSPI_* | SPI flash — do not use |
| 33–37 | — | Not connected (used by the in-package octal PSRAM) |
| 38 | SD_CLK | microSD |
| 39 | SD_D0 | microSD |
| 40 | SD_CMD | microSD |
| 41 | SD_D1 | microSD |
| 42 | RGB_LED | WS2812 RGB LED |
| 43 | TXD0 | UART socket P2 pin 3 (100 Ω series). ROM boot log comes out here. |
| 44 | RXD0 | UART socket P2 pin 4 (100 Ω series) |
| 45 | LCD_BL | Backlight (high = on). Strapping pin. |
| 46 | LCD_RS | Display D/C. Strapping pin. |
| 47 | SD_D3 | microSD |
| 48 | SD_D2 | microSD |

All GPIOs on the board are 3.3 V logic.

## Sockets

Pin numbers are the schematic's. Check the board silkscreen for which end is
pin 1.

| Socket | Pin 1 | Pin 2 | Pin 3 | Pin 4 |
|---|---|---|---|---|
| **P3 Expanded IO** | IO2 | IO3 | IO14 | IO21 |
| **P4 I2C** | 3V3 | GND | SCL (IO15) | SDA (IO16) |
| **P2 UART** | +5 V | GND | TXD0 (IO43) | RXD0 (IO44) |

- P3 has **no power pins**, only the four signals.
- P4's 3V3 comes from the board's main 3.3 V regulator (ME6217).
- P2's pin 1 is **5 V** (switched from +5 through a P-FET that is on by default),
  so don't use it to power 3.3 V parts.

## Other onboard parts

| Part | Notes |
|---|---|
| ES8311 audio codec | I2S on GPIO4–8, I2C 0x18, powered by its own 3.3 V regulator |
| LMA2718 MEMS mic | Analog, into the ES8311's MIC1 input — not an ESP32 GPIO |
| SC8002B speaker amp | Speaker on JP3, enable on GPIO1 (active-low) |
| TP4054 charger | Li-ion battery on JP1 |
| KEY1 | Reset (CHIP_PU), not a GPIO |

## Sources

- Freenove repo: <https://github.com/Freenove/Freenove_ESP32_S3_Display>
  - Schematic: `Schematic/2.8inch_ESP32-S3_Display_Schematic.pdf` (copied here)
  - SD pins per variant: `Tutorial_With_Touch/Sketches/Sketch_06.1_SDMMC_Test/Sketch_06.1_SDMMC_Test.ino`
- Freenove docs: <https://docs.freenove.com/projects/fnk0104/en/latest/>
