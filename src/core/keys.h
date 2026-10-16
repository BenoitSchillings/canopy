#pragma once
// Toolkit-independent key & mouse events and their encoding as terminal input bytes.
#include "terminal.h"
#include <string>

namespace canopy {

enum Mod : uint32_t { MOD_SHIFT = 1, MOD_ALT = 2, MOD_CTRL = 4, MOD_SUPER = 8 };

enum class Key : uint16_t {
    None = 0,
    // printable keys are delivered via KeyEvent::text (UTF-8) with Key::Char
    Char,
    Enter, Tab, Backspace, Escape, Space,
    Insert, Delete, Home, End, PageUp, PageDown,
    Up, Down, Left, Right,
    F1, F2, F3, F4, F5, F6, F7, F8, F9, F10, F11, F12,
    F13, F14, F15, F16, F17, F18, F19, F20,
    KP0, KP1, KP2, KP3, KP4, KP5, KP6, KP7, KP8, KP9,
    KPDecimal, KPDivide, KPMultiply, KPMinus, KPPlus, KPEnter, KPEquals,
    Menu, PrintScreen,
};

struct KeyEvent {
    Key key = Key::None;
    uint32_t mods = 0;          // Mod bits
    std::string text;           // UTF-8 text for Key::Char (already shifted, from text input / keysym)
    char32_t keysym = 0;        // base unshifted code point for Ctrl combos (e.g. 'a' for Ctrl+A), if known
};

// Returns bytes to write to the pty for this key under the terminal's current modes.
// Empty string means "not handled" (e.g. a bare modifier).
std::string encode_key(const KeyEvent& ev, const TerminalModes& modes);

enum class MouseButton : uint8_t { None = 0, Left = 1, Middle = 2, Right = 3, WheelUp = 4, WheelDown = 5, WheelLeft = 6, WheelRight = 7, Back = 8, Forward = 9 };
enum class MouseAction : uint8_t { Press, Release, Motion };

struct MouseEvent {
    MouseAction action = MouseAction::Press;
    MouseButton button = MouseButton::None;   // for Motion: button held (None if none)
    int col = 0, row = 0;                      // 0-based cell coordinates
    int px = 0, py = 0;                        // pixel coordinates within the terminal area (for 1016)
    uint32_t mods = 0;
};

// Returns bytes for a mouse event under the current mouse mode/encoding; empty if the
// terminal should handle it locally (mode None, or motion without button in mode 1002, ...).
std::string encode_mouse(const MouseEvent& ev, const TerminalModes& modes);

// Bracketed paste wrapping (if enabled) + newline normalization (\r\n, \n -> \r).
std::string encode_paste(std::string_view text, const TerminalModes& modes);

} // namespace canopy
