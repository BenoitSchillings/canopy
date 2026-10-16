#pragma once
// A live shell session: Pty + Terminal + BlockTracker + reader thread.
#include "pty.h"
#include "terminal.h"
#include "blocks.h"
#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace canopy {

class Session {
public:
    struct Options {
        std::vector<std::string> argv;      // empty => user shell with Canopy's bash integration
        std::vector<std::string> env_extra;
        std::string cwd;
        int cols = 80, rows = 24;
        int scrollback = 10000;
        bool shell_integration = true;      // inject shell/canopy.bash via --rcfile wrapper when shell is bash
        std::string integration_dir;        // directory containing canopy.bash (default: auto-detect)
        uint64_t activity_id = 0;           // exported as CANOPY_ACTIVITY
    };

    Session();
    ~Session();
    bool start(const Options& opts, std::string* err = nullptr);

    // Called by the main loop: drains bytes received by the reader thread into the Terminal.
    // Returns true if anything was fed. Never blocks.
    bool pump();
    // Wake callback invoked (from the reader thread!) when new bytes are queued — use it to
    // post an SDL event. Must be thread-safe.
    std::function<void()> on_wake;

    Terminal& term() { return *term_; }
    const Terminal& term() const { return *term_; }
    BlockTracker& blocks() { return blocks_; }
    Pty& pty() { return pty_; }
    bool alive() const;
    int exit_status() const { return pty_.exit_status(); }

    void write(std::string_view bytes);     // to the child
    void resize(int cols, int rows, int px_w = 0, int px_h = 0);
    std::string cwd() const;                 // best known cwd (OSC 7, else /proc)
    void stop();                             // SIGHUP + join

private:
    void reader_loop();
    Pty pty_;
    std::unique_ptr<Terminal> term_;
    BlockTracker blocks_;
    std::thread reader_;
    std::mutex mu_;
    std::string inbox_;                      // bytes queued by the reader thread
    std::atomic<bool> running_{false};
    int wake_pipe_[2] = {-1, -1};
};

// Locates shell/canopy.bash: $CANOPY_SHELL_DIR, next to the executable (../shell), or
// ~/.local/share/canopy/shell, or /usr/share/canopy/shell. Empty if not found.
std::string find_shell_integration_dir();

} // namespace canopy
