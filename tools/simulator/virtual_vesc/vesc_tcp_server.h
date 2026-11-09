#pragma once

#include <stdint.h>
#include <stddef.h>
#include <atomic>
#include "virtual_vesc.h"
#include "vesc_packet.h"

/**
 * @brief Non-blocking TCP bridge for external VESC Tool connections.
 * Listens on port 6510 (standard VESC TCP port) and bridges packets
 * to/from the VirtualVesc instance.
 */
class VescTcpServer {
public:
    VescTcpServer();
    ~VescTcpServer();

    bool start(uint16_t port = 6510);
    void stop();
    void update(VirtualVesc &vesc);

    bool isRunning() const { return _running; }
    bool isClientConnected() const { return _client_fd >= 0; }
    uint16_t getPort() const { return _port; }

private:
    int _server_fd;
    int _client_fd;
    uint16_t _port;
    std::atomic<bool> _running;
    VescPacketParser _tcp_parser;
};
