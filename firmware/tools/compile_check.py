"""Build check for the firmware without installing ESP-IDF.

Uses the ESP32-S3 compiler and the prebuilt ESP-IDF 5.5 headers and libraries that come with the
Arduino IDE's "esp32" board package (3.3.x). It:

1. compiles every firmware C file for the ESP32-S3 with -Wall -Wextra -Werror;
2. links the recorder and mic_test apps against ESP-IDF, which proves every IDF function they
   call exists;
3. converts the recorder into a flashable image with esptool and reports its size against the
   3 MB app partition;
4. optionally (--host-tests CC) builds and runs the lc_core unit tests with a PC C compiler,
   e.g. --host-tests "python -m ziglang cc" or --host-tests gcc.

It is a check, not the release build: the Arduino libraries were built with Arduino's sdkconfig,
not firmware/recorder/sdkconfig.defaults. Build release images with `idf.py build`.

    python firmware/tools/compile_check.py [--host-tests "python -m ziglang cc"]
"""

from __future__ import annotations

import argparse
import os
import platform
import re
import shlex
import subprocess
import sys
import tempfile
import urllib.request
from pathlib import Path

FIRMWARE = Path(__file__).resolve().parent.parent
# idf_component.yml asks for espressif/esp_websocket_client ^1.3.0; this is the newest 1.x.
WEBSOCKET_TAG = "websocket-v1.8.0"
WEBSOCKET_URL = (
    "https://raw.githubusercontent.com/espressif/esp-protocols/"
    f"{WEBSOCKET_TAG}/components/esp_websocket_client"
)
APP_PARTITION_BYTES = 0x300000  # firmware/recorder/partitions.csv, "factory"
FLASH_LAYOUT = "qio_opi"  # QIO flash + octal PSRAM (ESP32-S3-DevKitC-1-N8R8)


def arduino_esp32() -> Path:
    candidates = {
        "Windows": Path(os.environ.get("LOCALAPPDATA", "")) / "Arduino15",
        "Darwin": Path.home() / "Library" / "Arduino15",
    }
    root = candidates.get(platform.system(), Path.home() / ".arduino15") / "packages" / "esp32"
    if not root.is_dir():
        sys.exit(f"Arduino esp32 board package not found at {root}. Install it from the Boards Manager.")
    return root


def newest(directory: Path) -> Path:
    versions = sorted(directory.iterdir(), key=lambda p: [int(x) for x in re.findall(r"\d+", p.name)])
    if not versions:
        sys.exit(f"Nothing installed in {directory}")
    return versions[-1]


def kconfig_defines(kconfig: Path) -> list[str]:
    """The firmware's own Kconfig defaults as -D flags (idf.py would generate these in sdkconfig.h)."""
    defines: list[str] = []
    name = kind = None
    for raw in kconfig.read_text(encoding="utf-8").splitlines():
        line = raw.strip()
        if match := re.match(r"config\s+(\w+)", line):
            name, kind = match.group(1), None
        elif (match := re.match(r"(string|int|bool|hex)\b", line)) and name:
            kind = match.group(1)
        elif (match := re.match(r"default\s+(.+)", line)) and name:
            value = match.group(1).strip()
            if kind == "bool":
                if value == "y":
                    defines.append(f"-DCONFIG_{name}=1")
            else:
                defines.append(f"-DCONFIG_{name}={value}")
            name = None
    return defines


def fetch_websocket_client(cache: Path) -> Path:
    target = cache / WEBSOCKET_TAG
    for relative in ("include/esp_websocket_client.h", "esp_websocket_client.c"):
        path = target / relative
        if not path.exists():
            path.parent.mkdir(parents=True, exist_ok=True)
            url = f"{WEBSOCKET_URL}/{relative}"  # fixed https:// URL, pinned tag
            with urllib.request.urlopen(url, timeout=60) as response:  # noqa: S310
                path.write_bytes(response.read())
    return target


def run(command: list[str], what: str) -> None:
    # Local compilers and tools found above, never user input from a network.
    result = subprocess.run(command, capture_output=True, text=True)  # noqa: S603
    output = "\n".join(
        line
        for line in (result.stdout + result.stderr).splitlines()
        if "undefined reference" in line or "error" in line.lower() or "warning:" in line
    )
    if result.returncode != 0:
        sys.exit(f"FAILED: {what}\n{output or result.stderr[-4000:]}")


def response_file(path: Path, args: list[str]) -> str:
    # gcc strips one level of quotes in @files, so string defines need escaped quotes.
    path.write_text("\n".join(arg.replace('"', '\\"') for arg in args), encoding="utf-8")
    return f"@{path}"


def main() -> None:
    parser = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter
    )
    parser.add_argument("--host-tests", metavar="CC", help="PC C compiler for the lc_core unit tests")
    args = parser.parse_args()

    esp32 = arduino_esp32()
    libs = newest(esp32 / "tools" / "esp32s3-libs")
    toolchain = newest(esp32 / "tools" / "esp-x32") / "bin"
    exe = ".exe" if platform.system() == "Windows" else ""
    gcc = str(toolchain / f"xtensa-esp32s3-elf-gcc{exe}")
    esptool = next((newest(esp32 / "tools" / "esptool_py")).glob(f"esptool{exe}"), None)
    flags = libs / "flags"
    work = Path(tempfile.mkdtemp(prefix="lc_fw_check_"))
    websocket = fetch_websocket_client(Path(tempfile.gettempdir()) / "lc_fw_check_cache")
    print(f"ESP-IDF libraries: {libs}\nCompiler: {gcc}\nOutput: {work}")

    defines = response_file(
        work / "kconfig.rsp", kconfig_defines(FIRMWARE / "recorder" / "main" / "Kconfig.projbuild")
    )
    common = [
        f"@{flags / 'defines'}",
        defines,
        f"@{flags / 'c_flags'}",
        "-O2",
        "-iprefix",
        f"{libs / 'include'}{os.sep}",
        f"@{flags / 'includes'}",
        "-I",
        str(libs / FLASH_LAYOUT / "include"),
        "-I",
        str(websocket / "include"),
        "-I",
        str(FIRMWARE / "lc_core" / "include"),
    ]
    strict = ["-Wall", "-Wextra", "-Werror", "-Wno-unused-parameter"]

    objects: dict[str, list[str]] = {"lc_core": [], "recorder": [], "mic_test": []}
    sources = {
        "lc_core": sorted((FIRMWARE / "lc_core" / "src").glob("*.c")),
        "recorder": sorted((FIRMWARE / "recorder" / "main").glob("*.c")),
        "mic_test": sorted((FIRMWARE / "mic_test" / "main").glob("*.c")),
    }
    for group, files in sources.items():
        for source in files:
            obj = work / f"{group}_{source.stem}.o"
            extra = ["-I", str(source.parent)]
            run([gcc, "-c", str(source), "-o", str(obj), *common, *extra, *strict], f"compile {source.name}")
            objects[group].append(str(obj))
    websocket_obj = work / "esp_websocket_client.o"
    run(
        [gcc, "-c", str(websocket / "esp_websocket_client.c"), "-o", str(websocket_obj), *common],
        "compile websocket client",
    )
    count = sum(len(v) for v in objects.values())
    print(f"Compiled {count} firmware files with -Wall -Wextra -Werror: OK")

    # Arduino's link flags force its setup()/loop() symbols; an ESP-IDF app has app_main instead.
    ld_flags = (flags / "ld_flags").read_text(encoding="utf-8")
    ld_flags = ld_flags.replace("-u _Z5setupv", "").replace("-u _Z4loopv", "")
    (work / "ld_flags.rsp").write_text(ld_flags, encoding="utf-8")
    link = [
        f"@{work / 'ld_flags.rsp'}",
        "-L",
        str(libs / "lib"),
        "-L",
        str(libs / "ld"),
        "-L",
        str(libs / FLASH_LAYOUT),
        f"@{flags / 'ld_scripts'}",
        "-Wl,--start-group",
        f"@{flags / 'ld_libs'}",
        "-Wl,--end-group",
    ]
    apps = {
        "recorder": [*objects["recorder"], *objects["lc_core"], str(websocket_obj)],
        "mic_test": [*objects["mic_test"], *objects["lc_core"]],
    }
    for app, objs in apps.items():
        run([gcc, "-o", str(work / f"{app}.elf"), *objs, *link], f"link {app}")
        print(f"Linked {app} against ESP-IDF: OK")

    if esptool is not None:
        image = work / "recorder.bin"
        run(
            [
                str(esptool),
                "--chip",
                "esp32s3",
                "elf2image",
                "--flash-mode",
                "qio",
                "--flash-size",
                "8MB",
                "-o",
                str(image),
                str(work / "recorder.elf"),
            ],
            "make the recorder image",
        )
        size = image.stat().st_size
        if size > APP_PARTITION_BYTES:
            sys.exit(
                f"FAILED: recorder image is {size} bytes, over the {APP_PARTITION_BYTES}-byte app partition"
            )
        print(
            f"Recorder image: {size / 1e6:.2f} MB of the {APP_PARTITION_BYTES / 2**20:.0f} MB partition: OK"
        )

    if args.host_tests:
        tests = work / f"lc_core_tests{exe}"
        cc = shlex.split(args.host_tests, posix=os.name != "nt")  # keep Windows backslashes
        run(
            [
                *cc,
                "-std=c11",
                "-Wall",
                "-Wextra",
                "-Werror",
                "-D_USE_MATH_DEFINES",
                "-I",
                str(FIRMWARE / "lc_core" / "include"),
                *map(str, sources["lc_core"]),
                str(FIRMWARE / "lc_core" / "test_host" / "test_main.c"),
                "-lm",
                "-o",
                str(tests),
            ],
            "build lc_core host tests",
        )
        # The tests read contracts/test_vectors relative to the repository root.
        result = subprocess.run([str(tests)], cwd=FIRMWARE.parent, capture_output=True, text=True)  # noqa: S603
        print(result.stdout.strip().splitlines()[-1] if result.stdout.strip() else result.stderr)
        if result.returncode != 0:
            sys.exit("FAILED: lc_core host tests")


if __name__ == "__main__":
    main()
