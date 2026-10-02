#include "ota_manager.hpp"

#include <climits>
#include <cstdlib>
#include <cstring>

#include <esp_app_desc.h>
#include <esp_crt_bundle.h>
#include <esp_http_client.h>
#include <esp_https_ota.h>
#include <esp_log.h>
#include <esp_ota_ops.h>
#include <esp_system.h>

namespace {
constexpr char kTag[] = "fan_ota";
constexpr uint32_t kTaskStackSize = 8192;
constexpr UBaseType_t kTaskPriority = 5;

TickType_t seconds_to_ticks(uint32_t seconds)
{
    return static_cast<TickType_t>((static_cast<uint64_t>(seconds) * configTICK_RATE_HZ));
}
} // namespace

OtaManager::OtaManager(Config config) : config_(config) {}

esp_err_t OtaManager::start()
{
    if (config_.url == nullptr || config_.url[0] == '\0') {
        ESP_LOGW(kTag, "OTA disabled: release URL is not configured");
        return ESP_OK;
    }
    if (events_ != nullptr) {
        return ESP_ERR_INVALID_STATE;
    }

    events_ = xEventGroupCreate();
    if (events_ == nullptr) {
        return ESP_ERR_NO_MEM;
    }

    if (xTaskCreate(task_entry, "fan_ota", kTaskStackSize, this, kTaskPriority, &task_) != pdPASS) {
        vEventGroupDelete(events_);
        events_ = nullptr;
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(kTag, "OTA enabled for %s", config_.url);
    return ESP_OK;
}

void OtaManager::set_network_connected(bool connected)
{
    if (events_ == nullptr) {
        return;
    }
    if (connected) {
        xEventGroupSetBits(events_, kNetworkConnected);
    } else {
        xEventGroupClearBits(events_, kNetworkConnected);
    }
}

esp_err_t OtaManager::request_check()
{
    if (task_ == nullptr || events_ == nullptr || check_in_progress_.load() ||
        update_in_progress_.load() || !ready_to_update() ||
        (xEventGroupGetBits(events_) & kNetworkConnected) == 0) {
        ESP_LOGW(kTag, "OTA check unavailable: fan must be off, commissioned, online, and not checking");
        return ESP_ERR_INVALID_STATE;
    }

    xTaskNotifyGive(task_);
    ESP_LOGI(kTag, "Manual OTA check requested");
    return ESP_OK;
}

bool OtaManager::update_in_progress() const
{
    return update_in_progress_.load();
}

esp_err_t OtaManager::confirm_running_image()
{
#if CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE
    const esp_partition_t *partition = esp_ota_get_running_partition();
    esp_ota_img_states_t state;
    esp_err_t err = esp_ota_get_state_partition(partition, &state);
    if (err == ESP_OK && state == ESP_OTA_IMG_PENDING_VERIFY) {
        ESP_LOGI(kTag, "Confirming firmware after successful startup");
        return esp_ota_mark_app_valid_cancel_rollback();
    }
#endif
    return ESP_OK;
}

void OtaManager::task_entry(void *context)
{
    static_cast<OtaManager *>(context)->task();
}

void OtaManager::task()
{
    uint32_t delay_seconds = config_.startup_delay_seconds;

    while (true) {
        ulTaskNotifyTake(pdTRUE, seconds_to_ticks(delay_seconds));
        xEventGroupWaitBits(events_, kNetworkConnected, pdFALSE, pdTRUE, portMAX_DELAY);
        ulTaskNotifyTake(pdTRUE, 0);

        if (!ready_to_update()) {
            delay_seconds = config_.retry_interval_seconds;
            continue;
        }

        check_in_progress_.store(true);
        esp_err_t err = check_for_update();
        check_in_progress_.store(false);
        if (err != ESP_OK) {
            ESP_LOGW(kTag, "OTA check failed: %s", esp_err_to_name(err));
            delay_seconds = config_.retry_interval_seconds;
        } else {
            delay_seconds = config_.check_interval_seconds;
        }
    }
}

esp_err_t OtaManager::check_for_update()
{
    esp_http_client_config_t http_config = {};
    http_config.url = config_.url;
    http_config.user_agent = "smart-fan-ota";
    http_config.crt_bundle_attach = esp_crt_bundle_attach;
    http_config.timeout_ms = 15000;
    http_config.keep_alive_enable = true;
    http_config.max_redirection_count = 5;
    // GitHub release assets redirect to long signed URLs.
    http_config.buffer_size_tx = 4096;

    esp_https_ota_config_t ota_config = {};
    ota_config.http_config = &http_config;

    esp_https_ota_handle_t ota_handle = nullptr;
    esp_err_t err = esp_https_ota_begin(&ota_config, &ota_handle);
    if (err != ESP_OK) {
        return err;
    }

    esp_app_desc_t remote_description = {};
    err = esp_https_ota_get_img_desc(ota_handle, &remote_description);
    if (err != ESP_OK) {
        esp_https_ota_abort(ota_handle);
        return err;
    }

    const esp_app_desc_t *current_description = esp_app_get_description();
    Version current_version = {};
    Version remote_version = {};
    if (!parse_version(current_description->version, current_version) ||
        !parse_version(remote_description.version, remote_version)) {
        ESP_LOGE(kTag, "Invalid version: current=%s remote=%s", current_description->version,
                 remote_description.version);
        esp_https_ota_abort(ota_handle);
        return ESP_ERR_INVALID_VERSION;
    }

    if (compare_versions(remote_version, current_version) <= 0) {
        ESP_LOGI(kTag, "Firmware is current: %s", current_description->version);
        esp_https_ota_abort(ota_handle);
        return ESP_OK;
    }

    update_in_progress_.store(true);
    if (!ready_to_update()) {
        update_in_progress_.store(false);
        esp_https_ota_abort(ota_handle);
        return ESP_ERR_INVALID_STATE;
    }

    ESP_LOGI(kTag, "Installing firmware %s (current %s)", remote_description.version,
             current_description->version);

    do {
        err = esp_https_ota_perform(ota_handle);
    } while (err == ESP_ERR_HTTPS_OTA_IN_PROGRESS);

    if (err != ESP_OK || !esp_https_ota_is_complete_data_received(ota_handle)) {
        esp_https_ota_abort(ota_handle);
        update_in_progress_.store(false);
        return err == ESP_OK ? ESP_ERR_INVALID_SIZE : err;
    }

    err = esp_https_ota_finish(ota_handle);
    if (err != ESP_OK) {
        update_in_progress_.store(false);
        return err;
    }

    ESP_LOGI(kTag, "Firmware installed; rebooting");
    esp_restart();
    return ESP_OK;
}

bool OtaManager::ready_to_update() const
{
    return config_.ready_callback == nullptr || config_.ready_callback(config_.callback_context);
}

bool OtaManager::parse_version(const char *text, Version &version)
{
    if (text == nullptr) {
        return false;
    }

    const char *cursor = text;
    if (*cursor == 'v') {
        ++cursor;
    }

    uint32_t *parts[] = {&version.major, &version.minor, &version.patch};
    for (size_t index = 0; index < 3; ++index) {
        char *end = nullptr;
        unsigned long value = std::strtoul(cursor, &end, 10);
        if (end == cursor || value > UINT_MAX) {
            return false;
        }
        *parts[index] = static_cast<uint32_t>(value);
        cursor = end;

        if (index < 2) {
            if (*cursor != '.') {
                return false;
            }
            ++cursor;
        }
    }

    version.prerelease = *cursor == '-';
    return *cursor == '\0' || version.prerelease || *cursor == '+';
}

int OtaManager::compare_versions(const Version &left, const Version &right)
{
    if (left.major != right.major) {
        return left.major > right.major ? 1 : -1;
    }
    if (left.minor != right.minor) {
        return left.minor > right.minor ? 1 : -1;
    }
    if (left.patch != right.patch) {
        return left.patch > right.patch ? 1 : -1;
    }
    if (left.prerelease != right.prerelease) {
        return left.prerelease ? -1 : 1;
    }
    return 0;
}
