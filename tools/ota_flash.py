#!/usr/bin/env python3
"""
VESC Hyper Display - Wireless OTA Flasher
Uploads compiled firmware.bin to Waveshare ESP32-S3 over WiFi SoftAP or Local LAN.

Usage:
    python3 tools/ota_flash.py [--ip 192.168.4.1] [--no-build] [--status]
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
DEFAULT_BIN = ".pio/build/waveshare-esp32-s3-touch-lcd-43/firmware.bin"

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
    except Exception as e:
        return False
    return False

def build_firmware():
    print("[*] Kompiliere PlatformIO Firmware...")
    cmd = ["pio", "run", "-e", "waveshare-esp32-s3-touch-lcd-43"]
    res = subprocess.run(cmd)
    if res.returncode != 0:
        print("[!] Build fehlgeschlagen. Abbruch.")
        sys.exit(1)
    print("[✓] Firmware erfolgreich gebaut.")

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

def upload_firmware(ip, bin_path):
    if not os.path.exists(bin_path):
        print(f"[!] Firmware-Datei nicht gefunden: {bin_path}")
        sys.exit(1)

    filesize = os.path.getsize(bin_path)
    filename = os.path.basename(bin_path)
    url = f"http://{ip}/update"

    print(f"\n⚡ Starte kabellosen OTA-Flash: {filename} ({filesize / 1024:.1f} KB) -> http://{ip}/update")

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
    parser.add_argument("--ip", default=DEFAULT_IP, help=f"Display IP (Standard: {DEFAULT_IP})")
    parser.add_argument("--bin", default=DEFAULT_BIN, help="Pfad zur firmware.bin")
    parser.add_argument("--no-build", action="store_true", help="Build-Schritt ueberspringen")
    parser.add_argument("--status", action="store_true", help="Nur Status abfragen")
    args = parser.parse_args()

    if args.status:
        if not check_status(args.ip):
            print(f"[!] Display unter http://{args.ip} nicht erreichbar.")
            sys.exit(1)
        sys.exit(0)

    if not args.no_build:
        build_firmware()

    print(f"[*] Pruefe Verbindung zu {args.ip}...")
    if not check_status(args.ip):
        print(f"\n[!] Display unter http://{args.ip} nicht erreichbar!")
        print("    1. Verbinde dein Geraet mit dem WLAN: 'VESC-DASH-AP'")
        print("    2. Stelle sicher, dass auf dem Display der WiFi-Bridge Modus aktiv ist.")
        print(f"    3. Alternativ die IP per --ip <IP> angeben.\n")
        sys.exit(1)

    upload_firmware(args.ip, args.bin)

if __name__ == "__main__":
    main()
