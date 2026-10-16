#pragma once
// Small shared helpers for the UI layer.
#include "core/config.h"
#include "gfx/renderer.h"
#include <algorithm>
#include <cmath>
#include <string>

namespace canopy::ui {

using gfx::RGBA;
using gfx::Rect;

inline RGBA col(const RGB8& c, float a = 1.f) { return gfx::rgba8(c.r, c.g, c.b, a); }
inline RGBA mix(RGBA a, RGBA b, float t) { return {a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t, a.b + (b.b - a.b) * t, a.a + (b.a - a.a) * t}; }
inline RGBA alpha(RGBA c, float a) { c.a = a; return c; }
inline float clampf(float v, float lo, float hi) { return std::max(lo, std::min(hi, v)); }
inline float lerp(float a, float b, float t) { return a + (b - a) * t; }
// Exponential smoothing step toward target (frame-rate independent).
inline float approach(float cur, float target, float dt, float speed) {
    return target + (cur - target) * std::exp(-dt * speed);
}
inline std::string fmt_duration(double s) {
    char buf[32];
    if (s < 0.001) return "0ms";
    if (s < 1.0) snprintf(buf, sizeof buf, "%dms", (int)(s * 1000));
    else if (s < 60) snprintf(buf, sizeof buf, "%.1fs", s);
    else if (s < 3600) snprintf(buf, sizeof buf, "%dm%02ds", (int)s / 60, (int)s % 60);
    else snprintf(buf, sizeof buf, "%dh%02dm", (int)s / 3600, ((int)s % 3600) / 60);
    return buf;
}
// Shorten a path for display: ~ for home, collapse middle when too long.
std::string pretty_path(const std::string& path, size_t max_len = 48);
std::string basename_of(const std::string& path);
// Truncate UTF-8 string to at most n code points with an ellipsis.
std::string ellipsize(const std::string& s, size_t n);
size_t utf8_length(std::string_view s);
// Simple case-insensitive subsequence fuzzy match; returns score (>0) or 0 for no match.
int fuzzy_score(std::string_view needle, std::string_view hay);
std::string to_lower(std::string s);

} // namespace canopy::ui

namespace canopy::ui {
std::u32string utf8_decode(std::string_view s);
std::string utf8_encode(char32_t cp);
void utf8_append(std::string& out, char32_t cp);
bool is_wide_cp(char32_t cp);   // rough East-Asian-wide heuristic for UI-made lines
std::string shell_quote(const std::string& s);
std::string home_dir();
bool write_file(const std::string& path, const std::string& data);
}
