#pragma once

#include <string>

#include <sdkconfig.h>

namespace firmware {
#if CONFIG_FAN_DISPLAY_CONTROLLER_SH1106
inline constexpr char kDisplayVariant[] = "sh1106";
#else
inline constexpr char kDisplayVariant[] = "ssd1306";
#endif

inline std::string ota_url(const char *configured_url)
{
    std::string url = configured_url == nullptr ? "" : configured_url;
    const std::string legacy_asset = "/smart_fan.bin";
    if (url.size() >= legacy_asset.size() &&
        url.compare(url.size() - legacy_asset.size(), legacy_asset.size(), legacy_asset) == 0) {
        url.replace(url.size() - legacy_asset.size(), legacy_asset.size(), "/smart_fan-{display}.bin");
    }

    const std::string placeholder = "{display}";
    size_t position = 0;
    while ((position = url.find(placeholder, position)) != std::string::npos) {
        url.replace(position, placeholder.size(), kDisplayVariant);
        position += sizeof(kDisplayVariant) - 1;
    }
    return url;
}
} // namespace firmware
