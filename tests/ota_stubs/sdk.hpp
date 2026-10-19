#pragma once

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>
#include <sdkconfig.h>

using esp_err_t = int;
constexpr esp_err_t ESP_OK = 0, ESP_FAIL = -1, ESP_ERR_NO_MEM = 0x101;
constexpr esp_err_t ESP_ERR_INVALID_STATE = 0x103, ESP_ERR_INVALID_SIZE = 0x104;
constexpr esp_err_t ESP_ERR_INVALID_VERSION = 0x10a, ESP_ERR_HTTPS_OTA_IN_PROGRESS = 0x9001;
inline const char *esp_err_to_name(esp_err_t) { return "fake error"; }
using TickType_t = uint32_t;
using EventBits_t = uint32_t;
using UBaseType_t = unsigned;
using BaseType_t = int;
using TaskHandle_t = void *;
using EventGroupHandle_t = void *;
constexpr BaseType_t pdTRUE = 1, pdFALSE = 0, pdPASS = 1;
constexpr TickType_t configTICK_RATE_HZ = 100, portMAX_DELAY = UINT32_MAX;
constexpr EventBits_t BIT0 = 1;
#define pdMS_TO_TICKS(ms) (static_cast<TickType_t>((ms) * configTICK_RATE_HZ / 1000))

struct esp_app_desc_t { char version[32] = "1.0.0"; };
inline esp_err_t esp_crt_bundle_attach(void *) { return ESP_OK; }
struct esp_http_client_config_t {
    const char *url = nullptr;
    const char *user_agent = nullptr;
    esp_err_t (*crt_bundle_attach)(void *) = nullptr;
    int timeout_ms = 0;
    bool keep_alive_enable = false;
    int max_redirection_count = 0;
    int buffer_size_tx = 0;
};
using esp_https_ota_handle_t = void *;
struct esp_https_ota_config_t { const esp_http_client_config_t *http_config = nullptr; };
struct esp_partition_t {};
using esp_ota_img_states_t = int;
constexpr esp_ota_img_states_t ESP_OTA_IMG_PENDING_VERIFY = 1, ESP_OTA_IMG_VALID = 2;

namespace fake {
struct Stop {};
struct Step { int bytes; esp_err_t result; };
struct Runtime {
    void (*task)(void *) = nullptr;
    void *context = nullptr;
    EventBits_t bits = 0;
    unsigned notify_gives = 0, notify_takes = 0;
    std::vector<TickType_t> waits, delays;
    std::vector<std::string> trace;
    std::string url;
    esp_app_desc_t current, remote;
    esp_err_t begin_result = ESP_OK, header_result = ESP_OK, finish_result = ESP_OK;
    int descriptor_bytes = 512, bytes = 0, total = 100000;
    bool alive = false, complete = true;
    unsigned aborts = 0, finishes = 0, restarts = 0;
    std::vector<Step> steps{{100000, ESP_OK}};
    size_t next_step = 0;
    esp_ota_img_states_t image_state = ESP_OTA_IMG_VALID;
    esp_err_t state_result = ESP_OK, confirm_result = ESP_OK;
    unsigned confirmations = 0;
    Runtime() { std::strcpy(remote.version, "1.1.0"); }
};
inline Runtime runtime;
inline void require(bool condition, const std::string &message)
{
    if (!condition) throw std::runtime_error(message);
}
inline void handle(esp_https_ota_handle_t value)
{
    require(value == &runtime && runtime.alive, "OTA handle used after finish/abort or before begin");
}
} // namespace fake

#define ESP_LOGI(tag, ...) ((void)(tag))
#define ESP_LOGW(tag, ...) ((void)(tag))
#define ESP_LOGE(tag, ...) ((void)(tag))

inline EventGroupHandle_t xEventGroupCreate() { return &fake::runtime; }
inline void vEventGroupDelete(EventGroupHandle_t) {}
inline EventBits_t xEventGroupSetBits(EventGroupHandle_t, EventBits_t bits)
{ return fake::runtime.bits |= bits; }
inline EventBits_t xEventGroupClearBits(EventGroupHandle_t, EventBits_t bits)
{ return fake::runtime.bits &= ~bits; }
inline EventBits_t xEventGroupGetBits(EventGroupHandle_t) { return fake::runtime.bits; }
inline EventBits_t xEventGroupWaitBits(EventGroupHandle_t, EventBits_t bits, BaseType_t,
                                      BaseType_t, TickType_t)
{
    fake::require((fake::runtime.bits & bits) == bits, "test task must be online");
    return fake::runtime.bits;
}
inline BaseType_t xTaskCreate(void (*task)(void *), const char *, uint32_t, void *context,
                              UBaseType_t, TaskHandle_t *out)
{
    fake::runtime.task = task;
    fake::runtime.context = context;
    *out = &fake::runtime;
    return pdPASS;
}
inline void xTaskNotifyGive(TaskHandle_t) { ++fake::runtime.notify_gives; }
inline uint32_t ulTaskNotifyTake(BaseType_t, TickType_t wait)
{
    fake::runtime.waits.push_back(wait);
    if (++fake::runtime.notify_takes > 2) throw fake::Stop{};
    return 0;
}
inline void vTaskDelay(TickType_t ticks)
{
    fake::runtime.trace.push_back("delay");
    fake::runtime.delays.push_back(ticks);
}
inline esp_err_t esp_https_ota_begin(const esp_https_ota_config_t *config, esp_https_ota_handle_t *out)
{
    auto &r = fake::runtime;
    r.trace.push_back("begin");
    fake::require(!r.alive, "begin leaked previous handle");
    r.url = config->http_config->url;
    if (r.begin_result != ESP_OK) return r.begin_result;
    r.alive = true;
    *out = &r;
    return ESP_OK;
}
inline esp_err_t esp_https_ota_get_img_desc(esp_https_ota_handle_t handle, esp_app_desc_t *out)
{
    fake::handle(handle);
    auto &r = fake::runtime;
    r.trace.push_back("descriptor");
    r.bytes = r.descriptor_bytes;
    *out = r.remote;
    return r.header_result;
}
inline const esp_app_desc_t *esp_app_get_description() { return &fake::runtime.current; }
// SDK 5.5 returns signed int, including -1 for unavailable/chunked lengths.
inline int esp_https_ota_get_image_len_read(esp_https_ota_handle_t handle)
{ fake::handle(handle); return fake::runtime.bytes; }
inline int esp_https_ota_get_image_size(esp_https_ota_handle_t handle)
{ fake::handle(handle); return fake::runtime.total; }
inline esp_err_t esp_https_ota_perform(esp_https_ota_handle_t handle)
{
    fake::handle(handle);
    auto &r = fake::runtime;
    fake::require(r.next_step < r.steps.size(), "perform exceeded finite script");
    r.trace.push_back("perform");
    const auto step = r.steps[r.next_step++];
    r.bytes = step.bytes;
    return step.result;
}
inline bool esp_https_ota_is_complete_data_received(esp_https_ota_handle_t handle)
{
    fake::handle(handle);
    fake::runtime.trace.push_back("complete");
    return fake::runtime.complete;
}
inline esp_err_t esp_https_ota_abort(esp_https_ota_handle_t handle)
{
    fake::handle(handle);
    auto &r = fake::runtime;
    r.trace.push_back("abort");
    ++r.aborts;
    r.alive = false;
    return ESP_OK;
}
inline esp_err_t esp_https_ota_finish(esp_https_ota_handle_t handle)
{
    fake::handle(handle);
    auto &r = fake::runtime;
    r.trace.push_back("finish");
    ++r.finishes;
    r.alive = false; // finish releases the SDK handle on both success and failure.
    return r.finish_result;
}
inline void esp_restart()
{
    fake::runtime.trace.push_back("restart");
    ++fake::runtime.restarts;
}
inline const esp_partition_t *esp_ota_get_running_partition()
{ static esp_partition_t partition; return &partition; }
inline esp_err_t esp_ota_get_state_partition(const esp_partition_t *, esp_ota_img_states_t *state)
{ *state = fake::runtime.image_state; return fake::runtime.state_result; }
inline esp_err_t esp_ota_mark_app_valid_cancel_rollback()
{ ++fake::runtime.confirmations; return fake::runtime.confirm_result; }
