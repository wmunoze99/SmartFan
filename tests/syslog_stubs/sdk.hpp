#pragma once

// SDK surface only; the formatter, hook, queue policy and worker stay production code.
#include <algorithm>
#include <array>
#include <cstdarg>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <functional>
#include <limits>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>
#include <arpa/inet.h>
#include <netdb.h>
#include <sys/socket.h>

using esp_err_t = int;
constexpr esp_err_t ESP_OK = 0, ESP_FAIL = -1, ESP_ERR_NO_MEM = -2;
constexpr esp_err_t ESP_ERR_INVALID_STATE = -3, ESP_ERR_INVALID_ARG = -4;
using TaskHandle_t = void *;
using TickType_t = uint32_t;
using BaseType_t = int;
constexpr BaseType_t pdTRUE = 1, pdFALSE = 0, pdPASS = 1;
constexpr TickType_t portMAX_DELAY = std::numeric_limits<TickType_t>::max();
constexpr unsigned tskIDLE_PRIORITY = 0;
#define pdMS_TO_TICKS(ms) (static_cast<TickType_t>(ms))
using portMUX_TYPE = int;
#define portMUX_INITIALIZER_UNLOCKED 0
#define portENTER_CRITICAL(lock) (++*(lock))
#define portEXIT_CRITICAL(lock) (--*(lock))
struct StaticQueue_t { int unused = 0; };
using QueueHandle_t = StaticQueue_t *;
using vprintf_like_t = int (*)(const char *, va_list);

constexpr int ESP_MAC_WIFI_STA = 0;
enum esp_reset_reason_t {
    ESP_RST_UNKNOWN, ESP_RST_POWERON, ESP_RST_EXT, ESP_RST_SW, ESP_RST_PANIC,
    ESP_RST_INT_WDT, ESP_RST_TASK_WDT, ESP_RST_WDT, ESP_RST_DEEPSLEEP,
    ESP_RST_BROWNOUT, ESP_RST_SDIO, ESP_RST_USB, ESP_RST_JTAG, ESP_RST_EFUSE,
    ESP_RST_PWR_GLITCH, ESP_RST_CPU_LOCKUP
};
struct esp_partition_t { int unused; };
enum esp_ota_img_states_t {
    ESP_OTA_IMG_NEW, ESP_OTA_IMG_PENDING_VERIFY, ESP_OTA_IMG_VALID,
    ESP_OTA_IMG_INVALID, ESP_OTA_IMG_ABORTED, ESP_OTA_IMG_UNDEFINED
};
struct esp_app_desc_t { char version[32]; };
struct wifi_ap_record_t { int8_t rssi; };
struct esp_ip4_addr_t { uint32_t addr; };

namespace fake {
inline void require(bool condition, const std::string &message)
{
    if (!condition) throw std::runtime_error(message);
}
struct Stop {};
struct Step { int64_t at; std::function<void()> before; };
struct Datagram { int64_t at; std::string bytes; sockaddr_in destination; bool success; };
struct Resolution { int64_t at; std::string name; };
struct Runtime {
    int64_t now = 0;
    bool isr = false, in_hook = false;
    int worker_token = 0, producer_token = 0;
    TaskHandle_t current = &producer_token;
    void (*task)(void *) = nullptr;
    void *context = nullptr;
    vprintf_like_t hook = nullptr;
    int install_count = 0, notify_count = 0, console_return = -73;
    std::vector<std::string> console;
    size_t queue_length = 0, item_size = 0, head = 0, queued = 0;
    uint8_t *storage = nullptr;
    StaticQueue_t *queue = nullptr;
    int queue_sends = 0, queue_receives = 0, queue_deletes = 0;
    esp_err_t mac_error = ESP_OK;
    bool fail_queue = false, fail_task = false;
    std::vector<Step> steps;
    size_t next_step = 0;
    std::vector<TickType_t> waits;
    std::vector<Resolution> dns, mdns;
    bool resolve_ok = true;
    std::function<void()> on_resolve;
    int mdns_inits = 0, mdns_frees = 0;
    int socket_calls = 0, close_calls = 0, fail_sockets = 0, fail_sends = 0;
    int fcntl_gets = 0, fcntl_sets = 0;
    bool fail_fcntl = false;
    std::vector<Datagram> datagrams;
    std::function<void()> on_send;
};
inline Runtime runtime;

inline void networking()
{
    const auto &r = runtime;
    require(!r.in_hook && !r.isr && r.current == &r.worker_token, "network operation outside worker");
}

inline int old_vprintf(const char *format, va_list args)
{
    char output[8192];
    // Consume the original va_list: a missing production va_copy corrupts this output.
    const int count = std::vsnprintf(output, sizeof(output), format, args);
    require(count >= 0 && static_cast<size_t>(count) < sizeof(output), "console fixture overflow");
    runtime.console.emplace_back(output, static_cast<size_t>(count));
    return runtime.console_return;
}

inline std::string queued_text(size_t index)
{
    auto &r = runtime;
    require(index < r.queued, "queue index out of range");
    const char *text = reinterpret_cast<const char *>(r.storage + ((r.head + index) % r.queue_length) * r.item_size);
    const void *end = std::memchr(text, '\0', r.item_size);
    require(end != nullptr, "queued record not byte-bounded/NUL-terminated");
    return {text, static_cast<const char *>(end)};
}
} // namespace fake

inline bool xPortInIsrContext() { return fake::runtime.isr; }
inline TaskHandle_t xTaskGetCurrentTaskHandle() { return fake::runtime.current; }
inline vprintf_like_t esp_log_set_vprintf(vprintf_like_t hook)
{
    auto &r = fake::runtime;
    const auto previous = r.hook == nullptr ? fake::old_vprintf : r.hook;
    r.hook = hook;
    ++r.install_count;
    return previous;
}
inline esp_err_t esp_read_mac(uint8_t *mac, int kind)
{
    fake::require(kind == ESP_MAC_WIFI_STA, "wrong MAC interface");
    const uint8_t value[] = {0, 1, 2, 0xab, 0xcd, 0xef};
    std::memcpy(mac, value, sizeof(value));
    return fake::runtime.mac_error;
}
inline QueueHandle_t xQueueCreateStatic(unsigned length, unsigned item_size,
                                       uint8_t *storage, StaticQueue_t *control)
{
    auto &r = fake::runtime;
    if (r.fail_queue) return nullptr;
    r.queue_length = length;
    r.item_size = item_size;
    r.storage = storage;
    r.queue = control;
    r.head = r.queued = 0;
    return control;
}
inline void vQueueDelete(QueueHandle_t queue)
{
    fake::require(queue == fake::runtime.queue, "deleting unexpected queue");
    ++fake::runtime.queue_deletes;
}
inline BaseType_t xQueueSend(QueueHandle_t queue, const void *item, TickType_t wait)
{
    auto &r = fake::runtime;
    fake::require(queue == r.queue && wait == 0, "hook queue send must be nonblocking");
    fake::require(!r.isr && r.current != &r.worker_token, "ISR/worker reached producer queue send");
    ++r.queue_sends;
    if (r.queued == r.queue_length) return pdFALSE;
    std::memcpy(r.storage + ((r.head + r.queued) % r.queue_length) * r.item_size, item, r.item_size);
    ++r.queued;
    return pdTRUE;
}
inline BaseType_t xQueueReceive(QueueHandle_t queue, void *item, TickType_t wait)
{
    auto &r = fake::runtime;
    fake::require(queue == r.queue && wait == 0 && r.current == &r.worker_token,
                  "worker drain must be nonblocking");
    ++r.queue_receives;
    if (!r.queued) return pdFALSE;
    std::memcpy(item, r.storage + r.head * r.item_size, r.item_size);
    r.head = (r.head + 1) % r.queue_length;
    --r.queued;
    return pdTRUE;
}
inline BaseType_t xTaskCreate(void (*entry)(void *), const char *, unsigned,
                             void *context, unsigned, TaskHandle_t *out)
{
    auto &r = fake::runtime;
    if (r.fail_task) return pdFALSE;
    r.task = entry;
    r.context = context;
    *out = &r.worker_token;
    return pdPASS;
}
inline void xTaskNotifyGive(TaskHandle_t task)
{
    fake::require(task == &fake::runtime.worker_token, "wrong notified task");
    ++fake::runtime.notify_count;
}
inline uint32_t ulTaskNotifyTake(BaseType_t clear, TickType_t wait)
{
    auto &r = fake::runtime;
    fake::require(clear == pdTRUE && r.hook && r.notify_count > 0, "worker ran before start handshake");
    fake::require(wait == (r.waits.empty() ? portMAX_DELAY : pdMS_TO_TICKS(1000)),
                  "worker should yield between retries/drains");
    r.waits.push_back(wait);
    if (r.next_step == r.steps.size()) throw fake::Stop{};
    const auto step = r.steps[r.next_step++];
    fake::require(step.at >= r.now, "scripted clock moved backwards");
    r.now = step.at;
    if (step.before) step.before();
    return 1;
}
inline int64_t esp_timer_get_time() { return fake::runtime.now; }
inline const esp_app_desc_t *esp_app_get_description()
{
    static const esp_app_desc_t description{"native-test"};
    return &description;
}
inline esp_reset_reason_t esp_reset_reason() { return ESP_RST_POWERON; }
inline uint32_t esp_get_free_heap_size() { return 42000; }
inline uint32_t esp_get_minimum_free_heap_size() { return 21000; }
inline esp_err_t esp_wifi_sta_get_ap_info(wifi_ap_record_t *record)
{ record->rssi = -42; return ESP_OK; }
inline const esp_partition_t *esp_ota_get_running_partition()
{ static const esp_partition_t partition{}; return &partition; }
inline esp_err_t esp_ota_get_state_partition(const esp_partition_t *, esp_ota_img_states_t *state)
{ *state = ESP_OTA_IMG_VALID; return ESP_OK; }

inline esp_err_t mdns_init() { ++fake::runtime.mdns_inits; return ESP_OK; }
inline void mdns_free() { ++fake::runtime.mdns_frees; }
inline esp_err_t mdns_query_a(const char *name, uint32_t timeout, esp_ip4_addr_t *out)
{
    fake::networking();
    auto &r = fake::runtime;
    fake::require(timeout > 0 && timeout <= 1500, "unbounded mDNS query");
    r.mdns.push_back({r.now, name});
    if (r.on_resolve) r.on_resolve();
    out->addr = htonl(0xc0000201U);
    return r.resolve_ok ? ESP_OK : ESP_ERR_INVALID_STATE;
}
inline int fake_getaddrinfo(const char *name, const char *service, const addrinfo *hints, addrinfo **out)
{
    fake::networking();
    auto &r = fake::runtime;
    fake::require(service == nullptr && hints->ai_family == AF_INET && hints->ai_socktype == SOCK_DGRAM,
                  "DNS must request IPv4 UDP");
    r.dns.push_back({r.now, name});
    if (r.on_resolve) r.on_resolve();
    if (!r.resolve_ok) { *out = nullptr; return EAI_FAIL; }
    static sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(0xc0000201U);
    static addrinfo result{};
    result.ai_addr = reinterpret_cast<sockaddr *>(&address);
    result.ai_addrlen = sizeof(address);
    *out = &result;
    return 0;
}
inline void fake_freeaddrinfo(addrinfo *) { fake::networking(); }
#define getaddrinfo fake_getaddrinfo
#define freeaddrinfo fake_freeaddrinfo

inline int lwip_socket(int domain, int type, int protocol)
{
    fake::networking();
    auto &r = fake::runtime;
    fake::require(domain == AF_INET && type == SOCK_DGRAM && protocol == IPPROTO_UDP, "not IPv4 UDP");
    ++r.socket_calls;
    if (r.fail_sockets > 0) { --r.fail_sockets; return -1; }
    return 42;
}
inline int lwip_fcntl(int fd, int command, int flags)
{
    fake::networking();
    auto &r = fake::runtime;
    fake::require(fd == 42, "unexpected fcntl socket");
    if (command == F_GETFL) { ++r.fcntl_gets; return r.fail_fcntl ? -1 : 0; }
    fake::require(command == F_SETFL && (flags & O_NONBLOCK), "socket not configured nonblocking");
    ++r.fcntl_sets;
    return r.fail_fcntl ? -1 : 0;
}
inline int lwip_close(int fd)
{
    fake::networking();
    fake::require(fd == 42, "unexpected close socket");
    ++fake::runtime.close_calls;
    return 0;
}
inline ssize_t lwip_sendto(int fd, const void *data, size_t length, int flags,
                           const sockaddr *destination, socklen_t address_length)
{
    fake::networking();
    auto &r = fake::runtime;
    fake::require(fd == 42 && flags == 0 && address_length == sizeof(sockaddr_in), "unexpected UDP arguments");
    fake::require(length > 0 && length < 768 && std::memchr(data, '\0', length) == nullptr,
                  "wire bytes include terminator or exceed packet bound");
    const auto &address = *reinterpret_cast<const sockaddr_in *>(destination);
    const bool success = r.fail_sends == 0;
    if (!success) --r.fail_sends;
    r.datagrams.push_back({r.now, {static_cast<const char *>(data), length}, address, success});
    if (r.on_send) r.on_send();
    return success ? static_cast<ssize_t>(length) : -1;
}
