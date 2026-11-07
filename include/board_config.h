#pragma once

#include <Arduino.h>

// ==============================================================================
// Waveshare ESP32-S3-Touch-LCD-4.3 Board Configuration & Pin Mapping
// ==============================================================================
// Hardware: ESP32-S3 (16MB Flash, 8MB Octal PSRAM - qio_opi)
// Display:  ST7262 RGB Parallel Interface (800x480 landscape)
// Touch:    Goodix GT911 Capacitive Touch Controller (I2C)
// IO Exp.:  CH422G I2C Expander (Backlight on EXIO2, Touch RST on EXIO1)

// Display Dimensions
#define LCD_WIDTH   800
#define LCD_HEIGHT  480

// ST7262 RGB Parallel Timing & Clock (Rock-solid 30 FPS stable timing)
#define LCD_PIN_HSYNC   46
#define LCD_PIN_VSYNC   3
#define LCD_PIN_DE      5
#define LCD_PIN_PCLK    7
#define LCD_FREQ_HZ     14000000 // 14 MHz for rock-solid 30 FPS without horizontal wobble

// ST7262 RGB Timing Parameters (800x480 standard)
#define LCD_HPW         10
#define LCD_HBP         10
#define LCD_HFP         20
#define LCD_VPW         10
#define LCD_VBP         10
#define LCD_VFP         10

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
#define PIN_BTN_BOOT    0    // Onboard BOOT button (GPIO 0)

// ==============================================================================
// VESC Controller Interfaces (Flipsky / VESC on Waveshare 4.3" Pins)
// ==============================================================================
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
