#pragma once
// Activity tree model and persistence. No live-session state here (that's ui::Pane/Session).
#include "blocks.h"
#include "json.h"
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace canopy {

struct Activity {
    uint64_t id = 0;
    uint64_t parent = 0;               // 0 for root
    std::vector<uint64_t> children;    // ordered
    std::string name;
    std::string notes;
    std::string cwd;                   // last known cwd
    std::vector<std::string> env;      // extra KEY=VALUE for the shell
    int64_t created = 0, last_active = 0;  // unix seconds
    bool collapsed = false;            // in sidebar / canvas
    // canvas placement (world units == pixels at zoom 1). 0,0 + auto=true means auto-layout.
    float x = 0, y = 0;
    bool auto_place = true;
    int cols = 0, rows = 0;            // preferred size (0 = default)
    std::vector<Block> blocks;         // persisted blocks (with saved_output tails) + mirrored live blocks
    uint64_t next_block_id = 1;
    std::string color;                 // optional accent "#rrggbb"
};

class ActivityTree {
public:
    ActivityTree();
    uint64_t root() const { return root_; }
    Activity* get(uint64_t id);
    const Activity* get(uint64_t id) const;
    // Create a child under parent (0 => root). Returns the new id.
    uint64_t create(uint64_t parent, const std::string& name, const std::string& cwd);
    void remove(uint64_t id);                              // removes subtree
    void move(uint64_t id, uint64_t new_parent, int index = -1);
    void reorder(uint64_t id, int index);
    std::vector<uint64_t> preorder() const;                // root first, depth-first
    int depth(uint64_t id) const;
    uint64_t next_sibling(uint64_t id) const;             // 0 if none
    uint64_t prev_sibling(uint64_t id) const;
    size_t count() const { return nodes_.size(); }
    // Visit all nodes.
    void for_each(const std::function<void(Activity&)>& fn);

    Json to_json() const;
    bool from_json(const Json& j, std::string* err = nullptr);
    // Save/load at the default location (~/.local/share/canopy/tree.json), atomically.
    bool save(const std::string& path = "") const;
    bool load(const std::string& path = "");
    static std::string default_path();

    std::function<void()> on_changed;   // structural change notification (create/remove/move/rename)
    void notify() { if (on_changed) on_changed(); }

private:
    std::vector<std::unique_ptr<Activity>> nodes_;
    uint64_t root_ = 1;
    uint64_t next_id_ = 2;
};

// Block <-> JSON helpers (used by ActivityTree and tests).
Json block_to_json(const Block& b);
Block block_from_json(const Json& j);

} // namespace canopy
