#pragma once

#include <Arduino.h>
#include "dash_types.h"

struct PortalTelemetrySnapshot {
    float speed_kmh;
    float power_watts;
    float voltage;
    float current_amps;
    float phase_amps;
    float battery_pct;
};

class CaptivePortal {
public:
    static CaptivePortal& instance();

    void begin();
    void updateTelemetry(const DashTelemetry &telemetry);
    PortalTelemetrySnapshot getTelemetrySnapshot() const { return _telemetry_snapshot; }

private:
    CaptivePortal();
    void core0Loop();
    static void core0TaskEntry(void *param);

    PortalTelemetrySnapshot _telemetry_snapshot;
    bool _initialized;
};
