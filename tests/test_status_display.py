"""Execute the real OLED task as native C++17 with deterministic SDK fakes."""

import shutil
import struct
import subprocess
import tempfile
import unittest
import zlib
from contextlib import contextmanager
from pathlib import Path

PROJECT_ROOT = Path(__file__).resolve().parents[1]
STUBS = Path(__file__).resolve().parent / "status_display_stubs"
SDK_HEADERS = (
    "driver/gpio.h",
    "driver/i2c_master.h",
    "esp_err.h",
    "esp_check.h",
    "esp_log.h",
    "esp_lcd_panel_io.h",
    "esp_lcd_panel_ops.h",
    "esp_lcd_panel_ssd1306.h",
    "freertos/FreeRTOS.h",
    "freertos/queue.h",
    "freertos/semphr.h",
    "freertos/task.h",
    "qrcode.h",
)


@contextmanager
def _native_harness():
    compiler = shutil.which("c++")
    if compiler is None:
        raise RuntimeError("These tests require a native C++17 compiler named c++")

    build_root = PROJECT_ROOT / "build" / "status-display-tests"
    build_root.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="native-", dir=build_root) as temporary:
        directory = Path(temporary)
        # Keep SDK include shims and compiler artifacts out of the source tree.
        for name in SDK_HEADERS:
            header = directory / name
            header.parent.mkdir(parents=True, exist_ok=True)
            header.write_text('#include "sdk.hpp"\n', encoding="utf-8")

        executable = directory / "status_display_test"
        result = subprocess.run(
            [
                compiler,
                "-std=c++17",
                "-Wall",
                "-Wextra",
                "-Werror",
                "-I",
                str(directory),
                "-I",
                str(STUBS),
                "-I",
                str(PROJECT_ROOT / "main"),
                str(STUBS / "harness.cpp"),
                str(PROJECT_ROOT / "main" / "status_display.cpp"),
                "-o",
                str(executable),
            ],
            capture_output=True,
            text=True,
            timeout=60,
        )
        if result.returncode:
            raise RuntimeError(
                f"Native compilation failed:\n{result.stdout}{result.stderr}"
            )
        yield executable


def _capture_frames(executable, controller, scenario):
    result = subprocess.run(
        [str(executable), controller, scenario],
        capture_output=True,
        timeout=10,
    )
    if result.returncode or result.stderr:
        raise RuntimeError(
            f"{controller}/{scenario}: exit {result.returncode}\n"
            f"{result.stderr.decode(errors='replace')}"
        )
    if not result.stdout or len(result.stdout) % 1024:
        raise RuntimeError(f"Invalid raw framebuffer length: {len(result.stdout)}")
    return result.stdout


def _write_frame_png(frame, path, scale=6):
    """Unpack captured page-major pixels; enlarge without inventing any UI pixels."""
    if len(frame) != 1024:
        raise ValueError("A 128x64 monochrome framebuffer must contain 1024 bytes")
    if not isinstance(scale, int) or scale < 1:
        raise ValueError("Pixel scale must be a positive integer")

    rows = bytearray()
    for y in range(64):
        row = bytes(
            255 if frame[(y // 8) * 128 + x] & (1 << (y % 8)) else 0
            for x in range(128)
            for _ in range(scale)
        )
        rows.extend((b"\0" + row) * scale)  # PNG filter 0; nearest-neighbor rows.

    def chunk(kind, data):
        return (
            struct.pack(">I", len(data))
            + kind
            + data
            + struct.pack(">I", zlib.crc32(kind + data))
        )

    png = (
        b"\x89PNG\r\n\x1a\n"
        + chunk(
            b"IHDR", struct.pack(">IIBBBBB", 128 * scale, 64 * scale, 8, 0, 0, 0, 0)
        )
        + chunk(b"IDAT", zlib.compress(rows))
        + chunk(b"IEND", b"")
    )
    path = Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(png)
    return path


def export_dashboard_preview(path, *, controller="ssd1306", offline=False):
    """Export the production dashboard framebuffer as a 6× PNG."""
    if controller not in ("ssd1306", "sh1106"):
        raise ValueError(f"Unknown display controller: {controller}")
    with _native_harness() as executable:
        frame = _capture_frames(
            executable, controller, "export_offline" if offline else "export_dashboard"
        )
    return _write_frame_png(frame, path)


class StatusDisplayTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        harness = _native_harness()
        cls.executable = harness.__enter__()
        cls.addClassCleanup(harness.__exit__, None, None, None)

    def run_scenario(self, scenario):
        for controller in ("ssd1306", "sh1106"):
            with self.subTest(controller=controller):
                result = subprocess.run(
                    [str(self.executable), controller, scenario],
                    capture_output=True,
                    text=True,
                    timeout=10,
                )
                self.assertEqual(
                    result.returncode,
                    0,
                    f"{controller}/{scenario}:\n{result.stdout}{result.stderr}",
                )
                self.assertEqual(result.stderr, "")

    def test_default_idle_deadline_and_controller_power_api(self):
        self.run_scenario("idle")

    def test_fan_changes_wake_and_identical_reports_do_not_extend_deadline(self):
        self.run_scenario("fan_activity")

    def test_explicit_unchanged_state_wake_and_deadline_reset(self):
        self.run_scenario("explicit_wake")

    def test_status_does_not_wake_or_extend_deadline_and_latest_frame_is_cached(self):
        self.run_scenario("status_and_cache")

    def test_commissioning_qr_stays_awake_then_resumes_full_timeout(self):
        self.run_scenario("commissioning")

    def test_commissioning_without_qr_does_not_disable_inactivity(self):
        self.run_scenario("commissioning_without_qr")

    def test_zero_timeout_disables_automatic_sleep(self):
        self.run_scenario("disabled")

    def test_elapsed_ticks_across_uint32_overflow(self):
        self.run_scenario("tick_wrap")

    def test_failed_sleep_logs_and_backs_off_without_busy_loop(self):
        self.run_scenario("sleep_failure")

    def test_failed_wake_logs_and_waits_for_new_activity(self):
        self.run_scenario("wake_failure")

    def test_dashboard_layout_and_all_eight_states_match_between_controllers(self):
        frames = [
            _capture_frames(self.executable, controller, "dashboard_frames")
            for controller in ("ssd1306", "sh1106")
        ]
        self.assertEqual(len(frames[0]), 8 * 1024)
        self.assertEqual(frames[0], frames[1])
        self.assertEqual(
            len({frames[0][offset : offset + 1024] for offset in range(0, 8192, 1024)}),
            8,
            "OFF and speeds 1-3 / rotation ON-OFF must each produce distinct frames",
        )

    def test_ssid_preserves_ascii_case_underscore_and_punctuation(self):
        self.run_scenario("ssid_ascii")

    def test_ssid_bounded_32_byte_copy_and_18_glyph_ellipsis(self):
        self.run_scenario("ssid_bounds")

    def test_ssid_utf8_uses_one_fallback_per_codepoint(self):
        self.run_scenario("ssid_utf8")

    def test_explicit_connection_labels_hidden_ssid_and_disconnected_icon(self):
        self.run_scenario("connection_footers")

    def test_ssid_only_status_does_not_wake_or_extend_deadline_and_button_wake_refreshes(
        self,
    ):
        self.run_scenario("ssid_status_and_cache")

    def test_raw_dashboard_export_matches_speed_two_rotation_on(self):
        frames = _capture_frames(self.executable, "ssd1306", "dashboard_frames")
        for controller in ("ssd1306", "sh1106"):
            with self.subTest(controller=controller):
                online = _capture_frames(
                    self.executable, controller, "export_dashboard"
                )
                self.assertEqual(online, frames[5 * 1024 : 6 * 1024])
                offline = _capture_frames(self.executable, controller, "export_offline")
                self.assertEqual(len(offline), 1024)
                self.assertNotEqual(online, offline)

    def test_preview_png_is_exact_six_times_nearest_neighbor_captured_pixels(self):
        frame = _capture_frames(self.executable, "ssd1306", "export_dashboard")
        path = self.executable.parent / "preview.png"
        self.assertEqual(_write_frame_png(frame, path), path)
        png = path.read_bytes()
        self.assertEqual(png[:8], b"\x89PNG\r\n\x1a\n")
        chunks = []
        offset = 8
        while offset < len(png):
            length = struct.unpack_from(">I", png, offset)[0]
            kind = png[offset + 4 : offset + 8]
            data = png[offset + 8 : offset + 8 + length]
            crc = struct.unpack_from(">I", png, offset + 8 + length)[0]
            self.assertEqual(crc, zlib.crc32(kind + data))
            chunks.append((kind, data))
            offset += length + 12
        self.assertEqual(offset, len(png))
        self.assertEqual([kind for kind, _ in chunks], [b"IHDR", b"IDAT", b"IEND"])
        self.assertEqual(
            struct.unpack(">IIBBBBB", chunks[0][1]), (768, 384, 8, 0, 0, 0, 0)
        )
        pixels = zlib.decompress(chunks[1][1])
        self.assertEqual(len(pixels), 384 * (768 + 1))
        for y in range(64):
            row = b"\0" + b"".join(
                bytes([255 if frame[(y // 8) * 128 + x] & (1 << (y % 8)) else 0]) * 6
                for x in range(128)
            )
            for repeat in range(6):
                start = (y * 6 + repeat) * 769
                self.assertEqual(pixels[start : start + 769], row)


if __name__ == "__main__":
    unittest.main()
