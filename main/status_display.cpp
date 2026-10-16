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
    {'a', {0x20, 0x54, 0x54, 0x54, 0x78}}, {'b', {0x7f, 0x48, 0x44, 0x44, 0x38}},
    {'c', {0x38, 0x44, 0x44, 0x44, 0x20}}, {'d', {0x38, 0x44, 0x44, 0x48, 0x7f}},
    {'e', {0x38, 0x54, 0x54, 0x54, 0x18}}, {'f', {0x08, 0x7e, 0x09, 0x01, 0x02}},
    {'g', {0x0c, 0x52, 0x52, 0x52, 0x3e}}, {'h', {0x7f, 0x08, 0x04, 0x04, 0x78}},
    {'i', {0x00, 0x44, 0x7d, 0x40, 0x00}}, {'j', {0x20, 0x40, 0x44, 0x3d, 0x00}},
    {'k', {0x7f, 0x10, 0x28, 0x44, 0x00}}, {'l', {0x00, 0x41, 0x7f, 0x40, 0x00}},
    {'m', {0x7c, 0x04, 0x18, 0x04, 0x78}}, {'n', {0x7c, 0x08, 0x04, 0x04, 0x78}},
    {'o', {0x38, 0x44, 0x44, 0x44, 0x38}}, {'p', {0x7c, 0x14, 0x14, 0x14, 0x08}},
    {'q', {0x08, 0x14, 0x14, 0x18, 0x7c}}, {'r', {0x7c, 0x08, 0x04, 0x04, 0x08}},
    {'s', {0x48, 0x54, 0x54, 0x54, 0x20}}, {'t', {0x04, 0x3f, 0x44, 0x40, 0x20}},
    {'u', {0x3c, 0x40, 0x40, 0x20, 0x7c}}, {'v', {0x1c, 0x20, 0x40, 0x20, 0x1c}},
    {'w', {0x3c, 0x40, 0x30, 0x40, 0x3c}}, {'x', {0x44, 0x28, 0x10, 0x28, 0x44}},
    {'y', {0x0c, 0x50, 0x50, 0x50, 0x3c}}, {'z', {0x44, 0x64, 0x54, 0x4c, 0x44}},
    {'!', {0x00, 0x00, 0x5f, 0x00, 0x00}}, {'"', {0x00, 0x07, 0x00, 0x07, 0x00}},
    {'#', {0x14, 0x7f, 0x14, 0x7f, 0x14}}, {'$', {0x24, 0x2a, 0x7f, 0x2a, 0x12}},
    {'%', {0x23, 0x13, 0x08, 0x64, 0x62}}, {'&', {0x36, 0x49, 0x55, 0x22, 0x50}},
    {'\'', {0x00, 0x05, 0x03, 0x00, 0x00}}, {'(', {0x00, 0x1c, 0x22, 0x41, 0x00}},
    {')', {0x00, 0x41, 0x22, 0x1c, 0x00}}, {'*', {0x14, 0x08, 0x3e, 0x08, 0x14}},
    {'+', {0x08, 0x08, 0x3e, 0x08, 0x08}}, {',', {0x00, 0x50, 0x30, 0x00, 0x00}},
    {'.', {0x00, 0x60, 0x60, 0x00, 0x00}}, {'/', {0x20, 0x10, 0x08, 0x04, 0x02}},
    {';', {0x00, 0x56, 0x36, 0x00, 0x00}}, {'<', {0x08, 0x14, 0x22, 0x41, 0x00}},
    {'=', {0x14, 0x14, 0x14, 0x14, 0x14}}, {'>', {0x00, 0x41, 0x22, 0x14, 0x08}},
    {'@', {0x32, 0x49, 0x79, 0x41, 0x3e}}, {'[', {0x00, 0x7f, 0x41, 0x41, 0x00}},
    {'\\', {0x02, 0x04, 0x08, 0x10, 0x20}}, {']', {0x00, 0x41, 0x41, 0x7f, 0x00}},
    {'^', {0x04, 0x02, 0x01, 0x02, 0x04}}, {'_', {0x40, 0x40, 0x40, 0x40, 0x40}},
    {'`', {0x00, 0x01, 0x02, 0x04, 0x00}}, {'{', {0x00, 0x08, 0x36, 0x41, 0x00}},
    {'|', {0x00, 0x00, 0x7f, 0x00, 0x00}}, {'}', {0x00, 0x41, 0x36, 0x08, 0x00}},
    {'~', {0x08, 0x04, 0x08, 0x10, 0x08}},
};

constexpr uint32_t kFanIcon[] = {
    0b0000011100000,
    0b0000111110000,
    0b0000111110000,
    0b0000011100000,
    0b0110000000110,
    0b1111011101111,
    0b1111011101111,
    0b1111011101111,
    0b0110000000110,
    0b0000011100000,
    0b0000111110000,
    0b0000111110000,
    0b0000011100000,
};

constexpr uint32_t kRotationIcon[] = {
    0b000000011111111110000000,
    0b000001110000000011100000,
    0b000011000000000000110010,
    0b000110000000000000011110,
    0b000110000000000000001110,
    0b000000000000000000011110,
    0b011110000000000000000000,
    0b011100000000000000011000,
    0b011110000000000000011000,
    0b010011000000000000110000,
    0b000001110000000011100000,
    0b000000011111111110000000,
};

constexpr uint32_t kWifiIcon[] = {
    0b001111111100,
    0b110000000011,
    0b000111111000,
    0b001100001100,
    0b000001100000,
    0b000000000000,
    0b000001100000,
};

std::array<char, 19> ssid_footer(const std::array<char, 33> &network_name)
{
    std::array<char, 32> glyphs{};
    size_t count = 0;
    size_t offset = 0;
    while (offset < 32 && network_name[offset] != '\0') {
        const auto lead = static_cast<uint8_t>(network_name[offset++]);
        char character = '?';
        if (lead >= 0x20 && lead <= 0x7e) {
            character = static_cast<char>(lead);
        } else if (lead >= 0xc2 && lead <= 0xf4) {
            const size_t length = lead < 0xe0 ? 2 : (lead < 0xf0 ? 3 : 4);
            // Consume a whole unsupported UTF-8 codepoint, including a bounded partial tail.
            for (size_t byte = 1; byte < length && offset < 32; ++byte) {
                const auto continuation = static_cast<uint8_t>(network_name[offset]);
                if ((continuation & 0xc0) != 0x80) {
                    break;
                }
                ++offset;
            }
        }
        glyphs[count++] = character;
    }

    std::array<char, 19> result{};
    if (count > result.size() - 1) {
        std::copy_n(glyphs.begin(), 15, result.begin());
        std::copy_n("...", 3, result.begin() + 15);
    } else {
        std::copy_n(glyphs.begin(), count, result.begin());
    }
    return result;
}

const std::array<uint8_t, 5> &glyph_for(char character)
{
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
        ESP_LOGE(kTag, "OLED not found at 0x%02x", config_.i2c_address);
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

    if (config_.controller == Controller::sh1106) {
        err = init_sh1106();
    } else {
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
    }
    if (err != ESP_OK) {
        ESP_LOGE(kTag, "Failed to initialize OLED");
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
    ESP_LOGI(kTag, "%s initialized on SDA GPIO%d, SCL GPIO%d",
             config_.controller == Controller::sh1106 ? "SH1106" : "SSD1306", config_.sda_gpio, config_.scl_gpio);
    return ESP_OK;
}

esp_err_t StatusDisplay::init_sh1106()
{
    // SH1106 uses its own DC-DC enable command, not the SSD1306 charge-pump command.
    constexpr uint8_t commands[] = {
        0xae,             // Display off
        0xd5, 0x80,       // Clock divider
        0xa8, 0x3f,       // 64-row multiplex
        0xd3, 0x00,       // No display offset
        0x40,             // Start line 0
        0xad, 0x8b,       // Enable internal DC-DC converter
        0xa1, 0xc8,       // Segment remap and COM scan direction
        0xda, 0x12,       // COM pin configuration for 128x64
        0x81, 0x80,       // Contrast
        0xd9, 0x22,       // Precharge period
        0xdb, 0x35,       // VCOM deselect level
        0xa4, 0xa6,       // RAM contents, non-inverted
    };
    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(panel_io_, -1, commands, sizeof(commands)),
                        kTag, "SH1106 initialization failed");
    clear();
    ESP_RETURN_ON_ERROR(flush(), kTag, "SH1106 initial clear failed");
    return esp_lcd_panel_io_tx_param(panel_io_, 0xaf, nullptr, 0);
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
    state_.network_name.fill('\0');
    xSemaphoreGive(state_mutex_);
    publish();
}

void StatusDisplay::set_connection(Connection connection, const char *network_name)
{
    if (!ready_) {
        return;
    }
    std::array<char, 33> name{};
    if (connection == Connection::online && network_name != nullptr) {
        std::strncpy(name.data(), network_name, name.size() - 1);
    }
    xSemaphoreTake(state_mutex_, portMAX_DELAY);
    const bool changed = state_.connection != connection || state_.network_name != name;
    state_.connection = connection;
    state_.network_name = name;
    xSemaphoreGive(state_mutex_);
    if (changed) {
        publish();
    }
}

void StatusDisplay::set_fan_state(uint8_t speed, bool rotation)
{
    if (!ready_) {
        return;
    }
    speed = std::min<uint8_t>(speed, 3);
    xSemaphoreTake(state_mutex_, portMAX_DELAY);
    const bool changed = state_.speed != speed || state_.rotation != rotation;
    state_.speed = speed;
    state_.rotation = rotation;
    if (changed) {
        ++state_.activity_sequence;
    }
    xSemaphoreGive(state_mutex_);
    if (changed) {
        publish();
    }
}

void StatusDisplay::wake()
{
    if (!ready_) {
        return;
    }
    xSemaphoreTake(state_mutex_, portMAX_DELAY);
    ++state_.activity_sequence;
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

esp_err_t StatusDisplay::set_display_power(bool on)
{
    if (config_.controller == Controller::sh1106) {
        return esp_lcd_panel_io_tx_param(panel_io_, on ? 0xaf : 0xae, nullptr, 0);
    }
    return esp_lcd_panel_disp_on_off(panel_, on);
}

void StatusDisplay::task()
{
    State state;
    bool display_on = true;
    TickType_t last_activity = xTaskGetTickCount();
    const TickType_t timeout = static_cast<TickType_t>(
        static_cast<uint64_t>(config_.sleep_timeout_seconds) * configTICK_RATE_HZ);

    while (true) {
        const bool pairing = state.connection == Connection::commissioning && state.qr_payload[0] != '\0';
        TickType_t wait = portMAX_DELAY;
        if (display_on && timeout != 0 && !pairing) {
            const TickType_t elapsed = xTaskGetTickCount() - last_activity;
            wait = elapsed < timeout ? timeout - elapsed : 0;
        }

        State next;
        if (xQueueReceive(queue_, &next, wait) == pdTRUE) {
            const bool activity = next.activity_sequence != state.activity_sequence;
            const bool next_pairing = next.connection == Connection::commissioning && next.qr_payload[0] != '\0';
            if (activity || pairing != next_pairing) {
                last_activity = xTaskGetTickCount();
            }
            state = next;
            if (!display_on && !activity && !next_pairing) {
                continue;
            }

            // Refresh RAM before waking so the old frame is never briefly shown.
            render(state);
            if (!display_on) {
                esp_err_t err = set_display_power(true);
                if (err == ESP_OK) {
                    display_on = true;
                } else {
                    ESP_LOGW(kTag, "OLED wake failed: %s", esp_err_to_name(err));
                }
            }
        } else if (display_on && timeout != 0 && !pairing) {
            esp_err_t err = set_display_power(false);
            if (err == ESP_OK) {
                display_on = false;
            } else {
                ESP_LOGW(kTag, "OLED sleep failed: %s", esp_err_to_name(err));
                last_activity = xTaskGetTickCount();
            }
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
    draw_bitmap(3, 1, 13, 13, kFanIcon);
    draw_text(23, 1, "FAN", 2);
    fill_rect(0, 16, kWidth, 1, true);
    fill_rect(63, 19, 1, 35, true);

    if (state.speed == 0) {
        draw_text(15, 26, "OFF", 2);
    } else {
        const char speed[] = {static_cast<char>('0' + std::min<uint8_t>(state.speed, 3)), '\0'};
        draw_text(22, 19, speed, 4);
    }
    draw_text(17, 48, "SPEED");

    draw_bitmap(84, 19, 24, 12, kRotationIcon);
    draw_text(state.rotation ? 85 : 79, 33, state.rotation ? "ON" : "OFF", 2);
    draw_text(73, 48, "ROTATION");

    fill_rect(0, 55, kWidth, 1, true);
    draw_bitmap(2, 57, 12, 7, kWifiIcon);
    if (state.connection != Connection::online) {
        for (int row = 0; row < 7; ++row) {
            const int x = 11 - row;
            fill_rect(x - 1, 57 + row, 3, 1, false);
            set_pixel(x, 57 + row, true);
        }
    }

    const auto network_name = ssid_footer(state.network_name);
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
        connection = network_name[0] != '\0' ? network_name.data() : "WIFI ONLINE";
        break;
    case Connection::offline:
        connection = "WIFI OFFLINE";
        break;
    case Connection::starting:
        break;
    }
    draw_text(18, 57, connection);
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

void StatusDisplay::draw_bitmap(int x, int y, int width, int height, const uint32_t *rows)
{
    for (int row = 0; row < height; ++row) {
        for (int column = 0; column < width; ++column) {
            if ((rows[row] & (1U << (width - column - 1))) != 0) {
                set_pixel(x + column, y + row, true);
            }
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
    if (config_.controller == Controller::sh1106) {
        // The visible 128 columns start at column 2 of the SH1106's 132-column RAM.
        constexpr uint8_t column_offset = 2;
        for (int page = 0; page < kHeight / 8; ++page) {
            const uint8_t address[] = {
                static_cast<uint8_t>(0xb0 | page),
                static_cast<uint8_t>(column_offset & 0x0f),
                static_cast<uint8_t>(0x10 | (column_offset >> 4)),
            };
            ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(panel_io_, -1, address, sizeof(address)),
                                kTag, "SH1106 page address failed");
            ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_color(panel_io_, -1, buffer_.data() + page * kWidth, kWidth),
                                kTag, "SH1106 page write failed");
        }
        return ESP_OK;
    }
    return esp_lcd_panel_draw_bitmap(panel_, 0, 0, kWidth, kHeight, buffer_.data());
}
