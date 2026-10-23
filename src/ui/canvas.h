#pragma once
// The infinite zoomable canvas holding all panes, with camera animation and tree layout.
#include "app.h"
#include "pane.h"
#include <vector>

namespace canopy::ui {

class Canvas {
public:
    explicit Canvas(App& app);

    Rect viewport;                 // screen-space area of the canvas
    float scale = 1.f, tx = 0.f, ty = 0.f;
    gfx::Transform transform() const { return {scale, tx, ty}; }
    float to_screen_x(float wx) const { return wx * scale + tx; }
    float to_screen_y(float wy) const { return wy * scale + ty; }
    float to_world_x(float sx) const { return (sx - tx) / scale; }
    float to_world_y(float sy) const { return (sy - ty) / scale; }

    void fly_to_pane(Pane& p, bool instant = false);
    void ensure_visible(Pane& p);
    void fit_all(bool instant = false);
    void zoom_at(float sx, float sy, float factor);
    void set_zoom(float z);          // zoom around the viewport center
    void reset_zoom();               // 1.0 centered on the focused pane
    void toggle_overview();
    void exit_overview();
    bool overview() const { return overview_; }
    std::vector<uint64_t> ranked_panes(double now) const;
    bool key(const KeyEvent& ev);
    bool animating() const;

    void layout();                   // recompute auto-placed positions from the tree
    void sync_panes();               // pick up new/removed panes
    void bring_to_front(uint64_t id);
    bool update(double now, double dt);   // returns true if a redraw is needed
    void draw(const DrawCtx& base);

    // input, screen coordinates
    void mouse_down(float sx, float sy, MouseButton b, int clicks, uint32_t mods);
    void mouse_up(float sx, float sy, MouseButton b, uint32_t mods);
    void mouse_move(float sx, float sy, uint32_t mods, MouseButton held);
    void wheel(float sx, float sy, float dx, float dy, uint32_t mods);
    Pane* pane_at(float wx, float wy);
    void focus_direction(int dx, int dy);
    int cursor_for(float sx, float sy);
    Pane* hover_pane() const { return hover_pane_; }

    static constexpr float GAP_X = 70.f, GAP_Y = 36.f;

private:
    float layout_subtree(uint64_t id, float x, float y);
    void draw_background();
    void draw_connectors();
    void animate_to(float s, float x, float y, bool instant);

    App& app_;
    std::vector<uint64_t> order_;           // z-order, back to front
    float tscale_ = 1.f, ttx_ = 0.f, tty_ = 0.f;
    bool overview_ = false;
    uint64_t overview_sel_ = 0;
    float pre_overview_scale_ = 1.f, pre_overview_tx_ = 0.f, pre_overview_ty_ = 0.f;
    Rect hud_tree_rect_ = {0, 0, 0, 0};
    Rect hud_next_rect_ = {0, 0, 0, 0};
    uint64_t hud_next_id_ = 0;
    bool hover_hud_tree_ = false;
    bool hover_hud_next_ = false;
    enum Drag { None, Pan, Move, Resize, PaneDrag } drag_ = None;
    Pane* drag_pane_ = nullptr;
    float drag_x_ = 0, drag_y_ = 0, drag_px_ = 0, drag_py_ = 0;
    int drag_cols_ = 0, drag_rows_ = 0;
    bool moved_ = false;
    Pane* hover_pane_ = nullptr;
    MouseButton held_ = MouseButton::None;
    bool first_layout_ = true;
};

} // namespace canopy::ui
