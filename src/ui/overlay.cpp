#include "overlay.h"
#include "canvas.h"
#include "pane.h"
#include <algorithm>

namespace canopy::ui {

static constexpr float MENU_ITEM_H = 26.f, MENU_W = 260.f, PAL_W = 640.f, PAL_ROW = 30.f;

Overlay::Overlay(App& app) : app_(app) {}

void Overlay::close() {
    if (mode_ == Search) if (Pane* p = app_.focused_pane()) p->set_search("");
    mode_ = None; menu_.clear(); cands_.clear(); all_.clear(); input_.clear(); prompt_cb_ = nullptr;
    app_.request_redraw();
}

void Overlay::open_menu(float sx, float sy, std::vector<MenuItem> items) {
    close();
    mode_ = Menu; menu_ = std::move(items); mx_ = sx; my_ = sy; menu_hover_ = -1;
    Rect r = menu_rect();
    float W = app_.window().fb_width(), H = app_.window().fb_height();
    if (r.x + r.w > W) mx_ -= r.w;
    if (r.y + r.h > H) my_ = std::max(0.f, H - r.h);
    app_.request_redraw();
}

void Overlay::open_prompt(const std::string& label, const std::string& initial, std::function<void(const std::string&)> on_done) {
    close();
    mode_ = Prompt; label_ = label; input_ = initial; prompt_cb_ = std::move(on_done); select_all_ = !initial.empty();
    app_.request_redraw();
}

void Overlay::open_search() {
    if (mode_ == Search) { if (Pane* p = app_.focused_pane()) p->search_step(1); return; }
    close();
    mode_ = Search; input_.clear();
    if (Pane* p = app_.focused_pane()) { if (p->has_selection()) { input_ = p->selected_text(); if (input_.find('\n') != std::string::npos) input_.clear(); } p->set_search(input_); }
    app_.request_redraw();
}

void Overlay::toggle_help() { if (mode_ == Help) close(); else { close(); mode_ = Help; } app_.request_redraw(); }

void Overlay::toast(const std::string& text, float seconds) {
    double now = app_.now();
    toasts_.push_back({text, now + seconds, now});
    if (toasts_.size() > 5) toasts_.erase(toasts_.begin());
    app_.request_redraw();
}

void Overlay::open_palette(const std::string& initial) {
    close();
    mode_ = Palette; input_ = initial; sel_ = 0;
    build_palette_candidates();
    refilter();
}

void Overlay::build_palette_candidates() {
    all_.clear();
    for (const Action& a : app_.actions()) {
        if (a.enabled && !a.enabled()) continue;
        std::string id = a.id;
        all_.push_back({a.label, "action", a.shortcut, [this, id] { app_.run_action(id); }});
    }
    ActivityTree& t = app_.tree();
    for (uint64_t id : t.preorder()) {
        Activity* a = t.get(id);
        if (!a) continue;
        std::string path;
        for (uint64_t cur = a->parent; cur; ) { Activity* p = t.get(cur); if (!p) break; path = p->name + " › " + path; cur = p->parent; }
        all_.push_back({"→ " + a->name, path.empty() ? "activity" : path, "", [this, id] { app_.focus(id, true); }});
    }
    // recent commands across panes (live + restored)
    struct Rec { uint64_t act; uint64_t block; std::string cmd; int64_t when; std::string actname; int exit; };
    std::vector<Rec> recs;
    for (auto& kv : app_.panes()) {
        Pane& p = *kv.second;
        Activity* a = t.get(kv.first);
        for (int i = 0; i < p.block_count(); i++) {
            const Block* b = p.block(i);
            if (b->command.empty()) continue;
            recs.push_back({kv.first, b->id, b->command, b->unix_start, a ? a->name : "", b->exit_code});
        }
    }
    std::sort(recs.begin(), recs.end(), [](const Rec& x, const Rec& y) { return x.when > y.when; });
    std::vector<std::string> seen;
    for (auto& r : recs) {
        if (std::find(seen.begin(), seen.end(), r.cmd) != seen.end()) continue;
        seen.push_back(r.cmd);
        if (seen.size() > 300) break;
        uint64_t act = r.act, bid = r.block;
        std::string cmd = r.cmd;
        all_.push_back({"$ " + r.cmd, r.actname + (r.exit == 0 ? "  ✓" : r.exit > 0 ? "  ✗" : ""), "", [this, act, bid, cmd] {
            app_.focus(act, true);
            if (Pane* p = app_.pane(act)) {
                for (int k = 0; k < p->block_count(); k++) if (p->block(k)->id == bid) { p->select_block(k, true); return; }
            }
        }});
    }
}

void Overlay::refilter() {
    cands_.clear();
    for (auto& c : all_) {
        int s = input_.empty() ? 1 : std::max(fuzzy_score(input_, c.label), fuzzy_score(input_, c.detail) / 2);
        if (s > 0) { Cand cc = c; cc.score = s; cands_.push_back(cc); }
    }
    if (!input_.empty()) std::stable_sort(cands_.begin(), cands_.end(), [](const Cand& a, const Cand& b) { return a.score > b.score; });
    if (cands_.size() > 14) cands_.resize(14);
    sel_ = std::min<int>(sel_, std::max(0, (int)cands_.size() - 1));
    app_.request_redraw();
}

bool Overlay::edit_key(const KeyEvent& ev) {
    bool ctrl = ev.mods & MOD_CTRL;
    if (ev.key == Key::Backspace) {
        if (ctrl) { while (!input_.empty() && input_.back() == ' ') input_.pop_back(); while (!input_.empty() && input_.back() != ' ') input_.pop_back(); }
        else if (!input_.empty()) { size_t i = input_.size() - 1; while (i > 0 && ((unsigned char)input_[i] & 0xC0) == 0x80) i--; input_.erase(i); }
        return true;
    }
    if (ctrl && ev.key == Key::Char && ev.keysym == 'u') { input_.clear(); return true; }
    if (ctrl && ev.key == Key::Char && ev.keysym == 'w') { while (!input_.empty() && input_.back() == ' ') input_.pop_back(); while (!input_.empty() && input_.back() != ' ') input_.pop_back(); return true; }
    if (ctrl && ev.key == Key::Char && ev.keysym == 'v') { input_ += app_.clipboard(); return true; }
    return false;
}

bool Overlay::key(const KeyEvent& ev) {
    if (mode_ == None) return false;
    if (ev.key == Key::Escape) { close(); return true; }
    switch (mode_) {
    case Menu:
        if (ev.key == Key::Up || ev.key == Key::Down) {
            int n = (int)menu_.size(); if (!n) return true;
            int d = ev.key == Key::Up ? -1 : 1;
            int i = menu_hover_;
            for (int k = 0; k < n; k++) { i = ((i + d) % n + n) % n; if (menu_[i].label != "-" && menu_[i].enabled) break; }
            menu_hover_ = i; app_.request_redraw(); return true;
        }
        if (ev.key == Key::Enter && menu_hover_ >= 0 && menu_hover_ < (int)menu_.size()) {
            auto fn = menu_[menu_hover_].fn; close(); if (fn) fn(); return true;
        }
        return true;
    case Palette:
        if (ev.key == Key::Up) { sel_ = std::max(0, sel_ - 1); app_.request_redraw(); return true; }
        if (ev.key == Key::Down) { sel_ = std::min((int)cands_.size() - 1, sel_ + 1); app_.request_redraw(); return true; }
        if (ev.key == Key::PageUp) { sel_ = 0; app_.request_redraw(); return true; }
        if (ev.key == Key::PageDown) { sel_ = std::max(0, (int)cands_.size() - 1); app_.request_redraw(); return true; }
        if (ev.key == Key::Enter) {
            if (sel_ >= 0 && sel_ < (int)cands_.size()) { auto fn = cands_[sel_].fn; close(); if (fn) fn(); }
            else close();
            return true;
        }
        if (edit_key(ev)) { refilter(); return true; }
        if (ev.key == Key::Char && !ev.text.empty() && !(ev.mods & MOD_CTRL)) { input_ += ev.text; refilter(); }
        return true;
    case Prompt:
        if (ev.key == Key::Enter) { auto cb = prompt_cb_; std::string v = input_; close(); if (cb) cb(v); return true; }
        if (ev.key == Key::Left || ev.key == Key::Right || ev.key == Key::Home || ev.key == Key::End) { select_all_ = false; app_.request_redraw(); return true; }
        if (select_all_ && (ev.key == Key::Backspace || ev.key == Key::Delete)) { input_.clear(); select_all_ = false; app_.request_redraw(); return true; }
        if (edit_key(ev)) { select_all_ = false; app_.request_redraw(); return true; }
        if (ev.key == Key::Char && !ev.text.empty() && !(ev.mods & MOD_CTRL)) { if (select_all_) { input_.clear(); select_all_ = false; } input_ += ev.text; app_.request_redraw(); }
        return true;
    case Search: {
        Pane* p = app_.focused_pane();
        if (ev.key == Key::Enter) { if (p) p->search_step((ev.mods & MOD_SHIFT) ? -1 : 1); app_.request_redraw(); return true; }
        if (ev.key == Key::Up) { if (p) p->search_step(-1); app_.request_redraw(); return true; }
        if (ev.key == Key::Down) { if (p) p->search_step(1); app_.request_redraw(); return true; }
        if (edit_key(ev)) { if (p) p->set_search(input_); app_.request_redraw(); return true; }
        if (ev.key == Key::Char && !ev.text.empty() && !(ev.mods & MOD_CTRL)) { input_ += ev.text; if (p) p->set_search(input_); app_.request_redraw(); return true; }
        // let the terminal keep receiving other keys? No: the search bar owns the keyboard while open.
        return true;
    }
    case Help:
        if (ev.key == Key::Down) help_scroll_++;
        if (ev.key == Key::Up) help_scroll_ = std::max(0, help_scroll_ - 1);
        app_.request_redraw();
        return true;
    default: return false;
    }
}

bool Overlay::text(const std::string& utf8) {
    if (mode_ == Palette) { input_ += utf8; refilter(); return true; }
    if (mode_ == Prompt) { if (select_all_) { input_.clear(); select_all_ = false; } input_ += utf8; app_.request_redraw(); return true; }
    if (mode_ == Search) { input_ += utf8; if (Pane* p = app_.focused_pane()) p->set_search(input_); app_.request_redraw(); return true; }
    if (mode_ == Menu || mode_ == Help) return true;
    return false;
}

Rect Overlay::menu_rect() const {
    float h = 8;
    for (auto& it : menu_) h += it.label == "-" ? 7 : MENU_ITEM_H;
    return {mx_, my_, MENU_W, h};
}
Rect Overlay::palette_rect() const {
    float W = app_.window().fb_width();
    float h = 52 + cands_.size() * PAL_ROW + 8;
    return {(W - PAL_W) / 2, 80, PAL_W, h};
}
Rect Overlay::search_rect() const {
    Rect vp = app_.canvas().viewport;
    return {vp.x + vp.w - 360, vp.y + 10, 350, 36};
}

bool Overlay::mouse_down(float sx, float sy, MouseButton b) {
    if (mode_ == Menu) {
        Rect r = menu_rect();
        if (!r.contains(sx, sy)) { close(); return true; }
        float y = r.y + 4;
        for (size_t i = 0; i < menu_.size(); i++) {
            float h = menu_[i].label == "-" ? 7 : MENU_ITEM_H;
            if (sy >= y && sy < y + h) {
                if (menu_[i].label != "-" && menu_[i].enabled && b == MouseButton::Left) { auto fn = menu_[i].fn; close(); if (fn) fn(); }
                return true;
            }
            y += h;
        }
        return true;
    }
    if (mode_ == Palette) {
        Rect r = palette_rect();
        if (!r.contains(sx, sy)) { close(); return true; }
        int idx = (int)((sy - (r.y + 52)) / PAL_ROW);
        if (idx >= 0 && idx < (int)cands_.size() && b == MouseButton::Left) { auto fn = cands_[idx].fn; close(); if (fn) fn(); }
        return true;
    }
    if (mode_ == Prompt || mode_ == Help) { close(); return true; }
    if (mode_ == Search) {
        if (search_rect().contains(sx, sy)) return true;
        return false;    // clicks elsewhere go through
    }
    return false;
}

void Overlay::mouse_move(float sx, float sy) {
    if (mode_ == Menu) {
        Rect r = menu_rect();
        int prev = menu_hover_;
        menu_hover_ = -1;
        if (r.contains(sx, sy)) {
            float y = r.y + 4;
            for (size_t i = 0; i < menu_.size(); i++) {
                float h = menu_[i].label == "-" ? 7 : MENU_ITEM_H;
                if (sy >= y && sy < y + h && menu_[i].label != "-") menu_hover_ = (int)i;
                y += h;
            }
        }
        if (prev != menu_hover_) app_.request_redraw();
    } else if (mode_ == Palette) {
        Rect r = palette_rect();
        if (r.contains(sx, sy)) {
            int idx = (int)((sy - (r.y + 52)) / PAL_ROW);
            if (idx >= 0 && idx < (int)cands_.size() && idx != sel_) { sel_ = idx; app_.request_redraw(); }
        }
    }
}

bool Overlay::wheel(float sx, float sy, float dy) {
    if (mode_ == Help) { help_scroll_ = std::max(0, help_scroll_ - (int)dy); app_.request_redraw(); return true; }
    return captures_mouse();
}

bool Overlay::update(double now) {
    size_t before = toasts_.size();
    toasts_.erase(std::remove_if(toasts_.begin(), toasts_.end(), [&](const Toast& t) { return t.expires < now; }), toasts_.end());
    bool animating = false;
    for (auto& t : toasts_) if (now - t.born < 0.3 || t.expires - now < 0.3) animating = true;
    return before != toasts_.size() || animating || (mode_ == Search);
}

void Overlay::draw(const DrawCtx& ctx) {
    gfx::Renderer& r = app_.renderer();
    gfx::Font& font = app_.font();
    const Theme& th = app_.theme();
    r.set_transform({1, 0, 0});
    float asc = font.metrics().ascent;
    RGBA fg = col(th.chrome_fg), bg = col(th.chrome_bg);
    float W = app_.window().fb_width(), H = app_.window().fb_height();

    // toasts (bottom-right)
    float ty = H - 20;
    for (size_t i = toasts_.size(); i-- > 0;) {
        const Toast& t = toasts_[i];
        float a = 1.f;
        if (ctx.now - t.born < 0.25) a = (float)((ctx.now - t.born) / 0.25);
        if (t.expires - ctx.now < 0.3) a = std::min(a, (float)((t.expires - ctx.now) / 0.3));
        float w = r.measure_text(font, t.text) + 28;
        float h = app_.cell_h() + 16;
        Rect b = {W - w - 20, ty - h, w, h};
        r.rounded_rect(b, 8, alpha(bg, 0.95f * a));
        r.rounded_rect_stroke(b, 8, alpha(col(th.accent), 0.5f * a), 1);
        r.draw_text(font, b.x + 14, b.y + 8 + asc, t.text, alpha(col(th.foreground), a));
        ty -= h + 8;
    }

    if (mode_ == Menu) {
        Rect m = menu_rect();
        r.rounded_rect({m.x + 2, m.y + 4, m.w, m.h}, 8, {0, 0, 0, 0.4f});
        r.rounded_rect(m, 8, bg);
        r.rounded_rect_stroke(m, 8, alpha(fg, 0.25f), 1);
        float y = m.y + 4;
        for (size_t i = 0; i < menu_.size(); i++) {
            const MenuItem& it = menu_[i];
            if (it.label == "-") { r.fill_rect({m.x + 10, y + 3, m.w - 20, 1}, alpha(fg, 0.2f)); y += 7; continue; }
            if ((int)i == menu_hover_ && it.enabled) r.rounded_rect({m.x + 4, y, m.w - 8, MENU_ITEM_H}, 5, alpha(col(th.accent), 0.3f));
            r.draw_text(font, m.x + 14, y + (MENU_ITEM_H - app_.cell_h()) / 2 + asc, it.label, it.enabled ? col(th.foreground) : alpha(fg, 0.4f));
            y += MENU_ITEM_H;
        }
    } else if (mode_ == Palette) {
        Rect p = palette_rect();
        r.fill_rect({0, 0, W, H}, {0, 0, 0, 0.35f});
        r.rounded_rect({p.x + 3, p.y + 6, p.w, p.h}, 12, {0, 0, 0, 0.5f});
        r.rounded_rect(p, 12, bg);
        r.rounded_rect_stroke(p, 12, alpha(col(th.accent), 0.6f), 1.5f);
        Rect in = {p.x + 12, p.y + 10, p.w - 24, 34};
        r.rounded_rect(in, 6, alpha(col(th.background), 0.9f));
        std::string shown = input_.empty() ? "Type a command, activity or action…" : input_;
        r.draw_text(font, in.x + 10, in.y + 8 + asc, shown, input_.empty() ? alpha(fg, 0.45f) : col(th.foreground));
        if (!input_.empty()) { float cw = r.measure_text(font, input_); r.fill_rect({in.x + 11 + cw, in.y + 7, 2, app_.cell_h()}, col(th.cursor)); }
        float y = p.y + 52;
        for (size_t i = 0; i < cands_.size(); i++) {
            const Cand& c = cands_[i];
            if ((int)i == sel_) r.rounded_rect({p.x + 8, y, p.w - 16, PAL_ROW}, 6, alpha(col(th.accent), 0.3f));
            r.push_clip({p.x + 8, y, p.w - 16, PAL_ROW});
            float x = r.draw_text(font, p.x + 20, y + (PAL_ROW - app_.cell_h()) / 2 + asc, ellipsize(c.label, 60), col(th.foreground));
            if (!c.detail.empty()) r.draw_text(font, p.x + 28 + x, y + (PAL_ROW - app_.cell_h()) / 2 + asc, ellipsize(c.detail, 40), alpha(fg, 0.55f), gfx::STYLE_REGULAR, 0.85f);
            if (!c.shortcut.empty()) { float sw = r.measure_text(font, c.shortcut, gfx::STYLE_REGULAR, 0.85f); r.draw_text(font, p.x + p.w - 20 - sw, y + (PAL_ROW - app_.cell_h()) / 2 + asc, c.shortcut, alpha(fg, 0.6f), gfx::STYLE_REGULAR, 0.85f); }
            r.pop_clip();
            y += PAL_ROW;
        }
        if (cands_.empty()) r.draw_text(font, p.x + 20, y + asc + 4, "No matches", alpha(fg, 0.5f));
    } else if (mode_ == Prompt) {
        float w = 520, h = 92;
        Rect p = {(W - w) / 2, 120, w, h};
        r.fill_rect({0, 0, W, H}, {0, 0, 0, 0.3f});
        r.rounded_rect(p, 12, bg);
        r.rounded_rect_stroke(p, 12, alpha(col(th.accent), 0.6f), 1.5f);
        r.draw_text(font, p.x + 16, p.y + 12 + asc, label_, alpha(fg, 0.8f), gfx::STYLE_BOLD);
        Rect in = {p.x + 16, p.y + 44, w - 32, 34};
        r.rounded_rect(in, 6, alpha(col(th.background), 0.9f));
        if (select_all_ && !input_.empty()) r.fill_rect({in.x + 9, in.y + 6, r.measure_text(font, input_) + 2, app_.cell_h() + 2}, alpha(col(th.selection), 0.9f));
        r.draw_text(font, in.x + 10, in.y + 8 + asc, input_, col(th.foreground));
        float cw = r.measure_text(font, input_);
        r.fill_rect({in.x + 11 + cw, in.y + 7, 2, app_.cell_h()}, col(th.cursor));
    } else if (mode_ == Search) {
        Rect s = search_rect();
        r.rounded_rect({s.x + 2, s.y + 3, s.w, s.h}, 8, {0, 0, 0, 0.35f});
        r.rounded_rect(s, 8, bg);
        r.rounded_rect_stroke(s, 8, alpha(col(th.accent), 0.6f), 1);
        r.draw_text(font, s.x + 10, s.y + 9 + asc, "⌕", fg);
        std::string shown = input_.empty() ? "find in scrollback" : input_;
        r.push_clip({s.x + 30, s.y, s.w - 110, s.h});
        r.draw_text(font, s.x + 32, s.y + 9 + asc, shown, input_.empty() ? alpha(fg, 0.45f) : col(th.foreground));
        r.pop_clip();
        if (!input_.empty()) { float cw = std::min(r.measure_text(font, input_), s.w - 114); r.fill_rect({s.x + 33 + cw, s.y + 8, 2, app_.cell_h()}, col(th.cursor)); }
        Pane* p = app_.focused_pane();
        std::string cnt = p && p->search_count() ? std::to_string(p->search_index() + 1) + "/" + std::to_string(p->search_count()) : (input_.empty() ? "" : "0/0");
        float cw = r.measure_text(font, cnt);
        r.draw_text(font, s.x + s.w - cw - 12, s.y + 9 + asc, cnt, p && p->search_count() ? col(th.accent) : alpha(fg, 0.6f));
    } else if (mode_ == Help) {
        static const char* lines[] = {
            "Canopy — keys",
            "",
            "Ctrl+Shift+N      new child activity        Ctrl+Shift+T      new sibling activity",
            "Ctrl+Shift+W      close activity            Ctrl+Shift+R      rename activity",
            "Ctrl+Alt+Arrows   move focus between panes  F1                overview (zoom out to all)",
            "F2                toggle sidebar            Ctrl+Shift+P      command palette",
            "Ctrl+Tab          find a terminal (miniatures + summaries)   F3   peek rail",
            "Ctrl+wheel        zoom canvas               Ctrl+= / Ctrl+-   zoom in / out, Ctrl+0 reset",
            "Ctrl+Shift+= / -  font size                 Alt+drag / middle-drag   pan the canvas",
            "",
            "Ctrl+Shift+Up/Dn  previous / next block     Ctrl+Shift+Left/Right  fold / unfold block",
            "Ctrl+Shift+Enter  re-run selected block     Ctrl+Shift+E      jump to next error line",
            "Ctrl+Shift+J      pretty-print JSON block   Click chip        fold / JSON toggle",
            "Gutter click      select block; double-click folds; right-click for block menu",
            "Table header      click to sort (again: descending, again: clear)",
            "",
            "Ctrl+Shift+C / V  copy / paste              Shift+Insert / middle click   paste primary",
            "Ctrl+Shift+F      search scrollback         Shift+PgUp/PgDn   scroll",
            "Ctrl+click        open path / URL           Right-click       context menu",
            "",
            "Shell:  canopy new <name> · canopy note <text> · canopy name <name> · canopy mark",
            "",
            "F10 or Esc closes this help",
        };
        int n = (int)(sizeof(lines) / sizeof(lines[0]));
        float lh = app_.cell_h() + 6;
        float w = 900, h = std::min(H - 80, 40 + n * lh);
        Rect p = {(W - w) / 2, 40, w, h};
        r.fill_rect({0, 0, W, H}, {0, 0, 0, 0.45f});
        r.rounded_rect(p, 12, bg);
        r.rounded_rect_stroke(p, 12, alpha(col(th.accent), 0.6f), 1.5f);
        r.push_clip(p.inset(4));
        float y = p.y + 20 - help_scroll_ * lh;
        for (int i = 0; i < n; i++) {
            r.draw_text(font, p.x + 24, y + asc, lines[i], i == 0 ? col(th.foreground) : fg, i == 0 ? gfx::STYLE_BOLD : gfx::STYLE_REGULAR);
            y += lh;
        }
        r.pop_clip();
    }
}

} // namespace canopy::ui
