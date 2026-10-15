"""Native C++17 regression tests for the real firmware variant helper."""

import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

PROJECT_ROOT = Path(__file__).resolve().parents[1]
HARNESS = r"""
#include "firmware_variant.hpp"

#include <iostream>
#include <vector>

int main(int argc, char **argv)
{
    std::cout << firmware::kDisplayVariant << '\n';
    if (argc == 1) {
        std::cout << firmware::ota_url(nullptr) << '\n';
        return 0;
    }

    std::vector<std::string> urls;
    for (int i = 1; i < argc; ++i) {
        urls.push_back(firmware::ota_url(argv[i]));
    }
    for (const auto &url : urls) {
        std::cout << url << '\n';
    }
}
"""


class FirmwareVariantTests:
    @classmethod
    def setUpClass(cls):
        compiler = shutil.which("c++")
        if compiler is None:
            raise RuntimeError("These tests require a native C++17 compiler named c++")

        build_root = PROJECT_ROOT / "build" / "firmware-variant-tests"
        build_root.mkdir(parents=True, exist_ok=True)
        temporary = tempfile.TemporaryDirectory(
            prefix=f"{cls.variant}-", dir=build_root
        )
        cls.addClassCleanup(temporary.cleanup)
        directory = Path(temporary.name)
        # ESP-IDF leaves disabled bool options undefined in sdkconfig.h.
        config = "#pragma once\n"
        if cls.variant == "sh1106":
            config += "#define CONFIG_FAN_DISPLAY_CONTROLLER_SH1106 1\n"
        else:
            config += "#define CONFIG_FAN_DISPLAY_CONTROLLER_SSD1306 1\n"
        (directory / "sdkconfig.h").write_text(config, encoding="utf-8")
        source = directory / "firmware_variant_test.cpp"
        source.write_text(HARNESS, encoding="utf-8")
        cls.executable = directory / "firmware_variant_test"
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
                str(PROJECT_ROOT / "main"),
                str(source),
                "-o",
                str(cls.executable),
            ],
            capture_output=True,
            text=True,
            timeout=60,
        )
        if result.returncode:
            raise RuntimeError(
                f"Native compilation failed:\n{result.stdout}{result.stderr}"
            )

    def run_helper(self, *urls):
        result = subprocess.run(
            [str(self.executable), *urls],
            capture_output=True,
            text=True,
            check=True,
            timeout=10,
        )
        self.assertEqual(result.stderr, "")
        return result.stdout.splitlines()

    def assert_urls(self, urls, expected):
        self.assertEqual(self.run_helper(*urls), [self.variant, *expected])

    def test_exact_variant_name(self):
        self.assertEqual(self.run_helper(), [self.variant, ""])

    def test_templated_github_url(self):
        prefix = "https://github.com/OWNER/REPOSITORY/releases/latest/download/"
        self.assert_urls(
            [prefix + "smart_fan-{display}.bin"],
            [prefix + f"smart_fan-{self.variant}.bin"],
        )

    def test_legacy_trailing_asset(self):
        prefix = "https://github.com/OWNER/REPOSITORY/releases/latest/download"
        self.assert_urls(
            [prefix + "/smart_fan.bin", "/smart_fan.bin"],
            [
                prefix + f"/smart_fan-{self.variant}.bin",
                f"/smart_fan-{self.variant}.bin",
            ],
        )

    def test_null_and_empty(self):
        self.assertEqual(self.run_helper(), [self.variant, ""])
        self.assert_urls([""], [""])

    def test_custom_literal_urls_unchanged(self):
        urls = [
            "https://firmware.example/custom.bin",
            "https://firmware.example/smart_fan-sh1106.bin",
            "https://firmware.example/smart_fan-ssd1306.bin",
            "https://firmware.example/smart_fan.bin?token=abc",
            "https://firmware.example/smart_fan.bin#fragment",
            "https://firmware.example/smart_fan.bin/other.bin",
            "https://firmware.example/not_smart_fan.bin",
            "smart_fan.bin",
        ]
        self.assert_urls(urls, urls)

    def test_multiple_literal_placeholders(self):
        self.assert_urls(
            [
                "https://firmware.example/{display}/smart_fan-{display}.bin?{display}",
                "{display}{display}",
                "https://firmware.example/{display}/smart_fan.bin",
            ],
            [
                f"https://firmware.example/{self.variant}/smart_fan-{self.variant}.bin?{self.variant}",
                self.variant * 2,
                f"https://firmware.example/{self.variant}/smart_fan-{self.variant}.bin",
            ],
        )

    def test_repeated_calls_are_independent(self):
        urls = [
            "{display}",
            "",
            "/smart_fan.bin",
            "https://example.com/custom.bin",
            "{display}",
        ]
        expected = [
            self.variant,
            "",
            f"/smart_fan-{self.variant}.bin",
            urls[3],
            self.variant,
        ]
        self.assert_urls(urls * 3, expected * 3)


class SSD1306Tests(FirmwareVariantTests, unittest.TestCase):
    variant = "ssd1306"


class SH1106Tests(FirmwareVariantTests, unittest.TestCase):
    variant = "sh1106"


if __name__ == "__main__":
    unittest.main()
