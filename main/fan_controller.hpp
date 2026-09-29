#pragma once

#include <array>
#include <cstdint>

#include <driver/gpio.h>
#include <esp_err.h>

class FanController {
public:
    enum class Speed : uint8_t {
        off = 0,
        low = 1,
        medium = 2,
        high = 3,
    };

    struct Config {
        std::array<gpio_num_t, 3> speed_gpios;
        gpio_num_t rotation_gpio;
        bool active_high;
        uint32_t switch_delay_ms;
    };

    explicit FanController(Config config);

    esp_err_t init();
    esp_err_t set_speed(Speed speed);
    esp_err_t set_percent(uint8_t percent);
    esp_err_t set_rotation(bool enabled);

    Speed speed() const;
    bool rotation_enabled() const;

private:
    esp_err_t set_output(gpio_num_t gpio, bool active);
    esp_err_t disable_speed_relays();
    esp_err_t apply_rotation();
    bool config_is_valid() const;

    Config config_;
    Speed speed_ = Speed::off;
    bool rotation_enabled_ = false;
    bool initialized_ = false;
};
