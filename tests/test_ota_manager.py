"""Run the real OTA manager against finite, host-only SDK scripts."""

import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path
from typing import ClassVar

PROJECT_ROOT = Path(__file__).resolve().parents[1]
STUBS = Path(__file__).resolve().parent / "ota_stubs"
SDK_HEADERS = (
    "esp_err.h",
    "esp_app_desc.h",
    "esp_crt_bundle.h",
    "esp_http_client.h",
    "esp_https_ota.h",
    "esp_log.h",
    "esp_ota_ops.h",
    "esp_system.h",
    "freertos/FreeRTOS.h",
    "freertos/event_groups.h",
    "freertos/task.h",
)


class OtaManagerScenarios:
    class Base(unittest.TestCase):
        variant: ClassVar[str]

        @classmethod
        def setUpClass(cls):
            compiler = shutil.which("c++")
            if compiler is None:
                raise RuntimeError("These tests require a native C++17 compiler named c++")
            build_root = PROJECT_ROOT / "build" / "ota-manager-tests"
            build_root.mkdir(parents=True, exist_ok=True)
            temporary = tempfile.TemporaryDirectory(
                prefix=f"{cls.variant}-", dir=build_root
            )
            cls.addClassCleanup(temporary.cleanup)
            directory = Path(temporary.name)
            for name in SDK_HEADERS:
                header = directory / name
                header.parent.mkdir(parents=True, exist_ok=True)
                header.write_text('#include "sdk.hpp"\n', encoding="utf-8")
            (directory / "sdkconfig.h").write_text(
                "#pragma once\n#define CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE 1\n"
                f"#define CONFIG_FAN_DISPLAY_CONTROLLER_{cls.variant.upper()} 1\n",
                encoding="utf-8",
            )
            cls.executable = directory / "ota_manager_test"
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
                    str(PROJECT_ROOT / "main" / "ota_manager.cpp"),
                    "-o",
                    str(cls.executable),
                ],
                capture_output=True,
                text=True,
                check=False,
                timeout=60,
            )
            if result.returncode:
                raise RuntimeError(
                    f"Native compilation failed:\n{result.stdout}{result.stderr}"
                )

        def run_scenario(self, scenario):
            result = subprocess.run(
                [str(self.executable), scenario],
                capture_output=True,
                text=True,
                check=False,
                timeout=10,
            )
            self.assertEqual(
                result.returncode, 0, f"{scenario}:\n{result.stdout}{result.stderr}"
            )
            self.assertEqual(result.stderr, "")

        def test_current_version_is_silent(self):
            self.run_scenario("current")

        def test_older_version_is_silent(self):
            self.run_scenario("older")

        def test_begin_failure_is_silent(self):
            self.run_scenario("begin_failure")

        def test_descriptor_failure_is_silent(self):
            self.run_scenario("header_failure")

        def test_invalid_remote_version_is_silent(self):
            self.run_scenario("invalid_remote")

        def test_invalid_current_version_is_silent(self):
            self.run_scenario("invalid_current")

        def test_not_ready_at_install_guard_is_silent(self):
            self.run_scenario("not_ready")

        def test_task_skips_checks_when_not_ready(self):
            self.run_scenario("task_not_ready")

        def test_ordinary_task_check_clears_check_flag(self):
            self.run_scenario("task_current")

        def test_initial_descriptor_bytes_and_known_percent_throttling(self):
            self.run_scenario("known")

        def test_percent_uses_64_bit_math_and_clamps_at_100(self):
            self.run_scenario("large")

        def test_chunked_16k_threshold_and_final_actual_total(self):
            self.run_scenario("chunked")

        def test_zero_size_is_unknown(self):
            self.run_scenario("zero_total")

        def test_negative_size_is_sanitized(self):
            self.run_scenario("negative_total")

        def test_negative_received_bytes_are_sanitized(self):
            self.run_scenario("negative_received")

        def test_perform_failure_resets_ui_flag_and_aborts(self):
            self.run_scenario("perform_failure")

        def test_incomplete_download_preserves_invalid_size_error(self):
            self.run_scenario("incomplete")

        def test_finish_failure_resets_without_double_abort(self):
            self.run_scenario("finish_failure")

        def test_null_callback_success_never_delays(self):
            self.run_scenario("null_success")

        def test_null_callback_failure_cleans_up(self):
            self.run_scenario("null_failure")

        def test_failure_allows_subsequent_successful_install(self):
            self.run_scenario("retry")

        def test_variant_url_and_manual_check_guards(self):
            self.run_scenario("manual")

        def test_rollback_confirmation_is_silent(self):
            self.run_scenario("rollback")

        def test_stub_rejects_use_after_finish_and_abort(self):
            self.run_scenario("handle_guard")


class SSD1306OtaTests(OtaManagerScenarios.Base):
    variant = "ssd1306"


class SH1106OtaTests(OtaManagerScenarios.Base):
    variant = "sh1106"


if __name__ == "__main__":
    unittest.main()
