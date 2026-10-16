#pragma once
// Shared cell/color definitions. No dependencies. Used by core, gfx and ui.
#include <cstdint>
#include <vector>
#include <string>

namespace canopy {

struct Color {
    enum Kind : uint8_t { Default = 0, Indexed = 1, Rgb = 2 };
    Kind kind = Default;
    uint8_t idx = 0;        // valid when kind == Indexed (0..255)
    uint8_t r = 0, g = 0, b = 0;  // valid when kind == Rgb

    static Color def() { return {}; }
    static Color indexed(uint8_t i) { Color c; c.kind = Indexed; c.idx = i; return c; }
    static Color rgb(uint8_t r, uint8_t g, uint8_t b) { Color c; c.kind = Rgb; c.r = r; c.g = g; c.b = b; return c; }
    bool operator==(const Color& o) const { return kind == o.kind && idx == o.idx && r == o.r && g == o.g && b == o.b; }
    bool operator!=(const Color& o) const { return !(*this == o); }
};

enum Attr : uint16_t {
    ATTR_BOLD        = 1 << 0,
    ATTR_DIM         = 1 << 1,
    ATTR_ITALIC      = 1 << 2,
    ATTR_UNDERLINE   = 1 << 3,   // underline style in ul_style
    ATTR_BLINK       = 1 << 4,
    ATTR_INVERSE     = 1 << 5,
    ATTR_INVISIBLE   = 1 << 6,
    ATTR_STRIKE      = 1 << 7,
    ATTR_OVERLINE    = 1 << 8,
    ATTR_WIDE        = 1 << 9,   // this cell holds a double-width char; next cell is WIDE_CONT
    ATTR_WIDE_CONT   = 1 << 10,  // continuation of a wide char (no glyph)
};

enum class UnderlineStyle : uint8_t { None = 0, Single = 1, Double = 2, Curly = 3, Dotted = 4, Dashed = 5 };

struct Cell {
    char32_t cp = U' ';          // base code point (0 means empty/erased, render as space)
    uint32_t combining = 0;      // 0 or index+1 into Line::combining table
    uint32_t hyperlink = 0;      // 0 or hyperlink id (see Terminal::hyperlink(id))
    Color fg, bg, ul;            // ul = underline color (Default means use fg)
    uint16_t attrs = 0;
    UnderlineStyle ul_style = UnderlineStyle::None;

    bool is_blank() const { return (cp == U' ' || cp == 0) && !(attrs & ATTR_WIDE_CONT); }
};

struct Line {
    std::vector<Cell> cells;
    bool wrapped = false;                 // true if this line soft-wrapped into the next (for selection/text)
    std::vector<std::u32string> combining; // combining sequences referenced by Cell::combining (index+1)

    int width() const { return (int)cells.size(); }
    // Plain text of the line (trailing blanks trimmed), UTF-8.
    std::string text() const;
    std::string text(int col_begin, int col_end) const; // [begin,end)
};

} // namespace canopy
