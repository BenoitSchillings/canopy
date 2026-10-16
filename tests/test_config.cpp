// Tests for core/config.h
#include "core/config.h"
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>
#include <unistd.h>

using namespace canopy;

static int g_fail = 0, g_pass = 0;
#define CHECK(cond) do { if (cond) g_pass++; else { g_fail++; std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } } while (0)

static bool rgb_eq(RGB8 a, int r, int g, int b) { return a.r == r && a.g == g && a.b == b; }

static std::string tmpfile_with(const std::string& text) {
    char name[] = "/tmp/canopy_cfg_XXXXXX";
    int fd = mkstemp(name);
    if (fd < 0) return "";
    close(fd);
    std::ofstream(name) << text;
    return name;
}

static void test_defaults() {
    Config c;
    CHECK(c.font_family == "DejaVu Sans Mono");
    CHECK(c.font_size_px == 15.0f);
    CHECK(c.default_cols == 110 && c.default_rows == 32);
    CHECK(c.scrollback == 20000);
    CHECK(c.cursor_style == "block");
    CHECK(c.theme.ansi[0].r != 0 || c.theme.ansi[0].g != 0 || c.theme.ansi[0].b != 0);  // palette filled
    CHECK(rgb_eq(c.theme.ansi[1], 0xf7, 0x76, 0x8e));
    CHECK(rgb_eq(c.theme.ansi[15], 0xc8, 0xd3, 0xf5));
    for (int i = 0; i < 8; i++) {
        // bright variants are at least as bright as the normal ones (sum of channels)
        int n = c.theme.ansi[i].r + c.theme.ansi[i].g + c.theme.ansi[i].b;
        int b = c.theme.ansi[i + 8].r + c.theme.ansi[i + 8].g + c.theme.ansi[i + 8].b;
        CHECK(b >= n);
    }
    CHECK(c.config_path.empty());
}

static void test_default_path() {
    setenv("XDG_CONFIG_HOME", "/tmp/xdgtest", 1);
    CHECK(Config::default_path() == "/tmp/xdgtest/canopy/canopy.conf");
    unsetenv("XDG_CONFIG_HOME");
    setenv("HOME", "/home/someone", 1);
    CHECK(Config::default_path() == "/home/someone/.config/canopy/canopy.conf");
    // Missing default config: load() succeeds with defaults.
    Config c;
    std::string err;
    CHECK(c.load("", &err));
    CHECK(err.empty());
    CHECK(c.font_size_px == 15.0f);
    // Missing explicit path: failure.
    CHECK(!c.load("/nonexistent/dir/canopy.conf", &err));
    CHECK(!err.empty());
}

static void test_parse_hex() {
    RGB8 c;
    CHECK(Config::parse_hex("#1a2B3c", &c) && rgb_eq(c, 0x1a, 0x2b, 0x3c));
    CHECK(Config::parse_hex("ffffff", &c) && rgb_eq(c, 255, 255, 255));
    CHECK(Config::parse_hex("#abc", &c) && rgb_eq(c, 0xaa, 0xbb, 0xcc));
    CHECK(Config::parse_hex(" #000000 ", &c) && rgb_eq(c, 0, 0, 0));
    CHECK(Config::parse_hex("0x102030", &c) && rgb_eq(c, 0x10, 0x20, 0x30));
    CHECK(!Config::parse_hex("#12345", &c));
    CHECK(!Config::parse_hex("#gggggg", &c));
    CHECK(!Config::parse_hex("", &c));
    CHECK(!Config::parse_hex("red", &c));
}

static void test_load() {
    std::string text =
        "# Canopy test config\n"
        "font_family = \"JetBrains Mono\"   # quoted, with comment\n"
        "font_family_bold = Fira Code Bold\n"
        "font_size = 13.5\n"
        "cell_padding_x=1\n"
        "  cell_padding_y =   2  \n"
        "cols = 132\n"
        "rows = 50\n"
        "scrollback = 5000\n"
        "shell = /bin/zsh\n"
        "login_shell = yes\n"
        "cursor_blink = off\n"
        "cursor_blink_hz = 2\n"
        "cursor_style = Bar\n"
        "audible_bell = true\n"
        "visual_bell = no\n"
        "shell_integration = on\n"
        "structured_output = FALSE\n"
        "sidebar_visible = 0\n"
        "sidebar_width = 300\n"
        "animations = false\n"
        "animation_speed = 1.75\n"
        "window_width = 1280\n"
        "window_height = 720\n"
        "foreground = #ffffff\n"
        "background = #000000 # pure black\n"
        "cursor = #ff0000\n"
        "selection = #112233\n"
        "color0 = #010203\n"
        "color7 = #777777\n"
        "color15 = #fefefe\n"
        "chrome_bg = #0a0a0a\n"
        "chrome_fg = #bbbbbb\n"
        "accent = #00ff00\n"
        "ok = #00aa00\n"
        "error = #aa0000\n"
        "warning = #aaaa00\n"
        "canvas_bg = #050505\n"
        "\n"
        "   # indented comment\n"
        "; semicolon comment\n"
        "bogus_key = 1\n"
        "font_size = notanumber\n"
        "cursor_style = triangle\n"
        "no equals sign here\n";
    std::string path = tmpfile_with(text);
    CHECK(!path.empty());
    Config c;
    std::string err;
    CHECK(c.load(path, &err));
    CHECK(c.config_path == path);
    CHECK(c.font_family == "JetBrains Mono");
    CHECK(c.font_family_bold == "Fira Code Bold");
    CHECK(c.font_size_px == 13.5f);
    CHECK(c.cell_padding_x == 1 && c.cell_padding_y == 2);
    CHECK(c.default_cols == 132 && c.default_rows == 50);
    CHECK(c.scrollback == 5000);
    CHECK(c.shell == "/bin/zsh");
    CHECK(c.login_shell == true);
    CHECK(c.cursor_blink == false);
    CHECK(c.cursor_blink_hz == 2.0f);
    CHECK(c.cursor_style == "bar");
    CHECK(c.audible_bell == true && c.visual_bell == false);
    CHECK(c.shell_integration == true && c.structured_output == false);
    CHECK(c.sidebar_visible == false && c.sidebar_width == 300);
    CHECK(c.animations == false && c.animation_speed == 1.75f);
    CHECK(c.window_width == 1280 && c.window_height == 720);
    CHECK(rgb_eq(c.theme.foreground, 255, 255, 255));
    CHECK(rgb_eq(c.theme.background, 0, 0, 0));
    CHECK(rgb_eq(c.theme.cursor, 255, 0, 0));
    CHECK(rgb_eq(c.theme.selection, 0x11, 0x22, 0x33));
    CHECK(rgb_eq(c.theme.ansi[0], 1, 2, 3));
    CHECK(rgb_eq(c.theme.ansi[7], 0x77, 0x77, 0x77));
    CHECK(rgb_eq(c.theme.ansi[15], 0xfe, 0xfe, 0xfe));
    CHECK(rgb_eq(c.theme.ansi[1], 0xf7, 0x76, 0x8e));   // untouched default
    CHECK(rgb_eq(c.theme.chrome_bg, 0x0a, 0x0a, 0x0a));
    CHECK(rgb_eq(c.theme.chrome_fg, 0xbb, 0xbb, 0xbb));
    CHECK(rgb_eq(c.theme.accent, 0, 255, 0));
    CHECK(rgb_eq(c.theme.ok, 0, 0xaa, 0));
    CHECK(rgb_eq(c.theme.error, 0xaa, 0, 0));
    CHECK(rgb_eq(c.theme.warning, 0xaa, 0xaa, 0));
    CHECK(rgb_eq(c.theme.canvas_bg, 5, 5, 5));
    // Warnings: unknown key, bad float, bad cursor style, malformed line.
    CHECK(err.find("unknown key 'bogus_key'") != std::string::npos);
    CHECK(err.find("invalid value 'notanumber'") != std::string::npos);
    CHECK(err.find("invalid value 'triangle'") != std::string::npos);
    CHECK(err.find("expected 'key = value'") != std::string::npos);
    CHECK(err.find(":42:") != std::string::npos);   // line number of bogus_key
    unlink(path.c_str());
}

static void test_quotes() {
    std::string path = tmpfile_with(
        "shell = \"/usr/bin/env bash\"\n"
        "font_family = 'Single # not a comment'\n"
        "font_family_bold = \"esc\\\"aped\\\\\"\n");
    Config c;
    std::string err;
    CHECK(c.load(path, &err));
    CHECK(c.shell == "/usr/bin/env bash");
    CHECK(c.font_family == "Single # not a comment");
    CHECK(c.font_family_bold == "esc\"aped\\");
    CHECK(err.empty());
    unlink(path.c_str());
}

static void test_resolve() {
    Config c;
    CHECK(rgb_eq(c.resolve(Color::def(), true), c.theme.foreground.r, c.theme.foreground.g, c.theme.foreground.b));
    CHECK(rgb_eq(c.resolve(Color::def(), false), c.theme.background.r, c.theme.background.g, c.theme.background.b));
    CHECK(rgb_eq(c.resolve(Color::indexed(1), true), 0xf7, 0x76, 0x8e));
    CHECK(rgb_eq(c.resolve(Color::indexed(9), true), c.theme.ansi[9].r, c.theme.ansi[9].g, c.theme.ansi[9].b));
    CHECK(rgb_eq(c.resolve(Color::rgb(1, 2, 3), true), 1, 2, 3));
    // 6x6x6 cube
    CHECK(rgb_eq(Config::index_to_rgb(16, c.theme), 0, 0, 0));
    CHECK(rgb_eq(Config::index_to_rgb(17, c.theme), 0, 0, 95));
    CHECK(rgb_eq(Config::index_to_rgb(22, c.theme), 0, 95, 0));
    CHECK(rgb_eq(Config::index_to_rgb(52, c.theme), 95, 0, 0));
    CHECK(rgb_eq(Config::index_to_rgb(196, c.theme), 255, 0, 0));
    CHECK(rgb_eq(Config::index_to_rgb(46, c.theme), 0, 255, 0));
    CHECK(rgb_eq(Config::index_to_rgb(21, c.theme), 0, 0, 255));
    CHECK(rgb_eq(Config::index_to_rgb(231, c.theme), 255, 255, 255));
    CHECK(rgb_eq(Config::index_to_rgb(110, c.theme), 135, 175, 215));
    // grayscale
    CHECK(rgb_eq(Config::index_to_rgb(232, c.theme), 8, 8, 8));
    CHECK(rgb_eq(Config::index_to_rgb(244, c.theme), 128, 128, 128));
    CHECK(rgb_eq(Config::index_to_rgb(255, c.theme), 238, 238, 238));
}

int main() {
    test_defaults();
    test_default_path();
    test_parse_hex();
    test_load();
    test_quotes();
    test_resolve();
    std::printf("test_config: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
