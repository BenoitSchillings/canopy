#include "common.h"
#include <cstdlib>
#include <cctype>

namespace canopy::ui {

std::string pretty_path(const std::string& path, size_t max_len) {
    std::string p = path;
    const char* home = getenv("HOME");
    if (home && *home) {
        std::string h = home;
        if (p == h) p = "~";
        else if (p.rfind(h + "/", 0) == 0) p = "~" + p.substr(h.size());
    }
    if (p.size() <= max_len) return p;
    // keep the last components
    size_t keep = max_len - 2;
    return "…" + p.substr(p.size() - keep);
}

std::string basename_of(const std::string& path) {
    if (path.empty()) return path;
    size_t end = path.size();
    while (end > 1 && path[end - 1] == '/') end--;
    size_t slash = path.rfind('/', end - 1);
    if (slash == std::string::npos) return path.substr(0, end);
    return path.substr(slash + 1, end - slash - 1);
}

size_t utf8_length(std::string_view s) {
    size_t n = 0;
    for (unsigned char c : s) if ((c & 0xC0) != 0x80) n++;
    return n;
}

std::string ellipsize(const std::string& s, size_t n) {
    if (utf8_length(s) <= n) return s;
    size_t count = 0, i = 0;
    while (i < s.size() && count < (n > 0 ? n - 1 : 0)) {
        unsigned char c = s[i];
        size_t len = c < 0x80 ? 1 : (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : 4;
        i += len; count++;
    }
    return s.substr(0, i) + "…";
}

std::string to_lower(std::string s) { for (auto& c : s) c = (char)tolower((unsigned char)c); return s; }

int fuzzy_score(std::string_view needle, std::string_view hay) {
    if (needle.empty()) return 1;
    int score = 0; size_t hi = 0; int streak = 0;
    for (char nc : needle) {
        char n = (char)tolower((unsigned char)nc);
        bool found = false;
        while (hi < hay.size()) {
            char h = (char)tolower((unsigned char)hay[hi]);
            hi++;
            if (h == n) { found = true; break; }
            streak = 0;
        }
        if (!found) return 0;
        streak++;
        score += 1 + streak * 2 + (hi == 1 || hay[hi - 2] == ' ' || hay[hi - 2] == '/' || hay[hi - 2] == '_' ? 3 : 0);
    }
    return score + (int)(100 / (1 + hay.size() / 8));
}

} // namespace canopy::ui

#include <fstream>
#include <sys/stat.h>
namespace canopy::ui {

std::u32string utf8_decode(std::string_view s) {
    std::u32string out; out.reserve(s.size());
    size_t i = 0;
    while (i < s.size()) {
        unsigned char c = s[i];
        char32_t cp; size_t len;
        if (c < 0x80) { cp = c; len = 1; }
        else if ((c >> 5) == 6 && i + 1 < s.size()) { cp = ((c & 0x1F) << 6) | (s[i + 1] & 0x3F); len = 2; }
        else if ((c >> 4) == 14 && i + 2 < s.size()) { cp = ((c & 0x0F) << 12) | ((s[i + 1] & 0x3F) << 6) | (s[i + 2] & 0x3F); len = 3; }
        else if ((c >> 3) == 30 && i + 3 < s.size()) { cp = ((c & 0x07) << 18) | ((s[i + 1] & 0x3F) << 12) | ((s[i + 2] & 0x3F) << 6) | (s[i + 3] & 0x3F); len = 4; }
        else { cp = 0xFFFD; len = 1; }
        out.push_back(cp); i += len;
    }
    return out;
}

void utf8_append(std::string& out, char32_t cp) {
    if (cp < 0x80) out.push_back((char)cp);
    else if (cp < 0x800) { out.push_back((char)(0xC0 | (cp >> 6))); out.push_back((char)(0x80 | (cp & 0x3F))); }
    else if (cp < 0x10000) { out.push_back((char)(0xE0 | (cp >> 12))); out.push_back((char)(0x80 | ((cp >> 6) & 0x3F))); out.push_back((char)(0x80 | (cp & 0x3F))); }
    else { out.push_back((char)(0xF0 | (cp >> 18))); out.push_back((char)(0x80 | ((cp >> 12) & 0x3F))); out.push_back((char)(0x80 | ((cp >> 6) & 0x3F))); out.push_back((char)(0x80 | (cp & 0x3F))); }
}
std::string utf8_encode(char32_t cp) { std::string s; utf8_append(s, cp); return s; }

bool is_wide_cp(char32_t cp) {
    return (cp >= 0x1100 && cp <= 0x115F) || (cp >= 0x2E80 && cp <= 0xA4CF && cp != 0x303F) ||
           (cp >= 0xAC00 && cp <= 0xD7A3) || (cp >= 0xF900 && cp <= 0xFAFF) || (cp >= 0xFE30 && cp <= 0xFE4F) ||
           (cp >= 0xFF00 && cp <= 0xFF60) || (cp >= 0xFFE0 && cp <= 0xFFE6) || (cp >= 0x1F300 && cp <= 0x1F64F) ||
           (cp >= 0x1F900 && cp <= 0x1F9FF) || (cp >= 0x20000 && cp <= 0x3FFFD);
}

std::string shell_quote(const std::string& s) {
    std::string out = "'";
    for (char c : s) { if (c == '\'') out += "'\\''"; else out += c; }
    return out + "'";
}

std::string home_dir() { const char* h = getenv("HOME"); return h ? h : "/"; }

bool write_file(const std::string& path, const std::string& data) {
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    if (!f) return false;
    f.write(data.data(), (std::streamsize)data.size());
    return (bool)f;
}

} // namespace canopy::ui
