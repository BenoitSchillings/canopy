#pragma once
// Screen-space overlays: context menu, command palette, text prompt, search bar, help, toasts.
#include "app.h"

namespace canopy::ui {

class Overlay {
public:
    explicit Overlay(App& app);
    enum Mode { None, Menu, Palette, Prompt, Search, Help };
    Mode mode() const { return mode_; }
    bool active() const { return mode_ != None; }          // captures keyboard
    bool captures_mouse() const { return mode_ == Menu || mode_ == Palette || mode_ == Prompt || mode_ == Help; }

    void open_menu(float sx, float sy, std::vector<MenuItem> items);
    void open_palette(const std::string& initial = "");
    void open_prompt(const std::string& label, const std::string& initial, std::function<void(const std::string&)> on_done);
    void open_search();
    void toggle_help();
    void toast(const std::string& text, float seconds);
    void close();

    bool key(const KeyEvent& ev);
    bool text(const std::string& utf8);
    bool mouse_down(float sx, float sy, MouseButton b);
    void mouse_move(float sx, float sy);
    bool wheel(float sx, float sy, float dy);
    bool update(double now);
    void draw(const DrawCtx& ctx);

private:
    struct Cand { std::string label, detail, shortcut; std::function<void()> fn; int score = 0; };
    struct Toast { std::string text; double expires; double born; };
    void refilter();
    void build_palette_candidates();
    bool edit_key(const KeyEvent& ev);     // common text editing; true if handled
    Rect palette_rect() const;
    Rect menu_rect() const;
    Rect search_rect() const;

    App& app_;
    Mode mode_ = None;
    std::string input_, label_;
    std::function<void(const std::string&)> prompt_cb_;
    std::vector<Cand> all_, cands_;
    int sel_ = 0;
    std::vector<MenuItem> menu_;
    float mx_ = 0, my_ = 0;
    int menu_hover_ = -1;
    std::vector<Toast> toasts_;
    int help_scroll_ = 0;
    bool select_all_ = false;   // prompt: initial text is selected; first edit replaces it
};

} // namespace canopy::ui
