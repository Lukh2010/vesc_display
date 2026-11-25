#include "mode_manager.h"
#include "vesc_uart.h"
#include "settings_manager.h"
#include <math.h>

namespace {
struct ModeLockGuard {
#if !defined(NATIVE_SIMULATOR)
    SemaphoreHandle_t m;
    bool locked;
    explicit ModeLockGuard(SemaphoreHandle_t mutex) : m(mutex), locked(false) {
        if (m) {
            locked = (xSemaphoreTakeRecursive(m, portMAX_DELAY) == pdTRUE);
        }
    }
    ~ModeLockGuard() { unlock(); }
    void unlock() {
        if (m && locked) {
            xSemaphoreGiveRecursive(m);
            locked = false;
        }
    }
#else
    explicit ModeLockGuard(int = 0) {}
    void unlock() {}
#endif
};
} // namespace

#if !defined(NATIVE_SIMULATOR)
#define MODE_LOCK() ModeLockGuard mode_lock(_mutex)
#else
#define MODE_LOCK() ModeLockGuard mode_lock(0)
#endif

ModeManager& ModeManager::instance() {
    static ModeManager s_instance;
    return s_instance;
}

ModeManager::ModeManager()
    : _has_saved_snapshot(false),
      _tuning_mode(false),
      _boost_active(false),
      _is_locked(false),
      _ride_mode(MODE_ECO),
      _limits_dirty(false),
      _async_vesc_push(false)
#if !defined(NATIVE_SIMULATOR)
    , _mutex(xSemaphoreCreateRecursiveMutex())
#endif
{
    memset(&_config, 0, sizeof(_config));
    memset(&_last_saved_config, 0, sizeof(_last_saved_config));
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
    MODE_LOCK();
    // Boot state is ALWAYS legal mode (ungetunt / gedrosselt, dot lit)
    _tuning_mode = false;
    _boost_active = false;
    _ride_mode   = MODE_SPORT;
    _limits_dirty = false;

    _prefs.begin("vesc_modes", false);
    loadFromNvs();
    if (_config.battery_cells < 10 || _config.battery_cells > 24) {
        _config.battery_cells = 13;
    }
    Settings.get().battery_cells = _config.battery_cells;

    // Release ModeManager mutex before acquiring VescHandler UartLock inside applyLimitsToVesc()
    mode_lock.unlock();
    applyLimitsToVesc();
}

VehicleModeConfig ModeManager::getConfigSnapshot() const {
    MODE_LOCK();
    return _config;
}

uint8_t ModeManager::getBatteryCells() const {
    MODE_LOCK();
    return _config.battery_cells;
}

bool ModeManager::isTuningMode() const {
    MODE_LOCK();
    return _tuning_mode;
}

bool ModeManager::isBoostActive() const {
    MODE_LOCK();
    return _boost_active;
}

bool ModeManager::isLocked() const {
    MODE_LOCK();
    return _is_locked;
}

RideMode ModeManager::getRideMode() const {
    MODE_LOCK();
    return _ride_mode;
}

void ModeManager::setAsyncVescPush(bool enabled) {
    MODE_LOCK();
    _async_vesc_push = enabled;
}

void ModeManager::setBatteryCells(uint8_t cells) {
    if (cells < 10) cells = 10;
    if (cells > 24) cells = 24;

    bool cells_changed = false;
    bool settings_diff = false;
    float cut_s = 0.0f, cut_e = 0.0f;
    bool do_sync_push = false;
    {
        MODE_LOCK();
        cells_changed = (_config.battery_cells != cells);
        settings_diff = (Settings.get().battery_cells != cells);

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
            cut_s = _config.battery_cutoff_start;
            cut_e = _config.battery_cutoff_end;
            do_sync_push = !_async_vesc_push;
        }
    }

    if (cells_changed) {
        if (do_sync_push) {
            applyLimitsToVesc();
        }
        Serial.printf("[MODE_MGR] Battery cells set to %uS (Cutoffs: %.1fV .. %.1fV)\n",
                      cells, cut_s, cut_e);
    }
}

void ModeManager::loadFromNvs() {
    MODE_LOCK();
    VehicleModeConfigBlob blob;
    size_t read_bytes = _prefs.getBytes("cfg_blob_v5", &blob, sizeof(blob));
    if (read_bytes == sizeof(blob) && blob.magic == MODE_CONFIG_BLOB_MAGIC && blob.version == MODE_CONFIG_BLOB_VERSION) {
        _config = blob.config;
    } else {
        // Fallback / One-time migration from legacy individual keys
        bool has_legacy = _prefs.isKey("l0_spd") || _prefs.isKey("b_cells");
        if (has_legacy) {
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
        } else {
            setFactoryDefaults();
            uint8_t cells = Settings.get().battery_cells;
            if (cells >= 10 && cells <= 24) {
                _config.battery_cells = cells;
                _config.battery_cutoff_start = roundf((float)cells * 3.3f * 10.0f) / 10.0f;
                _config.battery_cutoff_end   = roundf((float)cells * 3.0f * 10.0f) / 10.0f;
            }
        }
    }

    if (_config.battery_cells < 10 || _config.battery_cells > 24) _config.battery_cells = 13;

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

        // Validate & heal tuned modes against NaN / corrupted values
        if (isnan(_config.tuned_modes[i].max_speed_kmh) || _config.tuned_modes[i].max_speed_kmh < 5.0f || _config.tuned_modes[i].max_speed_kmh > 150.0f) {
            _config.tuned_modes[i].max_speed_kmh = (i == PROFILE_MODE_ECO) ? 35.0f : ((i == PROFILE_MODE_SPORT) ? 55.0f : 100.0f);
            healed = true;
        }
        if (isnan(_config.tuned_modes[i].max_power_watts) || _config.tuned_modes[i].max_power_watts < 100.0f || _config.tuned_modes[i].max_power_watts > 35000.0f) {
            _config.tuned_modes[i].max_power_watts = (i == PROFILE_MODE_ECO) ? 1500.0f : ((i == PROFILE_MODE_SPORT) ? 3500.0f : 15000.0f);
            healed = true;
        }
        if (isnan(_config.tuned_modes[i].max_battery_amps) || _config.tuned_modes[i].max_battery_amps < 5.0f || _config.tuned_modes[i].max_battery_amps > 150.0f) {
            _config.tuned_modes[i].max_battery_amps = (i == PROFILE_MODE_ECO) ? 30.0f : 60.0f;
            healed = true;
        }
        if (isnan(_config.tuned_modes[i].max_phase_amps) || _config.tuned_modes[i].max_phase_amps < 10.0f || _config.tuned_modes[i].max_phase_amps > 300.0f) {
            _config.tuned_modes[i].max_phase_amps = (i == PROFILE_MODE_ECO) ? 60.0f : ((i == PROFILE_MODE_SPORT) ? 120.0f : 140.0f);
            healed = true;
        }
    }

    if (read_bytes != sizeof(blob) || healed) {
        saveToNvs();
    } else {
        _last_saved_config = _config;
        _has_saved_snapshot = true;
    }
}

void ModeManager::saveToNvs() {
    MODE_LOCK();
    if (_has_saved_snapshot && memcmp(&_config, &_last_saved_config, sizeof(VehicleModeConfig)) == 0) {
        return;
    }

    if (_config.battery_cells < 10) _config.battery_cells = 10;
    if (_config.battery_cells > 24) _config.battery_cells = 24;
    Settings.get().battery_cells = _config.battery_cells;

    VehicleModeConfigBlob blob;
    memset(&blob, 0, sizeof(blob));
    blob.magic = MODE_CONFIG_BLOB_MAGIC;
    blob.version = MODE_CONFIG_BLOB_VERSION;
    blob.reserved = 0;
    blob.config = _config;

    _prefs.putBytes("cfg_blob_v5", &blob, sizeof(blob));

    _last_saved_config = _config;
    _has_saved_snapshot = true;
    Serial.println("[MODE_MGR] Saved vehicle & mode configurations (v5 blob) to NVS flash.");
}

void ModeManager::resetDefaults() {
    bool do_sync_push = false;
    {
        MODE_LOCK();
        setFactoryDefaults();
        saveToNvs();
        _limits_dirty = true;
        do_sync_push = !_async_vesc_push;
    }
    if (do_sync_push) {
        applyLimitsToVesc();
    }
}

void ModeManager::setTuningMode(bool enable) {
    bool changed = false;
    bool do_sync_push = false;
    {
        MODE_LOCK();
        if (_tuning_mode != enable) {
            _tuning_mode = enable;
            if (!enable) {
                _boost_active = false; // Invariant: Police mode strictly cancels Hyper Boost
            }
            _limits_dirty = true;
            changed = true;
            do_sync_push = !_async_vesc_push;
        }
    }
    if (changed) {
        Serial.printf("[MODE_MGR] Tuning mode switched to: %s\n", enable ? "UNLOCKED" : "LOCKED_LEGAL");
        if (do_sync_push) {
            applyLimitsToVesc();
        }
    }
}

void ModeManager::setBoostActive(bool enable) {
    bool changed = false;
    bool do_sync_push = false;
    ModeLimits lim = { 0.0f, 0.0f, 0.0f, 0.0f, 0.0f };
    {
        MODE_LOCK();
        if (_boost_active != enable) {
            _boost_active = enable;
            _limits_dirty = true;
            lim = getActiveLimits();
            changed = true;
            do_sync_push = !_async_vesc_push;
        }
    }
    if (changed) {
        Serial.printf("[MODE_MGR] Hyper Boost switched to: %s -> Battery: %.1f A, Phase: %.1f A, Speed: %s\n",
                      enable ? "ON" : "OFF",
                      lim.max_battery_amps,
                      lim.max_phase_amps,
                      lim.max_speed_kmh >= 100.0f ? "UNLIMITED (KOMPLETT OFFEN)" : "LIMITED");
        if (do_sync_push) {
            applyLimitsToVesc();
        }
    }
}

void ModeManager::setLocked(bool locked) {
    bool changed = false;
    bool do_sync_push = false;
    {
        MODE_LOCK();
        if (_is_locked != locked) {
            _is_locked = locked;
            _limits_dirty = true;
            changed = true;
            do_sync_push = !_async_vesc_push;
        }
    }
    if (changed) {
        Serial.printf("[MODE_MGR] System Lock state: %s\n", locked ? "LOCKED (0A Output)" : "UNLOCKED");
        if (do_sync_push) {
            applyLimitsToVesc();
        }
    }
}

void ModeManager::setRideMode(RideMode mode) {
    bool changed = false;
    bool do_sync_push = false;
    bool tuning_snap = false;
    const char *mode_name = "SPORT";
    {
        MODE_LOCK();
        if (_ride_mode != mode) {
            _ride_mode = mode;
            _limits_dirty = true;
            tuning_snap = _tuning_mode;
            mode_name = getActiveModeName();
            changed = true;
            do_sync_push = !_async_vesc_push;
        }
    }
    if (changed) {
        if (do_sync_push) {
            applyLimitsToVesc();
        }
        Serial.printf("[MODE_MGR] Active ride mode set to: %d (%s, Tuning: %s)\n",
                      (int)mode, mode_name, tuning_snap ? "ON" : "OFF");
    }
}

void ModeManager::cycleMode() {
    RideMode next;
    {
        MODE_LOCK();
        next = (RideMode)(((int)_ride_mode + 1) % PROFILE_MODE_COUNT);
    }
    setRideMode(next);
}

ModeLimits ModeManager::getActiveLimits() const {
    MODE_LOCK();
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
    MODE_LOCK();
    switch (_ride_mode) {
        case MODE_ECO:   return "ECO";
        case MODE_SPORT: return "SPORT";
        case MODE_RACE:  return "RACE";
        default:         return "SPORT";
    }
}

bool ModeManager::checkAndClearLimitsDirty() {
    MODE_LOCK();
    if (_limits_dirty) {
        _limits_dirty = false;
        return true;
    }
    return false;
}

void ModeManager::markLimitsDirty() {
    MODE_LOCK();
    _limits_dirty = true;
}

void ModeManager::applyLimitsToVesc() {
    bool locked = false;
    bool tuning = false;
    const char *mode_name = "SPORT";
    ModeLimits lim = { 0.0f, 0.0f, 0.0f, 0.0f, 0.0f };
    float cut_s = 0.0f, cut_e = 0.0f;
    {
        MODE_LOCK();
        _limits_dirty = false;
        locked = _is_locked;
        tuning = _tuning_mode;
        mode_name = getActiveModeName();
        lim = getActiveLimits();
        cut_s = _config.battery_cutoff_start;
        cut_e = _config.battery_cutoff_end;
    }

    // Lock released before calling VescHandler (which acquires UartLock) to prevent AB-BA deadlock
    if (locked) {
        Serial.println("[MODE_MGR] System LOCKED: Sending 0A cutoff limits to VESC.");
        Vesc.sendModeLimits(0.0f, 0.0f, 0.0f, 0.0f, cut_s, cut_e);
        return;
    }
    Serial.printf("[MODE_MGR] Sending active limits (%s, Tuning:%s) to VESC: Spd=%.1f km/h, Pwr=%.0f W (Pull: %.0f W), Bat=%.1f A, Pha=%.1f A, Cut=%.1fV..%.1fV\n",
                  mode_name, tuning ? "ON" : "OFF",
                  lim.max_speed_kmh, lim.max_power_watts, lim.max_pull_watts, lim.max_battery_amps, lim.max_phase_amps,
                  cut_s, cut_e);
    Vesc.sendModeLimits(lim.max_speed_kmh, lim.max_power_watts, lim.max_battery_amps, lim.max_phase_amps,
                        cut_s, cut_e);
}

