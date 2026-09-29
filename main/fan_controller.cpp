#include "fan_controller.hpp"

#include <algorithm>

#include <esp_check.h>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

namespace {
constexpr char kTag[] = "fan_controller";

const char *speed_name(FanController::Speed speed)
{
    switch (speed) {
    case FanController::Speed::off:
        return "off";
    case FanController::Speed::low:
        return "low";
    case FanController::Speed::medium:
        return "medium";
    case FanController::Speed::high:
        return "high";
    }
    return "invalid";
}
} // namespace

FanController::FanController(Config config) : config_(config) {}

esp_err_t FanController::init()
{
    if (!config_is_valid()) {
        ESP_LOGE(kTag, "Relay GPIO configuration is invalid");
        return ESP_ERR_INVALID_ARG;
    }

    const int inactive_level = config_.active_high ? 0 : 1;
    uint64_t pin_mask = 1ULL << config_.rotation_gpio;
    for (gpio_num_t gpio : config_.speed_gpios) {
        pin_mask |= 1ULL << gpio;
        gpio_set_level(gpio, inactive_level);
    }
    gpio_set_level(config_.rotation_gpio, inactive_level);

    gpio_config_t io_config = {};
    io_config.pin_bit_mask = pin_mask;
    io_config.mode = GPIO_MODE_OUTPUT;
    io_config.pull_up_en = GPIO_PULLUP_DISABLE;
    io_config.pull_down_en = GPIO_PULLDOWN_DISABLE;
    io_config.intr_type = GPIO_INTR_DISABLE;
    ESP_RETURN_ON_ERROR(gpio_config(&io_config), kTag, "Failed to configure relay GPIOs");

    initialized_ = true;
    ESP_RETURN_ON_ERROR(disable_speed_relays(), kTag, "Failed to disable speed relays");
    ESP_RETURN_ON_ERROR(set_output(config_.rotation_gpio, false), kTag, "Failed to disable rotation relay");

    ESP_LOGI(kTag, "Relays initialized in safe state");
    return ESP_OK;
}

esp_err_t FanController::set_speed(Speed speed)
{
    if (!initialized_) {
        return ESP_ERR_INVALID_STATE;
    }
    if (speed > Speed::high) {
        return ESP_ERR_INVALID_ARG;
    }
    if (speed == speed_) {
        return apply_rotation();
    }

    const Speed previous_speed = speed_;
    ESP_RETURN_ON_ERROR(disable_speed_relays(), kTag, "Failed to disable speed relays");
    speed_ = Speed::off;
    ESP_RETURN_ON_ERROR(apply_rotation(), kTag, "Failed to update rotation relay");

    if (previous_speed != Speed::off && speed != Speed::off && config_.switch_delay_ms > 0) {
        vTaskDelay(pdMS_TO_TICKS(config_.switch_delay_ms));
    }

    if (speed != Speed::off) {
        const size_t relay_index = static_cast<size_t>(speed) - 1;
        esp_err_t err = set_output(config_.speed_gpios[relay_index], true);
        if (err != ESP_OK) {
            disable_speed_relays();
            return err;
        }
    }

    speed_ = speed;
    ESP_RETURN_ON_ERROR(apply_rotation(), kTag, "Failed to update rotation relay");
    ESP_LOGI(kTag, "Speed set to %s", speed_name(speed_));
    return ESP_OK;
}

esp_err_t FanController::set_percent(uint8_t percent)
{
    if (percent > 100) {
        return ESP_ERR_INVALID_ARG;
    }
    if (percent == 0) {
        return set_speed(Speed::off);
    }
    if (percent <= 33) {
        return set_speed(Speed::low);
    }
    if (percent <= 66) {
        return set_speed(Speed::medium);
    }
    return set_speed(Speed::high);
}

esp_err_t FanController::set_rotation(bool enabled)
{
    if (!initialized_) {
        return ESP_ERR_INVALID_STATE;
    }

    rotation_enabled_ = enabled;
    ESP_RETURN_ON_ERROR(apply_rotation(), kTag, "Failed to update rotation relay");
    ESP_LOGI(kTag, "Rotation %s", enabled ? "enabled" : "disabled");
    return ESP_OK;
}

FanController::Speed FanController::speed() const
{
    return speed_;
}

bool FanController::rotation_enabled() const
{
    return rotation_enabled_;
}

esp_err_t FanController::set_output(gpio_num_t gpio, bool active)
{
    const int level = active == config_.active_high ? 1 : 0;
    return gpio_set_level(gpio, level);
}

esp_err_t FanController::disable_speed_relays()
{
    for (gpio_num_t gpio : config_.speed_gpios) {
        ESP_RETURN_ON_ERROR(set_output(gpio, false), kTag, "Failed to disable GPIO %d", gpio);
    }
    return ESP_OK;
}

esp_err_t FanController::apply_rotation()
{
    return set_output(config_.rotation_gpio, rotation_enabled_ && speed_ != Speed::off);
}

bool FanController::config_is_valid() const
{
    std::array<gpio_num_t, 4> gpios = {
        config_.speed_gpios[0],
        config_.speed_gpios[1],
        config_.speed_gpios[2],
        config_.rotation_gpio,
    };

    for (gpio_num_t gpio : gpios) {
        if (!GPIO_IS_VALID_OUTPUT_GPIO(gpio)) {
            return false;
        }
    }

    std::sort(gpios.begin(), gpios.end());
    return std::adjacent_find(gpios.begin(), gpios.end()) == gpios.end();
}
