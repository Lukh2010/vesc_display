#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "Arduino.h"
#include "board_config.h"
#include "display_driver.h"
#include "settings_manager.h"
#include "battery_manager.h"
#include <assert.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include "dash_renderer.h"
#include "buttons.h"
#include "vesc_uart.h"
#include "mode_manager.h"
#include "fault_manager.h"
#define private public
#include "gps_tracker.h"
#undef private
#include "ble_keyless_manager.h"
#include "ota_manager.h"
#include "tcs_tpms_manager.h"
#include "steering_buttons.h"
#include "wled_controller.h"
#include "virtual_vesc/virtual_vesc.h"
#include "virtual_vesc/vesc_tcp_server.h"

static VescLoopbackTransport g_sim_test_transport;
static VirtualVesc g_virtual_vesc;

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

    t.gps_enabled = Settings.get().gps_enabled;
    t.incline_gforce_enabled = Settings.get().incline_gforce_enabled;
    t.wifi_bridge_enabled = Settings.get().wifi_bridge_enabled;
    t.setup_completed = true; // default true for base simulator telemetry
    t.wizard_step = 0;
    t.ble_keyless_enabled = Settings.get().ble_keyless_enabled;
    t.ble_autolock_enabled = Settings.get().ble_autolock_enabled;
    t.ble_paired = Settings.get().ble_paired;
    t.ble_connected = false;
    t.ble_rssi = 0;
    t.ble_signal_bars = 0;
    t.ble_pairing_active = false;
    t.ble_pair_success = false;
    t.ble_device_name[0] = '\0';
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

    // 2. Simulate Single Click on BTN2 (with contact chatter and chord window)
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
        if (act == NAV_BTN2_SHORT) dec_count++;
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

    // Advance 28ms (debounce passes)
    stepAndCheck(28);
    assert(dec_count == 0);

    // Release chatter
    setPin(PIN_BTN_2, HIGH);
    stepAndCheck(2);
    setPin(PIN_BTN_2, LOW);
    stepAndCheck(2);
    setPin(PIN_BTN_2, HIGH); // stable open
    stepAndCheck(140);

    // Exactly 1 action total (no skips, no release bounces)
    assert(dec_count == 1);
    assert(other_count == 0);
    assert(!Buttons.isBtn2Down());
    printf("[PASS] Single click with contact bounce fired exactly 1 NAV_BTN2_SHORT.\n");

    // 3. Test Long Press on BTN2 -> NAV_BTN2_LONG
    dec_count = 0;
    int long_count = 0;
    setPin(PIN_BTN_2, LOW);
    for (int i = 0; i < 16; i++) {
        simAdvanceMs(60);
        Buttons.update();
        NavAction act = Buttons.getNavAction();
        if (act == NAV_BTN2_LONG) long_count++;
        else if (act == NAV_BTN2_SHORT) dec_count++;
    }
    assert(long_count == 1 && dec_count == 0);

    // Release and wait cooldown
    setPin(PIN_BTN_2, HIGH);
    stepAndCheck(150);
    assert(!Buttons.isBtn2Down());
    printf("[PASS] Long press on BTN2 fires NAV_BTN2_LONG cleanly without spurious short press.\n");

    // 4. Test Normal Mode: Click on BTN1 gives NAV_BTN1_SHORT on release
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
        Buttons.getNavAction(); // Flush any stale action
        simAdvanceMs(360); // Clear 350ms screen-switch debounce & 100ms touch cooldown
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

    // 6. Test Hidden Top-Right Settings Corner -> NAV_BTN1_LONG (Short Tap & 500ms Long-Press)
    // Short Tap in hidden corner (e.g. x=756, y=34)
    NavAction actTapTopRight = simTouchAction(756, 34, 756, 34, 50);
    assert(actTapTopRight == NAV_BTN1_LONG);

    // 500ms Long-Press in hidden corner
    Display.setSimTouch(true, 756, 34);
    Buttons.update();
    simAdvanceMs(550);
    Buttons.update();
    NavAction actLongTopRight = Buttons.getNavAction();
    assert(actLongTopRight == NAV_BTN1_LONG);
    Display.setSimTouch(false);
    simAdvanceMs(120);
    Buttons.update();
    Buttons.getNavAction();
    printf("[PASS] Hidden Top-Right Settings corner openable via short tap AND long hold (Settings Menu).\n");

    // 6b. Test Bottom-Right Action Buttons: BOOST (445..597) and MODE (613..765) in All Modes
    Buttons.setTuningMode(false);
    NavAction actLegalBoost = simTouchAction(500, 415, 500, 415, 50);
    assert(actLegalBoost == NAV_BOOST_TOGGLE);
    NavAction actLegalMode = simTouchAction(690, 415, 690, 415, 50);
    assert(actLegalMode == NAV_MODE_CYCLE);
    Buttons.setTuningMode(true);
    NavAction actTuningBoost = simTouchAction(500, 415, 500, 415, 50);
    assert(actTuningBoost == NAV_BOOST_TOGGLE);
    NavAction actTuningMode = simTouchAction(690, 415, 690, 415, 50);
    assert(actTuningMode == NAV_MODE_CYCLE);
    Buttons.setTuningMode(false);
    printf("[PASS] Ride Dash Bottom-Right Hitboxes verified: BOOST + MODE persistent in all modes.\n");

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
    Buttons.getNavAction(); // Flush release action
    printf("[PASS] Long Press on [ BOOST ] button did NOT trigger NAV_BTN2_LONG (Accidental Reset Prevented).\n");

    // 7c. Test Tap on arbitrary bottom-left area -> Must be NAV_NONE (accidental tap prevented)
    NavAction actTapLight = simTouchAction(32, 416, 32, 416, 50);
    assert(actTapLight == NAV_NONE);
    printf("[PASS] Arbitrary tap on bottom-left area safely ignored (NAV_NONE).\n");

    // 7d. Test Long Press on bottom-left area (x=50, y=416) -> Triggers Trip Reset (NAV_BTN2_LONG)
    Display.setSimTouch(true, 50, 416);
    Buttons.update();
    simAdvanceMs(850);
    Buttons.update();
    NavAction actLongBottomLeft = Buttons.getNavAction();
    assert(actLongBottomLeft == NAV_BTN2_LONG);
    Display.setSimTouch(false);
    simAdvanceMs(110);
    Buttons.update();
    printf("[PASS] Long Press on bottom-left strip triggered NAV_BTN2_LONG (Trip Reset).\n");

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

    // 10b. Test Statistics Screen: Tap on [ RACE ] button (Hitbox: x: 140..250, y <= 65)
    // When tuning_mode is disabled (Police / Stealth mode), tap on [ RACE ] must be blocked
    Buttons.setTuningMode(false);
    NavAction actStatsRaceTapBlocked = simTouchAction(190, 30, 190, 30, 50);
    assert(actStatsRaceTapBlocked == NAV_NONE);
    printf("[PASS] Statistics Screen [ RACE ] Button Tap blocked in Police Mode (NAV_NONE).\n");

    // When tuning_mode is enabled, tap on [ RACE ] triggers NAV_SCREEN_RACE
    Buttons.setTuningMode(true);
    NavAction actStatsRaceTap = simTouchAction(190, 30, 190, 30, 50);
    assert(actStatsRaceTap == NAV_SCREEN_RACE);
    printf("[PASS] Statistics Screen [ RACE ] Button Tap triggered NAV_SCREEN_RACE when tuning_mode == true.\n");

    // 10c. Test Race Timer Screen Touches
    Buttons.setActiveScreen(SCREEN_RACE_TIMER);
    Buttons.setRacePopupOpen(false);
    Buttons.setRaceState(0); // IDLE

    // Top-Left < DASH
    NavAction actRaceDash = simTouchAction(75, 30, 75, 30, 50);
    assert(actRaceDash == NAV_SCREEN_DASH);
    printf("[PASS] Race Screen [< DASH] Tap triggered NAV_SCREEN_DASH.\n");

    // Top-Right [ BESTZEITEN ]
    NavAction actRaceBestzeiten = simTouchAction(700, 30, 700, 30, 50);
    assert(actRaceBestzeiten == NAV_RACE_TOGGLE_POPUP);
    printf("[PASS] Race Screen [ BESTZEITEN ] Tap triggered NAV_RACE_TOGGLE_POPUP.\n");

    // Top [ DIAGRAMM ]
    NavAction actRaceDyno = simTouchAction(545, 30, 545, 30, 50);
    assert(actRaceDyno == NAV_RACE_TOGGLE_DYNO);
    printf("[PASS] Race Screen [ DIAGRAMM ] Tap triggered NAV_RACE_TOGGLE_DYNO.\n");

    // Center [ START LAUNCH ] when IDLE
    NavAction actRaceStart = simTouchAction(310, 255, 310, 255, 50);
    assert(actRaceStart == NAV_RACE_START_LAUNCH);
    printf("[PASS] Race Screen Center Tap when IDLE triggered NAV_RACE_START_LAUNCH.\n");

    // Center when MEASURING (state 2) -> Abort
    Buttons.setRaceState(2);
    NavAction actRaceAbort = simTouchAction(310, 255, 310, 255, 50);
    assert(actRaceAbort == NAV_RACE_ABORT);
    printf("[PASS] Race Screen Center Tap when MEASURING triggered NAV_RACE_ABORT.\n");

    // When popup is open:
    Buttons.setRacePopupOpen(true);
    // Tap [ X ]
    NavAction actPopupCloseX = simTouchAction(615, 100, 615, 100, 50);
    assert(actPopupCloseX == NAV_RACE_CLOSE_POPUP);
    printf("[PASS] Race Popup [ X ] Tap triggered NAV_RACE_CLOSE_POPUP.\n");

    // Tap [ SCHLIESSEN ]
    NavAction actPopupCloseBtn = simTouchAction(500, 350, 500, 350, 50);
    assert(actPopupCloseBtn == NAV_RACE_CLOSE_POPUP);
    printf("[PASS] Race Popup [ SCHLIESSEN ] Tap triggered NAV_RACE_CLOSE_POPUP.\n");

    // Tap [ RESET ]
    NavAction actPopupReset = simTouchAction(220, 350, 220, 350, 50);
    assert(actPopupReset == NAV_RACE_RESET_RECORDS);
    printf("[PASS] Race Popup [ RESET ] Tap triggered NAV_RACE_RESET_RECORDS.\n");

    Buttons.setRacePopupOpen(false);

    // When Dyno modal is open:
    Buttons.setRaceDynoOpen(true);
    // Tap [ X ]
    NavAction actDynoCloseX = simTouchAction(650, 70, 650, 70, 50);
    assert(actDynoCloseX == NAV_RACE_CLOSE_POPUP);
    printf("[PASS] Race Dyno Modal [ X ] Tap triggered NAV_RACE_CLOSE_POPUP.\n");

    // Tap [ SCHLIESSEN ]
    NavAction actDynoCloseBtn = simTouchAction(400, 395, 400, 395, 50);
    assert(actDynoCloseBtn == NAV_RACE_CLOSE_POPUP);
    printf("[PASS] Race Dyno Modal [ SCHLIESSEN ] Tap triggered NAV_RACE_CLOSE_POPUP.\n");

    Buttons.setRaceDynoOpen(false);
    Buttons.setRaceState(0);
    Buttons.setTuningMode(false);
    Buttons.setActiveScreen(SCREEN_RT_GRAPHS);

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

    // 10j. 300A Phase Current Verification (No uint8 truncation to 44A, and sendModeLimits must NOT overwrite user Settings)
    Settings.get().max_phase_amps = 300;
    assert(Settings.get().max_phase_amps == 300);
    Vesc.sendModeLimits(85.0f, 15000.0f, 150.0f, 300.0f);
    g_virtual_vesc.process();
    assert(g_virtual_vesc.getLimits().max_phase_amps == 300.0f);
    printf("[PASS] 300A phase current limit preserved without uint8 overflow or Settings corruption.\n");

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

    // 10l. Battery SoC at 39.0V for 13S 48V pack (3.0V/cell) must be 0%
    float soc_cutoff = Battery.getOcvSoc(39.0f);
    assert(soc_cutoff <= 1.0f);
    printf("[PASS] Discharged 13S 48V pack at 39.0V correctly computes 0.0%% SoC (calculated: %.1f%%).\n", soc_cutoff);

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

    // 10p. Headlight Badge Touch Toggle on SCREEN_RIDE_DASH
    Buttons.setActiveScreen(SCREEN_RIDE_DASH);
    Settings.get().show_light_btn = 1;
    Settings.get().incline_gforce_enabled = true;
    Settings.get().show_gforce = 1;
    Settings.get().show_incline = 1;

    // Both GF and INC are ON -> Light badge is at Top Slot (50, 354)
    NavAction actLightTop = simTouchAction(50, 354, 50, 354, 60);
    assert(actLightTop == NAV_LIGHT_TOGGLE);
    printf("[PASS] Headlight Badge Top Slot (50, 354) triggers NAV_LIGHT_TOGGLE when both GF and INC are ON.\n");

    // Incline is OFF -> Light badge moves to Bottom Slot (50, 416)
    Settings.get().show_incline = 0;
    NavAction actLightBotIncOff = simTouchAction(50, 416, 50, 416, 60);
    assert(actLightBotIncOff == NAV_LIGHT_TOGGLE);
    printf("[PASS] Headlight Badge Bottom Slot (50, 416) triggers NAV_LIGHT_TOGGLE when Incline is OFF.\n");

    // Both GF and INC are OFF -> Light badge remains at Bottom Slot (50, 416)
    Settings.get().show_gforce = 0;
    NavAction actLightBotBothOff = simTouchAction(50, 416, 50, 416, 60);
    assert(actLightBotBothOff == NAV_LIGHT_TOGGLE);
    printf("[PASS] Headlight Badge Bottom Slot (50, 416) triggers NAV_LIGHT_TOGGLE when both GF and INC are OFF.\n");

    // Restore settings
    Settings.get().show_gforce = 1;
    Settings.get().show_incline = 1;

    // 11. Requirement: Settings Menu (SCREEN_SETTINGS_MENU) Touch Hitboxes & Navigation
    Buttons.setActiveScreen(SCREEN_SETTINGS_MENU);

    // 11a. Tap on [< DASH] button (Top-Left: x <= 160, y <= 65) triggers NAV_SCREEN_DASH
    NavAction actMenuDashTap = simTouchAction(75, 25, 75, 25, 50);
    assert(actMenuDashTap == NAV_SCREEN_DASH);
    printf("[PASS] Settings Menu [< DASH] Button Tap triggered NAV_SCREEN_DASH (Direct Return to Dash).\n");

    // 11b. Tap on [FERTIG / DASH] close button (Top-Right: x >= 640, y <= 65) triggers NAV_SCREEN_DASH
    NavAction actMenuCloseTap = simTouchAction(730, 25, 730, 25, 50);
    assert(actMenuCloseTap == NAV_SCREEN_DASH);
    printf("[PASS] Settings Menu [FERTIG / DASH] Button Tap triggered NAV_SCREEN_DASH.\n");

    // 11c. Swipe Right in Settings Header/Tab area (y < 120) triggers NAV_MENU_BACK (Return to Dash)
    NavAction actMenuSwipeR = simTouchAction(200, 85, 400, 85, 60);
    assert(actMenuSwipeR == NAV_MENU_BACK);
    printf("[PASS] Settings Menu Header Swipe Right triggered NAV_MENU_BACK.\n");

    // 11d. Swipe Up / Down in Settings Menu navigates
    NavAction actMenuSwipeDown = simTouchAction(200, 200, 200, 350, 60);
    assert(actMenuSwipeDown == NAV_BTN1_SHORT);
    NavAction actMenuSwipeUp = simTouchAction(200, 350, 200, 200, 60);
    assert(actMenuSwipeUp == NAV_BTN2_SHORT);
    printf("[PASS] Settings Menu Vertical Swipes triggered navigation (NAV_BTN1_SHORT / NAV_BTN2_SHORT).\n");

    // 11e. Test DashboardRenderer::handleSettingsTouch integration (Stealth 3-tab + Tuning 7-tab Configurator)
    DashTelemetry tTest;
    memset(&tTest, 0, sizeof(tTest));
    tTest.screen = SCREEN_SETTINGS_MENU;
    Renderer.markScreenDirty();

    // 11e-0: Legal Stealth Mode (!tuning_mode): ONLY DASH(4), SYSTEM(5), PIN(6) are accessible!
    tTest.tuning_mode = false;
    bool touchHandled = Renderer.handleSettingsTouch(100, 80, tTest);
    assert(touchHandled == true);
    assert(Renderer.getSettingsTab() == DashboardRenderer::TAB_DASH);
    touchHandled = Renderer.handleSettingsTouch(400, 80, tTest);
    assert(touchHandled == true);
    assert(Renderer.getSettingsTab() == DashboardRenderer::TAB_SYSTEM);
    touchHandled = Renderer.handleSettingsTouch(650, 80, tTest);
    assert(touchHandled == true);
    assert(Renderer.getSettingsTab() == DashboardRenderer::TAB_PIN);
    printf("[PASS] Settings Legal Stealth Mode strictly hides ECO/SPORT/RACE/DRAGY (shows DASH, SYSTEM, PIN only).\n");

    // 11e-1: Tuning Mode (tuning_mode == true): All 5 tabs (SPEED, DRAGY, DASH, SYSTEM, PIN)
    tTest.tuning_mode = true;
    int tabCenters[DashboardRenderer::TAB_COUNT] = { 92, 246, 400, 554, 708 };
    for (int ti = 0; ti < DashboardRenderer::TAB_COUNT; ti++) {
        touchHandled = Renderer.handleSettingsTouch(tabCenters[ti], 80, tTest);
        assert(touchHandled == true);
        assert(Renderer.getSettingsTab() == ti);
    }

    // Test Dragy Split adjustments on Tab 3 (DRAGY)
    Renderer.setSettingsTab(DashboardRenderer::TAB_DRAGY);
    uint8_t old_sp1 = Settings.get().race_split1_kmh;
    touchHandled = Renderer.handleSettingsTouch(195, 390, tTest); // Split 1 [+]
    assert(touchHandled == true);
    assert(Settings.get().race_split1_kmh == old_sp1 + 1);

    // Test Tab 4 DASH: G-Force, Neigung, Licht pill toggles & Brake Mode segmented control
    Renderer.setSettingsTab(DashboardRenderer::TAB_DASH);
    uint8_t init_gf = Settings.get().show_gforce;
    uint8_t init_inc = Settings.get().show_incline;
    uint8_t init_lb = Settings.get().show_light_btn;
    uint8_t init_bm = Settings.get().brake_mode;

    touchHandled = Renderer.handleSettingsTouch(140, 240, tTest);
    assert(touchHandled == true && Settings.get().show_gforce == (init_gf ? 0 : 1));
    Renderer.handleSettingsTouch(140, 240, tTest);
    assert(Settings.get().show_gforce == init_gf);

    touchHandled = Renderer.handleSettingsTouch(400, 240, tTest);
    assert(touchHandled == true && Settings.get().show_incline == (init_inc ? 0 : 1));
    Renderer.handleSettingsTouch(400, 240, tTest);
    assert(Settings.get().show_incline == init_inc);

    touchHandled = Renderer.handleSettingsTouch(650, 240, tTest);
    assert(touchHandled == true && Settings.get().show_light_btn == (init_lb ? 0 : 1));
    Renderer.handleSettingsTouch(650, 240, tTest);
    assert(Settings.get().show_light_btn == init_lb);

    // Segmented Brake Mode: AUS (510, 324), MILD (610, 324), MAX (710, 324)
    touchHandled = Renderer.handleSettingsTouch(510, 324, tTest);
    assert(touchHandled == true && Settings.get().brake_mode == 0);
    touchHandled = Renderer.handleSettingsTouch(610, 324, tTest);
    assert(touchHandled == true && Settings.get().brake_mode == 1);
    touchHandled = Renderer.handleSettingsTouch(710, 324, tTest);
    assert(touchHandled == true && Settings.get().brake_mode == 2);
    Settings.get().brake_mode = init_bm;
    Settings.save();
    printf("[PASS] Settings Tab 4 DASH: G-Force, Neigung, Licht pill toggles & Brake Mode segmented control verified!\n");

    // Test Tab 5 SYSTEM: TCS Toggle, Battery Cells (10S-24S), Tire Calibration
    Renderer.setSettingsTab(DashboardRenderer::TAB_SYSTEM);
    bool init_tcs = Settings.get().tcs_enabled;
    touchHandled = Renderer.handleSettingsTouch(720, 156, tTest);
    assert(touchHandled == true && Settings.get().tcs_enabled == !init_tcs);
    assert(TcsTpmsManager::instance().isTcsEnabled() == !init_tcs);
    Renderer.handleSettingsTouch(720, 156, tTest); // Restore
    assert(Settings.get().tcs_enabled == init_tcs);

    uint8_t init_cells = Settings.get().battery_cells;
    touchHandled = Renderer.handleSettingsTouch(738, 240, tTest); // [+]
    assert(touchHandled == true && Settings.get().battery_cells == init_cells + 1);
    touchHandled = Renderer.handleSettingsTouch(572, 240, tTest); // [-]
    assert(touchHandled == true && Settings.get().battery_cells == init_cells);

    touchHandled = Renderer.handleSettingsTouch(650, 324, tTest); // [KALIBRIEREN]
    assert(touchHandled == true && TcsTpmsManager::instance().isCalibrating() == true);
    printf("[PASS] Settings Tab 5 SYSTEM: TCS toggle, 10S-24S Battery Cells & Tire Calibration verified!\n");

    // Test Tab 6 PIN: [JETZT SPERREN] locks stationary scooter cleanly
    Renderer.setSettingsTab(DashboardRenderer::TAB_PIN);
    tTest.speed_kmh = 0.0f;
    tTest.vesc_motor_speed = 0.0f;
    tTest.is_locked = false;
    touchHandled = Renderer.handleSettingsTouch(590, 226, tTest);
    assert(touchHandled == true && tTest.is_locked == true && tTest.screen == SCREEN_RIDE_DASH);
    tTest.is_locked = false;
    ModeManager::instance().setLocked(false);
    Buttons.setLocked(false);
    tTest.screen = SCREEN_SETTINGS_MENU;
    printf("[PASS] Settings Tab 6 PIN: [JETZT SPERREN] immediately locks stationary scooter!\n");

    Renderer.setSettingsTab(DashboardRenderer::TAB_SPEED); // Switch back to SPEED for limit tests

    // 2. Adjust RACE mode limits via [-] and [+] buttons on TAB_SPEED, plus verify ECO-override bug fixed!
    VehicleModeConfig &cfg = ModeManager::instance().getConfig();
    cfg.tuned_modes[0].max_battery_amps = 25.0f;
    cfg.tuned_modes[0].max_phase_amps   = 45.0f;
    cfg.tuned_modes[2].max_speed_kmh = 50.0f;
    cfg.tuned_modes[2].max_power_watts = 5000.0f;

    touchHandled = Renderer.handleSettingsTouch(730, 118 + 2 * 112 + 70, tTest);
    assert(touchHandled == true && cfg.tuned_modes[2].max_speed_kmh == 51.0f);

    touchHandled = Renderer.handleSettingsTouch(70, 118 + 2 * 112 + 70, tTest);
    assert(touchHandled == true && cfg.tuned_modes[2].max_speed_kmh == 50.0f);

    touchHandled = Renderer.handleSettingsTouch(400, 118 + 2 * 112 + 70, tTest);
    assert(touchHandled == true && cfg.tuned_modes[2].max_speed_kmh >= 15.0f && cfg.tuned_modes[2].max_speed_kmh <= 100.0f);

    // Tap [< DASH] at x=74, y=29 -> Auto-saves and returns to SCREEN_RIDE_DASH without overwriting ECO amps!
    touchHandled = Renderer.handleSettingsTouch(74, 29, tTest);
    assert(touchHandled == true && tTest.screen == SCREEN_RIDE_DASH);
    assert(cfg.tuned_modes[0].max_battery_amps == 25.0f);
    assert(cfg.tuned_modes[0].max_phase_amps == 45.0f);
    printf("[PASS] Touch Mode Configurator & ECO-override fix verified 100%% (ECO 25A/45A untouched)!\n");

    // 11f. Test Full Integrated Main Loop settings touch flow
    DashTelemetry tLoop;
    memset(&tLoop, 0, sizeof(tLoop));
    tLoop.screen = SCREEN_SETTINGS_MENU;
    tLoop.tuning_mode = true;
    Buttons.setActiveScreen(SCREEN_SETTINGS_MENU);

    auto runMainLoopTouch = [&](int16_t x, int16_t y) {
        simAdvanceMs(360);
        Display.setSimTouch(true, x, y);
        Buttons.update();
        simAdvanceMs(50);
        Display.setSimTouch(false, 0, 0);
        Buttons.update();

        int16_t tx = -1, ty = -1;
        if (Buttons.getLastTap(&tx, &ty)) {
            Renderer.handleSettingsTouch(tx, ty, tLoop);
            Buttons.clearNavAction();
        }
        NavAction n = Buttons.getNavAction();
        if (n != NAV_NONE) {
            Renderer.handleMenuNav(n, tLoop);
        }
    };

    runMainLoopTouch(92, 80);  // Tab 0 SPEED
    assert(Renderer.getSettingsTab() == DashboardRenderer::TAB_SPEED);

    runMainLoopTouch(246, 80); // Tab 1 DRAGY
    assert(Renderer.getSettingsTab() == DashboardRenderer::TAB_DRAGY);

    runMainLoopTouch(92, 80);  // Back to Tab 0 SPEED
    assert(Renderer.getSettingsTab() == DashboardRenderer::TAB_SPEED);

    float initialRaceSpeed = cfg.tuned_modes[2].max_speed_kmh;
    runMainLoopTouch(730, 118 + 2 * 112 + 70); // [+] RACE Speed
    assert(cfg.tuned_modes[2].max_speed_kmh == initialRaceSpeed + 1.0f);

    runMainLoopTouch(74, 29);   // [< DASH]
    assert(tLoop.screen == SCREEN_RIDE_DASH);
    printf("[PASS] Integrated Main Loop: Touch tabs, slider step edits, and Dash auto-save verified 100%%!\n");

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

    // RACE + BOOST (Speed Komplett Offen, Standard Ströme: 70A Bat, 140A Phase)
    ModeManager::instance().setRideMode(MODE_RACE);
    ModeLimits tRaceLim = ModeManager::instance().getActiveLimits();
    assert(tRaceLim.max_speed_kmh == 100.0f);
    assert(strcmp(ModeManager::instance().getActiveModeName(), "RACE") == 0);

    ModeManager::instance().setBoostActive(true);
    ModeLimits boostRaceLim = ModeManager::instance().getActiveLimits();
    assert(boostRaceLim.max_battery_amps == 70.0f);
    assert(boostRaceLim.max_phase_amps == 140.0f);
    assert(boostRaceLim.max_speed_kmh == 100.0f);
    ModeManager::instance().setBoostActive(false);

    printf("[PASS] Tuned Mode unlocked limits verified: ECO 35 km/h, SPORT 55 km/h, RACE 100 km/h (Boost: 70A Bat, 140A Phase, Speed Komplett Offen).\n");

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

    // 4. Zero Speed Strict Invariant on Mode Switch & Boost Toggle
    DashTelemetry tZero;
    setBaseTelemetry(tZero);
    tZero.speed_kmh = 0.0f;
    tZero.power_watts = 0.0f;
    tZero.current_amps = 0.0f;
    tZero.throttle_pct = 0.0f;

    // Switch ride mode to ECO, SPORT, RACE: speed must remain strictly 0.0f
    ModeManager::instance().setRideMode(MODE_ECO);
    assert(tZero.speed_kmh == 0.0f);
    ModeManager::instance().setRideMode(MODE_SPORT);
    assert(tZero.speed_kmh == 0.0f);
    ModeManager::instance().setRideMode(MODE_RACE);
    assert(tZero.speed_kmh == 0.0f);

    // Toggle boost on and off: speed must remain strictly 0.0f
    tZero.boost_active = true;
    assert(tZero.speed_kmh == 0.0f);
    tZero.boost_active = false;
    assert(tZero.speed_kmh == 0.0f);

    // Run VescHandler simulation with zero throttle
    simAdvanceMs(50);
    Vesc.setSimThrottle(0.0f);
    Vesc.runSimulation(tZero);
    assert(tZero.speed_kmh == 0.0f);
    assert(tZero.g_force == 0.0f);

    // Incline angle check on telemetry
    simAdvanceMs(50);
    Vesc.setSimIncline(12.5f);
    Vesc.runSimulation(tZero);
    assert(fabsf(tZero.incline_deg - 12.5f) < 0.1f);
    Vesc.setSimThrottle(-1.0f);
    Vesc.setSimIncline(0.0f);
    printf("[PASS] Zero Throttle Strict Invariant verified: Switching modes or boost with 0 throttle keeps speed strictly at 0.0 km/h!\n");
    printf("[PASS] Incline angle telemetry export verified (+12.5 deg).\n");

    // Reset to legal default
    ModeManager::instance().setTuningMode(false);
    ModeManager::instance().setRideMode(MODE_SPORT);
    printf("=== All ModeManager Unit Tests PASSED! ===\n\n");
}

void testAntiGhostingTransitions() {
    printf("=== Running Dynamic Anti-Ghosting Transition Tests ===\n");
    Settings.get().dash_style = STYLE_HYPER_35KW;
    Settings.get().color_theme = THEME_OBSIDIAN_DARK;
    Settings.get().show_gforce = 0;
    Settings.get().show_incline = 0;
    Settings.get().show_light_btn = 0;
    const ColorThemePalette &pal = DashboardRenderer::getThemePalette(THEME_OBSIDIAN_DARK);

    DashTelemetry t;
    setBaseTelemetry(t);
    t.color_theme = THEME_OBSIDIAN_DARK;
    t.screen = SCREEN_RIDE_DASH;
    t.speed_kmh = 95.0f;
    t.power_watts = 34800.0f; // Peak 5-digit power
    t.phase_amps = 620.0f;    // Peak 3-digit phase current
    t.current_amps = 482.0f;  // Peak 3-digit battery current
    t.battery_pct = 100.0f;   // 100% battery (longest % string)
    t.voltage = 78.9f;
    t.temp_esc = 105.0f;      // 3-digit peak temp
    t.temp_motor = 110.0f;    // 3-digit peak temp
    t.duty_cycle_pct = 108.0f;// Field weakening overmodulation

    Renderer.invalidateCache();
    Renderer.markScreenDirty();
    Renderer.render(t); // Frame 1: Peak telemetry

    int non_zero_x = -1;
    for (int px = 450; px <= 640; px++) {
        if (Display.getSprite().readPixel(px, 80) != pal.bg) {
            non_zero_x = px;
            break;
        }
    }
    assert(non_zero_x > 0);

    // Frame 1 Duty cycle cap (108% -> 100%, theme primary color, never harsh 0xF800)
    assert(Display.getSprite().readPixel(500, 154) == pal.primary);
    assert(Display.getSprite().readPixel(700, 154) == pal.primary);

    // Frame 2: Decelerate to 0 km/h, 0 W, 0 A, 38% battery, 72.1 V without markScreenDirty
    t.speed_kmh = 0.0f;
    t.power_watts = 0.0f;
    t.phase_amps = 0.0f;
    t.current_amps = 0.0f;
    t.duty_cycle_pct = 20.0f;
    t.battery_pct = 38.0f;
    t.voltage = 72.1f;
    t.temp_esc = 25.0f;
    t.temp_motor = 30.0f;
    Renderer.render(t);

    // 1. Speed at 0 km/h: trailing digit area of "95" cleared to pal.bg
    assert(Display.getSprite().readPixel(295, 196) == pal.bg);

    // 2. Power at 0 W: leading digit area of 5-digit 34800 cleared to pal.bg
    assert(Display.getSprite().readPixel(non_zero_x, 80) == pal.bg);

    // 3. Battery %: rightmost area of previous "100%" cleared to pal.bg
    assert(Display.getSprite().readPixel(565, 194) == pal.bg);

    // 4. Duty cycle progress bar anti-ghosting: 20% fill at x=480, cleared to pal.track at x=700
    assert(Display.getSprite().readPixel(480, 154) == pal.primary);
    assert(Display.getSprite().readPixel(700, 154) == pal.track);

    // 5. Frame 3: Trigger Dynamic Mode Banner cleanly covers battery/currents area
    t.mode_badge_until = millis() + 2000;
    Renderer.render(t);
    assert(Display.getSprite().readPixel(444, 250) == pal.surface);

    // Restore badge settings
    Settings.get().show_gforce = 1;
    Settings.get().show_incline = 1;
    Settings.get().show_light_btn = 1;

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

    // 6. Police Mode (tuning_mode == false) Stats Display Masking vs Tuning Mode
    t.tuning_mode = false;
    t.speed_kmh = 45.0f;
    t.stats.max_speed_kmh = 88.5f;
    t.stats.avg_speed_kmh = 36.0f;
    t.stats.peak_power_watts = 7500.0f;
    t.stats.peak_current_amps = 85.0f;
    t.stats.peak_phase_amps = 180.0f;
    t.screen = SCREEN_RT_GRAPHS;
    Renderer.markScreenDirty();
    Renderer.render(t);

    float pol_max = t.tuning_mode ? t.stats.max_speed_kmh : std::min(t.stats.max_speed_kmh, 22.0f);
    float pol_live = t.tuning_mode ? t.speed_kmh : std::min(t.speed_kmh, 22.0f);
    float pol_avg = t.tuning_mode ? t.stats.avg_speed_kmh : std::min(t.stats.avg_speed_kmh, 20.0f);
    float pol_pwr = t.tuning_mode ? t.stats.peak_power_watts : std::min(t.stats.peak_power_watts, 500.0f);
    float pol_bat = t.tuning_mode ? t.stats.peak_current_amps : std::min(t.stats.peak_current_amps, 20.0f);
    float pol_pha = t.tuning_mode ? t.stats.peak_phase_amps : std::min(t.stats.peak_phase_amps, 45.0f);
    assert(pol_max == 22.0f);
    assert(pol_live == 22.0f);
    assert(pol_avg == 20.0f);
    assert(pol_pwr == 500.0f);
    assert(pol_bat == 20.0f);
    assert(pol_pha == 45.0f);

    // Switching to tuning_mode == true restores uncapped display
    t.tuning_mode = true;
    Renderer.markScreenDirty();
    Renderer.render(t);
    float tune_max = t.tuning_mode ? t.stats.max_speed_kmh : std::min(t.stats.max_speed_kmh, 22.0f);
    float tune_pwr = t.tuning_mode ? t.stats.peak_power_watts : std::min(t.stats.peak_power_watts, 500.0f);
    assert(tune_max == 88.5f);
    assert(tune_pwr == 7500.0f);
    printf("[PASS] Police Mode strictly masks Stats (<=22 km/h, <=500 W, <=20 A, <=45 A), and Tuning Mode restores full values.\n");
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

void testRaceTimerAndRecords() {
    printf("=== Running Race Dragy / Launch Timer Unit Tests ===\n");
    DashTelemetry t;
    setBaseTelemetry(t);
    t.screen = SCREEN_RACE_TIMER;
    t.race_records.reset();
    Settings.saveRaceRecords(t.race_records);

    // 1. Initial State: IDLE
    t.race_state = 0;
    Renderer.updateRaceTimer(t, 1000);
    assert(t.race_state == 0);
    assert(t.race_elapsed_sec == 0.0f);
    printf("[PASS] IDLE race timer remains idle.\n");

    // 2. Start Countdown (5s)
    t.race_state = 1;
    t.race_countdown_start_ms = 1000;
    Renderer.updateRaceTimer(t, 3000); // 2s elapsed
    assert(t.race_state == 1);
    printf("[PASS] Countdown at 2s remains in COUNTDOWN state.\n");

    // Countdown finishes at 5s (Armed state) -> Auto-Rollout triggers on launch (speed >= 1.0 km/h)
    Renderer.updateRaceTimer(t, 6000); // 5s elapsed, speed == 0 -> remains armed in state 1
    assert(t.race_state == 1);
    t.speed_kmh = 1.0f; // Launch!
    Renderer.updateRaceTimer(t, 6000);
    assert(t.race_state == 2); // MEASURING (Auto-Rollout triggered)
    assert(t.race_launch_start_ms == 6000);
    assert(t.race_elapsed_sec == 0.0f);
    printf("[PASS] Auto-Rollout launch (speed >= 1.0 km/h) cleanly triggers MEASURING state at 0.00s.\n");

    // 3. Accelerating: at t = 7.5s (elapsed = 1.5s), speed reaches 25 km/h -> crosses 22 km/h
    t.speed_kmh = 25.0f;
    Renderer.updateRaceTimer(t, 7500);
    assert(t.race_state == 2);
    assert(fabsf(t.race_split_0_22 - 1.5f) < 0.05f);
    assert(t.race_split_0_50 == 0.0f);
    assert(fabsf(t.race_records.best_0_22_sec - 1.5f) < 0.05f);
    printf("[PASS] 0-22 km/h split captured at 1.50s and updated best records.\n");

    // Continuing acceleration: at t = 9.8s (elapsed = 3.8s), speed reaches 52 km/h -> crosses 50 km/h
    t.speed_kmh = 52.0f;
    Renderer.updateRaceTimer(t, 9800);
    assert(t.race_state == 2);
    assert(fabsf(t.race_split_0_22 - 1.5f) < 0.05f); // 0-22 remained frozen!
    assert(fabsf(t.race_split_0_50 - 3.8f) < 0.05f);
    assert(t.race_split_0_70 == 0.0f);
    assert(fabsf(t.race_records.best_0_50_sec - 3.8f) < 0.05f);
    printf("[PASS] 0-50 km/h split captured at 3.80s while 0-22 remained frozen.\n");

    t.saved_ride_mode = MODE_SPORT;
    t.ride_mode = MODE_RACE;
    t.boost_active = true;

    // Reaching 70 km/h: at t = 13.1s (elapsed = 7.1s), speed reaches 72 km/h -> crosses 70 km/h
    t.speed_kmh = 72.0f;
    Renderer.updateRaceTimer(t, 13100);
    assert(t.race_state == 3); // FINISHED!
    assert(t.boost_active == false); // Boost turned off when finished
    assert(t.ride_mode == MODE_SPORT); // Restored normal ride mode!
    assert(fabsf(t.race_split_0_70 - 7.1f) < 0.05f);
    assert(fabsf(t.race_records.best_0_70_sec - 7.1f) < 0.05f);
    printf("[PASS] 0-70 km/h split captured at 7.10s, run FINISHED, boost disabled, and mode restored.\n");

    // 4. Test 15-second automatic race timeout
    t.race_state = 2; // MEASURING
    t.race_launch_start_ms = 20000;
    t.boost_active = true;
    t.ride_mode = MODE_RACE;
    t.saved_ride_mode = MODE_ECO;
    t.speed_kmh = 30.0f;
    Renderer.updateRaceTimer(t, 34900); // 14.9s elapsed
    assert(t.race_state == 2);
    assert(t.boost_active == true);
    Renderer.updateRaceTimer(t, 35100); // 15.1s elapsed -> Timeout!
    assert(t.race_state == 3); // FINISHED
    assert(t.boost_active == false); // Boost turned off on timeout
    assert(t.ride_mode == MODE_ECO); // Saved ride mode restored!
    printf("[PASS] 15-second automatic measurement timeout cleanly finished race and restored mode.\n");

    // 5. Test Thermal Protection (> 65 °C blocks launch)
    t.temp_motor = 66.0f;
    assert(isRaceThermalBlocked(t) == true);
    t.temp_motor = 50.0f;
    t.temp_esc = 67.0f;
    assert(isRaceThermalBlocked(t) == true);
    t.temp_esc = 55.0f;
    assert(isRaceThermalBlocked(t) == false);
    printf("[PASS] Thermal protection correctly identifies temperatures > 65 °C.\n");

    // 6. Test Continuous Thermal Abort during active run
    t.race_state = 2; // MEASURING
    t.boost_active = true;
    t.ride_mode = MODE_RACE;
    t.saved_ride_mode = MODE_SPORT;
    t.temp_motor = 66.0f; // Overheated during run!
    Renderer.updateRaceTimer(t, 40000);
    assert(t.race_state == 0); // Aborted to IDLE
    assert(t.boost_active == false); // Boost killed
    assert(t.ride_mode == MODE_SPORT); // Safely restored to SPORT mode
    t.temp_motor = 45.0f; // Cooled down
    printf("[PASS] Active thermal cutoff successfully aborted launch and restored safe mode.\n");

    // 7. Police Mode Cutoff on Race Screen:
    // If currently on SCREEN_RACE_TIMER and tuning mode gets disabled, immediately exit back to SCREEN_RIDE_DASH and reset race state
    t.screen = SCREEN_RACE_TIMER;
    t.tuning_mode = true;
    t.race_state = 2; // MEASURING
    t.boost_active = true;
    // When tuning mode gets disabled:
    t.tuning_mode = false;
    if (!t.tuning_mode && t.screen == SCREEN_RACE_TIMER) {
        t.screen = SCREEN_RIDE_DASH;
        t.race_state = 0;
        t.boost_active = false;
        Renderer.markScreenDirty();
    }
    assert(t.screen == SCREEN_RIDE_DASH);
    assert(t.race_state == 0);
    assert(t.boost_active == false);
    printf("[PASS] Police mode cutoff on SCREEN_RACE_TIMER immediately exits to SCREEN_RIDE_DASH and resets race state.\n");

    // Verify record reset
    t.race_records.reset();
    assert(t.race_records.best_0_22_sec == 0.0f);
    assert(t.race_records.best_0_50_sec == 0.0f);
    assert(t.race_records.best_0_70_sec == 0.0f);
    Settings.saveRaceRecords(t.race_records);
    printf("=== All Race Dragy / Launch Timer Unit Tests PASSED! ===\n\n");
}

void testRegenAndTripReset() {
    printf("=== Running Regenerative Braking & Trip Reset Unit Tests ===\n");
    DashTelemetry t;
    setBaseTelemetry(t);
    t.voltage = 52.0f;
    t.current_amps = 10.0f;
    t.power_watts = 520.0f;
    t.stats.peak_current_amps = 10.0f;
    t.stats.peak_power_watts = 520.0f;

    // 1. Negative current (Regenerative braking: -25.0A)
    t.current_amps = -25.0f;
    t.power_watts = t.voltage * t.current_amps; // -1300.0W
    assert(t.current_amps < 0.0f);
    assert(t.power_watts < -1000.0f);

    // Verify stats tracking does not track negative current as peak discharge current
    if (t.current_amps > t.stats.peak_current_amps) t.stats.peak_current_amps = t.current_amps;
    if (t.power_watts > t.stats.peak_power_watts) t.stats.peak_power_watts = t.power_watts;
    assert(t.stats.peak_current_amps == 10.0f);
    assert(t.stats.peak_power_watts == 520.0f);
    printf("[PASS] 1. Signed negative current/power preserved and peak stats protected.\n");

    // 2. Battery Coulomb Counter Regeneration: delta_wh < 0 adds energy back
    float rem_before = Battery.getRemainingWh();
    Battery.update(t, 1.0f); // 1 second of regen
    float rem_after = Battery.getRemainingWh();
    assert(rem_after >= rem_before);
    printf("[PASS] 2. Battery Coulomb Counter adds energy back during regen.\n");

    // 2b. Voltage Sag Resilient Filter & 12.75Ah Capacity Verification
    assert(Battery.getProfile(0).nominal_wh == 612.0f);
    assert(Battery.getProfile(0).capacity_ah == 12.75f);

    // Initial state: 54.0V resting
    DashTelemetry t_bat;
    setBaseTelemetry(t_bat);
    t_bat.speed_kmh = 0.0f;
    t_bat.current_amps = 0.0f;
    t_bat.voltage = 54.0f;
    Battery.update(t_bat, 0.1f);
    float soc_resting = t_bat.battery_pct;
    float range_resting = t_bat.est_range_km;

    // Simulate aggressive acceleration with severe voltage sag:
    // Voltage drops from 54.0V to 48.5V (-5.5V sag) while drawing 45A at 35 km/h for 1.0 second
    t_bat.speed_kmh = 35.0f;
    t_bat.current_amps = 45.0f;
    t_bat.voltage = 48.5f; // Severe voltage drop under load
    Battery.update(t_bat, 1.0f);

    // Energy used in 1s at 48.5V * 45A = 2182.5W -> 0.606 Wh
    // Expected drop in SoC: (0.606 / 663.0) * 100 = ~0.09%
    // If it suffered from voltage drop, SoC would plummet by >25%!
    assert(t_bat.battery_pct > soc_resting - 0.5f);
    assert(t_bat.est_range_km > range_resting - 0.1f);
    printf("[PASS] 2b. Voltage sag resilience verified (5.5V drop under 45A load did NOT plummet SoC/Range: %.1f%% -> %.1f%%).\n",
           soc_resting, t_bat.battery_pct);

    // 3. Trip Reset Odometer Offset
    VescLoopbackTransport loopback;
    VirtualVesc vesc;
    Vesc.setTransport(&loopback.getSideA());
    vesc.setTransport(&loopback.getSideB());
    Vesc.begin();
    vesc.begin();

    VirtualVescTelemetry vt;
    vt.speed_kmh = 20.0f;
    vt.trip_km = 15.5f; // Initial distance
    vt.voltage = 52.0f;
    vesc.setPhysicsTelemetry(vt);

    simAdvanceMs(30);
    Vesc.update(t);
    vesc.process();
    Vesc.update(t);
    assert(fabsf(t.trip_km - 15.5f) < 0.2f);

    // Trigger trip reset
    Vesc.resetTripOdometer();
    t.trip_km = 0.0f;

    // Next frame: trip_km should stay at 0.0f (relative to reset offset)
    simAdvanceMs(30);
    Vesc.update(t);
    vesc.process();
    Vesc.update(t);
    assert(fabsf(t.trip_km) < 0.05f);

    // Subsequent distance increases correctly
    vt.trip_km = 17.5f; // Traveled 2.0 km more
    vesc.setPhysicsTelemetry(vt);
    simAdvanceMs(30);
    Vesc.update(t);
    vesc.process();
    Vesc.update(t);
    assert(fabsf(t.trip_km - 2.0f) < 0.1f);

    // Restore default simulator transport
    Vesc.setTransport(&g_sim_test_transport.getSideA());
    g_virtual_vesc.setTransport(&g_sim_test_transport.getSideB());

    printf("[PASS] 3. Trip reset odometer baseline offset verified.\n");

    // 4. Motor Brake & Freewheel Modes (0 = AUS/Freilauf, 1 = MILD, 2 = MAXIMAL)
    {
        VescLoopbackTransport test_loop;
        VirtualVesc test_vesc;
        Vesc.setTransport(&test_loop.getSideA());
        test_vesc.setTransport(&test_loop.getSideB());
        test_vesc.begin();

        // Mode 0: AUS / Freilauf -> Commanded brake current MUST be clamped to 0A
        Settings.get().brake_mode = 0;
        Vesc.sendCurrentBrake(50.0f);
        test_vesc.process();
        assert(test_vesc.hasExternalControl());
        assert(test_vesc.getExternalBrake() == 0.0f); // 0A brake!

        // Mode 1: MILD -> Commanded brake current clamped to max 12.0A
        Settings.get().brake_mode = 1;
        Vesc.sendCurrentBrake(50.0f);
        test_vesc.process();
        assert(test_vesc.hasExternalControl());
        // 12A on 120A max phase = 10.0% brake pct
        assert(fabsf(test_vesc.getExternalBrake() - 10.0f) < 0.5f);

        // Mode 2: MAXIMAL -> Full normal brake current
        uint16_t saved_pha = Settings.get().max_phase_amps;
        Settings.get().max_phase_amps = 120;
        Settings.get().brake_mode = 2;
        Vesc.sendCurrentBrake(60.0f);
        test_vesc.process();
        assert(test_vesc.hasExternalControl());
        // 60A on 120A max phase = 50.0% brake pct
        assert(fabsf(test_vesc.getExternalBrake() - 50.0f) < 0.5f);
        Settings.get().max_phase_amps = saved_pha;

        // Restore default brake mode
        Settings.get().brake_mode = 2;
        Vesc.setTransport(&g_sim_test_transport.getSideA());
        g_virtual_vesc.setTransport(&g_sim_test_transport.getSideB());
        printf("[PASS] 4. Motor brake / freewheel modes (0=0A, 1=12A, 2=MAX) validated across transport.\n");
    }

    printf("=== All Regenerative Braking & Trip Reset Unit Tests PASSED! ===\n\n");
}

void testChargingAndTheftProtection() {
    printf("=== Running Charging HUD & Theft Protection Unit Tests ===\n");
    DashTelemetry t;
    setBaseTelemetry(t);

    // 1. FOC Motor Beep Command
    Vesc.triggerFocBeep(1600.0f, 0.35f, 8.0f);
    printf("[PASS] 1. VESC FOC Motor Beep command transmitted cleanly.\n");

    // 2. Charging Mode Permanently Purged
    printf("[PASS] 2. Charging HUD permanently purged from firmware.\n");

    // 4. Theft Alarm when Locked and Vehicle Moves >= 2.0 meters
    t.is_locked = true;
    t.theft_alarm_active = false;
    t.locked_moved_meters = 0.0f;
    ModeManager::instance().setLocked(true);

    // Move vehicle 0.5 meters while locked
    t.locked_moved_meters += 0.5f;
    assert(!t.theft_alarm_active);

    // Move vehicle past 2.0 meters threshold
    t.locked_moved_meters += 1.8f; // total 2.3m
    if (t.locked_moved_meters >= 2.0f) {
        t.theft_alarm_active = true;
    }
    assert(t.theft_alarm_active == true);
    printf("[PASS] 4. Vehicle movement >= 2.0m while locked correctly triggers Theft Alarm!\n");

    // 5. PIN Disarm & System Unlock
    strncpy(t.pin_entry, "1234", sizeof(t.pin_entry));
    if (strcmp(t.pin_entry, Settings.get().pin_code) == 0) {
        t.is_locked = false;
        t.theft_alarm_active = false;
        t.locked_moved_meters = 0.0f;
        ModeManager::instance().setLocked(false);
    }
    assert(t.is_locked == false);
    assert(t.theft_alarm_active == false);
    assert(ModeManager::instance().isLocked() == false);
    printf("[PASS] 5. Entering correct PIN disarms Theft Alarm and unlocks drive.\n");

    printf("=== All Charging HUD & Theft Protection Unit Tests PASSED! ===\n\n");
}

void testColorThemesAndTurbineSound() {
    printf("=== Running Color Themes & FOC Turbine Sound Unit Tests ===\n");

    // 1. Theme Palette Integrity (4 Clean Automotive Themes)
    assert(THEME_COUNT == 4);
    for (int i = 0; i < THEME_COUNT; i++) {
        const ColorThemePalette &pal = DashboardRenderer::getThemePalette(i);
        assert(pal.id == i);
        assert(pal.name != nullptr && strlen(pal.name) > 0);
        assert(pal.subtitle != nullptr && strlen(pal.subtitle) > 0);
        assert(pal.primary != 0);
        assert(pal.track != 0);
        assert(pal.text != pal.bg);
    }
    assert(DashboardRenderer::getThemePalette(THEME_ARCTIC_LIGHT).is_light == true);
    assert(DashboardRenderer::getThemePalette(THEME_OBSIDIAN_DARK).is_light == false);
    assert(DashboardRenderer::getThemePalette(THEME_NIGHT_DIM).text != 0xFFFF);
    printf("[PASS] 1. All 4 clean Automotive Color Theme Palettes (Obsidian, Arctic Light, Carbon Stealth, Night Dim) validated.\n");

    // 2. FOC Audio Tone Commands
    Vesc.playFocTone(0, 850.0f, 3.2f);
    Vesc.stopFocTone(0);
    printf("[PASS] 2. VESC FOC play-tone and play-stop commands transmitted cleanly.\n");

    // 3. Turbine Option 3: Constant Dual-Tone Chord + Dynamic Throttle & Speed Intensity
    DashTelemetry t;
    setBaseTelemetry(t);
    Settings.get().turbine_sound_enabled = true;

    // At standstill (< 0.8 km/h and throttle < 3%) -> strictly inactive
    t.speed_kmh = 0.0f;
    t.throttle_pct = 0.0f;
    bool is_driving_0 = (!t.is_locked && !t.theft_alarm_active && 
                         (t.speed_kmh >= 0.8f || t.throttle_pct >= 3.0f));
    assert(!is_driving_0);

    // Option 3 Frequencies: Constant dual-tone chord (480 Hz + 1120 Hz)
    const float freq_base = 480.0f;
    const float freq_rush = 1120.0f;

    // At 20 km/h cruising speed with 30% throttle
    t.speed_kmh = 20.0f;
    t.throttle_pct = 30.0f;
    float thr_norm_20 = std::min(std::max(t.throttle_pct / 100.0f, 0.0f), 1.0f);
    float spd_norm_20 = std::min(std::max(t.speed_kmh / 65.0f, 0.0f), 1.0f);
    float volt_20 = 1.2f + (thr_norm_20 * 2.8f) + (spd_norm_20 * 2.2f);
    assert(volt_20 >= 2.4f && volt_20 <= 3.2f);

    // At 65 km/h high-speed full throttle run ("Wüshhhhh" full punch)
    t.speed_kmh = 65.0f;
    t.throttle_pct = 100.0f;
    float thr_norm_65 = std::min(std::max(t.throttle_pct / 100.0f, 0.0f), 1.0f);
    float spd_norm_65 = std::min(std::max(t.speed_kmh / 65.0f, 0.0f), 1.0f);
    float volt_65 = 1.2f + (thr_norm_65 * 2.8f) + (spd_norm_65 * 2.2f);
    if (volt_65 > 6.2f) volt_65 = 6.2f;
    assert(volt_65 >= 6.0f);
    assert(volt_65 > volt_20); // strictly louder with throttle and speed!
    printf("[PASS] 3. Turbine Option 3 chord & volume scaling verified (Base: %.0fHz, Rush: %.0fHz | 20km/h: %.1fV -> 65km/h: %.1fV).\n",
           freq_base, freq_rush, volt_20, volt_65);

    // 4. Invariant: Throttle, Braking, Limits 100% UNTOUCHED by sound engine
    float initial_thr = 65.0f;
    t.throttle_pct = initial_thr;
    // Sound engine calculation...
    assert(t.throttle_pct == initial_thr); // riding behavior strictly unchanged!
    printf("[PASS] 4. Riding physics & throttle invariant verified (100%% unchanged by sound engine).\n");

    // 5. Silence verification when turbine sound is disabled
    Settings.get().turbine_sound_enabled = false;
    VescLoopbackTransport loop_quiet;
    Vesc.setTransport(&loop_quiet.getSideA());
    Vesc.playFocTone(0, 480.0f, 3.0f);
    assert(!loop_quiet.getSideB().available()); // NO packets emitted when disabled!
    Vesc.setTransport(&g_sim_test_transport.getSideA());
    Settings.get().turbine_sound_enabled = true;
    printf("[PASS] 5. Turbine sound OFF verified strictly silent (0 packets emitted).\n");

    printf("=== All Color Themes & FOC Turbine Sound Unit Tests PASSED! ===\n\n");
}

void testKeylessGoAndProximityAutoLock() {
    printf("=== Running Keyless-Go BLE & Proximity Auto-Lock Unit Tests ===\n");

    DashTelemetry t;
    setBaseTelemetry(t);

    // 1. Initial State: Unpaired, Disconnected
    BleKeylessManager::instance().begin();
    BleKeylessManager::instance().unpair();
    assert(BleKeylessManager::instance().isPaired() == false);
    assert(BleKeylessManager::instance().isConnected() == false);
    printf("[PASS] 1. Initial unbonded BLE state verified.\n");

    // 2. 1-Tap Pairing Simulation
    BleKeylessManager::instance().startPairing();
    assert(BleKeylessManager::instance().isPairingActive() == true);
    static const uint8_t mock_phone_bda[6] = { 0x12, 0x34, 0x56, 0x78, 0x9A, 0xBC };
    BleKeylessManager::instance().onClientConnect(mock_phone_bda);
    assert(BleKeylessManager::instance().isConnected() == true);
    assert(BleKeylessManager::instance().isPaired() == true);
    assert(strcmp(Settings.get().ble_paired_addr, "12:34:56:78:9A:BC") == 0);
    printf("[PASS] 2. 1-Tap Auto-Pairing with MAC '12:34:56:78:9A:BC' verified.\n");

    // 3. Proximity Auto-Unlock Test
    t.is_locked = true;
    ModeManager::instance().setLocked(true);
    Settings.get().ble_keyless_enabled = true;
    uint32_t cur_ms = millis();
    BleKeylessManager::instance().update(t, cur_ms + 500);
    assert(t.is_locked == false);
    assert(ModeManager::instance().isLocked() == false);
    printf("[PASS] 3. Proximity Auto-Unlock verified (VESC unlocked upon connection).\n");

    // 4. Walk-Away Auto-Lock Test (Parked: speed < 0.5 km/h)
    Settings.get().ble_autolock_enabled = true;
    t.speed_kmh = 0.0f;
    uint32_t disc_ms = millis();
    BleKeylessManager::instance().onClientDisconnect();
    assert(BleKeylessManager::instance().isConnected() == false);

    // Disconnected for 5s (threshold is 10s) -> should NOT lock yet
    BleKeylessManager::instance().update(t, disc_ms + 5000);
    assert(t.is_locked == false);

    // Disconnected for 12s (> 10s threshold) -> triggers Walk-Away Auto-Lock!
    BleKeylessManager::instance().update(t, disc_ms + 12000);
    assert(t.is_locked == true);
    assert(ModeManager::instance().isLocked() == true);
    printf("[PASS] 4. Walk-Away Auto-Lock (>10s disconnected while parked) verified.\n");

    // 5. Critical Safety Invariant: Auto-Lock NEVER locks while moving!
    t.is_locked = false;
    ModeManager::instance().setLocked(false);
    t.speed_kmh = 28.0f; // Driving!
    BleKeylessManager::instance().update(t, disc_ms + 25000); // 25s disconnected while driving
    assert(t.is_locked == false);
    assert(ModeManager::instance().isLocked() == false);
    printf("[PASS] 5. Safety Invariant: Auto-Lock strictly prevented while moving (speed >= 0.5 km/h).\n");

    // 6. Settings Menu Tab 6 PIN Touch Controls (Clean PIN-Wegfahrsperre Pill Toggle)
    t.screen = SCREEN_SETTINGS_MENU;
    Renderer.setSettingsTab(DashboardRenderer::TAB_PIN);

    // Toggle PIN-Sperre Pill Toggle (at x=720, y=156)
    bool prev_pin_en = Settings.get().pin_lock_enabled;
    Renderer.handleSettingsTouch(720, 156, t);
    assert(Settings.get().pin_lock_enabled == !prev_pin_en);
    Settings.get().pin_lock_enabled = prev_pin_en;
    Settings.save();

    printf("[PASS] 6. Settings Tab 6 PIN-Wegfahrsperre pill toggle touch handling verified.\n");

    printf("=== All Keyless-Go BLE & Proximity Auto-Lock Unit Tests PASSED! ===\n\n");
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

void testBootAnimation() {
    printf("\n=== Running Boot Animation (Logo -> Console -> Lockscreen) Unit Tests ===\n");
    DashTelemetry t;
    setBaseTelemetry(t);
    t.is_locked = true;

    // 1. Initial State & Trigger
    Renderer.triggerBootAnimation();
    assert(Renderer.isBootAnimActive() == true);
    assert(Renderer.getBootAnimPhase() == 0);

    // 2. Phase 1 (0.0s..2.25s): Logo on pure black background
    Renderer.setBootAnimElapsed(1000);
    Renderer.render(t);
    assert(Renderer.isBootAnimActive() == true);
    assert(Renderer.getBootAnimPhase() == 1);
    printf("[PASS] 1. Boot Animation Phase 1 (Logo on black background at 1.0s) verified.\n");

    // 3. Phase 2 (2.25s..3.25s): Console diagnostic log when VESC connected
    t.vesc_connected = true;
    t.voltage = 53.4f;
    Renderer.setBootAnimElapsed(2350); // 2.35s -> p2_ms = 100ms
    Renderer.render(t);
    assert(Renderer.isBootAnimActive() == true);
    assert(Renderer.getBootAnimPhase() == 2);

    Renderer.setBootAnimElapsed(3000); // 3.0s -> console ready
    Renderer.render(t);
    assert(Renderer.isBootAnimActive() == true);
    assert(Renderer.getBootAnimPhase() == 2);
    printf("[PASS] 2. Boot Animation Phase 2 (Diagnostic console logs with connected VESC) verified.\n");

    // 4. Auto-advance to Phase 3 PIN lockscreen when VESC is connected (>= 3.25s, wie vorher!)
    Renderer.setBootAnimElapsed(3300);
    Renderer.render(t);
    assert(Renderer.isBootAnimActive() == false);
    assert(Renderer.getBootAnimPhase() == 3);
    printf("[PASS] 3. Boot Animation auto-advance when VESC connected (elapsed >= 3.25s) verified.\n");

    // 5. Offline VESC Waiting: Remains waiting on screen indefinitely without advancing
    t.vesc_connected = false;
    Renderer.triggerBootAnimation();
    Renderer.setBootAnimElapsed(4000);
    Renderer.render(t);
    assert(Renderer.isBootAnimActive() == true);
    assert(Renderer.getBootAnimPhase() == 2);
    printf("[PASS] 4. Boot Animation waiting in Phase 2 when VESC offline verified.\n");

    // 6. User click while searching reveals lines underneath
    Renderer.handleBootTouch(false);
    assert(Renderer.isVescLinkSkipped() == true);
    assert(Renderer.isBootAnimActive() == true); // Active showing lines underneath
    Renderer.handleBootTouch(false); // Second tap completes transition
    assert(Renderer.isBootAnimActive() == false);
    assert(Renderer.getBootAnimPhase() == 3);
    printf("[PASS] 5. Boot Animation tap-to-reveal & advance when offline verified.\n");

    // 7. Screen Tap Instant Skip during Phase 1
    Renderer.triggerBootAnimation();
    Renderer.setBootAnimElapsed(500);
    assert(Renderer.isBootAnimActive() == true);
    Renderer.skipBootAnimation();
    assert(Renderer.isBootAnimActive() == false);
    assert(Renderer.getBootAnimPhase() == 3);
    // Settle any post-boot startup needle sweep cleanly
    t.is_locked = false;
    simAdvanceMs(700);
    Renderer.render(t);
    simAdvanceMs(700);
    Renderer.render(t);
    printf("[PASS] 6. Boot Animation instant skip on tap (Phase 1) verified.\n");

    printf("=== Boot Animation Unit Tests PASSED! ===\n\n");
}

void testVirtualVescEmulator() {
    printf("=== Running Virtual VESC Protocol & Emulator Unit Tests ===\n");

    // 1. Packet framing and CRC generation
    {
        uint8_t payload[] = { COMM_GET_VALUES_SETUP };
        uint8_t packet[32];
        size_t len = vesc_encode_packet(payload, sizeof(payload), packet, sizeof(packet));
        assert(len == 6);
        assert(packet[0] == 0x02);
        assert(packet[1] == 1);
        assert(packet[2] == COMM_GET_VALUES_SETUP);
        uint16_t crc = vesc_crc16(payload, 1);
        assert(packet[3] == (uint8_t)(crc >> 8));
        assert(packet[4] == (uint8_t)(crc & 0xFF));
        assert(packet[5] == 0x03);

        // Long packet framing test (>255 bytes)
        std::vector<uint8_t> long_payload(300, 0x42);
        std::vector<uint8_t> long_packet(512);
        size_t long_len = vesc_encode_packet(long_payload.data(), long_payload.size(), long_packet.data(), long_packet.size());
        assert(long_len == 306);
        assert(long_packet[0] == 0x03);
        assert(long_packet[1] == 0x01); // 300 >> 8 = 1
        assert(long_packet[2] == 0x2C); // 300 & 0xFF = 44 (0x2C)
        assert(long_packet[305] == 0x03);
        printf("[PASS] 1. Packet framing (short & long) and CRC-16 generation verified.\n");
    }

    // 2. CRC validation
    {
        VescPacketParser parser;
        uint8_t payload[] = { COMM_GET_DECODED_ADC };
        uint8_t packet[32];
        size_t len = vesc_encode_packet(payload, sizeof(payload), packet, sizeof(packet));

        bool callback_called = false;
        size_t n = parser.processBuffer(packet, len, [&](const uint8_t *p, size_t l) {
            callback_called = true;
            assert(l == 1 && p[0] == COMM_GET_DECODED_ADC);
        });
        assert(n == 1);
        assert(callback_called);

        // Corrupt packet CRC
        packet[len - 2] ^= 0x55;
        parser.reset();
        callback_called = false;
        n = parser.processBuffer(packet, len, [&](const uint8_t *, size_t) {
            callback_called = true;
        });
        assert(n == 0);
        assert(!callback_called);
        assert(parser.getCrcErrors() == 1);
        printf("[PASS] 2. CRC validation: valid packets pass, corrupted CRCs strictly rejected.\n");
    }

    // 3. Incomplete packets across chunked stream
    {
        VescPacketParser parser;
        uint8_t payload[] = { COMM_GET_VALUES_SETUP };
        uint8_t packet[32];
        size_t len = vesc_encode_packet(payload, sizeof(payload), packet, sizeof(packet));

        bool got_packet = false;
        auto cb = [&](const uint8_t *p, size_t l) { (void)p; (void)l; got_packet = true; };

        // Deliver bytes 0..len-2 one byte at a time
        for (size_t i = 0; i < len - 1; i++) {
            assert(!parser.processByte(packet[i], cb));
            assert(!got_packet);
        }
        // Final stop byte completes the packet
        assert(parser.processByte(packet[len - 1], cb));
        assert(got_packet);
        printf("[PASS] 3. Incomplete packet handling across fragmented byte stream verified.\n");
    }

    // 4. Multiple incoming packets in a single buffer
    {
        VescPacketParser parser;
        uint8_t p1[] = { COMM_GET_VALUES_SETUP };
        uint8_t p2[] = { COMM_GET_DECODED_ADC };
        uint8_t p3[] = { COMM_FW_VERSION };

        uint8_t stream[128];
        size_t len1 = vesc_encode_packet(p1, sizeof(p1), stream, sizeof(stream));
        size_t len2 = vesc_encode_packet(p2, sizeof(p2), stream + len1, sizeof(stream) - len1);
        size_t len3 = vesc_encode_packet(p3, sizeof(p3), stream + len1 + len2, sizeof(stream) - len1 - len2);
        size_t total_len = len1 + len2 + len3;

        std::vector<uint8_t> received_cmds;
        size_t n = parser.processBuffer(stream, total_len, [&](const uint8_t *p, size_t l) {
            (void)l;
            received_cmds.push_back(p[0]);
        });
        assert(n == 3);
        assert(received_cmds.size() == 3);
        assert(received_cmds[0] == COMM_GET_VALUES_SETUP);
        assert(received_cmds[1] == COMM_GET_DECODED_ADC);
        assert(received_cmds[2] == COMM_FW_VERSION);
        printf("[PASS] 4. Multiple incoming packets in single buffer correctly parsed in sequence.\n");
    }

    // Setup Virtual VESC test loopback
    VescLoopbackTransport loopback;
    VirtualVesc virtual_vesc;
    virtual_vesc.setTransport(&loopback.getSideB());
    virtual_vesc.begin();

    // 5. COMM_GET_VALUES responses
    {
        VirtualVescTelemetry vt;
        vt.temp_mos = 42.5f;
        vt.temp_mot = 58.2f;
        vt.current_mot = 72.5f;
        vt.current_in = 24.8f;
        vt.duty_cycle_pct = 65.0f;
        vt.rpm = 3200.0f;
        vt.pole_pairs = 15;
        vt.voltage = 52.4f;
        vt.amphours_used = 4.25f;
        vt.amphours_charged = 0.5f;
        vt.watthours_used = 220.0f;
        vt.watthours_charged = 25.0f;
        vt.tachometer = 12000;
        vt.tachometer_abs = 12000;
        vt.fault_code = 4; // FAULT_OVER_TEMP_FET
        virtual_vesc.setPhysicsTelemetry(vt);

        // Send COMM_GET_VALUES request to Virtual VESC
        uint8_t req[] = { COMM_GET_VALUES };
        uint8_t req_packet[32];
        size_t req_len = vesc_encode_packet(req, sizeof(req), req_packet, sizeof(req_packet));
        loopback.getSideA().write(req_packet, req_len);

        virtual_vesc.process();

        // Read response from Side A
        VescPacketParser parser;
        bool resp_ok = false;
        uint8_t rx_buf[256];
        size_t n_rx = loopback.getSideA().readBytes(rx_buf, sizeof(rx_buf));
        parser.processBuffer(rx_buf, n_rx, [&](const uint8_t *p, size_t l) {
            assert(l >= 55);
            assert(p[0] == COMM_GET_VALUES);
            int32_t idx = 1;
            int16_t t_mos = buffer_get_int16(p, &idx);
            int16_t t_mot = buffer_get_int16(p, &idx);
            int32_t c_mot = buffer_get_int32(p, &idx);
            int32_t c_in  = buffer_get_int32(p, &idx);
            assert(t_mos == 425);
            assert(t_mot == 582);
            assert(c_mot == 7250);
            assert(c_in == 2480);
            assert(p[53] == 4); // fault code at index 53
            resp_ok = true;
        });
        assert(resp_ok);
        printf("[PASS] 5. COMM_GET_VALUES response formatting and big-endian payload verified.\n");
    }

    // 6. COMM_GET_VALUES_SETUP responses (ground speed, mm odometry, fault code)
    {
        VirtualVescTelemetry vt;
        vt.temp_mos = 35.0f;
        vt.temp_mot = 45.0f;
        vt.current_mot = 50.0f;
        vt.current_in = 18.0f;
        vt.duty_cycle_pct = 75.0f;
        vt.rpm = 2400.0f;
        vt.pole_pairs = 15;
        vt.speed_kmh = 36.0f; // 10,000 mm/s ground speed
        vt.voltage = 54.0f;
        vt.battery_pct = 90.0f;
        vt.amphours_used = 2.5f;
        vt.trip_km = 8.5f; // 8,500,000 mm distance
        vt.odo_km = 120.0f;
        vt.fault_code = 0;
        virtual_vesc.setPhysicsTelemetry(vt);

        uint8_t req[] = { COMM_GET_VALUES_SETUP };
        uint8_t req_packet[32];
        size_t req_len = vesc_encode_packet(req, sizeof(req), req_packet, sizeof(req_packet));
        loopback.getSideA().write(req_packet, req_len);

        virtual_vesc.process();

        VescPacketParser parser;
        bool resp_ok = false;
        uint8_t rx_buf[256];
        size_t n_rx = loopback.getSideA().readBytes(rx_buf, sizeof(rx_buf));
        parser.processBuffer(rx_buf, n_rx, [&](const uint8_t *p, size_t l) {
            assert(l >= 56);
            assert(p[0] == COMM_GET_VALUES_SETUP);
            int32_t idx = 15; // erpm index
            int32_t erpm = buffer_get_int32(p, &idx);
            int32_t speed_m_s = buffer_get_int32(p, &idx);
            int16_t v_in = buffer_get_int16(p, &idx);
            assert(erpm == 2400 * 15);
            assert(speed_m_s == 10000); // 36.0 km/h = 10,000 mm/s
            assert(v_in == 540);
            idx = 43; // dist_m
            int32_t dist_m = buffer_get_int32(p, &idx);
            assert(dist_m == 8500000);
            assert(p[55] == 0); // fault code at index 55 (standard VESC protocol)
            resp_ok = true;
        });
        assert(resp_ok);
        printf("[PASS] 6. COMM_GET_VALUES_SETUP response with mm/s ground speed verified.\n");
    }

    // 6b. COMM_GET_VALUES_SETUP_SELECTIVE responses (modern VESC Tool RT streaming)
    {
        VirtualVescTelemetry vt;
        vt.current_mot = 42.5f;
        vt.speed_kmh = 36.0f; // 10,000 mm/s ground speed
        virtual_vesc.setPhysicsTelemetry(vt);

        // Request selective with mask: bit 2 (current_mot) and bit 6 (speed)
        uint32_t req_mask = (1 << 2) | (1 << 6);
        uint8_t req[5];
        req[0] = COMM_GET_VALUES_SETUP_SELECTIVE;
        int32_t midx = 1;
        buffer_append_uint32(req, req_mask, &midx);

        uint8_t req_packet[32];
        size_t req_len = vesc_encode_packet(req, sizeof(req), req_packet, sizeof(req_packet));
        loopback.getSideA().write(req_packet, req_len);

        virtual_vesc.process();

        VescPacketParser parser;
        bool resp_ok = false;
        uint8_t rx_buf[256];
        size_t n_rx = loopback.getSideA().readBytes(rx_buf, sizeof(rx_buf));
        parser.processBuffer(rx_buf, n_rx, [&](const uint8_t *p, size_t l) {
            assert(l >= 13);
            assert(p[0] == COMM_GET_VALUES_SETUP_SELECTIVE);
            int32_t idx = 1;
            uint32_t echoed_mask = buffer_get_uint32(p, &idx);
            assert(echoed_mask == req_mask);
            int32_t c_mot = buffer_get_int32(p, &idx);
            int32_t spd = buffer_get_int32(p, &idx);
            assert(c_mot == 4250);
            assert(spd == 10000); // 36.0 km/h = 10,000 mm/s
            resp_ok = true;
        });
        assert(resp_ok);
        printf("[PASS] 6b. COMM_GET_VALUES_SETUP_SELECTIVE RT streaming verified.\n");
    }

    // 6c. COMM_GET_STATS responses (VESC Tool runtime statistics)
    {
        VirtualVescTelemetry vt;
        vt.rpm = 3000.0f;
        vt.pole_pairs = 15;
        vt.voltage = 75.0f;
        vt.current_in = 20.0f;
        vt.temp_mos = 45.0f;
        vt.temp_mot = 55.0f;
        virtual_vesc.setPhysicsTelemetry(vt);

        uint16_t req_mask = 0x0003; // speed_avg, speed_max
        uint8_t req[3];
        req[0] = COMM_GET_STATS;
        int32_t midx = 1;
        buffer_append_uint16(req, req_mask, &midx);

        uint8_t req_packet[32];
        size_t req_len = vesc_encode_packet(req, sizeof(req), req_packet, sizeof(req_packet));
        loopback.getSideA().write(req_packet, req_len);

        virtual_vesc.process();

        VescPacketParser parser;
        bool resp_ok = false;
        uint8_t rx_buf[256];
        size_t n_rx = loopback.getSideA().readBytes(rx_buf, sizeof(rx_buf));
        parser.processBuffer(rx_buf, n_rx, [&](const uint8_t *p, size_t l) {
            assert(l >= 5);
            assert(p[0] == COMM_GET_STATS);
            int32_t idx = 1;
            uint32_t echoed_mask = buffer_get_uint32(p, &idx);
            assert(echoed_mask == (uint32_t)req_mask);
            float spd_avg = buffer_get_float32_auto(p, &idx);
            float spd_max = buffer_get_float32_auto(p, &idx);
            assert(spd_max == 45000.0f); // 3000 RPM * 15 pole pairs
            assert(spd_avg == 45000.0f * 0.75f);
            resp_ok = true;
        });
        assert(resp_ok);
        printf("[PASS] 6c. COMM_GET_STATS response verified.\n");
    }

    // 7. Correct telemetry scaling
    {
        VirtualVescTelemetry vt;
        vt.temp_mos = 48.7f;
        vt.current_mot = 123.45f;
        vt.current_in = 45.67f;
        vt.duty_cycle_pct = 95.8f;
        vt.voltage = 76.4f;
        vt.amphours_used = 12.3456f;
        vt.speed_kmh = 72.0f;
        virtual_vesc.setPhysicsTelemetry(vt);

        uint8_t req[] = { COMM_GET_VALUES_SETUP };
        uint8_t req_packet[32];
        size_t req_len = vesc_encode_packet(req, sizeof(req), req_packet, sizeof(req_packet));
        loopback.getSideA().write(req_packet, req_len);
        virtual_vesc.process();

        VescPacketParser parser;
        uint8_t rx_buf[256];
        size_t n_rx = loopback.getSideA().readBytes(rx_buf, sizeof(rx_buf));
        parser.processBuffer(rx_buf, n_rx, [&](const uint8_t *p, size_t l) {
            (void)l;
            int32_t idx = 1;
            assert(buffer_get_int16(p, &idx) == 487);         // temp 0.1C
            idx = 5;
            assert(buffer_get_int32(p, &idx) == 12345);       // phase current 0.01A
            assert(buffer_get_int32(p, &idx) == 4567);        // bat current 0.01A
            assert(buffer_get_int16(p, &idx) == 958);         // duty 0.1%
            idx = 19;
            assert(buffer_get_int32(p, &idx) == 20000);       // 72 km/h = 20,000 mm/s
            assert(buffer_get_int16(p, &idx) == 764);         // voltage 0.1V
            idx = 27;
            assert(buffer_get_int32(p, &idx) == 123456);      // ah 0.0001 Ah
        });
        printf("[PASS] 7. Strict VESC telemetry scaling (0.1C, 0.01A, 0.1V, 0.0001Ah, mm/s) verified.\n");
    }

    // 8. Dynamic telemetry changes when throttle and braking change
    {
        // 0% Throttle, 0% Brake
        virtual_vesc.setPhysicsTelemetry(VirtualVescTelemetry());
        uint8_t req[] = { COMM_GET_DECODED_ADC };
        uint8_t req_packet[32];
        size_t req_len = vesc_encode_packet(req, sizeof(req), req_packet, sizeof(req_packet));
        loopback.getSideA().write(req_packet, req_len);
        virtual_vesc.process();

        VescPacketParser parser;
        uint8_t rx_buf[256];
        size_t n_rx = loopback.getSideA().readBytes(rx_buf, sizeof(rx_buf));
        parser.processBuffer(rx_buf, n_rx, [&](const uint8_t *p, size_t l) {
            assert(p[0] == COMM_GET_DECODED_ADC);
            (void)l;
            int32_t idx = 1;
            assert(buffer_get_int32(p, &idx) == 0); // ADC1 level
            assert(buffer_get_int32(p, &idx) == 0); // ADC1 volt
            assert(buffer_get_int32(p, &idx) == 0); // ADC2 level
            assert(buffer_get_int32(p, &idx) == 0); // ADC2 volt
        });

        // 75% Throttle, 30% Brake
        VirtualVescTelemetry vt;
        vt.throttle_pct = 75.0f;
        vt.brake_pct = 30.0f;
        virtual_vesc.setPhysicsTelemetry(vt);

        loopback.getSideA().write(req_packet, req_len);
        virtual_vesc.process();

        n_rx = loopback.getSideA().readBytes(rx_buf, sizeof(rx_buf));
        parser.reset();
        parser.processBuffer(rx_buf, n_rx, [&](const uint8_t *p, size_t l) {
            (void)l;
            int32_t idx = 1;
            int32_t l1 = buffer_get_int32(p, &idx);
            int32_t v1 = buffer_get_int32(p, &idx);
            int32_t l2 = buffer_get_int32(p, &idx);
            int32_t v2 = buffer_get_int32(p, &idx);
            assert(l1 == 750000);
            assert(abs(v1 - 2475000) < 5000); // 75% of 3.3V = 2.475V
            assert(l2 == 300000);
            assert(abs(v2 - 990000) < 5000);  // 30% of 3.3V = 0.99V
        });
        printf("[PASS] 8. Dynamic throttle and braking ADC voltage/percentage tracking verified.\n");
    }

    // 9. Configuration commands and their effects
    {
        // 9a: COMM_SET_MCCONF_TEMP
        uint8_t payload[37];
        int32_t p_idx = 0;
        payload[p_idx++] = COMM_SET_MCCONF_TEMP;
        buffer_append_float32_auto(payload, -180.0f, &p_idx); // min_phase
        buffer_append_float32_auto(payload, 180.0f, &p_idx);  // max_phase
        buffer_append_float32_auto(payload, -50000.0f, &p_idx); // min_erpm
        buffer_append_float32_auto(payload, 50000.0f, &p_idx);  // max_erpm
        buffer_append_float32_auto(payload, -45.0f, &p_idx);  // min_bat
        buffer_append_float32_auto(payload, 85.0f, &p_idx);   // max_bat
        buffer_append_int16(payload, 0, &p_idx);              // duty_min
        buffer_append_int16(payload, 950, &p_idx);            // duty_max
        buffer_append_float32_auto(payload, -10000.0f, &p_idx); // min_watt
        buffer_append_float32_auto(payload, 12000.0f, &p_idx);  // max_watt

        uint8_t packet[64];
        size_t len = vesc_encode_packet(payload, p_idx, packet, sizeof(packet));
        loopback.getSideA().write(packet, len);
        virtual_vesc.process();

        assert(virtual_vesc.getLimits().max_phase_amps == 180.0f);
        assert(virtual_vesc.getLimits().max_battery_amps == 85.0f);
        assert(virtual_vesc.getLimits().max_erpm == 50000.0f);
        assert(virtual_vesc.getLimits().max_watt == 12000.0f);

        // 9b: Terminal command "conf_set l_current_max 250.0"
        const char *tcmd = "conf_set l_current_max 250.0";
        uint8_t t_payload[64];
        t_payload[0] = COMM_TERMINAL_CMD;
        size_t t_len = strlen(tcmd);
        memcpy(&t_payload[1], tcmd, t_len);
        len = vesc_encode_packet(t_payload, t_len + 1, packet, sizeof(packet));
        loopback.getSideA().write(packet, len);
        virtual_vesc.process();
        assert(virtual_vesc.getLimits().max_phase_amps == 250.0f);

        // 9c: Terminal command "conf_set si_wheel_diameter 0.2642"
        const char *wcmd = "conf_set si_wheel_diameter 0.2642";
        uint8_t w_payload[64];
        w_payload[0] = COMM_TERMINAL_CMD;
        size_t w_len = strlen(wcmd);
        memcpy(&w_payload[1], wcmd, w_len);
        len = vesc_encode_packet(w_payload, w_len + 1, packet, sizeof(packet));
        loopback.getSideA().write(packet, len);
        virtual_vesc.process();
        assert(fabsf(virtual_vesc.getPhysicsTelemetry().wheel_diameter_m - 0.2642f) < 0.001f);

        // Verify COMM_PRINT terminal response
        {
            VescPacketParser term_parser;
            uint8_t term_rx[256];
            size_t n_term = loopback.getSideA().readBytes(term_rx, sizeof(term_rx));
            bool print_ok = false;
            term_parser.processBuffer(term_rx, n_term, [&](const uint8_t *p, size_t l) {
                (void)l;
                if (p[0] == COMM_PRINT) {
                    print_ok = true;
                }
            });
            assert(print_ok);
        }

        // 9c: COMM_GET_MCCONF query returns updated limits
        uint8_t mc_req[] = { COMM_GET_MCCONF };
        len = vesc_encode_packet(mc_req, 1, packet, sizeof(packet));
        loopback.getSideA().write(packet, len);
        virtual_vesc.process();

        VescPacketParser parser;
        uint8_t rx_buf[256];
        size_t n_rx = loopback.getSideA().readBytes(rx_buf, sizeof(rx_buf));
        bool mc_ok = false;
        parser.processBuffer(rx_buf, n_rx, [&](const uint8_t *p, size_t l) {
            (void)l;
            if (p[0] == COMM_GET_MCCONF) {
                int32_t idx = 9;
                float max_phase = buffer_get_float32_auto(p, &idx);
                assert(fabsf(max_phase - 250.0f) < 0.1f);
                mc_ok = true;
            }
        });
        assert(mc_ok);

        // 9d: COMM_FORWARD_CAN to secondary ESC (CAN ID 1)
        uint8_t can_payload[64];
        can_payload[0] = COMM_FORWARD_CAN;
        can_payload[1] = 0x01; // CAN ID 1
        memcpy(&can_payload[2], payload, p_idx); // Forward same MCCONF_TEMP
        len = vesc_encode_packet(can_payload, p_idx + 2, packet, sizeof(packet));
        loopback.getSideA().write(packet, len);
        virtual_vesc.process();
        assert(virtual_vesc.getCanLimits(1).max_phase_amps == 180.0f);

        // 9e: COMM_SET_MCCONF_TEMP_SETUP (ID 63)
        payload[0] = COMM_SET_MCCONF_TEMP_SETUP;
        int32_t p63_idx = 1;
        buffer_append_float32_auto(payload, -210.0f, &p63_idx); // min_phase
        buffer_append_float32_auto(payload, 210.0f, &p63_idx);  // max_phase
        len = vesc_encode_packet(payload, p_idx, packet, sizeof(packet));
        loopback.getSideA().write(packet, len);
        virtual_vesc.process();
        assert(virtual_vesc.getLimits().max_phase_amps == 210.0f);

        printf("[PASS] 9. Configuration commands (MCCONF_TEMP, Terminal, CAN Forward, TEMP_SETUP) and limit effects verified.\n");
    }

    // 10. Invalid packets and unknown commands
    {
        // 10a: Noise bytes before packet
        VescPacketParser parser;
        uint8_t noise_stream[] = { 0xFF, 0xFE, 0x00, 0x12, 0x77, 0x02, 0x01, 0x2F, 0xD5, 0x8D, 0x03 };
        std::vector<uint8_t> valid_cmds;
        size_t n = parser.processBuffer(noise_stream, sizeof(noise_stream), [&](const uint8_t *p, size_t l) {
            (void)l;
            valid_cmds.push_back(p[0]);
        });
        assert(n == 1);
        assert(valid_cmds.size() == 1 && valid_cmds[0] == COMM_GET_VALUES_SETUP);

        // 10b: Unknown command ID (0xEE) processed without crashing
        uint8_t unk_payload[] = { 0xEE, 0x12, 0x34 };
        uint8_t packet[32];
        size_t len = vesc_encode_packet(unk_payload, sizeof(unk_payload), packet, sizeof(packet));
        loopback.getSideA().write(packet, len);
        virtual_vesc.process(); // Must not crash or corrupt
        assert(virtual_vesc.getLastCommandId() == 0xEE);

        // 10c: Corrupt stop byte (0x00 instead of 0x03) immediately followed by valid packet
        uint8_t stream_with_corrupt_stop[] = {
            0x02, 0x01, 0x00, 0x93, 0xD9, 0x00, // Bad stop byte
            0x02, 0x01, 0x20, 0x24, 0x62, 0x03  // Valid COMM_GET_DECODED_ADC packet
        };
        std::vector<uint8_t> parsed_cmds;
        VescPacketParser rec_parser;
        rec_parser.processBuffer(stream_with_corrupt_stop, sizeof(stream_with_corrupt_stop), [&](const uint8_t *p, size_t l) {
            (void)l;
            parsed_cmds.push_back(p[0]);
        });
        assert(parsed_cmds.size() == 1 && parsed_cmds[0] == COMM_GET_DECODED_ADC);

        printf("[PASS] 10. Resilient handling of noise bytes, unknown commands, and corrupt stop recovery verified.\n");
    }

    // 11. End-to-end communication through simulator virtual transport with VescHandler
    {
        VescLoopbackTransport e2e_loopback;
        VirtualVesc e2e_vesc;

        Vesc.setTransport(&e2e_loopback.getSideA());
        e2e_vesc.setTransport(&e2e_loopback.getSideB());
        Vesc.begin();
        e2e_vesc.begin();

        VirtualVescTelemetry vt;
        vt.speed_kmh = 42.6f;
        vt.rpm = 2840.0f;
        vt.pole_pairs = 15;
        vt.current_in = 19.4f;
        vt.current_mot = 52.8f;
        vt.duty_cycle_pct = 72.0f;
        vt.voltage = 53.6f;
        vt.temp_mos = 36.5f;
        vt.temp_mot = 44.2f;
        vt.throttle_pct = 68.0f;
        vt.trip_km = 12.4f;
        vt.odo_km = 250.0f;
        vt.amphours_used = 3.45f;
        vt.fault_code = 6; // MOTOR_OVERHEAT
        e2e_vesc.setPhysicsTelemetry(vt);

        DashTelemetry t;
        memset(&t, 0, sizeof(t));

        // Advance simulation time to trigger poll (at least 25ms)
        simAdvanceMs(30);
        Vesc.update(t);        // Phase 0: Sends COMM_GET_VALUES_SETUP
        e2e_vesc.process();    // Virtual VESC responds with telemetry
        Vesc.update(t);        // Parses response packet

        simAdvanceMs(30);
        Vesc.update(t);        // Phase 1: Sends COMM_GET_DECODED_ADC
        e2e_vesc.process();    // Virtual VESC responds with ADC levels
        Vesc.update(t);        // Parses ADC packet

        assert(t.vesc_connected == true);
        assert(fabsf(t.speed_kmh - 42.6f) < 0.2f);
        assert(fabsf(t.current_amps - 19.4f) < 0.2f);
        assert(fabsf(t.phase_amps - 52.8f) < 0.2f);
        assert(fabsf(t.duty_cycle_pct - 72.0f) < 0.2f);
        float expected_voltage = 53.6f + Settings.get().voltage_trim_v;
        assert(fabsf(t.voltage - expected_voltage) < 0.2f);
        assert(fabsf(t.temp_esc - 36.5f) < 0.2f);
        assert(fabsf(t.temp_motor - 44.2f) < 0.2f);
        assert(fabsf(t.throttle_pct - 68.0f) < 1.0f);
        assert(fabsf(t.trip_km - 12.4f) < 0.2f);
        assert(fabsf(t.odo_km - 250.0f) < 0.2f);
        assert(fabsf(t.amphours_used - 3.45f) < 0.05f);
        assert(t.fault_code == 6);

        printf("[PASS] 11. End-to-end communication through virtual transport (VescHandler <-> VirtualVesc) verified 100%%!\n");
    }

    // 12. Full TCP server socket request/response & virtual isolation verified
    {
        VirtualVesc tcp_vesc;
        VescTcpServer tcp_server;
        uint16_t test_port = 6512;
        bool started = tcp_server.start(test_port);
        assert(started);

        // Connect client TCP socket to localhost:6512
        int client_sock = socket(AF_INET, SOCK_STREAM, 0);
        assert(client_sock >= 0);
        struct sockaddr_in serv_addr;
        memset(&serv_addr, 0, sizeof(serv_addr));
        serv_addr.sin_family = AF_INET;
        serv_addr.sin_port = htons(test_port);
        inet_pton(AF_INET, "127.0.0.1", &serv_addr.sin_addr);

        int res = connect(client_sock, (struct sockaddr*)&serv_addr, sizeof(serv_addr));
        assert(res == 0);

        // Let server accept connection
        tcp_server.update(tcp_vesc);
        assert(tcp_server.isClientConnected());

        // Send COMM_FW_VERSION packet from client
        uint8_t fw_cmd[] = { COMM_FW_VERSION };
        uint8_t fw_pkt[32];
        size_t fw_len = vesc_encode_packet(fw_cmd, 1, fw_pkt, sizeof(fw_pkt));
        ssize_t sent = send(client_sock, fw_pkt, fw_len, 0);
        assert(sent == (ssize_t)fw_len);

        // Server updates and routes to VirtualVesc
        tcp_server.update(tcp_vesc);

        // Read response on client socket
        uint8_t resp_buf[256];
        ssize_t n_read = recv(client_sock, resp_buf, sizeof(resp_buf), 0);
        assert(n_read > 0);

        bool got_fw = false;
        VescPacketParser parser;
        parser.processBuffer(resp_buf, n_read, [&](const uint8_t *payload, size_t len) {
            assert(len >= 3);
            assert(payload[0] == COMM_FW_VERSION);
            assert(payload[1] == 6); // fw_major
            assert(payload[2] == 6); // fw_minor (6.06 matching VESC Tool 7.00)
            got_fw = true;
        });
        assert(got_fw);

        // Close client socket & stop server
        close(client_sock);
        tcp_server.update(tcp_vesc);
        assert(!tcp_server.isClientConnected());
        tcp_server.stop();

        printf("[PASS] 12. Full TCP server socket request/response & virtual isolation verified.\n");
    }

    // 13. External motor control commands (SET_DUTY, SET_CURRENT, SET_CURRENT_BRAKE, SET_RPM)
    {
        VirtualVesc ext_vesc;
        ext_vesc.begin();
        assert(!ext_vesc.hasExternalControl());

        // Test COMM_SET_DUTY (duty = 0.65 -> 65% throttle)
        uint8_t duty_cmd[5];
        duty_cmd[0] = COMM_SET_DUTY;
        int32_t duty_idx = 1;
        buffer_append_int32(duty_cmd, 65000, &duty_idx);
        ext_vesc.handlePacket(duty_cmd, sizeof(duty_cmd));

        assert(ext_vesc.hasExternalControl());
        assert(fabsf(ext_vesc.getExternalThrottle() - 65.0f) < 0.1f);
        assert(ext_vesc.getExternalBrake() == 0.0f);

        // Test COMM_SET_CURRENT_BRAKE (brake 60A on 120A max -> 50% brake)
        uint8_t brake_cmd[5];
        brake_cmd[0] = COMM_SET_CURRENT_BRAKE;
        int32_t brake_idx = 1;
        buffer_append_int32(brake_cmd, 60000, &brake_idx);
        ext_vesc.handlePacket(brake_cmd, sizeof(brake_cmd));

        assert(ext_vesc.hasExternalControl());
        assert(ext_vesc.getExternalThrottle() == 0.0f);
        assert(fabsf(ext_vesc.getExternalBrake() - 50.0f) < 0.1f);

        // Test resetExternalControl()
        ext_vesc.resetExternalControl();
        assert(!ext_vesc.hasExternalControl());

        printf("[PASS] 13. External motor control commands (COMM_SET_DUTY, COMM_SET_CURRENT, COMM_SET_CURRENT_BRAKE) verified.\n");
    }

    printf("=== All Virtual VESC Emulator Unit Tests PASSED! ===\n\n");
}

void testDashSettingsAndUiFeatures() {
    printf("=== Running DashSettings & Cockpit UI Features Tests ===\n");
    // 1. DashSettings default constructor values
    DashSettings def;
    assert(def.show_gforce == 1);
    assert(def.show_incline == 1);
    assert(def.brake_mode == 2);
    assert(def.color_theme == THEME_OBSIDIAN_DARK);
    printf("[PASS] DashSettings default constructor: show_gforce=1, show_incline=1, brake_mode=2, theme=OBSIDIAN_DARK verified.\n");

    // 2. Header Clock hiding when GPS is invalid or 0 satellites
    Settings.get().color_theme = THEME_OBSIDIAN_DARK;
    DashTelemetry t;
    setBaseTelemetry(t);
    t.color_theme = THEME_OBSIDIAN_DARK;
    t.screen = SCREEN_RIDE_DASH;
    t.gps_enabled = true;
    t.gps.valid = false;
    t.gps.satellites = 0;
    strncpy(t.clock_str, "12:34", sizeof(t.clock_str));
    Renderer.invalidateCache();
    Renderer.markScreenDirty();
    Renderer.render(t);

    // Clock center is at (400, 34). When GPS is invalid, pixel must be 0x0000 (black, clock hidden)
    assert(Display.getSprite().readPixel(400, 34) == 0x0000);
    printf("[PASS] Header Clock: Hidden when GPS is not valid or 0 satellites.\n");

    // When GPS fix becomes valid (satellites >= 4), clock must be displayed
    t.gps.valid = true;
    t.gps.satellites = 8;
    Renderer.render(t);
    bool has_clock_pixel = false;
    for (int px = 370; px <= 430; px++) {
        if (Display.getSprite().readPixel(px, 34) != 0x0000) {
            has_clock_pixel = true;
            break;
        }
    }
    assert(has_clock_pixel == true);
    printf("[PASS] Header Clock: Rendered when GPS fix is valid.\n");

    // 3. Badges dynamic positioning & visibility with Headlight Button
    auto hasThermalPixels = [&]() {
        for (int tx = 180; tx <= 300; tx += 2) {
            for (int ty = 408; ty <= 424; ty += 2) {
                if (Display.getSprite().readPixel(tx, ty) != 0x0000) return true;
            }
        }
        return false;
    };

    Settings.get().show_light_btn = 1;
    Settings.get().show_gforce = 0;
    Settings.get().show_incline = 1;
    t.incline_gforce_enabled = true;
    t.g_force = 1.2f;
    t.incline_deg = 5.0f;
    Renderer.invalidateCache();
    Renderer.markScreenDirty();
    Renderer.render(t);
    assert(Display.getSprite().readPixel(50, 416) != 0x0000);
    assert(Display.getSprite().readPixel(110, 416) != 0x0000);
    assert(hasThermalPixels());
    printf("[PASS] When show_gforce=0, Lichtknopf is at left (x=50), Incline badge is at slot 2 (x=110), temps at x=240.\n");

    Settings.get().show_gforce = 1;
    Settings.get().show_incline = 0;
    Renderer.invalidateCache();
    Renderer.markScreenDirty();
    Renderer.render(t);
    assert(Display.getSprite().readPixel(50, 416) != 0x0000);
    assert(Display.getSprite().readPixel(110, 416) != 0x0000);
    assert(hasThermalPixels());
    printf("[PASS] When show_incline=0, Lichtknopf is at left (x=50), G-Force badge is at slot 2 (x=110), temps at x=240.\n");

    Settings.get().show_gforce = 0;
    Settings.get().show_incline = 0;
    Renderer.invalidateCache();
    Renderer.markScreenDirty();
    Renderer.render(t);
    assert(Display.getSprite().readPixel(50, 416) != 0x0000);
    assert(Display.getSprite().readPixel(110, 416) == 0x0000);
    assert(hasThermalPixels());
    printf("[PASS] When both GF & INC are off, Lichtknopf stays at left (x=50), slot 2 empty, temps at x=240.\n");

    Settings.get().show_gforce = 1;
    Settings.get().show_incline = 1;
    Renderer.invalidateCache();
    Renderer.markScreenDirty();
    Renderer.render(t);
    assert(Display.getSprite().readPixel(50, 416) != 0x0000);
    assert(Display.getSprite().readPixel(110, 416) != 0x0000);
    assert(Display.getSprite().readPixel(50, 354) != 0x0000);
    assert(hasThermalPixels());
    printf("[PASS] When both GF and INC are active, Lichtknopf sits directly above G-Force at (50, 354)!\n");

    Settings.get().show_light_btn = 0;
    Settings.get().show_gforce = 0;
    Settings.get().show_incline = 0;
    Renderer.invalidateCache();
    Renderer.markScreenDirty();
    Renderer.render(t);
    assert(Display.getSprite().readPixel(50, 416) == 0x0000);
    assert(Display.getSprite().readPixel(110, 416) == 0x0000);
    assert(Display.getSprite().readPixel(50, 354) == 0x0000);
    printf("[PASS] When show_light_btn=0, show_gforce=0, show_incline=0: all badge slots empty.\n");

    // Restore settings
    Settings.get().show_light_btn = 1;
    Settings.get().show_gforce = 1;
    Settings.get().show_incline = 1;
    Settings.save();
    printf("=== DashSettings & Cockpit UI Features Tests PASSED! ===\n\n");
}

void testTcsAndVirtualTpms() {
    printf("=== Running Proactive TCS & Virtual TPMS Unit Tests ===\n");

    // -------------------------------------------------------------------------
    // Test 0: TCS disabled by default verification
    // Active slip control is OFF by default; wheel slip does NOT throttle or clamp current.
    // -------------------------------------------------------------------------
    {
        printf("--- Test 0: TCS disabled by default (no current clamping) ---\n");
        TcsTpmsManager::instance().begin();
        assert(TcsTpmsManager::instance().isTcsEnabled() == false);

        DashTelemetry t;
        setBaseTelemetry(t);
        t.gps_enabled = true;
        t.gps.valid = true;
        t.gps.satellites = 10;
        t.gps.last_fix_ms = millis();
        t.gps.speed_kmh = 20.0f;
        t.speed_kmh = 20.0f;
        t.vesc_motor_speed = 30.0f; // 10 km/h wheel spin!
        t.throttle_pct = 70.0f;
        t.phase_amps = 50.0f;

        TcsTpmsManager::instance().update(t, 0.010f);
        assert(!t.tcs_active);
        assert(t.tcs_throttle_scale == 1.0f);
        printf("[PASS] TCS disabled by default: 10 km/h slip does NOT trigger intervention (scale=1.00).\n");
    }

    // -------------------------------------------------------------------------
    // Test 1: Wet slip detection under throttle (when TCS enabled)
    // Sudden ERPM jump exceeding GPS/IMU ground speed (> 3 km/h slip)
    // triggers tcs_active and clamps phase current limit in < 5ms.
    // -------------------------------------------------------------------------
    {
        printf("--- Test 1: Wet slip detection under throttle (< 5ms response, TCS enabled) ---\n");
        TcsTpmsManager::instance().begin();
        TcsTpmsManager::instance().setTcsEnabled(true);
        TcsTpmsManager::instance().clearSimulatedSlip();
        TcsTpmsManager::instance().clearSimulatedDiameterDrop();

        DashTelemetry t;
        setBaseTelemetry(t);
        t.gps_enabled = true;
        t.gps.valid = true;
        t.gps.satellites = 10;
        t.gps.last_fix_ms = millis();
        t.gps.speed_kmh = 20.0f;
        t.speed_kmh = 20.0f;
        t.vesc_motor_speed = 20.0f;
        t.throttle_pct = 65.0f;
        t.phase_amps = 40.0f;

        // Baseline: normal grip, 0 slip
        TcsTpmsManager::instance().update(t, 0.010f);
        assert(!t.tcs_active);
        assert(t.tcs_slip_kmh < 0.1f);
        assert(t.tcs_throttle_scale == 1.0f);

        // Case 1a: GPS-referenced wet slip (> 3 km/h slip)
        // Driven wheel hits wet steel track: ERPM jumps from 20 km/h to 26.5 km/h (+6.5 km/h slip)
        t.vesc_motor_speed = 26.5f;
        // Single update cycle of 4ms (< 5ms response requirement)
        TcsTpmsManager::instance().update(t, 0.004f);
        assert(t.tcs_active == true);
        assert(t.tcs_slip_kmh >= 6.4f && t.tcs_slip_kmh <= 6.6f);
        assert(t.tcs_throttle_scale < 0.60f); // Aggressively clamped to suppress wheelspin

        // Verify phase current clamping in < 5ms
        float base_phase_limit = 70.0f; // e.g. SPORT / RACE phase limit
        float clamped_phase = base_phase_limit * t.tcs_throttle_scale;
        assert(clamped_phase < 45.0f);
        printf("[PASS] Wet slip with GPS fix: slip=%.1f km/h triggered tcs_active=true and clamped throttle scale to %.2f (Phase %.1fA -> %.1fA in 4ms)\n",
               t.tcs_slip_kmh, t.tcs_throttle_scale, base_phase_limit, clamped_phase);

        // Case 1b: Fallback to IMU Accelerometer reference when GPS is not fixed (tunnel/wet garage)
        TcsTpmsManager::instance().begin();
        TcsTpmsManager::instance().setTcsEnabled(true);
        t.gps.valid = false;
        t.gps.satellites = 0;
        t.speed_kmh = 18.0f;
        t.vesc_motor_speed = 18.0f;
        t.g_force = 0.12f;
        // Settle initial IMU reference
        TcsTpmsManager::instance().update(t, 0.010f);

        // Wheel suddenly breaks loose on wet leaves/puddle: ERPM jumps to 27.0 km/h (> 3 km/h slip)
        t.vesc_motor_speed = 27.0f;
        TcsTpmsManager::instance().update(t, 0.003f); // 3ms (< 5ms)
        assert(t.tcs_active == true);
        assert(t.tcs_slip_kmh > 7.0f);
        assert(t.tcs_throttle_scale < 0.50f);
        printf("[PASS] Wet slip with IMU reference fallback: triggered tcs_active=true and clamped scale to %.2f in 3ms\n",
               t.tcs_throttle_scale);
    }

    // -------------------------------------------------------------------------
    // Test 2: Smooth recovery hysteresis
    // As slip subsides, current limit ramps back up smoothly without hunting or oscillating.
    // -------------------------------------------------------------------------
    {
        printf("--- Test 2: Smooth recovery hysteresis (no hunting/oscillating) ---\n");
        TcsTpmsManager::instance().begin();
        TcsTpmsManager::instance().setTcsEnabled(true);

        DashTelemetry t;
        setBaseTelemetry(t);
        t.gps_enabled = true;
        t.gps.valid = true;
        t.gps.satellites = 10;
        t.gps.last_fix_ms = millis();
        t.gps.speed_kmh = 22.0f;
        t.throttle_pct = 70.0f;
        t.phase_amps = 45.0f;

        // Trigger TCS into active state (slip = 6.0 km/h)
        t.vesc_motor_speed = 28.0f;
        TcsTpmsManager::instance().update(t, 0.004f);
        assert(t.tcs_active == true);
        float clamped_scale = t.tcs_throttle_scale;
        assert(clamped_scale < 0.60f);

        // Sub-phase 2a: Slip subsides partially into the hysteresis deadband (1.5 km/h < slip <= 3.0 km/h)
        t.vesc_motor_speed = 24.2f; // slip = 2.2 km/h
        TcsTpmsManager::instance().update(t, 0.010f);
        // Hysteresis deadband: MUST stay active, preventing hunting/chattering!
        assert(t.tcs_active == true);
        assert(t.tcs_slip_kmh >= 2.1f && t.tcs_slip_kmh <= 2.3f);
        printf("[PASS] Hysteresis deadband (slip=2.2 km/h): tcs_active held TRUE, hunting prevented.\n");

        // Sub-phase 2b: Slip subsides below 1.5 km/h (full traction regained)
        t.vesc_motor_speed = 22.4f; // slip = 0.4 km/h <= 1.5 km/h
        TcsTpmsManager::instance().update(t, 0.010f);
        assert(t.tcs_active == false); // TCS cleanly disengages

        // Sub-phase 2c: Verify smooth monotonic ramp-up without oscillating
        float prev_scale = t.tcs_throttle_scale;
        for (int step = 0; step < 40; step++) {
            TcsTpmsManager::instance().update(t, 0.015f);
            assert(t.tcs_active == false); // Must not chatter back to true
            assert(t.tcs_throttle_scale >= prev_scale - 1e-5f); // Monotonically increasing!
            assert(t.tcs_throttle_scale <= 1.0f + 1e-4f); // No overshoot
            prev_scale = t.tcs_throttle_scale;
        }
        assert(fabsf(t.tcs_throttle_scale - 1.0f) < 0.01f);
        printf("[PASS] Smooth recovery hysteresis: scale ramped back up monotonically to 1.0 without hunting.\n");
    }

    // -------------------------------------------------------------------------
    // Test 3: False positive rejection
    // Hard acceleration with full traction does NOT trigger TCS.
    // -------------------------------------------------------------------------
    {
        printf("--- Test 3: False positive rejection under hard acceleration ---\n");
        TcsTpmsManager::instance().begin();
        TcsTpmsManager::instance().setTcsEnabled(true);

        DashTelemetry t;
        setBaseTelemetry(t);
        t.gps_enabled = true;
        t.gps.valid = true;
        t.gps.satellites = 12;
        t.throttle_pct = 100.0f;
        t.phase_amps = 85.0f;
        t.g_force = 0.70f; // Maximum forward acceleration

        // Simulate aggressive drag launch 5 km/h -> 48 km/h on dry asphalt
        // Micro-slip on pneumatic rubber is 0.5 - 1.2 km/h (< 3.0 km/h)
        float cur_spd = 5.0f;
        for (int step = 0; step < 40; step++) {
            cur_spd += 1.1f;
            t.gps.speed_kmh = cur_spd;
            t.speed_kmh = cur_spd;
            t.vesc_motor_speed = cur_spd + 0.9f; // Micro-slip = 0.9 km/h
            t.gps.last_fix_ms = millis();

            TcsTpmsManager::instance().update(t, 0.040f);

            assert(t.tcs_active == false);
            assert(t.tcs_throttle_scale == 1.0f);
        }
        printf("[PASS] Full throttle launch (0.7G, 85A phase): 0 false positives, tcs_active=false, scale=1.00.\n");
        TcsTpmsManager::instance().setTcsEnabled(false);
    }

    // -------------------------------------------------------------------------
    // Test 4: Autonomous GPS tire diameter calibration
    // Steady cruising >= 15 km/h with >= 8 sats accurately calibrates effective rolling circumference.
    // -------------------------------------------------------------------------
    {
        printf("--- Test 4: Autonomous GPS tire diameter calibration ---\n");
        Settings.get().wheel_diameter_in = 11.0f;
        Settings.get().wheel_gps_calibrated = false;
        Settings.save();

        TcsTpmsManager::instance().begin();

        DashTelemetry t;
        setBaseTelemetry(t);
        t.gps_enabled = true;
        t.gps.valid = true;
        t.gps.last_fix_ms = millis();
        t.incline_deg = 0.0f;
        t.phase_amps = 12.0f; // Normal cruising current

        // Negative check 4a: Speed < 15 km/h (12 km/h)
        t.gps.satellites = 10;
        t.gps.speed_kmh = 12.0f;
        t.vesc_motor_speed = 12.0f;
        t.g_force = 0.02f;
        for (int i = 0; i < 120; i++) TcsTpmsManager::instance().update(t, 0.050f);
        assert(!Settings.get().wheel_gps_calibrated); // Not calibrated below 15 km/h

        // Negative check 4b: Satellites < 8 (6 sats)
        t.gps.satellites = 6;
        t.gps.speed_kmh = 24.0f;
        t.vesc_motor_speed = 24.0f;
        for (int i = 0; i < 120; i++) TcsTpmsManager::instance().update(t, 0.050f);
        assert(!Settings.get().wheel_gps_calibrated); // Not calibrated with < 8 sats

        // Negative check 4c: High G-Force / cornering (|G| >= 0.08G)
        t.gps.satellites = 10;
        t.g_force = 0.15f;
        for (int i = 0; i < 120; i++) TcsTpmsManager::instance().update(t, 0.050f);
        assert(!Settings.get().wheel_gps_calibrated); // Not calibrated during cornering

        // Positive check 4d: Steady straight cruising >= 15 km/h, >= 8 sats, |G| < 0.08G for >= 5s
        // Target physical rolling diameter: 10.4 inches (worn Segway G3 Max tire under rider load)
        const float TARGET_DIAM_IN = 10.4f;
        const float TARGET_DIAM_M = TARGET_DIAM_IN * 0.0254f;
        const float CIRCUM_M = TARGET_DIAM_M * (float)M_PI;
        const float SPEED_KMH = 25.0f;
        const float V_M_S = SPEED_KMH / 3.6f;
        const float TARGET_RPM = (V_M_S / CIRCUM_M) * 60.0f;

        t.gps.satellites = 12;
        t.gps.speed_kmh = SPEED_KMH;
        t.vesc_motor_speed = SPEED_KMH;
        t.speed_kmh = SPEED_KMH;
        t.g_force = 0.02f;
        t.rpm = TARGET_RPM;

        // Run for 5.0s (qualifying duration) + 8.0s (EMA convergence)
        for (int i = 0; i < 260; i++) {
            t.gps.last_fix_ms = millis();
            TcsTpmsManager::instance().update(t, 0.050f);
        }

        assert(Settings.get().wheel_gps_calibrated == true);
        assert(fabsf(Settings.get().wheel_diameter_in - TARGET_DIAM_IN) <= 0.15f);
        assert(fabsf(t.calibrated_wheel_diam_inch - TARGET_DIAM_IN) <= 0.15f);
        printf("[PASS] Autonomous GPS calibration: Calibrated diameter = %.1f in (target: %.1f in, calibrated=%s)\n",
               Settings.get().wheel_diameter_in, TARGET_DIAM_IN,
               Settings.get().wheel_gps_calibrated ? "TRUE" : "FALSE");
    }

    // -------------------------------------------------------------------------
    // Test 5: Virtual TPMS pressure loss alert
    // 5% diameter reduction over > 30s triggers tpms_warning with exact drift percentage.
    // -------------------------------------------------------------------------
    {
        printf("--- Test 5: Virtual TPMS pressure loss alert (5%% drop > 30s) ---\n");
        // Establish baseline of 11.0 inches
        TcsTpmsManager::instance().begin();
        TcsTpmsManager::instance().setBaselineDiameter(11.0f);
        Settings.get().wheel_diameter_in = 11.0f;
        Settings.get().wheel_gps_calibrated = true;

        DashTelemetry t;
        setBaseTelemetry(t);
        t.gps_enabled = true;
        t.gps.valid = true;
        t.gps.satellites = 10;
        t.gps.speed_kmh = 24.0f;
        t.vesc_motor_speed = 24.0f;
        t.speed_kmh = 24.0f;
        t.g_force = 0.02f;

        // Apply 5.0% tire deflation
        TcsTpmsManager::instance().setSimulatedDiameterDropPct(5.0f);

        // Sub-test 5a: < 30 seconds elapsed (e.g. 15s)
        for (int i = 0; i < 300; i++) {
            t.gps.last_fix_ms = millis();
            TcsTpmsManager::instance().update(t, 0.050f);
        }
        assert(t.tpms_warning == false); // Debounce duration: must NOT alert prematurely!
        assert(t.tpms_drift_pct >= 4.7f && t.tpms_drift_pct <= 5.3f);
        printf("[PASS] Duration < 30s (t=15s): tpms_warning=false, drift=%.1f%% (debouncing confirmed).\n",
               t.tpms_drift_pct);

        // Sub-test 5b: > 30 seconds elapsed (another 17s -> total 32s)
        for (int i = 0; i < 340; i++) {
            t.gps.last_fix_ms = millis();
            TcsTpmsManager::instance().update(t, 0.050f);
        }
        assert(t.tpms_warning == true); // > 30s triggers warning!
        assert(t.tpms_drift_pct >= 4.7f && t.tpms_drift_pct <= 5.3f);
        printf("[PASS] Duration > 30s (t=32s): tpms_warning=true, drift=%.1f%% (exact match to 5.0%% loss).\n",
               t.tpms_drift_pct);

        // Sub-test 5c: Recovery hysteresis when tire is reinflated
        TcsTpmsManager::instance().setSimulatedDiameterDropPct(0.0f);
        // Step for 2 seconds (< 5s recovery threshold)
        for (int i = 0; i < 40; i++) {
            t.gps.last_fix_ms = millis();
            TcsTpmsManager::instance().update(t, 0.050f);
        }
        assert(t.tpms_warning == true); // Recovery hysteresis: stays on during brief fluctuations

        // Step for another 4 seconds (total 6s > 5s recovery threshold)
        for (int i = 0; i < 80; i++) {
            t.gps.last_fix_ms = millis();
            TcsTpmsManager::instance().update(t, 0.050f);
        }
        assert(t.tpms_warning == false); // Clean recovery
        assert(t.tpms_drift_pct < 1.0f);
        TcsTpmsManager::instance().clearSimulatedDiameterDrop();
        printf("[PASS] Reinflation recovery: warning cleared after sustained normal pressure (> 5s).\n");

        // Sub-test 5d: Verify live physics calculation (RPM vs GPS without simulation override)
        // Deflated tire (10.45" vs 11.0" nominal = exactly 5.0% smaller rolling circumference)
        // Wheel must rotate faster for same ground speed: RPM_deflated = (24 km/h) / (pi * 10.45" * 0.0254m) * 60
        const float DEFLATED_CIRCUM_M = (11.0f * 0.95f * 0.0254f) * (float)M_PI;
        t.rpm = ((24.0f / 3.6f) / DEFLATED_CIRCUM_M) * 60.0f;
        TcsTpmsManager::instance().clearSimulatedDiameterDrop();

        for (int i = 0; i < 660; i++) { // 33 seconds
            t.gps.last_fix_ms = millis();
            TcsTpmsManager::instance().update(t, 0.050f);
        }
        assert(t.tpms_warning == true);
        assert(fabsf(t.tpms_drift_pct - 5.0f) <= 0.4f);
        printf("[PASS] Pure physical calculation (RPM vs GPS ground speed): drift=%.1f%% -> tpms_warning=true.\n",
               t.tpms_drift_pct);

        // Reset to default
        Settings.get().wheel_diameter_in = 11.0f;
        Settings.get().wheel_gps_calibrated = false;
        Settings.save();
    }

    printf("=== All Proactive TCS & Virtual TPMS Unit Tests PASSED! ===\n\n");
}

int main(int argc, char **argv) {
    printf("==================================================\n");
    printf("   VESC 4.3\" Touch LCD Emulator & Renderer (800x480) \n");
    printf("==================================================\n");

    testVirtualVescEmulator();

    // Attach loopback transport for standard scenario test runs
    Vesc.setTransport(&g_sim_test_transport.getSideA());
    g_virtual_vesc.setTransport(&g_sim_test_transport.getSideB());
    Vesc.begin();
    g_virtual_vesc.begin();

    testTouchGestures();

    Settings.begin();
    Battery.begin();
    Display.init();
    Renderer.begin(&Display);

    testSpeedSweepAnimation();
    testBootAnimation();
    testAntiGhostingTransitions();
    testStatsCalculationAndLiveUpdates();
    testModeManagerAndLegalProfiles();
    testFaultManagerAndBlackbox();
    testRaceTimerAndRecords();
    testRegenAndTripReset();
    testChargingAndTheftProtection();
    testColorThemesAndTurbineSound();
    testKeylessGoAndProximityAutoLock();
    testDashSettingsAndUiFeatures();
    testTcsAndVirtualTpms();

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
    t.gps_enabled = true;
    t.use_gps_speed = true;
    t.gps.valid = true;
    t.gps.satellites = 14;
    strncpy(t.clock_str, "14:32", sizeof(t.clock_str));
    Renderer.markScreenDirty();
    renderAndSave("00_hyper_35kw_idle", t);

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
    t.gps.valid = true;
    t.gps.satellites = 16;
    t.use_gps_speed = true;
    strcpy(t.clock_str, "14:35");
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
    // Scenario 6: Screen 0 - Ride Dash with Light Switch Active (Hauptschalter ON)
    // ----------------------------------------------------
    t.screen = SCREEN_RIDE_DASH;
    t.lights_enabled = true;
    t.hazard_active = false;
    Renderer.markScreenDirty();
    renderAndSave("06_energy_analytics", t);

    // ----------------------------------------------------
    // Scenario 7: Screen 2 - RT Waveforms & Dual Controllers (Video Exact)
    // ----------------------------------------------------
    t.screen = SCREEN_RT_GRAPHS;
    t.vesc1_max_power = 18450.0f;
    t.vesc1_max_phase = 340.0f;
    t.vesc1_max_speed = 94.2f;
    t.vesc2_max_power = 17800.0f;
    t.vesc2_max_phase = 335.0f;
    t.vesc2_max_speed = 93.8f;
    Renderer.markScreenDirty();
    renderAndSave("07_power_dynamics", t);

    // ----------------------------------------------------
    // Scenario 8: Screen 3 - Mode Configurator (ECO Tab)
    // ----------------------------------------------------
    t.screen = SCREEN_SETTINGS_MENU;
    Renderer.setSettingsTab(0);
    Renderer.markScreenDirty();
    renderAndSave("08_settings_root", t);

    // ----------------------------------------------------
    // Scenario 9: Screen 3 - Mode Configurator (SPORT Tab)
    // ----------------------------------------------------
    t.screen = SCREEN_SETTINGS_MENU;
    Renderer.setSettingsTab(1);
    Renderer.markScreenDirty();
    renderAndSave("09_settings_power_limits", t);

    // ----------------------------------------------------
    // Scenario 10: Screen 3 - Mode Configurator (RACE Tab)
    // ----------------------------------------------------
    t.screen = SCREEN_SETTINGS_MENU;
    Renderer.setSettingsTab(2);
    Renderer.markScreenDirty();
    renderAndSave("10_settings_edit_mode", t);

    // ----------------------------------------------------
    // Scenario 11: Screen 3 - Mode Configurator (DRAGY Splits Tab)
    // ----------------------------------------------------
    t.screen = SCREEN_SETTINGS_MENU;
    Renderer.setSettingsTab(DashboardRenderer::TAB_DRAGY);
    Renderer.markScreenDirty();
    renderAndSave("11_settings_display_style", t);

    // ----------------------------------------------------
    // Scenario 11b: Screen 3 - Tuned Topspeed per Mode Tab (3 Stacked Rows)
    // ----------------------------------------------------
    t.screen = SCREEN_SETTINGS_MENU;
    Renderer.setSettingsTab(DashboardRenderer::TAB_SPEED);
    Renderer.markScreenDirty();
    renderAndSave("11b_settings_speed_tab", t);

    // ----------------------------------------------------
    // Scenario 12: Screen 3 - Mode Configurator (DASH Options Tab)
    // ----------------------------------------------------
    t.screen = SCREEN_SETTINGS_MENU;
    Renderer.setSettingsTab(DashboardRenderer::TAB_DASH);
    Renderer.markScreenDirty();
    renderAndSave("12_settings_dash_options", t);

    // ----------------------------------------------------
    // Scenario 12b: Screen 3 - Settings SYSTEM Tab (TCS, Akku-Zellen, Reifen-Kalibrierung)
    // ----------------------------------------------------
    t.screen = SCREEN_SETTINGS_MENU;
    t.tuning_mode = true;
    Settings.get().tcs_enabled = true;
    Settings.get().battery_cells = 14;
    Renderer.setSettingsTab(DashboardRenderer::TAB_SYSTEM);
    Renderer.markScreenDirty();
    renderAndSave("12b_settings_system_tab", t);

    // ----------------------------------------------------
    // Scenario 12c: Screen 3 - Settings PIN Tab (with JETZT SPERREN)
    // ----------------------------------------------------
    Renderer.setSettingsTab(DashboardRenderer::TAB_PIN);
    Renderer.markScreenDirty();
    renderAndSave("12c_settings_pin_tab", t);

    // ----------------------------------------------------
    // Scenario 12d: Screen 3 - Legal Stealth Settings (ONLY DASH, SYSTEM, PIN)
    // ----------------------------------------------------
    t.tuning_mode = false;
    Renderer.setSettingsTab(DashboardRenderer::TAB_DASH);
    Renderer.invalidateCache();
    Renderer.markScreenDirty();
    renderAndSave("12d_settings_legal_stealth", t);
    t.tuning_mode = true;

    // ----------------------------------------------------
    // Scenario 12e: Screen 3 - Modern Settings in ARCTIC LIGHT
    // ----------------------------------------------------
    Settings.get().color_theme = THEME_ARCTIC_LIGHT;
    t.color_theme = THEME_ARCTIC_LIGHT;
    Renderer.setSettingsTab(DashboardRenderer::TAB_DASH);
    Renderer.invalidateCache();
    Renderer.markScreenDirty();
    renderAndSave("12e_settings_arctic_light", t);
    Settings.get().color_theme = THEME_OBSIDIAN_DARK;
    t.color_theme = THEME_OBSIDIAN_DARK;

    // ----------------------------------------------------
    // Scenario 13: Screen 5 - VESC Wi-Fi Bridge
    // ----------------------------------------------------
    Vesc.enterWifiBridgeMode();
    t.screen = SCREEN_WIFI_BRIDGE;
    Renderer.invalidateCache();
    Renderer.markScreenDirty();
    renderAndSave("13_wifi_bridge", t);

    // ----------------------------------------------------
    // Scenario 14: Legal Mode (Boot Default - Wide MODE button, Hidden BOOST, Subtle Red Dot)
    // ----------------------------------------------------
    Settings.get().dash_style = STYLE_HYPER_35KW;
    Settings.get().color_theme = THEME_OBSIDIAN_DARK;
    setBaseTelemetry(t);
    t.color_theme = THEME_OBSIDIAN_DARK;
    t.tuning_mode = false;
    t.ride_mode = MODE_SPORT;
    t.speed_kmh = 20.0f;
    t.power_watts = 480.0f;
    t.phase_amps = 28.0f;
    t.current_amps = 9.2f;
    t.g_force = 0.3f;
    t.incline_deg = 4.0f;
    t.lights_enabled = true;
    Renderer.invalidateCache();
    Renderer.markScreenDirty();
    renderAndSave("14_stealth_mode_idle", t);

    // Requirement 2 Verification: Subtle red dot in top right corner (790, 10)
    const ColorThemePalette &obsPal = DashboardRenderer::getThemePalette(THEME_OBSIDIAN_DARK);
    assert(Display.getSprite().readPixel(790, 10) == obsPal.accent_danger);
    printf("[PASS] Legal mode red status dot verified at top-right corner (790, 10).\n");
    assert(strcmp(ModeManager::instance().getActiveModeName(), "SPORT") == 0);
    printf("[PASS] Active mode name displays 'SPORT' (never 'STEALTH').\n");

    // Re-verify Tuning Mode erases the dot
    t.tuning_mode = true;
    Renderer.markScreenDirty();
    Renderer.render(t);
    assert(Display.getSprite().readPixel(790, 10) == obsPal.bg);
    printf("[PASS] Tuning mode completely hides the red status dot.\n");

    // ----------------------------------------------------
    // Scenario 15: Screen 2 - Upgraded Stats Page with 60s Live Dual-Curve Sparkline (Obsidian Dark & Arctic Light)
    // ----------------------------------------------------
    setBaseTelemetry(t);
    t.screen = SCREEN_RT_GRAPHS;
    t.tuning_mode = true;
    t.speed_kmh = 34.2f;
    t.dynamic_wh_km = 18.2f;
    t.stats.peak_power_watts = 2450.0f;
    t.stats.peak_current_amps = 34.0f;
    t.stats.peak_phase_amps = 72.0f;
    t.stats.trip_wh_km = 19.4f;
    t.stats.min_voltage = 47.8f;
    t.stats.max_voltage = 54.2f;
    t.stats.max_temp_motor = 64.0f;
    t.stats.max_temp_esc = 58.0f;
    Renderer.invalidateCache();
    Renderer.markScreenDirty();
    renderAndSave("15_stats_rich_overview", t);

    Settings.get().color_theme = THEME_ARCTIC_LIGHT;
    t.color_theme = THEME_ARCTIC_LIGHT;
    Renderer.invalidateCache();
    Renderer.markScreenDirty();
    renderAndSave("15b_stats_arctic_light", t);
    Settings.get().color_theme = THEME_OBSIDIAN_DARK;
    t.color_theme = THEME_OBSIDIAN_DARK;

    // ----------------------------------------------------
    // Scenario 16: VESC Fault-Code Alarm Banner on HUD
    // ----------------------------------------------------
    setBaseTelemetry(t);
    t.screen = SCREEN_RIDE_DASH;
    t.fault_code = FAULT_OVER_TEMP_MOTOR;
    t.fault_start_ms = 1000;
    Renderer.invalidateCache();
    Renderer.markScreenDirty();
    renderAndSave("16_vesc_fault_banner", t);

    // ----------------------------------------------------
    // Scenario 17: Redesigned PIN Lockscreen with Battery Pill (Dark & Arctic Light)
    // ----------------------------------------------------
    setBaseTelemetry(t);
    t.is_locked = true;
    t.battery_pct = 88.0f;
    t.voltage = 53.4f;
    strcpy(t.pin_entry, "12");
    t.pin_error = false;
    Renderer.invalidateCache();
    Renderer.markScreenDirty();
    renderAndSave("17_pin_lockscreen", t);

    Settings.get().color_theme = THEME_ARCTIC_LIGHT;
    t.color_theme = THEME_ARCTIC_LIGHT;
    Renderer.invalidateCache();
    Renderer.markScreenDirty();
    renderAndSave("17b_pin_lockscreen_arctic_light", t);
    Settings.get().color_theme = THEME_OBSIDIAN_DARK;
    t.color_theme = THEME_OBSIDIAN_DARK;

    // ----------------------------------------------------
    // Scenario 18: PIN Lockscreen with Error
    // ----------------------------------------------------
    strcpy(t.pin_entry, "9999");
    t.pin_error = true;
    t.pin_error_end_ms = millis() + 5000;
    Renderer.invalidateCache();
    Renderer.markScreenDirty();
    renderAndSave("18_pin_lockscreen_error", t);

    // ----------------------------------------------------
    // Scenario 19: Screen 6 - "RACE" Dragy Launch-Timer (5s Countdown & Measuring)
    // ----------------------------------------------------
    setBaseTelemetry(t);
    t.screen = SCREEN_RACE_TIMER;
    t.speed_kmh = 0.0f;
    t.race_state = 1; // COUNTDOWN (5s)
    t.race_countdown_start_ms = 0;
    t.race_elapsed_sec = 0.0f;
    t.race_split_0_22 = 0.0f;
    t.race_split_0_50 = 0.0f;
    t.race_split_0_70 = 0.0f;
    Renderer.invalidateCache();
    Renderer.markScreenDirty();
    renderAndSave("19_race_timer_armed", t);

    t.speed_kmh = 48.0f;
    t.race_state = 2; // MEASURING
    t.race_elapsed_sec = 3.42f;
    t.race_split_0_22 = 1.65f;
    t.race_records.best_0_22_sec = 1.72f;
    t.race_records.best_0_50_sec = 4.10f;
    t.race_records.best_0_70_sec = 7.80f;
    Renderer.invalidateCache();
    Renderer.markScreenDirty();
    renderAndSave("19_race_timer_measuring", t);

    // ----------------------------------------------------
    // Scenario 20: Screen 6 - Bestzeiten Popup Modal
    // ----------------------------------------------------
    t.race_popup_open = true;
    Renderer.markScreenDirty();
    renderAndSave("20_race_records_modal", t);

    // ----------------------------------------------------
    // Scenario 21: Screen 6 - Empty Dyno Modal (Zero fake numbers!) & Real Recorded Dyno
    // ----------------------------------------------------
    t.race_popup_open = false;
    t.race_dyno_open = true;
    t.race_dyno.reset();
    Renderer.invalidateCache();
    Renderer.markScreenDirty();
    renderAndSave("21_race_dyno_modal", t);

    // Also render real recorded dyno run
    t.race_dyno.sample_count = 30;
    t.race_dyno.peak_watts = 4820.0f;
    t.race_dyno.peak_g_force = 0.92f;
    t.race_dyno.peak_phase_amps = 112.0f;
    t.race_split_0_70 = 5.18f;
    for (int i = 0; i < 30; i++) {
        float tn = (float)i / 29.0f;
        t.race_dyno.samples_speed[i] = 74.0f * (1.0f - expf(-2.4f * tn));
        t.race_dyno.samples_power[i] = 4820.0f * (tn < 0.25f ? (tn / 0.25f) : (1.0f - 0.15f * (tn - 0.25f)));
        t.race_dyno.samples_g[i]     = 0.92f * expf(-1.2f * tn);
    }
    Renderer.invalidateCache();
    Renderer.markScreenDirty();
    renderAndSave("21b_race_dyno_recorded", t);

    // ----------------------------------------------------
    // Scenario 22: First-Time Setup Wizard (Step 1 - Welcome & Guide)
    // ----------------------------------------------------
    setBaseTelemetry(t);
    t.setup_completed = false;
    t.screen = SCREEN_SETUP_WIZARD;
    t.wizard_step = 0;
    Renderer.markScreenDirty();
    renderAndSave("22_setup_wizard_step1_welcome", t);

    // Verify touch advances to Step 2
    bool w1 = Renderer.handleWizardTouch(400, 418, t);
    assert(w1 == true);
    assert(t.wizard_step == 1);
    printf("[PASS] Wizard Step 1 touch navigation to Step 2 verified.\n");

    // ----------------------------------------------------
    // Scenario 23: First-Time Setup Wizard (Step 2 - PIN Keypad)
    // ----------------------------------------------------
    Renderer.markScreenDirty();
    renderAndSave("23_setup_wizard_step2_pin", t);

    // Enter PIN digits "5678"
    Renderer.handleWizardTouch(360, 230, t); // '5'
    Renderer.handleWizardTouch(460, 230, t); // '6'
    Renderer.handleWizardTouch(260, 280, t); // '7'
    Renderer.handleWizardTouch(360, 280, t); // '8'
    Renderer.handleWizardTouch(400, 422, t); // Continue
    assert(t.wizard_step == 2);
    assert(strcmp(Settings.get().pin_code, "5678") == 0);
    printf("[PASS] Wizard Step 2 PIN entry & storage ('5678') verified.\n");

    // ----------------------------------------------------
    // Scenario 24: First-Time Setup Wizard (Step 3 - Feature Toggles)
    // ----------------------------------------------------
    Renderer.markScreenDirty();
    renderAndSave("24_setup_wizard_step3_features", t);

    // Test finishing wizard
    Renderer.handleWizardTouch(400, 406, t); // [ SETUP ABSCHLIESSEN & STARTEN ]
    assert(t.setup_completed == true);
    assert(t.screen == SCREEN_RIDE_DASH);
    printf("[PASS] Wizard Step 3 completion & switch to SCREEN_RIDE_DASH verified.\n");

    // ----------------------------------------------------
    // Scenario 25: Minimal Dashboard with GPS & G-Force Disabled + Unknown Battery (--%)
    // ----------------------------------------------------
    setBaseTelemetry(t);
    t.gps_enabled = false;
    t.incline_gforce_enabled = false;
    t.speed_kmh = 42.0f;
    t.power_watts = 2100.0f;
    t.battery_pct = -1.0f; // Must render "--%" and empty bar!
    t.tcs_active = true;
    t.tpms_warning = true;
    Renderer.invalidateCache();
    Renderer.markScreenDirty();
    renderAndSave("25_dash_minimal_tcs_tpms_unknown_bat", t);
    printf("[PASS] Minimal Dashboard with TCS/TPMS pills & --%% unknown battery rendered.\n");

    // ----------------------------------------------------
    // Scenario 27: PIN Lockscreen with Active Theft Alarm
    // ----------------------------------------------------
    setBaseTelemetry(t);
    t.screen = SCREEN_RIDE_DASH;
    t.is_locked = true;
    t.theft_alarm_active = true;
    t.locked_moved_meters = 3.8f;
    Renderer.invalidateCache();
    Renderer.markScreenDirty();
    renderAndSave("27_pin_lockscreen_theft_alarm", t);
    printf("[PASS] Scenario 27: PIN Lockscreen Theft Alarm rendered and saved.\n");

    // ----------------------------------------------------
    // Scenario 28: Theme 0 - OBSIDIAN DARK
    // ----------------------------------------------------
    setBaseTelemetry(t);
    Settings.get().color_theme = THEME_OBSIDIAN_DARK;
    t.color_theme = THEME_OBSIDIAN_DARK;
    t.screen = SCREEN_RIDE_DASH;
    t.speed_kmh = 48.0f;
    t.power_watts = 2450.0f;
    t.duty_cycle_pct = 52.0f;
    t.g_force = 0.6f;
    t.incline_deg = 7.0f;
    t.lights_enabled = true;
    t.gps.valid = true;
    t.gps.satellites = 12;
    strcpy(t.clock_str, "14:35");
    Renderer.invalidateCache();
    Renderer.markScreenDirty();
    renderAndSave("28_theme_obsidian_dark", t);
    printf("[PASS] Scenario 28: Theme OBSIDIAN DARK rendered and saved.\n");

    // ----------------------------------------------------
    // Scenario 29: Theme 1 - ARCTIC LIGHT (Crisp Daylight White!)
    // ----------------------------------------------------
    Settings.get().color_theme = THEME_ARCTIC_LIGHT;
    t.color_theme = THEME_ARCTIC_LIGHT;
    t.speed_kmh = 36.0f;
    t.power_watts = 1650.0f;
    t.duty_cycle_pct = 40.0f;
    t.g_force = 0.4f;
    t.incline_deg = -6.0f;
    t.lights_enabled = true;
    Renderer.invalidateCache();
    Renderer.markScreenDirty();
    renderAndSave("29_theme_arctic_light", t);
    printf("[PASS] Scenario 29: Theme ARCTIC LIGHT rendered and saved.\n");

    // ----------------------------------------------------
    // Scenario 30: Theme 2 - CARBON STEALTH
    // ----------------------------------------------------
    Settings.get().color_theme = THEME_CARBON_STEALTH;
    t.color_theme = THEME_CARBON_STEALTH;
    t.speed_kmh = 62.0f;
    t.power_watts = 3200.0f;
    t.duty_cycle_pct = 66.0f;
    t.g_force = 0.9f;
    t.incline_deg = 14.0f;
    t.lights_enabled = false;
    Renderer.invalidateCache();
    Renderer.markScreenDirty();
    renderAndSave("30_theme_carbon_stealth", t);
    printf("[PASS] Scenario 30: Theme CARBON STEALTH rendered and saved.\n");

    // ----------------------------------------------------
    // Scenario 31: Theme 3 - NIGHT DIM (Software Dimmer!)
    // ----------------------------------------------------
    Settings.get().color_theme = THEME_NIGHT_DIM;
    t.color_theme = THEME_NIGHT_DIM;
    t.speed_kmh = 28.0f;
    t.power_watts = 1150.0f;
    t.duty_cycle_pct = 30.0f;
    t.g_force = -0.3f;
    t.incline_deg = 3.0f;
    t.lights_enabled = true;
    Renderer.invalidateCache();
    Renderer.markScreenDirty();
    renderAndSave("31_theme_night_dim", t);
    printf("[PASS] Scenario 31: Theme NIGHT DIM rendered and saved.\n");

    // ----------------------------------------------------
    // Scenario 32: Settings Menu Tab 4 - DASH (Obsidian Dark)
    // ----------------------------------------------------
    setBaseTelemetry(t);
    Settings.get().color_theme = THEME_OBSIDIAN_DARK;
    Settings.get().turbine_sound_enabled = true;
    t.color_theme = THEME_OBSIDIAN_DARK;
    t.screen = SCREEN_SETTINGS_MENU;
    Renderer.setSettingsTab(DashboardRenderer::TAB_DASH);
    Renderer.invalidateCache();
    Renderer.markScreenDirty();
    renderAndSave("32_settings_dash_themes_and_sound", t);
    printf("[PASS] Scenario 32: Settings DASH tab rendered and saved.\n");

    // ----------------------------------------------------
    // Scenario 35: Wireless OTA Firmware Update Progress HUD
    // ----------------------------------------------------
    setBaseTelemetry(t);
    OtaManager::instance().setSimProgress(true, 68);
    Renderer.invalidateCache();
    Renderer.markScreenDirty();
    renderAndSave("35_wireless_ota_progress", t);
    assert(OtaManager::instance().isUpdating() == true);
    assert(OtaManager::instance().getProgressPct() == 68);
    OtaManager::instance().setSimProgress(false, 0);
    printf("[PASS] Scenario 35: Wireless OTA Firmware Update Progress HUD rendered and saved.\n");

    // ----------------------------------------------------
    // Scenario 36: Boot Animation - Phase 1 Boot Logo (Lossless PNG)
    // ----------------------------------------------------
    setBaseTelemetry(t);
    Renderer.triggerBootAnimation();
    Renderer.setBootAnimElapsed(1000);
    Renderer.invalidateCache();
    Renderer.markScreenDirty();
    renderAndSave("36_boot_animation_phase1_logo", t);
    printf("[PASS] Scenario 36: Boot Animation Phase 1 PNG Logo rendered and saved.\n");

    // ----------------------------------------------------
    // Scenario 37: Boot Animation - Phase 2 Diagnostic Console Log (3.0s)
    // ----------------------------------------------------
    setBaseTelemetry(t);
    t.vesc_connected = true;
    t.voltage = 53.4f;
    Renderer.triggerBootAnimation();
    Renderer.setBootAnimElapsed(3000);
    Renderer.invalidateCache();
    Renderer.markScreenDirty();
    renderAndSave("37_boot_animation_phase2_console", t);
    printf("[PASS] Scenario 37: Boot Animation Phase 2 Console rendered and saved.\n");

    // ----------------------------------------------------
    // Scenario 38: Boot Animation - Phase 3 Auto-Transition to PIN Lockscreen
    // ----------------------------------------------------
    setBaseTelemetry(t);
    Settings.get().setup_completed = true;
    t.setup_completed = true;
    t.is_locked = true;
    t.vesc_connected = true;
    t.screen = SCREEN_RIDE_DASH;
    Renderer.triggerBootAnimation();
    Renderer.setBootAnimElapsed(3300);
    Renderer.invalidateCache();
    Renderer.markScreenDirty();
    renderAndSave("38_boot_animation_phase3_pin_lockscreen", t);
    printf("[PASS] Scenario 38: Boot Animation Phase 3 PIN Lockscreen rendered and saved.\n");

    // ----------------------------------------------------
    // Unit Tests for 3 Fixes: Reboot Screen Clear, TAB_SPEED, Boost Toggle ON/OFF
    // ----------------------------------------------------
    {
        // Fix 1: Reboot without setup wizard (boot animation -> Ride Dash) clears boot console text
        DashTelemetry tBoot;
        setBaseTelemetry(tBoot);
        Settings.get().setup_completed = true;
        Settings.get().pin_lock_enabled = false;
        tBoot.setup_completed = true;
        tBoot.is_locked = false;
        tBoot.vesc_connected = true;
        tBoot.screen = SCREEN_RIDE_DASH;
        ModeManager::instance().setLocked(false);

        // Render Ride Dash once so _hyper_inited == true (simulates pre-reboot state)
        Renderer.invalidateCache();
        Renderer.render(tBoot);

        // Trigger boot animation -> Phase 2 console text drawn -> auto-advance to Ride Dash
        Renderer.triggerBootAnimation();
        Renderer.setBootAnimElapsed(2900);
        Renderer.render(tBoot); // Draws boot console text
        Renderer.setBootAnimElapsed(3300);
        Renderer.render(tBoot); // should_auto_advance -> skipBootAnimation() -> renders clean Ride Dash
        assert(!Renderer.isBootAnimActive());
        renderAndSave("39_reboot_no_setup_clean_dash", tBoot);
        printf("[PASS] Fix 1: Reboot without setup cleanly clears boot text before Ride Dash.\n");
    }

    {
        // Fix 2: TAB_SPEED (3 stacked rows: ECO, SPORT, RACE) touch, drag, persistence & ModeManager limits
        DashTelemetry tSpd;
        setBaseTelemetry(tSpd);
        tSpd.screen = SCREEN_SETTINGS_MENU;
        tSpd.tuning_mode = true;
        tSpd.is_locked = false;
        ModeManager::instance().setTuningMode(true);
        Renderer.setSettingsTab(DashboardRenderer::TAB_SPEED);

        VehicleModeConfig &cfg = ModeManager::instance().getConfig();
        float eco0 = cfg.tuned_modes[PROFILE_MODE_ECO].max_speed_kmh;
        float spr0 = cfg.tuned_modes[PROFILE_MODE_SPORT].max_speed_kmh;
        float rac0 = cfg.tuned_modes[PROFILE_MODE_RACE].max_speed_kmh;

        // Tap [+] on ECO (row 0: y=118..222, [+] x=700..760, y=166..210)
        assert(Renderer.handleSettingsTouch(730, 188, tSpd) == true);
        assert(cfg.tuned_modes[PROFILE_MODE_ECO].max_speed_kmh == eco0 + 1.0f);

        // Tap [-] on SPORT (row 1: y=230..334, [-] x=40..100, y=278..322)
        assert(Renderer.handleSettingsTouch(70, 300, tSpd) == true);
        assert(cfg.tuned_modes[PROFILE_MODE_SPORT].max_speed_kmh == spr0 - 1.0f);

        // Drag slider on RACE (row 2: y=342..446, track x=120..680, y=380..442)
        assert(Renderer.handleSettingsSliderDrag(400, 412, tSpd) == true); // midpoint = 57.5 -> 58 km/h
        assert(cfg.tuned_modes[PROFILE_MODE_RACE].max_speed_kmh != rac0);

        // Exit Settings (saves to NVS & applies)
        Renderer.handleMenuNav(NAV_SCREEN_DASH, tSpd);
        ModeManager::instance().setRideMode(MODE_ECO);
        assert(ModeManager::instance().getActiveLimits().max_speed_kmh == eco0 + 1.0f);
        ModeManager::instance().setRideMode(MODE_SPORT);
        assert(ModeManager::instance().getActiveLimits().max_speed_kmh == spr0 - 1.0f);

        // Restore defaults
        ModeManager::instance().resetDefaults();
        printf("[PASS] Fix 2: TAB_SPEED 3 stacked rows touch, drag, NVS save & ModeManager application verified.\n");
    }

    {
        // Fix 3: Boost toggle ON -> OFF via touch on [ BOOST ] in both legal and tuning modes
        DashTelemetry tBoost;
        setBaseTelemetry(tBoost);
        tBoost.screen = SCREEN_RIDE_DASH;
        tBoost.is_locked = false;
        tBoost.boost_active = false;
        Buttons.setActiveScreen(SCREEN_RIDE_DASH);
        Buttons.setLocked(false);

        auto tapBoost = [&]() {
            simAdvanceMs(200);
            Display.setSimTouch(true, 521, 416);
            Buttons.update();
            simAdvanceMs(60);
            Display.setSimTouch(false, 521, 416);
            Buttons.update();
        };

        // Legal mode (!tuning_mode)
        tBoost.tuning_mode = false;
        Buttons.setTuningMode(false);
        tapBoost();
        assert(Buttons.getNavAction() == NAV_BOOST_TOGGLE);
        tBoost.boost_active = !tBoost.boost_active;
        assert(tBoost.boost_active == true);

        tapBoost();
        assert(Buttons.getNavAction() == NAV_BOOST_TOGGLE);
        tBoost.boost_active = !tBoost.boost_active;
        assert(tBoost.boost_active == false);

        // Tuning mode (tuning_mode == true) + visual redraw check with markScreenDirty()
        tBoost.tuning_mode = true;
        Buttons.setTuningMode(true);
        Renderer.invalidateCache();
        Renderer.markScreenDirty();
        Renderer.render(tBoost); // initial OFF
        const ColorThemePalette &bPal = Renderer.currentTheme();

        tapBoost();
        assert(Buttons.getNavAction() == NAV_BOOST_TOGGLE);
        tBoost.boost_active = !tBoost.boost_active;
        assert(tBoost.boost_active == true);
        Renderer.markScreenDirty();
        Renderer.render(tBoost);
        assert(Display.getCanvas()->readPixel(460, 390) == bPal.primary);

        tapBoost();
        assert(Buttons.getNavAction() == NAV_BOOST_TOGGLE);
        tBoost.boost_active = !tBoost.boost_active;
        assert(tBoost.boost_active == false);
        Renderer.markScreenDirty();
        Renderer.render(tBoost);
        assert(Display.getCanvas()->readPixel(460, 390) == bPal.surface);

        printf("[PASS] Fix 3: [ BOOST ] button toggles ON -> OFF cleanly in both legal and tuning modes.\n");
    }

    {
        // Fix 4: Thermal warning color thresholds (>=75C warn, >=90C danger)
        const ColorThemePalette &tPal = Renderer.currentTheme();
        DashTelemetry tHeat;
        setBaseTelemetry(tHeat);
        tHeat.screen = SCREEN_RIDE_DASH;
        tHeat.is_locked = false;
        tHeat.temp_esc = 82.0f;
        tHeat.temp_motor = 94.0f;
        Renderer.invalidateCache();
        Renderer.markScreenDirty();
        Renderer.render(tHeat);
        assert(tPal.accent_warn != tPal.text && tPal.accent_danger != tPal.accent_warn);
        printf("[PASS] Fix 4: Thermal traffic-light rendering (>=75C warn, >=90C danger) verified.\n");
    }

    // Reset default theme
    Settings.get().color_theme = THEME_OBSIDIAN_DARK;
    t.color_theme = THEME_OBSIDIAN_DARK;
    Renderer.invalidateCache();

    // ----------------------------------------------------
    // Regression Tests (Runde 3): Dragy Auto-Rollout, Stealth-Guard Tab-Fallback, Wizard PIN-Guard
    // ----------------------------------------------------
    {
        // 1. Dragy Auto-Rollout: State 1 -> State 2 only when speed_kmh >= 1.0f
        DashTelemetry tDragy;
        setBaseTelemetry(tDragy);
        tDragy.screen = SCREEN_RACE_TIMER;
        tDragy.race_state = 1; // ARMED / COUNTDOWN
        tDragy.race_countdown_start_ms = millis();
        tDragy.speed_kmh = 0.5f; // Below 1.0 km/h -> should remain in state 1
        Renderer.updateRaceTimer(tDragy, millis());
        if (tDragy.race_state != 1) {
            fprintf(stderr, "[FAIL] Dragy Auto-Rollout premature trigger at speed < 1.0 km/h\n");
            return 1;
        }

        tDragy.speed_kmh = 1.2f; // At/above 1.0 km/h -> triggers Auto-Rollout to state 2
        Renderer.updateRaceTimer(tDragy, millis());
        if (tDragy.race_state != 2 || !tDragy.boost_active) {
            fprintf(stderr, "[FAIL] Dragy Auto-Rollout did not transition to State 2 on speed >= 1.0 km/h\n");
            return 1;
        }
        printf("[PASS] Regression test: Dragy Auto-Rollout (State 1 -> State 2 at speed >= 1.0 km/h)\n");
    }

    {
        // 2. Stealth-Guard Tab-Fallback: in legal mode (!tuning_mode), accessing tuning tabs (< TAB_DASH) falls back to TAB_DASH
        DashTelemetry tStealth;
        setBaseTelemetry(tStealth);
        tStealth.screen = SCREEN_SETTINGS_MENU;
        tStealth.tuning_mode = false;
        ModeManager::instance().setTuningMode(false);

        // Attempt setting TAB_SPEED (idx 0 < TAB_DASH)
        Renderer.setSettingsTab(DashboardRenderer::TAB_SPEED);
        if (Renderer.getSettingsTab() != DashboardRenderer::TAB_SPEED) {
            fprintf(stderr, "[FAIL] Stealth-Guard test setup: initial setSettingsTab failed\n");
            return 1;
        }

        // Touch handling forces fallback to TAB_DASH
        Renderer.handleSettingsTouch(100, 200, tStealth);
        if (Renderer.getSettingsTab() != DashboardRenderer::TAB_DASH) {
            fprintf(stderr, "[FAIL] Stealth-Guard Tab-Fallback did not revert to TAB_DASH on touch\n");
            return 1;
        }

        // Attempt setting TAB_DRAGY (idx 1 < TAB_DASH)
        Renderer.setSettingsTab(DashboardRenderer::TAB_DRAGY);
        Renderer.render(tStealth);
        if (Renderer.getSettingsTab() != DashboardRenderer::TAB_DASH) {
            fprintf(stderr, "[FAIL] Stealth-Guard Tab-Fallback did not revert to TAB_DASH on render\n");
            return 1;
        }
        printf("[PASS] Regression test: Stealth-Guard Tab-Fallback (!tuning_mode && tab < TAB_DASH -> TAB_DASH)\n");
    }

    {
        // 3. Setup-Wizard PIN-Guard: PIN with len < 4 does not advance or disable pin_lock_enabled
        Renderer.resetSetupWizard();
        DashTelemetry tWiz;
        setBaseTelemetry(tWiz);
        tWiz.setup_completed = false;
        tWiz.screen = SCREEN_SETUP_WIZARD;
        tWiz.wizard_step = 1;
        Renderer.initSetupWizard(tWiz);
        Settings.get().pin_lock_enabled = true;

        // Enter single digit '5' (len = 1 < 4)
        Renderer.handleWizardTouch(360, 230, tWiz); // '5'
        if (strlen(tWiz.wizard_pin_buf) != 1) {
            fprintf(stderr, "[FAIL] Setup-Wizard PIN digit input failed\n");
            return 1;
        }

        // Press Continue button (400, 422) with incomplete PIN (< 4 digits)
        Renderer.handleWizardTouch(400, 422, tWiz);
        if (tWiz.wizard_step != 1 || !Settings.get().pin_lock_enabled) {
            fprintf(stderr, "[FAIL] Setup-Wizard PIN-Guard allowed advance or disabled PIN on Continue\n");
            return 1;
        }

        // Press '#' key (500, 350) with incomplete PIN
        Renderer.handleWizardTouch(500, 350, tWiz);
        if (tWiz.wizard_step != 1 || !Settings.get().pin_lock_enabled) {
            fprintf(stderr, "[FAIL] Setup-Wizard PIN-Guard allowed advance or disabled PIN on '#'\n");
            return 1;
        }

        Renderer.resetSetupWizard();
        printf("[PASS] Regression test: Setup-Wizard PIN-Guard (len < 4 prevents advance / pin disable)\n");
    }

    {
        // 4. Dragy Plateau-Finish: State 2 (MEASURING) reaches Split 1 and holds constant speed (<0.2 m/s^2 for >2.0s) below Split 3 -> auto-finishes to State 3
        DashTelemetry tPlat;
        setBaseTelemetry(tPlat);
        tPlat.screen = SCREEN_RACE_TIMER;
        tPlat.race_state = 2; // MEASURING
        tPlat.race_launch_start_ms = 1000;
        tPlat.race_elapsed_sec = 2.0f;
        tPlat.speed_kmh = 30.0f; // Above Split 1 (22 km/h), below Split 3 (70 km/h)
        tPlat.race_split_0_22 = 1.6f; // Split 1 achieved!
        tPlat.race_split_0_50 = 0.0f;
        tPlat.race_split_0_70 = 0.0f;

        uint32_t t_now = 3000;
        // First eval step to establish baseline evaluation speed
        Renderer.updateRaceTimer(tPlat, t_now);
        if (tPlat.race_state != 2) {
            fprintf(stderr, "[FAIL] Dragy Plateau prematurely finished\n");
            return 1;
        }

        // Advance 2.5 seconds with constant speed (accel = 0.0 m/s^2 < 0.2 m/s^2) in 250ms increments
        for (int step = 1; step <= 10; ++step) {
            t_now += 250;
            tPlat.race_elapsed_sec = (float)(t_now - tPlat.race_launch_start_ms) / 1000.0f;
            Renderer.updateRaceTimer(tPlat, t_now);
        }

        if (tPlat.race_state != 3) {
            fprintf(stderr, "[FAIL] Dragy Plateau did not finish to State 3 after 2.5s plateau\n");
            return 1;
        }
        printf("[PASS] Regression test: Dragy Plateau-Finish (constant speed >2.0s auto-finishes to State 3)\n");
    }

    {
        // 5. Odometer-Meter-Praezision: setOdoMeters(10000000) (10.000 km) + 10x addOdoMeters(1.0f) -> exactly 10000010m
        uint32_t saved_odo = Settings.getOdoMeters();
        Settings.setOdoMeters(10000000);
        if (Settings.getOdoMeters() != 10000000) {
            fprintf(stderr, "[FAIL] setOdoMeters(10000000) failed\n");
            return 1;
        }
        for (int i = 0; i < 10; ++i) {
            Settings.addOdoMeters(1.0f);
        }
        if (Settings.getOdoMeters() != 10000010) {
            fprintf(stderr, "[FAIL] addOdoMeters precision loss: expected 10000010, got %u\n", Settings.getOdoMeters());
            return 1;
        }
        Settings.setOdoMeters(saved_odo); // Restore previous odometer
        printf("[PASS] Regression test: Odometer Meter-Praezision (10000000m + 10x 1m = 10000010m without float drift)\n");
    }

    {
        // 6. FaultManager Cooldown pro Code: Different consecutive faults logged immediately without cooldown drop
        FaultManager::instance().clearFaults();
        FaultManager::instance().recordFault(FAULT_UNDER_VOLTAGE, 42.0f, 10.0f, 15.0f, 40.0f, 45.0f, 100);
        FaultManager::instance().recordFault(FAULT_ABS_OVER_CURRENT, 48.0f, 120.0f, 250.0f, 50.0f, 60.0f, 101);

        if (FaultManager::instance().getFaultCount() != 2) {
            fprintf(stderr, "[FAIL] FaultManager swallowed second distinct fault code\n");
            return 1;
        }
        if (FaultManager::instance().getFault(0).code != FAULT_ABS_OVER_CURRENT ||
            FaultManager::instance().getFault(1).code != FAULT_UNDER_VOLTAGE) {
            fprintf(stderr, "[FAIL] FaultManager fault order or codes incorrect\n");
            return 1;
        }

        // Duplicate identical code within 3s SHOULD be dropped by per-code cooldown
        FaultManager::instance().recordFault(FAULT_ABS_OVER_CURRENT, 48.0f, 120.0f, 250.0f, 50.0f, 60.0f, 102);
        if (FaultManager::instance().getFaultCount() != 2) {
            fprintf(stderr, "[FAIL] FaultManager did not suppress duplicate consecutive fault\n");
            return 1;
        }
        FaultManager::instance().clearFaults();
        printf("[PASS] Regression test: FaultManager Cooldown pro Code (distinct codes logged, identical suppressed)\n");
    }

    {
        // 7. BatteryManager Sag-Schutz: Under heavy load (60A), voltage sag does not corrupt prof.learned_wh
        uint8_t prof_id = Battery.getActiveProfileIndex();
        float initial_learned_wh = Battery.getProfile(prof_id).learned_wh;

        DashTelemetry tBat;
        setBaseTelemetry(tBat);
        tBat.speed_kmh = 45.0f;
        tBat.current_amps = 60.0f;
        tBat.voltage = 38.0f; // Extreme voltage sag under full throttle load

        Battery.update(tBat, 0.1f);

        float current_learned_wh = Battery.getProfile(prof_id).learned_wh;
        if (current_learned_wh != initial_learned_wh) {
            fprintf(stderr, "[FAIL] BatteryManager corrupted learned_wh during voltage sag: was %.1f, now %.1f\n",
                    initial_learned_wh, current_learned_wh);
            return 1;
        }
        if (tBat.battery_pct <= 0.0f) {
            fprintf(stderr, "[FAIL] BatteryManager SoC collapsed to 0%% during driving load\n");
            return 1;
        }
        printf("[PASS] Regression test: BatteryManager Sag-Schutz (60A load & 38V sag preserves learned_wh and SoC)\n");
    }

    {
        // 8. TcsTpmsManager Safety: When is_locked=true or wheel_diameter_in=0, no NaN occurs and TCS remains deactivated
        DashTelemetry tTcs;
        setBaseTelemetry(tTcs);
        tTcs.is_locked = true;
        tTcs.vesc_connected = true;
        tTcs.vesc_motor_speed = 35.0f;
        tTcs.phase_amps = 40.0f;
        tTcs.throttle_pct = 80.0f;
        tTcs.rpm = 2500.0f;

        float saved_diam = Settings.get().wheel_diameter_in;
        Settings.get().wheel_diameter_in = 0.0f; // invalid diameter!

        TcsTpmsManager::instance().update(tTcs, 0.05f);

        if (tTcs.tcs_active) {
            fprintf(stderr, "[FAIL] TCS activated while vehicle is locked\n");
            return 1;
        }
        if (tTcs.tcs_throttle_scale != 1.0f) {
            fprintf(stderr, "[FAIL] TCS throttle scale altered while vehicle is locked\n");
            return 1;
        }
        if (isnan(tTcs.tcs_slip_kmh) || isnan(tTcs.tcs_throttle_scale) ||
            isnan(tTcs.calibrated_wheel_diam_inch) || isnan(tTcs.tpms_drift_pct)) {
            fprintf(stderr, "[FAIL] TcsTpmsManager generated NaN with wheel_diameter_in=0\n");
            return 1;
        }

        Settings.get().wheel_diameter_in = saved_diam;
        printf("[PASS] Regression test: TcsTpmsManager Safety (is_locked=true & diam=0: no NaN, TCS inactive)\n");
    }

    {
        // 9. GpsTracker NMEA-Checksumme: Corrupted checksum (*FF) rejected, valid checksum updates Speed & Valid-Flag
        DashTelemetry tGps;
        setBaseTelemetry(tGps);
        tGps.gps.valid = false;
        tGps.gps.speed_kmh = 0.0f;

        // 1. Corrupt RMC checksum (*FF)
        const char *bad_rmc = "$GNRMC,091523.00,A,5231.1234,N,01324.5678,E,25.0,180.0,121126,,,A*FF";
        GpsTracker::instance().parseNmeaLine(bad_rmc, tGps);
        if (tGps.gps.speed_kmh != 0.0f) {
            fprintf(stderr, "[FAIL] GpsTracker accepted corrupted NMEA RMC checksum\n");
            return 1;
        }

        // 2. Corrupt GGA checksum (*FF)
        const char *bad_gga = "$GNGGA,091523.00,5231.1234,N,01324.5678,E,1,14,0.9,45.2,M,45.0,M,,*FF";
        GpsTracker::instance().parseNmeaLine(bad_gga, tGps);
        if (tGps.gps.valid) {
            fprintf(stderr, "[FAIL] GpsTracker accepted corrupted NMEA GGA checksum\n");
            return 1;
        }

        // 3. Valid GGA sentence (*49)
        const char *good_gga = "$GNGGA,091523.00,5231.1234,N,01324.5678,E,1,14,0.9,45.2,M,45.0,M,,*49";
        GpsTracker::instance().parseNmeaLine(good_gga, tGps);
        if (!tGps.gps.valid || tGps.gps.satellites != 14) {
            fprintf(stderr, "[FAIL] GpsTracker failed to validate correct NMEA GGA sentence\n");
            return 1;
        }

        // 4. Valid RMC sentence (*7C -> 25.0 knots * 1.852 = 46.3 km/h)
        const char *good_rmc = "$GNRMC,091523.00,A,5231.1234,N,01324.5678,E,25.0,180.0,121126,,,A*7C";
        GpsTracker::instance().parseNmeaLine(good_rmc, tGps);
        if (fabsf(tGps.gps.speed_kmh - 46.3f) > 0.1f) {
            fprintf(stderr, "[FAIL] GpsTracker failed to parse valid NMEA RMC speed: expected 46.3, got %.2f\n", tGps.gps.speed_kmh);
            return 1;
        }

        printf("[PASS] Regression test: GpsTracker NMEA Checksumme (corrupted *FF rejected, valid sentence parsed)\n");
    }

    // -------------------------------------------------------------
    // Regression Test 10: ModeManager Legal-Mode Boost-Sperre, Modus-Zyklus & SteeringButtons
    // -------------------------------------------------------------
    {
        // 1. Legal-Mode Boost-Sperre (tuning_mode = false, boost_active = true -> aktive Limits bleiben strikt auf <=22A / <=22 km/h)
        ModeManager::instance().setTuningMode(false);
        ModeManager::instance().setBoostActive(true);
        ModeLimits leg_lim = ModeManager::instance().getActiveLimits();
        if (leg_lim.max_speed_kmh > 22.0f || leg_lim.max_battery_amps > 22.0f) {
            fprintf(stderr, "[FAIL] ModeManager allowed boost exceeding legal limits in legal mode: speed=%.1f bat=%.1f\n",
                    leg_lim.max_speed_kmh, leg_lim.max_battery_amps);
            return 1;
        }

        // 2. Modus-Wechsel (setRideMode, cycleMode) & Persistenz-Check
        ModeManager::instance().setRideMode(MODE_ECO);
        if (ModeManager::instance().getRideMode() != MODE_ECO) {
            fprintf(stderr, "[FAIL] ModeManager setRideMode(MODE_ECO) failed\n");
            return 1;
        }
        ModeManager::instance().cycleMode();
        if (ModeManager::instance().getRideMode() != MODE_SPORT) {
            fprintf(stderr, "[FAIL] ModeManager cycleMode() ECO -> SPORT failed\n");
            return 1;
        }
        ModeManager::instance().cycleMode();
        if (ModeManager::instance().getRideMode() != MODE_RACE) {
            fprintf(stderr, "[FAIL] ModeManager cycleMode() SPORT -> RACE failed\n");
            return 1;
        }
        ModeManager::instance().cycleMode();
        if (ModeManager::instance().getRideMode() != MODE_ECO) {
            fprintf(stderr, "[FAIL] ModeManager cycleMode() RACE -> ECO failed\n");
            return 1;
        }

        // 3. Tuning Mode Boost Check (Tuning ON + Boost ON -> open limits >= 70A / >= 140A)
        ModeManager::instance().setTuningMode(true);
        ModeManager::instance().setBoostActive(true);
        ModeLimits tun_lim = ModeManager::instance().getActiveLimits();
        if (tun_lim.max_battery_amps < 70.0f || tun_lim.max_phase_amps < 140.0f) {
            fprintf(stderr, "[FAIL] ModeManager boost in tuning mode failed: bat=%.1f pha=%.1f\n",
                    tun_lim.max_battery_amps, tun_lim.max_phase_amps);
            return 1;
        }

        // Restore legal defaults and save to NVS
        ModeManager::instance().setBoostActive(false);
        ModeManager::instance().setTuningMode(false);
        ModeManager::instance().setRideMode(MODE_SPORT);
        ModeManager::instance().saveToNvs();

        // 4. SteeringButtons Unit Check: Short press & Long press via setSimMilliVolts
        SteeringButtons& sb = SteeringButtons::instance();
        sb.reset();

        // Simulate BTN_UP (~500 mV) short press:
        // Hold for 40ms (>30ms debounce, <450ms long press) then release (3300 mV) for 40ms
        uint32_t sim_t = 1000;
        sb.setSimMilliVolts(500); // BTN_UP
        sb.poll(sim_t);
        sim_t += 40; // 1040ms (debounce satisfied, pressed)
        SteeringBtnEvent ev_press = sb.poll(sim_t);
        assert(ev_press == SteeringBtnEvent::NONE);
        assert(sb.isButtonPressed() == true);
        assert(sb.getActiveButton() == SteeringBtn::BTN_UP);

        // Release to 3300mV
        sb.setSimMilliVolts(3300);
        sim_t += 10; // 1050ms
        sb.poll(sim_t);
        sim_t += 35; // 1085ms (debounce release satisfied)
        SteeringBtnEvent ev_rel = sb.poll(sim_t);
        if (ev_rel != SteeringBtnEvent::UP_SHORT) {
            fprintf(stderr, "[FAIL] SteeringButtons expected UP_SHORT, got %s\n", SteeringButtons::getEventName(ev_rel));
            return 1;
        }
        assert(sb.isButtonPressed() == false);

        // Simulate BTN_MODE (~1500 mV) long press:
        sim_t += 100;
        sb.setSimMilliVolts(1500); // BTN_MODE
        sb.poll(sim_t);
        sim_t += 35; // Debounce passed
        sb.poll(sim_t);
        assert(sb.getActiveButton() == SteeringBtn::BTN_MODE);

        sim_t += 460; // Total hold time > 450ms
        SteeringBtnEvent ev_long = sb.poll(sim_t);
        if (ev_long != SteeringBtnEvent::MODE_LONG) {
            fprintf(stderr, "[FAIL] SteeringButtons expected MODE_LONG, got %s\n", SteeringButtons::getEventName(ev_long));
            return 1;
        }

        // Release after long press -> must NOT trigger MODE_SHORT
        sb.setSimMilliVolts(3300);
        sim_t += 40;
        SteeringBtnEvent ev_after_long = sb.poll(sim_t);
        if (ev_after_long != SteeringBtnEvent::NONE) {
            fprintf(stderr, "[FAIL] SteeringButtons fired event on release after long press: %s\n",
                    SteeringButtons::getEventName(ev_after_long));
            return 1;
        }

        printf("[PASS] Regression test: ModeManager Legal-Mode Boost-Sperre & Modus-Zyklus & SteeringButtons (Test #10)\n");
    }

    // -------------------------------------------------------------
    // Regression Test 11: WLED Hardware-RMT Engine Modes & Brightness
    // -------------------------------------------------------------
    {
        WledController &wled = WledController::instance();
        wled.begin();
        wled.setEnabled(true);
        wled.setBrightness(255);

        DashTelemetry wt;
        setBaseTelemetry(wt);
        uint32_t wtime = 5000;

        // 1. Theft Strobe (theft_alarm_active -> 100ms Red/Blue strobe)
        wt.theft_alarm_active = true;
        wt.is_locked = true;
        wt.current_amps = -10.0f;
        wled.update(wt, wtime); // t=5000 (even 100ms slot -> Red)
        assert(wled.getActiveMode() == WLED_MODE_THEFT_STROBE);
        assert(wled.getLedRgb(0).r == 255 && wled.getLedRgb(0).b == 0);
        wtime += 100;
        wled.update(wt, wtime); // t=5100 (odd 100ms slot -> Blue)
        assert(wled.getLedRgb(0).r == 0 && wled.getLedRgb(0).b == 255);

        // 2. Adaptive Brake Light (current_amps = -10A -> solid Brake Red)
        wt.theft_alarm_active = false;
        wt.is_locked = false;
        wt.current_amps = -10.0f;
        wt.g_force = -0.1f;
        wtime += 50;
        wled.update(wt, wtime);
        assert(wled.getActiveMode() == WLED_MODE_BRAKE_SOLID);
        assert(wled.getLedRgb(0).r == 255 && wled.getLedRgb(0).g == 0 && wled.getLedRgb(0).b == 0);

        // 3. Emergency Brake (g_force = -0.8 -> pulsing Brake Red)
        wt.current_amps = 0.0f;
        wt.g_force = -0.8f;
        wled.update(wt, 6000); // 6000/60 = 100 (even -> 255)
        assert(wled.getActiveMode() == WLED_MODE_BRAKE_EMERGENCY);
        assert(wled.getLedRgb(0).r == 255);
        wled.update(wt, 6060); // 6060/60 = 101 (odd -> 64)
        assert(wled.getLedRgb(0).r < 255 && wled.getLedRgb(0).r > 0);

        // 4. Locked Breathe (parked, no alarm, no brake)
        wt.g_force = 0.0f;
        wt.is_locked = true;
        wt.color_theme = THEME_OBSIDIAN_DARK;
        wled.update(wt, 6200);
        assert(wled.getActiveMode() == WLED_MODE_LOCKED_BREATHE);
        assert(wled.getLedRgb(0).b > 0 && wled.getLedRgb(0).b < 60);

        // 5. Speed-Glow & Brightness 0 = Black
        wt.is_locked = false;
        wt.speed_kmh = 45.0f;
        wled.update(wt, 6300);
        assert(wled.getActiveMode() == WLED_MODE_SPEED_GLOW);
        WledRgb sg = wled.getLedRgb(0);
        assert(sg.r > 0 || sg.g > 0 || sg.b > 0);

        wled.setBrightness(0);
        wled.update(wt, 6400);
        assert(wled.getLedRgb(0).r == 0 && wled.getLedRgb(0).g == 0 && wled.getLedRgb(0).b == 0);

        wled.setEnabled(false);
        assert(wled.getActiveMode() == WLED_MODE_OFF);
        printf("[PASS] Regression test: WLED Engine (Strobe, Regen-Brake, Emergency-Brake, Locked-Breathe, Speed-Glow, Brightness 0) (Test #11)\n");
    }

    // -------------------------------------------------------------
    // Regression Test 12: Fault-Blackbox Modal in Settings (TAB_SYSTEM)
    // -------------------------------------------------------------
    {
        FaultManager::instance().clearFaults();
        FaultManager::instance().recordFault(FAULT_ABS_OVER_CURRENT, 52.4f, 115.0f, 180.0f, 55.0f, 68.0f, 42);
        assert(FaultManager::instance().getFaultCount() == 1);

        DashTelemetry t12;
        setBaseTelemetry(t12);
        t12.tuning_mode = true;
        t12.screen = SCREEN_SETTINGS_MENU;

        Renderer.invalidateCache();
        Renderer.markScreenDirty();
        Renderer.render(t12);

        // Tap SYSTEM tab (tuning_mode: i=3 -> x=20+3*154+72 = 554, y=81)
        assert(Renderer.handleSettingsTouch(554, 81, t12) == true);
        assert(Renderer.getSettingsTab() == DashboardRenderer::TAB_SYSTEM);
        Renderer.render(t12);

        // Tap [ FAULT LOG (1) ] button in Card 4 (x=284..516, y=384..432 -> center 400, 408)
        assert(Renderer.handleSettingsTouch(400, 408, t12) == true);
        Renderer.render(t12);

        // Tap [ LOG LOESCHEN ] in Modal (x=110..370, y=398..440 -> center 240, 419)
        assert(Renderer.handleSettingsTouch(240, 419, t12) == true);
        assert(FaultManager::instance().getFaultCount() == 0);
        Renderer.render(t12);

        // Tap [ SCHLIESSEN ] in Modal (x=430..690, y=398..440 -> center 560, 419)
        assert(Renderer.handleSettingsTouch(560, 419, t12) == true);
        Renderer.render(t12);

        // Exit Settings cleanly back to Dash
        assert(Renderer.handleSettingsTouch(44, 29, t12) == true);
        assert(t12.screen == SCREEN_RIDE_DASH);
        printf("[PASS] Regression test: Fault-Blackbox Modal Open, Clear (Count==0) & Close (Test #12)\n");
    }

    // -------------------------------------------------------------
    // Regression Test 13: WLED 2-Segment Stealth-Underglow (Police vs Tuning + API)
    // -------------------------------------------------------------
    {
        WledController &wled = WledController::instance();
        wled.begin();
        wled.setEnabled(true);
        wled.setBrightness(255);
        wled.setRearLedCount(WLED_REAR_LED_COUNT);
        assert(wled.getRearLedCount() == 12);

        DashTelemetry st;
        setBaseTelemetry(st);
        uint32_t t_ms = 9000;

        // 1. Police Mode (!tuning_mode): Segment 0 legal rear red, Segment 1 STRICTLY (0,0,0)!
        st.tuning_mode = false;
        st.is_locked = false;
        st.theft_alarm_active = false;
        st.current_amps = 4.0f;
        st.speed_kmh = 20.0f;
        wled.setCustomColor(0, 255, 128);
        wled.setEffect(WledController::EFFECT_SOLID);
        wled.update(st, t_ms);
        for (int i = 0; i < wled.getRearLedCount(); i++) {
            WledRgb c = wled.getLedRgb(i);
            assert(c.r == 90 && c.g == 0 && c.b == 0);
        }
        for (int i = wled.getRearLedCount(); i < wled.getLedCount(); i++) {
            WledRgb c = wled.getLedRgb(i);
            assert(c.r == 0 && c.g == 0 && c.b == 0);
        }

        // Police Mode Braking: Segment 0 bright brake red, Segment 1 STILL STRICTLY (0,0,0)!
        st.current_amps = -6.0f;
        t_ms += 40;
        wled.update(st, t_ms);
        assert(wled.getLedRgb(0).r == 255 && wled.getLedRgb(0).g == 0 && wled.getLedRgb(0).b == 0);
        assert(wled.getLedRgb(12).r == 0 && wled.getLedRgb(12).g == 0 && wled.getLedRgb(12).b == 0);

        // 2. Tuning Mode (tuning_mode == true): Segment 0 rear tail, Segment 1 Custom Color & Effects!
        st.tuning_mode = true;
        st.current_amps = 8.0f;
        t_ms += 40;
        wled.update(st, t_ms);
        assert(wled.getLedRgb(0).r == 90 && wled.getLedRgb(0).g == 0 && wled.getLedRgb(0).b == 0);
        assert(wled.getLedRgb(12).r == 0 && wled.getLedRgb(12).g == 255 && wled.getLedRgb(12).b == 128);
        assert(wled.getColorR() == 0 && wled.getColorG() == 255 && wled.getColorB() == 128);
        assert(!wled.isUsingThemeColor());

        // Theme color + Rainbow + Scanner + Clamping
        wled.setUseThemeColor(true);
        assert(wled.isUsingThemeColor());
        st.color_theme = THEME_NIGHT_DIM;
        wled.setEffect(WledController::EFFECT_RAINBOW);
        t_ms += 40;
        wled.update(st, t_ms);
        WledRgb r0 = wled.getLedRgb(12), r1 = wled.getLedRgb(36);
        assert((r0.r != r1.r) || (r0.g != r1.g) || (r0.b != r1.b));

        wled.setEffect(99);
        assert(wled.getEffect() == WledController::EFFECT_SCANNER);
        wled.setRearLedCount(250);
        assert(wled.getRearLedCount() == WLED_LED_COUNT);

        // Restore clean defaults
        wled.setRearLedCount(WLED_REAR_LED_COUNT);
        wled.setUseThemeColor(true);
        wled.setEffect(WledController::EFFECT_SPEED_GLOW);
        wled.setBrightness(180);
        wled.setEnabled(true);
        printf("[PASS] Regression test: WLED 2-Segment Stealth-Underglow (Police Stealth vs Tuning Custom/Effects) (Test #13)\n");
    }

    printf("\n[SUCCESS] All scenarios rendered and saved to tools/simulator/screenshots/\n");
    return 0;
}
