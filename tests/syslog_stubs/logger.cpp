#include "sdk.hpp"
#include "remote_logger.hpp"

#include <iostream>

namespace {
using fake::require;
constexpr int64_t second = 1000000;

int log(const char *format, ...)
{
    auto &r = fake::runtime;
    require(r.hook != nullptr, "hook not installed");
    va_list args;
    va_start(args, format);
    r.in_hook = true;
    const int result = r.hook(format, args);
    r.in_hook = false;
    va_end(args);
    return result;
}

void producer(const std::function<void()> &action)
{
    auto &r = fake::runtime;
    const auto saved = r.current;
    r.current = &r.producer_token;
    action();
    r.current = saved;
}

struct Fixture {
    RemoteLogger logger;
    explicit Fixture(const char *host = "192.0.2.1", uint32_t interval = 0)
        : logger({host, 5514, interval})
    {
        require(logger.start() == ESP_OK, "real start failed");
        auto &r = fake::runtime;
        require(r.hook && r.task && r.install_count == 1 && r.notify_count == 1,
                "start did not install hook and notify worker");
        require(r.queue_length == 16 && r.item_size == 288, "production bounded queue/record changed");
        require(r.dns.empty() && r.mdns.empty() && r.socket_calls == 0 && r.datagrams.empty(),
                "start performed network operations");
    }
    void step(int64_t at, std::function<void()> before = {})
    { fake::runtime.steps.push_back({at, std::move(before)}); }
    void run()
    {
        auto &r = fake::runtime;
        r.current = &r.worker_token;
        try {
            r.task(r.context);
            throw std::runtime_error("worker returned unexpectedly");
        } catch (const fake::Stop &) {
            require(r.next_step == r.steps.size(), "worker missed scripted steps");
        }
        require(r.mdns_inits == 0 && r.mdns_frees == 0, "logger changed shared mDNS lifecycle");
    }
};

void has(const std::string &text, const std::string &part)
{ require(text.find(part) != std::string::npos, "missing: " + part + " in " + text); }

void hook_passthrough()
{
    Fixture f;
    auto &r = fake::runtime;
    char expected[256];
    std::snprintf(expected, sizeof(expected), "I (%u) %s: n=%d hex=%#x f=%*.*f text=%s char=%c\n",
                  123U, "smart_fan", -42, 0xabU, 8, 3, 1.25, "value", 'Z');
    require(log("I (%u) %s: n=%d hex=%#x f=%*.*f text=%s char=%c\n",
                123U, "smart_fan", -42, 0xabU, 8, 3, 1.25, "value", 'Z') == -73,
            "hook did not preserve previous vprintf return");
    require(r.console == std::vector<std::string>{expected}, "original vprintf arguments/output changed");
    std::string sanitized = expected;
    sanitized.back() = ' ';
    require(r.queued == 1 && fake::queued_text(0) == sanitized, "formatted argument copy corrupted");
    require(log("plain %s %lld %.2f", "printf", 1234567890123LL, 2.5) == -73 &&
                r.console.back() == "plain printf 1234567890123 2.50" && r.queued == 1,
            "unstructured console output/return changed");
    require(r.dns.empty() && r.mdns.empty() && r.socket_calls == 0 && r.datagrams.empty(),
            "hook performed DNS/socket/send operations");
}

void hook_filtering()
{
    Fixture f;
    auto &r = fake::runtime;
    for (const char *tag : {"smart_fan", "fan_controller", "fan_ota", "status_display", "remote_logger"})
        require(log("\x1b[32mI (12:34:56.789) %s: ok\x1b[0m\n", tag) == -73, "INFO return changed");
    require(r.queued == 5, "application INFO missing");
    for (const char *tag : {"wifi", "smart_fan_extra", "Smart_fan", "chip"})
        log("I (123) %s: filtered\n", tag);
    for (char level : {'D', 'V'}) {
        log("%c (123) smart_fan: filtered\n", level);
        log("%c (123) wifi: filtered\n", level);
    }
    log("unstructured Matter output\n");
    log("I (bad) smart_fan: invalid\n");
    require(r.queued == 5, "filtered/malformed output enqueued");
    for (char level : {'W', 'E'})
        for (const char *tag : {"wifi", "unknown", "smart_fan"}) log("%c (123) %s: forwarded\n", level, tag);
    require(r.queued == 11 && r.queue_sends == 11 && r.console.size() == 21,
            "WARN/ERROR forwarding or console preservation mismatch");
    require(fake::queued_text(0) == "I (12:34:56.789) smart_fan: ok ", "ANSI not removed by real hook");
}

void hook_context()
{
    Fixture f;
    auto &r = fake::runtime;
    r.isr = true;
    require(log("E (1) wifi: ISR %d\n", 7) == -73, "ISR console return changed");
    r.isr = false;
    r.current = &r.worker_token;
    require(log("W (2) wifi: worker %s\n", "message") == -73, "worker console return changed");
    r.current = &r.producer_token;
    require(log("E (3) wifi: producer\n") == -73, "producer console return changed");
    require(r.queue_sends == 1 && r.queued == 1 && fake::queued_text(0) == "E (3) wifi: producer ",
            "ISR/worker was remotely enqueued");
    require(r.console == std::vector<std::string>{"E (1) wifi: ISR 7\n", "W (2) wifi: worker message\n",
                                                "E (3) wifi: producer\n"}, "excluded console output changed");
}

void hook_copy_and_bounds()
{
    Fixture f;
    auto &r = fake::runtime;
    char text[] = "original";
    log("I (1) smart_fan: %s", text);
    std::memset(text, 'x', sizeof(text) - 1);
    const std::string prefix = "W (2) wifi: ";
    const std::string long_body(2000, 'z');
    require(log("%s%s", prefix.c_str(), long_body.c_str()) == -73, "truncated console return changed");
    require(r.console.back() == prefix + long_body, "hook truncated original console");
    require(fake::queued_text(0) == "I (1) smart_fan: original", "record retained caller string pointer");
    require(fake::queued_text(1) == prefix + long_body.substr(0, 287 - prefix.size()),
            "record not truncated at 287 bytes plus NUL");
    log("W (3) wifi: first\nE (4) wifi: second\r\n<131>1 - forged\tend");
    require(r.queued == 3 && fake::queued_text(2) ==
                "W (3) wifi: first E (4) wifi: second  <131>1 - forged end",
            "one multiline hook call must enqueue one sanitized record");
    f.logger.set_network_connected(true);
    f.step(0);
    f.run();
    require(r.datagrams.size() == 4 && r.queued == 0, "multiline hook input emitted extra datagrams");
    has(r.datagrams[3].bytes, " - log - W (3) wifi: first E (4) wifi: second  <131>1 - forged end");
    require(r.datagrams[3].bytes.find_first_of("\r\n\t\x1b") == std::string::npos,
            "multiline/control bytes reached wire");
}

void offline_queue()
{
    Fixture f;
    auto &r = fake::runtime;
    for (int i = 0; i < 24; ++i) require(log("W (1) wifi: item=%d", i) == -73, "full queue return changed");
    require(r.queued == 16 && r.queue_sends == 24 && r.console.size() == 24, "offline queue not bounded");
    for (int i = 0; i < 16; ++i)
        require(fake::queued_text(i) == "W (1) wifi: item=" + std::to_string(i), "queue overwrote older records");
    f.step(0);
    f.step(second, [&] {
        require(r.queued == 16 && r.queue_receives == 0 && r.datagrams.empty() &&
                    r.dns.empty() && r.mdns.empty() && r.socket_calls == 0, "offline worker drained/networked");
    });
    f.step(2 * second, [&] { f.logger.set_network_connected(true); });
    f.run();
    require(r.datagrams.size() == 17 && r.queued == 0, "backlog not drained after connection");
    has(r.datagrams[0].bytes, " - health - reason=startup");
    has(r.datagrams[0].bytes, " dropped=8 udp_send_failures=0");
    for (size_t i = 0; i < 16; ++i)
        has(r.datagrams[i + 1].bytes, " - log - W (1) wifi: item=" + std::to_string(i));
}

void startup_health()
{
    char host[] = "192.0.2.1";
    Fixture f(host);
    auto &r = fake::runtime;
    std::memset(host, 'x', sizeof(host) - 1);
    f.logger.set_fan_state(3, true);
    f.logger.set_network_connected(true);
    log("E (1) wifi: backlog");
    f.step(7 * second);
    f.run();
    require(r.datagrams.size() == 2 && r.dns.empty() && r.mdns.empty(), "literal IP must bypass resolution");
    require(r.fcntl_gets == 1 && r.fcntl_sets == 1, "nonblocking socket flags not set");
    const auto &health = r.datagrams[0];
    require(health.success && ntohs(health.destination.sin_port) == 5514 &&
                ntohl(health.destination.sin_addr.s_addr) == 0xc0000201U, "wrong copied destination/port");
    require(health.bytes == "<134>1 - smart-fan-abcdef remote_logger - health - reason=startup"
                " version=native-test display=ssd1306 reset=POWERON uptime_s=7 heap_free=42000"
                " heap_min=21000 rssi_dbm=-42 speed=3 rotation=1 dropped=0 udp_send_failures=0 ota_state=VALID",
            "startup health payload/header mismatch: " + health.bytes);
    require(r.datagrams[1].bytes == "<131>1 - smart-fan-abcdef wifi - log - E (1) wifi: backlog",
            "startup health did not precede backlog");
}

void reconnect()
{
    Fixture f;
    auto &r = fake::runtime;
    f.logger.set_network_connected(true);
    f.step(0);
    f.step(second, [&] {
        f.logger.set_network_connected(false);
        producer([] { log("W (1) wifi: offline backlog"); });
    });
    f.step(45 * second, [&] {
        require(r.datagrams.size() == 1 && r.queued == 1 && r.close_calls == 1,
                "disconnect must close socket and preserve backlog");
        f.logger.set_network_connected(true);
    });
    f.step(90 * second, [&] {
        require(r.datagrams.size() == 3, "reconnect backlog missing");
        has(r.datagrams[1].bytes, " - health - reason=reconnect");
        has(r.datagrams[2].bytes, "offline backlog");
        f.logger.set_network_connected(false);
        f.logger.set_network_connected(true);
        producer([] { log("W (2) wifi: between wakeups"); });
    });
    f.step(91 * second, [&] { f.logger.set_network_connected(true); });
    f.run();
    require(r.datagrams.size() == 5 && r.socket_calls == 3 && r.close_calls == 2,
            "collapsed disconnect/reconnect not observed, or identical state triggered health");
    has(r.datagrams[3].bytes, " - health - reason=reconnect");
    has(r.datagrams[4].bytes, "between wakeups");
}

void mdns(const char *host)
{
    Fixture f(host);
    auto &r = fake::runtime;
    f.logger.set_network_connected(true);
    f.step(0);
    f.run();
    const std::string expected = std::string(host).substr(0, std::string(host).find('.'));
    require(r.mdns.size() == 1 && r.mdns[0].name == expected && r.dns.empty(),
            ".local must use mDNS with suffix/trailing dot removed");
    require(r.datagrams.size() == 1 && r.mdns_inits == 0 && r.mdns_frees == 0,
            "mDNS resolution/ownership mismatch");
}

void resolution_retry(const char *host)
{
    Fixture f(host);
    auto &r = fake::runtime;
    const bool local = std::string(host).find("local") != std::string::npos;
    auto queries = [&]() -> const std::vector<fake::Resolution> & { return local ? r.mdns : r.dns; };
    r.resolve_ok = false;
    f.logger.set_network_connected(true);
    log("W (1) wifi: pending resolution");
    f.step(0);
    for (int i = 1; i < 45; ++i) f.step(i * second, [&] {
        require(queries().size() == 1 && r.socket_calls == 0 && r.datagrams.empty() && r.queued == 1,
                "resolution retried early or drained unresolved backlog");
    });
    f.step(45 * second, [&] { r.resolve_ok = true; });
    f.run();
    require(queries().size() == 2 && queries()[0].at == 0 && queries()[1].at == 45 * second,
            "45-second resolution retry deadline changed");
    require(r.datagrams.size() == 2 && r.datagrams[0].at == 45 * second,
            "recovery did not send health then queued log");
    has(r.datagrams[0].bytes, "reason=startup");
    has(r.datagrams[1].bytes, "pending resolution");
    require((local ? r.dns : r.mdns).empty(), "resolver used wrong DNS path");
}

void socket_retry(bool fcntl_failure)
{
    Fixture f;
    auto &r = fake::runtime;
    if (fcntl_failure) r.fail_fcntl = true;
    else r.fail_sockets = 1;
    f.logger.set_network_connected(true);
    log("W (1) wifi: pending socket");
    f.step(0);
    for (int i = 1; i < 45; ++i) f.step(i * second, [&] {
        require(r.socket_calls == 1 && r.datagrams.empty() && r.queued == 1,
                "socket retried early or drained without nonblocking socket");
    });
    f.step(45 * second, [&] { r.fail_fcntl = false; });
    f.run();
    require(r.socket_calls == 2 && r.datagrams.size() == 2 && r.datagrams[0].at == 45 * second,
            "socket did not recover at retry deadline");
    require(r.close_calls == (fcntl_failure ? 1 : 0), "failed nonblocking socket not closed");
}

void send_retry(bool health_failure)
{
    Fixture f;
    auto &r = fake::runtime;
    f.logger.set_network_connected(true);
    if (health_failure) r.fail_sends = 1;
    else r.on_send = [&] {
        if (r.datagrams.size() == 1) r.fail_sends = 1;
    };
    log("W (1) wifi: first");
    log("W (2) wifi: second");
    f.step(0);
    for (int i = 1; i < 45; ++i) f.step(i * second, [&] {
        require(r.socket_calls == 1 && r.close_calls == 1 &&
                    r.datagrams.size() == (health_failure ? 1U : 2U) &&
                    r.queued == (health_failure ? 2U : 1U), "send failure retry/backlog policy changed");
    });
    f.step(45 * second);
    f.step(46 * second, [&] {
        f.logger.set_network_connected(false);
        f.logger.set_network_connected(true);
    });
    f.run();
    require(r.socket_calls == 3 && r.queued == 0, "send recovery/reconnect incomplete");
    if (health_failure) {
        require(r.datagrams.size() == 5, "failed health must preserve and later drain whole backlog");
        has(r.datagrams[1].bytes, "reason=startup");
        has(r.datagrams[1].bytes, "dropped=0 udp_send_failures=1");
        has(r.datagrams[2].bytes, "first");
        has(r.datagrams[3].bytes, "second");
    } else {
        require(r.datagrams.size() == 4 && !r.datagrams[1].success, "failed log should be dropped, not replayed");
        has(r.datagrams[2].bytes, "second");
        has(r.datagrams[3].bytes, "reason=reconnect");
        has(r.datagrams[3].bytes, "dropped=1 udp_send_failures=1");
    }
}

void health_interval(uint32_t interval)
{
    Fixture f("192.0.2.1", interval);
    auto &r = fake::runtime;
    f.logger.set_network_connected(true);
    f.step(0);
    if (interval == 0) {
        f.step(86400 * second);
        f.step(86401 * second, [&] {
            require(r.datagrams.size() == 1, "zero interval emitted periodic health");
            producer([] { log("I (1) smart_fan: still forwarded"); });
        });
        f.run();
        require(r.datagrams.size() == 2, "zero interval disabled queued logs");
        has(r.datagrams[0].bytes, "reason=startup");
        has(r.datagrams[1].bytes, "still forwarded");
    } else {
        f.step(9 * second);
        f.step(10 * second, [&] {
            require(r.datagrams.size() == 1, "periodic health emitted early");
            f.logger.set_fan_state(2, false);
        });
        f.step(19 * second);
        f.step(20 * second);
        f.run();
        require(r.datagrams.size() == 3 && r.datagrams[1].at == 10 * second &&
                    r.datagrams[2].at == 20 * second, "periodic health deadline changed");
        has(r.datagrams[1].bytes, "reason=periodic");
        has(r.datagrams[1].bytes, "speed=2 rotation=0");
    }
}

void stale_dns_epoch()
{
    Fixture f("collector.example");
    auto &r = fake::runtime;
    f.logger.set_network_connected(true);
    log("W (1) wifi: retained");
    r.on_resolve = [&] {
        f.logger.set_network_connected(false);
        f.logger.set_network_connected(true);
    };
    f.step(0);
    f.step(second, [&] {
        require(r.datagrams.empty() && r.socket_calls == 0 && r.queued == 1,
                "worker sent/drained using connection epoch invalidated during DNS");
        r.on_resolve = {};
    });
    f.step(45 * second);
    f.run();
    require(r.dns.size() == 2 && r.datagrams.size() == 2, "epoch recovery did not resolve again");
    has(r.datagrams[0].bytes, "reason=startup");
    has(r.datagrams[1].bytes, "retained");
}

void bounded_drain()
{
    Fixture f;
    auto &r = fake::runtime;
    f.logger.set_network_connected(true);
    log("W (1) wifi: initial");
    r.on_send = [&] { producer([] { log("W (1) wifi: continuous producer"); }); };
    f.step(0);
    f.step(second, [&] {
        require(r.datagrams.size() == 17 && r.queued == 2, "worker did not bound drain to 16 records per wake");
        r.on_send = {};
    });
    f.run();
    require(r.datagrams.size() == 19 && r.queued == 0, "bounded drain lost remaining records");
}

void start_validation()
{
    auto &r = fake::runtime;
    r.isr = true;
    RemoteLogger from_isr({"192.0.2.1", 514, 0});
    require(from_isr.start() == ESP_ERR_INVALID_STATE, "ISR start allowed");
    r.isr = false;
    for (const char *host : {static_cast<const char *>(nullptr), "", "udp://nas.local", "host:514",
                             "a b", "a\nb", "caf\xc3\xa9", "[::1]"}) {
        RemoteLogger invalid({host, 514, 0});
        require(invalid.start() == ESP_ERR_INVALID_ARG, "invalid destination accepted");
    }
    const std::string too_long(254, 'x');
    RemoteLogger invalid_length({too_long.c_str(), 514, 0});
    require(invalid_length.start() == ESP_ERR_INVALID_ARG, "254-byte host accepted");
    RemoteLogger invalid_port({"nas.local", 0, 0});
    require(invalid_port.start() == ESP_ERR_INVALID_ARG, "port zero accepted");
    require(r.install_count == 0 && !r.task && r.console.empty(), "invalid start had side effects");
    Fixture f;
    require(f.logger.start() == ESP_OK && r.install_count == 1, "same-owner start not idempotent");
    RemoteLogger other({"nas.local", 514, 0});
    require(other.start() == ESP_ERR_INVALID_STATE && r.install_count == 1, "second owner replaced hook");
}

void start_retry()
{
    auto &r = fake::runtime;
    RemoteLogger logger({"nas.local", 514, 0});
    r.mac_error = ESP_FAIL;
    require(logger.start() == ESP_FAIL && r.install_count == 0, "MAC error not returned");
    r.mac_error = ESP_OK;
    r.fail_queue = true;
    require(logger.start() == ESP_ERR_NO_MEM && r.install_count == 0, "queue allocation error not returned");
    r.fail_queue = false;
    r.fail_task = true;
    require(logger.start() == ESP_ERR_NO_MEM && r.queue_deletes == 1 && r.install_count == 0,
            "task error did not clean up without installing hook");
    r.fail_task = false;
    require(logger.start() == ESP_OK && r.install_count == 1, "failed start could not be retried");
}
} // namespace

int main(int argc, char **argv)
{
    try {
        require(argc >= 2, "expected scenario");
        const std::string scenario = argv[1];
        const char *host = argc == 3 ? argv[2] : "nas.local";
        if (scenario == "hook_passthrough") hook_passthrough();
        else if (scenario == "hook_filtering") hook_filtering();
        else if (scenario == "hook_context") hook_context();
        else if (scenario == "hook_copy_and_bounds") hook_copy_and_bounds();
        else if (scenario == "offline_queue") offline_queue();
        else if (scenario == "startup_health") startup_health();
        else if (scenario == "reconnect") reconnect();
        else if (scenario == "mdns") mdns(host);
        else if (scenario == "resolution_retry") resolution_retry(host);
        else if (scenario == "socket_retry") socket_retry(false);
        else if (scenario == "fcntl_retry") socket_retry(true);
        else if (scenario == "health_send_retry") send_retry(true);
        else if (scenario == "log_send_retry") send_retry(false);
        else if (scenario == "health_zero") health_interval(0);
        else if (scenario == "health_periodic") health_interval(10);
        else if (scenario == "stale_dns_epoch") stale_dns_epoch();
        else if (scenario == "bounded_drain") bounded_drain();
        else if (scenario == "start_validation") start_validation();
        else if (scenario == "start_retry") start_retry();
        else throw std::runtime_error("unknown scenario: " + scenario);
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
