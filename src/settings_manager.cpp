#include "settings_manager.h"

SettingsManager Settings;

SettingsManager::SettingsManager() {
    loadDefaults();
}

void SettingsManager::loadDefaults() {
    _settings.dash_style             = STYLE_HYPER_35KW;
    _settings.brightness_pct         = 90;
    _settings.max_battery_amps       = 25;
    _settings.max_phase_amps         = 45;
    _settings.field_weak_amps        = 10;
    _settings.max_speed_kmh          = 0; // Unlimited
    _settings.throttle_ramp_sec      = 0.2f;
    _settings.wheel_diameter_in      = 26.0f;
    _settings.motor_pole_pairs       = 15;
    _settings.gear_ratio             = 1.0f;
    _settings.active_battery_profile = 0;
    _settings.voltage_trim_v         = -0.3f;
    _settings.throttle_adc_channel   = 0; // Default to ADC1 (standard VESC throttle)
    _settings.pin_lock_enabled       = true; // Handystyle 4-digit PIN lock on startup
    strncpy(_settings.pin_code, "1234", sizeof(_settings.pin_code));
}

void SettingsManager::begin() {
    _prefs.begin("ebike_cfg", false);

    if (!_prefs.isKey("cfg_init")) {
        // First boot: save initial defaults
        loadDefaults();
        save();
        _prefs.putBool("cfg_init", true);
        _prefs.putBool("adc_v2_fixed", true);
        Serial.println("[SETTINGS] Initialized default NVS configuration.");
    } else {
        _settings.dash_style             = _prefs.getUChar("style", STYLE_HYPER_35KW);
        _settings.brightness_pct         = _prefs.getUChar("bright", 90);
        _settings.max_battery_amps       = _prefs.getUShort("bat_a", 28);
        _settings.max_phase_amps         = _prefs.getUShort("pha_a", 55);
        _settings.field_weak_amps        = _prefs.getUChar("fw_a", 10);
        _settings.max_speed_kmh          = _prefs.getUChar("spd_lim", 0);
        _settings.throttle_ramp_sec      = _prefs.getFloat("t_ramp", 0.2f);
        _settings.wheel_diameter_in      = _prefs.getFloat("wheel", 26.0f);
        _settings.motor_pole_pairs       = _prefs.getUChar("poles", 15);
        _settings.gear_ratio             = _prefs.getFloat("gear", 1.0f);
        _settings.active_battery_profile = _prefs.getUChar("bat_prof", 0);
        _settings.voltage_trim_v         = _prefs.getFloat("v_trim", -0.3f);
        _settings.throttle_adc_channel   = _prefs.getUChar("adc_ch", 0);
        _settings.pin_lock_enabled       = _prefs.getBool("pin_en", true);
        _prefs.getString("pin_code", _settings.pin_code, sizeof(_settings.pin_code));
        if (strlen(_settings.pin_code) != 4) {
            strncpy(_settings.pin_code, "1234", sizeof(_settings.pin_code));
        }

        // One-time automatic migration: fix boards that had adc_ch set to 1 by previous buggy default
        if (!_prefs.getBool("adc_v2_fixed", false)) {
            _settings.throttle_adc_channel = 0; // Force default to ADC1
            _prefs.putUChar("adc_ch", 0);
            _prefs.putBool("adc_v2_fixed", true);
            Serial.println("[SETTINGS] Migrated throttle ADC channel to ADC1 (standard).");
        }

        Serial.println("[SETTINGS] Loaded configuration from NVS flash.");
    }
}

void SettingsManager::save() {
    _prefs.putUChar("style", _settings.dash_style);
    _prefs.putUChar("bright", _settings.brightness_pct);
    _prefs.putUShort("bat_a", _settings.max_battery_amps);
    _prefs.putUShort("pha_a", _settings.max_phase_amps);
    _prefs.putUChar("fw_a", _settings.field_weak_amps);
    _prefs.putUChar("spd_lim", _settings.max_speed_kmh);
    _prefs.putFloat("t_ramp", _settings.throttle_ramp_sec);
    _prefs.putFloat("wheel", _settings.wheel_diameter_in);
    _prefs.putUChar("poles", _settings.motor_pole_pairs);
    _prefs.putFloat("gear", _settings.gear_ratio);
    _prefs.putUChar("bat_prof", _settings.active_battery_profile);
    _prefs.putFloat("v_trim", _settings.voltage_trim_v);
    _prefs.putUChar("adc_ch", _settings.throttle_adc_channel);
    _prefs.putBool("pin_en", _settings.pin_lock_enabled);
    _prefs.putString("pin_code", _settings.pin_code);
}

void SettingsManager::resetToDefaults() {
    loadDefaults();
    save();
}
