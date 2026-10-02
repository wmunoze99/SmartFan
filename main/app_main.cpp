#include <atomic>
#include <cinttypes>
#include <cstdint>
#include <cstdlib>

#include <esp_check.h>
#include <esp_err.h>
#include <esp_log.h>
#include <nvs_flash.h>

#include <esp_matter.h>
#include <esp_matter_console.h>
#include <esp_matter_ota.h>

#include <app/server/Server.h>
#include <platform/CHIPDeviceLayer.h>
#include <setup_payload/OnboardingCodesUtil.h>
#include <setup_payload/QRCodeSetupPayloadGenerator.h>

#include "fan_controller.hpp"
#include "ota_manager.hpp"
#include "status_display.hpp"

using namespace chip::app::Clusters;
using namespace esp_matter;

namespace {
constexpr char kTag[] = "smart_fan";
constexpr uint8_t kRockLeftRight = 0x01;

uint16_t s_fan_endpoint_id = 0;
std::atomic_bool s_commissioned = false;
std::atomic_bool s_commissioning_window_open = false;

#if CONFIG_FAN_RELAY_ACTIVE_HIGH
constexpr bool kRelayActiveHigh = true;
#else
constexpr bool kRelayActiveHigh = false;
#endif

FanController s_fan({
    .speed_gpios = {
        static_cast<gpio_num_t>(CONFIG_FAN_SPEED_1_GPIO),
        static_cast<gpio_num_t>(CONFIG_FAN_SPEED_2_GPIO),
        static_cast<gpio_num_t>(CONFIG_FAN_SPEED_3_GPIO),
    },
    .rotation_gpio = static_cast<gpio_num_t>(CONFIG_FAN_ROTATION_GPIO),
    .active_high = kRelayActiveHigh,
    .switch_delay_ms = CONFIG_FAN_RELAY_SWITCH_DELAY_MS,
});

#if CONFIG_FAN_OTA_ENABLED
bool ota_ready(void *)
{
    return s_commissioned.load() && s_fan.speed() == FanController::Speed::off;
}

OtaManager s_ota({
    .url = CONFIG_FAN_OTA_RELEASE_URL,
    .startup_delay_seconds = CONFIG_FAN_OTA_STARTUP_DELAY_SECONDS,
    .check_interval_seconds = CONFIG_FAN_OTA_CHECK_INTERVAL_MINUTES * 60U,
    .retry_interval_seconds = CONFIG_FAN_OTA_RETRY_INTERVAL_MINUTES * 60U,
    .ready_callback = ota_ready,
    .callback_context = nullptr,
});

void update_ota_network_status()
{
    s_ota.set_network_connected(chip::DeviceLayer::ConnectivityMgr().IsWiFiStationConnected());
}

bool ota_update_in_progress()
{
    return s_ota.update_in_progress();
}
#else
void update_ota_network_status() {}
bool ota_update_in_progress() { return false; }
#endif

#if CONFIG_FAN_DISPLAY_ENABLED
static_assert(CONFIG_FAN_DISPLAY_SDA_GPIO != CONFIG_FAN_DISPLAY_SCL_GPIO,
              "OLED SDA and SCL GPIOs must be different");
static_assert(CONFIG_FAN_DISPLAY_SDA_GPIO != CONFIG_FAN_SPEED_1_GPIO &&
                  CONFIG_FAN_DISPLAY_SDA_GPIO != CONFIG_FAN_SPEED_2_GPIO &&
                  CONFIG_FAN_DISPLAY_SDA_GPIO != CONFIG_FAN_SPEED_3_GPIO &&
                  CONFIG_FAN_DISPLAY_SDA_GPIO != CONFIG_FAN_ROTATION_GPIO &&
                  CONFIG_FAN_DISPLAY_SCL_GPIO != CONFIG_FAN_SPEED_1_GPIO &&
                  CONFIG_FAN_DISPLAY_SCL_GPIO != CONFIG_FAN_SPEED_2_GPIO &&
                  CONFIG_FAN_DISPLAY_SCL_GPIO != CONFIG_FAN_SPEED_3_GPIO &&
                  CONFIG_FAN_DISPLAY_SCL_GPIO != CONFIG_FAN_ROTATION_GPIO,
              "OLED GPIOs must not overlap relay GPIOs");

StatusDisplay s_display({
    .sda_gpio = static_cast<gpio_num_t>(CONFIG_FAN_DISPLAY_SDA_GPIO),
    .scl_gpio = static_cast<gpio_num_t>(CONFIG_FAN_DISPLAY_SCL_GPIO),
    .i2c_address = CONFIG_FAN_DISPLAY_I2C_ADDRESS,
});

void update_fan_display()
{
    s_display.set_fan_state(static_cast<uint8_t>(s_fan.speed()), s_fan.rotation_enabled());
}

void update_connection_display()
{
    if (!s_commissioned.load()) {
        s_display.set_connection(s_commissioning_window_open.load() ? StatusDisplay::Connection::commissioning
                                                                    : StatusDisplay::Connection::commissioning_closed);
        return;
    }
    bool connected = chip::DeviceLayer::ConnectivityMgr().IsWiFiStationConnected();
    s_display.set_connection(connected ? StatusDisplay::Connection::online : StatusDisplay::Connection::offline);
}

void set_display_commissioning_payload()
{
    if (!s_display.ready()) {
        return;
    }
    char qr_payload[chip::QRCodeBasicSetupPayloadGenerator::kMaxQRCodeBase38RepresentationLength + 1] = {};
    chip::MutableCharSpan payload_span(qr_payload, sizeof(qr_payload));
    CHIP_ERROR err = GetQRCode(payload_span,
                               chip::RendezvousInformationFlags(chip::RendezvousInformationFlag::kBLE));
    if (err == CHIP_NO_ERROR) {
        s_display.set_commissioning_payload(qr_payload);
    } else {
        ESP_LOGE(kTag, "Failed to generate Matter QR payload: %s", err.AsString());
    }
}

void refresh_fabric_state()
{
    bool commissioned = chip::Server::GetInstance().GetFabricTable().FabricCount() != 0;
    s_commissioned.store(commissioned);
    if (!commissioned) {
        set_display_commissioning_payload();
    }
    update_connection_display();
}

void initialize_display_status()
{
    s_commissioning_window_open.store(
        chip::Server::GetInstance().GetCommissioningWindowManager().IsCommissioningWindowOpen());
    refresh_fabric_state();
    update_fan_display();
}
#else
void update_fan_display() {}
void update_connection_display() {}
void refresh_fabric_state() {}
void initialize_display_status() {}
#endif

uint8_t fan_percent()
{
    switch (s_fan.speed()) {
    case FanController::Speed::off:
        return 0;
    case FanController::Speed::low:
        return 33;
    case FanController::Speed::medium:
        return 66;
    case FanController::Speed::high:
        return 100;
    }
    return 0;
}

void report_fan_attribute(uint32_t attribute_id, esp_matter_attr_val_t value)
{
    esp_err_t err = attribute::report(s_fan_endpoint_id, FanControl::Id, attribute_id, &value);
    if (err != ESP_OK) {
        ESP_LOGW(kTag, "Failed to report fan attribute 0x%08" PRIx32 ": %s", attribute_id,
                 esp_err_to_name(err));
    }
}

void report_fan_state()
{
    const uint8_t speed = static_cast<uint8_t>(s_fan.speed());
    const uint8_t percent = fan_percent();
    const uint8_t rock = s_fan.rotation_enabled() ? kRockLeftRight : 0;

    report_fan_attribute(FanControl::Attributes::FanMode::Id, esp_matter_enum8(speed));
    report_fan_attribute(FanControl::Attributes::PercentSetting::Id,
                         esp_matter_nullable_uint8(nullable<uint8_t>(percent)));
    report_fan_attribute(FanControl::Attributes::PercentCurrent::Id, esp_matter_uint8(percent));
    report_fan_attribute(FanControl::Attributes::SpeedSetting::Id,
                         esp_matter_nullable_uint8(nullable<uint8_t>(speed)));
    report_fan_attribute(FanControl::Attributes::SpeedCurrent::Id, esp_matter_uint8(speed));
    report_fan_attribute(FanControl::Attributes::RockSetting::Id, esp_matter_bitmap8(rock));
}

void app_event_cb(const ChipDeviceEvent *event, intptr_t arg)
{
    switch (event->Type) {
    case chip::DeviceLayer::DeviceEventType::kCommissioningComplete:
        ESP_LOGI(kTag, "Commissioning complete");
        s_commissioned.store(true);
        update_connection_display();
        break;
    case chip::DeviceLayer::DeviceEventType::kCommissioningSessionStarted:
        ESP_LOGI(kTag, "Commissioning started");
        break;
    case chip::DeviceLayer::DeviceEventType::kCommissioningSessionStopped:
        ESP_LOGI(kTag, "Commissioning stopped");
        break;
    case chip::DeviceLayer::DeviceEventType::kCommissioningWindowOpened:
        s_commissioning_window_open.store(true);
        update_connection_display();
        break;
    case chip::DeviceLayer::DeviceEventType::kCommissioningWindowClosed:
        s_commissioning_window_open.store(false);
        update_connection_display();
        break;
    case chip::DeviceLayer::DeviceEventType::kFabricCommitted:
    case chip::DeviceLayer::DeviceEventType::kFabricRemoved:
        refresh_fabric_state();
        break;
    case chip::DeviceLayer::DeviceEventType::kFailSafeTimerExpired:
        ESP_LOGW(kTag, "Commissioning failed: fail-safe expired");
        update_connection_display();
        break;
    case chip::DeviceLayer::DeviceEventType::kWiFiConnectivityChange:
        update_connection_display();
        update_ota_network_status();
        break;
    case chip::DeviceLayer::DeviceEventType::kInterfaceIpAddressChanged:
        ESP_LOGI(kTag, "Network IP address changed");
        update_connection_display();
        update_ota_network_status();
        break;
    default:
        break;
    }
}

esp_err_t identification_cb(identification::callback_type_t type, uint16_t endpoint_id, uint8_t effect_id,
                            uint8_t effect_variant, void *priv_data)
{
    ESP_LOGI(kTag, "Identify request on endpoint %u", endpoint_id);
    return ESP_OK;
}

esp_err_t handle_fan_mode(uint8_t mode)
{
    switch (mode) {
    case 0:
        return s_fan.set_speed(FanController::Speed::off);
    case 1:
        return s_fan.set_speed(FanController::Speed::low);
    case 2:
        return s_fan.set_speed(FanController::Speed::medium);
    case 3:
        return s_fan.set_speed(FanController::Speed::high);
    case 4:
        return s_fan.set_speed(s_fan.speed() == FanController::Speed::off ? FanController::Speed::low : s_fan.speed());
    default:
        return ESP_ERR_NOT_SUPPORTED;
    }
}

esp_err_t attribute_update_cb(attribute::callback_type_t type, uint16_t endpoint_id, uint32_t cluster_id,
                              uint32_t attribute_id, esp_matter_attr_val_t *value, void *priv_data)
{
    if (endpoint_id != s_fan_endpoint_id || cluster_id != FanControl::Id) {
        return ESP_OK;
    }
    if (type == attribute::POST_UPDATE) {
        report_fan_state();
        update_fan_display();
        return ESP_OK;
    }
    if (type != attribute::PRE_UPDATE) {
        return ESP_OK;
    }
    if (ota_update_in_progress()) {
        return ESP_ERR_INVALID_STATE;
    }

    if (attribute_id == FanControl::Attributes::FanMode::Id) {
        return handle_fan_mode(value->val.u8);
    }
    if (attribute_id == FanControl::Attributes::PercentSetting::Id) {
        return s_fan.set_percent(value->val.u8);
    }
    if (attribute_id == FanControl::Attributes::SpeedSetting::Id) {
        return handle_fan_mode(value->val.u8);
    }
    if (attribute_id == FanControl::Attributes::RockSetting::Id) {
        return s_fan.set_rotation((value->val.u8 & kRockLeftRight) != 0);
    }

    return ESP_OK;
}

esp_err_t init_nvs()
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_RETURN_ON_ERROR(nvs_flash_erase(), kTag, "Failed to erase NVS");
        err = nvs_flash_init();
    }
    return err;
}

esp_err_t add_fan_features(endpoint_t *endpoint)
{
    cluster_t *fan_cluster = cluster::get(endpoint, FanControl::Id);
    if (fan_cluster == nullptr) {
        return ESP_ERR_NOT_FOUND;
    }

    cluster::fan_control::feature::multi_speed::config_t speed_config;
    speed_config.speed_max = 3;
    speed_config.speed_setting = static_cast<uint8_t>(0);
    speed_config.speed_current = 0;
    ESP_RETURN_ON_ERROR(cluster::fan_control::feature::multi_speed::add(fan_cluster, &speed_config), kTag,
                        "Failed to add multi-speed feature");

    cluster::fan_control::feature::rocking::config_t rocking_config;
    rocking_config.rock_support = kRockLeftRight;
    rocking_config.rock_setting = 0;
    return cluster::fan_control::feature::rocking::add(fan_cluster, &rocking_config);
}

void restore_fan_state()
{
    esp_matter_attr_val_t value = {};
    attribute_t *speed_attribute = attribute::get(s_fan_endpoint_id, FanControl::Id,
                                                  FanControl::Attributes::SpeedSetting::Id);
    if (speed_attribute != nullptr && attribute::get_val(speed_attribute, &value) == ESP_OK) {
        handle_fan_mode(value.val.u8);
    }

    attribute_t *rock_attribute = attribute::get(s_fan_endpoint_id, FanControl::Id,
                                                 FanControl::Attributes::RockSetting::Id);
    if (rock_attribute != nullptr && attribute::get_val(rock_attribute, &value) == ESP_OK) {
        s_fan.set_rotation((value.val.u8 & kRockLeftRight) != 0);
    }
    report_fan_state();
    update_fan_display();
}
} // namespace

extern "C" void app_main()
{
    ESP_ERROR_CHECK(s_fan.init());
    ESP_ERROR_CHECK(init_nvs());

#if CONFIG_FAN_DISPLAY_ENABLED
    esp_err_t display_err = s_display.init();
    if (display_err != ESP_OK) {
        ESP_LOGW(kTag, "Display disabled: %s", esp_err_to_name(display_err));
    }
#endif

    node::config_t node_config;
    node_t *node = node::create(&node_config, attribute_update_cb, identification_cb);
    if (node == nullptr) {
        ESP_LOGE(kTag, "Failed to create Matter node");
        abort();
    }

    endpoint::fan::config_t fan_config;
    fan_config.fan_control.fan_mode = 0;
    fan_config.fan_control.fan_mode_sequence = 0;
    fan_config.fan_control.percent_setting = static_cast<uint8_t>(0);
    fan_config.fan_control.percent_current = 0;

    endpoint_t *fan_endpoint = endpoint::fan::create(node, &fan_config, ENDPOINT_FLAG_NONE, &s_fan);
    if (fan_endpoint == nullptr) {
        ESP_LOGE(kTag, "Failed to create fan endpoint");
        abort();
    }

    s_fan_endpoint_id = endpoint::get_id(fan_endpoint);
    ESP_ERROR_CHECK(add_fan_features(fan_endpoint));
    ESP_LOGI(kTag, "Fan endpoint created: %u", s_fan_endpoint_id);

    ESP_ERROR_CHECK(esp_matter::start(app_event_cb));
    restore_fan_state();
    initialize_display_status();

#if CONFIG_FAN_OTA_ENABLED
    ESP_ERROR_CHECK(s_ota.confirm_running_image());
    ESP_ERROR_CHECK(s_ota.start());
    update_ota_network_status();
#endif

#if CONFIG_ENABLE_CHIP_SHELL
    esp_matter::console::diagnostics_register_commands();
    esp_matter::console::factoryreset_register_commands();
    esp_matter::console::attribute_register_commands();
    esp_matter::console::init();
#endif
}
