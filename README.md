# Smart Fan

Matter-over-Wi-Fi firmware for a Seeed Studio XIAO ESP32-C6 controlling three mutually exclusive speed relays and one rotation relay.

## Matter model

- Fan speed: Matter Fan Control with 3-speed support.
- Rotation: Matter rocking/oscillation support.
- Commissioning: Bluetooth LE; Home Assistant provides the Wi-Fi credentials.
- Network: Matter over 2.4 GHz Wi-Fi; no Thread border router is required.

## Default wiring

| Function | XIAO pin | GPIO |
|---|---:|---:|
| Speed 1 | D0 | 0 |
| Speed 2 | D1 | 1 |
| Speed 3 | D2 | 2 |
| Rotation | D3 | 21 |
| OLED SDA | D5 | 23 |
| OLED SCL | D4 | 22 |
| Speed button | D7 | 17 |
| Rotation button | D9 | 20 |

Connect a 128×64 I²C SSD1306 or SH1106 display to `3V3` and `GND`. Its default address is `0x3C`; the controller, pins, and address are configurable under `Smart fan display` with `idf.py menuconfig`.

Defaults use active-low inputs for the tested G3MB-202P relay module: GPIO Low enables a channel; GPIO High disables it. This applies to all three speed channels and rotation. Verify the module's input polarity and 3.3 V logic compatibility before connecting the fan; the SSR model alone does not determine the module's trigger polarity. Change polarity under `Smart fan hardware` with `idf.py menuconfig`.

For an existing build, disable `Relay inputs are active-high` in `menuconfig`; `sdkconfig.defaults` does not override an existing `sdkconfig`. Test with the fan's mains supply disconnected first. Firmware sets the inactive levels before enabling GPIO outputs, but it cannot guarantee those levels during reset or before startup. Use appropriate external biasing or a compatible interface circuit to keep the module inputs inactive then; never apply 5 V to an ESP32 GPIO.

The firmware always disables all speed relays before enabling another one. Turning the fan off also clears rotation; rotation cannot be enabled while the fan is off.

## Physical buttons

Connect a normally-open momentary button between **D7 (GPIO17) and GND** for speed, and another between **D9 (GPIO20) and GND** for rotation. Internal pullups are enabled; do not connect the buttons to 5 V. These are XIAO pin labels, not GPIO7/GPIO9.

- Speed: each press cycles **Off → Low → Medium → High → Off**.
- Rotation: each press toggles oscillation while the fan is running; ignored while off.
- Holding a button does not repeat the action. Espressif's `button` component handles debouncing.
- Buttons work without Wi-Fi or Home Assistant, and update Matter attributes and the OLED. Inputs are ignored during firmware installation.

D7 is also UART RX. Logging uses the native USB Serial/JTAG console to free GPIO17; continue using the USB-C serial port for monitoring, not a UART adapter on D6/D7. Button GPIOs and enablement are configurable under `Smart fan buttons` in `menuconfig`.

## Display

SSD1306 is the default controller, commonly used by 0.96-inch OLEDs. For an SH1106 display (commonly 1.3-inch), run:

```sh
eim run "idf.py menuconfig" v5.5.5
```

Under `Smart fan display` → `OLED controller`, select `SH1106`, save, then rebuild and flash. Alternatively, use the isolated controller profiles below. The controller is compiled into the firmware, not autodetected over I²C: both controllers can share the same address, and physical size alone does not identify them. Automatic OTA variant selection uses this compiled controller. Once installed, updates stay on that variant; swapping display controllers requires a wired flash of the other image. Both use the same 128×64 layout; the SH1106 backend handles its page addressing and two-column RAM offset.

The OLED shows the Matter QR code while the factory-new device is open for commissioning. The QR payload is generated from the active Matter commissioning provider, so production factory data will automatically replace the development payload. If the commissioning window expires, the display shows `PAIRING CLOSED`; reboot the factory-new device to reopen it.

After commissioning, the 128×64 dashboard uses a fan-icon header, a large speed digit (or `OFF`), and a separate rotation panel with arrows and `ON`/`OFF`. The footer shows a Wi-Fi icon and the connected SSID with its original ASCII case; long names are shortened to 18 glyphs with `...`, and unsupported characters use `?`. Offline devices show a slashed Wi-Fi icon and `WIFI OFFLINE` rather than the original QR. Both controllers render the same pixels.

The OLED turns off after **30 seconds of inactivity**, even while the fan runs. A speed/rotation change from Home Assistant or either physical button wakes it for another timeout period. Button presses still perform their normal action; pressing rotation while the fan is off only wakes the display. Unchanged reports and background Wi-Fi status updates do not extend the timeout or wake it at night; the latest status appears on the next wake. The pairing QR stays visible while commissioning is open.

Configure `OLED inactivity timeout` under `Smart fan display` in `menuconfig`, or set `CONFIG_FAN_DISPLAY_SLEEP_TIMEOUT_SECONDS` in the build configuration. Use `0` to keep the screen on. Both SSD1306 and SH1106 use their display-off command; the fan, Matter connection, and buttons continue running.

## Toolchain

- ESP-IDF v5.5.5
- `espressif/esp_matter` v1.6.0
- Target: `esp32c6`

Build either controller profile with EIM, starting with fresh build directories for reproducible defaults. These commands use separate generated configs and leave the existing root `sdkconfig` untouched; defaults do not override an existing generated config. To repeat a clean profile build, remove only its generated build directory first.

```sh
eim run "FIRMWARE_VERSION=0.2.2 idf.py -B build/ssd1306 -DSDKCONFIG=build/ssd1306/sdkconfig -DSDKCONFIG_DEFAULTS='sdkconfig.defaults;config/sdkconfig.ssd1306' reconfigure build" v5.5.5
eim run "FIRMWARE_VERSION=0.2.2 idf.py -B build/sh1106 -DSDKCONFIG=build/sh1106/sdkconfig -DSDKCONFIG_DEFAULTS='sdkconfig.defaults;config/sdkconfig.sh1106' reconfigure build" v5.5.5
```

The target comes from `sdkconfig.defaults`. Local outputs are still named `smart_fan.bin` (and `smart_fan.elf`) within each build directory; release assets are renamed by controller. `0.2.2` is an example version: an OTA release must be strictly newer than the installed firmware.

Flash and monitor the matching build, replacing the port if needed (use `-B build/sh1106` for SH1106):

```sh
eim run "FIRMWARE_VERSION=0.2.2 idf.py -B build/ssd1306 -p /dev/cu.usbmodemXXXX flash monitor" v5.5.5
```

### Native helper tests

With Python 3 and a native C++17 compiler named `c++`, run:

```sh
python3 -m unittest discover -s tests -v
```

These standard-library tests compile the real variant helper for both controllers with temporary `sdkconfig.h` files under `build/`; no ESP-IDF build or Python dependencies are required.

## Home Assistant commissioning

The Home Assistant mobile app needs Bluetooth access near the fan for initial commissioning. Add a Matter device and scan the QR code shown on the OLED or printed in the serial log; the commissioning flow provisions the fan with Wi-Fi credentials.

When replacing an already commissioned Thread build, clear its previous commissioning data before pairing the Wi-Fi build.

Development builds use Matter test credentials and the default setup code. Generate unique factory data and device attestation credentials before production.

The default build omits the development Matter shell to preserve flash space. To clear commissioning data, erase and reflash the device:

```sh
eim run "idf.py -p /dev/cu.usbmodemXXXX erase-flash flash" v5.5.5
```

## Remote diagnostics

UDP syslog is enabled by default for **`nas.local:5514`**. Configure or disable it under `Smart fan diagnostics` in `menuconfig`:

```text
CONFIG_FAN_SYSLOG_ENABLED=y
CONFIG_FAN_SYSLOG_HOST="nas.local"
CONFIG_FAN_SYSLOG_PORT=5514
CONFIG_FAN_SYSLOG_HEALTH_INTERVAL_SECONDS=300
```

The receiver gets RFC5424 messages with facility `local0`, a hostname such as `smart-fan-a16e40` (MAC suffix), and the ESP-IDF tag as the application name. Application INFO messages and parsed WARN/ERROR messages are forwarded; noisy Matter INFO, attribute reports, and commissioning-code output are excluded. Existing console logging is preserved. Capture requires ESP-IDF Log V1 text mode; ROM/panic output and arbitrary `printf` output are not forwarded.

Startup/reconnect health and five-minute reports include firmware version, display variant, reset reason, uptime, free/minimum heap, Wi-Fi RSSI, fan state, queue drops, UDP send failures, and running OTA-image state. Set the interval to `0` to keep only startup/reconnect reports. OTA state is not proof that a rollback occurred. Wall-clock timestamps are omitted because the device has no synchronized clock; the collector can timestamp receipt, and messages contain device uptime.

A fixed 16-record queue and a background worker keep DNS and socket operations out of relay control. Logs collected before Wi-Fi connects are buffered only up to that limit; excess messages are dropped. `.local` names use the existing mDNS responder; if multicast cannot reach the NAS, configure its stable IPv4 address instead. Allow UDP port `5514` from the fan network, not from the internet.

UDP logging is plaintext, lossy, and unacknowledged. Warning/error bodies are not comprehensively redacted—use only a trusted LAN and avoid logging secrets. No persistent crash dump or flash-backed log storage is added: after a crash, a subsequent successful boot can report the reset reason, but not the previous panic backtrace. Diagnostics initialization failure does not stop the fan.

## HTTPS OTA

The firmware checks a GitHub Release URL after Wi-Fi connects. It installs only a newer semantic version and only while the device is commissioned and the fan is off. Matter data in NVS is preserved. ESP-IDF rollback restores the previous image if the updated firmware cannot complete startup.

### Manual update check

When HTTPS OTA is enabled, an additional Matter switch requests an immediate check. It resets to off after accepting the request. The fan must be off, commissioned, and connected to Wi-Fi. A newer release is installed automatically; this is not a check-only action.

Home Assistant has no standard Matter button mapping for custom HTTP OTA checks. Rename the new switch to `switch.smart_fan_ota_check`, then optionally add a template button to `configuration.yaml` (merge it into an existing `template:` section):

```yaml
template:
  - button:
      - name: "Smart Fan Check for Updates"
        unique_id: smart_fan_check_for_updates
        icon: mdi:cloud-download
        press:
          - action: switch.turn_on
            target:
              entity_id: switch.smart_fan_ota_check
```

Use your actual switch entity ID if it differs. Reload template entities to expose the `button` entity, then add it to the dashboard. Results are logged by the firmware; the button does not expose update availability or progress.

The root, fan, and OTA switch require `CONFIG_ESP_MATTER_MAX_DYNAMIC_ENDPOINT_COUNT=3`. For an existing local build, update this setting in `idf.py menuconfig` too; changing `sdkconfig.defaults` alone does not override `sdkconfig`. Home Assistant may need an integration reload to discover the added endpoint; do not factory-reset the device for this.

Configure the URL under `Smart fan OTA` with `idf.py menuconfig`:

```text
https://github.com/OWNER/REPOSITORY/releases/latest/download/smart_fan-{display}.bin
```

The current helper replaces every literal `{display}` with the compiled `ssd1306` or `sh1106` variant. It also converts a configured URL ending exactly in `/smart_fan.bin` to `/smart_fan-{display}.bin` before substitution; other literal URLs remain unchanged.

**Legacy OTA clients migrate automatically to SH1106.** Releases also publish `smart_fan.bin` as a byte-for-byte alias of `smart_fan-sh1106.bin`. Existing firmware requesting that old filename can install a newer release without a wired flash. After reboot, the installed SH1106 image requests `smart_fan-sh1106.bin` for future updates.

This migration intentionally assumes SH1106 hardware; it is not display autodetection. Any legacy client still using `smart_fan.bin` receives SH1106 firmware, even if its hardware is SSD1306. Such SSD1306 devices need a one-time wired flash of the SSD1306 variant instead. Updates still require a strictly newer version and the fan to be off.

A wired full flash is also required when migrating from the old OTA partition layout; application-only OTA cannot change that layout. A normal wired flash preserves Matter data in NVS; do not erase flash for this migration.

Direct device downloads require a public repository. For private source code, publish firmware in a separate public release repository or serve it through an authenticated NAS proxy; never embed a GitHub token in firmware.

Release tags must use `vMAJOR.MINOR.PATCH`, for example:

```sh
git tag v0.2.2
git push origin v0.2.2
```

`.github/workflows/release.yml` injects the tag as the firmware version and builds both controller profiles with ESP-IDF v5.5.5. Only after both matrix builds pass does it publish:

- `smart_fan-ssd1306.bin`, `smart_fan-ssd1306.elf`, `smart_fan-ssd1306.sha256`
- `smart_fan-sh1106.bin`, `smart_fan-sh1106.elf`, `smart_fan-sh1106.sha256`

Each `.sha256` checksums its matching `.bin`. For legacy OTA clients, `smart_fan.bin` and `smart_fan.elf` alias the SH1106 build; `smart_fan.sha256` checksums the legacy binary filename. These aliases are created only after both variants build successfully—there are still just two distinct firmware variants. The workflow injects its own public release URL with `{display}`, so release builds need no hard-coded owner or repository. Use a new tag/version strictly greater than the firmware installed on devices that should update.
