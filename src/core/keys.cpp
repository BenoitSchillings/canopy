// Key / mouse / paste encoding following xterm conventions.
//
// Note for the UI layer: encode_key() does not know about Canopy's own bindings (Ctrl+Shift+C/V,
// F1/F2 overview/sidebar, ...). The UI must intercept its bindings BEFORE calling encode_key.
// Ctrl+Shift+<letter> deliberately returns "" so that unbound combos never leak odd bytes.
#include "keys.h"
#include "unicode.h"
#include <cstdio>

namespace canopy {

namespace {

int xterm_mod_param(uint32_t mods) {
    int m = 0;
    if (mods & MOD_SHIFT) m |= 1;
    if (mods & MOD_ALT) m |= 2;
    if (mods & MOD_CTRL) m |= 4;
    return m ? m + 1 : 0;
}

std::string csi_mod(const char* prefix, int mod, const char* fin) {
    char buf[32];
    if (mod) snprintf(buf, sizeof buf, "\x1b[%s;%d%s", prefix, mod, fin);
    else snprintf(buf, sizeof buf, "\x1b[%s%s", prefix, fin);
    return buf;
}

// Control-character mapping for Ctrl+<key> (key is the unshifted keysym or text char).
bool ctrl_char(char32_t c, char* out) {
    if (c >= 'a' && c <= 'z') { *out = (char)(c - 'a' + 1); return true; }
    if (c >= 'A' && c <= 'Z') { *out = (char)(c - 'A' + 1); return true; }
    switch (c) {
    case ' ': case '@': case '2': *out = 0; return true;
    case '[': case '3': *out = 0x1b; return true;
    case '\\': case '4': *out = 0x1c; return true;
    case ']': case '5': *out = 0x1d; return true;
    case '^': case '6': *out = 0x1e; return true;
    case '_': case '7': case '-': case '/': *out = 0x1f; return true;
    case '?': case '8': *out = 0x7f; return true;
    default: return false;
    }
}

} // namespace

std::string encode_key(const KeyEvent& ev, const TerminalModes& modes) {
    const bool shift = ev.mods & MOD_SHIFT, alt = ev.mods & MOD_ALT, ctrl = ev.mods & MOD_CTRL;
    const int mod = xterm_mod_param(ev.mods);
    auto alt_prefix = [&](std::string s) {
        if (alt && modes.alt_sends_esc && !s.empty()) s.insert(s.begin(), '\x1b');
        return s;
    };
    auto cursor_key = [&](char fin) {
        if (mod) return csi_mod("1", mod, std::string(1, fin).c_str());
        return std::string(modes.app_cursor_keys ? "\x1bO" : "\x1b[") + fin;
    };
    auto tilde_key = [&](int code) {
        char num[8]; snprintf(num, sizeof num, "%d", code);
        return csi_mod(num, mod, "~");
    };
    auto ss3_key = [&](char fin) {
        if (mod) return csi_mod("1", mod, std::string(1, fin).c_str());
        return std::string("\x1bO") + fin;
    };
    auto csi_u = [&](uint32_t codepoint) {
        char buf[32];
        if (mod) snprintf(buf, sizeof buf, "\x1b[%u;%du", (unsigned)codepoint, mod);
        else snprintf(buf, sizeof buf, "\x1b[%uu", (unsigned)codepoint);
        return std::string(buf);
    };
    auto modify_other = [&](uint32_t codepoint) {
        char buf[32];
        snprintf(buf, sizeof buf, "\x1b[27;%d;%u~", mod, (unsigned)codepoint);
        return std::string(buf);
    };

    switch (ev.key) {
    case Key::None: return "";
    case Key::Char: {
        char32_t base = ev.keysym;
        if (base == 0 && !ev.text.empty()) utf8_decode_one(ev.text, &base);
        if (ctrl && shift && ((base >= 'a' && base <= 'z') || (base >= 'A' && base <= 'Z'))) return "";   // UI-reserved
        if (modes.kitty_keyboard && (ctrl || alt) && base >= 0x20) {
            char32_t lower_cp = (base >= 'A' && base <= 'Z') ? (base + 32) : base;
            return csi_u(lower_cp);
        }
        if (modes.modify_other_keys >= 2 && (ctrl || alt) && base >= 0x20) {
            return modify_other(base);
        }
        if (ctrl) {
            char c;
            if (ctrl_char(base, &c)) return alt_prefix(std::string(1, c));
            if (modes.modify_other_keys >= 1 && base >= 0x20) return modify_other(base);
            if (!ev.text.empty() && ((unsigned char)ev.text[0] < 0x80)) {
                char32_t t = (unsigned char)ev.text[0];
                if (ctrl_char(t, &c)) return alt_prefix(std::string(1, c));
            }
            return alt_prefix(ev.text);
        }
        if (ev.text.empty()) return "";
        return alt_prefix(ev.text);
    }
    case Key::Space:
        if (modes.kitty_keyboard && mod) return csi_u(32);
        if (modes.modify_other_keys >= 1 && shift && !ctrl && !alt) return modify_other(32);
        if (ctrl) return alt_prefix(std::string(1, '\0'));
        return alt_prefix(" ");
    case Key::Enter:
    case Key::KPEnter:
        if (ev.key == Key::KPEnter && modes.app_keypad && !mod) return "\x1bOM";
        if (modes.kitty_keyboard && mod) return csi_u(13);
        if (modes.modify_other_keys >= 1 && mod) return modify_other(13);
        if (shift && !ctrl) return "\x1b\r";   // multi-line newline in claude, agy, Ink TUIs, and readline
        if (ctrl && !shift) return alt_prefix("\n");
        return alt_prefix(modes.lnm ? "\r\n" : "\r");
    case Key::Tab:
        if (modes.kitty_keyboard && (ctrl || alt)) return csi_u(9);
        if (modes.modify_other_keys >= 1 && (ctrl || alt)) return modify_other(9);
        if (shift) return alt_prefix("\x1b[Z");
        return alt_prefix("\t");
    case Key::Backspace:
        if (modes.kitty_keyboard && mod) return csi_u(127);
        if (modes.modify_other_keys >= 1 && mod) return modify_other(127);
        return alt_prefix(ctrl ? "\x08" : "\x7f");
    case Key::Escape:
        if (modes.kitty_keyboard && mod) return csi_u(27);
        if (modes.modify_other_keys >= 1 && mod) return modify_other(27);
        return alt_prefix("\x1b");
    case Key::Up: return cursor_key('A');
    case Key::Down: return cursor_key('B');
    case Key::Right: return cursor_key('C');
    case Key::Left: return cursor_key('D');
    case Key::Home: return cursor_key('H');
    case Key::End: return cursor_key('F');
    case Key::Insert: return tilde_key(2);
    case Key::Delete: return tilde_key(3);
    case Key::PageUp: return tilde_key(5);
    case Key::PageDown: return tilde_key(6);
    case Key::F1: return ss3_key('P');
    case Key::F2: return ss3_key('Q');
    case Key::F3: return ss3_key('R');
    case Key::F4: return ss3_key('S');
    case Key::F5: return tilde_key(15);
    case Key::F6: return tilde_key(17);
    case Key::F7: return tilde_key(18);
    case Key::F8: return tilde_key(19);
    case Key::F9: return tilde_key(20);
    case Key::F10: return tilde_key(21);
    case Key::F11: return tilde_key(23);
    case Key::F12: return tilde_key(24);
    case Key::F13: return tilde_key(25);
    case Key::F14: return tilde_key(26);
    case Key::F15: return tilde_key(28);
    case Key::F16: return tilde_key(29);
    case Key::F17: return tilde_key(31);
    case Key::F18: return tilde_key(32);
    case Key::F19: return tilde_key(33);
    case Key::F20: return tilde_key(34);
    case Key::Menu: return tilde_key(29);
    case Key::PrintScreen: return "";
    default: break;
    }

    // Keypad.
    char plain = 0, app = 0;
    switch (ev.key) {
    case Key::KP0: plain = '0'; app = 'p'; break;
    case Key::KP1: plain = '1'; app = 'q'; break;
    case Key::KP2: plain = '2'; app = 'r'; break;
    case Key::KP3: plain = '3'; app = 's'; break;
    case Key::KP4: plain = '4'; app = 't'; break;
    case Key::KP5: plain = '5'; app = 'u'; break;
    case Key::KP6: plain = '6'; app = 'v'; break;
    case Key::KP7: plain = '7'; app = 'w'; break;
    case Key::KP8: plain = '8'; app = 'x'; break;
    case Key::KP9: plain = '9'; app = 'y'; break;
    case Key::KPDecimal: plain = '.'; app = 'n'; break;
    case Key::KPDivide: plain = '/'; app = 'o'; break;
    case Key::KPMultiply: plain = '*'; app = 'j'; break;
    case Key::KPMinus: plain = '-'; app = 'm'; break;
    case Key::KPPlus: plain = '+'; app = 'k'; break;
    case Key::KPEquals: plain = '='; app = 'X'; break;
    default: return "";
    }
    if (modes.app_keypad) {
        if (mod) { char buf[16]; snprintf(buf, sizeof buf, "\x1bO%d%c", mod, app); return buf; }
        return std::string("\x1bO") + app;
    }
    if (ctrl) { char c; if (ctrl_char((char32_t)plain, &c)) return alt_prefix(std::string(1, c)); }
    return alt_prefix(std::string(1, plain));
}

std::string encode_mouse(const MouseEvent& ev, const TerminalModes& modes) {
    const MouseMode mode = modes.mouse;
    if (mode == MouseMode::None) return "";
    const bool wheel = ev.button >= MouseButton::WheelUp && ev.button <= MouseButton::WheelRight;
    const bool motion = ev.action == MouseAction::Motion;

    if (mode == MouseMode::X10) {
        if (ev.action != MouseAction::Press || wheel) return "";
    } else if (mode == MouseMode::Normal) {
        if (motion) return "";
    } else if (mode == MouseMode::ButtonEvent) {
        if (motion && ev.button == MouseButton::None) return "";
    }
    if (wheel && ev.action == MouseAction::Release) return "";

    int b;
    switch (ev.button) {
    case MouseButton::None: b = 3; break;
    case MouseButton::Left: b = 0; break;
    case MouseButton::Middle: b = 1; break;
    case MouseButton::Right: b = 2; break;
    case MouseButton::WheelUp: b = 64; break;
    case MouseButton::WheelDown: b = 65; break;
    case MouseButton::WheelLeft: b = 66; break;
    case MouseButton::WheelRight: b = 67; break;
    case MouseButton::Back: b = 128; break;
    case MouseButton::Forward: b = 129; break;
    default: b = 3; break;
    }
    if (mode != MouseMode::X10) {
        if (ev.mods & MOD_SHIFT) b |= 4;
        if (ev.mods & MOD_ALT) b |= 8;
        if (ev.mods & MOD_CTRL) b |= 16;
    }
    if (motion) b |= 32;
    const bool release = ev.action == MouseAction::Release;

    char buf[64];
    switch (modes.mouse_enc) {
    case MouseEncoding::Sgr:
    case MouseEncoding::SgrPixels: {
        int x = ev.col + 1, y = ev.row + 1;
        if (modes.mouse_enc == MouseEncoding::SgrPixels) { x = ev.px + 1; y = ev.py + 1; }
        snprintf(buf, sizeof buf, "\x1b[<%d;%d;%d%c", b, x, y, release ? 'm' : 'M');
        return buf;
    }
    case MouseEncoding::Urxvt: {
        int bb = release ? ((b & ~3) | 3) : b;
        snprintf(buf, sizeof buf, "\x1b[%d;%d;%dM", bb + 32, ev.col + 1, ev.row + 1);
        return buf;
    }
    case MouseEncoding::Utf8: {
        int bb = release ? ((b & ~3) | 3) : b;
        std::string s = "\x1b[M";
        s.push_back((char)(32 + bb));
        auto enc = [&](int v) { char32_t c = (char32_t)std::min(v + 33, 2047); if (c < 128) s.push_back((char)c); else utf8_append(s, c); };
        enc(ev.col); enc(ev.row);
        return s;
    }
    case MouseEncoding::Default:
    default: {
        int bb = release ? ((b & ~3) | 3) : b;
        std::string s = "\x1b[M";
        s.push_back((char)(32 + bb));
        s.push_back((char)(32 + std::min(ev.col + 1, 223)));
        s.push_back((char)(32 + std::min(ev.row + 1, 223)));
        return s;
    }
    }
}

std::string encode_paste(std::string_view text, const TerminalModes& modes) {
    std::string out;
    out.reserve(text.size() + 16);
    if (modes.bracketed_paste) out += "\x1b[200~";
    for (size_t i = 0; i < text.size(); ++i) {
        char c = text[i];
        if (c == '\r') { out.push_back('\r'); if (i + 1 < text.size() && text[i + 1] == '\n') ++i; }
        else if (c == '\n') out.push_back('\r');
        else if (c == '\x1b' && modes.bracketed_paste && text.substr(i, 6) == "\x1b[201~") { i += 5; }  // never let pasted text end the bracket
        else out.push_back(c);
    }
    if (modes.bracketed_paste) out += "\x1b[201~";
    return out;
}

} // namespace canopy
