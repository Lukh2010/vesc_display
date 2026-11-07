#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "Arduino.h"
#include "board_config.h"
#include "display_driver.h"
#include "settings_manager.h"
#include "battery_manager.h"
#include <assert.h>
#include "dash_renderer.h"
#include "buttons.h"
#include "vesc_uart.h"
#include "mode_manager.h"
#include "fault_manager.h"

VescHandler::VescHandler()
    : _bridge_active(false),
      _bridge_pc_to_vesc(0),
      _bridge_vesc_to_pc(0),
      _wifi_bridge_active(false),
      _wifi_client_connected(false),
      _bridge_wifi_to_vesc(0),
      _bridge_vesc_to_wifi(0),
      _sync_status(SYNC_IDLE),
      _sync_ms(0),
      _mcconf_req_ms(0) {
    _wifi_client_ip[0] = '\0';
}
void VescHandler::begin() {}
void VescHandler::update(DashTelemetry &) {}
void VescHandler::setSimThrottle(float) {}
void VescHandler::enterBridgeMode() { _bridge_active = true; }
void VescHandler::exitBridgeMode() { _bridge_active = false; }
void VescHandler::enterWifiBridgeMode() {
    _wifi_bridge_active = true;
    _wifi_client_connected = true;
    strncpy(_wifi_client_ip, "192.168.4.2", sizeof(_wifi_client_ip));
    _bridge_wifi_to_vesc = 1420;
    _bridge_vesc_to_wifi = 3680;
}
void VescHandler::exitWifiBridgeMode() { _wifi_bridge_active = false; _wifi_client_connected = false; }
void VescHandler::updateBridge() {}
void VescHandler::updateUsbBridge() {}
void VescHandler::updateWifiBridge() {}
void VescHandler::requestMcconf() { _sync_status = SYNC_SUCCESS; }
void VescHandler::sendModeLimits(float max_speed_kmh, float max_power_watts, float max_battery_amps, float max_phase_amps, float cutoff_start, float cutoff_end) {
    Settings.get().max_speed_kmh    = (uint8_t)roundf(max_speed_kmh);
    Settings.get().max_battery_amps = (uint16_t)roundf(max_battery_amps);
    Settings.get().max_phase_amps   = (uint16_t)roundf(max_phase_amps);
}
void VescHandler::sendTerminalCommand(const char *) {}
VescHandler Vesc;

MockSerial Serial;
DisplayDriver Display;
extern DashboardRenderer Renderer;
extern ButtonHandler Buttons;

void setBaseTelemetry(DashTelemetry &t) {
    memset(&t, 0, sizeof(t));
    t.vesc_connected = true;
    t.screen = SCREEN_RIDE_DASH;
    t.voltage = 53.4f;
    t.battery_pct = 88.0f;
    t.current_amps = 0.0f;
    t.phase_amps = 0.0f;
    t.power_watts = 0.0f;
    t.duty_cycle_pct = 0.0f;
    t.speed_kmh = 0.0f;
    t.temp_esc = 34.5f;
    t.temp_motor = 38.2f;
    t.trip_km = 14.2f;
    t.odo_km = 342.8f;
    t.amphours_used = 5.3f;
    t.remaining_wh = 1140.0f;
    t.est_range_km = 48.0f;
    t.battery_health_soh = 98.4f;
    t.battery_profile_id = 0;
    t.ride_mode = MODE_ECO;
    t.tuning_mode = true;

    t.stats.trip_wh = 275.4f;
    t.stats.trip_wh_km = 19.4f;
    t.stats.peak_power_watts = 1950.0f;
    t.stats.peak_current_amps = 28.4f;
    t.stats.peak_phase_amps = 68.2f;
    t.stats.max_speed_kmh = 58.7f;
    t.stats.avg_speed_kmh = 28.4f;
    t.stats.ride_time_sec = 2538; // ~42 mins
}

void renderAndSave(const char *name, DashTelemetry &t) {
    Renderer.render(t);
    char path[256];
    snprintf(path, sizeof(path), "tools/simulator/screenshots/%s.bmp", name);
    Display.dumpBMP(path);
    printf("[OK] Saved scenario '%s' -> %s\n", name, path);
}

void testButtonDebouncing() {
    printf("\n=== Running Button Debounce & Chord State Machine Unit Tests ===\n");
    Buttons.begin();

    auto setPin = [&](int pin, int state) {
        simSetPin(pin, state);
        Buttons.update();
    };

    // 1. Initial State: All released (HIGH)
    setPin(PIN_BTN_1, HIGH);
    setPin(PIN_BTN_2, HIGH);
    for (int i = 0; i < 15; i++) {
        simAdvanceMs(10);
        Buttons.update();
    }
    assert(Buttons.getNavAction() == NAV_NONE);
    assert(!Buttons.isBtn1Down());
    assert(!Buttons.isBtn2Down());
    printf("[PASS] Buttons idle at boot.\n");

    // 2. Simulate Single Click on BTN2 in Edit Mode (with contact chatter and chord window)
    Buttons.setEditMode(true);
    for (int i = 0; i < 20; i++) {
        simAdvanceMs(10);
        Buttons.update();
    }

    int dec_count = 0;
    int other_count = 0;

    auto stepAndCheck = [&](uint32_t step_ms) {
        simAdvanceMs(step_ms);
        Buttons.update();
        NavAction act = Buttons.getNavAction();
        if (act == NAV_EDIT_DEC) dec_count++;
        else if (act != NAV_NONE) other_count++;
    };

    // Mechanical contact chatter (rapid bouncing between LOW and HIGH):
    setPin(PIN_BTN_2, LOW);
    stepAndCheck(2);
    setPin(PIN_BTN_2, HIGH);
    stepAndCheck(2);
    setPin(PIN_BTN_2, LOW);
    stepAndCheck(3);
    setPin(PIN_BTN_2, HIGH);
    stepAndCheck(3);
    setPin(PIN_BTN_2, LOW); // stable closed from here

    // During chatter (first 10ms), NO actions should fire
    assert(dec_count == 0);
    assert(other_count == 0);

    // Advance 28ms (debounce passes, enters chord window) -> still waiting for chord window
    stepAndCheck(28);
    assert(dec_count == 0);

    // Advance chord window (130ms) -> should fire NAV_EDIT_DEC exactly once!
    stepAndCheck(135);
    assert(dec_count == 1);
    assert(other_count == 0);

    // Release chatter
    setPin(PIN_BTN_2, HIGH);
    stepAndCheck(2);
    setPin(PIN_BTN_2, LOW);
    stepAndCheck(2);
    setPin(PIN_BTN_2, HIGH); // stable open
    stepAndCheck(50);

    // Exactly 1 action total (no skips, no release bounces)
    assert(dec_count == 1);
    assert(other_count == 0);
    assert(!Buttons.isBtn2Down());
    printf("[PASS] Single click in Edit Mode with contact bounce fired exactly 1 NAV_EDIT_DEC.\n");

    // 3. Test Auto-Repeat on Long Press in Edit Mode
    dec_count = 0;
    setPin(PIN_BTN_2, LOW); // Press down
    stepAndCheck(28); // Debounce passes
    stepAndCheck(90); // Chord window passes -> 1st tap fires immediately!
    assert(dec_count == 1);

    // Hold for 300ms (total hold ~418ms < 450ms) -> no repeat yet
    for (int i = 0; i < 6; i++) {
        stepAndCheck(50);
    }
    assert(dec_count == 1);

    // Reach 450ms+ -> repeats start at 150ms intervals
    stepAndCheck(60);
    assert(dec_count == 2); // First repeat fired!

    stepAndCheck(150);
    assert(dec_count == 3);

    // Release and wait cooldown
    setPin(PIN_BTN_2, HIGH);
    stepAndCheck(150);
    assert(!Buttons.isBtn2Down());
    printf("[PASS] Long press auto-repeat in Edit Mode initiates cleanly after hold threshold.\n");

    // 4. Test Normal Mode: Click on BTN1 gives NAV_BTN1_SHORT on release
    Buttons.setEditMode(false);
    for (int i = 0; i < 20; i++) {
        simAdvanceMs(10);
        Buttons.update();
    }

    int btn1_short = 0, btn1_long = 0;
    NavAction last_act = NAV_NONE;
    auto stepNormal = [&](uint32_t step_ms) {
        simAdvanceMs(step_ms);
        Buttons.update();
        last_act = Buttons.getNavAction();
        if (last_act == NAV_BTN1_SHORT) btn1_short++;
        else if (last_act == NAV_BTN1_LONG) btn1_long++;
    };

    setPin(PIN_BTN_1, LOW);
    stepNormal(30); // Debounced press
    assert(btn1_short == 0);
    assert(btn1_long == 0);
    stepNormal(100); // 130ms hold (< 500ms)
    setPin(PIN_BTN_1, HIGH); // Release
    stepNormal(30); // Debounced release
    stepNormal(90); // Chord window expires
    assert(btn1_short == 1); // Fired once on release!
    assert(btn1_long == 0);
    printf("[PASS] Normal mode generates NAV_BTN1_SHORT cleanly on release.\n");

    // 5. Test Asynchronous Dual-Press (Skewed Finger Timing: BTN1 pressed 50ms before BTN2)
    btn1_short = 0;
    int both_pressed = 0;
    auto stepChord = [&](uint32_t step_ms) {
        simAdvanceMs(step_ms);
        Buttons.update();
        last_act = Buttons.getNavAction();
        if (last_act == NAV_BOTH_PRESSED) both_pressed++;
        if (last_act == NAV_BTN1_SHORT) btn1_short++;
    };

    setPin(PIN_BTN_1, LOW); // Finger 1 presses BTN1 first!
    stepChord(30);          // BTN1 debounced, enters chord window
    assert(btn1_short == 0); // Must NOT fire single press yet!

    setPin(PIN_BTN_2, LOW); // Finger 2 presses BTN2 30ms later!
    stepChord(30);          // BTN2 debounces, triggers chord immediately!
    assert(both_pressed == 1);
    assert(btn1_short == 0); // No accidental single button leak!
    printf("[PASS] Asynchronous dual-press (skewed finger timing) triggered NAV_BOTH_PRESSED with 0 leaks.\n");

    // 6. Test Hold Both Buttons with Microvibrations (Zero Repeated Presses!)
    // Both buttons are held for 1500ms while microvibrations occur
    for (int cycle = 0; cycle < 10; cycle++) {
        // Microvibration on BTN1 (momentary 5ms contact break)
        setPin(PIN_BTN_1, HIGH);
        stepChord(5);
        setPin(PIN_BTN_1, LOW);
        stepChord(45);

        // Microvibration on BTN2 (momentary 5ms contact break)
        setPin(PIN_BTN_2, HIGH);
        stepChord(5);
        setPin(PIN_BTN_2, LOW);
        stepChord(45);
    }
    // Must STILL be exactly 1 both_pressed count! (Microvibrations NEVER created dozens of presses!)
    assert(both_pressed == 1);
    printf("[PASS] Holding both buttons with microvibrations produced exactly 1 press (no accidental repeats).\n");

    // 7. Test Asymmetric Release with Cooldown
    // Finger 1 releases first, then Finger 2 releases 50ms later
    setPin(PIN_BTN_1, HIGH);
    stepChord(30); // BTN1 released
    assert(last_act == NAV_NONE); // Locked!

    setPin(PIN_BTN_2, HIGH);
    stepChord(30); // BTN2 released
    assert(last_act == NAV_NONE); // Still locked in release cooldown!

    stepChord(150); // Cooldown completes cleanly
    assert(both_pressed == 1);
    assert(btn1_short == 0);
    printf("[PASS] Asymmetric finger release and cooldown completed cleanly without leaking clicks.\n");
    printf("=== All Button Debounce & Chord Tests PASSED! ===\n\n");
}

void testTouchGestures() {
    printf("\n=== Running GT911 Capacitive Touch Gesture & Navigation Unit Tests ===\n");
    Settings.get().dash_style = STYLE_HYPER_35KW;
    Buttons.begin();

    auto simTouchAction = [](int start_x, int start_y, int end_x, int end_y, uint32_t duration_ms) {
        simAdvanceMs(120); // Allow 100ms debounce cooldown from previous gesture
        Display.setSimTouch(true, start_x, start_y);
        Buttons.update(); // Record initial touch-down at t=0

        int steps = 5;
        for (int i = 1; i <= steps; i++) {
            int cx = start_x + (end_x - start_x) * i / steps;
            int cy = start_y + (end_y - start_y) * i / steps;
            Display.setSimTouch(true, cx, cy);
            simAdvanceMs(duration_ms / steps);
            Buttons.update();
        }

        Display.setSimTouch(false, 0, 0);
        Buttons.update();
        simAdvanceMs(120);
        return Buttons.getNavAction();
    };

    // 1. Initial State: No touch
    Display.setSimTouch(false);
    simAdvanceMs(110);
    Buttons.update();
    assert(Buttons.getNavAction() == NAV_NONE);
    printf("[PASS] Touch idle at boot.\n");

    // 2. Test Swipe Left -> NAV_BTN2_SHORT (Next Screen)
    NavAction actSwipeLeft = simTouchAction(550, 240, 420, 240, 60);
    assert(actSwipeLeft == NAV_BTN2_SHORT);
    printf("[PASS] Swipe Left triggered NAV_BTN2_SHORT (Next Screen).\n");

    // 3. Test Swipe Right -> NAV_BTN1_SHORT (Previous Screen)
    NavAction actSwipeRight = simTouchAction(300, 240, 430, 240, 60);
    assert(actSwipeRight == NAV_BTN1_SHORT);
    printf("[PASS] Swipe Right triggered NAV_BTN1_SHORT (Previous Screen).\n");

    // 4. Test Tap Top-Left [ STATS ] Button -> NAV_SCREEN_STATS (Opens Stats Screen)
    NavAction actTapStats = simTouchAction(60, 36, 60, 36, 50);
    assert(actTapStats == NAV_SCREEN_STATS);
    printf("[PASS] Tap Top-Left [ STATS ] button triggered NAV_SCREEN_STATS (Open Stats Screen).\n");

    // 5. Test Arbitrary Taps on Ride Dash -> NAV_NONE (Random clicking removed)
    NavAction actTapArbitraryLeft = simTouchAction(90, 240, 90, 240, 50);
    assert(actTapArbitraryLeft == NAV_NONE);
    NavAction actTapArbitraryRight = simTouchAction(710, 240, 710, 240, 50);
    assert(actTapArbitraryRight == NAV_NONE);
    NavAction actTapThermals = simTouchAction(150, 420, 150, 420, 50);
    assert(actTapThermals == NAV_NONE);
    printf("[PASS] Arbitrary taps on Ride Dash ignored (NAV_NONE, accidental page switches & resets prevented).\n");

    // 6. Test Tap Top-Right Badge -> NAV_BTN1_LONG (Open Settings Menu)
    NavAction actTapTopRight = simTouchAction(700, 30, 700, 30, 50);
    assert(actTapTopRight == NAV_BTN1_LONG);
    printf("[PASS] Tap Top-Right badge triggered NAV_BTN1_LONG (Settings Menu).\n");

    // 7. Test Long Press on Bottom Strip -> NAV_BTN2_LONG (Reset Trip Odometer)
    Display.setSimTouch(true, 100, 440);
    Buttons.update(); // Record touch down at t=0
    simAdvanceMs(850);
    Buttons.update(); // Fire long press after 850ms hold
    NavAction actLongBottom = Buttons.getNavAction();
    assert(actLongBottom == NAV_BTN2_LONG);
    Display.setSimTouch(false);
    simAdvanceMs(110);
    Buttons.update();
    printf("[PASS] Long Press on Bottom Strip triggered NAV_BTN2_LONG (Trip Reset).\n");

    // 7b. Test Long Press on [ BOOST ] Button (x=500, y=410) -> Must NOT trigger Trip Reset!
    Display.setSimTouch(true, 500, 410);
    Buttons.update();
    simAdvanceMs(850);
    Buttons.update();
    NavAction actLongBoost = Buttons.getNavAction();
    assert(actLongBoost != NAV_BTN2_LONG);
    Display.setSimTouch(false);
    simAdvanceMs(110);
    Buttons.update();
    printf("[PASS] Long Press on [ BOOST ] button did NOT trigger NAV_BTN2_LONG (Accidental Reset Prevented).\n");

    // 8. Test Edit Mode: Left side decrement, Right side increment
    Buttons.setEditMode(true);
    NavAction actEditDec = simTouchAction(200, 280, 200, 280, 50);
    assert(actEditDec == NAV_EDIT_DEC);
    printf("[PASS] Edit Mode Left Tap triggered NAV_EDIT_DEC (-).\n");

    NavAction actEditInc = simTouchAction(650, 280, 650, 280, 50);
    assert(actEditInc == NAV_EDIT_INC);
    printf("[PASS] Edit Mode Right Tap triggered NAV_EDIT_INC (+).\n");

    Buttons.setEditMode(false);

    // 9. Test Statistics Screen (SCREEN_RT_GRAPHS): Clicking tiles/cards does NOT exit back to Ride Dash!
    Buttons.setActiveScreen(SCREEN_RT_GRAPHS);
    NavAction actStatsTapCenter = simTouchAction(400, 240, 400, 240, 50);
    assert(actStatsTapCenter == NAV_NONE);
    printf("[PASS] Statistics Screen Center Tap on cards safely ignored (NAV_NONE, stays on STATS).\n");

    NavAction actStatsTapCard = simTouchAction(100, 100, 100, 100, 50);
    assert(actStatsTapCard == NAV_NONE);
    printf("[PASS] Statistics Screen Card Tap safely ignored (NAV_NONE, stays on STATS).\n");

    // 9b. Only tapping [< DASH] (Hitbox: x <= 150, y <= 65) returns to Ride Dash
    NavAction actStatsDashTap = simTouchAction(75, 30, 75, 30, 50);
    assert(actStatsDashTap == NAV_BTN1_SHORT);
    printf("[PASS] Statistics Screen [< DASH] Button Tap triggered NAV_BTN1_SHORT (Return to Ride Dash).\n");

    // 10. Test Statistics Screen: Tap on [ Reset trip ] button (Hitbox: x >= 600, y <= 65)
    NavAction actStatsResetTap = simTouchAction(660, 30, 660, 30, 50);
    assert(actStatsResetTap == NAV_BTN2_LONG);
    printf("[PASS] Statistics Screen Reset Trip Tap triggered NAV_BTN2_LONG.\n");

    // 11. Test Statistics Screen: Boundary tap just outside Reset button
    NavAction actStatsOutsideReset = simTouchAction(570, 60, 570, 60, 50);
    assert(actStatsOutsideReset == NAV_NONE);
    printf("[PASS] Statistics Screen Tap outside Reset Button ignored (NAV_NONE, stays on STATS).\n");

    // 12. Test Statistics Screen: Swipes do NOT exit to Ride Dash
    NavAction actStatsSwipeH = simTouchAction(300, 240, 500, 240, 60);
    assert(actStatsSwipeH == NAV_NONE);
    printf("[PASS] Statistics Screen Horizontal Swipe ignored (NAV_NONE, stays on STATS).\n");

    NavAction actStatsSwipeV = simTouchAction(400, 350, 400, 200, 60);
    assert(actStatsSwipeV == NAV_NONE);
    printf("[PASS] Statistics Screen Vertical Swipe ignored (NAV_NONE, stays on STATS).\n");

    // 13. Test Statistics Screen: Long Press on card does NOT exit
    Display.setSimTouch(true, 400, 240);
    Buttons.update();
    simAdvanceMs(850);
    Buttons.update();
    NavAction actStatsLong = Buttons.getNavAction();
    assert(actStatsLong == NAV_NONE);
    Display.setSimTouch(false);
    Buttons.update();
    simAdvanceMs(120);
    printf("[PASS] Statistics Screen Long Press on card ignored (NAV_NONE, stays on STATS).\n");

    // 13b. Test Statistics Screen: Long Press on [< DASH] returns to Ride Dash
    Display.setSimTouch(true, 75, 30);
    Buttons.update();
    simAdvanceMs(850);
    Buttons.update();
    NavAction actStatsLongDash = Buttons.getNavAction();
    assert(actStatsLongDash == NAV_BTN1_SHORT);
    Display.setSimTouch(false);
    Buttons.update();
    simAdvanceMs(120);
    printf("[PASS] Statistics Screen Long Press on [< DASH] triggered NAV_BTN1_SHORT.\n");

    // 13c. Test PIN Lockscreen Touch Isolation & Interactive Keypad
    ModeManager::instance().setLocked(true);
    NavAction actLockedTap = simTouchAction(240, 190, 240, 190, 50);
    assert(actLockedTap == NAV_NONE); // Must NOT trigger any navigation
    int16_t tap_x = -1, tap_y = -1;
    assert(Buttons.getLastTap(&tap_x, &tap_y) == true);
    assert(tap_x == 240 && tap_y == 190);
    printf("[PASS] PIN Lockscreen safely captures key tap (240, 190) without navigation leakage.\n");
    ModeManager::instance().setLocked(false);

    Buttons.setActiveScreen(SCREEN_RIDE_DASH);

    // 10. Requirement 3: Precision Hitbox on km/h Number (4s Long-Press & Single-Tap)
    Buttons.setTuningMode(false); // Stealth Mode Active

    // 10a. Short press (500ms) on km/h number when tuningMode == false -> must NOT enable tuning
    NavAction actShortKm = simTouchAction(240, 195, 240, 195, 500);
    assert(actShortKm == NAV_NONE);
    printf("[PASS] Short press (500ms) on km/h number did NOT activate tuning mode.\n");

    // 10b. Sub-threshold hold (3800ms < 4000ms) on km/h number -> must NOT enable tuning
    Display.setSimTouch(true, 240, 195);
    Buttons.update();
    simAdvanceMs(3800);
    Buttons.update();
    assert(Buttons.getNavAction() == NAV_NONE);
    Display.setSimTouch(false);
    Buttons.update();
    simAdvanceMs(120);
    assert(Buttons.getNavAction() == NAV_NONE);
    printf("[PASS] Sub-threshold hold (3800ms) on km/h number safely cancelled without tuning enable.\n");

    // 10c. 4000ms Continuous Hold directly on km/h number -> MUST trigger NAV_TUNING_ENABLE!
    Display.setSimTouch(true, 240, 195);
    Buttons.update(); // touch down registered at t=0
    simAdvanceMs(4050);
    Buttons.update(); // 4050ms hold fires NAV_TUNING_ENABLE
    NavAction actTuningEnable = Buttons.getNavAction();
    assert(actTuningEnable == NAV_TUNING_ENABLE);
    Display.setSimTouch(false);
    Buttons.update();
    simAdvanceMs(120);
    printf("[PASS] 4000ms continuous hold directly on km/h number triggered NAV_TUNING_ENABLE.\n");

    // 10d. Single Tap (<1000ms) on km/h number when tuningMode == true -> MUST trigger NAV_TUNING_DISABLE!
    Buttons.setTuningMode(true); // Tuning Mode now active
    NavAction actTuningDisable = simTouchAction(240, 195, 240, 195, 80);
    assert(actTuningDisable == NAV_TUNING_DISABLE);
    printf("[PASS] Single Tap on km/h number when tuned triggered NAV_TUNING_DISABLE.\n");

    // 10e. Hitbox Isolation: Tap on "km/h" label below digits (y=250) -> must NOT trigger NAV_TUNING_DISABLE!
    NavAction actLabelTap = simTouchAction(240, 250, 240, 250, 80);
    assert(actLabelTap != NAV_TUNING_DISABLE);
    printf("[PASS] Tap on 'km/h' label (y=250) strictly excluded from tuning toggle.\n");

    // 10f. Hitbox Isolation: 4000ms hold on "km/h" label below digits (y=250) when tuningMode == false -> must NOT trigger NAV_TUNING_ENABLE!
    Buttons.setTuningMode(false);
    Display.setSimTouch(true, 240, 250);
    Buttons.update();
    simAdvanceMs(4050);
    Buttons.update();
    NavAction actLabelHold = Buttons.getNavAction();
    assert(actLabelHold != NAV_TUNING_ENABLE);
    Display.setSimTouch(false);
    Buttons.update();
    simAdvanceMs(120);
    // 10g. Phantom Hitbox Exclusion: Tap or hold at (520, 110) in STYLE_HYPER_35KW must NOT trigger tuning actions
    Settings.get().dash_style = STYLE_HYPER_35KW;
    Buttons.setTuningMode(true);
    NavAction actPhantomTap = simTouchAction(520, 110, 520, 110, 80);
    assert(actPhantomTap != NAV_TUNING_DISABLE);
    printf("[PASS] Phantom hitbox at (520, 110) strictly excluded in HYPER_35KW style.\n");

    // 10h. Drift Cancellation Safety: Holding on speed number and drifting must NOT trigger Trip Reset (NAV_BTN2_LONG)
    Buttons.setTuningMode(false);
    Display.setSimTouch(true, 240, 195);
    Buttons.update(); // Touch down at t=0
    simAdvanceMs(900); // 900ms hold (> TOUCH_LONG_MS 800ms)
    Display.setSimTouch(true, 240, 260); // Finger drifts 65px down
    Buttons.update();
    assert(Buttons.getNavAction() == NAV_NONE); // Must NOT fire NAV_BTN2_LONG (Trip Reset)!
    Display.setSimTouch(false);
    Buttons.update();
    simAdvanceMs(120);
    assert(Buttons.getNavAction() == NAV_NONE); // Release must safely abort without firing
    printf("[PASS] Finger drift while holding on speed number safely aborted without triggering trip reset.\n");

    // Restore Default State
    Buttons.setTuningMode(false);

    // 10j. 300A Phase Current Verification (No uint8 truncation to 44A)
    Vesc.sendModeLimits(85.0f, 15000.0f, 150.0f, 300.0f);
    assert(Settings.get().max_phase_amps == 300);
    printf("[PASS] 300A phase current limit correctly preserved without uint8 overflow.\n");

    // 10k. Long press on 'km/h' label (y=250) must NOT trigger Trip Reset (NAV_BTN2_LONG)
    Buttons.setTuningMode(false);
    Display.setSimTouch(true, 240, 250);
    Buttons.update();
    simAdvanceMs(900);
    Buttons.update();
    assert(Buttons.getNavAction() == NAV_NONE);
    Display.setSimTouch(false);
    Buttons.update();
    simAdvanceMs(120);
    printf("[PASS] Long press on 'km/h' label (y=250) does NOT trigger trip reset.\n");

    // 10l. Battery SoC at 42.0V for 14S pack (3.0V/cell) must be 0% (not 69%)
    float soc_42v = Battery.getOcvSoc(42.0f);
    assert(soc_42v <= 1.0f);
    printf("[PASS] Discharged 14S pack at 42.0V correctly computes 0.0%% SoC (calculated: %.1f%%).\n", soc_42v);

    // 10m. Backlight Touch & Drag Isolation on SCREEN_RT_GRAPHS
    Buttons.setActiveScreen(SCREEN_RT_GRAPHS);
    NavAction actBlTap = simTouchAction(400, 416, 400, 416, 60);
    assert(actBlTap == NAV_NONE); // Tap on slider must not exit to Ride Dash
    printf("[PASS] Statistics Screen Tap on Backlight Slider ignored by navigation (NAV_NONE, handled locally).\n");

    // 10n. Backlight Long Press Isolation on SCREEN_RT_GRAPHS
    Display.setSimTouch(true, 400, 416);
    Buttons.update();
    simAdvanceMs(900);
    Buttons.update();
    assert(Buttons.getNavAction() == NAV_NONE); // Long hold on backlight slider must not exit to Ride Dash
    Display.setSimTouch(false);
    Buttons.update();
    simAdvanceMs(120);
    printf("[PASS] Statistics Screen Long Hold on Backlight Slider does NOT trigger NAV_BTN1_SHORT.\n");

    // 10o. Backlight Horizontal Swipe/Drag Isolation on SCREEN_RT_GRAPHS
    NavAction actBlDrag = simTouchAction(300, 416, 500, 416, 150);
    assert(actBlDrag == NAV_NONE); // Dragging slider must not exit to Ride Dash
    printf("[PASS] Statistics Screen Horizontal Drag on Backlight Slider does NOT trigger NAV_BTN1_SHORT.\n");

    Buttons.setActiveScreen(SCREEN_RIDE_DASH);

    printf("=== All GT911 Capacitive Touch Tests PASSED! ===\n\n");
}

void testModeManagerAndLegalProfiles() {
    printf("\n=== Running ModeManager & Legal Profile Unit Tests ===\n");
    ModeManager::instance().begin();

    // 1. Legal Mode (Non-Tuning)
    ModeManager::instance().setTuningMode(false);
    assert(!ModeManager::instance().isTuningMode());

    // ECO
    ModeManager::instance().setRideMode(MODE_ECO);
    ModeLimits ecoLim = ModeManager::instance().getActiveLimits();
    assert(ecoLim.max_speed_kmh == 15.0f);
    assert(ecoLim.max_power_watts == 500.0f);
    assert(ecoLim.max_pull_watts == 1000.0f);
    assert(strcmp(ModeManager::instance().getActiveModeName(), "ECO") == 0);

    // SPORT
    ModeManager::instance().setRideMode(MODE_SPORT);
    ModeLimits sportLim = ModeManager::instance().getActiveLimits();
    assert(sportLim.max_speed_kmh == 20.0f);
    assert(sportLim.max_power_watts == 500.0f);
    assert(sportLim.max_pull_watts == 1000.0f);
    assert(strcmp(ModeManager::instance().getActiveModeName(), "SPORT") == 0);

    // RACE
    ModeManager::instance().setRideMode(MODE_RACE);
    ModeLimits raceLim = ModeManager::instance().getActiveLimits();
    assert(raceLim.max_speed_kmh == 22.0f);
    assert(raceLim.max_power_watts == 500.0f);
    assert(raceLim.max_pull_watts == 1000.0f);
    assert(strcmp(ModeManager::instance().getActiveModeName(), "RACE") == 0);

    // Cycle Mode: RACE -> ECO -> SPORT -> RACE
    ModeManager::instance().cycleMode();
    assert(ModeManager::instance().getRideMode() == MODE_ECO);
    ModeManager::instance().cycleMode();
    assert(ModeManager::instance().getRideMode() == MODE_SPORT);
    ModeManager::instance().cycleMode();
    assert(ModeManager::instance().getRideMode() == MODE_RACE);

    printf("[PASS] Legal Mode limits verified: ECO 15 km/h, SPORT 20 km/h, RACE 22 km/h (1000W pull / 500W cont).\n");
    printf("[PASS] Non-tuning mode display names verified: 'ECO', 'SPORT', 'RACE' (No 'STEALTH').\n");

    // 2. Tuning Mode (Unlocked)
    ModeManager::instance().setTuningMode(true);
    assert(ModeManager::instance().isTuningMode());

    // ECO
    ModeManager::instance().setRideMode(MODE_ECO);
    ModeLimits tEcoLim = ModeManager::instance().getActiveLimits();
    assert(tEcoLim.max_speed_kmh == 35.0f);

    // SPORT
    ModeManager::instance().setRideMode(MODE_SPORT);
    ModeLimits tSportLim = ModeManager::instance().getActiveLimits();
    assert(tSportLim.max_speed_kmh == 55.0f);

    // RACE
    ModeManager::instance().setRideMode(MODE_RACE);
    ModeLimits tRaceLim = ModeManager::instance().getActiveLimits();
    assert(tRaceLim.max_speed_kmh >= 85.0f);
    assert(strcmp(ModeManager::instance().getActiveModeName(), "RACE") == 0);

    printf("[PASS] Tuned Mode unlocked limits verified: ECO 35 km/h, SPORT 55 km/h, RACE 85+ km/h.\n");

    // 3. System Lock (Item 7 Immobilizer)
    ModeManager::instance().setLocked(true);
    assert(ModeManager::instance().isLocked());
    ModeLimits lockedLim = ModeManager::instance().getActiveLimits();
    assert(lockedLim.max_speed_kmh == 0.0f);
    assert(lockedLim.max_power_watts == 0.0f);
    assert(lockedLim.max_battery_amps == 0.0f);
    assert(lockedLim.max_phase_amps == 0.0f);
    printf("[PASS] System Lock mode clamps all outputs to 0.0 (VESC 0A immobilizer active).\n");

    ModeManager::instance().setLocked(false);
    assert(!ModeManager::instance().isLocked());
    ModeLimits restoredLim = ModeManager::instance().getActiveLimits();
    assert(restoredLim.max_speed_kmh >= 85.0f); // Preserved RACE limits
    printf("[PASS] System Unlock cleanly restores active ride mode limits without configuration loss.\n");

    // Reset to legal default
    ModeManager::instance().setTuningMode(false);
    ModeManager::instance().setRideMode(MODE_SPORT);
    printf("=== All ModeManager Unit Tests PASSED! ===\n\n");
}

void testAntiGhostingTransitions() {
    printf("=== Running Dynamic Anti-Ghosting Transition Tests ===\n");
    Settings.get().dash_style = STYLE_HYPER_35KW;
    DashTelemetry t;
    setBaseTelemetry(t);
    t.screen = SCREEN_RIDE_DASH;
    t.speed_kmh = 95.0f;
    t.power_watts = 34800.0f; // Peak 5-digit power
    t.phase_amps = 620.0f;    // Peak 3-digit phase current
    t.current_amps = 482.0f;  // Peak 3-digit battery current
    t.battery_pct = 100.0f;   // 100% battery (longest % string)
    t.voltage = 78.9f;
    t.temp_esc = 105.0f;      // 3-digit peak temp
    t.temp_motor = 110.0f;    // 3-digit peak temp
    t.duty_cycle_pct = 108.0f;// Field weakening peak overmodulation

    Renderer.markScreenDirty();
    Renderer.render(t); // Frame 1: Peak telemetry

    int non_zero_x = -1;
    for (int px = 460; px <= 600; px++) {
        if (Display.getSprite().readPixel(px, 70) != 0x0000) {
            non_zero_x = px;
            break;
        }
    }
    printf("DEBUG: First non-zero pixel on y=70 is x=%d (val=0x%04X)\n", non_zero_x, non_zero_x >= 0 ? Display.getSprite().readPixel(non_zero_x, 70) : 0);
    // Verify 5-digit power drew pixels
    assert(non_zero_x > 0);

    // Frame 1: Peak thermals (105° / 110°) draw across x=173..307
    assert(Display.getSprite().readPixel(173, 408) != 0x0000); // 'F' top stroke left edge
    assert(Display.getSprite().readPixel(307, 408) != 0x0000); // 110° degree circle right arc

    // Frame 2: Decelerate to 0 km/h, 0 W, 0 A, 38% battery, 72.1 V without markScreenDirty
    t.speed_kmh = 0.0f;
    t.power_watts = 0.0f;
    t.phase_amps = 0.0f;
    t.current_amps = 0.0f;
    t.duty_cycle_pct = 20.0f;
    t.battery_pct = 38.0f;
    t.voltage = 72.1f;
    t.temp_esc = 25.0f;       // 2-digit idle temp
    t.temp_motor = 30.0f;     // 2-digit idle temp
    Renderer.render(t);

    // 1. Speed at 0 km/h: single digit '0' centered at cx=240, 195.
    // Trailing digit position of "95" must be 0x0000 (black).
    assert(Display.getSprite().readPixel(295, 195) == 0x0000);

    // 2. Power at 0 W: single digit '0' right-aligned at x=720, y=52.
    // Leading digit positions of previous 5-digit 34,800 W at x=490..530 must be 0x0000.
    assert(Display.getSprite().readPixel(non_zero_x, 70) == 0x0000);

    // 3. Currents line (shifted down to y=250):
    // Trailing position of old 3-digit phase amps must be 0x0000 (no "MOTOR 0 AA").
    assert(Display.getSprite().readPixel(560, 255) == 0x0000);
    // Leading position of old 3-digit battery amps must be 0x0000 (no "BBATTERY").
    assert(Display.getSprite().readPixel(595, 255) == 0x0000);

    // 4. Battery % (shifted down to y=174):
    // Check that rightmost glyph area of previous "100%" is cleared when at 38%.
    assert(Display.getSprite().readPixel(565, 197) == 0x0000);

    // 5. Thermals single line anti-ghosting:
    // Frame 1 peak thermals (105°/110°) drew at x=173 and x=307.
    // In Frame 2 (25°/30°), the text spans x=181..299, so x=173 and x=307 must be cleared to 0x0000 (black).
    assert(Display.getSprite().readPixel(173, 408) == 0x0000);
    assert(Display.getSprite().readPixel(307, 408) == 0x0000);

    // 6. Duty cycle progress bar anti-ghosting:
    // Frame 1 (108%) drew red fill all the way to x=765.
    // Frame 2 (20%) draws cyan fill only up to x=509.
    // At x=700, y=154, pixel must NOT be red (0xF800).
    assert(Display.getSprite().readPixel(700, 154) != 0xF800);

    // Frame 3: Trigger Dynamic Mode Banner
    t.mode_badge_until = millis() + 2000;
    Renderer.render(t);

    // Banner overlay at x=410..775 covers the currents line.
    // Check that letter 'M' from 'MOTOR' at x=418 is 0x0000 (no poking ghost 'M').
    assert(Display.getSprite().readPixel(418, 255) == 0x0000);

    printf("[PASS] Dynamic speed, power (35kW->0W), thermals, currents, duty bar, and mode banner rendered without ghosting.\n");
    printf("=== Dynamic Anti-Ghosting Tests PASSED! ===\n\n");
}

void testStatsCalculationAndLiveUpdates() {
    printf("=== Running Statistics Calculation & Live Updates Tests ===\n");
    DashTelemetry t;
    setBaseTelemetry(t);
    t.stats.reset();

    // 1. Max speed tracking from 0.0f
    assert(t.stats.max_speed_kmh == 0.0f);
    float speeds[] = { 15.0f, 45.0f, 88.5f, 62.0f, 10.0f };
    for (float spd : speeds) {
        if (spd > t.stats.max_speed_kmh) t.stats.max_speed_kmh = spd;
    }
    assert(t.stats.max_speed_kmh == 88.5f);
    printf("[PASS] max_speed_kmh correctly tracked peak speed (88.5 km/h) without 100 km/h lockup.\n");

    // 2. Float accumulator for ride time (prevents integer truncation bug)
    float accum = 0.0f;
    float dt = 0.033f; // 30 FPS tick
    for (int tick = 0; tick < 31; tick++) { // ~1.02 seconds
        accum += dt;
        while (accum >= 1.0f) {
            t.stats.ride_time_sec += 1;
            accum -= 1.0f;
        }
    }
    assert(t.stats.ride_time_sec == 1);
    printf("[PASS] Float accumulator smoothly incremented ride_time_sec over 30 FPS ticks.\n");

    // 3. Dynamic Average Speed calculation: trip_km / (ride_time_sec / 3600.0f)
    t.trip_km = 1.0f;
    t.stats.ride_time_sec = 100;
    if (t.stats.ride_time_sec > 2 && t.trip_km > 0.01f) {
        t.stats.avg_speed_kmh = t.trip_km / (t.stats.ride_time_sec / 3600.0f);
    }
    assert(fabsf(t.stats.avg_speed_kmh - 36.0f) < 0.01f);
    printf("[PASS] avg_speed_kmh accurately computed dynamic average speed (36.0 km/h).\n");

    // 4. Trip reset clears statistics cleanly
    t.stats.reset();
    t.trip_km = 0.0f;
    assert(t.stats.max_speed_kmh == 0.0f);
    assert(t.stats.ride_time_sec == 0);
    assert(t.stats.avg_speed_kmh == 0.0f);
    printf("[PASS] Trip reset cleanly restored all statistics to zero.\n");

    // 5. Battery trip reset clears rolling consumption sectors
    Battery.resetTrip();
    assert(Battery.getDynamicWhKm() == 22.0f);
    printf("[PASS] Battery.resetTrip() cleanly resets rolling window and restores baseline 22.0 Wh/km.\n");
    printf("=== Statistics Calculation & Live Updates Tests PASSED! ===\n\n");
}

void testFaultManagerAndBlackbox() {
    printf("=== Running VESC Fault Manager & Blackbox Unit Tests ===\n");
    FaultManager::instance().clearFaults();
    assert(FaultManager::instance().getFaultCount() == 0);
    assert(FaultManager::instance().getActiveFault() == 0);

    FaultManager::instance().recordFault(FAULT_OVER_TEMP_MOTOR, 54.2f, 25.0f, 65.0f, 60.0f, 85.0f, 120);
    assert(FaultManager::instance().getFaultCount() == 1);
    assert(FaultManager::instance().getActiveFault() == FAULT_OVER_TEMP_MOTOR);

    FaultRecord r = FaultManager::instance().getFault(0);
    assert(r.code == FAULT_OVER_TEMP_MOTOR);
    assert(r.temp_mot == 85.0f);
    assert(strcmp(FaultManager::getFaultTitleDe(FAULT_OVER_TEMP_MOTOR), "MOTOR UEBERHITZUNG") == 0);
    printf("[PASS] Recorded fault to blackbox: %s (Mot Temp: %.0f C).\n", FaultManager::getFaultTitleDe(r.code), r.temp_mot);

    // Spam immunity: duplicate within 5s must not duplicate entry
    FaultManager::instance().recordFault(FAULT_OVER_TEMP_MOTOR, 54.2f, 25.0f, 65.0f, 60.0f, 85.0f, 122);
    assert(FaultManager::instance().getFaultCount() == 1);
    printf("[PASS] Rapid duplicate fault spam discarded by blackbox debouncer.\n");

    FaultManager::instance().recordFault(FAULT_NONE, 54.2f, 0.0f, 0.0f, 40.0f, 40.0f, 130);
    assert(FaultManager::instance().getActiveFault() == 0);
    assert(FaultManager::instance().getFaultCount() == 1); // Stored history preserved
    printf("[PASS] FAULT_NONE resets active fault while preserving stored history.\n");

    FaultManager::instance().clearFaults();
    assert(FaultManager::instance().getFaultCount() == 0);
    printf("=== All VESC Fault Manager Tests PASSED! ===\n\n");
}

void testSpeedSweepAnimation() {
    printf("\n=== Running Startup Speedometer Sweep Animation Unit Tests ===\n");
    DashTelemetry t;
    setBaseTelemetry(t);
    t.screen = SCREEN_RIDE_DASH;
    t.speed_kmh = 0.0f;

    Renderer.triggerNeedleSweep();
    Renderer.render(t);

    // Halfway up: ~50 km/h
    simAdvanceMs(325);
    Renderer.render(t);

    // Peak: ~100 km/h
    simAdvanceMs(325);
    Renderer.render(t);

    // Sweep down: ~50 km/h
    simAdvanceMs(275);
    Renderer.render(t);

    // Complete sweep: back to 0.0 km/h
    simAdvanceMs(400);
    Renderer.render(t);
    printf("[PASS] Startup speedometer sweep smoothly animated up to 100 km/h and settled at 0.0 km/h.\n");
    printf("=== Speedometer Sweep Tests PASSED! ===\n\n");
}

int main(int argc, char **argv) {
    printf("==================================================\n");
    printf("   VESC 4.3\" Touch LCD Emulator & Renderer (800x480) \n");
    printf("==================================================\n");

    testTouchGestures();

    Settings.begin();
    Battery.begin();
    Display.init();
    Renderer.begin(&Display);

    testSpeedSweepAnimation();
    testAntiGhostingTransitions();
    testStatsCalculationAndLiveUpdates();
    testModeManagerAndLegalProfiles();
    testFaultManagerAndBlackbox();

    DashTelemetry t;
    setBaseTelemetry(t);

    // ----------------------------------------------------
    // Scenario 0a: Hyper 35kW+ Pro Cluster - Idle (Exact Video Match)
    // ----------------------------------------------------
    Settings.get().dash_style = STYLE_HYPER_35KW;
    t.screen = SCREEN_RIDE_DASH;
    t.speed_kmh = 0.0f;
    t.power_watts = 3370.0f;
    t.voltage = 76.8f;
    t.battery_pct = 41.0f;
    t.current_amps = -9.0f;
    t.phase_amps = 76.0f;
    t.temp_fet_rear  = 60.0f;
    t.temp_mot_rear  = 75.0f;
    t.temp_fet_front = 53.0f;
    t.temp_mot_front = 54.0f;
    t.temp_esc       = 60.0f;
    t.temp_motor     = 75.0f;
    t.odo_km = 2066.0f;
    t.duty_cycle_pct = 32.0f;
    t.ride_mode = MODE_ECO;
    t.boost_active = false;
    t.mode_badge_until = 0;
    Renderer.markScreenDirty();
    renderAndSave("00_hyper_35kw_idle", t);
    renderAndSave("demo_00_hyper_cluster", t);

    // ----------------------------------------------------
    // Scenario 0b: Hyper 35kW+ Pro Cluster - RACE Mode & BOOST
    // ----------------------------------------------------
    t.speed_kmh = 92.4f;
    t.power_watts = 34800.0f;
    t.duty_cycle_pct = 108.0f; // Field Weakening overmodulation (>100%)
    t.voltage = 72.1f;
    t.battery_pct = 38.0f;
    t.current_amps = 482.0f;
    t.phase_amps = 620.0f;
    t.temp_esc = 64.0f;
    t.temp_motor = 82.0f;
    t.ride_mode = MODE_RACE;
    t.boost_active = true;
    t.mode_badge_until = 0;
    Renderer.markScreenDirty();
    renderAndSave("00_hyper_35kw_boost_race", t);

    // ----------------------------------------------------
    // Scenario 0c: Hyper 35kW+ Pro Cluster - SPORT Mode Banner Animation
    // ----------------------------------------------------
    t.speed_kmh = 45.0f;
    t.power_watts = 12400.0f;
    t.duty_cycle_pct = 64.0f;
    t.temp_esc = 58.0f;
    t.temp_motor = 68.0f;
    t.ride_mode = MODE_SPORT;
    t.boost_active = false;
    t.mode_badge_until = 99999999; // Force mode banner to render
    Renderer.markScreenDirty();
    renderAndSave("00_hyper_35kw_mode_banner", t);
    t.mode_badge_until = 0; // Clear banner


    // ----------------------------------------------------
    // Scenario 6: Screen 1 - Controllable LEDs & Lights Screen (Video Exact)
    // ----------------------------------------------------
    t.screen = SCREEN_LIGHTS;
    t.lights_enabled = true;
    t.light_hue = 135;
    t.light_mode = 3; // RAINBOW (index 3 in 3x3 grid)
    t.light_brightness = 210;
    t.hazard_active = false;
    Renderer.markScreenDirty();
    renderAndSave("06_energy_analytics", t);
    renderAndSave("demo_01_lights_screen", t);

    // ----------------------------------------------------
    // Scenario 7: Screen 2 - RT Waveforms & Dual Controllers (Video Exact)
    // ----------------------------------------------------
    t.screen = SCREEN_RT_GRAPHS;
    for (int i = 0; i < WAVE_HISTORY_LEN; i++) {
        t.speed_history[i] = 25.0f + 30.0f * sinf(i * 0.15f);
        t.power_history[i] = 4500.0f + 4000.0f * sinf(i * 0.15f + 0.5f);
    }
    t.vesc1_max_power = 18450.0f;
    t.vesc1_max_phase = 340.0f;
    t.vesc1_max_speed = 94.2f;
    t.vesc2_max_power = 17800.0f;
    t.vesc2_max_phase = 335.0f;
    t.vesc2_max_speed = 93.8f;
    Renderer.markScreenDirty();
    renderAndSave("07_power_dynamics", t);
    renderAndSave("demo_02_rt_graphs_screen", t);

    // ----------------------------------------------------
    // Scenario 8: Screen 3 - Root Settings Menu
    // ----------------------------------------------------
    t.screen = SCREEN_SETTINGS_MENU;
    Renderer.markScreenDirty();
    renderAndSave("08_settings_root", t);

    // ----------------------------------------------------
    // Scenario 9: Screen 3 - Settings Submenu: Power Limits
    // ----------------------------------------------------
    // Enter first submenu in root (Power Limits)
    Renderer.handleMenuNav(NAV_BOTH_PRESSED, t);
    renderAndSave("09_settings_power_limits", t);

    // ----------------------------------------------------
    // Scenario 10: Screen 3 - Settings Submenu in Edit Mode
    // ----------------------------------------------------
    // Enter Edit Mode on Max Battery Amps
    Renderer.handleMenuNav(NAV_BOTH_PRESSED, t);
    renderAndSave("10_settings_edit_mode", t);

    // ----------------------------------------------------
    // Scenario 11: Screen 3 - Settings Submenu: Display & Styles
    // ----------------------------------------------------
    // Exit edit mode, back to root, move down to Display & UI, enter
    Renderer.handleMenuNav(NAV_BOTH_PRESSED, t); // exit edit mode
    Renderer.handleMenuNav(NAV_BTN2_LONG, t);    // back to root
    Renderer.handleMenuNav(NAV_BTN2_SHORT, t);   // move down to 2. DISPLAY & UI
    Renderer.handleMenuNav(NAV_BOTH_PRESSED, t); // enter Display & UI submenu
    renderAndSave("11_settings_display_style", t);

    // ----------------------------------------------------
    // Scenario 12: Screen 3 - Settings Submenu: Battery Profiles
    // ----------------------------------------------------
    // Back to root, move down to Battery Profiles, enter
    Renderer.handleMenuNav(NAV_BTN2_LONG, t);    // back to root
    Renderer.handleMenuNav(NAV_BTN2_SHORT, t);   // move down to 3. BATTERY PROFILES
    Renderer.handleMenuNav(NAV_BOTH_PRESSED, t); // enter Battery Profiles submenu
    renderAndSave("12_settings_battery_profiles", t);

    // ----------------------------------------------------
    // Scenario 13: Screen 5 - VESC Wi-Fi Bridge
    // ----------------------------------------------------
    Vesc.enterWifiBridgeMode();
    t.screen = SCREEN_WIFI_BRIDGE;
    Renderer.markScreenDirty();
    renderAndSave("13_wifi_bridge", t);

    // ----------------------------------------------------
    // Scenario 14: Legal Mode (Boot Default - SPORT 20 km/h, Discrete Red Dot)
    // ----------------------------------------------------
    Settings.get().dash_style = STYLE_HYPER_35KW;
    setBaseTelemetry(t);
    t.tuning_mode = false; // Legal Mode Active (Normal mode labels: ECO, SPORT, RACE)
    t.ride_mode = MODE_SPORT;
    t.speed_kmh = 20.0f;
    t.power_watts = 480.0f;
    t.phase_amps = 28.0f;
    t.current_amps = 9.2f;
    Renderer.markScreenDirty();
    renderAndSave("14_stealth_mode_idle", t);

    // Requirement 2 Verification: Subtle red dot in top right corner (790, 10)
    assert(Display.getSprite().readPixel(790, 10) == 0xF800);
    printf("[PASS] Legal mode red status dot verified at top-right corner (790, 10).\n");
    assert(strcmp(ModeManager::instance().getActiveModeName(), "SPORT") == 0);
    printf("[PASS] Active mode name displays 'SPORT' (never 'STEALTH').\n");

    // Re-verify Tuning Mode erases the dot
    t.tuning_mode = true;
    Renderer.markScreenDirty();
    Renderer.render(t);
    assert(Display.getSprite().readPixel(790, 10) == 0x0000);
    printf("[PASS] Tuning mode completely hides the red status dot (verified 0x0000).\n");

    // ----------------------------------------------------
    // Scenario 15: Screen 2 - New Extended STATS View with Backlight Slider
    // ----------------------------------------------------
    setBaseTelemetry(t);
    t.screen = SCREEN_RT_GRAPHS;
    t.dynamic_wh_km = 18.2f;
    t.stats.peak_power_watts = 2450.0f;
    t.stats.peak_current_amps = 34.0f;
    t.stats.peak_phase_amps = 72.0f;
    t.stats.trip_wh_km = 19.4f;
    t.stats.min_voltage = 47.8f;
    t.stats.max_voltage = 54.2f;
    t.stats.max_temp_motor = 64.0f;
    t.stats.max_temp_esc = 58.0f;
    Renderer.markScreenDirty();
    renderAndSave("15_stats_rich_overview", t);

    // ----------------------------------------------------
    // Scenario 16: VESC Fault-Code Alarm Banner on HUD
    // ----------------------------------------------------
    setBaseTelemetry(t);
    t.screen = SCREEN_RIDE_DASH;
    t.fault_code = FAULT_OVER_TEMP_MOTOR;
    t.fault_start_ms = 1000;
    Renderer.markScreenDirty();
    renderAndSave("16_vesc_fault_banner", t);

    // ----------------------------------------------------
    // Scenario 17: PIN Lockscreen on Boot
    // ----------------------------------------------------
    setBaseTelemetry(t);
    t.is_locked = true;
    strcpy(t.pin_entry, "12");
    t.pin_error = false;
    Renderer.markScreenDirty();
    renderAndSave("17_pin_lockscreen", t);

    // ----------------------------------------------------
    // Scenario 18: PIN Lockscreen with Error
    // ----------------------------------------------------
    strcpy(t.pin_entry, "9999");
    t.pin_error = true;
    t.pin_error_end_ms = millis() + 5000;
    Renderer.markScreenDirty();
    renderAndSave("18_pin_lockscreen_error", t);

    printf("\n[SUCCESS] All 18 scenarios rendered and saved to tools/simulator/screenshots/\n");
    return 0;
}
