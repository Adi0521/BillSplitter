#pragma once
#include <chrono>
#include <deque>
#include <mutex>
#include <string>
#include <unordered_map>

// Rate limits failed login attempts.
//
// The design decision that matters: **only failures are counted.** A successful
// login clears the key's history entirely. Throttling all attempts would punish
// ordinary use — a shared office IP, or an integration suite that logs in
// hundreds of times — while doing nothing extra against a password guesser,
// whose attempts are failures by definition.
//
// Keyed per email+IP rather than per IP alone, so one person fat-fingering
// their password cannot lock out everyone behind the same NAT.
//
// In-process and therefore per-instance: behind several backends the effective
// limit multiplies by the instance count. That is a known limitation, fine for
// a single process, and the reason this lives behind a small interface — a
// shared store can replace the map without touching call sites.
class LoginThrottle {
public:
    LoginThrottle(std::size_t max_failures = 10,
                  std::chrono::seconds window = std::chrono::minutes(15))
        : max_failures_(max_failures), window_(window) {}

    // True when this key has too many recent failures and should be refused.
    bool is_blocked(const std::string& key) {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = failures_.find(key);
        if (it == failures_.end()) return false;
        prune(it->second);
        if (it->second.empty()) {
            failures_.erase(it);
            return false;
        }
        return it->second.size() >= max_failures_;
    }

    void record_failure(const std::string& key) {
        std::lock_guard<std::mutex> lock(mutex_);
        auto& hits = failures_[key];
        prune(hits);
        hits.push_back(Clock::now());

        // Opportunistic cleanup: without it the map keeps an entry for every
        // address that ever failed once, which is an unbounded leak on a
        // long-running process being scanned.
        if (failures_.size() > kMaxKeys) collect();
    }

    void record_success(const std::string& key) {
        std::lock_guard<std::mutex> lock(mutex_);
        failures_.erase(key);
    }

    std::chrono::seconds window() const { return window_; }

private:
    using Clock = std::chrono::steady_clock;
    static constexpr std::size_t kMaxKeys = 10000;

    void prune(std::deque<Clock::time_point>& hits) {
        const auto cutoff = Clock::now() - window_;
        while (!hits.empty() && hits.front() <= cutoff) hits.pop_front();
    }

    void collect() {
        for (auto it = failures_.begin(); it != failures_.end();) {
            prune(it->second);
            it = it->second.empty() ? failures_.erase(it) : std::next(it);
        }
    }

    std::size_t                                                  max_failures_;
    std::chrono::seconds                                         window_;
    std::mutex                                                   mutex_;
    std::unordered_map<std::string, std::deque<Clock::time_point>> failures_;
};
