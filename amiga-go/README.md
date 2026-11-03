# Amiga Go (`amiga-go`)

Amiga emulator port for Retro-Go, based on the platform-independent core of
[rlucencic/esp32ami](https://github.com/rlucencic/esp32ami) (MIT).

## Sources

- Core: `components/amiga/` (vendored into this project as `amiga-go/components/amiga/`)
  - `amiga.c`, `display.c`, `audio.c`, `floppy.c`, `input.c`, `mem.c`, ...
  - Public API in `components/amiga/include/amiga.h`.
- CPU core: `components/musashi/` (vendored from the evansm7 fork in
  `esp32emu`), with generated opcode tables.
- Debugger: intentionally **not** included (lean port; no `emudbg`/lwip).

## Building

Use ESP-IDF **v5.3.5** (as documented for the Martendo32 targets).

```bash
source ~/.espressif/v5.3.5/esp-idf/export.sh
python rg_tool.py --target martendo32-ips-hdmi --no-networking build amiga-go
python rg_tool.py --target martendo32-ips-hdmi --no-networking build-img launcher amiga-go
python rg_tool.py --target martendo32-ips-hdmi --no-networking flash amiga-go
```

The full image with launcher + amiga-go is written to
`retro-go_bootstrapped-26-g73f52-dirty_martendo32-ips-hdmi.img`.

## Configuration

- Kickstart/AROS ROMs are looked for in `/sd/retro-go/bios/` (then the content
  directory as a fallback).
- Models: A500, A2000, A600, A1200 (option; restart required after change).
- Content: ADF/RP9 floppy images inserted via the app options.

## Controls

- Joystick: Retro-Go gamepad -> Amiga joystick (port 2), fire via A/B, second
  fire via X/Y.
- Mouse: emulated from the gamepad — d-pad directions move the Amiga mouse
  (port 1); L/R shoulders are the left/right mouse buttons. The same d-pad
  directions also feed the Amiga joystick, so joystick and mouse based games
  both get movement.
- Keyboard-ish keys: START -> Return, SELECT -> Space.

## Known limitations

- No save states.
- PAL only for now (NTSC not implemented in this port yet).
- Output is a 320x240 downscale of the Amiga picture (720x568 hires PAL -> 320x240).
- Audio: 44100 Hz stereo Paula output.

## Status

First working build; Kickstart loading screen and a game (Nebulus) booted fine.

## Thanks

Thanks to Robert Lucencic and the [esp32ami](https://github.com/rlucencic/esp32ami)
team for the platform-independent Amiga core, and to the
[esp32emu](https://github.com/rlucencic/esp32emu) team for the Musashi 68000 core.

## Vendored sources / provenance

- Core: `esp32ami` @ `6f37fbead8c091f8e362b8e6a3fcccc32c79d35b` (MIT, Robert Lucencic).
- CPU core: `esp32emu` musashi @ `c704ab404ac582364fc8eb28c882221caf0b7a09` (MIT).
- Licenses: `components/amiga/LICENSE`, `components/musashi/LICENSE`.
- No Kickstart, Workbench or game images are included; ROMs are user supplied.
