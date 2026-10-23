#include "canvas.h"
#include <SDL_mouse.h>
#include <algorithm>
#include <cmath>

namespace canopy::ui {

Canvas::Canvas(App& app) : app_(app) {}

bool Canvas::animating() const {
    return std::fabs(scale - tscale_) > 1e-4f || std::fabs(tx - ttx_) > 0.3f || std::fabs(ty - tty_) > 0.3f;
}

void Canvas::animate_to(float s, float x, float y, bool instant) {
    tscale_ = clampf(s, 0.04f, 4.f); ttx_ = x; tty_ = y;
    if (instant || !app_.config().animations) { scale = tscale_; tx = ttx_; ty = tty_; }
    app_.request_redraw();
}

void Canvas::fly_to_pane(Pane& p, bool instant) {
    Rect R = p.rect();
    if (p.drawer_open() && !p.minimized()) {
        Rect D = p.drawer_rect();
        float full_w = (D.x + D.w) - R.x;
        if ((viewport.w - 48.f) / full_w >= 0.72f) {
            R.w = full_w;
            R.h = std::max(R.h, D.h);
        }
    }
    float margin = 24.f;
    float fit = std::min((viewport.w - 2 * margin) / R.w, (viewport.h - 2 * margin) / R.h);
    float s = std::min(1.f, fit);
    float x = viewport.x + (viewport.w - R.w * s) / 2 - R.x * s;
    float y = viewport.y + (viewport.h - R.h * s) / 2 - R.y * s;
    if (R.h * s > viewport.h - 2 * margin) y = viewport.y + margin - R.y * s;
    animate_to(s, x, y, instant);
}

void Canvas::ensure_visible(Pane& p) {
    Rect R = p.rect();
    float s = tscale_;
    float x0 = R.x * s + ttx_, y0 = R.y * s + tty_, x1 = x0 + R.w * s, y1 = y0 + R.h * s;
    float nx = ttx_, ny = tty_, m = 20.f;
    if (x1 > viewport.x + viewport.w - m) nx -= x1 - (viewport.x + viewport.w - m);
    if (x0 - (x1 - x0) * 0 < viewport.x + m) nx += (viewport.x + m) - x0;
    if (y1 > viewport.y + viewport.h - m) ny -= y1 - (viewport.y + viewport.h - m);
    if (y0 < viewport.y + m) ny += (viewport.y + m) - y0;
    if (R.w * s > viewport.w - 2 * m || R.h * s > viewport.h - 2 * m) { fly_to_pane(p); return; }
    if (nx != ttx_ || ny != tty_) animate_to(s, nx, ny, false);
}

void Canvas::fit_all(bool instant) {
    float x0 = 1e9f, y0 = 1e9f, x1 = -1e9f, y1 = -1e9f;
    for (auto& kv : app_.panes()) {
        Rect R = kv.second->rect();
        x0 = std::min(x0, R.x); y0 = std::min(y0, R.y); x1 = std::max(x1, R.x + R.w); y1 = std::max(y1, R.y + R.h);
    }
    if (x0 > x1) return;
    float margin = 60.f;
    float s = std::min((viewport.w - 2 * margin) / (x1 - x0), (viewport.h - 2 * margin) / (y1 - y0));
    s = std::min(s, 1.f);
    float x = viewport.x + (viewport.w - (x1 - x0) * s) / 2 - x0 * s;
    float y = viewport.y + (viewport.h - (y1 - y0) * s) / 2 - y0 * s;
    animate_to(s, x, y, instant);
}

void Canvas::zoom_at(float sx, float sy, float factor) {
    float ns = clampf(tscale_ * factor, 0.04f, 4.f);
    factor = ns / tscale_;
    // keep the world point under (sx,sy) fixed (relative to target transform)
    float wx = (sx - ttx_) / tscale_, wy = (sy - tty_) / tscale_;
    float nx = sx - wx * ns, ny = sy - wy * ns;
    overview_ = false;
    animate_to(ns, nx, ny, false);
}

void Canvas::set_zoom(float z) { zoom_at(viewport.x + viewport.w / 2, viewport.y + viewport.h / 2, z / tscale_); }

void Canvas::reset_zoom() {
    overview_ = false;
    if (Pane* p = app_.focused_pane()) fly_to_pane(*p); else set_zoom(1.f);
}

std::vector<uint64_t> Canvas::ranked_panes(double now) const {
    std::vector<uint64_t> others;
    uint64_t foc = app_.focused_id();
    for (auto& kv : app_.panes()) {
        uint64_t id = kv.first;
        Activity* a = app_.tree().get(id);
        bool hidden = false;
        for (uint64_t anc = a ? a->parent : 0; anc; ) {
            Activity* an = app_.tree().get(anc);
            if (!an) break;
            if (an->collapsed) { hidden = true; break; }
            anc = an->parent;
        }
        if (hidden) continue;
        if (id != foc) others.push_back(id);
    }
    std::stable_sort(others.begin(), others.end(), [&](uint64_t x, uint64_t y) {
        Pane* px = app_.pane(x);
        Pane* py = app_.pane(y);
        return (px ? px->attention_score(now, foc) : -1e9f) > (py ? py->attention_score(now, foc) : -1e9f);
    });
    if (foc && app_.pane(foc)) others.push_back(foc);
    return others;
}

void Canvas::toggle_overview() {
    if (overview_) { exit_overview(); return; }
    overview_ = true;
    auto rk = ranked_panes(app_.now());
    overview_sel_ = rk.empty() ? app_.focused_id() : rk[0];
    pre_overview_scale_ = tscale_; pre_overview_tx_ = ttx_; pre_overview_ty_ = tty_;
    fit_all(false);
}

void Canvas::exit_overview() {
    if (!overview_) return;
    overview_ = false;
    if (Pane* p = app_.focused_pane()) fly_to_pane(*p);
    else animate_to(pre_overview_scale_, pre_overview_tx_, pre_overview_ty_, false);
}

bool Canvas::key(const KeyEvent& ev) {
    if (!overview_) return false;
    auto rk = ranked_panes(app_.now());
    if (rk.empty()) { exit_overview(); return true; }
    if (std::find(rk.begin(), rk.end(), overview_sel_) == rk.end()) overview_sel_ = rk[0];

    if (ev.key == Key::Escape) { exit_overview(); return true; }
    if (ev.key == Key::Enter || (ev.key == Key::Char && ev.keysym == ' ' && !(ev.mods & (MOD_CTRL | MOD_ALT)))) {
        uint64_t target = overview_sel_ ? overview_sel_ : rk[0];
        overview_ = false;
        if (Pane* p = app_.pane(target); p && p->minimized()) p->set_minimized(false);
        app_.focus(target, true);
        return true;
    }
    if (ev.key == Key::Tab) {
        auto it = std::find(rk.begin(), rk.end(), overview_sel_);
        size_t idx = (it == rk.end()) ? 0 : (size_t)(it - rk.begin());
        int d = (ev.mods & MOD_SHIFT) ? -1 : 1;
        idx = (idx + rk.size() + d) % rk.size();
        overview_sel_ = rk[idx];
        app_.request_redraw();
        return true;
    }
    if (ev.key == Key::Left || ev.key == Key::Right || ev.key == Key::Up || ev.key == Key::Down) {
        int dx = (ev.key == Key::Right ? 1 : ev.key == Key::Left ? -1 : 0);
        int dy = (ev.key == Key::Down ? 1 : ev.key == Key::Up ? -1 : 0);
        Pane* cur = app_.pane(overview_sel_);
        if (cur) {
            Rect C = cur->rect();
            float cx = C.x + C.w / 2.f, cy = C.y + C.h / 2.f;
            uint64_t best = 0; float best_d = 1e18f;
            for (uint64_t id : rk) {
                if (id == overview_sel_) continue;
                Pane* p = app_.pane(id);
                if (!p) continue;
                Rect R = p->rect();
                float px = R.x + R.w / 2.f, py = R.y + R.h / 2.f;
                float vx = px - cx, vy = py - cy;
                float along = vx * dx + vy * dy;
                if (along <= 1.f) continue;
                float across = std::fabs(vx * dy) + std::fabs(vy * dx);
                float d = along + across * 2.5f;
                if (d < best_d) { best_d = d; best = id; }
            }
            if (best) { overview_sel_ = best; app_.request_redraw(); }
        }
        return true;
    }
    if (ev.key == Key::Char && !(ev.mods & (MOD_CTRL | MOD_ALT)) && ev.keysym >= '1' && ev.keysym <= '9') {
        size_t idx = (size_t)(ev.keysym - '1');
        if (idx < rk.size()) {
            uint64_t target = rk[idx];
            overview_ = false;
            if (Pane* p = app_.pane(target); p && p->minimized()) p->set_minimized(false);
            app_.focus(target, true);
        }
        return true;
    }
    return false;
}

void Canvas::sync_panes() {
    std::vector<uint64_t> fresh;
    for (uint64_t id : order_) if (app_.pane(id)) fresh.push_back(id);
    for (auto& kv : app_.panes()) if (std::find(fresh.begin(), fresh.end(), kv.first) == fresh.end()) fresh.push_back(kv.first);
    order_ = fresh;
    if (hover_pane_ && !app_.pane(hover_pane_->activity_id)) hover_pane_ = nullptr;
    if (drag_pane_ && !app_.pane(drag_pane_->activity_id)) { drag_pane_ = nullptr; drag_ = None; }
}

void Canvas::bring_to_front(uint64_t id) {
    auto it = std::find(order_.begin(), order_.end(), id);
    if (it != order_.end()) order_.erase(it);
    order_.push_back(id);
}

float Canvas::layout_subtree(uint64_t id, float x, float y) {
    Pane* p = app_.pane(id);
    Activity* a = app_.tree().get(id);
    if (!p || !a) return 0;
    if (a->auto_place) {
        if (first_layout_ || (p->x == 0 && p->y == 0 && a->x == 0 && a->y == 0)) { p->x = x; p->y = y; }
        a->x = x; a->y = y;
    } else { a->x = p->x; a->y = p->y; x = p->x; y = p->y; }
    float pane_w = p->width() + (p->drawer_open() && !p->minimized() ? p->drawer_rect().w + 14.f : 0.f);
    float child_x = a->x + pane_w + GAP_X;
    float cy = a->y, total = 0; int n = 0;
    if (!a->collapsed) {
        for (uint64_t c : a->children) {
            float h = layout_subtree(c, child_x, cy);
            if (h <= 0) continue;
            cy += h + GAP_Y; total += h + GAP_Y; n++;
        }
    }
    if (n) total -= GAP_Y;
    float pane_h = (p->drawer_open() && !p->minimized()) ? std::max(p->height(), p->drawer_rect().h) : p->height();
    return std::max(pane_h, total);
}

void Canvas::layout() {
    sync_panes();
    layout_subtree(app_.tree().root(), 0, 0);
    first_layout_ = false;
    app_.request_redraw();
}

bool Canvas::update(double now, double dt) {
    bool changed = false;
    float speed = 11.f * app_.config().animation_speed;
    if (animating()) {
        scale = approach(scale, tscale_, (float)dt, speed);
        tx = approach(tx, ttx_, (float)dt, speed);
        ty = approach(ty, tty_, (float)dt, speed);
        if (!animating()) { scale = tscale_; tx = ttx_; ty = tty_; }
        changed = true;
    }
    // pane position animation toward layout targets
    for (auto& kv : app_.panes()) {
        Pane& p = *kv.second;
        Activity* a = app_.tree().get(kv.first);
        if (!a || !a->auto_place || (drag_ == Move && drag_pane_ == &p)) continue;
        if (std::fabs(p.x - a->x) > 0.5f || std::fabs(p.y - a->y) > 0.5f) {
            p.x = approach(p.x, a->x, (float)dt, speed);
            p.y = approach(p.y, a->y, (float)dt, speed);
            if (std::fabs(p.x - a->x) <= 0.5f && std::fabs(p.y - a->y) <= 0.5f) { p.x = a->x; p.y = a->y; }
            changed = true;
        }
    }
    return changed;
}

void Canvas::draw_background() {
    gfx::Renderer& r = app_.renderer();
    const Theme& th = app_.theme();
    r.set_transform({1, 0, 0});
    r.fill_rect(viewport, col(th.canvas_bg));
    // dot grid in world space, spacing grows when zoomed out
    float spacing = 48.f;
    while (spacing * scale < 28.f) spacing *= 2;
    if (spacing * scale < 200.f) {
        float wx0 = to_world_x(viewport.x), wy0 = to_world_y(viewport.y);
        float wx1 = to_world_x(viewport.x + viewport.w), wy1 = to_world_y(viewport.y + viewport.h);
        float sx = std::floor(wx0 / spacing) * spacing, sy = std::floor(wy0 / spacing) * spacing;
        RGBA dc = alpha(col(th.chrome_fg), 0.13f);
        r.set_transform(transform());
        float rad = std::max(0.8f, 1.2f / scale);
        for (float yy = sy; yy < wy1; yy += spacing)
            for (float xx = sx; xx < wx1; xx += spacing) r.fill_rect({xx - rad / 2, yy - rad / 2, rad, rad}, dc);
    }
}

void Canvas::draw_connectors() {
    gfx::Renderer& r = app_.renderer();
    const Theme& th = app_.theme();
    r.set_transform(transform());
    uint64_t foc = app_.focused_id();
    for (auto& kv : app_.panes()) {
        Activity* a = app_.tree().get(kv.first);
        if (!a || !a->parent) continue;
        Pane* parent = app_.pane(a->parent);
        if (!parent) continue;
        Activity* pa = app_.tree().get(a->parent);
        if (pa && pa->collapsed) continue;
        Pane& c = *kv.second;
        float x0 = parent->x + parent->width() + (parent->drawer_open() && !parent->minimized() ? parent->drawer_rect().w + 12.f : 0.f), y0 = parent->y + Pane::TITLE_H / 2;
        float x1 = c.x, y1 = c.y + Pane::TITLE_H / 2;
        uint64_t highlight_id = overview_ ? overview_sel_ : foc;
        bool hot = kv.first == highlight_id || a->parent == highlight_id;
        RGBA cc = alpha(col(th.accent), hot ? 0.85f : 0.0f);
        RGBA base = alpha(col(th.chrome_fg), 0.28f);
        RGBA lc = hot ? cc : base;
        float thick = std::max(1.f, (hot ? 2.f : 1.2f) / std::max(scale, 0.25f));
        int segs = 18;
        float px = x0, py = y0;
        float cx1 = x0 + (x1 - x0) * 0.5f, cx2 = x1 - (x1 - x0) * 0.5f;
        for (int i = 1; i <= segs; i++) {
            float t = (float)i / segs, u = 1 - t;
            float bx = u * u * u * x0 + 3 * u * u * t * cx1 + 3 * u * t * t * cx2 + t * t * t * x1;
            float by = u * u * u * y0 + 3 * u * u * t * y0 + 3 * u * t * t * y1 + t * t * t * y1;
            r.line(px, py, bx, by, lc, thick);
            px = bx; py = by;
        }
        r.circle(x1, y1, 3.f / std::max(scale, 0.3f) , lc);
    }
}

void Canvas::draw(const DrawCtx& base) {
    gfx::Renderer& r = app_.renderer();
    r.push_clip(viewport);
    draw_background();
    draw_connectors();
    r.set_transform(transform());
    // frustum cull in world space
    float wx0 = to_world_x(viewport.x), wy0 = to_world_y(viewport.y);
    float wx1 = to_world_x(viewport.x + viewport.w), wy1 = to_world_y(viewport.y + viewport.h);
    for (uint64_t id : order_) {
        Pane* p = app_.pane(id);
        if (!p) continue;
        Activity* a = app_.tree().get(id);
        // hidden under a collapsed ancestor?
        bool hidden = false;
        for (uint64_t anc = a ? a->parent : 0; anc; ) { Activity* an = app_.tree().get(anc); if (!an) break; if (an->collapsed) { hidden = true; break; } anc = an->parent; }
        if (hidden) continue;
        Rect R = p->rect();
        float rw = R.w + (p->drawer_open() && !p->minimized() ? p->drawer_rect().w + 20.f : 0.f);
        float rh = (p->drawer_open() && !p->minimized()) ? std::max(R.h, p->drawer_rect().h) : R.h;
        if (R.x + rw + 8 < wx0 || R.x > wx1 || R.y + rh + 8 < wy0 || R.y > wy1) continue;
        DrawCtx ctx = base;
        ctx.focused = (id == app_.focused_id());
        ctx.scale = scale;
        ctx.overview = overview_;
        p->draw(r, app_.font(), ctx);
    }
    r.set_transform({1, 0, 0});
    const Theme& th = app_.theme();
    gfx::Font& font = app_.font();
    float asc = font.metrics().ascent;
    auto ranked = ranked_panes(base.now);
    if (overview_) {
        hud_tree_rect_ = {0, 0, 0, 0};
        hud_next_rect_ = {0, 0, 0, 0};
        hud_next_id_ = 0;
        if (!ranked.empty() && std::find(ranked.begin(), ranked.end(), overview_sel_) == ranked.end()) {
            overview_sel_ = ranked[0];
        }
        // Draw ranked badges & spotlight in screen space above each pane
        for (size_t i = 0; i < ranked.size(); ++i) {
            uint64_t id = ranked[i];
            Pane* p = app_.pane(id);
            Activity* a = app_.tree().get(id);
            if (!p || !a) continue;
            Rect R = p->rect();
            Rect sr = {to_screen_x(R.x), to_screen_y(R.y), R.w * scale, R.h * scale};
            if (sr.x + sr.w < viewport.x || sr.x > viewport.x + viewport.w || sr.y + sr.h < viewport.y || sr.y > viewport.y + viewport.h) continue;

            bool is_top = (i == 0 && (ranked.size() == 1 || id != app_.focused_id()));
            bool is_sel = (id == overview_sel_);
            bool urgent = p->waiting_for_user() || p->artifact_unread() || p->unread;
            RGBA badge_c = is_top ? (urgent ? col(th.warning) : col(th.accent)) : (is_sel ? col(th.accent) : col(th.chrome_fg));

            if (is_sel || is_top) {
                float pulse = is_top && urgent ? (0.65f + 0.25f * (float)std::sin(base.now * 6.0)) : 0.85f;
                r.rounded_rect_stroke(sr.inset(-3.f), Pane::RADIUS * scale + 3.f, alpha(badge_c, pulse * (is_sel ? 0.95f : 0.65f)), is_sel ? 2.5f : 1.5f);
                if (is_sel) r.rounded_rect_stroke(sr.inset(-6.f), Pane::RADIUS * scale + 6.f, alpha(badge_c, 0.28f), 2.f);
            }

            std::string reason = (id == app_.focused_id() && ranked.size() > 1) ? "current" : p->recommendation_reason(base.now);
            std::string pill_txt = (is_top ? "★ #1 NEXT UP · " : ("#" + std::to_string(i + 1) + " · ")) + a->name + "  (" + reason + ")";
            float pw = r.measure_text(font, pill_txt, (is_top || is_sel) ? gfx::STYLE_BOLD : gfx::STYLE_REGULAR, 0.85f) + 18.f;
            float ph = 22.f;
            Rect pr = {sr.x, std::max(viewport.y + 6.f, sr.y - ph - 6.f), pw, ph};
            r.rounded_rect(pr, 11.f, alpha(col(th.chrome_bg), 0.94f));
            r.rounded_rect_stroke(pr, 11.f, alpha(badge_c, (is_top || is_sel) ? 0.9f : 0.45f), (is_top || is_sel) ? 1.5f : 1.f);
            r.draw_text(font, pr.x + 9.f, pr.y + (ph - app_.cell_h() * 0.85f) / 2.f + asc * 0.85f, pill_txt,
                        (is_top || is_sel) ? (is_top && urgent ? col(th.warning) : col(th.foreground)) : alpha(col(th.chrome_fg), 0.85f),
                        (is_top || is_sel) ? gfx::STYLE_BOLD : gfx::STYLE_REGULAR, 0.85f);
        }

        std::string sel_info;
        if (Pane* sp = app_.pane(overview_sel_)) {
            Activity* sa = app_.tree().get(overview_sel_);
            sel_info = "★ Selected: " + (sa ? sa->name : std::string("?")) + " (" + sp->recommendation_reason(base.now) + ")  ·  ";
        }
        std::string hint = sel_info + "Enter/Space opens · Tab cycles · 1–9 jumps · Click pane · Esc returns";
        float w = r.measure_text(font, hint, gfx::STYLE_REGULAR, 0.9f) + 28.f;
        Rect b = {viewport.x + (viewport.w - w) / 2.f, viewport.y + viewport.h - 44.f, w, 30.f};
        r.rounded_rect(b, 15.f, alpha(col(th.chrome_bg), 0.94f));
        r.rounded_rect_stroke(b, 15.f, alpha(col(th.accent), 0.65f), 1.2f);
        r.draw_text(font, b.x + 14.f, b.y + (30.f - app_.cell_h() * 0.9f) / 2.f + asc * 0.9f, hint, col(th.foreground), gfx::STYLE_REGULAR, 0.9f);
    } else {
        // Floating Canvas HUD at bottom-left: [⊞ Tree (N)] + [★ Next: <name> (<reason>)]
        float hy = viewport.y + viewport.h - 36.f;
        float hx = viewport.x + 14.f;
        float hh = 26.f;
        std::string tree_lbl = "⊞ Tree (" + std::to_string(app_.panes().size()) + ")";
        float tw = r.measure_text(font, tree_lbl, gfx::STYLE_BOLD, 0.85f) + 20.f;
        hud_tree_rect_ = {hx, hy, tw, hh};
        r.rounded_rect(hud_tree_rect_, 13.f, alpha(col(th.chrome_bg), hover_hud_tree_ ? 0.96f : 0.86f));
        r.rounded_rect_stroke(hud_tree_rect_, 13.f, alpha(hover_hud_tree_ ? col(th.accent) : col(th.chrome_fg), hover_hud_tree_ ? 0.85f : 0.35f), 1.f);
        r.draw_text(font, hx + 10.f, hy + (hh - app_.cell_h() * 0.85f) / 2.f + asc * 0.85f, tree_lbl,
                    hover_hud_tree_ ? col(th.foreground) : col(th.chrome_fg), gfx::STYLE_BOLD, 0.85f);

        hud_next_id_ = 0;
        hud_next_rect_ = {0, 0, 0, 0};
        if (!ranked.empty() && ranked[0] != app_.focused_id()) {
            hud_next_id_ = ranked[0];
            Pane* np = app_.pane(hud_next_id_);
            Activity* na = app_.tree().get(hud_next_id_);
            if (np && na) {
                bool urgent = np->waiting_for_user() || np->artifact_unread() || np->unread;
                std::string next_lbl = (urgent ? "★ Next: " : "↪ Next: ") + na->name + " · " + np->recommendation_reason(base.now);
                float nw = r.measure_text(font, next_lbl, urgent ? gfx::STYLE_BOLD : gfx::STYLE_REGULAR, 0.85f) + 22.f;
                hud_next_rect_ = {hx + tw + 8.f, hy, nw, hh};
                RGBA nc = urgent ? col(th.warning) : col(th.accent);
                r.rounded_rect(hud_next_rect_, 13.f, alpha(col(th.chrome_bg), hover_hud_next_ ? 0.96f : (urgent ? 0.92f : 0.84f)));
                r.rounded_rect_stroke(hud_next_rect_, 13.f, alpha(nc, hover_hud_next_ || urgent ? 0.85f : 0.4f), urgent ? 1.3f : 1.f);
                r.draw_text(font, hud_next_rect_.x + 11.f, hy + (hh - app_.cell_h() * 0.85f) / 2.f + asc * 0.85f, next_lbl,
                            urgent ? col(th.warning) : (hover_hud_next_ ? col(th.foreground) : col(th.chrome_fg)),
                            urgent ? gfx::STYLE_BOLD : gfx::STYLE_REGULAR, 0.85f);
            }
        }
    }
    r.pop_clip();
}

Pane* Canvas::pane_at(float wx, float wy) {
    for (size_t i = order_.size(); i-- > 0;) {
        Pane* p = app_.pane(order_[i]);
        if (!p) continue;
        Activity* a = app_.tree().get(order_[i]);
        bool hidden = false;
        for (uint64_t anc = a ? a->parent : 0; anc; ) { Activity* an = app_.tree().get(anc); if (!an) break; if (an->collapsed) { hidden = true; break; } anc = an->parent; }
        if (hidden) continue;
        if (p->rect().contains(wx, wy)) return p;
        if (p->drawer_open() && !p->minimized() && p->drawer_rect().contains(wx, wy)) return p;
    }
    return nullptr;
}

void Canvas::mouse_down(float sx, float sy, MouseButton b, int clicks, uint32_t mods) {
    if (b == MouseButton::Left && !overview_) {
        if (hud_tree_rect_.contains(sx, sy)) {
            toggle_overview();
            return;
        }
        if (hud_next_id_ && hud_next_rect_.contains(sx, sy)) {
            if (Pane* bp = app_.pane(hud_next_id_); bp && bp->minimized()) bp->set_minimized(false);
            app_.focus(hud_next_id_, true);
            return;
        }
    }
    float wx = to_world_x(sx), wy = to_world_y(sy);
    held_ = b;
    moved_ = false;
    Pane* p = pane_at(wx, wy);
    bool pan_mod = (mods & MOD_ALT) && b == MouseButton::Left;
    if (p && !pan_mod && !(b == MouseButton::Middle && !p->term_rect().contains(wx, wy) && !p->title_rect().contains(wx, wy))) {
        if (b == MouseButton::Left && p->close_rect().contains(wx, wy) && scale * app_.cell_h() >= 2.5f) {
            app_.close_activity(p->activity_id);
            return;
        }
        if (b == MouseButton::Left && p->min_rect().contains(wx, wy) && scale * app_.cell_h() >= 2.5f) {
            p->toggle_minimized();
            return;
        }
        if (b == MouseButton::Left && !p->artifacts().empty() && p->artifact_chip_rect().contains(wx, wy) && scale * app_.cell_h() >= 2.5f) {
            if (clicks >= 2) {
                p->set_drawer_open(true);
                if (const AgyArtifact* art = p->active_artifact()) app_.open_path(art->path, 0, p->cwd(), p->activity_id);
            } else {
                p->toggle_drawer();
            }
            app_.focus(p->activity_id, false);
            return;
        }
        if (b == MouseButton::Middle && p->title_rect().contains(wx, wy)) {
            app_.close_activity(p->activity_id);
            return;
        }
        bring_to_front(p->activity_id);
        if (overview_) {
            overview_ = false;
            if (p->minimized()) p->set_minimized(false);
            app_.focus(p->activity_id, true);
            return;
        }
        if (p->minimized() && b == MouseButton::Left && (clicks >= 2 || !p->title_rect().contains(wx, wy))) {
            p->set_minimized(false);
            app_.focus(p->activity_id, true);
            return;
        }
        if (b == MouseButton::Left && !p->minimized() && p->resize_handle().contains(wx, wy)) {
            drag_ = Resize; drag_pane_ = p; drag_x_ = wx; drag_y_ = wy; drag_cols_ = p->cols; drag_rows_ = p->rows;
            app_.focus(p->activity_id, false);
            return;
        }
        if (b == MouseButton::Left && p->title_rect().contains(wx, wy)) {
            if (clicks == 2 && !p->minimized() && !animating()) { app_.focus(p->activity_id, false); app_.run_action("activity.rename"); return; }
            drag_ = Move; drag_pane_ = p; drag_x_ = wx; drag_y_ = wy; drag_px_ = p->x; drag_py_ = p->y;
            app_.focus(p->activity_id, false);
            return;
        }
        if (b == MouseButton::Right && p->title_rect().contains(wx, wy)) {
            uint64_t pid = p->activity_id;
            app_.focus(pid, false);
            Activity* a = app_.tree().get(pid);
            std::vector<MenuItem> items = {
                {"New child activity", [this] { app_.run_action("activity.new_child"); }},
                {"Rename…", [this] { app_.run_action("activity.rename"); }},
                {"Add note…", [this] { app_.run_action("activity.note"); }},
                {p->drawer_open() ? "Close AGY artifact drawer" : "Open AGY artifact drawer", [p] { p->toggle_drawer(); }},
                {p->minimized() ? "Expand pane" : "Minimize pane", [p] { p->toggle_minimized(); }},
                {"Restart shell", [this] { app_.run_action("activity.restart"); }},
                {"-", nullptr},
                {"Close activity", [this, pid] { app_.close_activity(pid); }},
            };
            if (a && !a->children.empty()) {
                items.push_back({"Close activity & children", [this, pid] { app_.close_subtree(pid); }});
            }
            app_.show_menu(sx, sy, std::move(items));
            return;
        }
        if (p->activity_id != app_.focused_id()) app_.focus(p->activity_id, false);
        if (scale < 0.55f) { fly_to_pane(*p); }
        drag_ = PaneDrag; drag_pane_ = p;
        p->mouse_down(wx, wy, b, clicks, mods);
        return;
    }
    if (b == MouseButton::Left || b == MouseButton::Middle) {
        drag_ = Pan; drag_x_ = sx; drag_y_ = sy; drag_px_ = ttx_; drag_py_ = tty_;
    } else if (b == MouseButton::Right) {
        std::vector<MenuItem> items = {
            {"New activity here", [this, wx, wy] { uint64_t id = app_.new_activity(app_.focused_id() ? app_.focused_id() : app_.tree().root(), "", "", true); if (Pane* np = app_.pane(id)) { np->x = wx; np->y = wy; if (Activity* a = app_.tree().get(id)) { a->auto_place = false; a->x = wx; a->y = wy; } fly_to_pane(*np); } }},
            {"Overview", [this] { toggle_overview(); }},
            {"Re-layout tree", [this] { for (auto& kv : app_.panes()) if (Activity* a = app_.tree().get(kv.first)) a->auto_place = true; layout(); }},
            {"Zoom to 100%", [this] { reset_zoom(); }},
        };
        app_.show_menu(sx, sy, std::move(items));
    }
}

void Canvas::mouse_move(float sx, float sy, uint32_t mods, MouseButton held) {
    float wx = to_world_x(sx), wy = to_world_y(sy);
    switch (drag_) {
    case Pan: {
        float nx = drag_px_ + (sx - drag_x_), ny = drag_py_ + (sy - drag_y_);
        tx = ttx_ = nx; ty = tty_ = ny;
        moved_ = true;
        app_.request_redraw();
        return;
    }
    case Move: {
        if (!drag_pane_) return;
        drag_pane_->x = drag_px_ + (wx - drag_x_);
        drag_pane_->y = drag_py_ + (wy - drag_y_);
        if (Activity* a = app_.tree().get(drag_pane_->activity_id)) { a->auto_place = false; a->x = drag_pane_->x; a->y = drag_pane_->y; }
        moved_ = true;
        layout();
        return;
    }
    case Resize: {
        if (!drag_pane_) return;
        int nc = drag_cols_ + (int)std::lround((wx - drag_x_) / app_.cell_w());
        int nr = drag_rows_ + (int)std::lround((wy - drag_y_) / app_.cell_h());
        drag_pane_->resize_cells(nc, nr);
        layout();
        return;
    }
    case PaneDrag:
        if (drag_pane_) { drag_pane_->mouse_move(wx, wy, mods, held); drag_pane_->set_hover(wx, wy); }
        return;
    default: break;
    }
    hover_hud_tree_ = !overview_ && hud_tree_rect_.contains(sx, sy);
    hover_hud_next_ = !overview_ && hud_next_id_ != 0 && hud_next_rect_.contains(sx, sy);
    Pane* p = (hover_hud_tree_ || hover_hud_next_) ? nullptr : pane_at(wx, wy);
    if (overview_ && p && overview_sel_ != p->activity_id) {
        overview_sel_ = p->activity_id;
    }
    if (hover_pane_ && hover_pane_ != p) hover_pane_->clear_hover();
    hover_pane_ = p;
    if (p && !overview_) { p->set_hover(wx, wy); p->mouse_move(wx, wy, mods, held); }
    app_.request_redraw();
}

void Canvas::mouse_up(float sx, float sy, MouseButton b, uint32_t mods) {
    float wx = to_world_x(sx), wy = to_world_y(sy);
    held_ = MouseButton::None;
    if (drag_ == PaneDrag && drag_pane_) drag_pane_->mouse_up(wx, wy, b, mods);
    if (drag_ == Move && drag_pane_ && !moved_) { /* click on title: just focus */ }
    if (drag_ == Move || drag_ == Resize) app_.save_state();
    drag_ = None; drag_pane_ = nullptr;
    // 'held' changes may alter the hover pane
    mouse_move(sx, sy, mods, MouseButton::None);
}

void Canvas::wheel(float sx, float sy, float dx, float dy, uint32_t mods) {
    if (mods & MOD_CTRL) { zoom_at(sx, sy, std::pow(1.15f, dy)); return; }
    float wx = to_world_x(sx), wy = to_world_y(sy);
    Pane* p = pane_at(wx, wy);
    if (p && !overview_ && scale > 0.3f) { p->wheel(wx, wy, dx, dy, mods); return; }
    // pan
    float step = 60.f;
    if (mods & MOD_SHIFT) { ttx_ += dy * step; tx = ttx_; }
    else { ttx_ += dx * step; tty_ += dy * step; tx = ttx_; ty = tty_; }
    app_.request_redraw();
}

int Canvas::cursor_for(float sx, float sy) {
    if (drag_ == Pan) return SDL_SYSTEM_CURSOR_SIZEALL;
    if (drag_ == Move) return SDL_SYSTEM_CURSOR_SIZEALL;
    if (drag_ == Resize) return SDL_SYSTEM_CURSOR_SIZENWSE;
    if (hover_hud_tree_ || hover_hud_next_) return SDL_SYSTEM_CURSOR_HAND;
    if (overview_ && hover_pane_) return SDL_SYSTEM_CURSOR_HAND;
    if (hover_pane_) return hover_pane_->system_cursor();
    return SDL_SYSTEM_CURSOR_ARROW;
}

void Canvas::focus_direction(int dx, int dy) {
    Pane* cur = app_.focused_pane();
    if (!cur) return;
    Rect C = cur->rect();
    float cx = C.x + C.w / 2, cy = C.y + C.h / 2;
    Pane* best = nullptr; float best_d = 1e18f;
    for (auto& kv : app_.panes()) {
        Pane* p = kv.second.get();
        if (p == cur) continue;
        Rect R = p->rect();
        float px = R.x + R.w / 2, py = R.y + R.h / 2;
        float vx = px - cx, vy = py - cy;
        float along = vx * dx + vy * dy;
        if (along <= 1) continue;
        float across = std::fabs(vx * dy) + std::fabs(vy * dx);
        float d = along + across * 2.5f;
        if (d < best_d) { best_d = d; best = p; }
    }
    if (best) app_.focus(best->activity_id, true);
}

} // namespace canopy::ui
