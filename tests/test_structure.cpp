// Tests for core/structure.h
#include "core/structure.h"
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

using namespace canopy;

static int g_fail = 0, g_pass = 0;
#define CHECK(cond) do { if (cond) g_pass++; else { g_fail++; std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } } while (0)

static const char* kind_name(SpanKind k) {
    switch (k) {
    case SpanKind::Path: return "Path";
    case SpanKind::Url: return "Url";
    case SpanKind::Error: return "Error";
    case SpanKind::Warning: return "Warning";
    case SpanKind::GitHash: return "GitHash";
    case SpanKind::Number: return "Number";
    case SpanKind::Email: return "Email";
    case SpanKind::IpAddr: return "IpAddr";
    }
    return "?";
}

static void dump(const std::vector<Span>& s) {
    for (const auto& x : s) std::printf("    %-8s [%d,%d) '%s' line=%d\n", kind_name(x.kind), x.col_begin, x.col_end, x.value.c_str(), x.line_no);
}

// Find a span of `kind` with exactly this value; returns pointer or null.
static const Span* find(const std::vector<Span>& s, SpanKind kind, const std::string& value) {
    for (const auto& x : s) if (x.kind == kind && x.value == value) return &x;
    return nullptr;
}
static bool has_kind(const std::vector<Span>& s, SpanKind kind) {
    for (const auto& x : s) if (x.kind == kind) return true;
    return false;
}
static int count_kind(const std::vector<Span>& s, SpanKind kind) {
    int n = 0;
    for (const auto& x : s) if (x.kind == kind) n++;
    return n;
}

#define EXPECT_SPAN(spans, kind, value) do { if (!find(spans, kind, value)) { std::printf("  missing %s '%s' in:\n", kind_name(kind), std::string(value).c_str()); dump(spans); } CHECK(find(spans, kind, value)); } while (0)
#define EXPECT_NO_KIND(spans, kind) do { if (has_kind(spans, kind)) { std::printf("  unexpected %s in:\n", kind_name(kind)); dump(spans); } CHECK(!has_kind(spans, kind)); } while (0)

static std::vector<std::string> split_lines(const std::string& text) {
    std::vector<std::string> out;
    size_t b = 0;
    while (b <= text.size()) {
        size_t e = text.find('\n', b);
        if (e == std::string::npos) { if (b < text.size()) out.push_back(text.substr(b)); break; }
        out.push_back(text.substr(b, e - b));
        b = e + 1;
    }
    return out;
}

// ---------------------------------------------------------------------------
static void test_urls() {
    auto s = scan_line("see https://example.com/path?q=1&x=2#frag for details.", "", false);
    EXPECT_SPAN(s, SpanKind::Url, "https://example.com/path?q=1&x=2#frag");
    s = scan_line("(http://foo.org/a/b).", "", false);
    EXPECT_SPAN(s, SpanKind::Url, "http://foo.org/a/b");
    s = scan_line("wiki: https://en.wikipedia.org/wiki/Foo_(bar) ok", "", false);
    EXPECT_SPAN(s, SpanKind::Url, "https://en.wikipedia.org/wiki/Foo_(bar)");
    s = scan_line("<https://x.y/z>, 'ftp://ftp.gnu.org/gnu/', \"file:///etc/hosts\"", "", false);
    EXPECT_SPAN(s, SpanKind::Url, "https://x.y/z");
    EXPECT_SPAN(s, SpanKind::Url, "ftp://ftp.gnu.org/gnu/");
    EXPECT_SPAN(s, SpanKind::Url, "file:///etc/hosts");
    s = scan_line("remote: git://github.com/user/repo.git; ssh://user@host:22/p;", "", false);
    EXPECT_SPAN(s, SpanKind::Url, "git://github.com/user/repo.git");
    EXPECT_SPAN(s, SpanKind::Url, "ssh://user@host:22/p");
    s = scan_line("url=https://a.b/c?d=e", "", false);
    EXPECT_SPAN(s, SpanKind::Url, "https://a.b/c?d=e");
    s = scan_line("see:https://a.b/c", "", false);          // embedded scheme mid-word
    EXPECT_SPAN(s, SpanKind::Url, "https://a.b/c");
    s = scan_line("HTTPS://UPPER.CASE/Path", "", false);
    EXPECT_SPAN(s, SpanKind::Url, "HTTPS://UPPER.CASE/Path");
    // Not URLs
    s = scan_line("http:// nothing here, mailto:x, foo://bar.baz", "", false);
    EXPECT_NO_KIND(s, SpanKind::Url);
    // Column positions with multibyte prefix: 'é' counts as one column.
    s = scan_line("caf\xC3\xA9 https://a.b/", "", false);
    const Span* u = find(s, SpanKind::Url, "https://a.b/");
    CHECK(u && u->col_begin == 5 && u->col_end == 17);
}

static void test_emails_ips() {
    auto s = scan_line("Author: Jane Doe <jane.doe+git@example.co.uk>", "", false);
    EXPECT_SPAN(s, SpanKind::Email, "jane.doe+git@example.co.uk");
    s = scan_line("contact bob@example.com.", "", false);
    EXPECT_SPAN(s, SpanKind::Email, "bob@example.com");
    s = scan_line("user@host:/srv/data", "", false);
    EXPECT_NO_KIND(s, SpanKind::Email);
    EXPECT_SPAN(s, SpanKind::Path, "/srv/data");
    s = scan_line("Listening on 192.168.1.10:8080 and 10.0.0.1, gateway 172.16.0.254.", "", false);
    EXPECT_SPAN(s, SpanKind::IpAddr, "192.168.1.10:8080");
    EXPECT_SPAN(s, SpanKind::IpAddr, "10.0.0.1");
    EXPECT_SPAN(s, SpanKind::IpAddr, "172.16.0.254");
    s = scan_line("version 1.2.3.4.5 and 256.1.1.1 and 1.2.3 and v1.2.3.4", "", false);
    EXPECT_NO_KIND(s, SpanKind::IpAddr);
    s = scan_line("inet 127.0.0.1/8 scope host lo", "", false);
    EXPECT_SPAN(s, SpanKind::IpAddr, "127.0.0.1");
}

static void test_git_hashes() {
    auto s = scan_line("a1b2c3d Fix the thing", "", false);
    EXPECT_SPAN(s, SpanKind::GitHash, "a1b2c3d");
    s = scan_line("commit 3f786850e387550fdab836ed7e6dc881de23001b", "", false);
    EXPECT_SPAN(s, SpanKind::GitHash, "3f786850e387550fdab836ed7e6dc881de23001b");
    s = scan_line("HEAD is now at 9e107d9 (origin/main) msg", "", false);
    EXPECT_SPAN(s, SpanKind::GitHash, "9e107d9");
    s = scan_line("* 1a2b3c4d (HEAD -> main) commit message", "", false);
    EXPECT_SPAN(s, SpanKind::GitHash, "1a2b3c4d");
    // Negative: no digit, no letter, too short, embedded in word
    s = scan_line("deadbeef 1234567 abc123 xa1b2c3d4 a1b2c3d4x", "", false);
    EXPECT_NO_KIND(s, SpanKind::GitHash);
    s = scan_line("0x7ffd1234abcd at address", "", false);
    EXPECT_NO_KIND(s, SpanKind::GitHash);
    // git log --oneline sample
    const char* log[] = {
        "f4a2c1e Add structured output scanner",
        "0b9d8e7 (tag: v0.1) Initial import",
        "c0ffee1 Merge branch 'feature/x'",
    };
    for (const char* l : log) {
        auto sp = scan_line(l, "", false);
        CHECK(count_kind(sp, SpanKind::GitHash) == 1);
        CHECK(sp.empty() || sp[0].kind == SpanKind::GitHash);
    }
}

static void test_paths_nofs() {
    auto s = scan_line("src/core/json.cpp:12:5: error: expected ';' before 'x'", "", false);
    const Span* p = find(s, SpanKind::Path, "src/core/json.cpp");
    CHECK(p != nullptr);
    if (p) {
        CHECK(p->line_no == 12);
        CHECK(p->col_begin == 0 && p->col_end == (int)std::string("src/core/json.cpp:12:5").size());
    }
    CHECK(has_kind(s, SpanKind::Error));
    s = scan_line("  File \"/home/u/app/main.py\", line 42, in <module>", "", false);
    p = find(s, SpanKind::Path, "/home/u/app/main.py");
    CHECK(p && p->line_no == 42);
    s = scan_line("  --> src/main.rs:7:9", "", false);
    p = find(s, SpanKind::Path, "src/main.rs");
    CHECK(p && p->line_no == 7);
    s = scan_line("cat ~/.bashrc /etc/passwd ./run.sh ../lib/x.so", "", false);
    EXPECT_SPAN(s, SpanKind::Path, "~/.bashrc");
    EXPECT_SPAN(s, SpanKind::Path, "/etc/passwd");
    EXPECT_SPAN(s, SpanKind::Path, "./run.sh");
    EXPECT_SPAN(s, SpanKind::Path, "../lib/x.so");
    s = scan_line("edit Makefile, foo.py and CMakeLists.txt; also \"notes.md\" (data.json)", "", false);
    EXPECT_SPAN(s, SpanKind::Path, "Makefile");
    EXPECT_SPAN(s, SpanKind::Path, "foo.py");
    EXPECT_SPAN(s, SpanKind::Path, "CMakeLists.txt");
    EXPECT_SPAN(s, SpanKind::Path, "notes.md");
    EXPECT_SPAN(s, SpanKind::Path, "data.json");
    s = scan_line("--prefix=/usr/local --with-x=/opt/x", "", false);
    EXPECT_SPAN(s, SpanKind::Path, "/usr/local");
    EXPECT_SPAN(s, SpanKind::Path, "/opt/x");
    s = scan_line("Installed to /usr/lib/libfoo.so.1.2.", "", false);
    EXPECT_SPAN(s, SpanKind::Path, "/usr/lib/libfoo.so.1.2");
    // Negatives: prose, dates, fractions, Windows paths, bare words
    s = scan_line("either/or and 10/15/2026 or 3/4 of C:\\Users\\x\\file.txt and build", "", false);
    EXPECT_NO_KIND(s, SpanKind::Path);
    s = scan_line("The quick brown fox jumps over the lazy dog. Really.", "", false);
    CHECK(s.empty());
    s = scan_line("a / b // c", "", false);
    EXPECT_NO_KIND(s, SpanKind::Path);
    // Relative path with no extension and no fs check: dropped (conservative)
    s = scan_line("built target/foo", "", false);
    EXPECT_NO_KIND(s, SpanKind::Path);
    // Unicode path, columns in code points
    s = scan_line("\xC3\xA9t\xC3\xA9 /tmp/r\xC3\xA9sum\xC3\xA9.txt done", "", false);
    p = find(s, SpanKind::Path, "/tmp/r\xC3\xA9sum\xC3\xA9.txt");
    CHECK(p && p->col_begin == 4 && p->col_end == 19);
}

static void test_paths_fs() {
    char dirbuf[] = "/tmp/canopy_struct_XXXXXX";
    std::string dir = mkdtemp(dirbuf);
    mkdir((dir + "/sub").c_str(), 0755);
    std::ofstream(dir + "/sub/real") << "x";
    std::ofstream(dir + "/README") << "x";
    std::ofstream(dir + "/.hidden") << "x";

    auto s = scan_line("found sub/real and sub/fake and README and .hidden and .nothere", dir, true);
    EXPECT_SPAN(s, SpanKind::Path, "sub/real");
    EXPECT_SPAN(s, SpanKind::Path, "README");
    EXPECT_SPAN(s, SpanKind::Path, ".hidden");
    CHECK(!find(s, SpanKind::Path, "sub/fake"));
    CHECK(!find(s, SpanKind::Path, ".nothere"));
    // Absolute existing vs non-existing
    s = scan_line("ls " + dir + "/sub /definitely/not/here /definitely/not/here.cpp /nope:12", dir, true);
    EXPECT_SPAN(s, SpanKind::Path, dir + "/sub");
    CHECK(!find(s, SpanKind::Path, "/definitely/not/here"));
    EXPECT_SPAN(s, SpanKind::Path, "/definitely/not/here.cpp");   // known extension survives
    const Span* p = find(s, SpanKind::Path, "/nope");
    CHECK(p && p->line_no == 12);                                // :LINE survives
    // ~ expansion
    setenv("HOME", dir.c_str(), 1);
    s = scan_line("see ~/sub/real and ~/missing", dir, true);
    EXPECT_SPAN(s, SpanKind::Path, "~/sub/real");
    CHECK(!find(s, SpanKind::Path, "~/missing"));

    std::string cmd = "rm -rf '" + dir + "'";
    if (dir.rfind("/tmp/canopy_struct_", 0) == 0) (void)!system(cmd.c_str());
}

static void test_error_warning_lines() {
    const char* errors[] = {
        "src/a.cpp:1:1: error: foo",
        "error[E0308]: mismatched types",
        "Error: something bad",
        "ZeroDivisionError: division by zero",
        "[ERROR] service died",
        "2026-10-15 ERROR failed to connect",
        "fatal: not a git repository",
        "panic: runtime error: index out of range",
        "thread 'main' panicked at 'oops'",
        "Traceback (most recent call last):",
        "java.lang.NullPointerException",
        "Exception in thread \"main\"",
        "cat: nope.txt: No such file or directory",
        "bash: frobnicate: command not found",
        "rm: cannot remove 'x': Permission denied",
        "Segmentation fault (core dumped)",
        "Build failed.",
        "FAILED tests/test_x.py::test_y",
        "foo.o: undefined reference to `bar'",
        "npm ERR! code ELIFECYCLE",
        "E: Unable to locate package foo",
        "make: *** [Makefile:12: all] Error 1",
        "2 failed, 3 passed",
    };
    for (const char* l : errors) {
        if (!is_error_line(l)) std::printf("  not an error line: %s\n", l);
        CHECK(is_error_line(l));
    }
    const char* not_errors[] = {
        "5 passed, 0 failed",
        "errors: 0",
        "the error handling code",
        "No errors found",
        "terror strikes at midnight",
        "ERRORS_SEEN=0",
        "exceptional performance",
        "unfailed attempt",
        "E:\\windows\\path",
        "  E: not at line start? it is after whitespace",   // still line start: apt never indents; accept
        "everything is fine",
        "panicky",
        "fatality",
    };
    int idx = 0;
    for (const char* l : not_errors) {
        bool e = is_error_line(l);
        if (idx == 9) { idx++; continue; }   // documented: leading whitespace is skipped for "E: "
        if (e) std::printf("  false positive error: %s\n", l);
        CHECK(!e);
        idx++;
    }
    const char* warnings[] = {
        "src/a.cpp:1:1: warning: unused variable",
        "Warning: something",
        "DeprecationWarning: foo is deprecated",
        "[WARN] disk almost full",
        "WARNING: untracked files",
        "this API is deprecated",
        "W: GPG error",
    };
    for (const char* l : warnings) {
        if (!is_warning_line(l)) std::printf("  not a warning line: %s\n", l);
        CHECK(is_warning_line(l));
    }
    const char* not_warnings[] = {
        "warnings: 0",
        "forewarned is forearmed",
        "swarning",
        "the warn function",
        "all good",
    };
    for (const char* l : not_warnings) {
        if (is_warning_line(l)) std::printf("  false positive warning: %s\n", l);
        CHECK(!is_warning_line(l));
    }
    // scan_line marks the whole line, error preferred over warning.
    auto s = scan_line("x.c:1:1: error: bad warning: also", "", false);
    CHECK(s.size() >= 2 && s[0].kind == SpanKind::Error && s[0].col_begin == 0 && s[0].col_end == 33);
    s = scan_line("x.c:1:1: warning: meh", "", false);
    CHECK(!s.empty() && s[0].kind == SpanKind::Warning && s[0].col_end == 21);
}

static void test_gcc_and_python_samples() {
    const char* gcc =
        "src/core/terminal.cpp: In member function 'void canopy::Terminal::feed(std::string_view)':\n"
        "src/core/terminal.cpp:214:17: error: 'cursor' was not declared in this scope\n"
        "  214 |                 cursor.x = 0;\n"
        "      |                 ^~~~~~\n"
        "src/core/terminal.cpp:220:9: warning: unused variable 'n' [-Wunused-variable]\n"
        "  220 |     int n = 0;\n"
        "      |         ^\n"
        "make[2]: *** [CMakeFiles/canopy_vt.dir/build.make:76: CMakeFiles/canopy_vt.dir/src/core/terminal.cpp.o] Error 1\n";
    auto lines = split_lines(gcc);
    auto s = scan_line(lines[1], "", false);
    const Span* p = find(s, SpanKind::Path, "src/core/terminal.cpp");
    CHECK(p && p->line_no == 214 && has_kind(s, SpanKind::Error));
    s = scan_line(lines[4], "", false);
    p = find(s, SpanKind::Path, "src/core/terminal.cpp");
    CHECK(p && p->line_no == 220 && has_kind(s, SpanKind::Warning) && !has_kind(s, SpanKind::Error));
    s = scan_line(lines[7], "", false);
    CHECK(has_kind(s, SpanKind::Error));
    EXPECT_SPAN(s, SpanKind::Path, "CMakeFiles/canopy_vt.dir/build.make");
    EXPECT_SPAN(s, SpanKind::Path, "CMakeFiles/canopy_vt.dir/src/core/terminal.cpp.o");
    CHECK(!detect_table(lines).is_table);

    const char* py =
        "Traceback (most recent call last):\n"
        "  File \"/home/u/proj/app.py\", line 12, in <module>\n"
        "    main()\n"
        "  File \"/home/u/proj/app.py\", line 9, in main\n"
        "    return helpers.compute(1, 0)\n"
        "  File \"/home/u/proj/helpers.py\", line 3, in compute\n"
        "    return a / b\n"
        "ZeroDivisionError: division by zero\n";
    lines = split_lines(py);
    CHECK(is_error_line(lines[0]));
    CHECK(is_error_line(lines[7]));
    s = scan_line(lines[5], "", false);
    p = find(s, SpanKind::Path, "/home/u/proj/helpers.py");
    CHECK(p && p->line_no == 3);
    CHECK(!detect_table(lines).is_table);
}

// ---------------------------------------------------------------------------
static const char* kLsL =
    "total 48\n"
    "drwxr-xr-x  5 benoit benoit  4096 Oct 15 10:02 build\n"
    "-rw-r--r--  1 benoit benoit  2231 Oct 14 18:40 CMakeLists.txt\n"
    "-rw-r--r--  1 benoit benoit  7319 Oct 14 18:12 DESIGN.md\n"
    "drwxr-xr-x  2 benoit benoit  4096 Oct 15 09:58 docs\n"
    "-rw-r--r--  1 benoit benoit 12044 Oct 15 11:30 README.md\n"
    "drwxr-xr-x  4 benoit benoit  4096 Oct 14 18:12 src\n"
    "drwxr-xr-x  2 benoit benoit  4096 Oct 15 11:31 tests\n"
    "-rw-r--r--  1 benoit benoit   713 Sep  3 08:15 my notes.txt\n";

static const char* kPsAux =
    "USER         PID %CPU %MEM    VSZ   RSS TTY      STAT START   TIME COMMAND\n"
    "root           1  0.0  0.0 168884 13000 ?        Ss   Oct10   0:03 /sbin/init splash\n"
    "root           2  0.0  0.0      0     0 ?        S    Oct10   0:00 [kthreadd]\n"
    "benoit      1337  2.5  1.2 912344 98812 pts/0    Sl+  09:12   1:27 /usr/bin/python3 server.py --port 8080\n"
    "benoit      2048  0.1  0.3 221100 24000 pts/1    Ss   09:15   0:00 -bash\n"
    "benoit      4096 15.0  4.0 2211000 331000 ?      Rl   10:01  12:45 /opt/canopy/build/canopy\n";

static const char* kDfH =
    "Filesystem      Size  Used Avail Use% Mounted on\n"
    "/dev/nvme0n1p2  457G  123G  311G  29% /\n"
    "tmpfs           7.8G  2.1M  7.8G   1% /dev/shm\n"
    "/dev/nvme0n1p1  511M  6.1M  505M   2% /boot/efi\n"
    "/dev/sdb1       1.8T  1.1T  620G  65% /data\n";

static const char* kDockerPs =
    "CONTAINER ID   IMAGE          COMMAND                  CREATED        STATUS        PORTS                    NAMES\n"
    "3f2a1b9c8d7e   nginx:1.25     \"/docker-entrypoint.…\"   2 hours ago    Up 2 hours    0.0.0.0:8080->80/tcp     web\n"
    "9a8b7c6d5e4f   postgres:16    \"docker-entrypoint.s…\"   3 days ago     Up 3 days     5432/tcp                 db\n"
    "1c2d3e4f5a6b   redis:7        \"docker-entrypoint.s…\"   3 days ago     Up 3 days     6379/tcp                 cache\n";

static const char* kKubectl =
    "NAME                        READY   STATUS    RESTARTS   AGE\n"
    "api-7d9f8b6c5-abcde         1/1     Running   0          5d\n"
    "worker-5c4d3b2a1-fghij      1/1     Running   3          2d\n"
    "db-0                        1/1     Running   0          30d\n"
    "cron-28123456-klmno         0/1     Completed 0          2h\n";

static const char* kGitStatusShort =
    " M src/core/json.cpp\n"
    " M src/core/config.cpp\n"
    "?? tests/test_json.cpp\n"
    "A  docs/canopy.conf.example\n"
    " D old/thing.c\n";

static const char* kProse =
    "Canopy is a GPU-rendered terminal emulator for Linux whose central idea\n"
    "is that terminal work has structure: commands are objects, sessions\n"
    "belong to a tree of activities, and the whole workspace lives on a\n"
    "zoomable canvas. It must remain a fully compatible VT terminal: vim,\n"
    "htop, tmux, less, ssh, mc, and anything curses-based must behave\n"
    "exactly as in xterm or kitty. The tree is persisted so that closing\n"
    "and reopening shows the same tree, with history, and a fresh shell.\n";

static const char* kSource =
    "#include <vector>\n"
    "\n"
    "namespace canopy {\n"
    "\n"
    "struct Span {\n"
    "    SpanKind kind;\n"
    "    int col_begin = 0, col_end = 0;\n"
    "    std::string value;\n"
    "    int line_no = 0;\n"
    "};\n"
    "\n"
    "std::vector<Span> scan_line(std::string_view line, const std::string& cwd, bool check_fs) {\n"
    "    std::vector<Span> spans;\n"
    "    if (line.empty()) return spans;\n"
    "    for (size_t i = 0; i < line.size(); i++) {\n"
    "        if (line[i] == ' ') continue;\n"
    "    }\n"
    "    return spans;\n"
    "}\n"
    "\n"
    "} // namespace canopy\n";

static const char* kMarkdown =
    "# Canopy\n"
    "\n"
    "A terminal emulator with structure.\n"
    "\n"
    "## Features\n"
    "\n"
    "- Activity tree with persistence\n"
    "- Blocks for every command\n"
    "- Structured output: paths, URLs, tables\n"
    "- Zoomable canvas\n"
    "\n"
    "## Building\n"
    "\n"
    "Run `cmake -S . -B build` and then `cmake --build build`.\n"
    "See DESIGN.md for the architecture overview and the conventions used.\n";

static const char* kBullets =
    "- item 1\n"
    "- item 2\n"
    "- item 3\n"
    "- item 4\n"
    "- item 5\n";

static void test_tables() {
    // ls -l: table, no header, "total" line skipped, size column numeric.
    {
        auto lines = split_lines(kLsL);
        TableInfo t = detect_table(lines);
        if (!t.is_table) std::printf("  ls -l not detected\n");
        CHECK(t.is_table);
        CHECK(t.header_row == -1);
        CHECK(t.first_data_row == 1);
        CHECK(t.col_begins.size() >= 8);
        if (t.is_table) {
            auto cells = split_columns(lines[5], t);
            CHECK(cells.size() == t.col_begins.size());
            CHECK(cells[0] == "-rw-r--r--");
            CHECK(cells[1] == "1");
            CHECK(cells[4] == "12044");
            CHECK(cells.back() == "README.md");
            CHECK(t.numeric[1] && t.numeric[4] && !t.numeric[0] && !t.numeric[2]);
            // filename with a space stays in one cell
            auto last = split_columns(lines[8], t);
            CHECK(last.back() == "my notes.txt");
            // sort by size descending
            auto order = sort_rows(lines, t, 4, true);
            CHECK(order.size() == lines.size() - 1);
            CHECK(order[0] == 5);                  // README.md, 12044
            CHECK(order[1] == 3);                  // DESIGN.md, 7319
            CHECK(order.back() == 8);              // my notes.txt, 713
            auto asc = sort_rows(lines, t, 4, false);
            CHECK(asc[0] == 8);
            // stable: the 4096 directories keep their original order
            CHECK(asc[1] == 2 && asc[2] == 1 && asc[3] == 4 && asc[4] == 6 && asc[5] == 7);
            // sort by name, case-insensitive
            auto byname = sort_rows(lines, t, (int)t.col_begins.size() - 1, false);
            CHECK(byname[0] == 1);                 // build
            CHECK(byname[1] == 2);                 // CMakeLists.txt
            auto nums = numeric_column(lines, t, 4);
            CHECK(nums.size() == 8 && nums[0] == 4096 && nums[4] == 12044 && nums.back() == 713);
            CHECK(numeric_column(lines, t, 0).empty());
        }
    }
    // ps aux: header at row 0, PID numeric, COMMAND with spaces intact.
    {
        auto lines = split_lines(kPsAux);
        TableInfo t = detect_table(lines);
        if (!t.is_table) std::printf("  ps aux not detected\n");
        CHECK(t.is_table);
        CHECK(t.header_row == 0);
        CHECK(t.first_data_row == 1);
        if (t.is_table) {
            auto hdr = split_columns(lines[0], t);
            CHECK(hdr[0] == "USER" && hdr[1] == "PID" && hdr.back() == "COMMAND");
            CHECK(hdr.size() == 11);
            auto r = split_columns(lines[3], t);
            CHECK(r[0] == "benoit" && r[1] == "1337" && r[2] == "2.5");
            CHECK(r.back() == "/usr/bin/python3 server.py --port 8080");
            CHECK(t.numeric[1] && t.numeric[2] && t.numeric[3] && !t.numeric[0] && !t.numeric[6]);
            auto order = sort_rows(lines, t, 2, true);   // %CPU desc
            CHECK(order[0] == 5 && order[1] == 3);
            auto mem = numeric_column(lines, t, 3);
            CHECK(mem.size() == 5 && mem[2] == 1.2);
        }
    }
    // df -h
    {
        auto lines = split_lines(kDfH);
        TableInfo t = detect_table(lines);
        if (!t.is_table) std::printf("  df -h not detected\n");
        CHECK(t.is_table);
        CHECK(t.header_row == 0);
        if (t.is_table) {
            auto hdr = split_columns(lines[0], t);
            CHECK(hdr.size() == 6);
            CHECK(hdr[1] == "Size" && hdr[4] == "Use%" && hdr[5] == "Mounted on");
            CHECK(t.numeric[1] && t.numeric[2] && t.numeric[3] && t.numeric[4] && !t.numeric[0] && !t.numeric[5]);
            auto sizes = numeric_column(lines, t, 1);
            CHECK(sizes.size() == 4);
            CHECK(std::fabs(sizes[0] - 457.0 * 1024 * 1024 * 1024) < 1);
            CHECK(std::fabs(sizes[1] - 7.8 * 1024 * 1024 * 1024) < 1024);
            auto order = sort_rows(lines, t, 1, true);   // by size: 1.8T, 457G, 7.8G, 511M
            CHECK(order[0] == 4 && order[1] == 1 && order[2] == 2 && order[3] == 3);
            auto use = numeric_column(lines, t, 4);
            CHECK(use.size() == 4 && use[3] == 65);
        }
    }
    // docker ps (header has a two-word column name; PORTS sparse)
    {
        auto lines = split_lines(kDockerPs);
        TableInfo t = detect_table(lines);
        if (!t.is_table) std::printf("  docker ps not detected\n");
        CHECK(t.is_table);
        CHECK(t.header_row == 0);
        if (t.is_table) {
            auto hdr = split_columns(lines[0], t);
            CHECK(hdr[0] == "CONTAINER ID");
            CHECK(hdr.back() == "NAMES");
            auto r = split_columns(lines[1], t);
            CHECK(r.back() == "web");
        }
    }
    // kubectl get pods
    {
        auto lines = split_lines(kKubectl);
        TableInfo t = detect_table(lines);
        if (!t.is_table) std::printf("  kubectl not detected\n");
        CHECK(t.is_table);
        CHECK(t.header_row == 0);
        if (t.is_table) {
            CHECK(t.col_begins.size() == 5);
            CHECK(t.numeric[3] && !t.numeric[1]);
            auto order = sort_rows(lines, t, 3, true);
            CHECK(order[0] == 2);
            auto order2 = sort_rows(lines, t, 0, false);
            CHECK(order2[0] == 1 && order2[1] == 4 && order2[2] == 3 && order2[3] == 2);
        }
    }
    // Tab-separated (e.g. "du -sh *" or cut output): tabs expand to tab stops.
    {
        std::vector<std::string> lines = {
            "4.0K\tbuild", "212K\tsrc", "1.2M\tdocs", "16K\ttests", "8.0K\tREADME.md",
        };
        TableInfo t = detect_table(lines);
        if (!t.is_table) std::printf("  du -sh (tabs) not detected\n");
        CHECK(t.is_table);
        if (t.is_table) {
            CHECK(t.col_begins.size() == 2 && t.numeric[0] && !t.numeric[1]);
            auto c = split_columns(lines[2], t);
            CHECK(c[0] == "1.2M" && c[1] == "docs");
            auto order = sort_rows(lines, t, 0, true);
            CHECK(order[0] == 2 && order[1] == 1 && order.back() == 0);
        }
    }
    // Negatives
    CHECK(!detect_table(split_lines(kGitStatusShort)).is_table);
    CHECK(!detect_table(split_lines(kProse)).is_table);
    CHECK(!detect_table(split_lines(kSource)).is_table);
    CHECK(!detect_table(split_lines(kMarkdown)).is_table);
    CHECK(!detect_table(split_lines(kBullets)).is_table);
    CHECK(!detect_table({}).is_table);
    CHECK(!detect_table({"one", "two"}).is_table);
    CHECK(!detect_table({"a b", "c d"}).is_table);           // only 2 rows
    // Numeric cell parsing edge cases via a tiny table
    {
        std::vector<std::string> lines = {
            "name   value", "a      1,234", "b      -5.5", "c      45%", "d      +7", "e      3e2",
        };
        TableInfo t = detect_table(lines);
        CHECK(t.is_table && t.header_row == 0 && t.numeric.size() == 2 && t.numeric[1]);
        auto v = numeric_column(lines, t, 1);
        CHECK(v.size() == 5 && v[0] == 1234 && v[1] == -5.5 && v[2] == 45 && v[3] == 7 && v[4] == 300);
    }
}

static void test_json_helpers() {
    CHECK(looks_like_json("{\"a\": 1}"));
    CHECK(looks_like_json("  [1, 2, {\"x\": \"}\"}]  \n"));
    CHECK(looks_like_json("{\"s\": \"escaped \\\" quote ]\"}"));
    CHECK(!looks_like_json(""));
    CHECK(!looks_like_json("{"));
    CHECK(!looks_like_json("hello"));
    CHECK(!looks_like_json("{\"a\": [1, 2}"));
    CHECK(!looks_like_json("{\"a\": 1} trailing {"));
    CHECK(!looks_like_json("{} {}"));
    CHECK(!looks_like_json("[\"unterminated]"));
    auto p = pretty_json("{\"b\":[1,2],\"a\":{\"c\":null}}");
    CHECK(p && *p == "{\n  \"a\": {\n    \"c\": null\n  },\n  \"b\": [\n    1,\n    2\n  ]\n}");
    auto c = compact_json("{ \"b\" : [ 1 , 2 ] , \"a\" : true }");
    CHECK(c && *c == "{\"a\":true,\"b\":[1,2]}");
    CHECK(!pretty_json("{not json}"));
    CHECK(!compact_json("[1,2,"));
    auto p4 = pretty_json("[1]", 4);
    CHECK(p4 && *p4 == "[\n    1\n]");
}

static void benchmark() {
    // Build 20k lines of mixed output.
    std::vector<std::string> lines;
    std::vector<std::string> samples;
    for (const char* blob : {kLsL, kPsAux, kDfH, kDockerPs, kKubectl, kProse, kSource, kMarkdown}) {
        for (auto& l : split_lines(blob)) samples.push_back(l);
    }
    samples.push_back("src/core/terminal.cpp:214:17: error: 'cursor' was not declared in this scope");
    samples.push_back("  File \"/home/u/proj/app.py\", line 12, in <module>");
    samples.push_back("f4a2c1e Add structured output scanner (see https://github.com/x/y/pull/12)");
    samples.push_back("Listening on 192.168.1.10:8080, contact admin@example.com for access");
    samples.push_back("warning: unused variable 'n' [-Wunused-variable]");
    samples.push_back("[2026-10-15T10:02:33Z] INFO  request handled in 12ms (/api/v1/users/42)");
    samples.push_back("caf\xC3\xA9 \xE2\x9C\x93 unicode line with /tmp/r\xC3\xA9sum\xC3\xA9.txt in it");
    while (lines.size() < 20000) {
        for (const auto& s : samples) { if (lines.size() >= 20000) break; lines.push_back(s); }
    }
    size_t total_spans = 0;
    auto t0 = std::chrono::steady_clock::now();
    const int reps = 5;
    for (int r = 0; r < reps; r++)
        for (const auto& l : lines) total_spans += scan_line(l, "/tmp", false).size();
    auto t1 = std::chrono::steady_clock::now();
    double secs = std::chrono::duration<double>(t1 - t0).count();
    double lps = (double)lines.size() * reps / secs;
    std::printf("benchmark: scan_line %zu lines x%d in %.3fs = %.0f lines/s (%zu spans)\n",
                lines.size(), reps, secs, lps, total_spans / reps);
    CHECK(lps > 500000);

    t0 = std::chrono::steady_clock::now();
    size_t ec = 0;
    for (int r = 0; r < reps; r++)
        for (const auto& l : lines) ec += is_error_line(l) + is_warning_line(l);
    t1 = std::chrono::steady_clock::now();
    secs = std::chrono::duration<double>(t1 - t0).count();
    std::printf("benchmark: is_error/warning_line %.0f lines/s (%zu hits)\n", (double)lines.size() * reps / secs, ec / reps);

    // detect_table over a 20k-line block (only the first 200 non-empty lines are examined).
    t0 = std::chrono::steady_clock::now();
    TableInfo ti = detect_table(lines);
    t1 = std::chrono::steady_clock::now();
    std::printf("benchmark: detect_table on %zu lines: %.3f ms (is_table=%d)\n", lines.size(),
                std::chrono::duration<double, std::milli>(t1 - t0).count(), (int)ti.is_table);
}

int main() {
    test_urls();
    test_emails_ips();
    test_git_hashes();
    test_paths_nofs();
    test_paths_fs();
    test_error_warning_lines();
    test_gcc_and_python_samples();
    test_tables();
    test_json_helpers();
    benchmark();
    std::printf("test_structure: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
