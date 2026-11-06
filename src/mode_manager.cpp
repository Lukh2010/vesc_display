#include "mode_manager.h"
#include "vesc_uart.h"

ModeManager& ModeManager::instance() {
    static ModeManager s_instance;
    return s_instance;
}

ModeManager::ModeManager()
    : _tuning_mode(false),
      _is_locked(false),
      _ride_mode(MODE_ECO),
      _limits_dirty(false) {
    setFactoryDefaults();
}

void ModeManager::setFactoryDefaults() {
    // -------------------------------------------------------------
    // Legal / Street Mode Limits (tuning_mode == false)
    // ECO: 15 km/h | SPORT: 20 km/h | RACE: 22 km/h (eKFV 20 + 10% tol)
    // Peak pull: 1000W acceleration | Continuous: 500W limit at 22 km/h
    // -------------------------------------------------------------
    // 0: Legal ECO
    _config.legal_modes[PROFILE_MODE_ECO].max_speed_kmh    = 15.0f;
    _config.legal_modes[PROFILE_MODE_ECO].max_power_watts  = 500.0f;
    _config.legal_modes[PROFILE_MODE_ECO].max_pull_watts   = 1000.0f;
    _config.legal_modes[PROFILE_MODE_ECO].max_battery_amps = 20.0f;
    _config.legal_modes[PROFILE_MODE_ECO].max_phase_amps   = 40.0f;

    // 1: Legal SPORT
    _config.legal_modes[PROFILE_MODE_SPORT].max_speed_kmh    = 20.0f;
    _config.legal_modes[PROFILE_MODE_SPORT].max_power_watts  = 500.0f;
    _config.legal_modes[PROFILE_MODE_SPORT].max_pull_watts   = 1000.0f;
    _config.legal_modes[PROFILE_MODE_SPORT].max_battery_amps = 20.0f;
    _config.legal_modes[PROFILE_MODE_SPORT].max_phase_amps   = 45.0f;

    // 2: Legal RACE
    _config.legal_modes[PROFILE_MODE_RACE].max_speed_kmh    = 22.0f;
    _config.legal_modes[PROFILE_MODE_RACE].max_power_watts  = 500.0f;
    _config.legal_modes[PROFILE_MODE_RACE].max_pull_watts   = 1000.0f;
    _config.legal_modes[PROFILE_MODE_RACE].max_battery_amps = 22.0f;
    _config.legal_modes[PROFILE_MODE_RACE].max_phase_amps   = 50.0f;

    // -------------------------------------------------------------
    // Tuned / Unlocked High-Performance Limits (tuning_mode == true)
    // -------------------------------------------------------------
    // 0: Tuned ECO
    _config.tuned_modes[PROFILE_MODE_ECO].max_speed_kmh    = 35.0f;
    _config.tuned_modes[PROFILE_MODE_ECO].max_power_watts  = 1500.0f;
    _config.tuned_modes[PROFILE_MODE_ECO].max_pull_watts   = 2000.0f;
    _config.tuned_modes[PROFILE_MODE_ECO].max_battery_amps = 30.0f;
    _config.tuned_modes[PROFILE_MODE_ECO].max_phase_amps   = 60.0f;

    // 1: Tuned SPORT
    _config.tuned_modes[PROFILE_MODE_SPORT].max_speed_kmh    = 55.0f;
    _config.tuned_modes[PROFILE_MODE_SPORT].max_power_watts  = 3500.0f;
    _config.tuned_modes[PROFILE_MODE_SPORT].max_pull_watts   = 5000.0f;
    _config.tuned_modes[PROFILE_MODE_SPORT].max_battery_amps = 55.0f;
    _config.tuned_modes[PROFILE_MODE_SPORT].max_phase_amps   = 110.0f;

    // 2: Tuned RACE (Full Performance 35kW+ Output)
    _config.tuned_modes[PROFILE_MODE_RACE].max_speed_kmh    = 85.0f;
    _config.tuned_modes[PROFILE_MODE_RACE].max_power_watts  = 15000.0f;
    _config.tuned_modes[PROFILE_MODE_RACE].max_pull_watts   = 25000.0f;
    _config.tuned_modes[PROFILE_MODE_RACE].max_battery_amps = 95.0f;
    _config.tuned_modes[PROFILE_MODE_RACE].max_phase_amps   = 220.0f;

    // Global parameters
    _config.throttle_curve       = 0;     // 0 = Linear
    _config.battery_cells        = 14;    // 14S Pack
    _config.battery_cutoff_start = 46.2f; // 3.3V / cell
    _config.battery_cutoff_end   = 42.0f; // 3.0V / cell
}

void ModeManager::begin() {
    // Boot state is ALWAYS legal mode (ungetunt / gedrosselt, dot lit)
    _tuning_mode = false;
    _ride_mode   = MODE_SPORT;
    _limits_dirty = false;

    _prefs.begin("vesc_modes", false);
    if (!_prefs.isKey("cfg_v2")) {
        setFactoryDefaults();
        saveToNvs();
        _prefs.putBool("cfg_v2", true);
        Serial.println("[MODE_MGR] Initialized factory default vehicle & mode profiles (v2) in NVS.");
    } else {
        loadFromNvs();
        Serial.println("[MODE_MGR] Loaded vehicle & mode configurations from NVS.");
    }

    // Send initial legal limits immediately to VESC
    applyLimitsToVesc();
}

void ModeManager::loadFromNvs() {
    const char *leg_prefixes[PROFILE_MODE_COUNT] = { "l0", "l1", "l2" };
    const char *tun_prefixes[PROFILE_MODE_COUNT] = { "t0", "t1", "t2" };
    char key[16];

    for (int i = 0; i < PROFILE_MODE_COUNT; i++) {
        // Legal profiles
        snprintf(key, sizeof(key), "%s_spd", leg_prefixes[i]);
        _config.legal_modes[i].max_speed_kmh = _prefs.getFloat(key, _config.legal_modes[i].max_speed_kmh);

        snprintf(key, sizeof(key), "%s_pwr", leg_prefixes[i]);
        _config.legal_modes[i].max_power_watts = _prefs.getFloat(key, _config.legal_modes[i].max_power_watts);

        snprintf(key, sizeof(key), "%s_pul", leg_prefixes[i]);
        _config.legal_modes[i].max_pull_watts = _prefs.getFloat(key, _config.legal_modes[i].max_pull_watts);

        snprintf(key, sizeof(key), "%s_bat", leg_prefixes[i]);
        _config.legal_modes[i].max_battery_amps = _prefs.getFloat(key, _config.legal_modes[i].max_battery_amps);

        snprintf(key, sizeof(key), "%s_pha", leg_prefixes[i]);
        _config.legal_modes[i].max_phase_amps = _prefs.getFloat(key, _config.legal_modes[i].max_phase_amps);

        // Tuned profiles
        snprintf(key, sizeof(key), "%s_spd", tun_prefixes[i]);
        _config.tuned_modes[i].max_speed_kmh = _prefs.getFloat(key, _config.tuned_modes[i].max_speed_kmh);

        snprintf(key, sizeof(key), "%s_pwr", tun_prefixes[i]);
        _config.tuned_modes[i].max_power_watts = _prefs.getFloat(key, _config.tuned_modes[i].max_power_watts);

        snprintf(key, sizeof(key), "%s_pul", tun_prefixes[i]);
        _config.tuned_modes[i].max_pull_watts = _prefs.getFloat(key, _config.tuned_modes[i].max_pull_watts);

        snprintf(key, sizeof(key), "%s_bat", tun_prefixes[i]);
        _config.tuned_modes[i].max_battery_amps = _prefs.getFloat(key, _config.tuned_modes[i].max_battery_amps);

        snprintf(key, sizeof(key), "%s_pha", tun_prefixes[i]);
        _config.tuned_modes[i].max_phase_amps = _prefs.getFloat(key, _config.tuned_modes[i].max_phase_amps);
    }

    _config.throttle_curve       = (int8_t)_prefs.getUChar("t_curve", (uint8_t)_config.throttle_curve);
    _config.battery_cells        = _prefs.getUChar("b_cells", _config.battery_cells);
    _config.battery_cutoff_start = _prefs.getFloat("b_cut_s", _config.battery_cutoff_start);
    _config.battery_cutoff_end   = _prefs.getFloat("b_cut_e", _config.battery_cutoff_end);
}

void ModeManager::saveToNvs() {
    const char *leg_prefixes[PROFILE_MODE_COUNT] = { "l0", "l1", "l2" };
    const char *tun_prefixes[PROFILE_MODE_COUNT] = { "t0", "t1", "t2" };
    char key[16];

    for (int i = 0; i < PROFILE_MODE_COUNT; i++) {
        // Legal profiles
        snprintf(key, sizeof(key), "%s_spd", leg_prefixes[i]);
        _prefs.putFloat(key, _config.legal_modes[i].max_speed_kmh);

        snprintf(key, sizeof(key), "%s_pwr", leg_prefixes[i]);
        _prefs.putFloat(key, _config.legal_modes[i].max_power_watts);

        snprintf(key, sizeof(key), "%s_pul", leg_prefixes[i]);
        _prefs.putFloat(key, _config.legal_modes[i].max_pull_watts);

        snprintf(key, sizeof(key), "%s_bat", leg_prefixes[i]);
        _prefs.putFloat(key, _config.legal_modes[i].max_battery_amps);

        snprintf(key, sizeof(key), "%s_pha", leg_prefixes[i]);
        _prefs.putFloat(key, _config.legal_modes[i].max_phase_amps);

        // Tuned profiles
        snprintf(key, sizeof(key), "%s_spd", tun_prefixes[i]);
        _prefs.putFloat(key, _config.tuned_modes[i].max_speed_kmh);

        snprintf(key, sizeof(key), "%s_pwr", tun_prefixes[i]);
        _prefs.putFloat(key, _config.tuned_modes[i].max_power_watts);

        snprintf(key, sizeof(key), "%s_pul", tun_prefixes[i]);
        _prefs.putFloat(key, _config.tuned_modes[i].max_pull_watts);

        snprintf(key, sizeof(key), "%s_bat", tun_prefixes[i]);
        _prefs.putFloat(key, _config.tuned_modes[i].max_battery_amps);

        snprintf(key, sizeof(key), "%s_pha", tun_prefixes[i]);
        _prefs.putFloat(key, _config.tuned_modes[i].max_phase_amps);
    }

    _prefs.putUChar("t_curve", (uint8_t)_config.throttle_curve);
    _prefs.putUChar("b_cells", _config.battery_cells);
    _prefs.putFloat("b_cut_s", _config.battery_cutoff_start);
    _prefs.putFloat("b_cut_e", _config.battery_cutoff_end);
    Serial.println("[MODE_MGR] Saved vehicle & mode configurations (v2) to NVS flash.");
}

void ModeManager::resetDefaults() {
    setFactoryDefaults();
    saveToNvs();
    applyLimitsToVesc();
}

void ModeManager::setTuningMode(bool enable) {
    if (_tuning_mode != enable) {
        _tuning_mode = enable;
        _limits_dirty = true;
        Serial.printf("[MODE_MGR] Tuning mode switched to: %s\n", enable ? "UNLOCKED" : "LOCKED_LEGAL");
        applyLimitsToVesc();
    }
}

void ModeManager::setLocked(bool locked) {
    if (_is_locked != locked) {
        _is_locked = locked;
        _limits_dirty = true;
        Serial.printf("[MODE_MGR] System Lock state: %s\n", locked ? "LOCKED (0A Output)" : "UNLOCKED");
        applyLimitsToVesc();
    }
}

void ModeManager::setRideMode(RideMode mode) {
    if (_ride_mode != mode) {
        _ride_mode = mode;
        _limits_dirty = true;
        applyLimitsToVesc();
        Serial.printf("[MODE_MGR] Active ride mode set to: %d (%s, Tuning: %s)\n",
                      (int)mode, getActiveModeName(), _tuning_mode ? "ON" : "OFF");
    }
}

void ModeManager::cycleMode() {
    RideMode next = (RideMode)(((int)_ride_mode + 1) % PROFILE_MODE_COUNT);
    setRideMode(next);
}

ModeLimits ModeManager::getActiveLimits() const {
    if (_is_locked) {
        ModeLimits zero = { 0.0f, 0.0f, 0.0f, 0.0f, 0.0f };
        return zero;
    }

    int idx = (int)_ride_mode % PROFILE_MODE_COUNT;
    if (idx < 0) idx = 0;

    if (!_tuning_mode) {
        // Legal street limits (ECO 15, SPORT 20, RACE 22)
        return _config.legal_modes[idx];
    } else {
        // Tuning mode unlocked limits (ECO, SPORT, RACE)
        return _config.tuned_modes[idx];
    }
}

const char* ModeManager::getActiveModeName() const {
    switch (_ride_mode) {
        case MODE_ECO:   return "ECO";
        case MODE_SPORT: return "SPORT";
        case MODE_RACE:  return "RACE";
        default:         return "SPORT";
    }
}

bool ModeManager::checkAndClearLimitsDirty() {
    if (_limits_dirty) {
        _limits_dirty = false;
        return true;
    }
    return false;
}

void ModeManager::markLimitsDirty() {
    _limits_dirty = true;
}

void ModeManager::applyLimitsToVesc() {
    _limits_dirty = false;
    if (_is_locked) {
        Serial.println("[MODE_MGR] System LOCKED: Sending 0A cutoff limits to VESC.");
        Vesc.sendModeLimits(0.0f, 0.0f, 0.0f, 0.0f,
                            _config.battery_cutoff_start, _config.battery_cutoff_end);
        return;
    }
    ModeLimits lim = getActiveLimits();
    Serial.printf("[MODE_MGR] Sending active limits (%s, Tuning:%s) to VESC: Spd=%.1f km/h, Pwr=%.0f W (Pull: %.0f W), Bat=%.1f A, Pha=%.1f A, Cut=%.1fV..%.1fV\n",
                  getActiveModeName(), _tuning_mode ? "ON" : "OFF",
                  lim.max_speed_kmh, lim.max_power_watts, lim.max_pull_watts, lim.max_battery_amps, lim.max_phase_amps,
                  _config.battery_cutoff_start, _config.battery_cutoff_end);
    Vesc.sendModeLimits(lim.max_speed_kmh, lim.max_power_watts, lim.max_battery_amps, lim.max_phase_amps,
                        _config.battery_cutoff_start, _config.battery_cutoff_end);
}

