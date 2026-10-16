#pragma once
// VT emulator core. Pure C++, no I/O. Feed bytes with feed(); read grid with row()/history().
#include "cell.h"
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <vector>
#include <deque>
#include <memory>

namespace canopy {

enum class CursorStyle : uint8_t { Block = 0, Underline = 1, Bar = 2 };

struct Cursor {
    int x = 0, y = 0;          // y is a screen row (0..rows-1)
    bool visible = true;
    bool blink = true;
    CursorStyle style = CursorStyle::Block;
};

enum class MouseMode : uint16_t { None = 0, X10 = 9, Normal = 1000, ButtonEvent = 1002, AnyEvent = 1003 };
enum class MouseEncoding : uint16_t { Default = 0, Utf8 = 1005, Sgr = 1006, Urxvt = 1015, SgrPixels = 1016 };

struct TerminalModes {
    bool app_cursor_keys = false;   // DECCKM
    bool app_keypad = false;        // DECKPAM
    bool bracketed_paste = false;   // 2004
    bool focus_events = false;      // 1004
    bool alt_screen = false;        // 1049/47/1047
    bool origin = false;            // DECOM
    bool autowrap = true;           // DECAWM
    bool insert = false;            // IRM
    bool lnm = false;               // LNM (newline mode)
    bool reverse_video = false;     // DECSCNM
    bool cursor_visible = true;     // DECTCEM
    bool alt_sends_esc = true;      // 1039 (meta sends escape)
    bool kitty_keyboard = false;    // reserved for future
    MouseMode mouse = MouseMode::None;
    MouseEncoding mouse_enc = MouseEncoding::Default;
};

// Structured OSC event forwarded to listeners (shell integration, custom protocol).
struct OscEvent {
    int code = 0;                    // e.g. 133, 7770, 8, 52, 0, 2, 7
    std::vector<std::string> args;   // split on ';' after the code (raw, not base64-decoded)
    int64_t abs_line = 0;            // absolute line of the cursor when received
    int col = 0;                     // cursor column when received
};

struct Hyperlink { std::string id; std::string uri; };

class Terminal {
public:
    Terminal(int cols, int rows, int scrollback_max = 10000);
    ~Terminal();
    Terminal(const Terminal&) = delete;
    Terminal& operator=(const Terminal&) = delete;

    // ---- input from the application (bytes from the pty) ----
    void feed(const char* data, size_t len);
    void feed(std::string_view s) { feed(s.data(), s.size()); }

    // ---- callbacks (all invoked synchronously from feed()) ----
    std::function<void(std::string_view)> on_write;          // reply bytes to send to the pty (DA, CPR, ...)
    std::function<void(const std::string&)> on_title;         // OSC 0/2
    std::function<void(const std::string&)> on_cwd;           // OSC 7 (already URL-decoded path; hostname stripped)
    std::function<void()> on_bell;
    std::function<void(const OscEvent&)> on_osc;              // every OSC not fully consumed internally, plus 133/7770 always
    std::function<void(const std::string& selection, const std::string& base64)> on_clipboard_set; // OSC 52 write
    std::function<void(bool alt)> on_alt_screen;              // alt screen entered/left
    std::function<void()> on_resize_request;                  // reserved

    // ---- geometry ----
    void resize(int cols, int rows);   // no reflow in v1: pad/truncate; keeps absolute indices stable
    int cols() const;
    int rows() const;

    // ---- screen access ----
    // Visible row y (0..rows-1) of the *current* screen (alt when alt_screen is on).
    const Line& row(int y) const;
    // Primary-screen history. Absolute indices in [history_base(), history_base()+history_count()).
    int64_t history_base() const;
    int64_t history_count() const;
    const Line& history(int64_t abs) const;
    // Absolute index of primary screen row 0 (== history_base()+history_count()).
    int64_t screen_base() const;
    // Convenience: any absolute line in [history_base(), screen_base()+rows()) on the primary screen.
    const Line& line_abs(int64_t abs) const;
    // Hyperlink lookup for Cell::hyperlink ids.
    const Hyperlink* hyperlink(uint32_t id) const;

    Cursor cursor() const;
    const TerminalModes& modes() const;
    // Current SGR state (used by the UI to render prompts consistently); optional.
    std::string title() const;
    std::string cwd() const;          // last OSC 7 cwd, or empty

    // ---- change tracking ----
    // Monotonic counter incremented whenever anything visible changes.
    uint64_t generation() const;
    // Per-row dirty flags for the current screen; cleared by clear_dirty().
    bool row_dirty(int y) const;
    void clear_dirty();
    // True if any line scrolled into history since last clear_dirty (viewport may need to move).
    bool scrolled_since_clear() const;

    // ---- text extraction (for selection / copy / search) ----
    // Text between two absolute positions inclusive of start, exclusive of end column.
    // Lines joined with '\n' unless the previous line has wrapped==true.
    std::string text(int64_t abs_line0, int col0, int64_t abs_line1, int col1) const;
    // Word boundaries at a position (for double-click): returns [col_begin, col_end).
    void word_at(int64_t abs_line, int col, int* col_begin, int* col_end) const;

    // ---- misc control ----
    void reset();                               // RIS
    void set_scrollback_max(int lines);
    // Focus in/out reporting (sends CSI I / CSI O when mode 1004 is set).
    void notify_focus(bool focused);
    // Title stack etc. are internal.

    // ---- environment supplied by the UI (optional; sensible defaults) ----
    void set_cell_pixel_size(int w, int h);                                 // for CSI 14/16 t replies
    void set_default_colors(uint32_t fg_rgb, uint32_t bg_rgb, uint32_t cursor_rgb); // 0xRRGGBB; OSC 10/11/12 replies
    void set_palette_color(int idx, uint32_t rgb);                          // 0..255, 0xRRGGBB; OSC 4 replies
    // Colors as currently reported (defaults, possibly overridden by OSC 10/11/12/4 from an app).
    uint32_t default_fg() const; uint32_t default_bg() const; uint32_t cursor_color() const;
    uint32_t palette_color(int idx) const;

    struct Impl;
private:
    std::unique_ptr<Impl> impl_;
};

} // namespace canopy
