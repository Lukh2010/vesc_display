#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
cd "$ROOT_DIR"

echo "=================================================="
echo "  VESC Display Local Quality Check (check_all.sh) "
echo "=================================================="

echo "=== [1/5] Python-Syntax & WLED-Routen Prüfung ==="
python3 -m py_compile tools/simulator/live_demo.py tools/ota_flash.py
grep -q '"/wled"' tools/simulator/live_demo.py || (echo "ERROR: /wled Route fehlt in live_demo.py" && exit 1)
grep -q '"/json/state"' tools/simulator/live_demo.py || (echo "ERROR: /json/state Route fehlt in live_demo.py" && exit 1)
grep -q '"/json/si"' tools/simulator/live_demo.py || (echo "ERROR: /json/si Route fehlt in live_demo.py" && exit 1)
echo "  -> OK: Python-Dateien syntaktisch fehlerfrei und WLED-Routen vollständig."

echo "=== [2/5] PlatformIO Dual-Target Konfigurationsprüfung ==="
grep -q "\[env:waveshare-esp32-s3-touch-lcd-43\]" platformio.ini || (echo "ERROR: Target 43 fehlt in platformio.ini" && exit 1)
grep -q "\[env:waveshare-esp32-s3-touch-lcd-7\]" platformio.ini || (echo "ERROR: Target 7 fehlt in platformio.ini" && exit 1)
echo "  -> OK: Beide Hardware-Targets (4.3\" und 7.0\") in platformio.ini definiert."

echo "=== [3/5] Simulator-Build (--build-only) ==="
bash tools/simulator/build_and_run.sh --build-only
echo "  -> OK: Simulator erfolgreich kompiliert."

echo "=== [4/5] Simulator-Regressionstests ausführen ==="
SIM_OUT=$(./tools/simulator/simulator)
if echo "$SIM_OUT" | grep -q "\[SUCCESS\]"; then
    echo "$SIM_OUT" | grep -E "\[PASS\]|\[SUCCESS\]"
    echo "  -> OK: Alle Simulator-Regressionstests erfolgreich bestanden."
else
    echo "$SIM_OUT"
    echo "ERROR: [SUCCESS] nicht in Simulator-Ausgabe gefunden!"
    exit 1
fi

echo "=== [5/5] Git Diff Check (Whitespace & Merge-Konflikte) ==="
git diff --check
echo "  -> OK: Keine Whitespace- oder Merge-Konflikt-Fehler vorhanden."

echo ""
echo "=================================================="
echo "  ALLE QUALITÄTSPRÜFUNGEN ERFOLGREICH (100% OK)   "
echo "=================================================="
