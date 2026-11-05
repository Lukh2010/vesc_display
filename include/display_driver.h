#pragma once

#define LGFX_USE_V1
#include <Arduino.h>
#include <LovyanGFX.hpp>
#ifdef NATIVE_SIMULATOR
#include "../tools/simulator/include/display_driver.h"
#else
#include <lgfx/v1/platforms/esp32s3/Bus_RGB.hpp>
#include <lgfx/v1/platforms/esp32s3/Panel_RGB.hpp>
#include "board_config.h"

// Complete LGFX Device configuration for Waveshare ESP32-S3-Touch-LCD-4.3
// ST7262 RGB panel (800x480) + Goodix GT911 Capacitive Touch
class WaveshareLGFX : public lgfx::LGFX_Device {
    lgfx::Panel_RGB   _panel_instance;
    lgfx::Bus_RGB     _bus_instance;
    lgfx::Touch_GT911 _touch_instance;

public:
    WaveshareLGFX();
};

class DisplayDriver {
public:
    DisplayDriver();
    bool init();
    void setBrightness(uint8_t brightness); // 0 (off) to 255 (max)
    void clear(uint16_t color = 0x0000);
    
    WaveshareLGFX& getLgfx() { return _lcd; }
    LovyanGFX* getCanvas() { return &_lcd; }
    void pushCanvas() { _lcd.display(); }

    // Capacitive Touch Query
    bool getTouch(int16_t *x, int16_t *y);

private:
    bool initIOExpander();
    WaveshareLGFX _lcd;
    uint8_t       _brightness;
};

extern DisplayDriver Display;
#endif
