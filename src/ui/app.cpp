#include "app.h"
#include "canvas.h"
#include "overlay.h"
#include "peek.h"
#include "switcher.h"
#include "pane.h"
#include "sidebar.h"
#include "gfx/gl.h"
#include <SDL.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <signal.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include <fcntl.h>

namespace canopy::ui {

App::App() {}
App::~App() {
    panes_.clear();
}

double App::now() const { return window_ ? window_->now() : 0.0; }
void App::wake() { if (window_) window_->wake(); }
float App::cell_w() const { return (float)font_->metrics().cell_w; }
float App::cell_h() const { return (float)font_->metrics().cell_h; }

Pane* App::pane(uint64_t id) { auto it = panes_.find(id); return it == panes_.end() ? nullptr : it->second.get(); }
Pane* App::focused_pane() { return pane(focused_); }

// ---------------------------------------------------------------- init
bool App::init(int argc, char** argv, std::string* err) {
    std::string cfg_path;
    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        if (a == "--config" && i + 1 < argc) cfg_path = argv[++i];
        else if (a == "--help" || a == "-h") { printf("usage: canopy [--config FILE]\n"); exit(0); }
    }
    std::string cfg_err;
    if (!cfg_.load(cfg_path, &cfg_err) && !cfg_err.empty()) fprintf(stderr, "canopy: config: %s\n", cfg_err.c_str());
    else if (!cfg_err.empty()) fprintf(stderr, "canopy: config warning: %s\n", cfg_err.c_str());

    window_ = std::make_unique<gfx::Window>();
    if (!window_->create("Canopy", cfg_.window_width, cfg_.window_height, err)) return false;
    if (!gfx::gl_load()) { if (err) *err = "failed to load OpenGL functions"; return false; }
    renderer_ = std::make_unique<gfx::Renderer>();
    if (!renderer_->init(err)) return false;
    font_ = std::make_unique<gfx::Font>();
    float px = cfg_.font_size_px * window_->dpi_scale();
    {
        std::vector<std::string> candidates = {cfg_.font_family, "DejaVu Sans Mono", "JetBrains Mono", "Fira Code", "Source Code Pro", "Ubuntu Mono", "Liberation Mono", "monospace"};
        bool ok = false;
        for (size_t i = 0; i < candidates.size() && !ok; i++) {
            if (candidates[i].empty() || (i > 0 && candidates[i] == cfg_.font_family)) continue;
            std::string ferr;
            if (font_->load(candidates[i], px, cfg_.cell_padding_x, cfg_.cell_padding_y, &ferr)) {
                if (i > 0) fprintf(stderr, "canopy: font '%s' unavailable; using '%s'\n", cfg_.font_family.c_str(), candidates[i].c_str());
                ok = true;
            }
        }
        if (!ok) { if (err) *err = "no usable monospace font found"; return false; }
    }
    shell_dir_ = find_shell_integration_dir();
    if (shell_dir_.empty()) fprintf(stderr, "canopy: shell integration script not found; blocks will be unavailable\n");

    canvas_ = std::make_unique<Canvas>(*this);
    sidebar_ = std::make_unique<Sidebar>(*this);
    overlay_ = std::make_unique<Overlay>(*this);
    sidebar_->visible = cfg_.sidebar_visible;
    sidebar_->width = (float)cfg_.sidebar_width * window_->dpi_scale();
    rail_ = std::make_unique<PeekRail>(*this);
    switcher_ = std::make_unique<Switcher>(*this);
    rail_->visible = cfg_.peek_visible;
    rail_->width = (float)cfg_.peek_width * window_->dpi_scale();
    if (cfg_.ai_summaries) {
        Summarizer::Options so;
        so.backend = cfg_.ai_backend; so.api_model = cfg_.ai_model; so.cli_model = cfg_.ai_cli_model; so.api_key = cfg_.ai_api_key;
        summarizer_.on_result = [this] { wake(); };
        if (summarizer_.configure(so) == Summarizer::Backend::None && cfg_.ai_backend != "local")
            fprintf(stderr, "canopy: no summary backend available (set ANTHROPIC_API_KEY or install the claude CLI); using local summaries\n");
    }
    setup_actions();
    layout_chrome();
    restore_tree();
    start_time_ = now();
    last_save_ = start_time_;
    window_->start_text_input();
    return true;
}

void App::restore_tree() {
    std::string err;
    if (!tree_.load()) fprintf(stderr, "canopy: could not load activity tree; starting fresh\n");
    uint64_t most_recent = 0; int64_t best = -1;
    for (uint64_t id : tree_.preorder()) {
        Activity* a = tree_.get(id);
        if (!a) continue;
        if (a->cwd.empty() && id == tree_.root()) a->cwd = home_dir();
        create_pane_for(id);
        if (a->last_active > best) { best = a->last_active; most_recent = id; }
    }
    if (Activity* root = tree_.get(tree_.root())) if (root->name.empty()) root->name = "Home";
    canvas_->layout();
    uint64_t first = most_recent ? most_recent : tree_.root();
    if (Pane* p = pane(first)) ensure_session(*p);
    focus(first, true);
    canvas_->fly_to_pane(*pane(first), true);
}

void App::create_pane_for(uint64_t id) {
    Activity* a = tree_.get(id);
    if (!a || panes_.count(id)) return;
    auto p = std::make_unique<Pane>(*this, id);
    if (a->cols > 0 && a->rows > 0) { p->cols = a->cols; p->rows = a->rows; }
    p->x = a->x; p->y = a->y;
    if (!a->blocks.empty()) p->load_restored(a->blocks);
    if (!a->summary.empty()) { p->summary = a->summary; p->summary_ai = a->summary_ai; p->summary_dirty = false; }
    panes_[id] = std::move(p);
    canvas_->sync_panes();
}

void App::ensure_session(Pane& p) {
    if (p.live()) return;
    std::string err;
    if (!p.start_session(&err)) toast("Could not start shell: " + err);
    else if (Activity* a = tree_.get(p.activity_id)) a->last_active = (int64_t)time(nullptr);
    request_redraw();
}

// ---------------------------------------------------------------- activities
void App::focus(uint64_t id, bool fly) {
    Pane* p = pane(id);
    if (!p) return;
    if (Pane* old = focused_pane()) if (old != p && old->live()) old->session->term().notify_focus(false);
    if (focused_ && focused_ != id) request_summary(focused_, false);
    focused_ = id;
    mru_.erase(std::remove(mru_.begin(), mru_.end(), id), mru_.end());
    mru_.insert(mru_.begin(), id);
    p->unread = false;
    if (Activity* a = tree_.get(id)) a->last_active = (int64_t)time(nullptr);
    canvas_->bring_to_front(id);
    ensure_session(*p);
    if (p->live()) p->session->term().notify_focus(window_->focused());
    if (fly) canvas_->fly_to_pane(*p); else canvas_->ensure_visible(*p);
    if (Activity* a = tree_.get(id)) title_ = a->name;
    window_->set_title(title_ + " — Canopy");
    request_redraw();
}

uint64_t App::new_activity(uint64_t parent, const std::string& name, const std::string& cwd, bool focus_it) {
    if (!tree_.get(parent)) parent = tree_.root();
    std::string c = cwd;
    if (c.empty()) { if (Pane* pp = pane(parent)) c = pp->cwd(); }
    std::string n = name;
    bool generated = n.empty();
    if (n.empty()) n = basename_of(c.empty() ? "activity" : c);
    uint64_t id = tree_.create(parent, n, c);
    if (Activity* a = tree_.get(id)) { a->auto_named = generated; a->cols = cfg_.default_cols; a->rows = cfg_.default_rows; if (Activity* pa = tree_.get(parent)) { a->env = pa->env; pa->collapsed = false; } }
    create_pane_for(id);
    canvas_->layout();
    if (Pane* p = pane(id)) {
        // start at the parent's position and animate into place
        if (Pane* pp = pane(parent)) { p->x = pp->x + 40; p->y = pp->y + 40; }
        ensure_session(*p);
    }
    if (focus_it) focus(id, true);
    tree_.notify();
    save_state();
    return id;
}

uint64_t App::run_in_child(uint64_t parent, const std::string& command, const std::string& cwd, const std::string& name) {
    uint64_t id = new_activity(parent, name.empty() ? ellipsize(command, 28) : name, cwd, true);
    if (Pane* p = pane(id)) p->queue_input_at_prompt(command + "\r");
    return id;
}

void App::close_activity(uint64_t id) {
    if (id == tree_.root()) { toast("The root activity cannot be closed"); return; }
    Activity* a = tree_.get(id);
    if (!a) return;
    static uint64_t pending_close = 0; static double pending_time = 0;
    Pane* p = pane(id);
    bool busy = p && p->live() && !p->at_prompt();
    bool kids = !a->children.empty();
    if ((busy || kids) && !(pending_close == id && now() - pending_time < 4.0)) {
        pending_close = id; pending_time = now();
        toast(std::string(kids ? "Activity has children" : "A command is running") + " — close again to confirm");
        return;
    }
    pending_close = 0;
    uint64_t parent = a->parent;
    std::vector<uint64_t> gone;
    for (uint64_t x : tree_.preorder()) { // collect subtree
        uint64_t cur = x; bool in = false;
        while (cur) { if (cur == id) { in = true; break; } Activity* c = tree_.get(cur); cur = c ? c->parent : 0; }
        if (in) gone.push_back(x);
    }
    for (uint64_t g : gone) { panes_.erase(g); mru_.erase(std::remove(mru_.begin(), mru_.end(), g), mru_.end()); }
    tree_.remove(id);
    canvas_->sync_panes();
    canvas_->layout();
    if (!pane(focused_)) focus(parent ? parent : tree_.root(), true);
    tree_.notify();
    save_state();
}

void App::rename_activity(uint64_t id, const std::string& name) {
    if (Activity* a = tree_.get(id)) { if (!name.empty()) { a->name = name; a->auto_named = false; } }
    if (id == focused_) { title_ = name; window_->set_title(title_ + " — Canopy"); }
    tree_.notify(); save_state(); request_redraw();
}

void App::add_note(uint64_t id, const std::string& note) {
    Activity* a = tree_.get(id);
    if (!a || note.empty()) return;
    if (!a->notes.empty()) a->notes += "\n";
    a->notes += note;
    toast("Note added to " + a->name);
    save_state();
}

void App::on_shell_request(uint64_t activity, const std::string& kind, const std::string& payload) {
    if (kind == "new") { Pane* p = pane(activity); new_activity(activity, payload, p ? p->cwd() : "", true); }
    else if (kind == "note") add_note(activity, payload);
    else if (kind == "name") rename_activity(activity, payload);
}

void App::on_block_finished(uint64_t activity, const Block& b) {
    request_redraw();
    if (!b.command.empty()) request_summary(activity, false);
    if (b.command.empty()) return;
    bool unfocused = activity != focused_ || !window_->focused();
    if (unfocused && b.duration() >= 3.0) {
        Activity* a = tree_.get(activity);
        std::string msg = std::string(b.exit_code == 0 ? "✓ " : "✗ ") + ellipsize(b.command, 40) + " finished in " + fmt_duration(b.duration()) + (a ? "  (" + a->name + ")" : "");
        toast(msg, 6.f);
        if (!window_->focused()) window_->request_attention();
    }
}

void App::bell(uint64_t activity) {
    if (cfg_.visual_bell) { Activity* a = tree_.get(activity); toast(std::string("🔔 bell") + (a ? " in " + a->name : "")); }
    if (!window_->focused()) window_->request_attention();
}

void App::diff_outputs(uint64_t activity, const std::string& cmd, const std::string& older, const std::string& newer) {
    std::string dir = home_dir() + "/.cache/canopy";
    mkdir((home_dir() + "/.cache").c_str(), 0755); mkdir(dir.c_str(), 0755);
    std::string stamp = std::to_string((long long)time(nullptr));
    std::string a = dir + "/diff-" + stamp + "-prev.txt", b = dir + "/diff-" + stamp + "-last.txt";
    write_file(a, older); write_file(b, newer);
    Pane* p = pane(activity);
    run_in_child(activity, "diff -u --color=always " + shell_quote(a) + " " + shell_quote(b) + " ; echo; echo \"[diff: previous vs latest run of: " + cmd + "]\"", p ? p->cwd() : "", "diff: " + ellipsize(cmd, 20));
}

// ---------------------------------------------------------------- services
void App::toast(const std::string& text, float seconds) { overlay_->toast(text, seconds); }
void App::show_menu(float sx, float sy, std::vector<MenuItem> items) { overlay_->open_menu(sx, sy, std::move(items)); }
void App::prompt_text(const std::string& label, const std::string& initial, std::function<void(const std::string&)> on_done) { overlay_->open_prompt(label, initial, std::move(on_done)); }
void App::set_clipboard(const std::string& s) { window_->set_clipboard(s); }
std::string App::clipboard() { return window_->clipboard(); }
void App::set_primary(const std::string& s) { window_->set_primary_selection(s); }
std::string App::primary() { std::string p = window_->primary_selection(); return p.empty() ? window_->clipboard() : p; }

static void spawn_detached(const std::vector<std::string>& argv) {
    pid_t pid = fork();
    if (pid == 0) {
        setsid();
        if (fork() != 0) _exit(0);
        std::vector<char*> av; for (auto& s : argv) av.push_back(const_cast<char*>(s.c_str())); av.push_back(nullptr);
        int devnull = open("/dev/null", O_RDWR);
        if (devnull >= 0) { dup2(devnull, 0); dup2(devnull, 1); dup2(devnull, 2); }
        execvp(av[0], av.data());
        _exit(127);
    } else if (pid > 0) { int st; waitpid(pid, &st, 0); }
}

void App::open_url(const std::string& url) { spawn_detached({"xdg-open", url}); toast("Opening " + ellipsize(url, 50)); }

void App::open_path(const std::string& path, int line, const std::string& cwd, uint64_t from) {
    std::string p = path;
    if (!p.empty() && p[0] == '~') p = home_dir() + p.substr(1);
    if (p.empty() || p[0] != '/') p = cwd + "/" + p;
    struct stat st;
    if (stat(p.c_str(), &st) != 0) { toast("Not found: " + ellipsize(path, 40)); return; }
    Pane* src = pane(from);
    if (S_ISDIR(st.st_mode)) {
        if (src && src->at_prompt()) src->write("cd " + shell_quote(p) + "\r");
        else new_activity(from, basename_of(p), p, true);
        return;
    }
    // text-ish file: open in the editor in a child activity; else xdg-open
    const char* ed = getenv("VISUAL"); if (!ed || !*ed) ed = getenv("EDITOR"); if (!ed || !*ed) ed = "vi";
    std::string lower = to_lower(p);
    static const char* binexts[] = {".png", ".jpg", ".jpeg", ".gif", ".pdf", ".mp4", ".mp3", ".zip", ".tar", ".gz", ".so", ".o", ".a", ".bin", ".webp", ".svg", ".deb"};
    for (const char* e : binexts) if (lower.size() > strlen(e) && lower.compare(lower.size() - strlen(e), strlen(e), e) == 0) { open_url(p); return; }
    std::string cmd = std::string(ed) + (line > 0 ? " +" + std::to_string(line) : "") + " " + shell_quote(p);
    run_in_child(from, cmd, cwd, basename_of(p));
}

void App::save_state() {
    for (auto& kv : panes_) {
        Activity* a = tree_.get(kv.first);
        if (!a) continue;
        Pane& p = *kv.second;
        a->blocks = p.export_blocks(60, 200);
        if (!a->auto_place) { a->x = p.x; a->y = p.y; }
        a->cols = p.cols; a->rows = p.rows;
        if (p.session) a->cwd = p.cwd();
        if (p.summary_ai) { a->summary = p.summary; a->summary_ai = true; }
    }
    if (!tree_.save()) fprintf(stderr, "canopy: failed to save activity tree\n");
    last_save_ = now();
}

void App::set_font_size(float px) {
    px = clampf(px, 6.f, 72.f);
    cfg_.font_size_px = px;
    if (!font_->set_size(px * window_->dpi_scale())) return;
    for (auto& kv : panes_) { Pane& p = *kv.second; if (p.session) p.session->resize(p.cols, p.rows, (int)(p.cols * cell_w()), (int)(p.rows * cell_h())); }
    canvas_->layout();
    if (Pane* p = focused_pane()) canvas_->fly_to_pane(*p);
    request_redraw();
}

// ---------------------------------------------------------------- actions
void App::setup_actions() {
    auto add = [this](const std::string& id, const std::string& label, const std::string& sc, std::function<void()> fn) { actions_.push_back({id, label, sc, std::move(fn), nullptr}); };
    add("activity.new_child", "New child activity", "Ctrl+Shift+N", [this] {
        uint64_t parent = focused_ ? focused_ : tree_.root();
        Pane* pp = pane(parent);
        new_activity(parent, "", pp ? pp->cwd() : "", true);
    });
    add("activity.new_sibling", "New sibling activity", "Ctrl+Shift+T", [this] {
        Activity* a = tree_.get(focused_);
        uint64_t parent = (a && a->parent) ? a->parent : tree_.root();
        Pane* pp = focused_pane();
        new_activity(parent, "", pp ? pp->cwd() : "", true);
    });
    add("activity.close", "Close activity", "Ctrl+Shift+W", [this] { close_activity(focused_); });
    add("activity.rename", "Rename activity…", "Ctrl+Shift+R", [this] {
        Activity* a = tree_.get(focused_); if (!a) return;
        uint64_t id = focused_;
        prompt_text("Rename activity", a->name, [this, id](const std::string& s) { rename_activity(id, s); });
    });
    add("activity.note", "Add note to activity…", "", [this] {
        uint64_t id = focused_;
        prompt_text("Add note", "", [this, id](const std::string& s) { add_note(id, s); });
    });
    add("activity.show_notes", "Show activity notes", "", [this] {
        Activity* a = tree_.get(focused_); if (!a) return;
        toast(a->notes.empty() ? "No notes for " + a->name : a->notes, 8.f);
    });
    add("activity.restart", "Restart shell in activity", "", [this] { if (Pane* p = focused_pane()) { p->stop_session(); ensure_session(*p); } });
    add("activity.collapse", "Collapse / expand children", "", [this] { if (Activity* a = tree_.get(focused_)) { a->collapsed = !a->collapsed; canvas_->layout(); } });
    add("view.overview", "Toggle overview", "F1", [this] { canvas_->toggle_overview(); });
    add("view.sidebar", "Toggle sidebar", "F2", [this] { sidebar_->visible = !sidebar_->visible; request_redraw(); });
    add("view.zoom_in", "Zoom in", "Ctrl+=", [this] { canvas_->zoom_at(canvas_->viewport.x + canvas_->viewport.w / 2, canvas_->viewport.y + canvas_->viewport.h / 2, 1.2f); });
    add("view.zoom_out", "Zoom out", "Ctrl+-", [this] { canvas_->zoom_at(canvas_->viewport.x + canvas_->viewport.w / 2, canvas_->viewport.y + canvas_->viewport.h / 2, 1 / 1.2f); });
    add("view.zoom_reset", "Zoom to 100%", "Ctrl+0", [this] { canvas_->reset_zoom(); });
    add("view.relayout", "Re-layout tree", "", [this] { for (auto& kv : panes_) if (Activity* a = tree_.get(kv.first)) a->auto_place = true; canvas_->layout(); });
    add("view.font_bigger", "Font size +", "Ctrl+Shift+=", [this] { set_font_size(cfg_.font_size_px + 1); });
    add("view.font_smaller", "Font size −", "Ctrl+Shift+-", [this] { set_font_size(cfg_.font_size_px - 1); });
    add("view.help", "Keyboard help", "F10", [this] { overlay_->toggle_help(); });
    add("view.rail", "Toggle peek rail (other terminals)", "F3", [this] { rail_->visible = !rail_->visible; request_redraw(); });
    add("view.switcher", "Find terminal…", "Ctrl+Tab", [this] { switcher_->show(); });
    add("ai.refresh", "Refresh summary of this terminal", "", [this] { request_summary(focused_, true); });
    add("ai.refresh_all", "Refresh all summaries", "", [this] { for (auto& kv : panes_) request_summary(kv.first, true); });
    add("block.prev", "Previous block", "Ctrl+Shift+Up", [this] { if (Pane* p = focused_pane()) p->select_block_relative(-1); });
    add("block.next", "Next block", "Ctrl+Shift+Down", [this] { if (Pane* p = focused_pane()) p->select_block_relative(1); });
    add("block.fold", "Fold selected block", "Ctrl+Shift+Left", [this] { if (Pane* p = focused_pane()) p->set_fold(p->selected_block, true); });
    add("block.unfold", "Unfold selected block", "Ctrl+Shift+Right", [this] { if (Pane* p = focused_pane()) p->set_fold(p->selected_block, false); });
    add("block.fold_all", "Fold all finished blocks", "", [this] { if (Pane* p = focused_pane()) for (int i = 0; i < p->block_count(); i++) p->set_fold(i, true); });
    add("block.unfold_all", "Unfold all blocks", "", [this] { if (Pane* p = focused_pane()) for (int i = 0; i < p->block_count(); i++) p->set_fold(i, false); });
    add("block.rerun", "Re-run selected block", "Ctrl+Shift+Enter", [this] { if (Pane* p = focused_pane()) p->rerun(p->selected_block >= 0 ? p->selected_block : p->block_count() - 2); });
    add("block.copy_output", "Copy selected block output", "", [this] { if (Pane* p = focused_pane()) if (p->selected_block >= 0) { set_clipboard(p->block_output(p->selected_block)); toast("Output copied"); } });
    add("block.pin", "Pin / unpin selected block", "", [this] { if (Pane* p = focused_pane()) if (p->selected_block >= 0 && p->session) { Block& b = p->session->blocks().blocks()[p->selected_block]; b.pinned = !b.pinned; } });
    add("block.json", "Pretty-print JSON in selected block", "Ctrl+Shift+J", [this] { if (Pane* p = focused_pane()) { int i = p->selected_block >= 0 ? p->selected_block : p->block_count() - 2; p->toggle_pretty_json(i); } });
    add("block.next_error", "Jump to next error line", "Ctrl+Shift+E", [this] { if (Pane* p = focused_pane()) p->next_error(1); });
    add("block.fork", "Run selected block in a child activity", "", [this] { if (Pane* p = focused_pane()) if (p->selected_block >= 0) run_in_child(focused_, p->block_command(p->selected_block), p->cwd()); });
    add("edit.copy", "Copy selection", "Ctrl+Shift+C", [this] { if (Pane* p = focused_pane()) if (p->has_selection()) set_clipboard(p->selected_text()); });
    add("edit.paste", "Paste", "Ctrl+Shift+V", [this] { if (Pane* p = focused_pane()) p->paste(clipboard()); });
    add("edit.search", "Search scrollback", "Ctrl+Shift+F", [this] { overlay_->open_search(); });
    add("edit.clear_selection", "Clear selection", "", [this] { if (Pane* p = focused_pane()) p->clear_selection(); });
    add("app.palette", "Command palette", "Ctrl+Shift+P", [this] { overlay_->open_palette(); });
    add("app.save", "Save workspace now", "", [this] { save_state(); toast("Workspace saved"); });
    add("app.reload_config", "Reload configuration", "", [this] { std::string e; cfg_.load("", &e); if (!e.empty()) toast(e); set_font_size(cfg_.font_size_px); });
    add("app.quit", "Quit Canopy", "", [this] { quit_ = true; });
}

void App::run_action(const std::string& id) {
    for (auto& a : actions_) if (a.id == id) { a.fn(); request_redraw(); return; }
}

bool App::handle_global_key(const KeyEvent& ev) {
    bool ctrl = ev.mods & MOD_CTRL, shift = ev.mods & MOD_SHIFT, alt = ev.mods & MOD_ALT;
    if (ev.key == Key::F1 && !ctrl && !alt) { run_action("view.overview"); return true; }
    if (ev.key == Key::F2 && !ctrl && !alt) { run_action("view.sidebar"); return true; }
    if (ev.key == Key::F10 && !ctrl && !alt) { run_action("view.help"); return true; }
    if (ev.key == Key::F3 && !ctrl && !alt) { run_action("view.rail"); return true; }
    if (ctrl && !alt && ev.key == Key::Tab) { if (switcher_->open()) switcher_->step(shift ? -1 : 1); else switcher_->show(); return true; }
    if (ev.key == Key::Escape && canvas_->overview()) { canvas_->exit_overview(); return true; }
    if (ctrl && alt && !shift) {
        if (ev.key == Key::Left) { canvas_->focus_direction(-1, 0); return true; }
        if (ev.key == Key::Right) { canvas_->focus_direction(1, 0); return true; }
        if (ev.key == Key::Up) { canvas_->focus_direction(0, -1); return true; }
        if (ev.key == Key::Down) { canvas_->focus_direction(0, 1); return true; }
    }
    if (shift && !ctrl && !alt && ev.key == Key::Insert) { run_action("edit.paste"); return true; }
    if (ctrl && !alt && ev.key == Key::Char) {
        char32_t k = ev.keysym;
        if (!shift) {
            if (k == '=' || k == '+') { run_action("view.zoom_in"); return true; }
            if (k == '-') { run_action("view.zoom_out"); return true; }
            if (k == '0') { run_action("view.zoom_reset"); return true; }
            return false;
        }
        switch (k) {
        case 'c': run_action("edit.copy"); return true;
        case 'v': run_action("edit.paste"); return true;
        case 'f': run_action("edit.search"); return true;
        case 'n': run_action("activity.new_child"); return true;
        case 't': run_action("activity.new_sibling"); return true;
        case 'w': run_action("activity.close"); return true;
        case 'r': run_action("activity.rename"); return true;
        case 'p': run_action("app.palette"); return true;
        case 'e': run_action("block.next_error"); return true;
        case 'j': run_action("block.json"); return true;
        case '=': case '+': run_action("view.font_bigger"); return true;
        case '-': case '_': run_action("view.font_smaller"); return true;
        default: break;
        }
    }
    if (ctrl && shift && !alt) {
        switch (ev.key) {
        case Key::Up: run_action("block.prev"); return true;
        case Key::Down: run_action("block.next"); return true;
        case Key::Left: run_action("block.fold"); return true;
        case Key::Right: run_action("block.unfold"); return true;
        case Key::Enter: run_action("block.rerun"); return true;
        default: break;
        }
    }
    return false;
}

// ---------------------------------------------------------------- events
void App::handle_event(const gfx::Event& ev) {
    using T = gfx::Event;
    switch (ev.type) {
    case T::Quit: quit_ = true; break;
    case T::Resize: request_redraw(); break;
    case T::Expose: request_redraw(); break;
    case T::Wake: break;
    case T::FocusIn: case T::FocusOut:
        if (Pane* p = focused_pane()) if (p->live()) p->session->term().notify_focus(ev.type == T::FocusIn);
        request_redraw(); break;
    case T::Key: {
        if (ev.repeat && (ev.key.key == Key::F1 || ev.key.key == Key::F2 || ev.key.key == Key::F3)) break;
        if (switcher_->open()) {
            bool ctrl = ev.key.mods & MOD_CTRL;
            if (ctrl && ev.key.key == Key::Tab) { switcher_->step((ev.key.mods & MOD_SHIFT) ? -1 : 1); break; }
            if (switcher_->key(ev.key)) break;
        }
        if (overlay_->active() && overlay_->key(ev.key)) break;
        if (handle_global_key(ev.key)) break;
        if (Pane* p = focused_pane()) {
            if (ev.key.key == Key::Enter && !p->live()) { ensure_session(*p); break; }
            p->key(ev.key);
        }
        break;
    }
    case T::Text:
        if (switcher_->open() && switcher_->text(ev.key.text)) break;
        if (overlay_->active() && overlay_->text(ev.key.text)) break;
        if (Pane* p = focused_pane()) { if (!p->live()) ensure_session(*p); p->text_input(ev.key.text); }
        break;
    case T::MouseDown:
        if (switcher_->open() && switcher_->mouse_down(ev.x, ev.y, ev.button)) break;
        if (overlay_->active() && overlay_->mouse_down(ev.x, ev.y, ev.button)) break;
        if (sidebar_->mouse_down(ev.x, ev.y, ev.button, ev.clicks, ev.mods)) break;
        if (rail_->mouse_down(ev.x, ev.y, ev.button, ev.clicks)) break;
        canvas_->mouse_down(ev.x, ev.y, ev.button, ev.clicks, ev.mods);
        break;
    case T::MouseUp:
        sidebar_->mouse_up(ev.x, ev.y, ev.button);
        canvas_->mouse_up(ev.x, ev.y, ev.button, ev.mods);
        break;
    case T::MouseMove:
        if (switcher_->open()) { switcher_->mouse_move(ev.x, ev.y); break; }
        if (overlay_->captures_mouse()) { overlay_->mouse_move(ev.x, ev.y); break; }
        sidebar_->mouse_move(ev.x, ev.y);
        rail_->mouse_move(ev.x, ev.y);
        if (rail_->visible && rail_->rect.contains(ev.x, ev.y)) { window_->set_cursor(SDL_SYSTEM_CURSOR_HAND); break; }
        canvas_->mouse_move(ev.x, ev.y, ev.mods, ev.button);
        window_->set_cursor(sidebar_->visible && sidebar_->rect.contains(ev.x, ev.y) ? SDL_SYSTEM_CURSOR_ARROW : canvas_->cursor_for(ev.x, ev.y));
        break;
    case T::Wheel:
        if (overlay_->active() && overlay_->wheel(ev.x, ev.y, ev.wheel_y)) break;
        if (switcher_->open()) break;
        if (sidebar_->visible && sidebar_->rect.contains(ev.x, ev.y)) { sidebar_->wheel(ev.wheel_y); break; }
        if (rail_->visible && rail_->rect.contains(ev.x, ev.y)) { rail_->wheel(ev.wheel_y); break; }
        canvas_->wheel(ev.x, ev.y, ev.wheel_x, ev.wheel_y, ev.mods);
        break;
    case T::DropFile:
        if (Pane* p = focused_pane()) p->write(shell_quote(ev.path) + " ");
        break;
    default: break;
    }
}

void App::check_notifications(double now) {
    // periodic save
    if (now - last_save_ > 45.0) save_state();
}

// ---------------------------------------------------------------- loop
int App::run() {
    std::vector<gfx::Event> events;
    double last = now();
    while (!quit_) {
        bool animating = canvas_->animating();
        bool blink = cfg_.cursor_blink && window_->focused();
        int timeout = animating ? 4 : (blink ? (int)(500 / std::max(0.2f, cfg_.cursor_blink_hz)) : 250);
        // any running command? refresh timers in chips
        for (auto& kv : panes_) if (kv.second->live()) { const auto& bl = kv.second->session->blocks().blocks(); if (!bl.empty() && bl.back().state == BlockState::Running) { timeout = std::min(timeout, 120); break; } }
        window_->poll(events, timeout);
        for (auto& ev : events) handle_event(ev);
        events.clear();
        double t = now();
        double dt = std::min(0.1, t - last);
        last = t;
        frame(t, dt);
    }
    save_state();
    summarizer_.stop();
    return 0;
}

void App::layout_chrome() {
    float W = (float)window_->fb_width(), H = (float)window_->fb_height();
    float sw = sidebar_->visible ? sidebar_->width : 0.f;
    float rw = rail_->visible ? std::min(rail_->width, W * 0.4f) : 0.f;
    sidebar_->rect = {0, 0, sw, H};
    rail_->rect = {W - rw, 0, rw, H};
    canvas_->viewport = {sw, 0, W - sw - rw, H};
}

const char* App::summary_backend_name() const { return cfg_.ai_summaries ? summarizer_.backend_name() : "off"; }

void App::request_summary(uint64_t id, bool force) {
    Pane* p = pane(id);
    if (!p) return;
    p->summary_dirty = true;
    p->summary_dirty_since = force ? now() - 10 : now();
    if (force) { p->summary_time = 0; p->summary_hash = 0; }
}

void App::pump_summaries(double t) {
    { std::string n = summarizer_.fallback_notice(); if (!n.empty()) { toast(n, 10.f); fprintf(stderr, "canopy: %s\n", n.c_str()); } }
    // results
    for (SummaryResult& res : summarizer_.take_results()) {
        Pane* p = pane(res.activity);
        if (!p) continue;
        p->summary_pending = false;
        if (res.ok) {
            p->summary = res.text; p->summary_ai = true; p->summary_hash = res.context_hash; p->summary_time = t;
            if (Activity* a = tree_.get(res.activity)) {
                a->summary = res.text; a->summary_ai = true; a->summary_time = (int64_t)time(nullptr);
                if (a->auto_named && !res.name.empty() && res.name != a->name) {
                    a->name = res.name;
                    if (res.activity == focused_) { title_ = a->name; window_->set_title(title_ + " — Canopy"); }
                    tree_.notify();
                }
            }
        }
        else { static int logged = 0; if (logged++ < 5) fprintf(stderr, "canopy: summary failed: %s\n", res.error.c_str()); p->summary_time = t; }
        redraw_ = true;
    }
    // placeholders + periodic refresh for live full-screen / long-running panes
    for (auto& kv : panes_) {
        Pane& p = *kv.second;
        if (p.summary.empty()) { p.summary = p.local_summary(); p.summary_ai = false; }
        if (p.live() && (p.session->term().modes().alt_screen || (p.block_count() && p.block(p.block_count() - 1)->state == BlockState::Running)) && t - p.summary_time > 60 && !p.summary_dirty) { p.summary_dirty = true; p.summary_dirty_since = t; }
    }
    if (!summarizer_.available() || summarizer_.busy() || summarizer_.queued() || t - last_summary_submit_ < 1.0) return;
    Pane* best = nullptr; float best_score = -1e9f;
    for (auto& kv : panes_) {
        Pane& p = *kv.second;
        if (!p.summary_dirty || p.summary_pending) continue;
        if (t - p.summary_dirty_since < 2.0) continue;                                    // debounce
        if (p.summary_time > 0 && t - p.summary_time < cfg_.ai_min_interval) continue;
        bool has_content = p.has_restored() || (p.live() && p.session->term().modes().alt_screen);
        for (int i = 0; i < p.block_count() && !has_content; i++) if (!p.block(i)->command.empty()) has_content = true;
        if (!has_content) { p.summary_dirty = false; continue; }
        float sc = p.attention_score(t, 0) + (kv.first == focused_ ? -5 : 0);
        if (sc > best_score) { best_score = sc; best = &p; }
    }
    if (!best) return;
    SummaryRequest req; req.activity = best->activity_id; req.context = best->summary_context(); req.context_hash = std::hash<std::string>{}(req.context);
    best->summary_dirty = false;
    if (req.context_hash == best->summary_hash) return;
    best->summary_pending = true;
    last_summary_submit_ = t;
    summarizer_.submit(std::move(req));
    redraw_ = true;
}

void App::frame(double t, double dt) {
    layout_chrome();
    for (auto& kv : panes_) if (kv.second->update(t)) redraw_ = true;
    pump_summaries(t);
    if (summarizer_.busy() && rail_->visible) redraw_ = true;   // spinner
    if (canvas_->update(t, dt)) redraw_ = true;
    if (overlay_->update(t)) redraw_ = true;
    // cursor blink
    bool blink = cfg_.cursor_blink && window_->focused();
    double period = 0.5 / std::max(0.2f, cfg_.cursor_blink_hz);
    if (blink) {
        bool on = fmod(t - start_time_, 2 * period) < period;
        if (on != cursor_on_) { cursor_on_ = on; redraw_ = true; }
    } else if (!cursor_on_) { cursor_on_ = true; redraw_ = true; }
    // running commands animate their timers
    for (auto& kv : panes_) if (kv.second->live()) { const auto& bl = kv.second->session->blocks().blocks(); if (!bl.empty() && bl.back().state == BlockState::Running) { redraw_ = true; break; } }
    check_notifications(t);
    if (redraw_) { draw(t); redraw_ = false; }
}

void App::draw(double t) {
    renderer_->begin_frame(window_->fb_width(), window_->fb_height(), col(cfg_.theme.canvas_bg));
    DrawCtx ctx;
    ctx.cfg = &cfg_; ctx.now = t; ctx.cursor_on = cursor_on_; ctx.window_focused = window_->focused();
    canvas_->draw(ctx);
    sidebar_->draw(ctx);
    rail_->draw(ctx);
    switcher_->draw(ctx);
    overlay_->draw(ctx);
    renderer_->end_frame();
    window_->swap();
}

} // namespace canopy::ui
