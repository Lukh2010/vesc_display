#pragma once

#ifndef DASH_DEBUG_LOGS
#define DASH_DEBUG_LOGS 0 // 0 = stumm fuer Release/geringerer Serial-Overhead, 1 = ausfuehrliche Debug-Logs
#endif

#if DASH_DEBUG_LOGS
  #if defined(NATIVE_SIMULATOR)
    #include <stdio.h>
    #define DASH_LOGF(fmt, ...) printf(fmt, ##__VA_ARGS__)
    #define DASH_LOGLN(msg)     printf("%s\n", (msg))
  #else
    #include <Arduino.h>
    #define DASH_LOGF(fmt, ...) Serial.printf(fmt, ##__VA_ARGS__)
    #define DASH_LOGLN(msg)     Serial.println(msg)
  #endif
#else
  #define DASH_LOGF(fmt, ...) ((void)0)
  #define DASH_LOGLN(msg)     ((void)0)
#endif
