#pragma once

#include "dash_renderer.h"
#include "buttons.h"
#include "vesc_uart.h"
#include "fault_manager.h"
#include "mode_manager.h"
#include "ota_manager.h"
#include "tcs_tpms_manager.h"
#include <math.h>
#include <algorithm>

// Base Neutral Constants (used for pure-black boot screen & fallback math)
static constexpr uint16_t COLOR_BLACK = 0x0000;
static constexpr uint16_t COLOR_WHITE = 0xFFFF;

// ==============================================================================
// Shared Color & Mode Helpers (Header-only inline for split screen modules)
// ==============================================================================
inline uint16_t blendRgb565(uint16_t c1, uint16_t c2, uint8_t alpha) {
    uint32_t r1 = (c1 >> 11) & 0x1F;
    uint32_t g1 = (c1 >> 5)  & 0x3F;
    uint32_t b1 =  c1        & 0x1F;
    uint32_t r2 = (c2 >> 11) & 0x1F;
    uint32_t g2 = (c2 >> 5)  & 0x3F;
    uint32_t b2 =  c2        & 0x1F;
    uint32_t inv = 255u - alpha;
    uint32_t r = (r1 * inv + r2 * alpha) / 255u;
    uint32_t g = (g1 * inv + g2 * alpha) / 255u;
    uint32_t b = (b1 * inv + b2 * alpha) / 255u;
    return (uint16_t)((r << 11) | (g << 5) | b);
}

inline uint16_t onPrimaryFg(const ColorThemePalette &pal) {
    return pal.is_light ? pal.surface : pal.bg;
}

inline uint16_t getModeColor(int mode_idx, const ColorThemePalette &pal) {
    switch (mode_idx % 3) {
        case 0:  return pal.accent_ok;     // ECO
        case 1:  return pal.primary;       // SPORT
        default: return pal.accent_danger; // RACE
    }
}

inline uint16_t getTempColorTheme(float temp, const ColorThemePalette &pal) {
    if (temp >= 90.0f) return pal.accent_danger;
    if (temp >= 75.0f) return pal.accent_warn;
    return pal.text;
}
