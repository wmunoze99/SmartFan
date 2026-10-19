#include "syslog_format.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
using namespace syslog_format;

void require(bool condition, const std::string &message)
{
    if (!condition) throw std::runtime_error(message);
}

std::string clean(std::string_view input)
{
    std::vector<char> output(input.size() + 1, '!');
    const size_t length = sanitize(output.data(), output.size(), input);
    require(output[length] == '\0', "sanitize did not terminate output");
    return {output.data(), length};
}

std::string packet(Level level, std::string_view host, std::string_view tag,
                   std::string_view id, std::string_view body)
{
    std::array<char, kPacketCapacity> output{};
    const size_t length = format_packet(output.data(), output.size(), level, host, tag, id, body);
    require(length > 0 && output[length] == '\0', "packet missing header or terminator");
    return {output.data(), length};
}

void parsing()
{
    const std::array<std::pair<char, Level>, 5> levels{{
        {'E', Level::error}, {'W', Level::warning}, {'I', Level::info},
        {'D', Level::debug}, {'V', Level::verbose}}};
    for (const auto &[letter, level] : levels) {
        for (const std::string &timestamp : {std::string("0"), std::string("4294967295"),
                std::string("12:34:56.789"), std::string("2026-10-16 12:34:56.789"),
                std::string("2026/10/16 12:34:56"), std::string(64, '9')}) {
            const std::string input = std::string(1, letter) + " (" + timestamp + ") smart_fan: body";
            LogLine line;
            require(parse_log_v1(input, line), "valid ESP LOG V1 timestamp rejected: " + input);
            require(line.level == level && line.tag == "smart_fan", "wrong parsed level/tag");
            require(line.tag.data() == input.data() + input.find("smart_fan"), "tag must view input");
        }
    }
    LogLine line;
    require(parse_log_v1("I (1) x: ", line) && line.tag == "x", "empty message rejected");
    // This parser recognizes timestamp characters, not calendar/time semantics.
    require(parse_log_v1("I (:.-/ ) x: body", line), "timestamp lexical contract changed");
    require(parse_log_v1("W (1) tag[]!~: body: more", line), "printable tag rejected");
}

void malformed()
{
    std::vector<std::string> inputs{
        "", "I", "I (", "I (1)", "I (1) x:", "I (1) x:body", "I (1) : body",
        "I (1)  x: body", "I (1)x: body", "I(1) x: body", "I  (1) x: body",
        "i (1) x: body", "X (1) x: body", "I () x: body", "I (abc) x: body",
        "I (+1) x: body", "I (1,2) x: body", "I (1\t2) x: body",
        "I (2026-10-16T12:00:00Z) x: body", "I (123 x: body", "I (1) x y: body",
        "I (1) x\t: body", "I (1) x\x7f: body", "I (1) caf\xc3\xa9: body",
        "printf output", "[123][CHIP:DL] Matter output", "<134>1 - host app - log - body",
        "\x1b[32mI (1) x: body", "I (" + std::string(65, '9') + ") x: body",
        std::string("I (1) x\0: body", 15)};
    for (const auto &input : inputs) {
        LogLine line{Level::warning, "unchanged"};
        require(!parse_log_v1(input, line), "malformed/unstructured input accepted: " + input);
        require(line.level == Level::warning && line.tag == "unchanged", "failure mutated result");
    }
}

void filtering()
{
    for (std::string_view tag : {"smart_fan", "fan_controller", "fan_ota", "status_display", "remote_logger"}) {
        require(should_forward({Level::info, tag}), "application INFO filtered");
    }
    for (std::string_view tag : {"", "wifi", "esp_netif", "chip", "Smart_fan", "smart_fan_extra",
                                "xsmart_fan", "smart_fan ", "fan_controller_extra"}) {
        require(!should_forward({Level::info, tag}), "non-allowlisted INFO forwarded");
    }
    for (std::string_view tag : {"smart_fan", "wifi", "arbitrary", ""}) {
        require(should_forward({Level::warning, tag}), "WARN filtered");
        require(should_forward({Level::error, tag}), "ERROR filtered");
        require(!should_forward({Level::debug, tag}), "DEBUG forwarded");
        require(!should_forward({Level::verbose, tag}), "VERBOSE forwarded");
    }
}

void headers()
{
    for (const auto &[level, pri] : std::array<std::pair<Level, unsigned>, 5>{{
            {Level::error, 131}, {Level::warning, 132}, {Level::info, 134},
            {Level::debug, 135}, {Level::verbose, 135}}}) {
        require(priority(level) == pri, "incorrect local0 severity");
        require(packet(level, "smart-fan-abcdef", "smart_fan", "log", "hello") ==
                    "<" + std::to_string(pri) + ">1 - smart-fan-abcdef smart_fan - log - hello",
                "RFC5424 header/wire body mismatch");
    }
    require(packet(Level::info, "", "", "", "") == "<134>1 - - - - - - ", "NILVALUE mismatch");
    require(packet(Level::info, std::string(256, 'h'), std::string(49, 'a'), std::string(33, 'm'), "body") ==
                "<134>1 - " + std::string(255, 'h') + " " + std::string(48, 'a') +
                " - " + std::string(32, 'm') + " - body", "RFC token byte limits changed");
}

void header_injection()
{
    const std::string input = std::string("a b\r\n\t\x1b\x7f") + "\xc3\xa9" + "<134>";
    require(packet(Level::info, input, input, input, "body") ==
                "<134>1 - a_b_______<134> a_b_______<134> - a_b_______<134> - body",
            "header controls/non-ASCII not replaced one byte at a time");
    std::string bytes, expected;
    for (unsigned c = 0; c < 256; ++c) {
        bytes += static_cast<char>(c);
        expected += c >= 33 && c <= 126 ? static_cast<char>(c) : '_';
        require(header_character(static_cast<unsigned char>(c)) == (c >= 33 && c <= 126),
                "header character range changed");
    }
    require(packet(Level::info, bytes, bytes, bytes, "") ==
                "<134>1 - " + expected.substr(0, 255) + " " + expected.substr(0, 48) +
                " - " + expected.substr(0, 32) + " - ", "binary header sanitization mismatch");
}

void ansi()
{
    const std::vector<std::pair<std::string, std::string>> cases{
        {"\x1b[31mred\x1b[0m", "red"}, {"a\x1b[2K\x1b[1;20Hb", "ab"},
        {"a\x1b]0;window title\a" "b", "ab"}, {"a\x1b]8;;https://host\x1b\\link\x1b]8;;\x1b\\b", "alinkb"},
        {"a\x1b", "a"}, {"a\x1b[", "a"}, {"a\x1b[31", "a"},
        {"a\x1b]unterminated", "a"}, {"a\x1b]title\x1b", "a"},
        {"a\x1bZb", "ab"}, {"\x1b[32mI (123) smart_fan: ok\x1b[0m\r\n", "I (123) smart_fan: ok  "}};
    for (const auto &[input, expected] : cases) {
        require(clean(input) == expected, "CSI/OSC sanitization mismatch");
        std::string inplace = input;
        const size_t length = sanitize(inplace.data(), inplace.size() + 1, {inplace.data(), inplace.size()});
        require(std::string(inplace.data(), length) == expected, "in-place CSI/OSC mismatch");
    }
    const std::string sanitized = clean(cases.back().first);
    LogLine line;
    require(parse_log_v1(sanitized, line) && should_forward(line), "colored V1 pipeline rejected");
}

void controls_multiline()
{
    std::string controls;
    for (int c = 0; c < 32; ++c) if (c != 27) controls += static_cast<char>(c);
    controls += '\x7f';
    require(clean(controls) == std::string(32, ' '), "control bytes must become spaces");
    const std::string body = "W (123) wifi: first\r\n<131>1 - forged app - log - second\nE (2) tag: third\tend";
    const std::string expected = "W (123) wifi: first  <131>1 - forged app - log - second E (2) tag: third end";
    require(packet(Level::warning, "host", "wifi", "log", body) ==
                "<132>1 - host wifi - log - " + expected, "multiline input must remain one packet body");
    require(clean(std::string("a\0b", 3)) == "a b", "embedded NUL not sanitized");
}

void null_and_zero()
{
    char output = '!';
    require(sanitize(nullptr, 42, "body") == 0 && sanitize(nullptr, 0, "body") == 0,
            "sanitize null output");
    require(sanitize(&output, 0, "body") == 0 && output == '!', "sanitize wrote at capacity zero");
    require(sanitize(&output, 1, "body") == 0 && output == '\0', "sanitize one-byte buffer");
    require(format_packet(nullptr, 42, Level::info, "h", "a", "m", "body") == 0,
            "format null output");
    require(format_packet(nullptr, 0, Level::info, "h", "a", "m", "body") == 0,
            "format null zero output");
    output = '!';
    require(format_packet(&output, 0, Level::info, "h", "a", "m", "body") == 0 && output == '!',
            "format wrote at capacity zero");
    require(format_packet(&output, 1, Level::info, "h", "a", "m", "body") == 0 && output == '\0',
            "format one-byte buffer");
}

void sanitize_bounds()
{
    std::string input;
    for (unsigned c = 0; c < 256; ++c) input += static_cast<char>(c);
    input += "\x1b]title\a\x1b[0m\r\nend";
    const std::string expected = clean(input);
    for (size_t capacity = 0; capacity <= input.size() + 2; ++capacity) {
        for (bool inplace : {false, true}) {
            std::vector<char> memory(input.size() + 16, '!');
            char *output = memory.data() + 4;
            if (inplace) std::memcpy(output, input.data(), input.size());
            const auto before = memory;
            const size_t length = sanitize(output, capacity,
                inplace ? std::string_view(output, input.size()) : std::string_view(input));
            require(length == (capacity == 0 ? 0 : std::min(capacity - 1, expected.size())),
                    "sanitize byte truncation length");
            require(std::equal(memory.begin(), memory.begin() + 4, before.begin()), "sanitize underrun");
            require(std::equal(memory.begin() + 4 + capacity, memory.end(), before.begin() + 4 + capacity),
                    "sanitize overrun");
            if (capacity) {
                require(output[length] == '\0' && std::string(output, length) == expected.substr(0, length),
                        "sanitize prefix/terminator mismatch");
            }
        }
    }
}

void packet_bounds()
{
    const std::string header = "<134>1 - h a - m - ";
    const std::string body(2000, 'x');
    for (size_t capacity = 0; capacity <= 1024; ++capacity) {
        std::vector<char> memory(1032, '!');
        char *output = memory.data() + 4;
        const size_t length = format_packet(output, capacity, Level::info, "h", "a", "m", body);
        const size_t effective = std::min(capacity, kPacketCapacity);
        require(std::all_of(memory.begin(), memory.begin() + 4, [](char c) { return c == '!'; }),
                "packet underrun");
        require(std::all_of(memory.begin() + 4 + effective, memory.end(), [](char c) { return c == '!'; }),
                "packet overrun/capacity clamp failed");
        if (capacity == 0) {
            require(length == 0, "zero capacity length");
        } else if (effective <= header.size()) {
            require(length == 0 && output[0] == '\0', "incomplete header must return empty output");
        } else {
            require(length == effective - 1 && output[length] == '\0', "packet byte length/terminator");
            require(std::string(output, length) == header + body.substr(0, length - header.size()),
                    "packet byte truncation mismatch");
        }
    }
    const std::string longest_header = "<134>1 - " + std::string(255, 'h') + " " +
        std::string(48, 'a') + " - " + std::string(32, 'm') + " - ";
    std::array<char, kPacketCapacity> output{};
    for (size_t capacity : {longest_header.size(), longest_header.size() + 1}) {
        const size_t length = format_packet(output.data(), capacity, Level::info,
            std::string(255, 'h'), std::string(48, 'a'), std::string(32, 'm'), "body");
        require(length == (capacity == longest_header.size() ? 0 : longest_header.size()),
                "longest header exact fit boundary");
    }
}

void utf8_bytes()
{
    const std::string utf8 = "caf\xc3\xa9 \xe2\x82\xac \xf0\x9f\x8c\xac";
    require(clean(utf8) == utf8, "non-ASCII body bytes changed");
    require(packet(Level::info, "h", "a", "m", utf8) == "<134>1 - h a - m - " + utf8,
            "UTF-8 body changed");
    // The production contract is byte-bounded, not codepoint-safe UTF-8 truncation.
    char output[3];
    require(sanitize(output, sizeof(output), "\xe2\x82\xac") == 2 &&
                std::string(output, 2) == "\xe2\x82" && output[2] == '\0', "expected split UTF-8 bytes");
    const std::string header = "<134>1 - h a - m - ";
    std::vector<char> buffer(header.size() + 3);
    const size_t length = format_packet(buffer.data(), buffer.size(), Level::info,
                                        "h", "a", "m", "\xe2\x82\xac");
    require(std::string(buffer.data(), length) == header + "\xe2\x82", "packet UTF-8 byte truncation");
}
} // namespace

int main(int argc, char **argv)
{
    try {
        require(argc == 2, "expected scenario");
        const std::string scenario = argv[1];
        if (scenario == "parsing") parsing();
        else if (scenario == "malformed") malformed();
        else if (scenario == "filtering") filtering();
        else if (scenario == "headers") headers();
        else if (scenario == "header_injection") header_injection();
        else if (scenario == "ansi") ansi();
        else if (scenario == "controls_multiline") controls_multiline();
        else if (scenario == "null_and_zero") null_and_zero();
        else if (scenario == "sanitize_bounds") sanitize_bounds();
        else if (scenario == "packet_bounds") packet_bounds();
        else if (scenario == "utf8_bytes") utf8_bytes();
        else throw std::runtime_error("unknown scenario: " + scenario);
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
