"""Run the real formatter and logger with scripted, single-threaded SDK fakes."""

import os
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

PROJECT_ROOT = Path(__file__).resolve().parents[1]
STUBS = Path(__file__).resolve().parent / "syslog_stubs"
SDK_HEADERS = (
    "esp_err.h",
    "esp_app_desc.h",
    "esp_log.h",
    "esp_mac.h",
    "esp_ota_ops.h",
    "esp_system.h",
    "esp_timer.h",
    "esp_wifi.h",
    "freertos/FreeRTOS.h",
    "freertos/queue.h",
    "freertos/task.h",
    "lwip/inet.h",
    "lwip/netdb.h",
    "lwip/sockets.h",
    "mdns.h",
)
EXECUTABLES = {}
SANITIZER_ENV = {
    **os.environ,
    "ASAN_OPTIONS": "detect_leaks=0:halt_on_error=1:abort_on_error=1",
    "UBSAN_OPTIONS": "halt_on_error=1:print_stacktrace=1",
}


def _compile(command):
    result = subprocess.run(
        command, capture_output=True, text=True, timeout=60, check=False
    )
    if result.returncode:
        raise RuntimeError(
            f"Native compilation failed:\n{result.stdout}{result.stderr}"
        )


def setUpModule():
    compiler = shutil.which("c++")
    if compiler is None:
        raise RuntimeError("These tests require a native C++17 compiler named c++")
    build_root = PROJECT_ROOT / "build" / "syslog-tests"
    build_root.mkdir(parents=True, exist_ok=True)
    temporary = tempfile.TemporaryDirectory(prefix="native-", dir=build_root)
    unittest.addModuleCleanup(temporary.cleanup)
    directory = Path(temporary.name)
    for name in SDK_HEADERS:
        header = directory / name
        header.parent.mkdir(parents=True, exist_ok=True)
        header.write_text('#include "sdk.hpp"\n', encoding="utf-8")
    (directory / "sdkconfig.h").write_text(
        "#pragma once\n"
        "#define CONFIG_FAN_SYSLOG_ENABLED 1\n"
        "#define CONFIG_LOG_VERSION_1 1\n"
        "#define CONFIG_LOG_MODE_TEXT 1\n"
        "#define CONFIG_FAN_DISPLAY_CONTROLLER_SSD1306 1\n",
        encoding="utf-8",
    )
    base = [
        compiler,
        "-std=c++17",
        "-Wall",
        "-Wextra",
        "-Werror",
        "-g",
        "-I",
        str(directory),
        "-I",
        str(STUBS),
        "-I",
        str(PROJECT_ROOT / "main"),
    ]
    configurations = {"native": []}
    sanitizer_flags = ["-fsanitize=address,undefined", "-fno-omit-frame-pointer"]
    probe = directory / "sanitizer_probe.cpp"
    probe.write_text("int main() { return 0; }\n", encoding="utf-8")
    probe_binary = directory / "sanitizer_probe"
    result = subprocess.run(
        [*base, *sanitizer_flags, str(probe), "-o", str(probe_binary)],
        capture_output=True,
        text=True,
        timeout=60,
        check=False,
    )
    if result.returncode == 0:
        result = subprocess.run(
            [str(probe_binary)],
            capture_output=True,
            text=True,
            env=SANITIZER_ENV,
            timeout=10,
            check=False,
        )
    if result.returncode == 0:
        configurations["asan+ubsan"] = sanitizer_flags
        print(
            "Syslog harnesses: native and ASan+UBSan (leak detection disabled).",
            flush=True,
        )
    else:
        print(
            "Syslog harnesses: native only; ASan+UBSan probe unavailable:\n"
            + result.stdout
            + result.stderr,
            flush=True,
        )
    EXECUTABLES.clear()
    for configuration, flags in configurations.items():
        for kind in ("formatter", "logger"):
            executable = directory / f"{kind}-{configuration}"
            sources = [str(STUBS / f"{kind}.cpp")]
            if kind == "logger":
                sources.append(str(PROJECT_ROOT / "main" / "remote_logger.cpp"))
            _compile([*base, *flags, *sources, "-o", str(executable)])
            EXECUTABLES[configuration, kind] = executable


class NativeScenarios(unittest.TestCase):
    def run_scenario(self, kind, scenario, *args):
        for (configuration, executable_kind), executable in EXECUTABLES.items():
            if executable_kind != kind:
                continue
            with self.subTest(configuration=configuration, args=args):
                result = subprocess.run(
                    [str(executable), scenario, *args],
                    capture_output=True,
                    text=True,
                    env=SANITIZER_ENV,
                    timeout=10,
                    check=False,
                )
                self.assertEqual(
                    result.returncode,
                    0,
                    f"{kind}/{scenario}/{configuration}:\n{result.stdout}{result.stderr}",
                )
                self.assertEqual(result.stdout, "")
                self.assertEqual(result.stderr, "")


class SyslogFormatterTests(NativeScenarios):
    def test_log_v1_levels_timestamp_grammar_and_tag_views(self):
        self.run_scenario("formatter", "parsing")

    def test_malformed_and_unstructured_lines_do_not_mutate_result(self):
        self.run_scenario("formatter", "malformed")

    def test_exact_application_info_allowlist_all_warn_error_and_no_debug_verbose(self):
        self.run_scenario("formatter", "filtering")

    def test_rfc5424_priorities_nilvalues_and_token_length_limits(self):
        self.run_scenario("formatter", "headers")

    def test_header_injection_controls_and_all_256_byte_values(self):
        self.run_scenario("formatter", "header_injection")

    def test_ansi_csi_osc_bel_st_unterminated_and_inplace_removal(self):
        self.run_scenario("formatter", "ansi")

    def test_controls_embedded_nul_and_multiline_stay_in_one_body(self):
        self.run_scenario("formatter", "controls_multiline")

    def test_null_zero_and_single_byte_output_buffers(self):
        self.run_scenario("formatter", "null_and_zero")

    def test_sanitizer_capacity_sweep_and_inplace_byte_canaries(self):
        self.run_scenario("formatter", "sanitize_bounds")

    def test_packet_capacity_sweep_exact_header_fit_and_768_byte_clamp(self):
        self.run_scenario("formatter", "packet_bounds")

    def test_utf8_bytes_preserved_but_truncation_can_split_codepoints(self):
        self.run_scenario("formatter", "utf8_bytes")


class RemoteLoggerTests(NativeScenarios):
    def test_previous_vprintf_arguments_output_return_and_no_hook_networking(self):
        self.run_scenario("logger", "hook_passthrough")

    def test_real_hook_ansi_parsing_and_level_tag_filtering(self):
        self.run_scenario("logger", "hook_filtering")

    def test_isr_and_worker_excluded_but_original_console_preserved(self):
        self.run_scenario("logger", "hook_context")

    def test_hook_copies_inputs_truncates_bytes_and_sends_multiline_once(self):
        self.run_scenario("logger", "hook_copy_and_bounds")

    def test_offline_queue_is_bounded_fifo_and_full_sends_drop_without_waiting(self):
        self.run_scenario("logger", "offline_queue")

    def test_startup_health_precedes_backlog_and_uses_copied_host(self):
        self.run_scenario("logger", "startup_health")

    def test_reconnect_health_precedes_backlog_even_between_worker_wakeups(self):
        self.run_scenario("logger", "reconnect")

    def test_local_hosts_use_mdns_without_shared_responder_init_or_free(self):
        for host in ("nas.local", "NAS.LoCaL."):
            self.run_scenario("logger", "mdns", host)

    def test_dns_and_not_yet_initialized_mdns_retry_after_45_seconds(self):
        for host in ("collector.example", "nas.local"):
            self.run_scenario("logger", "resolution_retry", host)

    def test_socket_failure_yields_and_preserves_backlog_until_retry(self):
        self.run_scenario("logger", "socket_retry")

    def test_nonblocking_fcntl_failure_closes_socket_and_backs_off(self):
        self.run_scenario("logger", "fcntl_retry")

    def test_health_send_failure_retains_startup_and_backlog(self):
        self.run_scenario("logger", "health_send_retry")

    def test_log_send_failure_drops_only_failed_record_and_reports_counters(self):
        self.run_scenario("logger", "log_send_retry")

    def test_zero_health_interval_disables_periodic_only(self):
        self.run_scenario("logger", "health_zero")

    def test_periodic_health_deadlines_and_latest_fan_state(self):
        self.run_scenario("logger", "health_periodic")

    def test_network_epoch_change_during_dns_does_not_drain_stale_backlog(self):
        self.run_scenario("logger", "stale_dns_epoch")

    def test_continuous_producers_cannot_prevent_worker_yield_after_16_records(self):
        self.run_scenario("logger", "bounded_drain")

    def test_start_validation_single_owner_and_idempotence(self):
        self.run_scenario("logger", "start_validation")

    def test_failed_start_can_retry_without_installing_hook_or_leaking_queue(self):
        self.run_scenario("logger", "start_retry")


if __name__ == "__main__":
    unittest.main()
