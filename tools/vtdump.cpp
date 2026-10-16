// vtdump: run a command under canopy::Session headlessly and dump the screen as text.
//
//   vtdump [-c cols] [-r rows] [-t seconds] [--blocks] [--attrs] [--keys "text"]
//          [--keys-delay s] [--send-after s "bytes"]... [--resize-after s cols rows]... [--no-integration] [--history] [-- cmd args...]
//
// Escapes in --keys / --send-after: \r \n \t \e \\ \xHH. Without a command, the user's shell with
// Canopy's bash integration is started (Session's default path).
#include "core/session.h"
#include "core/unicode.h"
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

using namespace canopy;

static std::string unescape(const std::string& s) {
    std::string out;
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] != '\\' || i + 1 >= s.size()) { out.push_back(s[i]); continue; }
        char c = s[++i];
        switch (c) {
        case 'r': out.push_back('\r'); break;
        case 'n': out.push_back('\n'); break;
        case 't': out.push_back('\t'); break;
        case 'e': out.push_back('\x1b'); break;
        case 'a': out.push_back('\a'); break;
        case '0': out.push_back('\0'); break;
        case '\\': out.push_back('\\'); break;
        case 'x': {
            if (i + 2 < s.size()) { out.push_back((char)strtol(s.substr(i + 1, 2).c_str(), nullptr, 16)); i += 2; }
            break;
        }
        default: out.push_back('\\'); out.push_back(c); break;
        }
    }
    return out;
}

static std::string color_str(const Color& c) {
    char b[32];
    if (c.kind == Color::Default) return "-";
    if (c.kind == Color::Indexed) { snprintf(b, sizeof b, "%d", c.idx); return b; }
    snprintf(b, sizeof b, "#%02x%02x%02x", c.r, c.g, c.b);
    return b;
}

static void print_line(const Line& l, int cols) {
    std::string s;
    int width = 0;
    for (int x = 0; x < l.width() && x < cols; ++x) {
        const Cell& c = l.cells[(size_t)x];
        if (c.attrs & ATTR_WIDE_CONT) continue;
        char32_t cp = c.cp == 0 ? U' ' : c.cp;
        utf8_append(s, cp);
        if (c.combining && c.combining <= l.combining.size())
            for (char32_t m : l.combining[c.combining - 1]) utf8_append(s, m);
        width += (c.attrs & ATTR_WIDE) ? 2 : 1;
    }
    for (; width < cols; ++width) s.push_back(' ');
    printf("|%s|%s\n", s.c_str(), l.wrapped ? "+" : "");
}

int main(int argc, char** argv) {
    int cols = 80, rows = 24;
    double seconds = 1.5, keys_delay = 0.3;
    bool show_blocks = false, show_attrs = false, show_history = false, integration = true;
    std::string keys;
    std::vector<std::pair<double, std::string>> sends;
    struct Resize { double at; int cols, rows; bool done = false; };
    std::vector<Resize> resizes;
    std::vector<std::string> cmd;
    int i = 1;
    for (; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--") { ++i; break; }
        else if (a == "-c" && i + 1 < argc) cols = atoi(argv[++i]);
        else if (a == "-r" && i + 1 < argc) rows = atoi(argv[++i]);
        else if (a == "-t" && i + 1 < argc) seconds = atof(argv[++i]);
        else if (a == "--blocks") show_blocks = true;
        else if (a == "--attrs") show_attrs = true;
        else if (a == "--history") show_history = true;
        else if (a == "--no-integration") integration = false;
        else if (a == "--keys" && i + 1 < argc) keys = unescape(argv[++i]);
        else if (a == "--keys-delay" && i + 1 < argc) keys_delay = atof(argv[++i]);
        else if (a == "--send-after" && i + 2 < argc) { double t = atof(argv[++i]); sends.emplace_back(t, unescape(argv[++i])); }
        else if (a == "--resize-after" && i + 3 < argc) { Resize r; r.at = atof(argv[++i]); r.cols = atoi(argv[++i]); r.rows = atoi(argv[++i]); resizes.push_back(r); }
        else if (a == "-h" || a == "--help") { fprintf(stderr, "usage: vtdump [-c cols] [-r rows] [-t seconds] [--blocks] [--attrs] [--history] [--keys text] [--keys-delay s] [--send-after s bytes] [--no-integration] [-- cmd args...]\n"); return 0; }
        else { fprintf(stderr, "vtdump: unknown option %s\n", a.c_str()); return 2; }
    }
    for (; i < argc; ++i) cmd.push_back(argv[i]);
    if (!keys.empty()) sends.emplace_back(keys_delay, keys);

    Session s;
    Session::Options o;
    o.argv = cmd; o.cols = cols; o.rows = rows; o.shell_integration = integration;
    std::string err;
    if (!s.start(o, &err)) { fprintf(stderr, "vtdump: %s\n", err.c_str()); return 1; }
    std::vector<std::string> titles;
    s.term().on_title = [&](const std::string& t) { titles.push_back(t); };
    std::string bells; s.term().on_bell = [&] { bells += "*"; };

    using clock = std::chrono::steady_clock;
    auto t0 = clock::now();
    auto elapsed = [&] { return std::chrono::duration<double>(clock::now() - t0).count(); };
    std::vector<bool> sent(sends.size(), false);
    bool exited = false;
    double exit_at = -1;
    for (;;) {
        bool fed = s.pump();
        double e = elapsed();
        for (size_t k = 0; k < sends.size(); ++k) if (!sent[k] && e >= sends[k].first) { s.write(sends[k].second); sent[k] = true; }
        for (auto& r : resizes) if (!r.done && e >= r.at) { s.resize(r.cols, r.rows); cols = r.cols; rows = r.rows; r.done = true; }
        if (!exited && !s.alive()) { exited = true; exit_at = e; }
        if (exited && !fed) {
            // drain anything still queued, then stop
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
            if (!s.pump()) break;
        }
        if (e >= seconds) break;
        if (!fed) std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    while (s.pump()) {}

    const Terminal& t = s.term();
    std::string border(cols, '-');
    if (show_history) {
        printf("history: base=%lld count=%lld screen_base=%lld\n", (long long)t.history_base(), (long long)t.history_count(), (long long)t.screen_base());
        for (int64_t a = t.history_base(); a < t.screen_base(); ++a) { printf("%6lld ", (long long)a); print_line(t.history(a), cols); }
    }
    printf("+%s+\n", border.c_str());
    for (int y = 0; y < rows; ++y) print_line(t.row(y), cols);
    printf("+%s+\n", border.c_str());
    Cursor c = t.cursor();
    const TerminalModes& m = t.modes();
    printf("cursor: x=%d y=%d visible=%d style=%d blink=%d\n", c.x, c.y, c.visible, (int)c.style, c.blink);
    printf("modes: alt=%d app_cursor=%d app_keypad=%d bracketed=%d focus=%d mouse=%d enc=%d origin=%d autowrap=%d insert=%d lnm=%d\n",
           m.alt_screen, m.app_cursor_keys, m.app_keypad, m.bracketed_paste, m.focus_events, (int)m.mouse, (int)m.mouse_enc, m.origin, m.autowrap, m.insert, m.lnm);
    printf("title: \"%s\"  cwd: \"%s\"  bells: %zu  history: %lld lines (base %lld)\n", t.title().c_str(), t.cwd().c_str(), bells.size(), (long long)t.history_count(), (long long)t.history_base());
    if (exited) printf("exit: status=%d after %.2fs\n", s.exit_status(), exit_at);
    else printf("exit: still running after %.2fs\n", elapsed());
    if (show_attrs) {
        printf("attrs (runs with non-default attributes/colors):\n");
        for (int y = 0; y < rows; ++y) {
            const Line& l = t.row(y);
            int x = 0;
            while (x < l.width()) {
                const Cell& a = l.cells[(size_t)x];
                int e = x + 1;
                while (e < l.width() && l.cells[(size_t)e].fg == a.fg && l.cells[(size_t)e].bg == a.bg && l.cells[(size_t)e].attrs == a.attrs && l.cells[(size_t)e].ul_style == a.ul_style) ++e;
                if (a.fg != Color::def() || a.bg != Color::def() || a.attrs) {
                    std::string flags;
                    if (a.attrs & ATTR_BOLD) flags += "B";
                    if (a.attrs & ATTR_DIM) flags += "D";
                    if (a.attrs & ATTR_ITALIC) flags += "I";
                    if (a.attrs & ATTR_UNDERLINE) flags += "U" + std::to_string((int)a.ul_style);
                    if (a.attrs & ATTR_BLINK) flags += "K";
                    if (a.attrs & ATTR_INVERSE) flags += "R";
                    if (a.attrs & ATTR_STRIKE) flags += "S";
                    if (a.attrs & ATTR_WIDE) flags += "W";
                    printf("  row %d [%d,%d) fg=%s bg=%s %s \"%s\"\n", y, x, e, color_str(a.fg).c_str(), color_str(a.bg).c_str(), flags.c_str(), l.text(x, e).c_str());
                }
                x = e;
            }
        }
    }
    if (show_blocks) {
        static const char* names[] = {"Prompt", "Typing", "Running", "Done"};
        printf("blocks: %zu\n", s.blocks().blocks().size());
        for (const Block& b : s.blocks().blocks()) {
            printf("  #%llu %-7s exit=%d prompt=%lld cmd=%lld out=%lld end=%lld col_end=%d cwd=\"%s\" command=\"%s\"\n",
                   (unsigned long long)b.id, names[(int)b.state], b.exit_code, (long long)b.prompt_line, (long long)b.cmd_line,
                   (long long)b.output_line, (long long)b.end_line, b.output_col_end, b.cwd.c_str(), b.command.c_str());
            for (auto& m : b.marks) printf("      mark: \"%s\"\n", m.c_str());
        }
    }
    s.stop();
    return 0;
}
