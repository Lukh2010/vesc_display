#pragma once

#include <stdint.h>

// ==============================================================================
// Single Source of Truth for Shared UI Hitboxes & Widget Geometry (800x480)
// ==============================================================================
struct UiRect {
    int16_t x;
    int16_t y;
    int16_t w;
    int16_t h;

    constexpr bool contains(int16_t px, int16_t py) const {
        return px >= x && px < (int16_t)(x + w) && py >= y && py < (int16_t)(y + h);
    }

    constexpr int16_t right() const { return (int16_t)(x + w); }
    constexpr int16_t bottom() const { return (int16_t)(y + h); }
    constexpr int16_t cx() const { return (int16_t)(x + w / 2); }
    constexpr int16_t cy() const { return (int16_t)(y + h / 2); }
};

namespace UiLayout {

// Ride Dashboard (SCREEN_RIDE_DASH)
static constexpr UiRect DASH_STATS_BTN         = { 20, 16, 96, 36 };
static constexpr UiRect DASH_STATS_HIT         = { 0, 0, 136, 66 };
static constexpr UiRect DASH_SETTINGS_TAP_HIT  = { 725, 0, 75, 66 };   // Clears TCS pill (x:664..720)
static constexpr UiRect DASH_SETTINGS_HOLD_HIT = { 620, 0, 180, 80 };

static constexpr int16_t DASH_SPEED_CX         = 240;
static constexpr int16_t DASH_SPEED_CY         = 196;
static constexpr UiRect  DASH_SPEED_NUM_HIT    = { 150, 145, 181, 94 }; // Excludes "km/h" label (y >= 242)

static constexpr UiRect  DASH_BOOST_BTN        = { 445, 380, 152, 72 };
static constexpr UiRect  DASH_MODE_BTN         = { 613, 380, 152, 72 };
static constexpr UiRect  DASH_ACTION_ROW_HIT   = { 430, 360, 346, 106 };
static constexpr int16_t DASH_ACTION_SPLIT_X   = 605;
static constexpr UiRect  DASH_BOTTOM_RESET_HIT = { 0, 391, 400, 89 };

static constexpr int16_t DASH_LIGHT_CX         = 50;
static constexpr int16_t DASH_LIGHT_Y_TOP      = 354;
static constexpr int16_t DASH_LIGHT_Y_BOT      = 416;
static constexpr int16_t DASH_LIGHT_RAD        = 27;
static constexpr int16_t DASH_LIGHT_HIT_RAD    = 34;

// Telemetry / Stats Screen (SCREEN_RT_GRAPHS)
static constexpr UiRect STATS_BACK_BTN         = { 20, 12, 110, 36 };
static constexpr UiRect STATS_BACK_HIT         = { 0, 0, 131, 66 };
static constexpr UiRect STATS_RACE_BTN         = { 140, 12, 100, 36 };
static constexpr UiRect STATS_RACE_HIT         = { 135, 0, 121, 66 };
static constexpr UiRect STATS_RESET_BTN        = { 640, 12, 140, 36 };
static constexpr UiRect STATS_RESET_HIT        = { 600, 0, 200, 66 };

// Settings Screen (SCREEN_SETTINGS_MENU)
static constexpr UiRect SETTINGS_BACK_BTN      = { 20, 12, 108, 34 };
static constexpr UiRect SETTINGS_BACK_HIT      = { 0, 0, 161, 56 };
static constexpr UiRect SETTINGS_BACK_HOLD_HIT = { 0, 0, 161, 66 };
static constexpr UiRect SETTINGS_DONE_BTN      = { 672, 12, 108, 34 };
static constexpr UiRect SETTINGS_DONE_HIT      = { 640, 0, 160, 56 };
static constexpr UiRect SETTINGS_DONE_HOLD_HIT = { 640, 0, 160, 66 };

// Race / Dragy Launch Screen (SCREEN_RACE_TIMER)
static constexpr UiRect RACE_BACK_BTN          = { 20, 12, 108, 34 };
static constexpr UiRect RACE_BACK_HIT          = { 0, 0, 131, 66 };
static constexpr UiRect RACE_DYNO_BTN          = { 475, 12, 140, 34 };
static constexpr UiRect RACE_DYNO_HIT          = { 470, 0, 160, 66 };
static constexpr UiRect RACE_RECORDS_BTN       = { 630, 12, 150, 34 };
static constexpr UiRect RACE_RECORDS_HIT       = { 630, 0, 170, 66 };
static constexpr UiRect RACE_LAUNCH_BTN        = { 204, 368, 212, 48 };
static constexpr UiRect RACE_CENTER_HIT        = { 150, 100, 361, 321 };

// Race Records ("Bestzeiten") Modal
static constexpr UiRect RACE_REC_MODAL         = { 140, 70, 520, 340 };
static constexpr UiRect RACE_REC_CLOSE_BTN     = { 608, 84, 38, 34 };
static constexpr UiRect RACE_REC_CLOSE_HIT     = { 585, 70, 71, 66 };
static constexpr UiRect RACE_REC_RESET_BTN     = { 170, 336, 150, 48 };
static constexpr UiRect RACE_REC_RESET_HIT     = { 160, 320, 181, 76 };
static constexpr UiRect RACE_REC_DONE_BTN      = { 450, 336, 180, 48 };
static constexpr UiRect RACE_REC_DONE_HIT      = { 430, 320, 211, 76 };

// Race Dyno Modal
static constexpr UiRect RACE_DYNO_MODAL        = { 100, 45, 600, 390 };
static constexpr UiRect RACE_DYNO_CLOSE_BTN    = { 650, 57, 36, 32 };
static constexpr UiRect RACE_DYNO_CLOSE_HIT    = { 630, 45, 71, 61 };
static constexpr UiRect RACE_DYNO_DONE_BTN     = { 300, 385, 200, 40 };
static constexpr UiRect RACE_DYNO_DONE_HIT     = { 280, 365, 241, 71 };

// PIN Lockscreen 3x4 Keypad
static constexpr int16_t PIN_KEY_W             = 120;
static constexpr int16_t PIN_KEY_H             = 56;
static constexpr int16_t PIN_COL_X[3]          = { 200, 340, 480 };
static constexpr int16_t PIN_ROW_Y[4]          = { 164, 234, 304, 374 };
static constexpr UiRect  PIN_KEYPAD_HIT        = { 100, 120, 601, 341 };

constexpr UiRect pinKeyRect(int r, int c) {
    return { PIN_COL_X[c], PIN_ROW_Y[r], PIN_KEY_W, PIN_KEY_H };
}

constexpr int8_t pinKeypadHit(int16_t tx, int16_t ty) {
    if (!PIN_KEYPAD_HIT.contains(tx, ty)) return -1;
    int c = (tx < 330) ? 0 : ((tx < 470) ? 1 : 2);
    int r = (ty < 227) ? 0 : ((ty < 297) ? 1 : ((ty < 367) ? 2 : 3));
    return (int8_t)(r * 3 + c);
}

} // namespace UiLayout
