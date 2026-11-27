# Firmware: Lecture Capture Device

| Folder | What it is |
|---|---|
| `board/board.h` | The pins of each supported board. The build target picks one. |
| `lc_core/` | Platform-independent C: frame encoding, IMA-ADPCM, selective ACK, RAM ring, offline log, backoff. Host-tested. |
| `bringup/` | Hardware check without the microphone: display, LEDs, SD card, Wi-Fi scan, buttons. |
| `mic_test/` | Module 1, hardware validation: INMP441 → WAV over USB, with automatic wiring diagnosis. |
| `recorder/` | Module 3, the device firmware: audio, net, spill, UI and button tasks with the state machine. |
| `tools/capture_wav.py` | Saves the WAV that `mic_test` prints. |
| `tools/make_test_clip.ps1` | Makes the speech clip the recorder can play instead of the microphone. |

## Boards

| | ESP32 DevKit V1 (**the board as built**) | ESP32-S3-DevKitC-1 N8R8 (original design) |
|---|---|---|
| `idf.py set-target` | `esp32` | `esp32s3` |
| Chip, memory | ESP32-D0WD-V3, 4 MB flash, **no PSRAM** | ESP32-S3, 8 MB flash, 8 MB PSRAM |
| RAM ring | 10 × 500 ms as ADPCM (40 KB internal RAM) | 256 × 500 ms as PCM (3.9 MB PSRAM) |
| Offline log | **MicroSD card**, file `LCBUF.BIN`, 20 min; internal flash (4.4 min) if no card | internal flash, 10.5 min |
| Live codec | ADPCM (codec 1) | PCM16 (codec 0) |
| Buttons | one main button (short = start/pause/resume, hold 1.5 s = stop), BOOT does the same; optional Stop | Start, Stop, Pause/Resume |
| Console | UART0 through the CP2102 (COM port) | USB-Serial/JTAG |

The classic ESP32 has about 266 KB of free RAM and no PSRAM, so it keeps only 5 s of audio in RAM,
as ADPCM, and moves anything older (or anything while offline) to the SD card. The server accepts
ADPCM on the live path, so nothing changes on the server side.

> **Verification status (2026-11-25):** all four projects build with ESP-IDF v5.5.1 for `esp32`, and `recorder` and `mic_test` also for `esp32s3`. `bringup` has run on the real ESP32 DevKit V1: display, LED, 32 GB SD card (470 KB/s write), Wi-Fi scan and the main button all passed. The microphone has not been wired yet, so `mic_test` and the recorder's I2S path have not run on hardware.

## 1. Wiring

The pins live in [board/board.h](board/board.h); this table repeats them. A drawn, step-by-step
version for the DevKit V1 is in [docs/project/wiring.html](../docs/project/wiring.html).

| Part | Pin | ESP32 DevKit V1 | ESP32-S3 | Notes |
|---|---|---|---|---|
| INMP441 | SCK (BCLK) | D26 | 4 | |
| | WS (LRCLK) | D25 | 5 | |
| | SD | D33 | 6 | 100 kΩ pull-down to GND |
| | L/R, GND, VDD | GND, GND, 3V3 | same | 100 nF across VDD–GND at the mic. **Never 5 V.** |
| SSD1306 0.96" | SDA / SCL | D21 / D22 | 8 / 9 | address 0x3C. The tested module labels its power pins V1/V2: V2 = VCC, V1 = GND (set by the 2+/1− pads on the back). |
| MicroSD adapter | CS / SCK / MISO / MOSI | D5 / D18 / D19 / D23 | — | VCC to **VIN (5 V)**: the adapter has its own 3.3 V regulator. FAT32 card. |
| Buttons | main / stop | D32 / D27 (optional); BOOT = main | Start 1, Stop 2, Pause 7 | to GND, internal pull-ups |
| Status LED | R / G / B | RX2 (16) / TX2 (17) / D4 | 16 / 17 / 18 | through 100–330 Ω; three single LEDs or one RGB LED |

**Pins not used, deliberately:**
- DevKit V1: D12, D15, D2 (boot straps; D2 drives the on-board LED), TX0/RX0 (USB serial). D5 is a
  strap pin too, but the SD adapter holds it high, so it is safe as CS. D34, D35, VP, VN are input-only.
- ESP32-S3: GPIO0, 3, 45, 46 (straps), 19/20 (USB), 35–37 (octal PSRAM), 43/44 (console), 38/48 (on-board LED).

## 2. Toolchain

- ESP-IDF **v5.3 or newer** (it needs the split driver components and the `i2c_master` API).
  On the development PC it is v5.5.1 in `C:\esp\esp-idf`, with its tools in `C:\esp\.espressif`.
- On first build, the component manager fetches `espressif/esp_websocket_client` automatically.

```powershell
$env:IDF_TOOLS_PATH = "C:\esp\.espressif"; . C:\esp\esp-idf\export.ps1     # Windows PowerShell
```

```bash
. $IDF_PATH/export.sh                                                   # Linux / macOS
```

The DevKit V1 appears as "Silicon Labs CP210x USB to UART Bridge (COMn)" in Device Manager.

## 2a. Bring-up check (no microphone needed)

```bash
cd firmware/bringup
idf.py set-target esp32 && idf.py -p COM3 build flash monitor
```

It prints PASS or FAIL with a hint for each part, and shows the results on the display:
I2C scan and display, each LED in turn (check by eye), SD card (mount, write and read back 1 MB,
speed), Wi-Fi scan (your 2.4 GHz network must be listed), then the buttons live. A button that
reads "pressed" for 8 s without being touched is plugged in the wrong way round: turn it 90°.

## 3. Module 1: microphone validation

```bash
cd firmware/mic_test
idf.py set-target esp32 && idf.py -p COM3 build flash      # esp32s3 for the S3 board
pip install pyserial
python ../tools/capture_wav.py --port COM3 --out mic.wav    # /dev/ttyUSB0 on Linux
```

**Expected output:** a `LEVEL` line every 0.5 s, then one `DIAG` line, then
`saved mic.wav (160044 bytes)`: 5 s on the DevKit V1, which has no PSRAM (10 s and 320044 bytes on the S3).

| DIAG | Meaning and fix |
|---|---|
| `ok` | Wiring good. Play `mic.wav`; speech at 1 m should be clearly audible. |
| `stuck` | The same raw word every sample: SD floating or shorted, or SCK and WS swapped. |
| `silent` | Below −70 dBFS: no VDD, L/R not grounded (data comes on the right slot), or SD not connected. |
| `clipping` | Too loud or too close; move the mic away (gain is fixed at +24 dB in the test). |
| `dc_offset` | A large DC offset: check grounding. The high-pass filter removes it in the recorder. |

## 4. Module 3: recorder

### Provisioning (D10)

Copy `recorder/nvs_config.csv.example` to `nvs_config.csv` and fill in:
- Wi-Fi SSID and password,
- `server_url` (`wss://<your-domain>/ws/device`),
- the device UUID and token that the admin web page shows **once** when the device is registered.

Then flash it:

```bash
cd firmware/recorder
python $IDF_PATH/components/nvs_flash/nvs_partition_generator/nvs_partition_gen.py \
       generate nvs_config.csv nvs_config.bin 0x6000
idf.py set-target esp32 && idf.py -p COM3 build flash        # esp32s3 for the S3 board
esptool.py --chip esp32 -p COM3 write_flash 0x9000 nvs_config.bin
idf.py -p COM3 monitor
```

**Without a microphone:** `idf.py menuconfig → Lecture Capture → Audio source → Built-in test clip`
makes the recorder play a 33 s spoken clip (`main/test_clip.adpcm`) in real time instead of reading
the microphone. Everything else (buttons, buffering, SD card, streaming, transcription, notes)
runs as normal, and the display shows `TEST CLIP, NOT MIC`. Remake the clip with
`tools/make_test_clip.ps1` from `tools/test_clip.txt`.

For development you can set the same values with `idf.py menuconfig → Lecture Capture` instead. Values in NVS take precedence. For deployment, enable NVS encryption and flash encryption so the token isn't stored in plaintext (`menuconfig → Security features`).

### Partition tables

| Partition | DevKit V1 (`partitions_esp32.csv`, 4 MB) | ESP32-S3 (`partitions.csv`, 8 MB) | Purpose |
|---|---|---|---|
| nvs | 24 KB | 24 KB | Wi-Fi, URL, device id, token, session state |
| factory | 1.875 MB | 3 MB | Application |
| audiobuf | 2.06 MB = 528 sectors = 4.4 min | 4.94 MB = 1264 sectors = 10.5 min | ADPCM offline audio (on the DevKit V1 only when no SD card is inserted) |

**Offline capacity.** DevKit V1: the SD log holds `CONFIG_LC_SD_LOG_CHUNKS` = 2400 chunks =
**20 min**, plus 5 s in RAM. Its RAM index costs 16 bytes per chunk, so RAM, not the card, sets the
limit. On first boot with a new card the firmware writes the 9.8 MB `LCBUF.BIN` once (about 20 s,
the display shows "Preparing SD card"). ESP32-S3: 128 s of PCM in PSRAM plus 10.5 min in flash,
about **12.7 min**. Both are arithmetic from these sizes, to be **measured** in the soak test.

### Tasks

| Task | Core | Priority | Job |
|---|---|---|---|
| `audio` | 1 | max−3 | I2S DMA (8 × 512 frames = 256 ms of buffering; 6 × 512 = 192 ms on the DevKit V1), 24-bit → high-pass → gain → 16-bit, 500 ms chunks into the ring (ADPCM-encoded on the DevKit V1), level meter, 60 s silence warning |
| `net` | 0 | 5 | Wi-Fi with backoff, WSS (certificate bundle), `hello`→`config`, live-first sending (≤ 40 chunks in flight), selective ACKs, `start`/`resume`/`pause`/`stop`, web `command`s, `status` every 1 s while recording (level meter) and 5 s otherwise |
| `spill` | 0 | 4 | Chunks un-ACKed for > 2 s while offline, or ring ≥ 80% full, move to the offline log (SD card or flash), ADPCM-encoded if they are PCM; refreshes buffer facts |
| `ui` | 0 | 2 | LED every 250 ms, OLED at 2 Hz |
| `buttons` | 0 | 6 | ISR → 30 ms settle → 400 ms repeat guard. DevKit V1: main button acts on release (short) or at 1.5 s held (stop) |

### Indicators

| State | LED | OLED |
|---|---|---|
| READY | green | READY, Wi-Fi bars, server OK, prepared lecture title, `Offline: SD 20 min` |
| RECORDING | red | `REC hh:mm:ss` (exact audio time), Wi-Fi, level meter |
| RECORDING_OFFLINE | red ↔ yellow at 1 Hz | `REC (offline)`, RAM % and SD (or FLASH) % |
| PAUSED | slow red blink | `PAUSED hh:mm:ss`, "Press to resume" (no audio is captured) |
| SYNCING / FINALIZING | blue | `Syncing N chunks` |
| ERROR, no Wi-Fi, no server | yellow | Error text; "Start still records" |

The bottom two OLED lines are reserved for warnings:
- `BUFFER FULL – AUDIO NOT SAVED`
- `STORAGE ALMOST FULL` (offline log ≥ 90%)
- `NO SPEECH 60 s – Check microphone`

## 5. Host tests (lc_core)

These run from the repository root with any C11 compiler:

```bash
cmake -S firmware/lc_core/test_host -B firmware/lc_core/test_host/build
cmake --build firmware/lc_core/test_host/build
./firmware/lc_core/test_host/build/lc_core_tests        # expect: "N checks, 0 failures"
```

They cover:
- frame encoding and validation;
- ADPCM round-trip and odd-length blocks;
- **byte-exact agreement with the Python codec** (`contracts/test_vectors`);
- selective ACK;
- live-first ring ordering and session isolation;
- the spill policy;
- the flash log: recovery after reboot, ACK persistence, power cut mid-write, never overwriting pending audio;
- backoff bounds.

Without CMake or a PC compiler, zig works as one: `pip install ziglang`, then

```bash
python -m ziglang cc -std=c11 -Wall -Wextra -Werror -Ifirmware/lc_core/include \
    firmware/lc_core/src/*.c firmware/lc_core/test_host/test_main.c -o lc_core_tests && ./lc_core_tests
```

## 6. Build check without ESP-IDF

`tools/compile_check.py` checks that the firmware builds, using the ESP32-S3 compiler and the
prebuilt ESP-IDF 5.5 libraries from the Arduino IDE's **esp32** board package (3.3.x, Boards
Manager). It does not need ESP-IDF itself:

```bash
python firmware/tools/compile_check.py --host-tests "python -m ziglang cc"
```

It compiles every C file with `-Wall -Wextra -Werror`, links `recorder` and `mic_test` against
ESP-IDF (so every IDF function they call exists), and turns the recorder into a flashable image
whose size it checks against the 3 MB app partition. With `--host-tests` it also runs the lc_core
tests. It downloads `esp_websocket_client` v1.8.0 (the version `idf_component.yml` resolves to)
on first use.

This is a check, not the release build: the Arduino libraries were built with Arduino's
`sdkconfig`, not `recorder/sdkconfig.defaults`. Flash the board with `idf.py build flash`.

Result on 2026-11-24: 17 files compiled with no warnings; both apps linked; recorder image
1.05 MB of 3 MB; host tests 80 checks, 0 failures (also clean under UBSan and `-Wconversion`).
Not yet run on the board.

## 7. Failure points to check first

1. **Flash erase stalls** (S3, or the DevKit V1 without an SD card). During an erase, both cores pause, so audio survives only on the I2S DMA buffering. Measure the worst erase time on your module. If chunks show gaps, raise `DMA_DESC` in `audio.c`. SD card writes don't stall the CPU.
2. **RAM on the DevKit V1.** The recorder logs `free RAM after buffers` at boot. If TLS fails to allocate, lower `CONFIG_LC_SD_LOG_CHUNKS` or `CONFIG_LC_RING_SLOTS`, or use `ws://` on a local network.
3. **SD card CRC errors** (`0x109`). Long jumper wires: the firmware already enables pull-ups and caps the clock at 10 MHz. Check the adapter's VCC is on 5 V and the MISO wire is firm.
4. **TLS memory.** If a handshake fails with `MBEDTLS_ERR_SSL_ALLOC_FAILED`, keep `CONFIG_MBEDTLS_DYNAMIC_BUFFER=y`, or allow mbedTLS to use PSRAM (`CONFIG_MBEDTLS_EXTERNAL_MEM_ALLOC`).
5. **Campus networks that block NTP.** The device falls back to the server time sent in `config`.
6. **Mic SD line noise.** Keep the mic wires under 15 cm, and keep the 100 kΩ pull-down on the mic's SD pin.
7. **Soak test.** Over 90 minutes, `heap_caps_get_free_size()` in the monitor must stay flat. Log it every minute.
