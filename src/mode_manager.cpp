#include "mode_manager.h"
#include "vesc_uart.h"
#include "settings_manager.h"
#include <math.h>

ModeManager& ModeManager::instance() {
    static ModeManager s_instance;
    return s_instance;
}

ModeManager::ModeManager()
    : _tuning_mode(false),
      _boost_active(false),
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
    _config.tuned_modes[PROFILE_MODE_SPORT].max_battery_amps = 60.0f;
    _config.tuned_modes[PROFILE_MODE_SPORT].max_phase_amps   = 120.0f;

    // 2: Tuned RACE (Full Performance 100+ km/h Output - Speed Komplett Offen)
    _config.tuned_modes[PROFILE_MODE_RACE].max_speed_kmh    = 100.0f;
    _config.tuned_modes[PROFILE_MODE_RACE].max_power_watts  = 15000.0f;
    _config.tuned_modes[PROFILE_MODE_RACE].max_pull_watts   = 25000.0f;
    _config.tuned_modes[PROFILE_MODE_RACE].max_battery_amps = 60.0f;
    _config.tuned_modes[PROFILE_MODE_RACE].max_phase_amps   = 140.0f;

    // Global parameters
    _config.throttle_curve       = 0;     // 0 = Linear
    _config.battery_cells        = 13;    // 13S Pack (48V)
    _config.battery_cutoff_start = 42.9f; // 3.3V / cell (13S)
    _config.battery_cutoff_end   = 39.0f; // 3.0V / cell (13S)
}

void ModeManager::begin() {
    // Boot state is ALWAYS legal mode (ungetunt / gedrosselt, dot lit)
    _tuning_mode = false;
    _boost_active = false;
    _ride_mode   = MODE_SPORT;
    _limits_dirty = false;

    _prefs.begin("vesc_modes", false);
    if (!_prefs.isKey("cfg_48v_v4")) {
        setFactoryDefaults();
        uint8_t s_cells = Settings.get().battery_cells;
        if (s_cells < 10 || s_cells > 24) s_cells = 13;
        _config.battery_cells        = s_cells;
        _config.battery_cutoff_start = roundf((float)s_cells * 3.3f * 10.0f) / 10.0f;
        _config.battery_cutoff_end   = roundf((float)s_cells * 3.0f * 10.0f) / 10.0f;
        Settings.get().battery_cells = s_cells;
        saveToNvs();
        _prefs.putBool("cfg_48v_v4", true);
        _prefs.putBool("cfg_v3", true);
        Serial.println("[MODE_MGR] Initialized/Migrated 48V (13S) vehicle & mode profiles in NVS.");
    } else {
        loadFromNvs();
        if (_config.battery_cells < 10 || _config.battery_cells > 24) {
            _config.battery_cells = 13;
        }
        Settings.get().battery_cells = _config.battery_cells;
        Serial.println("[MODE_MGR] Loaded vehicle & mode configurations from NVS.");
    }

    // Send initial legal limits immediately to VESC
    applyLimitsToVesc();
}

void ModeManager::setBatteryCells(uint8_t cells) {
    if (cells < 10) cells = 10;
    if (cells > 24) cells = 24;

    bool cells_changed = (_config.battery_cells != cells);
    bool settings_diff = (Settings.get().battery_cells != cells);

    _config.battery_cells = cells;
    Settings.get().battery_cells = cells;

    // Auto-recalculate sensible cutoff voltages when cell count changes (or if out of plausible range)
    float min_valid = (float)cells * 2.5f;
    float max_valid = (float)cells * 4.0f;
    if (cells_changed ||
        _config.battery_cutoff_start < min_valid || _config.battery_cutoff_start > max_valid ||
        _config.battery_cutoff_end < min_valid   || _config.battery_cutoff_end > max_valid ||
        _config.battery_cutoff_end >= _config.battery_cutoff_start) {
        _config.battery_cutoff_start = roundf((float)cells * 3.3f * 10.0f) / 10.0f;
        _config.battery_cutoff_end   = roundf((float)cells * 3.0f * 10.0f) / 10.0f;
        cells_changed = true;
    }

    if (settings_diff) {
        Settings.save();
    }
    if (cells_changed) {
        saveToNvs();
        _limits_dirty = true;
        applyLimitsToVesc();
        Serial.printf("[MODE_MGR] Battery cells set to %uS (Cutoffs: %.1fV .. %.1fV)\n",
                      cells, _config.battery_cutoff_start, _config.battery_cutoff_end);
    }
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
    if (_config.battery_cells < 10 || _config.battery_cells > 24) _config.battery_cells = 13;
    _config.battery_cutoff_start = _prefs.getFloat("b_cut_s", _config.battery_cutoff_start);
    _config.battery_cutoff_end   = _prefs.getFloat("b_cut_e", _config.battery_cutoff_end);

    // Strictly guarantee and heal legal street limits (eKFV: max 20 km/h + 10% tol = 22 km/h)
    // Ensures existing flashed devices with corrupted/NaN NVS recover legal limits immediately on boot!
    bool healed = false;
    if (!( _config.legal_modes[PROFILE_MODE_ECO].max_speed_kmh >= 5.0f && _config.legal_modes[PROFILE_MODE_ECO].max_speed_kmh <= 15.0f )) {
        _config.legal_modes[PROFILE_MODE_ECO].max_speed_kmh = 15.0f;
        healed = true;
    }
    if (!( _config.legal_modes[PROFILE_MODE_SPORT].max_speed_kmh >= 5.0f && _config.legal_modes[PROFILE_MODE_SPORT].max_speed_kmh <= 20.0f )) {
        _config.legal_modes[PROFILE_MODE_SPORT].max_speed_kmh = 20.0f;
        healed = true;
    }
    if (!( _config.legal_modes[PROFILE_MODE_RACE].max_speed_kmh >= 5.0f && _config.legal_modes[PROFILE_MODE_RACE].max_speed_kmh <= 22.0f )) {
        _config.legal_modes[PROFILE_MODE_RACE].max_speed_kmh = 22.0f;
        healed = true;
    }
    for (int i = 0; i < PROFILE_MODE_COUNT; i++) {
        if (!( _config.legal_modes[i].max_power_watts > 0.0f && _config.legal_modes[i].max_power_watts <= 1000.0f )) {
            _config.legal_modes[i].max_power_watts = 500.0f;
            healed = true;
        }
        if (!( _config.legal_modes[i].max_battery_amps > 0.0f && _config.legal_modes[i].max_battery_amps <= 25.0f )) {
            _config.legal_modes[i].max_battery_amps = 20.0f;
            healed = true;
        }
    }
    if (healed) {
        saveToNvs();
    }
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

    if (_config.battery_cells < 10) _config.battery_cells = 10;
    if (_config.battery_cells > 24) _config.battery_cells = 24;
    Settings.get().battery_cells = _config.battery_cells;

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
        if (!enable) {
            _boost_active = false; // Invariant: Police mode strictly cancels Hyper Boost
        }
        _limits_dirty = true;
        Serial.printf("[MODE_MGR] Tuning mode switched to: %s\n", enable ? "UNLOCKED" : "LOCKED_LEGAL");
        applyLimitsToVesc();
    }
}

void ModeManager::setBoostActive(bool enable) {
    if (_boost_active != enable) {
        _boost_active = enable;
        _limits_dirty = true;
        ModeLimits lim = getActiveLimits();
        Serial.printf("[MODE_MGR] Hyper Boost switched to: %s -> Battery: %.1f A, Phase: %.1f A, Speed: %s\n",
                      enable ? "ON" : "OFF",
                      lim.max_battery_amps,
                      lim.max_phase_amps,
                      lim.max_speed_kmh >= 100.0f ? "UNLIMITED (KOMPLETT OFFEN)" : "LIMITED");
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

    ModeLimits lim;
    if (!_tuning_mode) {
        // Legal street limits (ECO 15, SPORT 20, RACE 22) - Hyper Boost strictly forbidden in legal mode
        lim = _config.legal_modes[idx];
    } else {
        // Tuning mode unlocked limits (ECO, SPORT, RACE)
        lim = _config.tuned_modes[idx];

        if (_boost_active) {
            // Boost requested by user: raise to at least 70A battery, 140A phase, 4000W
            if (lim.max_battery_amps < 70.0f) {
                lim.max_battery_amps = 70.0f;
            }
            if (lim.max_phase_amps < 140.0f) {
                lim.max_phase_amps = 140.0f;
            }
            if (lim.max_power_watts < 4000.0f) {
                lim.max_power_watts = 4000.0f;
            }
            if (_ride_mode == MODE_RACE) {
                lim.max_speed_kmh = 100.0f; // Speed komplett offen (VESC ERPM 100.000)
            }
        }
    }

    return lim;
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

