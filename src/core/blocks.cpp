// BlockTracker: OSC 133 / 7770 events -> Block objects. Plus base64 helpers.
#include "blocks.h"
#include <algorithm>
#include <chrono>
#include <ctime>

namespace canopy {

// ---------------------------------------------------------------- base64

static const char B64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

std::string base64_encode(std::string_view in) {
    std::string out;
    out.reserve((in.size() + 2) / 3 * 4);
    size_t i = 0;
    while (i + 2 < in.size()) {
        uint32_t v = (uint8_t)in[i] << 16 | (uint8_t)in[i + 1] << 8 | (uint8_t)in[i + 2];
        out.push_back(B64[v >> 18]); out.push_back(B64[(v >> 12) & 63]); out.push_back(B64[(v >> 6) & 63]); out.push_back(B64[v & 63]);
        i += 3;
    }
    if (i + 1 == in.size()) {
        uint32_t v = (uint8_t)in[i] << 16;
        out.push_back(B64[v >> 18]); out.push_back(B64[(v >> 12) & 63]); out += "==";
    } else if (i + 2 == in.size()) {
        uint32_t v = (uint8_t)in[i] << 16 | (uint8_t)in[i + 1] << 8;
        out.push_back(B64[v >> 18]); out.push_back(B64[(v >> 12) & 63]); out.push_back(B64[(v >> 6) & 63]); out.push_back('=');
    }
    return out;
}

std::string base64_decode(std::string_view in) {
    std::string out;
    out.reserve(in.size() / 4 * 3 + 3);
    uint32_t acc = 0; int bits = 0;
    for (char c : in) {
        int v;
        if (c >= 'A' && c <= 'Z') v = c - 'A';
        else if (c >= 'a' && c <= 'z') v = c - 'a' + 26;
        else if (c >= '0' && c <= '9') v = c - '0' + 52;
        else if (c == '+' || c == '-') v = 62;
        else if (c == '/' || c == '_') v = 63;
        else if (c == '=') break;
        else continue;   // whitespace / junk
        acc = (acc << 6) | (uint32_t)v;
        bits += 6;
        if (bits >= 8) { bits -= 8; out.push_back((char)((acc >> bits) & 0xff)); }
    }
    return out;
}

// ---------------------------------------------------------------- BlockTracker

BlockTracker::BlockTracker() {
    now = [] {
        using namespace std::chrono;
        return duration<double>(steady_clock::now().time_since_epoch()).count();
    };
}

Block* BlockTracker::current() { return blocks_.empty() ? nullptr : &blocks_.back(); }

Block* BlockTracker::find(uint64_t id) {
    for (auto& b : blocks_) if (b.id == id) return &b;
    return nullptr;
}

int BlockTracker::index_at_line(int64_t abs) const {
    for (int i = (int)blocks_.size() - 1; i >= 0; --i) {
        const Block& b = blocks_[(size_t)i];
        int64_t start = b.prompt_line >= 0 ? b.prompt_line : b.cmd_line >= 0 ? b.cmd_line : b.output_line;
        if (start < 0) continue;
        if (abs < start) continue;
        if (b.end_line >= 0 && abs >= b.end_line && abs != start) continue;
        return i;
    }
    return -1;
}

Block* BlockTracker::at_line(int64_t abs) {
    int i = index_at_line(abs);
    return i < 0 ? nullptr : &blocks_[(size_t)i];
}

void BlockTracker::set_cwd(const std::string& cwd) {
    cwd_ = cwd;
    // A block that is still at the prompt picks up the newest cwd.
    if (Block* b = current(); b && (b->state == BlockState::Prompt || b->state == BlockState::Typing)) b->cwd = cwd;
}

void BlockTracker::on_osc(const OscEvent& ev) {
    auto new_block = [&](BlockState st) -> Block& {
        Block b;
        b.id = next_id_++;
        b.state = st;
        b.cwd = cwd_;
        b.prompt_line = ev.abs_line;
        blocks_.push_back(std::move(b));
        return blocks_.back();
    };
    if (ev.code == 133) {
        if (ev.args.empty()) return;
        char kind = ev.args[0].empty() ? 0 : ev.args[0][0];
        Block* cur = current();
        switch (kind) {
        case 'A': {
            if (cur && (cur->state == BlockState::Prompt || cur->state == BlockState::Typing)) {
                // Prompt redrawn (resize, Ctrl+L, multi-line prompt re-render): move the anchor.
                cur->prompt_line = ev.abs_line;
                cur->cmd_line = -1;
                cur->state = BlockState::Prompt;
                cur->cwd = cwd_;
            } else {
                new_block(BlockState::Prompt);
            }
            break;
        }
        case 'B': {
            if (!cur || cur->state == BlockState::Running || cur->state == BlockState::Done) cur = &new_block(BlockState::Prompt);
            cur->state = BlockState::Typing;
            cur->cmd_line = ev.abs_line;
            break;
        }
        case 'C': {
            if (!cur || cur->state == BlockState::Done) { cur = &new_block(BlockState::Prompt); cur->cmd_line = ev.abs_line; }
            if (cur->state == BlockState::Running) break;   // duplicate C
            cur->state = BlockState::Running;
            cur->output_line = ev.abs_line;
            if (cur->cmd_line < 0) cur->cmd_line = cur->prompt_line;
            cur->start_time = now ? now() : 0;
            cur->unix_start = (int64_t)::time(nullptr);
            if (cur->cwd.empty()) cur->cwd = cwd_;
            if (on_block_started) on_block_started(*cur);
            break;
        }
        case 'D': {
            if (!cur || cur->state == BlockState::Done) break;
            int exit_code = -1;
            if (ev.args.size() > 1 && !ev.args[1].empty()) exit_code = atoi(ev.args[1].c_str());
            if (cur->state != BlockState::Running && cur->command.empty()) {
                // Empty prompt (just Enter / Ctrl+C): no real command; drop it.
                blocks_.pop_back();
                break;
            }
            if (cur->state != BlockState::Running) {
                cur->output_line = ev.abs_line;
                if (cur->cmd_line < 0) cur->cmd_line = cur->prompt_line;
            }
            cur->state = BlockState::Done;
            cur->end_line = ev.abs_line;
            cur->output_col_end = ev.col;
            cur->exit_code = exit_code;
            cur->end_time = now ? now() : 0;
            if (cur->start_time == 0) cur->start_time = cur->end_time;
            if (on_block_finished) on_block_finished(*cur);
            break;
        }
        default: break;
        }
        return;
    }
    if (ev.code == 7770) {
        if (ev.args.empty()) return;
        const std::string& kind = ev.args[0];
        std::string payload = ev.args.size() > 1 ? base64_decode(ev.args[1]) : std::string();
        Block* cur = current();
        if (kind == "cmd") {
            if (!cur || cur->state == BlockState::Done) { cur = &new_block(BlockState::Typing); cur->cmd_line = ev.abs_line; }
            cur->command = payload;
        } else if (kind == "late") {
            // Command ran without a C event (subshell as first command). Anchor it approximately:
            // output starts on the line after the command line.
            if (!cur || cur->state == BlockState::Done) { cur = &new_block(BlockState::Typing); cur->cmd_line = ev.abs_line; }
            if (cur->state != BlockState::Running) {
                cur->command = payload;
                cur->state = BlockState::Running;
                if (cur->cmd_line < 0) cur->cmd_line = cur->prompt_line;
                cur->output_line = std::min(cur->cmd_line + 1, ev.abs_line);
                cur->unix_start = (int64_t)::time(nullptr);
                if (on_block_started) on_block_started(*cur);
            }
        } else if (kind == "mark") {
            if (cur) cur->marks.push_back(payload);
        } else if (kind == "new" || kind == "note" || kind == "name") {
            if (on_request) on_request(kind, payload);
        }
        return;
    }
}

void BlockTracker::on_history_base(int64_t new_base) {
    size_t drop = 0;
    while (drop < blocks_.size()) {
        const Block& b = blocks_[drop];
        if (b.end_line >= 0 && b.end_line < new_base) ++drop;
        else break;
    }
    if (drop) blocks_.erase(blocks_.begin(), blocks_.begin() + (long)drop);
    if (!blocks_.empty()) {
        Block& b = blocks_.front();
        if (b.prompt_line >= 0 && b.prompt_line < new_base) b.prompt_line = new_base;
        if (b.cmd_line >= 0 && b.cmd_line < new_base) b.cmd_line = new_base;
        if (b.output_line >= 0 && b.output_line < new_base) b.output_line = new_base;
    }
}

void BlockTracker::restore(std::vector<Block> saved) {
    uint64_t max_id = 0;
    for (auto& b : saved) max_id = std::max(max_id, b.id);
    for (auto& b : blocks_) max_id = std::max(max_id, b.id);
    saved.insert(saved.end(), std::make_move_iterator(blocks_.begin()), std::make_move_iterator(blocks_.end()));
    blocks_ = std::move(saved);
    if (next_id_ <= max_id) next_id_ = max_id + 1;
}

} // namespace canopy
