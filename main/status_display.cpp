#include "status_display.hpp"

#include <algorithm>
#include <cstring>

#include <esp_check.h>
#include <esp_lcd_panel_ssd1306.h>
#include <esp_log.h>
#include <qrcode.h>

namespace {
constexpr char kTag[] = "status_display";

struct Glyph {
    char character;
    std::array<uint8_t, 5> columns;
};

constexpr Glyph kFont[] = {
    {' ', {0x00, 0x00, 0x00, 0x00, 0x00}}, {'-', {0x08, 0x08, 0x08, 0x08, 0x08}},
    {':', {0x00, 0x36, 0x36, 0x00, 0x00}}, {'?', {0x02, 0x01, 0x51, 0x09, 0x06}},
    {'0', {0x3e, 0x51, 0x49, 0x45, 0x3e}}, {'1', {0x00, 0x42, 0x7f, 0x40, 0x00}},
    {'2', {0x42, 0x61, 0x51, 0x49, 0x46}}, {'3', {0x21, 0x41, 0x45, 0x4b, 0x31}},
    {'4', {0x18, 0x14, 0x12, 0x7f, 0x10}}, {'5', {0x27, 0x45, 0x45, 0x45, 0x39}},
    {'6', {0x3c, 0x4a, 0x49, 0x49, 0x30}}, {'7', {0x01, 0x71, 0x09, 0x05, 0x03}},
    {'8', {0x36, 0x49, 0x49, 0x49, 0x36}}, {'9', {0x06, 0x49, 0x49, 0x29, 0x1e}},
    {'A', {0x7e, 0x11, 0x11, 0x11, 0x7e}}, {'B', {0x7f, 0x49, 0x49, 0x49, 0x36}},
    {'C', {0x3e, 0x41, 0x41, 0x41, 0x22}}, {'D', {0x7f, 0x41, 0x41, 0x22, 0x1c}},
    {'E', {0x7f, 0x49, 0x49, 0x49, 0x41}}, {'F', {0x7f, 0x09, 0x09, 0x09, 0x01}},
    {'G', {0x3e, 0x41, 0x49, 0x49, 0x7a}}, {'H', {0x7f, 0x08, 0x08, 0x08, 0x7f}},
    {'I', {0x00, 0x41, 0x7f, 0x41, 0x00}}, {'J', {0x20, 0x40, 0x41, 0x3f, 0x01}},
    {'K', {0x7f, 0x08, 0x14, 0x22, 0x41}}, {'L', {0x7f, 0x40, 0x40, 0x40, 0x40}},
    {'M', {0x7f, 0x02, 0x0c, 0x02, 0x7f}}, {'N', {0x7f, 0x04, 0x08, 0x10, 0x7f}},
    {'O', {0x3e, 0x41, 0x41, 0x41, 0x3e}}, {'P', {0x7f, 0x09, 0x09, 0x09, 0x06}},
    {'Q', {0x3e, 0x41, 0x51, 0x21, 0x5e}}, {'R', {0x7f, 0x09, 0x19, 0x29, 0x46}},
    {'S', {0x46, 0x49, 0x49, 0x49, 0x31}}, {'T', {0x01, 0x01, 0x7f, 0x01, 0x01}},
    {'U', {0x3f, 0x40, 0x40, 0x40, 0x3f}}, {'V', {0x1f, 0x20, 0x40, 0x20, 0x1f}},
    {'W', {0x3f, 0x40, 0x38, 0x40, 0x3f}}, {'X', {0x63, 0x14, 0x08, 0x14, 0x63}},
    {'Y', {0x07, 0x08, 0x70, 0x08, 0x07}}, {'Z', {0x61, 0x51, 0x49, 0x45, 0x43}},
};

const std::array<uint8_t, 5> &glyph_for(char character)
{
    if (character >= 'a' && character <= 'z') {
        character = static_cast<char>(character - ('a' - 'A'));
    }
    for (const auto &glyph : kFont) {
        if (glyph.character == character) {
            return glyph.columns;
        }
    }
    return kFont[3].columns;
}
} // namespace

StatusDisplay::StatusDisplay(Config config) : config_(config) {}

esp_err_t StatusDisplay::init()
{
    i2c_master_bus_config_t bus_config = {};
    bus_config.i2c_port = I2C_NUM_0;
    bus_config.sda_io_num = config_.sda_gpio;
    bus_config.scl_io_num = config_.scl_gpio;
    bus_config.clk_source = I2C_CLK_SRC_DEFAULT;
    bus_config.glitch_ignore_cnt = 7;
    bus_config.flags.enable_internal_pullup = true;
    ESP_RETURN_ON_ERROR(i2c_new_master_bus(&bus_config, &i2c_bus_), kTag, "Failed to create I2C bus");
    esp_err_t err = i2c_master_probe(i2c_bus_, config_.i2c_address, 100);
    if (err != ESP_OK) {
        ESP_LOGE(kTag, "SSD1306 not found at 0x%02x", config_.i2c_address);
        cleanup();
        return err;
    }

    esp_lcd_panel_io_i2c_config_t io_config = {};
    io_config.dev_addr = config_.i2c_address;
    io_config.control_phase_bytes = 1;
    io_config.dc_bit_offset = 6;
    io_config.lcd_cmd_bits = 8;
    io_config.lcd_param_bits = 8;
    io_config.scl_speed_hz = 400000;
    err = esp_lcd_new_panel_io_i2c(i2c_bus_, &io_config, &panel_io_);
    if (err != ESP_OK) {
        ESP_LOGE(kTag, "Failed to create OLED panel IO");
        cleanup();
        return err;
    }

    esp_lcd_panel_ssd1306_config_t ssd1306_config = {};
    ssd1306_config.height = kHeight;

    esp_lcd_panel_dev_config_t panel_config = {};
    panel_config.reset_gpio_num = -1;
    panel_config.bits_per_pixel = 1;
    panel_config.vendor_config = &ssd1306_config;
    err = esp_lcd_new_panel_ssd1306(panel_io_, &panel_config, &panel_);
    if (err == ESP_OK) {
        err = esp_lcd_panel_reset(panel_);
    }
    if (err == ESP_OK) {
        err = esp_lcd_panel_init(panel_);
    }
    if (err == ESP_OK) {
        err = esp_lcd_panel_disp_on_off(panel_, true);
    }
    if (err != ESP_OK) {
        ESP_LOGE(kTag, "Failed to initialize SSD1306");
        cleanup();
        return err;
    }

    state_mutex_ = xSemaphoreCreateMutex();
    queue_ = xQueueCreate(1, sizeof(State));
    if (state_mutex_ == nullptr || queue_ == nullptr) {
        cleanup();
        return ESP_ERR_NO_MEM;
    }
    if (xTaskCreate(task_entry, "fan_oled", 4096, this, 3, &task_) != pdPASS) {
        cleanup();
        return ESP_ERR_NO_MEM;
    }

    ready_ = true;
    publish();
    ESP_LOGI(kTag, "SSD1306 initialized on SDA GPIO%d, SCL GPIO%d", config_.sda_gpio, config_.scl_gpio);
    return ESP_OK;
}

bool StatusDisplay::ready() const
{
    return ready_;
}

void StatusDisplay::set_commissioning_payload(const char *payload)
{
    if (!ready_ || payload == nullptr) {
        return;
    }
    xSemaphoreTake(state_mutex_, portMAX_DELAY);
    std::strncpy(state_.qr_payload.data(), payload, state_.qr_payload.size() - 1);
    state_.qr_payload.back() = '\0';
    state_.connection = Connection::commissioning;
    xSemaphoreGive(state_mutex_);
    publish();
}

void StatusDisplay::set_connection(Connection connection)
{
    if (!ready_) {
        return;
    }
    xSemaphoreTake(state_mutex_, portMAX_DELAY);
    state_.connection = connection;
    xSemaphoreGive(state_mutex_);
    publish();
}

void StatusDisplay::set_fan_state(uint8_t speed, bool rotation)
{
    if (!ready_) {
        return;
    }
    xSemaphoreTake(state_mutex_, portMAX_DELAY);
    state_.speed = std::min<uint8_t>(speed, 3);
    state_.rotation = rotation;
    xSemaphoreGive(state_mutex_);
    publish();
}

void StatusDisplay::task_entry(void *context)
{
    static_cast<StatusDisplay *>(context)->task();
}

void StatusDisplay::qr_callback(const uint8_t *qrcode, void *context)
{
    static_cast<StatusDisplay *>(context)->draw_qr(qrcode);
}

void StatusDisplay::task()
{
    State state;
    while (true) {
        if (xQueueReceive(queue_, &state, portMAX_DELAY) == pdTRUE) {
            render(state);
        }
    }
}

void StatusDisplay::publish()
{
    if (!ready_) {
        return;
    }
    State snapshot;
    xSemaphoreTake(state_mutex_, portMAX_DELAY);
    snapshot = state_;
    xSemaphoreGive(state_mutex_);
    xQueueOverwrite(queue_, &snapshot);
}

void StatusDisplay::cleanup()
{
    if (queue_ != nullptr) {
        vQueueDelete(queue_);
        queue_ = nullptr;
    }
    if (state_mutex_ != nullptr) {
        vSemaphoreDelete(state_mutex_);
        state_mutex_ = nullptr;
    }
    if (panel_ != nullptr) {
        esp_lcd_panel_del(panel_);
        panel_ = nullptr;
    }
    if (panel_io_ != nullptr) {
        esp_lcd_panel_io_del(panel_io_);
        panel_io_ = nullptr;
    }
    if (i2c_bus_ != nullptr) {
        i2c_del_master_bus(i2c_bus_);
        i2c_bus_ = nullptr;
    }
}

void StatusDisplay::render(const State &state)
{
    clear();
    if (state.connection == Connection::commissioning && state.qr_payload[0] != '\0') {
        render_qr(state.qr_payload.data());
    } else {
        render_dashboard(state);
    }
    esp_err_t err = flush();
    if (err != ESP_OK) {
        ESP_LOGE(kTag, "OLED update failed: %s", esp_err_to_name(err));
    }
}

void StatusDisplay::render_dashboard(const State &state)
{
    draw_text(2, 0, "SMART FAN");
    fill_rect(0, 9, kWidth, 1, true);

    const char *connection = "STARTING";
    switch (state.connection) {
    case Connection::commissioning:
        connection = "PAIRING";
        break;
    case Connection::commissioning_closed:
        connection = "PAIRING CLOSED";
        break;
    case Connection::connecting:
        connection = "CONNECTING";
        break;
    case Connection::online:
        connection = "WIFI ONLINE";
        break;
    case Connection::offline:
        connection = "WIFI OFFLINE";
        break;
    case Connection::starting:
        break;
    }
    draw_text(2, 13, connection);

    draw_text(2, 28, "SPEED");
    const char *speed = "OFF";
    if (state.speed == 1) {
        speed = "1";
    } else if (state.speed == 2) {
        speed = "2";
    } else if (state.speed == 3) {
        speed = "3";
    }
    draw_text(48, 24, speed, 2);

    draw_text(2, 51, "ROTATION");
    draw_text(72, 47, state.rotation ? "ON" : "OFF", 2);
}

void StatusDisplay::render_qr(const char *payload)
{
    esp_qrcode_config_t qr_config = ESP_QRCODE_CONFIG_DEFAULT();
    qr_config.display_func_with_cb = qr_callback;
    qr_config.user_data = this;
    qr_config.max_qrcode_version = 2;
    qr_config.qrcode_ecc_level = ESP_QRCODE_ECC_LOW;

    if (esp_qrcode_generate(&qr_config, payload) != ESP_OK) {
        draw_text(8, 16, "PAIRING");
        draw_text(8, 32, "QR ERROR");
        return;
    }

    draw_text(66, 4, "MATTER");
    draw_text(66, 18, "SCAN QR");
    draw_text(66, 32, "TO PAIR");
    draw_text(66, 48, "BT READY");
}

void StatusDisplay::draw_qr(const uint8_t *qrcode)
{
    constexpr int quiet_zone = 4;
    int qr_size = esp_qrcode_get_size(qrcode);
    int total_modules = qr_size + quiet_zone * 2;
    int scale = std::max(1, std::min(2, static_cast<int>(kHeight) / total_modules));
    int pixel_size = total_modules * scale;
    int origin_x = 2;
    int origin_y = (kHeight - pixel_size) / 2;

    fill_rect(origin_x, origin_y, pixel_size, pixel_size, true);
    for (int y = 0; y < qr_size; ++y) {
        for (int x = 0; x < qr_size; ++x) {
            if (esp_qrcode_get_module(qrcode, x, y)) {
                fill_rect(origin_x + (x + quiet_zone) * scale, origin_y + (y + quiet_zone) * scale, scale, scale,
                          false);
            }
        }
    }
}

void StatusDisplay::clear()
{
    buffer_.fill(0);
}

void StatusDisplay::set_pixel(int x, int y, bool on)
{
    if (x < 0 || x >= kWidth || y < 0 || y >= kHeight) {
        return;
    }
    uint8_t &value = buffer_[(y / 8) * kWidth + x];
    uint8_t mask = static_cast<uint8_t>(1U << (y % 8));
    if (on) {
        value |= mask;
    } else {
        value &= static_cast<uint8_t>(~mask);
    }
}

void StatusDisplay::fill_rect(int x, int y, int width, int height, bool on)
{
    for (int row = y; row < y + height; ++row) {
        for (int column = x; column < x + width; ++column) {
            set_pixel(column, row, on);
        }
    }
}

void StatusDisplay::draw_text(int x, int y, const char *text, int scale)
{
    while (*text != '\0') {
        draw_char(x, y, *text++, scale);
        x += 6 * scale;
    }
}

void StatusDisplay::draw_char(int x, int y, char character, int scale)
{
    const auto &columns = glyph_for(character);
    for (int column = 0; column < 5; ++column) {
        for (int row = 0; row < 7; ++row) {
            if ((columns[column] & (1U << row)) != 0) {
                fill_rect(x + column * scale, y + row * scale, scale, scale, true);
            }
        }
    }
}

esp_err_t StatusDisplay::flush()
{
    return esp_lcd_panel_draw_bitmap(panel_, 0, 0, kWidth, kHeight, buffer_.data());
}
