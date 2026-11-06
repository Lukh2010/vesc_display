#include "fault_manager.h"
#include <string.h>
#include <stdio.h>

FaultManager& FaultManager::instance() {
    static FaultManager inst;
    return inst;
}

FaultManager::FaultManager()
    : _fault_count(0),
      _active_fault(0),
      _last_logged_time_ms(0),
      _last_logged_code(0) {
    memset(_history, 0, sizeof(_history));
}

void FaultManager::begin() {
    loadFromNvs();
}

void FaultManager::loadFromNvs() {
    _prefs.begin("vesc_faults", false);
    _fault_count = _prefs.getUChar("f_count", 0);
    if (_fault_count > MAX_FAULT_HISTORY) {
        _fault_count = MAX_FAULT_HISTORY;
    }

    for (uint8_t i = 0; i < _fault_count; i++) {
        char k[16];
        snprintf(k, sizeof(k), "f_%d_c", i);
        _history[i].code = _prefs.getUChar(k, 0);

        snprintf(k, sizeof(k), "f_%d_t", i);
        _history[i].timestamp_s = _prefs.getUInt(k, 0);

        snprintf(k, sizeof(k), "f_%d_v", i);
        _history[i].voltage = _prefs.getFloat(k, 0.0f);

        snprintf(k, sizeof(k), "f_%d_i", i);
        _history[i].current = _prefs.getFloat(k, 0.0f);

        snprintf(k, sizeof(k), "f_%d_pi", i);
        _history[i].phase_current = _prefs.getFloat(k, 0.0f);

        snprintf(k, sizeof(k), "f_%d_tf", i);
        _history[i].temp_fet = _prefs.getFloat(k, 0.0f);

        snprintf(k, sizeof(k), "f_%d_tm", i);
        _history[i].temp_mot = _prefs.getFloat(k, 0.0f);
    }
}

void FaultManager::saveToNvs() {
    _prefs.putUChar("f_count", _fault_count);
    for (uint8_t i = 0; i < _fault_count; i++) {
        char k[16];
        snprintf(k, sizeof(k), "f_%d_c", i);
        _prefs.putUChar(k, _history[i].code);

        snprintf(k, sizeof(k), "f_%d_t", i);
        _prefs.putUInt(k, _history[i].timestamp_s);

        snprintf(k, sizeof(k), "f_%d_v", i);
        _prefs.putFloat(k, _history[i].voltage);

        snprintf(k, sizeof(k), "f_%d_i", i);
        _prefs.putFloat(k, _history[i].current);

        snprintf(k, sizeof(k), "f_%d_pi", i);
        _prefs.putFloat(k, _history[i].phase_current);

        snprintf(k, sizeof(k), "f_%d_tf", i);
        _prefs.putFloat(k, _history[i].temp_fet);

        snprintf(k, sizeof(k), "f_%d_tm", i);
        _prefs.putFloat(k, _history[i].temp_mot);
    }
}

void FaultManager::recordFault(uint8_t code, float v, float i, float phase_i, float fet_temp, float mot_temp, uint32_t uptime_s) {
    if (code == FAULT_NONE) {
        _active_fault = 0;
        return;
    }

    _active_fault = code;
    uint32_t now = millis();

    // Prevent spamming the blackbox if same fault occurs repeatedly within 5 seconds
    if (_last_logged_code == code && (now - _last_logged_time_ms < 5000)) {
        return;
    }

    _last_logged_time_ms = now;
    _last_logged_code = code;

    // Shift older records down (newest at index 0)
    for (int idx = MAX_FAULT_HISTORY - 1; idx > 0; idx--) {
        _history[idx] = _history[idx - 1];
    }

    _history[0].code          = code;
    _history[0].timestamp_s   = uptime_s;
    _history[0].voltage       = v;
    _history[0].current       = i;
    _history[0].phase_current = phase_i;
    _history[0].temp_fet      = fet_temp;
    _history[0].temp_mot      = mot_temp;

    if (_fault_count < MAX_FAULT_HISTORY) {
        _fault_count++;
    }

    saveToNvs();
    Serial.printf("[FAULT BLACKBOX] Recorded VESC Fault %d (%s) at Uptime %lus (V=%.1fV, I=%.1fA, FET=%.0fC, MOT=%.0fC)\n",
                  code, getFaultTitleDe(code), (unsigned long)uptime_s, v, i, fet_temp, mot_temp);
}

FaultRecord FaultManager::getFault(uint8_t index) const {
    if (index >= _fault_count) {
        FaultRecord empty = { 0, 0, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f };
        return empty;
    }
    return _history[index];
}

void FaultManager::clearFaults() {
    _fault_count = 0;
    _active_fault = 0;
    _last_logged_code = 0;
    memset(_history, 0, sizeof(_history));
    _prefs.clear();
    saveToNvs();
    Serial.println("[FAULT BLACKBOX] Cleared all stored fault history in NVS.");
}

const char* FaultManager::getFaultTitleDe(uint8_t code) {
    switch (code) {
        case FAULT_NONE: return "KEIN FEHLER";
        case FAULT_OVER_VOLTAGE: return "UEBERSPANNUNG";
        case FAULT_UNDER_VOLTAGE: return "UNTERSPANNUNG";
        case FAULT_DRV: return "DRV TREIBERFEHLER";
        case FAULT_ABS_OVER_CURRENT: return "ABS UEBERSTROM";
        case FAULT_OVER_TEMP_FET: return "MOSFET UEBERHITZUNG";
        case FAULT_OVER_TEMP_MOTOR: return "MOTOR UEBERHITZUNG";
        case FAULT_GATE_DRIVER_OVER_VOLTAGE: return "GATE-TREIBER UEBERSPG.";
        case FAULT_GATE_DRIVER_UNDER_VOLTAGE: return "GATE-TREIBER UNTERSPG.";
        case FAULT_MCU_UNDER_VOLTAGE: return "MCU UNTERSPANNUNG";
        case FAULT_BOOTING_FROM_WATCHDOG_RESET: return "WATCHDOG REBOOT";
        case FAULT_ENCODER_SPI: return "ENCODER SPI FEHLER";
        case FAULT_ENCODER_SINCOS_BELOW_MIN_AMPLITUDE: return "ENCODER AMPLITUDE TIEF";
        case FAULT_ENCODER_SINCOS_ABOVE_MAX_AMPLITUDE: return "ENCODER AMPLITUDE HOCH";
        case FAULT_FLASH_CORRUPTION: return "FLASH FEHLER";
        case FAULT_HIGH_OFFSET_CURRENT_SENSOR_1: return "SENSOR 1 OFFSET";
        case FAULT_HIGH_OFFSET_CURRENT_SENSOR_2: return "SENSOR 2 OFFSET";
        case FAULT_HIGH_OFFSET_CURRENT_SENSOR_3: return "SENSOR 3 OFFSET";
        case FAULT_UNBALANCED_CURRENTS: return "PHASEN-UNBALANCE";
        case FAULT_BRK: return "BREMSFEHLER (BRK)";
        case FAULT_RESOLVER_LOT: return "RESOLVER LOT";
        case FAULT_RESOLVER_DOS: return "RESOLVER SIGNAL VERLUST";
        case FAULT_RESOLVER_LOS: return "RESOLVER SIGNAL ABBRUCH";
        case FAULT_FLASH_CORRUPTION_APP_CFG: return "APP-KONFIG FEHLER";
        case FAULT_FLASH_CORRUPTION_MC_CFG: return "MOTOR-KONFIG FEHLER";
        case FAULT_ENCODER_NO_MAGNET: return "MAGNET FEHLT";
        default: return "VESC FEHLER";
    }
}

const char* FaultManager::getFaultDescriptionDe(uint8_t code) {
    switch (code) {
        case FAULT_NONE: return "Kein aktiver VESC-Fehler";
        case FAULT_OVER_VOLTAGE: return "Akkuspannung zu hoch! Rekuperation gestoppt.";
        case FAULT_UNDER_VOLTAGE: return "Akkuspannung zu niedrig! Sofort laden.";
        case FAULT_DRV: return "Gate-Treiber Fehler! Motor stromlos geschaltet.";
        case FAULT_ABS_OVER_CURRENT: return "Phasenstrom-Spitze ueberschritten! Last reduzieren.";
        case FAULT_OVER_TEMP_FET: return "ESC-Mosfets ueberhitzt! System abkuehlen lassen.";
        case FAULT_OVER_TEMP_MOTOR: return "Motorwicklung ueberhitzt! Leistung gedrosselt.";
        case FAULT_GATE_DRIVER_OVER_VOLTAGE: return "Hilfsspannung des Treibers zu hoch.";
        case FAULT_GATE_DRIVER_UNDER_VOLTAGE: return "Hilfsspannung des Treibers zu niedrig.";
        case FAULT_MCU_UNDER_VOLTAGE: return "3.3V Versorgungsspannung der MCU eingebrochen.";
        case FAULT_BOOTING_FROM_WATCHDOG_RESET: return "VESC-Firmware nach Watchdog-Timeout neu gestartet.";
        case FAULT_ENCODER_SPI: return "Kommunikation zum SPI-Winkelgeber unterbrochen.";
        case FAULT_ENCODER_SINCOS_BELOW_MIN_AMPLITUDE: return "Sin/Cos Winkelsignal zu schwach.";
        case FAULT_ENCODER_SINCOS_ABOVE_MAX_AMPLITUDE: return "Sin/Cos Winkelsignal uebersteuert.";
        case FAULT_FLASH_CORRUPTION: return "VESC interner Flash-Speicher beschaedigt.";
        case FAULT_HIGH_OFFSET_CURRENT_SENSOR_1: return "Shunt-Messverstaerker Phase 1 Nullpunkt-Fehler.";
        case FAULT_HIGH_OFFSET_CURRENT_SENSOR_2: return "Shunt-Messverstaerker Phase 2 Nullpunkt-Fehler.";
        case FAULT_HIGH_OFFSET_CURRENT_SENSOR_3: return "Shunt-Messverstaerker Phase 3 Nullpunkt-Fehler.";
        case FAULT_UNBALANCED_CURRENTS: return "Unsymmetrische Stroeme in Motorwicklungen gemessen.";
        case FAULT_BRK: return "Externer Notaus / Bremsabschaltung aktiv.";
        case FAULT_RESOLVER_LOT: return "Resolver hat Phasen-Synchronisation verloren.";
        case FAULT_RESOLVER_DOS: return "Resolver-Signalpegel degradiert.";
        case FAULT_RESOLVER_LOS: return "Verbindung zum Motor-Resolver abgerissen.";
        case FAULT_FLASH_CORRUPTION_APP_CFG: return "App-Konfiguration im Flash ungueltig.";
        case FAULT_FLASH_CORRUPTION_MC_CFG: return "Motor-Konfiguration im Flash ungueltig.";
        case FAULT_ENCODER_NO_MAGNET: return "Magnetfeld am Winkelsensor fehlt oder zu schwach.";
        default: return "Unbekannter Fehlercode aufgetreten.";
    }
}
