#!/usr/bin/env python3
"""
VESC Hyper Display - Wireless OTA Flasher
Uploads compiled firmware.bin to Waveshare ESP32-S3 (4.3" / 7.0") over WiFi SoftAP or Local LAN.

Usage:
    python3 tools/ota_flash.py [--ip 192.168.4.1] [--board 43|70] [--no-build] [--status]
"""

import sys
import os
import time
import argparse
import urllib.request
import urllib.error
import json
import subprocess

DEFAULT_IP = "192.168.4.1"

ENV_TARGETS = {
    "43": "waveshare-esp32-s3-touch-lcd-43",
    "4.3": "waveshare-esp32-s3-touch-lcd-43",
    "70": "waveshare-esp32-s3-touch-lcd-7",
    "7": "waveshare-esp32-s3-touch-lcd-7",
    "7.0": "waveshare-esp32-s3-touch-lcd-7",
    "waveshare-esp32-s3-touch-lcd-43": "waveshare-esp32-s3-touch-lcd-43",
    "waveshare-esp32-s3-touch-lcd-7": "waveshare-esp32-s3-touch-lcd-7",
}

def resolve_target(board=None, env=None, bin_path=None):
    chosen_env = "waveshare-esp32-s3-touch-lcd-43"
    if env:
        chosen_env = ENV_TARGETS.get(env.strip().lower(), env.strip())
    elif board:
        chosen_env = ENV_TARGETS.get(str(board).strip().lower(), "waveshare-esp32-s3-touch-lcd-43")

    if not bin_path:
        bin_path = f".pio/build/{chosen_env}/firmware.bin"

    return chosen_env, bin_path

def check_status(ip):
    url = f"http://{ip}/api/status"
    try:
        req = urllib.request.Request(url, headers={"User-Agent": "OtaFlasher/1.0"})
        with urllib.request.urlopen(req, timeout=3.0) as resp:
            if resp.status == 200:
                data = json.loads(resp.read().decode())
                print(f"[✓] VESC Display online @ {ip}:")
                print(f"    - Free Heap: {data.get('free_heap', 0) // 1024} KB")
                print(f"    - Status:    {data.get('status', 'online')}")
                return True
    except Exception:
        return False
    return False

def build_firmware(env_name="waveshare-esp32-s3-touch-lcd-43"):
    print(f"[*] Kompiliere PlatformIO Firmware fuer Target: {env_name}...")
    cmd = ["pio", "run", "-e", env_name]
    res = subprocess.run(cmd)
    if res.returncode != 0:
        print(f"[!] Build fuer {env_name} fehlgeschlagen. Abbruch.")
        sys.exit(1)
    print(f"[✓] Firmware ({env_name}) erfolgreich gebaut.")

class MultipartStream:
    def __init__(self, filename, filepath):
        self.boundary = "----WebKitFormBoundary" + hex(int(time.time() * 1000))[2:]
        self.filepath = filepath
        self.filename = filename
        self.filesize = os.path.getsize(filepath)

        header = (
            f"--{self.boundary}\r\n"
            f'Content-Disposition: form-data; name="update"; filename="{self.filename}"\r\n'
            f"Content-Type: application/octet-stream\r\n\r\n"
        ).encode("utf-8")
        footer = f"\r\n--{self.boundary}--\r\n".encode("utf-8")

        self.header = header
        self.footer = footer
        self.total_size = len(header) + self.filesize + len(footer)
        self.sent_bytes = 0

    def get_content_type(self):
        return f"multipart/form-data; boundary={self.boundary}"

def upload_firmware(ip, bin_path, env_name="waveshare-esp32-s3-touch-lcd-43"):
    if not os.path.exists(bin_path):
        print(f"[!] Firmware-Datei nicht gefunden: {bin_path}")
        print(f"    Tipp: Zuerst mit 'pio run -e {env_name}' kompilieren oder ohne '--no-build' aufrufen.")
        sys.exit(1)

    filesize = os.path.getsize(bin_path)
    filename = os.path.basename(bin_path)
    url = f"http://{ip}/update?size={filesize}"

    print(f"\n⚡ Starte kabellosen OTA-Flash: {filename} ({filesize / 1024:.1f} KB) -> {url}")
    print(f"   Target Environment: {env_name}")

    mp = MultipartStream(filename, bin_path)

    # Read binary and construct full payload
    with open(bin_path, "rb") as f:
        file_bytes = f.read()

    full_payload = mp.header + file_bytes + mp.footer
    total_len = len(full_payload)

    req = urllib.request.Request(url, data=full_payload, method="POST")
    req.add_header("Content-Type", mp.get_content_type())
    req.add_header("Content-Length", str(total_len))

    start_time = time.time()
    try:
        print("[*] Sende Firmware-Pakete...")
        with urllib.request.urlopen(req, timeout=30.0) as resp:
            resp_data = resp.read().decode()
            elapsed = time.time() - start_time
            speed_kb = (filesize / 1024.0) / elapsed if elapsed > 0 else 0

            print(f"\n[✓] {resp_data.strip()}")
            print(f"[✓] Upload abgeschlossen in {elapsed:.1f}s ({speed_kb:.1f} KB/s)")
            print("[*] Display startet neu... (ca. 4-6 Sekunden)")

            # Wait for reboot
            time.sleep(4)
            for _ in range(10):
                if check_status(ip):
                    print("\n🎉 [ERFOLG] Display ist wieder online mit der neuen Firmware!")
                    return
                time.sleep(1)

    except urllib.error.HTTPError as e:
        print(f"[!] HTTP Fehler beim Flashen: {e.code} - {e.read().decode()}")
        sys.exit(1)
    except Exception as e:
        print(f"[!] Fehler: {e}")
        sys.exit(1)

def main():
    parser = argparse.ArgumentParser(description="VESC Hyper Display Wireless OTA Flasher")
    parser.add_argument("--ip", default=DEFAULT_IP, help=f"Display IP oder Hostname (Standard: {DEFAULT_IP} oder vesc-dash.local)")
    parser.add_argument("--board", choices=["43", "70", "4.3", "7", "7.0"], default="43", help="Display-Variante: 43 (4.3 Zoll) oder 70 (7.0 Zoll, Standard: 43)")
    parser.add_argument("--env", default=None, help="Explizites PlatformIO Environment (z.B. waveshare-esp32-s3-touch-lcd-43 oder waveshare-esp32-s3-touch-lcd-7)")
    parser.add_argument("--bin", default=None, help="Expliziter Pfad zur firmware.bin (Standard: .pio/build/<env>/firmware.bin)")
    parser.add_argument("--no-build", action="store_true", help="Build-Schritt ueberspringen")
    parser.add_argument("--status", action="store_true", help="Nur Status abfragen")
    args = parser.parse_args()

    chosen_env, bin_path = resolve_target(args.board, args.env, args.bin)

    if args.status:
        if not check_status(args.ip):
            print(f"[!] Display unter http://{args.ip} nicht erreichbar.")
            sys.exit(1)
        sys.exit(0)

    if not args.no_build:
        build_firmware(chosen_env)

    print(f"[*] Pruefe Verbindung zu {args.ip}...")
    if not check_status(args.ip):
        print(f"\n[!] Display unter http://{args.ip} nicht erreichbar!")
        print("    1. Verbinde dein Geraet mit dem WLAN: 'VESC-DASH-AP'")
        print("    2. Stelle sicher, dass auf dem Display der WiFi-Bridge Modus aktiv ist.")
        print(f"    3. Alternativ die IP per --ip <IP> angeben (oder vesc-dash.local).\n")
        sys.exit(1)

    upload_firmware(args.ip, bin_path, chosen_env)

if __name__ == "__main__":
    main()
