#pragma once
#include <string>
#include <vector>
#include <sys/types.h>

namespace canopy {

// Thin wrapper over forkpty(3). Not thread-safe; fd may be polled from another thread.
class Pty {
public:
    Pty() = default;
    ~Pty();
    Pty(const Pty&) = delete;
    Pty& operator=(const Pty&) = delete;

    struct Options {
        std::vector<std::string> argv;         // program + args; empty => $SHELL -l (or /bin/bash)
        std::vector<std::string> env_extra;    // "KEY=VALUE" entries appended to the environment
        std::string cwd;                       // empty => inherit
        int cols = 80, rows = 24;
        bool login_shell = false;              // prepend '-' to argv[0]
    };

    // Spawns the child. Returns false (and sets err) on failure.
    bool spawn(const Options& opts, std::string* err = nullptr);
    int fd() const { return fd_; }
    pid_t pid() const { return pid_; }
    bool alive() const;                        // non-blocking waitpid check; caches exit status
    int exit_status() const { return exit_status_; }
    // Non-blocking read; returns bytes read, 0 on EOF/none, -1 on error (errno set).
    ssize_t read(char* buf, size_t len);
    // Writes all bytes (loops on partial writes). Returns false on error.
    bool write(const char* data, size_t len);
    bool write(const std::string& s) { return write(s.data(), s.size()); }
    void resize(int cols, int rows, int px_w = 0, int px_h = 0);
    // Foreground process cwd (reads /proc/<fgpid>/cwd); empty on failure.
    std::string foreground_cwd() const;
    std::string foreground_name() const;      // comm of foreground process group leader
    void kill(int sig);
    void close();

private:
    int fd_ = -1;
    pid_t pid_ = -1;
    mutable bool exited_ = false;
    mutable int exit_status_ = 0;
};

} // namespace canopy
