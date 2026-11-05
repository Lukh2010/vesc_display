#pragma once

#include <Arduino.h>
#include "board_config.h"
#include "dash_types.h"

class ButtonHandler {
public:
    ButtonHandler();
    void begin();
    void update();

    // Mode switch: changes button behavior between normal menu/dash navigation and value editing
    void setEditMode(bool in_edit);
    bool isInEditMode() const { return _in_edit_mode; }

    NavAction getNavAction();

    bool isBtn1Down() const { return _btn1.is_down; }
    bool isBtn2Down() const { return _btn2.is_down; }
    bool isTouching() const { return _touch_active; }

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

private:
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
    bool        _in_edit_mode;

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
    bool        _touch_long_fired;
    bool        _has_tap;
    int16_t     _last_tap_x;
    int16_t     _last_tap_y;

    void processDebounce(ButtonState &btn, bool raw, uint32_t now);
    void processTouch(uint32_t now);
};

extern ButtonHandler Buttons;
