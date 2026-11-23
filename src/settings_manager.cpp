#include "settings_manager.h"
#include <math.h>
#include <string.h>

SettingsManager Settings;

SettingsManager::SettingsManager()
    : _has_saved_snapshot(false) {
    loadDefaults();
    _last_saved = _settings;
    _last_saved_race = _race_records;
}

void SettingsManager::loadDefaults() {
    _settings.dash_style             = STYLE_HYPER_35KW;
    _settings.brightness_pct         = 90;
    _settings.max_battery_amps       = 25;
    _settings.max_phase_amps         = 45;
    _settings.field_weak_amps        = 10;
    _settings.max_speed_kmh          = 0; // Unlimited
    _settings.throttle_ramp_sec      = 0.2f;
    _settings.wheel_diameter_in      = 11.0f;
    _settings.wheel_gps_calibrated   = false;
    _settings.motor_pole_pairs       = 15;
    _settings.gear_ratio             = 1.0f;
    _settings.active_battery_profile = 0;
    _settings.voltage_trim_v         = -0.3f;
    _settings.throttle_adc_channel   = 0; // Default to ADC1 (standard VESC throttle)
    _settings.pin_lock_enabled       = true; // Handystyle 4-digit PIN lock on startup
    strncpy(_settings.pin_code, "1234", sizeof(_settings.pin_code));
    _settings.pin_code[sizeof(_settings.pin_code) - 1] = '\0';
    _settings.race_split1_kmh        = 22;
    _settings.race_split2_kmh        = 50;
    _settings.race_split3_kmh        = 70;
    _settings.gps_enabled            = true;
    _settings.incline_gforce_enabled = true;
    _settings.wifi_bridge_enabled    = false;
    _settings.setup_completed        = false;
    _settings.color_theme            = THEME_OBSIDIAN_DARK;
    _settings.turbine_sound_enabled  = false;
    _settings.ble_keyless_enabled    = true;
    _settings.ble_autolock_enabled   = true;
    _settings.ble_paired             = false;
    _settings.ble_paired_addr[0]     = '\0';
    strncpy(_settings.ble_paired_name, "Smartphone", sizeof(_settings.ble_paired_name));
    _settings.ble_paired_name[sizeof(_settings.ble_paired_name) - 1] = '\0';
    _settings.show_gforce            = 1;
    _settings.show_incline           = 1;
    _settings.show_light_btn         = 1;
    _settings.brake_mode             = 2;
    _settings.odo_km                 = 0.0f;
    _settings.tcs_enabled            = false;
    _settings.battery_cells          = 13;
    _race_records.reset();
}

void SettingsManager::begin() {
    _prefs.begin("ebike_cfg", false);

    if (!_prefs.isKey("cfg_init")) {
        // First boot: save initial defaults
        loadDefaults();
        saveInternal(true);
        _prefs.putBool("cfg_init", true);
        _prefs.putBool("adc_v2_fixed", true);
        Serial.println("[SETTINGS] Initialized default NVS configuration.");
    } else {
        _settings.dash_style             = _prefs.getUChar("style", STYLE_HYPER_35KW);
        if (_settings.dash_style >= STYLE_COUNT) _settings.dash_style = STYLE_HYPER_35KW;
        _settings.brightness_pct         = _prefs.getUChar("bright", 90);
        _settings.max_battery_amps       = _prefs.getUShort("bat_a", 25);
        _settings.max_phase_amps         = _prefs.getUShort("pha_a", 45);
        _settings.field_weak_amps        = _prefs.getUChar("fw_a", 10);
        _settings.max_speed_kmh          = _prefs.getUChar("spd_lim", 0);
        _settings.throttle_ramp_sec      = _prefs.getFloat("t_ramp", 0.2f);
        _settings.wheel_diameter_in      = _prefs.getFloat("wheel", 11.0f);
        _settings.wheel_gps_calibrated   = _prefs.getBool("wheel_gps", false);
        if (_settings.wheel_diameter_in > 18.0f) {
            // Auto-migrate legacy bicycle default (26.0") to Segway G3 Max 11.0"
            _settings.wheel_diameter_in = 11.0f;
            _prefs.putFloat("wheel", 11.0f);
        }
        _settings.motor_pole_pairs       = _prefs.getUChar("poles", 15);
        _settings.gear_ratio             = _prefs.getFloat("gear", 1.0f);
        _settings.active_battery_profile = _prefs.getUChar("bat_prof", 0);
        _settings.voltage_trim_v         = _prefs.getFloat("v_trim", -0.3f);
        _settings.throttle_adc_channel   = _prefs.getUChar("adc_ch", 0);
        _settings.pin_lock_enabled       = _prefs.getBool("pin_en", true);
        _prefs.getString("pin_code", _settings.pin_code, sizeof(_settings.pin_code));
        if (strlen(_settings.pin_code) != 4) {
            strncpy(_settings.pin_code, "1234", sizeof(_settings.pin_code));
            _settings.pin_code[sizeof(_settings.pin_code) - 1] = '\0';
        }

        _settings.race_split1_kmh        = _prefs.getUChar("rc_sp1", 22);
        _settings.race_split2_kmh        = _prefs.getUChar("rc_sp2", 50);
        _settings.race_split3_kmh        = _prefs.getUChar("rc_sp3", 70);
        if (_settings.race_split1_kmh < 5 || _settings.race_split1_kmh > 60) _settings.race_split1_kmh = 22;
        if (_settings.race_split2_kmh < 15 || _settings.race_split2_kmh > 90) _settings.race_split2_kmh = 50;
        if (_settings.race_split3_kmh < 25 || _settings.race_split3_kmh > 130) _settings.race_split3_kmh = 70;

        _settings.gps_enabled            = _prefs.getBool("gps_en", true);
        _settings.incline_gforce_enabled = _prefs.getBool("inc_en", true);
        _settings.show_gforce            = _prefs.getUChar("show_gf", 1);
        if (_settings.show_gforce > 1) _settings.show_gforce = 1;
        _settings.show_incline           = _prefs.getUChar("show_inc", 1);
        if (_settings.show_incline > 1) _settings.show_incline = 1;
        _settings.show_light_btn         = _prefs.getUChar("show_lb", 1);
        if (_settings.show_light_btn > 1) _settings.show_light_btn = 1;
        _settings.brake_mode             = _prefs.getUChar("brk_mode", 2);
        if (_settings.brake_mode > 2) _settings.brake_mode = 2;
        _settings.wifi_bridge_enabled    = _prefs.getBool("wifi_en", false);
        _settings.setup_completed        = _prefs.getBool("setup_done", false);

        // Migrate any legacy color_theme >= THEME_COUNT cleanly to THEME_OBSIDIAN_DARK
        uint8_t raw_theme = _prefs.getUChar("theme", THEME_OBSIDIAN_DARK);
        if (raw_theme >= THEME_COUNT) {
            _settings.color_theme = THEME_OBSIDIAN_DARK;
            _prefs.putUChar("theme", THEME_OBSIDIAN_DARK);
        } else {
            _settings.color_theme = raw_theme;
        }

        _settings.turbine_sound_enabled  = _prefs.getBool("turb_en", false);
        _settings.ble_keyless_enabled    = _prefs.getBool("ble_en", true);
        _settings.ble_autolock_enabled   = _prefs.getBool("ble_alock", true);
        _settings.ble_paired             = _prefs.getBool("ble_pair", false);
        _prefs.getString("ble_addr", _settings.ble_paired_addr, sizeof(_settings.ble_paired_addr));
        _prefs.getString("ble_name", _settings.ble_paired_name, sizeof(_settings.ble_paired_name));
        if (strlen(_settings.ble_paired_name) == 0) {
            strncpy(_settings.ble_paired_name, "Smartphone", sizeof(_settings.ble_paired_name));
            _settings.ble_paired_name[sizeof(_settings.ble_paired_name) - 1] = '\0';
        }

        _settings.odo_km = _prefs.getFloat("odo_km", 0.0f);
        if (isnan(_settings.odo_km) || _settings.odo_km < 0.0f || _settings.odo_km > 999999.0f) {
            _settings.odo_km = 0.0f;
        }
        _settings.tcs_enabled = _prefs.getBool("tcs_en", false);
        _settings.battery_cells = _prefs.getUChar("bat_cells", 13);
        if (_settings.battery_cells < 10 || _settings.battery_cells > 24) {
            _settings.battery_cells = 13;
            _prefs.putUChar("bat_cells", 13);
        }

        _race_records.best_0_22_sec = _prefs.getFloat("rc_best22", 0.0f);
        _race_records.best_0_50_sec = _prefs.getFloat("rc_best50", 0.0f);
        _race_records.best_0_70_sec = _prefs.getFloat("rc_best70", 0.0f);

        // One-time automatic migration: fix boards that had invalid adc_ch or keep existing valid channel
        if (!_prefs.getBool("adc_v2_fixed", false)) {
            if (_settings.throttle_adc_channel > 7) {
                _settings.throttle_adc_channel = 0; // Force default to ADC1
                _prefs.putUChar("adc_ch", 0);
                Serial.println("[SETTINGS] Fixed out-of-range throttle ADC channel to 0 (standard).");
            } else {
                Serial.printf("[SETTINGS] Preserved existing throttle ADC channel %u during migration.\n",
                              _settings.throttle_adc_channel);
            }
            _prefs.putBool("adc_v2_fixed", true);
        }

        _last_saved = _settings;
        _last_saved_race = _race_records;
        _has_saved_snapshot = true;

        Serial.println("[SETTINGS] Loaded configuration from NVS flash.");
    }
}

static inline bool floatChanged(float a, float b) {
    if (isnan(a) && isnan(b)) return false;
    if (isnan(a) || isnan(b)) return true;
    return fabsf(a - b) > 1e-5f;
}

void SettingsManager::saveInternal(bool force) {
    if (!_has_saved_snapshot) {
        force = true;
    }

    // Sanity clamp critical fields before persistence
    if (_settings.color_theme >= THEME_COUNT) {
        _settings.color_theme = THEME_OBSIDIAN_DARK;
    }
    if (_settings.battery_cells < 10) _settings.battery_cells = 10;
    if (_settings.battery_cells > 24) _settings.battery_cells = 24;
    if (isnan(_settings.odo_km) || _settings.odo_km < 0.0f) _settings.odo_km = 0.0f;

    if (force || _settings.dash_style != _last_saved.dash_style)
        _prefs.putUChar("style", _settings.dash_style);
    if (force || _settings.brightness_pct != _last_saved.brightness_pct)
        _prefs.putUChar("bright", _settings.brightness_pct);
    if (force || _settings.max_battery_amps != _last_saved.max_battery_amps)
        _prefs.putUShort("bat_a", _settings.max_battery_amps);
    if (force || _settings.max_phase_amps != _last_saved.max_phase_amps)
        _prefs.putUShort("pha_a", _settings.max_phase_amps);
    if (force || _settings.field_weak_amps != _last_saved.field_weak_amps)
        _prefs.putUChar("fw_a", _settings.field_weak_amps);
    if (force || _settings.max_speed_kmh != _last_saved.max_speed_kmh)
        _prefs.putUChar("spd_lim", _settings.max_speed_kmh);
    if (force || floatChanged(_settings.throttle_ramp_sec, _last_saved.throttle_ramp_sec))
        _prefs.putFloat("t_ramp", _settings.throttle_ramp_sec);
    if (force || floatChanged(_settings.wheel_diameter_in, _last_saved.wheel_diameter_in))
        _prefs.putFloat("wheel", _settings.wheel_diameter_in);
    if (force || _settings.wheel_gps_calibrated != _last_saved.wheel_gps_calibrated)
        _prefs.putBool("wheel_gps", _settings.wheel_gps_calibrated);
    if (force || _settings.motor_pole_pairs != _last_saved.motor_pole_pairs)
        _prefs.putUChar("poles", _settings.motor_pole_pairs);
    if (force || floatChanged(_settings.gear_ratio, _last_saved.gear_ratio))
        _prefs.putFloat("gear", _settings.gear_ratio);
    if (force || _settings.active_battery_profile != _last_saved.active_battery_profile)
        _prefs.putUChar("bat_prof", _settings.active_battery_profile);
    if (force || floatChanged(_settings.voltage_trim_v, _last_saved.voltage_trim_v))
        _prefs.putFloat("v_trim", _settings.voltage_trim_v);
    if (force || _settings.throttle_adc_channel != _last_saved.throttle_adc_channel)
        _prefs.putUChar("adc_ch", _settings.throttle_adc_channel);
    if (force || _settings.pin_lock_enabled != _last_saved.pin_lock_enabled)
        _prefs.putBool("pin_en", _settings.pin_lock_enabled);
    if (force || strncmp(_settings.pin_code, _last_saved.pin_code, sizeof(_settings.pin_code)) != 0)
        _prefs.putString("pin_code", _settings.pin_code);
    if (force || _settings.race_split1_kmh != _last_saved.race_split1_kmh)
        _prefs.putUChar("rc_sp1", _settings.race_split1_kmh);
    if (force || _settings.race_split2_kmh != _last_saved.race_split2_kmh)
        _prefs.putUChar("rc_sp2", _settings.race_split2_kmh);
    if (force || _settings.race_split3_kmh != _last_saved.race_split3_kmh)
        _prefs.putUChar("rc_sp3", _settings.race_split3_kmh);
    if (force || _settings.gps_enabled != _last_saved.gps_enabled)
        _prefs.putBool("gps_en", _settings.gps_enabled);
    if (force || _settings.incline_gforce_enabled != _last_saved.incline_gforce_enabled)
        _prefs.putBool("inc_en", _settings.incline_gforce_enabled);
    if (force || _settings.show_gforce != _last_saved.show_gforce)
        _prefs.putUChar("show_gf", _settings.show_gforce);
    if (force || _settings.show_incline != _last_saved.show_incline)
        _prefs.putUChar("show_inc", _settings.show_incline);
    if (force || _settings.show_light_btn != _last_saved.show_light_btn)
        _prefs.putUChar("show_lb", _settings.show_light_btn);
    if (force || _settings.brake_mode != _last_saved.brake_mode)
        _prefs.putUChar("brk_mode", _settings.brake_mode);
    if (force || _settings.wifi_bridge_enabled != _last_saved.wifi_bridge_enabled)
        _prefs.putBool("wifi_en", _settings.wifi_bridge_enabled);
    if (force || _settings.setup_completed != _last_saved.setup_completed)
        _prefs.putBool("setup_done", _settings.setup_completed);
    if (force || _settings.color_theme != _last_saved.color_theme)
        _prefs.putUChar("theme", _settings.color_theme);
    if (force || _settings.turbine_sound_enabled != _last_saved.turbine_sound_enabled)
        _prefs.putBool("turb_en", _settings.turbine_sound_enabled);
    if (force || _settings.ble_keyless_enabled != _last_saved.ble_keyless_enabled)
        _prefs.putBool("ble_en", _settings.ble_keyless_enabled);
    if (force || _settings.ble_autolock_enabled != _last_saved.ble_autolock_enabled)
        _prefs.putBool("ble_alock", _settings.ble_autolock_enabled);
    if (force || _settings.ble_paired != _last_saved.ble_paired)
        _prefs.putBool("ble_pair", _settings.ble_paired);
    if (force || strncmp(_settings.ble_paired_addr, _last_saved.ble_paired_addr, sizeof(_settings.ble_paired_addr)) != 0)
        _prefs.putString("ble_addr", _settings.ble_paired_addr);
    if (force || strncmp(_settings.ble_paired_name, _last_saved.ble_paired_name, sizeof(_settings.ble_paired_name)) != 0)
        _prefs.putString("ble_name", _settings.ble_paired_name);

    bool odo_written = false;
    if (force || fabsf(_settings.odo_km - _last_saved.odo_km) >= 0.1f) {
        _prefs.putFloat("odo_km", _settings.odo_km);
        odo_written = true;
    }
    if (force || _settings.tcs_enabled != _last_saved.tcs_enabled)
        _prefs.putBool("tcs_en", _settings.tcs_enabled);
    if (force || _settings.battery_cells != _last_saved.battery_cells)
        _prefs.putUChar("bat_cells", _settings.battery_cells);

    if (force || floatChanged(_race_records.best_0_22_sec, _last_saved_race.best_0_22_sec))
        _prefs.putFloat("rc_best22", _race_records.best_0_22_sec);
    if (force || floatChanged(_race_records.best_0_50_sec, _last_saved_race.best_0_50_sec))
        _prefs.putFloat("rc_best50", _race_records.best_0_50_sec);
    if (force || floatChanged(_race_records.best_0_70_sec, _last_saved_race.best_0_70_sec))
        _prefs.putFloat("rc_best70", _race_records.best_0_70_sec);

    float prev_saved_odo = _last_saved.odo_km;
    _last_saved = _settings;
    if (!odo_written && !force) {
        _last_saved.odo_km = prev_saved_odo;
    }
    _last_saved_race = _race_records;
    _has_saved_snapshot = true;
}

void SettingsManager::save() {
    saveInternal(false);
}

void SettingsManager::saveOdo(float odo_km) {
    if (isnan(odo_km) || odo_km < 0.0f || odo_km > 999999.0f) return;
    _settings.odo_km = odo_km;
    if (!_has_saved_snapshot || fabsf(odo_km - _last_saved.odo_km) >= 0.1f) {
        _prefs.putFloat("odo_km", odo_km);
        _last_saved.odo_km = odo_km;
    }
}

void SettingsManager::saveRaceRecords(const RaceRecords &records) {
    _race_records = records;
    if (!_has_saved_snapshot || floatChanged(_race_records.best_0_22_sec, _last_saved_race.best_0_22_sec)) {
        _prefs.putFloat("rc_best22", _race_records.best_0_22_sec);
        _last_saved_race.best_0_22_sec = _race_records.best_0_22_sec;
    }
    if (!_has_saved_snapshot || floatChanged(_race_records.best_0_50_sec, _last_saved_race.best_0_50_sec)) {
        _prefs.putFloat("rc_best50", _race_records.best_0_50_sec);
        _last_saved_race.best_0_50_sec = _race_records.best_0_50_sec;
    }
    if (!_has_saved_snapshot || floatChanged(_race_records.best_0_70_sec, _last_saved_race.best_0_70_sec)) {
        _prefs.putFloat("rc_best70", _race_records.best_0_70_sec);
        _last_saved_race.best_0_70_sec = _race_records.best_0_70_sec;
    }
}

void SettingsManager::resetRaceRecords() {
    _race_records.reset();
    saveRaceRecords(_race_records);
}

void SettingsManager::resetToDefaults() {
    loadDefaults();
    saveInternal(true);
}
