#include "pane.h"
#include "canvas.h"
#include <SDL_mouse.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <sstream>
#include <sys/stat.h>

namespace canopy::ui {

using gfx::Renderer;
using gfx::Font;

static const char* SPINNER[] = {"◐", "◓", "◑", "◒"};

Pane::Pane(App& app, uint64_t id) : activity_id(id), app_(app) {
    cols = app.config().default_cols;
    rows = app.config().default_rows;
}

Pane::~Pane() { stop_session(); }

// ---------------------------------------------------------------- geometry
float Pane::cell_w() const { return app_.cell_w(); }
float Pane::cell_h() const { return app_.cell_h(); }
bool Pane::minimized() const {
    const Activity* a = app_.tree().get(activity_id);
    return a && a->minimized;
}
void Pane::set_minimized(bool m) {
    if (Activity* a = app_.tree().get(activity_id)) {
        if (a->minimized == m) return;
        a->minimized = m;
        app_.canvas().layout();
        app_.save_state();
        app_.request_redraw();
    }
}
Rect Pane::rect() const {
    float w = GUTTER_W + cols * cell_w() + 2 * PAD;
    float h = minimized() ? (TITLE_H + cell_h() + 2 * PAD) : (TITLE_H + rows * cell_h() + 2 * PAD);
    return {x, y, w, h};
}
Rect Pane::title_rect() const { return {x, y, rect().w, TITLE_H}; }
Rect Pane::artifact_chip_rect() const {
    if (artifacts_.empty()) return {0, 0, 0, 0};
    const AgyArtifact* art = active_artifact();
    std::string stem = art ? art->name : "artifacts";
    if (stem.size() > 3 && stem.compare(stem.size() - 3, 3, ".md") == 0) stem.resize(stem.size() - 3);
    stem = ellipsize(stem, 15);
    if (artifacts_.size() > 1) stem += " (" + std::to_string(artifacts_.size()) + ")";
    float w = std::max(64.f, std::min(176.f, (float)(utf8_length(stem) + 3) * cell_w() + 10.f));
    Rect mb = min_rect();
    return {mb.x - w - 6.f, mb.y, w, 20.f};
}
Rect Pane::min_rect() const { Rect t = title_rect(); return {t.x + t.w - 52.f, t.y + (TITLE_H - 20.f) / 2.f, 20.f, 20.f}; }
Rect Pane::close_rect() const { Rect t = title_rect(); return {t.x + t.w - 28.f, t.y + (TITLE_H - 20.f) / 2.f, 20.f, 20.f}; }
Rect Pane::term_rect() const { return {x + GUTTER_W + PAD, y + TITLE_H + PAD, cols * cell_w(), rows * cell_h()}; }
Rect Pane::gutter_rect() const { return {x + 2, y + TITLE_H + PAD, GUTTER_W + PAD - 2, rows * cell_h()}; }
Rect Pane::resize_handle() const { Rect r = rect(); return {r.x + r.w - HANDLE, r.y + r.h - HANDLE, HANDLE, HANDLE}; }
Rect Pane::drawer_rect() const {
    Rect R = rect();
    float w = std::max(380.f, std::min(540.f, cols * cell_w() * 0.72f));
    float h = std::max(R.h, TITLE_H + 18 * cell_h());
    return {R.x + R.w + 12.f, R.y, w, h};
}
Rect Pane::drawer_close_rect() const {
    Rect d = drawer_rect();
    return {d.x + d.w - 28.f, d.y + (TITLE_H - 20.f) / 2.f, 20.f, 20.f};
}
Rect Pane::drawer_next_rect() const {
    if (artifacts_.size() <= 1) return {0, 0, 0, 0};
    Rect d = drawer_rect();
    return {d.x + d.w - 54.f, d.y + (TITLE_H - 20.f) / 2.f, 20.f, 20.f};
}
Rect Pane::drawer_prev_rect() const {
    if (artifacts_.size() <= 1) return {0, 0, 0, 0};
    Rect d = drawer_rect();
    return {d.x + d.w - 108.f, d.y + (TITLE_H - 20.f) / 2.f, 20.f, 20.f};
}
Rect Pane::drawer_proceed_rect() const {
    Rect d = drawer_rect();
    return {d.x + 10.f, d.y + TITLE_H + 5.f, 94.f, 22.f};
}
Rect Pane::drawer_feedback_rect() const {
    Rect d = drawer_rect();
    return {d.x + 110.f, d.y + TITLE_H + 5.f, 102.f, 22.f};
}
Rect Pane::drawer_edit_rect() const {
    Rect d = drawer_rect();
    return {d.x + 218.f, d.y + TITLE_H + 5.f, 88.f, 22.f};
}
Rect Pane::drawer_body_rect() const {
    Rect d = drawer_rect();
    return {d.x + 10.f, d.y + TITLE_H + 32.f, d.w - 20.f, d.h - TITLE_H - 40.f};
}

const AgyArtifact* Pane::active_artifact() const {
    if (artifacts_.empty() || artifact_idx_ < 0 || artifact_idx_ >= (int)artifacts_.size()) return nullptr;
    return &artifacts_[artifact_idx_];
}

void Pane::set_drawer_open(bool open) {
    if (open && artifacts_.empty()) update_artifacts(app_.now());
    if (open && artifacts_.empty()) {
        app_.toast("No AGY artifacts found for this workspace");
        return;
    }
    if (drawer_open_ == open) return;
    drawer_open_ = open;
    if (open) {
        artifact_unread_ = false;
        ensure_artifact_loaded();
    }
    app_.canvas().layout();
    app_.request_redraw();
}

void Pane::toggle_drawer() {
    set_drawer_open(!drawer_open());
}

void Pane::select_artifact(int idx) {
    if (artifacts_.empty()) return;
    idx = std::max(0, std::min((int)artifacts_.size() - 1, idx));
    if (artifact_idx_ != idx) {
        artifact_idx_ = idx;
        drawer_scroll_ = 0.f;
        ensure_artifact_loaded();
        app_.request_redraw();
    }
}

void Pane::step_artifact(int delta) {
    if (artifacts_.size() <= 1) return;
    int n = (int)artifacts_.size();
    select_artifact(((artifact_idx_ + delta) % n + n) % n);
}

void Pane::refresh_artifacts() {
    update_artifacts(app_.now());
    if (drawer_open()) ensure_artifact_loaded();
}

void Pane::open_artifact(const std::string& path) {
    if (path.empty()) {
        toggle_drawer();
        return;
    }
    std::string p = path;
    if (!p.empty() && p[0] == '~') p = home_dir() + p.substr(1);
    if (!p.empty() && p[0] != '/') p = cwd() + "/" + p;
    struct stat st;
    if (stat(p.c_str(), &st) == 0 && !S_ISDIR(st.st_mode)) {
        if (std::find(pinned_artifacts_.begin(), pinned_artifacts_.end(), p) == pinned_artifacts_.end()) {
            pinned_artifacts_.push_back(p);
        }
    }
    update_artifacts(app_.now());
    for (size_t i = 0; i < artifacts_.size(); ++i) {
        if (artifacts_[i].path == p || artifacts_[i].name == path || artifacts_[i].name == basename_of(p)) {
            artifact_idx_ = (int)i;
            drawer_scroll_ = 0.f;
            break;
        }
    }
    set_drawer_open(true);
}

bool Pane::cell_at(float wx, float wy, int* row, int* col) const {
    Rect t = term_rect();
    if (wx < t.x - GUTTER_W - PAD || wy < t.y || wy >= t.y + t.h || wx >= t.x + t.w) return false;
    *row = (int)((wy - t.y) / cell_h());
    *col = (int)std::floor((wx - t.x) / cell_w());
    if (*col < 0) *col = 0;
    if (*col >= cols) *col = cols - 1;
    return *row >= 0 && *row < rows;
}

// ---------------------------------------------------------------- session
bool Pane::start_session(std::string* err) {
    if (session && !session->blocks().blocks().empty()) {
        load_restored(export_blocks(60, 200));
    }
    stop_session();
    const Activity* a = app_.tree().get(activity_id);
    Session::Options o;
    o.cols = cols; o.rows = rows;
    o.scrollback = app_.config().scrollback;
    o.shell_integration = app_.config().shell_integration;
    o.integration_dir = app_.shell_dir();
    o.activity_id = activity_id;
    if (a) { o.cwd = a->cwd; o.env_extra = a->env; }
    if (!o.cwd.empty()) { struct stat st; if (stat(o.cwd.c_str(), &st) != 0) o.cwd.clear(); }
    if (!app_.config().shell.empty()) o.argv = {app_.config().shell};
    if (app_.config().login_shell && o.argv.empty()) o.env_extra.push_back("CANOPY_LOGIN=1");
    {
        std::string hist = ActivityTree::history_path(activity_id);
        struct stat st;
        if (stat(hist.c_str(), &st) != 0 || st.st_size == 0) {
            std::vector<std::string> seed;
            if (a && !a->history.empty()) {
                seed = a->history;
            } else {
                for (const Block& b : restored_blocks_) if (!b.command.empty()) seed.push_back(b.command);
                if (seed.empty() && a) {
                    for (const Block& b : a->blocks) if (!b.command.empty()) seed.push_back(b.command);
                }
            }
            ActivityTree::save_history(activity_id, seed);
        }
        o.env_extra.push_back("CANOPY_HISTFILE=" + hist);
        o.env_extra.push_back("HISTFILE=" + hist);
    }
    session = std::make_unique<Session>();
    session->on_wake = [this] { app_.wake(); };
    std::string e;
    if (!session->start(o, &e)) { if (err) *err = e; err_ = e; session.reset(); return false; }
    Terminal& t = session->term();
    {
        const Theme& th = app_.theme();
        auto pack = [](const RGB8& c) { return (uint32_t)c.r << 16 | (uint32_t)c.g << 8 | c.b; };
        t.set_default_colors(pack(th.foreground), pack(th.background), pack(th.cursor));
        for (int i = 0; i < 16; i++) t.set_palette_color(i, pack(th.ansi[i]));
        t.set_cell_pixel_size((int)cell_w(), (int)cell_h());
    }
    auto prev_cwd = t.on_cwd;
    t.on_cwd = [this, prev_cwd](const std::string& s) {
        if (prev_cwd) prev_cwd(s);
        if (Activity* a = app_.tree().get(activity_id)) a->cwd = s;
        last_artifact_scan_ = 0;
    };
    t.on_title = [this](const std::string& s) { last_title = s; app_.request_redraw(); };
    t.on_bell = [this] {
        if (app_.focused_id() != activity_id || !app_.window().focused()) {
            bell_unread_ = true;
            unread = true;
        }
        app_.bell(activity_id);
        app_.request_redraw();
    };
    t.on_notify = [this](const std::string& title, const std::string& body) {
        std::string msg = title.empty() ? body : (body.empty() ? title : title + ": " + body);
        notify_msg_ = msg;
        if (app_.focused_id() != activity_id || !app_.window().focused()) {
            bell_unread_ = true;
            unread = true;
        }
        const Activity* a = app_.tree().get(activity_id);
        app_.toast("🔔 " + (a ? a->name + ": " : std::string()) + ellipsize(msg, 60), 6.f);
        if (!app_.window().focused()) app_.window().request_attention();
        app_.request_redraw();
    };
    t.on_clipboard_set = [this](const std::string& sel, const std::string& b64) {
        std::string data = base64_decode(b64);
        if (sel.find('p') != std::string::npos || sel.find('s') != std::string::npos) app_.set_primary(data);
        else app_.set_clipboard(data);
    };
    session->blocks().on_block_started = [this](const Block& b) {
        if (!b.command.empty()) ActivityTree::append_history(activity_id, b.command);
        output_since_input_ = false;
        agent_notified_ = false;
        agent_state_ = AgentState::None;
    };
    session->blocks().on_block_finished = [this](const Block& b) {
        if (!b.command.empty()) ActivityTree::append_history(activity_id, b.command);
        last_finish = app_.now();
        agent_state_ = AgentState::None;
        agent_notified_ = false;
        app_.on_block_finished(activity_id, b);
    };
    session->blocks().on_request = [this](const std::string& k, const std::string& p) { app_.on_shell_request(activity_id, k, p); };
    exited_at_ = 0;
    vis_dirty_ = true;
    last_gen_ = 0;
    last_history_base_ = 0;
    anchor_abs_ = INT64_MIN;
    follow_ = true;
    return true;
}

void Pane::stop_session() {
    if (session) { session->stop(); session.reset(); }
    views_.clear();
    agent_state_ = AgentState::None;
    vis_dirty_ = true;
}

void Pane::queue_input_at_prompt(const std::string& text) { pending_input_.push_back(text); }

bool Pane::at_prompt() const {
    if (!live()) return false;
    const auto& bl = session->blocks().blocks();
    if (bl.empty()) return false;
    BlockState s = bl.back().state;
    return s == BlockState::Prompt || s == BlockState::Typing;
}

std::string Pane::cwd() const {
    if (session) {
        std::string c = session->cwd();
        if (!c.empty()) return c;
    }
    if (const Activity* a = app_.tree().get(activity_id)) if (!a->cwd.empty()) return a->cwd;
    return home_dir();
}

void Pane::clear_attention() {
    unread = false;
    bell_unread_ = false;
    artifact_unread_ = false;
    notify_msg_.clear();
}

void Pane::write(std::string_view bytes) {
    if (live()) {
        last_input_ = app_.now();
        output_since_input_ = false;
        agent_notified_ = false;
        bell_unread_ = false;
        notify_msg_.clear();
        if (agent_state_ == AgentState::WaitingInput || agent_state_ == AgentState::NeedsApproval) {
            agent_state_ = AgentState::Streaming;
        }
        session->write(bytes);
    }
}

void Pane::resize_cells(int c, int r) {
    c = std::max(20, std::min(500, c)); r = std::max(5, std::min(300, r));
    if (c == cols && r == rows) return;
    cols = c; rows = r;
    if (session) session->resize(cols, rows, (int)(cols * cell_w()), (int)(rows * cell_h()));
    if (Activity* a = app_.tree().get(activity_id)) { a->cols = cols; a->rows = rows; }
    vis_dirty_ = true;
    clamp_scroll();
}

bool Pane::is_agent_session() const {
    if (!session || !session->alive()) return false;
    auto match_name = [](const std::string& s) {
        if (s.empty()) return false;
        if (s.find("✳") != std::string::npos) return true;
        std::string l = to_lower(s);
        static const char* kAgents[] = {"claude", "agy", "antigravity", "codex", "gemini", "aider", "opencode", "goose", "cursor-agent"};
        for (const char* a : kAgents) {
            size_t pos = 0;
            size_t alen = std::strlen(a);
            while ((pos = l.find(a, pos)) != std::string::npos) {
                bool left_ok = (pos == 0) || (!std::isalnum((unsigned char)l[pos - 1]) && l[pos - 1] != '_');
                bool right_ok = (pos + alen >= l.size()) || (!std::isalnum((unsigned char)l[pos + alen]) && l[pos + alen] != '_');
                if (left_ok && right_ok) return true;
                pos += alen;
            }
        }
        return false;
    };
    const auto& bl = session->blocks().blocks();
    if (!bl.empty() && bl.back().state == BlockState::Running && match_name(bl.back().command)) return true;
    if (match_name(last_title)) return true;
    if (match_name(session->pty().foreground_name())) return true;
    return false;
}

void Pane::update_agent_state(double now) {
    if (!session || !session->alive()) { agent_state_ = AgentState::None; return; }
    const Terminal& t = session->term();
    const auto& bl = session->blocks().blocks();
    bool cmd_running = (!bl.empty() && bl.back().state == BlockState::Running) || t.modes().alt_screen;
    if (!cmd_running) {
        agent_state_ = AgentState::None;
        return;
    }
    bool agent = is_agent_session();
    double quiet = now - last_output;
    if (quiet < 1.2) {
        agent_state_ = agent ? AgentState::Streaming : AgentState::None;
        return;
    }
    // Output has been quiet for >= 1.2s while a command/TUI is running.
    // Scan the last few non-empty screen lines for permission/approval prompts or interactive questions.
    bool approval = false;
    bool interactive_prompt = false;
    int scanned = 0;
    for (int y = t.rows() - 1; y >= 0 && scanned < 10; --y) {
        std::string txt = t.row(y).text();
        size_t first = txt.find_first_not_of(" \t\r\n");
        if (first == std::string::npos) continue;
        size_t last = txt.find_last_not_of(" \t\r\n");
        txt = txt.substr(first, last - first + 1);
        std::string lt = to_lower(txt);
        if (lt.find("do you want to") != std::string::npos ||
            lt.find("would you like to") != std::string::npos ||
            lt.find("allow ") != std::string::npos ||
            lt.find("yes, allow") != std::string::npos ||
            lt.find("approve") != std::string::npos ||
            lt.find("proceed?") != std::string::npos ||
            lt.find("[y/n]") != std::string::npos ||
            lt.find("(y/n)") != std::string::npos ||
            lt.find("[y/n/a]") != std::string::npos ||
            (agent && (lt.find("❯ 1. yes") != std::string::npos || lt.find("> 1. yes") != std::string::npos))) {
            approval = true;
            break;
        }
        if (scanned == 0) {
            if (lt.find("password:") != std::string::npos ||
                lt.find("passphrase") != std::string::npos ||
                lt.find("continue?") != std::string::npos ||
                (!lt.empty() && lt.back() == '?' && lt.size() > 6)) {
                interactive_prompt = true;
            }
        }
        ++scanned;
    }
    if (approval) {
        agent_state_ = AgentState::NeedsApproval;
    } else if (agent && (output_since_input_ || last_input_ == 0)) {
        agent_state_ = AgentState::WaitingInput;
    } else if (interactive_prompt && output_since_input_) {
        agent_state_ = AgentState::WaitingInput;
    } else {
        agent_state_ = AgentState::None;
    }

    if ((agent_state_ == AgentState::NeedsApproval || agent_state_ == AgentState::WaitingInput) && !agent_notified_) {
        agent_notified_ = true;
        bool unfocused = (app_.focused_id() != activity_id) || !app_.window().focused();
        if (unfocused) {
            unread = true;
            const Activity* a = app_.tree().get(activity_id);
            std::string name = a ? a->name : "Terminal";
            std::string msg = (agent_state_ == AgentState::NeedsApproval)
                ? ("⚡ " + name + " needs approval")
                : ("⏸ " + name + " is waiting for input");
            app_.toast(msg, 6.f);
            if (!app_.window().focused()) app_.window().request_attention();
            app_.request_summary(activity_id, false);
        }
    }
}

bool Pane::update_artifacts(double now) {
    last_artifact_scan_ = now;
    std::string dir = cwd();
    auto found = agy_scan_artifacts(dir);
    for (const std::string& p : pinned_artifacts_) {
        bool already = false;
        for (const auto& a : found) if (a.path == p) { already = true; break; }
        if (!already) {
            if (auto a = agy_inspect_artifact(p)) found.insert(found.begin(), *a);
        }
    }
    if (found.empty() && artifacts_.empty()) return false;
    int64_t newest_mtime = 0;
    const AgyArtifact* newest_art = nullptr;
    for (const auto& a : found) {
        if (a.mtime > newest_mtime) { newest_mtime = a.mtime; newest_art = &a; }
    }
    bool first_scan = (last_artifact_mtime_ == 0);
    bool changed = (found.size() != artifacts_.size());
    if (!changed) {
        for (size_t i = 0; i < found.size(); ++i) {
            if (found[i].path != artifacts_[i].path || found[i].mtime != artifacts_[i].mtime) { changed = true; break; }
        }
    }
    std::string cur_path = active_artifact() ? active_artifact()->path : std::string();
    artifacts_ = std::move(found);
    if (!cur_path.empty()) {
        for (size_t i = 0; i < artifacts_.size(); ++i) {
            if (artifacts_[i].path == cur_path) { artifact_idx_ = (int)i; break; }
        }
    }
    if (artifact_idx_ >= (int)artifacts_.size()) artifact_idx_ = std::max(0, (int)artifacts_.size() - 1);
    if (newest_mtime > last_artifact_mtime_) {
        if (!first_scan) {
            artifact_unread_ = true;
            for (size_t i = 0; i < artifacts_.size(); ++i) {
                if (artifacts_[i].mtime == newest_mtime) { artifact_idx_ = (int)i; break; }
            }
            if (newest_art && newest_art->request_feedback && !drawer_open_) {
                drawer_open_ = true;
                app_.canvas().layout();
                const Activity* act = app_.tree().get(activity_id);
                app_.toast("📋 " + (act ? act->name + ": " : std::string()) + "Plan ready for review (" + newest_art->name + ")", 6.f);
            }
        }
        last_artifact_mtime_ = newest_mtime;
        changed = true;
    }
    if (artifacts_.empty() && drawer_open_) {
        drawer_open_ = false;
        app_.canvas().layout();
        changed = true;
    }
    if (drawer_open_) ensure_artifact_loaded();
    return changed;
}

static std::string read_text_file(const std::string& path) {
    std::ifstream f(path);
    if (!f) return "";
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

static std::string clean_inline_md(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    size_t i = 0;
    while (i < s.size()) {
        // [label](url) -> label
        if (s[i] == '[') {
            size_t rb = s.find(']', i + 1);
            if (rb != std::string_view::npos && rb + 1 < s.size() && s[rb + 1] == '(') {
                size_t rp = s.find(')', rb + 2);
                if (rp != std::string_view::npos) {
                    out.append(s.substr(i + 1, rb - i - 1));
                    i = rp + 1;
                    continue;
                }
            }
        }
        if (i + 1 < s.size() && s[i] == '*' && s[i + 1] == '*') {
            i += 2;
            continue;
        }
        out.push_back(s[i]);
        ++i;
    }
    return out;
}

void Pane::ensure_artifact_loaded() {
    const AgyArtifact* art = active_artifact();
    if (!art) {
        md_lines_.clear();
        loaded_artifact_path_.clear();
        loaded_artifact_mtime_ = 0;
        return;
    }
    int wrap_cols = std::max(24, (int)((drawer_body_rect().w - 16.f) / std::max(4.f, cell_w())));
    if (art->path == loaded_artifact_path_ && art->mtime == loaded_artifact_mtime_ && wrap_cols == loaded_wrap_cols_) return;
    loaded_artifact_path_ = art->path;
    loaded_artifact_mtime_ = art->mtime;
    loaded_wrap_cols_ = wrap_cols;
    md_lines_.clear();

    auto wrap_push = [&](MdLine::Kind kind, const std::string& raw, int indent) {
        std::string text = (kind == MdLine::Code) ? raw : clean_inline_md(raw);
        int max_c = std::max(14, wrap_cols - indent);
        if (text.empty()) {
            md_lines_.push_back({kind, "", indent});
            return;
        }
        std::u32string cps = utf8_decode(text);
        size_t pos = 0;
        bool first = true;
        while (pos < cps.size()) {
            int limit = first ? max_c : std::max(12, max_c - 2);
            int cur_ind = first ? indent : (indent + (kind == MdLine::Code ? 0 : 1));
            if ((int)(cps.size() - pos) <= limit) {
                std::string chunk;
                for (size_t k = pos; k < cps.size(); ++k) utf8_append(chunk, cps[k]);
                md_lines_.push_back({kind, std::move(chunk), cur_ind});
                break;
            }
            size_t brk = pos + limit;
            if (kind != MdLine::Code) {
                for (size_t k = brk; k > pos + limit / 3; --k) {
                    if (cps[k] == U' ' || cps[k] == U'\t') { brk = k; break; }
                }
            }
            std::string chunk;
            for (size_t k = pos; k < brk; ++k) utf8_append(chunk, cps[k]);
            md_lines_.push_back({kind, std::move(chunk), cur_ind});
            pos = brk;
            while (pos < cps.size() && kind != MdLine::Code && cps[pos] == U' ') ++pos;
            first = false;
        }
    };

    if (!art->summary.empty()) {
        size_t p = 0;
        while (p < art->summary.size()) {
            size_t nl = art->summary.find('\n', p);
            std::string sl = art->summary.substr(p, nl == std::string::npos ? std::string::npos : nl - p);
            if (!sl.empty() && sl.back() == '\r') sl.pop_back();
            if (!sl.empty()) wrap_push(MdLine::SummaryMeta, sl, 1);
            if (nl == std::string::npos) break;
            p = nl + 1;
        }
        if (!md_lines_.empty()) md_lines_.push_back({MdLine::Normal, "", 0});
    }

    std::string content = read_text_file(art->path);
    bool in_code = false;
    MdLine::Kind quote_kind = MdLine::Quote;
    size_t pos = 0;
    while (pos <= content.size()) {
        size_t nl = content.find('\n', pos);
        std::string line = content.substr(pos, nl == std::string::npos ? std::string::npos : nl - pos);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        size_t first_ns = line.find_first_not_of(" \t");
        std::string trimmed = (first_ns == std::string::npos) ? std::string() : line.substr(first_ns);

        if (trimmed.compare(0, 3, "```") == 0) {
            in_code = !in_code;
            if (nl == std::string::npos) break;
            pos = nl + 1;
            continue;
        }
        if (in_code) {
            wrap_push(MdLine::Code, line, 1);
            if (nl == std::string::npos) break;
            pos = nl + 1;
            continue;
        }
        if (trimmed.empty()) {
            quote_kind = MdLine::Quote;
            if (!md_lines_.empty() && (!md_lines_.back().text.empty() || md_lines_.back().kind != MdLine::Normal)) {
                md_lines_.push_back({MdLine::Normal, "", 0});
            }
            if (nl == std::string::npos) break;
            pos = nl + 1;
            continue;
        }
        if (trimmed == "---" || trimmed == "***" || trimmed == "___") {
            md_lines_.push_back({MdLine::Rule, "", 0});
            if (nl == std::string::npos) break;
            pos = nl + 1;
            continue;
        }
        if (trimmed.compare(0, 2, "# ") == 0) {
            wrap_push(MdLine::H1, trimmed.substr(2), 0);
        } else if (trimmed.compare(0, 3, "## ") == 0) {
            wrap_push(MdLine::H2, trimmed.substr(3), 0);
        } else if (trimmed.compare(0, 4, "### ") == 0) {
            wrap_push(MdLine::H3, trimmed.substr(4), 0);
        } else if (trimmed.compare(0, 5, "#### ") == 0) {
            wrap_push(MdLine::H3, trimmed.substr(5), 0);
        } else if (trimmed[0] == '>') {
            std::string q = trimmed.substr(1);
            if (!q.empty() && q[0] == ' ') q.erase(0, 1);
            auto check_alert = [&](const char* tag, MdLine::Kind k, const char* label) {
                size_t tl = std::strlen(tag);
                if (q.compare(0, tl, tag) == 0) {
                    quote_kind = k;
                    std::string rest = q.substr(tl);
                    size_t f = rest.find_first_not_of(" \t");
                    rest = (f == std::string::npos) ? std::string() : rest.substr(f);
                    wrap_push(k, rest.empty() ? std::string(label) : (std::string(label) + ": " + rest), 1);
                    return true;
                }
                return false;
            };
            if (!check_alert("[!NOTE]", MdLine::AlertNote, "ℹ Note") &&
                !check_alert("[!TIP]", MdLine::AlertTip, "💡 Tip") &&
                !check_alert("[!IMPORTANT]", MdLine::AlertImportant, "⚑ Important") &&
                !check_alert("[!WARNING]", MdLine::AlertWarning, "⚠ Warning") &&
                !check_alert("[!CAUTION]", MdLine::AlertCaution, "⛔ Caution")) {
                wrap_push(quote_kind, q, 1);
            }
        } else {
            int ind = std::min(6, (int)(first_ns == std::string::npos ? 0 : first_ns / 2));
            if (trimmed.compare(0, 6, "- [ ] ") == 0 || trimmed.compare(0, 6, "* [ ] ") == 0) {
                wrap_push(MdLine::Normal, "☐ " + trimmed.substr(6), ind + 1);
            } else if (trimmed.compare(0, 6, "- [x] ") == 0 || trimmed.compare(0, 6, "- [X] ") == 0 || trimmed.compare(0, 6, "* [x] ") == 0) {
                wrap_push(MdLine::Normal, "☑ " + trimmed.substr(6), ind + 1);
            } else if (trimmed.compare(0, 2, "- ") == 0 || trimmed.compare(0, 2, "* ") == 0) {
                wrap_push(MdLine::Normal, "• " + trimmed.substr(2), ind + 1);
            } else {
                wrap_push(MdLine::Normal, trimmed, ind);
            }
        }
        if (nl == std::string::npos) break;
        pos = nl + 1;
    }
}

// ---------------------------------------------------------------- update
bool Pane::update(double now) {
    bool changed = false;
    if (last_artifact_scan_ == 0 || now - last_artifact_scan_ >= 1.5) {
        if (update_artifacts(now)) changed = true;
    }
    if (!session) return changed;
    if (session->pump()) changed = true;
    Terminal& t = session->term();
    if (t.modes().synchronized_output) {
        if (sync_start_ == 0) sync_start_ = now;
        else if (now - sync_start_ > 0.1) {
            t.end_synchronized_output();
            sync_start_ = 0;
            vis_dirty_ = true;
            changed = true;
        }
    } else {
        sync_start_ = 0;
    }
    uint64_t gen = t.generation();
    if (gen != last_gen_) {
        last_gen_ = gen;
        last_output = now;
        output_since_input_ = true;
        agent_notified_ = false;
        if (app_.focused_id() != activity_id) unread = true;
        vis_dirty_ = true;
        changed = true;
        if (!matches_.empty()) set_search(search_);
    }
    AgentState prev_agent = agent_state_;
    update_agent_state(now);
    if (agent_state_ != prev_agent) changed = true;
    if (t.history_base() != last_history_base_) {
        last_history_base_ = t.history_base();
        session->blocks().on_history_base(last_history_base_);
        vis_dirty_ = true;
    }
    // scan finished blocks (a few per frame)
    int budget = 3;
    auto& bl = session->blocks().blocks();
    for (size_t i = bl.size(); i-- > 0 && budget > 0;) {
        const Block& b = bl[i];
        if (b.state != BlockState::Done) continue;
        BlockView& v = views_[b.id];
        if (v.scanned) break;   // older ones were scanned already
        scan_block(b, v);
        budget--;
        vis_dirty_ = true;
        changed = true;
    }
    if (!pending_input_.empty() && at_prompt()) {
        for (auto& s : pending_input_) write(s);
        pending_input_.clear();
    }
    if (!session->alive() && exited_at_ == 0) { exited_at_ = now; vis_dirty_ = true; changed = true; }
    return changed;
}

// ---------------------------------------------------------------- visible rows
const std::vector<VisRow>& Pane::vis() {
    if (vis_dirty_ || (session && session->term().generation() != vis_gen_)) rebuild_vis();
    return vis_;
}

void Pane::rebuild_vis() {
    vis_.clear();
    for (size_t i = 0; i < restored_.size(); i++) {
        VisRow r; r.kind = VisRow::Restored; r.abs = -(int64_t)restored_.size() + (int64_t)i; r.vidx = (int)i; r.block = restored_block_of_[i]; vis_.push_back(r);
    }
    if (session) {
        const Terminal& t = session->term();
        if (t.modes().alt_screen) {
            vis_.clear();
            for (int y = 0; y < t.rows(); y++) { VisRow r; r.kind = VisRow::Term; r.abs = t.screen_base() + y; vis_.push_back(r); }
        } else {
            const auto& bl = session->blocks().blocks();
            struct Range { int64_t start, end; int block; };
            std::vector<Range> ranges;
            for (size_t i = 0; i < bl.size(); i++) {
                const Block& b = bl[i];
                if (b.state != BlockState::Done || b.output_line < 0 || b.end_line <= b.output_line) continue;
                auto it = views_.find(b.id);
                bool special = b.folded || (it != views_.end() && (it->second.show_virtual || !it->second.order.empty()));
                if (special) ranges.push_back({b.output_line, b.end_line, (int)i});
            }
            int64_t base = t.history_base(), end = t.screen_base() + t.rows();
            if (!restored_.empty() && t.history_count() == 0) {
                // fresh shell under restored history: don't let blank screen rows push the history away
                int last = t.cursor().y;
                for (int y = t.rows() - 1; y > last; y--) {
                    const Line& ln = t.row(y);
                    bool blank = true;
                    for (const Cell& c : ln.cells) if (!c.is_blank() || c.bg.kind != Color::Default) { blank = false; break; }
                    if (!blank) { last = y; break; }
                }
                end = t.screen_base() + last + 1;
            }
            size_t ri = 0; int bj = -1; int n = (int)bl.size();
            vis_.reserve(vis_.size() + (size_t)(end - base));
            for (int64_t a = base; a < end; a++) {
                while (ri < ranges.size() && ranges[ri].end <= a) ri++;
                while (bj + 1 < n && bl[bj + 1].prompt_line >= 0 && bl[bj + 1].prompt_line <= a) bj++;
                int blk = (bj >= 0 && (bl[bj].end_line < 0 || a < bl[bj].end_line)) ? bj : -1;
                if (ri < ranges.size() && std::max(ranges[ri].start, base) == a) {
                    const Block& b = bl[ranges[ri].block];
                    BlockView& v = views_[b.id];
                    int bi = ranges[ri].block;
                    if (b.folded) {
                        VisRow r; r.kind = VisRow::Summary; r.abs = a; r.block = bi; vis_.push_back(r);
                    } else if (v.show_virtual) {
                        for (size_t k = 0; k < v.virtual_lines.size(); k++) { VisRow r; r.kind = VisRow::Virtual; r.abs = a; r.block = bi; r.vidx = (int)k; vis_.push_back(r); }
                    } else {
                        int64_t data_start = b.output_line + v.table.first_data_row;
                        for (int64_t q = a; q < std::min(data_start, b.end_line); q++) { VisRow r; r.abs = q; r.block = bi; vis_.push_back(r); }
                        for (int64_t q : v.order) if (q >= base) { VisRow r; r.abs = q; r.block = bi; vis_.push_back(r); }
                    }
                    a = ranges[ri].end - 1;
                    continue;
                }
                VisRow r; r.kind = VisRow::Term; r.abs = a; r.block = blk; vis_.push_back(r);
            }
        }
        vis_gen_ = t.generation();
    }
    vis_dirty_ = false;
    if (!follow_ && anchor_abs_ != INT64_MIN) {
        int r = row_of_abs(anchor_abs_);
        if (r >= 0) top_ = r;
    }
    clamp_scroll();
}

const Line* Pane::line_for(const VisRow& vr) const {
    switch (vr.kind) {
    case VisRow::Term: {
        if (!session) return nullptr;
        const Terminal& t = session->term();
        if (t.modes().alt_screen) return &t.row((int)(vr.abs - t.screen_base()));
        if (vr.abs < t.history_base() || vr.abs >= t.screen_base() + t.rows()) return nullptr;
        return &t.line_abs(vr.abs);
    }
    case VisRow::Virtual: {
        const Block* b = block(vr.block);
        if (!b) return nullptr;
        auto it = views_.find(b->id);
        if (it == views_.end() || vr.vidx >= (int)it->second.virtual_lines.size()) return nullptr;
        return &it->second.virtual_lines[vr.vidx];
    }
    case VisRow::Restored: return vr.vidx < (int)restored_.size() ? &restored_[vr.vidx] : nullptr;
    default: return nullptr;
    }
}

int Pane::row_of_abs(int64_t abs) const {
    // binary search over Restored+Term rows (abs is non-decreasing except sorted tables; fall back to linear)
    size_t lo = 0, hi = vis_.size();
    while (lo < hi) {
        size_t mid = (lo + hi) / 2;
        if (vis_[mid].abs < abs) lo = mid + 1; else hi = mid;
    }
    if (lo < vis_.size() && (vis_[lo].kind == VisRow::Term || vis_[lo].kind == VisRow::Restored) && vis_[lo].abs == abs) return (int)lo;
    for (size_t i = 0; i < vis_.size(); i++) if ((vis_[i].kind == VisRow::Term || vis_[i].kind == VisRow::Restored) && vis_[i].abs == abs) return (int)i;
    return -1;
}

void Pane::clamp_scroll() {
    int64_t maxtop = std::max<int64_t>(0, (int64_t)vis_.size() - rows);
    if (follow_) top_ = maxtop;
    if (top_ > maxtop) top_ = maxtop;
    if (top_ < 0) top_ = 0;
    if (top_ == maxtop) {
        follow_ = true;
        anchor_abs_ = INT64_MIN;
    } else if (top_ >= 0 && top_ < (int64_t)vis_.size()) {
        anchor_abs_ = vis_[top_].abs;
    }
}

int64_t Pane::lines_below() const {
    if (follow_ || (session && session->term().modes().alt_screen)) return 0;
    int64_t maxtop = std::max<int64_t>(0, (int64_t)vis_.size() - rows);
    return std::max<int64_t>(0, maxtop - top_);
}

Rect Pane::scroll_pill_rect() const {
    Rect T = term_rect();
    float w = std::min(T.w - 24.f, 290.f);
    float h = 24.f;
    return {T.x + (T.w - w) / 2.f, T.y + T.h - h - 8.f, w, h};
}

void Pane::scroll_lines(int delta) {
    vis();
    if (session && session->term().modes().alt_screen) return;
    anchor_abs_ = INT64_MIN;
    top_ += delta;
    follow_ = false;
    clamp_scroll();
}
void Pane::scroll_pages(int delta) { scroll_lines(delta * std::max(1, rows - 2)); }
void Pane::scroll_to_bottom() { follow_ = true; anchor_abs_ = INT64_MIN; clamp_scroll(); }
void Pane::scroll_to_abs(int64_t abs) {
    vis();
    int r = row_of_abs(abs);
    if (r < 0) return;
    anchor_abs_ = INT64_MIN;
    top_ = std::max<int64_t>(0, r - rows / 3);
    follow_ = false;
    clamp_scroll();
}

// ---------------------------------------------------------------- blocks
int Pane::block_count() const { return session ? (int)session->blocks().blocks().size() : 0; }
const Block* Pane::block(int idx) const {
    if (!session || idx < 0 || idx >= block_count()) return nullptr;
    return &session->blocks().blocks()[idx];
}
BlockView& Pane::view(const Block& b) { return views_[b.id]; }

void Pane::select_block(int idx, bool scroll_into_view) {
    if (idx < 0 || idx >= block_count()) { selected_block = -1; return; }
    selected_block = idx;
    if (scroll_into_view) {
        const Block* b = block(idx);
        int64_t target = b->prompt_line >= 0 ? b->prompt_line : b->cmd_line;
        if (target >= 0) {
            vis();
            int r = row_of_abs(target);
            if (r >= 0 && (r < top_ || r >= top_ + rows)) { top_ = std::max<int64_t>(0, r - 1); follow_ = false; clamp_scroll(); }
        }
    }
    app_.request_redraw();
}

int Pane::block_at_cursor() const {
    if (!session) return -1;
    const Terminal& t = session->term();
    return session->blocks().index_at_line(t.screen_base() + t.cursor().y);
}

void Pane::select_block_relative(int delta) {
    int n = block_count();
    if (n == 0) return;
    int cur = selected_block;
    if (cur < 0) {
        // start from the block nearest the viewport top (or the last one)
        vis();
        cur = n;
        for (int64_t i = top_; i < (int64_t)vis_.size() && i < top_ + rows; i++) if (vis_[i].block >= 0) { cur = vis_[i].block; break; }
        if (delta > 0 && cur == n) cur = n - 1;
        if (delta < 0) cur = std::min(n, cur);   // previous of "current" = cur-1
        else cur = cur - 1;
    }
    int nx = std::max(0, std::min(n - 1, cur + delta));
    select_block(nx, true);
}

void Pane::toggle_fold(int idx) { if (const Block* b = block(idx)) set_fold(idx, !b->folded); }
void Pane::set_fold(int idx, bool folded) {
    if (!session || idx < 0 || idx >= block_count()) return;
    Block& b = session->blocks().blocks()[idx];
    if (b.state != BlockState::Done) return;
    b.folded = folded;
    vis_dirty_ = true;
    app_.request_redraw();
}

void Pane::rerun(int idx) {
    const Block* b = block(idx);
    if (!b || b->command.empty()) return;
    if (!at_prompt()) { app_.toast("Shell is busy; cannot re-run now"); return; }
    write(b->command + "\r");
    scroll_to_bottom();
}

std::vector<std::string> Pane::block_lines(const Block& b, bool join_wrapped) const {
    std::vector<std::string> out;
    if (!session || b.output_line < 0) return out;
    const Terminal& t = session->term();
    int64_t s = std::max(b.output_line, t.history_base());
    int64_t e = b.end_line >= 0 ? b.end_line : t.screen_base() + t.cursor().y;
    e = std::min(e, t.screen_base() + t.rows());
    out.reserve((size_t)std::max<int64_t>(0, e - s));
    std::string cur;
    for (int64_t a = s; a < e; a++) {
        const Line& ln = t.line_abs(a);
        cur += ln.text();
        if (ln.wrapped && join_wrapped) continue;    // join soft-wrapped lines
        out.push_back(std::move(cur)); cur.clear();
    }
    if (!cur.empty()) out.push_back(cur);
    return out;
}

std::string Pane::block_output(int idx, int max_lines) const {
    const Block* b = block(idx);
    if (!b) return "";
    auto lines = block_lines(*b);
    size_t start = (max_lines > 0 && lines.size() > (size_t)max_lines) ? lines.size() - max_lines : 0;
    std::string out;
    for (size_t i = start; i < lines.size(); i++) { out += lines[i]; out += '\n'; }
    return out;
}
std::string Pane::block_command(int idx) const { const Block* b = block(idx); return b ? b->command : ""; }

void Pane::scan_block(const Block& b, BlockView& v) {
    v.scanned = true;
    v.error_lines.clear(); v.warning_lines.clear(); v.error_count = v.warning_count = 0;
    v.output_lines = b.output_lines();
    if (!session || b.output_line < 0 || b.end_line <= b.output_line) return;
    if (!app_.config().structured_output) return;
    const Terminal& t = session->term();
    int64_t s = std::max(b.output_line, t.history_base());
    int64_t e = std::min(b.end_line, t.screen_base() + t.rows());
    std::vector<std::string> lines;
    bool collect = (e - s) <= 5000;
    for (int64_t a = s; a < e; a++) {
        std::string txt = t.line_abs(a).text();
        if (is_error_line(txt)) { v.error_lines.push_back(a); v.error_count++; }
        else if (is_warning_line(txt)) { v.warning_lines.push_back(a); v.warning_count++; }
        if (collect) lines.push_back(std::move(txt));
    }
    if (collect && lines.size() >= 3) {
        v.table = detect_table(lines);
        if (!v.table.is_table) v.table = TableInfo{};
    }
    if (collect && !lines.empty()) {
        size_t first = 0; while (first < lines.size() && lines[first].find_first_not_of(" \t") == std::string::npos) first++;
        if (first < lines.size()) {
            char c = lines[first][lines[first].find_first_not_of(" \t")];
            if (c == '{' || c == '[') {
                std::string all; for (auto& l : lines) { all += l; all += '\n'; }
                v.is_json = looks_like_json(all) && pretty_json(all).has_value();
            }
        }
    }
}

void Pane::toggle_pretty_json(int idx) {
    const Block* b = block(idx);
    if (!b) return;
    BlockView& v = view(*b);
    if (!v.is_json) return;
    if (v.virtual_lines.empty()) {
        auto pretty = pretty_json(block_output(idx));
        if (!pretty) return;
        size_t pos = 0;
        while (pos <= pretty->size()) {
            size_t nl = pretty->find('\n', pos);
            std::string l = pretty->substr(pos, nl == std::string::npos ? std::string::npos : nl - pos);
            if (!(nl == std::string::npos && l.empty())) v.virtual_lines.push_back(make_line(l, Color::def(), 0));
            if (nl == std::string::npos) break;
            pos = nl + 1;
        }
    }
    v.show_virtual = !v.show_virtual;
    vis_dirty_ = true;
    app_.request_redraw();
}

void Pane::sort_table(int idx, int col) {
    const Block* b = block(idx);
    if (!b) return;
    BlockView& v = view(*b);
    if (!v.table.is_table || col < 0 || col >= (int)v.table.col_begins.size()) return;
    bool desc = false;
    if (v.sort_col == col) {
        if (!v.sort_desc) desc = true; else { clear_sort(idx); return; }
    }
    auto lines = block_lines(*b, false);
    auto perm = sort_rows(lines, v.table, col, desc);
    v.order.clear();
    const Terminal& t = session->term();
    for (int i : perm) {
        int64_t a = b->output_line + i;
        if (i > 0 && t.line_abs(a - 1).wrapped) continue;      // continuation rows follow their first row
        v.order.push_back(a);
        for (int64_t q = a; q + 1 < b->end_line && t.line_abs(q).wrapped; q++) v.order.push_back(q + 1);
    }
    v.sort_col = col; v.sort_desc = desc;
    vis_dirty_ = true;
    app_.request_redraw();
}
void Pane::clear_sort(int idx) {
    const Block* b = block(idx);
    if (!b) return;
    BlockView& v = view(*b);
    v.order.clear(); v.sort_col = -1; v.sort_desc = false;
    vis_dirty_ = true;
}

void Pane::next_error(int dir) {
    if (!session) return;
    std::vector<std::pair<int64_t, int>> errs;
    const auto& bl = session->blocks().blocks();
    for (size_t i = 0; i < bl.size(); i++) {
        auto it = views_.find(bl[i].id);
        if (it == views_.end()) continue;
        for (int64_t a : it->second.error_lines) errs.push_back({a, (int)i});
    }
    if (errs.empty()) { app_.toast("No error lines detected"); return; }
    std::sort(errs.begin(), errs.end());
    vis();
    int64_t ref = (top_ < (int64_t)vis_.size()) ? vis_[top_].abs + rows / 3 : 0;
    if (!matches_.empty() && match_idx_ >= 0) ref = matches_[match_idx_].abs;
    size_t pick = 0;
    if (dir > 0) { pick = errs.size() - 1; for (size_t i = 0; i < errs.size(); i++) if (errs[i].first > ref) { pick = i; break; } }
    else { pick = 0; for (size_t i = errs.size(); i-- > 0;) if (errs[i].first < ref) { pick = i; break; } }
    select_block(errs[pick].second, false);
    scroll_to_abs(errs[pick].first);
    // highlight the line via a one-off search match
    matches_.clear(); matches_.push_back({errs[pick].first, 0, cols}); match_idx_ = 0;
}

std::vector<Block> Pane::export_blocks(int max_blocks, int tail_lines) const {
    std::vector<Block> out = restored_blocks_;
    if (session) {
        const auto& bl = session->blocks().blocks();
        size_t start = bl.size() > (size_t)max_blocks ? bl.size() - max_blocks : 0;
        for (size_t i = start; i < bl.size(); i++) {
            if (bl[i].state != BlockState::Done || bl[i].command.empty()) continue;
            Block b = bl[i];
            auto lines = block_lines(b);
            size_t s = lines.size() > (size_t)tail_lines ? lines.size() - tail_lines : 0;
            b.saved_output.assign(lines.begin() + s, lines.end());
            out.push_back(std::move(b));
        }
    }
    if (out.size() > (size_t)max_blocks) out.erase(out.begin(), out.begin() + (out.size() - max_blocks));
    return out;
}

Line Pane::make_line(const std::string& utf8, Color fg, uint16_t attrs) const {
    Line ln;
    ln.cells.resize(cols);
    for (auto& c : ln.cells) { c.cp = U' '; }
    std::u32string cps = utf8_decode(utf8);
    int x = 0;
    for (char32_t cp : cps) {
        if (x >= cols) break;
        if (cp == U'\t') { int nx = std::min(cols, (x / 8 + 1) * 8); for (; x < nx; x++) { ln.cells[x].fg = fg; ln.cells[x].attrs = attrs; } continue; }
        if (cp < 0x20) continue;
        Cell& c = ln.cells[x];
        c.cp = cp; c.fg = fg; c.attrs = attrs;
        if (is_wide_cp(cp) && x + 1 < cols) { c.attrs |= ATTR_WIDE; ln.cells[x + 1].attrs = attrs | ATTR_WIDE_CONT; ln.cells[x + 1].fg = fg; x += 2; }
        else x++;
    }
    return ln;
}

void Pane::load_restored(const std::vector<Block>& saved) {
    restored_.clear(); restored_block_of_.clear();
    restored_blocks_ = saved;
    const Theme& th = app_.theme();
    Color accent = Color::rgb(th.accent.r, th.accent.g, th.accent.b);
    Color dim = Color::def();
    for (size_t i = 0; i < saved.size(); i++) {
        const Block& b = saved[i];
        if (b.command.empty()) continue;
        std::string head = "❯ " + b.command;
        restored_.push_back(make_line(head, accent, ATTR_BOLD)); restored_block_of_.push_back((int)i);
        for (auto& l : b.saved_output) { restored_.push_back(make_line(l, dim, ATTR_DIM)); restored_block_of_.push_back((int)i); }
    }
    vis_dirty_ = true;
}

// ---------------------------------------------------------------- selection
bool Pane::selected(int64_t abs, int col) const {
    if (sel_.empty()) return false;
    int64_t l0, l1; int c0, c1;
    sel_.ordered(l0, c0, l1, c1);
    if (abs < l0 || abs > l1) return false;
    if (sel_.mode == 2) return true;
    if (l0 == l1) return col >= c0 && col <= c1;
    if (abs == l0) return col >= c0;
    if (abs == l1) return col <= c1;
    return true;
}

void Pane::extend_selection(int64_t abs, int col) {
    if (!sel_.active) return;
    sel_.hl = abs; sel_.hc = col;
    if (sel_.mode == 1 && session) {
        int b, e;
        session->term().word_at(abs, col, &b, &e);
        bool forward = (abs > sel_.al) || (abs == sel_.al && col >= sel_.ac);
        sel_.hc = forward ? std::max(col, e - 1) : std::min(col, b);
    }
}

std::string Pane::selected_text() const {
    if (sel_.empty()) return "";
    int64_t l0, l1; int c0, c1;
    sel_.ordered(l0, c0, l1, c1);
    if (sel_.mode == 2) { c0 = 0; c1 = cols - 1; }
    if (l0 >= 0) return session ? session->term().text(l0, c0, l1, c1 + 1) : "";
    std::string out;
    int64_t rcount = (int64_t)restored_.size();
    int64_t rend = std::min<int64_t>(-1, l1);
    for (int64_t a = std::max<int64_t>(-rcount, l0); a <= rend; a++) {
        const Line& ln = restored_[(size_t)(a + rcount)];
        int b = (a == l0) ? c0 : 0;
        int e = (a == l1) ? (c1 + 1) : ln.width();
        out += ln.text(b, e);
        if (a < l1) out += '\n';
    }
    if (l1 >= 0 && session) {
        int64_t tb = session->term().history_base();
        out += session->term().text(tb, 0, l1, c1 + 1);
    }
    return out;
}

void Pane::clear_selection() { sel_ = {}; app_.request_redraw(); }

void Pane::select_all_block(int idx) {
    const Block* b = block(idx);
    if (!b || b->output_line < 0) return;
    sel_ = {};
    sel_.active = true; sel_.mode = 2;
    sel_.al = b->output_line; sel_.ac = 0;
    sel_.hl = std::max(b->output_line, (b->end_line >= 0 ? b->end_line : b->output_line + 1) - 1); sel_.hc = cols - 1;
    app_.set_primary(selected_text());
}

// ---------------------------------------------------------------- search
void Pane::set_search(const std::string& q) {
    search_ = q;
    matches_.clear(); match_idx_ = -1;
    if (q.empty()) return;
    std::string nq = to_lower(q);
    auto scan = [&](int64_t a, const std::string& txt) {
        if (txt.empty()) return;
        std::string lt = to_lower(txt);
        size_t pos = 0;
        while ((pos = lt.find(nq, pos)) != std::string::npos && matches_.size() < 5000) {
            int col = (int)utf8_length(std::string_view(txt).substr(0, pos));
            matches_.push_back({a, col, (int)utf8_length(nq)});
            pos += nq.size();
        }
    };
    for (size_t i = 0; i < restored_.size() && matches_.size() < 5000; i++) {
        scan(-(int64_t)restored_.size() + (int64_t)i, restored_[i].text());
    }
    if (session) {
        const Terminal& t = session->term();
        int64_t end = t.modes().alt_screen ? 0 : t.screen_base() + t.rows();
        for (int64_t a = t.history_base(); a < end && matches_.size() < 5000; a++) {
            scan(a, t.line_abs(a).text());
        }
    }
    // pick the match nearest to the viewport
    vis();
    int64_t ref = (top_ < (int64_t)vis_.size()) ? vis_[top_].abs : 0;
    for (size_t i = 0; i < matches_.size(); i++) if (matches_[i].abs >= ref) { match_idx_ = (int)i; break; }
    if (match_idx_ < 0 && !matches_.empty()) match_idx_ = (int)matches_.size() - 1;
    if (match_idx_ >= 0) scroll_to_abs(matches_[match_idx_].abs);
}

bool Pane::search_step(int dir) {
    if (matches_.empty()) return false;
    match_idx_ = (int)(((size_t)(match_idx_ + dir) + matches_.size()) % matches_.size());
    scroll_to_abs(matches_[match_idx_].abs);
    return true;
}

// ---------------------------------------------------------------- input
bool Pane::mouse_to_app() const {
    return live() && session->term().modes().mouse != MouseMode::None;
}

void Pane::send_mouse(const MouseEvent& ev) {
    if (!live()) return;
    std::string s = encode_mouse(ev, session->term().modes());
    if (!s.empty()) write(s);
}

void Pane::mouse_down(float wx, float wy, MouseButton b, int clicks, uint32_t mods) {
    set_hover(wx, wy);
    if (drawer_open() && !minimized() && drawer_rect().contains(wx, wy)) {
        if (b == MouseButton::Left) {
            if (hover.drawer_close) { set_drawer_open(false); return; }
            if (hover.drawer_prev) { step_artifact(-1); return; }
            if (hover.drawer_next) { step_artifact(1); return; }
            if (hover.drawer_proceed) {
                artifact_unread_ = false;
                app_.ensure_session(*this);
                write("Proceed\r");
                scroll_to_bottom();
                const Activity* act = app_.tree().get(activity_id);
                app_.toast("✓ Sent 'Proceed' to " + (act ? act->name : std::string("terminal")));
                app_.request_redraw();
                return;
            }
            if (hover.drawer_feedback) {
                const AgyArtifact* art = active_artifact();
                std::string label = art ? ("Feedback on " + art->name) : "Feedback for agent";
                app_.prompt_text(label, "", [this](const std::string& fb) {
                    if (!fb.empty()) {
                        artifact_unread_ = false;
                        app_.ensure_session(*this);
                        write(fb + "\r");
                        scroll_to_bottom();
                    }
                });
                return;
            }
            if (hover.drawer_edit || (clicks >= 2 && drawer_body_rect().contains(wx, wy))) {
                if (const AgyArtifact* art = active_artifact()) {
                    app_.open_path(art->path, 0, cwd(), activity_id);
                }
                return;
            }
        } else if (b == MouseButton::Right) {
            std::vector<MenuItem> items;
            for (size_t i = 0; i < artifacts_.size(); ++i) {
                int idx = (int)i;
                std::string prefix = (idx == artifact_idx_) ? "● " : "  ";
                items.push_back({prefix + artifacts_[i].name, [this, idx] { select_artifact(idx); }});
            }
            if (!items.empty()) items.push_back({"-", nullptr});
            if (const AgyArtifact* art = active_artifact()) {
                std::string path = art->path;
                items.push_back({"Open in editor", [this, path] { app_.open_path(path, 0, cwd(), activity_id); }});
                items.push_back({"Copy file path", [this, path] { app_.set_clipboard(path); app_.toast("Copied artifact path"); }});
                items.push_back({"Copy markdown content", [this, path] { app_.set_clipboard(read_text_file(path)); app_.toast("Copied markdown content"); }});
            }
            items.push_back({"Close artifact drawer", [this] { set_drawer_open(false); }});
            float sx = app_.canvas().to_screen_x(wx), sy = app_.canvas().to_screen_y(wy);
            app_.show_menu(sx, sy, std::move(items));
        }
        return;
    }
    int row, c;
    bool in_cells = cell_at(wx, wy, &row, &c);
    vis();
    int64_t vi = in_cells ? top_ + row : -1;
    const VisRow* vr = (vi >= 0 && vi < (int64_t)vis_.size()) ? &vis_[vi] : nullptr;

    if (hover.gutter) {
        if (b == MouseButton::Left && vr && vr->block >= 0) {
            select_block(vr->block, false);
            if (clicks == 2) toggle_fold(vr->block);
        } else if (b == MouseButton::Right && vr && vr->block >= 0) {
            select_block(vr->block, false);
            mouse_down(wx + GUTTER_W + PAD, wy, b, clicks, mods);  // reuse term-area menu
        }
        return;
    }
    if (!in_cells) return;

    if (b == MouseButton::Left) {
        if (hover.scroll_pill) { scroll_to_bottom(); app_.request_redraw(); return; }
        if (vr && vr->kind == VisRow::Summary) { set_fold(vr->block, false); return; }
        if (hover.chip && hover.block >= 0) {
            const Block* bk = block(hover.block);
            if (bk && view(*bk).is_json) toggle_pretty_json(hover.block); else toggle_fold(hover.block);
            return;
        }
        if (hover.header && hover.block >= 0 && hover.table_col >= 0) { sort_table(hover.block, hover.table_col); return; }
        if (mouse_to_app() && !(mods & MOD_SHIFT)) {
            MouseEvent me; me.action = MouseAction::Press; me.button = b; me.col = c; me.row = row; me.mods = mods;
            Rect t = term_rect(); me.px = (int)(wx - t.x); me.py = (int)(wy - t.y);
            send_mouse(me); return;
        }
        if ((mods & MOD_CTRL) && hover.valid) {
            if (hover.link) { app_.open_url(hover.link_uri); return; }
            if (hover.span) {
                const Span& sp = *hover.span;
                if (sp.kind == SpanKind::Url) app_.open_url(sp.value);
                else if (sp.kind == SpanKind::Path) app_.open_path(sp.value, sp.line_no, cwd(), activity_id);
                else if (sp.kind == SpanKind::Email) app_.open_url("mailto:" + sp.value);
                else app_.set_clipboard(sp.value);
                return;
            }
        }
        if (!vr || (vr->kind != VisRow::Term && vr->kind != VisRow::Restored)) { clear_selection(); return; }
        sel_ = {};
        sel_.active = true; sel_.mode = clicks >= 3 ? 2 : clicks == 2 ? 1 : 0;
        sel_.al = sel_.hl = vr->abs; sel_.ac = sel_.hc = c;
        if (sel_.mode == 1 && vr->kind == VisRow::Term && session) {
            int wb, we; session->term().word_at(vr->abs, c, &wb, &we);
            sel_.ac = wb; sel_.hc = std::max(wb, we - 1);
        }
        selecting_ = true;
        app_.request_redraw();
    } else if (b == MouseButton::Middle) {
        if (mouse_to_app() && !(mods & MOD_SHIFT)) {
            MouseEvent me; me.action = MouseAction::Press; me.button = b; me.col = c; me.row = row; me.mods = mods; send_mouse(me);
        } else {
            std::string p = app_.primary();
            if (!p.empty()) paste(p);
        }
    } else if (b == MouseButton::Right) {
        if (mouse_to_app() && !(mods & MOD_SHIFT)) {
            MouseEvent me; me.action = MouseAction::Press; me.button = b; me.col = c; me.row = row; me.mods = mods; send_mouse(me); return;
        }
        std::vector<MenuItem> items;
        int bi = vr ? vr->block : -1;
        if (vr && vr->kind == VisRow::Restored && bi >= 0 && bi < (int)restored_blocks_.size()) {
            const Block& bk = restored_blocks_[bi];
            std::string cmd = bk.command;
            uint64_t aid = activity_id;
            if (!cmd.empty()) {
                items.push_back({"Re-run  " + ellipsize(cmd, 30), [this, cmd] {
                    app_.ensure_session(*this);
                    if (at_prompt()) { write(cmd + "\r"); scroll_to_bottom(); }
                    else queue_input_at_prompt(cmd + "\r");
                }});
                items.push_back({"Run in child activity", [this, cmd, aid] { app_.run_in_child(aid, cmd, cwd()); }});
                items.push_back({"Copy command", [this, cmd] { app_.set_clipboard(cmd); }});
            }
            if (!bk.saved_output.empty()) {
                std::string out;
                for (const auto& l : bk.saved_output) { out += l; out += '\n'; }
                items.push_back({"Copy output", [this, out] { app_.set_clipboard(out); }});
            }
            if (!items.empty()) items.push_back({"-", nullptr});
        } else if (bi >= 0 && block(bi)) {
            const Block* bk = block(bi);
            std::string cmd = bk->command;
            uint64_t aid = activity_id;
            if (!cmd.empty()) {
                items.push_back({"Re-run  " + ellipsize(cmd, 30), [this, bi] { rerun(bi); }});
                items.push_back({"Run in child activity", [this, cmd, aid] { app_.run_in_child(aid, cmd, cwd()); }});
                items.push_back({"Copy command", [this, cmd] { app_.set_clipboard(cmd); }});
            }
            if (bk->state == BlockState::Done) {
                items.push_back({"Copy output", [this, bi] { app_.set_clipboard(block_output(bi)); }});
                items.push_back({"Select output", [this, bi] { select_all_block(bi); }});
                items.push_back({bk->folded ? "Unfold" : "Fold", [this, bi] { toggle_fold(bi); }});
                // previous run with the same command
                int prev = -1;
                for (int k = bi - 1; k >= 0; k--) if (block(k)->command == cmd && block(k)->state == BlockState::Done) { prev = k; break; }
                if (prev >= 0) items.push_back({"Diff with previous run", [this, bi, prev, cmd] { app_.diff_outputs(activity_id, cmd, block_output(prev), block_output(bi)); }});
                BlockView& v = view(*bk);
                if (v.is_json) items.push_back({v.show_virtual ? "Show raw JSON" : "Pretty-print JSON", [this, bi] { toggle_pretty_json(bi); }});
                if (!v.order.empty()) items.push_back({"Clear table sort", [this, bi] { clear_sort(bi); }});
            }
            Block* mb = const_cast<Block*>(bk);
            items.push_back({bk->pinned ? "Unpin" : "Pin", [this, mb] { mb->pinned = !mb->pinned; app_.request_redraw(); }});
            items.push_back({"-", nullptr});
        }
        if (hover.span) {
            const Span sp = *hover.span;
            std::string label = sp.kind == SpanKind::Url ? "Open link" : sp.kind == SpanKind::Path ? "Open " + ellipsize(sp.value, 24) : "Copy " + ellipsize(sp.value, 24);
            items.push_back({label, [this, sp] {
                if (sp.kind == SpanKind::Url) app_.open_url(sp.value);
                else if (sp.kind == SpanKind::Path) app_.open_path(sp.value, sp.line_no, cwd(), activity_id);
                else app_.set_clipboard(sp.value);
            }});
            if (sp.kind == SpanKind::Path) {
                items.push_back({"Insert path", [this, sp] { write(shell_quote(sp.value)); }});
                items.push_back({"Copy path", [this, sp] { app_.set_clipboard(sp.value); }});
            }
            items.push_back({"-", nullptr});
        }
        if (hover.link) { std::string u = hover.link_uri; items.push_back({"Open hyperlink", [this, u] { app_.open_url(u); }}); items.push_back({"-", nullptr}); }
        items.push_back({"Copy", [this] { app_.set_clipboard(selected_text()); }, has_selection()});
        items.push_back({"Paste", [this] { paste(app_.clipboard()); }, live()});
        items.push_back({"-", nullptr});
        items.push_back({"New child activity", [this] { app_.run_action("activity.new_child"); }});
        items.push_back({"Rename activity…", [this] { app_.run_action("activity.rename"); }});
        items.push_back({"Add note…", [this] { app_.run_action("activity.note"); }});
        items.push_back({"Close activity", [this] { app_.run_action("activity.close"); }});
        float sx = app_.canvas().to_screen_x(wx), sy = app_.canvas().to_screen_y(wy);
        app_.show_menu(sx, sy, std::move(items));
    }
}

void Pane::mouse_up(float wx, float wy, MouseButton b, uint32_t mods) {
    int row, c;
    bool in_cells = cell_at(wx, wy, &row, &c);
    if (selecting_) {
        selecting_ = false;
        if (sel_.empty() && sel_.mode == 0) { clear_selection(); }
        else { std::string s = selected_text(); if (!s.empty()) app_.set_primary(s); }
        return;
    }
    if (mouse_to_app() && !(mods & MOD_SHIFT)) {
        MouseEvent me; me.action = MouseAction::Release; me.button = b; me.mods = mods;
        if (in_cells) { me.col = c; me.row = row; } else { me.col = std::max(0, std::min(cols - 1, c)); me.row = std::max(0, std::min(rows - 1, row)); }
        Rect t = term_rect(); me.px = (int)(wx - t.x); me.py = (int)(wy - t.y);
        send_mouse(me);
    }
}

void Pane::mouse_move(float wx, float wy, uint32_t mods, MouseButton held) {
    int row, c;
    Rect t = term_rect();
    bool in_cells = cell_at(wx, wy, &row, &c);
    if (selecting_) {
        // allow dragging beyond the term rect (clamp) and autoscroll
        if (!in_cells) {
            row = (int)std::floor((wy - t.y) / cell_h());
            c = (int)std::floor((wx - t.x) / cell_w());
            c = std::max(0, std::min(cols - 1, c));
            if (row < 0) { scroll_lines(-1); row = 0; }
            else if (row >= rows) { scroll_lines(1); row = rows - 1; }
        }
        vis();
        int64_t vi = top_ + row;
        if (vi >= 0 && vi < (int64_t)vis_.size() && (vis_[vi].kind == VisRow::Term || vis_[vi].kind == VisRow::Restored)) extend_selection(vis_[vi].abs, c);
        app_.request_redraw();
        return;
    }
    if (mouse_to_app() && !(mods & MOD_SHIFT) && in_cells) {
        MouseMode mm = session->term().modes().mouse;
        if (mm == MouseMode::AnyEvent || (mm == MouseMode::ButtonEvent && held != MouseButton::None)) {
            MouseEvent me; me.action = MouseAction::Motion; me.button = held; me.col = c; me.row = row; me.mods = mods;
            me.px = (int)(wx - t.x); me.py = (int)(wy - t.y);
            send_mouse(me);
        }
    }
}

void Pane::wheel(float wx, float wy, float dx, float dy, uint32_t mods) {
    if (drawer_open() && !minimized() && drawer_rect().contains(wx, wy)) {
        ensure_artifact_loaded();
        float line_h = cell_h() + 2.f;
        float total_h = (float)md_lines_.size() * line_h + 12.f;
        float max_scroll = std::max(0.f, total_h - drawer_body_rect().h);
        drawer_scroll_ = clampf(drawer_scroll_ - dy * 3.f * line_h, 0.f, max_scroll);
        app_.request_redraw();
        return;
    }
    if (session && session->term().modes().mouse != MouseMode::None && live() && !(mods & MOD_SHIFT)) {
        int row, c;
        if (!cell_at(wx, wy, &row, &c)) return;
        int n = std::max(1, (int)std::lround(std::fabs(dy)));
        for (int i = 0; i < n; i++) {
            MouseEvent me; me.action = MouseAction::Press; me.button = dy > 0 ? MouseButton::WheelUp : MouseButton::WheelDown; me.col = c; me.row = row; me.mods = mods;
            send_mouse(me);
        }
        if (dx != 0) {
            MouseEvent me; me.action = MouseAction::Press; me.button = dx > 0 ? MouseButton::WheelRight : MouseButton::WheelLeft; me.col = c; me.row = row; me.mods = mods;
            send_mouse(me);
        }
        return;
    }
    if (session && session->term().modes().alt_screen && live()) {
        // Emulate wheel as arrow keys for full-screen apps without mouse support (less, man...)
        int n = std::max(1, (int)std::lround(std::fabs(dy))) * 3;
        KeyEvent ke; ke.key = dy > 0 ? Key::Up : Key::Down;
        std::string s = encode_key(ke, session->term().modes());
        for (int i = 0; i < n; i++) write(s);
        return;
    }
    scroll_lines((int)std::lround(-dy * 3));
    app_.request_redraw();
}

bool Pane::key(const KeyEvent& ev) {
    if (!session || !session->alive()) {
        if (ev.key == Key::Enter) { app_.ensure_session(*this); return true; }
        return false;
    }
    const TerminalModes& m = session->term().modes();
    if ((ev.mods & MOD_SHIFT) && !m.alt_screen) {
        if (ev.key == Key::PageUp) { scroll_pages(-1); return true; }
        if (ev.key == Key::PageDown) { scroll_pages(1); return true; }
        if (ev.key == Key::Home && (ev.mods & MOD_CTRL)) { top_ = 0; follow_ = false; clamp_scroll(); return true; }
        if (ev.key == Key::End && (ev.mods & MOD_CTRL)) { scroll_to_bottom(); return true; }
    }
    std::string s = encode_key(ev, m);
    if (s.empty()) return false;
    write(s);
    scroll_to_bottom();
    return true;
}

void Pane::text_input(const std::string& utf8) {
    if (!live()) return;
    write(utf8);
    scroll_to_bottom();
}

void Pane::paste(const std::string& text) {
    if (!live() || text.empty()) return;
    write(encode_paste(text, session->term().modes()));
    scroll_to_bottom();
}

// ---------------------------------------------------------------- hover
void Pane::set_hover(float wx, float wy) {
    hover = {};
    if (drawer_open() && !minimized() && drawer_rect().contains(wx, wy)) {
        hover.valid = true;
        hover.drawer = true;
        hover.drawer_close = drawer_close_rect().contains(wx, wy);
        hover.drawer_prev = drawer_prev_rect().contains(wx, wy);
        hover.drawer_next = drawer_next_rect().contains(wx, wy);
        hover.drawer_proceed = drawer_proceed_rect().contains(wx, wy);
        hover.drawer_feedback = drawer_feedback_rect().contains(wx, wy);
        hover.drawer_edit = drawer_edit_rect().contains(wx, wy);
        return;
    }
    Rect R = rect();
    if (!R.contains(wx, wy)) return;
    hover.valid = true;
    if (!minimized() && resize_handle().contains(wx, wy)) { hover.handle = true; return; }
    if (title_rect().contains(wx, wy)) {
        hover.title = true;
        hover.artifact_chip = artifact_chip_rect().contains(wx, wy);
        hover.min_btn = min_rect().contains(wx, wy);
        hover.close_btn = close_rect().contains(wx, wy);
        return;
    }
    if (minimized()) return;
    if (lines_below() > 0 && scroll_pill_rect().contains(wx, wy)) { hover.scroll_pill = true; return; }
    int row, c;
    if (!cell_at(wx, wy, &row, &c)) return;
    hover.row = row; hover.col = c;
    hover.gutter = wx < term_rect().x;
    vis();
    int64_t vi = top_ + row;
    if (vi < 0 || vi >= (int64_t)vis_.size()) return;
    const VisRow& vr = vis_[vi];
    hover.block = vr.block;
    hover.summary = vr.kind == VisRow::Summary;
    if (vr.kind != VisRow::Term || !session) return;
    hover.abs = vr.abs;
    const Line& ln = session->term().line_abs(vr.abs);
    if (c < ln.width() && ln.cells[c].hyperlink) {
        hover.link = session->term().hyperlink(ln.cells[c].hyperlink);
        if (hover.link) hover.link_uri = hover.link->uri;
    }
    // chip hit test: right-aligned chip on the command row
    const Block* bk = block(vr.block);
    if (bk && bk->cmd_line == vr.abs && bk->state != BlockState::Prompt && bk->state != BlockState::Typing) {
        if (c >= cols - 14) hover.chip = true;
    }
    if (bk && bk->state == BlockState::Done && !hover.gutter) {
        auto it = views_.find(bk->id);
        if (it != views_.end() && it->second.table.is_table && it->second.table.header_row >= 0 &&
            vr.abs == bk->output_line + it->second.table.header_row) {
            hover.header = true;
            const auto& cb = it->second.table.col_begins;
            for (size_t k = 0; k < cb.size(); k++) if (c >= cb[k] && (k + 1 >= cb.size() || c < cb[k + 1])) hover.table_col = (int)k;
        }
    }
    if (!hover.gutter && app_.config().structured_output) {
        std::string txt = ln.text();
        if (!txt.empty()) {
            auto spans = scan_line(txt, cwd(), true);
            for (auto& sp : spans) if (c >= sp.col_begin && c < sp.col_end && sp.kind != SpanKind::Error && sp.kind != SpanKind::Warning) { hover.span = sp; break; }
        }
    }
}

int Pane::system_cursor() const {
    if (hover.handle) return SDL_SYSTEM_CURSOR_SIZENWSE;
    if (hover.artifact_chip || hover.min_btn || hover.close_btn || hover.scroll_pill) return SDL_SYSTEM_CURSOR_HAND;
    if (hover.drawer_close || hover.drawer_prev || hover.drawer_next || hover.drawer_proceed || hover.drawer_feedback || hover.drawer_edit) return SDL_SYSTEM_CURSOR_HAND;
    if (hover.drawer || hover.title) return SDL_SYSTEM_CURSOR_ARROW;
    if (minimized() && hover.valid) return SDL_SYSTEM_CURSOR_HAND;
    if (hover.gutter || hover.summary || hover.chip || hover.header) return SDL_SYSTEM_CURSOR_HAND;
    if (hover.link || hover.span) return SDL_SYSTEM_CURSOR_HAND;
    if (hover.valid && hover.row >= 0) return SDL_SYSTEM_CURSOR_IBEAM;
    return SDL_SYSTEM_CURSOR_ARROW;
}

std::string Pane::status_text() const {
    if (!session) return has_restored() ? "dormant" : "not started";
    if (!session->alive()) return "exited";
    if (agent_state_ == AgentState::NeedsApproval) return "⚡ needs approval";
    if (agent_state_ == AgentState::WaitingInput) return "⏸ waiting for input";
    if (bell_unread_ && !notify_msg_.empty()) return "🔔 " + ellipsize(notify_msg_, 22);
    const auto& bl = session->blocks().blocks();
    for (size_t i = bl.size(); i-- > 0;) {
        const Block& b = bl[i];
        if (b.state == BlockState::Running) return "⟳ " + ellipsize(b.command, 24);
        if (b.state == BlockState::Done && !b.command.empty()) return (b.exit_code == 0 ? "✓ " : "✗ ") + ellipsize(b.command, 24);
    }
    return "idle";
}

// ---------------------------------------------------------------- drawing
void Pane::draw(Renderer& r, Font& font, const DrawCtx& ctx) {
    const Theme& th = app_.theme();
    Rect R = rect();
    float s = ctx.scale;
    RGBA bg = col(th.background);
    RGBA chrome = col(th.chrome_bg);
    // shadow
    r.rounded_rect({R.x + 2, R.y + 4, R.w, R.h}, RADIUS, {0, 0, 0, 0.35f});
    r.rounded_rect(R, RADIUS, chrome);
    Rect body = {R.x, R.y + TITLE_H, R.w, R.h - TITLE_H};
    r.rounded_rect(body, RADIUS, bg);
    r.fill_rect({body.x, body.y, body.w, RADIUS}, bg);

    if (s * cell_h() < 2.5f) {
        // Far zoom: card with the name only
        draw_title(r, font, ctx);
        draw_summary_banner(r, font, ctx);
        r.rounded_rect_stroke(R, RADIUS, ctx.focused ? col(th.accent) : alpha(col(th.chrome_fg), 0.35f), ctx.focused ? 2.f : 1.f);
        return;
    }

    draw_title(r, font, ctx);
    if (minimized() && !ctx.in_rail) {
        std::string sum = summary.empty() ? local_summary() : summary;
        r.push_clip({R.x + 12, R.y + TITLE_H, R.w - 24, R.h - TITLE_H});
        r.draw_text(font, R.x + 14, R.y + TITLE_H + PAD + font.metrics().ascent, sum, alpha(col(th.foreground), summary_ai ? 0.9f : 0.7f), summary_ai ? gfx::STYLE_REGULAR : gfx::STYLE_ITALIC);
        r.pop_clip();
        RGBA border = ctx.focused ? col(th.accent) : alpha(col(th.chrome_fg), 0.3f);
        r.rounded_rect_stroke(R, RADIUS, border, ctx.focused ? 1.5f : 1.f);
        return;
    }
    const auto& v = vis();
    Rect T = term_rect();
    Rect clip = {R.x + 1, T.y - 2, R.w - 2, T.h + 4};
    r.push_clip(clip);
    int count = 0;
    for (int i = 0; i < rows; i++) {
        int64_t vi = top_ + i;
        if (vi >= (int64_t)v.size()) break;
        const VisRow& vr = v[vi];
        float y0 = T.y + i * cell_h();
        if (vr.kind == VisRow::Summary) draw_summary(r, font, ctx, vr, y0);
        else if (const Line* ln = line_for(vr)) draw_line(r, font, *ln, T.x, y0, ctx, vr, i);
        count++;
    }
    draw_gutter(r, font, ctx, (int)top_, count);
    if (s >= 0.45f) {
        // chips: on the command row if there is room at the right edge, else on the last output row
        int chip_cols = 16;
        auto row_free = [&](int i) {
            const VisRow& vr = v[top_ + i];
            const Line* ln = vr.kind == VisRow::Summary ? nullptr : line_for(vr);
            if (!ln) return vr.kind == VisRow::Summary ? false : true;
            for (int c = std::max(0, cols - chip_cols); c < std::min(cols, ln->width()); c++)
                if (!ln->cells[c].is_blank()) return false;
            return true;
        };
        for (int i = 0; i < count; i++) {
            const VisRow& vr = v[top_ + i];
            if (vr.kind != VisRow::Term || vr.block < 0) continue;
            const Block* b = block(vr.block);
            if (!b || b->cmd_line != vr.abs || (b->state != BlockState::Running && b->state != BlockState::Done)) continue;
            int pick = -1;
            if (row_free(i)) pick = i;
            else {
                int j = i;
                while (j + 1 < count && v[top_ + j + 1].block == vr.block && v[top_ + j + 1].kind != VisRow::Summary) j++;
                for (int k = i + 1; k <= j; k++) if (row_free(k)) { pick = k; break; }
            }
            if (pick >= 0) draw_chip(r, font, ctx, *b, T.y + pick * cell_h());
        }
    }
    draw_cursor(r, ctx);
    r.pop_clip();
    draw_scrollbar(r, ctx);
    if (lines_below() > 0 && session && session->alive() && !ctx.in_rail && s >= 0.45f) draw_scroll_pill(r, font, ctx);
    if (!session || !session->alive()) draw_dormant(r, font, ctx);
    if ((s < 0.6f || ctx.overview) && !ctx.in_rail) draw_summary_banner(r, font, ctx);
    // border
    RGBA border = ctx.focused ? col(th.accent) : (waiting_for_user() ? alpha(col(th.warning), 0.75f) : alpha(col(th.chrome_fg), 0.3f));
    r.rounded_rect_stroke(R, RADIUS, border, (ctx.focused || waiting_for_user()) ? 1.5f : 1.f);
    // resize handle
    if (s > 0.5f) {
        Rect h = resize_handle();
        RGBA hc = alpha(col(th.chrome_fg), hover.handle ? 0.9f : 0.35f);
        r.line(h.x + h.w - 3, h.y + h.h - 9, h.x + h.w - 9, h.y + h.h - 3, hc, 1.f);
        r.line(h.x + h.w - 3, h.y + h.h - 5, h.x + h.w - 5, h.y + h.h - 3, hc, 1.f);
    }
    if (drawer_open() && !minimized() && !ctx.in_rail) {
        draw_artifact_drawer(r, font, ctx);
    }
}

void Pane::draw_title(Renderer& r, Font& font, const DrawCtx& ctx) {
    const Theme& th = app_.theme();
    const Activity* a = app_.tree().get(activity_id);
    Rect t = title_rect();
    float s = ctx.scale;
    float base = t.y + TITLE_H / 2 + font.metrics().ascent / 2.f - 1;
    std::string name = a ? a->name : "?";
    RGBA fg = col(th.chrome_fg);
    // status dot
    RGBA dot = alpha(fg, 0.4f);
    bool running = false;
    bool waiting = waiting_for_user();
    if (session && session->alive()) {
        const auto& bl = session->blocks().blocks();
        if (waiting) { dot = col(th.warning); }
        else if (!bl.empty() && bl.back().state == BlockState::Running) { running = true; dot = col(th.accent); }
        else if (!bl.empty() && bl.back().state == BlockState::Done) dot = bl.back().exit_code == 0 ? col(th.ok) : col(th.error);
        else { for (size_t i = bl.size(); i-- > 0;) if (bl[i].state == BlockState::Done) { dot = bl[i].exit_code == 0 ? col(th.ok) : col(th.error); break; } }
    } else if (session) dot = col(th.error);
    float x0 = t.x + 12;
    if (s * cell_h() < 2.5f) {
        // big label when zoomed far out
        float sc = std::min(4.f, 2.5f / s);
        r.circle(x0 + 6 * sc, t.y + TITLE_H / 2, 5 * sc, dot);
        r.draw_text(font, x0 + 16 * sc, t.y + TITLE_H / 2 + font.metrics().ascent * sc / 2.f, name, col(th.foreground), gfx::STYLE_BOLD, sc);
        return;
    }
    r.circle(x0 + 5, t.y + TITLE_H / 2, (running || waiting) ? 5 : 4, dot);
    if (running || waiting) r.circle(x0 + 5, t.y + TITLE_H / 2, 7 + 2 * (float)std::sin(ctx.now * 6), alpha(dot, 0.25f));
    float x = x0 + 18;
    x += r.draw_text(font, x, base, name, ctx.focused ? col(th.foreground) : fg, gfx::STYLE_BOLD) + 10;
    std::string sub = pretty_path(cwd(), 40);
    if (!last_title.empty() && last_title != name && last_title.find(sub) == std::string::npos) sub += "  ·  " + ellipsize(last_title, 40);
    Rect cb = close_rect();
    Rect mb = min_rect();
    Rect ac = artifact_chip_rect();
    if (!ctx.in_rail) {
        if (!artifacts_.empty()) {
            const AgyArtifact* art = active_artifact();
            std::string stem = art ? art->name : "artifacts";
            if (stem.size() > 3 && stem.compare(stem.size() - 3, 3, ".md") == 0) stem.resize(stem.size() - 3);
            stem = ellipsize(stem, 15);
            std::string label = (art && art->request_feedback ? "📋 " : "📄 ") + stem;
            if (artifacts_.size() > 1) label += " (" + std::to_string(artifacts_.size()) + ")";
            bool hot_ac = hover.artifact_chip || drawer_open();
            bool need_rev = art && art->request_feedback && artifact_unread_;
            RGBA ac_c = need_rev ? col(th.warning) : col(th.accent);
            r.rounded_rect(ac, 5.f, alpha(ac_c, hot_ac ? 0.35f : (artifact_unread_ ? 0.25f : 0.14f)));
            r.rounded_rect_stroke(ac, 5.f, alpha(ac_c, hot_ac || artifact_unread_ ? 0.85f : 0.45f), 1.f);
            float lw = r.measure_text(font, label, (drawer_open() || artifact_unread_) ? gfx::STYLE_BOLD : gfx::STYLE_REGULAR);
            r.push_clip(ac);
            r.draw_text(font, ac.x + std::max(4.f, (ac.w - lw) / 2.f), base, label,
                        need_rev ? col(th.warning) : (hot_ac ? col(th.foreground) : ac_c),
                        (drawer_open() || artifact_unread_) ? gfx::STYLE_BOLD : gfx::STYLE_REGULAR);
            r.pop_clip();
        }

        bool hot_min = hover.min_btn;
        RGBA mbg = hot_min ? alpha(col(th.accent), 0.65f) : alpha(fg, ctx.focused ? 0.14f : 0.08f);
        RGBA mfg = hot_min ? col(th.foreground) : alpha(fg, ctx.focused ? 0.85f : 0.55f);
        r.rounded_rect(mb, 5.f, mbg);
        std::string mstr = minimized() ? "+" : "−";
        float mw = r.measure_text(font, mstr, gfx::STYLE_BOLD);
        r.draw_text(font, mb.x + (mb.w - mw) / 2.f, base, mstr, mfg, gfx::STYLE_BOLD);

        bool hot = hover.close_btn;
        RGBA cbg = hot ? alpha(col(th.error), 0.85f) : alpha(fg, ctx.focused ? 0.14f : 0.08f);
        RGBA cfg_ = hot ? col(th.foreground) : alpha(fg, ctx.focused ? 0.85f : 0.55f);
        r.rounded_rect(cb, 5.f, cbg);
        std::string xstr = "×";
        float xw = r.measure_text(font, xstr, gfx::STYLE_BOLD);
        r.draw_text(font, cb.x + (cb.w - xw) / 2.f, base, xstr, cfg_, gfx::STYLE_BOLD);
    }
    float right = ctx.in_rail ? (t.x + t.w - 12) : ((artifacts_.empty() ? mb.x : ac.x) - 8);
    float avail = right - x - 90;
    if (avail > 40) {
        r.push_clip({x, t.y, avail, TITLE_H});
        r.draw_text(font, x, base, sub, alpha(fg, 0.7f));
        r.pop_clip();
    }
    // right side: agent status, running time, or unread badge
    std::string rt;
    RGBA rt_col = col(th.accent);
    if (agent_state_ == AgentState::NeedsApproval) {
        rt = "⚡ approve";
        rt_col = col(th.warning);
    } else if (agent_state_ == AgentState::WaitingInput) {
        rt = "⏸ input";
        rt_col = col(th.warning);
    } else if (bell_unread_) {
        rt = "🔔 notify";
        rt_col = col(th.warning);
    } else if (running) {
        const Block& b = session->blocks().blocks().back();
        rt = std::string(SPINNER[(int)(ctx.now * 8) % 4]) + " " + fmt_duration(ctx.now - b.start_time);
        rt_col = col(th.accent);
    } else if (unread && !ctx.focused) {
        rt = "● new";
        rt_col = col(th.warning);
    }
    if (!rt.empty()) {
        float w = r.measure_text(font, rt, waiting ? gfx::STYLE_BOLD : gfx::STYLE_REGULAR);
        r.draw_text(font, right - w, base, rt, rt_col, waiting ? gfx::STYLE_BOLD : gfx::STYLE_REGULAR);
    }
    if (a && !a->notes.empty() && ctx.scale > 0.6f) {
        float w = r.measure_text(font, rt);
        std::string note = "✎";
        r.draw_text(font, right - w - 22, base, note, alpha(fg, 0.7f));
    }
}

void Pane::draw_line(Renderer& r, Font& font, const Line& ln, float x0, float y0, const DrawCtx& ctx, const VisRow& vr, int disp_row) {
    const Config& cfg = app_.config();
    const Theme& th = app_.theme();
    float cw = cell_w(), ch = cell_h();
    const gfx::FontMetrics& fm = font.metrics();
    float base = y0 + fm.ascent;
    bool lod = ctx.scale * ch < 5.f;
    RGBA selc = col(th.selection);
    RGBA defbg = col(th.background);
    bool rv = session && session->term().modes().reverse_video;
    bool selectable = (vr.kind == VisRow::Term || vr.kind == VisRow::Restored);
    // search matches on this row
    const Match* mstart = nullptr; size_t mcount = 0;
    if (!matches_.empty() && selectable) {
        auto it = std::lower_bound(matches_.begin(), matches_.end(), vr.abs, [](const Match& m, int64_t a) { return m.abs < a; });
        if (it != matches_.end() && it->abs == vr.abs) { mstart = &*it; while (it != matches_.end() && it->abs == vr.abs) { it++; mcount++; } }
    }
    int cur_match_col = -1, cur_match_len = 0;
    if (match_idx_ >= 0 && match_idx_ < (int)matches_.size() && matches_[match_idx_].abs == vr.abs && selectable) { cur_match_col = matches_[match_idx_].col; cur_match_len = matches_[match_idx_].len; }
    // table header / sorted column tint
    int tint_col_begin = -1, tint_col_end = -1; bool header = false;
    const Block* bk = block(vr.block);
    if (bk && vr.kind == VisRow::Term && bk->state == BlockState::Done) {
        auto it = views_.find(bk->id);
        if (it != views_.end() && it->second.table.is_table) {
            const BlockView& v = it->second;
            header = v.table.header_row >= 0 && vr.abs == bk->output_line + v.table.header_row;
            int tc = v.sort_col >= 0 ? v.sort_col : (hover.header && hover.block == vr.block ? hover.table_col : -1);
            if (tc >= 0 && tc < (int)v.table.col_begins.size() && vr.abs >= bk->output_line && vr.abs < bk->end_line) {
                tint_col_begin = v.table.col_begins[tc];
                tint_col_end = tc + 1 < (int)v.table.col_begins.size() ? v.table.col_begins[tc + 1] : cols;
            }
        }
    }
    bool is_err = false;
    if (bk && vr.kind == VisRow::Term) {
        auto it = views_.find(bk->id);
        if (it != views_.end()) is_err = std::binary_search(it->second.error_lines.begin(), it->second.error_lines.end(), vr.abs);
    }
    if (is_err) r.fill_rect({x0 - 2, y0, cw * cols + 4, ch}, alpha(col(th.error), 0.08f));
    if (header) r.fill_rect({x0, y0, cw * cols, ch}, alpha(col(th.accent), 0.10f));
    if (tint_col_begin >= 0) r.fill_rect({x0 + tint_col_begin * cw, y0, (tint_col_end - tint_col_begin) * cw, ch}, alpha(col(th.accent), 0.07f));

    // pass 1: backgrounds (merged runs)
    int n = std::min(cols, ln.width());
    int run_start = 0; RGBA run_col = defbg; bool run_draw = false;
    auto flush_bg = [&](int end) {
        if (run_draw && end > run_start) r.fill_rect({x0 + run_start * cw, y0, (end - run_start) * cw, ch}, run_col);
    };
    for (int c = 0; c < cols; c++) {
        RGBA bgc = defbg; bool draw = false;
        if (c < n) {
            const Cell& cell = ln.cells[c];
            bool inv = (cell.attrs & ATTR_INVERSE) != 0;
            if (rv) inv = !inv;
            Color bgcol = inv ? cell.fg : cell.bg;
            if (inv || bgcol.kind != Color::Default) {
                RGB8 rgb = cfg.resolve(bgcol, inv);
                if (inv && bgcol.kind == Color::Default) rgb = th.foreground;
                bgc = col(rgb); draw = true;
            }
        }
        if (selectable && selected(vr.abs, c)) { bgc = selc; draw = true; }
        if (mstart) for (size_t k = 0; k < mcount; k++) if (c >= mstart[k].col && c < mstart[k].col + mstart[k].len) { bgc = alpha(col(th.warning), 0.55f); draw = true; }
        if (cur_match_col >= 0 && c >= cur_match_col && c < cur_match_col + cur_match_len) { bgc = col(th.warning); draw = true; }
        if (c == 0) { run_start = 0; run_col = bgc; run_draw = draw; continue; }
        if (draw != run_draw || (draw && (bgc.r != run_col.r || bgc.g != run_col.g || bgc.b != run_col.b || bgc.a != run_col.a))) {
            flush_bg(c); run_start = c; run_col = bgc; run_draw = draw;
        }
    }
    flush_bg(cols);

    // pass 2: glyphs
    for (int c = 0; c < n; c++) {
        const Cell& cell = ln.cells[c];
        if (cell.attrs & ATTR_WIDE_CONT) continue;
        bool wide = cell.attrs & ATTR_WIDE;
        bool inv = (cell.attrs & ATTR_INVERSE) != 0; if (rv) inv = !inv;
        Color fgcol = inv ? cell.bg : cell.fg;
        RGB8 rgb;
        if (inv && fgcol.kind == Color::Default) rgb = th.background;
        else {
            Color fc = fgcol;
            if ((cell.attrs & ATTR_BOLD) && fc.kind == Color::Indexed && fc.idx < 8) fc.idx += 8;
            rgb = cfg.resolve(fc, !inv);
        }
        RGBA fg = col(rgb);
        if (cell.attrs & ATTR_DIM) fg.a *= 0.55f;
        if (selectable && selected(vr.abs, c)) fg = col(th.foreground);
        float cx = x0 + c * cw;
        bool blank = cell.cp == 0 || cell.cp == U' ';
        if (lod) {
            if (!blank && !(cell.attrs & ATTR_INVISIBLE)) r.fill_rect({cx, y0 + ch * 0.3f, cw * (wide ? 2 : 1), ch * 0.4f}, alpha(fg, 0.55f));
            continue;
        }
        if (!blank && !(cell.attrs & ATTR_INVISIBLE)) {
            if (Font::is_box_drawing(cell.cp)) {
                r.draw_box_char(cell.cp, {cx, y0, cw * (wide ? 2 : 1), ch}, fg, std::max(1.f, std::round(cell_h() / 14.f)));
            } else {
                gfx::GlyphStyle st = (gfx::GlyphStyle)(((cell.attrs & ATTR_BOLD) ? 1 : 0) | ((cell.attrs & ATTR_ITALIC) ? 2 : 0));
                const gfx::Glyph& g = font.glyph(cell.cp, st);
                float gx = cx;
                if (wide && g.aw < cw * 2) gx = cx + (cw * 2 - g.advance) / 2 - (g.advance > cw ? 0 : 0);
                r.draw_glyph(font, g, gx, base, fg);
                if (cell.combining && cell.combining <= ln.combining.size()) {
                    for (char32_t cc : ln.combining[cell.combining - 1]) {
                        const gfx::Glyph& cg = font.glyph(cc, st);
                        r.draw_glyph(font, cg, cx, base, fg);
                    }
                }
            }
        }
        // decorations
        float w = cw * (wide ? 2 : 1);
        bool ul = (cell.attrs & ATTR_UNDERLINE) || cell.ul_style != UnderlineStyle::None;
        bool hover_ul = hover.valid && hover.abs == vr.abs && vr.kind == VisRow::Term &&
                        ((hover.link && cell.hyperlink && session && session->term().hyperlink(cell.hyperlink) == hover.link) ||
                         (hover.span && c >= hover.span->col_begin && c < hover.span->col_end));
        if (ul || hover_ul) {
            RGBA uc = fg;
            if (cell.ul.kind != Color::Default) uc = col(cfg.resolve(cell.ul, true));
            if (hover_ul) uc = col(th.accent);
            float uy = y0 + fm.underline_pos;   // measured from the cell top
            float th_ = std::max(1.f, (float)fm.underline_thickness);
            UnderlineStyle us = cell.ul_style == UnderlineStyle::None ? UnderlineStyle::Single : cell.ul_style;
            if (hover_ul && !ul) us = UnderlineStyle::Dotted;
            switch (us) {
            case UnderlineStyle::Double: r.fill_rect({cx, uy - th_, w, th_}, uc); r.fill_rect({cx, uy + th_ + 1, w, th_}, uc); break;
            case UnderlineStyle::Curly: {
                int segs = 4; float sw = w / segs;
                for (int k = 0; k < segs; k++) {
                    float ya = (k % 2 == 0) ? uy - 1 : uy + 1.5f, yb = (k % 2 == 0) ? uy + 1.5f : uy - 1;
                    r.line(cx + k * sw, ya, cx + (k + 1) * sw, yb, uc, th_);
                }
                break;
            }
            case UnderlineStyle::Dotted: for (float dx = 0; dx < w; dx += 3) r.fill_rect({cx + dx, uy, std::min(1.5f, w - dx), th_}, uc); break;
            case UnderlineStyle::Dashed: r.fill_rect({cx, uy, w * 0.6f, th_}, uc); break;
            default: r.fill_rect({cx, uy, w, th_}, uc); break;
            }
        }
        if (cell.attrs & ATTR_STRIKE) r.fill_rect({cx, y0 + fm.strike_pos, w, 1.f}, fg);
    }
    if (header && !lod) {
        r.fill_rect({x0, y0 + ch - 1, cw * cols, 1}, alpha(col(th.accent), 0.6f));
        // sort indicator
        auto it = views_.find(bk->id);
        if (it != views_.end() && it->second.sort_col >= 0) {
            int sc = it->second.table.col_begins[it->second.sort_col];
            float tx = x0 + sc * cw - cw * 0.9f;
            r.draw_text(font, tx, base, it->second.sort_desc ? "▼" : "▲", col(th.accent), gfx::STYLE_REGULAR, 0.7f);
        }
    }
}

void Pane::draw_summary(Renderer& r, Font& font, const DrawCtx& ctx, const VisRow& vr, float y0) {
    const Theme& th = app_.theme();
    const Block* b = block(vr.block);
    if (!b) return;
    Rect T = term_rect();
    float ch = cell_h();
    r.fill_rect({T.x, y0, T.w, ch}, alpha(col(th.chrome_fg), 0.08f));
    if (ctx.scale * ch < 5.f) return;
    auto it = views_.find(b->id);
    std::string s = "▸ " + std::to_string((long long)b->output_lines()) + " lines hidden";
    if (it != views_.end() && it->second.error_count) s += "  ·  " + std::to_string(it->second.error_count) + " error" + (it->second.error_count > 1 ? "s" : "");
    s += "  ·  exit " + std::to_string(b->exit_code) + "  ·  " + fmt_duration(b->duration());
    float base = y0 + font.metrics().ascent;
    r.draw_text(font, T.x + cell_w(), base, s, alpha(col(th.chrome_fg), 0.9f), gfx::STYLE_ITALIC);
}

void Pane::draw_chip(Renderer& r, Font& font, const DrawCtx& ctx, const Block& b, float y0) {
    const Theme& th = app_.theme();
    Rect T = term_rect();
    float ch = cell_h();
    std::string s; RGBA c;
    if (b.state == BlockState::Running) { s = std::string(SPINNER[(int)(ctx.now * 8) % 4]) + " " + fmt_duration(ctx.now - b.start_time); c = col(th.accent); }
    else {
        if (b.exit_code == 0) { s = "✓"; c = col(th.ok); } else { s = "✗ " + std::to_string(b.exit_code); c = col(th.error); }
        s += " " + fmt_duration(b.duration());
        auto it = views_.find(b.id);
        if (it != views_.end()) {
            if (it->second.error_count) s += " · " + std::to_string(it->second.error_count) + "⚠";
            if (it->second.is_json) s += " · {}";
            else if (it->second.table.is_table) s += " · ▦";
        }
        if (b.folded) s += " ▸";
        if (b.pinned) s = "📌 " + s;
    }
    float w = r.measure_text(font, s) + 10;
    float x = T.x + T.w - w - 4;
    Rect chip = {x, y0 + 1, w, ch - 2};
    bool hot = hover.chip && hover.block >= 0 && block(hover.block) == &b;
    r.rounded_rect(chip, (ch - 2) / 2, alpha(hot ? c : col(th.chrome_bg), hot ? 0.35f : 0.85f));
    r.rounded_rect_stroke(chip, (ch - 2) / 2, alpha(c, 0.6f), 1.f);
    r.draw_text(font, x + 5, y0 + font.metrics().ascent, s, c);
}

void Pane::draw_gutter(Renderer& r, Font& font, const DrawCtx& ctx, int first, int count) {
    const Theme& th = app_.theme();
    Rect G = gutter_rect();
    float ch = cell_h();
    float bx = G.x + 7;
    const auto& v = vis_;
    int i = 0;
    while (i < count) {
        const VisRow& vr = v[first + i];
        int bi = vr.block;
        if (bi < 0 || vr.kind == VisRow::Restored) { i++; continue; }
        int j = i;
        while (j < count && v[first + j].block == bi && v[first + j].kind != VisRow::Restored) j++;
        const Block* b = block(bi);
        if (b) {
            RGBA c = alpha(col(th.chrome_fg), 0.35f);
            if (b->state == BlockState::Running) c = col(th.accent);
            else if (b->state == BlockState::Done) c = b->exit_code == 0 ? col(th.ok) : col(th.error);
            bool sel = bi == selected_block;
            bool hot = hover.gutter && hover.block == bi;
            float y0 = G.y + i * ch, y1 = G.y + j * ch;
            if (sel) r.fill_rect({G.x, y0, G.w + term_rect().w + PAD, y1 - y0}, alpha(col(th.accent), 0.06f));
            r.rounded_rect({bx - (sel ? 2.f : 1.f), y0 + 1, sel ? 5.f : 3.f, y1 - y0 - 2}, 1.5f, alpha(c, hot || sel ? 1.f : 0.7f));
            // command marker
            for (int k = i; k < j; k++) {
                const VisRow& rr = v[first + k];
                if (rr.kind == VisRow::Term && rr.abs == b->cmd_line) {
                    r.circle(bx + 0.5f, G.y + k * ch + ch / 2, ctx.scale > 0.5f ? 4.5f : 3.f, c);
                    if (b->pinned) r.circle(bx + 0.5f, G.y + k * ch + ch / 2, 2.f, col(th.background));
                }
            }
            // error ticks
            auto it = views_.find(b->id);
            if (it != views_.end() && !it->second.error_lines.empty()) {
                for (int k = i; k < j; k++) {
                    const VisRow& rr = v[first + k];
                    if (rr.kind == VisRow::Term && std::binary_search(it->second.error_lines.begin(), it->second.error_lines.end(), rr.abs))
                        r.fill_rect({bx + 6, G.y + k * ch + 2, 3, ch - 4}, col(th.error));
                }
            }
        }
        i = j;
    }
    // restored history marker
    for (int k = 0; k < count; k++) {
        const VisRow& rr = v[first + k];
        if (rr.kind == VisRow::Restored) r.fill_rect({bx, G.y + k * ch, 2, ch}, alpha(col(th.chrome_fg), 0.15f));
    }
}

void Pane::draw_cursor(Renderer& r, const DrawCtx& ctx) {
    if (!session || !session->alive()) return;
    const Terminal& t = session->term();
    Cursor cu = t.cursor();
    if (!cu.visible || !t.modes().cursor_visible) return;
    int64_t abs = t.screen_base() + cu.y;
    int disp;
    if (t.modes().alt_screen) disp = cu.y;
    else { int rr = row_of_abs(abs); if (rr < 0) return; disp = rr - (int)top_; }
    if (disp < 0 || disp >= rows) return;
    Rect T = term_rect();
    float cw = cell_w(), ch = cell_h();
    int cx = std::min(cu.x, cols - 1);
    Rect cr = {T.x + cx * cw, T.y + disp * ch, cw, ch};
    const Line& ln = t.row(cu.y);
    if (cx < ln.width() && (ln.cells[cx].attrs & ATTR_WIDE)) cr.w *= 2;
    RGBA cc = col(app_.theme().cursor);
    bool blink_on = ctx.cursor_on || !app_.config().cursor_blink || !cu.blink;
    if (!ctx.focused || !ctx.window_focused) { r.stroke_rect(cr, alpha(cc, 0.8f), 1.f); return; }
    if (!blink_on) return;
    CursorStyle st = cu.style;
    switch (st) {
    case CursorStyle::Bar: r.fill_rect({cr.x, cr.y, 2.f, cr.h}, cc); break;
    case CursorStyle::Underline: r.fill_rect({cr.x, cr.y + cr.h - 2, cr.w, 2.f}, cc); break;
    default: {
        r.fill_rect(cr, cc);
        // redraw glyph in background color on top
        if (cx < ln.width()) {
            const Cell& cell = ln.cells[cx];
            if (cell.cp && cell.cp != U' ' && !Font::is_box_drawing(cell.cp)) {
                gfx::GlyphStyle gs = (gfx::GlyphStyle)(((cell.attrs & ATTR_BOLD) ? 1 : 0) | ((cell.attrs & ATTR_ITALIC) ? 2 : 0));
                const gfx::Glyph& g = app_.font().glyph(cell.cp, gs);
                r.draw_glyph(app_.font(), g, cr.x, cr.y + app_.font().metrics().ascent, col(app_.theme().background));
            }
        }
    }
    }
}

void Pane::draw_scrollbar(Renderer& r, const DrawCtx& ctx) {
    if (vis_.size() <= (size_t)rows) return;
    Rect T = term_rect();
    float total = (float)vis_.size();
    float frac = rows / total;
    float h = std::max(14.f, T.h * frac);
    float yy = T.y + (T.h - h) * (top_ / std::max(1.f, total - rows));
    RGBA c = alpha(col(app_.theme().chrome_fg), follow_ ? 0.15f : 0.45f);
    r.rounded_rect({T.x + T.w - 4, yy, 3, h}, 1.5f, c);
    // block markers along the scrollbar (errors)
    if (session) {
        for (auto& kv : views_) {
            for (int64_t a : kv.second.error_lines) {
                int rr = row_of_abs(a);
                if (rr < 0) continue;
                r.fill_rect({T.x + T.w - 6, T.y + T.h * (rr / total), 6, 2}, alpha(col(app_.theme().error), 0.8f));
            }
        }
    }
}

void Pane::draw_scroll_pill(Renderer& r, Font& font, const DrawCtx& ctx) {
    int64_t below = lines_below();
    if (below <= 0) return;
    const Theme& th = app_.theme();
    bool streaming = (ctx.now - last_output) < 1.5;
    std::string label = streaming
        ? ("↓ Streaming below (" + std::to_string((long long)below) + " lines) · click to follow")
        : ("↓ " + std::to_string((long long)below) + " lines below · click to follow");
    Rect box = scroll_pill_rect();
    float tw = r.measure_text(font, label, gfx::STYLE_BOLD);
    box.w = std::min(term_rect().w - 20.f, std::max(box.w, tw + 24.f));
    box.x = term_rect().x + (term_rect().w - box.w) / 2.f;
    RGBA border_c = streaming ? col(th.warning) : col(th.accent);
    r.rounded_rect(box, 12.f, alpha(col(th.chrome_bg), hover.scroll_pill ? 0.98f : 0.92f));
    r.rounded_rect_stroke(box, 12.f, alpha(border_c, hover.scroll_pill ? 0.95f : 0.7f), 1.2f);
    float tx = box.x + (box.w - tw) / 2.f;
    float ty = box.y + box.h / 2.f + font.metrics().ascent / 2.f - 1.f;
    r.draw_text(font, tx, ty, label, streaming ? col(th.warning) : col(th.foreground), gfx::STYLE_BOLD);
}

void Pane::draw_dormant(Renderer& r, Font& font, const DrawCtx& ctx) {
    const Theme& th = app_.theme();
    Rect T = term_rect();
    if (ctx.scale * cell_h() < 4.f) return;
    std::string msg;
    if (!session) msg = err_.empty() ? (has_restored() ? "dormant · press Enter or click to resume the shell here" : "press Enter to start a shell") : "failed to start shell: " + err_;
    else msg = "shell exited (" + std::to_string(session->exit_status()) + ") · press Enter to restart · × or Ctrl+Shift+W to close";
    float w = r.measure_text(font, msg) + 24;
    float h = cell_h() + 12;
    Rect box = {T.x + (T.w - w) / 2, T.y + T.h - h - 12, w, h};
    r.rounded_rect(box, 6, alpha(col(th.chrome_bg), 0.95f));
    r.rounded_rect_stroke(box, 6, alpha(col(th.accent), 0.6f), 1.f);
    r.draw_text(font, box.x + 12, box.y + 6 + font.metrics().ascent, msg, col(th.chrome_fg));
}

void Pane::draw_artifact_drawer(Renderer& r, Font& font, const DrawCtx& ctx) {
    const AgyArtifact* art = active_artifact();
    if (!art) return;
    ensure_artifact_loaded();
    const Theme& th = app_.theme();
    Rect R = rect();
    Rect D = drawer_rect();
    RGBA bg = col(th.background);
    RGBA chrome = col(th.chrome_bg);
    RGBA fg = col(th.chrome_fg);
    float asc = font.metrics().ascent;

    // Subtle connector bridge between terminal card and artifact drawer
    float cy = R.y + TITLE_H / 2.f;
    r.line(R.x + R.w, cy, D.x, cy, alpha(col(th.accent), 0.65f), 2.f);

    // Shadow & card background
    r.rounded_rect({D.x + 2, D.y + 4, D.w, D.h}, RADIUS, {0, 0, 0, 0.35f});
    r.rounded_rect(D, RADIUS, chrome);
    Rect body_card = {D.x, D.y + TITLE_H + 30.f, D.w, D.h - TITLE_H - 30.f};
    r.rounded_rect(body_card, RADIUS, bg);
    r.fill_rect({body_card.x, body_card.y, body_card.w, RADIUS}, bg);
    r.fill_rect({D.x, D.y + TITLE_H, D.w, 1.f}, alpha(fg, 0.15f));
    r.fill_rect({D.x, D.y + TITLE_H + 30.f, D.w, 1.f}, alpha(fg, 0.18f));

    // Title bar
    float tbase = D.y + TITLE_H / 2.f + asc / 2.f - 1.f;
    std::string title = (art->request_feedback ? "📋 " : "📄 ") + art->name;
    float right_ctrl = drawer_close_rect().x - 8.f;
    if (artifacts_.size() > 1) {
        Rect pv = drawer_prev_rect();
        Rect nx = drawer_next_rect();
        right_ctrl = pv.x - 8.f;
        r.rounded_rect(pv, 4.f, alpha(hover.drawer_prev ? col(th.accent) : fg, hover.drawer_prev ? 0.5f : 0.12f));
        float pvw = r.measure_text(font, "◀");
        r.draw_text(font, pv.x + (pv.w - pvw) / 2.f, tbase, "◀", col(th.foreground));

        std::string cnt = std::to_string(artifact_idx_ + 1) + "/" + std::to_string(artifacts_.size());
        float cntw = r.measure_text(font, cnt);
        r.draw_text(font, pv.x + pv.w + (nx.x - (pv.x + pv.w) - cntw) / 2.f, tbase, cnt, alpha(fg, 0.85f));

        r.rounded_rect(nx, 4.f, alpha(hover.drawer_next ? col(th.accent) : fg, hover.drawer_next ? 0.5f : 0.12f));
        float nxw = r.measure_text(font, "▶");
        r.draw_text(font, nx.x + (nx.w - nxw) / 2.f, tbase, "▶", col(th.foreground));
    }
    Rect cl = drawer_close_rect();
    r.rounded_rect(cl, 5.f, hover.drawer_close ? alpha(col(th.error), 0.85f) : alpha(fg, 0.12f));
    float xw = r.measure_text(font, "×", gfx::STYLE_BOLD);
    r.draw_text(font, cl.x + (cl.w - xw) / 2.f, tbase, "×", col(th.foreground), gfx::STYLE_BOLD);

    r.push_clip({D.x + 12.f, D.y, std::max(20.f, right_ctrl - (D.x + 12.f)), TITLE_H});
    float tx = D.x + 12.f + r.draw_text(font, D.x + 12.f, tbase, title, col(th.foreground), gfx::STYLE_BOLD) + 8.f;
    if (art->request_feedback) {
        r.draw_text(font, tx, tbase, "· Review Plan", col(th.warning), gfx::STYLE_BOLD);
    } else {
        r.draw_text(font, tx, tbase, "· AGY Artifact", alpha(fg, 0.65f));
    }
    r.pop_clip();

    // Action bar (Proceed / Feedback / Edit)
    auto draw_btn = [&](const Rect& br, const std::string& lbl, RGBA accent_c, bool hot, bool primary) {
        r.rounded_rect(br, 5.f, alpha(accent_c, hot ? 0.42f : (primary ? 0.24f : 0.12f)));
        r.rounded_rect_stroke(br, 5.f, alpha(accent_c, hot || primary ? 0.8f : 0.35f), 1.f);
        float lw = r.measure_text(font, lbl, primary ? gfx::STYLE_BOLD : gfx::STYLE_REGULAR);
        float by = br.y + br.h / 2.f + asc / 2.f - 1.f;
        r.draw_text(font, br.x + (br.w - lw) / 2.f, by, lbl, hot || primary ? col(th.foreground) : accent_c, primary ? gfx::STYLE_BOLD : gfx::STYLE_REGULAR);
    };
    draw_btn(drawer_proceed_rect(), "✓ Proceed", col(th.ok), hover.drawer_proceed, art->request_feedback);
    draw_btn(drawer_feedback_rect(), "✎ Feedback", col(th.accent), hover.drawer_feedback, false);
    draw_btn(drawer_edit_rect(), "↗ Editor", fg, hover.drawer_edit, false);

    // Body content
    Rect B = drawer_body_rect();
    float line_h = cell_h() + 2.f;
    float total_h = (float)md_lines_.size() * line_h + 12.f;
    float max_scroll = std::max(0.f, total_h - B.h);
    drawer_scroll_ = clampf(drawer_scroll_, 0.f, max_scroll);

    r.push_clip(B);
    int first = std::max(0, (int)(drawer_scroll_ / line_h));
    int last = std::min((int)md_lines_.size(), first + (int)(B.h / line_h) + 3);
    for (int i = first; i < last; ++i) {
        const MdLine& ml = md_lines_[i];
        float y0 = B.y + i * line_h - drawer_scroll_;
        float base = y0 + asc;
        float x0 = B.x + 6.f + ml.indent * cell_w();

        switch (ml.kind) {
        case MdLine::SummaryMeta:
            r.fill_rect({B.x + 2.f, y0, B.w - 8.f, line_h}, alpha(col(th.accent), 0.10f));
            r.fill_rect({B.x + 2.f, y0, 3.f, line_h}, col(th.accent));
            r.draw_text(font, x0 + 4.f, base, ml.text, alpha(col(th.foreground), 0.9f), gfx::STYLE_ITALIC);
            break;
        case MdLine::H1:
            r.draw_text(font, x0, base, ml.text, col(th.accent), gfx::STYLE_BOLD);
            r.fill_rect({B.x + 4.f, y0 + line_h - 1.f, B.w - 12.f, 1.f}, alpha(col(th.accent), 0.4f));
            break;
        case MdLine::H2:
            r.draw_text(font, x0, base, ml.text, col(th.accent), gfx::STYLE_BOLD);
            break;
        case MdLine::H3:
            r.draw_text(font, x0, base, ml.text, col(th.foreground), gfx::STYLE_BOLD);
            break;
        case MdLine::Code:
            r.fill_rect({B.x + 4.f, y0, B.w - 12.f, line_h}, alpha(chrome, 0.75f));
            r.fill_rect({B.x + 4.f, y0, 2.f, line_h}, alpha(col(th.ansi[6]), 0.6f));
            r.draw_text(font, x0 + 2.f, base, ml.text, col(th.ansi[6]));
            break;
        case MdLine::Quote:
            r.fill_rect({B.x + 4.f, y0, 3.f, line_h}, alpha(fg, 0.45f));
            r.draw_text(font, x0 + 4.f, base, ml.text, alpha(col(th.foreground), 0.78f), gfx::STYLE_ITALIC);
            break;
        case MdLine::AlertNote:
        case MdLine::AlertTip:
        case MdLine::AlertImportant:
        case MdLine::AlertWarning:
        case MdLine::AlertCaution: {
            RGBA ac = col(th.accent);
            if (ml.kind == MdLine::AlertTip) ac = col(th.ok);
            else if (ml.kind == MdLine::AlertImportant || ml.kind == MdLine::AlertWarning) ac = col(th.warning);
            else if (ml.kind == MdLine::AlertCaution) ac = col(th.error);
            r.fill_rect({B.x + 4.f, y0, B.w - 12.f, line_h}, alpha(ac, 0.11f));
            r.fill_rect({B.x + 4.f, y0, 3.f, line_h}, ac);
            r.draw_text(font, x0 + 4.f, base, ml.text, col(th.foreground));
            break;
        }
        case MdLine::Rule:
            r.fill_rect({B.x + 4.f, y0 + line_h / 2.f, B.w - 12.f, 1.f}, alpha(fg, 0.25f));
            break;
        case MdLine::Normal:
        default:
            r.draw_text(font, x0, base, ml.text, col(th.foreground));
            break;
        }
    }
    r.pop_clip();

    if (total_h > B.h) {
        float frac = B.h / total_h;
        float sh = std::max(16.f, B.h * frac);
        float sy = B.y + (B.h - sh) * (drawer_scroll_ / std::max(1.f, max_scroll));
        r.rounded_rect({D.x + D.w - 6.f, sy, 3.f, sh}, 1.5f, alpha(fg, 0.4f));
    }

    RGBA border = (ctx.focused || art->request_feedback) ? alpha(col(art->request_feedback ? th.warning : th.accent), 0.85f) : alpha(fg, 0.35f);
    r.rounded_rect_stroke(D, RADIUS, border, ctx.focused ? 1.5f : 1.f);
}

} // namespace canopy::ui


namespace canopy::ui {

// ---------------------------------------------------------------- summaries
static std::string tail_lines(const std::vector<std::string>& lines, size_t n, size_t max_chars = 160) {
    std::string out;
    size_t start = lines.size() > n ? lines.size() - n : 0;
    for (size_t i = start; i < lines.size(); i++) {
        if (lines[i].empty()) continue;
        out += "    " + (lines[i].size() > max_chars ? lines[i].substr(0, max_chars) + "…" : lines[i]) + "\n";
    }
    return out;
}

std::string Pane::summary_context() const {
    std::string ctx;
    const Activity* a = app_.tree().get(activity_id);
    ctx += "activity: " + (a ? a->name : std::string("?")) + "  (cwd: " + pretty_path(cwd(), 80) + ")\n";
    if (a && a->parent) if (const Activity* pa = app_.tree().get(a->parent)) ctx += "parent activity: " + pa->name + "\n";
    if (a && !a->notes.empty()) ctx += "user notes: " + ellipsize(a->notes, 300) + "\n";
    if (!session) {
        ctx += "status: dormant (no shell running); history restored from a previous session\n";
    } else if (!session->alive()) {
        ctx += "status: shell exited\n";
    } else {
        const Terminal& t = session->term();
        const auto& bl = session->blocks().blocks();
        const Block* running = (!bl.empty() && bl.back().state == BlockState::Running) ? &bl.back() : nullptr;
        if (t.modes().alt_screen) {
            std::string prog = session->pty().foreground_name();
            ctx += "status: full-screen program running: " + (prog.empty() ? std::string("unknown") : prog);
            if (!last_title.empty()) ctx += "  (window title: " + last_title + ")";
            if (running) ctx += "  started by: " + running->command;
            ctx += "\n";
        } else if (running) {
            ctx += "status: command running for " + fmt_duration(app_.now() - running->start_time) + ": " + running->command + "\n";
        } else ctx += "status: idle at the shell prompt\n";
    }
    // recent commands
    std::vector<const Block*> recent;
    if (session) { const auto& bl = session->blocks().blocks(); for (const Block& b : bl) if (!b.command.empty()) recent.push_back(&b); }
    size_t limit = 8;
    std::string cmds;
    size_t start = recent.size() > limit ? recent.size() - limit : 0;
    for (size_t i = start; i < recent.size(); i++) {
        const Block& b = *recent[i];
        std::string line = "  [";
        if (b.state == BlockState::Done) line += "exit " + std::to_string(b.exit_code) + ", " + fmt_duration(b.duration());
        else line += "running";
        line += "] " + ellipsize(b.command, 200) + "\n";
        cmds += line;
        if (i + 3 >= recent.size()) cmds += tail_lines(block_lines(b), b.state == BlockState::Running ? 12 : 8);
    }
    if (cmds.empty() && !restored_blocks_.empty()) {
        size_t rs = restored_blocks_.size() > limit ? restored_blocks_.size() - limit : 0;
        for (size_t i = rs; i < restored_blocks_.size(); i++) {
            const Block& b = restored_blocks_[i];
            cmds += "  [exit " + std::to_string(b.exit_code) + "] " + ellipsize(b.command, 200) + "\n";
            if (i + 2 >= restored_blocks_.size()) cmds += tail_lines(b.saved_output, 6);
        }
    }
    if (!cmds.empty()) ctx += "recent commands (oldest first):\n" + cmds;
    if (session && session->alive()) {
        const Terminal& t = session->term();
        bool show_screen = t.modes().alt_screen || recent.empty() || (!recent.empty() && recent.back()->state == BlockState::Running);
        if (show_screen) {
            std::string scr;
            int shown = 0;
            for (int y = t.rows() - 1; y >= 0 && shown < 24; y--) {
                std::string txt = t.row(y).text();
                if (txt.find_first_not_of(" \t") == std::string::npos) continue;
                scr = "    " + (txt.size() > 160 ? txt.substr(0, 160) : txt) + "\n" + scr;
                shown++;
            }
            if (!scr.empty()) ctx += "current screen (last lines):\n" + scr;
        }
    }
    if (ctx.size() > 7000) ctx = ctx.substr(0, 7000) + "\n[truncated]";
    return ctx;
}

uint64_t Pane::context_hash() const { return std::hash<std::string>{}(summary_context()); }

std::string Pane::local_summary() const {
    std::string dir = pretty_path(cwd(), 32);
    if (!session) {
        if (!restored_blocks_.empty()) return "Dormant · last ran " + ellipsize(restored_blocks_.back().command, 36) + " in " + dir;
        return "Dormant shell in " + dir;
    }
    if (!session->alive()) return "Shell exited in " + dir;
    const Terminal& t = session->term();
    const auto& bl = session->blocks().blocks();
    std::string cmd_name = (!bl.empty() && !bl.back().command.empty()) ? ellipsize(bl.back().command, 32) : session->pty().foreground_name();
    if (cmd_name.empty()) cmd_name = "agent";
    if (agent_state_ == AgentState::NeedsApproval) return "⚡ Needs approval in " + cmd_name + " (" + dir + ")";
    if (agent_state_ == AgentState::WaitingInput) return "⏸ Waiting for input in " + cmd_name + " (" + dir + ")";
    if (bell_unread_ && !notify_msg_.empty()) return "🔔 " + ellipsize(notify_msg_, 48) + " (" + dir + ")";
    if (t.modes().alt_screen) {
        std::string prog = session->pty().foreground_name();
        std::string s = "In " + (prog.empty() ? std::string("a full-screen program") : prog);
        if (!last_title.empty()) s += ": " + ellipsize(last_title, 40);
        else if (!bl.empty() && !bl.back().command.empty()) s += " (" + ellipsize(bl.back().command, 36) + ")";
        return s;
    }
    if (!bl.empty() && bl.back().state == BlockState::Running)
        return "Running " + ellipsize(bl.back().command, 40) + " for " + fmt_duration(app_.now() - bl.back().start_time) + " in " + dir;
    int n = 0; const Block* last = nullptr;
    for (const Block& b : bl) if (b.state == BlockState::Done && !b.command.empty()) { n++; last = &b; }
    if (last) return std::string(last->exit_code == 0 ? "Idle after " : "Idle after failed ") + ellipsize(last->command, 40) + " in " + dir;
    return "Fresh shell in " + dir;
}

float Pane::attention_score(double now, uint64_t focused) const {
    float s = 0;
    if (activity_id == focused) s -= 1000;              // the current one: last in "find another" lists
    if (agent_state_ == AgentState::NeedsApproval) s += 95;
    else if (agent_state_ == AgentState::WaitingInput) s += 75;
    if (artifact_unread_ && active_artifact() && active_artifact()->request_feedback) s += 85;
    else if (artifact_unread_) s += 40;
    if (bell_unread_) s += 65;
    if (unread) s += 50;
    if (session && session->alive()) {
        const auto& bl = session->blocks().blocks();
        if (!bl.empty() && bl.back().state == BlockState::Running) s += 30;
        for (size_t i = bl.size(); i-- > 0;) { if (bl[i].state == BlockState::Done && !bl[i].command.empty()) { if (bl[i].exit_code != 0 && unread) s += 25; break; } }
        if (session->term().modes().alt_screen) s += 10;
    } else if (session) s += 5;                          // exited
    else s -= 20;                                        // dormant
    double age = now - std::max(last_output, last_finish);
    s += (float)std::max(0.0, 40.0 - age / 30.0);        // recency: fades over 20 minutes
    return s;
}

std::vector<std::string> Pane::attention_badges(double now) const {
    std::vector<std::string> b;
    if (session && session->alive()) {
        if (agent_state_ == AgentState::NeedsApproval) b.push_back("⚡ needs approval");
        else if (agent_state_ == AgentState::WaitingInput) b.push_back("⏸ waiting for input");
        if (bell_unread_) b.push_back(notify_msg_.empty() ? "🔔 bell" : "🔔 " + ellipsize(notify_msg_, 20));
        const auto& bl = session->blocks().blocks();
        if (!bl.empty() && bl.back().state == BlockState::Running) b.push_back("⟳ " + fmt_duration(now - bl.back().start_time));
        else if (session->term().modes().alt_screen) { std::string p = session->pty().foreground_name(); b.push_back(p.empty() ? "full-screen" : p); }
        for (size_t i = bl.size(); i-- > 0;) if (bl[i].state == BlockState::Done && !bl[i].command.empty()) { if (bl[i].exit_code != 0) b.push_back("✗ exit " + std::to_string(bl[i].exit_code)); break; }
    } else if (session) b.push_back("exited");
    else b.push_back("dormant");
    if (unread && !waiting_for_user()) b.push_back("● new output");
    if (const AgyArtifact* art = active_artifact()) {
        if (art->request_feedback && artifact_unread_) b.push_back("📋 plan review");
        else b.push_back("📄 " + ellipsize(art->name, 16));
    }
    return b;
}

std::string Pane::recommendation_reason(double now) const {
    if (agent_state_ == AgentState::NeedsApproval) return "⚡ needs approval";
    if (artifact_unread_ && active_artifact() && active_artifact()->request_feedback) return "📋 plan ready for review";
    if (agent_state_ == AgentState::WaitingInput) return "⏸ waiting for your input";
    if (bell_unread_) return notify_msg_.empty() ? "🔔 notification" : ("🔔 " + ellipsize(notify_msg_, 24));
    if (session && session->alive()) {
        const auto& bl = session->blocks().blocks();
        for (size_t i = bl.size(); i-- > 0;) {
            if (bl[i].state == BlockState::Done && !bl[i].command.empty()) {
                if (bl[i].exit_code != 0 && unread) return "✗ failed (exit " + std::to_string(bl[i].exit_code) + ")";
                break;
            }
        }
        if (artifact_unread_ && active_artifact()) return "📄 updated " + ellipsize(active_artifact()->name, 18);
        if (unread) return "● new output";
        if (!bl.empty() && bl.back().state == BlockState::Running) return "⟳ running " + ellipsize(bl.back().command, 18);
        if (session->term().modes().alt_screen) {
            std::string p = session->pty().foreground_name();
            return p.empty() ? "active TUI" : ("in " + p);
        }
        double age = now - std::max(last_output, last_finish);
        if (last_output > 0 && age < 120.0) return "active " + fmt_duration(age) + " ago";
        return "idle at prompt";
    }
    if (session) return "shell exited";
    return "dormant";
}

void Pane::draw_summary_banner(gfx::Renderer& r, gfx::Font& font, const DrawCtx& ctx) {
    std::string text = summary.empty() ? local_summary() : summary;
    if (text.empty()) return;
    const Theme& th = app_.theme();
    Rect R = rect();
    float sc = clampf(1.0f / std::max(ctx.scale, 0.05f), 1.4f, 6.f) * 0.9f;   // readable at any zoom
    float lh = app_.cell_h() * sc;
    float pad = 10 * sc;
    float maxw = R.w - 2 * pad;
    // wrap into at most 3 lines
    std::vector<std::string> lines; std::string cur;
    size_t pos = 0;
    while (pos < text.size() && lines.size() < 3) {
        size_t sp = text.find(' ', pos);
        std::string word = text.substr(pos, sp == std::string::npos ? std::string::npos : sp - pos);
        std::string trial = cur.empty() ? word : cur + " " + word;
        if (r.measure_text(font, trial, gfx::STYLE_BOLD, sc) > maxw && !cur.empty()) { lines.push_back(cur); cur = word; }
        else cur = trial;
        if (sp == std::string::npos) break;
        pos = sp + 1;
    }
    if (!cur.empty() && lines.size() < 3) lines.push_back(cur);
    float h = lines.size() * lh + 2 * pad;
    Rect box = {R.x + pad * 0.6f, R.y + TITLE_H + pad * 0.6f, R.w - pad * 1.2f, h};
    r.rounded_rect(box, 6 * sc, alpha(col(th.chrome_bg), 0.88f));
    r.rounded_rect_stroke(box, 6 * sc, alpha(summary_ai ? col(th.accent) : col(th.chrome_fg), 0.5f), std::max(1.f, sc * 0.6f));
    float y = box.y + pad + font.metrics().ascent * sc;
    for (auto& l : lines) { r.draw_text(font, box.x + pad, y, l, col(th.foreground), gfx::STYLE_BOLD, sc); y += lh; }
    // badges
    auto badges = attention_badges(ctx.now);
    if (!badges.empty()) {
        std::string b; for (size_t i = 0; i < badges.size(); i++) b += (i ? "   " : "") + badges[i];
        r.draw_text(font, box.x + pad, y + lh * 0.1f, b, alpha(col(th.warning), 0.95f), gfx::STYLE_REGULAR, sc * 0.85f);
    }
}

} // namespace canopy::ui
