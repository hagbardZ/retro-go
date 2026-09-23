# Resume — USB Host on Martendo32 (ESP32-P4)

Last save: Sep 22 (late). Work is uncommitted in the working tree (HEAD `4d2d682d`).

## Status: A/B button issue FIXED
Root cause: the OTG11/FSLS PHY block was holding GPIO26/27 (buttons A/B) low.
Fix in `components/retro-go/rg_storage.c` `rg_usb_host_task` (after `usb_host_install`):
- `LP_AON_CLKRST.hp_usb_clkrst_ctrl0.usb_otg11_48m_clk_en = 0`
- `HP_SYS_CLKRST.soc_clk_ctrl1.reg_usb_otg11_sys_clk_en = 0`
- `LP_AON_CLKRST.hp_usb_clkrst_ctrl1.rst_en_usb_otg11` 1 then 0
- `USB_WRAP.otg_conf.usb_pad_enable = 0`
- re-assert GPIO26/27 as input + pull-up via `gpio_config()`

Verified in capture `/tmp/diag19.log`: `DIAG gp26=1 gp27=1 raw=0x00000000 otg_conf=0x00000000`, `rg_usb: host installed`, `OTG11 FSLS PHY powered down`. User confirmed MP3 player launches, A/B work.

DIAG instrumentation is still present (enabled by `#if 1`-style additions):
- `launcher/main/main.c` DIAG line prints raw gamepad + clock/select regs every 20 loops.
- Full decode: gp26/gp27 input level, gamepad (debounced), raw, mux26/27 (0x1b00 = input+pullup), enable=0x00fe0000, in (incl. bits 26/27), otg_conf, lp_usb (LP_SYS.usb_ctrl), clk0 (hp_usb_clkrst_ctrl0), clk1 (soc_clk_ctrl1).

## Tomorrow's steps
1. Plug FAT32 USB stick into module pins **16/17** (HS/UTMI dedicated pads).
   - Watch serial for `rg_usb: mounted /usb0` (looked for in `/tmp/diag20.log` but no device detected yet).
2. Play MP3 from `/usb0` in the mp3-player app to confirm host + MSC fully work.
3. On success: strip DIAG logging from main.c + rg_storage.c, rebuild, final flash.

## Commands / procedure
- Build: `env -i HOME=/home/steff PATH=/usr/bin:/bin bash -c 'source /home/steff/.espressif/v5.3.5/esp-idf/export.sh >/dev/null 2>&1 && cd /home/steff/Dokumente/Martendo32/retro-go && python rg_tool.py --target martendo32-ips release launcher retro-core mp3-player --no-networking'`
- Image: `retro-go_bootstrapped-19-g4d2d6-dirty_martendo32-ips.img`
- Flash (full image, note rg_tool.py flash path is broken — needs esptool directly):
  `esptool.py --chip esp32p4 -p /dev/ttyACM0 -b 460800 write_flash 0x0 <img>`
- Serial capture (pyserial, 2000000 baud, wake via DTR=false/RTS=true 0.3s then RTS=false); port flips ttyACM0/ttyACM1.

## Gotchas
- `rg_tool.py flash` iterates ALL apps including prboom-go (not built) → use full-image esptool flash instead (it already flashed launcher+retro-core last time; full image was flashed cleanly after).
- `usb_wrap_ll_enable_bus_clock`/`usb_wrap_ll_reset_register` use `__DECLARE_RCC_ATOMIC_ENV`, not available outside IDF driver code → write the clock/reset bits directly (as done).
- `rg` CLI not installed; use the Grep tool or `grep -n`.