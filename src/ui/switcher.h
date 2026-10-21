#pragma once
// Terminal switcher (Ctrl+Tab): every terminal as a card with miniature, summary and status,
// ranked by attention, filterable by typing. Enter jumps to the selected one.
#include "app.h"
#include "pane.h"

namespace canopy::ui {

class Switcher {
public:
    explicit Switcher(App& app);
    bool open() const { return open_; }
    void show();
    void close();
    void step(int delta);                // Ctrl+Tab again / arrows
    bool key(const KeyEvent& ev);
    bool text(const std::string& utf8);
    bool mouse_down(float sx, float sy, MouseButton b);
    void mouse_move(float sx, float sy);
    void draw(const DrawCtx& ctx);

private:
    struct Entry { uint64_t id; int score; Rect r; Rect close_btn; };
    void rebuild();
    App& app_;
    bool open_ = false;
    std::string query_;
    std::vector<Entry> entries_;
    int sel_ = 0;
    int cols_ = 3;
    uint64_t hover_close_ = 0;
};

} // namespace canopy::ui
