#include "display_driver.h"
#include <Wire.h>

DisplayDriver Display;

WaveshareLGFX::WaveshareLGFX() {
    {
        auto cfg = _panel_instance.config();
        cfg.memory_width  = LCD_WIDTH;   // 800
        cfg.memory_height = LCD_HEIGHT;  // 480
        cfg.panel_width   = LCD_WIDTH;   // 800
        cfg.panel_height  = LCD_HEIGHT;  // 480
        cfg.offset_x      = 0;
        cfg.offset_y      = 0;
        _panel_instance.config(cfg);
    }

    {
        auto cfg = _panel_instance.config_detail();
        cfg.use_psram = 1; // 800x480x2 = 768KB frame buffer allocated in Octal PSRAM
        _panel_instance.config_detail(cfg);
    }

    {
        auto cfg = _bus_instance.config();
        cfg.panel = &_panel_instance;

        // RGB Bus Pins (16-bit 565)
        cfg.pin_d0  = LCD_PIN_B0; // 14
        cfg.pin_d1  = LCD_PIN_B1; // 38
        cfg.pin_d2  = LCD_PIN_B2; // 18
        cfg.pin_d3  = LCD_PIN_B3; // 17
        cfg.pin_d4  = LCD_PIN_B4; // 10
        cfg.pin_d5  = LCD_PIN_G0; // 39
        cfg.pin_d6  = LCD_PIN_G1; // 0
        cfg.pin_d7  = LCD_PIN_G2; // 45
        cfg.pin_d8  = LCD_PIN_G3; // 48
        cfg.pin_d9  = LCD_PIN_G4; // 47
        cfg.pin_d10 = LCD_PIN_G5; // 21
        cfg.pin_d11 = LCD_PIN_R0; // 1
        cfg.pin_d12 = LCD_PIN_R1; // 2
        cfg.pin_d13 = LCD_PIN_R2; // 42
        cfg.pin_d14 = LCD_PIN_R3; // 41
        cfg.pin_d15 = LCD_PIN_R4; // 40

        // Timing & Sync Pins
        cfg.pin_henable = LCD_PIN_DE;    // 5
        cfg.pin_vsync   = LCD_PIN_VSYNC; // 3
        cfg.pin_hsync   = LCD_PIN_HSYNC; // 46
        cfg.pin_pclk    = LCD_PIN_PCLK;  // 7
        cfg.freq_write  = LCD_FREQ_HZ;   // 14 MHz

        cfg.hsync_polarity    = 0;
        cfg.hsync_front_porch = LCD_HFP; // 20
        cfg.hsync_pulse_width = LCD_HPW; // 10
        cfg.hsync_back_porch  = LCD_HBP; // 10

        cfg.vsync_polarity    = 0;
        cfg.vsync_front_porch = LCD_VFP; // 10
        cfg.vsync_pulse_width = LCD_VPW; // 10
        cfg.vsync_back_porch  = LCD_VBP; // 10

        cfg.pclk_idle_high    = 0;

        _bus_instance.config(cfg);
        _panel_instance.setBus(&_bus_instance);
    }

    {
        auto cfg = _touch_instance.config();
        cfg.x_min           = 0;
        cfg.x_max           = LCD_WIDTH - 1;
        cfg.y_min           = 0;
        cfg.y_max           = LCD_HEIGHT - 1;
        cfg.pin_int         = TOUCH_PIN_INT;
        cfg.pin_rst         = TOUCH_PIN_RST; // Handled via CH422G EXIO1
        cfg.bus_shared      = true;
        cfg.offset_rotation = 0;
        cfg.i2c_port        = 0;
        cfg.pin_sda         = TOUCH_I2C_SDA;
        cfg.pin_scl         = TOUCH_I2C_SCL;
        cfg.freq            = 400000;
        cfg.i2c_addr        = TOUCH_I2C_ADDR; // 0x5D
        _touch_instance.config(cfg);
        _panel_instance.setTouch(&_touch_instance);
    }

    setPanel(&_panel_instance);
}

DisplayDriver::DisplayDriver() : _brightness(DEFAULT_BRIGHTNESS) {}

bool DisplayDriver::initIOExpander() {
    Wire.begin(TOUCH_I2C_SDA, TOUCH_I2C_SCL);
    delay(10);

    // CH422G Init: set EXIO pins to output mode
    Wire.beginTransmission(CH422G_I2C_ADDR);
    Wire.write(0x01);
    if (Wire.endTransmission() != 0) {
        Serial.println("[DISPLAY WARN] CH422G IO Expander mode write returned non-zero");
    }

    // Pull TP_RST, LCD_BL, LCD_RST, SD_CS high (0x1E)
    // Bit 1: TP_RST, Bit 2: LCD_BL, Bit 3: LCD_RST, Bit 4: SD_CS, Bit 5: USB_SEL (0 = CAN mode)
    Wire.beginTransmission(CH422G_I2C_DATA);
    Wire.write(0x1E);
    if (Wire.endTransmission() != 0) {
        Serial.println("[DISPLAY WARN] CH422G IO Expander data write returned non-zero");
        return false;
    }
    delay(50);
    return true;
}

bool DisplayDriver::init() {
    if (ESP.getFreePsram() < (uint32_t)(LCD_WIDTH * LCD_HEIGHT * 2)) {
        Serial.printf("[DISPLAY WARN] Low PSRAM: %u bytes available, %d required for 800x480 FB\n",
                      ESP.getFreePsram(), LCD_WIDTH * LCD_HEIGHT * 2);
    }

    Serial.println("[DISPLAY] Initializing CH422G IO Expander...");
    initIOExpander();

    Serial.println("[DISPLAY] Initializing ST7262 RGB Panel and GT911 Touch via LovyanGFX...");
    if (!_lcd.init()) {
        Serial.println("[DISPLAY ERROR] _lcd.init() failed!");
        return false;
    }

    _lcd.setColorDepth(16);
    _lcd.setRotation(0); // Landscape: 800 x 480
    _lcd.fillScreen(0x0000);

    Serial.printf("[DISPLAY OK] Resolution: %d x %d\n", _lcd.width(), _lcd.height());
    Serial.printf("[DISPLAY OK] Free Heap: %u bytes, Free PSRAM: %u bytes\n", ESP.getFreeHeap(), ESP.getFreePsram());
    return true;
}

void DisplayDriver::setBrightness(uint8_t brightness) {
    _brightness = brightness;
    // On Waveshare 4.3", LCD_BL on CH422G is digital ON/OFF (Bit 2).
    Wire.beginTransmission(CH422G_I2C_DATA);
    if (brightness > 20) {
        Wire.write(0x1E); // Backlight ON
    } else {
        Wire.write(0x1A); // Backlight OFF
    }
    Wire.endTransmission();
}

void DisplayDriver::clear(uint16_t color) {
    _lcd.fillScreen(color);
}

bool DisplayDriver::getTouch(int16_t *x, int16_t *y) {
    return _lcd.getTouch(x, y);
}
