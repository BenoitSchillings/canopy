#include "switcher.h"
#include "peek.h"
#include <algorithm>
#include <cmath>

namespace canopy::ui {

Switcher::Switcher(App& app) : app_(app) {}

void Switcher::show() {
    open_ = true; query_.clear();
    rebuild();
    // preselect the most relevant other terminal (MRU-like)
    sel_ = 0;
    uint64_t prev = app_.mru().size() > 1 ? app_.mru()[1] : 0;
    bool found = false;
    for (size_t i = 0; i < entries_.size() && !found; i++) if (entries_[i].id == prev) { sel_ = (int)i; found = true; }
    for (size_t i = 0; i < entries_.size() && !found; i++) if (entries_[i].id != app_.focused_id()) { sel_ = (int)i; found = true; }
    app_.request_redraw();
}

void Switcher::close() { open_ = false; app_.request_redraw(); }

void Switcher::rebuild() {
    entries_.clear();
    double now = app_.now();
    uint64_t foc = app_.focused_id();
    for (auto& kv : app_.panes()) {
        Pane& p = *kv.second;
        Activity* a = app_.tree().get(kv.first);
        if (!a) continue;
        int score = 1;
        if (!query_.empty()) {
            std::string hay = a->name + " " + (p.summary.empty() ? p.local_summary() : p.summary) + " " + pretty_path(p.cwd(), 60) + " " + a->notes;
            if (p.live()) { std::string fg = p.session->pty().foreground_name(); if (!fg.empty()) hay += " " + fg; if (!p.last_title.empty()) hay += " " + p.last_title; }
            for (auto& b : p.attention_badges(now)) hay += " " + b;
            for (int i = std::max(0, p.block_count() - 6); i < p.block_count(); i++) hay += " " + p.block(i)->command;
            std::string lq = to_lower(query_), lh = to_lower(hay);
            size_t at = lh.find(lq);
            if (at != std::string::npos) score = 100000 - (int)std::min<size_t>(at, 50000) + (at == 0 || lh[at - 1] == ' ' ? 20000 : 0);
            else { score = fuzzy_score(query_, hay); if (score <= 0) continue; }
        }
        entries_.push_back({kv.first, score, {}});
    }
    std::stable_sort(entries_.begin(), entries_.end(), [&](const Entry& x, const Entry& y) {
        if (!query_.empty() && x.score != y.score) return x.score > y.score;
        Pane* px = app_.pane(x.id); Pane* py = app_.pane(y.id);
        return px->attention_score(now, foc) > py->attention_score(now, foc);
    });
    sel_ = std::max(0, std::min(sel_, (int)entries_.size() - 1));
}

void Switcher::step(int delta) {
    if (entries_.empty()) return;
    sel_ = (int)(((size_t)(sel_ + delta) + entries_.size()) % entries_.size());
    app_.request_redraw();
}

bool Switcher::key(const KeyEvent& ev) {
    if (!open_) return false;
    bool ctrl = ev.mods & MOD_CTRL;
    if (ev.key == Key::Escape) { close(); return true; }
    if (ev.key == Key::Enter) { if (sel_ >= 0 && sel_ < (int)entries_.size()) { uint64_t id = entries_[sel_].id; close(); app_.focus(id, true); } else close(); return true; }
    if (ev.key == Key::Tab) { step((ev.mods & MOD_SHIFT) ? -1 : 1); return true; }
    if (ev.key == Key::Right) { step(1); return true; }
    if (ev.key == Key::Left) { step(-1); return true; }
    if (ev.key == Key::Down) { step(cols_); return true; }
    if (ev.key == Key::Up) { step(-cols_); return true; }
    if (ev.key == Key::Backspace) {
        if (!query_.empty()) { size_t i = query_.size() - 1; while (i > 0 && ((unsigned char)query_[i] & 0xC0) == 0x80) i--; query_.erase(i); rebuild(); }
        return true;
    }
    if (ctrl && ev.key == Key::Char && ev.keysym == 'u') { query_.clear(); rebuild(); return true; }
    if (ev.key == Key::Char && !ev.text.empty() && !ctrl) { query_ += ev.text; rebuild(); return true; }
    return true;
}

bool Switcher::text(const std::string& utf8) {
    if (!open_) return false;
    query_ += utf8; rebuild(); app_.request_redraw();
    return true;
}

bool Switcher::mouse_down(float sx, float sy, MouseButton b) {
    if (!open_) return false;
    for (size_t i = 0; i < entries_.size(); i++) if (entries_[i].r.contains(sx, sy)) {
        if (b == MouseButton::Left) { uint64_t id = entries_[i].id; close(); app_.focus(id, true); }
        return true;
    }
    close();
    return true;
}

void Switcher::mouse_move(float sx, float sy) {
    if (!open_) return;
    for (size_t i = 0; i < entries_.size(); i++) if (entries_[i].r.contains(sx, sy)) { if ((int)i != sel_) { sel_ = (int)i; app_.request_redraw(); } return; }
}

void Switcher::draw(const DrawCtx& ctx) {
    if (!open_) return;
    gfx::Renderer& r = app_.renderer();
    gfx::Font& font = app_.font();
    const Theme& th = app_.theme();
    float W = app_.window().fb_width(), H = app_.window().fb_height();
    r.set_transform({1, 0, 0});
    r.fill_rect({0, 0, W, H}, {0, 0, 0, 0.72f});
    float asc = font.metrics().ascent;
    RGBA fg = col(th.chrome_fg);
    // search field
    float margin = 60;
    Rect in = {margin, 36, W - 2 * margin, 40};
    r.rounded_rect(in, 8, col(th.chrome_bg));
    r.rounded_rect_stroke(in, 8, alpha(col(th.accent), 0.7f), 1.5f);
    r.draw_text(font, in.x + 14, in.y + 11 + asc, "⌕", fg);
    std::string shown = query_.empty() ? "Which terminal?  type to filter by name, summary, directory or recent commands" : query_;
    r.draw_text(font, in.x + 40, in.y + 11 + asc, shown, query_.empty() ? alpha(fg, 0.45f) : col(th.foreground));
    if (!query_.empty()) { float cw = r.measure_text(font, query_); r.fill_rect({in.x + 41 + cw, in.y + 10, 2, app_.cell_h()}, col(th.cursor)); }
    std::string hint = "Tab / arrows move · Enter opens · Esc cancels";
    float hw = r.measure_text(font, hint, gfx::STYLE_REGULAR, 0.8f);
    r.draw_text(font, W - margin - hw, in.y + in.h + 18 + asc * 0.8f, hint, alpha(fg, 0.5f), gfx::STYLE_REGULAR, 0.8f);
    // grid
    float card_w = 400, gap = 20;
    cols_ = std::max(1, (int)((W - 2 * margin + gap) / (card_w + gap)));
    card_w = (W - 2 * margin - gap * (cols_ - 1)) / cols_;
    float thumb_h = card_w * 0.5f;
    float lh = app_.cell_h() * 0.9f + 2;
    float card_h = 10 + 24 + thumb_h + 8 + 2 * lh + 26 + 10;
    float y0 = in.y + in.h + 44;
    r.push_clip({0, y0 - 4, W, H - y0});
    int row_off = 0;
    if (!entries_.empty()) {
        int sel_row = sel_ / cols_;
        int rows_fit = std::max(1, (int)((H - y0) / (card_h + gap)));
        if (sel_row >= rows_fit) row_off = sel_row - rows_fit + 1;
    }
    for (size_t i = 0; i < entries_.size(); i++) {
        Entry& e = entries_[i];
        int row = (int)i / cols_ - row_off, c = (int)i % cols_;
        e.r = {margin + c * (card_w + gap), y0 + row * (card_h + gap), card_w, card_h};
        if (row < 0 || e.r.y > H) continue;
        Pane* p = app_.pane(e.id); Activity* a = app_.tree().get(e.id);
        if (!p || !a) continue;
        bool sel = (int)i == sel_;
        bool current = e.id == app_.focused_id();
        r.rounded_rect({e.r.x + 2, e.r.y + 4, e.r.w, e.r.h}, 10, {0, 0, 0, 0.4f});
        r.rounded_rect(e.r, 10, sel ? mix(col(th.chrome_bg), col(th.accent), 0.18f) : col(th.chrome_bg));
        r.rounded_rect_stroke(e.r, 10, sel ? col(th.accent) : alpha(fg, 0.2f), sel ? 2.f : 1.f);
        float x = e.r.x + 12, y = e.r.y + 10;
        // header
        RGBA dot = alpha(fg, 0.35f);
        if (p->live()) {
            const auto& bl = p->session->blocks().blocks();
            dot = col(th.ok);
            for (size_t k = bl.size(); k-- > 0;) { if (bl[k].state == BlockState::Running) { dot = col(th.accent); break; } if (bl[k].state == BlockState::Done) { dot = bl[k].exit_code == 0 ? col(th.ok) : col(th.error); break; } }
        } else if (p->session) dot = col(th.error);
        r.circle(x + 5, y + 9, 4.5f, dot);
        r.push_clip({x + 16, y, e.r.w - 150, 22});
        r.draw_text(font, x + 16, y + asc, a->name + (current ? "  (current)" : ""), col(th.foreground), gfx::STYLE_BOLD);
        r.pop_clip();
        std::string dir = pretty_path(p->cwd(), 24);
        float dw = r.measure_text(font, dir, gfx::STYLE_REGULAR, 0.8f);
        r.draw_text(font, e.r.x + e.r.w - 12 - dw, y + asc, dir, alpha(fg, 0.6f), gfx::STYLE_REGULAR, 0.8f);
        // thumbnail
        Rect tb = {x, y + 24, e.r.w - 24, thumb_h};
        PeekRail::draw_miniature(app_, *p, tb, ctx, true);
        r.rounded_rect_stroke(tb, 3, alpha(fg, 0.15f), 1.f);
        // summary
        std::string text = p->summary.empty() ? p->local_summary() : p->summary;
        auto lines = PeekRail::wrap_text(r, font, text, e.r.w - 24, 0.9f, 2);
        float ty = tb.y + tb.h + 8;
        for (auto& l : lines) { r.draw_text(font, x, ty + asc * 0.9f, l, col(th.foreground), p->summary_ai ? gfx::STYLE_REGULAR : gfx::STYLE_ITALIC, 0.9f); ty += lh; }
        ty = tb.y + tb.h + 8 + 2 * lh;
        // badges
        float bx = x;
        for (auto& b : p->attention_badges(ctx.now)) {
            float w = r.measure_text(font, b, gfx::STYLE_REGULAR, 0.75f) + 12;
            if (bx + w > e.r.x + e.r.w - 12) break;
            RGBA bc = b.rfind("✗", 0) == 0 ? col(th.error) : b.rfind("●", 0) == 0 ? col(th.warning) : b.rfind("⟳", 0) == 0 ? col(th.accent) : alpha(fg, 0.7f);
            r.rounded_rect({bx, ty + 4, w, 16}, 8, alpha(bc, 0.18f));
            r.draw_text(font, bx + 6, ty + 4 + asc * 0.75f + 1, b, bc, gfx::STYLE_REGULAR, 0.75f);
            bx += w + 6;
        }
        // number hint
        if (i < 9) { std::string n = std::to_string(i + 1); float nw = r.measure_text(font, n, gfx::STYLE_BOLD, 0.8f); r.draw_text(font, e.r.x + e.r.w - 12 - nw, ty + 4 + asc * 0.8f, n, alpha(fg, 0.5f), gfx::STYLE_BOLD, 0.8f); }
    }
    if (entries_.empty()) r.draw_text(font, margin, y0 + 20 + asc, "No terminal matches", alpha(fg, 0.6f));
    r.pop_clip();
}

} // namespace canopy::ui
