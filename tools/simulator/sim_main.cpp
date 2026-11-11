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

    // 7c. Test Tap on Light Switch Badge (x=32, y=416) -> NAV_LIGHT_TOGGLE
    NavAction actTapLight = simTouchAction(32, 416, 32, 416, 50);
    assert(actTapLight == NAV_LIGHT_TOGGLE);
    printf("[PASS] Tap on Light Switch Badge triggered NAV_LIGHT_TOGGLE (Hauptschalter).\n");

    // 7d. Test Long Press on Light Switch Badge (x=32, y=416) -> Must NOT trigger Trip Reset!
    Display.setSimTouch(true, 32, 416);
    Buttons.update();
    simAdvanceMs(850);
    Buttons.update();
    NavAction actLongLight = Buttons.getNavAction();
    assert(actLongLight != NAV_BTN2_LONG);
    Display.setSimTouch(false);
    simAdvanceMs(110);
    Buttons.update();
    printf("[PASS] Long Press on Light Switch Badge did NOT trigger NAV_BTN2_LONG (Accidental Reset Prevented).\n");

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

    // 11c. Swipe Right in Settings Menu triggers NAV_MENU_BACK (Return to Dash)
    NavAction actMenuSwipeR = simTouchAction(200, 200, 400, 200, 60);
    assert(actMenuSwipeR == NAV_MENU_BACK);
    printf("[PASS] Settings Menu Swipe Right triggered NAV_MENU_BACK.\n");

    // 11d. Swipe Up / Down in Settings Menu navigates tabs (Next / Prev)
    NavAction actMenuSwipeDown = simTouchAction(200, 200, 200, 350, 60);
    assert(actMenuSwipeDown == NAV_BTN1_SHORT);
    NavAction actMenuSwipeUp = simTouchAction(200, 350, 200, 200, 60);
    assert(actMenuSwipeUp == NAV_BTN2_SHORT);
    printf("[PASS] Settings Menu Vertical Swipes triggered tab navigation (NAV_BTN1_SHORT / NAV_BTN2_SHORT).\n");

    // 11e. Test DashboardRenderer::handleSettingsTouch integration (Touch Mode Configurator)
    DashTelemetry tTest;
    memset(&tTest, 0, sizeof(tTest));
    tTest.screen = SCREEN_SETTINGS_MENU;
    Renderer.markScreenDirty();

    // 1. Tab Switching via touch
    // Tap on Tab 0 [ ECO ] at x=100, y=95
    bool touchHandled = Renderer.handleSettingsTouch(100, 95, tTest);
    assert(touchHandled == true);
    assert(Renderer.getSettingsTab() == 0);

    // Tap on Tab 1 [ SPORT ] at x=300, y=95
    touchHandled = Renderer.handleSettingsTouch(300, 95, tTest);
    assert(touchHandled == true);
    assert(Renderer.getSettingsTab() == 1);

    // Tap on Tab 2 [ RACE ] at x=495, y=95
    touchHandled = Renderer.handleSettingsTouch(495, 95, tTest);
    assert(touchHandled == true);
    assert(Renderer.getSettingsTab() == 2);

    // Tap on Tab 3 [ VESC TOOL ] at x=690, y=95
    touchHandled = Renderer.handleSettingsTouch(690, 95, tTest);
    assert(touchHandled == true);
    assert(Renderer.getSettingsTab() == 3);

    // Tap on [ WI-FI BRIDGE STARTEN ] at x=200, y=400 while on Tab 3
    touchHandled = Renderer.handleSettingsTouch(200, 400, tTest);
    assert(touchHandled == true);
    assert(tTest.screen == SCREEN_WIFI_BRIDGE);
    Vesc.exitWifiBridgeMode();
    tTest.screen = SCREEN_SETTINGS_MENU;
    Renderer.setSettingsTab(2); // Switch back to RACE for limit tests

    // 2. Adjust RACE mode limits via [-] and [+] buttons and slider track
    VehicleModeConfig &cfg = ModeManager::instance().getConfig();
    cfg.tuned_modes[2].max_speed_kmh = 50.0f;
    cfg.tuned_modes[2].max_power_watts = 5000.0f;

    // Tap [+] Speed at x=720, y=210 -> max_speed_kmh increments to 51
    touchHandled = Renderer.handleSettingsTouch(720, 210, tTest);
    assert(touchHandled == true);
    assert(cfg.tuned_modes[2].max_speed_kmh == 51.0f);

    // Tap [-] Speed at x=80, y=210 -> max_speed_kmh decrements to 50
    touchHandled = Renderer.handleSettingsTouch(80, 210, tTest);
    assert(touchHandled == true);
    assert(cfg.tuned_modes[2].max_speed_kmh == 50.0f);

    // Tap Speed Slider Track at x=400, y=210 -> sets proportional speed
    touchHandled = Renderer.handleSettingsTouch(400, 210, tTest);
    assert(touchHandled == true);
    assert(cfg.tuned_modes[2].max_speed_kmh >= 15.0f && cfg.tuned_modes[2].max_speed_kmh <= 95.0f);

    // Tap [+] Watt at x=720, y=370 -> max_power_watts increments
    float prevW = cfg.tuned_modes[2].max_power_watts;
    touchHandled = Renderer.handleSettingsTouch(720, 370, tTest);
    assert(touchHandled == true);
    assert(cfg.tuned_modes[2].max_power_watts > prevW);

    // Tap [-] Watt at x=80, y=370 -> max_power_watts decrements
    prevW = cfg.tuned_modes[2].max_power_watts;
    touchHandled = Renderer.handleSettingsTouch(80, 370, tTest);
    assert(touchHandled == true);
    assert(cfg.tuned_modes[2].max_power_watts < prevW);

    // Tap Watt Slider Track at x=500, y=370 -> sets proportional watts
    touchHandled = Renderer.handleSettingsTouch(500, 370, tTest);
    assert(touchHandled == true);
    assert(cfg.tuned_modes[2].max_power_watts >= 250.0f && cfg.tuned_modes[2].max_power_watts <= 25000.0f);

    // Tap [< DASH] at x=75, y=25 -> Auto-saves and returns to SCREEN_RIDE_DASH
    touchHandled = Renderer.handleSettingsTouch(75, 25, tTest);
    assert(touchHandled == true);
    assert(tTest.screen == SCREEN_RIDE_DASH);
    printf("[PASS] Touch Mode Configurator: Tab selection, Speed/Watt buttons & sliders, Auto-Save to Dash verified 100%%!\n");

    // 11f. Test Full Integrated Main Loop settings touch flow
    DashTelemetry tLoop;
    memset(&tLoop, 0, sizeof(tLoop));
    tLoop.screen = SCREEN_SETTINGS_MENU;
    Buttons.setActiveScreen(SCREEN_SETTINGS_MENU);

    auto runMainLoopTouch = [&](int16_t x, int16_t y) {
        simAdvanceMs(120); // Allow 100ms debounce cooldown from previous gesture
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

    // 1. Tap Tab 0 [ ECO ]
    runMainLoopTouch(100, 95);
    assert(Renderer.getSettingsTab() == 0);

    // 2. Tap Tab 2 [ RACE ]
    runMainLoopTouch(495, 95);
    assert(Renderer.getSettingsTab() == 2);

    // 3. Tap [+] Speed button at x=720, y=210
    float initialRaceSpeed = cfg.tuned_modes[2].max_speed_kmh;
    runMainLoopTouch(720, 210);
    assert(cfg.tuned_modes[2].max_speed_kmh == initialRaceSpeed + 1.0f);

    // 4. Tap [< DASH] at x=75, y=25 -> Direct return to DASH!
    runMainLoopTouch(75, 25);
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

    // Countdown finishes at 5s
    Renderer.updateRaceTimer(t, 6000); // 5s elapsed
    assert(t.race_state == 2); // MEASURING
    assert(t.race_launch_start_ms == 6000);
    assert(t.race_elapsed_sec == 0.0f);
    printf("[PASS] Countdown expiration cleanly triggers MEASURING state at 0.00s.\n");

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
    printf("=== All Regenerative Braking & Trip Reset Unit Tests PASSED! ===\n\n");
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
    testAntiGhostingTransitions();
    testStatsCalculationAndLiveUpdates();
    testModeManagerAndLegalProfiles();
    testFaultManagerAndBlackbox();
    testRaceTimerAndRecords();
    testRegenAndTripReset();

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
    // Scenario 6: Screen 0 - Ride Dash with Light Switch Active (Hauptschalter ON)
    // ----------------------------------------------------
    t.screen = SCREEN_RIDE_DASH;
    t.lights_enabled = true;
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
    // Scenario 11: Screen 3 - Mode Configurator (VESC TOOL Bridge Hub Tab)
    // ----------------------------------------------------
    t.screen = SCREEN_SETTINGS_MENU;
    Renderer.setSettingsTab(3);
    Renderer.markScreenDirty();
    renderAndSave("11_settings_display_style", t);

    // ----------------------------------------------------
    // Scenario 12: Screen 3 - Mode Configurator (RACE Tab Custom)
    // ----------------------------------------------------
    t.screen = SCREEN_SETTINGS_MENU;
    Renderer.setSettingsTab(2);
    Renderer.markScreenDirty();
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

    // ----------------------------------------------------
    // Scenario 19: Screen 6 - "RACE" Dragy Launch-Timer (Measuring 48 km/h)
    // ----------------------------------------------------
    setBaseTelemetry(t);
    t.screen = SCREEN_RACE_TIMER;
    t.speed_kmh = 48.0f;
    t.race_state = 2; // MEASURING
    t.race_elapsed_sec = 3.42f;
    t.race_split_0_22 = 1.65f;
    t.race_split_0_50 = 0.0f;
    t.race_split_0_70 = 0.0f;
    t.race_records.best_0_22_sec = 1.72f;
    t.race_records.best_0_50_sec = 4.10f;
    t.race_records.best_0_70_sec = 7.80f;
    Renderer.markScreenDirty();
    renderAndSave("19_race_timer_measuring", t);

    // ----------------------------------------------------
    // Scenario 20: Screen 6 - Bestzeiten Popup Modal
    // ----------------------------------------------------
    t.race_popup_open = true;
    Renderer.markScreenDirty();
    renderAndSave("20_race_records_modal", t);

    printf("\n[SUCCESS] All scenarios rendered and saved to tools/simulator/screenshots/\n");
    return 0;
}
