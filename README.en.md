# ESP32-S3 MP3 Player

A portable MP3 player based on the ESP32-S3: reads MP3 files from an SD card, decodes them through `esp_audio_codec`, and outputs audio via an I2S class-D amplifier. Features a 2.4" ST7789 SPI color display with an LVGL GUI, 5 physical buttons, and GBK Chinese filename support.

[English](README.en.md) | [简体中文](README.md)

![ESP-IDF](https://img.shields.io/badge/ESP--IDF-v6.0-blue)
![Chip](https://img.shields.io/badge/Chip-ESP32--S3-green)
![LVGL](https://img.shields.io/badge/LVGL-v9-orange)

## Table of Contents

- [Features](#features)
- [Measured Performance](#measured-performance)
- [Hardware & Wiring](#hardware--wiring)
- [System Architecture](#system-architecture)
- [Directory Layout](#directory-layout)
- [Key Implementation Details](#key-implementation-details)
- [Tunable Parameters](#tunable-parameters-app_configh)
- [Toolchain & Dependencies](#toolchain--dependencies)
- [Build & Flash](#build--flash)
- [Usage](#usage)
- [UI Layout](#ui-layout)
- [Serial Logs & Diagnostics](#serial-logs--diagnostics)
- [Hardware-Specific Adaptations (Important)](#hardware-specific-adaptations-important)
- [FAQ](#faq)
- [Known Limitations](#known-limitations)

## Features

- **MP3 decoding & playback**: Built on Espressif's official `esp_audio_codec` component (simple decoder API). Supports MPEG-1/2/2.5 Layer III, any sample rate (8 / 11.025 / 12 / 16 / 22.05 / 24 / 32 / 44.1 / 48 kHz), VBR/CBR. The I2S interface is dynamically reconfigured after the first frame reveals the real format; mono audio is automatically upmixed to stereo.
- **Stable, glitch-free playback**: Feeds the decoder in small slices (≤ 768 B ≈ 2 frames) with a holdover buffer that preserves unconsumed bytes between calls. A 16 × 240-frame I2S DMA buffer (~87 ms of headroom) absorbs decoding bursts. Measured rate deviation is +0.003% (WS measured at 44101 Hz) with zero DMA underruns during continuous playback.
- **SD card storage**: SDSPI (SPI2_HOST) + FATFS VFS. Automatic 2-retry remount on failure. Recursively scans the whole card for audio files, playlist up to 2048 entries, all paths stored in PSRAM.
- **Chinese filenames**: FATFS UTF-8 filename API enabled; a built-in Noto Sans SC 16 px / 4 bpp full GB2312 font lets GBK-encoded Chinese song names display correctly.
- **LVGL GUI**: 240×320 ST7789 RGB565 color display with song-name marquee, playback state, volume bar, progress bar, and played/total time.
- **Full-screen song list**: Long-press Prev/Next to enter a full-screen song browser. Inside the list, short-press Prev/Next moves the cursor up/down (with blue highlight + `>` prefix marker), short-press Play plays the selected song and returns to the playback view, long-press Prev/Next exits the list.
- **5 physical buttons**: Play/Pause, Prev, Next, Vol+, Vol-. 30 ms software debouncing. Volume keys auto-repeat (500 ms long-press threshold, 200 ms repeat interval). Prev/Next support 500 ms long-press to toggle the list view.
- **Duration probing**: Parses Xing/Info/VBRI VBR headers for accurate frame counts; falls back to first-frame bitrate + file size estimate for CBR; subtracts ID3v2/ID3v1 tag sizes.
- **Fault-tolerant boot**: Display and UI initialize first; a missing/unresponsive SD card does not abort boot. Auto-advances to the next song when the current one finishes.
- **Robust SD read tolerance**: Transient read errors (data CRC, etc.) automatically retry by rewinding to the failed sector (up to 20 attempts). I2S write timeouts preserve already-enqueued bytes and only retry the remainder.

## Measured Performance

| Metric | Value |
| --- | --- |
| Playback rate deviation (44.1 kHz source) | +0.003% (I2S WS measured 44101 Hz) |
| DMA underruns | 0 |
| I2S write timeouts | 0 |
| Peak decoding time (768 B input) | ≤ 17.9 ms |
| I2S DMA buffer capacity | 16 × 240 stereo frames ≈ 15.4 KB ≈ 87 ms |
| Firmware size | ~1.5 MB (factory partition 4 MB) |
| LCD SPI clock | 40 MHz |
| SD card working clock | Limited to 2 MHz (stable on long Dupont wires) |

## Hardware & Wiring

### Bill of Materials

| Item | Spec |
| --- | --- |
| Main board | ESP32-S3 (onboard 16 MB Flash (Boya) + 8 MB octal PSRAM). USB port is native USB CDC (`/dev/ttyACM0`, GPIO19/20). |
| Display | ST7789 240×320 SPI 7-pin (no MISO, no backlight pin; VCC direct power). |
| Audio amp | MAX98357A I2S class-D module, VIN = 5 V, directly drives a small speaker. |
| Storage | Micro SD card module (with regulator + level shifter), FAT32. Dev card: SDSC "SD512" 478 MB. |
| Buttons | 5 tactile buttons, active-low (one end to GPIO, other to GND, using internal pull-up). |

### Wiring Table

All pins are centralized in [`main/config/board.h`](main/config/board.h); changing wires only requires editing that file.

| Module | Signal | GPIO | Notes |
| --- | --- | --- | --- |
| SD card | CS | 4 | **Module VCC must be 5 V**; at 3.3 V ACMD41 never completes. |
| SD card | SCK | 5 | SPI2_HOST, clock limited to 2 MHz. |
| SD card | MOSI | 6 | |
| SD card | MISO | 7 | |
| LCD | SCK | 8 | SPI3_HOST, 40 MHz. |
| LCD | MOSI | 9 | No MISO on this panel. |
| LCD | CS | 1 | |
| LCD | DC | 2 | |
| LCD | RST | 3 | |
| Button | Play/Pause | 10 | In list: plays selected song. |
| Button | Prev | 11 | Long-press toggles list; in list: cursor up. |
| Button | Next | 12 | Long-press toggles list; in list: cursor down. |
| Button | Vol + | 13 | Long-press auto-repeat. |
| Button | Vol − | 14 | Long-press auto-repeat. |
| MAX98357A | BCLK | 15 | VIN = 5 V. |
| MAX98357A | LRC (WS) | 16 | |
| MAX98357A | DIN | 17 | |

> Note: GPIO18 is reserved as `I2S_GAIN_GPIO` in `board.h` but currently unused. The BOOT button (GPIO0) is intentionally not bound to any playback function.

### Wiring Diagram

```
ESP32-S3
├── SPI2 (SD card module, VCC=5V)    ├── SPI3 (ST7789 LCD)
│   SCK  ────────── GPIO5            │   SCK  ────────── GPIO8
│   MOSI ────────── GPIO6            │   MOSI ────────── GPIO9
│   MISO ────────── GPIO7            │   CS   ────────── GPIO1
│   CS   ────────── GPIO4            │   DC   ────────── GPIO2
│                                    │   RST  ────────── GPIO3
├── I2S0 (MAX98357A, VIN=5V)         │
│   BCLK ────────── GPIO15          └── Buttons (other end all to GND)
│   LRC  ────────── GPIO16              Play  GPIO10  Prev  GPIO11
│   DIN  ────────── GPIO17              Next  GPIO12  Vol+  GPIO13
└──                                       Vol-  GPIO14
```

## System Architecture

### Boot Flow (`app_main`)

```
power_init()                         # Power (placeholder; 3.3V direct)
  └─ display_init()                  # SPI3 + ST7789 + LVGL port (boots first, lights up without SD)
      └─ ui_init()                   # Build LVGL UI
          └─ sd_card_mount()         # SDSPI mount; failure only warns, does not abort
              └─ file_list_scan()    # Recursively scan /sdcard, sort; create /sdcard/music if empty
                  └─ i2s_output_init()
                      └─ player_init()   # Command queue + 256 KB file ring buffer
                          └─ button_init()
                              └─ Start player_task / file_task / button_task / monitor_task
```

### Dual-Core Task Division

| Task | Core | Priority | Stack | Responsibility |
| --- | --- | --- | --- | --- |
| `player_task` | Core 0 | 8 | 12 KB | Pulls compressed data from ring buffer → MP3 decode → software volume → write I2S; handles commands, song switching, duration tracking. |
| `file_task` | Core 1 | 5 | 4 KB | `fread()` SD file in 512 B single-sector units, writes to file ring buffer; retries on read errors. |
| LVGL task (component-internal) | Core 1 | 5 | 6 KB | LVGL tick at 5 ms. |
| `button_task` | Core 1 | 3 | 4 KB | Scans 5 buttons every 10 ms; debounce/long-press/repeat; dispatches commands to player queue. |
| `monitor_task` | Core 1 | 1 | 2 KB | Refreshes UI every 100 ms (state/song/progress/volume on playback screen; no refresh while list is visible). |

### Data Flow

```
SD card (FATFS)
   │  512 B sector reads, file_task @Core1
   ▼
File ring buffer (256 KB, PSRAM)
   │  feed ≤ 768 B at a time, player_task @Core0
   ▼
MP3 decoder (esp_audio_codec)
   │  holdover preserves unconsumed bytes to keep frame boundaries intact
   ▼
PCM buffer (128 KB, PSRAM, 16-bit)
   │  mono expanded to stereo in place; software volume scaling
   ▼
I2S DMA (16 × 240 frames ≈ 87 ms)  ──►  MAX98357A  ──►  Speaker
```

Playback control flows through a FreeRTOS queue (depth 10): the button task only dispatches commands; all heavy operations (`fopen` / decoder open/close) run inside `player_task` with a 12 KB stack to avoid stack overflow in the button task.

## Directory Layout

```
├── CMakeLists.txt                 # Top-level project file (project: mp3_player)
├── partitions.csv                 # Custom partition table: nvs 24K / phy 4K / factory 4M
├── sdkconfig.defaults             # Default config: ESP32-S3, octal PSRAM, 16MB Flash,
│                                  #   FATFS UTF-8, LVGL 16-bit color
├── sdkconfig                      # Current active config
├── dependencies.lock              # Managed component version lock
└── main/
    ├── CMakeLists.txt             # Component registration (sources, REQUIRES, INCLUDE_DIRS)
    ├── idf_component.yml          # Managed component dependencies
    ├── app_main.c                  # Boot entry, task orchestration, monitor_task
    ├── config/
    │   ├── board.h                # All hardware pin definitions (edit here to rewire)
    │   └── app_config.h           # Buffer/DMA/button timing/task stack & priority
    ├── storage/
    │   ├── sd_card.c/.h           # SDSPI bus init, FAT mount (2 retries), unmount
    │   └── file_list.c/.h         # Recursive scan, extension filter, dictionary sort,
    │                              #   prev/next cyclic cursor (PSRAM storage)
    ├── audio/
    │   ├── decoder.c/.h           # esp_audio_codec wrapper: holdover, first-frame format probe,
    │                              #   mono upmix, PCM buffer, EOS flush, decode stats
    │   └── i2s_output.c/.h        # I2S STD master channel, dynamic sample-rate reconfig, auto_clear
    ├── player/
    │   ├── player.c/.h            # Playback state machine, command queue, volume, duration probe,
    │                              #   player_task/file_task, tempo self-check log
    │   └── ringbuf.c/.h           # Byte ring buffer (PSRAM)
    ├── display/
    │   ├── display.c/.h           # SPI3 bus, ST7789 panel, LVGL port init
    │   ├── ui.c/.h                # LVGL UI: title/state/song/progress/volume/full-screen list
    │   ├── cjk16.c                # Noto Sans SC 16px 4bpp full GB2312 font (~6 MB source file)
    │   └── gb2312_symbols.txt     # Character table used to generate the font
    ├── input/
    │   └── button.c/.h            # 5-key scan: debounce, long-press, volume repeat,
    │                              #   long-press Prev/Next toggles list, in-list nav & play
    └── power/
        └── power.c/.h             # Power init placeholder
```

## Key Implementation Details

### 1. MP3 Frame Boundaries & Holdover

The decoder has its own frame-sync parser; each `process()` call only consumes part of the input (`raw.consumed`). Dropping the unconsumed bytes would make the next frame start at a wrong offset, causing noise or pitch shifts. `decoder.c` keeps a `hold[]` cache: before each feed, the previous residual is prepended to the new chunk (`scratch = hold + new chunk`); after `drain()` returns, remaining bytes are copied back into `hold`, so the decoder always sees a continuous, gap-free byte stream.

### 2. First-Frame Probe + Dynamic I2S Reconfiguration

The true sample rate / channel count is only known after decoding the first frame. The decoder's `get_info()` is unreliable, so the code additionally scans for the first frame-sync word (11-bit sync + MPEG version/layer/sample-rate index/channel mode) to determine the format before feeding. After the first PCM frame is produced, `get_info()` is used to cross-check. When `player_task` detects a sample-rate change, it calls `i2s_output_set_sample_rate()`: disable channel → `i2s_channel_reconfig_std_clock()` (new API in ESP-IDF v6.0) → enable.

### 3. Mono Upmix & DMA Memory

- For mono files, the decoder only outputs mono PCM, while I2S is fixed to stereo 16-bit Philips time slots; `decoder_read_pcm()` expands each `int16` into two L/R copies in place, working from tail to head.
- The decode output buffer (8 KB `frame_out`) is always heap-allocated and prefers `MALLOC_CAP_DMA`; the PCM buffer (128 KB) lives in PSRAM to avoid stack overflow from large buffers.

### 4. DMA Buffer vs. Feed Granularity

Early versions fed 4096 B at a time, with decode bursts averaging 62 ms (peak 77.5 ms), exceeding the 65 ms DMA buffer at the time and causing slow playback (measured −1.8%). Final parameters:

- Feed granularity reduced to **768 B (≤ 2 MP3 frames)**, peak decode ≤ 17.9 ms;
- I2S DMA enlarged to **16 descriptors × 240 frames ≈ 87 ms** buffer;
- PCM is written to I2S in **4 KB blocks** (200 ms timeout); on timeout, already-enqueued bytes are preserved and only the remainder is retried — never discarded wholesale.

### 5. Pause Without Artifacts

The I2S channel is configured with `.auto_clear = true`: the DMA buffer is auto-zeroed after playback. After pause/stop, the speaker receives silence rather than a looping tail of the last audio chunk ("tape-loop" artifact).

### 6. Duration Probing

`mp3_probe_total_sec()` opens the file independently (without disturbing ongoing playback): skips the ID3v2 tag (including footer), checks the last 128 bytes for an ID3v1 `TAG`, finds the first valid Layer III frame header (and uses "the next frame must also sync" to reject false syncs), prefers reading the total frame count from Xing/Info or VBRI headers for an exact calculation, otherwise estimates CBR duration using file size minus tags divided by the first-frame bitrate.

### 7. SD Reads & Song-Switch Race Handling

- The card occasionally glitches on multi-block reads (CMD18) in SPI mode, so `file_task` always reads **512 B single sectors**;
- On transient read errors, `fseek()` rewinds to the failed sector boundary and retries; after 20 failures the song is abandoned;
- During fast song switching, `file_task` (Core 1) may be in the middle of `fread()` on the same `FILE*`. Before switching, the state is moved to PAUSED, and a `s_file_reading` flag + `wait_file_reader_idle()` waits for the read side to exit the critical window before `decoder_close()/fclose()`, preventing cross-core use-after-free.

### 8. ST7789 Display Notes

- This panel requires `esp_lcd_panel_invert_color(panel, true)` or colors appear inverted;
- RGB565 is sent big-endian (`swap_bytes = true`); SPI clock 40 MHz;
- LVGL draw buffers live in PSRAM: 240 × 40 rows double-buffered;
- The song-name label uses `LV_LABEL_LONG_SCROLL` so long names scroll horizontally.

### 9. Chinese Filenames & Font

- `sdkconfig.defaults` sets `CONFIG_FATFS_API_ENCODING_UTF_8=y`; GBK filenames on FAT are converted to UTF-8 by the VFS;
- The font is generated from Noto Sans SC Regular via LVGL's font converter (16 px, 4 bpp, uncompressed, full GB2312 set) and compiled into the firmware; UI labels explicitly use `&cjk16`.

## Tunable Parameters (app_config.h)

| Macro | Default | Description |
| --- | --- | --- |
| `I2S_DMA_DESC_NUM` | 16 | DMA descriptor count |
| `I2S_DMA_FRAME_NUM` | 240 | Stereo frames per descriptor (16×240 ≈ 87 ms) |
| `FILE_BUF_SIZE` | 256 KB | File → decoder ring buffer (PSRAM) |
| `PCM_BUF_SIZE` | 128 KB | Post-decode PCM buffer (PSRAM) |
| `MAX_PLAYLIST_SIZE` | 2048 | Max playlist entries |
| `BTN_SCAN_PERIOD_MS` / `BTN_DEBOUNCE_MS` | 10 / 30 | Button scan period & debounce |
| `BTN_LONG_PRESS_MS` / `BTN_VOL_REPEAT_MS` | 500 / 200 | Long-press threshold & repeat interval |
| `PLAYER_TASK_STACK` | 12288 | Decode task stack (must be large enough) |

Volume is software-scaled PCM, range 0–100, step 5, default 30 on boot.

## Toolchain & Dependencies

- **ESP-IDF v6.0** (must be v6.x; the code uses v6.0 audio/I2S/SD new APIs, see below)
- Target chip: ESP32-S3, octal PSRAM, 16 MB Flash
- Managed components (in [`main/idf_component.yml`](main/idf_component.yml), auto-downloaded at build):
  - `espressif/esp_audio_codec >= 2.3.0` (MP3 decode)
  - `espressif/esp_lvgl_port ^2` (bundles LVGL v9)
- Built-in components (fatfs, sdmmc, esp_driver_sdspi, esp_driver_spi, esp_driver_i2s, esp_lcd, etc.) ship with IDF and need not be declared in the yml.

Notable ESP-IDF v6.0 API differences (code is written accordingly):

- Simple decoder header is `esp_audio_simple_dec.h`; types/functions prefixed `esp_audio_simple_dec_*`; error code `ESP_AUDIO_ERR_DATA_LACK`;
- I2S clock config uses `I2S_STD_CLK_DEFAULT_CONFIG()`; dynamic sample-rate change uses `i2s_channel_reconfig_std_clock()`;
- FAT card unmount uses `esp_vfs_fat_sdcard_unmount(mount_point, card)`, taking the card handle;
- SDSPI host header is `driver/sdspi_host.h`.

Partition table ([`partitions.csv`](partitions.csv)):

| Partition | Type | Offset | Size |
| --- | --- | --- | --- |
| nvs | data/nvs | 0x9000 | 24 KB |
| phy_init | data/phy | 0xF000 | 4 KB |
| factory | app/factory | 0x10000 | 4 MB |

## Build & Flash

```bash
# 0. Export ESP-IDF v6.0 environment (adjust to your install path)
. $HOME/esp/esp-idf/export.sh

# 1. Set the target chip (once)
idf.py set-target esp32s3

# 2. Build
idf.py build

# 3. Flash and monitor (native USB CDC; device: /dev/ttyACM0)
idf.py -p /dev/ttyACM0 flash monitor
```

- Flash size must be 16 MB (`sdkconfig.defaults` sets `CONFIG_ESPTOOLPY_FLASHSIZE_16MB=y`);
- On Linux, if `/dev/ttyACM0` cannot be opened, add your user to the `dialout` group (`sudo usermod -aG dialout $USER`); also watch for ModemManager grabbing the serial port (`systemctl stop ModemManager`);
- Manual download mode: **hold BOOT → short-press RST → keep holding BOOT for ~1 s, then release**.

## Usage

1. Copy MP3 files to the SD card (root or `/music`; any nesting depth is recursively scanned); format the card as FAT32.
2. Insert the card and power on. The display lights up and enters the playback view. If no audio files are found and `/music` does not exist, the directory is auto-created.
3. **Playback view**: short-press Play to play/pause, Prev/Next to switch songs, Vol ± to adjust volume (hold to auto-repeat).
4. **Enter the song list**: long-press Prev or Next ≥ 500 ms to open the full-screen song list; the currently playing song is highlighted by default.
5. **Inside the song list**:
   - Short-press Prev/Next to move the cursor up/down (selected row: blue background + green text + `>` prefix; auto-scrolls when out of view).
   - Short-press Play to play the selected song and return to the playback view (stops the current song first, then starts the selected file).
   - Long-press Prev or Next to cancel and return to the playback view.
6. When a song finishes, the next one auto-plays; the list loops.
7. With no SD card inserted, the system still boots — the list is empty and Play does nothing.

## UI Layout

### Playback View (default)

```
┌──────────────────────────┐
│       MP3 Player         │  Title
│       >> PLAYING         │  State (PLAYING / PAUSED / STOPPED)
│                          │
│   Song name (scrolls)    │
│  ─────────██████───────  │  Progress bar
│      01:23 / 03:45       │  Played / Total (--:-- if probe failed)
│                          │
│  Vol: 30%                │
│  ████████░░░░░░░░░░░░░   │  Volume bar
└──────────────────────────┘
```

### Song List View (long-press Prev/Next, full-screen overlay)

```
┌──────────────────────────┐
│        Song List         │  Title
│                          │
│    SongA.mp3             │
│  > SongB.mp3             │  ← Selected row (blue bg + green text + > prefix)
│    SongC.mp3             │
│    SongD.mp3             │  Auto-scrolls here when out of view
│                          │
└──────────────────────────┘
```

## Serial Logs & Diagnostics

Key info is logged via standard tags (`MAIN` / `SD_CARD` / `DECODER` / `PLAYER` / `I2S_OUT`):

- `DECODER: mp3 hdr @..: ver=.. rate=44100Hz ch=2` — real format from frame header.
- `DECODER: stream format: 44100Hz 2ch 16bps bitrate=..` — decoder-reported format.
- `I2S_OUT: sample rate changed to .. Hz` — I2S reconfiguration.
- `PLAYER: duration: 3:45` — duration probe result.
- Every 60 s a tempo self-check line: `tempo: audio=60.01s wall=60.00s (44100Hz) frames=.. harderr=0 pcmdrop=0 ok`. If `UNDERRUN!` appears, the DMA ran dry.

Monitor the serial port:

```bash
idf.py -p /dev/ttyACM0 monitor      # Ctrl+] to exit
# or
picocom /dev/ttyACM0 -b 115200
```

## Hardware-Specific Adaptations (Important)

This project has targeted adaptations for two non-standard hardware conditions. **Pay special attention when swapping SD cards or modules:**

1. **SD card module must be powered at 5 V.** This module has onboard regulation and level-shifting. At 3.3 V the card responds to CMD0/CMD8 correctly, but ACMD41 initialization never completes (typical brownout symptom), manifesting as `ESP_ERR_INVALID_RESPONSE` mount failure.
2. **Some old SDSC cards (such as the SD512 used here) reject CMD59 (CRC toggle)** and return "illegal command". Two small patches to the ESP-IDF install tree are needed (**these are lost on IDF reinstall/upgrade and must be re-applied**):
   - `components/sdmmc/sdmmc_sd.c` in `sdmmc_init_spi_crc()`: treat `ESP_ERR_NOT_SUPPORTED` from CMD59 as acceptable;
   - `components/sdmmc/sdmmc_common.h`: bump `MAX_ERRORS` from 3 to 5.
3. SD working clock is limited to **2 MHz** in [`sd_card.c`](main/storage/sd_card.c) — Dupont wires cause persistent read errors at higher speeds (previously triggered interrupt watchdog resets). Short wires or a PCB may allow raising `host.max_freq_khz`.
4. If the ST7789 shows inverted colors, that's expected — the code already enables color inversion. For screen garbling, lower `LCD_SPI_FREQ_HZ` in [`board.h`](main/config/board.h) from 40 MHz to 20 MHz.

## FAQ

**Q: Only noise, no music?**
A: Historically caused by frame-data misalignment; fixed by the holdover mechanism. If it recurs, capture the serial log and check the `stream format` line's sample rate / channel count, and whether `harderr` / `pcmdrop` counters are growing.

**Q: Playback tempo too slow?**
A: Decoding is exceeding the DMA buffer headroom (log shows `UNDERRUN!`). Confirm `app_config.h` has feed granularity 768 B and DMA 16×240 frames; do not blindly raise the SD clock — read-error retries also slow the data stream.

**Q: Mount log reports `ESP_ERR_INVALID_RESPONSE`?**
A: First confirm the SD module VCC is 5 V; for old cards, also confirm the IDF CMD59/MAX_ERRORS patches are still present (must be re-applied after any IDF reinstall).

**Q: Chinese filenames show as boxes / gibberish?**
A: Confirm `CONFIG_FATFS_API_ENCODING_UTF_8=y` and that UI labels use the `cjk16` font (already set in the UI code). The font covers full GB2312; characters outside that set cannot render.

**Q: Buttons not responding?**
A: Buttons are active-low; confirm the other end is tied to GND. Play/Prev/Next require audio files on the SD card; volume keys should work in any state. The BOOT button (GPIO0) is intentionally not bound to any function.

**Q: A faint "beee—" sound after pause?**
A: Old-firmware artifact; confirm the I2S channel has `.auto_clear = true` (current code already does).

## Known Limitations

- File scanning collects `.mp3 / .wav / .flac / .aac` extensions, but only the MP3 decoder is registered; selecting a non-MP3 file fails to open and returns to the stopped state (in practice, only place MP3s on the card).
- No playback-position memory, shuffle, playlist persistence, low-power management, or battery charging.
- `I2S_GAIN_GPIO` (GPIO18) is defined but unused; MAX98357A gain follows the module default.
- `power.c` is currently a placeholder (3.3 V always-on).

---

Hardware: ESP32-S3 (16 MB Flash + 8 MB octal PSRAM) · Framework: ESP-IDF v6.0 · GUI: LVGL v9
