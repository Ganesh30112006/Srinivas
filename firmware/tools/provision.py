"""Writes a recorder's settings into its NVS partition (0x9000) over USB.

  python firmware/tools/provision.py --port COM3 --device-id <uuid> --token <token> \
      --iot-endpoint <prefix>-ats.iot.ap-south-1.amazonaws.com \
      --cert backend/secrets/iot/<uuid>/cert.pem --key backend/secrets/iot/<uuid>/private.key \
      [--wifi-ssid NAME --wifi-pass PASSWORD]...   (up to 3 networks, tried in this order)

Use --server-url wss://host/ws/device instead of the three --iot-* options for the WebSocket
gateway. Without --wifi-ssid the recorder starts in Wi-Fi setup mode (its display explains how).

This REPLACES the whole NVS partition: saved Wi-Fi networks and any unfinished session state
are erased (audio still waiting on the SD card stays there but is no longer sent). The generated
files are written to a temporary folder and deleted afterwards; nothing secret is printed.
Needs ESP-IDF's Python environment (it uses nvs_partition_gen and esptool).
"""

from __future__ import annotations

import argparse
import csv
import os
import subprocess
import sys
import tempfile
import uuid
from pathlib import Path

NVS_OFFSET = "0x9000"
NVS_SIZE = "0x6000"


def main() -> int:
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--port", required=True)
    p.add_argument("--device-id", required=True)
    p.add_argument("--token", required=True)
    p.add_argument("--iot-endpoint")
    p.add_argument("--cert", type=Path)
    p.add_argument("--key", type=Path)
    p.add_argument("--server-url")
    p.add_argument("--wifi-ssid", action="append", default=[])
    p.add_argument("--wifi-pass", action="append", default=[])
    p.add_argument("--chip", default="esp32")
    a = p.parse_args()

    try:
        device_id = str(uuid.UUID(a.device_id))
    except ValueError:
        p.error("--device-id must be a UUID")
    use_iot = a.iot_endpoint is not None
    if use_iot and (a.cert is None or a.key is None):
        p.error("--iot-endpoint needs --cert and --key")
    if not use_iot and not a.server_url:
        p.error("give --iot-endpoint (AWS IoT Core) or --server-url (WebSocket gateway)")
    if len(a.wifi_ssid) > 3 or len(a.wifi_pass) > len(a.wifi_ssid):
        p.error("give up to 3 --wifi-ssid, each followed by its --wifi-pass")
    passwords = a.wifi_pass + [""] * (len(a.wifi_ssid) - len(a.wifi_pass))
    if any(0 < len(pw) < 8 for pw in passwords):
        p.error("a WPA2 password has at least 8 characters")
    for f in (a.cert, a.key):
        if f is not None and not f.is_file():
            p.error(f"{f} not found")

    idf = os.environ.get("IDF_PATH", r"C:\esp\esp-idf")
    gen = Path(idf) / "components" / "nvs_flash" / "nvs_partition_generator" / "nvs_partition_gen.py"
    if not gen.is_file():
        print(f"error: {gen} not found; set IDF_PATH", file=sys.stderr)
        return 1

    with tempfile.TemporaryDirectory() as tmp:
        rows: list[list[str]] = [["key", "type", "encoding", "value"], ["lc_cfg", "namespace", "", ""]]
        for i, (ssid, pw) in enumerate(zip(a.wifi_ssid, passwords, strict=True)):
            n = "" if i == 0 else str(i)
            rows += [[f"wifi_ssid{n}", "data", "string", ssid], [f"wifi_pass{n}", "data", "string", pw]]
        rows += [["device_id", "data", "string", device_id], ["device_token", "data", "string", a.token]]
        if use_iot:
            rows += [
                ["iot_endpoint", "data", "string", a.iot_endpoint],
                ["iot_cert", "file", "string", str(a.cert.resolve())],
                ["iot_key", "file", "string", str(a.key.resolve())],
            ]
        else:
            rows.append(["server_url", "data", "string", a.server_url])
        csv_path = Path(tmp) / "nvs.csv"
        with csv_path.open("w", newline="", encoding="utf-8") as fh:
            csv.writer(fh).writerows(rows)
        bin_path = Path(tmp) / "nvs.bin"
        subprocess.run([sys.executable, str(gen), "generate", str(csv_path), str(bin_path), NVS_SIZE], check=True,
                       stdout=subprocess.DEVNULL)
        subprocess.run([sys.executable, "-m", "esptool", "--chip", a.chip, "-p", a.port, "-b", "460800",
                        "write_flash", NVS_OFFSET, str(bin_path)], check=True)
    print(f"Provisioned {device_id} ({'AWS IoT Core' if use_iot else a.server_url}); "
          f"Wi-Fi: {', '.join(a.wifi_ssid) or 'none, the recorder starts in setup mode'}.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
