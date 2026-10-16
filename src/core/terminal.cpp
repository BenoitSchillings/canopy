// VT emulator: screen grid, scrollback, modes, and the dispatch side of the parser.
#include "terminal.h"
#include "vt_parser.h"
#include "unicode.h"
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <unordered_map>

namespace canopy {

namespace {

struct Pen {
    Color fg, bg, ul;
    uint16_t attrs = 0;
    UnderlineStyle ul_style = UnderlineStyle::None;
    uint32_t hyperlink = 0;
};

struct SavedCursor {
    int x = 0, y = 0;
    Pen pen;
    bool origin = false;
    bool autowrap = true;
    char g[2] = {'B', 'B'};
    int gl = 0;
    bool valid = false;
};

struct ScreenState {
    std::vector<Line> lines;
    int x = 0, y = 0;
    SavedCursor saved;
    std::vector<uint8_t> dirty;
};

const char32_t DEC_GRAPHICS[32] = {
    U' ',      U'◆', U'▒', U'␉', U'␌', U'␍', U'␊', U'°', // _ ` a b c d e f
    U'±', U'␤', U'␋', U'┘', U'┐', U'┌', U'└', U'┼', // g h i j k l m n
    U'⎺', U'⎻', U'─', U'⎼', U'⎽', U'├', U'┤', U'┴', // o p q r s t u v
    U'┬', U'│', U'≤', U'≥', U'π', U'≠', U'£', U'·', // w x y z { | } ~
};

uint32_t xterm_default_palette(int i) {
    static const uint32_t base[16] = {
        0x000000, 0xcd0000, 0x00cd00, 0xcdcd00, 0x0000ee, 0xcd00cd, 0x00cdcd, 0xe5e5e5,
        0x7f7f7f, 0xff0000, 0x00ff00, 0xffff00, 0x5c5cff, 0xff00ff, 0x00ffff, 0xffffff,
    };
    if (i < 16) return base[i];
    if (i < 232) {
        int c = i - 16;
        int r = c / 36, g = (c / 6) % 6, b = c % 6;
        auto lv = [](int v) { return v == 0 ? 0 : 55 + v * 40; };
        return (uint32_t)(lv(r) << 16 | lv(g) << 8 | lv(b));
    }
    int v = 8 + (i - 232) * 10;
    return (uint32_t)(v << 16 | v << 8 | v);
}

std::string rgb_spec(uint32_t rgb) {
    char buf[32];
    unsigned r = (rgb >> 16) & 0xff, g = (rgb >> 8) & 0xff, b = rgb & 0xff;
    snprintf(buf, sizeof buf, "rgb:%02x%02x/%02x%02x/%02x%02x", r, r, g, g, b, b);
    return buf;
}

// Parse "#rrggbb", "#rgb", "rgb:rr/gg/bb", "rgb:rrrr/gggg/bbbb". Returns false if unrecognized.
bool parse_color_spec(std::string_view s, uint32_t* out) {
    auto hexv = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    if (!s.empty() && s[0] == '#') {
        s.remove_prefix(1);
        size_t n = s.size();
        if (n != 3 && n != 6 && n != 9 && n != 12) return false;
        size_t per = n / 3;
        unsigned comp[3];
        for (int i = 0; i < 3; ++i) {
            int hi = hexv(s[i * per]);
            int lo = per > 1 ? hexv(s[i * per + 1]) : hi;
            if (hi < 0 || lo < 0) return false;
            comp[i] = (unsigned)(hi * 16 + lo);
        }
        *out = comp[0] << 16 | comp[1] << 8 | comp[2];
        return true;
    }
    if (s.size() > 4 && (s.substr(0, 4) == "rgb:")) {
        s.remove_prefix(4);
        unsigned comp[3];
        for (int i = 0; i < 3; ++i) {
            size_t slash = s.find('/');
            std::string_view part = (i < 2) ? s.substr(0, slash) : s;
            if (part.empty() || part.size() > 4) return false;
            unsigned v = 0;
            for (char c : part) { int h = hexv(c); if (h < 0) return false; v = v * 16 + (unsigned)h; }
            // Scale to 8 bits.
            unsigned bits = (unsigned)part.size() * 4;
            comp[i] = bits == 8 ? v : (unsigned)((v * 255ull) / ((1ull << bits) - 1));
            if (i < 2) { if (slash == std::string_view::npos) return false; s.remove_prefix(slash + 1); }
        }
        *out = comp[0] << 16 | comp[1] << 8 | comp[2];
        return true;
    }
    return false;
}

std::string url_decode(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '%' && i + 2 < s.size() + 0 && isxdigit((unsigned char)s[i + 1]) && isxdigit((unsigned char)s[i + 2])) {
            out.push_back((char)strtol(std::string(s.substr(i + 1, 2)).c_str(), nullptr, 16));
            i += 2;
        } else {
            out.push_back(s[i]);
        }
    }
    return out;
}

} // namespace

struct Terminal::Impl : VtHandler {
    Terminal& owner;
    VtParser parser;

    int cols, rows;
    int scrollback_max;
    ScreenState primary, alt;
    ScreenState* scr = &primary;
    std::deque<Line> history;
    int64_t history_base = 0;

    Pen pen;
    bool pending_wrap = false;
    TerminalModes modes;
    Cursor cursor_meta;             // visible/blink/style (x,y live in ScreenState)
    int scroll_top = 0, scroll_bottom;
    std::vector<uint8_t> tabs;
    char g[2] = {'B', 'B'};
    int gl = 0;
    char32_t last_printed = 0;
    bool reverse_wrap = false;

    std::string title, icon_title, cwd;
    std::vector<std::string> title_stack;

    std::vector<Hyperlink> links;   // id = index + 1
    std::unordered_map<std::string, uint32_t> link_ids;

    uint64_t gen = 1;
    bool scrolled = false;

    int cell_w = 8, cell_h = 16;
    uint32_t theme_fg = 0xd0d0d0, theme_bg = 0x000000, theme_cursor = 0xd0d0d0;
    uint32_t cur_fg, cur_bg, cur_cursor;
    uint32_t palette[256];
    uint32_t theme_palette[256];

    // DCS in progress
    bool dcs_active = false;
    std::string dcs_inter;
    uint8_t dcs_final = 0;
    std::string dcs_data;

    Impl(Terminal& o, int c, int r, int sb) : owner(o), parser(*this), cols(std::max(1, c)), rows(std::max(1, r)), scrollback_max(std::max(0, sb)) {
        scroll_bottom = rows - 1;
        for (int i = 0; i < 256; ++i) theme_palette[i] = palette[i] = xterm_default_palette(i);
        cur_fg = theme_fg; cur_bg = theme_bg; cur_cursor = theme_cursor;
        init_screen(primary);
        init_screen(alt);
        reset_tabs();
    }

    // ---------------------------------------------------------------- helpers
    Cell erase_cell() const {
        Cell c;
        c.bg = pen.bg;
        return c;
    }
    Line blank_line() const {
        Line l;
        l.cells.assign((size_t)cols, erase_cell());
        return l;
    }
    void init_screen(ScreenState& s) {
        s.lines.clear();
        s.lines.resize((size_t)rows);
        for (auto& l : s.lines) l.cells.assign((size_t)cols, Cell{});
        s.dirty.assign((size_t)rows, 1);
        s.x = s.y = 0;
    }
    void dirty_row(int y) { if (y >= 0 && y < rows) scr->dirty[(size_t)y] = 1; ++gen; }
    void dirty_rows(int a, int b) { for (int y = std::max(0, a); y <= b && y < rows; ++y) scr->dirty[(size_t)y] = 1; ++gen; }
    void dirty_all() { std::fill(scr->dirty.begin(), scr->dirty.end(), 1); ++gen; }
    Line& cur_line() { return scr->lines[(size_t)scr->y]; }
    int64_t abs_cursor_line() const { return history_base + (int64_t)history.size() + scr->y; }
    void write(std::string_view s) { if (owner.on_write) owner.on_write(s); }
    static bool line_blank(const Line& l) {
        for (const Cell& c : l.cells) if (!c.is_blank() || c.combining) return false;
        return true;
    }

    void reset_tabs() {
        tabs.assign((size_t)cols, 0);
        for (int x = 8; x < cols; x += 8) tabs[(size_t)x] = 1;
    }

    void push_history(Line&& l) {
        if (scrollback_max <= 0) { ++history_base; return; }
        history.push_back(std::move(l));
        while ((int)history.size() > scrollback_max) { history.pop_front(); ++history_base; }
        scrolled = true;
    }

    // Fresh line in place (reuses the cell allocation when possible).
    void clear_line(Line& l) {
        l.cells.assign((size_t)cols, erase_cell());
        l.combining.clear();
        l.wrapped = false;
    }

    // Scroll lines [top, bot] up by n; new blank lines appear at the bottom.
    void scroll_up(int top, int bot, int n) {
        if (top > bot) return;
        int h = bot - top + 1;
        if (n > h) n = h;
        if (n <= 0) return;
        bool to_history = (scr == &primary) && top == 0 && bot == rows - 1;
        auto& L = scr->lines;
        if (to_history) {
            for (int i = 0; i < n; ++i) push_history(std::move(L[(size_t)(top + i)]));
            for (int i = 0; i < n; ++i) L[(size_t)(top + i)] = Line{};
        }
        std::rotate(L.begin() + top, L.begin() + top + n, L.begin() + bot + 1);
        for (int i = 0; i < n; ++i) clear_line(L[(size_t)(bot - n + 1 + i)]);
        dirty_rows(top, bot);
    }
    // Scroll lines [top, bot] down by n; blank lines appear at the top.
    void scroll_down(int top, int bot, int n) {
        if (top > bot) return;
        int h = bot - top + 1;
        if (n > h) n = h;
        if (n <= 0) return;
        auto& L = scr->lines;
        std::rotate(L.begin() + top, L.begin() + bot + 1 - n, L.begin() + bot + 1);
        for (int i = 0; i < n; ++i) clear_line(L[(size_t)(top + i)]);
        dirty_rows(top, bot);
    }

    void index() {
        if (scr->y == scroll_bottom) scroll_up(scroll_top, scroll_bottom, 1);
        else if (scr->y < rows - 1) { scr->y++; ++gen; }
    }
    void reverse_index() {
        if (scr->y == scroll_top) scroll_down(scroll_top, scroll_bottom, 1);
        else if (scr->y > 0) { scr->y--; ++gen; }
    }
    void linefeed() {
        index();
        if (modes.lnm) scr->x = 0;
        pending_wrap = false;
    }
    void carriage_return() { scr->x = 0; pending_wrap = false; ++gen; }

    // Cursor positioning with clamping; honors origin mode for absolute moves.
    void move_to(int x, int y, bool absolute = true) {
        int top = 0, bot = rows - 1;
        if (absolute && modes.origin) { top = scroll_top; bot = scroll_bottom; y += top; }
        scr->x = std::clamp(x, 0, cols - 1);
        scr->y = std::clamp(y, top, bot);
        pending_wrap = false;
        ++gen;
    }
    void move_rel(int dx, int dy) {
        // Relative moves are confined to the scroll region when the cursor is inside it.
        int top = 0, bot = rows - 1;
        if (dy < 0 && scr->y >= scroll_top) top = scroll_top;
        if (dy > 0 && scr->y <= scroll_bottom) bot = scroll_bottom;
        scr->x = std::clamp(scr->x + dx, 0, cols - 1);
        scr->y = std::clamp(scr->y + dy, top, bot);
        pending_wrap = false;
        ++gen;
    }

    void clear_wide_at(Line& l, int x) {
        Cell& c = l.cells[(size_t)x];
        if ((c.attrs & ATTR_WIDE_CONT) && x > 0) {
            Cell& p = l.cells[(size_t)x - 1];
            p.cp = U' '; p.attrs &= (uint16_t)~ATTR_WIDE; p.combining = 0;
        }
        if ((c.attrs & ATTR_WIDE) && x + 1 < cols) {
            Cell& nx = l.cells[(size_t)x + 1];
            nx.cp = U' '; nx.attrs &= (uint16_t)~ATTR_WIDE_CONT; nx.combining = 0;
        }
    }

    void erase_cells(Line& l, int a, int b) {   // [a, b)
        a = std::max(0, a); b = std::min(cols, b);
        if (a >= b) return;
        if (a > 0 && (l.cells[(size_t)a].attrs & ATTR_WIDE_CONT)) clear_wide_at(l, a);
        if (b < cols && (l.cells[(size_t)b].attrs & ATTR_WIDE_CONT)) clear_wide_at(l, b);
        Cell e = erase_cell();
        for (int x = a; x < b; ++x) l.cells[(size_t)x] = e;
    }
    void erase_line_full(Line& l) {
        clear_line(l);
    }

    void put_combining(char32_t cp) {
        Line& l = cur_line();
        int x = pending_wrap ? cols - 1 : scr->x - 1;
        if (x < 0) return;
        if ((l.cells[(size_t)x].attrs & ATTR_WIDE_CONT) && x > 0) --x;
        Cell& c = l.cells[(size_t)x];
        if (c.combining == 0) {
            l.combining.emplace_back(1, cp);
            c.combining = (uint32_t)l.combining.size();
        } else if (c.combining <= l.combining.size()) {
            auto& seq = l.combining[c.combining - 1];
            if (seq.size() < 8) seq.push_back(cp);
        }
        dirty_row(scr->y);
    }

    void put_char(char32_t cp, int w) {
        if (pending_wrap) {
            if (modes.autowrap) {
                cur_line().wrapped = true;
                scr->x = 0;
                index();
            }
            pending_wrap = false;
        }
        if (w == 2 && scr->x >= cols - 1) {
            if (modes.autowrap && cols >= 2) {
                // Not enough room: pad the last cell and wrap.
                Line& l = cur_line();
                clear_wide_at(l, cols - 1);
                l.cells[(size_t)cols - 1] = erase_cell();
                l.wrapped = true;
                scr->x = 0;
                index();
            } else {
                scr->x = std::max(0, cols - 2);
            }
        }
        if (w == 2 && cols < 2) w = 1;
        Line& l = cur_line();
        int x = scr->x;
        if (modes.insert) {
            // Shift cells right by w, dropping those that fall off.
            if (x + w < cols) {
                std::move_backward(l.cells.begin() + x, l.cells.end() - w, l.cells.end());
                // A wide char cut in half at the end is dropped.
                if (l.cells.back().attrs & ATTR_WIDE) { l.cells.back().cp = U' '; l.cells.back().attrs &= (uint16_t)~ATTR_WIDE; }
            }
            for (int i = 0; i < w && x + i < cols; ++i) l.cells[(size_t)(x + i)] = Cell{};
        }
        clear_wide_at(l, x);
        if (w == 2) clear_wide_at(l, x + 1);
        Cell& c = l.cells[(size_t)x];
        c.cp = cp;
        c.combining = 0;
        c.fg = pen.fg; c.bg = pen.bg; c.ul = pen.ul;
        c.attrs = pen.attrs;
        c.ul_style = pen.ul_style;
        c.hyperlink = pen.hyperlink;
        if (w == 2) {
            c.attrs |= ATTR_WIDE;
            Cell& n = l.cells[(size_t)x + 1];
            n = c;
            n.cp = U' ';
            n.attrs = (uint16_t)((pen.attrs & ~ATTR_WIDE) | ATTR_WIDE_CONT);
        }
        last_printed = cp;
        x += w;
        if (x >= cols) {
            scr->x = cols - 1;
            if (modes.autowrap) pending_wrap = true;
        } else {
            scr->x = x;
        }
        dirty_row(scr->y);
    }

    char32_t translate(char32_t cp) const {
        char cs = g[gl];
        if (cs == '0' && cp >= 0x5F && cp <= 0x7E) return DEC_GRAPHICS[cp - 0x5F];
        return cp;
    }

    // ---------------------------------------------------------------- VtHandler
    void print(const char32_t* cps, size_t n) override {
        for (size_t i = 0; i < n; ++i) {
            char32_t cp = cps[i];
            if (cp < 0x80) {
                put_char(translate(cp), 1);
                continue;
            }
            int w = char_width(cp);
            if (w == 0) { put_combining(cp); continue; }
            put_char(cp, w);
        }
    }

    void execute(uint8_t c) override {
        ++gen;
        switch (c) {
        case 0x05: write("\x1b[?6c"); break;      // ENQ: answerback (minimal)
        case 0x07: if (owner.on_bell) owner.on_bell(); break;
        case 0x08: if (scr->x > 0) { scr->x--; } else if (reverse_wrap && scr->y > 0) { scr->y--; scr->x = cols - 1; } pending_wrap = false; break;
        case 0x09: tab_forward(1); break;
        case 0x0A: case 0x0B: case 0x0C: linefeed(); break;
        case 0x0D: carriage_return(); break;
        case 0x0E: gl = 1; break;                 // SO
        case 0x0F: gl = 0; break;                 // SI
        case 0x84: index(); pending_wrap = false; break;                // IND
        case 0x85: index(); scr->x = 0; pending_wrap = false; break;    // NEL
        case 0x88: tabs[(size_t)scr->x] = 1; break;                     // HTS
        case 0x8D: reverse_index(); pending_wrap = false; break;        // RI
        default: break;
        }
    }

    void tab_forward(int n) {
        pending_wrap = false;
        while (n-- > 0) {
            int x = scr->x + 1;
            while (x < cols - 1 && !tabs[(size_t)x]) ++x;
            scr->x = std::min(x, cols - 1);
        }
        ++gen;
    }
    void tab_back(int n) {
        pending_wrap = false;
        while (n-- > 0) {
            int x = scr->x - 1;
            while (x > 0 && !tabs[(size_t)x]) --x;
            scr->x = std::max(x, 0);
        }
        ++gen;
    }

    void save_cursor() {
        SavedCursor& s = scr->saved;
        s.x = scr->x; s.y = scr->y; s.pen = pen; s.origin = modes.origin; s.autowrap = modes.autowrap;
        s.g[0] = g[0]; s.g[1] = g[1]; s.gl = gl; s.valid = true;
    }
    void restore_cursor() {
        SavedCursor& s = scr->saved;
        if (!s.valid) {
            scr->x = scr->y = 0; pen = Pen{}; modes.origin = false; g[0] = g[1] = 'B'; gl = 0;
        } else {
            pen = s.pen; modes.origin = s.origin; modes.autowrap = s.autowrap;
            g[0] = s.g[0]; g[1] = s.g[1]; gl = s.gl;
            scr->x = std::clamp(s.x, 0, cols - 1);
            scr->y = std::clamp(s.y, 0, rows - 1);
        }
        pending_wrap = false;
        ++gen;
    }

    void switch_screen(bool to_alt, bool clear_alt) {
        if (to_alt == modes.alt_screen) {
            if (to_alt && clear_alt) { for (auto& l : alt.lines) clear_line(l); dirty_all(); }
            return;
        }
        modes.alt_screen = to_alt;
        scr = to_alt ? &alt : &primary;
        if (to_alt && clear_alt) for (auto& l : alt.lines) clear_line(l);
        pending_wrap = false;
        dirty_all();
        if (owner.on_alt_screen) owner.on_alt_screen(to_alt);
    }

    void soft_reset() {
        modes.cursor_visible = true; cursor_meta.visible = true;
        modes.origin = false;
        modes.insert = false;
        modes.app_cursor_keys = false;
        modes.app_keypad = false;
        modes.autowrap = true;
        scroll_top = 0; scroll_bottom = rows - 1;
        pen = Pen{};
        g[0] = g[1] = 'B'; gl = 0;
        pending_wrap = false;
        scr->saved = SavedCursor{};
        ++gen;
    }

    void full_reset() {
        if (modes.alt_screen) switch_screen(false, false);
        modes = TerminalModes{};
        cursor_meta = Cursor{};
        soft_reset();
        reset_tabs();
        for (auto& l : primary.lines) clear_line(l);
        for (auto& l : alt.lines) clear_line(l);
        primary.x = primary.y = alt.x = alt.y = 0;
        primary.saved = alt.saved = SavedCursor{};
        title_stack.clear();
        cur_fg = theme_fg; cur_bg = theme_bg; cur_cursor = theme_cursor;
        std::copy(std::begin(theme_palette), std::end(theme_palette), std::begin(palette));
        parser.reset();
        dirty_all();
    }

    void esc_dispatch(std::string_view inter, uint8_t f) override {
        ++gen;
        if (inter.empty()) {
            switch (f) {
            case '7': save_cursor(); break;
            case '8': restore_cursor(); break;
            case 'D': index(); pending_wrap = false; break;
            case 'E': index(); scr->x = 0; pending_wrap = false; break;
            case 'H': tabs[(size_t)scr->x] = 1; break;
            case 'M': reverse_index(); pending_wrap = false; break;
            case 'c': full_reset(); break;
            case '=': modes.app_keypad = true; break;
            case '>': modes.app_keypad = false; break;
            case 'n': gl = 2; break;   // LS2 (unsupported charset => treated as ASCII)
            case 'o': gl = 3; break;
            case '\\': break;          // ST
            default: break;
            }
            if (gl > 1) gl = 0;        // we only model G0/G1
            return;
        }
        if (inter == "(" || inter == ")" || inter == "*" || inter == "+") {
            int idx = inter[0] == '(' ? 0 : inter[0] == ')' ? 1 : -1;
            if (idx >= 0) g[idx] = (f == '0') ? '0' : 'B';
            return;
        }
        if (inter == "#") {
            if (f == '8') {   // DECALN
                for (auto& l : scr->lines) { clear_line(l); for (auto& c : l.cells) c.cp = U'E'; }
                scroll_top = 0; scroll_bottom = rows - 1;
                scr->x = scr->y = 0;
                dirty_all();
            }
            return;
        }
        if (inter == " ") return;   // S7C1T/S8C1T etc.
        if (inter == "%") return;   // charset selection (UTF-8 always)
    }

    // ------------------------------------------------------------------- CSI
    void csi_dispatch(const CsiParams& p, std::string_view inter, uint8_t f) override;
    void set_mode(int m, bool set);
    void set_dec_mode(int m, bool set);
    int dec_mode_state(int m) const;
    int ansi_mode_state(int m) const;
    void sgr(const CsiParams& p);
    void erase_display(int mode);
    void erase_line(int mode);
    void window_op(const CsiParams& p);
    void report_cursor(bool dec);

    // ------------------------------------------------------------------- OSC
    void osc_dispatch(std::string_view payload) override;
    void dcs_hook(const CsiParams& p, std::string_view inter, uint8_t f) override;
    void dcs_put(std::string_view d) override;
    void dcs_unhook() override;
    void string_dispatch(char, std::string_view) override { ++gen; }

    // --------------------------------------------------------------- resize
    void resize(int nc, int nr);
    void fit_line(Line& l, int w) {
        if ((int)l.cells.size() == w) return;
        if ((int)l.cells.size() > w) {
            if (w > 0 && (l.cells[(size_t)w - 1].attrs & ATTR_WIDE)) { l.cells[(size_t)w - 1].cp = U' '; l.cells[(size_t)w - 1].attrs &= (uint16_t)~ATTR_WIDE; }
            l.cells.resize((size_t)w);
        } else {
            l.cells.resize((size_t)w, Cell{});
        }
    }
    const Line& line_abs(int64_t a) const;
    const Line* line_abs_ptr(int64_t a) const;
};

// ====================================================================== CSI

void Terminal::Impl::report_cursor(bool dec) {
    int y = scr->y, x = scr->x;
    if (modes.origin) y -= scroll_top;
    char buf[48];
    snprintf(buf, sizeof buf, dec ? "\x1b[?%d;%dR" : "\x1b[%d;%dR", y + 1, x + 1);
    write(buf);
}

void Terminal::Impl::erase_display(int mode) {
    switch (mode) {
    case 0:
        erase_cells(cur_line(), scr->x, cols);
        for (int y = scr->y + 1; y < rows; ++y) clear_line(scr->lines[(size_t)y]);
        dirty_rows(scr->y, rows - 1);
        break;
    case 1:
        for (int y = 0; y < scr->y; ++y) clear_line(scr->lines[(size_t)y]);
        erase_cells(cur_line(), 0, scr->x + 1);
        dirty_rows(0, scr->y);
        break;
    case 2:
        if (scr == &primary && scrollback_max > 0) {
            // Like kitty: content erased from the primary screen is pushed into scrollback instead of
            // being lost, so absolute line indices (blocks, selections) keep pointing at real text.
            int last = rows - 1;
            while (last >= 0 && line_blank(scr->lines[(size_t)last])) --last;
            if (last >= 0) scroll_up(0, rows - 1, last + 1);
        }
        for (auto& l : scr->lines) clear_line(l);
        dirty_all();
        break;
    case 3:
        history_base += (int64_t)history.size();
        history.clear();
        ++gen;
        break;
    default: break;
    }
}

void Terminal::Impl::erase_line(int mode) {
    Line& l = cur_line();
    switch (mode) {
    case 0: erase_cells(l, scr->x, cols); break;
    case 1: erase_cells(l, 0, scr->x + 1); break;
    case 2: erase_cells(l, 0, cols); l.wrapped = false; break;
    default: return;
    }
    dirty_row(scr->y);
}

void Terminal::Impl::sgr(const CsiParams& p) {
    if (p.count == 0) { uint32_t hl = pen.hyperlink; pen = Pen{}; pen.hyperlink = hl; return; }
    int n = p.count;
    auto parse_ext_color = [&](int& i, Color* out) {
        // p[i] is 38/48/58. Returns via out; advances i past consumed params.
        if (i + 1 < n && p.is_sub(i + 1)) {
            // colon form: collect sub params
            int subs[8]; int sc = 0;
            int j = i + 1;
            while (j < n && p.is_sub(j) && sc < 8) subs[sc++] = p.get(j++, 0);
            if (sc >= 1 && subs[0] == 5 && sc >= 2) *out = Color::indexed((uint8_t)std::clamp(subs[1], 0, 255));
            else if (sc >= 1 && subs[0] == 2) {
                int off = sc >= 5 ? 2 : 1;   // 38:2:cs:r:g:b vs 38:2:r:g:b
                if (sc >= off + 3) *out = Color::rgb((uint8_t)std::clamp(subs[off], 0, 255), (uint8_t)std::clamp(subs[off + 1], 0, 255), (uint8_t)std::clamp(subs[off + 2], 0, 255));
            }
            i = j - 1;
            return;
        }
        int kind = p.get(i + 1, 0);
        if (kind == 5) {
            *out = Color::indexed((uint8_t)std::clamp(p.get(i + 2, 0), 0, 255));
            i += 2;
        } else if (kind == 2) {
            *out = Color::rgb((uint8_t)std::clamp(p.get(i + 2, 0), 0, 255), (uint8_t)std::clamp(p.get(i + 3, 0), 0, 255), (uint8_t)std::clamp(p.get(i + 4, 0), 0, 255));
            i += 4;
        } else {
            i += 1;
        }
    };
    for (int i = 0; i < n; ++i) {
        if (p.is_sub(i)) continue;   // stray sub-parameter
        int v = p.get(i, 0);
        switch (v) {
        case 0: { uint32_t hl = pen.hyperlink; pen = Pen{}; pen.hyperlink = hl; break; }
        case 1: pen.attrs |= ATTR_BOLD; break;
        case 2: pen.attrs |= ATTR_DIM; break;
        case 3: pen.attrs |= ATTR_ITALIC; break;
        case 4: {
            int style = 1;
            if (i + 1 < n && p.is_sub(i + 1)) { style = p.get(i + 1, 1); ++i; }
            if (style <= 0) { pen.attrs &= (uint16_t)~ATTR_UNDERLINE; pen.ul_style = UnderlineStyle::None; }
            else { pen.attrs |= ATTR_UNDERLINE; pen.ul_style = (UnderlineStyle)std::min(style, 5); }
            break;
        }
        case 5: case 6: pen.attrs |= ATTR_BLINK; break;
        case 7: pen.attrs |= ATTR_INVERSE; break;
        case 8: pen.attrs |= ATTR_INVISIBLE; break;
        case 9: pen.attrs |= ATTR_STRIKE; break;
        case 21: pen.attrs |= ATTR_UNDERLINE; pen.ul_style = UnderlineStyle::Double; break;
        case 22: pen.attrs &= (uint16_t)~(ATTR_BOLD | ATTR_DIM); break;
        case 23: pen.attrs &= (uint16_t)~ATTR_ITALIC; break;
        case 24: pen.attrs &= (uint16_t)~ATTR_UNDERLINE; pen.ul_style = UnderlineStyle::None; break;
        case 25: pen.attrs &= (uint16_t)~ATTR_BLINK; break;
        case 27: pen.attrs &= (uint16_t)~ATTR_INVERSE; break;
        case 28: pen.attrs &= (uint16_t)~ATTR_INVISIBLE; break;
        case 29: pen.attrs &= (uint16_t)~ATTR_STRIKE; break;
        case 38: parse_ext_color(i, &pen.fg); break;
        case 39: pen.fg = Color::def(); break;
        case 48: parse_ext_color(i, &pen.bg); break;
        case 49: pen.bg = Color::def(); break;
        case 53: pen.attrs |= ATTR_OVERLINE; break;
        case 55: pen.attrs &= (uint16_t)~ATTR_OVERLINE; break;
        case 58: parse_ext_color(i, &pen.ul); break;
        case 59: pen.ul = Color::def(); break;
        default:
            if (v >= 30 && v <= 37) pen.fg = Color::indexed((uint8_t)(v - 30));
            else if (v >= 40 && v <= 47) pen.bg = Color::indexed((uint8_t)(v - 40));
            else if (v >= 90 && v <= 97) pen.fg = Color::indexed((uint8_t)(v - 90 + 8));
            else if (v >= 100 && v <= 107) pen.bg = Color::indexed((uint8_t)(v - 100 + 8));
            break;
        }
    }
}

void Terminal::Impl::set_mode(int m, bool set) {
    switch (m) {
    case 4: modes.insert = set; break;
    case 20: modes.lnm = set; break;
    default: break;
    }
}

int Terminal::Impl::ansi_mode_state(int m) const {
    switch (m) {
    case 4: return modes.insert ? 1 : 2;
    case 20: return modes.lnm ? 1 : 2;
    case 2: case 12: return 4;   // permanently reset/ set-ish; report "permanently reset"
    default: return 0;
    }
}

void Terminal::Impl::set_dec_mode(int m, bool set) {
    switch (m) {
    case 1: modes.app_cursor_keys = set; break;
    case 3:   // DECCOLM: we do not switch column count, but it clears the screen and homes the cursor.
        erase_display(2); scroll_top = 0; scroll_bottom = rows - 1; move_to(0, 0); break;
    case 5: if (modes.reverse_video != set) { modes.reverse_video = set; dirty_all(); } break;
    case 6: modes.origin = set; move_to(0, 0); break;
    case 7: modes.autowrap = set; if (!set) pending_wrap = false; break;
    case 9: modes.mouse = set ? MouseMode::X10 : MouseMode::None; break;
    case 12: cursor_meta.blink = set; break;
    case 25: modes.cursor_visible = set; cursor_meta.visible = set; break;
    case 45: reverse_wrap = set; break;
    case 47: switch_screen(set, false); break;
    case 66: modes.app_keypad = set; break;
    case 1000: modes.mouse = set ? MouseMode::Normal : MouseMode::None; break;
    case 1002: modes.mouse = set ? MouseMode::ButtonEvent : MouseMode::None; break;
    case 1003: modes.mouse = set ? MouseMode::AnyEvent : MouseMode::None; break;
    case 1004: modes.focus_events = set; break;
    case 1005: modes.mouse_enc = set ? MouseEncoding::Utf8 : MouseEncoding::Default; break;
    case 1006: modes.mouse_enc = set ? MouseEncoding::Sgr : MouseEncoding::Default; break;
    case 1015: modes.mouse_enc = set ? MouseEncoding::Urxvt : MouseEncoding::Default; break;
    case 1016: modes.mouse_enc = set ? MouseEncoding::SgrPixels : MouseEncoding::Default; break;
    case 1034: break;   // meta sets 8th bit: not supported (we always send ESC)
    case 1036: case 1039: modes.alt_sends_esc = set; break;
    case 1047:
        if (set) switch_screen(true, false);
        else { if (modes.alt_screen) { for (auto& l : alt.lines) clear_line(l); } switch_screen(false, false); }
        break;
    case 1048: if (set) save_cursor(); else restore_cursor(); break;
    case 1049:
        if (set) { save_cursor(); switch_screen(true, true); }
        else { switch_screen(false, false); restore_cursor(); }
        break;
    case 2004: modes.bracketed_paste = set; break;
    default: break;
    }
    ++gen;
}

int Terminal::Impl::dec_mode_state(int m) const {
    auto b = [](bool v) { return v ? 1 : 2; };
    switch (m) {
    case 1: return b(modes.app_cursor_keys);
    case 3: return 2;
    case 5: return b(modes.reverse_video);
    case 6: return b(modes.origin);
    case 7: return b(modes.autowrap);
    case 9: return b(modes.mouse == MouseMode::X10);
    case 12: return b(cursor_meta.blink);
    case 25: return b(modes.cursor_visible);
    case 45: return b(reverse_wrap);
    case 47: case 1047: case 1049: return b(modes.alt_screen);
    case 66: return b(modes.app_keypad);
    case 1000: return b(modes.mouse == MouseMode::Normal);
    case 1002: return b(modes.mouse == MouseMode::ButtonEvent);
    case 1003: return b(modes.mouse == MouseMode::AnyEvent);
    case 1004: return b(modes.focus_events);
    case 1005: return b(modes.mouse_enc == MouseEncoding::Utf8);
    case 1006: return b(modes.mouse_enc == MouseEncoding::Sgr);
    case 1015: return b(modes.mouse_enc == MouseEncoding::Urxvt);
    case 1016: return b(modes.mouse_enc == MouseEncoding::SgrPixels);
    case 1036: case 1039: return b(modes.alt_sends_esc);
    case 2004: return b(modes.bracketed_paste);
    default: return 0;
    }
}

void Terminal::Impl::window_op(const CsiParams& p) {
    char buf[96];
    switch (p.get(0, 0)) {
    case 11: write("\x1b[1t"); break;
    case 13: write("\x1b[3;0;0t"); break;
    case 14: snprintf(buf, sizeof buf, "\x1b[4;%d;%dt", rows * cell_h, cols * cell_w); write(buf); break;
    case 15: snprintf(buf, sizeof buf, "\x1b[5;%d;%dt", rows * cell_h, cols * cell_w); write(buf); break;
    case 16: snprintf(buf, sizeof buf, "\x1b[6;%d;%dt", cell_h, cell_w); write(buf); break;
    case 18: case 19: snprintf(buf, sizeof buf, "\x1b[%d;%d;%dt", p.get(0, 0) == 18 ? 8 : 9, rows, cols); write(buf); break;
    case 20: write("\x1b]L" + icon_title + "\x1b\\"); break;
    case 21: write("\x1b]l" + title + "\x1b\\"); break;
    case 22: {
        int which = p.get(1, 0);
        if (which == 0 || which == 2) title_stack.push_back(title);
        if (title_stack.size() > 16) title_stack.erase(title_stack.begin());
        break;
    }
    case 23: {
        int which = p.get(1, 0);
        if ((which == 0 || which == 2) && !title_stack.empty()) {
            title = title_stack.back(); title_stack.pop_back();
            if (owner.on_title) owner.on_title(title);
        }
        break;
    }
    default: break;   // resize/move/iconify requests are ignored
    }
}

void Terminal::Impl::csi_dispatch(const CsiParams& p, std::string_view inter, uint8_t f) {
    ++gen;
    const int n1 = p.get1(0);
    if (inter.empty()) {
        switch (f) {
        case '@': {   // ICH
            Line& l = cur_line(); int x = scr->x, n = std::min(n1, cols - x);
            clear_wide_at(l, x);
            std::move_backward(l.cells.begin() + x, l.cells.end() - n, l.cells.end());
            if (l.cells.back().attrs & ATTR_WIDE) { l.cells.back().cp = U' '; l.cells.back().attrs &= (uint16_t)~ATTR_WIDE; }
            Cell e = erase_cell();
            for (int i = 0; i < n; ++i) l.cells[(size_t)(x + i)] = e;
            dirty_row(scr->y);
            break;
        }
        case 'A': move_rel(0, -n1); break;
        case 'B': case 'e': move_rel(0, n1); break;
        case 'C': case 'a': move_rel(n1, 0); break;
        case 'D': move_rel(-n1, 0); break;
        case 'E': move_rel(0, n1); scr->x = 0; break;
        case 'F': move_rel(0, -n1); scr->x = 0; break;
        case 'G': case '`': move_to(n1 - 1, scr->y, false); break;
        case 'H': case 'f': move_to(p.get1(1) - 1, n1 - 1); break;
        case 'I': tab_forward(n1); break;
        case 'J': erase_display(p.get(0, 0)); break;
        case 'K': erase_line(p.get(0, 0)); break;
        case 'L': if (scr->y >= scroll_top && scr->y <= scroll_bottom) { scroll_down(scr->y, scroll_bottom, n1); scr->x = 0; pending_wrap = false; } break;
        case 'M': if (scr->y >= scroll_top && scr->y <= scroll_bottom) { scroll_up(scr->y, scroll_bottom, n1); scr->x = 0; pending_wrap = false; } break;
        case 'P': {   // DCH
            Line& l = cur_line(); int x = scr->x, n = std::min(n1, cols - x);
            clear_wide_at(l, x);
            if (x + n < cols) clear_wide_at(l, x + n);
            std::move(l.cells.begin() + x + n, l.cells.end(), l.cells.begin() + x);
            Cell e = erase_cell();
            for (int i = cols - n; i < cols; ++i) l.cells[(size_t)i] = e;
            dirty_row(scr->y);
            break;
        }
        case 'S': scroll_up(scroll_top, scroll_bottom, n1); break;
        case 'T': scroll_down(scroll_top, scroll_bottom, n1); break;
        case 'X': erase_cells(cur_line(), scr->x, scr->x + n1); dirty_row(scr->y); break;
        case 'Z': tab_back(n1); break;
        case 'b': if (last_printed) { int w = char_width(last_printed); if (w == 0) w = 1; for (int i = 0; i < std::min(n1, cols * rows); ++i) put_char(last_printed, w); } break;
        case 'c': write("\x1b[?62;1;2;6;9;15;16;17;18;21;22c"); break;
        case 'd': move_to(scr->x, n1 - 1); break;   // VPA (origin-relative)
        case 'g': if (p.get(0, 0) == 0) tabs[(size_t)scr->x] = 0; else if (p.get(0, 0) == 3) std::fill(tabs.begin(), tabs.end(), 0); break;
        case 'h': for (int i = 0; i < std::max(1, p.count); ++i) set_mode(p.get(i, 0), true); break;
        case 'l': for (int i = 0; i < std::max(1, p.count); ++i) set_mode(p.get(i, 0), false); break;
        case 'm': sgr(p); break;
        case 'n':
            if (p.get(0, 0) == 5) write("\x1b[0n");
            else if (p.get(0, 0) == 6) report_cursor(false);
            break;
        case 'r': {   // DECSTBM
            int top = p.get1(0) - 1;
            int bot = p.get(1, rows) - 1;
            if (bot < 0 || bot >= rows) bot = rows - 1;
            if (top < 0) top = 0;
            if (top < bot) { scroll_top = top; scroll_bottom = bot; move_to(0, 0); }
            break;
        }
        case 's': save_cursor(); break;
        case 'u': restore_cursor(); break;
        case 't': window_op(p); break;
        case 'q': break;   // DECLL
        case 'x': write("\x1b[2;1;1;112;112;1;0x"); break;   // DECREQTPARM
        default: break;
        }
        return;
    }
    if (inter == "?") {
        switch (f) {
        case 'h': for (int i = 0; i < std::max(1, p.count); ++i) set_dec_mode(p.get(i, 0), true); break;
        case 'l': for (int i = 0; i < std::max(1, p.count); ++i) set_dec_mode(p.get(i, 0), false); break;
        case 'J': erase_display(p.get(0, 0)); break;     // DECSED (no selective erase; same as ED)
        case 'K': erase_line(p.get(0, 0)); break;
        case 'n':
            if (p.get(0, 0) == 6) report_cursor(true);
            else if (p.get(0, 0) == 15) write("\x1b[?11n");
            else if (p.get(0, 0) == 25) write("\x1b[?21n");
            else if (p.get(0, 0) == 26) write("\x1b[?27;1;0;0n");
            break;
        default: break;
        }
        return;
    }
    if (inter == "?$" && f == 'p') {   // DECRQM (DEC private)
        char buf[32];
        snprintf(buf, sizeof buf, "\x1b[?%d;%d$y", p.get(0, 0), dec_mode_state(p.get(0, 0)));
        write(buf);
        return;
    }
    if (inter == "$" && f == 'p') {    // DECRQM (ANSI)
        char buf[32];
        snprintf(buf, sizeof buf, "\x1b[%d;%d$y", p.get(0, 0), ansi_mode_state(p.get(0, 0)));
        write(buf);
        return;
    }
    if (inter == ">") {
        switch (f) {
        case 'c': write("\x1b[>41;377;0c"); break;        // DA2: xterm-ish
        case 'q': write("\x1bP>|Canopy 0.1\x1b\\"); break; // XTVERSION
        default: break;   // XTMODKEYS, title modes, etc.
        }
        return;
    }
    if (inter == "=") {
        if (f == 'c') write("\x1bP!|00000000\x1b\\");   // DA3
        return;
    }
    if (inter == " " && f == 'q') {   // DECSCUSR
        int s = p.get(0, 0);
        switch (s) {
        case 0: case 1: cursor_meta.style = CursorStyle::Block; cursor_meta.blink = true; break;
        case 2: cursor_meta.style = CursorStyle::Block; cursor_meta.blink = false; break;
        case 3: cursor_meta.style = CursorStyle::Underline; cursor_meta.blink = true; break;
        case 4: cursor_meta.style = CursorStyle::Underline; cursor_meta.blink = false; break;
        case 5: cursor_meta.style = CursorStyle::Bar; cursor_meta.blink = true; break;
        case 6: cursor_meta.style = CursorStyle::Bar; cursor_meta.blink = false; break;
        default: break;
        }
        return;
    }
    if (inter == "!" && f == 'p') { soft_reset(); return; }   // DECSTR
    // DECSCL (" p), DECSCA (" q), DECSLRM etc.: ignored.
}

// ====================================================================== OSC / DCS

void Terminal::Impl::osc_dispatch(std::string_view payload) {
    ++gen;
    // Split "code;args..."
    size_t i = 0;
    int code = 0;
    bool have_digit = false;
    while (i < payload.size() && payload[i] >= '0' && payload[i] <= '9') { code = code * 10 + (payload[i] - '0'); if (code > 99999) code = 99999; ++i; have_digit = true; }
    if (!have_digit) return;
    std::string_view rest;
    if (i < payload.size() && payload[i] == ';') rest = payload.substr(i + 1);
    else if (i < payload.size()) return;   // garbage after code

    OscEvent ev;
    ev.code = code;
    ev.abs_line = abs_cursor_line();
    ev.col = scr->x;
    {
        size_t start = 0;
        for (;;) {
            size_t semi = rest.find(';', start);
            if (semi == std::string_view::npos) { ev.args.emplace_back(rest.substr(start)); break; }
            ev.args.emplace_back(rest.substr(start, semi - start));
            start = semi + 1;
        }
        if (rest.empty() && i >= payload.size()) ev.args.clear();
    }
    auto forward = [&]() { if (owner.on_osc) owner.on_osc(ev); };

    switch (code) {
    case 0: case 1: case 2: {
        std::string t(rest);
        if (code != 1) { title = t; if (owner.on_title) owner.on_title(title); }
        if (code != 2) icon_title = t;
        return;
    }
    case 7: {
        std::string s(rest);
        std::string path;
        auto strip_scheme = [&](const char* scheme) {
            size_t n = strlen(scheme);
            if (s.compare(0, n, scheme) == 0) {
                std::string_view r(s); r.remove_prefix(n);
                size_t slash = r.find('/');
                path = url_decode(slash == std::string_view::npos ? std::string_view() : r.substr(slash));
                return true;
            }
            return false;
        };
        if (!strip_scheme("file://") && !strip_scheme("kitty-shell-cwd://")) {
            if (!s.empty() && s[0] == '/') path = url_decode(s);
        }
        if (!path.empty()) { cwd = path; if (owner.on_cwd) owner.on_cwd(cwd); }
        forward();
        return;
    }
    case 8: {
        // params;uri
        size_t semi = rest.find(';');
        std::string params(rest.substr(0, semi));
        std::string uri = semi == std::string_view::npos ? std::string() : std::string(rest.substr(semi + 1));
        if (uri.empty()) { pen.hyperlink = 0; return; }
        std::string id;
        size_t pos = 0;
        while (pos < params.size()) {
            size_t colon = params.find(':', pos);
            std::string_view kv(params.data() + pos, (colon == std::string::npos ? params.size() : colon) - pos);
            if (kv.substr(0, 3) == "id=") id = std::string(kv.substr(3));
            if (colon == std::string::npos) break;
            pos = colon + 1;
        }
        std::string key = id + '\x01' + uri;
        auto it = link_ids.find(key);
        if (it != link_ids.end()) pen.hyperlink = it->second;
        else {
            links.push_back(Hyperlink{id, uri});
            pen.hyperlink = (uint32_t)links.size();
            link_ids.emplace(std::move(key), pen.hyperlink);
        }
        return;
    }
    case 4: {
        // 4;idx;spec[;idx;spec...]  spec may be "?"
        bool changed = false;
        for (size_t k = 0; k + 1 < ev.args.size(); k += 2) {
            int idx = atoi(ev.args[k].c_str());
            if (idx < 0 || idx > 255) continue;
            if (ev.args[k + 1] == "?") {
                char buf[64];
                snprintf(buf, sizeof buf, "\x1b]4;%d;", idx);
                write(std::string(buf) + rgb_spec(palette[idx]) + "\x1b\\");
            } else {
                uint32_t rgb;
                if (parse_color_spec(ev.args[k + 1], &rgb)) { palette[idx] = rgb; changed = true; }
            }
        }
        if (changed) dirty_all();
        forward();
        return;
    }
    case 104: {
        if (ev.args.empty() || (ev.args.size() == 1 && ev.args[0].empty())) std::copy(std::begin(theme_palette), std::end(theme_palette), std::begin(palette));
        else for (auto& a : ev.args) { int idx = atoi(a.c_str()); if (idx >= 0 && idx < 256) palette[idx] = theme_palette[idx]; }
        dirty_all();
        forward();
        return;
    }
    case 10: case 11: case 12: {
        // Possibly several specs: 10;fg;bg;cursor
        int c = code;
        bool changed = false;
        for (auto& a : ev.args) {
            uint32_t* slot = c == 10 ? &cur_fg : c == 11 ? &cur_bg : c == 12 ? &cur_cursor : nullptr;
            if (slot) {
                if (a == "?") {
                    char buf[16];
                    snprintf(buf, sizeof buf, "\x1b]%d;", c);
                    write(std::string(buf) + rgb_spec(*slot) + "\x1b\\");
                } else {
                    uint32_t rgb;
                    if (parse_color_spec(a, &rgb)) { *slot = rgb; changed = true; }
                }
            }
            ++c;
        }
        if (changed) dirty_all();
        forward();
        return;
    }
    case 110: cur_fg = theme_fg; dirty_all(); forward(); return;
    case 111: cur_bg = theme_bg; dirty_all(); forward(); return;
    case 112: cur_cursor = theme_cursor; dirty_all(); forward(); return;
    case 52: {
        if (ev.args.size() >= 2) {
            std::string sel = ev.args[0];
            const std::string& data = ev.args[1];
            if (data == "?") return;   // clipboard reads are not supported
            if (sel.empty()) sel = "c";
            bool want = false;
            for (char ch : sel) if (ch == 'c' || ch == 'p' || ch == 's' || ch == '0') want = true;
            if (want && owner.on_clipboard_set) owner.on_clipboard_set(sel, data);
        }
        return;
    }
    default:
        forward();   // 133, 7770, 9, 777, 1337, 99, ... all forwarded
        return;
    }
}

void Terminal::Impl::dcs_hook(const CsiParams& p, std::string_view inter, uint8_t f) {
    dcs_active = true;
    dcs_inter = std::string(inter);
    dcs_final = f;
    dcs_data.clear();
}
void Terminal::Impl::dcs_put(std::string_view d) {
    if (dcs_data.size() < 4096) dcs_data.append(d.substr(0, 4096 - dcs_data.size()));
}
void Terminal::Impl::dcs_unhook() {
    ++gen;
    if (!dcs_active) return;
    dcs_active = false;
    if (dcs_inter == "$" && dcs_final == 'q') {   // DECRQSS
        std::string reply;
        if (dcs_data == "m") {
            reply = "0";
            if (pen.attrs & ATTR_BOLD) reply += ";1";
            if (pen.attrs & ATTR_DIM) reply += ";2";
            if (pen.attrs & ATTR_ITALIC) reply += ";3";
            if (pen.attrs & ATTR_UNDERLINE) reply += ";4";
            if (pen.attrs & ATTR_BLINK) reply += ";5";
            if (pen.attrs & ATTR_INVERSE) reply += ";7";
            if (pen.attrs & ATTR_INVISIBLE) reply += ";8";
            if (pen.attrs & ATTR_STRIKE) reply += ";9";
            auto col = [&](const Color& c, int base) {
                char b[32];
                if (c.kind == Color::Indexed) { if (c.idx < 8) snprintf(b, sizeof b, ";%d", base + c.idx); else if (c.idx < 16) snprintf(b, sizeof b, ";%d", base + 60 + c.idx - 8); else snprintf(b, sizeof b, ";%d:5:%d", base + 8, c.idx); reply += b; }
                else if (c.kind == Color::Rgb) { snprintf(b, sizeof b, ";%d:2::%d:%d:%d", base + 8, c.r, c.g, c.b); reply += b; }
            };
            col(pen.fg, 30); col(pen.bg, 40);
            reply += "m";
            write("\x1bP1$r" + reply + "\x1b\\");
        } else if (dcs_data == "r") {
            char b[32]; snprintf(b, sizeof b, "%d;%dr", scroll_top + 1, scroll_bottom + 1);
            write(std::string("\x1bP1$r") + b + "\x1b\\");
        } else if (dcs_data == " q") {
            int s = cursor_meta.style == CursorStyle::Block ? 1 : cursor_meta.style == CursorStyle::Underline ? 3 : 5;
            if (!cursor_meta.blink) ++s;
            char b[32]; snprintf(b, sizeof b, "%d q", s);
            write(std::string("\x1bP1$r") + b + "\x1b\\");
        } else {
            write("\x1bP0$r\x1b\\");
        }
        return;
    }
    if (dcs_inter == "+" && dcs_final == 'q') {   // XTGETTCAP: report everything as unknown
        write("\x1bP0+r\x1b\\");
        return;
    }
}

// ====================================================================== resize

void Terminal::Impl::resize(int nc, int nr) {
    nc = std::max(1, nc); nr = std::max(1, nr);
    if (nc == cols && nr == rows) return;
    // Primary screen: adjust row count by moving lines to/from history.
    {
        ScreenState& s = primary;
        int cur_rows = (int)s.lines.size();
        while (cur_rows > nr) {
            if (s.y < cur_rows - 1 && line_blank(s.lines.back())) {
                s.lines.pop_back();
            } else {
                push_history(std::move(s.lines.front()));
                s.lines.erase(s.lines.begin());
                if (s.y > 0) s.y--;
                if (s.saved.y > 0) s.saved.y--;
            }
            --cur_rows;
        }
        while (cur_rows < nr) {
            if (!history.empty()) {
                s.lines.insert(s.lines.begin(), std::move(history.back()));
                history.pop_back();
                s.y++;
                if (s.saved.valid) s.saved.y++;
            } else {
                s.lines.emplace_back();
            }
            ++cur_rows;
        }
    }
    // Alternate screen: truncate/pad rows.
    alt.lines.resize((size_t)nr);
    cols = nc; rows = nr;
    for (auto& l : primary.lines) fit_line(l, cols);
    for (auto& l : alt.lines) fit_line(l, cols);
    for (auto& l : history) fit_line(l, cols);
    for (ScreenState* s : {&primary, &alt}) {
        s->x = std::clamp(s->x, 0, cols - 1);
        s->y = std::clamp(s->y, 0, rows - 1);
        s->saved.x = std::clamp(s->saved.x, 0, cols - 1);
        s->saved.y = std::clamp(s->saved.y, 0, rows - 1);
        s->dirty.assign((size_t)rows, 1);
    }
    scroll_top = 0; scroll_bottom = rows - 1;
    pending_wrap = false;
    // Tab stops: keep existing, extend with every-8 defaults.
    size_t old = tabs.size();
    tabs.resize((size_t)cols, 0);
    for (size_t x = old; x < (size_t)cols; ++x) if (x % 8 == 0) tabs[x] = 1;
    ++gen;
}

const Line* Terminal::Impl::line_abs_ptr(int64_t a) const {
    if (a < history_base) return nullptr;
    int64_t hi = (int64_t)history.size();
    if (a < history_base + hi) return &history[(size_t)(a - history_base)];
    int64_t y = a - history_base - hi;
    if (y < 0 || y >= rows) return nullptr;
    // Screen rows: the current screen (so selection works on the alt screen as well).
    return &scr->lines[(size_t)y];
}

const Line& Terminal::Impl::line_abs(int64_t a) const {
    static const Line empty;
    const Line* l = line_abs_ptr(a);
    return l ? *l : empty;
}

// ====================================================================== Terminal

Terminal::Terminal(int cols, int rows, int scrollback_max) : impl_(std::make_unique<Impl>(*this, cols, rows, scrollback_max)) {}
Terminal::~Terminal() = default;

void Terminal::feed(const char* data, size_t len) { impl_->parser.feed(data, len); }
void Terminal::resize(int cols, int rows) { impl_->resize(cols, rows); }
int Terminal::cols() const { return impl_->cols; }
int Terminal::rows() const { return impl_->rows; }

const Line& Terminal::row(int y) const {
    static const Line empty;
    if (y < 0 || y >= impl_->rows) return empty;
    return impl_->scr->lines[(size_t)y];
}
int64_t Terminal::history_base() const { return impl_->history_base; }
int64_t Terminal::history_count() const { return (int64_t)impl_->history.size(); }
const Line& Terminal::history(int64_t abs) const {
    static const Line empty;
    int64_t i = abs - impl_->history_base;
    if (i < 0 || i >= (int64_t)impl_->history.size()) return empty;
    return impl_->history[(size_t)i];
}
int64_t Terminal::screen_base() const { return impl_->history_base + (int64_t)impl_->history.size(); }
const Line& Terminal::line_abs(int64_t abs) const { return impl_->line_abs(abs); }
const Hyperlink* Terminal::hyperlink(uint32_t id) const {
    if (id == 0 || id > impl_->links.size()) return nullptr;
    return &impl_->links[id - 1];
}

Cursor Terminal::cursor() const {
    Cursor c = impl_->cursor_meta;
    c.x = impl_->scr->x;
    c.y = impl_->scr->y;
    c.visible = impl_->modes.cursor_visible;
    return c;
}
const TerminalModes& Terminal::modes() const { return impl_->modes; }
std::string Terminal::title() const { return impl_->title; }
std::string Terminal::cwd() const { return impl_->cwd; }

uint64_t Terminal::generation() const { return impl_->gen; }
bool Terminal::row_dirty(int y) const { return y >= 0 && y < impl_->rows && impl_->scr->dirty[(size_t)y]; }
void Terminal::clear_dirty() {
    std::fill(impl_->scr->dirty.begin(), impl_->scr->dirty.end(), 0);
    impl_->scrolled = false;
}
bool Terminal::scrolled_since_clear() const { return impl_->scrolled; }

std::string Terminal::text(int64_t l0, int c0, int64_t l1, int c1) const {
    std::string out;
    if (l1 < l0 || (l1 == l0 && c1 <= c0)) return out;
    for (int64_t a = l0; a <= l1; ++a) {
        const Line* l = impl_->line_abs_ptr(a);
        if (!l) { if (a < l1) out.push_back('\n'); continue; }
        int b = (a == l0) ? c0 : 0;
        int e = (a == l1) ? std::min(c1, l->width()) : l->width();
        out += l->text(b, e);
        if (a < l1 && !l->wrapped) out.push_back('\n');
    }
    return out;
}

void Terminal::word_at(int64_t abs_line, int col, int* col_begin, int* col_end) const {
    const Line* l = impl_->line_abs_ptr(abs_line);
    if (!l || l->width() == 0) { *col_begin = *col_end = col; return; }
    int w = l->width();
    col = std::clamp(col, 0, w - 1);
    auto cls = [&](int x) -> int {
        const Cell& c = l->cells[(size_t)x];
        char32_t cp = c.cp;
        if (c.attrs & ATTR_WIDE_CONT) cp = x > 0 ? l->cells[(size_t)x - 1].cp : U' ';
        if (cp == 0 || cp == U' ' || cp == U'\t') return 0;
        static const char32_t delims[] = U"\"'`()[]{}<>,;|&=";
        for (char32_t d : delims) if (d && cp == d) return 2;
        return 1;
    };
    int k = cls(col);
    int b = col, e = col + 1;
    if (k == 2) { *col_begin = b; *col_end = e; return; }
    while (b > 0 && cls(b - 1) == k) --b;
    while (e < w && cls(e) == k) ++e;
    *col_begin = b; *col_end = e;
}

void Terminal::reset() { impl_->full_reset(); }
void Terminal::set_scrollback_max(int lines) {
    impl_->scrollback_max = std::max(0, lines);
    while ((int)impl_->history.size() > impl_->scrollback_max) { impl_->history.pop_front(); ++impl_->history_base; }
}
void Terminal::notify_focus(bool focused) {
    if (impl_->modes.focus_events) impl_->write(focused ? "\x1b[I" : "\x1b[O");
}

void Terminal::set_cell_pixel_size(int w, int h) { impl_->cell_w = std::max(1, w); impl_->cell_h = std::max(1, h); }
void Terminal::set_default_colors(uint32_t fg, uint32_t bg, uint32_t cur) {
    bool fg_default = impl_->cur_fg == impl_->theme_fg, bg_default = impl_->cur_bg == impl_->theme_bg, cu_default = impl_->cur_cursor == impl_->theme_cursor;
    impl_->theme_fg = fg; impl_->theme_bg = bg; impl_->theme_cursor = cur;
    if (fg_default) impl_->cur_fg = fg;
    if (bg_default) impl_->cur_bg = bg;
    if (cu_default) impl_->cur_cursor = cur;
}
void Terminal::set_palette_color(int idx, uint32_t rgb) {
    if (idx < 0 || idx > 255) return;
    bool was_default = impl_->palette[idx] == impl_->theme_palette[idx];
    impl_->theme_palette[idx] = rgb;
    if (was_default) impl_->palette[idx] = rgb;
}
uint32_t Terminal::default_fg() const { return impl_->cur_fg; }
uint32_t Terminal::default_bg() const { return impl_->cur_bg; }
uint32_t Terminal::cursor_color() const { return impl_->cur_cursor; }
uint32_t Terminal::palette_color(int idx) const { return (idx >= 0 && idx < 256) ? impl_->palette[idx] : 0; }

} // namespace canopy
