#pragma once

#include "dash_types.h"
#include <stdint.h>

void handleSerialCli(DashTelemetry &telemetry, uint32_t now);

#if !defined(NATIVE_SIMULATOR)
uint32_t getRealtimeTaskStackHighWaterMark();
#else
inline uint32_t getRealtimeTaskStackHighWaterMark() { return 0; }
#endif
