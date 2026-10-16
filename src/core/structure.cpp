// Structured-output detection: spans (paths, urls, errors, ...), tables, JSON.
// Hand-written scanners only: this runs over every finished block's output.
#include "structure.h"
#include "json.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <unordered_set>
#include <sys/stat.h>

namespace canopy {

namespace {

// ---------------------------------------------------------------------------
// Character classes
// ---------------------------------------------------------------------------
inline bool is_space(unsigned char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\f' || c == '\v'; }
inline bool is_digit(unsigned char c) { return c >= '0' && c <= '9'; }
inline bool is_alpha(unsigned char c) { return (c | 0x20) >= 'a' && (c | 0x20) <= 'z'; }
inline bool is_alnum(unsigned char c) { return is_digit(c) || is_alpha(c); }
inline bool is_word(unsigned char c) { return is_alnum(c) || c == '_'; }
inline bool is_lhex(unsigned char c) { return is_digit(c) || (c >= 'a' && c <= 'f'); }
inline bool is_upper(unsigned char c) { return c >= 'A' && c <= 'Z'; }
inline unsigned char lower(unsigned char c) { return is_upper(c) ? (unsigned char)(c + 32) : c; }

// Characters that open a new token inside a whitespace-delimited word.
inline bool is_opener(unsigned char c) {
    switch (c) {
    case '(': case '[': case '{': case '<': case '"': case '\'': case '`': case '=': case ',': case ';': case '|':
        return true;
    default:
        return false;
    }
}

// ---------------------------------------------------------------------------
// UTF-8 helpers
// ---------------------------------------------------------------------------
inline bool is_cont(unsigned char c) { return (c & 0xC0) == 0x80; }

// Decode UTF-8 to code points, expanding tabs to 8-column tab stops (what a
// terminal does, so output captured from the screen never carries tabs; raw
// tab-separated text passed directly gets the same geometry).
std::u32string to_cps(std::string_view s) {
    std::u32string out;
    out.reserve(s.size());
    size_t i = 0, n = s.size();
    while (i < n) {
        unsigned char c = (unsigned char)s[i];
        if (c == '\t') {
            size_t col = out.size();
            size_t next = (col / 8 + 1) * 8;
            out.append(next - col, U' ');
            i++;
            continue;
        }
        if (c == '\r' || c == '\n') { i++; continue; }
        if (c < 0x80) { out.push_back(c); i++; continue; }
        int len = (c >= 0xF0) ? 4 : (c >= 0xE0) ? 3 : (c >= 0xC0) ? 2 : 1;
        char32_t cp;
        if (len == 1 || i + (size_t)len > n) { out.push_back(0xFFFD); i++; continue; }
        if (len == 2) cp = ((c & 0x1F) << 6) | ((unsigned char)s[i + 1] & 0x3F);
        else if (len == 3) cp = ((c & 0x0F) << 12) | (((unsigned char)s[i + 1] & 0x3F) << 6) | ((unsigned char)s[i + 2] & 0x3F);
        else cp = ((c & 0x07) << 18) | (((unsigned char)s[i + 1] & 0x3F) << 12) | (((unsigned char)s[i + 2] & 0x3F) << 6) | ((unsigned char)s[i + 3] & 0x3F);
        out.push_back(cp);
        i += (size_t)len;
    }
    return out;
}

void append_utf8(std::string& s, char32_t cp) {
    if (cp < 0x80) s.push_back((char)cp);
    else if (cp < 0x800) { s.push_back((char)(0xC0 | (cp >> 6))); s.push_back((char)(0x80 | (cp & 0x3F))); }
    else if (cp < 0x10000) { s.push_back((char)(0xE0 | (cp >> 12))); s.push_back((char)(0x80 | ((cp >> 6) & 0x3F))); s.push_back((char)(0x80 | (cp & 0x3F))); }
    else { s.push_back((char)(0xF0 | (cp >> 18))); s.push_back((char)(0x80 | ((cp >> 12) & 0x3F))); s.push_back((char)(0x80 | ((cp >> 6) & 0x3F))); s.push_back((char)(0x80 | (cp & 0x3F))); }
}

std::string from_cps(const std::u32string& cps, size_t b, size_t e) {
    std::string out;
    out.reserve(e - b);
    for (size_t i = b; i < e && i < cps.size(); i++) append_utf8(out, cps[i]);
    return out;
}

// ---------------------------------------------------------------------------
// Keyword tables for error/warning classification
// ---------------------------------------------------------------------------
enum KwFlags : uint8_t {
    KW_LEFT = 1,        // require a word boundary on the left
    KW_RIGHT = 2,       // require a word boundary on the right
    KW_LINE_START = 4,  // only at column 0 (after optional whitespace)
    KW_NOT_AFTER_ZERO = 8, // skip when the preceding token is "0" ("0 failed")
};

struct Keyword {
    const char* text;
    uint8_t len;
    uint8_t flags;
};

#define KW(s, f) { s, (uint8_t)(sizeof(s) - 1), (uint8_t)(f) }

constexpr Keyword kErrorKw[] = {
    KW("error:", KW_LEFT),
    KW("error[E", KW_LEFT),                   // rustc: error[E0308]
    KW("Error:", 0),                          // also CamelCase: ZeroDivisionError:
    KW("ERROR", KW_LEFT | KW_RIGHT),
    KW("fatal:", KW_LEFT),
    KW("FATAL", KW_LEFT | KW_RIGHT),
    KW("panic:", KW_LEFT),
    KW("panicked at", KW_LEFT),
    KW("Traceback (most recent call last)", 0),
    KW("Exception", KW_RIGHT),                // NullPointerException, Exception:
    KW("No such file or directory", 0),
    KW("command not found", 0),
    KW("Permission denied", 0),
    KW("Segmentation fault", 0),
    KW("segfault", KW_LEFT),
    KW("failed", KW_LEFT | KW_RIGHT | KW_NOT_AFTER_ZERO),
    KW("FAILED", KW_LEFT | KW_RIGHT | KW_NOT_AFTER_ZERO),
    KW("undefined reference", 0),
    KW("npm ERR!", 0),
    KW("E: ", KW_LINE_START),                 // apt
    KW("] Error ", 0),                        // make: *** [Makefile:12: all] Error 1
    KW("Aborted (core dumped)", 0),
};

constexpr Keyword kWarningKw[] = {
    KW("warning:", KW_LEFT),
    KW("Warning:", 0),                        // DeprecationWarning:
    KW("WARNING", KW_LEFT | KW_RIGHT),
    KW("WARN", KW_LEFT | KW_RIGHT),
    KW("warn:", KW_LEFT),
    KW("deprecated", KW_LEFT | KW_RIGHT),
    KW("Deprecated", KW_LEFT | KW_RIGHT),
    KW("DEPRECATED", KW_LEFT | KW_RIGHT),
    KW("W: ", KW_LINE_START),                 // apt
};
#undef KW

struct KwTable {
    const Keyword* kws;
    size_t count;
    uint8_t first[256];   // first-byte bitmap so the scan skips most positions cheaply
    KwTable(const Keyword* k, size_t n) : kws(k), count(n) {
        std::memset(first, 0, sizeof first);
        for (size_t i = 0; i < n; i++) first[(unsigned char)k[i].text[0]] = 1;
    }
};

bool match_keywords(std::string_view line, const KwTable& tbl) {
    const size_t n = line.size();
    const char* d = line.data();
    size_t content_start = 0;
    while (content_start < n && is_space((unsigned char)d[content_start])) content_start++;

    for (size_t p = 0; p < n; p++) {
        unsigned char c = (unsigned char)d[p];
        if (!tbl.first[c]) continue;
        for (size_t ki = 0; ki < tbl.count; ki++) {
            const Keyword& k = tbl.kws[ki];
            if (k.text[0] != (char)c || p + k.len > n) continue;
            if (std::memcmp(d + p, k.text, k.len) != 0) continue;
            if ((k.flags & KW_LINE_START) && p != content_start) continue;
            if ((k.flags & KW_LEFT) && p > 0 && is_word((unsigned char)d[p - 1])) continue;
            if ((k.flags & KW_RIGHT) && p + k.len < n && is_word((unsigned char)d[p + k.len])) continue;
            if (k.flags & KW_NOT_AFTER_ZERO) {
                size_t q = p;
                while (q > 0 && d[q - 1] == ' ') q--;
                if (q > 0 && d[q - 1] == '0' && (q == 1 || !is_word((unsigned char)d[q - 2]))) continue;
            }
            return true;
        }
    }
    return false;
}

// ---------------------------------------------------------------------------
// Known file extensions and bare file names (for extension-based path detection)
// ---------------------------------------------------------------------------
bool known_extension(std::string_view ext) {
    char buf[12];
    if (ext.empty() || ext.size() >= sizeof buf) return false;
    for (size_t i = 0; i < ext.size(); i++) buf[i] = (char)lower((unsigned char)ext[i]);
    std::string_view e(buf, ext.size());
    static const std::unordered_set<std::string_view> kExts = {
        // source
        "c", "cc", "cpp", "cxx", "h", "hh", "hpp", "hxx", "inl", "ipp", "m", "mm", "java", "kt", "kts", "scala",
        "go", "rs", "py", "pyi", "pyx", "rb", "php", "pl", "pm", "lua", "sh", "bash", "zsh", "fish", "ps1",
        "js", "mjs", "cjs", "jsx", "ts", "tsx", "vue", "svelte", "html", "htm", "css", "scss", "sass", "less",
        "swift", "dart", "ex", "exs", "erl", "hrl", "hs", "lhs", "ml", "mli", "clj", "cljs", "el", "lisp",
        "scm", "rkt", "nim", "zig", "v", "sv", "vhd", "vhdl", "f", "f90", "f95", "asm", "s", "tex", "bib",
        "proto", "thrift", "graphql", "gql", "sol", "tf", "hcl", "nix", "cs", "fs", "fsx", "vb", "r", "jl",
        "ipynb", "cmake", "mk", "m4", "ac", "am", "in", "glsl", "vert", "frag", "wgsl", "cu", "cuh", "d",
        "groovy", "gradle", "sbt", "bat", "cmd", "awk", "sed", "vim", "diff", "patch",
        // config / data / text
        "json", "json5", "yaml", "yml", "toml", "ini", "cfg", "conf", "config", "xml", "md", "markdown", "rst",
        "txt", "log", "csv", "tsv", "sql", "lock", "env", "properties", "plist", "desktop", "service", "rc",
        "pem", "crt", "key", "pub", "asc",
        // binaries / archives / media
        "o", "a", "so", "dylib", "dll", "exe", "bin", "elf", "deb", "rpm", "tar", "gz", "tgz", "bz2", "xz",
        "zst", "zip", "7z", "rar", "jar", "war", "class", "pyc", "wasm", "iso", "img", "dmg", "apk",
        "png", "jpg", "jpeg", "gif", "bmp", "svg", "webp", "ico", "tiff", "pdf", "doc", "docx", "xls",
        "xlsx", "ppt", "pptx", "odt", "ods", "mp3", "mp4", "wav", "ogg", "flac", "mkv", "avi", "mov",
        "webm", "ttf", "otf", "woff", "woff2", "db", "sqlite", "sqlite3", "bak", "tmp", "pid", "sock",
        "out", "core", "dump", "map", "sum", "mod",
    };
    return kExts.count(e) != 0;
}

bool known_filename(std::string_view name) {
    static const std::unordered_set<std::string_view> kNames = {
        "Makefile", "makefile", "GNUmakefile", "Dockerfile", "Containerfile", "Jenkinsfile", "Vagrantfile",
        "Rakefile", "Gemfile", "Procfile", "Brewfile", "Justfile", "justfile", "BUILD", "WORKSPACE",
        "LICENSE", "LICENCE", "README", "CHANGELOG", "CHANGES", "TODO", "COPYING", "AUTHORS", "INSTALL",
        "NEWS", "NOTICE", "CONTRIBUTING", "Cargo.toml", "Cargo.lock", "CMakeLists.txt", "package.json",
    };
    return kNames.count(name) != 0;
}

bool has_known_extension(std::string_view path) {
    size_t slash = path.rfind('/');
    std::string_view base = slash == std::string_view::npos ? path : path.substr(slash + 1);
    if (base.empty()) return false;
    if (known_filename(base)) return true;
    size_t dot = base.rfind('.');
    if (dot == std::string_view::npos || dot == 0 || dot + 1 >= base.size()) return false;
    return known_extension(base.substr(dot + 1));
}

bool is_dotfile(std::string_view path) {
    size_t slash = path.rfind('/');
    std::string_view base = slash == std::string_view::npos ? path : path.substr(slash + 1);
    return base.size() >= 3 && base[0] == '.' && is_alpha((unsigned char)base[1]);
}

bool path_exists(const std::string& p, const std::string& cwd) {
    std::string full;
    if (!p.empty() && p[0] == '/') {
        full = p;
    } else if (!p.empty() && p[0] == '~') {
        const char* h = std::getenv("HOME");
        full = std::string(h ? h : "") + p.substr(1);
    } else {
        full = cwd.empty() ? p : cwd + "/" + p;
    }
    struct stat st;
    return ::stat(full.c_str(), &st) == 0;
}

// ---------------------------------------------------------------------------
// Matchers. Each works on [i, wend) where wend is the end of the whitespace-
// delimited word; on success sets *end (byte offset past the match).
// ---------------------------------------------------------------------------
bool known_scheme(std::string_view s) {
    static const std::unordered_set<std::string_view> kSchemes = {"http", "https", "ftp", "ftps", "sftp", "file", "ssh", "git", "ws", "wss", "git+ssh", "git+https", "ssh+git"};
    char buf[16];
    if (s.size() >= sizeof buf) return false;
    for (size_t i = 0; i < s.size(); i++) buf[i] = (char)lower((unsigned char)s[i]);
    return kSchemes.count(std::string_view(buf, s.size())) != 0;
}

// Consume a URL body starting right after "://" at position i.
size_t url_body_end(std::string_view line, size_t i, size_t wend) {
    int paren = 0, bracket = 0;
    size_t p = i;
    for (; p < wend; p++) {
        unsigned char c = (unsigned char)line[p];
        if (c < 0x21 || c == '<' || c == '>' || c == '"' || c == '`' || c == '\\' || c == '^' || c == '{' || c == '}' || c == '|') break;
        if (c == '(') paren++;
        else if (c == ')') { if (paren == 0) break; paren--; }
        else if (c == '[') bracket++;
        else if (c == ']') { if (bracket == 0) break; bracket--; }
    }
    // Strip trailing punctuation that is almost never part of a URL.
    while (p > i) {
        unsigned char c = (unsigned char)line[p - 1];
        if (c == '.' || c == ',' || c == ';' || c == ':' || c == '!' || c == '?' || c == '\'' || c == '*') p--;
        else if (c == ')' && paren == 0 && std::count(line.begin() + (long)i, line.begin() + (long)p, '(') < std::count(line.begin() + (long)i, line.begin() + (long)p, ')')) p--;
        else break;
    }
    return p;
}

bool match_url(std::string_view line, size_t i, size_t wend, size_t* end) {
    if (!is_alpha((unsigned char)line[i])) return false;
    size_t p = i + 1;
    while (p < wend && (is_alnum((unsigned char)line[p]) || line[p] == '+' || line[p] == '-' || line[p] == '.')) p++;
    if (p + 3 > wend || line[p] != ':' || line[p + 1] != '/' || line[p + 2] != '/') return false;
    if (!known_scheme(line.substr(i, p - i))) return false;
    size_t body = p + 3;
    size_t e = url_body_end(line, body, wend);
    if (e <= body) return false;
    *end = e;
    return true;
}

bool match_email(std::string_view line, size_t i, size_t wend, size_t* end) {
    size_t p = i;
    auto local_ok = [](unsigned char c) { return is_alnum(c) || c == '.' || c == '_' || c == '%' || c == '+' || c == '-'; };
    while (p < wend && local_ok((unsigned char)line[p])) p++;
    if (p == i || p >= wend || line[p] != '@') return false;
    if (p - i > 64) return false;
    size_t at = p;
    p++;
    // domain: labels separated by dots, TLD alphabetic >= 2
    size_t last_label = p;
    int dots = 0;
    while (p < wend && (is_alnum((unsigned char)line[p]) || line[p] == '-' || line[p] == '.')) {
        if (line[p] == '.') {
            if (p == last_label) return false;   // empty label
            dots++;
            last_label = p + 1;
        }
        p++;
    }
    // Trim trailing dots (sentence end).
    while (p > at + 1 && line[p - 1] == '.') { p--; dots--; last_label = p; }
    if (dots < 1) return false;
    // Recompute the last label.
    size_t ll = p;
    while (ll > at + 1 && line[ll - 1] != '.') ll--;
    std::string_view tld = line.substr(ll, p - ll);
    if (tld.size() < 2) return false;
    for (unsigned char c : tld) if (!is_alpha(c)) return false;
    *end = p;
    return true;
}

bool match_ipv4(std::string_view line, size_t i, size_t wend, size_t* end) {
    if (!is_digit((unsigned char)line[i])) return false;
    if (i > 0 && (is_word((unsigned char)line[i - 1]) || line[i - 1] == '.')) return false;
    size_t p = i;
    for (int oct = 0; oct < 4; oct++) {
        if (oct) {
            if (p >= wend || line[p] != '.') return false;
            p++;
        }
        size_t s = p;
        int v = 0;
        while (p < wend && is_digit((unsigned char)line[p]) && p - s < 3) v = v * 10 + (line[p++] - '0');
        if (p == s || v > 255) return false;
        if (p < wend && is_digit((unsigned char)line[p])) return false;
    }
    size_t e = p;
    // Optional :port
    if (p + 1 < wend && line[p] == ':' && is_digit((unsigned char)line[p + 1])) {
        size_t q = p + 1;
        int port = 0;
        while (q < wend && is_digit((unsigned char)line[q]) && q - p <= 5) port = port * 10 + (line[q++] - '0');
        if (port <= 65535 && (q >= wend || !is_alnum((unsigned char)line[q]))) e = q;
    }
    if (e < wend) {
        unsigned char c = (unsigned char)line[e];
        if (is_alnum(c) || c == '-' || c == '_') return false;
        if (c == '.' && e + 1 < wend && is_digit((unsigned char)line[e + 1])) return false;   // 5-part dotted thing
    }
    *end = e;
    return true;
}

bool match_git_hash(std::string_view line, size_t i, size_t wend, size_t* end) {
    if (!is_lhex((unsigned char)line[i])) return false;
    size_t p = i;
    bool has_digit = false, has_alpha = false;
    while (p < wend && is_lhex((unsigned char)line[p])) {
        if (is_digit((unsigned char)line[p])) has_digit = true; else has_alpha = true;
        p++;
    }
    size_t len = p - i;
    if (len < 7 || len > 40 || !has_digit || !has_alpha) return false;
    // Left context: line start, whitespace, or an opening bracket/quote, or "commit ".
    if (i > 0) {
        unsigned char l = (unsigned char)line[i - 1];
        if (!(is_space(l) || l == '(' || l == '[' || l == '"' || l == '\'' || l == '/')) return false;
    }
    // Right context: end, whitespace, or closing punctuation.
    if (p < wend) {
        unsigned char r = (unsigned char)line[p];
        if (!(r == ')' || r == ']' || r == ',' || r == '.' || r == ':' || r == ';' || r == '"' || r == '\'' || r == '/')) return false;
        if (r == '.' && p + 1 < wend && line[p + 1] != '.' && !is_space((unsigned char)line[p + 1])) return false;
    }
    *end = p;
    return true;
}

// Path scan: returns the end of the bare path (no :LINE suffix) and fills
// line_no/col/suffix_end when a gcc/rust-style or python-style suffix follows.
struct PathMatch {
    size_t end = 0;         // end of the path text
    size_t span_end = 0;    // end including :LINE[:COL] suffix
    int line_no = 0;
};

inline bool is_path_char(unsigned char c) {
    if (c < 0x21) return false;
    switch (c) {
    case '"': case '\'': case '`': case '<': case '>': case '(': case ')': case '[': case ']': case '{': case '}':
    case ',': case ';': case '|': case '=': case ':': case '*': case '?': case '\\': case '&':
        return false;
    default:
        return true;
    }
}

bool match_path(std::string_view line, size_t i, size_t wend, const std::string& cwd, bool check_fs, PathMatch* m) {
    unsigned char c0 = (unsigned char)line[i];
    // Windows-style (C:\...) and backslashes: not handled.
    if (i + 1 < wend && is_alpha(c0) && line[i + 1] == ':' && i + 2 < wend && (line[i + 2] == '\\' || line[i + 2] == '/')) return false;
    if (c0 == '/' && i + 1 < wend && line[i + 1] == '/') return false;   // "//comment"
    size_t p = i;
    while (p < wend && is_path_char((unsigned char)line[p])) p++;
    // Strip trailing sentence punctuation.
    while (p > i && (line[p - 1] == '.' || line[p - 1] == '!')) p--;
    if (p == i) return false;
    std::string_view path = line.substr(i, p - i);
    if (path.size() < 2) return false;
    if (path == "./" || path == "../" || path == "..") return false;
    if (path.find('\\') != std::string_view::npos) return false;

    bool tier_a = path[0] == '/' || (path[0] == '~' && path.size() > 1 && path[1] == '/') ||
                  path.compare(0, 2, "./") == 0 || path.compare(0, 3, "../") == 0;
    bool has_slash = path.find('/') != std::string_view::npos;
    bool ext = has_known_extension(path);
    bool dotfile = is_dotfile(path);
    if (!tier_a) {
        // Relative candidates must carry a slash, a known extension, or be a dotfile.
        if (!has_slash && !ext && !dotfile) return false;
        // First char must be a plausible filename start.
        if (!(is_alnum(c0) || c0 == '_' || c0 == '.' || c0 == '-' || c0 == '@' || c0 == '~')) return false;
        if (c0 == '~') return false;   // "~user/..." not supported
        // Pure digits/slashes/dots: dates, fractions, version numbers.
        bool only_num = true;
        for (unsigned char c : path) if (!(is_digit(c) || c == '/' || c == '.' || c == '-')) { only_num = false; break; }
        if (only_num) return false;
        // A relative token that contains '@' is probably an scp target or an email.
        if (path.find('@') != std::string_view::npos) return false;
    }

    // Optional :LINE[:COL] suffix.
    size_t span_end = p;
    int line_no = 0;
    if (p + 1 < wend && line[p] == ':' && is_digit((unsigned char)line[p + 1])) {
        size_t q = p + 1;
        long v = 0;
        while (q < wend && is_digit((unsigned char)line[q]) && q - p <= 8) v = v * 10 + (line[q++] - '0');
        bool ok = (q >= wend || !is_alnum((unsigned char)line[q]));
        if (ok) {
            line_no = (int)v;
            span_end = q;
            if (q + 1 < wend && line[q] == ':' && is_digit((unsigned char)line[q + 1])) {
                size_t r = q + 1;
                while (r < wend && is_digit((unsigned char)line[r])) r++;
                if (r >= wend || !is_alnum((unsigned char)line[r])) span_end = r;
            }
        }
    } else if (p + 8 <= line.size() && line.compare(p, 8, "\", line ") == 0 && p + 8 < line.size() && is_digit((unsigned char)line[p + 8])) {
        // Python: File "x.py", line 12, in f
        size_t q = p + 8;
        long v = 0;
        while (q < line.size() && is_digit((unsigned char)line[q])) v = v * 10 + (line[q++] - '0');
        line_no = (int)v;
    }

    std::string pstr(path);
    bool keep;
    if (check_fs) {
        keep = path_exists(pstr, cwd) || line_no > 0 || ext;
    } else {
        keep = tier_a || line_no > 0 || ext;
    }
    if (!keep) return false;
    m->end = p;
    m->span_end = span_end;
    m->line_no = line_no;
    return true;
}

// ---------------------------------------------------------------------------
// Numeric cell parsing (tables)
// ---------------------------------------------------------------------------
// Parses "1,234", "-12.5", "45%", "457G", "1.5Gi", "13000K", "12MB", "+3".
// Returns NaN if the cell is not numeric.
double parse_cell_number(std::string_view s) {
    size_t b = 0, e = s.size();
    while (b < e && is_space((unsigned char)s[b])) b++;
    while (e > b && is_space((unsigned char)s[e - 1])) e--;
    if (b == e) return NAN;
    size_t p = b;
    bool neg = false;
    if (s[p] == '-' || s[p] == '+') { neg = s[p] == '-'; p++; }
    double v = 0;
    int digits = 0;
    while (p < e && (is_digit((unsigned char)s[p]) || s[p] == ',')) {
        if (s[p] == ',') { if (p + 1 >= e || !is_digit((unsigned char)s[p + 1])) return NAN; }
        else { v = v * 10 + (s[p] - '0'); digits++; }
        p++;
    }
    if (p < e && s[p] == '.') {
        p++;
        double scale = 0.1;
        while (p < e && is_digit((unsigned char)s[p])) { v += (s[p] - '0') * scale; scale *= 0.1; digits++; p++; }
    }
    if (digits == 0) return NAN;
    if (p < e && (s[p] == 'e' || s[p] == 'E') && p + 1 < e && (is_digit((unsigned char)s[p + 1]) || s[p + 1] == '-' || s[p + 1] == '+')) {
        size_t q = p + 1;
        bool eneg = false;
        if (s[q] == '-' || s[q] == '+') { eneg = s[q] == '-'; q++; }
        int ex = 0;
        size_t qs = q;
        while (q < e && is_digit((unsigned char)s[q])) ex = ex * 10 + (s[q++] - '0');
        if (q > qs) { v *= std::pow(10.0, eneg ? -ex : ex); p = q; }
    }
    if (p < e) {
        unsigned char c = (unsigned char)s[p];
        double mult = 0;
        switch (lower(c)) {
        case 'k': mult = 1024.0; break;
        case 'm': mult = 1024.0 * 1024; break;
        case 'g': mult = 1024.0 * 1024 * 1024; break;
        case 't': mult = 1024.0 * 1024 * 1024 * 1024; break;
        case 'p': mult = 1024.0 * 1024 * 1024 * 1024 * 1024; break;
        case 'b': mult = 1; break;     // "0B", "512B"
        case '%': mult = 1; break;
        default: return NAN;
        }
        p++;
        if (c != '%' && lower(c) != 'b') {
            if (p < e && (s[p] == 'i' || s[p] == 'I')) p++;
            if (p < e && (s[p] == 'B' || s[p] == 'b')) p++;
        }
        if (p != e) return NAN;
        v *= mult;
    }
    return neg ? -v : v;
}

std::u32string trim_cps(const std::u32string& s, size_t b, size_t e) {
    e = std::min(e, s.size());
    b = std::min(b, e);
    while (b < e && (s[b] == U' ' || s[b] == U'\t')) b++;
    while (e > b && (s[e - 1] == U' ' || s[e - 1] == U'\t')) e--;
    return s.substr(b, e - b);
}

std::string cell_at(const std::u32string& cps, const TableInfo& t, size_t col) {
    size_t b = (size_t)t.col_begins[col];
    size_t e = col + 1 < t.col_begins.size() ? (size_t)t.col_begins[col + 1] : cps.size();
    std::u32string c = trim_cps(cps, b, e);
    return from_cps(c, 0, c.size());
}

bool all_caps_word(const std::string& s) {
    int letters = 0;
    for (unsigned char c : s) {
        if (is_upper(c)) letters++;
        else if (is_alpha(c)) return false;
    }
    return letters >= 2;
}

} // namespace

// ===========================================================================
// Public API
// ===========================================================================
bool is_error_line(std::string_view line) {
    static const KwTable tbl(kErrorKw, sizeof(kErrorKw) / sizeof(kErrorKw[0]));
    return match_keywords(line, tbl);
}
bool is_warning_line(std::string_view line) {
    static const KwTable tbl(kWarningKw, sizeof(kWarningKw) / sizeof(kWarningKw[0]));
    return match_keywords(line, tbl);
}

std::vector<Span> scan_line(std::string_view line, const std::string& cwd, bool check_fs) {
    std::vector<Span> spans;
    const size_t n = line.size();
    if (n == 0) return spans;

    // Byte -> code point column mapping (identity for ASCII lines).
    bool ascii = true;
    for (size_t i = 0; i < n; i++) if ((unsigned char)line[i] >= 0x80) { ascii = false; break; }
    std::vector<int> cols;
    if (!ascii) {
        cols.resize(n + 1);
        int c = 0;
        for (size_t i = 0; i < n; i++) {
            cols[i] = c;
            if (!is_cont((unsigned char)line[i])) c++;
        }
        cols[n] = c;
    }
    auto col_of = [&](size_t byte) -> int { return ascii ? (int)byte : cols[byte]; };
    int total_cols = col_of(n);

    if (is_error_line(line)) {
        spans.push_back(Span{SpanKind::Error, 0, total_cols, std::string(line), 0});
    } else if (is_warning_line(line)) {
        spans.push_back(Span{SpanKind::Warning, 0, total_cols, std::string(line), 0});
    }

    auto emit = [&](SpanKind k, size_t b, size_t e, size_t value_end, int line_no) {
        Span s;
        s.kind = k;
        s.col_begin = col_of(b);
        s.col_end = col_of(e);
        s.value.assign(line.data() + b, value_end - b);
        s.line_no = line_no;
        spans.push_back(std::move(s));
    };

    size_t i = 0;
    while (i < n) {
        unsigned char c = (unsigned char)line[i];
        if (is_space(c)) { i++; continue; }
        if (is_opener(c)) { i++; continue; }
        // End of the whitespace-delimited word.
        size_t wend = i;
        while (wend < n && !is_space((unsigned char)line[wend])) wend++;

        size_t end = 0;
        PathMatch pm;
        if (match_url(line, i, wend, &end)) {
            emit(SpanKind::Url, i, end, end, 0);
            i = end;
            continue;
        }
        if (match_email(line, i, wend, &end)) {
            emit(SpanKind::Email, i, end, end, 0);
            i = end;
            continue;
        }
        if (match_ipv4(line, i, wend, &end)) {
            emit(SpanKind::IpAddr, i, end, end, 0);
            i = end;
            continue;
        }
        if ((c == '/' || c == '~' || c == '.' || is_alnum(c) || c == '_' || c == '-' || c == '@') &&
            match_path(line, i, wend, cwd, check_fs, &pm)) {
            emit(SpanKind::Path, i, pm.span_end, pm.end, pm.line_no);
            i = pm.span_end;
            continue;
        }
        if (match_git_hash(line, i, wend, &end)) {
            emit(SpanKind::GitHash, i, end, end, 0);
            i = end;
            continue;
        }
        // No match: skip the rest of this word, but stop at openers and at
        // embedded URL schemes ("see:https://...") or ":/path" suffixes.
        size_t p = i;
        while (p < wend) {
            unsigned char d = (unsigned char)line[p];
            if (is_opener(d)) break;
            if (d == ':') {
                if (p + 1 < wend && (line[p + 1] == '/' || line[p + 1] == '~')) {
                    // ":/x" is a path start unless it is "://" (handled by the URL rule below).
                    if (!(p + 2 < wend && line[p + 1] == '/' && line[p + 2] == '/')) break;
                }
                if (p + 2 < wend && line[p + 1] == '/' && line[p + 2] == '/') {
                    // Look back for a scheme.
                    size_t s = p;
                    while (s > i && (is_alnum((unsigned char)line[s - 1]) || line[s - 1] == '+' || line[s - 1] == '-' || line[s - 1] == '.')) s--;
                    while (s < p && !is_alpha((unsigned char)line[s])) s++;
                    if (s < p && known_scheme(line.substr(s, p - s))) {
                        size_t body = p + 3;
                        size_t e = url_body_end(line, body, wend);
                        if (e > body) {
                            emit(SpanKind::Url, s, e, e, 0);
                            p = e;
                            break;
                        }
                    }
                }
            }
            p++;
        }
        i = (p == i) ? i + 1 : p;
        if (p < wend && (line[p] == ':')) i = p + 1;
    }

    std::stable_sort(spans.begin(), spans.end(), [](const Span& a, const Span& b) { return a.col_begin < b.col_begin; });
    return spans;
}

// ---------------------------------------------------------------------------
// Tables
// ---------------------------------------------------------------------------
TableInfo detect_table(const std::vector<std::string>& lines) {
    TableInfo t;
    // Collect up to 200 non-empty lines (indices + code points).
    struct Row { int idx; std::u32string cps; };
    std::vector<Row> rows;
    for (size_t i = 0; i < lines.size() && rows.size() < 200; i++) {
        std::u32string cps = to_cps(lines[i]);
        while (!cps.empty() && cps.back() == U' ') cps.pop_back();
        bool blank = true;
        for (char32_t c : cps) if (c != U' ') { blank = false; break; }
        if (blank) continue;
        rows.push_back(Row{(int)i, std::move(cps)});
    }
    const size_t nrows = rows.size();
    if (nrows < 3) return t;

    // Compiler-style diagnostics are not tables, even if they happen to align.
    {
        size_t diag = 0;
        for (const auto& r : rows) {
            const std::string& l = lines[(size_t)r.idx];
            if (is_error_line(l) || is_warning_line(l)) diag++;
        }
        if (diag * 2 >= nrows) return t;
    }

    size_t width = 0;
    for (const auto& r : rows) width = std::max(width, r.cps.size());
    if (width < 3) return t;

    // Separator mask: position p is a separator when >= 80% of rows have a
    // space there (rows shorter than p count as space).
    const size_t need = (nrows * 4 + 4) / 5;   // ceil(0.8 * nrows)
    std::vector<uint16_t> space_count(width, 0);
    for (const auto& r : rows) {
        const auto& s = r.cps;
        for (size_t p = 0; p < width; p++) {
            if (p >= s.size() || s[p] == U' ') space_count[p]++;
        }
    }
    std::vector<bool> sep(width, false);
    for (size_t p = 0; p < width; p++) sep[p] = space_count[p] >= need;

    // Column starts: position 0, plus one per run of separators. Inside a run
    // the boundary goes right after the last position that is blank in EVERY
    // row: the tail of the run that only 80-99% of rows leave blank is the
    // leading padding of a right-aligned column (a 5-digit size among 4-digit
    // ones in `ls -l`) and must stay inside that column.
    std::vector<int> begins;
    begins.push_back(0);
    for (size_t p = 1; p < width; p++) {
        if (!sep[p] && sep[p - 1]) {
            size_t a = p - 1;
            while (a > 0 && sep[a - 1]) a--;
            size_t boundary = p;
            for (size_t q = p; q > a; q--) {
                if (space_count[q - 1] == nrows) { boundary = q; break; }
            }
            begins.push_back((int)boundary);
        }
    }
    if (sep[0]) {
        // Leading gap: the first real column starts after it, but keep column 0
        // anchored at 0 so stray content in the margin stays in column 0.
        // If the first begin after the gap exists, merge: [0, begins[1]) is column 0.
        if (begins.size() >= 2) begins.erase(begins.begin() + 1);
    }
    if (begins.size() < 2) return t;

    // Column content presence: each column must have a non-space cell in >= 50% of rows,
    // otherwise it is merged into the previous one (ragged tail / spaces in filenames).
    // (The separator before it is already blank in >= 80% of rows, so this only
    // rejects columns that exist in a minority of rows.)
    auto column_filled = [&](size_t b, size_t e, const std::u32string& s) {
        for (size_t p = b; p < e && p < s.size(); p++) if (s[p] != U' ') return true;
        return false;
    };
    {
        std::vector<int> kept;
        kept.push_back(begins[0]);
        for (size_t k = 1; k < begins.size(); k++) {
            size_t b = (size_t)begins[k];
            size_t e = k + 1 < begins.size() ? (size_t)begins[k + 1] : width;
            size_t filled = 0;
            for (const auto& r : rows) if (column_filled(b, e, r.cps)) filled++;
            if (filled * 2 >= nrows) kept.push_back(begins[k]);
            // else: dropped => merges into the previous column
        }
        begins = std::move(kept);
    }
    if (begins.size() < 2) return t;

    // Row conformance: no non-space at a separator that bounds a kept column,
    // and content in at least half the columns.
    const size_t ncols = begins.size();
    auto conforms = [&](const std::u32string& s) {
        for (size_t k = 1; k < ncols; k++) {
            size_t p = (size_t)begins[k] - 1;
            if (p < s.size() && s[p] != U' ') return false;
        }
        size_t filled = 0;
        for (size_t k = 0; k < ncols; k++) {
            size_t b = (size_t)begins[k];
            size_t e = k + 1 < ncols ? (size_t)begins[k + 1] : width;
            if (column_filled(b, e, s)) filled++;
        }
        return filled * 2 >= ncols;
    };
    std::vector<bool> conf(nrows);
    size_t nconf = 0;
    for (size_t r = 0; r < nrows; r++) { conf[r] = conforms(rows[r].cps); if (conf[r]) nconf++; }
    if (nconf < 3 || nconf * 4 < nrows * 3) return t;   // >= 75% of rows must fit the grid

    // First conforming row; leading odd rows ("total 48") are skipped.
    size_t first = 0;
    while (first < nrows && !conf[first]) first++;
    if (first >= nrows) return t;

    TableInfo cand;
    cand.col_begins = begins;

    // Cells of conforming rows.
    std::vector<std::vector<std::string>> cells(nrows);
    for (size_t r = first; r < nrows; r++) {
        if (!conf[r]) continue;
        cells[r].resize(ncols);
        for (size_t k = 0; k < ncols; k++) cells[r][k] = cell_at(rows[r].cps, cand, k);
    }

    // Header detection on the first conforming row.
    auto numeric_stats = [&](size_t k, size_t from, size_t* nonempty, size_t* numeric) {
        *nonempty = *numeric = 0;
        for (size_t r = from; r < nrows; r++) {
            if (!conf[r]) continue;
            const std::string& c = cells[r][k];
            if (c.empty()) continue;
            (*nonempty)++;
            if (!std::isnan(parse_cell_number(c))) (*numeric)++;
        }
    };
    bool header = false;
    {
        const auto& h = cells[first];
        size_t alpha = 0, caps = 0, numeric_h = 0, nonempty_h = 0;
        for (size_t k = 0; k < ncols; k++) {
            if (h[k].empty()) continue;
            nonempty_h++;
            if (is_alpha((unsigned char)h[k][0])) alpha++;
            if (all_caps_word(h[k])) caps++;
            if (!std::isnan(parse_cell_number(h[k]))) numeric_h++;
        }
        if (nonempty_h > 0 && numeric_h == 0 && alpha * 2 >= nonempty_h) {
            // Type difference with the data rows below.
            bool differs = false;
            size_t data_caps = 0, data_cells = 0;
            for (size_t k = 0; k < ncols && !differs; k++) {
                size_t ne, nu;
                numeric_stats(k, first + 1, &ne, &nu);
                if (ne >= 2 && nu * 5 >= ne * 4 && !h[k].empty()) differs = true;
            }
            for (size_t r = first + 1; r < nrows; r++) {
                if (!conf[r]) continue;
                for (size_t k = 0; k < ncols; k++) {
                    if (cells[r][k].empty()) continue;
                    data_cells++;
                    if (all_caps_word(cells[r][k])) data_caps++;
                }
            }
            if (!differs && caps * 2 >= nonempty_h && data_cells > 0 && data_caps * 2 < data_cells) differs = true;
            header = differs;
        }
    }
    size_t data_from = header ? first + 1 : first;
    size_t data_rows = 0;
    for (size_t r = data_from; r < nrows; r++) if (conf[r]) data_rows++;
    if (data_rows < 2 || (header ? nrows : data_rows) < 3) return t;

    // Numeric columns and informative-column count.
    cand.numeric.assign(ncols, false);
    size_t informative = 0;
    size_t numeric_cols = 0;
    for (size_t k = 0; k < ncols; k++) {
        size_t ne, nu;
        numeric_stats(k, data_from, &ne, &nu);
        if (ne >= 1 && nu * 5 >= ne * 4) { cand.numeric[k] = true; numeric_cols++; }
        // A column whose data cells are all identical carries no information
        // (bullet markers, repeated "error:" tokens, ...).
        bool constant = true;
        const std::string* firstv = nullptr;
        for (size_t r = data_from; r < nrows && constant; r++) {
            if (!conf[r]) continue;
            if (!firstv) firstv = &cells[r][k];
            else if (cells[r][k] != *firstv) constant = false;
        }
        if (!constant || data_rows < 2) informative++;
    }
    if (informative < 3 && !(informative == 2 && numeric_cols > 0)) return t;

    cand.is_table = true;
    cand.header_row = header ? rows[first].idx : -1;
    cand.first_data_row = rows[data_from].idx;
    return cand;
}

std::vector<std::string> split_columns(std::string_view line, const TableInfo& t) {
    std::vector<std::string> out;
    if (t.col_begins.empty()) { out.emplace_back(line); return out; }
    std::u32string cps = to_cps(line);
    out.reserve(t.col_begins.size());
    for (size_t k = 0; k < t.col_begins.size(); k++) out.push_back(cell_at(cps, t, k));
    return out;
}

std::vector<int> sort_rows(const std::vector<std::string>& lines, const TableInfo& t, int col, bool descending) {
    std::vector<int> idx;
    if (col < 0 || (size_t)col >= t.col_begins.size()) {
        for (size_t i = (size_t)std::max(0, t.first_data_row); i < lines.size(); i++) idx.push_back((int)i);
        return idx;
    }
    bool numeric = (size_t)col < t.numeric.size() && t.numeric[(size_t)col];
    struct Key { std::string s; double v; bool has_v; };
    std::vector<Key> keys(lines.size());
    for (size_t i = (size_t)std::max(0, t.first_data_row); i < lines.size(); i++) {
        idx.push_back((int)i);
        std::u32string cps = to_cps(lines[i]);
        Key k;
        k.s = cell_at(cps, t, (size_t)col);
        for (auto& ch : k.s) ch = (char)lower((unsigned char)ch);
        k.v = numeric ? parse_cell_number(k.s) : NAN;
        k.has_v = !std::isnan(k.v);
        keys[i] = std::move(k);
    }
    auto less = [&](int a, int b) {
        const Key& ka = keys[(size_t)a];
        const Key& kb = keys[(size_t)b];
        if (numeric) {
            if (ka.has_v && kb.has_v) return ka.v < kb.v;
            if (ka.has_v != kb.has_v) return ka.has_v;   // numbers before non-numbers
        }
        // Empty cells sort last.
        if (ka.s.empty() != kb.s.empty()) return !ka.s.empty();
        return ka.s < kb.s;
    };
    if (descending) std::stable_sort(idx.begin(), idx.end(), [&](int a, int b) { return less(b, a); });
    else std::stable_sort(idx.begin(), idx.end(), less);
    return idx;
}

std::vector<double> numeric_column(const std::vector<std::string>& lines, const TableInfo& t, int col) {
    std::vector<double> out;
    if (!t.is_table || col < 0 || (size_t)col >= t.col_begins.size()) return out;
    if ((size_t)col >= t.numeric.size() || !t.numeric[(size_t)col]) return out;
    for (size_t i = (size_t)std::max(0, t.first_data_row); i < lines.size(); i++) {
        std::u32string cps = to_cps(lines[i]);
        double v = parse_cell_number(cell_at(cps, t, (size_t)col));
        if (!std::isnan(v)) out.push_back(v);
    }
    return out;
}

// ---------------------------------------------------------------------------
// JSON
// ---------------------------------------------------------------------------
bool looks_like_json(std::string_view text) {
    size_t b = 0, e = text.size();
    while (b < e && is_space((unsigned char)text[b])) b++;
    while (e > b && is_space((unsigned char)text[e - 1])) e--;
    if (e - b < 2) return false;
    char first = text[b], last = text[e - 1];
    if (!((first == '{' && last == '}') || (first == '[' && last == ']'))) return false;
    // Balanced-bracket check ignoring brackets inside strings.
    std::vector<char> stack;
    bool in_str = false;
    for (size_t i = b; i < e; i++) {
        char c = text[i];
        if (in_str) {
            if (c == '\\') i++;
            else if (c == '"') in_str = false;
            continue;
        }
        switch (c) {
        case '"': in_str = true; break;
        case '{': case '[': stack.push_back(c); break;
        case '}': if (stack.empty() || stack.back() != '{') return false; stack.pop_back(); break;
        case ']': if (stack.empty() || stack.back() != '[') return false; stack.pop_back(); break;
        default: break;
        }
        if (stack.empty() && i + 1 < e) return false;   // closed early: trailing garbage
    }
    return !in_str && stack.empty();
}

std::optional<std::string> pretty_json(std::string_view text, int indent) {
    auto j = Json::parse(text);
    if (!j) return std::nullopt;
    return j->dump(indent > 0 ? indent : 2);
}

std::optional<std::string> compact_json(std::string_view text) {
    auto j = Json::parse(text);
    if (!j) return std::nullopt;
    return j->dump(0);
}

} // namespace canopy
