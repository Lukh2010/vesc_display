#!/bin/bash
set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "$SCRIPT_DIR/../.." && pwd)"

cd "$ROOT_DIR"

LGFX_SRC_DIR=$(find .pio/libdeps -name "LovyanGFX" -type d | head -n 1)/src
if [ ! -d "$LGFX_SRC_DIR" ]; then
    echo "LovyanGFX not found in .pio/libdeps, installing via platformio..."
    pio pkg install --library "lovyan03/LovyanGFX@^1.1.16"
    LGFX_SRC_DIR=$(find .pio/libdeps -name "LovyanGFX" -type d | head -n 1)/src
fi

echo "=== Compiling VESC Display Simulator (800x480) ==="
g++ -std=c++17 -O2 -DLGFX_LINUX_FB -DNATIVE_SIMULATOR \
  -Itools/simulator/include \
  -Iinclude \
  -I"$LGFX_SRC_DIR" \
  src/dash_renderer.cpp \
  src/settings_manager.cpp \
  src/battery_manager.cpp \
  src/buttons.cpp \
  src/mode_manager.cpp \
  src/fault_manager.cpp \
  tools/simulator/sim_main.cpp \
  "$LGFX_SRC_DIR/lgfx/v1/LGFXBase.cpp" \
  "$LGFX_SRC_DIR/lgfx/v1/lgfx_fonts.cpp" \
  "$LGFX_SRC_DIR/lgfx/v1/lgfx_v1.cpp" \
  "$LGFX_SRC_DIR/lgfx/v1/lgfx_v1_panel.cpp" \
  "$LGFX_SRC_DIR/lgfx/v1/lgfx_v1_platforms.cpp" \
  "$LGFX_SRC_DIR/lgfx/Fonts/IPA/lgfx_font_japan.c" \
  "$LGFX_SRC_DIR/lgfx/Fonts/efont/lgfx_efont_cn.c" \
  "$LGFX_SRC_DIR/lgfx/Fonts/efont/lgfx_efont_ja.c" \
  "$LGFX_SRC_DIR/lgfx/Fonts/efont/lgfx_efont_kr.c" \
  "$LGFX_SRC_DIR/lgfx/Fonts/efont/lgfx_efont_tw.c" \
  "$LGFX_SRC_DIR/lgfx/utility/lgfx_miniz.c" \
  "$LGFX_SRC_DIR/lgfx/utility/lgfx_pngle.c" \
  "$LGFX_SRC_DIR/lgfx/utility/lgfx_qoi.c" \
  "$LGFX_SRC_DIR/lgfx/utility/lgfx_qrcode.c" \
  "$LGFX_SRC_DIR/lgfx/utility/lgfx_tjpgd.c" \
  -o tools/simulator/simulator

echo "=== Compiling VESC Display Simulator Library (libdash_sim.so) ==="
g++ -std=c++17 -O2 -fPIC -shared -DLGFX_LINUX_FB -DNATIVE_SIMULATOR \
  -Itools/simulator/include \
  -Iinclude \
  -I"$LGFX_SRC_DIR" \
  src/dash_renderer.cpp \
  src/settings_manager.cpp \
  src/battery_manager.cpp \
  src/buttons.cpp \
  src/mode_manager.cpp \
  src/fault_manager.cpp \
  tools/simulator/sim_backend.cpp \
  "$LGFX_SRC_DIR/lgfx/v1/LGFXBase.cpp" \
  "$LGFX_SRC_DIR/lgfx/v1/lgfx_fonts.cpp" \
  "$LGFX_SRC_DIR/lgfx/v1/lgfx_v1.cpp" \
  "$LGFX_SRC_DIR/lgfx/v1/lgfx_v1_panel.cpp" \
  "$LGFX_SRC_DIR/lgfx/v1/lgfx_v1_platforms.cpp" \
  "$LGFX_SRC_DIR/lgfx/Fonts/IPA/lgfx_font_japan.c" \
  "$LGFX_SRC_DIR/lgfx/Fonts/efont/lgfx_efont_cn.c" \
  "$LGFX_SRC_DIR/lgfx/Fonts/efont/lgfx_efont_ja.c" \
  "$LGFX_SRC_DIR/lgfx/Fonts/efont/lgfx_efont_kr.c" \
  "$LGFX_SRC_DIR/lgfx/Fonts/efont/lgfx_efont_tw.c" \
  "$LGFX_SRC_DIR/lgfx/utility/lgfx_miniz.c" \
  "$LGFX_SRC_DIR/lgfx/utility/lgfx_pngle.c" \
  "$LGFX_SRC_DIR/lgfx/utility/lgfx_qoi.c" \
  "$LGFX_SRC_DIR/lgfx/utility/lgfx_qrcode.c" \
  "$LGFX_SRC_DIR/lgfx/utility/lgfx_tjpgd.c" \
  -o tools/simulator/libdash_sim.so

echo "=== Running Scenarios ==="
mkdir -p tools/simulator/screenshots
./tools/simulator/simulator

echo "=== Converting to PNG ==="
python3 -c '
import glob
from PIL import Image
for f in glob.glob("tools/simulator/screenshots/*.bmp"):
    im = Image.open(f)
    im.save(f.replace(".bmp", ".png"))
'
echo "=== Complete! PNGs available in tools/simulator/screenshots/ ==="
