// Tests for core/activity.h
#include "core/activity.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

using namespace canopy;

static int g_fail = 0, g_pass = 0;
#define CHECK(cond) do { if (cond) g_pass++; else { g_fail++; std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } } while (0)

static bool exists(const std::string& p) { struct stat st; return stat(p.c_str(), &st) == 0; }

static std::string tmpdir() {
    char name[] = "/tmp/canopy_act_XXXXXX";
    const char* d = mkdtemp(name);
    return d ? d : "/tmp";
}

static void test_basic_tree() {
    ActivityTree t;
    CHECK(t.root() == 1);
    CHECK(t.count() == 1);
    const Activity* r = t.get(1);
    CHECK(r && r->name == "Home" && r->parent == 0);
    CHECK(r->created > 0 && r->last_active > 0);

    int changes = 0;
    t.on_changed = [&] { changes++; };

    uint64_t a = t.create(0, "A", "/tmp");
    uint64_t b = t.create(0, "B", "/var");
    uint64_t a1 = t.create(a, "A1", "");
    uint64_t a2 = t.create(a, "A2", "/opt");
    CHECK(a == 2 && b == 3 && a1 == 4 && a2 == 5);   // monotonically increasing
    CHECK(changes == 4);
    CHECK(t.count() == 5);
    CHECK(t.get(a1)->cwd == "/tmp");                // inherited from parent when empty
    CHECK(t.get(a1)->parent == a);
    CHECK(t.get(a)->children.size() == 2 && t.get(a)->children[0] == a1 && t.get(a)->children[1] == a2);
    CHECK(t.get(1)->children.size() == 2);
    CHECK(t.get(a)->created > 0 && t.get(a)->last_active == t.get(a)->created);

    auto pre = t.preorder();
    CHECK(pre.size() == 5);
    CHECK(pre[0] == 1 && pre[1] == a && pre[2] == a1 && pre[3] == a2 && pre[4] == b);
    CHECK(t.depth(1) == 0 && t.depth(a) == 1 && t.depth(a1) == 2 && t.depth(999) == -1);
    CHECK(t.next_sibling(a) == b && t.prev_sibling(b) == a);
    CHECK(t.next_sibling(b) == 0 && t.prev_sibling(a) == 0);
    CHECK(t.next_sibling(1) == 0 && t.prev_sibling(1) == 0);
    CHECK(t.next_sibling(a1) == a2 && t.prev_sibling(a2) == a1);

    // Unknown parent falls back to root.
    uint64_t c = t.create(12345, "C", "/");
    CHECK(t.get(c)->parent == 1);

    int visited = 0;
    t.for_each([&](Activity& x) { visited++; (void)x; });
    CHECK(visited == 6);
}

static void test_move_reorder_remove() {
    ActivityTree t;
    uint64_t a = t.create(0, "A", "/");
    uint64_t b = t.create(0, "B", "/");
    uint64_t a1 = t.create(a, "A1", "/");
    uint64_t a11 = t.create(a1, "A11", "/");

    // Cycle prevention: moving A under its own descendant is a silent no-op.
    t.move(a, a11);
    CHECK(t.get(a)->parent == 1 && t.get(a11)->children.empty());
    t.move(a, a);
    CHECK(t.get(a)->parent == 1);
    // Root cannot be moved or removed.
    t.move(1, a);
    CHECK(t.get(1)->parent == 0 && t.get(a)->children.size() == 1);
    t.remove(1);
    CHECK(t.count() == 5);

    // Move A11 under B at index 0.
    t.move(a11, b, 0);
    CHECK(t.get(a11)->parent == b && t.get(b)->children[0] == a11 && t.get(a1)->children.empty());
    // Move with index -1 appends; move to 0 means root.
    t.move(a1, 0, -1);
    CHECK(t.get(a1)->parent == 1 && t.get(1)->children.back() == a1 && t.get(a)->children.empty());
    // Reorder siblings: root children are [a, b, a1]; move a1 to the front.
    t.reorder(a1, 0);
    CHECK(t.get(1)->children[0] == a1 && t.get(1)->children[1] == a && t.get(1)->children[2] == b);
    t.reorder(a1, 1);
    CHECK(t.get(1)->children[0] == a && t.get(1)->children[1] == a1 && t.get(1)->children[2] == b);
    t.reorder(a, 99);   // out-of-range index => end
    CHECK(t.get(1)->children.back() == a);

    // Remove a subtree.
    uint64_t b2 = t.create(b, "B2", "/");
    uint64_t b21 = t.create(b2, "B21", "/");
    size_t before = t.count();
    t.remove(b);
    CHECK(t.count() == before - 4);
    CHECK(!t.get(b) && !t.get(a11) && !t.get(b2) && !t.get(b21));
    CHECK(t.get(1)->children.size() == 2);
    for (auto id : t.get(1)->children) CHECK(id != b);
    // Removing an unknown id is harmless.
    t.remove(4242);
    CHECK(t.count() == before - 4);
    // Ids keep increasing after removals.
    uint64_t n = t.create(0, "N", "/");
    CHECK(n > b21);
}

static Block sample_block(uint64_t id) {
    Block b;
    b.id = id;
    b.state = BlockState::Done;
    b.command = "make -j12 \"quoted\" \xE2\x9C\x93";
    b.cwd = "/home/u/proj";
    b.exit_code = 2;
    b.start_time = 100.5;
    b.end_time = 103.75;
    b.unix_start = 1760000000;
    b.prompt_line = 10;
    b.cmd_line = 11;
    b.output_line = 12;
    b.end_line = 40;
    b.output_col_end = 7;
    b.folded = true;
    b.pinned = true;
    b.marks = {"first", "second mark"};
    b.saved_output = {"line one", "", "  indented\ttab", "last \xC3\xA9"};
    return b;
}

static void test_block_json() {
    Block b = sample_block(7);
    Json j = block_to_json(b);
    CHECK(j.is_object());
    CHECK(j["saved_output"].is_array() && j["saved_output"].size() == 4);
    CHECK(std::fabs(j["duration"].as_double() - 3.25) < 1e-9);
    CHECK(!j.has("start_time") && !j.has("end_time"));   // steady-clock values are not persisted

    Block r = block_from_json(j);
    CHECK(r.id == 7);
    CHECK(r.state == BlockState::Done);
    CHECK(r.command == b.command);
    CHECK(r.cwd == b.cwd);
    CHECK(r.exit_code == 2);
    CHECK(std::fabs(r.duration() - 3.25) < 1e-9);         // duration survives via synthetic anchors
    CHECK(r.start_time == 1.0 && std::fabs(r.end_time - 4.25) < 1e-9);
    CHECK(r.unix_start == 1760000000);
    CHECK(r.prompt_line == 10 && r.cmd_line == 11 && r.output_line == 12 && r.end_line == 40);
    CHECK(r.output_col_end == 7);
    CHECK(r.folded && r.pinned);
    CHECK(r.marks == b.marks);
    CHECK(r.saved_output == b.saved_output);
    CHECK(r.failed());
    CHECK(r.output_lines() == 28);

    // Zero-duration / unknown-time block keeps 0 anchors.
    Block z;
    z.state = BlockState::Prompt;
    Block zr = block_from_json(block_to_json(z));
    CHECK(zr.start_time == 0 && zr.end_time == 0 && zr.duration() == 0);
    CHECK(zr.state == BlockState::Prompt && zr.exit_code == -1 && zr.prompt_line == -1);
    for (BlockState s : {BlockState::Prompt, BlockState::Typing, BlockState::Running, BlockState::Done}) {
        Block x; x.state = s;
        CHECK(block_from_json(block_to_json(x)).state == s);
    }
    // Missing fields -> defaults
    Block m = block_from_json(Json::object());
    CHECK(m.id == 0 && m.exit_code == -1 && m.end_line == -1 && m.saved_output.empty());
}

static void test_tree_json_roundtrip() {
    ActivityTree t;
    uint64_t a = t.create(0, "Alpha", "/home/u");
    uint64_t b = t.create(0, "Beta", "/srv");
    uint64_t a1 = t.create(a, "Alpha one", "/home/u/x");
    (void)b;
    Activity* A = t.get(a);
    A->notes = "some notes\nwith newline";
    A->env = {"FOO=bar", "BAZ=qux quux"};
    A->collapsed = true;
    A->x = 120.5f;
    A->y = -33.25f;
    A->auto_place = false;
    A->cols = 100;
    A->rows = 40;
    A->color = "#ff8800";
    A->next_block_id = 17;
    A->blocks.push_back(sample_block(3));
    A->blocks.push_back(sample_block(9));
    A->blocks[1].state = BlockState::Running;
    A->blocks[1].saved_output.clear();
    t.get(a1)->last_active = 1760001234;

    Json j = t.to_json();
    CHECK(j["version"].as_int() == 1);
    CHECK(j["root"].as_int() == 1);
    CHECK(j["next_id"].as_int() == 5);
    CHECK(j["activities"].is_array() && j["activities"].size() == 4);
    CHECK(j["activities"][0]["id"].as_int() == 1);         // preorder: root first

    std::string text = j.dump(2);
    auto parsed = Json::parse(text);
    CHECK(parsed.has_value());

    ActivityTree u;
    uint64_t junk = u.create(0, "junk", "/");
    (void)junk;
    std::string err;
    CHECK(u.from_json(*parsed, &err));
    CHECK(err.empty());
    CHECK(u.count() == 4);
    CHECK(!u.get(junk) || junk == a);   // old content replaced
    CHECK(u.root() == 1);
    const Activity* UA = u.get(a);
    CHECK(UA != nullptr);
    if (UA) {
        CHECK(UA->name == "Alpha");
        CHECK(UA->parent == 1);
        CHECK(UA->children.size() == 1 && UA->children[0] == a1);
        CHECK(UA->notes == A->notes);
        CHECK(UA->cwd == "/home/u");
        CHECK(UA->env == A->env);
        CHECK(UA->created == A->created && UA->last_active == A->last_active);
        CHECK(UA->collapsed);
        CHECK(UA->x == 120.5f && UA->y == -33.25f);
        CHECK(!UA->auto_place);
        CHECK(UA->cols == 100 && UA->rows == 40);
        CHECK(UA->color == "#ff8800");
        CHECK(UA->next_block_id == 17);
        CHECK(UA->blocks.size() == 2);
        CHECK(UA->blocks[0].id == 3 && UA->blocks[0].saved_output.size() == 4);
        CHECK(UA->blocks[1].id == 9 && UA->blocks[1].state == BlockState::Running);
    }
    CHECK(u.get(a1) && u.get(a1)->last_active == 1760001234);
    CHECK(u.preorder() == t.preorder());
    // Re-serialization is identical.
    CHECK(u.to_json().dump() == j.dump());
    // next id continues after the loaded ones.
    uint64_t n = u.create(0, "N", "/");
    CHECK(n == 5);
}

static void test_from_json_validation_and_repair() {
    ActivityTree t;
    t.create(0, "keep", "/");
    std::string err;
    // Garbage: tree untouched.
    CHECK(!t.from_json(Json(5), &err) && !err.empty());
    CHECK(!t.from_json(*Json::parse(R"({"version":1})"), &err));
    CHECK(!t.from_json(*Json::parse(R"({"version":2,"root":1,"activities":[{"id":1}]})"), &err));
    CHECK(!t.from_json(*Json::parse(R"({"version":1,"root":1,"activities":[]})"), &err));
    CHECK(!t.from_json(*Json::parse(R"({"version":1,"root":9,"activities":[{"id":1}]})"), &err));
    CHECK(!t.from_json(*Json::parse(R"({"version":1,"root":1,"activities":[{"id":1},{"id":1}]})"), &err));
    CHECK(t.count() == 2 && t.get(2) && t.get(2)->name == "keep");

    // Repairable: dangling child reference, orphan with missing parent, cycle.
    auto j = Json::parse(R"({"version":1,"root":1,"next_id":3,"activities":[
        {"id":1,"parent":0,"children":[2,99],"name":"Home"},
        {"id":2,"parent":1,"children":[],"name":"child"},
        {"id":5,"parent":77,"children":[],"name":"orphan"},
        {"id":6,"parent":7,"children":[7],"name":"cyc-a"},
        {"id":7,"parent":6,"children":[6],"name":"cyc-b"}
    ]})");
    CHECK(j.has_value());
    CHECK(t.from_json(*j, &err));
    CHECK(t.count() == 5);
    CHECK(t.get(1)->children.size() >= 3);
    CHECK(t.get(5)->parent == 1);
    CHECK(t.depth(6) >= 1 && t.depth(7) >= 1);
    CHECK(t.preorder().size() == 5);           // everything reachable
    uint64_t n = t.create(0, "n", "/");
    CHECK(n == 8);                             // next_id bumped past max id
}

static void test_save_load() {
    std::string dir = tmpdir();
    std::string path = dir + "/nested/deeper/tree.json";

    ActivityTree t;
    uint64_t a = t.create(0, "Saved", "/x");
    t.get(a)->blocks.push_back(sample_block(1));
    CHECK(t.save(path));
    CHECK(exists(path));
    CHECK(!exists(path + ".tmp"));

    ActivityTree u;
    CHECK(u.load(path));
    CHECK(u.count() == 2 && u.get(a) && u.get(a)->name == "Saved");
    CHECK(u.get(a)->blocks.size() == 1 && u.get(a)->blocks[0].saved_output.size() == 4);

    // Missing file: fresh tree, success.
    ActivityTree v;
    v.create(0, "old", "/");
    CHECK(v.load(dir + "/does-not-exist.json"));
    CHECK(v.count() == 1 && v.root() == 1);

    // Corrupt file: failure, tree intact, file backed up as .bad.
    std::string bad = dir + "/corrupt.json";
    std::ofstream(bad) << "{ this is not json";
    ActivityTree w;
    uint64_t keep = w.create(0, "keep me", "/");
    CHECK(!w.load(bad));
    CHECK(w.count() == 2 && w.get(keep) && w.get(keep)->name == "keep me");
    CHECK(!exists(bad));
    CHECK(exists(bad + ".bad"));

    // Valid JSON but invalid tree is also "corrupt".
    std::string bad2 = dir + "/corrupt2.json";
    std::ofstream(bad2) << "{\"version\":1}";
    CHECK(!w.load(bad2));
    CHECK(exists(bad2 + ".bad"));
    CHECK(w.count() == 2);

    // Overwrite is atomic (rename over existing).
    CHECK(t.save(path));
    CHECK(u.load(path) && u.count() == 2);

    // Default path honours XDG_DATA_HOME.
    setenv("XDG_DATA_HOME", dir.c_str(), 1);
    CHECK(ActivityTree::default_path() == dir + "/canopy/tree.json");
    CHECK(t.save());
    CHECK(exists(dir + "/canopy/tree.json"));
    unsetenv("XDG_DATA_HOME");
    setenv("HOME", "/home/zed", 1);
    CHECK(ActivityTree::default_path() == "/home/zed/.local/share/canopy/tree.json");

    // Cleanup (best effort).
    std::string cmd = "rm -rf '" + dir + "'";
    if (dir.rfind("/tmp/canopy_act_", 0) == 0) (void)!system(cmd.c_str());
}

static void test_remove_one_and_history() {
    ActivityTree t;
    // Cannot remove the only remaining node.
    CHECK(t.remove_one(1) == 0);
    CHECK(t.count() == 1);

    uint64_t a = t.create(0, "A", "/a");
    uint64_t b = t.create(0, "B", "/b");
    uint64_t a1 = t.create(a, "A1", "/a1");
    uint64_t a2 = t.create(a, "A2", "/a2");
    CHECK(t.count() == 5);

    // Removing intermediate node A splices A1 and A2 into root's children in A's position.
    uint64_t next = t.remove_one(a);
    CHECK(next == a1);
    CHECK(t.count() == 4);
    CHECK(!t.get(a));
    CHECK(t.get(a1) && t.get(a1)->parent == 1);
    CHECK(t.get(a2) && t.get(a2)->parent == 1);
    CHECK(t.get(1)->children.size() == 3);
    CHECK(t.get(1)->children[0] == a1 && t.get(1)->children[1] == a2 && t.get(1)->children[2] == b);

    // Removing the root node promotes its first child (a1) to root and reparents a2 and b under a1.
    uint64_t new_root = t.remove_one(1);
    CHECK(new_root == a1);
    CHECK(t.root() == a1);
    CHECK(!t.get(1));
    CHECK(t.get(a1)->parent == 0);
    CHECK(t.get(a2)->parent == a1 && t.get(b)->parent == a1);
    CHECK(t.count() == 3);

    // Per-terminal persistent history files.
    std::string dir = tmpdir();
    std::string hdir = dir + "/hist";
    CHECK(ActivityTree::load_history(10, hdir).empty());
    CHECK(ActivityTree::append_history(10, "git status", hdir));
    CHECK(ActivityTree::append_history(10, "git status", hdir)); // consecutive duplicate ignored
    CHECK(ActivityTree::append_history(10, "make -j", hdir));
    auto h10 = ActivityTree::load_history(10, hdir);
    CHECK(h10.size() == 2 && h10[0] == "git status" && h10[1] == "make -j");

    // Copy history to child activity 11, then diverge.
    CHECK(ActivityTree::copy_history(10, 11, hdir));
    CHECK(ActivityTree::append_history(11, "./build/canopy", hdir));
    auto h11 = ActivityTree::load_history(11, hdir);
    CHECK(h11.size() == 3 && h11[2] == "./build/canopy");
    CHECK(ActivityTree::load_history(10, hdir).size() == 2);

    // Remove history when activity closes.
    ActivityTree::remove_history(11, hdir);
    CHECK(ActivityTree::load_history(11, hdir).empty());

    // Activity::history JSON roundtrip.
    t.get(a1)->history = {"ls -la", "pwd"};
    auto j = t.to_json();
    ActivityTree u;
    CHECK(u.from_json(j));
    CHECK(u.get(a1) && u.get(a1)->history.size() == 2 && u.get(a1)->history[1] == "pwd");

    std::string cmd = "rm -rf '" + dir + "'";
    if (dir.rfind("/tmp/canopy_act_", 0) == 0) (void)!system(cmd.c_str());
}

int main() {
    test_basic_tree();
    test_move_reorder_remove();
    test_block_json();
    test_tree_json_roundtrip();
    test_from_json_validation_and_repair();
    test_save_load();
    test_remove_one_and_history();
    std::printf("test_activity: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
