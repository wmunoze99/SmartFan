#include <sdkconfig.h>

#if CONFIG_FAN_SYSLOG_ENABLED

#if !CONFIG_LOG_VERSION_1 || !CONFIG_LOG_MODE_TEXT
#error "RemoteLogger requires ESP LOG V1 text mode"
#endif

#include "remote_logger.hpp"
#include "firmware_variant.hpp"
#include "syslog_format.hpp"

#include <cinttypes>
#include <cstdio>
#include <cstring>
#include <fcntl.h>

#include <esp_app_desc.h>
#include <esp_log.h>
#include <esp_mac.h>
#include <esp_ota_ops.h>
#include <esp_system.h>
#include <esp_timer.h>
#include <esp_wifi.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>
#include <lwip/inet.h>
#include <lwip/netdb.h>
#include <lwip/sockets.h>
#include <mdns.h>

namespace {
constexpr size_t kQueueLength = 16;
constexpr int64_t kRetryUs = 45LL * 1000000;
constexpr int64_t kRefreshUs = 600LL * 1000000;
using PrintHook = int (*)(const char *, va_list);
portMUX_TYPE s_hook_install_lock = portMUX_INITIALIZER_UNLOCKED;

static_assert(std::atomic<uint32_t>::is_always_lock_free);
static_assert(std::atomic<PrintHook>::is_always_lock_free);
static_assert(std::atomic<TaskHandle_t>::is_always_lock_free);

const char *reset_name(esp_reset_reason_t reason)
{
    switch (reason) {
    case ESP_RST_POWERON: return "POWERON";
    case ESP_RST_EXT: return "EXT";
    case ESP_RST_SW: return "SW";
    case ESP_RST_PANIC: return "PANIC";
    case ESP_RST_INT_WDT: return "INT_WDT";
    case ESP_RST_TASK_WDT: return "TASK_WDT";
    case ESP_RST_WDT: return "WDT";
    case ESP_RST_DEEPSLEEP: return "DEEPSLEEP";
    case ESP_RST_BROWNOUT: return "BROWNOUT";
    case ESP_RST_SDIO: return "SDIO";
    case ESP_RST_USB: return "USB";
    case ESP_RST_JTAG: return "JTAG";
    case ESP_RST_EFUSE: return "EFUSE";
    case ESP_RST_PWR_GLITCH: return "PWR_GLITCH";
    case ESP_RST_CPU_LOCKUP: return "CPU_LOCKUP";
    default: return "UNKNOWN";
    }
}

const char *running_ota_state()
{
    const esp_partition_t *partition = esp_ota_get_running_partition();
    esp_ota_img_states_t state;
    if (partition == nullptr || esp_ota_get_state_partition(partition, &state) != ESP_OK) {
        return "unavailable";
    }
    switch (state) {
    case ESP_OTA_IMG_NEW: return "NEW";
    case ESP_OTA_IMG_PENDING_VERIFY: return "PENDING_VERIFY";
    case ESP_OTA_IMG_VALID: return "VALID";
    case ESP_OTA_IMG_INVALID: return "INVALID";
    case ESP_OTA_IMG_ABORTED: return "ABORTED";
    case ESP_OTA_IMG_UNDEFINED: return "UNDEFINED";
    default: return "unknown";
    }
}

bool resolve_ipv4(const char *host, in_addr &address)
{
    if (inet_pton(AF_INET, host, &address) == 1) {
        return true;
    }
    size_t length = std::strlen(host);
    if (length != 0 && host[length - 1] == '.') {
        --length;
    }
    constexpr std::string_view suffix = ".local";
    bool local = length > suffix.size();
    for (size_t i = 0; local && i < suffix.size(); ++i) {
        char c = host[length - suffix.size() + i];
        if (c >= 'A' && c <= 'Z') {
            c = static_cast<char>(c + ('a' - 'A'));
        }
        local = c == suffix[i];
    }
    if (local) {
        char name[254];
        const size_t name_length = length - suffix.size();
        std::memcpy(name, host, name_length);
        name[name_length] = '\0';
        esp_ip4_addr_t result = {};
        // Matter owns mDNS initialization. ESP_ERR_INVALID_STATE is retried
        // later, without initializing/deinitializing its shared responder.
        if (mdns_query_a(name, 1500, &result) != ESP_OK) {
            return false;
        }
        address.s_addr = result.addr;
        return true;
    }
    addrinfo hints = {};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    addrinfo *result = nullptr;
    if (getaddrinfo(host, nullptr, &hints, &result) != 0) {
        return false;
    }
    const bool valid = result != nullptr && result->ai_addr != nullptr &&
                       result->ai_addrlen >= sizeof(sockaddr_in);
    if (valid) {
        address = reinterpret_cast<const sockaddr_in *>(result->ai_addr)->sin_addr;
    }
    if (result != nullptr) {
        freeaddrinfo(result);
    }
    return valid;
}
} // namespace

struct RemoteLogger::Runtime {
    struct Record {
        char text[288];
    };
    std::atomic<RemoteLogger *> owner{nullptr};
    std::atomic<TaskHandle_t> worker{nullptr};
    std::atomic<PrintHook> previous{&vprintf};
    std::atomic_bool ready{false};
    QueueHandle_t queue = nullptr;
    StaticQueue_t queue_control{};
    uint8_t queue_storage[kQueueLength * sizeof(Record)]{};
    char destination[254]{};
    char hostname[17]{};
};

RemoteLogger::Runtime RemoteLogger::runtime_;

RemoteLogger::RemoteLogger(Config config) : config_(config) {}

esp_err_t RemoteLogger::start()
{
    if (xPortInIsrContext()) {
        return ESP_ERR_INVALID_STATE;
    }
    if (config_.host == nullptr || config_.port == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    size_t length = 0;
    for (; length < sizeof(runtime_.destination) && config_.host[length] != '\0'; ++length) {
        const char c = config_.host[length];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == '.' || c == '-' || c == '_')) {
            return ESP_ERR_INVALID_ARG;
        }
    }
    if (length == 0 || length == sizeof(runtime_.destination)) {
        return ESP_ERR_INVALID_ARG;
    }
    RemoteLogger *expected = nullptr;
    if (!runtime_.owner.compare_exchange_strong(expected, this)) {
        return expected == this && runtime_.ready.load() ? ESP_OK : ESP_ERR_INVALID_STATE;
    }
    uint8_t mac[6];
    esp_err_t err = esp_read_mac(mac, ESP_MAC_WIFI_STA);
    if (err != ESP_OK) {
        runtime_.owner.store(nullptr);
        return err;
    }
    std::memcpy(runtime_.destination, config_.host, length + 1);
    std::snprintf(runtime_.hostname, sizeof(runtime_.hostname), "smart-fan-%02x%02x%02x",
                  static_cast<unsigned>(mac[3]), static_cast<unsigned>(mac[4]),
                  static_cast<unsigned>(mac[5]));
    runtime_.queue = xQueueCreateStatic(kQueueLength, sizeof(Runtime::Record),
                                      runtime_.queue_storage, &runtime_.queue_control);
    if (runtime_.queue == nullptr) {
        runtime_.owner.store(nullptr);
        return ESP_ERR_NO_MEM;
    }
    TaskHandle_t worker = nullptr;
    // DNS/libc formatting and health collection need more room than the hook.
    if (xTaskCreate(task_entry, "remote_syslog", 6144, this, tskIDLE_PRIORITY + 1, &worker) != pdPASS) {
        vQueueDelete(runtime_.queue);
        runtime_.queue = nullptr;
        runtime_.owner.store(nullptr);
        return ESP_ERR_NO_MEM;
    }
    runtime_.worker.store(worker);
    // ESP32-C6 is single-core. Prevent a task/ISR from entering the new hook
    // before the previous hook has been saved. The SDK setter is atomic only.
    portENTER_CRITICAL(&s_hook_install_lock);
    runtime_.previous.store(esp_log_set_vprintf(log_hook));
    runtime_.ready.store(true);
    portEXIT_CRITICAL(&s_hook_install_lock);
    xTaskNotifyGive(worker);
    return ESP_OK;
}

void RemoteLogger::set_network_connected(bool connected)
{
    uint32_t old = network_state_.load(std::memory_order_relaxed);
    do {
        if (static_cast<bool>(old & 1U) == connected) {
            return;
        }
    } while (!network_state_.compare_exchange_weak(old, ((old + 2U) & ~1U) | (connected ? 1U : 0U)));
    if (runtime_.ready.load() && runtime_.owner.load() == this) {
        TaskHandle_t worker = runtime_.worker.load();
        if (worker != nullptr) {
            xTaskNotifyGive(worker);
        }
    }
}

void RemoteLogger::set_fan_state(uint8_t speed, bool rotation)
{
    fan_state_.store(static_cast<uint32_t>(speed) | (rotation ? 0x100U : 0U), std::memory_order_relaxed);
}

int RemoteLogger::log_hook(const char *format, va_list args)
{
    RemoteLogger *owner = runtime_.owner.load();
    // Always preserve the original console output and its return value, even
    // for ISR, worker, filtered, truncated, or dropped remote records.
    if (!xPortInIsrContext() && owner != nullptr && runtime_.ready.load() &&
        xTaskGetCurrentTaskHandle() != runtime_.worker.load()) {
        Runtime::Record record = {};
        va_list copy;
        va_copy(copy, args);
        const int count = std::vsnprintf(record.text, sizeof(record.text), format, copy);
        va_end(copy);
        if (count > 0) {
            const size_t length = static_cast<size_t>(count) < sizeof(record.text)
                                      ? static_cast<size_t>(count) : sizeof(record.text) - 1;
            const size_t clean_length = syslog_format::sanitize(record.text, sizeof(record.text),
                                                               {record.text, length});
            syslog_format::LogLine line;
            if (syslog_format::parse_log_v1({record.text, clean_length}, line) &&
                syslog_format::should_forward(line) && xQueueSend(runtime_.queue, &record, 0) != pdTRUE) {
                owner->dropped_.fetch_add(1, std::memory_order_relaxed);
            }
        }
    }
    return runtime_.previous.load()(format, args);
}

void RemoteLogger::task_entry(void *context)
{
    static_cast<RemoteLogger *>(context)->task();
}

void RemoteLogger::task()
{
    int socket_fd = -1;
    sockaddr_in destination = {};
    destination.sin_family = AF_INET;
    destination.sin_port = htons(config_.port);
    bool resolved = false;
    bool health_pending = true;
    bool reported_health = false;
    uint32_t observed_network = UINT32_MAX;
    int64_t next_resolution = 0;
    int64_t refresh_resolution = 0;
    int64_t next_socket = 0;
    int64_t next_health = 0;

    const auto close_socket = [&] {
        if (socket_fd >= 0) {
            lwip_close(socket_fd);
            socket_fd = -1;
        }
    };
    const auto send_message = [&](syslog_format::Level level, std::string_view tag,
                                  const char *msgid, std::string_view message) {
        char packet[syslog_format::kPacketCapacity];
        const size_t length = syslog_format::format_packet(packet, sizeof(packet), level,
                                                           runtime_.hostname, tag, msgid, message);
        if (length == 0) {
            return false;
        }
        const ssize_t sent = lwip_sendto(socket_fd, packet, length, 0,
                                         reinterpret_cast<const sockaddr *>(&destination), sizeof(destination));
        if (sent != static_cast<ssize_t>(length)) {
            udp_send_failures_.fetch_add(1, std::memory_order_relaxed);
            return false;
        }
        return true;
    };
    const auto send_health = [&](int64_t now) {
        wifi_ap_record_t access_point = {};
        char rssi[16] = "unavailable";
        if (esp_wifi_sta_get_ap_info(&access_point) == ESP_OK) {
            std::snprintf(rssi, sizeof(rssi), "%d", static_cast<int>(access_point.rssi));
        }
        const uint32_t fan = fan_state_.load(std::memory_order_relaxed);
        char body[512];
        std::snprintf(body, sizeof(body),
                      "reason=%s version=%s display=%s reset=%s uptime_s=%" PRIu64
                      " heap_free=%" PRIu32 " heap_min=%" PRIu32 " rssi_dbm=%s"
                      " speed=%u rotation=%u dropped=%" PRIu32 " udp_send_failures=%" PRIu32 " ota_state=%s",
                      health_pending ? (reported_health ? "reconnect" : "startup") : "periodic",
                      esp_app_get_description()->version, firmware::kDisplayVariant,
                      reset_name(esp_reset_reason()), static_cast<uint64_t>(now / 1000000),
                      esp_get_free_heap_size(), esp_get_minimum_free_heap_size(), rssi,
                      static_cast<unsigned>(fan & 0xffU), static_cast<unsigned>((fan >> 8) & 1U),
                      dropped_.load(std::memory_order_relaxed), udp_send_failures_.load(std::memory_order_relaxed),
                      running_ota_state());
        return send_message(syslog_format::Level::info, "remote_logger", "health", body);
    };

    // Startup handshake: no socket/DNS/worker logs before hook installation.
    TickType_t wait = portMAX_DELAY;
    for (;;) {
        ulTaskNotifyTake(pdTRUE, wait);
        wait = pdMS_TO_TICKS(1000);
        const uint32_t network = network_state_.load();
        const bool connected = (network & 1U) != 0;
        int64_t now = esp_timer_get_time();
        if (network != observed_network) {
            observed_network = network;
            resolved = false;
            close_socket();
            if (connected) {
                health_pending = true;
            }
        }
        if (!connected) {
            continue; // Keep a bounded backlog; producers drop new records when full.
        }
        if (!resolved || now >= refresh_resolution) {
            if (now < next_resolution) {
                continue;
            }
            resolved = resolve_ipv4(runtime_.destination, destination.sin_addr);
            now = esp_timer_get_time();
            next_resolution = now + kRetryUs;
            if (!resolved) {
                close_socket();
                continue;
            }
            refresh_resolution = now + kRefreshUs;
        }
        // DNS may have blocked while the network changed. Never drain the
        // backlog using a stale connection epoch; reconnect health goes first.
        if (network_state_.load() != observed_network) {
            continue;
        }
        if (socket_fd < 0) {
            if (now < next_socket) {
                continue;
            }
            socket_fd = lwip_socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
            if (socket_fd >= 0) {
                const int flags = lwip_fcntl(socket_fd, F_GETFL, 0);
                if (flags < 0 || lwip_fcntl(socket_fd, F_SETFL, flags | O_NONBLOCK) < 0) {
                    close_socket();
                }
            }
            if (socket_fd < 0) {
                next_socket = now + kRetryUs;
                continue;
            }
        }
        if (health_pending || (config_.health_interval_seconds != 0 && now >= next_health)) {
            if (!send_health(now)) {
                close_socket();
                next_socket = esp_timer_get_time() + kRetryUs;
                continue;
            }
            health_pending = false;
            reported_health = true;
            next_health = esp_timer_get_time() + static_cast<int64_t>(config_.health_interval_seconds) * 1000000;
        }
        Runtime::Record record;
        // Bound each drain even if producers are continuously logging.
        for (size_t i = 0; i < kQueueLength && network_state_.load() == observed_network; ++i) {
            if (xQueueReceive(runtime_.queue, &record, 0) != pdTRUE) {
                break;
            }
            syslog_format::LogLine line;
            if (syslog_format::parse_log_v1(record.text, line) &&
                !send_message(line.level, line.tag, "log", record.text)) {
                dropped_.fetch_add(1, std::memory_order_relaxed);
                close_socket();
                next_socket = esp_timer_get_time() + kRetryUs;
                break;
            }
        }
    }
}

#endif // CONFIG_FAN_SYSLOG_ENABLED
