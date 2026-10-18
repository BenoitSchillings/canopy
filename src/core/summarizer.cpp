#include "summarizer.h"
#include "json.h"
#include <cerrno>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <cctype>
#include <fcntl.h>
#include <poll.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include <chrono>

extern char** environ;

namespace canopy {

std::string find_in_path(const std::string& name) {
    const char* path = getenv("PATH");
    if (!path) return "";
    std::string p = path;
    size_t start = 0;
    while (start <= p.size()) {
        size_t end = p.find(':', start);
        if (end == std::string::npos) end = p.size();
        std::string dir = p.substr(start, end - start);
        if (!dir.empty()) {
            std::string full = dir + "/" + name;
            struct stat st;
            if (stat(full.c_str(), &st) == 0 && (st.st_mode & S_IXUSR)) return full;
        }
        start = end + 1;
    }
    return "";
}

int run_process(const std::vector<std::string>& argv, const std::string& input, std::string* out, std::string* err,
                int timeout_seconds, const std::vector<std::string>& extra_env, const std::vector<std::string>& unset_env) {
    if (argv.empty()) return -1;
    int in[2], o[2], e[2];
    if (pipe(in) || pipe(o) || pipe(e)) return -1;
    pid_t pid = fork();
    if (pid < 0) return -1;
    if (pid == 0) {
        dup2(in[0], 0); dup2(o[1], 1); dup2(e[1], 2);
        for (int fd : {in[0], in[1], o[0], o[1], e[0], e[1]}) close(fd);
        for (auto& u : unset_env) unsetenv(u.c_str());
        for (auto& kv : extra_env) { size_t eq = kv.find('='); if (eq != std::string::npos) setenv(kv.substr(0, eq).c_str(), kv.substr(eq + 1).c_str(), 1); }
        std::vector<char*> av; for (auto& s : argv) av.push_back(const_cast<char*>(s.c_str())); av.push_back(nullptr);
        signal(SIGPIPE, SIG_DFL);
        execvp(av[0], av.data());
        _exit(127);
    }
    close(in[0]); close(o[1]); close(e[1]);
    fcntl(in[1], F_SETFL, O_NONBLOCK); fcntl(o[0], F_SETFL, O_NONBLOCK); fcntl(e[0], F_SETFL, O_NONBLOCK);
    size_t written = 0;
    bool in_open = true, o_open = true, e_open = true;
    if (input.empty()) { close(in[1]); in_open = false; }
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(timeout_seconds);
    char buf[16384];
    while (o_open || e_open || in_open) {
        struct pollfd fds[3]; int n = 0;
        int oi = -1, ei = -1, ii = -1;
        if (o_open) { fds[n] = {o[0], POLLIN, 0}; oi = n++; }
        if (e_open) { fds[n] = {e[0], POLLIN, 0}; ei = n++; }
        if (in_open) { fds[n] = {in[1], POLLOUT, 0}; ii = n++; }
        auto now = std::chrono::steady_clock::now();
        if (now >= deadline) { kill(pid, SIGKILL); break; }
        int ms = (int)std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now).count();
        int r = poll(fds, (nfds_t)n, std::min(ms, 500));
        if (r < 0 && errno != EINTR) break;
        if (oi >= 0 && (fds[oi].revents & (POLLIN | POLLHUP | POLLERR))) {
            ssize_t k = read(o[0], buf, sizeof buf);
            if (k > 0) { if (out) out->append(buf, (size_t)k); } else if (k == 0 || (k < 0 && errno != EAGAIN && errno != EINTR)) { close(o[0]); o_open = false; }
        }
        if (ei >= 0 && (fds[ei].revents & (POLLIN | POLLHUP | POLLERR))) {
            ssize_t k = read(e[0], buf, sizeof buf);
            if (k > 0) { if (err) err->append(buf, (size_t)k); } else if (k == 0 || (k < 0 && errno != EAGAIN && errno != EINTR)) { close(e[0]); e_open = false; }
        }
        if (ii >= 0 && (fds[ii].revents & (POLLOUT | POLLERR | POLLHUP))) {
            if (fds[ii].revents & (POLLERR | POLLHUP)) { close(in[1]); in_open = false; }
            else {
                ssize_t k = write(in[1], input.data() + written, input.size() - written);
                if (k > 0) written += (size_t)k;
                else if (k < 0 && errno != EAGAIN && errno != EINTR) { close(in[1]); in_open = false; }
                if (written >= input.size()) { close(in[1]); in_open = false; }
            }
        }
    }
    if (o_open) close(o[0]);
    if (e_open) close(e[0]);
    if (in_open) close(in[1]);
    int status = 0;
    bool timed_out = std::chrono::steady_clock::now() >= deadline;
    if (waitpid(pid, &status, 0) < 0) return -1;
    if (timed_out) return -2;
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

Summarizer::Summarizer() {}
Summarizer::~Summarizer() { stop(); }

const char* Summarizer::backend_name() const {
    switch (backend_) { case Backend::Api: return "api"; case Backend::Cli: return "claude"; default: return "local"; }
}

Summarizer::Backend Summarizer::configure(const Options& opts) {
    stop();
    opts_ = opts;
    const char* k = getenv("ANTHROPIC_API_KEY"); api_key_ = !opts.api_key.empty() ? opts.api_key : (k ? k : "");
    const char* t = getenv("ANTHROPIC_AUTH_TOKEN"); auth_token_ = t ? t : "";
    cli_path_ = find_in_path("claude");
    backend_ = Backend::None;
    bool api_ok = (!api_key_.empty() || !auth_token_.empty()) && !find_in_path("curl").empty();
    if (opts.backend == "api") backend_ = api_ok ? Backend::Api : Backend::None;
    else if (opts.backend == "claude") backend_ = cli_path_.empty() ? Backend::None : Backend::Cli;
    else if (opts.backend == "auto") backend_ = api_ok ? Backend::Api : (!cli_path_.empty() ? Backend::Cli : Backend::None);
    if (backend_ != Backend::None) { running_ = true; thread_ = std::thread([this] { worker(); }); }
    return backend_;
}

void Summarizer::stop() {
    if (!running_) return;
    { std::lock_guard<std::mutex> lk(mu_); running_ = false; }
    cv_.notify_all();
    if (thread_.joinable()) thread_.join();
}

void Summarizer::submit(SummaryRequest req) {
    if (backend_ == Backend::None) return;
    {
        std::lock_guard<std::mutex> lk(mu_);
        for (auto it = queue_.begin(); it != queue_.end(); ++it) if (it->activity == req.activity) { queue_.erase(it); break; }
        queue_.push_back(std::move(req));
    }
    cv_.notify_one();
}

size_t Summarizer::queued() const { std::lock_guard<std::mutex> lk(const_cast<std::mutex&>(mu_)); return queue_.size(); }

std::string Summarizer::fallback_notice() { std::lock_guard<std::mutex> lk(mu_); std::string n; n.swap(notice_); return n; }

std::vector<SummaryResult> Summarizer::take_results() {
    std::lock_guard<std::mutex> lk(mu_);
    std::vector<SummaryResult> r; r.swap(results_);
    return r;
}

std::string Summarizer::system_prompt() {
    return "You label terminal sessions so a user can find the one they need among many. "
           "Given a snapshot of one terminal (recent commands, their exit codes, output tails and the current screen), "
           "reply with exactly two lines:\n"
           "name: a 2-4 word label for this terminal (e.g. \"nginx config edit\", \"renderer build\", \"dataset download\")\n"
           "summary: ONE sentence of at most 14 words describing what this terminal is being used for right now, "
           "in the present tense, e.g. \"Building the Rust renderer; last cargo build failed on lifetimes\" or "
           "\"Editing nginx.conf in vim\" or \"Idle shell in ~/projects/api after running the test suite\". "
           "Name the project or directory when useful. Mention failures or things waiting for the user. "
           "No quotes, no trailing periods, no other text.";
}

void Summarizer::worker() {
    while (true) {
        SummaryRequest req;
        {
            std::unique_lock<std::mutex> lk(mu_);
            cv_.wait(lk, [this] { return !running_ || !queue_.empty(); });
            if (!running_) return;
            req = std::move(queue_.front()); queue_.pop_front();
            in_flight_ = true;
        }
        SummaryResult res = backend_ == Backend::Api ? run_api(req) : run_cli(req);
        if (backend_ == Backend::Api && !res.ok && opts_.backend == "auto") {
            // Billing/auth problems will not fix themselves this session: switch to the CLI if we can.
            bool fatal = res.error.find("credit balance") != std::string::npos || res.error.find("authentication") != std::string::npos ||
                         res.error.find("invalid x-api-key") != std::string::npos || res.error.find("permission") != std::string::npos || ++api_failures_ >= 3;
            if (fatal && !cli_path_.empty()) {
                backend_ = Backend::Cli;
                { std::lock_guard<std::mutex> lk(mu_); notice_ = "API summaries unavailable (" + res.error.substr(0, 90) + "); using the claude CLI instead"; }
                res = run_cli(req);
            }
        }
        in_flight_ = false;
        { std::lock_guard<std::mutex> lk(mu_); results_.push_back(std::move(res)); }
        if (on_result) on_result();
    }
}

static std::string clean_line(std::string line) {
    size_t a = line.find_first_not_of(" \t\r"); size_t b = line.find_last_not_of(" \t\r");
    if (a == std::string::npos) return "";
    line = line.substr(a, b - a + 1);
    if (line.size() >= 2 && (line.front() == '"' || line.front() == '\'') && line.back() == line.front()) line = line.substr(1, line.size() - 2);
    while (!line.empty() && (line.back() == '.' || line.back() == ' ')) line.pop_back();
    return line;
}

static bool starts_with_ci(const std::string& s, const char* prefix) {
    size_t n = strlen(prefix);
    if (s.size() < n) return false;
    for (size_t i = 0; i < n; i++) if (tolower((unsigned char)s[i]) != tolower((unsigned char)prefix[i])) return false;
    return true;
}

// Accepts "name: X\nsummary: Y" (either order, case-insensitive); a bare line is taken as the summary.
static void parse_reply(const std::string& raw, std::string* name, std::string* summary) {
    size_t pos = 0;
    std::vector<std::string> lines;
    while (pos <= raw.size()) {
        size_t nl = raw.find('\n', pos);
        std::string l = clean_line(raw.substr(pos, nl == std::string::npos ? std::string::npos : nl - pos));
        if (!l.empty()) lines.push_back(l);
        if (nl == std::string::npos) break;
        pos = nl + 1;
    }
    for (auto& l : lines) {
        if (starts_with_ci(l, "name:")) { if (name->empty()) *name = clean_line(l.substr(5)); }
        else if (starts_with_ci(l, "summary:")) { if (summary->empty()) *summary = clean_line(l.substr(8)); }
    }
    if (summary->empty()) for (auto& l : lines) if (!starts_with_ci(l, "name:")) { *summary = l; break; }
    if (name->size() > 32) *name = name->substr(0, 32);
}

SummaryResult Summarizer::run_api(const SummaryRequest& req) {
    SummaryResult res; res.activity = req.activity; res.context_hash = req.context_hash;
    Json body = Json::object();
    body["model"] = Json(opts_.api_model);
    body["max_tokens"] = Json(200);
    body["fallbacks"] = Json("default");
    body["output_config"] = Json(Json::Object{{"effort", Json("low")}});
    body["system"] = Json(system_prompt());
    body["messages"] = Json(Json::Array{Json(Json::Object{{"role", Json("user")}, {"content", Json(req.context)}})});
    std::vector<std::string> argv = {"curl", "-sS", "--max-time", std::to_string(opts_.timeout_seconds), "https://api.anthropic.com/v1/messages",
        "-H", "content-type: application/json", "-H", "anthropic-version: 2023-06-01",
        "-H", "anthropic-beta: server-side-fallback-2026-07-01", "-d", "@-"};
    if (!api_key_.empty()) { argv.push_back("-H"); argv.push_back("x-api-key: " + api_key_); }
    else { argv.push_back("-H"); argv.push_back("Authorization: Bearer " + auth_token_); argv.push_back("-H"); argv.push_back("anthropic-beta: oauth-2025-04-20"); }
    std::string out, err;
    int rc = run_process(argv, body.dump(), &out, &err, opts_.timeout_seconds + 5);
    if (rc != 0) { res.error = rc == -2 ? "request timed out" : ("curl failed: " + err); return res; }
    std::string perr;
    auto j = Json::parse(out, &perr);
    if (!j) { res.error = "bad JSON from API: " + perr; return res; }
    if ((*j)["type"].as_string() == "error") { res.error = "API error: " + (*j)["error"]["message"].as_string(); return res; }
    if ((*j)["stop_reason"].as_string() == "refusal") { res.error = "model declined to summarize"; return res; }
    std::string text;
    for (const Json& block : (*j)["content"].as_array()) if (block["type"].as_string() == "text") text += block["text"].as_string();
    parse_reply(text, &res.name, &res.text);
    res.ok = !res.text.empty();
    if (!res.ok) res.error = "empty response";
    return res;
}

SummaryResult Summarizer::run_cli(const SummaryRequest& req) {
    SummaryResult res; res.activity = req.activity; res.context_hash = req.context_hash;
    std::vector<std::string> argv = {cli_path_, "-p", "--output-format", "text", "--no-session-persistence",
        "--tools", "", "--system-prompt", system_prompt()};
    if (!opts_.cli_model.empty()) { argv.push_back("--model"); argv.push_back(opts_.cli_model); }
    std::string out, err;
    // The CLI refuses to run nested inside another Claude Code session; strip those markers.
    int rc = run_process(argv, req.context, &out, &err, opts_.timeout_seconds, {}, {"CLAUDECODE", "CLAUDE_CODE_ENTRYPOINT"});
    if (rc != 0) { res.error = rc == -2 ? "claude CLI timed out" : ("claude CLI failed (" + std::to_string(rc) + "): " + err.substr(0, 200)); return res; }
    parse_reply(out, &res.name, &res.text);
    res.ok = !res.text.empty();
    if (!res.ok) res.error = "empty response";
    return res;
}

} // namespace canopy
