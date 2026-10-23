#include "sidebar.h"
#include "canvas.h"
#include "peek.h"
#include <algorithm>
#include <cmath>

namespace canopy::ui {

static const char* SPIN[] = {"◐", "◓", "◑", "◒"};
static constexpr float GUTTER_W_EST = 18.f;

Sidebar::Sidebar(App& app) : app_(app) {}

void Sidebar::build_rows() {
    rows_.clear();
    const float HDR_H = 30.f, PIN_H = 26.f;
    gfx::Renderer& rr = app_.renderer(); gfx::Font& font = app_.font();
    float lh = app_.cell_h() * 0.85f + 2;
    float y = rect.y + 8 - scroll_;
    Row hdr; hdr.kind = Row::Header; hdr.label = "ACTIVITIES"; hdr.r = {rect.x, y, rect.w, HDR_H}; rows_.push_back(hdr); y += HDR_H;
    ActivityTree& t = app_.tree();
    // depth-first, honoring collapsed
    std::vector<std::pair<uint64_t, int>> stack = {{t.root(), 0}};
    while (!stack.empty()) {
        auto [id, depth] = stack.back(); stack.pop_back();
        Activity* a = t.get(id);
        if (!a) continue;
        Row r; r.kind = Row::Activity; r.id = id; r.depth = depth; r.has_children = !a->children.empty();
        r.label = a->name;
        float h = 40.f;
        if (Pane* p = app_.pane(id)) {
            r.sub = (p->waiting_for_user() || p->summary.empty()) ? p->local_summary() : p->summary;
            float inner = rect.w - 24 - depth * 14.f;
            if (compact) {
                h = 6 + 20 + lh + 6;
            } else {
                float pw = GUTTER_W_EST + p->cols * app_.cell_w() + 12.f;
                float ph = Pane::TITLE_H + p->rows * app_.cell_h() + 12.f;
                float th = std::min(110.f, ph * inner / std::max(1.f, pw));
                size_t nl = PeekRail::wrap_text(rr, font, r.sub, inner, 0.85f, 2).size();
                h = 6 + 22 + th + 4 + nl * lh + 8;
            }
        }
        r.r = {rect.x, y, rect.w, h};
        r.arrow = {rect.x + 6 + depth * 14.f, y + 4, 16, 20};
        r.close_btn = {rect.x + rect.w - 30.f, y + 5.f, 18.f, 18.f};
        rows_.push_back(r); y += h;
        if (!a->collapsed) for (size_t i = a->children.size(); i-- > 0;) stack.push_back({a->children[i], depth + 1});
    }
    // pinned blocks
    std::vector<Row> pins;
    for (auto& kv : app_.panes()) {
        Pane& p = *kv.second;
        for (int i = 0; i < p.block_count(); i++) {
            const Block* b = p.block(i);
            if (!b->pinned) continue;
            Row r; r.kind = Row::Pinned; r.id = kv.first; r.block_id = b->id;
            r.label = b->command;
            Activity* a = t.get(kv.first);
            r.sub = (a ? a->name : "") + (b->state == BlockState::Done ? (b->exit_code == 0 ? "  ✓" : "  ✗") : "");
            pins.push_back(r);
        }
    }
    if (!pins.empty()) {
        y += 8;
        Row h; h.kind = Row::Header; h.label = "PINNED"; h.r = {rect.x, y, rect.w, HDR_H}; rows_.push_back(h); y += HDR_H;
        for (auto& r : pins) { Row rr = r; rr.r = {rect.x, y, rect.w, PIN_H}; rows_.push_back(rr); y += PIN_H; }
    }
    content_h_ = y + scroll_ - rect.y + 60;
}

void Sidebar::draw(const DrawCtx& ctx) {
    if (!visible) return;
    gfx::Renderer& r = app_.renderer();
    gfx::Font& font = app_.font();
    const Theme& th = app_.theme();
    build_rows();
    r.set_transform({1, 0, 0});
    r.fill_rect(rect, col(th.chrome_bg));
    r.fill_rect({rect.x + rect.w - 1, rect.y, 1, rect.h}, alpha(col(th.chrome_fg), 0.15f));
    r.push_clip(rect);
    float asc = font.metrics().ascent;
    RGBA fg = col(th.chrome_fg);
    for (size_t i = 0; i < rows_.size(); i++) {
        const Row& row = rows_[i];
        if (row.r.y + row.r.h < rect.y || row.r.y > rect.y + rect.h) continue;
        if (row.kind == Row::Header) {
            r.draw_text(font, rect.x + 14, row.r.y + 18, row.label, alpha(fg, 0.55f), gfx::STYLE_BOLD, 0.8f);
            if (row.label == "ACTIVITIES") {
                bool hot = hover_ == (int)i;
                Rect ob = {rect.x + rect.w - 78, row.r.y + 4, 20, 20};
                std::string oicon = "⊞";
                float ow = r.measure_text(font, oicon);
                r.rounded_rect(ob, 5, alpha(col(th.accent), app_.canvas().overview() ? 0.5f : (hot ? 0.3f : 0.14f)));
                r.draw_text(font, ob.x + (ob.w - ow) / 2, ob.y + 15, oicon, col(th.foreground));

                Rect cb = {rect.x + rect.w - 54, row.r.y + 4, 20, 20};
                std::string cicon = compact ? "▤" : "☰";
                float cw = r.measure_text(font, cicon);
                r.rounded_rect(cb, 5, alpha(col(th.accent), compact ? 0.45f : (hot ? 0.3f : 0.14f)));
                r.draw_text(font, cb.x + (cb.w - cw) / 2, cb.y + 15, cicon, col(th.foreground));

                std::string plus = "+";
                float w = r.measure_text(font, plus);
                Rect pb = {rect.x + rect.w - 30, row.r.y + 4, 20, 20};
                r.rounded_rect(pb, 5, alpha(col(th.accent), hot ? 0.5f : 0.18f));
                r.draw_text(font, pb.x + (pb.w - w) / 2, pb.y + 15, plus, col(th.foreground));
            }
            continue;
        }
        bool focused = row.id == app_.focused_id() && row.kind == Row::Activity;
        bool hot = (int)i == hover_;
        if (focused) r.fill_rect({rect.x, row.r.y, rect.w - 1, row.r.h}, alpha(col(th.accent), 0.16f));
        else if (hot) r.fill_rect({rect.x, row.r.y, rect.w - 1, row.r.h}, alpha(fg, 0.07f));
        if (focused) r.fill_rect({rect.x, row.r.y, 3, row.r.h}, col(th.accent));
        if (row.kind == Row::Activity) {
            float x = rect.x + 12 + row.depth * 14.f;
            float inner = rect.w - 24 - row.depth * 14.f;
            Activity* a = app_.tree().get(row.id);
            Pane* p = app_.pane(row.id);
            float hy = row.r.y + 6;
            if (row.has_children) r.draw_text(font, x - 4, hy + asc * 0.8f + 2, a && a->collapsed ? "▸" : "▾", alpha(fg, 0.7f), gfx::STYLE_REGULAR, 0.8f);
            float nx = x + (row.has_children ? 12 : 0);
            // status dot
            RGBA dot = alpha(fg, 0.3f); bool running = false;
            bool waiting = p && p->waiting_for_user();
            if (p && p->live()) {
                const auto& bl = p->session->blocks().blocks();
                dot = col(th.ok);
                if (waiting) {
                    dot = col(th.warning);
                } else {
                    for (size_t k = bl.size(); k-- > 0;) {
                        if (bl[k].state == BlockState::Running) { running = true; dot = col(th.accent); break; }
                        if (bl[k].state == BlockState::Done) { dot = bl[k].exit_code == 0 ? col(th.ok) : col(th.error); break; }
                    }
                }
            } else if (p && p->session) dot = col(th.error);
            if (!focused && waiting) r.fill_rect({rect.x, row.r.y, 3, row.r.h}, col(th.warning));
            if (waiting) {
                const char* icon = (p->agent_state() == Pane::AgentState::NeedsApproval) ? "⚡" : "⏸";
                r.draw_text(font, nx - 1, hy + asc * 0.9f + 1, icon, dot, gfx::STYLE_BOLD, 0.85f);
            } else if (running) r.draw_text(font, nx - 1, hy + asc * 0.9f + 1, SPIN[(int)(ctx.now * 8) % 4], dot, gfx::STYLE_REGULAR, 0.9f);
            else r.circle(nx + 4, hy + 10, 3.5f, dot);
            bool hot_close = (int)i == hover_close_;
            RGBA cb_bg = hot_close ? alpha(col(th.error), 0.85f) : alpha(fg, hot || focused ? 0.14f : 0.06f);
            RGBA cb_fg = hot_close ? col(th.foreground) : alpha(fg, hot || focused ? 0.85f : 0.45f);
            r.rounded_rect(row.close_btn, 4.f, cb_bg);
            float xw = r.measure_text(font, "×", gfx::STYLE_BOLD, 0.85f);
            r.draw_text(font, row.close_btn.x + (row.close_btn.w - xw) / 2.f, row.close_btn.y + 2 + asc * 0.85f, "×", cb_fg, gfx::STYLE_BOLD, 0.85f);
            float right = row.close_btn.x - 6;
            float name_w = right - (nx + 14) - 24;
            r.push_clip({nx + 14, hy, std::max(10.f, name_w), 22});
            r.draw_text(font, nx + 14, hy + asc * 0.95f, row.label, focused ? col(th.foreground) : fg, gfx::STYLE_BOLD, 0.95f);
            r.pop_clip();
            if (p && p->unread && !focused) r.circle(right - 6, hy + 10, 3.5f, col(th.warning));
            if (a && !a->notes.empty()) r.draw_text(font, right - 22, hy + asc * 0.8f + 1, "✎", alpha(fg, 0.5f), gfx::STYLE_REGULAR, 0.8f);
            if (p) {
                float lh = app_.cell_h() * 0.85f + 2;
                if (compact) {
                    // Tree guide line for nested activities
                    for (int d = 1; d <= row.depth; d++) {
                        float gx = rect.x + 10 + (d - 1) * 14.f;
                        r.fill_rect({gx, row.r.y, 1.f, row.r.h}, alpha(fg, 0.14f));
                    }
                    float y = hy + 20;
                    auto lines = PeekRail::wrap_text(r, font, row.sub, inner, 0.82f, 1);
                    if (!lines.empty()) {
                        r.draw_text(font, x, y + asc * 0.82f, lines[0], alpha(fg, p->summary_ai ? 0.85f : 0.6f), p->summary_ai ? gfx::STYLE_REGULAR : gfx::STYLE_ITALIC, 0.82f);
                    }
                } else {
                    float pw = GUTTER_W_EST + p->cols * app_.cell_w() + 12.f;
                    float ph = Pane::TITLE_H + p->rows * app_.cell_h() + 12.f;
                    float th_ = std::min(110.f, ph * inner / std::max(1.f, pw));
                    Rect tb = {x, hy + 22, inner, th_};
                    PeekRail::draw_miniature(app_, *p, tb, ctx, true);
                    r.rounded_rect_stroke(tb, 3, alpha(fg, focused ? 0.35f : 0.15f), 1.f);
                    float y = tb.y + tb.h + 4;
                    for (auto& l : PeekRail::wrap_text(r, font, row.sub, inner, 0.85f, 2)) {
                        r.draw_text(font, x, y + asc * 0.85f, l, alpha(fg, p->summary_ai ? 0.95f : 0.7f), p->summary_ai ? gfx::STYLE_REGULAR : gfx::STYLE_ITALIC, 0.85f);
                        y += lh;
                    }
                    if (p->summary_pending) r.draw_text(font, right - 12, tb.y + tb.h + 4 + asc * 0.8f, SPIN[(int)(ctx.now * 8) % 4], alpha(col(th.accent), 0.8f), gfx::STYLE_REGULAR, 0.8f);
                }
            }
        } else {
            float x = rect.x + 16;
            r.draw_text(font, x, row.r.y + 5 + asc * 0.85f, "📌", fg, gfx::STYLE_REGULAR, 0.85f);
            r.push_clip({x + 20, row.r.y, rect.x + rect.w - x - 28, row.r.h});
            r.draw_text(font, x + 20, row.r.y + 5 + asc * 0.85f, ellipsize(row.label, 60), fg, gfx::STYLE_REGULAR, 0.85f);
            r.pop_clip();
        }
    }
    // drop indicator
    if (dragging_ && drop_index_ >= 0 && drop_index_ < (int)rows_.size()) {
        const Row& t = rows_[drop_index_];
        r.fill_rect({rect.x + 8, t.r.y + t.r.h / 2 - 1, rect.w - 16, 2}, col(th.accent));
    }
    r.pop_clip();
    // footer hints
    float fy = rect.y + rect.h - 10;
    r.fill_rect({rect.x, fy - 54, rect.w - 1, 54}, col(th.chrome_bg));
    r.fill_rect({rect.x, fy - 54, rect.w - 1, 1}, alpha(fg, 0.12f));
    r.draw_text(font, rect.x + 12, fy - 36, "Ctrl+Tab  find terminal", alpha(fg, 0.5f), gfx::STYLE_REGULAR, 0.8f);
    r.draw_text(font, rect.x + 12, fy - 20, "Ctrl+Shift+N  new child   Ctrl+Shift+O  overview", alpha(fg, 0.5f), gfx::STYLE_REGULAR, 0.8f);
    r.draw_text(font, rect.x + 12, fy - 4, "Ctrl+Shift+H  all keys", alpha(fg, 0.5f), gfx::STYLE_REGULAR, 0.8f);
}

bool Sidebar::mouse_down(float sx, float sy, MouseButton b, int clicks, uint32_t mods) {
    if (!visible || !rect.contains(sx, sy)) return false;
    build_rows();
    for (size_t i = 0; i < rows_.size(); i++) {
        const Row& row = rows_[i];
        if (!row.r.contains(sx, sy)) continue;
        if (row.kind == Row::Header) {
            if (row.label == "ACTIVITIES" && b == MouseButton::Left) {
                if (sx >= rect.x + rect.w - 32) app_.run_action("activity.new_child");
                else if (sx >= rect.x + rect.w - 56 && sx < rect.x + rect.w - 32) { compact = !compact; app_.request_redraw(); }
                else if (sx >= rect.x + rect.w - 82 && sx < rect.x + rect.w - 56) { app_.canvas().toggle_overview(); }
            }
            return true;
        }
        if (row.kind == Row::Pinned) {
            if (Pane* p = app_.pane(row.id)) {
                app_.focus(row.id, true);
                for (int k = 0; k < p->block_count(); k++) if (p->block(k)->id == row.block_id) { p->select_block(k, true); break; }
            }
            return true;
        }
        Activity* a = app_.tree().get(row.id);
        if (b == MouseButton::Left) {
            if (row.close_btn.contains(sx, sy)) {
                app_.close_activity(row.id);
                return true;
            }
            if (row.has_children && row.arrow.contains(sx, sy) && a) {
                a->collapsed = !a->collapsed;
                app_.canvas().layout();
                app_.tree().notify();
                return true;
            }
            Pane* sp = app_.pane(row.id);
            bool was_min = sp && sp->minimized();
            app_.focus(row.id, true);
            if (clicks == 2 && !was_min) app_.run_action("activity.rename");
            else { drag_id_ = row.id; drag_y_ = sy; dragging_ = false; }
        } else if (b == MouseButton::Middle) {
            app_.close_activity(row.id);
        } else if (b == MouseButton::Right) {
            uint64_t rid = row.id;
            app_.focus(rid, false);
            std::vector<MenuItem> items = {
                {"New child activity", [this] { app_.run_action("activity.new_child"); }},
                {"Rename…", [this] { app_.run_action("activity.rename"); }},
                {"Add note…", [this] { app_.run_action("activity.note"); }},
                {a && a->collapsed ? "Expand" : "Collapse", [this, a] { if (a) { a->collapsed = !a->collapsed; app_.canvas().layout(); } }, a && !a->children.empty()},
                {"-", nullptr},
                {"Close activity", [this, rid] { app_.close_activity(rid); }},
            };
            if (a && !a->children.empty()) {
                items.push_back({"Close activity & children", [this, rid] { app_.close_subtree(rid); }});
            }
            app_.show_menu(sx, sy, std::move(items));
        }
        return true;
    }
    return true;
}

void Sidebar::mouse_move(float sx, float sy) {
    hover_ = -1;
    hover_close_ = -1;
    if (!visible) return;
    if (drag_id_ && std::fabs(sy - drag_y_) > 6) dragging_ = true;
    if (!rect.contains(sx, sy) && !dragging_) return;
    for (size_t i = 0; i < rows_.size(); i++) {
        if (rows_[i].r.contains(sx, sy)) {
            hover_ = (int)i;
            if (rows_[i].kind == Row::Activity && rows_[i].close_btn.contains(sx, sy)) hover_close_ = (int)i;
            break;
        }
    }
    drop_index_ = dragging_ ? hover_ : -1;
    app_.request_redraw();
}

void Sidebar::mouse_up(float sx, float sy, MouseButton b) {
    if (dragging_ && drag_id_ && drop_index_ >= 0 && drop_index_ < (int)rows_.size()) {
        const Row& target = rows_[drop_index_];
        if (target.kind == Row::Activity && target.id != drag_id_) {
            // drop onto a row: become a child of it (before/after semantics kept simple)
            app_.tree().move(drag_id_, target.id, -1);
            app_.canvas().layout();
            app_.tree().notify();
            app_.save_state();
        }
    }
    drag_id_ = 0; dragging_ = false; drop_index_ = -1;
}

void Sidebar::wheel(float dy) {
    scroll_ = clampf(scroll_ - dy * 40, 0, std::max(0.f, content_h_ - rect.h));
    app_.request_redraw();
}

} // namespace canopy::ui
