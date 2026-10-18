#pragma once
// User configuration: ~/.config/canopy/canopy.conf, "key = value" lines, '#' comments.
#include "cell.h"
#include <array>
#include <string>
#include <cstdint>

namespace canopy {

struct RGB8 { uint8_t r = 0, g = 0, b = 0; };

struct Theme {
    RGB8 foreground{0xd8, 0xdc, 0xe4};
    RGB8 background{0x12, 0x14, 0x18};
    RGB8 cursor{0xff, 0xcc, 0x66};
    RGB8 selection{0x3b, 0x5b, 0x8a};
    std::array<RGB8, 16> ansi{};      // filled with defaults by Config()
    // UI chrome
    RGB8 chrome_bg{0x0c, 0x0e, 0x11};
    RGB8 chrome_fg{0xa0, 0xa8, 0xb4};
    RGB8 accent{0x5e, 0xa0, 0xff};
    RGB8 ok{0x58, 0xc3, 0x7a};
    RGB8 error{0xe5, 0x5a, 0x5a};
    RGB8 warning{0xe8, 0xb3, 0x4a};
    RGB8 canvas_bg{0x08, 0x09, 0x0b};
};

struct Config {
    std::string font_family = "DejaVu Sans Mono";   // fontconfig family; App tries a fallback chain
    std::string font_family_bold;                  // empty => synthesize/auto
    float font_size_px = 15.0f;
    int cell_padding_x = 0, cell_padding_y = 1;    // extra pixels per cell
    int default_cols = 110, default_rows = 32;
    int scrollback = 20000;
    std::string shell;                             // empty => $SHELL
    bool login_shell = false;
    bool cursor_blink = true;
    float cursor_blink_hz = 1.2f;
    std::string cursor_style = "block";            // block|bar|underline
    bool audible_bell = false, visual_bell = true;
    bool shell_integration = true;
    bool structured_output = true;
    bool sidebar_visible = true;
    int sidebar_width = 240;
    bool animations = true;
    float animation_speed = 1.0f;
    int window_width = 1600, window_height = 1000;
    // Peek rail (miniatures of the other terminals) and AI summaries
    bool peek_visible = true;
    int peek_width = 330;
    bool ai_summaries = true;
    std::string ai_backend = "auto";            // auto | api | claude | local
    std::string ai_model = "claude-opus-5-5";   // Messages API model (api backend)
    std::string ai_cli_model;                   // --model for the claude CLI backend (empty = CLI default)
    int ai_min_interval = 20;                   // seconds between summaries of the same terminal
    Theme theme;
    std::string config_path;                       // where it was loaded from (may be empty)

    Config();                                      // fills defaults (ansi palette etc.)
    static std::string default_path();
    bool load(const std::string& path = "", std::string* err = nullptr);
    // Resolve a cell color against the theme (bold brightening applied by caller if desired).
    RGB8 resolve(const Color& c, bool is_fg) const;
    static RGB8 index_to_rgb(uint8_t idx, const Theme& t);  // 0-15 theme, 16-255 standard cube/gray
    static bool parse_hex(const std::string& s, RGB8* out);
};

} // namespace canopy
