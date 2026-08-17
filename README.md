# MCUME esp64 — Commodore 64 for the TTGO VGA32

A port of the [MCUME](https://github.com/Jean-MarcHarvengt/MCUME) Commodore 64 emulator (by Jean-Marc Harvengt) to the **TTGO VGA32** board, with native **VGA output**, a **PS/2 keyboard**, and an optional **Bluetooth controller** over a serial bridge to a TTGO T-Display.

The C64 core is MCUME's (based on Frank Bösing's Teensy64). This fork replaces the original TFT video layer with VGA output, migrates the project from ESP-IDF to Arduino, and adds keyboard support, an on-screen file browser, and `.t64` handling.


---

## Features

- Native **VGA 320×240** output (via FabGL's `VGADirectController`), ~50 fps
- **PS/2 keyboard**
- On-screen file browser with an 8×8 font
- **`.prg`** loading (working) and **`.t64`** 
- Boots straight into BASIC, like a real C64 — the menu is called on demand

---

## Hardware

Board: **TTGO VGA32** (ESP32-PICO-D4). The board has PSRAM, but it is deliberately left **disabled** — enabling it costs ~30% emulator performance and doesn't help, because the framebuffer needs internal RAM either way.

### Wiring

| Signal | GPIO |
|---|---|
| VGA R1 / R0 | 22 / 21 |
| VGA G1 / G0 | 19 / 18 |
| VGA B1 / B0 | 5 / 4 |
| VGA HSync / VSync | 23 / 15 |
| SD card (CS/CLK/MISO/MOSI) | 13 / 14 / 2 / 12 |
| PS/2 keyboard (CLK / DAT) | 33 / 32 |
| T-Display bridge (RX / TX) | 34 / 26 |

The SD MISO is 2 on LilyGO boards and 35 on ROBGO/Olimex — adjust in `iopins.h` if needed.

---

## Building

**PlatformIO** project using the **Arduino** framework.

```bash
pio run --target upload
pio device monitor
```

Relevant `platformio.ini` settings: flash in **QIO 80 MHz**, **`-Os`** optimization, PSRAM **disabled**. Don't re-enable PSRAM — the reason is documented in the file.

---

## Usage

1. Copy your `.prg` (and `.t64`) files into a **`/c64`** folder at the root of the SD card.
2. Power on the board: it drops straight into the C64 BASIC.
3. Press **F6** to open the file browser.
4. Navigate with the **arrow keys**, enter folders or pick a file with **ENTER**.
5. Back in BASIC, press **F1** to load and run the selected file.

### Keys

| Key | In the menu | In BASIC / game |
|---|---|---|
| Arrows | Navigate the list | Cursor / joystick |
| ENTER | Enter folder or select file | — |
| F1 | Toggle joystick port (SWAP) | `LOAD""` + `RUN` the selected file |
| F6 | — | Open the file browser |

**SWAP** toggles between joystick port 1 and port 2 — different games expect the joystick on different ports.

`._*` files (the junk macOS leaves on FAT cards) are ignored automatically.

---

## Current status

| Item | Status |
|---|---|
| VGA video | ✅ Working, ~50 fps |
| PS/2 keyboard | ✅ Working |
| File browser | ✅ Working |
| `.prg` loading | ✅ Working |
| `.t64` loading | 🚧 In testing |
| Audio (SID) | ⚠️ Untested |
| Horizontal aspect | 🚧 Slight stretch, no side border |
| SD card | ⚠️ Fails a few attempts before mounting |

---

## Credits

- **Jean-Marc Harvengt** — [MCUME](https://github.com/Jean-MarcHarvengt/MCUME), the basis for this project
- **Frank Bösing** — Teensy64, the C64 core
- **Fabrizio Di Vittorio** — [FabGL](https://github.com/fdivitto/FabGL), VGA video and PS/2 keyboard
- TTGO VGA32 port by **fg1998** / Alternative Bits

## License

Follows the original MCUME license (GPL). See the source files for details.
