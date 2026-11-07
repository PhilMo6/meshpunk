// wio_l2_lgfx.h — LovyanGFX device for the Wio Tracker L2 panel: NV3031B,
// 240x320 native portrait, quad-SPI on SPI3. Header-only so the main
// firmware's display backend and the updater build share one definition.
//
// Every bus and panel value is the wadamesh WioTrackerL2Display
// configuration (hardware-tested on this board with LovyanGFX 1.2.27 on
// espressif32@6.11). offset_rotation 1 makes LovyanGFX rotation 0 the
// board's native landscape (320x240). The panel has no reset line on a GPIO:
// the reset pulse is expander P06, driven by wio_l2_board_init() before
// init() runs.

#pragma once

#if defined(BOARD_WIO_L2)

#include <LovyanGFX.hpp>
#include <lgfx/v1/panel/Panel_NV3031B.hpp>

#include "wio_l2_board.h"

class WioL2Lgfx : public lgfx::LGFX_Device {
  lgfx::Panel_NV3031B _panel;
  lgfx::Bus_SPI       _bus;

 public:
  WioL2Lgfx() {
    {
      auto cfg = _bus.config();
      cfg.spi_host    = SPI3_HOST;
      cfg.spi_mode    = 3;
      cfg.freq_write  = 75000000;
      cfg.freq_read   = 16000000;
      cfg.spi_3wire   = false;
      cfg.use_lock    = true;
      cfg.dma_channel = SPI_DMA_CH_AUTO;
      cfg.pin_sclk    = WIO_L2_LCD_SCLK;
      cfg.pin_miso    = -1;
      cfg.pin_mosi    = -1;
      cfg.pin_dc      = -1;
      cfg.pin_io0     = WIO_L2_LCD_IO0;
      cfg.pin_io1     = WIO_L2_LCD_IO1;
      cfg.pin_io2     = WIO_L2_LCD_IO2;
      cfg.pin_io3     = WIO_L2_LCD_IO3;
      _bus.config(cfg);
      _panel.setBus(&_bus);
    }
    {
      auto cfg = _panel.config();
      cfg.pin_cs          = WIO_L2_LCD_CS;
      cfg.pin_rst         = -1;
      cfg.pin_busy        = -1;
      cfg.panel_width     = 240;
      cfg.panel_height    = 320;
      cfg.memory_width    = 240;
      cfg.memory_height   = 320;
      cfg.offset_x        = 0;
      cfg.offset_y        = 0;
      cfg.offset_rotation = 1;
      cfg.readable        = false;
      cfg.invert          = true;
      cfg.rgb_order       = true;
      cfg.dlen_16bit      = false;
      cfg.bus_shared      = false;
      _panel.config(cfg);
    }
    setPanel(&_panel);
  }
};

#endif // BOARD_WIO_L2
