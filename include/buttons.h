#pragma once

#include <Arduino.h>
#include "board_config.h"
#include "dash_types.h"

class ButtonHandler {
public:
    ButtonHandler();
    void begin();
    void update();

    NavAction getNavAction();

    void setActiveScreen(DashboardScreen screen) {
        if (_active_screen != screen) {
            _active_screen = screen;
            _has_tap = false;
            _touch_active = false;
            _pending_action = NAV_NONE;
            _last_touch_release_ms = millis();
            _touch_screen_switch_ms = millis();
            _speed_touch_started = false;
            _speed_touch_held = false;
            _speed_touch_fired = false;
        }
    }
    DashboardScreen getActiveScreen() const { return _active_screen; }

    bool isBtn1Down() const { return _btn1.is_down; }
    bool isBtn2Down() const { return _btn2.is_down; }
    bool isTouching() const { return _touch_active; }
    int16_t getTouchX() const { return _touch_curr_x; }
    int16_t getTouchY() const { return _touch_curr_y; }

    void setLocked(bool locked) { _is_locked = locked; }
    bool isLocked() const { return _is_locked; }

    // Last tap coordinate query
    bool getLastTap(int16_t *x, int16_t *y) {
        if (_has_tap) {
            if (x) *x = _last_tap_x;
            if (y) *y = _last_tap_y;
            _has_tap = false;
            return true;
        }
        return false;
    }

    // Clear any pending navigation action (used when touch tap is consumed by GUI)
    void clearNavAction() { _pending_action = NAV_NONE; }

    // Tuning Mode & Speed Number Hitbox
    void setTuningMode(bool active) { _tuning_mode = active; }
    bool isTuningMode() const { return _tuning_mode; }
    bool isSpeedNumberTouch(int16_t x, int16_t y) const;

    // Race / Dragy Timer Input State
    void setRacePopupOpen(bool open) { _race_popup_open = open; }
    bool isRacePopupOpen() const { return _race_popup_open; }
    void setRaceDynoOpen(bool open) { _race_dyno_open = open; }
    bool isRaceDynoOpen() const { return _race_dyno_open; }
    void setRaceState(uint8_t state) { _race_state = state; }
    uint8_t getRaceState() const { return _race_state; }

    // Action Button / FOC Horn & Police Mode Panic Lock
    void setActionButton(bool is_down);
    void actionButtonPress() { setActionButton(true); }
    void actionButtonRelease() { setActionButton(false); }
    bool isActionButtonDown() const { return _action_btn_down; }
    bool isHornActive() const { return _horn_active; }

private:
    DashboardScreen _active_screen;
    struct ButtonState {
        int8_t   pin;
        bool     raw_state;
        bool     is_down;
        uint32_t last_change_ms;
        uint32_t press_start_ms;
        uint32_t last_repeat_ms;
        bool     long_press_fired;
        bool     single_pending;
        uint32_t pending_since_ms;
    };

    ButtonState _btn1;
    ButtonState _btn2;
    NavAction   _pending_action;

    bool        _chord_active;
    uint32_t    _all_up_since_ms;
    bool        _require_all_up;

    // Touch State Tracking
    bool        _touch_active;
    int16_t     _touch_start_x;
    int16_t     _touch_start_y;
    int16_t     _touch_curr_x;
    int16_t     _touch_curr_y;
    uint32_t    _touch_start_ms;
    uint32_t    _last_touch_release_ms;
    uint32_t    _touch_screen_switch_ms;
    bool        _touch_long_fired;
    bool        _has_tap;
    int16_t     _last_tap_x;
    int16_t     _last_tap_y;

    // Speed Number Touch Tracking
    bool        _is_locked;
    bool        _tuning_mode;
    bool        _speed_touch_started;
    bool        _speed_touch_held;
    bool        _speed_touch_fired;

    // Race State
    bool        _race_popup_open;
    bool        _race_dyno_open;
    uint8_t     _race_state;

    // Action Button / Horn State
    bool        _action_btn_down;
    bool        _action_btn_sim_down;
    bool        _action_btn_boot_raw;
    bool        _action_btn_boot_down;
    uint32_t    _action_btn_press_ms;
    uint32_t    _action_btn_change_ms;
    bool        _action_btn_held_fired;
    bool        _horn_active;
    uint32_t    _last_horn_ms;

    void processDebounce(ButtonState &btn, bool raw, uint32_t now);
    void processTouch(uint32_t now);
    void checkActionButtonState(uint32_t now);
};

extern ButtonHandler Buttons;
