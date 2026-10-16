#pragma once

// Only the SDK surface used by status_display.cpp. No display policy lives here.
#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>
#include <limits>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

using esp_err_t = int;
constexpr esp_err_t ESP_OK = 0;
constexpr esp_err_t ESP_FAIL = -1;
constexpr esp_err_t ESP_ERR_NO_MEM = -2;
inline const char *esp_err_to_name(esp_err_t) { return "ESP_FAIL"; }
using gpio_num_t = int;
using i2c_master_bus_handle_t = void *;
using esp_lcd_panel_io_handle_t = void *;
using esp_lcd_panel_handle_t = void *;
using QueueHandle_t = void *;
using SemaphoreHandle_t = void *;
using TaskHandle_t = void *;
using TickType_t = uint32_t;
using BaseType_t = int;
constexpr BaseType_t pdTRUE = 1, pdFALSE = 0, pdPASS = 1;
constexpr TickType_t configTICK_RATE_HZ = 1000;
constexpr TickType_t portMAX_DELAY = std::numeric_limits<TickType_t>::max();
constexpr int I2C_NUM_0 = 0, I2C_CLK_SRC_DEFAULT = 0;

struct i2c_master_bus_config_t {
    int i2c_port, sda_io_num, scl_io_num, clk_source, glitch_ignore_cnt;
    struct { bool enable_internal_pullup; } flags;
};
struct esp_lcd_panel_io_i2c_config_t {
    int dev_addr, control_phase_bytes, dc_bit_offset, lcd_cmd_bits, lcd_param_bits, scl_speed_hz;
};
struct esp_lcd_panel_ssd1306_config_t { int height; };
struct esp_lcd_panel_dev_config_t {
    int reset_gpio_num, bits_per_pixel;
    void *vendor_config;
};
struct esp_qrcode_config_t {
    void (*display_func_with_cb)(const uint8_t *, void *) = nullptr;
    void *user_data = nullptr;
    int max_qrcode_version = 0, qrcode_ecc_level = 0;
};
constexpr int ESP_QRCODE_ECC_LOW = 0;
#define ESP_QRCODE_CONFIG_DEFAULT() esp_qrcode_config_t{}

namespace fake {
struct Stop {};
using Frame = std::array<uint8_t, 1024>;
struct Operation {
    std::string kind;
    uint64_t at;
    Frame frame{};
};
struct Event { uint64_t at; std::function<void()> action; };
struct Wait { uint64_t at; TickType_t ticks; };
struct Runtime {
    uint64_t now = 0;
    std::vector<Event> events;
    size_t next_event = 0;
    std::vector<uint8_t> mailbox;
    size_t item_size = 0;
    void (*task)(void *) = nullptr;
    void *context = nullptr;
    std::vector<Wait> waits;
    std::vector<Operation> operations;
    std::vector<std::string> warnings;
    std::vector<std::string> qr_payloads;
    Frame sh_frame{};
    int page = -1;
    int fail_off = 0, fail_on = 0;
};
inline Runtime runtime;
inline void require(bool condition, const std::string &message)
{
    if (!condition) throw std::runtime_error(message);
}
inline esp_err_t power(const char *api, bool on)
{
    runtime.operations.push_back({std::string(api) + (on ? "_on" : "_off"), runtime.now});
    int &failures = on ? runtime.fail_on : runtime.fail_off;
    if (failures > 0) { --failures; return ESP_FAIL; }
    return ESP_OK;
}
inline void frame(const Frame &pixels)
{
    runtime.operations.push_back({"frame", runtime.now, pixels});
}
template <typename... Args> void warning(const char *format, Args... args)
{
    if constexpr (sizeof...(args) == 0) {
        runtime.warnings.emplace_back(format);
    } else {
        char message[256];
        std::snprintf(message, sizeof(message), format, args...);
        runtime.warnings.emplace_back(message);
    }
}
} // namespace fake

#define ESP_LOGW(tag, ...) fake::warning(__VA_ARGS__)
#define ESP_LOGE(tag, ...) fake::warning(__VA_ARGS__)
#define ESP_LOGI(tag, ...) ((void)(tag))
#define ESP_RETURN_ON_ERROR(expression, tag, ...) do { \
    const esp_err_t result = (expression); \
    if (result != ESP_OK) { ESP_LOGE(tag, __VA_ARGS__); return result; } \
} while (false)

inline esp_err_t i2c_new_master_bus(const i2c_master_bus_config_t *, i2c_master_bus_handle_t *out)
{ *out = &fake::runtime; return ESP_OK; }
inline esp_err_t i2c_master_probe(i2c_master_bus_handle_t, uint8_t, int) { return ESP_OK; }
inline esp_err_t i2c_del_master_bus(i2c_master_bus_handle_t) { return ESP_OK; }
inline esp_err_t esp_lcd_new_panel_io_i2c(i2c_master_bus_handle_t,
    const esp_lcd_panel_io_i2c_config_t *, esp_lcd_panel_io_handle_t *out)
{ *out = &fake::runtime; return ESP_OK; }
inline esp_err_t esp_lcd_new_panel_ssd1306(esp_lcd_panel_io_handle_t,
    const esp_lcd_panel_dev_config_t *, esp_lcd_panel_handle_t *out)
{ *out = &fake::runtime; return ESP_OK; }
inline esp_err_t esp_lcd_panel_reset(esp_lcd_panel_handle_t) { return ESP_OK; }
inline esp_err_t esp_lcd_panel_init(esp_lcd_panel_handle_t) { return ESP_OK; }
inline esp_err_t esp_lcd_panel_del(esp_lcd_panel_handle_t) { return ESP_OK; }
inline esp_err_t esp_lcd_panel_io_del(esp_lcd_panel_io_handle_t) { return ESP_OK; }
inline esp_err_t esp_lcd_panel_disp_on_off(esp_lcd_panel_handle_t, bool on)
{ return fake::power("ssd", on); }
inline esp_err_t esp_lcd_panel_draw_bitmap(esp_lcd_panel_handle_t,
    int x0, int y0, int x1, int y1, const void *pixels)
{
    fake::require(x0 == 0 && y0 == 0 && x1 == 128 && y1 == 64, "unexpected SSD1306 frame bounds");
    fake::Frame frame;
    std::memcpy(frame.data(), pixels, frame.size());
    fake::frame(frame);
    return ESP_OK;
}
inline esp_err_t esp_lcd_panel_io_tx_param(esp_lcd_panel_io_handle_t,
    int command, const void *data, size_t size)
{
    if (command == 0xae || command == 0xaf) {
        fake::require(data == nullptr && size == 0, "power command must not carry parameters");
        return fake::power("sh", command == 0xaf);
    }
    fake::require(command == -1 && data != nullptr, "unexpected SH1106 command");
    const auto *bytes = static_cast<const uint8_t *>(data);
    // Initialization is a command sequence; redraws use page address then page data.
    if (size == 3) {
        fake::runtime.page = bytes[0] - 0xb0;
        fake::require(fake::runtime.page >= 0 && fake::runtime.page < 8 &&
                      bytes[1] == 2 && bytes[2] == 0x10, "unexpected SH1106 page address");
    }
    return ESP_OK;
}
inline esp_err_t esp_lcd_panel_io_tx_color(esp_lcd_panel_io_handle_t,
    int command, const void *data, size_t size)
{
    auto &r = fake::runtime;
    fake::require(command == -1 && size == 128 && r.page >= 0, "unexpected SH1106 page data");
    std::memcpy(r.sh_frame.data() + r.page * 128, data, size);
    if (r.page == 7) fake::frame(r.sh_frame);
    r.page = -1;
    return ESP_OK;
}
inline SemaphoreHandle_t xSemaphoreCreateMutex() { return &fake::runtime; }
inline BaseType_t xSemaphoreTake(SemaphoreHandle_t, TickType_t) { return pdTRUE; }
inline BaseType_t xSemaphoreGive(SemaphoreHandle_t) { return pdTRUE; }
inline void vSemaphoreDelete(SemaphoreHandle_t) {}
inline QueueHandle_t xQueueCreate(unsigned length, unsigned item_size)
{
    fake::require(length == 1, "harness models the production one-slot overwrite queue");
    fake::runtime.item_size = item_size;
    return &fake::runtime;
}
inline BaseType_t xQueueOverwrite(QueueHandle_t, const void *item)
{
    auto &r = fake::runtime;
    const auto *bytes = static_cast<const uint8_t *>(item);
    r.mailbox.assign(bytes, bytes + r.item_size);
    return pdTRUE;
}
inline void vQueueDelete(QueueHandle_t) {}
inline BaseType_t xTaskCreate(void (*entry)(void *), const char *, unsigned,
    void *context, unsigned, TaskHandle_t *out)
{
    fake::runtime.task = entry;
    fake::runtime.context = context;
    *out = &fake::runtime;
    return pdPASS;
}
inline TickType_t xTaskGetTickCount() { return static_cast<TickType_t>(fake::runtime.now); }
inline BaseType_t xQueueReceive(QueueHandle_t, void *out, TickType_t wait)
{
    auto &r = fake::runtime;
    fake::require(r.waits.size() < 1000, "task busy-looped (1000 queue receives)");
    r.waits.push_back({r.now, wait});
    const auto deliver = [&]() {
        std::memcpy(out, r.mailbox.data(), r.item_size);
        r.mailbox.clear();
        return pdTRUE;
    };
    if (!r.mailbox.empty()) return deliver();
    const uint64_t deadline = wait == portMAX_DELAY ? UINT64_MAX : r.now + wait;
    while (r.next_event < r.events.size() && r.events[r.next_event].at <= deadline) {
        auto event = r.events[r.next_event++];
        fake::require(event.at >= r.now, "events must be chronological");
        r.now = event.at;
        event.action();
        if (!r.mailbox.empty()) return deliver();
    }
    if (wait == portMAX_DELAY) throw fake::Stop{};
    r.now = deadline;
    return pdFALSE;
}
inline esp_err_t esp_qrcode_generate(const esp_qrcode_config_t *config, const char *payload)
{
    fake::runtime.qr_payloads.emplace_back(payload);
    const uint8_t qr[] = {0};
    config->display_func_with_cb(qr, config->user_data);
    return ESP_OK;
}
inline int esp_qrcode_get_size(const uint8_t *) { return 21; }
inline bool esp_qrcode_get_module(const uint8_t *, int x, int y) { return (x + y) % 2 == 0; }
