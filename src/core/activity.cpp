// Activity tree model + JSON persistence.
#include "activity.h"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <set>
#include <sstream>
#include <sys/stat.h>
#include <pwd.h>
#include <unistd.h>

namespace canopy {

namespace {

int64_t unix_now() {
    return (int64_t)std::chrono::duration_cast<std::chrono::seconds>(
               std::chrono::system_clock::now().time_since_epoch()).count();
}

std::string home_dir() {
    const char* h = std::getenv("HOME");
    if (h && *h) return h;
    if (const passwd* pw = getpwuid(getuid())) {
        if (pw->pw_dir && *pw->pw_dir) return pw->pw_dir;
    }
    return "/";
}

// mkdir -p for the directory part of `path`.
bool make_parent_dirs(const std::string& path) {
    size_t slash = path.rfind('/');
    if (slash == std::string::npos || slash == 0) return true;
    std::string dir = path.substr(0, slash);
    for (size_t i = 1; i <= dir.size(); i++) {
        if (i == dir.size() || dir[i] == '/') {
            std::string part = dir.substr(0, i);
            if (::mkdir(part.c_str(), 0755) != 0 && errno != EEXIST) return false;
        }
    }
    return true;
}

bool file_exists(const std::string& path) {
    struct stat st;
    return ::stat(path.c_str(), &st) == 0;
}

const char* state_name(BlockState s) {
    switch (s) {
    case BlockState::Prompt: return "prompt";
    case BlockState::Typing: return "typing";
    case BlockState::Running: return "running";
    case BlockState::Done: return "done";
    }
    return "prompt";
}

BlockState state_from_name(const std::string& s) {
    if (s == "typing") return BlockState::Typing;
    if (s == "running") return BlockState::Running;
    if (s == "done") return BlockState::Done;
    return BlockState::Prompt;
}

Json strings_to_json(const std::vector<std::string>& v) {
    Json::Array a;
    a.reserve(v.size());
    for (const auto& s : v) a.emplace_back(s);
    return Json(std::move(a));
}

std::vector<std::string> strings_from_json(const Json& j) {
    std::vector<std::string> out;
    for (const auto& e : j.as_array()) out.push_back(e.as_string());
    return out;
}

} // namespace

// ---------------------------------------------------------------------------
// Block <-> JSON
// ---------------------------------------------------------------------------
Json block_to_json(const Block& b) {
    Json j = Json::object();
    j["id"] = Json((uint64_t)b.id);
    j["state"] = Json(state_name(b.state));
    j["command"] = Json(b.command);
    j["cwd"] = Json(b.cwd);
    j["exit_code"] = Json(b.exit_code);
    // start_time/end_time are steady-clock values and meaningless across runs;
    // persist only the duration (see block_from_json for how it is restored).
    j["duration"] = Json(b.duration());
    j["unix_start"] = Json((int64_t)b.unix_start);
    j["prompt_line"] = Json((int64_t)b.prompt_line);
    j["cmd_line"] = Json((int64_t)b.cmd_line);
    j["output_line"] = Json((int64_t)b.output_line);
    j["end_line"] = Json((int64_t)b.end_line);
    j["output_col_end"] = Json(b.output_col_end);
    j["folded"] = Json(b.folded);
    j["pinned"] = Json(b.pinned);
    j["marks"] = strings_to_json(b.marks);
    j["saved_output"] = strings_to_json(b.saved_output);
    return j;
}

Block block_from_json(const Json& j) {
    Block b;
    b.id = (uint64_t)j["id"].as_int(0);
    b.state = state_from_name(j["state"].as_string());
    b.command = j["command"].as_string();
    b.cwd = j["cwd"].as_string();
    b.exit_code = (int)j["exit_code"].as_int(-1);
    // Duration trick: Block has no persisted-duration field and we must not
    // change blocks.h. Block::duration() returns end_time - start_time when both
    // are > 0, so a restored block gets the synthetic anchors start_time = 1.0,
    // end_time = 1.0 + duration. These are not real steady-clock values, but
    // restored blocks are never "running", so nothing compares them to now().
    double dur = j["duration"].as_double(0);
    if (dur > 0) { b.start_time = 1.0; b.end_time = 1.0 + dur; }
    else { b.start_time = 0; b.end_time = 0; }
    b.unix_start = j["unix_start"].as_int(0);
    b.prompt_line = j["prompt_line"].as_int(-1);
    b.cmd_line = j["cmd_line"].as_int(-1);
    b.output_line = j["output_line"].as_int(-1);
    b.end_line = j["end_line"].as_int(-1);
    b.output_col_end = (int)j["output_col_end"].as_int(0);
    b.folded = j["folded"].as_bool(false);
    b.pinned = j["pinned"].as_bool(false);
    b.marks = strings_from_json(j["marks"]);
    b.saved_output = strings_from_json(j["saved_output"]);
    return b;
}

// ---------------------------------------------------------------------------
// Activity <-> JSON
// ---------------------------------------------------------------------------
static Json activity_to_json(const Activity& a) {
    Json j = Json::object();
    j["id"] = Json((uint64_t)a.id);
    j["parent"] = Json((uint64_t)a.parent);
    Json::Array ch;
    ch.reserve(a.children.size());
    for (auto c : a.children) ch.emplace_back((uint64_t)c);
    j["children"] = Json(std::move(ch));
    j["name"] = Json(a.name);
    j["notes"] = Json(a.notes);
    j["summary"] = Json(a.summary);
    j["summary_time"] = Json(a.summary_time);
    j["summary_ai"] = Json(a.summary_ai);
    j["auto_named"] = Json(a.auto_named);
    j["cwd"] = Json(a.cwd);
    j["env"] = strings_to_json(a.env);
    j["created"] = Json((int64_t)a.created);
    j["last_active"] = Json((int64_t)a.last_active);
    j["collapsed"] = Json(a.collapsed);
    if (a.minimized) j["minimized"] = Json(true);
    j["x"] = Json((double)a.x);
    j["y"] = Json((double)a.y);
    j["auto_place"] = Json(a.auto_place);
    j["cols"] = Json(a.cols);
    j["rows"] = Json(a.rows);
    Json::Array blocks;
    blocks.reserve(a.blocks.size());
    for (const auto& b : a.blocks) blocks.push_back(block_to_json(b));
    j["blocks"] = Json(std::move(blocks));
    if (!a.history.empty()) j["history"] = strings_to_json(a.history);
    j["next_block_id"] = Json((uint64_t)a.next_block_id);
    j["color"] = Json(a.color);
    return j;
}

static std::unique_ptr<Activity> activity_from_json(const Json& j) {
    auto a = std::make_unique<Activity>();
    a->id = (uint64_t)j["id"].as_int(0);
    a->parent = (uint64_t)j["parent"].as_int(0);
    for (const auto& c : j["children"].as_array()) a->children.push_back((uint64_t)c.as_int(0));
    a->name = j["name"].as_string();
    a->notes = j["notes"].as_string();
    a->summary = j["summary"].as_string();
    a->summary_time = j["summary_time"].as_int();
    a->summary_ai = j["summary_ai"].as_bool();
    a->auto_named = j["auto_named"].as_bool();
    a->cwd = j["cwd"].as_string();
    a->env = strings_from_json(j["env"]);
    a->created = j["created"].as_int(0);
    a->last_active = j["last_active"].as_int(0);
    a->collapsed = j["collapsed"].as_bool(false);
    a->minimized = j["minimized"].as_bool(false);
    a->x = (float)j["x"].as_double(0);
    a->y = (float)j["y"].as_double(0);
    a->auto_place = j["auto_place"].as_bool(true);
    a->cols = (int)j["cols"].as_int(0);
    a->rows = (int)j["rows"].as_int(0);
    for (const auto& b : j["blocks"].as_array()) a->blocks.push_back(block_from_json(b));
    if (j.has("history")) a->history = strings_from_json(j["history"]);
    a->next_block_id = (uint64_t)j["next_block_id"].as_int(1);
    a->color = j["color"].as_string();
    return a;
}

// ---------------------------------------------------------------------------
// ActivityTree
// ---------------------------------------------------------------------------
ActivityTree::ActivityTree() {
    auto root = std::make_unique<Activity>();
    root->id = root_;
    root->parent = 0;
    root->name = "Home";
    root->cwd = home_dir();
    root->created = root->last_active = unix_now();
    nodes_.push_back(std::move(root));
    next_id_ = root_ + 1;
}

Activity* ActivityTree::get(uint64_t id) {
    for (auto& n : nodes_) if (n->id == id) return n.get();
    return nullptr;
}

const Activity* ActivityTree::get(uint64_t id) const {
    for (const auto& n : nodes_) if (n->id == id) return n.get();
    return nullptr;
}

uint64_t ActivityTree::create(uint64_t parent, const std::string& name, const std::string& cwd) {
    Activity* p = get(parent == 0 ? root_ : parent);
    if (!p) p = get(root_);
    auto a = std::make_unique<Activity>();
    a->id = next_id_++;
    a->parent = p->id;
    a->name = name;
    a->cwd = cwd.empty() ? p->cwd : cwd;
    a->env = p->env;                      // children inherit the parent's environment
    a->created = a->last_active = unix_now();
    uint64_t id = a->id;
    p->children.push_back(id);
    nodes_.push_back(std::move(a));
    notify();
    return id;
}

void ActivityTree::remove(uint64_t id) {
    if (id == root_) return;
    Activity* a = get(id);
    if (!a) return;
    // Collect the subtree.
    std::vector<uint64_t> doomed;
    std::vector<uint64_t> stack{id};
    while (!stack.empty()) {
        uint64_t cur = stack.back();
        stack.pop_back();
        doomed.push_back(cur);
        if (const Activity* n = get(cur))
            for (auto c : n->children) stack.push_back(c);
    }
    // Detach from parent.
    if (Activity* p = get(a->parent)) {
        auto& ch = p->children;
        ch.erase(std::remove(ch.begin(), ch.end(), id), ch.end());
    }
    nodes_.erase(std::remove_if(nodes_.begin(), nodes_.end(), [&](const std::unique_ptr<Activity>& n) {
        return std::find(doomed.begin(), doomed.end(), n->id) != doomed.end();
    }), nodes_.end());
    notify();
}

uint64_t ActivityTree::remove_one(uint64_t id) {
    if (nodes_.size() <= 1) return 0;
    Activity* a = get(id);
    if (!a) return 0;
    uint64_t next_focus = 0;
    if (id == root_) {
        if (a->children.empty()) return 0;
        uint64_t nr_id = a->children[0];
        Activity* nr = get(nr_id);
        if (!nr) return 0;
        nr->parent = 0;
        for (size_t i = 1; i < a->children.size(); i++) {
            uint64_t cid = a->children[i];
            if (Activity* ch = get(cid)) ch->parent = nr_id;
            nr->children.push_back(cid);
        }
        root_ = nr_id;
        next_focus = nr_id;
    } else {
        uint64_t pid = a->parent;
        Activity* p = get(pid);
        if (!p) p = get(root_);
        uint64_t sib = next_sibling(id);
        if (!sib) sib = prev_sibling(id);
        next_focus = !a->children.empty() ? a->children[0] : (sib ? sib : (p ? p->id : root_));
        if (p) {
            auto& ch = p->children;
            auto it = std::find(ch.begin(), ch.end(), id);
            size_t idx = (it != ch.end()) ? (size_t)(it - ch.begin()) : ch.size();
            if (it != ch.end()) ch.erase(it);
            for (size_t i = 0; i < a->children.size(); i++) {
                uint64_t cid = a->children[i];
                if (Activity* c = get(cid)) c->parent = p->id;
                ch.insert(ch.begin() + (long)(idx + i), cid);
            }
        }
    }
    nodes_.erase(std::remove_if(nodes_.begin(), nodes_.end(), [&](const std::unique_ptr<Activity>& n) {
        return n->id == id;
    }), nodes_.end());
    notify();
    return next_focus;
}

void ActivityTree::move(uint64_t id, uint64_t new_parent, int index) {
    if (id == root_) return;
    Activity* a = get(id);
    if (!a) return;
    if (new_parent == 0) new_parent = root_;
    Activity* np = get(new_parent);
    if (!np) return;
    // Refuse to move a node under itself or one of its descendants.
    for (uint64_t cur = new_parent; cur != 0; ) {
        if (cur == id) return;
        const Activity* n = get(cur);
        if (!n) break;
        cur = n->parent;
    }
    if (Activity* op = get(a->parent)) {
        auto& ch = op->children;
        ch.erase(std::remove(ch.begin(), ch.end(), id), ch.end());
    }
    auto& ch = np->children;
    if (index < 0 || (size_t)index >= ch.size()) ch.push_back(id);
    else ch.insert(ch.begin() + index, id);
    a->parent = new_parent;
    notify();
}

void ActivityTree::reorder(uint64_t id, int index) {
    const Activity* a = get(id);
    if (!a || id == root_) return;
    move(id, a->parent, index);
}

std::vector<uint64_t> ActivityTree::preorder() const {
    std::vector<uint64_t> out;
    out.reserve(nodes_.size());
    std::function<void(uint64_t)> visit = [&](uint64_t id) {
        const Activity* a = get(id);
        if (!a) return;
        out.push_back(id);
        for (auto c : a->children) visit(c);
    };
    visit(root_);
    return out;
}

int ActivityTree::depth(uint64_t id) const {
    int d = 0;
    const Activity* a = get(id);
    if (!a) return -1;
    while (a && a->parent != 0) {
        a = get(a->parent);
        d++;
        if (d > (int)nodes_.size()) break;   // corrupt cycle guard
    }
    return d;
}

uint64_t ActivityTree::next_sibling(uint64_t id) const {
    const Activity* a = get(id);
    if (!a) return 0;
    const Activity* p = get(a->parent);
    if (!p) return 0;
    auto it = std::find(p->children.begin(), p->children.end(), id);
    if (it == p->children.end() || it + 1 == p->children.end()) return 0;
    return *(it + 1);
}

uint64_t ActivityTree::prev_sibling(uint64_t id) const {
    const Activity* a = get(id);
    if (!a) return 0;
    const Activity* p = get(a->parent);
    if (!p) return 0;
    auto it = std::find(p->children.begin(), p->children.end(), id);
    if (it == p->children.end() || it == p->children.begin()) return 0;
    return *(it - 1);
}

void ActivityTree::for_each(const std::function<void(Activity&)>& fn) {
    for (auto& n : nodes_) fn(*n);
}

Json ActivityTree::to_json() const {
    Json j = Json::object();
    j["version"] = Json(1);
    j["root"] = Json((uint64_t)root_);
    j["next_id"] = Json((uint64_t)next_id_);
    Json::Array acts;
    acts.reserve(nodes_.size());
    for (auto id : preorder()) {
        if (const Activity* a = get(id)) acts.push_back(activity_to_json(*a));
    }
    // Also persist nodes that are somehow unreachable (should not happen).
    if (acts.size() != nodes_.size()) {
        auto order = preorder();
        for (const auto& n : nodes_)
            if (std::find(order.begin(), order.end(), n->id) == order.end())
                acts.push_back(activity_to_json(*n));
    }
    j["activities"] = Json(std::move(acts));
    return j;
}

bool ActivityTree::from_json(const Json& j, std::string* err) {
    auto fail = [&](const std::string& m) { if (err) *err = m; return false; };
    if (!j.is_object()) return fail("tree: top level is not an object");
    int64_t version = j["version"].as_int(0);
    if (version < 1) return fail("tree: missing or invalid version");
    if (version > 1) return fail("tree: unsupported version " + std::to_string(version));
    if (!j["activities"].is_array()) return fail("tree: missing activities array");

    std::vector<std::unique_ptr<Activity>> nodes;
    std::set<uint64_t> ids;
    for (const auto& aj : j["activities"].as_array()) {
        if (!aj.is_object()) return fail("tree: activity is not an object");
        auto a = activity_from_json(aj);
        if (a->id == 0) return fail("tree: activity with id 0");
        if (!ids.insert(a->id).second) return fail("tree: duplicate activity id " + std::to_string(a->id));
        nodes.push_back(std::move(a));
    }
    if (nodes.empty()) return fail("tree: no activities");

    uint64_t root = (uint64_t)j["root"].as_int(0);
    if (root == 0 || !ids.count(root)) return fail("tree: root id missing");

    auto find = [&](uint64_t id) -> Activity* {
        for (auto& n : nodes) if (n->id == id) return n.get();
        return nullptr;
    };
    Activity* r = find(root);
    r->parent = 0;

    // Repair structure: children lists only reference existing nodes whose
    // parent field agrees; every non-root node must be listed by its parent.
    for (auto& n : nodes) {
        std::vector<uint64_t> kept;
        for (auto c : n->children) {
            Activity* cn = find(c);
            if (!cn || c == n->id || c == root || cn->parent != n->id) continue;
            if (std::find(kept.begin(), kept.end(), c) == kept.end()) kept.push_back(c);
        }
        n->children = std::move(kept);
    }
    for (auto& n : nodes) {
        if (n->id == root) continue;
        Activity* p = find(n->parent);
        if (!p || n->parent == n->id) p = r;
        if (std::find(p->children.begin(), p->children.end(), n->id) == p->children.end()) {
            n->parent = p->id;
            p->children.push_back(n->id);
        }
    }
    // Anything unreachable from the root (cycles) gets re-attached to the root.
    std::set<uint64_t> seen;
    std::vector<uint64_t> stack{root};
    while (!stack.empty()) {
        uint64_t cur = stack.back();
        stack.pop_back();
        if (!seen.insert(cur).second) continue;
        for (auto c : find(cur)->children) stack.push_back(c);
    }
    for (auto& n : nodes) {
        if (seen.count(n->id)) continue;
        // Break the cycle: detach from the (unreachable) parent and hang under root.
        if (Activity* p = find(n->parent)) {
            auto& ch = p->children;
            ch.erase(std::remove(ch.begin(), ch.end(), n->id), ch.end());
        }
        n->parent = root;
        r->children.push_back(n->id);
        // Mark its subtree as reachable now.
        std::vector<uint64_t> st{n->id};
        while (!st.empty()) {
            uint64_t cur = st.back();
            st.pop_back();
            if (!seen.insert(cur).second) continue;
            for (auto c : find(cur)->children) st.push_back(c);
        }
    }

    uint64_t max_id = 0;
    for (auto& n : nodes) max_id = std::max(max_id, n->id);
    uint64_t next_id = (uint64_t)j["next_id"].as_int(0);
    if (next_id <= max_id) next_id = max_id + 1;

    nodes_ = std::move(nodes);
    root_ = root;
    next_id_ = next_id;
    if (err) err->clear();
    notify();
    return true;
}

std::string ActivityTree::default_path() {
    const char* xdg = std::getenv("XDG_DATA_HOME");
    std::string base = (xdg && *xdg) ? std::string(xdg) : home_dir() + "/.local/share";
    return base + "/canopy/tree.json";
}

std::string ActivityTree::history_dir() {
    const char* xdg = std::getenv("XDG_DATA_HOME");
    std::string base = (xdg && *xdg) ? std::string(xdg) : home_dir() + "/.local/share";
    return base + "/canopy/history";
}

std::string ActivityTree::history_path(uint64_t id, const std::string& dir) {
    std::string d = dir.empty() ? history_dir() : dir;
    return d + "/activity-" + std::to_string(id) + ".history";
}

std::vector<std::string> ActivityTree::load_history(uint64_t id, const std::string& dir) {
    std::vector<std::string> out;
    std::string path = history_path(id, dir);
    std::ifstream f(path);
    if (!f) return out;
    std::string line;
    while (std::getline(f, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (!line.empty()) out.push_back(std::move(line));
    }
    return out;
}

bool ActivityTree::save_history(uint64_t id, const std::vector<std::string>& commands, const std::string& dir) {
    std::string path = history_path(id, dir);
    if (!make_parent_dirs(path)) return false;
    std::string tmp = path + ".tmp";
    FILE* f = std::fopen(tmp.c_str(), "wb");
    if (!f) return false;
    constexpr size_t MAX_HIST = 20000;
    size_t start = commands.size() > MAX_HIST ? commands.size() - MAX_HIST : 0;
    bool ok = true;
    for (size_t i = start; i < commands.size(); i++) {
        if (commands[i].empty()) continue;
        std::string line = commands[i];
        for (char& c : line) if (c == '\n' || c == '\r') c = ' ';
        line.push_back('\n');
        if (std::fwrite(line.data(), 1, line.size(), f) != line.size()) { ok = false; break; }
    }
    if (std::fclose(f) != 0) ok = false;
    if (!ok) { ::unlink(tmp.c_str()); return false; }
    if (::rename(tmp.c_str(), path.c_str()) != 0) { ::unlink(tmp.c_str()); return false; }
    return true;
}

bool ActivityTree::append_history(uint64_t id, const std::string& command, const std::string& dir) {
    if (command.empty() || command.find_first_not_of(" \t\r\n") == std::string::npos) return true;
    std::string clean = command;
    for (char& c : clean) if (c == '\n' || c == '\r') c = ' ';
    auto existing = load_history(id, dir);
    if (!existing.empty() && existing.back() == clean) return true;
    std::string path = history_path(id, dir);
    if (!make_parent_dirs(path)) return false;
    FILE* f = std::fopen(path.c_str(), "ab");
    if (!f) return false;
    clean.push_back('\n');
    bool ok = std::fwrite(clean.data(), 1, clean.size(), f) == clean.size();
    if (std::fclose(f) != 0) ok = false;
    return ok;
}

bool ActivityTree::copy_history(uint64_t from_id, uint64_t to_id, const std::string& dir) {
    if (from_id == 0 || to_id == 0 || from_id == to_id) return false;
    auto cmds = load_history(from_id, dir);
    if (cmds.empty()) return false;
    return save_history(to_id, cmds, dir);
}

void ActivityTree::remove_history(uint64_t id, const std::string& dir) {
    std::string path = history_path(id, dir);
    ::unlink(path.c_str());
}

bool ActivityTree::save(const std::string& path_in) const {
    std::string path = path_in.empty() ? default_path() : path_in;
    if (!make_parent_dirs(path)) return false;
    std::string tmp = path + ".tmp";
    std::string text = to_json().dump(2);
    text.push_back('\n');
    {
        FILE* f = std::fopen(tmp.c_str(), "wb");
        if (!f) return false;
        bool ok = std::fwrite(text.data(), 1, text.size(), f) == text.size();
        ok = (std::fflush(f) == 0) && ok;
        if (ok) ok = (::fsync(fileno(f)) == 0) || errno == EINVAL;  // EINVAL: fs without fsync
        if (std::fclose(f) != 0) ok = false;
        if (!ok) { ::unlink(tmp.c_str()); return false; }
    }
    if (::rename(tmp.c_str(), path.c_str()) != 0) {
        ::unlink(tmp.c_str());
        return false;
    }
    return true;
}

bool ActivityTree::load(const std::string& path_in) {
    std::string path = path_in.empty() ? default_path() : path_in;
    if (!file_exists(path)) {
        // Nothing saved yet: start fresh.
        ActivityTree fresh;
        nodes_ = std::move(fresh.nodes_);
        root_ = fresh.root_;
        next_id_ = fresh.next_id_;
        notify();
        return true;
    }
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    std::stringstream ss;
    ss << f.rdbuf();
    std::string text = ss.str();

    std::string err;
    auto j = Json::parse(text, &err);
    if (!j || !from_json(*j, &err)) {
        // Keep the current tree; preserve the corrupt file for inspection.
        std::string bad = path + ".bad";
        ::rename(path.c_str(), bad.c_str());
        std::fprintf(stderr, "canopy: could not load %s: %s (moved to %s)\n", path.c_str(), err.c_str(), bad.c_str());
        return false;
    }
    return true;
}

} // namespace canopy
