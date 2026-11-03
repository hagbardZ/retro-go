# Porting rlucencic/esp32ami to Retro-Go on Martendo32 (ESP32-P4)

This document is a step-by-step plan for turning the Amiga core from
[rlucencic/esp32ami](https://github.com/rlucencic/esp32ami) into a Retro-Go
application (`amiga-go`) that runs on the **Martendo32** target.

It is written to be executed incrementally: each phase has a concrete goal,
the files involved, and an acceptance check. Do not skip Phase 0/1; the IDF
version decision affects everything after it.

---

## 0. Facts and constraints

### Source project

| Item | Value |
|---|---|
| Project | `rlucencic/esp32ami` (MIT) |
| Core | `components/amiga` (platform independent) |
| CPU core | `Musashi` from `rlucencic/esp32emu` (`components/musashi`) |
| Debugger glue | `emudbg` from `rlucencic/esp32emu` (`components/emudbg`) |
| Board layer | `main/` is M5Stack Tab5 specific (LVGL, touch, BLE, WiFi, MIPI-DSI) |
| ESP-IDF | v5.5 (developed with 5.5.4) |
| Validation | A500/A600/A2000 OCS/ECS and A1200/AGA; desktop host build in `components/amiga/host` |

Useful public API (`components/amiga/include/amiga.h`):

```c
bool amiga_init(const amiga_config_t *config);
void amiga_reset(void);
void amiga_run_frame(void);

int  amiga_screen_hz(void);
int  amiga_screen_width(void);              /* 720, or 1440 for ECS super-hires */
int  amiga_screen_height(void);             /* 568 PAL, 484 NTSC */
uint32_t amiga_screen_signature(void);
void amiga_render_line_rgb565(int y, uint16_t *dst);

void amiga_key(uint8_t code, bool pressed);
void amiga_mouse(int dx, int dy, uint8_t buttons);
void amiga_joystick(uint8_t bits);

void amiga_audio_enable(bool on);
int  amiga_audio_render(int16_t *dst, int frames);
int  amiga_audio_pending(void);
```

Audio is fixed at `AMIGA_AUDIO_HZ == 44100`, stereo, signed 16-bit.

### Target: Martendo32

| Item | Value |
|---|---|
| MCU | ESP32-P4, dual RISC-V, 360 MHz |
| Module | Wireless-Tag WT0132P4-A1-N16R32: **16 MB flash, 32 MB PSRAM** |
| Display | ST7789V 320x240 SPI (80 MHz in `martendo32` target) |
| Audio | NS4168 over I2S |
| Storage | microSD over 4-bit SDMMC |
| Input | gamepad only (no keyboard/mouse) |
| Retro-Go target | `components/retro-go/targets/martendo32` (and `-ips`, `-hdmi`) |
| Pinned IDF | **5.3.5** (`BUILDING.md:15`) |

### Consequence

The Amiga core is a good architectural fit, but this is a real port:

- The Tab5 board layer must be discarded.
- Somebody must reconcile IDF 5.5 (upstream) with IDF 5.3.5 (Martendo32).
- Retro-Go has no Amiga save states because `esp32ami` does not implement
  machine serialization.
- Martendo32 has no keyboard or mouse, so a virtual keyboard and a
  gamepad-as-mouse mode are required.

---

## Phase 0 — Decisions before writing code

### 0.1 Choose the IDF strategy

**Option A (recommended): backport the core to IDF 5.3.5.**

The platform-independent core uses standard C + `heap_caps_malloc`, so it has a
reasonable chance of compiling unchanged. Build early and fix the fallout.
Advantages: keeps the existing, tested Martendo32 target intact (USB host,
display, SDMMC, audio). Risks: Musashi/emudbg may use newer APIs.

**Option B: move the Martendo32 target to IDF 5.5.**

Only do this if Option A fails hard. The Martendo32 target has custom USB host
work (commits `ef9d3c7e` and `2c57c073`) that may not survive an IDF upgrade.
Budget time to re-validate USB, SDMMC, audio and display.

Record the decision at the top of this file before continuing.

### 0.2 Confirm licensing and ROM policy

- `esp32ami` is MIT. Keep its `LICENSE` and upstream provenance.
- Musashi is MIT.
- Kickstart ROMs are copyrighted. Do not embed them. Support:
  - a user-supplied Kickstart in `/sd/retro-go/bios/`, and
  - the free AROS ROM (preferred default for a distributable build).
- Disk images (`.adf`, `.hdf`, `.rp9`) are user supplied.

### 0.3 Decide initial scope

Recommended first release scope:

- A500 OCS, 512 KB chip + 512 KB slow RAM, Kickstart 1.3 or AROS.
- One floppy drive (DF0), optionally DF1.
- PAL, 50 Hz, 320x240 output.
- Joystick + virtual keyboard.
- No hard disks, no AGA, no save states.
- No WiFi/BLE/web UI.

Add A600/A2000/A1200, HDF, fast RAM and save states in later phases.

**Acceptance for Phase 0:** a short note in this file stating IDF strategy,
ROM policy, and first-release scope.

---

## Phase 1 — Vendor the emulator core

### 1.1 Fetch upstream sources

Work outside the tree first so the vendored copy is deliberate:

```bash
git clone --depth 1 https://github.com/rlucencic/esp32ami.git /tmp/esp32ami
git clone --depth 1 https://github.com/rlucencic/esp32emu.git /tmp/esp32emu
```

### 1.2 Create the app directory

Mirror the layout used by `frodo/` and `gwenesis/`:

```bash
mkdir -p amiga-go/components amiga-go/main
cp -r /tmp/esp32ami/components/amiga amiga-go/components/amiga
cp -r /tmp/esp32emu/components/musashi amiga-go/components/musashi
cp -r /tmp/esp32emu/components/emudbg amiga-go/components/emudbg
```

Keep `components/amiga/host/` for desktop testing. It is not part of the IDF
component (only the `SRCS` listed in `components/amiga/CMakeLists.txt` are
compiled), so it does no harm.

### 1.3 Handle the debugger dependency

`components/amiga/CMakeLists.txt` requires `musashi` and privately requires
`emudbg`. The only emudbg use in the Amiga core is the debugger description in
`components/amiga/amiga.c:102-136` plus `emudbg_set_machine()`.

Two options:

- **Easy:** keep the real `emudbg` component. Note it pulls in `lwip`
  (`emudbg_freertos.c` uses sockets). Retro-Go has networking available even
  when `RG_ENABLE_NETWORKING=0`, so this normally links.
- **Lean:** remove `m68k_dbg.c` from `musashi/CMakeLists.txt`, drop
  `REQUIRES emudbg`, and delete/`#if 0` the debugger block in `amiga.c`.
  This avoids the debugger and lwip entirely. Recommended for the first
  release.

If you take the lean path, also remove the `display_dbg_*` declarations/uses
from the core if they cause link errors (they are only used by the debugger).

### 1.4 Verify the core compiles standalone

Before touching Retro-Go, build the desktop host:

```bash
make -C amiga-go/components/amiga/host
```

Run one of the upstream smoke tests (from the `esp32ami` README) using a
Kickstart or AROS ROM. This proves the vendored copy is intact.

**Acceptance for Phase 1:** host build succeeds; `make` output binary exists;
no Retro-Go files modified yet.

---

## Phase 2 — Create the `amiga-go` Retro-Go application

### 2.1 Create the project files

`amiga-go/CMakeLists.txt`:

```cmake
cmake_minimum_required(VERSION 3.5)

set(COMPONENTS "main retro-go amiga")
set(RG_ENABLE_NETWORKING 0)

include(../base.cmake)
project(amiga-go)
```

`amiga-go/main/CMakeLists.txt`:

```cmake
set(COMPONENT_SRCDIRS ".")
set(COMPONENT_ADD_INCLUDEDIRS ".")
set(COMPONENT_REQUIRES "retro-go amiga")

register_component()
rg_setup_compile_options(-O2)
```

### 2.2 Create the minimum `main/main.c`

Start with a skeleton that boots and exits cleanly, then add the emulator.

```c
#include <rg_system.h>
#include <amiga.h>

static rg_app_t *app;
static rg_surface_t *surface;
static amiga_config_t machine;
static bool running;

static bool reset_handler(bool hard)
{
    amiga_reset();
    return true;
}

static bool screenshot_handler(const char *filename, int width, int height)
{
    while (rg_display_is_busy()) rg_task_yield();
    return surface && rg_surface_save_image_file(surface, filename, width, height);
}

static void event_handler(int event, void *arg)
{
    if (event == RG_EVENT_REDRAW && surface)
        rg_display_submit(surface, 0);
}

void app_main(void)
{
    const rg_config_t config = {
        .sampleRate = 44100,
        .frameRate = 50,
        .storageRequired = true,
        .romRequired = true,
        .handlers.reset = &reset_handler,
        .handlers.screenshot = &screenshot_handler,
        .handlers.event = &event_handler,
    };

    app = rg_system_init(&config);
    app->frameskip = 0;

    surface = rg_surface_create(320, 240, RG_PIXEL_565_BE, MEM_FAST);

    /* TODO: load Kickstart, amiga_init(), insert disk, run loop */
}
```

Build it empty first:

```bash
python rg_tool.py --target martendo32 build amiga-go
```

At this point `rg_tool.py` does not know `amiga-go` yet (Phase 8), so either
add the entries now or build with `idf.py` manually inside `amiga-go/`.
Adding the `rg_tool.py` entry early is usually easier.

**Acceptance for Phase 2:** `amiga-go` compiles for `esp32p4`; the app starts
and shows a blank 320x240 surface (or exits to launcher if launched from it).

---

## Phase 3 — Kickstart/AROS discovery and machine init

### 3.1 Where the Kickstart comes from

Retro-Go passes the selected content (the disk image) as `app->romPath`. The
Kickstart is separate. Recommended search order:

1. Setting stored by the user in the app options (absolute path).
2. `RG_BASE_PATH_BIOS "/kickstart.rom"`
3. `RG_BASE_PATH_BIOS "/kickstart13.rom"` / `kickstart31.rom`
4. Files matching `amiga-os-*.rom`, `aros*.rom` in `RG_BASE_PATH_BIOS`.
5. If none: show `rg_gui_file_picker()` scoped to the BIOS directory.

Use `amiga_kickstart_version(image, size, &major, &minor)` to validate before
`amiga_init`. This prevents a random file from being treated as a ROM.

### 3.2 Allocate machine memory in PSRAM

`amiga_config_t` takes explicit buffers. Allocate with
`heap_caps_malloc(size, MALLOC_CAP_SPIRAM)` and keep them for the lifetime of
the app. Suggested defaults:

| Buffer | Size |
|---|---|
| `chip` | 512 KB (A500) |
| `slow` | 512 KB (A500) |
| `fast` | 0 (enable later) |
| `rom` | same as Kickstart (256/512 KB working copy) |
| `opcode_tables` | `AMIGA_OPCODE_TABLES_SIZE` or `NULL` |
| `picture` | `amiga_picture_size()` or `NULL` |

With 32 MB PSRAM this is comfortable. Do not allocate per frame.

### 3.3 Insert the selected disk

Map the launcher's content extension to the core:

```c
if (extension == ".adf") amiga_floppy_insert(0, path);
else if (extension == ".hdf") amiga_hd_insert(0, path);
else if (extension == ".rp9") amiga_floppy_insert(0, path);
```

See Phase 6 for writable media and ZIP.

**Acceptance for Phase 3:** app boots the selected Kickstart/AROS, inserts the
disk, and `amiga_run_frame()` executes (log `amiga_get_status()` periodically).

---

## Phase 4 — Display

### 4.1 Understand the source picture

The core produces `amiga_screen_width()` x `amiga_screen_height()`:

- PAL: 720 x 568 (284 beam lines, each doubled).
- Lores pixels are doubled horizontally, so the real lores width is 360.
- ECS super-hires can be 1440 wide.
- The picture is line records; rendering is per line.

The Martendo32 panel is 320x240. You must crop and downscale.

### 4.2 Downscale strategy

Recommended: render directly into the 320x240 Retro-Go surface.

For each output row `y` in `0..239`:

1. Map `y` to a source line in the visible region (skip top/bottom overscan).
2. Render that source line into a scratch `uint16_t line[1440]`.
3. Horizontally downsample 720 -> 360 by taking every second pixel
   (for super-hires, 1440 -> 360 by taking every fourth).
4. Center-crop 360 -> 320.
5. Byte-swap on write if the panel expects `RG_PIXEL_565_BE`.

Do not allocate scratch per line; keep it in the app struct or use a static
`uint16_t` buffer.

Alternative: submit a 360x284 surface and let `rg_display` scale. This is
simpler but costs more CPU and memory bandwidth, and the result is softer.
Measure both if time allows.

### 4.3 Pixel format

`martendo32/config.h` sets `RG_SCREEN_PIXEL_FORMAT 0` (565_BE). The Amiga core
returns host-order RGB565 (little-endian on RISC-V). Therefore:

- Create the surface as `RG_PIXEL_565_BE`, and swap each pixel while writing
  (`__builtin_bswap16`), **or**
- Create it as `RG_PIXEL_565_LE` and confirm the display driver performs the
  swap. Check the colors on hardware; do not assume.

### 4.4 Submit and pace

```c
if (rg_display_is_busy()) { /* optionally frameskip */ }
rg_display_submit(surface, 0);
```

Call it once per emulated frame. Use `app->frameRate = 50` for PAL. Do not
submit partial updates unless you also implement the core's signature/dirty
tracking; a full 320x240 frame is small enough.

**Acceptance for Phase 4:** AROS or Workbench 1.3 is visible, correctly
oriented, correct colors, stable 50 Hz, no tearing/rolling. Compare against
the desktop host screenshot.

---

## Phase 5 — Audio

### 5.1 Rate and format

The core produces 44100 Hz stereo `int16_t` frames via
`amiga_audio_render(dst, frames)`. Retro-Go's `rg_audio_submit()` takes an
array of `rg_audio_frame_t` (`int16_t left, right`), so the types are
compatible.

```c
int16_t buf[512 * 2];
int frames = amiga_audio_render(buf, 512);
rg_audio_submit((const rg_audio_frame_t *)buf, frames);
```

Set `.sampleRate = 44100` in `rg_config_t`. Do not resample unless CPU
measurements demand it; if you must, 22050 is the usual Retro-Go fallback.

### 5.2 Buffering

The core already has an internal ring (`RING 8192` frames, ~190 ms). Call
`amiga_audio_render` often enough (every frame or two). If audio underruns,
increase the render chunk size before changing the sample rate.

### 5.3 Thread safety

If audio submission runs on core 0 while `amiga_run_frame()` runs on core 1,
confirm the core's ring is safe for one producer/one consumer. It is designed
that way upstream (emulation on core 1, presenter/audio on core 0), but test
with heavy Amiga audio (e.g. a MOD player or a game with music).

**Acceptance for Phase 5:** Paula output is audible, no crackle, no underruns
during a 10-minute run, and volume control from Retro-Go works.

---

## Phase 6 — Input (the largest UX gap)

Martendo32 has only a gamepad. The Amiga needs a joystick, a mouse, and a
keyboard.

### 6.1 Joystick mapping

Use `rg_input_read_gamepad()` and map:

| Retro-Go | Amiga |
|---|---|
| D-pad | `AMIGA_JOY_UP/DOWN/LEFT/RIGHT` |
| A / B | `AMIGA_JOY_FIRE` |
| X / Y | `AMIGA_JOY_FIRE2` |
| START | keyboard menu / special key |
| SELECT | virtual keyboard |
| MENU / OPTION | reserved for Retro-Go menus |

Call `amiga_joystick(bits)` only when the state changes.

### 6.2 Mouse mode

Provide a mode switch (option + chord) between joystick and mouse:

- Accumulate D-pad movement into `dx/dy`, optionally with acceleration.
- Map A/B to left/right mouse buttons.
- Call `amiga_mouse(dx, dy, buttons)` once per frame.
- Reset accumulators after the call.

Workbench and point-and-click games need this; most action games do not.

### 6.3 Virtual keyboard

Retro-Go already provides the UI used by `frodo/main/main.cpp:280`:

- `rg_keyboard_layout_t` (`rg_gui.h:70`)
- `rg_gui_draw_virtual_keyboard()`
- `rg_input_wait_for_key()` for edge detection

Build an Amiga layout (letters, digits, Return, Space, Esc, Ctrl, Alt, Amiga
keys) and map the selected character to an Amiga raw key code
(`amiga_key(code, true/false)`). Raw codes from the core header:

| Key | Code |
|---|---|
| Esc | `$45` |
| Return | `$44` |
| Space | `$40` |
| Shift | `$60` / `$61` |
| Ctrl | `$63` |
| Alt | `$64` / `$65` |
| Amiga keys | `$66` / `$67` |
| Caps Lock | `$62` |

Important: send a release (`false`) for every key you pressed. Some games and
Workbench detect key edges and will behave as if a key is stuck otherwise.
`Ctrl` + both Amiga keys is the core's reset chord (useful for the game menu).

### 6.4 Menus

Reserve `RG_KEY_MENU` and `RG_KEY_OPTION` for Retro-Go
(`rg_gui_game_menu()`, `rg_gui_options_menu()`), exactly as Gwenesis does at
`gwenesis/main/main.c:341`. Never send them to the Amiga.

**Acceptance for Phase 6:** joystick game playable; Workbench navigable with
mouse mode; virtual keyboard can type a filename and run a command; no stuck
keys after exiting the keyboard.

---

## Phase 7 — Storage, disks and settings

### 7.1 Content types

Advertise only what works. Suggested progression:

1. `.adf` (floppy, read/write).
2. `.hdf` (hard disk, read/write) once the model supports IDE.
3. `.rp9` (Amiga Forever package) if you keep the core's RP9 support.
4. `.zip` only if you implement extraction. The core does not read ZIP.

### 7.2 Writable media

`esp32ami` writes ADF/HDF in place. Retro-Go's Frodo port instead copies
writable disks into `RG_BASE_PATH_CACHE` so the user's original image is never
modified (`frodo/main/main.cpp:98`). Decide explicitly:

- **Safe (recommended):** copy `.adf` to
  `RG_BASE_PATH_CACHE "/amiga_df0.adf"` on launch, insert the cache path, and
  call `amiga_floppy_flush()` on exit/menu.
- **Direct:** insert the original path; simpler but games can corrupt the
  source image.

HDF images can be large; either copy to the SD cache or document that the
image is written in place.

### 7.3 ZIP

If advertising `zip`, use the Frodo pattern:

1. Read the first local-file header to learn the entry name.
2. Validate its extension.
3. `rg_storage_unzip_file()` into `RG_BASE_PATH_CACHE`.
4. Insert the extracted file.

Otherwise omit `zip` from the launcher's extension list.

### 7.4 Options menu

Expose via `rg_gui_option_t` and `rg_settings`:

- Model: A500 / A600 / A2000 / A1200 (restart required)
- Kickstart/AROS file
- Chip RAM: 512 KB / 1 MB / 2 MB
- Slow RAM: off / 512 KB
- Fast RAM: off / 1 / 2 / 4 / 8 MB
- PAL / NTSC
- Joystick port: 1 / 2
- Input mode: joystick / mouse
- Mouse speed
- Frameskip: auto / 0 / 1 / 2

For settings that require reallocation (model, RAM, Kickstart), save the
setting and restart the app (`rg_system_restart()` or return to launcher) rather
than trying to hot-swap buffers.

**Acceptance for Phase 7:** a game with disk writes works, the source ADF on
the SD card is unchanged (if cache mode), settings persist across reboots, and
an invalid Kickstart is rejected with a clear message.

---

## Phase 8 — Performance, multicore and memory

### 8.1 Core assignment

Follow the upstream architecture and Gwenesis:

- **Core 1:** `amiga_run_frame()` (CPU + custom chips).
- **Core 0:** presenter (`amiga_render_line_rgb565` + downscale), audio,
  input, display submit.

Use `rg_task_create()`, `rg_task_send()`, `rg_task_receive()` and the
`RG_TASK_MSG_STOP` convention (see `gwenesis/main/main.c:292-315`). Start
single-core first, then split once correctness is established.

### 8.2 Frameskip and pacing

Use `rg_system_timer()` around the frame, `rg_system_tick(busyTime)`, and the
same adaptive frameskip logic as Gwenesis (`gwenesis/main/main.c:488-514`).
PAL is 50 Hz; do not run the loop at 60 Hz.

### 8.3 Memory budget

Allocate persistent buffers once:

| Item | Approx. |
|---|---|
| chip RAM | 0.5-2 MB |
| slow RAM | 0-0.5 MB |
| fast RAM | 0-8 MB |
| Kickstart working copy | 0.25-0.5 MB |
| opcode tables | ~0.6 MB |
| picture records | 6 * 284 * sizeof(row_t) (several MB) |
| framebuffer | 320*240*2 = 150 KB |
| floppy/HD buffers | small |

All in PSRAM; internal RAM stays for stacks, DMA and Retro-Go. Watch
`rg_system_get_stats()` during development.

### 8.4 Flash budget

Martendo32 has 16 MB flash and the full Retro-Go image already approaches
that. Adding `amiga-go` may overflow when building every app. Mitigations:

- Build a reduced image for Amiga users, or
- Drop/omit a large app in that image, or
- Keep `amiga-go` partition at 1 MB and confirm it fits.

Do not silently raise the partition size without checking the total image
size.

**Acceptance for Phase 8:** PAL frame time under 20 ms in Workbench and a
representative game, no audio underruns, free PSRAM reported stable, and the
image flashes successfully.

---

## Phase 9 — Register the app in Retro-Go

### 9.1 `rg_tool.py`

Add to `DEFAULT_APPS`:

```
... frodo pcsx_rearmed mk64-go quake2-go amiga-go
```

Add to `PROJECT_APPS`:

```python
'amiga-go':     [0, 16, 1048576],
```

Increase the size to 1572864 if the binary does not fit. The second field is
the OTA subtype; Retro-Go rewrites OTA indexes automatically, so `16` is fine.

### 9.2 Launcher

In `launcher/main/applications.c`, near the other systems (around line 726):

```c
application("Commodore Amiga", "amiga", "adf hdf rp9", "amiga-go", 0);
```

Add `zip` only if Phase 7.3 is implemented. `crc_offset` is `0` unless you
implement Retro-Go's CRC-based save/cover naming.

### 9.3 Build and image

```bash
python rg_tool.py --target martendo32 build amiga-go
python rg_tool.py --target martendo32 build launcher
python rg_tool.py --target martendo32 build-img amiga-go launcher \
    --no-networking
```

If you want the full image, add the usual apps and watch the 16 MB limit.

**Acceptance for Phase 9:** the Amiga tab appears in the launcher, lists
`.adf` files, launches `amiga-go`, and returns to the launcher with the menu.

---

## Phase 10 — Save states (deferred, but plan it)

`esp32ami` has **no machine serialization**. `amiga_get_status()` is for
display, not restore. Therefore:

- Do not advertise working save states initially.
- Supply `handlers.saveState` / `handlers.loadState` that return `false`, and
  expect Retro-Go to show "Save failed" if the user tries. Better: hide or
  label the feature clearly in the README.
- If save states are required, treat it as a separate project:
  - Serialize CPU registers, chip registers, CIA state, copper/blitter state,
    chip/slow/fast RAM, ROM/RTC/disk state, and the floppy/HDF backing.
  - Version the format and reject mismatched core builds, like Frodo does.
  - Test restore across reset and across app relaunch.

Estimate: substantial. Do not promise it in the first release.

---

## Phase 11 — Testing plan

### 11.1 Host build regression first

Before every hardware test, run the desktop host build with the same ROM/ADF
and compare a screenshot:

```bash
make -C amiga-go/components/amiga/host
./amiga-go/components/amiga/host/build/ami_host \
    /path/to/kickstart.rom 3500 /tmp/out.ppm
```

This separates core regressions from integration regressions.

### 11.2 Hardware smoke tests

1. Boot AROS (no copyrighted ROM needed).
2. Boot Kickstart 1.3, Workbench 1.3 floppy, open a window, move the mouse.
3. Boot Kickstart 3.1 A600 (later) from HDF.
4. Run at least these game classes:
   - OCS platformer / shooter (joystick, chip RAM)
   - game with music (Paula audio)
   - game with disk loading (ADF/DF0)
   - game using raster effects/copper
5. Exercise the virtual keyboard in a cracktro or Workbench shell.
6. Run 30+ minutes; watch free PSRAM, frame time, audio underruns.
7. Save/load state menu: confirm failure is graceful if unimplemented.
8. Exit to launcher and relaunch; confirm the SD image was not corrupted.

### 11.3 Target variants

At minimum test `martendo32`. Then `martendo32-ips` (different panel init) and
`martendo32-hdmi` if display timing changes. The `gb300-p4` and
`esp32-p4-devkit` targets are useful for regression but are not the product.

**Acceptance for Phase 11:** a written test matrix with pass/fail for the
above, including screenshots and measured FPS.

---

## Phase 12 — Documentation, licensing and release

- Add `amiga-go/README.md` modeled on `frodo/README.md`:
  supported models, Kickstart/AROS setup, disk formats, controls, keyboard
  shortcuts, known limitations, and the save-state caveat.
- Keep `components/amiga/LICENSE` and add provenance notes for the vendored
  snapshot (commit hash of `esp32ami` and `esp32emu`).
- State clearly that no Kickstart, Workbench or game images are included.
- Document the IDF version actually used and the Martendo32 build command.
- Update `CHANGELOG.md` and, if applicable, the top-level `README.md`.

---

## Appendix A — Suggested file map

```
amiga-go/
  CMakeLists.txt
  components/
    amiga/                 # vendored, platform-independent core
    musashi/               # vendored 68000 core
    emudbg/                # only if not stubbed
  main/
    CMakeLists.txt
    main.c                 # Retro-Go glue: app, loop, input, display, audio
    amiga_rom.c/.h         # Kickstart/AROS discovery and validation
    amiga_input.c/.h       # keymap, virtual keyboard, mouse mode
    amiga_video.c/.h       # crop/downscale/byte-swap
    amiga_storage.c/.h     # ADF/HDF/RP9/ZIP and cache handling
```

Keeping these in separate files makes it much easier to review and to port the
same glue to another P4 target later.

## Appendix B — API mapping cheat sheet

| Retro-Go | esp32ami core |
|---|---|
| `rg_system_init()` | app setup, not core |
| `rg_system_tick()` | frame pacing |
| `rg_display_submit()` | `amiga_render_line_rgb565()` into surface |
| `rg_audio_submit()` | `amiga_audio_render()` |
| `rg_input_read_gamepad()` | `amiga_joystick()` / `amiga_mouse()` |
| `rg_gui_draw_virtual_keyboard()` | `amiga_key()` |
| `rg_emu_reset()` handler | `amiga_reset()` |
| `rg_emu_save_state()` handler | not available |
| `rg_emu_load_state()` handler | not available |
| `rg_emu_screenshot()` handler | `rg_surface_save_image_file()` |
| `app->romPath` | `amiga_floppy_insert()` / `amiga_hd_insert()` |
| `RG_BASE_PATH_BIOS` | Kickstart/AROS discovery |

## Appendix C — Known risks, ranked

1. **IDF 5.3.5 vs 5.5.** Resolve in Phase 0/1; everything else depends on it.
2. **No save states.** Do not let the UI imply otherwise.
3. **Input ergonomics.** Keyboard and mouse on a gamepad is the main reason
   users may reject the port. Budget real time for the virtual keyboard and
   mouse mode.
4. **Display downscale cost.** 720x568 -> 320x240 per frame can dominate CPU
   if done naively. Render only the lines you display.
5. **16 MB flash budget.** The full app set may no longer fit.
6. **PSRAM bandwidth.** The picture-record architecture depends on PSRAM;
   profile with the real panel and real SD card.
7. **Licensing hygiene.** Never commit Kickstart or game images.

## Appendix D — Definition of done for the first usable build

- [ ] `amiga-go` builds for `martendo32` with the documented IDF version.
- [ ] Launcher lists Amiga content and launches the app.
- [ ] AROS and Kickstart 1.3 boot to a usable Workbench.
- [ ] 320x240 PAL output is correct, stable and 50 Hz.
- [ ] Paula audio works with no underruns in a 10-minute test.
- [ ] Joystick, mouse mode and virtual keyboard all work without stuck keys.
- [ ] ADF load works; writable disk handling does not corrupt the source.
- [ ] Exit to launcher and relaunch works reliably.
- [ ] README documents setup, controls, limitations and ROM policy.
- [ ] No copyrighted ROMs or software are included in the repository/image.
