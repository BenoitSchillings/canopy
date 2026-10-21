#include "peek.h"
#include "canvas.h"
#include <algorithm>
#include <cmath>

namespace canopy::ui {

static constexpr float CARD_PAD = 10.f, THUMB_MAX_H = 150.f;

PeekRail::PeekRail(App& app) : app_(app) {}

std::vector<std::string> PeekRail::wrap_text(gfx::Renderer& r, gfx::Font& font, const std::string& text, float maxw, float scale, size_t max_lines) {
    std::vector<std::string> lines; std::string cur;
    size_t pos = 0;
    while (pos < text.size() && lines.size() < max_lines) {
        size_t sp = text.find(' ', pos);
        std::string word = text.substr(pos, sp == std::string::npos ? std::string::npos : sp - pos);
        std::string trial = cur.empty() ? word : cur + " " + word;
        if (r.measure_text(font, trial, gfx::STYLE_REGULAR, scale) > maxw && !cur.empty()) { lines.push_back(cur); cur = word; }
        else cur = trial;
        if (sp == std::string::npos) { pos = text.size(); break; }
        pos = sp + 1;
    }
    if (!cur.empty() && lines.size() < max_lines) lines.push_back(cur);
    else if (!cur.empty() && !lines.empty()) lines.back() = ellipsize(lines.back(), std::max<size_t>(3, utf8_length(lines.back()) - 1));
    return lines;
}

void PeekRail::draw_miniature(App& app, Pane& p, Rect box, const DrawCtx& base, bool align_bottom) {
    Activity* a = app.tree().get(p.activity_id);
    bool was_min = a && a->minimized;
    if (was_min) a->minimized = false;
    gfx::Renderer& r = app.renderer();
    float s = std::min(box.w / p.width(), box.h / p.height());
    bool clip_tall = false;
    if (p.height() * s < box.h * 0.6f) { s = box.w / p.width(); clip_tall = p.height() * s > box.h; }
    float tx = box.x - p.x * s + (box.w - p.width() * s) / 2;
    float ty;
    if (clip_tall && align_bottom) ty = box.y + box.h - p.height() * s - p.y * s;
    else ty = box.y - p.y * s + (clip_tall ? 0 : (box.h - p.height() * s) / 2);
    r.push_clip(box);
    r.set_transform({s, tx, ty});
    DrawCtx ctx = base;
    ctx.scale = s; ctx.focused = false; ctx.in_rail = true;
    p.draw(r, app.font(), ctx);
    r.set_transform({1, 0, 0});
    r.pop_clip();
    if (was_min) a->minimized = true;
}

void PeekRail::build_cards() {
    cards_.clear();
    gfx::Renderer& r = app_.renderer();
    gfx::Font& font = app_.font();
    std::vector<Pane*> panes;
    for (auto& kv : app_.panes()) if (kv.first != app_.focused_id()) panes.push_back(kv.second.get());
    double now = app_.now();
    uint64_t foc = app_.focused_id();
    std::stable_sort(panes.begin(), panes.end(), [&](Pane* a, Pane* b) { return a->attention_score(now, foc) > b->attention_score(now, foc); });
    float y = rect.y + 44 - scroll_;
    float inner = rect.w - 2 * CARD_PAD;
    for (Pane* p : panes) {
        float thumb_h = std::min(THUMB_MAX_H, p->height() * (inner - 2 * CARD_PAD) / p->width());
        std::string text = p->summary.empty() ? p->local_summary() : p->summary;
        auto lines = wrap_text(r, font, text, inner - 2 * CARD_PAD, 0.9f, 3);
        float h = CARD_PAD + 22 + thumb_h + 8 + lines.size() * (app_.cell_h() * 0.9f + 2) + 22 + CARD_PAD;
        Rect cr = {rect.x + CARD_PAD, y, inner, h};
        Rect cb = {cr.x + cr.w - CARD_PAD - 18.f, cr.y + CARD_PAD - 1.f, 18.f, 18.f};
        cards_.push_back({p->activity_id, cr, cb});
        y += h + 10;
    }
    content_h_ = y + scroll_ - rect.y + 10;
}

void PeekRail::draw(const DrawCtx& ctx) {
    if (!visible) return;
    gfx::Renderer& r = app_.renderer();
    gfx::Font& font = app_.font();
    const Theme& th = app_.theme();
    build_cards();
    r.set_transform({1, 0, 0});
    r.fill_rect(rect, col(th.chrome_bg));
    r.fill_rect({rect.x, rect.y, 1, rect.h}, alpha(col(th.chrome_fg), 0.15f));
    float asc = font.metrics().ascent;
    RGBA fg = col(th.chrome_fg);
    r.draw_text(font, rect.x + 14, rect.y + 24, "OTHER TERMINALS", alpha(fg, 0.55f), gfx::STYLE_BOLD, 0.8f);
    std::string hint = "Ctrl+Tab finds";
    float hw = r.measure_text(font, hint, gfx::STYLE_REGULAR, 0.75f);
    r.draw_text(font, rect.x + rect.w - hw - 12, rect.y + 24, hint, alpha(fg, 0.4f), gfx::STYLE_REGULAR, 0.75f);
    r.push_clip({rect.x, rect.y + 36, rect.w, rect.h - 36});
    if (cards_.empty()) {
        auto lines = wrap_text(r, font, "No other terminals yet. Ctrl+Shift+N opens a child activity; this rail shows what each one is doing.", rect.w - 40, 0.9f, 6);
        float y = rect.y + 70;
        for (auto& l : lines) { r.draw_text(font, rect.x + 20, y + asc, l, alpha(fg, 0.6f), gfx::STYLE_REGULAR, 0.9f); y += app_.cell_h() * 0.9f + 2; }
    }
    for (const Card& c : cards_) {
        Pane* p = app_.pane(c.id);
        Activity* a = app_.tree().get(c.id);
        if (!p || !a) continue;
        if (c.r.y + c.r.h < rect.y || c.r.y > rect.y + rect.h) continue;
        bool hot = hover_ == c.id;
        r.rounded_rect(c.r, 8, alpha(col(th.background), hot ? 1.f : 0.7f));
        r.rounded_rect_stroke(c.r, 8, alpha(hot ? col(th.accent) : fg, hot ? 0.8f : 0.2f), 1.f);
        // header: dot + name + age
        float hx = c.r.x + CARD_PAD, hy = c.r.y + CARD_PAD;
        RGBA dot = alpha(fg, 0.35f); bool running = false;
        if (p->live()) {
            const auto& bl = p->session->blocks().blocks();
            for (size_t k = bl.size(); k-- > 0;) {
                if (bl[k].state == BlockState::Running) { running = true; dot = col(th.accent); break; }
                if (bl[k].state == BlockState::Done) { dot = bl[k].exit_code == 0 ? col(th.ok) : col(th.error); break; }
            }
            if (bl.empty()) dot = col(th.ok);
        } else if (p->session) dot = col(th.error);
        r.circle(hx + 5, hy + 8, 4, dot);
        if (running) r.circle(hx + 5, hy + 8, 6.5f + 1.5f * (float)std::sin(ctx.now * 6), alpha(dot, 0.25f));
        r.push_clip({hx + 14, hy, c.r.w - 2 * CARD_PAD - 102, 20});
        r.draw_text(font, hx + 14, hy + asc * 0.85f, a->name, col(th.foreground), gfx::STYLE_BOLD, 0.9f);
        r.pop_clip();
        bool hot_close = hover_close_ == c.id;
        RGBA cb_bg = hot_close ? alpha(col(th.error), 0.85f) : alpha(fg, hot ? 0.16f : 0.08f);
        RGBA cb_fg = hot_close ? col(th.foreground) : alpha(fg, hot ? 0.85f : 0.5f);
        r.rounded_rect(c.close_btn, 4.f, cb_bg);
        float xw = r.measure_text(font, "×", gfx::STYLE_BOLD, 0.85f);
        r.draw_text(font, c.close_btn.x + (c.close_btn.w - xw) / 2.f, c.close_btn.y + 2 + asc * 0.85f, "×", cb_fg, gfx::STYLE_BOLD, 0.85f);
        double age = ctx.now - std::max(p->last_output, p->last_finish);
        std::string ag = p->last_output > 0 ? (age < 5 ? "now" : fmt_duration(age) + " ago") : "";
        if (p->unread) ag = "● " + ag;
        float aw = r.measure_text(font, ag, gfx::STYLE_REGULAR, 0.75f);
        r.draw_text(font, c.close_btn.x - 6 - aw, hy + asc * 0.85f, ag, p->unread ? col(th.warning) : alpha(fg, 0.5f), gfx::STYLE_REGULAR, 0.75f);
        // miniature
        float inner = c.r.w - 2 * CARD_PAD;
        float thumb_h = std::min(THUMB_MAX_H, p->height() * inner / p->width());
        Rect tb = {hx, hy + 22, inner, thumb_h};
        draw_miniature(app_, *p, tb, ctx, true);
        r.rounded_rect_stroke(tb, 3, alpha(fg, 0.15f), 1.f);
        // summary
        std::string text = p->summary.empty() ? p->local_summary() : p->summary;
        auto lines = wrap_text(r, font, text, inner, 0.9f, 3);
        float y = tb.y + tb.h + 8;
        float lh = app_.cell_h() * 0.9f + 2;
        for (auto& l : lines) { r.draw_text(font, hx, y + asc * 0.9f, l, p->summary_ai ? col(th.foreground) : alpha(col(th.foreground), 0.75f), p->summary_ai ? gfx::STYLE_REGULAR : gfx::STYLE_ITALIC, 0.9f); y += lh; }
        // badges
        auto badges = p->attention_badges(ctx.now);
        float bx = hx;
        for (auto& b : badges) {
            float w = r.measure_text(font, b, gfx::STYLE_REGULAR, 0.75f) + 12;
            if (bx + w > c.r.x + c.r.w - CARD_PAD) break;
            RGBA bc = b.rfind("✗", 0) == 0 ? col(th.error) : b.rfind("●", 0) == 0 ? col(th.warning) : b.rfind("⟳", 0) == 0 ? col(th.accent) : alpha(fg, 0.7f);
            r.rounded_rect({bx, y + 3, w, 16}, 8, alpha(bc, 0.18f));
            r.draw_text(font, bx + 6, y + 3 + asc * 0.75f + 1, b, bc, gfx::STYLE_REGULAR, 0.75f);
            bx += w + 6;
        }
        if (p->summary_pending) {
            static const char* SP[] = {"◐", "◓", "◑", "◒"};
            r.draw_text(font, c.r.x + c.r.w - CARD_PAD - 14, y + 3 + asc * 0.75f + 1, SP[(int)(ctx.now * 8) % 4], alpha(col(th.accent), 0.8f), gfx::STYLE_REGULAR, 0.8f);
        }
    }
    r.pop_clip();
    // backend indicator
    std::string be = std::string("summaries: ") + app_.summary_backend_name();
    r.draw_text(font, rect.x + 14, rect.y + rect.h - 10, be, alpha(fg, 0.35f), gfx::STYLE_REGULAR, 0.75f);
}

bool PeekRail::mouse_down(float sx, float sy, MouseButton b, int clicks) {
    if (!visible || !rect.contains(sx, sy)) return false;
    build_cards();
    for (const Card& c : cards_) if (c.r.contains(sx, sy)) {
        if (b == MouseButton::Left) {
            if (c.close_btn.contains(sx, sy)) app_.close_activity(c.id);
            else app_.focus(c.id, true);
        } else if (b == MouseButton::Middle) {
            app_.close_activity(c.id);
        } else if (b == MouseButton::Right) {
            uint64_t id = c.id;
            app_.show_menu(sx, sy, {
                {"Go to terminal", [this, id] { app_.focus(id, true); }},
                {"Refresh summary", [this, id] { app_.request_summary(id, true); }},
                {"Close activity", [this, id] { app_.close_activity(id); }},
            });
        }
        return true;
    }
    return true;
}

void PeekRail::mouse_move(float sx, float sy) {
    uint64_t prev = hover_, prev_close = hover_close_;
    hover_ = 0; hover_close_ = 0;
    if (visible && rect.contains(sx, sy)) {
        for (const Card& c : cards_) if (c.r.contains(sx, sy)) {
            hover_ = c.id;
            if (c.close_btn.contains(sx, sy)) hover_close_ = c.id;
            break;
        }
    }
    if (prev != hover_ || prev_close != hover_close_) app_.request_redraw();
}

void PeekRail::wheel(float dy) {
    scroll_ = clampf(scroll_ - dy * 40, 0, std::max(0.f, content_h_ - rect.h));
    app_.request_redraw();
}

} // namespace canopy::ui
