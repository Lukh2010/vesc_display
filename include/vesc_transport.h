#pragma once

#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <deque>
#include <mutex>
#include <algorithm>
#include <Arduino.h>

#if !defined(NATIVE_SIMULATOR)
#include <HardwareSerial.h>
#endif

/**
 * @brief Transport-independent byte-stream interface for VESC packet communication.
 * Allows VescHandler to communicate with real hardware UART (Serial1 on ESP32)
 * or virtual in-memory loopback / network transports in the simulator.
 */
class IVescTransport {
public:
    virtual ~IVescTransport() = default;

    virtual int available() = 0;
    virtual int read() = 0;
    virtual size_t readBytes(uint8_t *buffer, size_t length) {
        size_t n = 0;
        while (n < length && available() > 0) {
            int c = read();
            if (c < 0) break;
            buffer[n++] = (uint8_t)c;
        }
        return n;
    }
    virtual size_t write(uint8_t b) = 0;
    virtual size_t write(const uint8_t *buffer, size_t size) = 0;
    virtual void flush() {}
};

#if !defined(NATIVE_SIMULATOR)
/**
 * @brief Production ESP32 Hardware UART Transport wrapper.
 */
class HardwareSerialTransport : public IVescTransport {
public:
    explicit HardwareSerialTransport(HardwareSerial &serial) : _serial(serial) {}

    int available() override {
        return _serial.available();
    }

    int read() override {
        return _serial.read();
    }

    size_t readBytes(uint8_t *buffer, size_t length) override {
        return _serial.readBytes(buffer, length);
    }

    size_t write(uint8_t b) override {
        return _serial.write(b);
    }

    size_t write(const uint8_t *buffer, size_t size) override {
        return _serial.write(buffer, size);
    }

    void flush() override {
        _serial.flush();
    }

private:
    HardwareSerial &_serial;
};
#endif

/**
 * @brief Thread-safe in-memory loopback endpoint.
 */
class VescLoopbackEndpoint : public IVescTransport {
public:
    VescLoopbackEndpoint(std::deque<uint8_t> &rx_queue, std::deque<uint8_t> &tx_queue, std::mutex &mtx)
        : _rx(rx_queue), _tx(tx_queue), _mtx(mtx) {}

    int available() override {
        std::lock_guard<std::mutex> lock(_mtx);
        return (int)_rx.size();
    }

    int read() override {
        std::lock_guard<std::mutex> lock(_mtx);
        if (_rx.empty()) return -1;
        uint8_t b = _rx.front();
        _rx.pop_front();
        return b;
    }

    size_t readBytes(uint8_t *buffer, size_t length) override {
        std::lock_guard<std::mutex> lock(_mtx);
        size_t n = std::min(length, _rx.size());
        for (size_t i = 0; i < n; i++) {
            buffer[i] = _rx.front();
            _rx.pop_front();
        }
        return n;
    }

    size_t write(uint8_t b) override {
        std::lock_guard<std::mutex> lock(_mtx);
        if (_tx.size() >= MAX_BUFFER_SIZE) return 0;
        _tx.push_back(b);
        return 1;
    }

    size_t write(const uint8_t *buffer, size_t size) override {
        std::lock_guard<std::mutex> lock(_mtx);
        size_t written = 0;
        for (size_t i = 0; i < size; i++) {
            if (_tx.size() >= MAX_BUFFER_SIZE) break;
            _tx.push_back(buffer[i]);
            written++;
        }
        return written;
    }

    void flush() override {}

    void clear() {
        std::lock_guard<std::mutex> lock(_mtx);
        _rx.clear();
    }

private:
    static constexpr size_t MAX_BUFFER_SIZE = 65536;
    std::deque<uint8_t> &_rx;
    std::deque<uint8_t> &_tx;
    std::mutex &_mtx;
};

/**
 * @brief Bidirectional in-memory loopback transport connecting two endpoints:
 * Side A (e.g. VescHandler) and Side B (e.g. VirtualVesc).
 */
class VescLoopbackTransport {
public:
    VescLoopbackTransport()
        : _sideA(_b_to_a, _a_to_b, _mtx),
          _sideB(_a_to_b, _b_to_a, _mtx) {}

    IVescTransport& getSideA() { return _sideA; }
    IVescTransport& getSideB() { return _sideB; }

    void clear() {
        std::lock_guard<std::mutex> lock(_mtx);
        _a_to_b.clear();
        _b_to_a.clear();
    }

private:
    std::deque<uint8_t> _a_to_b;
    std::deque<uint8_t> _b_to_a;
    std::mutex _mtx;
    VescLoopbackEndpoint _sideA;
    VescLoopbackEndpoint _sideB;
};
