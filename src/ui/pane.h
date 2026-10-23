#pragma once
// A Pane is the view of one Activity: a terminal grid with block gutter, selection, structure.
#include "app.h"
#include "core/blocks.h"
#include "core/keys.h"
#include "core/structure.h"
#include <optional>
#include <unordered_map>

namespace canopy::ui {

struct VisRow {
    enum Kind : uint8_t { Term, Summary, Virtual, Restored };
    Kind kind = Term;
    int64_t abs = -1;     // Term: absolute line
    int block = -1;       // Summary/Virtual: block index; Term: block index containing it (or -1)
    int vidx = 0;         // Virtual: index into BlockView::virtual_lines; Restored: index into restored_
};

struct Selection {
    bool active = false;
    int64_t al = 0, hl = 0;   // anchor / head line (absolute)
    int ac = 0, hc = 0;       // anchor / head col
    int mode = 0;             // 0 char, 1 word, 2 line
    bool empty() const { return !active || (al == hl && ac == hc); }
    void ordered(int64_t& l0, int& c0, int64_t& l1, int& c1) const {
        if (al < hl || (al == hl && ac <= hc)) { l0 = al; c0 = ac; l1 = hl; c1 = hc; }
        else { l0 = hl; c0 = hc; l1 = al; c1 = ac; }
    }
};

struct BlockView {
    bool scanned = false;
    int error_count = 0, warning_count = 0;
    std::vector<int64_t> error_lines, warning_lines;
    TableInfo table;
    bool is_json = false;
    int sort_col = -1; bool sort_desc = false;
    std::vector<int64_t> order;           // permuted absolute lines of data rows (when sorted)
    std::vector<Line> virtual_lines;      // e.g. pretty-printed JSON
    bool show_virtual = false;
    int64_t output_lines = 0;
};

class Pane {
public:
    Pane(App& app, uint64_t activity_id);
    ~Pane();

    uint64_t activity_id;
    float x = 0, y = 0;         // world position (top-left of card)
    int cols = 80, rows = 24;

    static constexpr float TITLE_H = 30.f, GUTTER_W = 18.f, PAD = 6.f, RADIUS = 10.f, HANDLE = 14.f;
    Rect rect() const;
    Rect title_rect() const;
    Rect artifact_chip_rect() const;
    Rect min_rect() const;
    Rect close_rect() const;
    Rect term_rect() const;
    Rect gutter_rect() const;
    Rect resize_handle() const;
    Rect drawer_rect() const;
    Rect drawer_close_rect() const;
    Rect drawer_prev_rect() const;
    Rect drawer_next_rect() const;
    Rect drawer_proceed_rect() const;
    Rect drawer_feedback_rect() const;
    Rect drawer_edit_rect() const;
    Rect drawer_body_rect() const;
    float width() const { return rect().w; }
    float height() const { return rect().h; }
    bool minimized() const;
    void set_minimized(bool m);
    void toggle_minimized() { set_minimized(!minimized()); }

    // ---- AGY artifacts & plan drawer ----
    const std::vector<AgyArtifact>& artifacts() const { return artifacts_; }
    const AgyArtifact* active_artifact() const;
    bool drawer_open() const { return drawer_open_ && !artifacts_.empty(); }
    void set_drawer_open(bool open);
    void toggle_drawer();
    void select_artifact(int idx);
    void step_artifact(int delta);
    void open_artifact(const std::string& path);
    void refresh_artifacts();
    bool artifact_unread() const { return artifact_unread_; }

    // ---- session ----
    std::unique_ptr<Session> session;
    bool live() const { return session && session->alive(); }
    bool start_session(std::string* err = nullptr);
    void stop_session();
    void queue_input_at_prompt(const std::string& text);
    bool at_prompt() const;
    std::string cwd() const;

    // ---- per frame ----
    bool update(double now);               // true if anything changed
    void draw(gfx::Renderer& r, gfx::Font& font, const DrawCtx& ctx);

    // ---- input (world coordinates) ----
    void mouse_down(float wx, float wy, MouseButton b, int clicks, uint32_t mods);
    void mouse_up(float wx, float wy, MouseButton b, uint32_t mods);
    void mouse_move(float wx, float wy, uint32_t mods, MouseButton held);
    void wheel(float wx, float wy, float dx, float dy, uint32_t mods);
    bool key(const KeyEvent& ev);          // true if consumed
    void text_input(const std::string& utf8);
    void paste(const std::string& text);
    void write(std::string_view bytes);

    // ---- selection / clipboard ----
    bool has_selection() const { return !sel_.empty(); }
    std::string selected_text() const;
    void clear_selection();
    void select_all_block(int idx);

    // ---- scrolling ----
    void scroll_lines(int delta);
    void scroll_pages(int delta);
    void scroll_to_bottom();
    void scroll_to_abs(int64_t abs);
    bool following() const { return follow_; }
    int64_t lines_below() const;
    Rect scroll_pill_rect() const;

    // ---- blocks ----
    int block_count() const;
    const Block* block(int idx) const;
    int selected_block = -1;
    void select_block(int idx, bool scroll_into_view);
    void select_block_relative(int delta);
    int block_at_cursor() const;           // block index containing the cursor line
    void toggle_fold(int idx);
    void set_fold(int idx, bool folded);
    void rerun(int idx);
    std::string block_output(int idx, int max_lines = -1) const;
    std::string block_command(int idx) const;
    void toggle_pretty_json(int idx);
    void sort_table(int idx, int col);
    void clear_sort(int idx);
    BlockView& view(const Block& b);
    void next_error(int dir);
    std::vector<Block> export_blocks(int max_blocks, int tail_lines) const;
    void load_restored(const std::vector<Block>& saved);
    bool has_restored() const { return !restored_.empty(); }
    const std::vector<Block>& restored_blocks() const { return restored_blocks_; }

    // ---- search ----
    void set_search(const std::string& q);
    bool search_step(int dir);
    int search_count() const { return (int)matches_.size(); }
    int search_index() const { return match_idx_; }
    const std::string& search_query() const { return search_; }

    // ---- hover ----
    struct Hover {
        bool valid = false;
        int64_t abs = -1; int col = -1; int row = -1;
        int block = -1;
        bool gutter = false, title = false, artifact_chip = false, min_btn = false, close_btn = false, handle = false, summary = false, chip = false, header = false, scroll_pill = false;
        bool drawer = false, drawer_close = false, drawer_prev = false, drawer_next = false;
        bool drawer_proceed = false, drawer_feedback = false, drawer_edit = false;
        int table_col = -1;
        std::optional<Span> span;
        const Hyperlink* link = nullptr;
        std::string link_uri;
    } hover;
    void set_hover(float wx, float wy);
    void clear_hover() { hover = {}; }
    int system_cursor() const;             // SDL_SYSTEM_CURSOR_* for current hover

    // ---- summary (what this terminal is doing) ----
    std::string summary;                   // shown in rail/switcher/sidebar; AI or heuristic
    bool summary_ai = false;               // produced by the model
    bool summary_pending = false;          // request in flight
    double summary_time = 0;               // steady seconds when last set
    uint64_t summary_hash = 0;             // context hash the AI summary was made from
    bool summary_dirty = true;             // content changed since the last request
    double summary_dirty_since = 0;
    std::string summary_context() const;   // snapshot sent to the model
    std::string local_summary() const;     // instant heuristic
    uint64_t context_hash() const;
    // Relevance for ranking in the rail/switcher: higher = more likely what the user is looking for.
    float attention_score(double now, uint64_t focused) const;
    std::vector<std::string> attention_badges(double now) const;

    // ---- state ----
    enum class AgentState : uint8_t { None = 0, Streaming, WaitingInput, NeedsApproval };
    AgentState agent_state() const { return agent_state_; }
    bool waiting_for_user() const { return agent_state_ == AgentState::WaitingInput || agent_state_ == AgentState::NeedsApproval || bell_unread_; }
    bool is_agent_session() const;
    void clear_attention();
    bool unread = false;
    double last_output = 0;
    double last_finish = 0;
    std::string last_title;
    std::string status_text() const;       // for sidebar/title
    void resize_cells(int c, int r);
    const std::vector<VisRow>& vis();
    int vis_top() const { return (int)top_; }

    // cell geometry
    float cell_w() const;
    float cell_h() const;
    bool cell_at(float wx, float wy, int* row, int* col) const;   // display row/col in term area

private:
    void rebuild_vis();
    const Line* line_for(const VisRow& vr) const;
    int row_of_abs(int64_t abs) const;      // display row index in vis (or -1)
    void draw_line(gfx::Renderer& r, gfx::Font& font, const Line& ln, float x0, float y0, const DrawCtx& ctx, const VisRow& vr, int disp_row);
    void draw_gutter(gfx::Renderer& r, gfx::Font& font, const DrawCtx& ctx, int first, int count);
    void draw_summary(gfx::Renderer& r, gfx::Font& font, const DrawCtx& ctx, const VisRow& vr, float y0);
    void draw_chip(gfx::Renderer& r, gfx::Font& font, const DrawCtx& ctx, const Block& b, float y0);
    void draw_cursor(gfx::Renderer& r, const DrawCtx& ctx);
    void draw_title(gfx::Renderer& r, gfx::Font& font, const DrawCtx& ctx);
    void draw_scrollbar(gfx::Renderer& r, const DrawCtx& ctx);
    void draw_scroll_pill(gfx::Renderer& r, gfx::Font& font, const DrawCtx& ctx);
    void draw_dormant(gfx::Renderer& r, gfx::Font& font, const DrawCtx& ctx);
    void draw_summary_banner(gfx::Renderer& r, gfx::Font& font, const DrawCtx& ctx);
    void draw_artifact_drawer(gfx::Renderer& r, gfx::Font& font, const DrawCtx& ctx);
    void update_agent_state(double now);
    bool update_artifacts(double now);
    void ensure_artifact_loaded();
    void scan_block(const Block& b, BlockView& v);
    void extend_selection(int64_t abs, int col);
    bool selected(int64_t abs, int col) const;
    void send_mouse(const MouseEvent& ev);
    bool mouse_to_app() const;             // terminal wants mouse events
    void clamp_scroll();
    std::vector<std::string> block_lines(const Block& b, bool join_wrapped = true) const;
    Line make_line(const std::string& utf8, Color fg, uint16_t attrs) const;

    App& app_;
    std::vector<VisRow> vis_;
    uint64_t vis_gen_ = ~0ull;
    bool vis_dirty_ = true;
    int64_t top_ = 0;
    int64_t anchor_abs_ = INT64_MIN;
    bool follow_ = true;
    Selection sel_;
    bool selecting_ = false;
    std::unordered_map<uint64_t, BlockView> views_;
    std::vector<std::string> pending_input_;
    std::vector<Line> restored_;
    std::vector<int> restored_block_of_;   // block index (into restored_blocks_) per restored line
    std::vector<Block> restored_blocks_;
    std::string search_;
    struct Match { int64_t abs; int col, len; };
    std::vector<Match> matches_;
    int match_idx_ = -1;
    uint64_t last_gen_ = 0;
    int64_t last_history_base_ = 0;
    size_t last_block_count_ = 0;
    double exited_at_ = 0;
    double last_input_ = 0;
    double sync_start_ = 0;
    bool output_since_input_ = false;
    bool agent_notified_ = false;
    bool bell_unread_ = false;
    std::string notify_msg_;
    AgentState agent_state_ = AgentState::None;
    std::string err_;

    struct MdLine {
        enum Kind : uint8_t { Normal = 0, H1, H2, H3, Code, Quote, AlertNote, AlertTip, AlertImportant, AlertWarning, AlertCaution, Rule, SummaryMeta };
        Kind kind = Normal;
        std::string text;
        int indent = 0;
    };
    std::vector<AgyArtifact> artifacts_;
    std::vector<std::string> pinned_artifacts_;
    int artifact_idx_ = 0;
    bool drawer_open_ = false;
    bool artifact_unread_ = false;
    int64_t last_artifact_mtime_ = 0;
    double last_artifact_scan_ = 0;
    float drawer_scroll_ = 0.f;
    std::string loaded_artifact_path_;
    int64_t loaded_artifact_mtime_ = 0;
    int loaded_wrap_cols_ = 0;
    std::vector<MdLine> md_lines_;
};

} // namespace canopy::ui
