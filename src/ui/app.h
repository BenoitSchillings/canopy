#pragma once
// Application: owns window, renderer, font, config, activity tree, panes, canvas, sidebar, overlays.
#include "common.h"
#include "core/activity.h"
#include "core/config.h"
#include "core/session.h"
#include "core/summarizer.h"
#include "core/structure.h"
#include "gfx/font.h"
#include "gfx/renderer.h"
#include "gfx/window.h"
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace canopy::ui {

class Pane;
class Canvas;
class Sidebar;
class Overlay;
class PeekRail;
class Switcher;

struct DrawCtx {
    const Config* cfg = nullptr;
    bool focused = false;       // this pane has keyboard focus
    float scale = 1.f;          // camera scale (for LOD decisions)
    double now = 0;
    bool cursor_on = true;      // blink phase
    bool overview = false;
    bool window_focused = true;
    bool in_rail = false;       // drawn as a miniature in the peek rail / switcher
};

struct Action {
    std::string id;
    std::string label;
    std::string shortcut;
    std::function<void()> fn;
    std::function<bool()> enabled;   // optional
};

struct MenuItem {
    std::string label;      // "-" => separator
    std::function<void()> fn;
    bool enabled = true;
};

class App {
public:
    App();
    ~App();
    bool init(int argc, char** argv, std::string* err);
    int run();

    // ---- services used by UI components ----
    const Config& config() const { return cfg_; }
    Config& config() { return cfg_; }
    const Theme& theme() const { return cfg_.theme; }
    gfx::Font& font() { return *font_; }
    gfx::Renderer& renderer() { return *renderer_; }
    gfx::Window& window() { return *window_; }
    ActivityTree& tree() { return tree_; }
    Canvas& canvas() { return *canvas_; }
    Overlay& overlay() { return *overlay_; }
    PeekRail& rail() { return *rail_; }
    Switcher& switcher() { return *switcher_; }
    const std::vector<uint64_t>& mru() const { return mru_; }
    const char* summary_backend_name() const;
    void request_summary(uint64_t activity, bool force);
    double now() const;
    void request_redraw() { redraw_ = true; }
    void wake();                                   // thread-safe

    // panes / activities
    Pane* pane(uint64_t activity_id);
    Pane* focused_pane();
    uint64_t focused_id() const { return focused_; }
    const std::map<uint64_t, std::unique_ptr<Pane>>& panes() const { return panes_; }
    void focus(uint64_t activity_id, bool fly = true);
    uint64_t new_activity(uint64_t parent, const std::string& name, const std::string& cwd, bool focus_it = true);
    void close_activity(uint64_t id);
    void rename_activity(uint64_t id, const std::string& name);
    void add_note(uint64_t id, const std::string& note);
    // Create a child activity under `parent` and run `command` in it once the prompt is ready.
    uint64_t run_in_child(uint64_t parent, const std::string& command, const std::string& cwd, const std::string& name = "");
    void ensure_session(Pane& p);               // start a dormant pane's shell

    // actions & menus
    const std::vector<Action>& actions() const { return actions_; }
    void run_action(const std::string& id);
    void show_menu(float sx, float sy, std::vector<MenuItem> items);
    void toast(const std::string& text, float seconds = 4.f);
    void prompt_text(const std::string& label, const std::string& initial, std::function<void(const std::string&)> on_done);

    // helpers
    void open_url(const std::string& url);
    void open_path(const std::string& path, int line, const std::string& cwd, uint64_t from_activity);
    void set_clipboard(const std::string& s);
    std::string clipboard();
    void set_primary(const std::string& s);
    std::string primary();
    void save_state();
    void bell(uint64_t activity);
    void on_block_finished(uint64_t activity, const Block& b);
    void on_shell_request(uint64_t activity, const std::string& kind, const std::string& payload);
    void diff_outputs(uint64_t activity, const std::string& cmd, const std::string& older, const std::string& newer);
    std::string shell_dir() const { return shell_dir_; }

    // metrics
    float cell_w() const;
    float cell_h() const;
    void set_font_size(float px);

private:
    void setup_actions();
    bool handle_global_key(const KeyEvent& ev);
    void handle_event(const gfx::Event& ev);
    void frame(double now, double dt);
    void draw(double now);
    void restore_tree();
    void create_pane_for(uint64_t id);
    void place_new_pane(Pane& p, uint64_t parent);
    void on_block_request(uint64_t activity, const std::string& kind, const std::string& payload);
    void check_notifications(double now);

    Config cfg_;
    std::unique_ptr<gfx::Window> window_;
    std::unique_ptr<gfx::Renderer> renderer_;
    std::unique_ptr<gfx::Font> font_;
    ActivityTree tree_;
    std::map<uint64_t, std::unique_ptr<Pane>> panes_;
    std::unique_ptr<Canvas> canvas_;
    std::unique_ptr<Sidebar> sidebar_;
    std::unique_ptr<Overlay> overlay_;
    std::unique_ptr<PeekRail> rail_;
    std::unique_ptr<Switcher> switcher_;
    Summarizer summarizer_;
    std::vector<uint64_t> mru_;
    double last_summary_submit_ = 0;
    void pump_summaries(double now);
    void layout_chrome();
    std::vector<Action> actions_;
    uint64_t focused_ = 0;
    bool redraw_ = true;
    bool quit_ = false;
    double last_save_ = 0;
    double last_blink_ = 0;
    bool cursor_on_ = true;
    std::string shell_dir_;
    std::string title_;
    double start_time_ = 0;
    friend class Pane;
    friend class Canvas;
    friend class Sidebar;
    friend class Overlay;
    friend class PeekRail;
    friend class Switcher;
};

} // namespace canopy::ui
