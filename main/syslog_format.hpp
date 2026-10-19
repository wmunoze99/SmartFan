#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>

// Allocation-free helpers; no ESP-IDF dependencies. All lengths are bytes.
namespace syslog_format {
constexpr size_t kPacketCapacity = 768; // Includes the terminating NUL (not sent).
enum class Level : uint8_t { error, warning, info, debug, verbose };

struct LogLine {
    Level level = Level::info;
    std::string_view tag;
};

// Supports in-place sanitizing. Drops ANSI CSI/OSC sequences, replaces controls
// (including line breaks) with spaces, and preserves non-ASCII UTF-8 bytes.
// Input and output may be byte-truncated; UTF-8 validity is not guaranteed.
inline size_t sanitize(char *output, size_t capacity, std::string_view input)
{
    if (output == nullptr || capacity == 0) {
        return 0;
    }
    size_t written = 0;
    for (size_t i = 0; i < input.size() && written + 1 < capacity;) {
        unsigned char c = static_cast<unsigned char>(input[i++]);
        if (c == 0x1b) {
            if (i == input.size()) {
                break;
            }
            const char kind = input[i++];
            if (kind == '[') {
                while (i < input.size()) {
                    const unsigned char part = static_cast<unsigned char>(input[i++]);
                    if (part >= 0x40 && part <= 0x7e) {
                        break;
                    }
                }
            } else if (kind == ']') {
                while (i < input.size()) {
                    if (input[i++] == '\a') {
                        break;
                    }
                    if (input[i - 1] == '\x1b' && i < input.size() && input[i] == '\\') {
                        ++i;
                        break;
                    }
                }
            }
            continue;
        }
        output[written++] = c < 0x20 || c == 0x7f ? ' ' : static_cast<char>(c);
    }
    output[written] = '\0';
    return written;
}

inline bool header_character(unsigned char c)
{
    return c >= 33 && c <= 126;
}

// Accepts sanitized ESP LOG V1 text: "I (123) tag: message", including system
// time timestamps. Unstructured printf/Matter output is deliberately rejected.
// The returned tag is a view into input; keep input alive while using it.
inline bool parse_log_v1(std::string_view input, LogLine &result)
{
    if (input.size() < 8 || input[1] != ' ' || input[2] != '(') {
        return false;
    }
    Level level;
    switch (input[0]) {
    case 'E': level = Level::error; break;
    case 'W': level = Level::warning; break;
    case 'I': level = Level::info; break;
    case 'D': level = Level::debug; break;
    case 'V': level = Level::verbose; break;
    default: return false;
    }
    size_t end = 3;
    for (; end < input.size() && input[end] != ')'; ++end) {
        const char c = input[end];
        if (end >= 67 || !((c >= '0' && c <= '9') || c == ':' || c == '.' ||
                           c == '-' || c == '/' || c == ' ')) {
            return false;
        }
    }
    if (end == 3 || end + 2 >= input.size() || input[end + 1] != ' ') {
        return false;
    }
    const size_t start = end + 2;
    end = start;
    while (end < input.size() && input[end] != ':') {
        if (!header_character(static_cast<unsigned char>(input[end]))) {
            return false;
        }
        ++end;
    }
    if (end == start || end + 1 >= input.size() || input[end + 1] != ' ') {
        return false;
    }
    result = {level, input.substr(start, end - start)};
    return true;
}

inline bool should_forward(const LogLine &line)
{
    if (line.level == Level::warning || line.level == Level::error) {
        return true;
    }
    if (line.level != Level::info) {
        return false;
    }
    return line.tag == "smart_fan" || line.tag == "fan_controller" || line.tag == "fan_ota" ||
           line.tag == "status_display" || line.tag == "remote_logger";
}

inline unsigned priority(Level level)
{
    switch (level) {
    case Level::error: return 128 + 3;
    case Level::warning: return 128 + 4;
    case Level::info: return 128 + 6;
    default: return 128 + 7;
    }
}

// RFC5424, local0, no wall-clock timestamp. Header tokens become printable ASCII
// without spaces, with RFC length limits; empty tokens become NILVALUE ("-").
// Returns wire length, or zero if the complete header cannot fit. The body is
// sanitized and truncated; output is NUL-terminated and never exceeds 768 bytes.
inline size_t format_packet(char *output, size_t capacity, Level level,
                            std::string_view hostname, std::string_view app_tag,
                            std::string_view msgid, std::string_view message)
{
    if (output == nullptr || capacity == 0) {
        return 0;
    }
    if (capacity > kPacketCapacity) {
        capacity = kPacketCapacity;
    }
    size_t used = 0;
    bool fits = true;
    const auto append = [&](char c) {
        if (used + 1 < capacity) {
            output[used++] = c;
        } else {
            fits = false;
        }
    };
    const auto literal = [&](std::string_view text) {
        for (char c : text) {
            append(c);
        }
    };
    const auto token = [&](std::string_view text, size_t limit) {
        if (text.empty()) {
            append('-');
        }
        for (size_t i = 0; i < text.size() && i < limit; ++i) {
            const unsigned char c = static_cast<unsigned char>(text[i]);
            append(header_character(c) ? static_cast<char>(c) : '_');
        }
    };
    const unsigned pri = priority(level);
    append('<');
    append(static_cast<char>('0' + pri / 100));
    append(static_cast<char>('0' + (pri / 10) % 10));
    append(static_cast<char>('0' + pri % 10));
    literal(">1 - ");
    token(hostname, 255);
    append(' ');
    token(app_tag, 48);
    literal(" - ");
    token(msgid, 32);
    literal(" - ");
    if (!fits) {
        output[0] = '\0';
        return 0;
    }
    return used + sanitize(output + used, capacity - used, message);
}
} // namespace syslog_format
