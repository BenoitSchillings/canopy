// User configuration loader: "key = value" lines, '#' comments.
#include "config.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <functional>
#include <map>
#include <sstream>
#include <string_view>
#include <sys/stat.h>
#include <pwd.h>
#include <unistd.h>

namespace canopy {

namespace {

std::string home_dir() {
    const char* h = std::getenv("HOME");
    if (h && *h) return h;
    if (const passwd* pw = getpwuid(getuid())) {
        if (pw->pw_dir && *pw->pw_dir) return pw->pw_dir;
    }
    return "/";
}

std::string trim(std::string_view s) {
    size_t b = 0, e = s.size();
    while (b < e && (s[b] == ' ' || s[b] == '\t' || s[b] == '\r' || s[b] == '\n')) b++;
    while (e > b && (s[e - 1] == ' ' || s[e - 1] == '\t' || s[e - 1] == '\r' || s[e - 1] == '\n')) e--;
    return std::string(s.substr(b, e - b));
}

std::string lower(std::string s) {
    for (auto& c : s) if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
    return s;
}

bool parse_bool(const std::string& v, bool* out) {
    std::string l = lower(v);
    if (l == "true" || l == "yes" || l == "on" || l == "1") { *out = true; return true; }
    if (l == "false" || l == "no" || l == "off" || l == "0") { *out = false; return true; }
    return false;
}

bool parse_int(const std::string& v, int* out) {
    if (v.empty()) return false;
    char* end = nullptr;
    long r = std::strtol(v.c_str(), &end, 10);
    if (end == v.c_str() || *end != 0) return false;
    *out = (int)r;
    return true;
}

bool parse_float(const std::string& v, float* out) {
    if (v.empty()) return false;
    char* end = nullptr;
    double r = std::strtod(v.c_str(), &end);
    if (end == v.c_str() || *end != 0) return false;
    *out = (float)r;
    return true;
}

// Default ANSI palette. Derived from the "Tokyo Night" theme (storm variant),
// with the bright set lifted slightly so bold-brightening is visible.
constexpr RGB8 kDefaultAnsi[16] = {
    {0x1b, 0x1d, 0x2b}, // 0  black
    {0xf7, 0x76, 0x8e}, // 1  red
    {0x9e, 0xce, 0x6a}, // 2  green
    {0xe0, 0xaf, 0x68}, // 3  yellow
    {0x7a, 0xa2, 0xf7}, // 4  blue
    {0xbb, 0x9a, 0xf7}, // 5  magenta
    {0x7d, 0xcf, 0xff}, // 6  cyan
    {0xa9, 0xb1, 0xd6}, // 7  white
    {0x44, 0x4b, 0x6a}, // 8  bright black
    {0xff, 0x9e, 0xa8}, // 9  bright red
    {0xb9, 0xf2, 0x7c}, // 10 bright green
    {0xff, 0xc7, 0x77}, // 11 bright yellow
    {0x8d, 0xb0, 0xff}, // 12 bright blue
    {0xcf, 0xb3, 0xff}, // 13 bright magenta
    {0x9a, 0xdc, 0xff}, // 14 bright cyan
    {0xc8, 0xd3, 0xf5}, // 15 bright white
};

} // namespace

Config::Config() {
    for (int i = 0; i < 16; i++) theme.ansi[(size_t)i] = kDefaultAnsi[i];
}

std::string Config::default_path() {
    const char* xdg = std::getenv("XDG_CONFIG_HOME");
    std::string base = (xdg && *xdg) ? std::string(xdg) : home_dir() + "/.config";
    return base + "/canopy/canopy.conf";
}

bool Config::parse_hex(const std::string& in, RGB8* out) {
    std::string s = trim(in);
    if (s.empty()) return false;
    if (s[0] == '#') s.erase(0, 1);
    else if (s.size() > 2 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) s.erase(0, 2);
    auto hex = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    int v[6];
    if (s.size() == 6) {
        for (int i = 0; i < 6; i++) { v[i] = hex(s[(size_t)i]); if (v[i] < 0) return false; }
    } else if (s.size() == 3) {
        for (int i = 0; i < 3; i++) {
            int h = hex(s[(size_t)i]);
            if (h < 0) return false;
            v[2 * i] = v[2 * i + 1] = h;
        }
    } else {
        return false;
    }
    if (out) {
        out->r = (uint8_t)(v[0] * 16 + v[1]);
        out->g = (uint8_t)(v[2] * 16 + v[3]);
        out->b = (uint8_t)(v[4] * 16 + v[5]);
    }
    return true;
}

RGB8 Config::index_to_rgb(uint8_t idx, const Theme& t) {
    if (idx < 16) return t.ansi[idx];
    if (idx < 232) {
        static constexpr uint8_t steps[6] = {0, 95, 135, 175, 215, 255};
        int i = idx - 16;
        return RGB8{steps[i / 36], steps[(i / 6) % 6], steps[i % 6]};
    }
    uint8_t g = (uint8_t)(8 + 10 * (idx - 232));
    return RGB8{g, g, g};
}

RGB8 Config::resolve(const Color& c, bool is_fg) const {
    switch (c.kind) {
    case Color::Indexed: return index_to_rgb(c.idx, theme);
    case Color::Rgb: return RGB8{c.r, c.g, c.b};
    case Color::Default:
    default: return is_fg ? theme.foreground : theme.background;
    }
}

bool Config::load(const std::string& path_in, std::string* err) {
    if (err) err->clear();
    std::string path = path_in.empty() ? default_path() : path_in;

    std::ifstream f(path);
    if (!f) {
        if (path_in.empty()) return true;   // no user config: defaults are fine
        if (err) *err = "cannot open " + path;
        return false;
    }
    config_path = path;

    std::string warnings;
    auto warn = [&](int line_no, const std::string& msg) {
        char buf[32];
        std::snprintf(buf, sizeof buf, "%d: ", line_no);
        warnings += path + ":" + buf + msg + "\n";
    };

    // Table of setters. Each returns false if the value could not be parsed.
    using Setter = std::function<bool(const std::string&)>;
    std::map<std::string, Setter> setters;
    auto str_key = [&](const char* k, std::string* dst) { setters[k] = [dst](const std::string& v) { *dst = v; return true; }; };
    auto int_key = [&](const char* k, int* dst) { setters[k] = [dst](const std::string& v) { return parse_int(v, dst); }; };
    auto float_key = [&](const char* k, float* dst) { setters[k] = [dst](const std::string& v) { return parse_float(v, dst); }; };
    auto bool_key = [&](const char* k, bool* dst) { setters[k] = [dst](const std::string& v) { return parse_bool(v, dst); }; };
    auto rgb_key = [&](const std::string& k, RGB8* dst) { setters[k] = [dst](const std::string& v) { return parse_hex(v, dst); }; };

    str_key("font_family", &font_family);
    str_key("font_family_bold", &font_family_bold);
    float_key("font_size", &font_size_px);
    int_key("cell_padding_x", &cell_padding_x);
    int_key("cell_padding_y", &cell_padding_y);
    int_key("cols", &default_cols);
    int_key("rows", &default_rows);
    int_key("scrollback", &scrollback);
    str_key("shell", &shell);
    bool_key("login_shell", &login_shell);
    bool_key("cursor_blink", &cursor_blink);
    float_key("cursor_blink_hz", &cursor_blink_hz);
    setters["cursor_style"] = [this](const std::string& v) {
        std::string l = lower(v);
        if (l != "block" && l != "bar" && l != "underline") return false;
        cursor_style = l;
        return true;
    };
    bool_key("audible_bell", &audible_bell);
    bool_key("visual_bell", &visual_bell);
    bool_key("shell_integration", &shell_integration);
    bool_key("structured_output", &structured_output);
    bool_key("sidebar_visible", &sidebar_visible);
    int_key("sidebar_width", &sidebar_width);
    bool_key("animations", &animations);
    float_key("animation_speed", &animation_speed);
    int_key("window_width", &window_width);
    int_key("window_height", &window_height);
    bool_key("peek_visible", &peek_visible);
    int_key("peek_width", &peek_width);
    bool_key("ai_summaries", &ai_summaries);
    setters["ai_backend"] = [this](const std::string& v) {
        std::string l = lower(v);
        if (l != "auto" && l != "api" && l != "claude" && l != "local") return false;
        ai_backend = l; return true;
    };
    str_key("ai_model", &ai_model);
    str_key("ai_api_key", &ai_api_key);
    str_key("ai_cli_model", &ai_cli_model);
    int_key("ai_min_interval", &ai_min_interval);
    rgb_key("foreground", &theme.foreground);
    rgb_key("background", &theme.background);
    rgb_key("cursor", &theme.cursor);
    rgb_key("selection", &theme.selection);
    for (int i = 0; i < 16; i++) rgb_key("color" + std::to_string(i), &theme.ansi[(size_t)i]);
    rgb_key("chrome_bg", &theme.chrome_bg);
    rgb_key("chrome_fg", &theme.chrome_fg);
    rgb_key("accent", &theme.accent);
    rgb_key("ok", &theme.ok);
    rgb_key("error", &theme.error);
    rgb_key("warning", &theme.warning);
    rgb_key("canvas_bg", &theme.canvas_bg);

    std::string raw;
    int line_no = 0;
    while (std::getline(f, raw)) {
        line_no++;
        std::string line = trim(raw);
        if (line.empty() || line[0] == '#' || line[0] == ';') continue;
        size_t eq = line.find('=');
        if (eq == std::string::npos) { warn(line_no, "expected 'key = value'"); continue; }
        std::string key = lower(trim(line.substr(0, eq)));
        std::string rest = trim(line.substr(eq + 1));
        if (key.empty()) { warn(line_no, "empty key"); continue; }

        // Value: quoted string (with escapes) or bare text up to a '#' comment.
        std::string value;
        if (!rest.empty() && (rest[0] == '"' || rest[0] == '\'')) {
            char q = rest[0];
            size_t i = 1;
            bool closed = false;
            for (; i < rest.size(); i++) {
                char c = rest[i];
                if (c == '\\' && q == '"' && i + 1 < rest.size()) {
                    char n = rest[++i];
                    switch (n) {
                    case 'n': value.push_back('\n'); break;
                    case 't': value.push_back('\t'); break;
                    case '\\': value.push_back('\\'); break;
                    case '"': value.push_back('"'); break;
                    default: value.push_back('\\'); value.push_back(n); break;
                    }
                } else if (c == q) {
                    closed = true;
                    break;
                } else {
                    value.push_back(c);
                }
            }
            if (!closed) warn(line_no, "unterminated quoted string");
        } else {
            // A '#' preceded by whitespace starts a comment; a leading '#' is a
            // color value (#rrggbb), so the search starts after the first char.
            size_t hash = rest.find('#', 1);
            while (hash != std::string::npos && rest[hash - 1] != ' ' && rest[hash - 1] != '\t') hash = rest.find('#', hash + 1);
            if (hash != std::string::npos) rest = rest.substr(0, hash);
            value = trim(rest);
        }

        auto it = setters.find(key);
        if (it == setters.end()) { warn(line_no, "unknown key '" + key + "'"); continue; }
        if (!it->second(value)) warn(line_no, "invalid value '" + value + "' for '" + key + "'");
    }

    if (err) *err = warnings;
    return true;
}

} // namespace canopy
