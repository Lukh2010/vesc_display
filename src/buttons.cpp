#include "buttons.h"
#include "display_driver.h"
#include "settings_manager.h"
#include "mode_manager.h"
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
    : _active_screen(SCREEN_RIDE_DASH),
      _pending_action(NAV_NONE),
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
      _last_tap_y(0),
      _tuning_mode(false),
      _speed_touch_started(false),
      _speed_touch_held(false),
      _speed_touch_fired(false) {
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

bool ButtonHandler::isSpeedNumberTouch(int16_t x, int16_t y) const {
    if (ModeManager::instance().isLocked()) return false;
    if (_active_screen != SCREEN_RIDE_DASH) return false;

    // Flagship Hyper-VESC 35kW+ Cluster:
    // Hero digital speed number centered at cx=240, cy=195 (FreeSansBold24pt7b size 2).
    // "km/h" label starts at y=245. Hitbox strictly excludes y>=240 and outer screen area.
    return (x >= 150 && x <= 330 && y >= 145 && y <= 238);
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
            _speed_touch_started = isSpeedNumberTouch(tx, ty);
            _speed_touch_held = _speed_touch_started;
            _speed_touch_fired = false;
        } else {
            // Touch continuing
            _touch_curr_x = tx;
            _touch_curr_y = ty;

            // 1. Precise Speed Number 4-Second Continuous Hold (Tuning Mode Activation)
            if (_speed_touch_started) {
                if (_speed_touch_held) {
                    int16_t dx = tx - _touch_start_x;
                    int16_t dy = ty - _touch_start_y;
                    if (abs(dx) > 35 || abs(dy) > 35 || !isSpeedNumberTouch(tx, ty)) {
                        _speed_touch_held = false; // Finger drifted off the km/h number - safely cancel hold
                    } else if (!_tuning_mode && !_speed_touch_fired && (now - _touch_start_ms >= 4000)) {
                        // Continuous 4000ms hold exclusively on the km/h number
                        _speed_touch_fired = true;
                        _touch_long_fired = true;
                        _pending_action = NAV_TUNING_ENABLE;
                    }
                }
            }

            // 2. Generic Long Press check (MUST NOT fire if touch started on speed number or while locked)
            if (!ModeManager::instance().isLocked() && !_speed_touch_started && !_touch_long_fired && (now - _touch_start_ms >= TOUCH_LONG_MS)) {
                int16_t dx = tx - _touch_start_x;
                int16_t dy = ty - _touch_start_y;
                if (abs(dx) < 35 && abs(dy) < 35) {
                    _touch_long_fired = true;

                    // Contextual Long Touch Action
                    if (_in_edit_mode) {
                        _pending_action = NAV_BOTH_PRESSED; // Save & exit edit mode
                    } else if (_active_screen == SCREEN_RT_GRAPHS) {
                        if (_touch_start_x >= 600 && _touch_start_y <= 65) {
                            _pending_action = NAV_BTN2_LONG; // Reset Trip
                        } else if (_touch_start_x <= 150 && _touch_start_y <= 65) {
                            _pending_action = NAV_BTN1_SHORT; // Top-left < DASH returns to Ride Dash
                        }
                        // All other holds in STATS do NOT exit screen
                    } else if (_touch_start_y > 390 && _touch_start_x < 400) {
                        // Long press on bottom-left status strip -> Reset Trip Odometer
                        // (Touches on x >= 400 are action buttons [ BOOST ] / [ MODE ], prevent accidental reset)
                        _pending_action = NAV_BTN2_LONG;
                    } else if (_touch_start_y < 80 && _touch_start_x > 620) {
                        // Long press top right -> Open Settings Menu
                        _pending_action = NAV_BTN1_LONG;
                    }
                    // Arbitrary holds in other screen areas safely do nothing (prevent accidental trip wipes)
                }
            }
        }
    } else {
        if (_touch_active) {
            // Touch released
            _touch_active = false;
            _last_touch_release_ms = now;
            uint32_t touch_dur = now - _touch_start_ms;

            // If system is locked by PIN, directly register tap from touch_start_x/y (100% reliable keypad taps)
            if (ModeManager::instance().isLocked()) {
                if (touch_dur >= 15) {
                    _has_tap = true;
                    _last_tap_x = _touch_start_x;
                    _last_tap_y = _touch_start_y;
                }
                _speed_touch_started = false;
                _speed_touch_held = false;
                _speed_touch_fired = false;
                return;
            }

            // Check if touch was started on the km/h number
            if (_speed_touch_started) {
                if (_speed_touch_held && !_speed_touch_fired) {
                    int16_t dx = _touch_curr_x - _touch_start_x;
                    int16_t dy = _touch_curr_y - _touch_start_y;
                    if (abs(dx) < 35 && abs(dy) < 35 && isSpeedNumberTouch(_touch_curr_x, _touch_curr_y)) {
                        if (_tuning_mode && touch_dur < 1000) {
                            // Single Tap directly on km/h number -> Immediately deactivate Tuning Mode!
                            _pending_action = NAV_TUNING_DISABLE;
                        }
                    }
                }
                _speed_touch_started = false;
                _speed_touch_held = false;
                _speed_touch_fired = false;
                return;
            }

            _speed_touch_started = false;
            _speed_touch_held = false;
            _speed_touch_fired = false;

            if (!_touch_long_fired && touch_dur >= 20) {
                int16_t dx = _touch_curr_x - _touch_start_x;
                int16_t dy = _touch_curr_y - _touch_start_y;
                int16_t adx = abs(dx);
                int16_t ady = abs(dy);

                if (adx >= SWIPE_THRESHOLD_PX && adx > ady) {
                    // Horizontal Swipe
                    if (_active_screen == SCREEN_RT_GRAPHS) {
                        // In STATS screen: do NOT switch screen on swipe! Only < DASH exits.
                    } else if (dx > 0) {
                        // Swipe Right -> Previous Screen / Value Up
                        _pending_action = _in_edit_mode ? NAV_EDIT_INC : NAV_BTN1_SHORT;
                    } else {
                        // Swipe Left -> Next Screen / Value Down
                        _pending_action = _in_edit_mode ? NAV_EDIT_DEC : NAV_BTN2_SHORT;
                    }
                } else if (ady >= SWIPE_THRESHOLD_PX && ady > adx) {
                    // Vertical Swipe (in menu: scroll up/down)
                    if (_active_screen == SCREEN_RT_GRAPHS) {
                        // In STATS screen: do NOT switch screen on swipe! Only < DASH exits.
                    } else if (dy > 0) {
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
                    } else if (_active_screen == SCREEN_LIGHTS) {
                        // On LIGHTS Screen:
                        if (_last_tap_x >= 400 && _last_tap_x <= 775 && _last_tap_y >= 320 && _last_tap_y <= 435) {
                            _pending_action = NAV_HAZARD_TOGGLE; // Bottom-Right Large [ ▲ HAZARD ]
                        } else if (_last_tap_x >= 80 && _last_tap_x <= 260 && _last_tap_y >= 390 && _last_tap_y <= 450) {
                            _pending_action = NAV_HAZARD_TOGGLE; // Bottom-Left Hazard Toggle Switch
                        } else if (_last_tap_x < 50) {
                            _pending_action = NAV_BTN1_SHORT; // Extreme Left Edge -> Prev Screen
                        } else if (_last_tap_x > 760 && _last_tap_y > 60) {
                            _pending_action = NAV_BTN2_SHORT; // Extreme Right Edge -> Next Screen
                        }
                    } else if (_active_screen == SCREEN_RT_GRAPHS) {
                        // On RT GRAPHS Screen (Statistics Menu):
                        if (_last_tap_x >= 600 && _last_tap_y <= 65) {
                            _pending_action = NAV_BTN2_LONG; // Top Right -> Reset Trip
                        } else if (_last_tap_x <= 150 && _last_tap_y <= 65) {
                            _pending_action = NAV_BTN1_SHORT; // Top Left -> Return to Ride Dash
                        }
                        // All other taps (on cards, tiles, empty space, brightness controls)
                        // do NOT exit back to Dash! Only pressing < DASH returns.
                    } else {
                        // On SCREEN_RIDE_DASH / Default:
                        // 1. Top Left [ STATS ] Navigation Touch Button -> Open Statistics Screen (SCREEN_RT_GRAPHS)
                        if (_last_tap_x >= 18 && _last_tap_x <= 125 && _last_tap_y >= 12 && _last_tap_y <= 60) {
                            _pending_action = NAV_SCREEN_STATS;
                        }
                        // 2. Action Buttons on Ride Dash (Bottom Right)
                        else if (_last_tap_y >= 360 && _last_tap_y <= 465) {
                            if (_last_tap_x >= 430 && _last_tap_x <= 605) {
                                _pending_action = NAV_BOOST_TOGGLE;
                            } else if (_last_tap_x >= 605 && _last_tap_x <= 775) {
                                _pending_action = NAV_MODE_CYCLE;
                            }
                        }
                        // 3. Top Right Badge -> Open Settings Menu
                        else if (_last_tap_y < 65 && _last_tap_x > 620) {
                            _pending_action = NAV_BTN1_LONG;
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
