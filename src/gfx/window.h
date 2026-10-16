#pragma once
// SDL2 window + GL context + event translation. The only file that includes SDL headers besides gl.cpp/font?.
#include "../core/keys.h"
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace canopy::gfx {

struct Event {
    enum Type { None, Quit, Resize, Key, Text, MouseDown, MouseUp, MouseMove, Wheel, FocusIn, FocusOut, DropFile, Wake, Expose, ClipboardUpdate };
    Type type = None;
    // Resize: framebuffer size in pixels
    int w = 0, h = 0;
    // Key / Text
    canopy::KeyEvent key;        // key.text filled for Text events (SDL_TEXTINPUT) and for Key::Char when known
    bool repeat = false;
    // Mouse (framebuffer pixel coords)
    float x = 0, y = 0;
    canopy::MouseButton button = canopy::MouseButton::None;
    int clicks = 0;              // click count for MouseDown
    uint32_t mods = 0;           // Mod bits
    // Wheel: precise deltas (positive = up / right); fallback to +-1 per notch
    float wheel_x = 0, wheel_y = 0;
    std::string path;            // DropFile
};

class Window {
public:
    Window();
    ~Window();
    bool create(const std::string& title, int w, int h, std::string* err = nullptr);
    // Blocks up to timeout_ms for events; appends all pending events to out. Returns false on Quit.
    void poll(std::vector<Event>& out, int timeout_ms);
    // Thread-safe: wakes poll() from another thread (posts a Wake event).
    void wake();
    void swap();
    void set_title(const std::string& title);
    void set_vsync(bool on);
    int fb_width() const { return fb_w_; }
    int fb_height() const { return fb_h_; }
    float dpi_scale() const { return dpi_scale_; }   // framebuffer px per logical px
    bool focused() const { return focused_; }
    void set_clipboard(const std::string& text);
    std::string clipboard() const;
    void set_primary_selection(const std::string& text);  // X11 PRIMARY (SDL 2.26+); no-op otherwise
    std::string primary_selection() const;
    void start_text_input();
    void stop_text_input();
    void set_text_input_rect(int x, int y, int w, int h);
    void set_cursor(int sdl_system_cursor);   // SDL_SYSTEM_CURSOR_* value
    void request_attention();                 // flash / urgency hint
    void* sdl_window() const { return win_; }
    double now() const;                       // seconds, monotonic

private:
    void* win_ = nullptr;
    void* ctx_ = nullptr;
    int fb_w_ = 0, fb_h_ = 0;
    float dpi_scale_ = 1.f;
    bool focused_ = true;
    uint32_t wake_event_ = 0;
    void* cursors_[16] = {};
};

} // namespace canopy::gfx
