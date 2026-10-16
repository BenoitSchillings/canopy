#pragma once
// Batched 2D renderer: one shader, one VBO, draw order preserved. World->screen transform
// (uniform scale + translation) so the same drawing code works for the zoomable canvas.
#include "font.h"
#include <cstdint>
#include <string_view>
#include <vector>

namespace canopy::gfx {

struct RGBA { float r = 0, g = 0, b = 0, a = 1; };
inline RGBA rgba8(uint8_t r, uint8_t g, uint8_t b, float a = 1.f) { return {r / 255.f, g / 255.f, b / 255.f, a}; }
struct Rect { float x = 0, y = 0, w = 0, h = 0;
    bool contains(float px, float py) const { return px >= x && py >= y && px < x + w && py < y + h; }
    Rect inset(float d) const { return {x + d, y + d, w - 2 * d, h - 2 * d}; }
};

struct Transform { float scale = 1.f; float tx = 0.f, ty = 0.f;   // screen = world * scale + t
    float sx(float wx) const { return wx * scale + tx; }
    float sy(float wy) const { return wy * scale + ty; }
    float wx(float sx_) const { return (sx_ - tx) / scale; }
    float wy(float sy_) const { return (sy_ - ty) / scale; }
};

class Renderer {
public:
    Renderer();
    ~Renderer();
    bool init(std::string* err = nullptr);   // after GL context + gl_load()

    void begin_frame(int fb_w, int fb_h, RGBA clear);
    void end_frame();                         // flushes
    void flush();

    // Transform applies to all subsequent world-space coordinates. Clip rects are in world space too.
    void set_transform(const Transform& t);
    const Transform& transform() const { return xf_; }
    void push_clip(Rect world_rect);          // intersects with current clip
    void pop_clip();

    // Primitives (world space)
    void fill_rect(Rect r, RGBA c);
    void fill_rect_gradient(Rect r, RGBA top, RGBA bottom);
    void stroke_rect(Rect r, RGBA c, float thickness = 1.f);
    void rounded_rect(Rect r, float radius, RGBA fill);
    void rounded_rect_stroke(Rect r, float radius, RGBA c, float thickness = 1.f);
    void line(float x0, float y0, float x1, float y1, RGBA c, float thickness = 1.f);
    void circle(float cx, float cy, float radius, RGBA c);
    // Curly/dotted/dashed underline helpers are built from the above by the caller.

    // Glyph from a Font atlas, positioned with pen at (x, baseline_y). Scale 1 => native pixels.
    void draw_glyph(Font& font, const Glyph& g, float x, float baseline_y, RGBA color, float scale = 1.f);
    // UI text (UTF-8), returns advance width in world units. Does not wrap.
    float draw_text(Font& font, float x, float baseline_y, std::string_view utf8, RGBA color, GlyphStyle style = STYLE_REGULAR, float scale = 1.f);
    float measure_text(Font& font, std::string_view utf8, GlyphStyle style = STYLE_REGULAR, float scale = 1.f);
    // Procedural box drawing char into a cell rect.
    void draw_box_char(char32_t cp, Rect cell, RGBA color, float thickness);

    // Stats
    int draw_calls() const { return draw_calls_; }
    int quads() const { return quads_; }

private:
    struct Impl;
    Impl* impl_;
    Transform xf_;
    int draw_calls_ = 0, quads_ = 0;
};

} // namespace canopy::gfx
