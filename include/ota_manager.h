#pragma once

#include <Arduino.h>

#if !defined(NATIVE_SIMULATOR)
#include <WiFi.h>
#include <WebServer.h>
#include <Update.h>
#include <ArduinoOTA.h>
#endif

class OtaManager {
public:
    static OtaManager& instance();

    void begin();
    void update();
    void stop();

    bool isRunning() const { return _running; }
    bool isUpdating() const { return _is_updating; }
    int  getProgressPct() const;
    size_t getBytesWritten() const { return _bytes_written; }
    size_t getTotalBytes() const { return _total_bytes; }
    const char* getStatusText() const { return _status_text; }

    // Simulation helpers
    void setSimProgress(bool updating, int pct);

private:
    OtaManager();
    ~OtaManager() = default;

    bool _running;
    bool _is_updating;
    size_t _bytes_written;
    size_t _total_bytes;
    char _status_text[64];

#if !defined(NATIVE_SIMULATOR)
    WebServer _server;
    void setupRoutes();
#endif
};
