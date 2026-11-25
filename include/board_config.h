#pragma once

#include <Arduino.h>

// ==============================================================================
// Waveshare ESP32-S3-Touch-LCD-4.3 & 7.0 Dual-Target Board Configuration
// ==============================================================================
// Hardware: ESP32-S3 (16MB Flash, 8MB Octal PSRAM - qio_opi)
// Display:  ST7262 RGB Parallel Interface (800x480 landscape, harmonized 12 MHz)
// Touch:    Goodix GT911 Capacitive Touch Controller (I2C)
// IO Exp.:  CH422G I2C Expander (Backlight on EXIO2, Touch RST on EXIO1)

#if defined(WAVESHARE_LCD_7_0) && WAVESHARE_LCD_7_0
#define BOARD_DISPLAY_NAME "Waveshare ESP32-S3-Touch-LCD-7.0"
#else
#ifndef WAVESHARE_LCD_4_3
#define WAVESHARE_LCD_4_3 1
#endif
#define BOARD_DISPLAY_NAME "Waveshare ESP32-S3-Touch-LCD-4.3"
#endif

// Display Dimensions
#ifndef LCD_WIDTH
#define LCD_WIDTH   800
#endif
#ifndef LCD_HEIGHT
#define LCD_HEIGHT  480
#endif

// ST7262 RGB Parallel Timing & Clock (Harmonized for Waveshare 4.3" & 7.0" 800x480)
#define LCD_PIN_HSYNC   46
#define LCD_PIN_VSYNC   3
#define LCD_PIN_DE      5
#define LCD_PIN_PCLK    7
#ifndef LCD_FREQ_HZ
#define LCD_FREQ_HZ     12000000 // 12 MHz (exact 240MHz/20 integer divisor, zero jitter on 4.3" & 7.0")
#endif

// ST7262 RGB Timing Parameters (Conservative blanking porches for 4.3" & 7.0" stability)
#ifndef LCD_HPW
#define LCD_HPW         8
#endif
#ifndef LCD_HBP
#define LCD_HBP         16
#endif
#ifndef LCD_HFP
#define LCD_HFP         20
#endif
#ifndef LCD_VPW
#define LCD_VPW         4
#endif
#ifndef LCD_VBP
#define LCD_VBP         12
#endif
#ifndef LCD_VFP
#define LCD_VFP         12
#endif

// ST7262 RGB Bus Data Pins (16-bit RGB565)
// Blue: B0 - B4 (D0 - D4)
#define LCD_PIN_B0      14
#define LCD_PIN_B1      38
#define LCD_PIN_B2      18
#define LCD_PIN_B3      17
#define LCD_PIN_B4      10

// Green: G0 - G5 (D5 - D10)
#define LCD_PIN_G0      39
#define LCD_PIN_G1      0
#define LCD_PIN_G2      45
#define LCD_PIN_G3      48
#define LCD_PIN_G4      47
#define LCD_PIN_G5      21

// Red: R0 - R4 (D11 - D15)
#define LCD_PIN_R0      1
#define LCD_PIN_R1      2
#define LCD_PIN_R2      42
#define LCD_PIN_R3      41
#define LCD_PIN_R4      40

// Goodix GT911 Capacitive Touch Controller (I2C)
#define TOUCH_I2C_SDA   8
#define TOUCH_I2C_SCL   9
#define TOUCH_PIN_INT   4
#define TOUCH_PIN_RST   -1       // Controlled via CH422G EXIO1
#define TOUCH_I2C_ADDR  0x5D     // Standard Goodix GT911 I2C address

// CH422G I2C IO Expander
#define CH422G_I2C_ADDR 0x24
#define CH422G_I2C_DATA 0x38
#define EXIO_TP_RST     (1 << 1) // Bit 1: Touch Reset
#define EXIO_LCD_BL     (1 << 2) // Bit 2: LCD Backlight Enable
#define EXIO_LCD_RST    (1 << 3) // Bit 3: LCD Reset
#define EXIO_SD_CS      (1 << 4) // Bit 4: MicroSD CS
#define EXIO_USB_SEL    (1 << 5) // Bit 5: USB / CAN multiplexer select

// Backlight Default Brightness (0 - 255)
#define DEFAULT_BRIGHTNESS 230

// ==============================================================================
// Optional Hardware Buttons (Fallback if wired; primary control is GT911 Touch)
// ==============================================================================
#define PIN_BTN_1       -1   // No physical buttons by default
#define PIN_BTN_2       -1
// Onboard BOOT button (GPIO 0)
// WARNING: On physical ESP32-S3 hardware, GPIO 0 is the strapping pin tied to the USB-UART DTR/RTS circuit.
// When plugged into USB, DTR/RTS holds GPIO 0 LOW, which falsely triggered the continuous FOC horn & theft alarm!
// For physical hardware, disable GPIO 0 (set to -1). Keep 0 only for native simulator testing.
#if defined(NATIVE_SIMULATOR)
#define PIN_BTN_BOOT    0
#else
#define PIN_BTN_BOOT    -1
#endif

// ==============================================================================
// VESC Controller Interfaces (Spintend Ubox / Flipsky / VESC on Waveshare 4.3" Pins)
// ==============================================================================
// 1 = Live VESC CAN Bus (onboard TJA1051, GPIO 20 TX / GPIO 19 RX, 500k baud)
// 0 = Live VESC UART (GPIO 43 TX / GPIO 44 RX, 115200 baud)
#define VESC_USE_CAN_BUS    1

// Option 1: VESC UART Interface (Serial1)
// Connect VESC TX -> ESP32 RX (GPIO 44)
// Connect VESC RX -> ESP32 TX (GPIO 43)
#define VESC_UART_PORT      Serial1
#define VESC_UART_TX_PIN    43
#define VESC_UART_RX_PIN    44
#define VESC_UART_BAUDRATE  115200

// Option 2: VESC CAN Bus Interface (onboard TJA1051 transceiver)
// Connect VESC CANH -> Screw terminal CANH
// Connect VESC CANL -> Screw terminal CANL
#define CAN_TX_PIN          GPIO_NUM_20
#define CAN_RX_PIN          GPIO_NUM_19
#define CAN_BAUDRATE_VESC   500000 // 500k standard VESC CAN
#define VESC_CAN_ID_DEFAULT 76     // Target VESC Controller ID (Spintend Ubox = 76)

// ==============================================================================
// Accessory Interfaces (Cockpit Header Pins)
// ==============================================================================
// Headlight 12V MOSFET Trigger Switch (Dual D4184 Module, 3.3V Logic)
// Note: GPIO 43 is shared with USB UART0 TX on the Waveshare board.
// Set to -1 during USB serial debugging / flashing to avoid breaking Serial output.
#define PIN_HEADLIGHT_MOSFET -1

// ATGM336H / NEO-M8N GPS Module (NMEA 9600 Baud)
// Connect GPS Module TX -> Waveshare UART Header Pin RX (GPIO 44)
// Note: GPIO 44 is shared with USB UART0 RX on the Waveshare board.
// Set to -1 during USB serial debugging / commands to avoid breaking Serial input.
#define PIN_GPS_RX          -1
#define PIN_GPS_TX          -1   // RX-only listening mode

// ==============================================================================
// Feature Flags
// ==============================================================================
// 1 = Self-running simulation mode (test without VESC connected)
// 0 = Live VESC UART / CAN communication
#define SIMULATION_MODE     0

// Light Modules / Underglow Control Screen
// 0 = Kept in background (swiping switches directly between Ride Dash & RT Graphs)
// 1 = Active in screen swipe loop (Ride Dash <-> Lights Screen <-> RT Graphs)
#define ENABLE_LIGHTS_SCREEN 0

// Bluetooth Low Energy Keyless-Go
// 0 = Disabled (excludes ESP32 BLE stack completely from Flash & RAM)
// 1 = Enabled
#define ENABLE_BLE_KEYLESS  0

// WLED Addressable Underglow / Brake Strip (WS2812B / SK6812 / WS2815 via ESP32-S3 RMT)
#define PIN_WLED_DATA       15
#define WLED_LED_COUNT      60
#ifndef WLED_REAR_LED_COUNT
#define WLED_REAR_LED_COUNT 12
#endif
#ifndef ENABLE_WLED
#define ENABLE_WLED         1
#endif
