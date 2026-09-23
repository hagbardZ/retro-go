# MP3 Player for Retro-Go

A music player app for the [Retro-Go](https://github.com/ducalex/retro-go)
ecosystem. It decodes MP3 audio with [Helix](https://github.com/ultrafunkamsterdam/helix)
(player core in `components/libhelix-mp3`), plays files from the SD card and
from a plugged-in USB mass storage drive (FAT32), and shows a
handheld-friendly now-playing screen. Developed on and primarily tested for
the ESP32-P4 / Martendo32 target.

<img width="679" height="821" alt="MartendoMp3PlayerScreen" src="https://github.com/user-attachments/assets/371ffcf4-8794-4c0a-9626-283b8e806c41" />

## Features

- **USB mass storage** – a FAT32 USB stick plugged into the OTG port is
  mounted as `/usb0`; its MP3s are browsed with the file picker, played, and
  included in playlists
- **File picker** (Y) to browse and play any `.mp3` on the SD card or USB stick
- **Playlist** of the `music/` folder and of mounted USB drives with
  previous/next track
- **Random play** (X) and **Repeat** (B) modes
- **Seeking** by ±5 seconds (left/right) with accurate byte-based seek
- **ID3v2** (v2.2, v2.3, v2.4) and **ID3v1/APEv2** tag reading, including
  UTF-8 text handling and numeric genre lookup (tags shown on the player screen)
- **VBR support** via Xing/Info headers, including the 100-entry seek table,
  plus fallback bitrate sampling for accurate duration and seeking
- **Pause / resume** (A)
- **Session resume** – remembers the last track, position, random and repeat
  settings and restores them on the next launch
- Bitrate, sample rate, audio driver and volume shown in the status bar
- No network required (networking is disabled in the build)

## Installation

1. Install Retro-Go and format your SD card.
2. Place your MP3 files in the `music/` folder at the root of the SD card:
   `music/`
3. Build and flash the app as any other Retro-Go application.

### Playing from a USB stick

1. Use a **FAT32-formatted** USB stick (the Pocket Pico / `usb_host_msc`
   driver supports FAT and exFAT volumes).
2. Plug it into the handheld's USB host / OTG port before or while the app is
   running. When detected it is mounted as `/usb0` (see the boot log:
   `rg_usb: mounted /usb0`).
3. Press **Y** for the file picker and browse into the stick to play any MP3.
4. Stick support requires the target build to define `RG_STORAGE_USBOTG_HOST`
   (enabled for the ESP32-P4 / Martendo32 targets).

## USB mass storage support

USB Host Mass Storage is provided by the Retro-Go system layer, not by this
app. It is implemented in `components/retro-go/rg_storage.c` and exposed by
`components/retro-go/rg_storage.h` (`rg_storage_usb_mount_count()`,
`rg_storage_usb_is_mounted()`, the `/usb0` path helper
`rg_storage_usb_mount_path()`...).

**Files changed for USB host storage:**

- `components/retro-go/rg_storage.c` – USB Host + MSC driver integration:
  a background task (`rg_usb_host_task`) installs the USB host, registers the
  FAT on a plug-in event and mounts it at `/usb0`; an event task
  (`rg_usb_event_task`) handles connect/disconnect; on the ESP32-P4 it also
  powers down the OTG11/FSLS PHY and re-asserts GPIO26/27 (buttons A/B) as
  inputs with pull-up, because the FSLS bus-idle state was holding the gamepad
  lines low.
- `components/retro-go/rg_storage.h` – USB mount API and `RG_STORAGE_USB_MOUNT_PATH`.
- `components/retro-go/CMakeLists.txt` / `components/retro-go/idf_component.yml`
  – depend on the `espressif/usb_host_msc` driver component.
- `components/retro-go/targets/<target>/config.h` + `sdkconfig` – enable
  `RG_STORAGE_USBOTG_HOST` per target.
- `mp3-player/main/main.c` – file picker start path and playlist/usb handling:
  when a USB drive is mounted the picker opens inside `/usb0` (it cannot open
  at `/` because ESP-IDF never registers a VFS layer at the root, so
  `opendir("/")` fails), and the playlist scan includes mounted USB drives.

**Known hardware notes:**

- The ESP32-P4 UTMI PHY used for the host port is High-Speed-only; USB 1.1
  (Full-Speed-only) devices that are marginal on High-Speed may fail to
  enumerate (`CHECK_SHORT_DEV_DESC FAILED`). Use a High-Speed-capable stick (a
  full-speed-only hub or USB-1.1-only isolator is the only way to force every
  device to 12 Mbps).
- All three of the developer's test sticks work on a PC; only sticks that
  enumerate reliably at High Speed work on the handheld.

## Controls

| Button | Action |
|---|---|
| A | Pause / resume |
| B | Toggle repeat |
| X | Toggle random play |
| Y | Open file picker |
| Up / Down | Previous / next track |
| Left / Right | Seek −5 s / +5 s |
| MENU | Save and exit |

## Credits

- **Retro-Go:** The ESP32 emulation ecosystem providing the launcher, display,
  audio, input and system services.
- **Helix:** The MP3 decode core, ported and optimised for ESP32 targets.