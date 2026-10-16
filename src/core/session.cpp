// Session: Pty + Terminal + BlockTracker + reader thread.
#include "session.h"
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstring>
#include <fcntl.h>
#include <poll.h>
#include <pwd.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

namespace canopy {

namespace {
bool file_exists(const std::string& p) { struct stat st; return ::stat(p.c_str(), &st) == 0; }
std::string dir_of(const std::string& p) { size_t s = p.rfind('/'); return s == std::string::npos ? "." : (s == 0 ? "/" : p.substr(0, s)); }
std::string base_of(const std::string& p) { size_t s = p.rfind('/'); return s == std::string::npos ? p : p.substr(s + 1); }
std::string canonical(const std::string& p) {
    char buf[PATH_MAX];
    if (realpath(p.c_str(), buf)) return buf;
    return p;
}
std::string home_dir() {
    const char* h = getenv("HOME");
    if (h && *h) return h;
    if (struct passwd* pw = getpwuid(getuid())) return pw->pw_dir ? pw->pw_dir : "";
    return "";
}
constexpr size_t INBOX_LIMIT = 8 << 20;   // reader pauses when this much is queued and unpumped
} // namespace

std::string find_shell_integration_dir() {
    auto ok = [](const std::string& d) { return !d.empty() && file_exists(d + "/canopy.bash"); };
    if (const char* e = getenv("CANOPY_SHELL_DIR"); e && ok(e)) return canonical(e);
    char exe[PATH_MAX];
    ssize_t n = readlink("/proc/self/exe", exe, sizeof exe - 1);
    if (n > 0) {
        exe[n] = 0;
        std::string d = dir_of(exe);
        for (const char* rel : {"/../shell", "/../../shell", "/shell", "/../share/canopy/shell"}) {
            std::string c = d + rel;
            if (ok(c)) return canonical(c);
        }
    }
    std::string h = home_dir();
    if (!h.empty() && ok(h + "/.local/share/canopy/shell")) return h + "/.local/share/canopy/shell";
    for (const char* sys : {"/usr/local/share/canopy/shell", "/usr/share/canopy/shell"}) if (ok(sys)) return sys;
    return {};
}

Session::Session() = default;
Session::~Session() { stop(); }

bool Session::start(const Options& opts, std::string* err) {
    if (running_) { if (err) *err = "already started"; return false; }
    term_ = std::make_unique<Terminal>(opts.cols, opts.rows, opts.scrollback);
    last_history_base_ = term_->history_base();

    if (pipe2(wake_pipe_, O_CLOEXEC | O_NONBLOCK) != 0) {
        if (err) *err = std::string("pipe2: ") + strerror(errno);
        return false;
    }

    Pty::Options po;
    po.cols = opts.cols; po.rows = opts.rows; po.cwd = opts.cwd;
    po.env_extra = opts.env_extra;
    po.argv = opts.argv;
    po.env_extra.push_back("CANOPY=1");
    po.env_extra.push_back("CANOPY_ACTIVITY=" + std::to_string(opts.activity_id));
    if (po.argv.empty()) {
        const char* sh = getenv("SHELL");
        std::string shell = (sh && *sh) ? sh : "/bin/bash";
        po.argv = {shell};
        if (opts.shell_integration && base_of(shell) == "bash") {
            std::string dir = opts.integration_dir.empty() ? find_shell_integration_dir() : opts.integration_dir;
            if (!dir.empty() && file_exists(dir + "/canopy-rcfile.bash")) {
                po.argv = {shell, "--rcfile", dir + "/canopy-rcfile.bash"};
                po.env_extra.push_back("CANOPY_SHELL_DIR=" + dir);
            }
        }
    }
    if (!pty_.spawn(po, err)) {
        ::close(wake_pipe_[0]); ::close(wake_pipe_[1]); wake_pipe_[0] = wake_pipe_[1] = -1;
        return false;
    }

    term_->on_write = [this](std::string_view s) { pty_.write(s.data(), s.size()); };
    term_->on_osc = [this](const OscEvent& ev) { blocks_.on_osc(ev); };
    term_->on_cwd = [this](const std::string& c) { blocks_.set_cwd(c); };

    running_ = true;
    eof_ = false;
    reader_ = std::thread([this] { reader_loop(); });
    return true;
}

void Session::reader_loop() {
    std::vector<char> buf(1 << 16);
    std::string batch;
    batch.reserve(1 << 18);
    while (running_) {
        struct pollfd fds[2];
        fds[0] = {pty_.fd(), POLLIN, 0};
        fds[1] = {wake_pipe_[0], POLLIN, 0};
        int r = poll(fds, 2, 1000);
        if (!running_) break;
        if (r < 0) { if (errno == EINTR) continue; break; }
        if (fds[1].revents) { char d[64]; while (::read(wake_pipe_[0], d, sizeof d) > 0) {} if (!running_) break; }
        if (!(fds[0].revents & (POLLIN | POLLHUP | POLLERR))) continue;

        bool eof = false;
        batch.clear();
        for (;;) {
            ssize_t n = pty_.read(buf.data(), buf.size());
            if (n > 0) { batch.append(buf.data(), (size_t)n); if (batch.size() >= (1 << 18)) break; continue; }
            if (n == 0) { if ((fds[0].revents & POLLHUP) && batch.empty()) eof = true; break; }
            eof = true;   // -1: EIO after the child closed its side
            break;
        }
        if (!batch.empty()) {
            std::unique_lock<std::mutex> lk(mu_);
            drained_.wait(lk, [&] { return inbox_.size() < INBOX_LIMIT || !running_; });
            inbox_.append(batch);
            lk.unlock();
            if (on_wake) on_wake();
        }
        if (eof) {
            eof_ = true;
            if (on_wake) on_wake();
            break;
        }
    }
}

bool Session::pump() {
    if (!term_) return false;
    std::string data;
    {
        std::lock_guard<std::mutex> lk(mu_);
        data.swap(inbox_);
    }
    if (data.empty()) return false;
    drained_.notify_all();
    constexpr size_t CHUNK = 1 << 16;
    for (size_t off = 0; off < data.size(); off += CHUNK)
        term_->feed(data.data() + off, std::min(CHUNK, data.size() - off));
    int64_t base = term_->history_base();
    if (base != last_history_base_) { last_history_base_ = base; blocks_.on_history_base(base); }
    return true;
}

bool Session::alive() const { return pty_.alive(); }

void Session::write(std::string_view bytes) { pty_.write(bytes.data(), bytes.size()); }

void Session::resize(int cols, int rows, int px_w, int px_h) {
    if (term_) term_->resize(cols, rows);
    pty_.resize(cols, rows, px_w, px_h);
}

std::string Session::cwd() const {
    if (term_) { std::string c = term_->cwd(); if (!c.empty()) return c; }
    return pty_.foreground_cwd();
}

void Session::stop() {
    bool was_running = running_.exchange(false);
    if (wake_pipe_[1] >= 0) { char c = 1; (void)!::write(wake_pipe_[1], &c, 1); }
    { std::lock_guard<std::mutex> lk(mu_); }
    drained_.notify_all();
    if (reader_.joinable()) reader_.join();
    if (was_running || pty_.pid() > 0) {
        pty_.kill(SIGHUP);
        // Give the child a moment to exit, then escalate. Never block indefinitely.
        for (int i = 0; i < 40 && pty_.alive(); ++i) usleep(5000);
        if (pty_.alive()) {
            pty_.kill(SIGKILL);
            for (int i = 0; i < 40 && pty_.alive(); ++i) usleep(5000);
        }
    }
    pty_.close();
    for (int& fd : wake_pipe_) if (fd >= 0) { ::close(fd); fd = -1; }
}

} // namespace canopy
