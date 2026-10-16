#include "pane.h"
#include "canvas.h"
#include <SDL_mouse.h>
#include <algorithm>
#include <cmath>
#include <cstring>
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
Rect Pane::rect() const { return {x, y, GUTTER_W + cols * cell_w() + 2 * PAD, TITLE_H + rows * cell_h() + 2 * PAD}; }
Rect Pane::title_rect() const { return {x, y, rect().w, TITLE_H}; }
Rect Pane::term_rect() const { return {x + GUTTER_W + PAD, y + TITLE_H + PAD, cols * cell_w(), rows * cell_h()}; }
Rect Pane::gutter_rect() const { return {x + 2, y + TITLE_H + PAD, GUTTER_W + PAD - 2, rows * cell_h()}; }
Rect Pane::resize_handle() const { Rect r = rect(); return {r.x + r.w - HANDLE, r.y + r.h - HANDLE, HANDLE, HANDLE}; }

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
    session = std::make_unique<Session>();
    session->on_wake = [this] { app_.wake(); };
    std::string e;
    if (!session->start(o, &e)) { if (err) *err = e; err_ = e; session.reset(); return false; }
    Terminal& t = session->term();
    auto prev_cwd = t.on_cwd;
    t.on_cwd = [this, prev_cwd](const std::string& s) {
        if (prev_cwd) prev_cwd(s);
        if (Activity* a = app_.tree().get(activity_id)) a->cwd = s;
    };
    t.on_title = [this](const std::string& s) { last_title = s; app_.request_redraw(); };
    t.on_bell = [this] { app_.bell(activity_id); };
    t.on_clipboard_set = [this](const std::string& sel, const std::string& b64) {
        std::string data = base64_decode(b64);
        if (sel.find('p') != std::string::npos || sel.find('s') != std::string::npos) app_.set_primary(data);
        else app_.set_clipboard(data);
    };
    session->blocks().on_block_finished = [this](const Block& b) { last_finish = app_.now(); app_.on_block_finished(activity_id, b); };
    session->blocks().on_request = [this](const std::string& k, const std::string& p) { app_.on_shell_request(activity_id, k, p); };
    exited_at_ = 0;
    vis_dirty_ = true;
    last_gen_ = 0;
    last_history_base_ = 0;
    follow_ = true;
    return true;
}

void Pane::stop_session() {
    if (session) { session->stop(); session.reset(); }
    views_.clear();
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

void Pane::write(std::string_view bytes) {
    if (live()) session->write(bytes);
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

// ---------------------------------------------------------------- update
bool Pane::update(double now) {
    if (!session) return false;
    bool changed = session->pump();
    Terminal& t = session->term();
    uint64_t gen = t.generation();
    if (gen != last_gen_) {
        last_gen_ = gen;
        last_output = now;
        if (app_.focused_id() != activity_id) unread = true;
        vis_dirty_ = true;
        changed = true;
        if (!matches_.empty()) set_search(search_);
    }
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
        VisRow r; r.kind = VisRow::Restored; r.vidx = (int)i; r.block = restored_block_of_[i]; vis_.push_back(r);
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
    // binary search over Term rows (abs is non-decreasing except sorted tables; fall back to linear)
    size_t lo = 0, hi = vis_.size();
    while (lo < hi) {
        size_t mid = (lo + hi) / 2;
        int64_t v = vis_[mid].kind == VisRow::Restored ? -1 : vis_[mid].abs;
        if (v < abs) lo = mid + 1; else hi = mid;
    }
    if (lo < vis_.size() && vis_[lo].kind == VisRow::Term && vis_[lo].abs == abs) return (int)lo;
    for (size_t i = 0; i < vis_.size(); i++) if (vis_[i].kind == VisRow::Term && vis_[i].abs == abs) return (int)i;
    return -1;
}

void Pane::clamp_scroll() {
    int64_t maxtop = std::max<int64_t>(0, (int64_t)vis_.size() - rows);
    if (follow_) top_ = maxtop;
    if (top_ > maxtop) top_ = maxtop;
    if (top_ < 0) top_ = 0;
    if (top_ == maxtop) follow_ = true;
}

void Pane::scroll_lines(int delta) {
    vis();
    if (session && session->term().modes().alt_screen) return;
    top_ += delta;
    follow_ = false;
    clamp_scroll();
}
void Pane::scroll_pages(int delta) { scroll_lines(delta * std::max(1, rows - 2)); }
void Pane::scroll_to_bottom() { follow_ = true; clamp_scroll(); }
void Pane::scroll_to_abs(int64_t abs) {
    vis();
    int r = row_of_abs(abs);
    if (r < 0) return;
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
    if (sel_.empty() || !session) return "";
    int64_t l0, l1; int c0, c1;
    sel_.ordered(l0, c0, l1, c1);
    if (sel_.mode == 2) { c0 = 0; c1 = cols - 1; }
    return session->term().text(l0, c0, l1, c1 + 1);
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
    if (q.empty() || !session) return;
    std::string nq = to_lower(q);
    const Terminal& t = session->term();
    int64_t end = t.modes().alt_screen ? 0 : t.screen_base() + t.rows();
    for (int64_t a = t.history_base(); a < end && matches_.size() < 5000; a++) {
        std::string txt = t.line_abs(a).text();
        if (txt.empty()) continue;
        std::string lt = to_lower(txt);
        size_t pos = 0;
        while ((pos = lt.find(nq, pos)) != std::string::npos) {
            int col = (int)utf8_length(std::string_view(txt).substr(0, pos));
            matches_.push_back({a, col, (int)utf8_length(nq)});
            pos += nq.size();
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
        if (!vr || vr->kind != VisRow::Term) { clear_selection(); return; }
        sel_ = {};
        sel_.active = true; sel_.mode = clicks >= 3 ? 2 : clicks == 2 ? 1 : 0;
        sel_.al = sel_.hl = vr->abs; sel_.ac = sel_.hc = c;
        if (sel_.mode == 1 && session) {
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
        if (bi >= 0 && block(bi)) {
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
        if (vi >= 0 && vi < (int64_t)vis_.size() && vis_[vi].kind == VisRow::Term) extend_selection(vis_[vi].abs, c);
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
    Rect R = rect();
    if (!R.contains(wx, wy)) return;
    hover.valid = true;
    if (resize_handle().contains(wx, wy)) { hover.handle = true; return; }
    if (title_rect().contains(wx, wy)) { hover.title = true; return; }
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
    if (hover.title) return SDL_SYSTEM_CURSOR_ARROW;
    if (hover.gutter || hover.summary || hover.chip || hover.header) return SDL_SYSTEM_CURSOR_HAND;
    if (hover.link || hover.span) return SDL_SYSTEM_CURSOR_HAND;
    if (hover.valid && hover.row >= 0) return SDL_SYSTEM_CURSOR_IBEAM;
    return SDL_SYSTEM_CURSOR_ARROW;
}

std::string Pane::status_text() const {
    if (!session) return has_restored() ? "dormant" : "not started";
    if (!session->alive()) return "exited";
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
        r.rounded_rect_stroke(R, RADIUS, ctx.focused ? col(th.accent) : alpha(col(th.chrome_fg), 0.35f), ctx.focused ? 2.f : 1.f);
        return;
    }

    draw_title(r, font, ctx);
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
        for (int i = 0; i < count; i++) {
            const VisRow& vr = v[top_ + i];
            if (vr.kind != VisRow::Term || vr.block < 0) continue;
            const Block* b = block(vr.block);
            if (b && b->cmd_line == vr.abs && (b->state == BlockState::Running || b->state == BlockState::Done)) draw_chip(r, font, ctx, *b, T.y + i * cell_h());
        }
    }
    draw_cursor(r, ctx);
    r.pop_clip();
    draw_scrollbar(r, ctx);
    if (!session || !session->alive()) draw_dormant(r, font, ctx);
    // border
    RGBA border = ctx.focused ? col(th.accent) : alpha(col(th.chrome_fg), 0.3f);
    r.rounded_rect_stroke(R, RADIUS, border, ctx.focused ? 1.5f : 1.f);
    // resize handle
    if (s > 0.5f) {
        Rect h = resize_handle();
        RGBA hc = alpha(col(th.chrome_fg), hover.handle ? 0.9f : 0.35f);
        r.line(h.x + h.w - 3, h.y + h.h - 9, h.x + h.w - 9, h.y + h.h - 3, hc, 1.f);
        r.line(h.x + h.w - 3, h.y + h.h - 5, h.x + h.w - 5, h.y + h.h - 3, hc, 1.f);
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
    if (session && session->alive()) {
        const auto& bl = session->blocks().blocks();
        if (!bl.empty() && bl.back().state == BlockState::Running) { running = true; dot = col(th.accent); }
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
    r.circle(x0 + 5, t.y + TITLE_H / 2, running ? 5 : 4, dot);
    if (running) r.circle(x0 + 5, t.y + TITLE_H / 2, 7 + 2 * (float)std::sin(ctx.now * 6), alpha(dot, 0.25f));
    float x = x0 + 18;
    x += r.draw_text(font, x, base, name, ctx.focused ? col(th.foreground) : fg, gfx::STYLE_BOLD) + 10;
    std::string sub = pretty_path(cwd(), 40);
    if (!last_title.empty() && last_title != name && last_title.find(sub) == std::string::npos) sub += "  ·  " + ellipsize(last_title, 40);
    float right = t.x + t.w - 12;
    float avail = right - x - 90;
    if (avail > 40) {
        r.push_clip({x, t.y, avail, TITLE_H});
        r.draw_text(font, x, base, sub, alpha(fg, 0.7f));
        r.pop_clip();
    }
    // right side: running time or unread badge, child count
    std::string rt;
    if (running) {
        const Block& b = session->blocks().blocks().back();
        rt = std::string(SPINNER[(int)(ctx.now * 8) % 4]) + " " + fmt_duration(ctx.now - b.start_time);
    } else if (unread && !ctx.focused) rt = "● new";
    if (!rt.empty()) {
        float w = r.measure_text(font, rt);
        r.draw_text(font, right - w, base, rt, unread && !running ? col(th.warning) : col(th.accent));
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
    // search matches on this row
    const Match* mstart = nullptr; size_t mcount = 0;
    if (!matches_.empty() && vr.kind == VisRow::Term) {
        auto it = std::lower_bound(matches_.begin(), matches_.end(), vr.abs, [](const Match& m, int64_t a) { return m.abs < a; });
        if (it != matches_.end() && it->abs == vr.abs) { mstart = &*it; while (it != matches_.end() && it->abs == vr.abs) { it++; mcount++; } }
    }
    int cur_match_col = -1, cur_match_len = 0;
    if (match_idx_ >= 0 && match_idx_ < (int)matches_.size() && matches_[match_idx_].abs == vr.abs && vr.kind == VisRow::Term) { cur_match_col = matches_[match_idx_].col; cur_match_len = matches_[match_idx_].len; }
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
        if (vr.kind == VisRow::Term && selected(vr.abs, c)) { bgc = selc; draw = true; }
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
        if (vr.kind == VisRow::Term && selected(vr.abs, c)) fg = col(th.foreground);
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
            float uy = y0 + fm.ascent + fm.underline_pos;
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
        if (cell.attrs & ATTR_STRIKE) r.fill_rect({cx, y0 + fm.ascent - fm.strike_pos, w, 1.f}, fg);
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

void Pane::draw_dormant(Renderer& r, Font& font, const DrawCtx& ctx) {
    const Theme& th = app_.theme();
    Rect T = term_rect();
    if (ctx.scale * cell_h() < 4.f) return;
    std::string msg;
    if (!session) msg = err_.empty() ? (has_restored() ? "dormant · press Enter or click to resume the shell here" : "press Enter to start a shell") : "failed to start shell: " + err_;
    else msg = "shell exited (" + std::to_string(session->exit_status()) + ") · press Enter to restart";
    float w = r.measure_text(font, msg) + 24;
    float h = cell_h() + 12;
    Rect box = {T.x + (T.w - w) / 2, T.y + T.h - h - 12, w, h};
    r.rounded_rect(box, 6, alpha(col(th.chrome_bg), 0.95f));
    r.rounded_rect_stroke(box, 6, alpha(col(th.accent), 0.6f), 1.f);
    r.draw_text(font, box.x + 12, box.y + 6 + font.metrics().ascent, msg, col(th.chrome_fg));
}

} // namespace canopy::ui
