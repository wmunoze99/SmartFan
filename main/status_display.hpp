#pragma once

#include <array>
#include <cstdint>

#include <driver/gpio.h>
#include <driver/i2c_master.h>
#include <esp_err.h>
#include <esp_lcd_panel_io.h>
#include <esp_lcd_panel_ops.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

class StatusDisplay {
public:
    enum class Connection : uint8_t {
        starting,
        commissioning,
        commissioning_closed,
        connecting,
        online,
        offline,
    };

    struct Config {
        gpio_num_t sda_gpio;
        gpio_num_t scl_gpio;
        uint8_t i2c_address;
    };

    explicit StatusDisplay(Config config);

    esp_err_t init();
    bool ready() const;
    void set_commissioning_payload(const char *payload);
    void set_connection(Connection connection);
    void set_fan_state(uint8_t speed, bool rotation);

private:
    static constexpr uint16_t kWidth = 128;
    static constexpr uint16_t kHeight = 64;
    static constexpr size_t kBufferSize = kWidth * kHeight / 8;
    static constexpr size_t kMaxQrPayloadLength = 129;

    struct State {
        Connection connection = Connection::starting;
        uint8_t speed = 0;
        bool rotation = false;
        std::array<char, kMaxQrPayloadLength> qr_payload{};
    };

    static void task_entry(void *context);
    static void qr_callback(const uint8_t *qrcode, void *context);

    void task();
    void publish();
    void cleanup();
    void render(const State &state);
    void render_dashboard(const State &state);
    void render_qr(const char *payload);
    void draw_qr(const uint8_t *qrcode);
    void clear();
    void set_pixel(int x, int y, bool on);
    void fill_rect(int x, int y, int width, int height, bool on);
    void draw_text(int x, int y, const char *text, int scale = 1);
    void draw_char(int x, int y, char character, int scale);
    esp_err_t flush();

    Config config_;
    State state_{};
    std::array<uint8_t, kBufferSize> buffer_{};
    i2c_master_bus_handle_t i2c_bus_ = nullptr;
    esp_lcd_panel_io_handle_t panel_io_ = nullptr;
    esp_lcd_panel_handle_t panel_ = nullptr;
    QueueHandle_t queue_ = nullptr;
    SemaphoreHandle_t state_mutex_ = nullptr;
    TaskHandle_t task_ = nullptr;
    bool ready_ = false;
};
