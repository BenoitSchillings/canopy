// Unit tests for the VT core (Terminal). Minimal self-contained harness; exit code = #failures.
#include "core/terminal.h"
#include "core/unicode.h"
#include <chrono>
#include <type_traits>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using namespace canopy;

static int g_fail = 0, g_pass = 0;
#define CHECK(c) do { if (c) ++g_pass; else { ++g_fail; fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)
#define CHECK_EQ(a, b) do { auto _a = (a); auto _b = (b); if (_a == _b) ++g_pass; else { ++g_fail; std::string sa = to_str(_a), sb = to_str(_b); fprintf(stderr, "FAIL %s:%d: %s == %s  (got \"%s\", want \"%s\")\n", __FILE__, __LINE__, #a, #b, sa.c_str(), sb.c_str()); } } while (0)

static std::string vis(const std::string& s) { std::string o; for (unsigned char c : s) { if (c < 0x20 || c == 0x7f) { char b[8]; snprintf(b, sizeof b, "\\x%02x", c); o += b; } else o.push_back((char)c); } return o; }
template <class T> std::string to_str(const T& v) {
    if constexpr (std::is_same_v<T, std::string>) return vis(v);
    else if constexpr (std::is_same_v<T, const char*>) return vis(std::string(v));
    else if constexpr (std::is_same_v<T, bool>) return v ? "true" : "false";
    else return std::to_string((long long)v);
}

static std::string row_text(const Terminal& t, int y) { return t.row(y).text(); }
static std::string screen_text(const Terminal& t) { std::string s; for (int y = 0; y < t.rows(); ++y) { s += row_text(t, y); s += '\n'; } return s; }

struct Replies {
    std::string out;
    void attach(Terminal& t) { t.on_write = [this](std::string_view s) { out.append(s); }; }
    std::string take() { std::string s; s.swap(out); return s; }
};

static void test_cursor_movement() {
    Terminal t(20, 5);
    t.feed("\x1b[3;4H");
    CHECK_EQ(t.cursor().x, 3); CHECK_EQ(t.cursor().y, 2);
    t.feed("\x1b[99;99H");
    CHECK_EQ(t.cursor().x, 19); CHECK_EQ(t.cursor().y, 4);
    t.feed("\x1b[H\x1b[5A\x1b[5D");
    CHECK_EQ(t.cursor().x, 0); CHECK_EQ(t.cursor().y, 0);
    t.feed("\x1b[2B\x1b[3C");
    CHECK_EQ(t.cursor().x, 3); CHECK_EQ(t.cursor().y, 2);
    t.feed("\x1b[E"); CHECK_EQ(t.cursor().x, 0); CHECK_EQ(t.cursor().y, 3);
    t.feed("\x1b[2F"); CHECK_EQ(t.cursor().y, 1);
    t.feed("\x1b[7G"); CHECK_EQ(t.cursor().x, 6);
    t.feed("\x1b[4d"); CHECK_EQ(t.cursor().y, 3);
    t.feed("\x1b[0;0H"); CHECK_EQ(t.cursor().x, 0); CHECK_EQ(t.cursor().y, 0);
    // backspace at column 0 stays
    t.feed("\b"); CHECK_EQ(t.cursor().x, 0);
    // origin mode: CUP relative to region
    t.feed("\x1b[2;4r\x1b[?6h\x1b[1;1H");
    CHECK_EQ(t.cursor().y, 1);
    t.feed("\x1b[99;1H"); CHECK_EQ(t.cursor().y, 3);
    t.feed("\x1b[?6l\x1b[r");
    // CUU confined to region when inside it
    t.feed("\x1b[2;4r\x1b[3;1H\x1b[9A"); CHECK_EQ(t.cursor().y, 1);
    t.feed("\x1b[9B"); CHECK_EQ(t.cursor().y, 3);
}

static void test_scroll_region() {
    Terminal t(10, 5, 100);
    t.feed("L1\r\nL2\r\nL3\r\nL4\r\nL5");
    t.feed("\x1b[2;4r\x1b[4;1H\n");   // LF at bottom of region scrolls only the region
    CHECK_EQ(row_text(t, 0), "L1"); CHECK_EQ(row_text(t, 1), "L3"); CHECK_EQ(row_text(t, 2), "L4");
    CHECK_EQ(row_text(t, 3), ""); CHECK_EQ(row_text(t, 4), "L5");
    CHECK_EQ(t.history_count(), (long long)0);   // region scroll never goes to history
    t.feed("\x1b[2;1H\x1bM");                     // RI at top of region scrolls down
    CHECK_EQ(row_text(t, 1), ""); CHECK_EQ(row_text(t, 2), "L3"); CHECK_EQ(row_text(t, 3), "L4");
    t.feed("\x1b[r");
    CHECK_EQ(t.cursor().y, 0);
    // LF outside region (below) does not scroll
    t.feed("\x1b[2;3r\x1b[5;1H\n"); CHECK_EQ(t.cursor().y, 4); CHECK_EQ(row_text(t, 4), "L5");
    t.feed("\x1b[r");
    // SU/SD
    t.feed("\x1b[2J\x1b[H" "A\r\nB\r\nC\x1b[H\x1b[1S");
    CHECK_EQ(row_text(t, 0), "B"); CHECK_EQ(row_text(t, 1), "C");
    t.feed("\x1b[1T"); CHECK_EQ(row_text(t, 0), ""); CHECK_EQ(row_text(t, 1), "B");
    // IL/DL within region
    t.feed("\x1b[2J\x1b[H" "A\r\nB\r\nC\r\nD\r\nE\x1b[2;4r\x1b[3;1H\x1b[L");
    CHECK_EQ(row_text(t, 1), "B"); CHECK_EQ(row_text(t, 2), ""); CHECK_EQ(row_text(t, 3), "C"); CHECK_EQ(row_text(t, 4), "E");
    t.feed("\x1b[M"); CHECK_EQ(row_text(t, 2), "C"); CHECK_EQ(row_text(t, 3), ""); CHECK_EQ(row_text(t, 4), "E");
    t.feed("\x1b[r");
}

static void test_erase() {
    Terminal t(10, 3);
    t.feed("abcdefghij\r\nklmnopqrst\r\nuvwxyz0123");
    t.feed("\x1b[2;5H\x1b[K"); CHECK_EQ(row_text(t, 1), "klmn");
    t.feed("\x1b[1;5H\x1b[1K"); CHECK_EQ(row_text(t, 0), "     fghij");
    t.feed("\x1b[3;5H\x1b[2K"); CHECK_EQ(row_text(t, 2), "");
    t.feed("\x1b[2J\x1b[H" "abcdefghij\r\nklmnopqrst\r\nuvwxyz0123");
    t.feed("\x1b[2;5H\x1b[J"); CHECK_EQ(row_text(t, 0), "abcdefghij"); CHECK_EQ(row_text(t, 1), "klmn"); CHECK_EQ(row_text(t, 2), "");
    t.feed("\x1b[2J\x1b[H" "abcdefghij\r\nklmnopqrst\r\nuvwxyz0123");
    t.feed("\x1b[2;5H\x1b[1J"); CHECK_EQ(row_text(t, 0), ""); CHECK_EQ(row_text(t, 1), "     pqrst"); CHECK_EQ(row_text(t, 2), "uvwxyz0123");
    // ED 2 on the primary screen moves content to scrollback; ED 3 clears scrollback.
    Terminal t2(10, 3, 100);
    t2.feed("one\r\ntwo\r\n");
    t2.feed("\x1b[2J");
    CHECK_EQ(t2.history_count(), (long long)2);
    CHECK_EQ(t2.history(0).text(), "one");
    CHECK_EQ(screen_text(t2), "\n\n\n");
    t2.feed("\x1b[3J");
    CHECK_EQ(t2.history_count(), (long long)0);
    CHECK_EQ(t2.history_base(), (long long)2);
    CHECK_EQ(t2.screen_base(), (long long)2);
    // ECH
    t.feed("\x1b[2J\x1b[H" "abcdefghij\x1b[1;3H\x1b[3X"); CHECK_EQ(row_text(t, 0), "ab   fghij");
    // ED with bg color: erased cells carry bg (BCE)
    t.feed("\x1b[44m\x1b[2J\x1b[m");
    CHECK(t.row(0).cells[5].bg == Color::indexed(4));
}

static void test_sgr() {
    Terminal t(40, 2);
    t.feed("\x1b[1;3;4;7;9ma");
    const Cell& c = t.row(0).cells[0];
    CHECK((c.attrs & ATTR_BOLD) && (c.attrs & ATTR_ITALIC) && (c.attrs & ATTR_UNDERLINE) && (c.attrs & ATTR_INVERSE) && (c.attrs & ATTR_STRIKE));
    CHECK(c.ul_style == UnderlineStyle::Single);
    t.feed("\x1b[22;23;24;27;29mb");
    CHECK_EQ((int)t.row(0).cells[1].attrs, 0);
    t.feed("\x1b[31;42mc"); CHECK(t.row(0).cells[2].fg == Color::indexed(1)); CHECK(t.row(0).cells[2].bg == Color::indexed(2));
    t.feed("\x1b[91;104md"); CHECK(t.row(0).cells[3].fg == Color::indexed(9)); CHECK(t.row(0).cells[3].bg == Color::indexed(12));
    t.feed("\x1b[38;5;196me"); CHECK(t.row(0).cells[4].fg == Color::indexed(196));
    t.feed("\x1b[48;2;10;20;30mf"); CHECK(t.row(0).cells[5].bg == Color::rgb(10, 20, 30));
    t.feed("\x1b[38:2::1:2:3mg"); CHECK(t.row(0).cells[6].fg == Color::rgb(1, 2, 3));
    t.feed("\x1b[38:2:4:5:6mh"); CHECK(t.row(0).cells[7].fg == Color::rgb(4, 5, 6));
    t.feed("\x1b[38:5:100mi"); CHECK(t.row(0).cells[8].fg == Color::indexed(100));
    t.feed("\x1b[0;4:3mj"); CHECK(t.row(0).cells[9].ul_style == UnderlineStyle::Curly); CHECK(t.row(0).cells[9].attrs & ATTR_UNDERLINE);
    t.feed("\x1b[58:5:9mk"); CHECK(t.row(0).cells[10].ul == Color::indexed(9));
    t.feed("\x1b[58;2;7;8;9ml"); CHECK(t.row(0).cells[11].ul == Color::rgb(7, 8, 9));
    t.feed("\x1b[59;4:0mm"); CHECK(t.row(0).cells[12].ul == Color::def()); CHECK(!(t.row(0).cells[12].attrs & ATTR_UNDERLINE));
    t.feed("\x1b[39;49mn"); CHECK(t.row(0).cells[13].fg == Color::def()); CHECK(t.row(0).cells[13].bg == Color::def());
    t.feed("\x1b[21mo"); CHECK(t.row(0).cells[14].ul_style == UnderlineStyle::Double);
    t.feed("\x1b[mp"); CHECK_EQ((int)t.row(0).cells[15].attrs, 0); CHECK(t.row(0).cells[15].ul_style == UnderlineStyle::None);
    // combined with colon sub-params in the middle of a sequence
    t.feed("\x1b[1;38:5:12;4mq"); CHECK(t.row(0).cells[16].fg == Color::indexed(12)); CHECK(t.row(0).cells[16].attrs & ATTR_BOLD); CHECK(t.row(0).cells[16].attrs & ATTR_UNDERLINE);
    t.feed("\x1b[0;2;5;8;53mr"); CHECK((t.row(0).cells[17].attrs & (ATTR_DIM | ATTR_BLINK | ATTR_INVISIBLE | ATTR_OVERLINE)) == (ATTR_DIM | ATTR_BLINK | ATTR_INVISIBLE | ATTR_OVERLINE));
}

static void test_wrap() {
    Terminal t(10, 4, 100);
    t.feed("0123456789");
    CHECK_EQ(t.cursor().x, 9); CHECK_EQ(t.cursor().y, 0);        // pending wrap, cursor stays on the line
    t.feed("\r\n");                                                 // CR/LF consumes the pending wrap: no blank line
    CHECK_EQ(t.cursor().y, 1); CHECK_EQ(row_text(t, 0), "0123456789"); CHECK(!t.row(0).wrapped);
    t.feed("abcdefghijK");
    CHECK_EQ(row_text(t, 1), "abcdefghij"); CHECK(t.row(1).wrapped); CHECK_EQ(row_text(t, 2), "K"); CHECK_EQ(t.cursor().x, 1);
    // autowrap off: overwrite last column
    t.feed("\x1b[?7l\x1b[4;1H" "0123456789XYZ"); CHECK_EQ(row_text(t, 3), "012345678Z"); CHECK_EQ(t.cursor().x, 9);
    t.feed("\x1b[?7h");
    // pending wrap is cleared by cursor movement
    t.feed("\x1b[1;1H" "0123456789\x1b[1;5H" "Q"); CHECK_EQ(row_text(t, 0), "0123Q56789");
    // EL does not clear the flag; printing afterwards wraps (xterm semantics)
    t.feed("\x1b[2J\x1b[H" "0123456789\x1b[0KZ"); CHECK_EQ(row_text(t, 0), "012345678"); CHECK_EQ(row_text(t, 1), "Z");
}

static void test_insert_delete() {
    Terminal t(10, 3);
    t.feed("abcdefghij\x1b[1;3H\x1b[2@"); CHECK_EQ(row_text(t, 0), "ab  cdefgh");
    t.feed("\x1b[1;1H\x1b[3P"); CHECK_EQ(row_text(t, 0), " cdefgh");
    t.feed("\x1b[2J\x1b[H" "abc\x1b[1;2H\x1b[4hXY\x1b[4l"); CHECK_EQ(row_text(t, 0), "aXYbc"); CHECK(!t.modes().insert);
    t.feed("\x1b[2J\x1b[H" "A\r\nB\r\nC\x1b[1;1H\x1b[L"); CHECK_EQ(row_text(t, 0), ""); CHECK_EQ(row_text(t, 1), "A"); CHECK_EQ(row_text(t, 2), "B");
    t.feed("\x1b[2M"); CHECK_EQ(row_text(t, 0), "B"); CHECK_EQ(row_text(t, 1), ""); CHECK_EQ(row_text(t, 2), "");
    // REP
    t.feed("\x1b[2J\x1b[H" "x\x1b[3b"); CHECK_EQ(row_text(t, 0), "xxxx");
}

static void test_tabs() {
    Terminal t(40, 2);
    t.feed("\t"); CHECK_EQ(t.cursor().x, 8);
    t.feed("\t\t"); CHECK_EQ(t.cursor().x, 24);
    t.feed("\x1b[2I"); CHECK_EQ(t.cursor().x, 39);         // stops at last column
    t.feed("\x1b[Z"); CHECK_EQ(t.cursor().x, 32);
    t.feed("\x1b[3Z"); CHECK_EQ(t.cursor().x, 8);
    t.feed("\x1b[1;13H\x1bH\x1b[1;1H\t\t"); CHECK_EQ(t.cursor().x, 12);   // HTS at col 12
    t.feed("\x1b[g\x1b[1;1H\t\t"); CHECK_EQ(t.cursor().x, 16);              // TBC 0 clears it... wait, cursor was at 12 when TBC ran
    t.feed("\x1b[3g\x1b[1;1H\t"); CHECK_EQ(t.cursor().x, 39);               // TBC 3 clears all
    t.feed("\x1b" "c\t"); CHECK_EQ(t.cursor().x, 8);                           // RIS restores defaults
}

static void test_alt_screen() {
    Terminal t(10, 3, 100);
    int alt_events = 0;
    t.on_alt_screen = [&](bool) { ++alt_events; };
    t.feed("main\x1b[2;3H");
    t.feed("\x1b[?1049h");
    CHECK(t.modes().alt_screen); CHECK_EQ(screen_text(t), "\n\n\n"); CHECK_EQ(alt_events, 1);
    t.feed("\x1b[H" "ALT\r\nalt2\r\nalt3\r\nalt4");
    CHECK_EQ(t.history_count(), (long long)0);   // alt screen never scrolls into history
    CHECK_EQ(row_text(t, 2), "alt4");
    t.feed("\x1b[?1049l");
    CHECK(!t.modes().alt_screen); CHECK_EQ(row_text(t, 0), "main");
    CHECK_EQ(t.cursor().x, 2); CHECK_EQ(t.cursor().y, 1); CHECK_EQ(alt_events, 2);
    // 47: switch without clearing; content persists across switches
    t.feed("\x1b[?47h"); CHECK_EQ(row_text(t, 0), "alt2"); t.feed("\x1b[?47l"); CHECK_EQ(row_text(t, 0), "main");
    // 1047 leave clears alt
    t.feed("\x1b[?1047h"); CHECK_EQ(row_text(t, 0), "alt2"); t.feed("\x1b[?1047l\x1b[?47h"); CHECK_EQ(row_text(t, 0), ""); t.feed("\x1b[?47l");
}

static void test_osc() {
    Terminal t(20, 4, 100);
    std::string title;
    std::vector<OscEvent> evs;
    std::string cwd;
    t.on_title = [&](const std::string& s) { title = s; };
    t.on_osc = [&](const OscEvent& e) { evs.push_back(e); };
    t.on_cwd = [&](const std::string& s) { cwd = s; };
    t.feed("\x1b]0;hello\x07"); CHECK_EQ(title, "hello");
    t.feed("\x1b]2;with;semi\x1b\\"); CHECK_EQ(title, "with;semi"); CHECK_EQ(t.title(), "with;semi");
    // split across feeds, in the middle of the sequence and of the terminator
    t.feed("\x1b]2;sp"); t.feed("lit\x1b"); t.feed("\\"); CHECK_EQ(title, "split");
    t.feed("\x1b]133;A\x07"); CHECK_EQ(evs.size(), (size_t)1); CHECK_EQ(evs[0].code, 133); CHECK_EQ(evs[0].args[0], "A");
    t.feed("x\r\n\r\n  \x1b]133;D;3\x1b\\");
    CHECK_EQ(evs.size(), (size_t)2); CHECK_EQ(evs[1].args[1], "3"); CHECK_EQ(evs[1].abs_line, (long long)2); CHECK_EQ(evs[1].col, 2);
    t.feed("\x1b]7770;cmd;bHM=\x07"); CHECK_EQ(evs.back().code, 7770); CHECK_EQ(evs.back().args[1], "bHM=");
    t.feed("\x1b]7;file://myhost/home/me/dir%20with%20space\x07");
    CHECK_EQ(cwd, "/home/me/dir with space"); CHECK_EQ(t.cwd(), "/home/me/dir with space"); CHECK_EQ(evs.back().code, 7);
    // hyperlinks
    t.feed("\x1b]8;id=x;http://a.b/\x1b\\L\x1b]8;;\x1b\\M");
    CHECK(t.row(2).cells[2].hyperlink != 0); CHECK_EQ(t.row(2).cells[3].hyperlink, 0u);
    const Hyperlink* h = t.hyperlink(t.row(2).cells[2].hyperlink);
    CHECK(h && h->uri == "http://a.b/" && h->id == "x");
    // clipboard
    std::string sel, data;
    t.on_clipboard_set = [&](const std::string& s, const std::string& d) { sel = s; data = d; };
    t.feed("\x1b]52;c;aGVsbG8=\x07"); CHECK_EQ(sel, "c"); CHECK_EQ(data, "aGVsbG8=");
    t.feed("\x1b]52;c;?\x07"); CHECK_EQ(data, "aGVsbG8=");   // query ignored
    // unknown OSC forwarded
    size_t n = evs.size(); t.feed("\x1b]1337;Foo=bar\x07"); CHECK_EQ(evs.size(), n + 1); CHECK_EQ(evs.back().code, 1337);
    // OSC interrupted by a CSI (ESC not followed by backslash): the CSI still executes
    t.feed("\x1b]2;bad\x1b[2;2Hq"); CHECK_EQ(t.cursor().x, 2); CHECK_EQ(t.cursor().y, 1);
    // color queries
    Replies r; r.attach(t);
    t.set_default_colors(0x112233, 0x445566, 0xffffff);
    t.feed("\x1b]10;?\x1b\\"); CHECK_EQ(r.take(), "\x1b]10;rgb:1111/2222/3333\x1b\\");
    t.feed("\x1b]11;?\x07"); CHECK_EQ(r.take(), "\x1b]11;rgb:4444/5555/6666\x1b\\");
    t.feed("\x1b]4;1;?\x07"); CHECK_EQ(r.take(), "\x1b]4;1;rgb:cdcd/0000/0000\x1b\\");
    t.feed("\x1b]11;#010203\x07"); CHECK_EQ(t.default_bg(), 0x010203u);
    t.feed("\x1b]111\x07"); CHECK_EQ(t.default_bg(), 0x445566u);
    // title stack
    t.feed("\x1b]2;T1\x07\x1b[22;0t\x1b]2;T2\x07\x1b[23;0t"); CHECK_EQ(t.title(), "T1");
}

static void test_utf8() {
    Terminal t(20, 3);
    const char* s = "h\xc3\xa9llo \xe4\xb8\xad \xf0\x9f\x99\x82!";
    // feed one byte at a time
    for (const char* p = s; *p; ++p) t.feed(p, 1);
    CHECK_EQ(row_text(t, 0), s);
    CHECK(t.row(0).cells[6].attrs & ATTR_WIDE); CHECK(t.row(0).cells[7].attrs & ATTR_WIDE_CONT);
    CHECK(t.row(0).cells[9].attrs & ATTR_WIDE);
    CHECK_EQ(t.cursor().x, 12);
    // invalid sequences -> U+FFFD, and the following byte is not swallowed
    t.feed("\r\n\xff" "a\xc3" "b\xe4\xb8" "c");
    CHECK_EQ(row_text(t, 1), "\xef\xbf\xbd" "a\xef\xbf\xbd" "b\xef\xbf\xbd" "c");
    // overlong and surrogates
    t.feed("\r\n\xc0\x80\xed\xa0\x80z");
    std::u32string u = utf8_to_u32(row_text(t, 2));
    CHECK_EQ(u.size(), (size_t)3); CHECK_EQ(u[0], (char32_t)0xFFFD); CHECK_EQ(u[1], (char32_t)0xFFFD); CHECK_EQ(u[2], (char32_t)'z');
    // combining chars attach to the previous cell
    Terminal t2(10, 2);
    t2.feed("e\xcc\x81x");
    CHECK_EQ(t2.cursor().x, 2); CHECK_EQ(row_text(t2, 0), "e\xcc\x81x");
    CHECK_EQ(t2.row(0).cells[0].combining, 1u);
    // width helpers
    CHECK_EQ(char_width(U'a'), 1); CHECK_EQ(char_width(0x4E2D), 2); CHECK_EQ(char_width(0x1F642), 2);
    CHECK_EQ(char_width(0x301), 0); CHECK_EQ(char_width(0x200D), 0); CHECK_EQ(char_width(0xFE0F), 0); CHECK_EQ(char_width(0xAD), 1);
    CHECK_EQ(char_width(0xFF21), 2); CHECK_EQ(char_width(0x263A), 1); CHECK_EQ(char_width(0x2615), 2);
}

static void test_wide_last_column() {
    Terminal t(10, 3);
    t.feed("123456789\xe4\xb8\xad");   // wide char does not fit in the last column
    CHECK_EQ(row_text(t, 0), "123456789"); CHECK(t.row(0).wrapped);
    CHECK_EQ(row_text(t, 1), "\xe4\xb8\xad"); CHECK_EQ(t.cursor().x, 2);
    // overwriting half of a wide char clears the other half
    t.feed("\x1b[2;2HZ"); CHECK_EQ(row_text(t, 1), " Z"); CHECK_EQ((int)(t.row(1).cells[0].attrs & ATTR_WIDE), 0);
    t.feed("\x1b[2;1H\xe4\xb8\xad\x1b[2;1HY"); CHECK_EQ(row_text(t, 1), "Y"); CHECK_EQ((int)(t.row(1).cells[1].attrs & ATTR_WIDE_CONT), 0);
    // wide char exactly filling the last two columns sets pending wrap; the next char wraps
    t.feed("\x1b[3;9H\xe4\xb8\xad"); CHECK_EQ(row_text(t, 2), "        \xe4\xb8\xad"); CHECK_EQ(t.cursor().x, 9);
    t.feed("q"); CHECK_EQ(row_text(t, 1), "        \xe4\xb8\xad"); CHECK(t.row(1).wrapped); CHECK_EQ(row_text(t, 2), "q");
}

static void test_resize() {
    Terminal t(10, 4, 100);
    for (int i = 0; i < 10; ++i) t.feed("line" + std::to_string(i) + "\r\n");
    // 10 lines + cursor on an empty 11th: screen shows line7..line9 + empty
    CHECK_EQ(t.screen_base(), (long long)7); CHECK_EQ(row_text(t, 0), "line7"); CHECK_EQ(t.cursor().y, 3);
    t.resize(10, 2);
    CHECK_EQ(t.screen_base(), t.history_base() + t.history_count());
    CHECK_EQ(t.line_abs(7).text(), "line7"); CHECK_EQ(t.line_abs(9).text(), "line9");
    CHECK_EQ(t.cursor().y, 1); CHECK_EQ(row_text(t, 0), "line9");
    t.resize(10, 6);
    CHECK_EQ(t.screen_base(), t.history_base() + t.history_count());
    CHECK_EQ(t.line_abs(5).text(), "line5"); CHECK_EQ(row_text(t, 0), "line5"); CHECK_EQ(t.cursor().y, 5);
    CHECK_EQ(t.screen_base(), (long long)5);
    // columns: truncate / pad, indices unchanged
    t.resize(4, 6); CHECK_EQ(row_text(t, 0), "line"); CHECK_EQ(t.row(0).width(), 4); CHECK_EQ(t.history(0).width(), 4);
    t.resize(12, 6); CHECK_EQ(row_text(t, 0), "line"); CHECK_EQ(t.row(0).width(), 12);
    CHECK_EQ(t.screen_base(), (long long)5);
    // alt screen resize keeps primary intact
    t.feed("\x1b[?1049h"); t.resize(12, 3); t.feed("\x1b[?1049l");
    CHECK_EQ(t.rows(), 3); CHECK_EQ(t.screen_base(), t.history_base() + t.history_count());
    CHECK_EQ(t.line_abs(9).text(), "line");
}

static void test_scrollback_overflow() {
    Terminal t(10, 3, 5);
    for (int i = 0; i < 20; ++i) t.feed(std::to_string(i) + "\r\n");
    CHECK_EQ(t.history_count(), (long long)5);
    CHECK_EQ(t.screen_base(), (long long)18);        // 20 lines printed, cursor on line 20, screen rows 18..20
    CHECK_EQ(t.history_base(), (long long)13);
    CHECK_EQ(t.history(13).text(), "13"); CHECK_EQ(t.line_abs(17).text(), "17"); CHECK_EQ(row_text(t, 0), "18");
    CHECK(t.scrolled_since_clear()); t.clear_dirty(); CHECK(!t.scrolled_since_clear());
    t.set_scrollback_max(2); CHECK_EQ(t.history_count(), (long long)2); CHECK_EQ(t.history_base(), (long long)16);
    Terminal t0(10, 2, 0);
    t0.feed("a\r\nb\r\nc\r\n"); CHECK_EQ(t0.history_count(), (long long)0); CHECK_EQ(t0.screen_base(), (long long)2); CHECK_EQ(row_text(t0, 0), "c");
}

static void test_text_extraction() {
    Terminal t(10, 5, 100);
    t.feed("0123456789abc\r\nsecond  \r\n\r\nlast");
    int64_t b = t.screen_base();
    CHECK_EQ(b, (long long)0);
    CHECK_EQ(t.text(b, 0, b + 1, 3), "0123456789abc");          // wrapped lines join without newline
    CHECK_EQ(t.text(b, 5, b + 3, 0), "56789abc\nsecond\n");
    CHECK_EQ(t.text(b + 2, 0, b + 5, 0), "second\n\nlast\n");
    CHECK_EQ(t.text(b + 2, 1, b + 2, 4), "eco");
    CHECK_EQ(t.text(b + 2, 4, b + 2, 2), "");
    int cb, ce;
    t.word_at(b + 2, 2, &cb, &ce); CHECK_EQ(cb, 0); CHECK_EQ(ce, 6);
    Terminal t3(30, 2, 10);
    t3.feed("foo/bar-baz.txt (x)");
    t3.word_at(0, 5, &cb, &ce); CHECK_EQ(cb, 0); CHECK_EQ(ce, 15);
    t3.word_at(0, 16, &cb, &ce); CHECK_EQ(cb, 16); CHECK_EQ(ce, 17);  // '(' is a single delimiter
    t3.word_at(0, 15, &cb, &ce); CHECK_EQ(cb, 15); CHECK_EQ(ce, 16);  // space run
    // Line::text with wide + combining
    Terminal w(10, 1);
    w.feed("\xe4\xb8\xad" "e\xcc\x81  ");
    CHECK_EQ(w.row(0).text(), "\xe4\xb8\xad" "e\xcc\x81");
    CHECK_EQ(w.text(w.screen_base(), 0, w.screen_base(), 3), "\xe4\xb8\xad" "e\xcc\x81");
}

static void test_replies() {
    Terminal t(80, 24);
    Replies r; r.attach(t);
    t.feed("\x1b[3;7H\x1b[6n"); CHECK_EQ(r.take(), "\x1b[3;7R");
    t.feed("\x1b[?6n"); CHECK_EQ(r.take(), "\x1b[?3;7R");
    t.feed("\x1b[5n"); CHECK_EQ(r.take(), "\x1b[0n");
    t.feed("\x1b[c"); CHECK(r.take().rfind("\x1b[?62;", 0) == 0);
    t.feed("\x1b[>c"); CHECK_EQ(r.take(), "\x1b[>41;377;0c");
    t.feed("\x1b[>q"); CHECK_EQ(r.take(), "\x1bP>|Canopy 0.1\x1b\\");
    t.feed("\x1b[18t"); CHECK_EQ(r.take(), "\x1b[8;24;80t");
    t.set_cell_pixel_size(9, 18);
    t.feed("\x1b[14t"); CHECK_EQ(r.take(), "\x1b[4;432;720t");
    t.feed("\x1b[16t"); CHECK_EQ(r.take(), "\x1b[6;18;9t");
    t.feed("\x1b[?2004h\x1b[?2004$p"); CHECK_EQ(r.take(), "\x1b[?2004;1$y");
    t.feed("\x1b[?2004l\x1b[?2004$p"); CHECK_EQ(r.take(), "\x1b[?2004;2$y");
    t.feed("\x1b[?9999$p"); CHECK_EQ(r.take(), "\x1b[?9999;0$y");
    t.feed("\x1b[4h\x1b[4$p"); CHECK_EQ(r.take(), "\x1b[4;1$y"); t.feed("\x1b[4l");
    // origin mode makes CPR relative
    t.feed("\x1b[5;10r\x1b[?6h\x1b[2;1H\x1b[6n"); CHECK_EQ(r.take(), "\x1b[2;1R"); t.feed("\x1b[?6l\x1b[r");
    // DECRQSS
    t.feed("\x1b[1;31m\x1bP$qm\x1b\\"); CHECK_EQ(r.take(), "\x1bP1$r0;1;31m\x1b\\");
    t.feed("\x1b[3;10r\x1bP$qr\x1b\\"); CHECK_EQ(r.take(), "\x1bP1$r3;10r\x1b\\"); t.feed("\x1b[r");
    t.feed("\x1bP+q544e\x1b\\"); CHECK_EQ(r.take(), "\x1bP0+r\x1b\\");
    // focus events
    t.feed("\x1b[?1004h"); t.notify_focus(true); CHECK_EQ(r.take(), "\x1b[I"); t.notify_focus(false); CHECK_EQ(r.take(), "\x1b[O");
    t.feed("\x1b[?1004l"); t.notify_focus(true); CHECK_EQ(r.take(), "");
    // modes reflected in modes()
    t.feed("\x1b[?1h\x1b[?1000h\x1b[?1006h\x1b[?25l\x1b[?12h\x1b[6 q\x1b[20h\x1b[?1049h\x1b=");
    const TerminalModes& m = t.modes();
    CHECK(m.app_cursor_keys && m.mouse == MouseMode::Normal && m.mouse_enc == MouseEncoding::Sgr && !m.cursor_visible && m.lnm && m.alt_screen && m.app_keypad);
    CHECK(!t.cursor().visible); CHECK(t.cursor().style == CursorStyle::Bar); CHECK(!t.cursor().blink);
    t.feed("\x1b[?1002h"); CHECK(m.mouse == MouseMode::ButtonEvent);
    t.feed("\x1b[?1002l"); CHECK(m.mouse == MouseMode::None);
    t.feed("\x1b[?1049l\x1b>\x1b[?1l\x1b[?25h\x1b[20l"); CHECK(!m.alt_screen && !m.app_keypad && !m.app_cursor_keys && m.cursor_visible && !m.lnm);
}

static void test_charsets_and_misc() {
    Terminal t(10, 3);
    t.feed("\x1b(0qx\x1b(Bq"); CHECK_EQ(row_text(t, 0), "\xe2\x94\x80\xe2\x94\x82q");
    t.feed("\r\n\x1b)0\x0eq\x0fq"); CHECK_EQ(row_text(t, 1), "\xe2\x94\x80q");
    // DECSC / DECRC incl. attributes
    t.feed("\x1b[1;5H\x1b[31m\x1b""7\x1b[m\x1b[3;1H\x1b""8X");
    CHECK_EQ(t.cursor().x, 5); CHECK_EQ(t.cursor().y, 0); CHECK(t.row(0).cells[4].fg == Color::indexed(1));
    t.feed("\x1b[s\x1b[3;3H\x1b[u"); CHECK_EQ(t.cursor().x, 5); CHECK_EQ(t.cursor().y, 0);
    // DECSTR
    t.feed("\x1b[?6h\x1b[?1h\x1b[2;3r\x1b[!p"); CHECK(!t.modes().origin); CHECK(!t.modes().app_cursor_keys);
    t.feed("\x1b[9;1H"); CHECK_EQ(t.cursor().y, 2);   // margins reset
    // reverse video / bell
    int bells = 0; t.on_bell = [&] { ++bells; };
    t.feed("\x07\x07"); CHECK_EQ(bells, 2);
    t.feed("\x1b[?5h"); CHECK(t.modes().reverse_video); t.feed("\x1b[?5l");
    // C1 controls via UTF-8 encoding (NEL)
    t.feed("\x1b[2J\x1b[Ha\xc2\x85" "b"); CHECK_EQ(row_text(t, 0), "a"); CHECK_EQ(row_text(t, 1), "b");
    // 8-bit CSI
    t.feed("\xc2\x9b" "2;4Hz"); CHECK_EQ(t.row(1).cells[3].cp, (char32_t)'z');
    // CAN aborts a sequence
    t.feed("\x1b[2;\x18" "k"); CHECK_EQ(t.row(1).cells[4].cp, (char32_t)'k');
    // garbage/unknown sequences are ignored without breaking the parser
    t.feed("\x1b[?99999;1;2;3;4;5;6;7;8;9;0;1;2;3;4;5;6;7;8;9;0;1;2;3;4;5;6;7;8;9;0;1;2;3;4;5;6;7;8;9;0;1;2;3;4;5;6;7;8;9;0;1;2;3;4;5;6;7;8;9;0;1;2;3;4;5;6;7;8;9;0;1;2;3;4;5;6;7;8;9;0;1;2;3;4;5;6;7;8;9;0;1;2;3;4;5;6;7;8;9;0;1;2;3;4;5;6;7;8;9;0;1;2;3;4;5;6;7;8;9;0;1;2;3;4;5;6;7;8;9;0z" "\x1b_apc payload\x1b\\" "\x1bP1;2;3q#1~~\x1b\\" "\x1b[>4;2m" "\x1b[=5u" "ok");
    CHECK_EQ(t.row(1).cells[5].cp, (char32_t)'o');
    // generation & dirty rows
    uint64_t g = t.generation(); t.clear_dirty();
    CHECK(!t.row_dirty(0)); t.feed("\x1b[1;1HQ"); CHECK(t.row_dirty(0)); CHECK(!t.row_dirty(2)); CHECK(t.generation() > g);
    // dirty bits cover scrolled rows
    t.clear_dirty(); t.feed("\x1b[3;1H\n"); CHECK(t.row_dirty(0) && t.row_dirty(1) && t.row_dirty(2)); CHECK(t.scrolled_since_clear());
    // DECALN
    t.feed("\x1b#8"); CHECK_EQ(row_text(t, 2), "EEEEEEEEEE");
    // RIS
    t.feed("\x1b" "c"); CHECK_EQ(screen_text(t), "\n\n\n"); CHECK_EQ(t.cursor().x, 0);
    // zero-size defensive
    Terminal z(1, 1); z.feed("abc\r\n\xe4\xb8\xad"); CHECK_EQ(z.cols(), 1);
}

static void test_throughput() {
    std::string line;
    for (int i = 0; i < 79; ++i) line.push_back((char)('a' + i % 26));
    line += "\n";
    std::string chunk;
    while (chunk.size() < (1 << 20)) chunk += line;
    const size_t total = 64u << 20;
    Terminal t(80, 40, 1000);
    auto t0 = std::chrono::steady_clock::now();
    size_t fed = 0;
    while (fed < total) { t.feed(chunk); fed += chunk.size(); }
    double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    double mbps = (double)fed / (1024.0 * 1024.0) / secs;
    printf("throughput: plain text %.0f MB/s (%.2f s for %zu MiB)\n", mbps, secs, fed >> 20);
    CHECK(mbps > 50);
    // SGR-heavy content
    std::string colored;
    while (colored.size() < (1 << 20)) colored += "\x1b[1;31mred\x1b[0m \x1b[38;2;10;20;30mrgb\x1b[m \xe4\xb8\xad\xe6\x96\x87 \x1b[K\n";
    t0 = std::chrono::steady_clock::now();
    fed = 0;
    while (fed < (16u << 20)) { t.feed(colored); fed += colored.size(); }
    secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    printf("throughput: SGR/UTF-8 mix %.0f MB/s\n", (double)fed / (1024.0 * 1024.0) / secs);
}

int main() {
    test_cursor_movement();
    test_scroll_region();
    test_erase();
    test_sgr();
    test_wrap();
    test_insert_delete();
    test_tabs();
    test_alt_screen();
    test_osc();
    test_utf8();
    test_wide_last_column();
    test_resize();
    test_scrollback_overflow();
    test_text_extraction();
    test_replies();
    test_charsets_and_misc();
    test_throughput();
    printf("test_terminal: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
