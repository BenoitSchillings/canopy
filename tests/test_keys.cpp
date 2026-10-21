// Key / mouse / paste encoding tests.
#include "core/keys.h"
#include <cstdio>
#include <string>

using namespace canopy;

static int g_fail = 0, g_pass = 0;
static std::string vis(const std::string& s) { std::string o; for (unsigned char c : s) { if (c < 0x20 || c >= 0x7f) { char b[8]; snprintf(b, sizeof b, "\\x%02x", c); o += b; } else o.push_back((char)c); } return o; }
#define CHECK_EQ(a, b) do { std::string _a = (a), _b = (b); if (_a == _b) ++g_pass; else { ++g_fail; fprintf(stderr, "FAIL %s:%d: %s -> \"%s\", want \"%s\"\n", __FILE__, __LINE__, #a, vis(_a).c_str(), vis(_b).c_str()); } } while (0)

static KeyEvent K(Key k, uint32_t mods = 0, const char* text = "", char32_t sym = 0) { KeyEvent e; e.key = k; e.mods = mods; e.text = text; e.keysym = sym; return e; }
static KeyEvent C(const char* text, uint32_t mods = 0, char32_t sym = 0) { KeyEvent e; e.key = Key::Char; e.mods = mods; e.text = text; e.keysym = sym; return e; }

int main() {
    TerminalModes normal;
    TerminalModes app; app.app_cursor_keys = true; app.app_keypad = true;
    TerminalModes lnm; lnm.lnm = true;
    TerminalModes noalt; noalt.alt_sends_esc = false;
    TerminalModes kitty; kitty.kitty_keyboard = true; kitty.kitty_flags = 1;
    TerminalModes mok1; mok1.modify_other_keys = 1;
    TerminalModes mok2; mok2.modify_other_keys = 2;

    struct Case { KeyEvent ev; const TerminalModes* m; const char* want; };
    const Case cases[] = {
        {C("a"), &normal, "a"},
        {C("A", MOD_SHIFT), &normal, "A"},
        {C("\xc3\xa9"), &normal, "\xc3\xa9"},
        {C("", MOD_CTRL, 'a'), &normal, "\x01"},
        {C("", MOD_CTRL, 'z'), &normal, "\x1a"},
        {C("", MOD_CTRL, '['), &normal, "\x1b"},
        {C("", MOD_CTRL, '\\'), &normal, "\x1c"},
        {C("", MOD_CTRL, ']'), &normal, "\x1d"},
        {C("", MOD_CTRL, '^'), &normal, "\x1e"},
        {C("", MOD_CTRL, '_'), &normal, "\x1f"},
        {C("", MOD_CTRL, '-'), &normal, "\x1f"},
        {C("", MOD_CTRL, '2'), &normal, std::string("\0", 1).c_str()},   // NUL (compared below specially)
        {C("", MOD_CTRL, '?'), &normal, "\x7f"},
        {C("", MOD_CTRL | MOD_SHIFT, 'c'), &normal, ""},                   // reserved for the UI
        {C("", MOD_CTRL | MOD_ALT, 'a'), &normal, "\x1b\x01"},
        {C("x", MOD_ALT), &normal, "\x1bx"},
        {C("x", MOD_ALT), &noalt, "x"},
        {K(Key::Space), &normal, " "},
        {K(Key::Space, MOD_CTRL), &normal, std::string("\0", 1).c_str()},
        {K(Key::Space, MOD_ALT), &normal, "\x1b "},
        {K(Key::Enter), &normal, "\r"},
        {K(Key::Enter), &lnm, "\r\n"},
        {K(Key::Enter, MOD_SHIFT), &normal, "\x1b\r"},
        {K(Key::Enter, MOD_ALT), &normal, "\x1b\r"},
        {K(Key::Enter, MOD_CTRL), &normal, "\n"},
        {K(Key::Enter), &kitty, "\r"},
        {K(Key::Enter, MOD_SHIFT), &kitty, "\x1b[13;2u"},
        {K(Key::Enter, MOD_ALT), &kitty, "\x1b[13;3u"},
        {K(Key::Enter, MOD_CTRL), &kitty, "\x1b[13;5u"},
        {C("", MOD_CTRL, 'a'), &kitty, "\x1b[97;5u"},
        {K(Key::Enter, MOD_SHIFT), &mok1, "\x1b[27;2;13~"},
        {K(Key::Enter, MOD_CTRL), &mok1, "\x1b[27;5;13~"},
        {C("", MOD_CTRL, 'a'), &mok2, "\x1b[27;5;97~"},
        {K(Key::Tab), &normal, "\t"},
        {K(Key::Tab, MOD_SHIFT), &normal, "\x1b[Z"},
        {K(Key::Backspace), &normal, "\x7f"},
        {K(Key::Backspace, MOD_CTRL), &normal, "\x08"},
        {K(Key::Backspace, MOD_ALT), &normal, "\x1b\x7f"},
        {K(Key::Escape), &normal, "\x1b"},
        {K(Key::Escape, MOD_ALT), &normal, "\x1b\x1b"},
        {K(Key::Up), &normal, "\x1b[A"},
        {K(Key::Up), &app, "\x1bOA"},
        {K(Key::Down), &app, "\x1bOB"},
        {K(Key::Right), &normal, "\x1b[C"},
        {K(Key::Left), &normal, "\x1b[D"},
        {K(Key::Home), &normal, "\x1b[H"},
        {K(Key::End), &app, "\x1bOF"},
        {K(Key::Up, MOD_SHIFT), &normal, "\x1b[1;2A"},
        {K(Key::Up, MOD_ALT), &normal, "\x1b[1;3A"},
        {K(Key::Up, MOD_CTRL), &app, "\x1b[1;5A"},
        {K(Key::Right, MOD_CTRL | MOD_SHIFT), &normal, "\x1b[1;6C"},
        {K(Key::Left, MOD_CTRL | MOD_ALT | MOD_SHIFT), &normal, "\x1b[1;8D"},
        {K(Key::Home, MOD_CTRL), &normal, "\x1b[1;5H"},
        {K(Key::Insert), &normal, "\x1b[2~"},
        {K(Key::Delete), &normal, "\x1b[3~"},
        {K(Key::Delete, MOD_CTRL), &normal, "\x1b[3;5~"},
        {K(Key::PageUp), &normal, "\x1b[5~"},
        {K(Key::PageDown, MOD_SHIFT), &normal, "\x1b[6;2~"},
        {K(Key::F1), &normal, "\x1bOP"},
        {K(Key::F2), &normal, "\x1bOQ"},
        {K(Key::F3), &normal, "\x1bOR"},
        {K(Key::F4), &normal, "\x1bOS"},
        {K(Key::F1, MOD_SHIFT), &normal, "\x1b[1;2P"},
        {K(Key::F5), &normal, "\x1b[15~"},
        {K(Key::F6), &normal, "\x1b[17~"},
        {K(Key::F7), &normal, "\x1b[18~"},
        {K(Key::F8), &normal, "\x1b[19~"},
        {K(Key::F9), &normal, "\x1b[20~"},
        {K(Key::F10), &normal, "\x1b[21~"},
        {K(Key::F11), &normal, "\x1b[23~"},
        {K(Key::F12), &normal, "\x1b[24~"},
        {K(Key::F12, MOD_CTRL), &normal, "\x1b[24;5~"},
        {K(Key::F13), &normal, "\x1b[25~"},
        {K(Key::F20), &normal, "\x1b[34~"},
        {K(Key::KP5), &normal, "5"},
        {K(Key::KP5), &app, "\x1bOu"},
        {K(Key::KPEnter), &normal, "\r"},
        {K(Key::KPEnter), &app, "\x1bOM"},
        {K(Key::KPPlus), &app, "\x1bOk"},
        {K(Key::KPDecimal), &normal, "."},
        {K(Key::Menu), &normal, "\x1b[29~"},
        {K(Key::None), &normal, ""},
    };
    for (const Case& c : cases) {
        std::string got = encode_key(c.ev, *c.m);
        std::string want = c.want;
        // a few entries encode a NUL byte, which the const char* initializer cannot carry
        if ((c.ev.key == Key::Space && (c.ev.mods & MOD_CTRL)) || (c.ev.key == Key::Char && (c.ev.mods & MOD_CTRL) && c.ev.keysym == '2')) want = std::string("\0", 1);
        if (got == want) ++g_pass; else { ++g_fail; fprintf(stderr, "FAIL key %d mods %u: got \"%s\" want \"%s\"\n", (int)c.ev.key, c.ev.mods, vis(got).c_str(), vis(want).c_str()); }
    }

    // ---- mouse ----
    TerminalModes m;
    MouseEvent e; e.col = 4; e.row = 2; e.button = MouseButton::Left; e.action = MouseAction::Press;
    CHECK_EQ(encode_mouse(e, m), "");                          // mode None: handled locally
    m.mouse = MouseMode::Normal;
    CHECK_EQ(encode_mouse(e, m), "\x1b[M \x25\x23");            // 32+0, 32+5, 32+3
    e.action = MouseAction::Release; CHECK_EQ(encode_mouse(e, m), "\x1b[M#\x25\x23");
    e.action = MouseAction::Motion; CHECK_EQ(encode_mouse(e, m), "");   // no motion in 1000
    m.mouse = MouseMode::ButtonEvent; CHECK_EQ(encode_mouse(e, m), "\x1b[M@\x25\x23");   // 32+32+0
    e.button = MouseButton::None; CHECK_EQ(encode_mouse(e, m), "");                       // motion without button
    m.mouse = MouseMode::AnyEvent; CHECK_EQ(encode_mouse(e, m), "\x1b[MC\x25\x23");       // 32+32+3
    m.mouse = MouseMode::X10; e.action = MouseAction::Press; e.button = MouseButton::Right; e.mods = MOD_CTRL;
    CHECK_EQ(encode_mouse(e, m), "\x1b[M\"\x25\x23");          // no modifiers in X10
    e.action = MouseAction::Release; CHECK_EQ(encode_mouse(e, m), "");
    m.mouse = MouseMode::Normal; e.action = MouseAction::Press; e.button = MouseButton::WheelDown; e.mods = 0;
    CHECK_EQ(encode_mouse(e, m), "\x1b[Ma\x25\x23");            // 32+65
    e.button = MouseButton::WheelUp; e.mods = MOD_SHIFT | MOD_ALT | MOD_CTRL;
    CHECK_EQ(encode_mouse(e, m), "\x1b[M\x7c\x25\x23");         // 32+64+4+8+16
    e.action = MouseAction::Release; CHECK_EQ(encode_mouse(e, m), "");   // wheel has no release
    // clamping in the default encoding
    e.action = MouseAction::Press; e.button = MouseButton::Left; e.mods = 0; e.col = 300; e.row = 250;
    CHECK_EQ(encode_mouse(e, m), "\x1b[M \xff\xff");
    // SGR
    m.mouse_enc = MouseEncoding::Sgr; e.col = 4; e.row = 2;
    CHECK_EQ(encode_mouse(e, m), "\x1b[<0;5;3M");
    e.action = MouseAction::Release; CHECK_EQ(encode_mouse(e, m), "\x1b[<0;5;3m");
    e.action = MouseAction::Motion; m.mouse = MouseMode::AnyEvent; e.button = MouseButton::Middle; e.mods = MOD_CTRL;
    CHECK_EQ(encode_mouse(e, m), "\x1b[<49;5;3M");             // 1+16+32
    e.col = 500; e.row = 400; e.action = MouseAction::Press; e.mods = 0; e.button = MouseButton::Left;
    CHECK_EQ(encode_mouse(e, m), "\x1b[<0;501;401M");
    // SGR pixels
    m.mouse_enc = MouseEncoding::SgrPixels; e.px = 100; e.py = 50;
    CHECK_EQ(encode_mouse(e, m), "\x1b[<0;101;51M");
    // urxvt
    m.mouse_enc = MouseEncoding::Urxvt; e.col = 4; e.row = 2;
    CHECK_EQ(encode_mouse(e, m), "\x1b[32;5;3M");
    e.action = MouseAction::Release; CHECK_EQ(encode_mouse(e, m), "\x1b[35;5;3M");
    // utf8 (1005)
    m.mouse_enc = MouseEncoding::Utf8; e.action = MouseAction::Press; e.col = 200; e.row = 2;
    CHECK_EQ(encode_mouse(e, m), "\x1b[M \xc3\xa9\x23");         // 200+33 = 233 -> U+00E9

    // ---- paste ----
    TerminalModes bp; bp.bracketed_paste = true;
    CHECK_EQ(encode_paste("a\nb\r\nc\rd", normal), "a\rb\rc\rd");
    CHECK_EQ(encode_paste("hi\n", bp), "\x1b[200~hi\r\x1b[201~");
    CHECK_EQ(encode_paste("x\x1b[201~y", bp), "\x1b[200~xy\x1b[201~");

    printf("test_keys: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
