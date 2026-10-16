#pragma once
// Activity outliner on the left: tree of activities with status, plus pinned blocks.
#include "app.h"
#include "pane.h"

namespace canopy::ui {

class Sidebar {
public:
    explicit Sidebar(App& app);
    bool visible = true;
    float width = 240.f;
    Rect rect;                      // screen space
    void draw(const DrawCtx& ctx);
    bool mouse_down(float sx, float sy, MouseButton b, int clicks, uint32_t mods);
    void mouse_move(float sx, float sy);
    void wheel(float dy);
    void mouse_up(float sx, float sy, MouseButton b);

private:
    struct Row {
        enum Kind { Activity, Header, Pinned } kind;
        uint64_t id = 0;            // activity id
        uint64_t block_id = 0;      // for pinned
        int depth = 0;
        Rect r;
        Rect arrow;
        bool has_children = false;
        std::string label, sub;
    };
    void build_rows();
    App& app_;
    std::vector<Row> rows_;
    float scroll_ = 0;
    int hover_ = -1;
    float content_h_ = 0;
    uint64_t drag_id_ = 0; float drag_y_ = 0; bool dragging_ = false;
    int drop_index_ = -1;
};

} // namespace canopy::ui
