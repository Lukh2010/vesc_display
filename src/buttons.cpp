#include "buttons.h"
#include "display_driver.h"
#include <math.h>

ButtonHandler Buttons;

static const uint32_t DEBOUNCE_TIME_MS   = 20;
static const uint32_t CHORD_WINDOW_MS    = 130;
static const uint32_t CHORD_COOLDOWN_MS  = 150;
static const uint32_t REPEAT_DELAY_MS    = 450;
static const uint32_t LONG_PRESS_TIME_MS = 500;
static const uint32_t TOUCH_LONG_MS      = 800; // Hold threshold for long touch
static const uint32_t TOUCH_COOLDOWN_MS  = 100; // Debounce cooldown between touches
static const int16_t  SWIPE_THRESHOLD_PX = 60;  // Movement distance required for swipe

ButtonHandler::ButtonHandler()
    : _pending_action(NAV_NONE),
      _in_edit_mode(false),
      _chord_active(false),
      _all_up_since_ms(0),
      _require_all_up(false),
      _touch_active(false),
      _touch_start_x(0),
      _touch_start_y(0),
      _touch_curr_x(0),
      _touch_curr_y(0),
      _touch_start_ms(0),
      _last_touch_release_ms(0),
      _touch_long_fired(false),
      _has_tap(false),
      _last_tap_x(0),
      _last_tap_y(0) {
    _btn1 = { PIN_BTN_1, false, false, 0, 0, 0, false, false, 0 };
    _btn2 = { PIN_BTN_2, false, false, 0, 0, 0, false, false, 0 };
}

void ButtonHandler::begin() {
    if (_btn1.pin >= 0) {
        pinMode(_btn1.pin, INPUT_PULLUP);
    }
    if (_btn2.pin >= 0) {
        pinMode(_btn2.pin, INPUT_PULLUP);
    }
}

void ButtonHandler::setEditMode(bool in_edit) {
    _in_edit_mode = in_edit;
    _require_all_up = true;
    _chord_active = true;
    _all_up_since_ms = 0;
    _btn1.single_pending = false;
    _btn2.single_pending = false;
    _pending_action = NAV_NONE;
}

void ButtonHandler::processDebounce(ButtonState &btn, bool raw, uint32_t now) {
    if (btn.pin < 0) return;

    if (raw != btn.raw_state) {
        btn.raw_state = raw;
        btn.last_change_ms = now;
    }

    if ((now - btn.last_change_ms) >= DEBOUNCE_TIME_MS) {
        if (btn.is_down != btn.raw_state) {
            btn.is_down = btn.raw_state;
            if (btn.is_down) {
                btn.press_start_ms = now;
                btn.last_repeat_ms = now;
                btn.long_press_fired = false;
                if (!_chord_active && !_require_all_up) {
                    btn.single_pending = true;
                    btn.pending_since_ms = now;
                }
            }
        }
    }
}

void ButtonHandler::processTouch(uint32_t now) {
    int16_t tx = 0, ty = 0;
    bool touching = Display.getTouch(&tx, &ty);

    if (touching) {
        // Safety bound check for touch coordinates
        if (tx < 0) tx = 0;
        if (tx >= LCD_WIDTH) tx = LCD_WIDTH - 1;
        if (ty < 0) ty = 0;
        if (ty >= LCD_HEIGHT) ty = LCD_HEIGHT - 1;

        if (!_touch_active) {
            // Check debounce cooldown from previous release
            if ((now - _last_touch_release_ms) < TOUCH_COOLDOWN_MS) {
                return;
            }

            // Touch down detected
            _touch_active = true;
            _touch_start_x = tx;
            _touch_start_y = ty;
            _touch_curr_x = tx;
            _touch_curr_y = ty;
            _touch_start_ms = now;
            _touch_long_fired = false;
        } else {
            // Touch continuing
            _touch_curr_x = tx;
            _touch_curr_y = ty;

            // Check for Long Press while finger is held
            if (!_touch_long_fired && (now - _touch_start_ms >= TOUCH_LONG_MS)) {
                int16_t dx = tx - _touch_start_x;
                int16_t dy = ty - _touch_start_y;
                if (abs(dx) < 35 && abs(dy) < 35) {
                    _touch_long_fired = true;

                    // Contextual Long Touch Action
                    if (_in_edit_mode) {
                        _pending_action = NAV_BOTH_PRESSED; // Save & exit edit mode
                    } else if (_touch_start_y > 390) {
                        // Long press on bottom status strip -> Reset Trip Odometer
                        _pending_action = NAV_BTN2_LONG;
                    } else if (_touch_start_y < 80 && _touch_start_x > 620) {
                        // Long press top right -> Open Settings Menu
                        _pending_action = NAV_BTN1_LONG;
                    } else {
                        // General long press on screen -> Reset Trip or Back
                        _pending_action = NAV_BTN2_LONG;
                    }
                }
            }
        }
    } else {
        if (_touch_active) {
            // Touch released
            _touch_active = false;
            _last_touch_release_ms = now;
            uint32_t touch_dur = now - _touch_start_ms;

            if (!_touch_long_fired && touch_dur >= 20) {
                int16_t dx = _touch_curr_x - _touch_start_x;
                int16_t dy = _touch_curr_y - _touch_start_y;
                int16_t adx = abs(dx);
                int16_t ady = abs(dy);

                if (adx >= SWIPE_THRESHOLD_PX && adx > ady) {
                    // Horizontal Swipe
                    if (dx > 0) {
                        // Swipe Right -> Previous Screen / Value Up
                        _pending_action = _in_edit_mode ? NAV_EDIT_INC : NAV_BTN1_SHORT;
                    } else {
                        // Swipe Left -> Next Screen / Value Down
                        _pending_action = _in_edit_mode ? NAV_EDIT_DEC : NAV_BTN2_SHORT;
                    }
                } else if (ady >= SWIPE_THRESHOLD_PX && ady > adx) {
                    // Vertical Swipe (in menu: scroll up/down)
                    if (dy > 0) {
                        // Swipe Down -> Up in menu
                        _pending_action = _in_edit_mode ? NAV_EDIT_INC : NAV_BTN1_SHORT;
                    } else {
                        // Swipe Up -> Down in menu
                        _pending_action = _in_edit_mode ? NAV_EDIT_DEC : NAV_BTN2_SHORT;
                    }
                } else {
                    // Clean Tap Detected (Movement was minimal)
                    _has_tap = true;
                    _last_tap_x = _touch_curr_x;
                    _last_tap_y = _touch_curr_y;

                    if (_in_edit_mode) {
                        // In Edit Mode:
                        if (_last_tap_x < 365) {
                            _pending_action = NAV_EDIT_DEC; // Left side: -
                        } else if (_last_tap_x > 580) {
                            _pending_action = NAV_EDIT_INC; // Right side: +
                        } else {
                            _pending_action = NAV_BOTH_PRESSED; // Center: Save & exit
                        }
                    } else {
                        // In Normal Mode:
                        if (_last_tap_y < 60 && _last_tap_x > 620) {
                            // Top Right Badge -> Open Settings Menu
                            _pending_action = NAV_BTN1_LONG;
                        } else if (_last_tap_y < 60 && _last_tap_x < 220) {
                            // Top Left Profile Badge -> Toggle Battery Profile
                            _pending_action = NAV_BOTH_PRESSED;
                        } else if (_last_tap_y > 400 && _last_tap_x < 280) {
                            // Bottom Left Trip Button -> Reset Trip
                            _pending_action = NAV_BTN2_LONG;
                        } else if (_last_tap_x < 180) {
                            // Left Zone Tap -> Previous Screen / Menu Up
                            _pending_action = NAV_BTN1_SHORT;
                        } else if (_last_tap_x > 620) {
                            // Right Zone Tap -> Next Screen / Menu Down
                            _pending_action = NAV_BTN2_SHORT;
                        } else {
                            // Center Zone Tap:
                            // In Menu -> Select / Enter; In Ride Dash -> Next Screen
                            _pending_action = NAV_BTN2_SHORT;
                        }
                    }
                }
            }
        }
    }
}

void ButtonHandler::update() {
    uint32_t now = millis();

    // 1. Process Capacitive Touch Controller (Primary Input)
    processTouch(now);

    // If an action was queued from touch, return immediately
    if (_pending_action != NAV_NONE) {
        return;
    }

    // 2. Process Physical Buttons (if pins are configured)
    if (_btn1.pin < 0 && _btn2.pin < 0) {
        return;
    }

    bool raw1 = (_btn1.pin >= 0) ? (digitalRead(_btn1.pin) == LOW) : false;
    bool raw2 = (_btn2.pin >= 0) ? (digitalRead(_btn2.pin) == LOW) : false;

    processDebounce(_btn1, raw1, now);
    processDebounce(_btn2, raw2, now);

    bool both_down = (_btn1.is_down && _btn2.is_down);
    bool both_up   = (!_btn1.is_down && !_btn2.is_down);

    if (both_up && !raw1 && !raw2) {
        if (_all_up_since_ms == 0) {
            _all_up_since_ms = now;
        }
    } else {
        _all_up_since_ms = 0;
    }

    if (_chord_active) {
        if (both_up && !raw1 && !raw2 && (now - _all_up_since_ms >= CHORD_COOLDOWN_MS)) {
            _chord_active = false;
            _require_all_up = false;
            _btn1.single_pending = false;
            _btn2.single_pending = false;
            _btn1.long_press_fired = true;
            _btn2.long_press_fired = true;
        }
        return;
    }

    if (_require_all_up) {
        if (both_up && !raw1 && !raw2 && (now - _all_up_since_ms >= 50)) {
            _require_all_up = false;
            _btn1.single_pending = false;
            _btn2.single_pending = false;
            _btn1.long_press_fired = true;
            _btn2.long_press_fired = true;
        }
        return;
    }

    bool chord_detected = both_down ||
                          (_btn1.is_down && raw2) ||
                          (_btn2.is_down && raw1) ||
                          (_btn1.single_pending && (_btn2.is_down || raw2)) ||
                          (_btn2.single_pending && (_btn1.is_down || raw1));

    if (chord_detected) {
        _chord_active = true;
        _all_up_since_ms = 0;
        _btn1.single_pending = false;
        _btn2.single_pending = false;
        _btn1.long_press_fired = true;
        _btn2.long_press_fired = true;
        _pending_action = NAV_BOTH_PRESSED;
        return;
    }

    if (_in_edit_mode) {
        if (_btn1.single_pending) {
            if (now - _btn1.pending_since_ms >= CHORD_WINDOW_MS) {
                _btn1.single_pending = false;
                _pending_action = NAV_EDIT_INC;
                _btn1.last_repeat_ms = now;
            }
        } else if (_btn1.is_down) {
            uint32_t hold_time = now - _btn1.press_start_ms;
            if (hold_time >= REPEAT_DELAY_MS) {
                uint32_t interval = (hold_time > 1800) ? 60 : 150;
                if (now - _btn1.last_repeat_ms >= interval) {
                    _btn1.last_repeat_ms = now;
                    _pending_action = NAV_EDIT_INC;
                }
            }
        }

        if (_btn2.single_pending) {
            if (now - _btn2.pending_since_ms >= CHORD_WINDOW_MS) {
                _btn2.single_pending = false;
                _pending_action = NAV_EDIT_DEC;
                _btn2.last_repeat_ms = now;
            }
        } else if (_btn2.is_down) {
            uint32_t hold_time = now - _btn2.press_start_ms;
            if (hold_time >= REPEAT_DELAY_MS) {
                uint32_t interval = (hold_time > 1800) ? 60 : 150;
                if (now - _btn2.last_repeat_ms >= interval) {
                    _btn2.last_repeat_ms = now;
                    _pending_action = NAV_EDIT_DEC;
                }
            }
        }
    } else {
        if (_btn1.single_pending) {
            if (!_btn1.is_down) {
                if (now - _btn1.pending_since_ms >= CHORD_WINDOW_MS) {
                    _btn1.single_pending = false;
                    _pending_action = NAV_BTN1_SHORT;
                }
            } else {
                if (now - _btn1.press_start_ms >= LONG_PRESS_TIME_MS) {
                    _btn1.single_pending = false;
                    _btn1.long_press_fired = true;
                    _pending_action = NAV_BTN1_LONG;
                }
            }
        }

        if (_btn2.single_pending) {
            if (!_btn2.is_down) {
                if (now - _btn2.pending_since_ms >= CHORD_WINDOW_MS) {
                    _btn2.single_pending = false;
                    _pending_action = NAV_BTN2_SHORT;
                }
            } else {
                if (now - _btn2.press_start_ms >= LONG_PRESS_TIME_MS) {
                    _btn2.single_pending = false;
                    _btn2.long_press_fired = true;
                    _pending_action = NAV_BTN2_LONG;
                }
            }
        }
    }
}

NavAction ButtonHandler::getNavAction() {
    NavAction act = _pending_action;
    _pending_action = NAV_NONE;
    return act;
}
