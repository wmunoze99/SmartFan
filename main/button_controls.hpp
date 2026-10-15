#pragma once

#include <cstdint>

#include <driver/gpio.h>
#include <esp_err.h>
#include <iot_button.h>

class ButtonControls {
public:
    enum class Action : intptr_t {
        cycle_speed = 0,
        toggle_rotation = 1,
    };

    // Runs in the button task; the module must remain alive for the app lifetime.
    using Callback = void (*)(Action);

    struct Config {
        gpio_num_t speed_gpio;
        gpio_num_t rotation_gpio;
        Callback callback;
    };

    explicit ButtonControls(Config config);
    ButtonControls(const ButtonControls &) = delete;
    ButtonControls &operator=(const ButtonControls &) = delete;

    esp_err_t init();

private:
    struct ButtonContext {
        ButtonControls *controls;
        Action action;
    };

    static void press_callback(void *button, void *context);
    static esp_err_t create_button(gpio_num_t gpio, ButtonContext *context, button_handle_t *handle);
    void cleanup();

    Config config_;
    ButtonContext speed_context_;
    ButtonContext rotation_context_;
    button_handle_t speed_button_ = nullptr;
    button_handle_t rotation_button_ = nullptr;
};
