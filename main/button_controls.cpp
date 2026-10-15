#include "button_controls.hpp"

#include <button_gpio.h>

ButtonControls::ButtonControls(Config config)
    : config_(config), speed_context_{this, Action::cycle_speed},
      rotation_context_{this, Action::toggle_rotation}
{}

esp_err_t ButtonControls::init()
{
    if (speed_button_ != nullptr || rotation_button_ != nullptr) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!GPIO_IS_VALID_GPIO(config_.speed_gpio) || !GPIO_IS_VALID_GPIO(config_.rotation_gpio) ||
        config_.speed_gpio == config_.rotation_gpio || config_.callback == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t err = create_button(config_.speed_gpio, &speed_context_, &speed_button_);
    if (err == ESP_OK) {
        err = create_button(config_.rotation_gpio, &rotation_context_, &rotation_button_);
    }
    if (err != ESP_OK) {
        cleanup();
    }
    return err;
}

esp_err_t ButtonControls::create_button(gpio_num_t gpio, ButtonContext *context, button_handle_t *handle)
{
    button_config_t button_config = {};
    button_gpio_config_t gpio_config = {};
    gpio_config.gpio_num = gpio;
    gpio_config.active_level = 0;
    gpio_config.enable_power_save = false;
    gpio_config.disable_pull = false;

    esp_err_t err = iot_button_new_gpio_device(&button_config, &gpio_config, handle);
    if (err != ESP_OK) {
        return err;
    }
    return iot_button_register_cb(*handle, BUTTON_PRESS_DOWN, nullptr, press_callback, context);
}

void ButtonControls::press_callback(void *, void *context)
{
    auto *button_context = static_cast<ButtonContext *>(context);
    button_context->controls->config_.callback(button_context->action);
}

void ButtonControls::cleanup()
{
    if (speed_button_ != nullptr) {
        iot_button_delete(speed_button_);
        speed_button_ = nullptr;
    }
    if (rotation_button_ != nullptr) {
        iot_button_delete(rotation_button_);
        rotation_button_ = nullptr;
    }
}
