#include "sidebar.h"
#include "canvas.h"
#include <algorithm>
#include <cmath>

namespace canopy::ui {

static const char* SPIN[] = {"◐", "◓", "◑", "◒"};

Sidebar::Sidebar(App& app) : app_(app) {}

void Sidebar::build_rows() {
    rows_.clear();
    const float ROW_H = 40.f, HDR_H = 30.f, PIN_H = 26.f;
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
        r.r = {rect.x, y, rect.w, ROW_H};
        r.arrow = {rect.x + 8 + depth * 14.f, y + 10, 16, 20};
        r.label = a->name;
        if (Pane* p = app_.pane(id)) r.sub = p->status_text();
        rows_.push_back(r); y += ROW_H;
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
                std::string plus = "+";
                float w = r.measure_text(font, plus);
                Rect pb = {rect.x + rect.w - 30, row.r.y + 4, 20, 20};
                bool hot = hover_ == (int)i;
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
            Activity* a = app_.tree().get(row.id);
            if (row.has_children) r.draw_text(font, x, row.r.y + 10 + asc * 0.8f, a && a->collapsed ? "▸" : "▾", alpha(fg, 0.7f), gfx::STYLE_REGULAR, 0.8f);
            x += 16;
            // status dot
            Pane* p = app_.pane(row.id);
            RGBA dot = alpha(fg, 0.3f); bool running = false;
            if (p && p->live()) {
                const auto& bl = p->session->blocks().blocks();
                for (size_t k = bl.size(); k-- > 0;) {
                    if (bl[k].state == BlockState::Running) { running = true; dot = col(th.accent); break; }
                    if (bl[k].state == BlockState::Done) { dot = bl[k].exit_code == 0 ? col(th.ok) : col(th.error); break; }
                }
                if (bl.empty()) dot = col(th.ok);
            } else if (p && p->session) dot = col(th.error);
            if (running) r.draw_text(font, x - 2, row.r.y + 9 + asc, SPIN[(int)(ctx.now * 8) % 4], dot, gfx::STYLE_REGULAR, 0.9f);
            else r.circle(x + 4, row.r.y + 14, 3.5f, dot);
            x += 16;
            r.push_clip({x, row.r.y, rect.x + rect.w - x - 8, row.r.h});
            r.draw_text(font, x, row.r.y + 6 + asc, row.label, focused ? col(th.foreground) : fg, gfx::STYLE_BOLD);
            if (!row.sub.empty()) r.draw_text(font, x, row.r.y + 22 + asc * 0.8f, row.sub, alpha(fg, 0.6f), gfx::STYLE_REGULAR, 0.8f);
            r.pop_clip();
            if (p && p->unread && !focused) r.circle(rect.x + rect.w - 16, row.r.y + 14, 3.5f, col(th.warning));
            if (a && !a->notes.empty()) r.draw_text(font, rect.x + rect.w - 30, row.r.y + 8 + asc * 0.8f, "✎", alpha(fg, 0.5f), gfx::STYLE_REGULAR, 0.8f);
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
    r.draw_text(font, rect.x + 12, fy - 36, "F1 overview   F2 sidebar", alpha(fg, 0.5f), gfx::STYLE_REGULAR, 0.8f);
    r.draw_text(font, rect.x + 12, fy - 20, "^⇧P palette   ^⇧N new child", alpha(fg, 0.5f), gfx::STYLE_REGULAR, 0.8f);
    r.draw_text(font, rect.x + 12, fy - 4, "^⇧↑↓ blocks   ^⇧F search", alpha(fg, 0.5f), gfx::STYLE_REGULAR, 0.8f);
}

bool Sidebar::mouse_down(float sx, float sy, MouseButton b, int clicks, uint32_t mods) {
    if (!visible || !rect.contains(sx, sy)) return false;
    build_rows();
    for (size_t i = 0; i < rows_.size(); i++) {
        const Row& row = rows_[i];
        if (!row.r.contains(sx, sy)) continue;
        if (row.kind == Row::Header) {
            if (row.label == "ACTIVITIES" && sx > rect.x + rect.w - 34 && b == MouseButton::Left) app_.run_action("activity.new_child");
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
            if (row.has_children && row.arrow.contains(sx, sy) && a) {
                a->collapsed = !a->collapsed;
                app_.canvas().layout();
                app_.tree().notify();
                return true;
            }
            app_.focus(row.id, true);
            if (clicks == 2) app_.run_action("activity.rename");
            else { drag_id_ = row.id; drag_y_ = sy; dragging_ = false; }
        } else if (b == MouseButton::Right) {
            app_.focus(row.id, false);
            std::vector<MenuItem> items = {
                {"New child activity", [this] { app_.run_action("activity.new_child"); }},
                {"Rename…", [this] { app_.run_action("activity.rename"); }},
                {"Add note…", [this] { app_.run_action("activity.note"); }},
                {a && a->collapsed ? "Expand" : "Collapse", [this, a] { if (a) { a->collapsed = !a->collapsed; app_.canvas().layout(); } }, a && !a->children.empty()},
                {"-", nullptr},
                {"Close activity", [this] { app_.run_action("activity.close"); }},
            };
            app_.show_menu(sx, sy, std::move(items));
        }
        return true;
    }
    return true;
}

void Sidebar::mouse_move(float sx, float sy) {
    hover_ = -1;
    if (!visible) return;
    if (drag_id_ && std::fabs(sy - drag_y_) > 6) dragging_ = true;
    if (!rect.contains(sx, sy) && !dragging_) return;
    for (size_t i = 0; i < rows_.size(); i++) if (rows_[i].r.contains(sx, sy)) { hover_ = (int)i; break; }
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
