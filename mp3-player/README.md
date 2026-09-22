# MP3 Player for Retro-Go

A music player app for the [Retro-Go](https://github.com/ducalex/retro-go)
ecosystem. It decodes MP3 audio with [Helix](https://github.com/ultrafunkamsterdam/helix)
(player core in `components/libhelix-mp3`), plays files from the SD card and
shows a handheld-friendly now-playing screen. Developed on and primarily
tested for the ESP32-P4 / Martendo32 target.

<img width="679" height="821" alt="MartendoMp3PlayerScreen" src="https://github.com/user-attachments/assets/371ffcf4-8794-4c0a-9626-283b8e806c41" />

## Features

- **File picker** (Y) to browse and play any `.mp3` on the SD card
- **Playlist** of the `music/` folder with previous/next track
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