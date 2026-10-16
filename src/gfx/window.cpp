// SDL2 window + GL 3.3 core context + event translation into gfx::Event.
//
// Delivery rules for keyboard input (the UI relies on these):
//  * Every SDL_KEYDOWN becomes one Event::Key. For printable keys key.key == Key::Char and
//    key.keysym is the *unshifted* base code point from the SDL keycode ('a' for Shift+A).
//  * Key events for Key::Char carry key.text ONLY when Ctrl or Alt (not AltGr) is held, so
//    the UI can encode Ctrl+A / Alt+x itself. For plain printable keys key.text is empty and
//    the committed text arrives afterwards as Event::Text (from SDL_TEXTINPUT). Text events
//    therefore never duplicate a Key event: we drop the SDL_TEXTINPUT that follows a key we
//    already delivered with text, and the one that follows a non-Char key (Space, keypad digits
//    with NumLock on) because the UI encodes those from the Key event.
//  * mods: MOD_SHIFT / MOD_ALT / MOD_CTRL / MOD_SUPER. Right Alt counts as Alt only when it is
//    not acting as AltGr (KMOD_MODE); text composed through AltGr is delivered as Event::Text.
#include "window.h"
#include "gl.h"
#include <SDL.h>
#include <clocale>
#include <cstdio>
#include <cstring>
#include <strings.h>

namespace canopy::gfx {

namespace {

// Internal per-window state that we do not want to expose through window.h.
struct WinState {
    std::string last_key_text;   // text we attached to the most recent Key event ("" if none)
    bool suppress_next_text = false;
    bool have_mouse_pos = false;
};
WinState g_state;   // one window per process is all Canopy needs

uint32_t translate_mods(SDL_Keymod m) {
    uint32_t r = 0;
    if (m & KMOD_SHIFT) r |= MOD_SHIFT;
    if (m & KMOD_CTRL) r |= MOD_CTRL;
    if (m & KMOD_GUI) r |= MOD_SUPER;
    // Left Alt is always Alt. Right Alt is Alt unless it acts as AltGr (level-3 shift).
    if (m & KMOD_LALT) r |= MOD_ALT;
    else if ((m & KMOD_RALT) && !(m & KMOD_MODE)) r |= MOD_ALT;
    return r;
}

bool mods_have_altgr(SDL_Keymod m) { return (m & KMOD_MODE) || (m & KMOD_RALT); }

// Append UTF-8 for one code point.
void put_utf8(std::string& s, char32_t cp) {
    if (cp < 0x80) s += (char)cp;
    else if (cp < 0x800) { s += (char)(0xC0 | (cp >> 6)); s += (char)(0x80 | (cp & 0x3F)); }
    else if (cp < 0x10000) { s += (char)(0xE0 | (cp >> 12)); s += (char)(0x80 | ((cp >> 6) & 0x3F)); s += (char)(0x80 | (cp & 0x3F)); }
    else { s += (char)(0xF0 | (cp >> 18)); s += (char)(0x80 | ((cp >> 12) & 0x3F)); s += (char)(0x80 | ((cp >> 6) & 0x3F)); s += (char)(0x80 | (cp & 0x3F)); }
}

// Maps an SDL keycode to a canopy::Key. Returns Key::Char for printable keys (keysym set).
// numlock: state of NumLock, used to turn keypad digits into navigation keys when off.
Key translate_key(SDL_Keycode kc, bool numlock, char32_t* keysym) {
    *keysym = 0;
    switch (kc) {
    case SDLK_RETURN: case SDLK_RETURN2: return Key::Enter;
    case SDLK_TAB: return Key::Tab;
    case SDLK_BACKSPACE: return Key::Backspace;
    case SDLK_ESCAPE: return Key::Escape;
    case SDLK_SPACE: return Key::Space;
    case SDLK_INSERT: return Key::Insert;
    case SDLK_DELETE: return Key::Delete;
    case SDLK_HOME: return Key::Home;
    case SDLK_END: return Key::End;
    case SDLK_PAGEUP: return Key::PageUp;
    case SDLK_PAGEDOWN: return Key::PageDown;
    case SDLK_UP: return Key::Up;
    case SDLK_DOWN: return Key::Down;
    case SDLK_LEFT: return Key::Left;
    case SDLK_RIGHT: return Key::Right;
    case SDLK_F1: return Key::F1;   case SDLK_F2: return Key::F2;   case SDLK_F3: return Key::F3;
    case SDLK_F4: return Key::F4;   case SDLK_F5: return Key::F5;   case SDLK_F6: return Key::F6;
    case SDLK_F7: return Key::F7;   case SDLK_F8: return Key::F8;   case SDLK_F9: return Key::F9;
    case SDLK_F10: return Key::F10; case SDLK_F11: return Key::F11; case SDLK_F12: return Key::F12;
    case SDLK_F13: return Key::F13; case SDLK_F14: return Key::F14; case SDLK_F15: return Key::F15;
    case SDLK_F16: return Key::F16; case SDLK_F17: return Key::F17; case SDLK_F18: return Key::F18;
    case SDLK_F19: return Key::F19; case SDLK_F20: return Key::F20;
    case SDLK_KP_0: return numlock ? Key::KP0 : Key::Insert;
    case SDLK_KP_1: return numlock ? Key::KP1 : Key::End;
    case SDLK_KP_2: return numlock ? Key::KP2 : Key::Down;
    case SDLK_KP_3: return numlock ? Key::KP3 : Key::PageDown;
    case SDLK_KP_4: return numlock ? Key::KP4 : Key::Left;
    case SDLK_KP_5: return Key::KP5;
    case SDLK_KP_6: return numlock ? Key::KP6 : Key::Right;
    case SDLK_KP_7: return numlock ? Key::KP7 : Key::Home;
    case SDLK_KP_8: return numlock ? Key::KP8 : Key::Up;
    case SDLK_KP_9: return numlock ? Key::KP9 : Key::PageUp;
    case SDLK_KP_PERIOD: case SDLK_KP_COMMA: case SDLK_KP_DECIMAL: return numlock ? Key::KPDecimal : Key::Delete;
    case SDLK_KP_DIVIDE: return Key::KPDivide;
    case SDLK_KP_MULTIPLY: return Key::KPMultiply;
    case SDLK_KP_MINUS: return Key::KPMinus;
    case SDLK_KP_PLUS: return Key::KPPlus;
    case SDLK_KP_ENTER: return Key::KPEnter;
    case SDLK_KP_EQUALS: case SDLK_KP_EQUALSAS400: return Key::KPEquals;
    case SDLK_MENU: case SDLK_APPLICATION: return Key::Menu;
    case SDLK_PRINTSCREEN: return Key::PrintScreen;
    default: break;
    }
    // Keycodes below the scancode mask are Unicode code points of the unshifted key.
    if (!(kc & SDLK_SCANCODE_MASK) && kc >= 0x20 && kc != 0x7F) {
        *keysym = (char32_t)kc;
        return Key::Char;
    }
    return Key::None;   // modifiers, media keys, ...
}

MouseButton translate_button(uint8_t b) {
    switch (b) {
    case SDL_BUTTON_LEFT: return MouseButton::Left;
    case SDL_BUTTON_MIDDLE: return MouseButton::Middle;
    case SDL_BUTTON_RIGHT: return MouseButton::Right;
    case SDL_BUTTON_X1: return MouseButton::Back;
    case SDL_BUTTON_X2: return MouseButton::Forward;
    default: return MouseButton::None;
    }
}

} // namespace

Window::Window() {}

Window::~Window() {
    for (void*& c : cursors_) {
        if (c) SDL_FreeCursor(static_cast<SDL_Cursor*>(c));
        c = nullptr;
    }
    if (ctx_) SDL_GL_DeleteContext(static_cast<SDL_GLContext>(ctx_));
    if (win_) SDL_DestroyWindow(static_cast<SDL_Window*>(win_));
    if (ctx_ || win_) SDL_Quit();
}

bool Window::create(const std::string& title, int w, int h, std::string* err) {
    // SDL's X11 backend converts titles/IME text with the C library's LC_CTYPE; in the "C"
    // locale non-ASCII titles silently fail (only WM_NAME gets set). A terminal emulator needs
    // a UTF-8 ctype anyway, so pick the environment's locale and fall back to C.UTF-8.
    {
        const char* loc = setlocale(LC_CTYPE, "");
        bool utf8 = loc && (strcasestr(loc, "utf-8") || strcasestr(loc, "utf8"));
        if (!utf8) setlocale(LC_CTYPE, "C.UTF-8");
    }
    SDL_SetHint(SDL_HINT_VIDEO_X11_NET_WM_BYPASS_COMPOSITOR, "0");
    SDL_SetHint(SDL_HINT_IME_SHOW_UI, "1");
    SDL_SetHint(SDL_HINT_MOUSE_FOCUS_CLICKTHROUGH, "1");
    SDL_SetHint(SDL_HINT_VIDEO_ALLOW_SCREENSAVER, "1");

    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS) != 0) {
        if (err) *err = std::string("SDL_Init: ") + SDL_GetError();
        return false;
    }
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    SDL_GL_SetAttribute(SDL_GL_RED_SIZE, 8);
    SDL_GL_SetAttribute(SDL_GL_GREEN_SIZE, 8);
    SDL_GL_SetAttribute(SDL_GL_BLUE_SIZE, 8);
    SDL_GL_SetAttribute(SDL_GL_ALPHA_SIZE, 8);
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 0);
    SDL_GL_SetAttribute(SDL_GL_STENCIL_SIZE, 0);
    SDL_GL_SetAttribute(SDL_GL_FRAMEBUFFER_SRGB_CAPABLE, 0);

    SDL_Window* win = SDL_CreateWindow(title.c_str(), SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, w, h,
                                       SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI);
    if (!win) {
        if (err) *err = std::string("SDL_CreateWindow: ") + SDL_GetError();
        SDL_Quit();
        return false;
    }
    SDL_GLContext ctx = SDL_GL_CreateContext(win);
    if (!ctx) {
        if (err) *err = std::string("SDL_GL_CreateContext: ") + SDL_GetError();
        SDL_DestroyWindow(win);
        SDL_Quit();
        return false;
    }
    SDL_GL_MakeCurrent(win, ctx);
    win_ = win;
    ctx_ = ctx;
    if (!gl_load()) {
        if (err) *err = "gl_load: required OpenGL 3.3 functions missing";
        return false;
    }
    set_vsync(true);

    SDL_GL_GetDrawableSize(win, &fb_w_, &fb_h_);
    int ww = 0, wh = 0;
    SDL_GetWindowSize(win, &ww, &wh);
    dpi_scale_ = (ww > 0) ? (float)fb_w_ / (float)ww : 1.f;
    focused_ = (SDL_GetWindowFlags(win) & SDL_WINDOW_INPUT_FOCUS) != 0;

    wake_event_ = SDL_RegisterEvents(1);
    if (wake_event_ == (uint32_t)-1) wake_event_ = 0;
    SDL_EventState(SDL_DROPFILE, SDL_ENABLE);
    SDL_StartTextInput();
    return true;
}

void Window::set_vsync(bool on) {
    if (!ctx_) return;
    if (on) {
        if (SDL_GL_SetSwapInterval(-1) != 0) SDL_GL_SetSwapInterval(1);   // adaptive if available
    } else {
        SDL_GL_SetSwapInterval(0);
    }
}

void Window::swap() {
    if (win_) SDL_GL_SwapWindow(static_cast<SDL_Window*>(win_));
}

void Window::set_title(const std::string& title) {
    if (win_) SDL_SetWindowTitle(static_cast<SDL_Window*>(win_), title.c_str());
}

void Window::wake() {
    if (!wake_event_) return;
    SDL_Event ev;
    SDL_memset(&ev, 0, sizeof(ev));
    ev.type = wake_event_;
    SDL_PushEvent(&ev);   // thread-safe
}

void Window::poll(std::vector<Event>& out, int timeout_ms) {
    SDL_Event sev;
    bool have = false;
    if (timeout_ms > 0) have = SDL_WaitEventTimeout(&sev, timeout_ms) != 0;
    else have = SDL_PollEvent(&sev) != 0;
    if (!have) return;
    SDL_Window* win = static_cast<SDL_Window*>(win_);
    const uint32_t my_id = win ? SDL_GetWindowID(win) : 0;
    const float s = dpi_scale_;

    do {
        Event e;
        switch (sev.type) {
        case SDL_QUIT:
            e.type = Event::Quit;
            out.push_back(std::move(e));
            break;
        case SDL_WINDOWEVENT:
            if (my_id && sev.window.windowID != my_id) break;
            switch (sev.window.event) {
            case SDL_WINDOWEVENT_SIZE_CHANGED:
            case SDL_WINDOWEVENT_RESIZED: {
                SDL_GL_GetDrawableSize(win, &fb_w_, &fb_h_);
                int ww = 0, wh = 0;
                SDL_GetWindowSize(win, &ww, &wh);
                dpi_scale_ = (ww > 0) ? (float)fb_w_ / (float)ww : 1.f;
                e.type = Event::Resize;
                e.w = fb_w_;
                e.h = fb_h_;
                out.push_back(std::move(e));
                break;
            }
            case SDL_WINDOWEVENT_EXPOSED:
            case SDL_WINDOWEVENT_SHOWN:
            case SDL_WINDOWEVENT_RESTORED:
            case SDL_WINDOWEVENT_MAXIMIZED:
                e.type = Event::Expose;
                out.push_back(std::move(e));
                break;
            case SDL_WINDOWEVENT_FOCUS_GAINED:
                focused_ = true;
                e.type = Event::FocusIn;
                out.push_back(std::move(e));
                break;
            case SDL_WINDOWEVENT_FOCUS_LOST:
                focused_ = false;
                e.type = Event::FocusOut;
                out.push_back(std::move(e));
                break;
            case SDL_WINDOWEVENT_CLOSE:
                e.type = Event::Quit;
                out.push_back(std::move(e));
                break;
            default: break;
            }
            break;
        case SDL_KEYDOWN: {
            SDL_Keymod km = (SDL_Keymod)sev.key.keysym.mod;
            e.type = Event::Key;
            e.repeat = sev.key.repeat != 0;
            e.mods = translate_mods(km);
            e.key.mods = e.mods;
            e.key.key = translate_key(sev.key.keysym.sym, (km & KMOD_NUM) != 0, &e.key.keysym);
            g_state.last_key_text.clear();
            g_state.suppress_next_text = false;
            if (e.key.key == Key::None) break;   // bare modifier etc.: not delivered
            if (e.key.key == Key::Char) {
                bool ctrl = (e.mods & MOD_CTRL) != 0;
                bool alt = (e.mods & MOD_ALT) != 0;
                if (ctrl || alt) {
                    // The UI encodes this itself; give it the best text we know (shift applied to letters).
                    char32_t c = e.key.keysym;
                    if ((e.mods & MOD_SHIFT) && c >= 'a' && c <= 'z') c = c - 'a' + 'A';
                    put_utf8(e.key.text, c);
                    g_state.last_key_text = e.key.text;
                    // SDL only sends TEXTINPUT with Alt held (never with Ctrl); drop that duplicate,
                    // except AltGr composition which produces different text (handled in TEXTINPUT).
                    g_state.suppress_next_text = true;
                }
            } else {
                // Non-Char keys that also produce text (Space, keypad with NumLock): UI encodes from Key.
                g_state.suppress_next_text = true;
            }
            out.push_back(std::move(e));
            break;
        }
        case SDL_KEYUP:
            break;   // the UI has no use for key releases in v1
        case SDL_TEXTINPUT: {
            SDL_Keymod km = SDL_GetModState();
            std::string text = sev.text.text;
            if (text.empty()) break;
            if (km & KMOD_CTRL) break;   // never text with Ctrl (SDL normally filters this already)
            if (g_state.suppress_next_text) {
                g_state.suppress_next_text = false;
                // RAlt used as AltGr/level3 composes different text than the base key -> deliver it.
                bool composed = mods_have_altgr(km) && !g_state.last_key_text.empty() && text != g_state.last_key_text;
                if (!composed) break;
            }
            e.type = Event::Text;
            e.mods = translate_mods(km);
            e.key.key = Key::Char;
            e.key.mods = e.mods;
            e.key.text = std::move(text);
            out.push_back(std::move(e));
            break;
        }
        case SDL_TEXTEDITING:
            break;   // IME preedit: not surfaced in v1
        case SDL_MOUSEBUTTONDOWN:
        case SDL_MOUSEBUTTONUP:
            if (sev.button.which == SDL_TOUCH_MOUSEID) break;
            e.type = sev.type == SDL_MOUSEBUTTONDOWN ? Event::MouseDown : Event::MouseUp;
            e.x = sev.button.x * s;
            e.y = sev.button.y * s;
            e.button = translate_button(sev.button.button);
            e.clicks = sev.button.clicks;
            e.mods = translate_mods(SDL_GetModState());
            out.push_back(std::move(e));
            break;
        case SDL_MOUSEMOTION: {
            if (sev.motion.which == SDL_TOUCH_MOUSEID) break;
            e.type = Event::MouseMove;
            e.x = sev.motion.x * s;
            e.y = sev.motion.y * s;
            uint32_t st = sev.motion.state;
            if (st & SDL_BUTTON_LMASK) e.button = MouseButton::Left;
            else if (st & SDL_BUTTON_RMASK) e.button = MouseButton::Right;
            else if (st & SDL_BUTTON_MMASK) e.button = MouseButton::Middle;
            else if (st & SDL_BUTTON_X1MASK) e.button = MouseButton::Back;
            else if (st & SDL_BUTTON_X2MASK) e.button = MouseButton::Forward;
            e.mods = translate_mods(SDL_GetModState());
            out.push_back(std::move(e));
            break;
        }
        case SDL_MOUSEWHEEL: {
            if (sev.wheel.which == SDL_TOUCH_MOUSEID) break;
            e.type = Event::Wheel;
            float wx = sev.wheel.preciseX, wy = sev.wheel.preciseY;
            if (wx == 0.f && wy == 0.f) { wx = (float)sev.wheel.x; wy = (float)sev.wheel.y; }
            if (sev.wheel.direction == SDL_MOUSEWHEEL_FLIPPED) { wx = -wx; wy = -wy; }
            e.wheel_x = wx;
            e.wheel_y = wy;
            // SDL 2.26+ gives the mouse position in the wheel event; older builds leave it 0.
            int mx = sev.wheel.mouseX, my = sev.wheel.mouseY;
            if (mx == 0 && my == 0) SDL_GetMouseState(&mx, &my);
            e.x = mx * s;
            e.y = my * s;
            e.mods = translate_mods(SDL_GetModState());
            out.push_back(std::move(e));
            break;
        }
        case SDL_DROPFILE:
            if (sev.drop.file) {
                e.type = Event::DropFile;
                e.path = sev.drop.file;
                SDL_free(sev.drop.file);
                out.push_back(std::move(e));
            }
            break;
        case SDL_DROPTEXT:
            if (sev.drop.file) {
                e.type = Event::Text;
                e.key.key = Key::Char;
                e.key.text = sev.drop.file;
                SDL_free(sev.drop.file);
                out.push_back(std::move(e));
            }
            break;
        case SDL_CLIPBOARDUPDATE:
            e.type = Event::ClipboardUpdate;
            out.push_back(std::move(e));
            break;
        default:
            if (wake_event_ && sev.type == wake_event_) {
                e.type = Event::Wake;
                out.push_back(std::move(e));
            }
            break;
        }
    } while (SDL_PollEvent(&sev));
}

void Window::set_clipboard(const std::string& text) { SDL_SetClipboardText(text.c_str()); }

std::string Window::clipboard() const {
    std::string r;
    if (SDL_HasClipboardText()) {
        char* t = SDL_GetClipboardText();
        if (t) { r = t; SDL_free(t); }
    }
    return r;
}

void Window::set_primary_selection(const std::string& text) {
#if SDL_VERSION_ATLEAST(2, 26, 0)
    SDL_SetPrimarySelectionText(text.c_str());
#else
    (void)text;
#endif
}

std::string Window::primary_selection() const {
    std::string r;
#if SDL_VERSION_ATLEAST(2, 26, 0)
    if (SDL_HasPrimarySelectionText()) {
        char* t = SDL_GetPrimarySelectionText();
        if (t) { r = t; SDL_free(t); }
    }
#endif
    return r;
}

void Window::start_text_input() { SDL_StartTextInput(); }
void Window::stop_text_input() { SDL_StopTextInput(); }

void Window::set_text_input_rect(int x, int y, int w, int h) {
    // Takes framebuffer pixels; SDL wants logical window coordinates.
    float s = dpi_scale_ > 0 ? dpi_scale_ : 1.f;
    SDL_Rect r{ (int)(x / s), (int)(y / s), (int)(w / s), (int)(h / s) };
    SDL_SetTextInputRect(&r);
}

void Window::set_cursor(int sdl_system_cursor) {
    if (sdl_system_cursor < 0 || sdl_system_cursor >= (int)(sizeof(cursors_) / sizeof(cursors_[0]))) return;
    if (!cursors_[sdl_system_cursor])
        cursors_[sdl_system_cursor] = SDL_CreateSystemCursor((SDL_SystemCursor)sdl_system_cursor);
    SDL_Cursor* c = static_cast<SDL_Cursor*>(cursors_[sdl_system_cursor]);
    if (c && SDL_GetCursor() != c) SDL_SetCursor(c);
}

void Window::request_attention() {
    if (win_ && !focused_) SDL_FlashWindow(static_cast<SDL_Window*>(win_), SDL_FLASH_UNTIL_FOCUSED);
}

double Window::now() const {
    static const Uint64 freq = SDL_GetPerformanceFrequency();
    return (double)SDL_GetPerformanceCounter() / (double)freq;
}

} // namespace canopy::gfx
