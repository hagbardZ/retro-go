# Martendo32
- Build Guide & BOM: https://github.com/hagbardZ/Martendo32
- Status: Complete

Command to build (ESP-IDF v5.3.5): `python rg_tool.py --target martendo32 build-img --no-networking`

python rg_tool.py --target martendo32 build-img launcher snes9x retro-core gwenesis sm64-go --no-networking
python rg_tool.py --target martendo32 release launcher snes9x retro-core gwenesis sm64-go --no-networking
Flash size: 15.688 MB

## Audio
- `RG_AUDIO_MONO_OUT` is enabled for the Martendo32 (single speaker): the Amiga
  stereo output is mixed to mono.

## Hardware
- Martendo32
- Wireless-Tag ESP32-P4 WT0132P4-A1-N16R32
- ST7789V 320*240 2.8" SPI Display 
- SD card over SDMMC (4 bits)
- NS4168 DAC
- TP4056 charge chip
- 2.5mm audio jack
- Volume wheel
- USB-C for charging and firmware updates


## Input
- The directional pad (Up/Down/Left/Right) is driven by an Adafruit 2765
  Mini 2-Axis Analog Thumbstick over the ADC (`RG_GAMEPAD_ADC_MAP`), wired
  X = GPIO52 and Y = GPIO53 (potentiometers between +3V3 and GND).
  The up/down/left/right buttons are therefore not used.

- The d-pad source can be selected with `RG_GAMEPAD_DPAD_SOURCE_STICK`
  (1 = Adafruit 2765 analog thumbstick over ADC, default; 0 = d-pad buttons
  over GPIO).


## Images

<img width="1718" height="1050" alt="Martendo32" src="https://github.com/user-attachments/assets/7c5d70eb-7909-46c6-8a48-3085a08f871d" />
