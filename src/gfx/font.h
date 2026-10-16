#pragma once
// FreeType glyph rasterization into a single RGBA atlas, with fontconfig-based fallback.
#include <cstdint>
#include <string>
#include <vector>
#include <unordered_map>
#include <memory>

namespace canopy::gfx {

enum GlyphStyle : uint8_t { STYLE_REGULAR = 0, STYLE_BOLD = 1, STYLE_ITALIC = 2, STYLE_BOLD_ITALIC = 3 };

struct Glyph {
    // Atlas rectangle in pixels and normalized UVs.
    int ax = 0, ay = 0, aw = 0, ah = 0;
    float u0 = 0, v0 = 0, u1 = 0, v1 = 0;
    int bearing_x = 0, bearing_y = 0;   // offset from pen (baseline) to top-left of bitmap
    float advance = 0;                  // horizontal advance in pixels
    bool color = false;                 // true for color (emoji) bitmaps: sample RGBA, not alpha mask
    bool valid = false;                 // false => .notdef
};

struct FontMetrics {
    int cell_w = 0, cell_h = 0;         // grid cell size in pixels (includes config padding)
    int ascent = 0;                     // baseline offset from cell top
    int descent = 0;
    int underline_pos = 0, underline_thickness = 1;   // y offset from the cell top
    int strike_pos = 0;                                // y offset from the cell top
};

class FontAtlas;

// A font set: primary monospace family at a pixel size, with bold/italic variants and
// fallback faces found through fontconfig (for CJK, symbols, emoji).
class Font {
public:
    Font();
    ~Font();
    // family: fontconfig family name (empty => "monospace"). size_px: nominal pixel size.
    bool load(const std::string& family, float size_px, int pad_x, int pad_y, std::string* err = nullptr);
    // Change size (rebuilds faces, clears atlas).
    bool set_size(float size_px);
    float size_px() const { return size_px_; }
    const FontMetrics& metrics() const { return metrics_; }

    // Rasterize-on-demand. Thread-unsafe. Returns a glyph (possibly .notdef with valid=false).
    const Glyph& glyph(char32_t cp, GlyphStyle style = STYLE_REGULAR);
    // Draws box-drawing/block characters (U+2500..U+259F, U+E0B0.. powerline) procedurally for crispness.
    static bool is_box_drawing(char32_t cp);

    // Atlas texture (GL texture id, RGBA8, premultiplied alpha for color glyphs, alpha-only in .a for masks).
    unsigned atlas_texture() const;
    int atlas_size() const;
    // True if the atlas was (re)uploaded since last call; renderer should rebind.
    bool take_atlas_dirty();
    // Resolve a family through fontconfig to a file path (+ index). Public for diagnostics.
    static bool resolve_family(const std::string& family, bool bold, bool italic, std::string* path, int* index);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    FontMetrics metrics_;
    float size_px_ = 0;
};

} // namespace canopy::gfx
