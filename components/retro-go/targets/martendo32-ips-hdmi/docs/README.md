
# Martendo32-IPS
- Build Guide & BOM: https://github.com/hagbardZ/Martendo32
- Status: Complete

Command to build (ESP-IDF v5.3.5): `python rg_tool.py --target martendo32-IPS build-img --no-networking`

python rg_tool.py --target martendo32 build-img launcher snes9x retro-core gwenesis sm64-go --no-networking
python rg_tool.py --target martendo32 release launcher snes9x retro-core gwenesis sm64-go --no-networking
Flash size: 15.688 MB


## Hardware
- Martendo32
- Wireless-Tag ESP32-P4 WT0132P4-A1-N16R32
- Display: https://de.aliexpress.com/item/1005005797273047.html?spm=a2g0o.order_list.order_list_main.23.4af05c5fI1S7JY&gatewayAdapt=glo2deu
- ST7789V 320*240 2.8" SPI Display - IPS full view screen RGB565
- SD card over SDMMC (4 bits)
- NS4168 DAC
- TP4056 charge chip
- 2.5mm audio jack
- Volume wheel
- USB-C for charging and firmware updates


## Images
<img width="1718" height="1050" alt="Martendo32" src="https://github.com/user-attachments/assets/e1ad5767-983e-4a02-aa00-4a6fad012994" />

