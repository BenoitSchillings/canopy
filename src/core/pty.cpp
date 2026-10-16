// forkpty wrapper.
#include "pty.h"
#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <poll.h>
#include <pty.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>

extern char** environ;

namespace canopy {

Pty::~Pty() { close(); }

bool Pty::spawn(const Options& opts, std::string* err) {
    close();
    exited_ = false; exit_status_ = 0;

    struct winsize ws{};
    ws.ws_row = (unsigned short)opts.rows;
    ws.ws_col = (unsigned short)opts.cols;

    // Build argv in the parent (no allocation after fork).
    std::vector<std::string> args = opts.argv;
    if (args.empty()) {
        const char* sh = getenv("SHELL");
        args.push_back(sh && *sh ? sh : "/bin/bash");
    }
    std::string prog = args[0];
    std::string argv0 = args[0];
    if (opts.login_shell) {
        size_t slash = argv0.rfind('/');
        argv0 = "-" + (slash == std::string::npos ? argv0 : argv0.substr(slash + 1));
    }
    std::vector<char*> cargv;
    cargv.push_back(argv0.data());
    for (size_t i = 1; i < args.size(); ++i) cargv.push_back(args[i].data());
    cargv.push_back(nullptr);

    bool have_term = false, have_colorterm = false;
    for (auto& e : opts.env_extra) {
        if (e.rfind("TERM=", 0) == 0) have_term = true;
        if (e.rfind("COLORTERM=", 0) == 0) have_colorterm = true;
    }
    std::vector<std::string> env_copy = opts.env_extra;   // keep strings alive for putenv in the child

    // Block signals around fork so the child starts with a clean mask after we reset it.
    sigset_t all, old;
    sigfillset(&all);
    pthread_sigmask(SIG_SETMASK, &all, &old);

    int fd = -1;
    pid_t pid = forkpty(&fd, nullptr, nullptr, &ws);
    if (pid < 0) {
        pthread_sigmask(SIG_SETMASK, &old, nullptr);
        if (err) *err = std::string("forkpty: ") + strerror(errno);
        return false;
    }
    if (pid == 0) {
        // ---- child ----
        sigset_t none;
        sigemptyset(&none);
        sigprocmask(SIG_SETMASK, &none, nullptr);
        for (int s = 1; s < NSIG; ++s) {
            if (s == SIGKILL || s == SIGSTOP) continue;
            signal(s, SIG_DFL);
        }
        if (!opts.cwd.empty() && chdir(opts.cwd.c_str()) != 0) {
            const char* home = getenv("HOME");
            if (home) (void)!chdir(home);
        }
        if (!have_term) setenv("TERM", "xterm-256color", 1);
        if (!have_colorterm) setenv("COLORTERM", "truecolor", 1);
        unsetenv("COLUMNS");
        unsetenv("LINES");
        unsetenv("TERMCAP");
        for (auto& e : env_copy) putenv(e.data());
        execvp(prog.c_str(), cargv.data());
        int e = errno;
        char msg[256];
        int n = snprintf(msg, sizeof msg, "canopy: cannot execute %s: %s\r\n", prog.c_str(), strerror(e));
        if (n > 0) (void)!::write(STDERR_FILENO, msg, (size_t)n);
        _exit(127);
    }
    pthread_sigmask(SIG_SETMASK, &old, nullptr);

    fcntl(fd, F_SETFD, FD_CLOEXEC);
    int fl = fcntl(fd, F_GETFL);
    fcntl(fd, F_SETFL, fl | O_NONBLOCK);
    fd_ = fd;
    pid_ = pid;
    return true;
}

bool Pty::alive() const {
    if (pid_ <= 0) return false;
    if (exited_) return false;
    int st = 0;
    pid_t r = waitpid(pid_, &st, WNOHANG);
    if (r == 0) return true;
    exited_ = true;
    if (r > 0) {
        if (WIFEXITED(st)) exit_status_ = WEXITSTATUS(st);
        else if (WIFSIGNALED(st)) exit_status_ = 128 + WTERMSIG(st);
        else exit_status_ = -1;
    } else {
        exit_status_ = -1;   // ECHILD etc.
    }
    return false;
}

ssize_t Pty::read(char* buf, size_t len) {
    if (fd_ < 0) { errno = EBADF; return -1; }
    for (;;) {
        ssize_t n = ::read(fd_, buf, len);
        if (n >= 0) return n;
        if (errno == EINTR) continue;
        if (errno == EAGAIN || errno == EWOULDBLOCK) return 0;
        return -1;   // EIO: slave side closed (child exited)
    }
}

bool Pty::write(const char* data, size_t len) {
    if (fd_ < 0) return false;
    size_t off = 0;
    while (off < len) {
        ssize_t n = ::write(fd_, data + off, len - off);
        if (n >= 0) { off += (size_t)n; continue; }
        if (errno == EINTR) continue;
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            struct pollfd p{fd_, POLLOUT, 0};
            if (poll(&p, 1, 1000) <= 0) return false;
            continue;
        }
        return false;
    }
    return true;
}

void Pty::resize(int cols, int rows, int px_w, int px_h) {
    if (fd_ < 0) return;
    struct winsize ws{};
    ws.ws_col = (unsigned short)cols;
    ws.ws_row = (unsigned short)rows;
    ws.ws_xpixel = (unsigned short)px_w;
    ws.ws_ypixel = (unsigned short)px_h;
    ioctl(fd_, TIOCSWINSZ, &ws);
}

static pid_t foreground_pgid(int fd, pid_t fallback) {
    if (fd < 0) return fallback;
    pid_t pg = tcgetpgrp(fd);
    return pg > 0 ? pg : fallback;
}

std::string Pty::foreground_cwd() const {
    pid_t pg = foreground_pgid(fd_, pid_);
    if (pg <= 0) return {};
    char path[64], buf[4096];
    snprintf(path, sizeof path, "/proc/%d/cwd", (int)pg);
    ssize_t n = readlink(path, buf, sizeof buf - 1);
    if (n <= 0) {
        if (pg != pid_ && pid_ > 0) {   // process group leader may be gone; try the shell
            snprintf(path, sizeof path, "/proc/%d/cwd", (int)pid_);
            n = readlink(path, buf, sizeof buf - 1);
        }
        if (n <= 0) return {};
    }
    return std::string(buf, (size_t)n);
}

std::string Pty::foreground_name() const {
    pid_t pg = foreground_pgid(fd_, pid_);
    if (pg <= 0) return {};
    char path[64];
    snprintf(path, sizeof path, "/proc/%d/comm", (int)pg);
    FILE* f = fopen(path, "r");
    if (!f) return {};
    char buf[256] = {0};
    if (!fgets(buf, sizeof buf, f)) buf[0] = 0;
    fclose(f);
    std::string s(buf);
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();
    return s;
}

void Pty::kill(int sig) {
    if (pid_ > 0 && !exited_) ::kill(pid_, sig);
}

void Pty::close() {
    if (fd_ >= 0) { ::close(fd_); fd_ = -1; }
}

} // namespace canopy
