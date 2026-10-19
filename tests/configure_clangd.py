"""Generate persistent native-test compile commands and SDK include shims."""

import ast
import json
import shutil
from pathlib import Path

PROJECT_ROOT = Path(__file__).resolve().parents[1]
GROUPS = {
    "status_display": "test_status_display.py",
    "ota": "test_ota_manager.py",
    "syslog": "test_syslog.py",
}


def sdk_headers(test_file: Path) -> tuple[str, ...]:
    tree = ast.parse(test_file.read_text(encoding="utf-8"))
    for node in tree.body:
        if isinstance(node, ast.Assign) and any(
            isinstance(target, ast.Name) and target.id == "SDK_HEADERS"
            for target in node.targets
        ):
            return tuple(ast.literal_eval(node.value))
    raise ValueError(f"SDK_HEADERS not found in {test_file}")


def main() -> None:
    compiler = shutil.which("clang++") or shutil.which("c++")
    if compiler is None:
        raise SystemExit("A native C++17 compiler is required")

    database = PROJECT_ROOT / ".cache" / "clangd" / "host-tests"
    commands = []
    for group, test_name in GROUPS.items():
        stubs = PROJECT_ROOT / "tests" / f"{group}_stubs"
        includes = database / "include" / group
        includes.mkdir(parents=True, exist_ok=True)
        for name in sdk_headers(PROJECT_ROOT / "tests" / test_name):
            header = includes / name
            header.parent.mkdir(parents=True, exist_ok=True)
            header.write_text('#include "sdk.hpp"\n', encoding="utf-8")
        (includes / "sdkconfig.h").write_text(
            "#pragma once\n"
            "#define CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE 1\n"
            "#define CONFIG_FAN_DISPLAY_CONTROLLER_SSD1306 1\n"
            "#define CONFIG_FAN_SYSLOG_ENABLED 1\n"
            "#define CONFIG_LOG_VERSION_1 1\n"
            "#define CONFIG_LOG_MODE_TEXT 1\n",
            encoding="utf-8",
        )
        for source in sorted(stubs.iterdir()):
            if source.suffix not in (".cpp", ".hpp", ".h"):
                continue
            commands.append(
                {
                    "directory": str(PROJECT_ROOT),
                    "file": str(source),
                    "arguments": [
                        compiler,
                        "-x",
                        "c++" if source.suffix == ".cpp" else "c++-header",
                        "-std=c++17",
                        "-fexceptions",
                        "-frtti",
                        "-Wall",
                        "-Wextra",
                        "-I",
                        str(includes),
                        "-I",
                        str(stubs),
                        "-I",
                        str(PROJECT_ROOT / "main"),
                        "-c",
                        str(source),
                    ],
                }
            )
    output = database / "compile_commands.json"
    output.write_text(json.dumps(commands, indent=2) + "\n", encoding="utf-8")
    print(
        f"Wrote {len(commands)} native-test entries to {output.relative_to(PROJECT_ROOT)}"
    )


if __name__ == "__main__":
    main()
