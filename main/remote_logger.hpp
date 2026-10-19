#pragma once

#include <atomic>
#include <cstdarg>
#include <cstdint>

#include <esp_err.h>

// Static lifetime; one process-wide log hook with no teardown.
class RemoteLogger {
public:
    struct Config {
        const char *host; // Copied by start(); hostname or literal IPv4, no scheme.
        uint16_t port;
        uint32_t health_interval_seconds; // Zero disables periodic health only.
    };

    explicit RemoteLogger(Config config);
    RemoteLogger(const RemoteLogger &) = delete;
    RemoteLogger &operator=(const RemoteLogger &) = delete;
    RemoteLogger(RemoteLogger &&) = delete;
    RemoteLogger &operator=(RemoteLogger &&) = delete;

    // Returns errors without logging or aborting. A failed start can be retried.
    esp_err_t start();
    // Thread-safe task-context setters; no networking.
    void set_network_connected(bool connected);
    void set_fan_state(uint8_t speed, bool rotation);

private:
    struct Runtime;
    static Runtime runtime_;
    static int log_hook(const char *format, va_list args);
    static void task_entry(void *context);
    void task();

    Config config_;
    // Bit 0 = connected; remaining bits change on every connection transition so
    // a disconnect/reconnect between worker wakeups still triggers health.
    std::atomic<uint32_t> network_state_{0};
    std::atomic<uint32_t> fan_state_{0};
    std::atomic<uint32_t> dropped_{0};
    std::atomic<uint32_t> udp_send_failures_{0};
};
