#include "sdk.hpp"
#include "status_display.hpp"

#include <iostream>
#include <optional>

namespace {
using Connection = StatusDisplay::Connection;
using Controller = StatusDisplay::Controller;
using fake::require;

struct Fixture {
    uint64_t start;
    Controller controller;
    StatusDisplay display;

    static StatusDisplay::Config config(Controller controller, std::optional<uint32_t> timeout)
    {
        StatusDisplay::Config result{1, 2, 0x3c, controller};
        if (timeout) result.sleep_timeout_seconds = *timeout;
        return result;
    }

    explicit Fixture(Controller controller, std::optional<uint32_t> timeout = {}, uint64_t start = 0)
        : start(start), controller(controller), display(config(controller, timeout))
    {
        fake::runtime = {};
        fake::runtime.now = start;
        require(display.init() == ESP_OK && display.ready(), "real display initialization failed");
        require(fake::runtime.task != nullptr, "init did not create the display task");
    }

    void at(uint64_t milliseconds, std::function<void()> action)
    {
        fake::runtime.events.push_back({start + milliseconds, std::move(action)});
    }

    void run(bool expect_warning = false)
    {
        try {
            fake::runtime.task(fake::runtime.context);
            throw std::runtime_error("display task returned unexpectedly");
        } catch (const fake::Stop &) {
            require(fake::runtime.next_event == fake::runtime.events.size(), "task missed scheduled events");
        }
        if (!expect_warning) require(fake::runtime.warnings.empty(), "unexpected SDK warning/error");
    }

    size_t frames() const
    {
        return std::count_if(fake::runtime.operations.begin(), fake::runtime.operations.end(),
                             [](const auto &op) { return op.kind == "frame"; });
    }

    fake::Frame last_frame() const
    {
        for (auto it = fake::runtime.operations.rbegin(); it != fake::runtime.operations.rend(); ++it) {
            if (it->kind == "frame") return it->frame;
        }
        throw std::runtime_error("no framebuffer was flushed");
    }

    void power(std::initializer_list<std::pair<bool, uint64_t>> expected) const
    {
        std::vector<std::pair<std::string, uint64_t>> actual, wanted;
        for (const auto &op : fake::runtime.operations) {
            if (op.kind != "frame") actual.emplace_back(op.kind, op.at - start);
        }
        const std::string api = controller == Controller::ssd1306 ? "ssd" : "sh";
        for (const auto &[on, at] : expected) wanted.emplace_back(api + (on ? "_on" : "_off"), at);
        if (actual != wanted) {
            std::string message = "power API/timing mismatch; actual:";
            for (const auto &[kind, at] : actual) message += " " + kind + "@" + std::to_string(at);
            message += "; expected:";
            for (const auto &[kind, at] : wanted) message += " " + kind + "@" + std::to_string(at);
            throw std::runtime_error(message);
        }
    }

    void wakes_redrawn() const
    {
        bool slept = false;
        const auto &ops = fake::runtime.operations;
        for (size_t i = 0; i < ops.size(); ++i) {
            if (ops[i].kind == "ssd_off" || ops[i].kind == "sh_off") slept = true;
            if (slept && (ops[i].kind == "ssd_on" || ops[i].kind == "sh_on")) {
                require(i > 0 && ops[i - 1].kind == "frame" && ops[i - 1].at == ops[i].at,
                        "wake must flush the entire fresh frame BEFORE powering on");
            }
        }
    }

    void blocked(TickType_t ticks) const
    {
        require(!fake::runtime.waits.empty() && fake::runtime.waits.back().ticks == ticks,
                "unexpected task queue wait: expected " + std::to_string(ticks));
    }
};

void idle(Controller controller)
{
    Fixture f(controller); // Deliberately use production's default timeout.
    f.at(29999, [&] { f.power({{true, 0}}); require(f.frames() > 0, "no initial render"); });
    f.at(30001, [&] { f.power({{true, 0}, {false, 30000}}); f.blocked(portMAX_DELAY); });
    f.at(120000, [&] { f.power({{true, 0}, {false, 30000}}); });
    f.run();
    f.power({{true, 0}, {false, 30000}});
}

void fan_activity(Controller controller)
{
    Fixture f(controller);
    // Actual changes reset an already-running deadline (rotation alone counts).
    f.at(5000, [&] { f.display.set_fan_state(1, false); });
    f.at(10000, [&] { f.display.set_fan_state(1, true); });
    f.at(39999, [&] { f.power({{true, 0}}); });
    f.at(40001, [&] { f.power({{true, 0}, {false, 40000}}); });
    f.at(41000, [&] { f.display.set_fan_state(2, true); });
    f.at(41001, [&] { f.power({{true, 0}, {false, 40000}, {true, 41000}}); });
    // Repeated identical reports must not extend the wake's 30-second deadline.
    f.at(50000, [&] { f.display.set_fan_state(2, true); });
    f.at(70999, [&] { f.display.set_fan_state(2, true); });
    f.at(71001, [&] { f.power({{true, 0}, {false, 40000}, {true, 41000}, {false, 71000}}); });
    f.at(72000, [&] { f.display.set_fan_state(2, false); });
    f.run();
    f.power({{true, 0}, {false, 40000}, {true, 41000}, {false, 71000},
             {true, 72000}, {false, 102000}});
    f.wakes_redrawn();
}

void explicit_wake(Controller controller)
{
    Fixture f(controller);
    fake::Frame initial;
    f.at(1, [&] { initial = f.last_frame(); });
    f.at(20000, [&] { f.display.wake(); }); // Wake while already on resets the deadline.
    f.at(49999, [&] { f.power({{true, 0}}); });
    f.at(50001, [&] { f.power({{true, 0}, {false, 50000}}); });
    f.at(51000, [&] { f.display.wake(); }); // No connection, speed or rotation change.
    f.at(51001, [&] {
        f.power({{true, 0}, {false, 50000}, {true, 51000}});
        require(f.last_frame() == initial, "unchanged-state wake rendered different state");
    });
    f.run();
    f.power({{true, 0}, {false, 50000}, {true, 51000}, {false, 81000}});
    f.wakes_redrawn();
}

void status_and_cache(Controller controller)
{
    Fixture f(controller);
    fake::Frame offline;
    size_t sleeping_frames = 0, sleeping_operations = 0;
    f.at(10000, [&] { f.display.set_connection(Connection::offline); });
    f.at(10001, [&] { offline = f.last_frame(); });
    f.at(20000, [&] { f.display.set_connection(Connection::online); });
    f.at(29999, [&] { f.display.set_connection(Connection::connecting); });
    f.at(30001, [&] {
        f.power({{true, 0}, {false, 30000}}); // Status updates did not postpone sleep.
        sleeping_frames = f.frames();
        sleeping_operations = fake::runtime.operations.size();
        require(f.last_frame() != offline, "reference status frames must differ");
    });
    f.at(31000, [&] { f.display.set_connection(Connection::online); });
    f.at(32000, [&] { f.display.set_connection(Connection::commissioning_closed); });
    f.at(33000, [&] { f.display.set_connection(Connection::offline); });
    f.at(34000, [&] { f.display.set_fan_state(0, false); });
    f.at(40000, [&] {
        f.power({{true, 0}, {false, 30000}});
        require(f.frames() == sleeping_frames && fake::runtime.operations.size() == sleeping_operations,
                "sleeping status/unchanged fan reports must not render or power on");
        f.blocked(portMAX_DELAY);
        f.display.wake();
    });
    f.at(40001, [&] {
        require(f.frames() == sleeping_frames + 1, "wake did not flush exactly one fresh frame");
        require(f.last_frame() == offline, "wake did not render the latest cached sleeping status");
    });
    f.run();
    f.power({{true, 0}, {false, 30000}, {true, 40000}, {false, 70000}});
    f.wakes_redrawn();
}

void commissioning(Controller controller)
{
    Fixture f(controller);
    f.at(10000, [&] { f.display.set_commissioning_payload("MT:TEST-PAYLOAD"); });
    f.at(10001, [&] {
        require(fake::runtime.qr_payloads == std::vector<std::string>{"MT:TEST-PAYLOAD"},
                "real QR rendering path did not call the QR API");
        f.blocked(portMAX_DELAY);
    });
    f.at(120000, [&] { f.power({{true, 0}}); f.display.set_connection(Connection::commissioning_closed); });
    f.at(149999, [&] { f.power({{true, 0}}); });
    f.at(150001, [&] { f.power({{true, 0}, {false, 150000}}); });
    // Opening QR also wakes an already-sleeping panel, without fan activity.
    f.at(160000, [&] { f.display.set_commissioning_payload("MT:REOPENED"); });
    f.at(160001, [&] {
        f.power({{true, 0}, {false, 150000}, {true, 160000}});
        require(fake::runtime.qr_payloads.back() == "MT:REOPENED", "reopened QR payload not rendered");
        f.blocked(portMAX_DELAY);
    });
    f.at(300000, [&] { f.power({{true, 0}, {false, 150000}, {true, 160000}}); });
    f.at(310000, [&] { f.display.set_connection(Connection::online); });
    f.run();
    f.power({{true, 0}, {false, 150000}, {true, 160000}, {false, 340000}});
    f.wakes_redrawn();
}

void commissioning_without_qr(Controller controller)
{
    Fixture f(controller);
    f.at(10000, [&] { f.display.set_connection(Connection::commissioning); });
    f.at(20000, [&] { f.display.set_commissioning_payload(""); });
    f.at(30001, [&] { f.power({{true, 0}, {false, 30000}}); });
    f.run();
    require(fake::runtime.qr_payloads.empty(), "empty payload should not render a QR");
    f.power({{true, 0}, {false, 30000}});
}

void disabled(Controller controller)
{
    Fixture f(controller, 0);
    f.at(30000, [&] { f.blocked(portMAX_DELAY); f.display.set_connection(Connection::online); });
    f.at(60000, [&] { f.display.set_fan_state(3, true); });
    f.at(90000, [&] { f.display.wake(); });
    f.at(86400000, [&] { f.blocked(portMAX_DELAY); f.power({{true, 0}}); });
    f.run();
    f.power({{true, 0}});
}

void tick_wrap(Controller controller)
{
    Fixture f(controller, {}, static_cast<uint64_t>(UINT32_MAX) - 15000);
    f.at(10000, [&] { f.display.set_connection(Connection::online); });
    f.at(20000, [&] { f.display.set_fan_state(0, false); });
    f.at(29999, [&] { f.power({{true, 0}}); });
    f.at(30001, [&] { f.power({{true, 0}, {false, 30000}}); });
    f.at(31000, [&] { f.display.wake(); });
    f.run();
    f.power({{true, 0}, {false, 30000}, {true, 31000}, {false, 61000}});
    f.wakes_redrawn();
}

void sleep_failure(Controller controller)
{
    Fixture f(controller);
    fake::runtime.fail_off = 1;
    f.at(30001, [&] {
        f.power({{true, 0}, {false, 30000}});
        f.blocked(30000); // Failed sleep backs off a full timeout rather than polling at zero.
        require(fake::runtime.warnings == std::vector<std::string>{"OLED sleep failed: ESP_FAIL"},
                "sleep failure must be logged with the error name");
    });
    f.at(59999, [&] { f.power({{true, 0}, {false, 30000}}); });
    f.at(60001, [&] { f.power({{true, 0}, {false, 30000}, {false, 60000}}); f.blocked(portMAX_DELAY); });
    f.run(true);
    f.power({{true, 0}, {false, 30000}, {false, 60000}});
    require(fake::runtime.waits.size() < 10, "sleep failure caused excessive queue polling");
}

void wake_failure(Controller controller)
{
    Fixture f(controller);
    fake::runtime.fail_on = 1; // Initialization already succeeded; fail the next power-on only.
    size_t sleeping_frames = 0;
    f.at(31000, [&] { f.display.wake(); });
    f.at(31001, [&] {
        f.power({{true, 0}, {false, 30000}, {true, 31000}});
        f.blocked(portMAX_DELAY);
        require(fake::runtime.warnings == std::vector<std::string>{"OLED wake failed: ESP_FAIL"},
                "wake failure must be logged with the error name");
        sleeping_frames = f.frames();
    });
    f.at(40000, [&] { f.display.set_connection(Connection::offline); });
    f.at(60000, [&] {
        require(f.frames() == sleeping_frames, "failed wake must leave the panel logically asleep");
        f.power({{true, 0}, {false, 30000}, {true, 31000}});
    });
    f.at(70000, [&] { f.display.wake(); });
    f.run(true);
    f.power({{true, 0}, {false, 30000}, {true, 31000}, {true, 70000}, {false, 100000}});
    f.wakes_redrawn();
    require(fake::runtime.waits.size() < 15, "wake failure caused excessive queue polling");
}

bool pixel(const fake::Frame &frame, int x, int y)
{
    require(x >= 0 && x < 128 && y >= 0 && y < 64, "pixel assertion outside framebuffer");
    return (frame[(y / 8) * 128 + x] & (1U << (y % 8))) != 0;
}

std::vector<bool> region(const fake::Frame &frame, int x, int y, int width, int height)
{
    std::vector<bool> result;
    for (int row = y; row < y + height; ++row) {
        for (int column = x; column < x + width; ++column) result.push_back(pixel(frame, column, row));
    }
    return result;
}

// Independent glyph expectations, used only for assertions, never for exported frames.
const std::array<uint8_t, 5> &expected_glyph(char character)
{
    static const std::pair<char, std::array<uint8_t, 5>> glyphs[] = {
        {' ', {0x00, 0x00, 0x00, 0x00, 0x00}}, {'?', {0x02, 0x01, 0x51, 0x09, 0x06}},
        {'_', {0x40, 0x40, 0x40, 0x40, 0x40}}, {'.', {0x00, 0x60, 0x60, 0x00, 0x00}},
        {'-', {0x08, 0x08, 0x08, 0x08, 0x08}},
        {'0', {0x3e, 0x51, 0x49, 0x45, 0x3e}}, {'1', {0x00, 0x42, 0x7f, 0x40, 0x00}},
        {'2', {0x42, 0x61, 0x51, 0x49, 0x46}}, {'3', {0x21, 0x41, 0x45, 0x4b, 0x31}},
        {'4', {0x18, 0x14, 0x12, 0x7f, 0x10}}, {'5', {0x27, 0x45, 0x45, 0x45, 0x39}},
        {'6', {0x3c, 0x4a, 0x49, 0x49, 0x30}}, {'7', {0x01, 0x71, 0x09, 0x05, 0x03}},
        {'8', {0x36, 0x49, 0x49, 0x49, 0x36}}, {'9', {0x06, 0x49, 0x49, 0x29, 0x1e}},
        {'A', {0x7e, 0x11, 0x11, 0x11, 0x7e}}, {'C', {0x3e, 0x41, 0x41, 0x41, 0x22}},
        {'D', {0x7f, 0x41, 0x41, 0x22, 0x1c}}, {'E', {0x7f, 0x49, 0x49, 0x49, 0x41}},
        {'F', {0x7f, 0x09, 0x09, 0x09, 0x01}}, {'G', {0x3e, 0x41, 0x49, 0x49, 0x7a}},
        {'H', {0x7f, 0x08, 0x08, 0x08, 0x7f}}, {'I', {0x00, 0x41, 0x7f, 0x41, 0x00}},
        {'L', {0x7f, 0x40, 0x40, 0x40, 0x40}}, {'M', {0x7f, 0x02, 0x0c, 0x02, 0x7f}},
        {'N', {0x7f, 0x04, 0x08, 0x10, 0x7f}},
        {'O', {0x3e, 0x41, 0x41, 0x41, 0x3e}}, {'P', {0x7f, 0x09, 0x09, 0x09, 0x06}},
        {'R', {0x7f, 0x09, 0x19, 0x29, 0x46}}, {'S', {0x46, 0x49, 0x49, 0x49, 0x31}},
        {'T', {0x01, 0x01, 0x7f, 0x01, 0x01}}, {'W', {0x3f, 0x40, 0x38, 0x40, 0x3f}},
        {'Z', {0x61, 0x51, 0x49, 0x45, 0x43}},
        {'a', {0x20, 0x54, 0x54, 0x54, 0x78}}, {'c', {0x38, 0x44, 0x44, 0x44, 0x20}},
        {'d', {0x38, 0x44, 0x44, 0x48, 0x7f}}, {'e', {0x38, 0x54, 0x54, 0x54, 0x18}},
        {'f', {0x08, 0x7e, 0x09, 0x01, 0x02}}, {'h', {0x7f, 0x08, 0x04, 0x04, 0x78}},
        {'i', {0x00, 0x44, 0x7d, 0x40, 0x00}}, {'l', {0x00, 0x41, 0x7f, 0x40, 0x00}},
        {'m', {0x7c, 0x04, 0x18, 0x04, 0x78}}, {'n', {0x7c, 0x08, 0x04, 0x04, 0x78}},
        {'o', {0x38, 0x44, 0x44, 0x44, 0x38}}, {'r', {0x7c, 0x08, 0x04, 0x04, 0x08}},
        {'s', {0x48, 0x54, 0x54, 0x54, 0x20}}, {'w', {0x3c, 0x40, 0x30, 0x40, 0x3c}},
    };
    for (const auto &glyph : glyphs) {
        if (glyph.first == character) return glyph.second;
    }
    throw std::runtime_error(std::string("missing test glyph: ") + character);
}

void expect_text(const fake::Frame &frame, int x, int y, const std::string &text, int scale = 1, int width = 0)
{
    const int text_width = static_cast<int>(text.size()) * 6 * scale - scale;
    if (width == 0) width = text_width;
    require(width >= text_width, "expected text exceeds its reserved width");
    for (int row = 0; row < 7 * scale; ++row) {
        for (int column = 0; column < width; ++column) {
            const size_t index = column / (6 * scale);
            const int glyph_column = (column / scale) % 6;
            const bool expected = index < text.size() && glyph_column < 5 &&
                (expected_glyph(text[index])[glyph_column] & (1U << (row / scale))) != 0;
            require(pixel(frame, x + column, y + row) == expected,
                    "text mismatch for '" + text + "' at " + std::to_string(x + column) + "," +
                    std::to_string(y + row));
        }
    }
}

void expect_footer(const fake::Frame &frame, const std::string &text)
{
    expect_text(frame, 18, 57, text, 1, 110); // Also reject trailing byte garbage and stale glyphs.
}

fake::Frame capture_dashboard(Controller controller, uint8_t speed = 2, bool rotation = true,
                              Connection connection = Connection::online, const char *ssid = "Home_WiFi")
{
    Fixture f(controller, 0);
    f.display.set_connection(connection, ssid);
    f.display.set_fan_state(speed, rotation);
    f.run();
    return f.last_frame();
}

void expect_dashboard(const fake::Frame &frame, uint8_t speed, bool rotation)
{
    for (int x = 0; x < 128; ++x) {
        require(pixel(frame, x, 16) && pixel(frame, x, 55), "missing horizontal separator");
    }
    for (int y = 19; y <= 53; ++y) require(pixel(frame, 63, y), "missing center separator");
    expect_text(frame, 23, 1, "FAN", 2);
    if (speed == 0) expect_text(frame, 15, 26, "OFF", 2);
    else expect_text(frame, 22, 19, std::to_string(speed), 4);
    expect_text(frame, 17, 48, "SPEED");
    expect_text(frame, rotation ? 85 : 79, 33, rotation ? "ON" : "OFF", 2);
    expect_text(frame, 73, 48, "ROTATION");
    expect_footer(frame, "Home_WiFi");
    require(region(frame, 3, 1, 13, 13) != std::vector<bool>(13 * 13), "missing fan icon");
    require(region(frame, 84, 19, 24, 12) != std::vector<bool>(24 * 12), "missing rotation icon");

    for (int y = 0; y < 64; ++y) {
        for (int x = 0; x < 128; ++x) {
            const auto inside = [=](int left, int top, int width, int height) {
                return x >= left && x < left + width && y >= top && y < top + height;
            };
            const bool reserved = y == 16 || y == 55 || inside(63, 19, 1, 35) ||
                inside(3, 1, 13, 13) || inside(23, 1, 34, 14) ||
                (speed == 0 ? inside(15, 26, 34, 14) : inside(22, 19, 20, 28)) ||
                inside(17, 48, 29, 7) || inside(84, 19, 24, 12) ||
                inside(rotation ? 85 : 79, 33, rotation ? 22 : 34, 14) ||
                inside(73, 48, 47, 7) || inside(2, 57, 12, 7) || inside(18, 57, 110, 7);
            require(reserved || !pixel(frame, x, y),
                    "dashboard overlaps a gutter at " + std::to_string(x) + "," + std::to_string(y));
        }
    }
}

void write_frame(const fake::Frame &frame)
{
    // Exactly 1024 page-major bytes: pixel(x,y) is byte[(y/8)*128+x], bit y%8.
    std::cout.write(reinterpret_cast<const char *>(frame.data()), frame.size());
    require(std::cout.good(), "raw framebuffer export failed");
}

void dashboard_frames(Controller controller)
{
    for (uint8_t speed = 0; speed <= 3; ++speed) {
        for (bool rotation : {false, true}) {
            const auto frame = capture_dashboard(controller, speed, rotation);
            expect_dashboard(frame, speed, rotation);
            write_frame(frame);
        }
    }
}

void ssid_ascii(Controller controller)
{
    const auto mixed = capture_dashboard(controller);
    expect_footer(mixed, "Home_WiFi");
    const auto upper = capture_dashboard(controller, 2, true, Connection::online, "HOME_WIFI");
    expect_footer(upper, "HOME_WIFI");
    require(region(mixed, 18, 57, 110, 7) != region(upper, 18, 57, 110, 7), "SSID was uppercased");
    expect_footer(capture_dashboard(controller, 2, true, Connection::online, "Home_WiFi.5G-1"), "Home_WiFi.5G-1");
}

void ssid_bounds(Controller controller)
{
    for (const auto *ssid : {"12345678901234567", "123456789012345678"}) {
        expect_footer(capture_dashboard(controller, 2, true, Connection::online, ssid), ssid);
    }
    for (const auto *ssid : {"1234567890123456789", "12345678901234567890123456789012"}) {
        expect_footer(capture_dashboard(controller, 2, true, Connection::online, ssid), "123456789012345...");
    }

    // 32 bytes but only nine glyphs: the final ASCII byte must survive the bounded copy.
    constexpr char full[] = u8"😀😀😀😀😀😀😀猫Z";
    static_assert(sizeof(full) - 1 == 32, "bounded SSID fixture must be exactly 32 bytes");
    std::array<char, 32> raw;
    std::copy_n(full, raw.size(), raw.begin()); // Deliberately no terminating NUL.
    Fixture f(controller, 0);
    f.display.set_connection(Connection::online, raw.data());
    raw.fill('X'); // The queued snapshot must own the name, not retain the caller's buffer.
    f.run();
    expect_footer(f.last_frame(), "????????Z");
    const std::string overlong = std::string(full) + "SHOULDNOTAPPEAR";
    expect_footer(capture_dashboard(controller, 2, true, Connection::online, overlong.c_str()), "????????Z");
}

void ssid_utf8(Controller controller)
{
    const std::pair<const char *, const char *> cases[] = {
        {u8"café_猫😀", "caf?_??"},
        {u8"😀😀😀😀😀😀😀😀", "????????"},
        {u8"12345678901234567é", "12345678901234567?"},
        {u8"123456789012345678é", "123456789012345..."},
        {u8"😀😀😀😀😀😀😀\xf0\x9f\x98", "????????"},
        {"a\xe2" "c", "a?c"},
    };
    for (const auto &entry : cases) {
        expect_footer(capture_dashboard(controller, 2, true, Connection::online, entry.first), entry.second);
    }
}

void connection_footers(Controller controller)
{
    const auto online = capture_dashboard(controller);
    const auto online_icon = region(online, 2, 57, 12, 7);
    for (const char *hidden : {static_cast<const char *>(nullptr), ""}) {
        const auto frame = capture_dashboard(controller, 2, true, Connection::online, hidden);
        expect_footer(frame, "WIFI ONLINE");
        require(region(frame, 2, 57, 12, 7) == online_icon, "hidden SSID lost connected icon");
    }
    const std::pair<Connection, const char *> cases[] = {
        {Connection::offline, "WIFI OFFLINE"}, {Connection::connecting, "CONNECTING"},
        {Connection::starting, "STARTING"}, {Connection::commissioning, "PAIRING"},
        {Connection::commissioning_closed, "PAIRING CLOSED"},
    };
    for (const auto &entry : cases) {
        Fixture f(controller, 0);
        f.display.set_connection(Connection::online, "Home_WiFi");
        f.at(1, [&] { f.display.set_connection(entry.first, "IgnoredName"); });
        f.run();
        const auto frame = f.last_frame();
        expect_footer(frame, entry.second);
        require(region(frame, 2, 57, 12, 7) != online_icon, "non-online footer still shows connected Wi-Fi");
        for (int row = 0; row < 7; ++row) require(pixel(frame, 11 - row, 57 + row), "missing disconnected slash");
    }
}

void ssid_status_and_cache(Controller controller)
{
    Fixture f(controller);
    f.display.set_connection(Connection::online, "Home_WiFi");
    size_t awake_frames = 0, sleeping_frames = 0, sleeping_operations = 0;
    fake::Frame sleeping_frame;
    f.at(1, [&] { expect_footer(f.last_frame(), "Home_WiFi"); });
    f.at(10000, [&] { f.display.set_connection(Connection::online, "Second_WiFi"); });
    f.at(10001, [&] {
        expect_footer(f.last_frame(), "Second_WiFi");
        awake_frames = f.frames();
    });
    f.at(20000, [&] { f.display.set_connection(Connection::online, "Second_WiFi"); });
    f.at(20001, [&] { require(f.frames() == awake_frames, "unchanged SSID caused a redraw"); });
    f.at(29999, [&] { f.display.set_connection(Connection::online, "Third_WiFi"); });
    f.at(30001, [&] {
        f.power({{true, 0}, {false, 30000}});
        sleeping_frames = f.frames();
        sleeping_operations = fake::runtime.operations.size();
        sleeping_frame = f.last_frame();
        expect_footer(sleeping_frame, "Third_WiFi");
    });
    f.at(31000, [&] { f.display.set_connection(Connection::online, "Old_WiFi"); });
    f.at(32000, [&] { f.display.set_connection(Connection::online, "New_WiFi"); });
    f.at(33000, [&] { f.display.set_connection(Connection::online, "New_WiFi"); });
    f.at(40000, [&] {
        require(f.frames() == sleeping_frames && fake::runtime.operations.size() == sleeping_operations,
                "SSID-only status woke or wrote to a sleeping display");
        f.power({{true, 0}, {false, 30000}});
        f.blocked(portMAX_DELAY);
        f.display.wake(); // The same explicit wake API used by the physical button.
    });
    f.at(40001, [&] {
        require(f.frames() == sleeping_frames + 1, "button wake must flush exactly one frame");
        expect_footer(f.last_frame(), "New_WiFi");
        require(region(f.last_frame(), 0, 0, 128, 56) == region(sleeping_frame, 0, 0, 128, 56),
                "SSID-only update changed the dashboard body");
    });
    f.at(50000, [&] { f.display.set_connection(Connection::online, "Final_WiFi"); });
    f.at(50001, [&] { expect_footer(f.last_frame(), "Final_WiFi"); });
    f.at(70001, [&] { f.power({{true, 0}, {false, 30000}, {true, 40000}, {false, 70000}}); });
    f.run();
    f.power({{true, 0}, {false, 30000}, {true, 40000}, {false, 70000}});
    f.wakes_redrawn();
}
} // namespace

int main(int argc, char **argv)
{
    try {
        require(argc == 3, "usage: status_display_test CONTROLLER SCENARIO");
        const std::string variant = argv[1], scenario = argv[2];
        require(variant == "ssd1306" || variant == "sh1106", "unknown controller");
        const auto controller = variant == "ssd1306" ? Controller::ssd1306 : Controller::sh1106;
        if (scenario == "idle") idle(controller);
        else if (scenario == "fan_activity") fan_activity(controller);
        else if (scenario == "explicit_wake") explicit_wake(controller);
        else if (scenario == "status_and_cache") status_and_cache(controller);
        else if (scenario == "commissioning") commissioning(controller);
        else if (scenario == "commissioning_without_qr") commissioning_without_qr(controller);
        else if (scenario == "disabled") disabled(controller);
        else if (scenario == "tick_wrap") tick_wrap(controller);
        else if (scenario == "sleep_failure") sleep_failure(controller);
        else if (scenario == "wake_failure") wake_failure(controller);
        else if (scenario == "dashboard_frames") dashboard_frames(controller);
        else if (scenario == "ssid_ascii") ssid_ascii(controller);
        else if (scenario == "ssid_bounds") ssid_bounds(controller);
        else if (scenario == "ssid_utf8") ssid_utf8(controller);
        else if (scenario == "connection_footers") connection_footers(controller);
        else if (scenario == "ssid_status_and_cache") ssid_status_and_cache(controller);
        else if (scenario == "export_dashboard") write_frame(capture_dashboard(controller));
        else if (scenario == "export_offline") write_frame(capture_dashboard(controller, 0, false, Connection::offline));
        else throw std::runtime_error("unknown scenario: " + scenario);
        return 0;
    } catch (const std::exception &error) {
        std::cerr << "status_display harness: " << error.what() << '\n';
        return 1;
    }
}
