# Circuit Diagram and Wiring

This is the recorder device as built: an **ESP32 DevKit V1** (ESP32-WROOM-32, 30 pins, no PSRAM) with an INMP441 microphone, an SSD1306 OLED, a MicroSD card adapter, one main button (plus an optional Stop button) and three status LEDs. The source of truth for pins is [firmware/board/board.h](../../firmware/board/board.h). If this page and that file ever disagree, the file wins.

A drawn wiring guide with a wire-by-wire breadboard list is in [wiring.html](wiring.html). Open it in a browser.

The first design used an ESP32-S3-DevKitC-1 N8R8. The firmware still builds for it (`idf.py set-target esp32s3`); its pins are at the end of this page.

## Bill of materials

| # | Part | Qty | Notes |
|---|---|---|---|
| 1 | ESP32 DevKit V1, 30 pins (ESP32-WROOM-32, CP2102 USB) | 1 | 4 MB flash, no PSRAM. The tested board has an ESP32-D0WD-V3 and a BoyaMicro flash chip |
| 2 | INMP441 I²S MEMS microphone module | 1 | 3.3 V only |
| 3 | SSD1306 0.96" 128×64 OLED, I²C | 1 | Address 0x3C |
| 4 | MicroSD card adapter (SPI, with its own 3.3 V regulator) | 1 | Powered from 5 V (VIN) |
| 5 | MicroSD card, up to 32 GB, FAT32 | 1 | The recorder keeps one 9.8 MB file on it |
| 6 | Tactile push button, 6×6 mm | 1–2 | Main button; the second one is an optional Stop |
| 7 | LEDs, 5 mm: red, green and a third colour | 3 | Or one RGB LED (common cathode or common anode) |
| 8 | Resistor 100–330 Ω | 3 | One per LED (the build uses 100 Ω and 220 Ω) |
| 9 | Resistor 100 kΩ | 1 | Pull-down on the microphone data line |
| 10 | Capacitor 100 nF ceramic | 1 | Decoupling at the microphone |
| 11 | Electrolytic capacitor 10–100 µF | 1 | Across the 3.3 V rails; longer leg (+) to 3.3 V |
| 12 | Breadboard, jumper wires (female-to-male for the board) | — | The DevKit is wide; it is wired with jumpers rather than plugged into the breadboard |
| 13 | Micro-USB cable and 5 V / 1 A supply | 1 | Powers the board |
| 14 | Enclosure | 1 | Leave an opening over the microphone port |

## Schematic (block form)

```
                          ESP32 DevKit V1 (ESP32-WROOM-32)
                       ┌──────────────────────────────────┐
   INMP441             │                                  │
  ┌────────┐           │                                  │
  │ VDD ───┼───────────┤ 3V3                              │
  │ GND ───┼───────────┤ GND                              │
  │ L/R ───┼──── GND   │                                  │
  │ SCK ───┼───────────┤ D26  (I2S BCLK)                  │
  │ WS  ───┼───────────┤ D25  (I2S LRCLK)                 │
  │ SD  ───┼────┬──────┤ D33  (I2S DIN)                   │
  └────────┘    │      │                                  │
             100kΩ     │                                  │
                │      │                                  │
               GND     │                                  │
   100 nF between VDD and GND, close to the mic           │
                       │                                  │
   SSD1306 OLED        │                                  │
  ┌────────┐           │                                  │
  │ V2/VCC ┼───────────┤ 3V3                              │
  │ V1/GND ┼───────────┤ GND                              │
  │ SDA ───┼───────────┤ D21  (I2C SDA)                   │
  │ SCL ───┼───────────┤ D22  (I2C SCL)                   │
  └────────┘           │                                  │
                       │                                  │
   MicroSD adapter     │                                  │
  ┌────────┐           │                                  │
  │ VCC ───┼───────────┤ VIN  (5 V from USB)              │
  │ GND ───┼───────────┤ GND                              │
  │ CS  ───┼───────────┤ D5   (SPI CS)                    │
  │ SCK ───┼───────────┤ D18  (SPI SCK)                   │
  │ MISO ──┼───────────┤ D19  (SPI MISO)                  │
  │ MOSI ──┼───────────┤ D23  (SPI MOSI)                  │
  └────────┘           │                                  │
                       │                                  │
   MAIN  ─o o─ GND ────┤ D32  (internal pull-up)          │
   STOP  ─o o─ GND ────┤ D27  (optional, internal pull-up)│
   BOOT button on the board (GPIO0) acts as MAIN          │
                       │                                  │
   Status LEDs (long leg toward the resistor)             │
     red   ── R ───────┤ RX2 (GPIO16)                     │
     green ── R ───────┤ TX2 (GPIO17)                     │
     third ── R ───────┤ D4  (GPIO4)                      │
     short legs ── GND │                                  │
                       │                                  │
                       │ micro-USB ── 5 V supply          │
                       └──────────────────────────────────┘
```

## Pin table

| Part | Part pin | DevKit V1 pin | Notes |
|---|---|---|---|
| INMP441 | VDD | 3V3 | **Never 5 V or VIN.** 100 nF capacitor across VDD–GND at the module |
| INMP441 | GND, L/R | GND | L/R to GND selects the left channel |
| INMP441 | SCK | D26 | I²S bit clock |
| INMP441 | WS | D25 | I²S word select |
| INMP441 | SD | D33 | I²S data; 100 kΩ to GND so the line is defined when the mic is idle |
| SSD1306 | V2 / V1 | 3V3 / GND | The tested module labels its power pins V1 and V2. Pads on the back (2+ and 1− soldered) make V2 = VCC and V1 = GND. Other modules print VCC/GND |
| SSD1306 | SDA / SCL | D21 / D22 | |
| MicroSD | VCC | VIN | The adapter's regulator needs 5 V; on 3.3 V many cards fail |
| MicroSD | GND | GND | |
| MicroSD | CS / SCK / MISO / MOSI | D5 / D18 / D19 / D23 | The firmware enables pull-ups on MISO, MOSI and CS and caps the clock at 10 MHz |
| Main button | — | D32 → button → GND | Short press: start, pause, resume. Hold 1.5 s: stop |
| Stop button | — | D27 → button → GND | Optional |
| LEDs | red / green / third | RX2 (16) / TX2 (17) / D4 | Each through its own resistor; short legs to GND |

### Pins deliberately not used

| Pins | Reason |
|---|---|
| D12, D15, D2 | Strapping pins; they affect boot. D2 also drives the board's blue LED |
| TX0, RX0 | USB serial (console and flashing) |
| D34, D35, VP, VN | Input-only, no pull-ups; kept free |

D5 is also a strapping pin. It is safe as the SD card's CS because the adapter holds it high at boot.

## Power

- The two breadboard rail pairs can be powered separately. The build uses the bottom rails for 3.3 V (3V3 and GND), with the capacitor across them. The top rails are either GND only, or 5 V from VIN for the SD adapter. If the top + rail carries 5 V, nothing else may use it.
- Each – rail needs its own wire to one of the board's two GND pins. The breadboard does not join the top and bottom rails.

## Design notes

- **Why the DevKit V1 works without PSRAM:** the firmware keeps only 5 s of audio in RAM, as ADPCM (4 KB per 500 ms instead of 16 KB), and moves anything older to the SD card. The server accepts ADPCM on the live path. See [firmware/README.md](../../firmware/README.md), "Boards".
- **Offline storage:** a 20-minute log file on the MicroSD card; internal flash (4.4 minutes) if no card is inserted. The original no-MicroSD plan assumed the S3's 8 MB of PSRAM and 8 MB of flash, which this board doesn't have.
- **Resistor values:** any value from 150 Ω to 1 kΩ works for the LEDs. At 3.3 V, 100 Ω gives about 13 mA through a red LED, within the pin's limit. The firmware also dims the LEDs with PWM.
- **Button debouncing:** done in firmware (30 ms settle, 400 ms repeat guard), so the buttons need no capacitors. A button that sits in one half of the breadboard works in one orientation only. If it reads as always pressed, turn it 90°.
- **Microphone placement:** keep the microphone away from the board's antenna end and away from fans. Point its port toward the lecturer.

## First power-on check

1. Flash `firmware/bringup`. It checks the display, each LED, the SD card (write and read back 1 MB), the Wi-Fi scan and the buttons, and prints PASS or FAIL with a hint. Measured on the built board, 2026-11-25: all passed; SD write 470 KB/s, read 410 KB/s.
2. Flash `firmware/mic_test`, once the microphone is wired. It records 5 s and reports whether the I²S wiring is correct, including swapped SCK/WS and a floating SD line.
3. Flash `firmware/recorder`. The display shows READY and the green LED lights once Wi-Fi and the server are reachable.

## Appendix: ESP32-S3-DevKitC-1 N8R8 pins (original design)

| Part | Pin | ESP32-S3 GPIO |
|---|---|---|
| INMP441 | SCK / WS / SD | 4 / 5 / 6 |
| SSD1306 | SDA / SCL | 8 / 9 |
| Buttons | Start / Stop / Pause | 1 / 2 / 7 |
| RGB LED | R / G / B | 16 / 17 / 18 |

Unused on purpose: GPIO0, 3, 45, 46 (straps), 19/20 (USB), 35–37 (octal PSRAM), 43/44 (console), 38/48 (on-board LED). There is no SD card; offline audio goes to PSRAM and internal flash, about 12.7 minutes.
