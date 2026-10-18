#pragma once
// Asynchronous one-line summaries of terminal sessions via Claude (Messages API through curl,
// or the claude CLI), with a synchronous local heuristic as fallback/placeholder.
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace canopy {

struct SummaryRequest {
    uint64_t activity = 0;
    uint64_t context_hash = 0;
    std::string context;          // what the model sees (see Pane::summary_context)
};

struct SummaryResult {
    uint64_t activity = 0;
    uint64_t context_hash = 0;
    std::string text;             // one-line summary; empty on failure
    std::string name;             // suggested short name (2-4 words), may be empty
    std::string error;            // human-readable failure, if any
    bool ok = false;
};

class Summarizer {
public:
    enum class Backend { None, Api, Cli };
    struct Options {
        std::string backend = "auto";      // auto | api | claude | local
        std::string api_model = "claude-opus-5-5";
        std::string api_key;               // overrides ANTHROPIC_API_KEY when set
        std::string cli_model;             // empty => CLI default
        int timeout_seconds = 90;
    };

    Summarizer();
    ~Summarizer();
    // Picks the backend: api when ANTHROPIC_API_KEY/ANTHROPIC_AUTH_TOKEN is set, else the
    // claude CLI if found in PATH, else None. Returns the chosen backend.
    Backend configure(const Options& opts);
    Backend backend() const { return backend_; }
    const char* backend_name() const;
    bool available() const { return backend_ != Backend::None; }
    // Set when the API backend was abandoned at runtime (billing/auth error) and the CLI took over.
    std::string fallback_notice();   // non-empty once, then cleared

    // Thread-safe. One request is processed at a time; a newer request for the same
    // activity replaces a queued (not yet started) one.
    void submit(SummaryRequest req);
    bool busy() const { return in_flight_.load(); }
    size_t queued() const;
    // Invoked from the worker thread when a result is ready; must be cheap and thread-safe
    // (e.g. post a wake event). Results are then collected with take_results() on the main thread.
    std::function<void()> on_result;
    std::vector<SummaryResult> take_results();
    void stop();

    // The instruction given to the model.
    static std::string system_prompt();

private:
    void worker();
    SummaryResult run_api(const SummaryRequest& req);
    SummaryResult run_cli(const SummaryRequest& req);
    Options opts_;
    Backend backend_ = Backend::None;
    std::string api_key_, auth_token_, cli_path_;
    std::thread thread_;
    std::mutex mu_;
    std::condition_variable cv_;
    std::deque<SummaryRequest> queue_;
    std::vector<SummaryResult> results_;
    std::atomic<bool> in_flight_{false};
    std::atomic<bool> running_{false};
    std::string notice_;
    int api_failures_ = 0;
};

// Runs argv with `input` on stdin, captures stdout/stderr; kills after timeout. Returns exit code
// (or -1 on failure to spawn, -2 on timeout). extra_env entries are "KEY=VALUE"; unset_env names are removed.
int run_process(const std::vector<std::string>& argv, const std::string& input, std::string* out, std::string* err,
                int timeout_seconds, const std::vector<std::string>& extra_env = {}, const std::vector<std::string>& unset_env = {});
std::string find_in_path(const std::string& name);

} // namespace canopy
