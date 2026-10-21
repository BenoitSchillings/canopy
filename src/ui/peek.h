#pragma once
// Peek rail: live miniatures of the other terminals with a summary of what each is doing,
// ranked by what most likely needs the user. Click a card to jump there.
#include "app.h"
#include "pane.h"

namespace canopy::ui {

class PeekRail {
public:
    explicit PeekRail(App& app);
    bool visible = true;
    float width = 330.f;
    Rect rect;                                  // screen space
    void draw(const DrawCtx& ctx);
    bool mouse_down(float sx, float sy, MouseButton b, int clicks);
    void mouse_move(float sx, float sy);
    void wheel(float dy);
    // Draws one pane as a miniature into a screen-space box (shared with the switcher).
    static void draw_miniature(App& app, Pane& p, Rect box, const DrawCtx& ctx, bool align_bottom);
    static std::vector<std::string> wrap_text(gfx::Renderer& r, gfx::Font& font, const std::string& text, float maxw, float scale, size_t max_lines);

private:
    struct Card { uint64_t id; Rect r; Rect close_btn; };
    void build_cards();
    App& app_;
    std::vector<Card> cards_;
    float scroll_ = 0, content_h_ = 0;
    uint64_t hover_ = 0;
    uint64_t hover_close_ = 0;
};

} // namespace canopy::ui
