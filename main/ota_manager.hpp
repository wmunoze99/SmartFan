#pragma once

#include "ota_progress.hpp"

#include <atomic>
#include <cstdint>
#include <string>

#include <esp_err.h>
#include <freertos/FreeRTOS.h>
#include <freertos/event_groups.h>
#include <freertos/task.h>

class OtaManager {
public:
    using ReadyCallback = bool (*)(void *context);
    using ProgressCallback = void (*)(const OtaProgress &, void *context);

    struct Config {
        const char *url;
        uint32_t startup_delay_seconds;
        uint32_t check_interval_seconds;
        uint32_t retry_interval_seconds;
        ReadyCallback ready_callback;
        void *callback_context;
        ProgressCallback progress_callback = nullptr;
    };

    explicit OtaManager(Config config);

    esp_err_t start();
    esp_err_t request_check();
    void set_network_connected(bool connected);
    bool update_in_progress() const;
    esp_err_t confirm_running_image();

private:
    struct Version {
        uint32_t major;
        uint32_t minor;
        uint32_t patch;
        bool prerelease;
    };

    static constexpr EventBits_t kNetworkConnected = BIT0;

    static void task_entry(void *context);
    static bool parse_version(const char *text, Version &version);
    static int compare_versions(const Version &left, const Version &right);

    void task();
    esp_err_t check_for_update();
    bool ready_to_update() const;
    void notify_progress(const OtaProgress &progress) const;

    Config config_;
    std::string release_url_;
    EventGroupHandle_t events_ = nullptr;
    TaskHandle_t task_ = nullptr;
    std::atomic_bool update_in_progress_ = false;
    std::atomic_bool check_in_progress_ = false;
};
