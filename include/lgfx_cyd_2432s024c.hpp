// LovyanGFX board class for the Sunton ESP32-2432S024C
//   2.4" ILI9341 240x320 over SPI2 + CST820 capacitive touch over I2C0
#pragma once

#ifndef LGFX_USE_V1
#define LGFX_USE_V1
#endif
#include <LovyanGFX.hpp>

class LGFX_CYD_2432S024C : public lgfx::LGFX_Device {
    lgfx::Bus_SPI       _bus;
    lgfx::Panel_ILI9341 _panel;
    lgfx::Light_PWM     _light;
    lgfx::Touch_CST816S _touch;   // CST820 speaks the same protocol

public:
    LGFX_CYD_2432S024C() {
        {   // SPI bus to the display
            auto cfg = _bus.config();
            cfg.spi_host    = SPI2_HOST;
            cfg.spi_mode    = 0;
            cfg.freq_write  = 40000000;
            cfg.freq_read   = 16000000;
            cfg.spi_3wire   = false;
            cfg.use_lock    = true;
            cfg.dma_channel = SPI_DMA_CH_AUTO;
            cfg.pin_sclk    = 14;
            cfg.pin_mosi    = 13;
            cfg.pin_miso    = 12;
            cfg.pin_dc      = 2;
            _bus.config(cfg);
            _panel.setBus(&_bus);
        }
        {   // ILI9341 panel
            auto cfg = _panel.config();
            cfg.pin_cs           = 15;
            cfg.pin_rst          = -1;   // tied to EN
            cfg.pin_busy         = -1;
            cfg.panel_width      = 240;
            cfg.panel_height     = 320;
            cfg.offset_x         = 0;
            cfg.offset_y         = 0;
            cfg.offset_rotation  = 0;    // 0-3 rotate, 4-7 rotate + mirror
            cfg.dummy_read_pixel = 8;
            cfg.dummy_read_bits  = 1;
            cfg.readable         = true;
            cfg.invert           = false;
            cfg.rgb_order        = false;
            cfg.dlen_16bit       = false;
            cfg.bus_shared       = false;
            _panel.config(cfg);
        }
        {   // Backlight PWM
            auto cfg = _light.config();
            cfg.pin_bl      = 27;
            cfg.invert      = false;
            cfg.freq        = 12000;
            cfg.pwm_channel = 7;
            _light.config(cfg);
            _panel.setLight(&_light);
        }
        {   // CST820 capacitive touch, I2C
            auto cfg = _touch.config();
            cfg.x_min           = 0;
            cfg.x_max           = 239;
            cfg.y_min           = 0;
            cfg.y_max           = 319;
            cfg.pin_int         = 22;
            cfg.pin_rst         = 25;
            cfg.bus_shared      = false;
            cfg.offset_rotation = 0;
            cfg.i2c_port        = 0;
            cfg.i2c_addr        = 0x15;
            cfg.pin_sda         = 33;
            cfg.pin_scl         = 32;
            cfg.freq            = 400000;
            _touch.config(cfg);
            _panel.setTouch(&_touch);
        }
        setPanel(&_panel);
    }
};
