// gfxdemo: standalone exercise of the Canopy gfx layer (window, font, renderer).
// Renders a fake 80x24 terminal grid at scale 1, miniatures at 0.25/0.5, a 2.0 zoom,
// UI chrome, and reports frame statistics in the title and on stdout.
// Keys: Esc quits, Ctrl+wheel zooms around the mouse, left-drag pans, S toggles stress mode.
//   --stress     start with many extra grid copies (~100k quads) for batching measurements
//   --novsync    disable vsync
//   --frames N   exit after N frames (prints stats)
#include "gfx/window.h"
#include "gfx/renderer.h"
#include "gfx/font.h"
#include "gfx/gl.h"

#include <SDL_mouse.h>
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <string>
#include <vector>

using namespace canopy;
using namespace canopy::gfx;

namespace {

constexpr int COLS = 80, ROWS = 24;

enum : uint16_t { A_BOLD = 1, A_ITALIC = 2, A_UNDER = 4, A_REVERSE = 8, A_STRIKE = 16, A_CURLY = 32, A_DOUBLE_UL = 64, A_DIM = 128 };

struct Cell {
    char32_t cp = U' ';
    std::u32string combining;
    RGBA fg{0.85f, 0.85f, 0.85f, 1.f};
    RGBA bg{0, 0, 0, 0};   // alpha 0 => default background
    uint16_t attrs = 0;
    bool wide_cont = false;
};

struct Grid {
    std::vector<Cell> cells;
    Grid() : cells(COLS * ROWS) {}
    Cell& at(int c, int r) { return cells[r * COLS + c]; }
};

const RGBA kPalette[16] = {
    rgba8(40, 42, 54), rgba8(255, 85, 85), rgba8(80, 250, 123), rgba8(241, 250, 140), rgba8(98, 114, 164), rgba8(255, 121, 198), rgba8(139, 233, 253), rgba8(248, 248, 242),
    rgba8(98, 114, 164), rgba8(255, 110, 110), rgba8(105, 255, 148), rgba8(255, 255, 165), rgba8(214, 172, 255), rgba8(255, 146, 223), rgba8(164, 255, 255), rgba8(255, 255, 255),
};

std::u32string u32(const char* utf8) {
    std::u32string out;
    const unsigned char* s = (const unsigned char*)utf8;
    while (*s) {
        unsigned char c = *s++;
        char32_t cp;
        int n = 0;
        if (c < 0x80) cp = c;
        else if (c >= 0xF0) { cp = c & 7; n = 3; }
        else if (c >= 0xE0) { cp = c & 15; n = 2; }
        else { cp = c & 31; n = 1; }
        for (int k = 0; k < n && *s; ++k) cp = (cp << 6) | (*s++ & 0x3F);
        out.push_back(cp);
    }
    return out;
}

bool is_wide(char32_t cp) {
    return (cp >= 0x1100 && cp <= 0x115F) || (cp >= 0x2E80 && cp <= 0xA4CF) || (cp >= 0xAC00 && cp <= 0xD7A3) ||
           (cp >= 0xF900 && cp <= 0xFAFF) || (cp >= 0xFE30 && cp <= 0xFE4F) || (cp >= 0xFF00 && cp <= 0xFF60) ||
           (cp >= 0xFFE0 && cp <= 0xFFE6) || (cp >= 0x1F300 && cp <= 0x1FAFF) || (cp >= 0x20000 && cp <= 0x3FFFD);
}

bool is_combining(char32_t cp) { return (cp >= 0x0300 && cp <= 0x036F) || (cp >= 0xFE20 && cp <= 0xFE2F) || (cp >= 0xFE00 && cp <= 0xFE0F) || cp == 0x200D; }

// Writes utf8 text into the grid at (c, r); returns the column after the text.
int put(Grid& g, int c, int r, const char* text, RGBA fg, uint16_t attrs = 0, RGBA bg = RGBA{0, 0, 0, 0}) {
    for (char32_t cp : u32(text)) {
        if (is_combining(cp)) {
            if (c > 0) g.at(c - 1, r).combining.push_back(cp);
            continue;
        }
        if (c >= COLS) break;
        Cell& cell = g.at(c, r);
        cell.cp = cp; cell.fg = fg; cell.bg = bg; cell.attrs = attrs; cell.wide_cont = false;
        ++c;
        if (is_wide(cp) && c < COLS) {
            Cell& cont = g.at(c, r);
            cont.cp = 0; cont.fg = fg; cont.bg = bg; cont.attrs = attrs; cont.wide_cont = true;
            ++c;
        }
    }
    return c;
}

void build_grid(Grid& g) {
    RGBA fg = rgba8(248, 248, 242), dim = rgba8(140, 140, 150);
    // Title bar (reverse)
    for (int c = 0; c < COLS; ++c) g.at(c, 0).attrs = A_REVERSE, g.at(c, 0).fg = fg;
    put(g, 1, 0, " canopy gfxdemo — fake 80x24 terminal: colors, attrs, box drawing, CJK, emoji ", fg, A_REVERSE);
    // Shell-ish lines
    put(g, 0, 1, "benoit@canopy:~/terminal$ ", kPalette[2]);
    put(g, 26, 1, "ls -la src/gfx && make -j12", fg);
    put(g, 0, 2, "drwxrwxr-x 2 benoit benoit  4096 Oct 15 17:31 .", kPalette[4]);
    put(g, 0, 3, "-rw-rw-r-- 1 benoit benoit 18213 Oct 15 18:02 renderer.cpp", fg);
    put(g, 0, 4, "-rw-rw-r-- 1 benoit benoit 15102 Oct 15 18:02 font.cpp", fg);
    put(g, 0, 5, "[ 50%] Building CXX object CMakeFiles/canopy_gfx.dir/src/gfx/font.cpp.o", dim);
    put(g, 0, 6, "warning: unused variable 'x' [-Wunused-variable]", kPalette[3]);
    put(g, 0, 7, "error: no member named 'frob' in 'canopy::gfx::Font'", kPalette[9], A_BOLD);

    // Attribute samples
    int c = put(g, 0, 9, "bold ", fg, A_BOLD);
    c = put(g, c, 9, "italic ", fg, A_ITALIC);
    c = put(g, c, 9, "bold-italic ", fg, A_BOLD | A_ITALIC);
    c = put(g, c, 9, "underline ", fg, A_UNDER);
    c = put(g, c, 9, "double ", fg, A_DOUBLE_UL);
    c = put(g, c, 9, "curly ", kPalette[13], A_CURLY);
    c = put(g, c, 9, "strike ", fg, A_STRIKE);
    c = put(g, c, 9, "reverse ", fg, A_REVERSE);
    c = put(g, c, 9, "dim", fg, A_DIM);
    // 16 colors fg + bg
    for (int i = 0; i < 16; ++i) {
        char buf[8];
        std::snprintf(buf, sizeof buf, "%2d ", i);
        put(g, i * 3, 10, buf, kPalette[i]);
        put(g, i * 3, 11, "   ", kPalette[0], 0, kPalette[i]);
    }
    for (int i = 0; i < 32; ++i) {   // truecolor ramp
        float t = i / 31.f;
        put(g, 48 + i, 10, " ", fg, 0, RGBA{t, 0.3f, 1.f - t, 1.f});
        put(g, 48 + i, 11, "▓", RGBA{1.f - t, t, 0.5f, 1.f});
    }

    // Box drawing frame (light), with double and heavy inner lines, tees and crosses.
    int fx = 0, fy = 13, fw = 40, fh = 9;
    for (int x = 1; x < fw - 1; ++x) { put(g, fx + x, fy, "─", kPalette[6]); put(g, fx + x, fy + fh - 1, "─", kPalette[6]); }
    for (int y = 1; y < fh - 1; ++y) { put(g, fx, fy + y, "│", kPalette[6]); put(g, fx + fw - 1, fy + y, "│", kPalette[6]); }
    put(g, fx, fy, "┌", kPalette[6]); put(g, fx + fw - 1, fy, "┐", kPalette[6]);
    put(g, fx, fy + fh - 1, "└", kPalette[6]); put(g, fx + fw - 1, fy + fh - 1, "┘", kPalette[6]);
    put(g, fx, fy + 3, "├──────────┬────────────┬──────────────┤", kPalette[6]);
    put(g, fx, fy + 4, "│ name     │ size       │ status       │", kPalette[6]);
    put(g, fx, fy + 5, "├──────────┼────────────┼──────────────┤", kPalette[6]);
    put(g, fx, fy + 6, "│ font.cpp │ 15102      │ ok           │", kPalette[6]);
    put(g, fx + 2, fy + 1, "Frame title", fg, A_BOLD);
    put(g, fx + 2, fy + 2, "light ─│┼  heavy ━┃╋  double ═║╬  mix ╪╫", fg);
    put(g, fx + 2, fy + 7, "╔══╦══╗ ┏━━┳━━┓ ╭──┬──╮ ┄┄┅┅┆┇╌╍ ╱╲╳", kPalette[3]);
    // Block elements and powerline
    put(g, 42, 13, "blocks: █▓▒░ ▀▄ ▌▐ ▖▗▘▝ ▙▟▛▜ ▁▂▃▄▅▆▇█", fg);
    put(g, 42, 14, "▏▎▍▌▋▊▉█ ▔▕", fg);
    c = put(g, 42, 15, " ~/terminal ", kPalette[0], 0, kPalette[4]);
    c = put(g, c, 15, "", kPalette[4], 0, kPalette[2]);
    c = put(g, c, 15, " main ", kPalette[0], 0, kPalette[2]);
    c = put(g, c, 15, "", kPalette[2], 0, kPalette[0]);
    c = put(g, c, 15, "  ", kPalette[7]);
    c = put(g, c, 15, "", kPalette[5], 0, kPalette[0]);
    c = put(g, c, 15, " ok ", kPalette[0], 0, kPalette[5]);
    c = put(g, c, 15, "", kPalette[5], 0, kPalette[0]);
    c = put(g, c, 15, " ", kPalette[2]);
    c = put(g, c, 15, "", kPalette[3], 0, kPalette[0]);
    c = put(g, c, 15, " ", kPalette[3]);
    put(g, 42, 16, "▲▼ ▲▼ ◢◣◥◤", kPalette[7]);
    // CJK, combining, emoji, symbols
    put(g, 42, 18, "CJK: 日本語テキスト 中文 한국어", fg);
    put(g, 42, 19, "combining: e\xCC\x81 a\xCC\x8A n\xCC\x83 o\xCC\x88 ", fg);
    put(g, 42, 20, "emoji: 😀 🚀 🎉 🐍 ❤️ ✅", fg);
    put(g, 42, 21, "symbols: → ← ↑ ↓ ✓ ✗ ★ ☆ λ π ∑ ∞ ≠ ≤ ≥ ", fg);
    put(g, 0, 22, "benoit@canopy:~/terminal$ ", kPalette[2]);
    put(g, 26, 22, "█", fg);   // cursor
    put(g, 0, 23, "0:bash  1:vim  2:htop*  3:ssh                                    15:04  canopy", kPalette[0], 0, kPalette[6]);
}

struct DrawStats { int cells = 0; };

// Draws the grid with its top-left at world (ox, oy), under the renderer's current transform.
void draw_grid(Renderer& r, Font& font, Grid& g, float ox, float oy, double time, bool with_cursor_blink) {
    const FontMetrics& m = font.metrics();
    const float cw = (float)m.cell_w, ch = (float)m.cell_h;
    RGBA defbg = rgba8(30, 31, 40);
    r.fill_rect({ox, oy, COLS * cw, ROWS * ch}, defbg);
    // Pass 1: backgrounds
    for (int row = 0; row < ROWS; ++row)
        for (int col = 0; col < COLS; ++col) {
            const Cell& cell = g.at(col, row);
            RGBA bg = cell.bg;
            if (cell.attrs & A_REVERSE) bg = cell.fg;
            if (bg.a <= 0.f) continue;
            r.fill_rect({ox + col * cw, oy + row * ch, cw, ch}, bg);
        }
    // Pass 2: glyphs and decorations
    for (int row = 0; row < ROWS; ++row) {
        float y = oy + row * ch;
        float baseline = y + m.ascent;
        for (int col = 0; col < COLS; ++col) {
            const Cell& cell = g.at(col, row);
            if (cell.wide_cont || cell.cp == 0) continue;
            float x = ox + col * cw;
            RGBA fg = cell.fg;
            if (cell.attrs & A_REVERSE) fg = cell.bg.a > 0 ? cell.bg : defbg;
            if (cell.attrs & A_DIM) fg.a *= 0.55f;
            if (cell.cp != U' ') {
                if (Font::is_box_drawing(cell.cp)) {
                    r.draw_box_char(cell.cp, {x, y, cw, ch}, fg, (float)m.underline_thickness);
                } else {
                    GlyphStyle st = (GlyphStyle)(((cell.attrs & A_BOLD) ? STYLE_BOLD : 0) | ((cell.attrs & A_ITALIC) ? STYLE_ITALIC : 0));
                    const Glyph& gl = font.glyph(cell.cp, st);
                    r.draw_glyph(font, gl, x, baseline, fg);
                    for (char32_t cc : cell.combining) r.draw_glyph(font, font.glyph(cc, st), x, baseline, fg);
                }
            }
            float ut = (float)m.underline_thickness;
            float uy = y + m.underline_pos;
            if (cell.attrs & A_UNDER) r.fill_rect({x, uy, cw, ut}, fg);
            if (cell.attrs & A_DOUBLE_UL) { r.fill_rect({x, uy - ut - 1, cw, ut}, fg); r.fill_rect({x, uy + 1, cw, ut}, fg); }
            if (cell.attrs & A_CURLY) {
                // two half-waves per cell built from short lines
                float amp = 1.5f, step = cw / 4.f;
                for (int k = 0; k < 4; ++k) {
                    float x0 = x + k * step, x1 = x0 + step;
                    float y0 = uy + ((k % 2) ? amp : -amp), y1 = uy + ((k % 2) ? -amp : amp);
                    r.line(x0, y0 + ut * 0.5f, x1, y1 + ut * 0.5f, fg, ut);
                }
            }
            if (cell.attrs & A_STRIKE) r.fill_rect({x, y + m.strike_pos, cw, ut}, fg);
        }
    }
    if (with_cursor_blink && (int)(time * 2) % 2 == 0) {
        r.fill_rect({ox + 26 * cw, oy + 22 * ch, cw, ch}, rgba8(248, 248, 242, 0.6f));
    }
}

Transform compose(const Transform& cam, float scale, float tx, float ty) {
    return Transform{cam.scale * scale, cam.sx(tx), cam.sy(ty)};
}

} // namespace

int main(int argc, char** argv) {
    bool stress = false, vsync = true;
    int max_frames = 0;
    for (int i = 1; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--stress")) stress = true;
        else if (!std::strcmp(argv[i], "--novsync")) vsync = false;
        else if (!std::strcmp(argv[i], "--frames") && i + 1 < argc) max_frames = std::atoi(argv[++i]);
    }

    Window win;
    std::string err;
    if (!win.create("gfxdemo", 1400, 900, &err)) {
        std::fprintf(stderr, "window: %s\n", err.c_str());
        return 1;
    }
    win.set_vsync(vsync);
    std::printf("GL_VERSION: %s\n", (const char*)glGetString(GL_VERSION));
    Renderer r;
    if (!r.init(&err)) {
        std::fprintf(stderr, "renderer: %s\n", err.c_str());
        return 1;
    }
    Font font;
    if (!font.load("Source Code Pro", 15.f, 0, 0, &err)) {
        std::fprintf(stderr, "font: Source Code Pro failed (%s), falling back to monospace\n", err.c_str());
        if (!font.load("", 15.f, 0, 0, &err)) { std::fprintf(stderr, "font: %s\n", err.c_str()); return 1; }
    }
    {
        std::string p; int idx = 0;
        if (!Font::resolve_family("Source Code Pro", false, false, &p, &idx)) Font::resolve_family("", false, false, &p, &idx);
        const FontMetrics& m = font.metrics();
        std::printf("font: %s[%d] cell %dx%d ascent %d descent %d underline %d/%d strike %d\n", p.c_str(), idx,
                    m.cell_w, m.cell_h, m.ascent, m.descent, m.underline_pos, m.underline_thickness, m.strike_pos);
    }
    Font ui_font;
    ui_font.load("DejaVu Sans Mono", 13.f, 0, 0);

    Grid grid;
    build_grid(grid);

    Transform cam;                       // canvas camera
    bool dragging = false;
    float drag_x = 0, drag_y = 0, mouse_x = 0, mouse_y = 0;
    bool running = true;
    int frames = 0;
    double last_title = win.now(), fps = 0;
    double cpu_acc = 0, cpu_max = 0, gl_acc = 0;
    int cpu_n = 0;
    std::vector<Event> events;
    const double t0 = win.now();

    while (running) {
        events.clear();
        win.poll(events, vsync ? 1 : 0);
        for (const Event& e : events) {
            switch (e.type) {
            case Event::Quit: running = false; break;
            case Event::Key:
                if (e.key.key == Key::Escape) running = false;
                if (e.key.key == Key::Char && e.key.keysym == 's' && !e.repeat) stress = !stress;
                if (e.key.key == Key::Char && e.key.keysym == '0' && (e.mods & MOD_CTRL)) cam = Transform{};
                if (e.key.key == Key::Char && !e.key.text.empty())
                    std::printf("key: Char keysym=U+%04X text='%s' mods=%u\n", (unsigned)e.key.keysym, e.key.text.c_str(), e.mods);
                break;
            case Event::Text: std::printf("text: '%s'\n", e.key.text.c_str()); break;
            case Event::MouseDown:
                if (e.button == MouseButton::Left) { dragging = true; drag_x = e.x; drag_y = e.y; win.set_cursor(SDL_SYSTEM_CURSOR_SIZEALL); }
                break;
            case Event::MouseUp:
                if (e.button == MouseButton::Left) { dragging = false; win.set_cursor(SDL_SYSTEM_CURSOR_ARROW); }
                break;
            case Event::MouseMove:
                mouse_x = e.x; mouse_y = e.y;
                if (dragging) { cam.tx += e.x - drag_x; cam.ty += e.y - drag_y; drag_x = e.x; drag_y = e.y; }
                break;
            case Event::Wheel:
                if (e.mods & MOD_CTRL) {
                    float f = e.wheel_y > 0 ? 1.1f : 1.f / 1.1f;
                    float ns = cam.scale * f;
                    if (ns < 0.05f) ns = 0.05f;
                    if (ns > 8.f) ns = 8.f;
                    // keep the world point under the mouse fixed
                    float wx = cam.wx(e.x), wy = cam.wy(e.y);
                    cam.scale = ns;
                    cam.tx = e.x - wx * ns;
                    cam.ty = e.y - wy * ns;
                } else {
                    cam.tx += e.wheel_x * 40.f;
                    cam.ty += e.wheel_y * 40.f;
                }
                break;
            case Event::DropFile: std::printf("drop: %s\n", e.path.c_str()); break;
            default: break;
            }
        }

        double now = win.now();
        auto cpu0 = std::chrono::steady_clock::now();
        int fw = win.fb_width(), fh = win.fb_height();
        r.begin_frame(fw, fh, rgba8(18, 18, 24));

        const FontMetrics& m = font.metrics();
        const float gw = COLS * m.cell_w, gh = ROWS * m.cell_h;

        // --- UI chrome in screen space: sidebar strip ---
        r.set_transform(Transform{});
        r.fill_rect_gradient({0, 0, 220, (float)fh}, rgba8(28, 30, 40), rgba8(22, 23, 30));
        r.line(220, 0, 220, (float)fh, rgba8(60, 64, 80), 1.f);
        r.draw_text(ui_font, 16, 30, "Activities", rgba8(230, 230, 240), STYLE_BOLD);
        const char* tree[] = {"▾ terminal", "   ▾ gfx layer", "      renderer", "      font atlas", "   ▸ core vt", "   ▸ tests", "▸ scratch"};
        for (int i = 0; i < 7; ++i) {
            if (i == 2) r.rounded_rect({8, 44.f + i * 24, 204, 22}, 6, rgba8(60, 90, 160, 0.5f));
            r.draw_text(ui_font, 16, 60.f + i * 24, tree[i], i == 2 ? rgba8(255, 255, 255) : rgba8(190, 195, 210));
        }
        r.rounded_rect_stroke({8, 44.f + 2 * 24, 204, 22}, 6, rgba8(120, 160, 255, 0.9f), 1.f);
        r.circle(196, 60.f + 4 * 24 - 4, 4, rgba8(80, 250, 123));
        r.draw_text(ui_font, 16, (float)fh - 16, "Ctrl+wheel zoom, drag pan, S stress", rgba8(120, 125, 140));

        // --- canvas content under the camera ---
        const float panel_x = 240, panel_y = 40;
        auto panel = [&](float x, float y, float scale, const char* label) {
            // rounded panel frame around a pane, drawn in camera space
            r.set_transform(cam);
            Rect frame{x - 8, y - 28, gw * scale + 16, gh * scale + 36};
            r.rounded_rect(frame, 8, rgba8(44, 46, 60));
            r.rounded_rect_stroke(frame, 8, rgba8(90, 95, 120), 1.f);
            r.draw_text(ui_font, x, y - 10, label, rgba8(200, 205, 220));
            r.push_clip({x, y, gw * scale, gh * scale});
            r.set_transform(compose(cam, scale, x, y));
            draw_grid(r, font, grid, 0, 0, now - t0, true);
            r.pop_clip();
        };
        panel(panel_x, panel_y, 1.f, "pane: zoom 1.0 (pixel exact)");
        panel(panel_x + gw + 40, panel_y, 0.5f, "miniature 0.5");
        panel(panel_x + gw + 40, panel_y + gh * 0.5f + 60, 0.25f, "miniature 0.25");
        panel(panel_x + gw + 40 + gw * 0.5f + 40, panel_y + gh * 0.5f + 60, 0.25f, "miniature 0.25 (b)");
        // 2.0 zoom: a clipped window onto the grid's top-left corner
        {
            float zx = panel_x, zy = panel_y + gh + 60, zw = gw, zh = 180;
            r.set_transform(cam);
            r.rounded_rect({zx - 8, zy - 28, zw + 16, zh + 36}, 8, rgba8(44, 46, 60));
            r.draw_text(ui_font, zx, zy - 10, "zoom 2.0 (clipped)", rgba8(200, 205, 220));
            r.push_clip({zx, zy, zw, zh});
            r.set_transform(compose(cam, 2.f, zx, zy));
            draw_grid(r, font, grid, 0, 0, now - t0, false);
            r.pop_clip();
        }
        if (stress) {
            // many miniatures: approximates the overview with ~25 panes
            for (int i = 0; i < 24; ++i) {
                float sx = panel_x + (i % 6) * (gw * 0.5f + 20), sy = panel_y + gh + 300 + (i / 6) * (gh * 0.5f + 20);
                r.set_transform(compose(cam, 0.5f, sx, sy));
                draw_grid(r, font, grid, 0, 0, now - t0, false);
            }
        }
        // mouse crosshair in screen space (proves transform reset)
        r.set_transform(Transform{});
        r.line(mouse_x - 8, mouse_y, mouse_x + 8, mouse_y, rgba8(255, 255, 255, 0.4f), 1.f);
        r.line(mouse_x, mouse_y - 8, mouse_x, mouse_y + 8, rgba8(255, 255, 255, 0.4f), 1.f);

        auto cpu1 = std::chrono::steady_clock::now();   // batching done (pending quads not yet submitted)
        r.end_frame();
        auto cpu2 = std::chrono::steady_clock::now();
        double cpu_ms = std::chrono::duration<double, std::milli>(cpu1 - cpu0).count();
        double gl_ms = std::chrono::duration<double, std::milli>(cpu2 - cpu1).count();
        cpu_acc += cpu_ms; cpu_max = std::max(cpu_max, cpu_ms); gl_acc += gl_ms; ++cpu_n;
        if (cpu_ms > 4.0 && getenv("GFXDEMO_SPIKES")) std::printf("spike: batch %.2f ms submit %.2f ms (quads %d, draws %d)\n", cpu_ms, gl_ms, r.quads(), r.draw_calls());
        gl_check("frame");
        win.swap();
        ++frames;

        if (now - last_title >= 1.0) {
            fps = frames / (now - last_title);
            char title[256];
            std::snprintf(title, sizeof title, "gfxdemo — %.1f fps | batch %.2f ms avg, %.2f max | submit %.2f ms | %d draw calls | %d quads | zoom %.2f",
                          fps, cpu_acc / std::max(1, cpu_n), cpu_max, gl_acc / std::max(1, cpu_n), r.draw_calls(), r.quads(), cam.scale);
            win.set_title(title);
            std::printf("%s\n", title);
            std::fflush(stdout);
            frames = 0; cpu_acc = 0; cpu_max = 0; gl_acc = 0; cpu_n = 0; last_title = now;
        }
        if (max_frames && --max_frames == 0) running = false;
    }
    return 0;
}
