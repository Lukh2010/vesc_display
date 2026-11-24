#include "serial_cli.h"
#include <Arduino.h>
#include <string.h>
#include <strings.h>
#include <stdlib.h>
#include "board_config.h"
#include "dash_renderer.h"
#include "settings_manager.h"
#include "vesc_uart.h"
#include "mode_manager.h"
#include "buttons.h"
#include "dash_log.h"

void handleSerialCli(DashTelemetry &telemetry, uint32_t now) {
    // Interactive Serial Command Console via USB (115200 Baud)
    // Active both in SIMULATION_MODE and on real hardware (when USB bridge is inactive)
    static uint32_t s_test_gas_start_ms = 0;
    static uint32_t s_test_gas_duration_ms = 0;
    static bool s_test_gas_active = false;
    static float s_test_gas_current = 0.0f;
    static float s_test_gas_duty = 0.0f;
    static bool s_test_gas_is_duty = false;
    static uint32_t s_last_test_gas_stream_ms = 0;
    static uint32_t s_last_test_telem_log_ms = 0;
    static char s_cmd_line[48] = {0};
    static size_t s_cmd_len = 0;

    // Continuous drive stream while test gas active (every 30ms = ~33Hz refresh for VESC watchdog)
    if (s_test_gas_active) {
        if ((uint32_t)(now - s_test_gas_start_ms) < s_test_gas_duration_ms) {
            if (now - s_last_test_gas_stream_ms >= 30) {
                s_last_test_gas_stream_ms = now;
                if (s_test_gas_is_duty) {
                    Vesc.sendDuty(s_test_gas_duty);
                } else {
                    Vesc.sendCurrent(s_test_gas_current);
                }
            }
            if (now - s_last_test_telem_log_ms >= 200) {
                s_last_test_telem_log_ms = now;
                DASH_LOGF("[TEST TELEM] Spd: %.1f km/h | ERPM: %.0f | MotI: %.1fA | BatI: %.1fA | Duty: %.1f%%\n",
                          telemetry.vesc_motor_speed, telemetry.rpm * 15.0f, telemetry.phase_amps, telemetry.current_amps, telemetry.duty_cycle_pct);
            }
        } else {
            // Auto-stop safety timeout expired
            s_test_gas_active = false;
            s_test_gas_start_ms = 0;
            s_test_gas_duration_ms = 0;
            s_test_gas_current = 0.0f;
            s_test_gas_duty = 0.0f;
            s_test_gas_is_duty = false;
            Vesc.sendCurrent(0.0f);
            Vesc.sendDuty(0.0f);
            Vesc.setSimThrottle(0.0f);
            Renderer.markScreenDirty();
            Serial.println("[GAS SAFETY] Test-Gas Timer abgelaufen -> Motor 0.0A gestoppt.");
            Serial.printf("[FINAL TELEM] Spd: %.1f km/h | ERPM: %.0f | MotI: %.1fA | BatI: %.1fA\n",
                          telemetry.vesc_motor_speed, telemetry.rpm * 15.0f, telemetry.phase_amps, telemetry.current_amps);
        }
    }

    if (!Vesc.isUsbBridgeActive()) {
        while (Serial.available()) {
            char c = (char)Serial.read();

            // 1. PIN Digit Direct Entry while Locked
            if (telemetry.is_locked && (c >= '0' && c <= '9')) {
                int len = strlen(telemetry.pin_entry);
                if (len < 4) {
                    telemetry.pin_entry[len] = c;
                    telemetry.pin_entry[len + 1] = '\0';
                    telemetry.pin_error = false;
                    len++;
                    Renderer.markScreenDirty();
                    Serial.printf("[SERIAL PIN] Ziffer '%c' eingetippt -> Aktuell: '%s' (%d/4)\n", c, telemetry.pin_entry, len);

                    if (len == 4) {
                        if (strcmp(telemetry.pin_entry, Settings.get().pin_code) == 0) {
                            telemetry.is_locked = false;
                            telemetry.theft_alarm_active = false;
                            telemetry.locked_moved_meters = 0.0f;
                            telemetry.pin_error = false;
                            telemetry.pin_entry[0] = '\0';
                            ModeManager::instance().setLocked(false);
                            Vesc.sendFocPlayStop();
                            Vesc.sendCurrentBrake(0.0f);
                            Renderer.markScreenDirty();
                            Renderer.invalidateCache();
                            Renderer.triggerNeedleSweep();
                            Serial.println("[PIN] System ERFOLGREICH ENTSPERRT via Serial PIN-Eingabe! Alarm deaktiviert.");
                        } else {
                            telemetry.pin_error = true;
                            telemetry.pin_error_start_ms = millis();
                            telemetry.pin_error_end_ms = telemetry.pin_error_start_ms + 800;
                            Renderer.markScreenDirty();
                            Serial.printf("[PIN] Falscher PIN-Code '%s' eingegeben! (Erwartet: '%s')\n",
                                          telemetry.pin_entry, Settings.get().pin_code);
                        }
                    }
                }
            } else if (telemetry.is_locked && (c == '<' || c == '\b' || c == 127)) {
                int len = strlen(telemetry.pin_entry);
                if (len > 0) {
                    telemetry.pin_entry[len - 1] = '\0';
                    telemetry.pin_error = false;
                    Renderer.markScreenDirty();
                    Serial.printf("[SERIAL PIN] Ziffer geloescht -> Aktuell: '%s'\n", telemetry.pin_entry);
                }
            }

            // 2. Immediate Emergency Stop / Gas Shortcuts (only non-letter keys when line buffer is empty)
            if (s_cmd_len == 0) {
                if (c == '+') {
                    if (telemetry.is_locked) {
                        Serial.println("[WARNUNG] System ist GESPERRT! Erst PIN eingeben (z. B. '1234') vor Gas geben.");
                    } else {
                        float test_amps = 5.0f;
                        s_test_gas_current = test_amps;
                        s_test_gas_duty = 0.0f;
                        s_test_gas_is_duty = false;
                        s_test_gas_active = true;
                        s_test_gas_start_ms = millis();
                        s_test_gas_duration_ms = 1500; // Auto-cutoff 1.5s
                        Vesc.sendCurrent(test_amps);
                        Vesc.setSimThrottle(25.0f);
                        Renderer.markScreenDirty();
                        Serial.printf("[TEST GAS] Motor-Strom %.1fA gesendet (Auto-Cutoff in 1.5s)!\n", test_amps);
                    }
                } else if (c == ' ' || c == '-') {
                    s_test_gas_active = false;
                    s_test_gas_start_ms = 0;
                    s_test_gas_duration_ms = 0;
                    s_test_gas_current = 0.0f;
                    s_test_gas_duty = 0.0f;
                    s_test_gas_is_duty = false;
                    Vesc.sendCurrent(0.0f);
                    Vesc.sendDuty(0.0f);
                    Vesc.sendCurrentBrake(0.0f);
                    Vesc.setSimThrottle(0.0f);
                    Renderer.markScreenDirty();
                    Serial.println("[TEST STOP] Motor sofort gestoppt (0.0A Freilauf).");
                }
            }

            // 3. Line accumulator for text & single-letter commands (e.g. 'unlock', 'reset', 'help', 'w', 'p', 'r', 'h')
            if (c == '\r' || c == '\n') {
                while (s_cmd_len > 0 && (s_cmd_line[s_cmd_len - 1] == ' ' || s_cmd_line[s_cmd_len - 1] == '\t')) {
                    s_cmd_line[--s_cmd_len] = '\0';
                }
                char *cmd = s_cmd_line;
                while (*cmd == ' ' || *cmd == '\t') cmd++;
                if (*cmd != '\0') {
                    if (strcasecmp(cmd, "w") == 0) {
                        if (telemetry.is_locked) {
                            Serial.println("[WARNUNG] System ist GESPERRT! Erst PIN eingeben (z. B. '1234') vor Gas geben.");
                        } else {
                            float test_amps = 5.0f;
                            s_test_gas_current = test_amps;
                            s_test_gas_duty = 0.0f;
                            s_test_gas_is_duty = false;
                            s_test_gas_active = true;
                            s_test_gas_start_ms = millis();
                            s_test_gas_duration_ms = 1500;
                            Vesc.sendCurrent(test_amps);
                            Vesc.setSimThrottle(25.0f);
                            Renderer.markScreenDirty();
                            Serial.printf("[TEST GAS] Motor-Strom %.1fA gesendet (Auto-Cutoff in 1.5s)!\n", test_amps);
                        }
                    } else if (strcasecmp(cmd, "p") == 0) {
                        if (!telemetry.is_locked && (telemetry.speed_kmh > 0.8f || telemetry.vesc_motor_speed > 0.8f)) {
                            Serial.println("[SAFETY] Locking rejected: vehicle is moving!");
                        } else {
                            telemetry.is_locked = !telemetry.is_locked;
                            ModeManager::instance().setLocked(telemetry.is_locked);
                            telemetry.theft_alarm_active = false;
                            Renderer.markScreenDirty();
                            Renderer.invalidateCache();
                            if (!telemetry.is_locked) {
                                Vesc.sendFocPlayStop();
                                Vesc.sendCurrentBrake(0.0f);
                                Renderer.triggerNeedleSweep();
                            }
                            Serial.printf("[SERIAL] PIN-Lockscreen: %s\n", telemetry.is_locked ? "GESPERRT" : "ENTSPERRT");
                        }
                    } else if (strcasecmp(cmd, "r") == 0) {
                        strncpy(Settings.get().pin_code, "1234", sizeof(Settings.get().pin_code));
                        Settings.save();
                        telemetry.is_locked = false;
                        telemetry.pin_error = false;
                        telemetry.pin_entry[0] = '\0';
                        ModeManager::instance().setLocked(false);
                        Vesc.sendFocPlayStop();
                        Vesc.sendCurrentBrake(0.0f);
                        Renderer.markScreenDirty();
                        Renderer.invalidateCache();
                        Renderer.triggerNeedleSweep();
                        Serial.println("[SERIAL] PIN auf '1234' resetet & System entsperrt!");
                    } else if (strcasecmp(cmd, "h") == 0 || strcmp(cmd, "?") == 0 || strcasecmp(cmd, "help") == 0) {
                        Serial.println("=== DISPLAY SERIAL COMMANDS ===");
                        Serial.println("  0-9       : PIN Ziffer eintippen (1234 zum Entsperren)");
                        Serial.println("  < / bksp  : PIN Ziffer loeschen");
                        Serial.println("  w / +     : Test-Gas geben (5.0A, 1.5s Auto-Cutoff)");
                        Serial.println("  gas <A>   : Test-Gas mit Amps geben (z.B. 'gas 3.5')");
                        Serial.println("  duty <% > : Test-Duty cycle geben (z.B. 'duty 5' = 5%)");
                        Serial.println("  s / space : Motor stoppen (0.0A / 0% Duty)");
                        Serial.println("  p / lock  : Sperren / Entsperren toggeln");
                        Serial.println("  r         : PIN auf 1234 resetten & entsperren");
                        Serial.println("  status    : System Telemetrie anzeigen");
                        Serial.println("  theme     : Farb-Theme umschalten");
                        Serial.println("  unlock    : Direkt per Textbefehl entsperren");
                        Serial.println("===============================");
                    } else if (strcasecmp(cmd, "unlock") == 0 || strcmp(cmd, Settings.get().pin_code) == 0) {
                        telemetry.is_locked = false;
                        telemetry.theft_alarm_active = false;
                        telemetry.locked_moved_meters = 0.0f;
                        telemetry.pin_error = false;
                        telemetry.pin_entry[0] = '\0';
                        ModeManager::instance().setLocked(false);
                        Vesc.sendFocPlayStop();
                        Vesc.sendCurrentBrake(0.0f);
                        Renderer.markScreenDirty();
                        Renderer.invalidateCache();
                        Renderer.triggerNeedleSweep();
                        Serial.println("[PIN] System UNLOCKED via Serial Command! Alarm disarmed.");
                    } else if (strcasecmp(cmd, "lock") == 0) {
                        if (telemetry.speed_kmh > 0.8f || telemetry.vesc_motor_speed > 0.8f) {
                            Serial.println("[SAFETY] Locking rejected: vehicle is moving!");
                        } else {
                            telemetry.is_locked = true;
                            ModeManager::instance().setLocked(true);
                            Renderer.markScreenDirty();
                            Renderer.invalidateCache();
                            Serial.println("[PIN] System LOCKED via Serial Command.");
                        }
                    } else if (strcasecmp(cmd, "tuning on") == 0 || strcasecmp(cmd, "tuning") == 0) {
                        telemetry.tuning_mode = true;
                        ModeManager::instance().setTuningMode(true);
                        Buttons.setTuningMode(true);
                        Renderer.markScreenDirty();
                        Renderer.invalidateCache();
                        Serial.println("[MODE] TUNING-MODUS AKTIVIERT (Offener Modus, Drossel aus)!");
                    } else if (strcasecmp(cmd, "tuning off") == 0 || strcasecmp(cmd, "legal") == 0) {
                        telemetry.tuning_mode = false;
                        ModeManager::instance().setTuningMode(false);
                        Buttons.setTuningMode(false);
                        Renderer.markScreenDirty();
                        Renderer.invalidateCache();
                        Serial.println("[MODE] LEGALER POLIZEI-MODUS AKTIVIERT (20 km/h gedrosselt)!");
                    } else if (strcasecmp(cmd, "mode eco") == 0) {
                        ModeManager::instance().setRideMode(MODE_ECO);
                        Serial.println("[MODE] Ride-Mode -> ECO");
                    } else if (strcasecmp(cmd, "mode sport") == 0) {
                        ModeManager::instance().setRideMode(MODE_SPORT);
                        Serial.println("[MODE] Ride-Mode -> SPORT");
                    } else if (strcasecmp(cmd, "mode race") == 0) {
                        ModeManager::instance().setRideMode(MODE_RACE);
                        Serial.println("[MODE] Ride-Mode -> RACE");
                    } else if (strcasecmp(cmd, "vollgas") == 0 || strcasecmp(cmd, "max") == 0) {
                        if (telemetry.is_locked) {
                            Serial.println("[WARNUNG] System ist GESPERRT! Erst PIN eingeben.");
                        } else {
                            ModeLimits lim = ModeManager::instance().getActiveLimits();
                            float mode_phase = lim.max_phase_amps;
                            if (mode_phase > 60.0f) mode_phase = 60.0f; // safety max for bench
                            s_test_gas_current = mode_phase;
                            s_test_gas_duty = 0.0f;
                            s_test_gas_is_duty = false;
                            s_test_gas_active = true;
                            s_test_gas_start_ms = millis();
                            s_test_gas_duration_ms = 3000; // 3.0s full throttle
                            Vesc.sendCurrent(mode_phase);
                            Vesc.setSimThrottle(100.0f);
                            Renderer.markScreenDirty();
                            Serial.printf("[VOLLGAS] Sende %.1fA Phasenstrom (3.0s Vollgas im Modus %s, Tuning=%s)!\n",
                                          mode_phase, ModeManager::instance().getActiveModeName(),
                                          telemetry.tuning_mode ? "AN" : "AUS");
                        }
                    } else if (strncasecmp(cmd, "gas ", 4) == 0 || strncasecmp(cmd, "cur ", 4) == 0) {
                        if (telemetry.is_locked) {
                            Serial.println("[WARNUNG] System ist GESPERRT! Erst PIN eingeben.");
                        } else {
                            const char *param = cmd + 4;
                            while (*param == ' ') param++;
                            char *endptr = nullptr;
                            float amps = strtof(param, &endptr);
                            float secs = 1.5f;
                            if (endptr && *endptr == ' ') {
                                while (*endptr == ' ') endptr++;
                                secs = strtof(endptr, nullptr);
                                if (secs > 5.0f) secs = 5.0f;
                                if (secs < 0.2f) secs = 0.2f;
                            }
                            if (amps > 60.0f) amps = 60.0f;
                            if (amps < 0.0f) amps = 0.0f;
                            if (amps > 0.0f) {
                                s_test_gas_current = amps;
                                s_test_gas_duty = 0.0f;
                                s_test_gas_is_duty = false;
                                s_test_gas_active = true;
                                s_test_gas_start_ms = millis();
                                s_test_gas_duration_ms = (uint32_t)(secs * 1000.0f);
                                Vesc.sendCurrent(amps);
                                Vesc.setSimThrottle(amps * 2.0f);
                                Serial.printf("[TEST GAS] Motor-Strom %.1fA fuer %.1fs gesendet!\n", amps, secs);
                            } else {
                                s_test_gas_active = false;
                                s_test_gas_start_ms = 0;
                                s_test_gas_duration_ms = 0;
                                s_test_gas_current = 0.0f;
                                s_test_gas_duty = 0.0f;
                                s_test_gas_is_duty = false;
                                Vesc.sendCurrent(0.0f);
                                Vesc.sendDuty(0.0f);
                                Vesc.setSimThrottle(0.0f);
                                Serial.println("[TEST GAS] Motor 0.0A gestoppt.");
                            }
                        }
                    } else if (strncasecmp(cmd, "duty ", 5) == 0) {
                        if (telemetry.is_locked) {
                            Serial.println("[WARNUNG] System ist GESPERRT! Erst PIN eingeben.");
                        } else {
                            const char *param = cmd + 5;
                            while (*param == ' ') param++;
                            char *endptr = nullptr;
                            float duty_pct = strtof(param, &endptr);
                            float secs = 1.5f;
                            if (endptr && *endptr == ' ') {
                                while (*endptr == ' ') endptr++;
                                secs = strtof(endptr, nullptr);
                                if (secs > 5.0f) secs = 5.0f;
                                if (secs < 0.2f) secs = 0.2f;
                            }
                            float max_duty = telemetry.tuning_mode ? 85.0f : 20.0f;
                            if (duty_pct > max_duty) duty_pct = max_duty;
                            if (duty_pct < 0.0f) duty_pct = 0.0f;
                            float duty_frac = duty_pct / 100.0f;
                            if (duty_frac > 0.0f) {
                                s_test_gas_duty = duty_frac;
                                s_test_gas_current = 0.0f;
                                s_test_gas_is_duty = true;
                                s_test_gas_active = true;
                                s_test_gas_start_ms = millis();
                                s_test_gas_duration_ms = (uint32_t)(secs * 1000.0f);
                                Vesc.sendDuty(duty_frac);
                                Vesc.setSimThrottle(duty_pct);
                                Serial.printf("[TEST DUTY] Motor Duty %.1f%% (%.3f) fuer %.1fs gesendet!\n", duty_pct, duty_frac, secs);
                            } else {
                                s_test_gas_active = false;
                                s_test_gas_start_ms = 0;
                                s_test_gas_duration_ms = 0;
                                s_test_gas_current = 0.0f;
                                s_test_gas_duty = 0.0f;
                                s_test_gas_is_duty = false;
                                Vesc.sendCurrent(0.0f);
                                Vesc.sendDuty(0.0f);
                                Vesc.setSimThrottle(0.0f);
                                Serial.println("[TEST DUTY] Motor 0% gestoppt.");
                            }
                        }
                    } else if (strcasecmp(cmd, "stop") == 0 || strcasecmp(cmd, "s") == 0) {
                        s_test_gas_active = false;
                        s_test_gas_start_ms = 0;
                        s_test_gas_duration_ms = 0;
                        s_test_gas_current = 0.0f;
                        s_test_gas_duty = 0.0f;
                        s_test_gas_is_duty = false;
                        Vesc.sendCurrent(0.0f);
                        Vesc.sendDuty(0.0f);
                        Vesc.sendCurrentBrake(0.0f);
                        Vesc.setSimThrottle(0.0f);
                        Renderer.markScreenDirty();
                        Serial.println("[TEST STOP] Motor gestoppt.");
                    } else if (strcasecmp(cmd, "status") == 0 || strcasecmp(cmd, "i") == 0) {
                        Serial.println("=== SYSTEM STATUS ===");
                        Serial.printf("  Lock: %s | PIN-Code: '%s'\n", telemetry.is_locked ? "GESPERRT" : "ENTSPERRT", Settings.get().pin_code);
                        Serial.printf("  Alarm: %s | Moved: %.2fm\n", telemetry.theft_alarm_active ? "AKTIV" : "INAKTIV", telemetry.locked_moved_meters);
                        Serial.printf("  VESC: %s | Spd: %.1f km/h | RPM: %.0f | MotI: %.1fA | BatI: %.1fA | Vin: %.1fV\n",
                                      telemetry.vesc_connected ? "VERBUNDEN" : "GETRENNT",
                                      telemetry.vesc_motor_speed, telemetry.rpm, telemetry.phase_amps, telemetry.current_amps, telemetry.voltage);
                        Serial.printf("  Mode: %s | Theme: %s\n", ModeManager::instance().getActiveModeName(), DashboardRenderer::getThemePalette(telemetry.color_theme).name);
                        Serial.println("=====================");
                    } else if (strcasecmp(cmd, "theme") == 0) {
                        uint8_t th = (Settings.get().color_theme + 1) % THEME_COUNT;
                        Settings.get().color_theme = th;
                        Settings.save();
                        telemetry.color_theme = th;
                        Renderer.invalidateCache();
                        Renderer.markScreenDirty();
                        Serial.printf("[SERIAL] Farb-Theme gewechselt auf: %s\n", DashboardRenderer::getThemePalette(th).name);
                    } else if (strcasecmp(cmd, "help") == 0) {
                        Serial.println("Befehle: unlock, lock, gas <A>, duty <%>, stop, status, theme, help");
                    }
                }
                s_cmd_line[0] = '\0';
                s_cmd_len = 0;
            } else if (c >= 32 && c <= 126) {
                if (s_cmd_len < sizeof(s_cmd_line) - 1) {
                    s_cmd_line[s_cmd_len++] = c;
                    s_cmd_line[s_cmd_len] = '\0';
                }
            }
        }
    }
}
