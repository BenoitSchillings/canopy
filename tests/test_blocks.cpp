// BlockTracker + base64 tests.
#include "core/blocks.h"
#include <cstdio>
#include <string>

using namespace canopy;

static int g_fail = 0, g_pass = 0;
#define CHECK(c) do { if (c) ++g_pass; else { ++g_fail; fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

static OscEvent ev(int code, std::vector<std::string> args, int64_t line, int col = 0) { OscEvent e; e.code = code; e.args = std::move(args); e.abs_line = line; e.col = col; return e; }

int main() {
    CHECK(base64_encode("") == "");
    CHECK(base64_encode("f") == "Zg==");
    CHECK(base64_encode("fo") == "Zm8=");
    CHECK(base64_encode("foo") == "Zm9v");
    CHECK(base64_encode("hello world") == "aGVsbG8gd29ybGQ=");
    CHECK(base64_decode("aGVsbG8gd29ybGQ=") == "hello world");
    CHECK(base64_decode("aGVsbG8gd29ybGQ") == "hello world");       // missing padding
    CHECK(base64_decode("aGVs\nbG8g\nd29y\nbGQ=\n") == "hello world"); // wrapped
    { std::string bin("\x00\xff\x10 binary \xe4\xb8\xad", 15); CHECK(base64_decode(base64_encode(bin)) == bin); }
    CHECK(base64_decode("bHM=") == "ls");

    BlockTracker bt;
    double clock = 100;
    bt.now = [&] { return clock; };
    int started = 0, finished = 0;
    bt.on_block_started = [&](const Block&) { ++started; };
    bt.on_block_finished = [&](const Block&) { ++finished; };
    std::vector<std::pair<std::string, std::string>> reqs;
    bt.on_request = [&](const std::string& k, const std::string& p) { reqs.emplace_back(k, p); };

    // Fresh shell: first prompt
    bt.on_osc(ev(7, {"file://h/home/u"}, 0)); bt.set_cwd("/home/u");
    bt.on_osc(ev(133, {"A"}, 0));
    bt.on_osc(ev(133, {"B"}, 0));
    CHECK(bt.blocks().size() == 1 && bt.current()->state == BlockState::Typing && bt.current()->cmd_line == 0);
    // Prompt redrawn (Ctrl+L): same block, new anchor
    bt.on_osc(ev(133, {"A"}, 5)); bt.on_osc(ev(133, {"B"}, 5));
    CHECK(bt.blocks().size() == 1 && bt.current()->prompt_line == 5 && bt.current()->cmd_line == 5);
    // Command runs
    bt.on_osc(ev(7770, {"cmd", base64_encode("ls -l")}, 5, 20));
    bt.on_osc(ev(133, {"C"}, 6));
    CHECK(bt.current()->state == BlockState::Running && bt.current()->command == "ls -l" && bt.current()->output_line == 6 && bt.current()->start_time == 100 && started == 1);
    CHECK(bt.current()->cwd == "/home/u");
    bt.on_osc(ev(7770, {"mark", base64_encode("here")}, 8));
    clock = 102.5;
    bt.on_osc(ev(133, {"D", "0"}, 10, 3));
    CHECK(bt.current()->state == BlockState::Done && bt.current()->exit_code == 0 && bt.current()->end_line == 10 && bt.current()->output_col_end == 3 && finished == 1);
    CHECK(bt.current()->duration() == 2.5 && bt.current()->output_lines() == 4 && bt.current()->marks.size() == 1 && bt.current()->marks[0] == "here");
    // Next prompt; empty Enter -> D without exit: block removed
    bt.on_osc(ev(133, {"A"}, 10)); bt.on_osc(ev(133, {"B"}, 10));
    CHECK(bt.blocks().size() == 2);
    bt.on_osc(ev(133, {"D"}, 11));
    CHECK(bt.blocks().size() == 1);
    // D with status but no command (the shell always reports status): also removed
    bt.on_osc(ev(133, {"A"}, 11)); bt.on_osc(ev(133, {"B"}, 11)); bt.on_osc(ev(133, {"D", "0"}, 12));
    CHECK(bt.blocks().size() == 1);
    // Failing command
    bt.on_osc(ev(133, {"A"}, 12)); bt.on_osc(ev(133, {"B"}, 12));
    bt.on_osc(ev(7770, {"cmd", base64_encode("false")}, 12));
    bt.on_osc(ev(133, {"C"}, 13)); bt.on_osc(ev(133, {"D", "1"}, 13));
    CHECK(bt.blocks().size() == 2 && bt.current()->failed() && bt.current()->exit_code == 1 && bt.current()->output_lines() == 0);
    // ids are unique and increasing
    CHECK(bt.blocks()[0].id < bt.blocks()[1].id);
    // late command (subshell): approximate anchoring
    bt.on_osc(ev(133, {"A"}, 13)); bt.on_osc(ev(133, {"B"}, 13));
    bt.on_osc(ev(7770, {"late", base64_encode("(exit 3)")}, 14)); bt.on_osc(ev(133, {"D", "3"}, 14));
    CHECK(bt.blocks().size() == 3 && bt.current()->command == "(exit 3)" && bt.current()->exit_code == 3 && bt.current()->output_line == 14 && bt.current()->state == BlockState::Done);
    // requests
    bt.on_osc(ev(7770, {"new", base64_encode("fix tests")}, 14));
    bt.on_osc(ev(7770, {"note", base64_encode("a note")}, 14));
    bt.on_osc(ev(7770, {"name", base64_encode("renamed")}, 14));
    CHECK(reqs.size() == 3 && reqs[0].first == "new" && reqs[0].second == "fix tests" && reqs[2].second == "renamed");
    // lookup by line
    CHECK(bt.index_at_line(5) == 0 && bt.index_at_line(9) == 0 && bt.index_at_line(12) == 1 && bt.index_at_line(13) == 2);
    CHECK(bt.at_line(4) == nullptr);
    CHECK(bt.find(bt.blocks()[1].id) == &bt.blocks()[1] && bt.find(999) == nullptr);
    // A running block extends to the end
    bt.on_osc(ev(133, {"A"}, 14)); bt.on_osc(ev(133, {"B"}, 14)); bt.on_osc(ev(7770, {"cmd", base64_encode("sleep 9")}, 14)); bt.on_osc(ev(133, {"C"}, 15));
    CHECK(bt.at_line(1000) == bt.current() && bt.current()->running());
    // history base: drop fully-scrolled blocks, clamp the partially dropped one
    size_t before = bt.blocks().size();
    bt.on_history_base(11);
    CHECK(bt.blocks().size() == before - 1 && bt.blocks()[0].command == "false" && bt.blocks()[0].prompt_line == 12);
    bt.on_history_base(13);
    CHECK(bt.blocks()[0].command == "false" && bt.blocks()[0].prompt_line == 13 && bt.blocks()[0].cmd_line == 13 && bt.blocks()[0].output_line == 13);
    bt.on_history_base(14);
    CHECK(bt.blocks()[0].command == "(exit 3)");
    bt.on_history_base(100);
    CHECK(bt.blocks().size() == 1 && bt.blocks()[0].command == "sleep 9" && bt.blocks()[0].prompt_line == 100);
    // C without A/B (integration partially missing) still yields a block
    BlockTracker b2;
    b2.on_osc(ev(7770, {"cmd", base64_encode("make")}, 3)); b2.on_osc(ev(133, {"C"}, 4)); b2.on_osc(ev(133, {"D", "2"}, 9));
    CHECK(b2.blocks().size() == 1 && b2.blocks()[0].command == "make" && b2.blocks()[0].exit_code == 2 && b2.blocks()[0].prompt_line == 3 && b2.blocks()[0].output_line == 4);
    // D with nothing before it is ignored
    BlockTracker b3; b3.on_osc(ev(133, {"D", "0"}, 0)); CHECK(b3.blocks().empty() && b3.current() == nullptr);
    // restore
    Block saved; saved.id = 40; saved.command = "old"; saved.state = BlockState::Done;
    b2.restore({saved});
    CHECK(b2.blocks().size() == 2 && b2.blocks()[0].command == "old" && b2.next_id() == 41);

    printf("test_blocks: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
