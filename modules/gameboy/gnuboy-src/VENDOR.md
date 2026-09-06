# Vendored gnuboy core

Source: https://github.com/ducalex/retro-go — `retro-core/components/gnuboy/`
Commit: `4ced120669750ca7228fd0414211430c1d923166` (master, fetched 2026-07-02)
License: GPL-2 (see COPYING). CREDITS retained.

Files: cpu.c hw.c lcd.c sound.c gnuboy.c + cpu.h hw.h lcd.h sound.h gnuboy.h tables.h

Local modifications (all tagged MESHPUNK in-source):
- gnuboy.c/gnuboy.h: MBC3 real-time catch-up. The .sav RTC footer's timestamp
  slots (rtc_buf[10]/[11]) carry the real unix time from `gb_host_unix_time()`
  (platform glue; 0 = unknown) instead of upstream's RTC_BASE+counter value,
  and the new `gnuboy_rtc_catchup()` — called by the module after sram AND
  savestate loading — advances the clock by the elapsed off time. Old-format
  footers (>= RTC_BASE) are recognized and left alone.
Built WITHOUT `-DRETRO_GO` so logging stays on plain printf (host-exported).
