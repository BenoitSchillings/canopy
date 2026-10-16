#pragma once
// Turns shell-integration OSC events into Block objects anchored to absolute line numbers.
#include "terminal.h"
#include <cstdint>
#include <functional>
#include <string>
#include <vector>
#include <optional>

namespace canopy {

enum class BlockState : uint8_t { Prompt, Typing, Running, Done };

struct Block {
    uint64_t id = 0;
    BlockState state = BlockState::Prompt;
    std::string command;          // decoded command text (may be empty for an empty prompt)
    std::string cwd;              // cwd when the command started
    int exit_code = -1;           // valid when state == Done
    double start_time = 0;        // steady seconds (0 if unknown)
    double end_time = 0;
    int64_t unix_start = 0;       // wall clock for persistence

    // Absolute line anchors (-1 if unknown). prompt_line <= cmd_line <= output_line <= end_line.
    int64_t prompt_line = -1;     // OSC 133;A
    int64_t cmd_line = -1;        // OSC 133;B (where the user types)
    int64_t output_line = -1;     // OSC 133;C (first output line)
    int64_t end_line = -1;        // OSC 133;D line (exclusive end of output; next prompt starts here)
    int output_col_end = 0;       // cursor column at D (to detect "no trailing newline")

    bool folded = false;
    bool pinned = false;
    std::vector<std::string> marks;        // user bookmarks (OSC 7770;mark)
    // For restored (persisted) blocks only: saved output tail.
    std::vector<std::string> saved_output;

    bool running() const { return state == BlockState::Running; }
    bool failed() const { return state == BlockState::Done && exit_code != 0; }
    double duration() const { return (end_time > 0 && start_time > 0) ? end_time - start_time : 0; }
    int64_t output_lines() const { return (output_line >= 0 && end_line >= 0) ? end_line - output_line : 0; }
};

class BlockTracker {
public:
    BlockTracker();
    // Wire to Terminal::on_osc. Handles codes 133 and 7770 (cmd/mark); ignores others.
    void on_osc(const OscEvent& ev);
    // Call when the terminal's history_base() advances (lines dropped) so blocks fully above are removed.
    void on_history_base(int64_t new_base);
    // Call with the current cwd (from OSC 7) so new blocks record it.
    void set_cwd(const std::string& cwd);
    // Monotonic clock in seconds; injectable for tests.
    std::function<double()> now;

    const std::vector<Block>& blocks() const { return blocks_; }
    std::vector<Block>& blocks() { return blocks_; }
    Block* current();                           // last block or nullptr
    Block* find(uint64_t id);
    // Block containing absolute line (by prompt_line..end_line range), or nullptr.
    Block* at_line(int64_t abs_line);
    int index_at_line(int64_t abs_line) const;  // -1 if none
    std::function<void(const Block&)> on_block_started;   // after 133;C
    std::function<void(const Block&)> on_block_finished;  // after 133;D
    std::function<void(const std::string& kind, const std::string& payload)> on_request; // 7770 new/note/name (payload decoded)

    // Restore persisted blocks (from Activity), placed before any live blocks.
    void restore(std::vector<Block> saved);
    uint64_t next_id() const { return next_id_; }
    void set_next_id(uint64_t id) { next_id_ = id; }

private:
    std::vector<Block> blocks_;
    uint64_t next_id_ = 1;
    std::string cwd_;
};

// Helpers shared with other modules.
std::string base64_decode(std::string_view in);
std::string base64_encode(std::string_view in);

} // namespace canopy
