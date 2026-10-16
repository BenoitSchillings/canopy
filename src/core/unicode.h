#pragma once
// Small locale-independent Unicode helpers used by the VT core. Implemented in cell.cpp.
#include <cstdint>
#include <string>
#include <string_view>

namespace canopy {

// Display width of a code point in terminal cells: 0 (combining/zero-width), 1, or 2 (East Asian
// Wide/Fullwidth, emoji presentation). Control characters return 0 here; callers filter them first.
int char_width(char32_t cp);
// True if the code point is a combining mark / zero-width joiner / variation selector.
bool is_combining(char32_t cp);

// Append the UTF-8 encoding of cp to out.
void utf8_append(std::string& out, char32_t cp);
// Decode one UTF-8 sequence at s[0..]; returns number of bytes consumed (>=1), cp set to U+FFFD on
// invalid input. Never consumes past the end of the view.
size_t utf8_decode_one(std::string_view s, char32_t* cp);
// Decode a whole string (invalid bytes become U+FFFD).
std::u32string utf8_to_u32(std::string_view s);
std::string u32_to_utf8(std::u32string_view s);

} // namespace canopy
