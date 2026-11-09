#include "vesc_tcp_server.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>

VescTcpServer::VescTcpServer()
    : _server_fd(-1),
      _client_fd(-1),
      _port(6510),
      _running(false) {}

VescTcpServer::~VescTcpServer() {
    stop();
}

bool VescTcpServer::start(uint16_t port) {
    if (_running) return true;

    _port = port;
    _server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (_server_fd < 0) {
        printf("[VESC_TCP] Failed to create socket: %s\n", strerror(errno));
        return false;
    }

    int opt = 1;
    setsockopt(_server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    // Make socket non-blocking
    int flags = fcntl(_server_fd, F_GETFL, 0);
    fcntl(_server_fd, F_SETFL, flags | O_NONBLOCK);

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(_port);

    if (bind(_server_fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        printf("[VESC_TCP] Failed to bind to port %d: %s (external VESC Tool disabled)\n", _port, strerror(errno));
        close(_server_fd);
        _server_fd = -1;
        return false;
    }

    if (listen(_server_fd, 2) < 0) {
        printf("[VESC_TCP] Failed to listen: %s\n", strerror(errno));
        close(_server_fd);
        _server_fd = -1;
        return false;
    }

    _running = true;
    printf("[VESC_TCP] Virtual VESC TCP Server listening on port %d for external VESC Tool.\n", _port);
    return true;
}

void VescTcpServer::stop() {
    if (_client_fd >= 0) {
        close(_client_fd);
        _client_fd = -1;
    }
    if (_server_fd >= 0) {
        close(_server_fd);
        _server_fd = -1;
    }
    _tcp_parser.reset();
    _running = false;
}

void VescTcpServer::update(VirtualVesc &vesc) {
    if (!_running || _server_fd < 0) return;

    // Check for incoming client connection
    if (_client_fd < 0) {
        struct sockaddr_in client_addr;
        socklen_t addr_len = sizeof(client_addr);
        int fd = accept(_server_fd, (struct sockaddr*)&client_addr, &addr_len);
        if (fd >= 0) {
            int flags = fcntl(fd, F_GETFL, 0);
            fcntl(fd, F_SETFL, flags | O_NONBLOCK);
            int nodelay = 1;
            setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay));
            _client_fd = fd;
            _tcp_parser.reset();
            printf("[VESC_TCP] External VESC client connected on port %d.\n", _port);
        }
    }

    // Process client data
    if (_client_fd >= 0) {
        uint8_t rx_buf[512];
        ssize_t n = recv(_client_fd, rx_buf, sizeof(rx_buf), 0);
        if (n > 0) {
            _tcp_parser.processBuffer(rx_buf, (size_t)n, [this, &vesc](const uint8_t *payload, size_t len) {
                printf("[VESC_TCP RX] Received command ID: %u (len: %zu)\n", payload[0], len);
                vesc.handlePacket(payload, len, [this](const uint8_t *resp_packet, size_t resp_len) {
                    if (_client_fd >= 0 && resp_packet && resp_len > 0) {
                        ::send(_client_fd, resp_packet, resp_len, MSG_NOSIGNAL);
                    }
                });
            });
        } else if (n == 0 || (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK)) {
            printf("[VESC_TCP] External VESC client disconnected.\n");
            close(_client_fd);
            _client_fd = -1;
            _tcp_parser.reset();
        }
    }
}
