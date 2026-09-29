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
| OLED SDA | D4 | 22 |
| OLED SCL | D5 | 23 |

Connect a 128×64 I²C SSD1306 display to `3V3` and `GND`. Its default address is `0x3C`; the pins and address are configurable under `Smart fan display` with `idf.py menuconfig`.

Defaults assume active-high relay inputs. Verify the GPIO mapping and relay polarity before connecting the fan. Change them under `Smart fan hardware` with `idf.py menuconfig`.

The firmware always disables all speed relays before enabling another one. Rotation is physically disabled while the fan is off, while its requested state is retained.

## Display

The OLED shows the Matter QR code while the factory-new device is open for commissioning. The QR payload is generated from the active Matter commissioning provider, so production factory data will automatically replace the development payload. If the commissioning window expires, the display shows `PAIRING CLOSED`; reboot the factory-new device to reopen it.

After commissioning, the display shows Wi-Fi connectivity, fan speed, and rotation state. An offline commissioned device shows `WIFI OFFLINE` instead of the original QR because that QR cannot reopen a closed commissioning window.

## Toolchain

- ESP-IDF v5.5.5
- `espressif/esp_matter` v1.6.0
- Target: `esp32c6`

Build with EIM:

```sh
eim run "idf.py set-target esp32c6" v5.5.5
eim run "idf.py build" v5.5.5
```

Flash and monitor, replacing the port if needed:

```sh
eim run "idf.py -p /dev/cu.usbmodemXXXX flash monitor" v5.5.5
```

## Home Assistant commissioning

The Home Assistant mobile app needs Bluetooth access near the fan for initial commissioning. Add a Matter device and scan the QR code shown on the OLED or printed in the serial log; the commissioning flow provisions the fan with Wi-Fi credentials.

When replacing an already commissioned Thread build, clear its previous commissioning data before pairing the Wi-Fi build.

Development builds use Matter test credentials and the default setup code. Generate unique factory data and device attestation credentials before production.

The default build omits the development Matter shell to preserve flash space. To clear commissioning data, erase and reflash the device:

```sh
eim run "idf.py -p /dev/cu.usbmodemXXXX erase-flash flash" v5.5.5
```

## HTTPS OTA

The firmware checks a GitHub Release URL after Wi-Fi connects. It installs only a newer semantic version and only while the device is commissioned and the fan is off. Matter data in NVS is preserved. ESP-IDF rollback restores the previous image if the updated firmware cannot complete startup.

Configure the URL under `Smart fan OTA` with `idf.py menuconfig`:

```text
https://github.com/OWNER/REPOSITORY/releases/latest/download/smart_fan.bin
```

After creating the repository, perform one wired full flash with this URL configured. The OTA partition layout changed and cannot update itself through an application-only OTA. A normal wired flash preserves Matter data in NVS:

```sh
eim run "idf.py -p /dev/cu.usbmodemXXXX flash" v5.5.5
```

Direct device downloads require a public repository. For private source code, publish firmware in a separate public release repository or serve it through an authenticated NAS proxy; never embed a GitHub token in firmware.

Release tags must use `vMAJOR.MINOR.PATCH`, for example:

```sh
git tag v0.2.0
git push origin v0.2.0
```

`.github/workflows/release.yml` injects the tag as the firmware version, builds with ESP-IDF v5.5.5, and publishes `smart_fan.bin`. The workflow also injects its own public release URL, so release builds do not need the owner or repository name hard-coded.
